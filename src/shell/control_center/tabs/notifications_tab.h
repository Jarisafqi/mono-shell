#pragma once

#include "render/animation/animation_manager.h"
#include "shell/control_center/tab.h"
#include "system/icon_resolver.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class NotificationManager;
struct NotificationHistoryEntry;
class Button;
class Segmented;
class VirtualListView;
class Label;
class NotificationHistoryAdapter;
class INetworkService;
class BluetoothService;

class NotificationsTab : public Tab {
public:
  explicit NotificationsTab(
      NotificationManager* notifications, INetworkService* network, BluetoothService* bluetooth
  );
  ~NotificationsTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;

  // Number of filtered notification entries currently populating the list.
  [[nodiscard]] std::size_t filteredCount() const noexcept { return m_filtered.size(); }

  // Height of the notification list content, in this tab's scaled units. Falls
  // back to an approximation based on the filtered count when the list has not
  // been measured yet (first open frame).
  [[nodiscard]] float estimatedContentHeight() const;

  // Approximate height of one *collapsed* (never-expanded) notification card in
  // the tab's scaled units. Used as the panel's minimum floor so the panel
  // shrinks no further than a single card + header. Derived from the same
  // metrics as measureNotificationCard() (icon size, line caps, paddings).
  [[nodiscard]] float collapsedCardHeight() const;

  // Fixed vertical chrome of this tab that sits around the notification list
  // (the quick-toggle row + column gap + paddings), in scaled units. Added to
  // the panel height budget so a card + the toggles below it both fit fully.
  [[nodiscard]] float chromeHeight() const;

private:
  void onPanelCardOpacityChanged(float opacity) override;
  friend class NotificationHistoryAdapter;

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void clearAllNotifications();
  void removeNotificationEntry(uint32_t id, bool wasActive);
  void toggleDoNotDisturb();
  void toggleNotificationExpanded(uint32_t id);
  void invokeNotificationAction(uint32_t id, const std::string& actionKey);
  bool refreshDataSnapshot();
  // Whether a notification card renders expanded. A lone notification is shown
  // with its full body automatically so a single card fits the panel without
  // needing the user to expand it (which would force scrolling).
  [[nodiscard]] bool expandedForEntry(uint32_t id) const;
  void syncDndButton();
  void syncQuickToggles();
  void updateEmptyState(bool hasHistory, bool hasFiltered);
  std::optional<std::size_t> filteredIndexForId(uint32_t id) const;
  void cancelFilterSlide();
  void beginFilterSlideOut(std::size_t nextIndex);
  void beginFilterSlideIn();
  void applyFilterSlide(float progress, bool slidingIn);
  [[nodiscard]] bool filterSlideOutActive() const;

  NotificationManager* m_notifications = nullptr;
  INetworkService* m_network = nullptr;
  BluetoothService* m_bluetooth = nullptr;
  IconResolver m_iconResolver;
  std::unique_ptr<NotificationHistoryAdapter> m_adapter;
  std::vector<const NotificationHistoryEntry*> m_filtered;
  Flex* m_root = nullptr;
  VirtualListView* m_list = nullptr;
  Flex* m_emptyCard = nullptr;
  Label* m_emptyTitle = nullptr;
  Label* m_emptyBody = nullptr;
  Button* m_clearAllButton = nullptr;
  Button* m_dndButton = nullptr;
  Button* m_wifiButton = nullptr;
  Button* m_bluetoothButton = nullptr;
  Segmented* m_filter = nullptr;
  std::size_t m_filterIndex = 0;
  std::unordered_set<uint32_t> m_expandedIds;
  std::uint64_t m_lastSerial = 0;
  /// Wall-clock coarse slot so relative times (e.g. "2 min ago") refresh without churning every frame.
  std::int64_t m_lastRelativeTimeSlot = -1;
  std::size_t m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
  std::size_t m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
  bool m_startFilterSlideIn = false;
  int m_filterSlideDirection = 0;
  float m_filterSlideBaseX = 0.0f;
  float m_filterSlideBaseY = 0.0f;
  AnimationManager::Id m_filterSlideAnimId = 0;
  // Content height the panel was last sized for (as reported by the list after
  // it has laid out and measured its items). The list only computes its true
  // content height during its own layout, which happens after the panel picks
  // its size, so we re-request a panel relayout whenever this settles to a new
  // value to keep the panel height following the notification list.
  float m_lastSizedContentHeight = -1.0f;
};
