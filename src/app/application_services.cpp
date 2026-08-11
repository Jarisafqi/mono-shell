#include "app/main_loop.h"
#include "application.h"
#include "application_internal.h"
#include "compositors/compositor_detect.h"
#include "config/config_types.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/idle/screensaver_poll_source.h"
#include "dbus/idle/screensaver_service.h"
#include "dbus/logind/logind_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/iwd_secret_agent.h"
#include "dbus/network/iwd_service.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/wpa_supplicant_service.h"
#include "dbus/notification/kde_notification_client.h"
#include "dbus/notification/notification_dbus_host.h"
#include "dbus/notification/notification_service.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus.h"
#include "dbus/system_bus_poll_source.h"
#include "dbus/tray/tray_service.h"
#include "dbus/upower/upower_service.h"
#include "debug/debug_service.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "ipc/ipc_arg_parse.h"
#include "notification/notifications.h"
#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "pipewire/pipewire_spectrum_poll_source.h"
#include "pipewire/sound_player.h"
#include "pipewire/wireplumber_mixer.h"
#include "render/animation/motion_service.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"
#include "render/text/font_weight_catalog.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "scripting/script_runtime.h"
#include "shell/panel/plugin_panel.h"
#include "shell/session/session_ipc.h"
#include "shell/session/session_panel.h"
#include "shell/test/test_panel.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/brightness_poll_source.h"
#include "system/brightness_service.h"
#include "system/distro_info.h"
#include "system/easyeffects_service.h"
#include "system/keyboard_backlight_service.h"
#include "system/system_monitor_service.h"
#include "ui/app_icon_colorization.h"
#include "ui/controls/input.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <malloc.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
  constexpr Logger kLog("app");
  constexpr std::string_view kSecretServiceBusName = "org.freedesktop.secrets";

  void signal_handler(int signum) {
    if (signum == SIGTERM || signum == SIGINT) {
      scripting::ScriptRuntime::setShutdownSignal(signum);
      Application::s_shutdownRequested = true;
    }
  }
} // namespace

void Application::scheduleNotificationShellRefresh() {
  if (m_notificationShellRefreshScheduled) {
    return;
  }
  m_notificationShellRefreshScheduled = true;
  DeferredCall::callLater([this]() {
    m_notificationShellRefreshScheduled = false;
    m_bar.refresh();
  });
}

void Application::syncNotificationDaemon() {
  if (m_bus == nullptr) {
    m_notificationPollSource.setDbusService(nullptr);
    m_notificationDbus.reset();
    m_notificationDaemonEnabled.reset();
    m_notificationDaemonInitFailed = false;
    return;
  }

  const bool enabled = m_configService.config().notification.enableDaemon;
  const bool enabledChanged = !m_notificationDaemonEnabled.has_value() || *m_notificationDaemonEnabled != enabled;
  m_notificationDaemonEnabled = enabled;

  if (!enabled) {
    if (m_notificationDbus != nullptr) {
      kLog.info("notification daemon disabled by config");
    }
    m_notificationPollSource.setDbusService(nullptr);
    m_notificationDbus.reset();
    m_notificationDaemonInitFailed = false;
    return;
  }

  if (m_notificationDbus != nullptr) {
    if (m_notificationDbus->isHealthy()) {
      m_notificationDaemonInitFailed = false;
      return;
    }
    kLog.info("notification daemon connection lost; re-registering");
    m_notificationPollSource.setDbusService(nullptr);
    m_notificationDbus.reset();
  }

  if (m_notificationDaemonInitFailed && !enabledChanged) {
    return;
  }

  try {
    m_notificationDbus = std::make_unique<NotificationService>(*m_bus, m_notificationManager);
    m_notificationPollSource.setDbusService(m_notificationDbus.get());
    m_notificationDaemonInitFailed = false;
    kLog.info("listening on org.freedesktop.Notifications");
  } catch (const std::exception& ownerError) {
    if (compositors::isKde()) {
      try {
        m_notificationDbus = std::make_unique<KdeNotificationClient>(*m_bus, m_notificationManager);
        m_notificationPollSource.setDbusService(m_notificationDbus.get());
        m_notificationDaemonInitFailed = false;
        kLog.info("listening on KDE Plasma notifications via NotificationWatcher");
        return;
      } catch (const std::exception& kdeError) {
        kLog.warn("notifications disabled: {}", kdeError.what());
        m_notificationDbus.reset();
        m_notificationPollSource.setDbusService(nullptr);
        m_notificationDaemonInitFailed = true;
        m_notificationManager.addInternal(
            "Mono Shell", i18n::tr("notifications.internal.dbus-disabled"), kdeError.what(), Urgency::Low
        );
        return;
      }
    }

    kLog.warn("notifications disabled: {}", ownerError.what());
    m_notificationDbus.reset();
    m_notificationPollSource.setDbusService(nullptr);
    m_notificationDaemonInitFailed = true;
    m_notificationManager.addInternal(
        "Mono Shell", i18n::tr("notifications.internal.dbus-disabled"), ownerError.what(), Urgency::Low
    );
  }
}

void Application::installNotificationBusNameWatch() {
  if (m_notificationBusNameWatchInstalled || m_bus == nullptr) {
    return;
  }

  m_notificationBusNameWatchProxy = sdbus::createProxy(
      m_bus->connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
  );
  m_notificationBusNameWatchProxy->uponSignal("NameOwnerChanged")
      .onInterface("org.freedesktop.DBus")
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& /*newOwner*/) {
        if (name != notification_dbus::kFreedesktopNotificationsBusName) {
          return;
        }
        DeferredCall::callLater([this]() {
          m_notificationDaemonInitFailed = false;
          syncNotificationDaemon();
        });
      });
  m_notificationBusNameWatchInstalled = true;
}

