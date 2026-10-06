#pragma once

#include <windows.h>
#include <cstdint>

namespace tiez_glass_detail {

// No capture or HWND calls here. The renderer supplies an independently
// validated scene snapshot before and after the normal window procedure.
struct ZOrderSnapshot {
    HWND host = nullptr, previous = nullptr, next = nullptr;
    bool topmost = false, previous_topmost = false;
};

inline bool same_z_order(const ZOrderSnapshot& a, const ZOrderSnapshot& b) {
    return a.host == b.host && a.previous == b.previous && a.next == b.next &&
        a.topmost == b.topmost && a.previous_topmost == b.previous_topmost;
}

inline bool pure_z_order_request(const WINDOWPOS& position, HWND host, bool allow_no_z_order = false) {
    constexpr UINT unrelated = SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED |
        SWP_NOCOPYBITS | SWP_NOREDRAW;
    return host && position.hwnd == host &&
        (position.flags & (SWP_NOMOVE | SWP_NOSIZE)) == (SWP_NOMOVE | SWP_NOSIZE) &&
        !(position.flags & unrelated) && (allow_no_z_order || !(position.flags & SWP_NOZORDER));
}

inline bool z_order_request_is_noop(const WINDOWPOS& position, const ZOrderSnapshot& order,
    bool allow_no_z_order = false) {
    if (!pure_z_order_request(position, order.host, allow_no_z_order) ||
        order.previous == order.host || order.next == order.host ||
        (order.previous && order.previous == order.next) ||
        (order.topmost && order.previous && !order.previous_topmost)) return false;
    const bool first_in_band = !order.previous || (!order.topmost && order.previous_topmost);
    // Activating an inactive window can raise it even when the explicit
    // insertion anchor is already its predecessor. Admit only an existing
    // band leader when activation is allowed; do not change the request.
    if (!(position.flags & SWP_NOACTIVATE) && !first_in_band) return false;
    if (position.flags & SWP_NOZORDER) return true;
    if (position.hwndInsertAfter == HWND_TOP) return first_in_band;
    if (position.hwndInsertAfter == HWND_TOPMOST) return order.topmost && !order.previous;
    if (position.hwndInsertAfter == HWND_NOTOPMOST) return !order.topmost;
    // BOTTOM and band/owner-group moves intentionally keep the existing
    // withdrawal path. An ordinary anchor must already precede this HWND in
    // the same band; a topmost anchor for a non-topmost host is not equivalent.
    return position.hwndInsertAfter != HWND_BOTTOM && order.previous &&
        position.hwndInsertAfter == order.previous && order.topmost == order.previous_topmost;
}

inline bool z_order_request_can_be_observed(const WINDOWPOS& position,
    const ZOrderSnapshot& order, bool allow_no_z_order = false) {
    return z_order_request_is_noop(position, order, allow_no_z_order) ||
        (pure_z_order_request(position, order.host, allow_no_z_order) &&
         order.topmost && position.hwndInsertAfter == HWND_TOP);
}

struct ZOrderNoopGate {
    static constexpr uint64_t lifetime_ms = 250;
    uint64_t generation = 0, started_at = 0;
    bool waiting = false, default_returned = false;
    ZOrderSnapshot order{};

    bool current(uint64_t token) const { return waiting && token && token == generation; }
    bool within_deadline(uint64_t now) const {
        return waiting && now >= started_at && now - started_at < lifetime_ms;
    }
    uint64_t begin(const WINDOWPOS& position, const ZOrderSnapshot& snapshot, uint64_t now) {
        // TOP on an existing topmost HWND can be held below protected system
        // windows. Freeze capture while observing it; only identical actual
        // order after the normal procedure may complete this transaction.
        if (waiting || !z_order_request_can_be_observed(position, snapshot)) return 0;
        ++generation;
        started_at = now;
        waiting = true;
        default_returned = false;
        order = snapshot;
        return generation;
    }
    bool after_default(uint64_t token, const WINDOWPOS& position,
        const ZOrderSnapshot& snapshot, uint64_t now) {
        if (!current(token) || !within_deadline(now) || !same_z_order(order, snapshot) ||
            !z_order_request_can_be_observed(position, snapshot, true)) return false;
        default_returned = true;
        return true;
    }
    bool can_finish(uint64_t token, const ZOrderSnapshot& snapshot, uint64_t now) const {
        return current(token) && default_returned && within_deadline(now) && same_z_order(order, snapshot);
    }
    void cancel() {
        ++generation;
        waiting = default_returned = false;
    }
};

} // namespace tiez_glass_detail
