#include "shell/bar/widget_gesture_defaults.h"

#include "core/log.h"
#include "scripting/plugin_registry.h"

#include <array>
#include <vector>

namespace noctalia::bar {

  namespace {

    constexpr std::array<GestureBinding, 0> kBuiltinDefaults{};

    constexpr std::array<GestureBinding, 0> kDeadZoneDefaults{};

    struct TypeDefaults {
      std::string_view type;
      std::span<const GestureBinding> bindings;
    };

    // Widgets whose whole-widget gestures are declared here rather than wired by hand in create().
    // The battery widget binds none: its icon area's left click used to open the control center.
    constexpr std::array<GestureBinding, 0> kBattery{};
    // Clock owns its left button in ClockWidget (click toggles time <-> date), so no
    // panel gesture is bound here. Freedom from a binding keeps the left click on the
    // widget's own input area instead of being consumed by the gesture dispatcher.
    constexpr std::array<GestureBinding, 0> kClock{};
    constexpr std::array<GestureBinding, 1> kKeyboardLayout{{{Gesture::Left, "keyboard-layout-cycle"}}};
    constexpr std::array<GestureBinding, 2> kWorkspaces{
        {{Gesture::ScrollUp, "workspace-switch prev"}, {Gesture::ScrollDown, "workspace-switch next"}}
    };

    constexpr std::array<TypeDefaults, 4> kTypeDefaults{{
        {"battery", kBattery},
        {"clock", kClock},
        {"keyboard_layout", kKeyboardLayout},
        {"workspaces", kWorkspaces},
    }};

    struct TypeReserved {
      std::string_view type;
      GestureMask gestures;
    };

    const std::array<TypeReserved, 2> kTypeReserved{{
        // Left activates an individual workspace.
        {"workspaces", GestureMask{Gesture::Left}},
        // Left activates a tray item, right opens its menu.
        {"tray", GestureMask{Gesture::Left, Gesture::Right}},
    }};

    constexpr Logger kLog("bar.actions");

    // A plugin [[widget]] entry declares its defaults in plugin.toml, where the settings GUI can
    // read them too. The manifest parser keeps them as strings, so the vocabulary is checked here.
    std::vector<GestureBinding> pluginGestureDefaults(std::string_view type) {
      auto& registry = scripting::PluginRegistry::instance();
      registry.ensureScanned();
      const auto resolved = registry.resolve(type);
      if (!resolved.has_value() || resolved->entry->kind != scripting::PluginEntryKind::Widget) {
        return {};
      }

      std::vector<GestureBinding> bindings;
      bindings.reserve(resolved->entry->widgetActions.size());
      for (const auto& [gestureKey, action] : resolved->entry->widgetActions) {
        const auto gesture = parseGestureKey(gestureKey);
        if (!gesture.has_value()) {
          kLog.error("{}: [widget.actions] '{}' is not a gesture", type, gestureKey);
          continue;
        }
        bindings.push_back(GestureBinding{*gesture, action});
      }
      return bindings;
    }

  } // namespace

  std::span<const GestureBinding> builtinGestureDefaults() noexcept { return kBuiltinDefaults; }

  std::span<const GestureBinding> deadZoneGestureDefaults() noexcept { return kDeadZoneDefaults; }

  std::vector<GestureBinding>
  gestureDefaultsForType(std::string_view type, [[maybe_unused]] const WidgetConfig* config) {
    const auto collect = [](std::span<const GestureBinding> bindings) {
      return std::vector<GestureBinding>(bindings.begin(), bindings.end());
    };

    for (const auto& entry : kTypeDefaults) {
      if (entry.type == type) {
        return collect(entry.bindings);
      }
    }
    return pluginGestureDefaults(type);
  }

  GestureMask reservedGesturesForType(std::string_view type) noexcept {
    for (const auto& entry : kTypeReserved) {
      if (entry.type == type) {
        return entry.gestures;
      }
    }
    return {};
  }

  std::unordered_map<std::string, std::string>
  defaultActionsForType(std::string_view type, const WidgetConfig* config) {
    std::unordered_map<std::string, std::string> actions;
    const auto apply = [&actions](std::span<const GestureBinding> bindings) {
      for (const auto& binding : bindings) {
        actions[std::string(gestureConfigKey(binding.gesture))] = std::string(binding.action);
      }
    };
    apply(builtinGestureDefaults());
    const auto typeDefaults = gestureDefaultsForType(type, config);
    apply(typeDefaults);
    return actions;
  }

} // namespace noctalia::bar