void Application::installSecretServiceNameWatch() {
  if (m_secretServiceNameWatchInstalled || m_bus == nullptr) {
    return;
  }

  m_secretServiceNameWatchProxy = sdbus::createProxy(
      m_bus->connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
  );
  m_secretServiceNameWatchProxy->uponSignal("NameOwnerChanged")
      .onInterface("org.freedesktop.DBus")
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name != kSecretServiceBusName) {
          return;
        }
        m_secretServiceOwned = !newOwner.empty();
        if (!m_secretServiceOwned) {
          return;
        }
        // A provider that just showed up is a fresh chance for every consumer that gave up.
        m_storageKeyAutoRetried = false;
        m_calendarCredentialAutoRetried = false;
        kLog.info("secret service provider appeared on {}", kSecretServiceBusName);
        DeferredCall::callLater([this]() { retrySecretServiceConsumers(); });
      });
  m_secretServiceNameWatchInstalled = true;

  // The provider may have claimed the name between our first lookup and this watch. Consumers are
  // still opening at this point, so their change callbacks drive the actual retry.
  try {
    bool hasOwner = false;
    m_secretServiceNameWatchProxy->callMethod("NameHasOwner")
        .onInterface("org.freedesktop.DBus")
        .withArguments(std::string{kSecretServiceBusName})
        .storeResultsTo(hasOwner);
    m_secretServiceOwned = hasOwner;
  } catch (const sdbus::Error& e) {
    kLog.debug("secret service NameHasOwner failed: {}", e.what());
  }
}

void Application::retrySecretServiceConsumers() {
  if (!m_secretServiceOwned) {
    return;
  }
  if (!m_storageKeyAutoRetried && m_storageKeyProvider.state() == security::StorageKeyState::Unavailable) {
    m_storageKeyAutoRetried = true;
    kLog.info("secret service is running; reopening encrypted storage");
    DeferredCall::callLater([this]() { m_storageKeyProvider.retry(); });
  }
  if (!m_calendarCredentialAutoRetried
      && m_calendarService.credentialState() == calendar::CredentialState::Unavailable) {
    m_calendarCredentialAutoRetried = true;
    kLog.info("secret service is running; reopening calendar credentials");
    DeferredCall::callLater([this]() { m_calendarService.retryCredentialMigration(); });
  }
}

void Application::syncScreenTimeService() {
  m_screenTimeService.setEnabled(m_configService.config().shell.screenTimeEnabled);
}

void Application::syncClipboardService() {
  const bool enabled = m_configService.config().shell.clipboardEnabled;

  // The live clipboard transport (read current selection + set selection)
  // stays active regardless of config so basic copy/paste keeps working in
  // every text field. The toggle only controls history retention/persistence
  // and the history UI.
  m_wayland.setClipboardService(&m_clipboardService);
  Input::setTextClipboard(&m_clipboardService);
  m_clipboardService.setHistoryRetentionEnabled(enabled);
  // Taking the selection over when its owner exits belongs to that same live
  // transport, so it follows its own setting rather than history retention.
  m_clipboardService.setKeepFromClosedApps(m_configService.config().shell.clipboardKeepFromClosedApps);
  m_clipboardService.setMaxHistoryEntries(
      static_cast<std::size_t>(m_configService.config().shell.clipboardHistoryMaxEntries)
  );

  if (!enabled) {
    kLog.info("clipboard history disabled by config (live copy/paste still active)");
  }

  m_bar.refresh();
}

void Application::syncStorageKeyProvider() {
  const StorageConfig& storage = m_configService.config().storage;
  const bool encryptedDataExists =
      m_clipboardService.hasEncryptedPersistence() || m_calendarService.hasEncryptedCache();
  m_storageKeyProvider.configure(storage.keySource, storage.keyFile, encryptedDataExists);
}

void Application::initServices() {
  if (!security::initializeSecurityPrimitives()) {
    kLog.error("libsodium initialization failed; encrypted persistence is unavailable");
  }
  m_storageKeyProvider.setChangeCallback([this]() {
    m_clipboardService.syncPersistence();
    m_calendarService.syncCachePersistence();
    retrySecretServiceConsumers();
  });
  syncStorageKeyProvider();
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().storage) {
          syncStorageKeyProvider();
        }
      },
      "storage-key"
  );
  m_secretStore.retryAvailabilityCheck();
  initStyleThemeAndWayland();
  initWaylandCallbacks();
  initAuxServicesAndHooks();
  initSystemBusServices();
  initBrightnessAndPipewire();
  initSessionBusServices();
}

