//==============================================================================
/**
@file       AudioSwitcherStreamDeckPlugin.cpp

@brief      CPU plugin

@copyright  (c) 2018, Corsair Memory, Inc.
@copyright  (c) 2018-present, Fred Emmott.
      This source code is licensed under the MIT-style license found in the
LICENSE file.

**/
//==============================================================================

#include "AudioSwitcherStreamDeckPlugin.h"

#include <AudioDevices/AudioDevices.h>
#include <StreamDeckSDK/EPLJSONUtils.h>
#include <StreamDeckSDK/ESDConnectionManager.h>
#include <StreamDeckSDK/ESDLogger.h>

#include <atomic>
#include <functional>
#include <mutex>

#ifdef _MSC_VER
#include <objbase.h>
#endif

#include "audio_json.h"

// Add Windows and macOS specific includes
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#include <unistd.h>
#endif

// Remove custom file logging
#include <ctime>
#include <fstream>
#include <iostream>

using namespace FredEmmott::Audio;
using json = nlohmann::json;

namespace {
constexpr std::string_view SET_ACTION_ID{
  "com.fredemmott.audiooutputswitch.set"};
constexpr std::string_view TOGGLE_ACTION_ID{
  "com.fredemmott.audiooutputswitch.toggle"};

bool FillAudioDeviceInfo(AudioDeviceInfo& di) {
  if (di.id.empty()) {
    return false;
  }
  if (!di.displayName.empty()) {
    return false;
  }

  const auto devices = GetAudioDeviceList(di.direction);
  if (!devices.contains(di.id)) {
    return false;
  }
  di = devices.at(di.id);
  return true;
}

}// namespace

AudioSwitcherStreamDeckPlugin::AudioSwitcherStreamDeckPlugin() {
  // Remove FileLog calls
#ifdef _MSC_VER
  CoInitializeEx(
    NULL, COINIT_MULTITHREADED);// initialize COM for the main thread
#endif
  mCallbackHandle = AddDefaultAudioDeviceChangeCallback(std::bind_front(
    &AudioSwitcherStreamDeckPlugin::OnDefaultDeviceChanged, this));
}

AudioSwitcherStreamDeckPlugin::~AudioSwitcherStreamDeckPlugin() {
  mCallbackHandle = {};
}

void AudioSwitcherStreamDeckPlugin::OnDefaultDeviceChanged(
  AudioDeviceDirection direction,
  AudioDeviceRole role,
  const std::string& device) {
  std::scoped_lock lock(mVisibleContextsMutex);
  for (const auto& [context, button] : mButtons) {
    if (button.settings.direction != direction) {
      continue;
    }
    if (button.settings.role != role) {
      continue;
    }
    UpdateState(context, device);
  }
}

void AudioSwitcherStreamDeckPlugin::KeyDownForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");
}

void AudioSwitcherStreamDeckPlugin::KeyUpForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  ESDDebug("{}: {}", __FUNCTION__, inPayload.dump());
  std::scoped_lock lock(mVisibleContextsMutex);

  if (!inPayload.contains("settings")) {
    return;
  }

  auto& settings = mButtons[inContext].settings;
  settings = inPayload.at("settings");

  FillButtonDeviceInfo(inContext);

  const auto state = EPLJSONUtils::GetIntByName(inPayload, "state");

  // this looks inverted - but if state is 0, we want to move to state 1, so
  // we want the secondary devices. if state is 1, we want state 0, so we want
  // the primary device
  const auto deviceID = (state != 0 || inAction == SET_ACTION_ID)
    ? settings.VolatilePrimaryID()
    : settings.VolatileSecondaryID();

  if (deviceID.empty()) {
    ESDDebug("Doing nothing, no device ID");
    return;
  }

  const auto deviceState = GetAudioDeviceState(deviceID);
  if (deviceState != AudioDeviceState::CONNECTED) {
    if (inAction == SET_ACTION_ID) {
      mConnectionManager->SetState(1, inContext);
    }
    mConnectionManager->ShowAlertForContext(inContext);
    return;
  }

  if (
    inAction == SET_ACTION_ID
    && deviceID == GetDefaultAudioDeviceID(settings.direction, settings.role)) {
    // We already have the correct device, undo the state change
    mConnectionManager->SetState(state, inContext);
    ESDDebug("Already set, nothing to do");
    return;
  }

  ESDDebug("Setting device to {}", deviceID);
  SetDefaultAudioDeviceID(settings.direction, settings.role, deviceID);

  // Determine which hotkey to use based on which device we're switching to
  const HotkeyConfig& hotkeyToUse = (state != 0 || inAction == SET_ACTION_ID)
    ? settings.primaryHotkey
    : settings.secondaryHotkey;

  // Trigger hotkey if enabled
  if (hotkeyToUse.enabled && !hotkeyToUse.keyCode.empty()) {
    ESDDebug("Triggering hotkey: {}", hotkeyToUse.keyCode);
    TriggerHotkey(hotkeyToUse);
  }
}

