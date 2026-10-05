#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
#define TIEZ_GLASS_NOEXCEPT noexcept
extern "C" {
#else
#define TIEZ_GLASS_NOEXCEPT
#endif

typedef struct TiezGlass TiezGlass;

typedef struct TiezGlassTrack {
    float x, y, width, height, radius, progress;
    float active_rgba[4];
    float inactive_rgba[4];
} TiezGlassTrack;

typedef struct TiezGlassControl {
    float x, y, width, height, radius, pressure;
    uint32_t material; // 0 = clear, 1 = regular, 2 = reading; independent of size.
    // Zero width and height disable the optional underlay. Each control may
    // carry a track; tracks are sampled before refraction, never as foreground.
    TiezGlassTrack track;
} TiezGlassControl;

typedef struct TiezGlassState {
    float viewport_width;
    float viewport_height;
    float blur_radius;
    float tint_rgba[4];
    const TiezGlassControl* controls;
    uint32_t control_count;
    uint64_t geometry_revision;
} TiezGlassState;

// All calls and callbacks run on the host HWND's UI thread. HWND parameters are
// opaque pointers here so the Rust boundary does not depend on Windows headers.
// State: 0 = pending/suspended, 1 = a foreground frame for geometry_revision
// was submitted, 2 = failed, 3 = background-only resize output. Only state 1
// acknowledges geometry; this is not a monitor/display-completion fence.
// qa_source is null in production. A non-null value permits only an opaque,
// same-process fixture covering the entire crop, with no foreign window above.
// A callback must not destroy or reenter this renderer synchronously; schedule
// follow-up work after the callback returns.
typedef void (*TiezGlassStateCallback)(void* context, int32_t state, int32_t hresult,
    uint64_t geometry_revision);

int32_t tiez_glass_create(void* host, void* qa_source, void* context,
    TiezGlassStateCallback on_state, TiezGlass** result) TIEZ_GLASS_NOEXCEPT;
int32_t tiez_glass_update(TiezGlass* glass, const TiezGlassState* state) TIEZ_GLASS_NOEXCEPT;
int32_t tiez_glass_set_active(TiezGlass* glass, bool active) TIEZ_GLASS_NOEXCEPT;
void tiez_glass_destroy(TiezGlass* glass) TIEZ_GLASS_NOEXCEPT;

#ifdef __cplusplus
}