void Application::initStyleThemeAndWayland() {
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGINT, signal_handler);

  auto applyMotionConfig = [this]() {
    auto& motion = MotionService::instance();
    motion.setSpeed(m_configService.config().shell.animation.speed);
    motion.setEnabled(m_configService.config().shell.animation.enabled);
  };
  auto applyStyleConfig = [this, lastCornerRadiusScale = std::numeric_limits<float>::quiet_NaN()]() mutable {
    const float corner = m_configService.config().shell.cornerRadiusScale;
    const bool cornerChanged =
        std::isfinite(lastCornerRadiusScale) && std::abs(corner - lastCornerRadiusScale) > 1.0e-4f;
    Style::setCornerRadiusScale(corner);
    Style::setButtonBordersEnabled(m_configService.config().shell.buttonBorders);
    Style::setInputBordersEnabled(m_configService.config().shell.inputBorders);
    Style::setPopupBordersEnabled(m_configService.config().shell.popupBorders);
    Style::setPopupShadowsEnabled(m_configService.config().shell.popupShadows);
    Style::setCardBordersEnabled(m_configService.config().shell.cardBorders);
    lastCornerRadiusScale = corner;
    if (cornerChanged) {
      m_notificationToast.requestLayout();
      m_panelManager.requestLayout();
    }
  };
  auto applyPasswordMaskStyle = [this]() {
    const auto style = m_configService.config().shell.passwordMaskStyle == PasswordMaskStyle::RandomIcons
        ? Input::PasswordMaskStyle::RandomIcons
        : Input::PasswordMaskStyle::CircleFilled;
    Input::setPasswordMaskStyle(style);
  };
  applyMotionConfig();
  applyStyleConfig();
  applyPasswordMaskStyle();
  m_httpClient.setOfflineMode(m_configService.config().shell.offlineMode);
  m_configService.addReloadCallback(applyMotionConfig);
  m_configService.addReloadCallback(applyStyleConfig);
  m_configService.addReloadCallback(applyPasswordMaskStyle);
  m_configService.addReloadCallback([this]() {
    m_httpClient.setOfflineMode(m_configService.config().shell.offlineMode);
  });
  m_configService.addReloadCallback([this]() { syncClipboardService(); });
  m_configService.addReloadCallback([this]() { syncScreenTimeService(); });
  m_communityPaletteService.setReadyCallback([this]() {
    // A refreshed catalog may carry a new md5 for the selected palette; re-resolve
    // so a stale cached palette gets re-downloaded and faded in.
    m_themeService.onConfigReload();
  });
  m_communityPaletteService.sync();
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().theme) {
          m_communityPaletteService.sync();
        }
      },
      "community-palette-sync"
  );
  m_communityTemplateService.setReadyCallback([this]() {
    if (m_configService.config().theme.templates.enableCommunityTemplates) {
      m_themeService.onConfigReload();
    }
  });
  m_communityTemplateService.sync(m_configService.config().theme.templates);
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().theme) {
          m_communityTemplateService.sync(m_configService.config().theme.templates);
        }
      },
      "community-template-sync"
  );

  // i18n has no dependencies on other services and must be ready before any
  // UI construction reads a translated string.
  i18n::Service::instance().init(m_configService.config().shell.lang);
  m_configService.addReloadCallback([this]() {
    i18n::Service::instance().setLanguage(m_configService.config().shell.lang);
  });

  // Apply theme before any UI constructs palette-dependent scene nodes.
  auto syncScriptApiWallpaperDirectory = [this]() {
    const ThemeMode mode = m_themeService.resolvedMode() == "light" ? ThemeMode::Light : ThemeMode::Dark;
    m_scriptApi.setWallpaperDirectory(
        wallpaper::resolveGlobalWallpaperDirectory(m_configService.config().wallpaper, mode)
    );
  };

  auto syncScriptApiShellTimeFormats = [this]() {
    m_scriptApi.setTimeFormat(m_configService.config().shell.timeFormat);
    m_scriptApi.setDateFormat(m_configService.config().shell.dateFormat);
  };

  // Publish the connected outputs to plugin scripts (mono-shell.outputs()), refreshed on every
  // output change so the worker-thread binding reads a race-free copy.
  m_syncScriptApiOutputs = [this]() {
    std::vector<scripting::ScriptOutputInfo> infos;
    wl_output* const focused = m_compositorPlatform.preferredInteractiveOutput();
    for (const auto& out : m_wayland.outputs()) {
      if (!out.done || out.connectorName.empty()) {
        continue;
      }
      infos.push_back({
          .name = out.connectorName,
          .description = out.description,
          .width = out.effectiveLogicalWidth(),
          .height = out.effectiveLogicalHeight(),
          .x = out.logicalX,
          .y = out.logicalY,
          .scale = out.scale,
          .focused = out.output == focused,
      });
    }
    m_scriptApi.setOutputs(std::move(infos));
  };
  m_syncScriptApiOutputs();

  // Let a plugin (e.g. mpvpaper) take over an output's wallpaper surface.

  // Let a plugin apply a wallpaper image (e.g. wallhaven)

  // Let a plugin toggle one of its own panels.
  m_scriptApi.setTogglePanelHook([this](const std::string& panelId) { m_panelManager.togglePanel(panelId); });

  m_scriptApi.setOpenPluginSettingsHook([this](const std::string& pluginId) {
    if (!m_panelManager.openPluginSettings(pluginId)) {
      kLog.warn("plugin openSettings ignored: \"{}\" has no settings", pluginId);
    }
  });

  m_themeService.setResolvedCallback([this, lastResolvedThemeMode = std::optional<std::string>{},
                                      syncScriptApiWallpaperDirectory](
                                         const noctalia::theme::GeneratedPalette& generated, std::string_view mode
                                     ) mutable {
    const std::string resolvedMode(mode);
    const std::string configuredMode(enumToKey(kThemeModes, m_themeService.configuredMode()));
    m_scriptApi.setDarkMode(resolvedMode != "light");
    syncScriptApiWallpaperDirectory();
    const std::optional<std::string> previousMode = lastResolvedThemeMode;
    lastResolvedThemeMode = resolvedMode;
    m_templateApplyService.setAfterApplyCallback([this]() { m_hookManager.fire(HookKind::ColorsChanged); });
    m_templateApplyService.apply(generated, mode);
    if (previousMode.has_value() && *previousMode != resolvedMode) {
      m_hookManager.fire(
          HookKind::ThemeModeChanged,
          {{"MONO_SHELL_THEME_MODE", resolvedMode},
           {"MONO_SHELL_THEME_MODE_PREVIOUS", *previousMode},
           {"MONO_SHELL_THEME_MODE_CONFIGURED", configuredMode}}
      );
    }
  });
  m_themeService.apply();
  syncScriptApiWallpaperDirectory();
  syncScriptApiShellTimeFormats();
  m_configService.addReloadCallback([this]() { m_themeService.onConfigReload(); }, "theme");
  m_configService.addReloadCallback(syncScriptApiWallpaperDirectory, "wallpaper");
  m_configService.addReloadCallback(syncScriptApiShellTimeFormats, "shell-time-formats");
  {
    static ShellAppIconColorizationSettings lastAppIconColorization =
        shellAppIconColorizationSettings(m_configService.config().shell);
    m_configService.addReloadCallback(
        [this]() {
          const auto current = shellAppIconColorizationSettings(m_configService.config().shell);
          if (current == lastAppIconColorization) {
            return;
          }
          lastAppIconColorization = current;
          notifyShellAppIconColorizationChanged();
        },
        "app-icon-colorization"
    );
  }

  if (!m_wayland.connect()) {
    throw std::runtime_error("failed to connect to Wayland display");
  }
  m_compositorPlatform.initialize();
  m_screenTimeService.initialize(&m_wayland);
  syncScreenTimeService();
  if (m_configService.config().shell.disableMipmaps) {
    TextureManager::setGlobalMipmapsEnabled(false);
  }
  m_glShared.initialize(m_wayland.display(), m_configService.config().shell.sharedGlContext);
  auto* sharedGlPtr = m_glShared.hasSharedContext() ? &m_glShared : nullptr;
  m_sharedTextureCache.initialize(sharedGlPtr);
  m_asyncTextureCache.initialize(sharedGlPtr);
  m_wayland.setTextInputService(&m_textInputService);
  m_wayland.setVirtualKeyboardService(&m_virtualKeyboardService);

  auto bindKeybind = [this](KeybindAction action) {
    return [this, action](std::uint32_t sym, std::uint32_t modifiers) {
      return m_configService.matchesKeybind(action, sym, modifiers);
    };
  };
  KeybindMatcher::setMatcher(KeybindAction::Validate, bindKeybind(KeybindAction::Validate));
  KeybindMatcher::setMatcher(KeybindAction::Cancel, bindKeybind(KeybindAction::Cancel));
  KeybindMatcher::setMatcher(KeybindAction::Left, bindKeybind(KeybindAction::Left));
  KeybindMatcher::setMatcher(KeybindAction::Right, bindKeybind(KeybindAction::Right));
  KeybindMatcher::setMatcher(KeybindAction::Up, bindKeybind(KeybindAction::Up));
  KeybindMatcher::setMatcher(KeybindAction::Down, bindKeybind(KeybindAction::Down));
  KeybindMatcher::setMatcher(KeybindAction::TabNext, bindKeybind(KeybindAction::TabNext));
  KeybindMatcher::setMatcher(KeybindAction::TabPrevious, bindKeybind(KeybindAction::TabPrevious));
  KeybindMatcher::setMatcher(KeybindAction::Copy, bindKeybind(KeybindAction::Copy));
  KeybindMatcher::setMatcher(KeybindAction::Save, bindKeybind(KeybindAction::Save));
  KeybindMatcher::setMatcher(KeybindAction::Delete, bindKeybind(KeybindAction::Delete));

  Input::setValidateKeyMatcher([this](std::uint32_t sym, std::uint32_t modifiers) {
    return m_configService.matchesKeybind(KeybindAction::Validate, sym, modifiers);
  });
}

