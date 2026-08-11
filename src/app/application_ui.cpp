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
#include "render/animation/motion_service.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"
#include "render/text/font_weight_catalog.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/tooltip/tooltip_manager.h"
#include "system/brightness_poll_source.h"
#include "system/brightness_service.h"
#include "system/distro_info.h"
#include "system/easyeffects_service.h"
#include "system/system_monitor_service.h"
#include "ui/app_icon_colorization.h"
#include "ui/controls/context_menu_popup.h"
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
} // namespace

void Application::initUi() {
  initUiRenderSurfacesAndSettings();
  initLockScreenAndSession();
  initInputDispatch();
  initPanelManagerAndPanels();
  initNotificationAndOsd();
  initBarDockAndLayout();
  initWidgetControllersAndCallbacks();
  // Wiring is complete and outputs are enumerated; build every per-output layer
  // surface once in canonical order. initialize() above only wired dependencies.
  reconcileOutputSurfaces();
}

void Application::initUiRenderSurfacesAndSettings() {

  m_renderContext.initialize(m_glShared);
  m_renderContext.setGraphicsResetCallback([this](RenderGraphicsResetStatus status) { onGraphicsReset(status); });
  if (!m_glShared.hasSharedContext()) {
    m_asyncTextureCache.setMakeCurrentCallback([this]() { m_renderContext.backend().makeCurrentNoSurface(); });
  }
  m_renderContext.setTextFontFamily(m_configService.config().shell.fontFamily);
}

void Application::initLockScreenAndSession() {
  SessionActionHooks sessionActionHooks;
  sessionActionHooks.onLogout = [this]() { return m_hookManager.fireBlocking(HookKind::LoggingOut); };
  sessionActionHooks.onReboot = [this]() { return m_hookManager.fireBlocking(HookKind::Rebooting); };
  sessionActionHooks.onShutdown = [this]() { return m_hookManager.fireBlocking(HookKind::ShuttingDown); };
  m_sessionActionRunner.setHooks(std::move(sessionActionHooks));
  m_sessionActionRunner.setPowerConfig(m_configService.config().shell.session.power);
  m_configService.addReloadCallback(
      [this]() { m_sessionActionRunner.setPowerConfig(m_configService.config().shell.session.power); }, "session-power"
  );
}

void Application::initInputDispatch() {
  m_wayland.setPointerEventCallback([this](const PointerEvent& event) {
    if (m_colorPickerDialogPopup.onPointerEvent(event)) {
      return;
    }
    if (m_glyphPickerDialogPopup.onPointerEvent(event)) {
      return;
    }
    if (m_fileDialogPopup.onPointerEvent(event)) {
      return;
    }
    // Region overlay is layer Overlay + exclusive keyboard; prefer it over the
    // widgets editors (Bottom / OnDemand) so confirm/cancel still work mid-edit.
    if (m_screenshotService.onPointerEvent(event))
      return;
    if (m_bar.onPointerEvent(event))
      return;
    if (m_panelManager.onPointerEvent(event))
      return;
    m_notificationToast.onPointerEvent(event);
  });

  m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) {
    // Grab popups are modal — while one is open it owns the keyboard and ESC
    // dismisses it before anything behind can react.
    if (ContextMenuPopup::dispatchKeyboardEvent(event)) {
      return;
    }
    if (m_colorPickerDialogPopup.isOpen()) {
      m_colorPickerDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_glyphPickerDialogPopup.isOpen()) {
      m_glyphPickerDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_fileDialogPopup.isOpen()) {
      m_fileDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_screenshotService.onKeyboardEvent(event)) {
      return;
    }
    if (m_notificationToast.onKeyboardEvent(event)) {
      return;
    }
    m_panelManager.onKeyboardEvent(event);
  });
}

