#include "shell/bar/widget_factory.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "shell/bar/widget_custom_image.h"
#include "shell/bar/widgets/battery_widget.h"
#include "shell/bar/widgets/battery_widget_definition.h"
#include "shell/bar/widgets/clock_widget.h"
#include "shell/bar/widgets/clock_widget_definition.h"
#include "shell/bar/widgets/keyboard_layout_widget.h"
#include "shell/bar/widgets/plugin_widget.h"
#include "shell/bar/widgets/spacer_widget.h"
#include "shell/bar/widgets/spacer_widget_definition.h"
#include "shell/bar/widgets/workspaces_widget.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
  constexpr Logger kLog("shell");

  template <typename T, typename... Args> std::unique_ptr<Widget> createWidget(float contentScale, Args&&... args) {
    auto widget = std::make_unique<T>(std::forward<Args>(args)...);
    widget->setContentScale(contentScale);
    return widget;
  }

  WidgetCustomImage customImageFor(const WidgetConfig* wc) {
    if (wc == nullptr) {
      return {};
    }
    return widget_custom_image::fromConfig(
        wc->getString("custom_image", ""), wc->getBool("custom_image_colorize", false)
    );
  }

} // namespace

WidgetFactory::WidgetFactory(const BarServices& services)
    : m_platform(services.platform), m_configService(services.config), m_config(services.config.config()),
      m_notifications(services.notifications), m_audio(services.audio), m_easyEffects(services.easyEffects),
      m_upower(services.upower), m_sysmon(services.sysmon), m_powerProfiles(services.powerProfiles),
      m_network(services.network), m_externalIp(services.externalIp), m_idleInhibitor(services.idleInhibitor),
      m_mpris(services.mpris), m_audioSpectrum(services.audioSpectrum), m_httpClient(services.httpClient),
      m_weather(services.weather), m_nightLight(services.nightLight), m_themeService(services.theme),
      m_bluetooth(services.bluetooth), m_brightness(services.brightness), m_lockKeys(services.lockKeys),
      m_clipboard(services.clipboard), m_fileWatcher(services.fileWatcher), m_screenshots(services.screenshots),
      m_renderContext(services.renderContext), m_scriptApi(services.scriptApi) {
  scripting::PluginRegistry::instance().ensureScanned();
}

WidgetFactory::~WidgetFactory() = default;