void Application::reconcileOutputSurfaces() {
  // Canonical bottom-to-top (re)creation order for per-output layer surfaces.
  // This is the ONLY place this order is defined: it runs once after initUi()
  // wiring for first creation and again on every output change, so same-layer
  // stacking (e.g. screen corners above the dock) is identical in both cases.
  // Each owner's onOutputChange() reconciles idempotently against the current
  // output set, so re-running it is safe. initialize() only wires dependencies.
  m_bar.onOutputChange();
  m_idleGraceOverlay.onOutputChange();
  m_idleInhibitor.onOutputChange();
  m_screenshotService.onOutputChange();
  m_notificationToast.onOutputChange();
  m_osdOverlay.onOutputChange();
}

void Application::initWaylandCallbacks() {
  m_wayland.setOutputChangeCallback([this]() {
    if (m_syncScriptApiOutputs) {
      m_syncScriptApiOutputs();
    }
    if (m_brightnessService != nullptr) {
      m_brightnessService->onOutputsChanged();
    }
    m_gammaService.onOutputsChanged();
    m_pluginServiceHost.onOutputChange();
    reconcileOutputSurfaces();
  });
  m_clipboardService.setChangeCallback([this]() { m_scriptApi.setClipboardText(m_clipboardService.clipboardText()); });
  m_scriptApi.setClipboardText(m_clipboardService.clipboardText());
  m_compositorPlatform.setWorkspaceAlertService(&m_workspaceAlertService);
  m_compositorPlatform.setWorkspaceChangeCallback([this]() {
    // Clear alerts for the workspace the user just switched to. Limit to the
    // focused output so activity on one monitor doesn't dismiss alerts on
    // another; fall back to all outputs when no focused output is known.
    if (wl_output* output = m_compositorPlatform.preferredInteractiveOutput(); output != nullptr) {
      (void)m_compositorPlatform.clearActiveWorkspaceAlerts(output);
    } else {
      (void)m_compositorPlatform.clearActiveWorkspaceAlerts();
    }
    m_bar.onWorkspaceChanged();
    m_bar.refresh();
  });
  m_compositorPlatform.setKeyboardLayoutChangeCallback([this]() {
    m_bar.refresh();
    if (m_configService.config().osd.kinds.keyboardLayout) {
      m_keyboardLayoutOsd.onLayoutChanged(m_compositorPlatform, m_configService.config());
    }
  });
  m_compositorPlatform.setToplevelChangeCallback([this]() {
    m_screenTimeService.onFocusChange();
    m_bar.scheduleSmartAutoHideReevaluation();
    m_bar.refresh();
    if (m_wayland.hasPointerPosition() && !m_wayland.activeToplevel().has_value()) {
      const std::uint32_t serial = m_wayland.lastInputSerial();
      if (serial != 0) {
        m_wayland.setCursorShape(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      }
    }
  });
  if constexpr (kLockKeysEnabled) {
    if (lockKeysConsumersEnabled(m_configService.config())) {
      m_lockKeysService.refreshNow();
    }
    m_lockKeysService.setChangeCallback(
        [this](const WaylandSeat::LockKeysState& previous, const WaylandSeat::LockKeysState& current) {
          const Config& config = m_configService.config();
          if (config.osd.kinds.lockKeys) {
            m_lockKeysOsd.onLockKeysChanged(previous, current);
          }
        }
    );
  }
  m_idleInhibitor.initialize(m_wayland);
  m_idleInhibitor.setChangeCallback([this]() {
    if (m_configService.config().osd.kinds.caffeine) {
      m_osdOverlay.show(caffeineOsdContent(m_idleInhibitor.enabled()));
    }
    m_bar.refresh();
  });
}

void Application::initAuxServicesAndHooks() {
  m_hookManager.setCommandRunner([this](const std::string& command) { return runShellCommand(command); });
  m_hookManager.setBlockingCommandRunner([this](const std::string& command) {
    return runShellCommandBlocking(command);
  });
  m_hookManager.reload(m_configService.config().hooks);
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().hooks) {
          m_hookManager.reload(m_configService.config().hooks);
        }
      },
      "hooks"
  );
  m_gammaService.setLocationResolving(m_locationService.resolving());
  m_gammaService.reload(m_configService.config().nightlight, m_configService.config().location);
  m_gammaService.setChangeCallback([this]() {
    m_bar.refresh();
  });
  m_gammaService.setStateFeedbackCallback([this]() {
    if (m_configService.config().osd.kinds.nightlight) {
      m_osdOverlay.show(nightLightOsdContent(m_gammaService));
    }
  });
  m_configService.addReloadCallback([this]() {
    m_gammaService.reload(m_configService.config().nightlight, m_configService.config().location);
  });

  m_themeService.setChangeCallback([this]() { requestAllSurfacesRedraw(); });

  if (const auto distro = DistroDetector::detect(); distro.has_value()) {
    const auto& label = !distro->prettyName.empty() ? distro->prettyName
        : !distro->name.empty()                     ? distro->name
                                                    : distro->id;
    kLog.info("distro: {}", label);
  } else {
    kLog.info("distro: unknown");
  }

  try {
    m_systemMonitor = std::make_unique<SystemMonitorService>(m_configService.config().system.monitor);
    m_scriptApi.setSystemMonitor(m_systemMonitor.get());
    if (m_systemMonitor->isRunning()) {
      kLog.info("system monitor service active");
    } else {
      kLog.info("system monitor service disabled by config");
    }
    m_configService.addReloadCallback([this]() {
      if (m_systemMonitor == nullptr) {
        return;
      }

      const bool wasRunning = m_systemMonitor->isRunning();
      try {
        m_systemMonitor->applyConfig(m_configService.config().system.monitor);
      } catch (const std::exception& e) {
        kLog.warn("system monitor service failed to start: {}", e.what());
        return;
      }

      if (wasRunning != m_systemMonitor->isRunning()) {
        kLog.info("system monitor service {}", m_systemMonitor->isRunning() ? "active" : "disabled by config");
      }
      m_bar.refresh();
    });
  } catch (const std::exception& e) {
    kLog.warn("system monitor service disabled: {}", e.what());
    m_scriptApi.setSystemMonitor(nullptr);
    m_systemMonitor.reset();
  }
}

