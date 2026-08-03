#include "render/text/glyph_registry.h"

#include "core/files/resource_paths.h"
#include "core/log.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace {

  constexpr Logger kLog("glyph");
  constexpr char32_t kMissingGlyph = 0xF292; // skull

  // Hand-curated Noctalia alias -> Material Design Icon name map.
  // Use these for semantic shell states and stable Noctalia-facing names.
  // clang-format off
const std::unordered_map<std::string, std::string_view> kAliases = {
    {"activity", "timeline"},
    {"add", "add"},
    {"alert-triangle", "warning_amber"},
    {"app-window", "sensor_window"},
    {"apps", "apps"},
    {"arrow-big-up", "arrow_upward"},
    {"arrows-exchange", "swap_vert"},
    {"arrows-horizontal", "swap_horiz"},
    {"balanced", "tune"},
    {"ball-football", "sports_soccer"},
    {"battery-0", "battery_alert"},
    {"battery-1", "battery_std"},
    {"battery-2", "battery_std"},
    {"battery-3", "battery_std"},
    {"battery-4", "battery_full"},
    {"battery-charging", "battery_charging_full"},
    {"battery-exclamation", "battery_alert"},
    {"battery-plugged", "battery_charging_full"},
    {"bell", "notifications"},
    {"bell-off", "notifications_off"},
    {"blob-filled", "lens"},
    {"bluetooth", "bluetooth"},
    {"bluetooth-off", "bluetooth_disabled"},
    {"bluetooth-device-earbuds", "headset"},
    {"bluetooth-device-gamepad", "sports_esports"},
    {"bluetooth-device-generic", "bluetooth"},
    {"bluetooth-device-headphones", "headset"},
    {"bluetooth-device-headset", "headset"},
    {"bluetooth-device-keyboard", "keyboard"},
    {"bluetooth-device-microphone", "mic"},
    {"bluetooth-device-mouse", "mouse"},
    {"bluetooth-device-phone", "smartphone"},
    {"bluetooth-device-speaker", "speaker"},
    {"bluetooth-device-tv", "tv"},
    {"bluetooth-device-watch", "watch"},
    {"bolt", "flash_on"},
    {"border-corner-pill", "crop_square"},
    {"brand-apple", "laptop_mac"},
    {"brand-git", "code"},
    {"brand-google", "public"},
    {"brightness-high", "brightness_high"},
    {"brightness-low", "brightness_low"},
    {"bug", "bug_report"},
    {"busy", "hourglass_empty"},
    {"caffeine-off", "local_cafe"},
    {"caffeine-on", "local_cafe"},
    {"calculator", "calculate"},
    {"calendar-cog", "event"},
    {"camera-off", "videocam_off"},
    {"capslock", "keyboard"},
    {"check", "check"},
    {"chevron-down", "keyboard_arrow_down"},
    {"chevron-left", "chevron_left"},
    {"chevron-right", "chevron_right"},
    {"chevron-up", "keyboard_arrow_up"},
    {"circle-filled", "lens"},
    {"circuit-pushbutton", "toggle_on"},
    {"clipboard", "content_paste"},
    {"clock", "access_time"},
    {"close", "close"},
    {"color-picker", "colorize"},
    {"copy-plus", "content_copy"},
    {"cpu-intensive", "speed"},
    {"cpu-temperature", "thermostat"},
    {"cpu-usage", "memory"},
    {"device-desktop", "desktop_windows"},
    {"device-floppy", "save"},
    {"disc", "album"},
    {"disc-filled", "disc_full"},
    {"download", "download"},
    {"download-speed", "download"},
    {"external-link", "open_in_new"},
    {"eye", "visibility"},
    {"file-lock", "enhanced_encryption"},
    {"flask", "science"},
    {"flip-horizontal", "flip"},
    {"flip-vertical", "flip"},
    {"folder", "folder"},
    {"gpu-usage", "memory"},
    {"grid-dots", "grid_on"},
    {"guitar-pick-filled", "album"},
    {"headphones", "headset"},
    {"heart", "favorite"},
    {"hibernate", "bedtime"},
    {"home", "home"},
    {"hourglass-empty", "hourglass_empty"},
    {"image", "image"},
    {"info", "info"},
    {"key", "vpn_key"},
    {"keyboard", "keyboard"},
    {"layers-intersect", "layers"},
    {"layout-grid", "grid_on"},
    {"letter-t", "title"},
    {"lock", "lock"},
    {"map-pin-off", "location_off"},
    {"media-next", "skip_next"},
    {"media-pause", "pause"},
    {"media-play", "play_arrow"},
    {"media-prev", "skip_previous"},
    {"memory", "memory"},
    {"menu-2", "menu"},
    {"michelin-star-filled", "star_half"},
    {"microphone", "mic"},
    {"microphone-mute", "mic_off"},
    {"microphone-off", "mic_off"},
    {"moon", "bedtime"},
    {"more-vertical", "more_vert"},
    {"mountain", "terrain"},
    {"music-off", "music_off"},
    {"nightlight-forced", "bedtime"},
    {"nightlight-off", "bedtime"},
    {"nightlight-on", "bedtime"},
    {"mono-shell", "star"},
    {"numlock", "keyboard"},
    {"official-plugin", "verified_user"},
    {"pentagon-filled", "stop"},
    {"performance", "speed"},
    {"person", "person"},
    {"photo-off", "no_photography"},
    {"pin", "push_pin"},
    {"plug-off", "power_off"},
    {"plugin", "extension"},
    {"plus", "add"},
    {"powersaver", "eco"},
    {"puzzle", "extension"},
    {"reboot", "refresh"},
    {"refresh", "refresh"},
    {"repeat", "repeat"},
    {"screen-share-off", "stop_screen_share"},
    {"screenshot", "photo_camera"},
    {"scrolllock", "keyboard"},
    {"search", "search"},
    {"send", "send"},
    {"settings", "settings"},
    {"shield-check", "verified_user"},
    {"shield-lock", "security"},
    {"shuffle", "shuffle"},
    {"shutdown", "power_settings_new"},
    {"square-rounded-filled", "square_foot"},
    {"stack-2", "layers"},
    {"stack-back", "flip_to_back"},
    {"stack-front", "flip_to_front"},
    {"stack-pop", "open_in_full"},
    {"star", "star"},
    {"star-filled", "star"},
    {"stop", "stop"},
    {"storage", "storage"},
    {"sun", "wb_sunny"},
    {"suspend", "pause"},
    {"temperature", "thermostat"},
    {"temperature-sun", "wb_sunny"},
    {"terminal", "code"},
    {"theme-mode", "invert_colors"},
    {"toast-error", "error_outline"},
    {"toast-notice", "check_circle"},
    {"toast-warning", "warning_amber"},
    {"trash", "delete"},
    {"triangle-filled", "play_arrow"},
    {"unpin", "push_pin"},
    {"upload-speed", "upload"},
    {"video", "videocam"},
    {"volume-high", "volume_up"},
    {"volume-low", "volume_down"},
    {"volume-mute", "volume_mute"},
    {"volume-x", "volume_off"},
    {"volume-zero", "volume_off"},
    {"wallpaper-selector", "wallpaper"},
    {"warning", "warning_amber"},
    {"wave-sine", "graphic_eq"},
    {"weather-cloud", "cloud"},
    {"weather-cloud-haze", "blur_on"},
    {"weather-cloud-lightning", "flash_on"},
    {"weather-cloud-off", "cloud_off"},
    {"weather-cloud-rain", "umbrella"},
    {"weather-cloud-snow", "ac_unit"},
    {"weather-cloud-sun", "wb_cloudy"},
    {"weather-moon", "bedtime"},
    {"weather-moon-stars", "nights_stay"},
    {"weather-sun", "wb_sunny"},
    {"weather-sunrise", "wb_sunny"},
    {"weather-sunset", "wb_sunny"},
    {"wifi", "wifi"},
    {"wifi-0", "wifi_off"},
    {"wifi-1", "wifi"},
    {"wifi-2", "wifi"},
    {"wifi-3", "wifi"},
    {"wifi-off", "wifi_off"},
    {"wind", "flag"},
    {"world", "language"},

};
  // clang-format on

  [[nodiscard]] std::optional<char32_t> parseCodepointLiteral(std::string_view value) {
    if (value.size() < 3) {
      return std::nullopt;
    }

    std::string_view hex;
    if ((value[0] == 'U' || value[0] == 'u') && value[1] == '+') {
      hex = value.substr(2);
    } else if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
      hex = value.substr(2);
    } else {
      return std::nullopt;
    }

    if (hex.empty()) {
      return std::nullopt;
    }

    // A glyph name may itself contain trailing hex-ish characters (e.g. "a-b-2");
    // only treat the value as a codepoint literal when it is a pure hex token.
    if (hex.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) {
      return std::nullopt;
    }

    std::uint32_t codepoint = 0;
    const auto* begin = hex.data();
    const auto* end = begin + hex.size();
    const auto result = std::from_chars(begin, end, codepoint, 16);
    if (result.ec != std::errc{} || result.ptr != end || codepoint == 0 || codepoint > 0x10FFFF) {
      return std::nullopt;
    }
    return static_cast<char32_t>(codepoint);
  }

  [[nodiscard]] std::unordered_map<std::string, GlyphRegistry::MaterialGlyphMetadata> loadMaterialMetadata() {
    std::unordered_map<std::string, GlyphRegistry::MaterialGlyphMetadata> icons;
    const std::filesystem::path path = paths::assetPath("fonts/material-icons.json");
    std::ifstream file(path);
    if (!file.is_open()) {
      kLog.warn("failed to open Material glyph metadata: {}", path.string());
      return icons;
    }

    try {
      const auto root = nlohmann::json::parse(file);
      if (!root.is_object()) {
        kLog.warn("Material glyph metadata is not an object: {}", path.string());
        return icons;
      }

      icons.reserve(root.size());
      for (const auto& [name, value] : root.items()) {
        if (!value.is_object()) {
          continue;
        }
        const auto codepointIt = value.find("codepoint");
        const auto categoryIt = value.find("category");
        if (codepointIt == value.end()
            || categoryIt == value.end()
            || !codepointIt->is_string()
            || !categoryIt->is_string()) {
          continue;
        }
        const std::string codepoint = codepointIt->get<std::string>();
        if (auto parsed = parseCodepointLiteral(codepoint)) {
          icons.emplace(
              name,
              GlyphRegistry::MaterialGlyphMetadata{
                  .codepoint = *parsed,
                  .category = categoryIt->get<std::string>(),
              }
          );
        }
      }
      kLog.debug("loaded {} Material glyph names from {}", icons.size(), path.string());
    } catch (const nlohmann::json::exception& e) {
      kLog.warn("failed to parse Material glyph metadata '{}': {}", path.string(), e.what());
    }
    return icons;
  }

  [[nodiscard]] const std::unordered_map<std::string, GlyphRegistry::MaterialGlyphMetadata>& materialMetadata() {
    static const std::unordered_map<std::string, GlyphRegistry::MaterialGlyphMetadata> icons = loadMaterialMetadata();
    return icons;
  }

  [[nodiscard]] const std::unordered_map<std::string, char32_t>& materialIcons() {
    static const std::unordered_map<std::string, char32_t> icons = [] {
      std::unordered_map<std::string, char32_t> flat;
      const auto& metadata = materialMetadata();
      flat.reserve(metadata.size());
      for (const auto& [name, entry] : metadata) {
        flat.emplace(name, entry.codepoint);
      }
      return flat;
    }();
    return icons;
  }

  [[nodiscard]] const char32_t* resolveToMaterial(std::string_view name) {
    const auto& icons = materialIcons();
    const std::string key{name};
    if (const auto alias = kAliases.find(key); alias != kAliases.end()) {
      if (const auto it = icons.find(std::string(alias->second)); it != icons.end()) {
        return &it->second;
      }
      return nullptr;
    }
    if (const auto it = icons.find(key); it != icons.end()) {
      return &it->second;
    }
    return nullptr;
  }

} // namespace