void Application::initPanelManagerAndPanels() {
  // Panel manager must be before bar so widgets can access PanelManager::instance()
  m_panelManager.initialize(m_compositorPlatform, &m_configService, &m_renderContext);
  m_calendarService.setCredentialChangeCallback([this]() { retrySecretServiceConsumers(); });
  syncClipboardService();
  if (m_configService.config().shell.panel.enabled) {
    reloadPluginPanels();
    m_compositorPlatform.setOverviewChangeCallback([this]() { m_bar.scheduleSmartAutoHideReevaluation(); });
    m_panelManager.setPanelOpenedCallback([this]() {
      if (m_panelManager.isAttachedOpen()) {
        m_bar.revealAutoHideForAttachedPanel(
            m_panelManager.attachedPanelOutput(), m_panelManager.attachedSourceBarName()
        );
      }
    });
    m_panelManager.setPanelClosedCallback([this]() {
      m_bar.reevaluateAutoHide();
      // Widgets that stay visible while their panel is open re-evaluate on the next update.
      m_bar.refresh();
    });
  }
}

void Application::initNotificationAndOsd() {
  m_notificationToast.initialize(m_wayland, &m_configService, &m_notificationManager, &m_renderContext, &m_httpClient);
  m_configService.addReloadCallback([this]() { m_notificationToast.onConfigReload(); });
  auto applyNotificationFilterConfig = [this]() {
    m_notificationManager.setFilters(m_configService.config().notification.filters);
  };
  auto applyHistoryRetention = [this]() {
    m_notificationManager.setHistoryRetentionHours(m_configService.config().notification.historyRetentionHours);
  };
  applyHistoryRetention();
  m_configService.addReloadCallback(applyHistoryRetention);
  applyNotificationFilterConfig();
  m_configService.addReloadCallback(applyNotificationFilterConfig);
  m_configService.setNotificationManager(&m_notificationManager);
  m_notificationManager.setSoundPlayer(m_soundPlayer.get());

  TooltipManager::instance().initialize(m_wayland, &m_configService, &m_renderContext);
  m_osdOverlay.initialize(m_wayland, &m_configService, &m_renderContext);
  m_configService.addReloadCallback([this]() { m_osdOverlay.onConfigReload(); });
  m_idleGraceOverlay.initialize(m_wayland, &m_renderContext);
  m_wayland.setIdleCapabilitiesReadyCallback([this]() { m_idleManager.reload(m_configService.config().idle); });
  m_idleManager.initialize(
      m_wayland,
      [this](
          const std::string& behaviorName, std::chrono::milliseconds fadeIn, bool willLockSession,
          std::function<void()> onFadeComplete
      ) {
        (void)behaviorName;
        (void)willLockSession;
        DeferredCall::callLater([this, fadeIn, done = std::move(onFadeComplete)]() mutable {
          m_idleGraceOverlay.show(fadeIn, std::move(done));
        });
      },
      [this](bool /*userCancelled*/, bool /*willLockSession*/) { m_idleGraceOverlay.hide(); }
  );
  m_idleManager.setActionRunner(
      [this](const IdleBehaviorConfig& /*behavior*/, const IdleActionRequest& action) -> bool {
        return runIdleAction(action);
      }
  );
  m_idleManager.reload(m_configService.config().idle);
  try {
    m_screenSaverService = std::make_unique<ScreenSaverService>(m_systemBus.get());
    if (m_screenSaverService->active()) {
      m_screenSaverService->setChangeCallback([this](std::int64_t locks) {
        m_idleManager.setScreenSaverInhibitLocks(locks);
      });
      m_idleManager.setScreenSaverInhibitLocks(m_screenSaverService->inhibitLocks());
    } else {
      m_screenSaverService.reset();
    }
  } catch (const std::exception& e) {
    kLog.warn("idle inhibit service disabled: {}", e.what());
    m_screenSaverService.reset();
  }
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().idle) {
          m_idleManager.reload(m_configService.config().idle);
        }
      },
      "idle"
  );
  m_audioOsd.bindOverlay(m_osdOverlay);
  m_audioOsd.setSoundPlayer(m_soundPlayer.get());
  if (m_pipewireService != nullptr) {
    m_audioOsd.primeFromService(*m_pipewireService);
  }
  m_brightnessOsd.bindOverlay(m_osdOverlay);
  if (m_brightnessService != nullptr) {
    m_brightnessOsd.primeFromService(*m_brightnessService);
  }
  m_keyboardBacklightOsd.bindOverlay(m_osdOverlay);
  if constexpr (kLockKeysEnabled) {
    m_lockKeysOsd.bindOverlay(m_osdOverlay);
    m_lockKeysOsd.primeFromService(m_lockKeysService);
  }
  m_keyboardLayoutOsd.bindOverlay(m_osdOverlay);
  m_keyboardLayoutOsd.prime(m_compositorPlatform);
  m_mediaOsd.bindOverlay(m_osdOverlay);
  m_privacyOsd.bindOverlay(m_osdOverlay);
  m_privacyOsd.configure(m_configService.config());
  m_configService.addReloadCallback(
      [this]() {
        if (m_configService.lastChange().shell) {
          m_privacyOsd.onConfigReload(m_configService.config(), m_pipewireService.get());
          m_bar.refresh();
        }
      },
      "privacy-filters"
  );
}