void Application::releaseSleepDelayInhibitIfPending() {
  if (!m_releaseSleepDelayWhenLocked) {
    return;
  }
  m_releaseSleepDelayWhenLocked = false;
  if (m_logindService != nullptr) {
    m_logindService->releaseSleepDelayInhibit();
  }
}

void Application::initSystemBusServices() {
  try {
    m_systemBus = std::make_unique<SystemBus>();
    kLog.info("connected to system bus");
  } catch (const std::exception& e) {
    kLog.warn("system dbus disabled: {}", e.what());
    m_systemBus.reset();
  }

  if (m_systemBus != nullptr) {
    if (m_systemBus->nameHasOwner("org.freedesktop.login1")) {
      try {
        m_logindService = std::make_unique<LogindService>(*m_systemBus);
        m_logindService->setPrepareForSleepCallback([this](bool sleeping) {
          // Idle grace overlay must not survive suspend; hide on both edges as a fallback when
          // fade-complete cleanup races with process freeze. No shell lock screen exists, so a
          // pending delay inhibit can never be cleared from a lock path — release it up front.
          m_idleGraceOverlay.hide();
          if (sleeping) {
            m_releaseSleepDelayWhenLocked = false;
            if (m_logindService != nullptr) {
              m_logindService->releaseSleepDelayInhibit();
            }
            return;
          }
          m_releaseSleepDelayWhenLocked = false;
          kLog.info("system resumed; rechecking night light schedule");
          m_gammaService.reevaluateSchedule();
          // BlueZ property-change signals can be missed across the suspend window, leaving our
          // cached adapter state stale. Re-sync now and again shortly after, since BlueZ may take a
          // moment to restore the adapter on resume.
          if (m_bluetoothService != nullptr) {
            m_bluetoothService->refresh();
            m_bluetoothResumeTimer.start(std::chrono::seconds(2), [this]() {
              if (m_bluetoothService != nullptr) {
                m_bluetoothService->refresh();
              }
            });
          }
          requestAllSurfacesRedraw();
        });
        kLog.info("logind sleep monitor active");
        m_idleInhibitor.setLogindService(m_logindService.get());
      } catch (const std::exception& e) {
        kLog.warn("logind sleep monitor disabled: {}", e.what());
        m_logindService.reset();
      }
    } else {
      kLog.info("logind not available on system bus; sleep monitor disabled");
    }

    try {
      m_accountsService = std::make_unique<AccountsService>(*m_systemBus);
      m_accountsService->setChangeCallback([this]() { m_panelManager.refresh(); });
      kLog.info("accounts service active for uid {}", m_accountsService->sessionUid());
    } catch (const std::exception& e) {
      kLog.info("accounts service disabled: {}", e.what());
      m_accountsService.reset();
    }

    try {
      m_powerProfilesService = std::make_unique<PowerProfilesService>(*m_systemBus);
      m_powerProfilesService->setChangeCallback(
          [this](const PowerProfilesState& state, PowerProfilesChangeOrigin origin) {
            m_bar.refresh();

            const std::string& active = state.activeProfile;
            if (active.empty()) {
              return;
            }
            onPowerProfileChangedForEvents(state, origin);
          }
      );
      if (m_powerProfilesService->hasStateSnapshot() && !m_powerProfilesService->activeProfile().empty()) {
        m_prevPowerProfileActiveForEvents = m_powerProfilesService->activeProfile();
        kLog.info("power profiles active profile: {}", m_powerProfilesService->activeProfile());
      } else if (!m_powerProfilesService->hasStateSnapshot()) {
        kLog.info("power profiles service active (state loading asynchronously)");
      } else {
        kLog.info("power profiles service active");
      }
    } catch (const std::exception& e) {
      kLog.warn("power profiles disabled: {}", e.what());
      m_powerProfilesService.reset();
    }

    try {
      m_upowerService = std::make_unique<UPowerService>(*m_systemBus);
      m_batteryHookState.reset(m_upowerService->state());
      m_batteryWarningMonitor.evaluate(m_configService.config().battery, *m_upowerService, m_notificationManager);
      m_upowerService->setChangeCallback([this]() {
        onUpowerStateChangedForHooks();
        m_batteryWarningMonitor.evaluate(m_configService.config().battery, *m_upowerService, m_notificationManager);
        if (m_bluetoothService != nullptr) {
          m_bluetoothService->refreshBatteryFromUPower();
        }
        m_bar.refresh();
      });
      m_configService.addReloadCallback(
          [this]() {
            if (m_configService.lastChange().battery && m_upowerService != nullptr) {
              m_batteryWarningMonitor.evaluate(
                  m_configService.config().battery, *m_upowerService, m_notificationManager
              );
              m_bar.refresh();
            }
          },
          "battery"
      );
    } catch (const std::exception& e) {
      kLog.warn("upower disabled: {}", e.what());
      m_upowerService.reset();
    }

    try {
      m_keyboardBacklightService = std::make_unique<KeyboardBacklightService>(*m_systemBus);
      m_keyboardBacklightService->setChangeCallback([this]() {
        m_keyboardBacklightOsd.onBrightnessChanged(*m_keyboardBacklightService);
      });
    } catch (const std::exception& e) {
      kLog.warn("keyboard backlight disabled: {}", e.what());
      m_keyboardBacklightService.reset();
    }

    try {
      m_networkService = std::make_unique<NetworkManagerService>(*m_systemBus);
      m_networkService->setChangeCallback(
          [this](const NetworkState& state, NetworkChangeOrigin origin) {
            onNetworkStateChangedForEvents(state, origin);
            m_externalIpService.onNetworkChanged();
            m_bar.refresh();
          }
      );
      if (m_networkService->hasStateSnapshot()) {
        m_prevWirelessEnabledForEvents = m_networkService->state().wirelessEnabled;
      }
      kLog.info("network service active");
    } catch (const std::exception& e) {
      kLog.warn("NetworkManager unavailable ({}), trying wpa_supplicant", e.what());
      try {
        m_networkService = std::make_unique<WpaSupplicantService>(*m_systemBus);
        m_networkService->setChangeCallback(
            [this](const NetworkState& state, NetworkChangeOrigin origin) {
              onNetworkStateChangedForEvents(state, origin);
              m_externalIpService.onNetworkChanged();
              m_bar.refresh();
            }
        );
        if (m_networkService->hasStateSnapshot()) {
          m_prevWirelessEnabledForEvents = m_networkService->state().wirelessEnabled;
        }
        kLog.info("network service active (wpa_supplicant)");
      } catch (const std::exception& e2) {
        kLog.warn("wpa_supplicant unavailable ({}), trying iwd", e2.what());
        try {
          m_networkService = std::make_unique<IwdService>(*m_systemBus);
          m_networkService->setChangeCallback(
              [this](const NetworkState& state, NetworkChangeOrigin origin) {
                onNetworkStateChangedForEvents(state, origin);
                m_externalIpService.onNetworkChanged();
                m_bar.refresh();
              }
          );
          if (m_networkService->hasStateSnapshot()) {
            m_prevWirelessEnabledForEvents = m_networkService->state().wirelessEnabled;
          }
          kLog.info("network service active (iwd)");
        } catch (const std::exception& e3) {
          kLog.warn("network service disabled: {}", e3.what());
          m_networkService.reset();
        }
      }
    }

    if (m_networkService != nullptr) {
      m_externalIpService.setNetworkService(m_networkService.get());
      m_externalIpService.setChangeCallback([this]() {
        m_bar.refresh();
      });
      m_externalIpService.onNetworkChanged();
    }
    m_configService.addReloadCallback([this]() { m_externalIpService.onConfigReload(); });

    if (m_networkService != nullptr && m_networkService->supportsSecretAgent()) {
      try {
        m_networkSecretAgent = std::make_unique<NetworkSecretAgent>(*m_systemBus);
      } catch (const std::exception& e) {
        kLog.warn("network secret agent disabled: {}", e.what());
        m_networkSecretAgent.reset();
      }
    }

    // Initialize iwd secret agent if iwd is the active network service
    if (auto* iwdService = dynamic_cast<IwdService*>(m_networkService.get())) {
      try {
        m_iwdSecretAgent = std::make_unique<IwdSecretAgent>(*m_systemBus);
        iwdService->setSecretAgent(m_iwdSecretAgent.get());
      } catch (const std::exception& e) {
        kLog.warn("iwd secret agent disabled: {}", e.what());
        m_iwdSecretAgent.reset();
      }
    }

    try {
      m_bluetoothService = std::make_unique<BluetoothService>(*m_systemBus, m_upowerService.get());
      auto refreshBluetoothUi = [this]() {
        m_bar.refresh();
      };
      m_bluetoothService->setStateCallback(
          [this, refreshBluetoothUi](const BluetoothState& state, BluetoothStateChangeOrigin origin) {
            onBluetoothStateChangedForEvents(state, origin);
            refreshBluetoothUi();
          }
      );
      m_bluetoothService->setDevicesCallback([refreshBluetoothUi](const std::vector<BluetoothDeviceInfo>& /*devices*/) {
        refreshBluetoothUi();
      });
      if (m_bluetoothService->hasStateSnapshot()) {
        m_prevBluetoothPoweredForEvents = m_bluetoothService->state().powered;
      }
      kLog.info("bluetooth service active");
    } catch (const std::exception& e) {
      kLog.warn("bluetooth service disabled: {}", e.what());
      m_bluetoothService.reset();
    }

    if (m_bluetoothService != nullptr) {
      try {
        m_bluetoothAgent = std::make_unique<BluetoothAgent>(*m_systemBus);
        m_bluetoothAgent->setRequestCallback([this](const BluetoothPairingRequest& /*request*/) {});
      } catch (const std::exception& e) {
        kLog.warn("bluetooth agent disabled: {}", e.what());
        m_bluetoothAgent.reset();
      }
    }
  }
}