void AudioSwitcherStreamDeckPlugin::WillAppearForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  std::scoped_lock lock(mVisibleContextsMutex);
  // Remember the context
  mVisibleContexts.insert(inContext);
  auto& button = mButtons[inContext];
  button = {inAction, inContext};

  if (!inPayload.contains("settings")) {
    return;
  }
  button.settings = inPayload.at("settings");

  UpdateState(inContext);
  FillButtonDeviceInfo(inContext);
}

void AudioSwitcherStreamDeckPlugin::FillButtonDeviceInfo(
  const std::string& context) {
  auto& settings = mButtons.at(context).settings;

  const auto filledPrimary = FillAudioDeviceInfo(settings.primaryDevice);
  const auto filledSecondary = FillAudioDeviceInfo(settings.secondaryDevice);
  if (filledPrimary || filledSecondary) {
    ESDDebug("Backfilling settings to {}", json(settings).dump());
    mConnectionManager->SetSettings(settings, context);
  }
}

void AudioSwitcherStreamDeckPlugin::WillDisappearForAction(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  // Remove the context
  std::scoped_lock lock(mVisibleContextsMutex);
  mVisibleContexts.erase(inContext);
  mButtons.erase(inContext);
}

void AudioSwitcherStreamDeckPlugin::SendToPlugin(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  json outPayload;

  const auto event = EPLJSONUtils::GetStringByName(inPayload, "event");
  ESDDebug("Received event {}", event);

  if (event == "getDeviceList") {
    const auto outputList = GetAudioDeviceList(AudioDeviceDirection::OUTPUT);
    const auto inputList = GetAudioDeviceList(AudioDeviceDirection::INPUT);
    mConnectionManager->SendToPropertyInspector(
      inAction,
      inContext,
      json({
        {"event", event},
        {"outputDevices", outputList},
        {"inputDevices", inputList},
      }));
    return;
  }
}

void AudioSwitcherStreamDeckPlugin::UpdateState(
  const std::string& context,
  const std::string& optionalDefaultDevice) {
  const auto button = mButtons[context];
  const auto action = button.action;
  const auto settings = button.settings;
  const auto activeDevice = optionalDefaultDevice.empty()
    ? GetDefaultAudioDeviceID(settings.direction, settings.role)
    : optionalDefaultDevice;

  const auto primaryID = settings.VolatilePrimaryID();
  const auto secondaryID = settings.VolatileSecondaryID();

  std::scoped_lock lock(mVisibleContextsMutex);
  if (action == SET_ACTION_ID) {
    mConnectionManager->SetState(activeDevice == primaryID ? 0 : 1, context);
    return;
  }

  if (activeDevice == primaryID) {
    mConnectionManager->SetState(0, context);
    return;
  }

  if (activeDevice == secondaryID) {
    mConnectionManager->SetState(1, context);
    return;
  }

  mConnectionManager->ShowAlertForContext(context);
}

void AudioSwitcherStreamDeckPlugin::DeviceDidConnect(
  const std::string& inDeviceID,
  const json& inDeviceInfo) {
  // Nothing to do
}

void AudioSwitcherStreamDeckPlugin::DeviceDidDisconnect(
  const std::string& inDeviceID) {
  // Nothing to do
}