// Shared by the renderer and its window-free native regression tests.
#include <algorithm>
#include <cmath>
namespace tiez_glass_detail {
constexpr uint64_t resize_cache_lifetime_ms = 1500;
constexpr uint32_t solid_background_extent = 8192;
inline uint32_t solid_tint_pixel(float red, float green, float blue) {
    // The caller validates finite [0,1] material parameters. No opacity or
    // sampled color enters this source-independent, fully opaque BGRA pixel.
    return 0xff000000u | (static_cast<uint32_t>(std::lround(red * 255.0f)) << 16) |
        (static_cast<uint32_t>(std::lround(green * 255.0f)) << 8) |
        static_cast<uint32_t>(std::lround(blue * 255.0f));
}
inline bool retain_solid_on_withdrawal(bool active, bool tearing_down, bool own_host,
    bool visible, bool healthy_device, bool solid_available) {
    return active && !tearing_down && own_host && visible && healthy_device && solid_available;
}
inline bool suppress_owned_nonclient_activation(bool active, bool tearing_down, bool own_host,
    bool visible, bool same_frame, bool borderless_client, bool exact_owned_mask) {
    return active && !tearing_down && own_host && visible && same_frame && borderless_client && exact_owned_mask;
}
struct PixelRect { int32_t left, top, right, bottom; };
inline bool borderless_client_insets(const PixelRect& client, const PixelRect& outer, uint32_t dpi) {
    const int64_t width = int64_t(client.right) - client.left, height = int64_t(client.bottom) - client.top;
    const int64_t top = int64_t(client.top) - outer.top;
    const int64_t topLimit = std::max<int64_t>(1, (uint64_t(dpi) + 48) / 96);
    return dpi && width > 0 && height > 0 && width <= solid_background_extent && height <= solid_background_extent &&
        client.left >= outer.left && client.right <= outer.right && client.bottom <= outer.bottom &&
        top >= 0 && top <= topLimit;
}
inline bool same_rect(const PixelRect& a, const PixelRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
struct ResizeContext {
    uintptr_t source, source_owner, monitor;
    uint32_t dpi;
    uint64_t display_epoch;
    PixelRect desktop;
    uint32_t frame_style = 0, frame_extended_style = 0;
};
struct ResizeSnapshot {
    ResizeContext context{};
    PixelRect crop{};
    uint64_t captured_at = 0;
};
struct ResizeLease {
    uint64_t started_at = 0;
    bool active = false;
    void begin(uint64_t now) {
        if (!active) { started_at = now; active = true; }
    }
    void refresh_from_capture(uint64_t now) {
        if (active) started_at = now;
    }
};
struct PresentationEpoch {
    uint64_t value = 1;
    void invalidate() { ++value; }
    template<typename Action, typename Cancel>
    bool perform(uint64_t expected, Action action, Cancel cancel) {
        if (value != expected) { cancel(); return false; }
        action();
        if (value == expected) return true;
        // A COM/DXGI call may have dispatched an invalidation while blocked.
        // Cancel its pending mount before a caller can advance or report ready.
        cancel();
        return false;
    }
};
struct ResizeSizeGate {
    bool waiting = false;
    bool position_known = false;
    uint32_t target_width = 0, target_height = 0;
    uint64_t generation = 0;
    void begin() {
        ++generation;
        waiting = true;
        position_known = false;
    }
    void cancel() {
        ++generation;
        waiting = position_known = false;
    }
    void position_changed(uint32_t width, uint32_t height) {
        if (!waiting || !width || !height) return;
        target_width = width;
        target_height = height;
        position_known = true;
    }
    bool accept(uint64_t message_generation, uint32_t message_width, uint32_t message_height,
        uint32_t actual_width, uint32_t actual_height) {
        if (!waiting || !position_known || message_generation != generation ||
            message_width != target_width || message_height != target_height ||
            message_width != actual_width || message_height != actual_height) return false;
        waiting = false;
        return true;
    }
};
inline bool reusable_resize(const ResizeSnapshot& capture, const ResizeContext& current,
    const ResizeLease& lease, uint64_t now, bool visible, bool scope_matches,
    bool region_matches, bool device_healthy) {
    const auto& previous = capture.context;
    return visible && scope_matches && region_matches && device_healthy &&
        now >= capture.captured_at && (!lease.active ||
            (now >= lease.started_at && now - lease.started_at < resize_cache_lifetime_ms)) &&
        previous.source == current.source && previous.source_owner == current.source_owner &&
        previous.monitor == current.monitor && previous.dpi == current.dpi && current.dpi != 0 &&
        previous.frame_style == current.frame_style && previous.frame_extended_style == current.frame_extended_style &&
        previous.display_epoch == current.display_epoch && same_rect(previous.desktop, current.desktop);
}
inline bool predict_resize_client(const PixelRect& client, const PixelRect& outer,
    int32_t next_left, int32_t next_top, int32_t next_width, int32_t next_height,
    PixelRect& predicted_client, PixelRect& predicted_outer) {
    const int64_t left = int64_t(client.left) - outer.left, top = int64_t(client.top) - outer.top;
    const int64_t right = int64_t(outer.right) - client.right, bottom = int64_t(outer.bottom) - client.bottom;
    const int64_t next_right = int64_t(next_left) + next_width, next_bottom = int64_t(next_top) + next_height;
    const int64_t width = int64_t(next_width) - left - right, height = int64_t(next_height) - top - bottom;
    if (left < 0 || top < 0 || right < 0 || bottom < 0 ||
        client.right <= client.left || client.bottom <= client.top ||
        next_width < 1 || next_height < 1 || next_width > 16384 || next_height > 16384 ||
        width < 1 || height < 1 || width > 8192 || height > 8192 ||
        next_right > INT32_MAX || next_bottom > INT32_MAX) return false;
    predicted_outer = {next_left, next_top, static_cast<int32_t>(next_right), static_cast<int32_t>(next_bottom)};
    predicted_client = {static_cast<int32_t>(int64_t(next_left) + left),
        static_cast<int32_t>(int64_t(next_top) + top), static_cast<int32_t>(next_right - right),
        static_cast<int32_t>(next_bottom - bottom)};
    return true;
}
struct ResizeCopy { uint32_t source_x, source_y, destination_x, destination_y, width, height; };
inline ResizeCopy resize_intersection(const PixelRect& source, const PixelRect& destination) {
    const auto left = std::max(source.left, destination.left);
    const auto top = std::max(source.top, destination.top);
    const auto right = std::min(source.right, destination.right);
    const auto bottom = std::min(source.bottom, destination.bottom);
    if (right <= left || bottom <= top) return {};
    return {static_cast<uint32_t>(left - source.left), static_cast<uint32_t>(top - source.top),
        static_cast<uint32_t>(left - destination.left), static_cast<uint32_t>(top - destination.top),
        static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)};
}
inline bool optics_ready(bool captured, uint32_t width, uint32_t height,
    float viewport_width, float viewport_height, float scale) {
    return captured && std::isfinite(scale) && scale > 0.0f &&
        std::isfinite(viewport_width) && std::isfinite(viewport_height) &&
        // window.innerWidth/Height round to whole CSS pixels, including at
        // fractional and high DPI. Compare in that same coordinate space.
        std::abs(viewport_width - static_cast<float>(width) / scale) <= 0.51f &&
        std::abs(viewport_height - static_cast<float>(height) / scale) <= 0.51f;
}
inline bool background_only(bool forced, bool captured, uint32_t width, uint32_t height,
    float viewport_width, float viewport_height, float scale) {
    return forced || !optics_ready(captured, width, height, viewport_width, viewport_height, scale);
}
inline bool refresh_background_surface(bool announce, bool forced, bool output_mounted) {
    // The opaque foreground swap contains the latest base on ordinary frames.
    // Snapshot the current base only when it must survive a resize without the
    // foreground. Continuous capture must never remount the composition tree.
    return announce && (forced || !output_mounted);
}
inline bool suppress_resize_nonclient_paint(bool own_host, bool active, bool output_mounted,
    bool resizing, bool failed, bool tearing_down, bool reusable_source, bool exact_owned_mask) {
    return own_host && active && output_mounted && resizing && !failed && !tearing_down &&
        reusable_source && exact_owned_mask;
}
} // namespace tiez_glass_detail
#endif
#undef TIEZ_GLASS_NOEXCEPT