void Application::initBrightnessAndPipewire() {
  try {
    m_brightnessService = std::make_unique<BrightnessService>(
        m_systemBus.get(), m_compositorPlatform, m_configService.config().brightness
    );
    m_brightnessService->setChangeCallback([this]() {
      m_brightnessOsd.onBrightnessChanged(*m_brightnessService);
      m_bar.refresh();
    });
    m_configService.addReloadCallback(
        [this]() {
          if (m_brightnessService == nullptr || !m_configService.lastChange().brightness) {
            return;
          }
          m_brightnessService->reload(m_configService.config().brightness);
          m_bar.refresh();
        },
        "brightness"
    );
  } catch (const std::exception& e) {
    kLog.warn("brightness service disabled: {}", e.what());
    m_brightnessService.reset();
  }

  try {
    m_pipewireService = std::make_unique<PipeWireService>();
    m_wirePlumberMixer = std::make_unique<WirePlumberMixer>();
    m_pipewireService->setWirePlumberMixer(m_wirePlumberMixer.get());
    m_wirePlumberMixer->setChangeCallback([svc = m_pipewireService.get()](std::uint32_t id, float volume, bool muted) {
      svc->onMixerVolumeChanged(id, volume, muted);
    });
    m_easyEffectsService = std::make_unique<EasyEffectsService>();
    m_easyEffectsService->refreshProfiles();
    m_easyEffectsService->refreshActiveEffectsProfiles();
    m_pipewireSpectrum = std::make_unique<PipeWireSpectrum>(*m_pipewireService);
    m_soundPlayer = std::make_shared<SoundPlayer>(m_pipewireService->loop());

    struct LoadedSoundPaths {
      std::filesystem::path volumeChange;
      std::filesystem::path notification;
    };
    auto loadedSoundPaths = std::make_shared<LoadedSoundPaths>();

    auto applySoundConfig = [this, loadedSoundPaths]() {
      if (m_soundPlayer == nullptr) {
        return;
      }

      const auto& audio = m_configService.config().audio;
      m_soundPlayer->setVolume(audio.enableSounds ? audio.soundVolume : 0.0f);

      auto resolveSoundPath = [](const std::string& configured, std::string_view bundledRelative) {
        if (configured.empty()) {
          return paths::assetPath(bundledRelative);
        }
        const std::filesystem::path expanded = FileUtils::expandUserPath(configured);
        if (expanded.is_absolute()) {
          return expanded;
        }
        return paths::assetPath(expanded.string());
      };

      const auto volumeChangePath = resolveSoundPath(audio.volumeChangeSound, "sounds/volume-change.wav");
      if (loadedSoundPaths->volumeChange != volumeChangePath) {
        if (m_soundPlayer->load("volume-change", volumeChangePath)) {
          loadedSoundPaths->volumeChange = volumeChangePath;
        }
      }

      const auto notificationPath = resolveSoundPath(audio.notificationSound, "sounds/notification.wav");
      if (loadedSoundPaths->notification != notificationPath) {
        if (m_soundPlayer->load("notification", notificationPath)) {
          loadedSoundPaths->notification = notificationPath;
        }
      }
    };
    applySoundConfig();
    m_configService.addReloadCallback(
        [this, applySoundConfig]() {
          if (m_configService.lastChange().audio) {
            applySoundConfig();
          }
        },
        "sound"
    );
  } catch (const std::exception& e) {
    kLog.warn("pipewire disabled: {}", e.what());
    m_soundPlayer.reset();
    m_pipewireSpectrum.reset();
    m_easyEffectsService.reset();
    m_pipewireService.reset();
    m_wirePlumberMixer.reset();
  }

  const std::weak_ptr<SoundPlayer> soundPlayer = m_soundPlayer;
  m_scriptApi.setLoadSoundHook(
      [soundPlayer](
          std::uint64_t ownerId, const std::string& name, const std::string& path
      ) -> std::optional<std::string> {
        const auto player = soundPlayer.lock();
        if (player == nullptr) {
          return "sound playback is unavailable";
        }
        return player->loadPluginSound(ownerId, name, std::filesystem::path(path));
      }
  );
  m_scriptApi.setPlaySoundHook([soundPlayer](std::uint64_t ownerId, const std::string& name) {
    if (const auto player = soundPlayer.lock()) {
      player->playPluginSound(ownerId, name);
    }
  });
  m_scriptApi.setUnloadPluginSoundsHook([soundPlayer](std::uint64_t ownerId) {
    if (const auto player = soundPlayer.lock()) {
      player->unloadPluginSounds(ownerId);
    }
  });
}