void AudioSwitcherStreamDeckPlugin::DidReceiveGlobalSettings(
  const json& inPayload) {
}

void AudioSwitcherStreamDeckPlugin::DidReceiveSettings(
  const std::string& inAction,
  const std::string& inContext,
  const json& inPayload,
  const std::string& inDeviceID) {
  WillAppearForAction(inAction, inContext, inPayload, inDeviceID);
}

// Add the TriggerHotkey function
void TriggerHotkey(const HotkeyConfig& hotkey) {
  if (!hotkey.enabled || hotkey.keyCode.empty()) {
    return;
  }

#ifdef _WIN32
  INPUT inputs[10] = {};// Maximum 5 keys (4 modifiers + 1 key) plus releases
  int inputCount = 0;

  // Press modifiers
  if (hotkey.ctrl) {
    inputs[inputCount].type = INPUT_KEYBOARD;
    inputs[inputCount].ki.wVk = VK_CONTROL;
    inputCount++;
  }
  if (hotkey.alt) {
    inputs[inputCount].type = INPUT_KEYBOARD;
    inputs[inputCount].ki.wVk = VK_MENU;
    inputCount++;
  }
  if (hotkey.shift) {
    inputs[inputCount].type = INPUT_KEYBOARD;
    inputs[inputCount].ki.wVk = VK_SHIFT;
    inputCount++;
  }
  if (hotkey.win) {
    inputs[inputCount].type = INPUT_KEYBOARD;
    inputs[inputCount].ki.wVk = VK_LWIN;
    inputCount++;
  }

  // Press the key
  inputs[inputCount].type = INPUT_KEYBOARD;
  // Convert string key code to virtual key code
  WORD vkCode = 0;

  if (hotkey.keyCode.length() == 1) {
    // Convert single character to virtual key code
    SHORT scanCode = VkKeyScanA(hotkey.keyCode[0]);
    if (scanCode != -1) {
      vkCode = LOBYTE(scanCode);
    }
  } else if (
    hotkey.keyCode.substr(0, 1) == "F" && hotkey.keyCode.length() <= 3) {
    // Handle function keys F1-F24
    try {
      int fKey = std::stoi(hotkey.keyCode.substr(1));
      if (fKey >= 1 && fKey <= 24) {
        vkCode = VK_F1 + (fKey - 1);
      }
    } catch (const std::exception& e) {
      // Error parsing function key
    }
  } else {
    // Handle named keys using a switch for cleaner code
    // First check for special key names
    if (hotkey.keyCode == "SPACE") {
      vkCode = VK_SPACE;
    } else if (hotkey.keyCode == "ENTER" || hotkey.keyCode == "RETURN") {
      vkCode = VK_RETURN;
    } else if (hotkey.keyCode == "ESCAPE" || hotkey.keyCode == "ESC") {
      vkCode = VK_ESCAPE;
    } else if (hotkey.keyCode == "TAB") {
      vkCode = VK_TAB;
    } else {
      // For single letters and numbers, use a switch statement
      if (hotkey.keyCode.length() == 1) {
        char key = hotkey.keyCode[0];
        vkCode = key;// ASCII values map directly to VK codes for A-Z and 0-9
      } else {
        // For named letter keys (e.g., "A", "B")
        switch (hotkey.keyCode[0]) {
          case 'A':
            vkCode = 'A';
            break;
          case 'B':
            vkCode = 'B';
            break;
          case 'C':
            vkCode = 'C';
            break;
          case 'D':
            vkCode = 'D';
            break;
          case 'E':
            vkCode = 'E';
            break;
          case 'F':
            vkCode = 'F';
            break;
          case 'G':
            vkCode = 'G';
            break;
          case 'H':
            vkCode = 'H';
            break;
          case 'I':
            vkCode = 'I';
            break;
          case 'J':
            vkCode = 'J';
            break;
          case 'K':
            vkCode = 'K';
            break;
          case 'L':
            vkCode = 'L';
            break;
          case 'M':
            vkCode = 'M';
            break;
          case 'N':
            vkCode = 'N';
            break;
          case 'O':
            vkCode = 'O';
            break;
          case 'P':
            vkCode = 'P';
            break;
          case 'Q':
            vkCode = 'Q';
            break;
          case 'R':
            vkCode = 'R';
            break;
          case 'S':
            vkCode = 'S';
            break;
          case 'T':
            vkCode = 'T';
            break;
          case 'U':
            vkCode = 'U';
            break;
          case 'V':
            vkCode = 'V';
            break;
          case 'W':
            vkCode = 'W';
            break;
          case 'X':
            vkCode = 'X';
            break;
          case 'Y':
            vkCode = 'Y';
            break;
          case 'Z':
            vkCode = 'Z';
            break;
          case '0':
            vkCode = '0';
            break;
          case '1':
            vkCode = '1';
            break;
          case '2':
            vkCode = '2';
            break;
          case '3':
            vkCode = '3';
            break;
          case '4':
            vkCode = '4';
            break;
          case '5':
            vkCode = '5';
            break;
          case '6':
            vkCode = '6';
            break;
          case '7':
            vkCode = '7';
            break;
          case '8':
            vkCode = '8';
            break;
          case '9':
            vkCode = '9';
            break;
          default:
            break;// Key not recognized
        }
      }
    }
  }

  if (vkCode != 0) {
    inputs[inputCount].ki.wVk = LOBYTE(vkCode);
    inputCount++;

    // Now set up the key releases (in reverse order)
    // First, release the key
    inputs[inputCount] = inputs[inputCount - 1];
    inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
    inputCount++;

    // Then release modifiers in reverse order
    for (int i = inputCount - 3; i >= 0; i--) {
      inputs[inputCount] = inputs[i];
      inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
      inputCount++;
    }

    // Send key events
    UINT result = SendInput(inputCount, inputs, sizeof(INPUT));
    if (result != inputCount) {
      DWORD errorCode = GetLastError();
      ESDDebug("SendInput failed with error: {}", errorCode);
    }
  }

#elif defined(__APPLE__)
  // macOS implementation using CGEventCreateKeyboardEvent
  CGEventSourceRef source
    = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);

  // Get the key code for the specified key
  CGKeyCode keyCode = 0;
  // Basic mapping for common keys
  if (hotkey.keyCode.length() == 1) {
    char key = hotkey.keyCode[0];
    if (key >= 'A' && key <= 'Z') {
      // Map A-Z to macOS virtual key codes
      keyCode = 0x00 + (key - 'A');// 'A' is 0x00, 'B' is 0x0B, etc.
    }
  } else if (
    hotkey.keyCode.substr(0, 1) == "F" && hotkey.keyCode.length() <= 3) {
    // Handle function keys F1-F12
    int fKey = std::stoi(hotkey.keyCode.substr(1));
    if (fKey >= 1 && fKey <= 12) {
      static const CGKeyCode fKeyCodes[12] = {
        0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F};
      keyCode = fKeyCodes[fKey - 1];
    }
  }

  if (keyCode != 0) {
    // Create key down event
    CGEventRef keyDownEvent = CGEventCreateKeyboardEvent(source, keyCode, true);

    // Set modifiers
    CGEventFlags flags = 0;
    if (hotkey.ctrl)
      flags |= kCGEventFlagMaskControl;
    if (hotkey.alt)
      flags |= kCGEventFlagMaskAlternate;
    if (hotkey.shift)
      flags |= kCGEventFlagMaskShift;
    if (hotkey.win)
      flags |= kCGEventFlagMaskCommand;

    CGEventSetFlags(keyDownEvent, flags);

    // Create key up event
    CGEventRef keyUpEvent = CGEventCreateKeyboardEvent(source, keyCode, false);
    CGEventSetFlags(keyUpEvent, flags);

    // Post events
    CGEventPost(kCGHIDEventTap, keyDownEvent);
    usleep(10000);// Small delay
    CGEventPost(kCGHIDEventTap, keyUpEvent);

    // Release resources
    CFRelease(keyDownEvent);
    CFRelease(keyUpEvent);
    CFRelease(source);
  }
#endif
}
