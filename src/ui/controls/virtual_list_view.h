#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/node.h"
#include "ui/controls/flex.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Renderer;
class ScrollView;

// Adapter that drives a VirtualListView from an external data source.
//
// The list materializes only the visible items plus a small overscan. Items are
// created once via createItem() and recycled via bindItem() as the user scrolls
// or the data changes. Rows may have variable heights; measureItem() must be
// pure measurement work and should not mutate scene children or upload textures.
class VirtualListAdapter {
public:
  virtual ~VirtualListAdapter() = default;

  [[nodiscard]] virtual std::size_t itemCount() const = 0;
  [[nodiscard]] virtual std::uint64_t itemKey(std::size_t index) const { return static_cast<std::uint64_t>(index); }
  [[nodiscard]] virtual std::uint64_t itemRevision(std::size_t /*index*/) const { return 0; }
  [[nodiscard]] virtual bool itemInteractive(std::size_t /*index*/) const { return false; }

  [[nodiscard]] virtual float measureItem(Renderer& renderer, std::size_t index, float width) = 0;
  [[nodiscard]] virtual std::unique_ptr<Node> createItem() = 0;
  virtual void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) = 0;
  virtual void onActivate(std::size_t /*index*/) {}
};

class VirtualListView : public Flex {
public:
  VirtualListView();

  // Adapter is non-owning and must outlive the list.
  void setAdapter(VirtualListAdapter* adapter);

  void notifyDataChanged();
  void notifyItemChanged(std::size_t index);

  void setItemGap(float gap);
  void setOverscanItems(std::size_t items);
  void scrollToIndex(std::size_t index);

  [[nodiscard]] ScrollView& scrollView() noexcept { return *m_scroll; }

  // Whether a scroll indicator is drawn when content overflows.
  void setScrollbarVisible(bool visible);

  // Begins a removal (collapse) animation for the item identified by `key`. The
  // item squashes and fades while the items below slide up, and the reported
  // contentHeight() shrinks in sync. `onComplete` is fired once the item has
  // fully collapsed so the caller can commit the data removal, after which the
  // list rebuilds with that item gone. Returns false — and leaves the list
  // untouched — when the animation cannot run yet (never laid out, item not
  // found, another removal already in flight, or no animation clock available);
  // callers should then remove the item instantly instead.
  bool prepareRemoveItem(std::uint64_t key, std::function<void()> onComplete);

  // Whether a removal collapse animation is currently running. Callers use this
  // to avoid issuing another prepared removal (which would be rejected) or an
  // instant removal (which would cancel the in-flight collapse): queue instead.
  [[nodiscard]] bool isRemovalAnimating() const noexcept { return m_removalActive; }

  // Total height of the measured content (sum of all items + gaps). Valid once
  // the list has been measured; otherwise 0. While a removal animation runs this
  // reports the shrinking height so hosts can track the collapse live.
  [[nodiscard]] float contentHeight() const noexcept {
    if (!m_removalActive) {
      return m_virtualHeight;
    }
    return m_virtualHeight > m_removalShrink ? m_virtualHeight - m_removalShrink : 0.0f;
  }

protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  class Canvas;
  class Slot;

  struct HeightCache {
    std::uint64_t key = 0;
    std::uint64_t revision = 0;
    int widthKey = 0;
    float height = 0.0f;
    bool valid = false;
  };

  void onScrollChanged(float offset);
  void setHoveredIndex(std::optional<std::size_t> index);
  void activateSlot(const Slot& slot);
  void recomputeMetrics(Renderer& renderer, float width);
  void clearSlotBindings();
  void clearHeightCache(std::size_t index);
  // Cancels an in-flight removal animation and fires its completion callback so
  // a pending removal is still committed even when the data changes underneath
  // the animation (the collapsed visual state is discarded).
  void finishRemovalNow();
  [[nodiscard]] std::size_t firstVisibleIndex(float scrollY) const noexcept;
  [[nodiscard]] std::size_t visibleEndIndex(std::size_t first, float scrollBottom) const noexcept;

  ScrollView* m_scroll = nullptr;
  Canvas* m_canvas = nullptr;

  VirtualListAdapter* m_adapter = nullptr;
  std::vector<Slot*> m_pool;
  std::vector<std::optional<std::size_t>> m_slotBoundIndex;
  std::vector<std::uint64_t> m_slotBoundKey;
  std::vector<std::uint64_t> m_slotBoundRevision;
  std::vector<int> m_slotBoundWidthKey;
  std::vector<bool> m_slotBoundHovered;

  std::vector<HeightCache> m_heightCache;
  std::vector<float> m_itemHeights;
  std::vector<float> m_itemOffsets;

  float m_itemGap = 0.0f;
  float m_virtualWidth = 0.0f;
  float m_virtualHeight = 0.0f;
  bool m_showScrollbar = true;
  std::size_t m_overscanItems = 3;
  std::size_t m_itemCount = 0;
  std::optional<std::size_t> m_hoveredIndex;
  bool m_pendingScrollToIndex = false;
  std::size_t m_pendingScrollIndex = 0;

  // Keys of the last committed layout. Used by prepareRemoveItem() to resolve an
  // item key to its index without asking the adapter to rebuild.
  std::vector<std::uint64_t> m_lastKeys;
  bool m_lastKeysValid = false;

  // State of the in-progress removal collapse, if any.
  bool m_removalActive = false;
  std::size_t m_removalIndex = 0;
  float m_removalShrink = 0.0f;
  float m_removalTarget = 0.0f;
  AnimationManager::Id m_removalAnimId = 0;
  std::function<void()> m_removalOnComplete;
};
