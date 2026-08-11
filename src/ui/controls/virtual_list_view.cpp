#include "ui/controls/virtual_list_view.h"

#include "render/animation/animation.h"
#include "render/scene/input_area.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>

class VirtualListView::Canvas : public Node {
public:
  Canvas() = default;

  void setVirtualSize(float width, float height) {
    m_virtualWidth = std::max(0.0f, width);
    m_virtualHeight = std::max(0.0f, height);
    setSize(m_virtualWidth, m_virtualHeight);
  }

protected:
  LayoutSize doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) override {
    return constraints.constrain(LayoutSize{.width = m_virtualWidth, .height = m_virtualHeight});
  }

  void doLayout(Renderer& /*renderer*/) override {}

  void doArrange(Renderer& /*renderer*/, const LayoutRect& rect) override {
    setPosition(rect.x, rect.y);
    setSize(m_virtualWidth, m_virtualHeight);
  }

private:
  float m_virtualWidth = 0.0f;
  float m_virtualHeight = 0.0f;
};

class VirtualListView::Slot : public InputArea {
public:
  explicit Slot(std::unique_ptr<Node> item) {
    if (item != nullptr) {
      m_item = addChild(std::move(item));
    }
  }

  [[nodiscard]] Node* item() const noexcept { return m_item; }
  [[nodiscard]] std::optional<std::size_t> boundIndex() const noexcept { return m_boundIndex; }

  void setBoundIndex(std::optional<std::size_t> index) { m_boundIndex = index; }

  void setItemGeometry(float width, float height) {
    setSize(width, height);
    if (m_item != nullptr) {
      m_item->setPosition(0.0f, 0.0f);
      m_item->setSize(width, height);
    }
  }

private:
  Node* m_item = nullptr;
  std::optional<std::size_t> m_boundIndex;
};

namespace {

  int widthCacheKey(float width) { return static_cast<int>(std::lround(std::max(0.0f, width))); }

  float saneHeight(float height) {
    if (!std::isfinite(height)) {
      return 1.0f;
    }
    return std::max(1.0f, height);
  }

} // namespace

VirtualListView::VirtualListView() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(0.0f);
  setPadding(0.0f);
  setFillWidth(true);
  setFillHeight(true);

  auto scroll = std::make_unique<ScrollView>();
  scroll->setFlexGrow(1.0f);
  scroll->setViewportPaddingH(0.0f);
  scroll->setViewportPaddingV(0.0f);
  scroll->setOnScrollChanged([this](float offset) { onScrollChanged(offset); });
  m_scroll = static_cast<ScrollView*>(addChild(std::move(scroll)));

  auto canvas = std::make_unique<Canvas>();
  m_canvas = static_cast<Canvas*>(m_scroll->content()->addChild(std::move(canvas)));
}

void VirtualListView::setAdapter(VirtualListAdapter* adapter) {
  if (m_adapter == adapter) {
    return;
  }
  m_adapter = adapter;
  if (m_canvas != nullptr) {
    for (Slot* slot : m_pool) {
      if (slot != nullptr) {
        m_canvas->removeChild(slot);
      }
    }
  }
  m_pool.clear();
  m_slotBoundIndex.clear();
  m_slotBoundKey.clear();
  m_slotBoundRevision.clear();
  m_slotBoundWidthKey.clear();
  m_slotBoundHovered.clear();
  m_heightCache.clear();
  m_itemHeights.clear();
  m_itemOffsets.clear();
  m_hoveredIndex.reset();
  if (m_removalAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_removalAnimId);
  }
  m_removalAnimId = 0;
  m_removalActive = false;
  m_removalShrink = 0.0f;
  m_removalTarget = 0.0f;
  m_removalIndex = 0;
  m_removalOnComplete = {};
  m_lastKeys.clear();
  m_lastKeysValid = false;
  markLayoutDirty();
}

void VirtualListView::notifyDataChanged() {
  // A structural change mid-collapse supersedes the pending removal, so commit
  // it now (fire the completion callback) instead of animating stale indices.
  finishRemovalNow();
  clearSlotBindings();
  m_heightCache.clear();
  m_itemHeights.clear();
  m_itemOffsets.clear();
  markLayoutDirty();
}

void VirtualListView::notifyItemChanged(std::size_t index) {
  clearHeightCache(index);
  for (auto& boundIndex : m_slotBoundIndex) {
    if (boundIndex.has_value() && *boundIndex == index) {
      boundIndex.reset();
      markLayoutDirty();
    }
  }
}

