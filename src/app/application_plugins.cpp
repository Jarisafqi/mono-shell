#include "application.h"
#include "core/log.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/panel/plugin_panel.h"

void Application::reloadPluginPanels() {
  // Retire the previously registered plugin panels (closing any that are open),
  // then register the current enabled set under their canonical full ids.
  for (const auto& id : m_pluginPanelIds) {
    m_panelManager.unregisterPanel(id);
  }
  m_pluginPanelIds.clear();

  auto& registry = scripting::PluginRegistry::instance();
  const auto& pluginSettings = m_configService.config().plugins.pluginSettings;
  static const std::unordered_map<std::string, WidgetSettingValue> kNoOverrides;

  for (const auto& resolved : registry.entriesOfKind(scripting::PluginEntryKind::Panel)) {
    if (resolved.entry == nullptr || resolved.manifest == nullptr) {
      continue;
    }
    // Panels are singletons: plugin_settings holds both plugin-level and entry-level keys.
    const auto psIt = pluginSettings.find(resolved.manifest->id);
    const auto& overrides = psIt != pluginSettings.end() ? psIt->second : kNoOverrides;
    auto seeded = scripting::seedEntrySettings(*resolved.entry, overrides);
    scripting::mergePluginSettings(*resolved.manifest, overrides, seeded);
    const auto shellConfig = scripting::resolvePluginPanelShellConfig(*resolved.entry, seeded);

    std::string fullId = resolved.fullId();
    m_panelManager.registerPanel(
        fullId,
        std::make_unique<PluginPanel>(
            scripting::PluginRuntimeContext{
                .entryId = fullId,
                .sourcePath = resolved.sourcePath,
                .settings = std::move(seeded),
                .scriptApi = m_scriptApi,
                .fileWatcher = &m_fileWatcher,
                .httpClient = &m_httpClient,
                .clipboard = &m_clipboardService,
            },
            PluginPanelOptions{
                .width = resolved.entry->panelWidth,
                .height = resolved.entry->panelHeight,
                .widthFill = resolved.entry->panelWidthFill,
                .heightFill = resolved.entry->panelHeightFill,
                .dismissOnOutsideClick = resolved.entry->panelDismissOnOutsideClick,
                .keyboardFocus = resolved.entry->panelKeyboardFocus,
                .persistent = resolved.entry->panelPersistent,
                .captureKeys = resolved.entry->panelCaptureKeys,
                .shellConfig = shellConfig,
            }
        )
    );
    m_pluginPanelIds.push_back(std::move(fullId));
  }
}

void Application::runPluginAutoUpdate() {
  // With [plugins].auto_update on, pull every git source in the background. Each update
  // runs off-thread and serializes per-source via a source lock, so a tick that overlaps
  // a still-running update queues behind it rather than racing (at 6h it never does).
  if (!m_configService.config().plugins.autoUpdate) {
    return;
  }
  m_pluginManager.updateAll();
}