bool GlyphRegistry::contains(std::string_view name) {
  if (parseCodepointLiteral(name).has_value()) {
    return true;
  }
  return resolveToMaterial(name) != nullptr;
}

char32_t GlyphRegistry::lookup(std::string_view name) {
  if (auto codepoint = parseCodepointLiteral(name)) {
    return *codepoint;
  }

  if (const char32_t* codepoint = resolveToMaterial(name)) {
    return *codepoint;
  }

  kLog.warn("missing glyph: {}", name);
  return kMissingGlyph;
}

const std::unordered_map<std::string, GlyphRegistry::MaterialGlyphMetadata>& GlyphRegistry::materialGlyphMetadata() {
  return ::materialMetadata();
}

const std::unordered_map<std::string, char32_t>& GlyphRegistry::materialIcons() { return ::materialIcons(); }

std::optional<std::string_view> GlyphRegistry::categoryFor(std::string_view name) {
  if (resolveToMaterial(name) == nullptr) {
    return std::nullopt;
  }
  const auto& metadata = materialMetadata();
  const std::string key{name};
  if (const auto it = metadata.find(key); it != metadata.end()) {
    return it->second.category;
  }
  for (const auto& [aliasKey, target] : kAliases) {
    if (aliasKey == name) {
      if (const auto it = metadata.find(std::string(target)); it != metadata.end()) {
        return it->second.category;
      }
      break;
    }
  }
  return std::nullopt;
}

const std::unordered_map<std::string, std::string_view>& GlyphRegistry::aliases() { return kAliases; }