void VirtualListView::setItemGap(float gap) {
  const float next = std::max(0.0f, gap);
  if (m_itemGap == next) {
    return;
  }
  m_itemGap = next;
  markLayoutDirty();
}

void VirtualListView::setOverscanItems(std::size_t items) {
  if (m_overscanItems == items) {
    return;
  }
  m_overscanItems = items;
  markLayoutDirty();
}

void VirtualListView::setScrollbarVisible(bool visible) {
  if (m_showScrollbar == visible) {
    return;
  }
  m_showScrollbar = visible;
  if (m_scroll != nullptr) {
    m_scroll->setScrollbarVisible(visible);
  }
  markLayoutDirty();
}

void VirtualListView::scrollToIndex(std::size_t index) {
  m_pendingScrollToIndex = true;
  m_pendingScrollIndex = index;
  markLayoutDirty();
}

bool VirtualListView::prepareRemoveItem(std::uint64_t key, std::function<void()> onComplete) {
  if (m_adapter == nullptr || m_removalActive || !m_lastKeysValid || m_itemCount == 0) {
    return false;
  }
  if (m_itemCount > m_lastKeys.size() || m_itemCount > m_itemHeights.size()) {
    return false;
  }

  std::size_t index = m_itemCount;
  for (std::size_t i = 0; i < m_itemCount; ++i) {
    if (m_lastKeys[i] == key) {
      index = i;
      break;
    }
  }
  if (index >= m_itemCount) {
    return false;
  }

  AnimationManager* animations = animationManager();
  if (animations == nullptr) {
    return false;
  }

  const float collapsedHeight = std::max(1.0f, m_itemHeights[index]);
  const float target = collapsedHeight + (index + 1 < m_itemCount ? m_itemGap : 0.0f);

  m_removalActive = true;
  m_removalIndex = index;
  m_removalShrink = 0.0f;
  m_removalTarget = target;
  m_removalOnComplete = std::move(onComplete);

  m_removalAnimId = animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this, target](float progress) {
        m_removalShrink = target * progress;
        markLayoutDirty();
      },
      [this]() {
        m_removalAnimId = 0;
        m_removalActive = false;
        // m_removalShrink already holds the fully-collapsed value; the layout
        // rendered on the final tick shows the collapsed state, so committing
        // the data change now lands without a visual jump.
        std::function<void()> callback = std::move(m_removalOnComplete);
        m_removalOnComplete = {};
        markLayoutDirty();
        if (callback) {
          callback();
        }
      },
      this
  );
  return true;
}

void VirtualListView::finishRemovalNow() {
  if (!m_removalActive) {
    return;
  }
  if (m_removalAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_removalAnimId);
  }
  m_removalAnimId = 0;
  m_removalActive = false;
  m_removalShrink = 0.0f;
  m_removalTarget = 0.0f;
  m_removalIndex = 0;
  std::function<void()> callback = std::move(m_removalOnComplete);
  m_removalOnComplete = {};
  markLayoutDirty();
  if (callback) {
    callback();
  }
}

