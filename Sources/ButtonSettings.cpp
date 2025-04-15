/* Copyright (c) 2018-present, Fred Emmott
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file.
 */

#include "ButtonSettings.h"

#include <StreamDeckSDK/ESDLogger.h>

#include <regex>

#include "audio_json.h"

// Forward declaration of FileLog for consistency with
// AudioSwitcherStreamDeckPlugin.cpp
// FileLog removed - no longer needed

NLOHMANN_JSON_SERIALIZE_ENUM(
  DeviceMatchStrategy,
  {
    {DeviceMatchStrategy::ID, "ID"},
    {DeviceMatchStrategy::Fuzzy, "Fuzzy"},
  });

void from_json(const nlohmann::json& j, HotkeyConfig& hk) {
  // Log all keys in the JSON for debugging
  for (auto it = j.begin(); it != j.end(); ++it) {
  }

  if (j.contains("enabled")) {
    hk.enabled = j.at("enabled");
  } else if (j.contains("hotkeyEnabled")) {
    // For backward compatibility
    hk.enabled = j.at("hotkeyEnabled");
  }

  if (j.contains("ctrl")) {
    hk.ctrl = j.at("ctrl");
  } else if (j.contains("hotkeyCtrl")) {
    // For backward compatibility
    hk.ctrl = j.at("hotkeyCtrl");
  }

  if (j.contains("alt")) {
    hk.alt = j.at("alt");
  } else if (j.contains("hotkeyAlt")) {
    // For backward compatibility
    hk.alt = j.at("hotkeyAlt");
  }

  if (j.contains("shift")) {
    hk.shift = j.at("shift");
  } else if (j.contains("hotkeyShift")) {
    // For backward compatibility
    hk.shift = j.at("hotkeyShift");
  }

  if (j.contains("win")) {
    hk.win = j.at("win");
  } else if (j.contains("hotkeyWin")) {
    // For backward compatibility
    hk.win = j.at("hotkeyWin");
  }

  if (j.contains("keyCode")) {
    hk.keyCode = j.at("keyCode");
  } else if (j.contains("hotkeyKey")) {
    // For backward compatibility
    hk.keyCode = j.at("hotkeyKey");
  }
}

void to_json(nlohmann::json& j, const HotkeyConfig& hk) {
  j = {
    {"enabled", hk.enabled},
    {"ctrl", hk.ctrl},
    {"alt", hk.alt},
    {"shift", hk.shift},
    {"win", hk.win},
    {"keyCode", hk.keyCode}};
}

void from_json(const nlohmann::json& j, ButtonSettings& bs) {
  if (!j.contains("direction")) {
    return;
  }

  bs.direction = j.at("direction");

  if (j.contains("role")) {
    bs.role = j.at("role");
  }

  if (j.contains("primary")) {
    const auto& primary = j.at("primary");
    if (primary.is_string()) {
      bs.primaryDevice.id = primary;
    } else {
      bs.primaryDevice = primary;
    }
  }

  if (j.contains("secondary")) {
    const auto& secondary = j.at("secondary");
    if (secondary.is_string()) {
      bs.secondaryDevice.id = secondary;
    } else {
      bs.secondaryDevice = secondary;
    }
  }

  if (j.contains("matchStrategy")) {
    bs.matchStrategy = j.at("matchStrategy");
  }

  if (j.contains("primaryHotkey")) {
    bs.primaryHotkey = j.at("primaryHotkey");
  } else if (j.contains("hotkey")) {
    // For backward compatibility
    bs.primaryHotkey = j.at("hotkey");
  }

  if (j.contains("secondaryHotkey")) {
    bs.secondaryHotkey = j.at("secondaryHotkey");
  }
}

void to_json(nlohmann::json& j, const ButtonSettings& bs) {
  j = {
    {"direction", bs.direction},
    {"role", bs.role},
    {"primary", bs.primaryDevice},
    {"secondary", bs.secondaryDevice},
    {"matchStrategy", bs.matchStrategy},
    {"primaryHotkey", bs.primaryHotkey},
    {"secondaryHotkey", bs.secondaryHotkey}};
}

namespace {

std::string FuzzifyInterface(const std::string& name) {
  // Windows likes to replace "Foo" with "2- Foo"
  const std::regex pattern{"^([0-9]+- )?(.+)$"};
  std::smatch captures;
  if (!std::regex_match(name, captures, pattern)) {
    return name;
  }
  return captures[2];
}

std::string GetVolatileID(
  const AudioDeviceInfo& device,
  DeviceMatchStrategy strategy) {
  if (device.id.empty()) {
    return {};
  }

  if (strategy == DeviceMatchStrategy::ID) {
    return device.id;
  }

  if (GetAudioDeviceState(device.id) == AudioDeviceState::CONNECTED) {
    return device.id;
  }

  const auto fuzzyInterface = FuzzifyInterface(device.interfaceName);
  ESDDebug(
    "Looking for a fuzzy match: {} -> {}",
    device.interfaceName,
    fuzzyInterface);

  for (const auto& [otherID, other] : GetAudioDeviceList(device.direction)) {
    if (other.state != AudioDeviceState::CONNECTED) {
      continue;
    }
    const auto otherFuzzyInterface = FuzzifyInterface(other.interfaceName);
    ESDDebug("Trying {} -> {}", other.interfaceName, otherFuzzyInterface);
    if (
      fuzzyInterface == otherFuzzyInterface
      && device.endpointName == other.endpointName) {
      ESDDebug(
        "Fuzzy device match for {}/{}",
        device.interfaceName,
        device.endpointName);
      return otherID;
    }
  }
  ESDDebug(
    "Failed fuzzy match for {}/{}", device.interfaceName, device.endpointName);
  return device.id;
}
}// namespace

std::string ButtonSettings::VolatilePrimaryID() const {
  return GetVolatileID(primaryDevice, matchStrategy);
}

std::string ButtonSettings::VolatileSecondaryID() const {
  return GetVolatileID(secondaryDevice, matchStrategy);
}