void Application::initSessionBusServices() {
  try {
    m_bus = std::make_unique<SessionBus>();
    kLog.info("connected to session bus");
  } catch (const std::exception& e) {
    kLog.warn("dbus disabled: {}", e.what());
    m_notificationManager.addInternal(
        "Mono Shell", i18n::tr("notifications.internal.session-bus-unavailable"), e.what(), Urgency::Low
    );
  }

  if (m_bus != nullptr) {
    try {
      m_debugService = std::make_unique<DebugService>(*m_bus, m_notificationManager);
      kLog.info("debug service active on dev.monoshell.Debug");
    } catch (const std::exception& e) {
      kLog.warn("debug service disabled: {}", e.what());
      m_debugService.reset();
    }

    try {
      m_mprisService = std::make_unique<MprisService>(*m_bus);
      auto applyMprisConfig = [this]() {
        if (m_mprisService == nullptr) {
          return;
        }
        m_mprisService->setBlacklist(m_configService.config().shell.mpris.blacklist);
      };
      applyMprisConfig();
      m_configService.addReloadCallback(applyMprisConfig);
      m_mprisService->setChangeCallback([this]() {
        m_bar.refresh();
        m_mediaOsd.onMprisChanged(*m_mprisService);
      });
      kLog.info("mpris discovery active");
    } catch (const std::exception& e) {
      kLog.warn("mpris disabled: {}", e.what());
      m_mprisService.reset();
      m_notificationManager.addInternal(
          "Mono Shell", i18n::tr("notifications.internal.mpris-disabled"), e.what(), Urgency::Low
      );
    }

    installNotificationBusNameWatch();
    syncNotificationDaemon();
    m_configService.addReloadCallback([this]() { syncNotificationDaemon(); });
    installSecretServiceNameWatch();

    m_compositorPlatform.startKdeActiveWindow(*m_bus);

    m_trayService = std::make_unique<TrayService>(*m_bus);
    m_trayService->setChangeCallback([this]() {
      m_bar.refresh();
      m_keyboardLayoutOsd.onTrayChanged(
          *m_trayService, m_configService.config(), m_configService.config().osd.kinds.keyboardLayout
      );
    });
  }

  m_locationService.initialize();
  m_weatherService.initialize();
  m_calendarService.initialize();

  // LocationService is the single source of "where am I": push its resolved coordinates to the
  // weather service, night light, and theme auto mode. Manual latitude/longitude and fixed
  // sunrise/sunset live in [location] and reach night light/theme through their config reloads.
  auto pushLocation = [this]() {
    m_gammaService.setLocationResolving(m_locationService.resolving());
    const auto location = m_locationService.resolvedLocation();
    if (location.has_value()) {
      m_weatherService.setLocation(
          WeatherCoordinates{.latitude = location->latitude, .longitude = location->longitude}, location->name,
          location->sourceLabel
      );
      m_gammaService.setResolvedCoordinates(location->latitude, location->longitude);
      m_themeService.setAutoCoordinates(location->latitude, location->longitude);
    } else {
      m_weatherService.setLocation(std::nullopt, {}, {});
      m_gammaService.setResolvedCoordinates(std::nullopt, std::nullopt);
      m_themeService.setAutoCoordinates(std::nullopt, std::nullopt);
    }
  };
  pushLocation();
  m_locationService.addChangeCallback([this, pushLocation]() {
    pushLocation();
    m_bar.refresh();
  });
  m_weatherService.addChangeCallback([this]() {
    m_bar.refresh();
  });
}

void Application::startTrayService() {
  if (m_bus == nullptr || m_trayService == nullptr) {
    return;
  }

  try {
    m_trayService->start();
  } catch (const std::exception& e) {
    kLog.warn("tray watcher disabled: {}", e.what());
  }
}

LayerShellLayer Application::hotCornerLayerForOutput(wl_output* output) const noexcept {
  return m_bar.highestLayerForOutput(output);
}

void Application::triggerShellAction(const std::string& action, wl_output* output) {
  (void)output;
  if (action == "overview") {
    // There is no public toggle for overview in OverviewLauncherCapture.
    // Try to execute a generic compositor action, or use niri directly if using niri.
    runShellCommand("niri msg action toggle-overview");
  }
}