void VirtualListView::doLayout(Renderer& renderer) {
  if (m_adapter == nullptr || m_scroll == nullptr || m_canvas == nullptr) {
    Flex::doLayout(renderer);
    return;
  }

  const float ourW = std::max(0.0f, width());
  const float ourH = std::max(0.0f, height());
  const float padH = m_scroll->viewportPaddingH();
  const float padV = m_scroll->viewportPaddingV();
  const float innerW = std::max(0.0f, ourW - 2.0f * padH);
  const float viewportH = std::max(0.0f, ourH - 2.0f * padV);
  const float scrollbarGutter = Style::scrollbarWidth + Style::scrollbarGap;

  // Only reserve the scrollbar gutter when we actually draw a scroll indicator.
  // Reserving it otherwise changes the viewport width, which rewraps variable
  // height rows and can feed back into a height/width oscillation.
  recomputeMetrics(renderer, innerW);
  float viewportW = innerW;
  if (m_showScrollbar && m_virtualHeight > viewportH + 0.5f) {
    viewportW = std::max(0.0f, innerW - scrollbarGutter);
    recomputeMetrics(renderer, viewportW);
  }
  m_canvas->setVirtualSize(m_virtualWidth, m_virtualHeight);

  if (m_pendingScrollToIndex) {
    m_pendingScrollToIndex = false;
    if (m_pendingScrollIndex < m_itemCount && m_pendingScrollIndex < m_itemOffsets.size()) {
      m_scroll->setScrollOffset(m_itemOffsets[m_pendingScrollIndex]);
    }
  }

  const float scrollY = m_scroll->scrollOffset();
  const float scrollBottom = scrollY + viewportH;
  std::size_t first = firstVisibleIndex(scrollY);
  std::size_t end = visibleEndIndex(first, scrollBottom);
  if (first > m_overscanItems) {
    first -= m_overscanItems;
  } else {
    first = 0;
  }
  end = std::min(m_itemCount, end + m_overscanItems);

  const std::size_t desiredPoolSize = end > first ? end - first : 0;
  while (m_pool.size() < desiredPoolSize) {
    auto item = m_adapter->createItem();
    if (item == nullptr) {
      break;
    }
    auto slot = std::make_unique<Slot>(std::move(item));
    Slot* slotPtr = slot.get();
    slot->setVisible(false);
    slot->setParticipatesInLayout(false);
    slot->setOnEnter([this, slotPtr](const InputArea::PointerData&) { setHoveredIndex(slotPtr->boundIndex()); });
    slot->setOnMotion([this, slotPtr](const InputArea::PointerData&) { setHoveredIndex(slotPtr->boundIndex()); });
    slot->setOnLeave([this, slotPtr]() {
      if (m_hoveredIndex == slotPtr->boundIndex()) {
        setHoveredIndex(std::nullopt);
      }
    });
    slot->setOnPress([this, slotPtr](const InputArea::PointerData& data) {
      if (!data.pressed && data.button == BTN_LEFT) {
        activateSlot(*slotPtr);
      }
    });
    m_pool.push_back(static_cast<Slot*>(m_canvas->addChild(std::move(slot))));
    m_slotBoundIndex.emplace_back();
    m_slotBoundKey.push_back(0);
    m_slotBoundRevision.push_back(0);
    m_slotBoundWidthKey.push_back(0);
    m_slotBoundHovered.push_back(false);
  }

  std::vector<bool> slotActive(m_pool.size(), false);
  if (!m_pool.empty() && end > first) {
    const int bindWidthKey = widthCacheKey(viewportW);
    for (std::size_t index = first; index < end; ++index) {
      const std::size_t slotIndex = index % m_pool.size();
      Slot* slot = m_pool[slotIndex];
      if (slot == nullptr || index >= m_itemHeights.size() || index >= m_itemOffsets.size()) {
        continue;
      }

      const float itemH = m_itemHeights[index];
      const float itemY = m_itemOffsets[index];
      const std::uint64_t key = m_adapter->itemKey(index);
      const std::uint64_t revision = m_adapter->itemRevision(index);
      const bool hovered = m_hoveredIndex.has_value() && *m_hoveredIndex == index;
      const bool dirty = !m_slotBoundIndex[slotIndex].has_value()
          || *m_slotBoundIndex[slotIndex] != index
          || m_slotBoundKey[slotIndex] != key
          || m_slotBoundRevision[slotIndex] != revision
          || m_slotBoundWidthKey[slotIndex] != bindWidthKey
          || m_slotBoundHovered[slotIndex] != hovered;

      float laidOutH = itemH;
      float laidOutY = itemY;
      float slotOpacity = 1.0f;
      bool interactive = m_adapter->itemInteractive(index);
      if (m_removalActive) {
        if (index == m_removalIndex) {
          const float progress =
              m_removalTarget > 0.0f ? std::clamp(m_removalShrink / m_removalTarget, 0.0f, 1.0f) : 1.0f;
          laidOutH = std::max(0.0f, laidOutH - m_removalShrink);
          slotOpacity = 1.0f - progress;
          interactive = false;
        } else if (index > m_removalIndex) {
          laidOutY = std::max(0.0f, laidOutY - m_removalShrink);
        }
      }

      slotActive[slotIndex] = true;
      slot->setBoundIndex(index);
      slot->setEnabled(interactive);
      slot->setPosition(0.0f, laidOutY);
      slot->setItemGeometry(viewportW, laidOutH);
      slot->setOpacity(slotOpacity);

      if (dirty && slot->item() != nullptr) {
        m_adapter->bindItem(renderer, *slot->item(), index, viewportW, hovered);
        m_slotBoundIndex[slotIndex] = index;
        m_slotBoundKey[slotIndex] = key;
        m_slotBoundRevision[slotIndex] = revision;
        m_slotBoundWidthKey[slotIndex] = bindWidthKey;
        m_slotBoundHovered[slotIndex] = hovered;
      }

      slot->setVisible(true);
      slot->layout(renderer);
    }
  }

  for (std::size_t slotIndex = 0; slotIndex < m_pool.size(); ++slotIndex) {
    if (slotActive[slotIndex] || m_pool[slotIndex] == nullptr) {
      continue;
    }
    m_pool[slotIndex]->setVisible(false);
    m_pool[slotIndex]->setBoundIndex(std::nullopt);
    m_pool[slotIndex]->setEnabled(false);
    m_slotBoundIndex[slotIndex].reset();
  }

  Flex::doLayout(renderer);
}