std::unique_ptr<Widget> WidgetFactory::create(
    const std::string& name, wl_output* output, float contentScale, const std::string& barPosition,
    const std::string& barName
) const {
  // Resolve: if name matches a [widget.<name>] entry, use its type + settings.
  // Otherwise treat the name itself as the widget type with default settings.
  const WidgetConfig* wc = nullptr;
  std::string type = name;

  auto it = m_config.widgets.find(name);
  if (it != m_config.widgets.end()) {
    wc = &it->second;
    type = it->second.type;
  }

  // Config path prefix used when a widget definition reports a bad setting value.
  const std::string settingContext = std::format("widget.{}", name);

  if (type == "battery") {
    return createWidget<BatteryWidget>(
        contentScale, m_upower, m_bluetooth,
        batteryWidgetDefinition().resolve(
            wc, settingContext, BatteryWidgetDefinitionContext{.batteryConfig = &m_config.battery, .upower = m_upower}
        )
    );
  }

  if (type == "clock") {
    return createWidget<ClockWidget>(contentScale, output, clockWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "keyboard_layout") {
    const std::string display = wc != nullptr ? wc->getString("display", "short") : std::string("short");
    const bool showGlyph = wc != nullptr ? wc->getBool("show_glyph", true) : true;
    const bool showLabel = wc != nullptr ? wc->getBool("show_label", true) : true;
    const bool hideWhenSingleLayout = wc != nullptr ? wc->getBool("hide_when_single_layout", false) : false;
    auto customLabels =
        wc != nullptr ? wc->getStringMap("custom_labels") : std::unordered_map<std::string, std::string>{};
    std::string glyph = wc != nullptr ? wc->getString("glyph", "keyboard") : std::string{"keyboard"};
    if (glyph.empty()) {
      glyph = "keyboard";
    }
    auto widget = std::make_unique<KeyboardLayoutWidget>(
        m_platform, KeyboardLayoutWidget::parseDisplayMode(display), showGlyph, showLabel, hideWhenSingleLayout,
        std::move(customLabels), std::move(glyph), customImageFor(wc)
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (auto pluginEntry = scripting::PluginRegistry::instance().resolve(type);
      pluginEntry.has_value() && pluginEntry->entry->kind == scripting::PluginEntryKind::Widget) {
    if (m_scriptApi == nullptr) {
      return nullptr;
    }
    const auto* outputInfo = m_platform.findOutputByWl(output);
    const std::string outputName = outputInfo != nullptr ? outputInfo->connectorName : std::string{};
    std::unordered_map<std::string, WidgetSettingValue> overrides;
    if (wc != nullptr) {
      overrides = wc->settings;
      for (const auto& field : pluginEntry->entry->settings) {
        if (field.type != scripting::ManifestFieldType::StringMap) {
          continue;
        }
        if (const auto tableIt = wc->tables.find(field.key); tableIt != wc->tables.end()) {
          overrides.insert_or_assign(field.key, tableIt->second);
        }
      }
    }
    auto seeded = scripting::seedEntrySettings(*pluginEntry->entry, overrides);
    const auto& pluginSettings = m_config.plugins.pluginSettings;
    const auto psIt = pluginSettings.find(pluginEntry->manifest->id);
    static const std::unordered_map<std::string, WidgetSettingValue> kNoPluginOverrides;
    scripting::mergePluginSettings(
        *pluginEntry->manifest, psIt != pluginSettings.end() ? psIt->second : kNoPluginOverrides, seeded
    );
    auto widget = std::make_unique<PluginWidget>(
        scripting::PluginRuntimeContext{
            .entryId = pluginEntry->fullId(),
            .sourcePath = pluginEntry->sourcePath,
            .settings = std::move(seeded),
            .scriptApi = *m_scriptApi,
            .fileWatcher = m_fileWatcher,
            .httpClient = m_httpClient,
            .clipboard = m_clipboard,
            .platform = &m_platform,
            .audioSpectrum = m_audioSpectrum,
            .mpris = m_mpris,
        },
        barName, outputName, wc != nullptr ? wc->getBool("enable_scroll", true) : true
    );
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "spacer") {
    const bool verticalBar = barPosition == "left" || barPosition == "right";
    return createWidget<SpacerWidget>(contentScale, verticalBar, spacerWidgetDefinition().resolve(wc, settingContext));
  }

  if (type == "workspaces") {
    const std::string display = wc != nullptr ? wc->getString("display", "id") : std::string("id");
    const ColorSpec focusedColor = wc != nullptr
        ? wc->getColorSpec("focused_color", colorSpecFromRole(ColorRole::Primary), "widget." + name + ".focused_color")
        : colorSpecFromRole(ColorRole::Primary);
    const ColorSpec occupiedColor = wc != nullptr
        ? wc->getColorSpec(
              "occupied_color", colorSpecFromRole(ColorRole::Secondary), "widget." + name + ".occupied_color"
          )
        : colorSpecFromRole(ColorRole::Secondary);
    const ColorSpec emptyColor = wc != nullptr
        ? wc->getColorSpec("empty_color", colorSpecFromRole(ColorRole::Secondary), "widget." + name + ".empty_color")
        : colorSpecFromRole(ColorRole::Secondary);
    const ColorSpec urgentColor = wc != nullptr
        ? wc->getColorSpec("urgent_color", colorSpecFromRole(ColorRole::Error), "widget." + name + ".urgent_color")
        : colorSpecFromRole(ColorRole::Error);
    WorkspacesWidget::DisplayMode displayMode = WorkspacesWidget::DisplayMode::Id;
    if (display == "id") {
      displayMode = WorkspacesWidget::DisplayMode::Id;
    } else if (display == "name") {
      displayMode = WorkspacesWidget::DisplayMode::Name;
    } else if (display == "none") {
      displayMode = WorkspacesWidget::DisplayMode::None;
    }
    std::size_t maxLabelChars = 1; // Default: truncate names to 1 char (v4 behavior)
    if (wc != nullptr && wc->hasSetting("max_label_chars")) {
      maxLabelChars = static_cast<std::size_t>(wc->getInt("max_label_chars", 1));
    }
    const std::string workspaceStyle = wc != nullptr ? wc->getString("style", "regular") : "regular";
    WorkspacesWidget::Options options{
        .displayMode = displayMode,
        .focusedColor = focusedColor,
        .occupiedColor = occupiedColor,
        .emptyColor = emptyColor,
        .urgentColor = urgentColor,
        .changeColorOnHover = wc != nullptr ? wc->getBool("change_color_on_hover", true) : true,
        .maxLabelChars = maxLabelChars,
        .labelsOnlyWhenOccupied = wc != nullptr ? wc->getBool("labels_only_when_occupied", false) : false,
        .hideWhenEmpty = wc != nullptr ? wc->getBool("hide_when_empty", false) : false,
        .pillScale = static_cast<float>(wc != nullptr ? wc->getDouble("pill_scale", 1.0) : 1.0),
        .labelScale = static_cast<float>(wc != nullptr ? wc->getDouble("label_size", 1.0) : 1.0),
        .activePillSize = static_cast<float>(wc != nullptr ? wc->getDouble("active_pill_size", 2.2) : 2.2),
        .inactivePillSize = static_cast<float>(wc != nullptr ? wc->getDouble("inactive_pill_size", 1.0) : 1.0),
        .minimal = workspaceStyle == "minimal",
        .focusedPill = workspaceStyle == "focus_hint",
        .focusedOutputOnly = wc != nullptr ? wc->getBool("focused_output_only", false) : false,
    };
    auto widget = std::make_unique<WorkspacesWidget>(m_platform, m_configService, output, options);
    widget->setContentScale(contentScale);
    return widget;
  }

  kLog.warn("widget factory: unknown widget \"{}\"", name);
  return nullptr;
}