void Application::initBarDockAndLayout() {

  m_bar.initialize({
      .platform = m_compositorPlatform,
      .config = m_configService,
      .notifications = &m_notificationManager,
      .audio = m_pipewireService.get(),
      .easyEffects = m_easyEffectsService.get(),
      .upower = m_upowerService.get(),
      .sysmon = m_systemMonitor.get(),
      .powerProfiles = m_powerProfilesService.get(),
      .network = m_networkService.get(),
      .externalIp = &m_externalIpService,
      .idleInhibitor = &m_idleInhibitor,
      .mpris = m_mprisService.get(),
      .audioSpectrum = m_pipewireSpectrum.get(),
      .httpClient = &m_httpClient,
      .weather = &m_weatherService,
      .renderContext = &m_renderContext,
      .nightLight = &m_gammaService,
      .theme = &m_themeService,
      .bluetooth = m_bluetoothService.get(),
      .brightness = m_brightnessService.get(),
      .lockKeys = kLockKeysEnabled ? &m_lockKeysService : nullptr,
      .clipboard = &m_clipboardService,
      .fileWatcher = &m_fileWatcher,
      .screenshots = &m_screenshotService,
      .scriptApi = &m_scriptApi,
  });
  m_idleInhibitor.setAnchorSurfacesProvider([this]() { return m_bar.caffeineAnchorSurfaces(); });
  m_panelManager.setAttachedPanelGeometryCallback(
      [this](wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry) {
        m_bar.setAttachedPanelGeometry(output, barName, geometry);
      }
  );
  m_panelManager.setClickShieldExcludeRectsProvider([this](wl_output* output) {
    return m_bar.surfaceRectsForOutput(output);
  });
  m_panelManager.setFocusGrabBarSurfacesProvider([this]() { return m_bar.allBarSurfaces(); });
  m_panelManager.setAttachedPanelAvailabilityCallback([this](wl_output* output, std::string_view barName) {
    return m_bar.canAttachPanelToBar(output, barName);
  });
  m_panelManager.setAttachedPanelLayerProvider([this](wl_output* output, std::string_view barName) {
    return m_bar.layerForBar(output, barName);
  });
  m_panelManager.setAttachedPanelBarSettledCallback([this](wl_output* output, std::string_view barName) {
    return m_bar.isAttachedPanelBarSettled(output, barName);
  });
  m_bar.setAutoHideSuppressionCallback([this](const BarInstance& instance) {
    return m_panelManager.isAttachedOpen() && m_panelManager.attachedSourceBarName() == instance.barConfig.name;
  });
  // When config reloads, refresh any open panel: bar-driven attached decoration restyle and
  // shell-driven compositor blur.
  m_configService.addReloadCallback([this]() { m_panelManager.onConfigReloaded(); });
  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) { return m_panelManager.popupParentContextForSurface(surface); },
      [this](wl_surface* surface) { m_panelManager.beginAttachedPopup(surface); },
      [this](wl_surface* surface) { m_panelManager.endAttachedPopup(surface); },
      [this]() { return m_panelManager.fallbackPopupParentContext(); }
  );
  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) { return m_bar.popupParentContextForSurface(surface); },
      [this](wl_surface* surface) { m_bar.beginAttachedPopup(surface); },
      [this](wl_surface* surface) { m_bar.endAttachedPopup(surface); },
      [this]() {
        return m_bar.preferredPopupParentContext(
            m_compositorPlatform.preferredInteractiveOutput(std::chrono::milliseconds(1200))
        );
      }
  );

  m_colorPickerDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts);
  ColorPickerDialog::setPresenter(&m_colorPickerDialogPopup);

  m_glyphPickerDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts);
  GlyphPickerDialog::setPresenter(&m_glyphPickerDialogPopup);

  m_fileDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts, m_thumbnailService);
  FileDialog::setPresenter(&m_fileDialogPopup);
}