LayoutSize VirtualListView::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
  const float w = constraints.hasExactWidth() ? constraints.maxWidth
      : constraints.hasMaxWidth               ? constraints.maxWidth
                                              : 0.0f;
  const float h = constraints.hasExactHeight() ? constraints.maxHeight
      : constraints.hasMaxHeight               ? constraints.maxHeight
                                               : 0.0f;
  return LayoutSize{.width = w, .height = h};
}

void VirtualListView::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void VirtualListView::onScrollChanged(float /*offset*/) { markLayoutDirty(); }

void VirtualListView::setHoveredIndex(std::optional<std::size_t> index) {
  if (m_hoveredIndex == index) {
    return;
  }
  m_hoveredIndex = index;
  markLayoutDirty();
}

void VirtualListView::activateSlot(const Slot& slot) {
  if (m_adapter == nullptr || !slot.boundIndex().has_value()) {
    return;
  }
  m_adapter->onActivate(*slot.boundIndex());
}

void VirtualListView::recomputeMetrics(Renderer& renderer, float width) {
  m_itemCount = m_adapter != nullptr ? m_adapter->itemCount() : 0;
  m_virtualWidth = std::max(0.0f, width);
  m_heightCache.resize(m_itemCount);
  m_itemHeights.resize(m_itemCount);
  m_itemOffsets.resize(m_itemCount + 1);

  const int widthKey = widthCacheKey(width);
  float cursor = 0.0f;
  for (std::size_t index = 0; index < m_itemCount; ++index) {
    const std::uint64_t key = m_adapter->itemKey(index);
    const std::uint64_t revision = m_adapter->itemRevision(index);
    HeightCache& cache = m_heightCache[index];
    if (!cache.valid || cache.key != key || cache.revision != revision || cache.widthKey != widthKey) {
      cache.key = key;
      cache.revision = revision;
      cache.widthKey = widthKey;
      cache.height = saneHeight(m_adapter->measureItem(renderer, index, width));
      cache.valid = true;
    }

    m_itemOffsets[index] = cursor;
    m_itemHeights[index] = cache.height;
    cursor += cache.height;
    if (index + 1 < m_itemCount) {
      cursor += m_itemGap;
    }
  }

  m_itemOffsets[m_itemCount] = cursor;
  m_virtualHeight = cursor;

  // Snapshot the committed keys so prepareRemoveItem() can resolve a key to an
  // index from the last stable layout.
  m_lastKeys.clear();
  m_lastKeys.reserve(m_itemCount);
  for (std::size_t index = 0; index < m_itemCount; ++index) {
    m_lastKeys.push_back(m_adapter != nullptr ? m_adapter->itemKey(index) : static_cast<std::uint64_t>(index));
  }
  m_lastKeysValid = true;
}

void VirtualListView::clearSlotBindings() {
  for (auto& index : m_slotBoundIndex) {
    index.reset();
  }
}

void VirtualListView::clearHeightCache(std::size_t index) {
  if (index < m_heightCache.size()) {
    m_heightCache[index].valid = false;
  }
}

std::size_t VirtualListView::firstVisibleIndex(float scrollY) const noexcept {
  if (m_itemCount == 0 || m_itemOffsets.empty()) {
    return 0;
  }
  const auto begin = m_itemOffsets.begin();
  const auto end = m_itemOffsets.begin() + static_cast<std::ptrdiff_t>(m_itemCount);
  auto it = std::upper_bound(begin, end, std::max(0.0f, scrollY));
  std::size_t index = it == begin ? 0 : static_cast<std::size_t>((it - begin) - 1);
  while (index + 1 < m_itemCount
         && index < m_itemHeights.size()
         && m_itemOffsets[index] + m_itemHeights[index] < scrollY) {
    ++index;
  }
  return std::min(index, m_itemCount - 1);
}

std::size_t VirtualListView::visibleEndIndex(std::size_t first, float scrollBottom) const noexcept {
  std::size_t index = first;
  while (index < m_itemCount && index < m_itemOffsets.size() && m_itemOffsets[index] < scrollBottom) {
    ++index;
  }
  return index;
}
