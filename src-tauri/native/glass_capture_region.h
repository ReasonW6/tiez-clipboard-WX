#pragma once

#include "glass_renderer.h"
#include <utility>

namespace tiez_glass_detail {

inline bool contains_rect(const PixelRect& outer, const PixelRect& inner) {
    return outer.right > outer.left && outer.bottom > outer.top &&
        inner.right > inner.left && inner.bottom > inner.top &&
        outer.left <= inner.left && outer.top <= inner.top &&
        outer.right >= inner.right && outer.bottom >= inner.bottom;
}

inline bool intersect_rect(const PixelRect& a, const PixelRect& b, PixelRect& result) {
    result = {std::max(a.left, b.left), std::max(a.top, b.top),
        std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
    return result.right > result.left && result.bottom > result.top;
}

inline uint32_t capture_padding(float scale, float physical_sigma) {
    if (!std::isfinite(scale) || scale <= 0 || scale > 16 ||
        !std::isfinite(physical_sigma) || physical_sigma < 0) return 0;
    // Real future coverage plus BlurPS support and one bilinear edge texel.
    return static_cast<uint32_t>(std::ceil(64.0f * scale) +
        std::ceil(3.0f * std::min(physical_sigma, 128.0f)) + 1.0f);
}

inline bool capture_region(const PixelRect& client, const PixelRect& available,
    uint32_t padding, PixelRect& result) {
    const int64_t w = int64_t(client.right) - client.left, h = int64_t(client.bottom) - client.top;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || padding > 8192) return false;
    // A client entirely off the desktop keeps the exact-client path. A partly
    // visible one keeps the same reserve extent inside available, covering its
    // visible part: dragging across a screen edge or edge docking must not
    // resize the source. Pixels outside available are never claimed.
    PixelRect visible{};
    if (!intersect_rect(client, available, visible) || !padding) { result = client; return true; }
    const auto axis = [&](int32_t first, int32_t last, int32_t low, int32_t high) {
        const int64_t extent = std::min<int64_t>({int64_t(last) - first + 2 * padding,
            int64_t(high) - low, 16384});
        const int64_t start = std::clamp<int64_t>(int64_t(first) - padding, low, int64_t(high) - extent);
        return std::pair<int32_t, int32_t>{static_cast<int32_t>(start), static_cast<int32_t>(start + extent)};
    };
    const auto x = axis(client.left, client.right, available.left, available.right);
    const auto y = axis(client.top, client.bottom, available.top, available.bottom);
    const PixelRect candidate{x.first, y.first, x.second, y.second};
    if (!contains_rect(candidate, visible) || !contains_rect(available, candidate)) return false;
    result = candidate;
    return true;
}

inline bool capture_viewport_copy(const PixelRect& source, const PixelRect& viewport, ResizeCopy& result) {
    if (!contains_rect(source, viewport) || int64_t(source.right) - source.left > 16384 ||
        int64_t(source.bottom) - source.top > 16384 || int64_t(viewport.right) - viewport.left > 8192 ||
        int64_t(viewport.bottom) - viewport.top > 8192) return false;
    result = resize_intersection(source, viewport);
    return true;
}

inline PixelRect union_rect(const PixelRect& a, const PixelRect& b) {
    return {std::min(a.left, b.left), std::min(a.top, b.top),
        std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

inline bool capture_supports(const PixelRect& source, const PixelRect& client,
    const PixelRect& available, float sigma) {
    // Only the client's on-desktop part needs real pixels; the rest is never seen.
    PixelRect visible{};
    if (!intersect_rect(client, available, visible) || !std::isfinite(sigma) || sigma < 0) return false;
    const int64_t halo = static_cast<int64_t>(std::ceil(3.0f * std::min(sigma, 128.0f))) + 1;
    const PixelRect required{
        static_cast<int32_t>(std::max<int64_t>(available.left, int64_t(client.left) - halo)),
        static_cast<int32_t>(std::max<int64_t>(available.top, int64_t(client.top) - halo)),
        static_cast<int32_t>(std::min<int64_t>(available.right, int64_t(client.right) + halo)),
        static_cast<int32_t>(std::min<int64_t>(available.bottom, int64_t(client.bottom) + halo))};
    return contains_rect(source, required);
}

inline bool capture_viewport_can_translate(const PixelRect& configured, const PixelRect& actual,
    int32_t requested_width, int32_t requested_height, bool no_size, bool resize_pending) {
    const int64_t width = int64_t(actual.right) - actual.left;
    const int64_t height = int64_t(actual.bottom) - actual.top;
    // A -> B -> A is still a resize when the queued output snapshot is A.
    // Compare the actual HWND too, and never bypass an unfinished size handoff.
    return !resize_pending && width > 0 && height > 0 &&
        width == int64_t(configured.right) - configured.left &&
        height == int64_t(configured.bottom) - configured.top &&
        (no_size || (requested_width == width && requested_height == height));
}

} // namespace tiez_glass_detail