void Application::initWidgetControllersAndCallbacks() {
  m_iconThemePollSource.setChangeCallback([this]() { onIconThemeChanged(); });

  std::string lastShellFontFamily = m_configService.config().shell.fontFamily;
  m_configService.addReloadCallback(
      [this, lastShellFontFamily]() mutable {
        const std::string& newShellFontFamily = m_configService.config().shell.fontFamily;
        if (newShellFontFamily == lastShellFontFamily) {
          return;
        }

        lastShellFontFamily = newShellFontFamily;
        text::invalidateFontWeightCatalogCache();
        m_renderContext.setTextFontFamily(newShellFontFamily);
        m_bar.requestLayout();
        m_panelManager.requestLayout();
        m_notificationToast.requestLayout();
        m_osdOverlay.requestLayout();
        m_colorPickerDialogPopup.requestLayout();
        m_glyphPickerDialogPopup.requestLayout();
        m_fileDialogPopup.requestLayout();
      },
      "shell-font-family"
  );

  m_timeService.setTickSecondCallback([this]() {
    m_bar.onSecondTick();
    if (m_configService.config().osd.kinds.keyboardLayout) {
      m_keyboardLayoutOsd.onLayoutChanged(m_compositorPlatform, m_configService.config());
    }
    m_idleManager.onSecondTick();
  });

  if (m_pipewireService != nullptr) {
    m_audioOsd.suppressFor(std::chrono::milliseconds(2000));
    m_pipewireService->setChangeCallback([this]() {
      if (m_pipewireSpectrum != nullptr) {
        m_pipewireSpectrum->handleAudioStateChanged();
      }
      m_bar.refresh();
      if (m_pipewireService != nullptr) {
        m_audioOsd.onAudioStateChanged(*m_pipewireService);
        m_privacyOsd.onPrivacyStateChanged(*m_pipewireService);
      }
    });
    m_pipewireService->setVolumePreviewCallback([this](bool isInput, std::uint32_t id, float volume, bool muted) {
      if (isInput) {
        m_audioOsd.showInput(id, volume, muted);
      } else {
        m_audioOsd.showOutput(id, volume, muted);
      }
    });
  }
  if (m_easyEffectsService != nullptr) {
    m_easyEffectsService->setChangeCallback([this]() {
      m_bar.refresh();
    });
  }

  // Wire the corner surface owners here alongside the dock. Surface creation and
  // stacking order live entirely in reconcileOutputSurfaces(): screen corners and
  // the hot-corner trigger zones are built after the bar and dock so they are
  // never occluded by shell chrome on their shared layer.
}
