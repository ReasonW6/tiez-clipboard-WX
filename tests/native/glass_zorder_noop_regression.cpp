// Pure policy/message-sequence checks: no HWND is created or queried, no UI,
// capture, desktop pixels or D3D device is used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "glass_zorder_noop.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace tiez_glass_detail;
namespace {
struct Check { std::string name; bool passed; };
std::vector<Check> checks;
void check(const char* name, bool passed) { checks.push_back({name, passed}); }
HWND window(uintptr_t id) { return reinterpret_cast<HWND>(0x1000 + id); }
constexpr UINT stationary = SWP_NOMOVE | SWP_NOSIZE;
WINDOWPOS request(HWND host, HWND after, UINT extra = SWP_NOACTIVATE) {
    return {host, after, 999, -999, 1, 8192, stationary | extra};
}

void classification_cases() {
    const auto host = window(1), cover = window(2), other = window(3), top = window(4);
    const ZOrderSnapshot pinned{host, nullptr, cover, true, false};
    const ZOrderSnapshot normalFirst{host, top, cover, false, true};
    const ZOrderSnapshot normalMiddle{host, other, cover, false, false};
    const ZOrderSnapshot pinnedMiddle{host, top, cover, true, true};
    for (const UINT activation : {0u, UINT(SWP_NOACTIVATE)}) {
        check("already-first pinned TOPMOST preserves the session policy",
            z_order_request_is_noop(request(host, HWND_TOPMOST, activation), pinned));
        check("already-first pinned TOP remains in its band",
            z_order_request_is_noop(request(host, HWND_TOP, activation), pinned));
        check("already-first ordinary TOP remains below the topmost band",
            z_order_request_is_noop(request(host, HWND_TOP, activation), normalFirst));
        check("already-first ordinary NOTOPMOST is unchanged",
            z_order_request_is_noop(request(host, HWND_NOTOPMOST, activation), normalFirst));
        check("a real topmost promotion is rejected before changing",
            !z_order_request_is_noop(request(host, HWND_TOPMOST, activation), normalFirst));
        check("a real topmost demotion is rejected before changing",
            !z_order_request_is_noop(request(host, HWND_NOTOPMOST, activation), pinned));
        check("raising over another pinned window is not a no-op",
            !z_order_request_is_noop(request(host, HWND_TOPMOST, activation), pinnedMiddle));
        check("raising over an ordinary window is not a no-op",
            !z_order_request_is_noop(request(host, HWND_TOP, activation), normalMiddle));
    }
    check("nonactivating repeated explicit same-band predecessor is unchanged",
        z_order_request_is_noop(request(host, other), normalMiddle));
    check("activation cannot silently promote a middle window through the fast path",
        !z_order_request_is_noop(request(host, other, 0), normalMiddle));
    check("NOTOPMOST on an ordinary window has no effect without activation",
        z_order_request_is_noop(request(host, HWND_NOTOPMOST), normalMiddle));
    check("NOTOPMOST plus possible activation in the middle is rejected",
        !z_order_request_is_noop(request(host, HWND_NOTOPMOST, 0), normalMiddle));
    check("an ordinary insertion after the topmost band is not assumed equivalent",
        !z_order_request_is_noop(request(host, top), normalFirst));
    check("BOTTOM is not opted into the optimization",
        !z_order_request_is_noop(request(host, HWND_BOTTOM), {host, other, nullptr, false, false}));
    check("crossing a helper still uses the original withdrawal path",
        !z_order_request_is_noop(request(host, cover), pinned));
    check("crossing an unrelated same-process popup is also rejected",
        !z_order_request_is_noop(request(host, window(5)), pinned));
    check("explicit self insertion is not a retention proof",
        !z_order_request_is_noop(request(host, host), pinned));
    check("a missing host cannot qualify", !z_order_request_is_noop(request(nullptr, HWND_TOP), {}));
    check("a different target HWND cannot qualify",
        !z_order_request_is_noop(request(other, HWND_TOPMOST), pinned));
    check("impossible band ordering cannot qualify",
        !z_order_request_is_noop(request(host, other), {host, other, cover, true, false}));
    for (const UINT flag : {UINT(SWP_SHOWWINDOW), UINT(SWP_HIDEWINDOW), UINT(SWP_FRAMECHANGED),
        UINT(SWP_NOCOPYBITS), UINT(SWP_NOREDRAW), UINT(SWP_NOZORDER)}) {
        check("mixed rendering/visibility/frame requests keep the normal path",
            !z_order_request_is_noop(request(host, HWND_TOPMOST, SWP_NOACTIVATE | flag), pinned));
    }
    for (const UINT flag : {UINT(SWP_NOMOVE), UINT(SWP_NOSIZE)}) {
        auto moving = request(host, HWND_TOPMOST);
        moving.flags &= ~flag;
        check("position and size changes cannot be hidden by the z-order exception",
            !z_order_request_is_noop(moving, pinned));
    }
    auto ignored = request(host, other, SWP_NOACTIVATE | SWP_NOZORDER);
    check("a final NOZORDER request may ignore an irrelevant insertion anchor",
        z_order_request_is_noop(ignored, pinned, true));

    // Independent permutation oracle: enumerate small actual orders, apply
    // each accepted insertion to the full list, and require list identity.
    // Omitted windows are unrelated roots, not a privileged QA source.
    std::array<HWND, 5> order{host, cover, other, top, window(5)};
    unsigned accepted = 0, rejected = 0;
    do {
        const auto index = std::find(order.begin(), order.end(), host) - order.begin();
        const ZOrderSnapshot snapshot{host, index ? order[index - 1] : nullptr,
            index < 4 ? order[index + 1] : nullptr, false, false};
        for (HWND anchor : {HWND_TOP, HWND_TOPMOST, HWND_BOTTOM, HWND_NOTOPMOST,
            host, cover, other, top, window(5)}) {
            if (!z_order_request_is_noop(request(host, anchor), snapshot)) { ++rejected; continue; }
            ++accepted;
            std::vector<HWND> actual(order.begin(), order.end());
            if (anchor != HWND_NOTOPMOST) {
                actual.erase(std::find(actual.begin(), actual.end(), host));
                if (anchor != HWND_TOP && std::find(actual.begin(), actual.end(), anchor) == actual.end()) {
                    check("an accepted insertion anchor must exist independently of the removed host", false);
                    continue;
                }
                const auto insertion = anchor == HWND_TOP ? actual.begin() :
                    std::find(actual.begin(), actual.end(), anchor) + 1;
                actual.insert(insertion, host);
            }
            check("every accepted ordinary-window request preserves the independent full-order oracle",
                std::equal(actual.begin(), actual.end(), order.begin()));
        }
    } while (std::next_permutation(order.begin(), order.end(), [](HWND a, HWND b) {
        return reinterpret_cast<uintptr_t>(a) < reinterpret_cast<uintptr_t>(b);
    }));
    check("permutation test exercised both retention and withdrawal", accepted == 240 && rejected == 840);
}

void message_cases() {
    const ZOrderSnapshot order{window(1), nullptr, window(2), true, false};
    const auto original = request(order.host, HWND_TOPMOST, 0);
    ZOrderNoopGate gate;
    const auto first = gate.begin(original, order, 100);
    check("CHANGING begins one bounded transaction", first && gate.current(first) && gate.started_at == 100);
    check("frame processing must wait before the downstream procedure returns",
        gate.waiting && !gate.can_finish(first, order, 101));
    check("reentrant CHANGING cannot renew or replace the original proof",
        gate.begin(original, order, 200) == 0 && gate.started_at == 100 && gate.generation == first);
    check("CHANGED received during the lower procedure cannot finish the outer request",
        !gate.can_finish(first, order, 200));
    check("unchanged default-procedure return is accepted",
        gate.after_default(first, original, order, 201));
    check("a queued recheck can finish when no CHANGED is generated", gate.can_finish(first, order, 202));
    check("the final millisecond before the fixed deadline remains valid", gate.can_finish(first, order, 349));
    check("the exact deadline cannot be renewed by a matching request", !gate.can_finish(first, order, 350));
    check("a backward clock observation cannot grant retention", !gate.can_finish(first, order, 99));
    gate.cancel();
    check("cancelled state releases the frame gate", !gate.waiting && !gate.default_returned);
    check("late CHANGED cannot revive cancelled content", !gate.can_finish(first, order, 202));
    const auto second = gate.begin(original, order, 400);
    check("later transactions receive a new generation", second && second != first);
    check("a stale downstream return cannot authorize the next request",
        !gate.after_default(first, original, order, 401) && !gate.default_returned);
    auto rewritten = original;
    rewritten.hwndInsertAfter = HWND_NOTOPMOST;
    check("a downstream band rewrite is rejected", !gate.after_default(second, rewritten, order, 401));
    rewritten = original;
    rewritten.flags &= ~SWP_NOMOVE;
    check("a downstream move rewrite is rejected", !gate.after_default(second, rewritten, order, 401));
    rewritten = original;
    rewritten.flags |= SWP_HIDEWINDOW;
    check("a downstream visibility rewrite is rejected", !gate.after_default(second, rewritten, order, 401));
    check("failed revalidation never arms completion", !gate.default_returned);
    for (unsigned changed = 0; changed < 5; ++changed) {
        auto altered = order;
        if (changed == 0) altered.host = window(9);
        if (changed == 1) altered.previous = window(8);
        if (changed == 2) altered.next = window(7);
        if (changed == 3) altered.topmost = false;
        if (changed == 4) altered.previous_topmost = true;
        check("host, neighbors and both band attributes must survive the lower procedure",
            !gate.after_default(second, original, altered, 402));
    }
    check("valid return can still be processed without restarting resources",
        gate.after_default(second, original, order, 410) && gate.can_finish(second, order, 411));
    auto covered = order;
    covered.previous = window(8);
    covered.previous_topmost = true;
    check("a later external reorder invalidates completion even after a valid default return",
        !gate.can_finish(second, covered, 412));
    gate.cancel();
    ZOrderNoopGate changedOnly;
    check("CHANGED without a prior CHANGING proof cannot retain a session",
        !changedOnly.can_finish(second, order, 413));

    // The old host procedure unconditionally stopped capture for this exact
    // request. Keep that counterexample explicit in the regression evidence.
    check("old unconditional z-order branch would withdraw the accepted pinned no-op",
        !(original.flags & SWP_NOZORDER) && z_order_request_is_noop(original, order));
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    classification_cases();
    message_cases();
    size_t failed = 0;
    std::ofstream output(argv[1]);
    output << "{\"checks\":[\n";
    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& item = checks[i];
        if (!item.passed) { ++failed; std::fprintf(stderr, "FAIL: %s\n", item.name.c_str()); }
        output << (i ? ",\n" : "") << "{\"name\":\"" << item.name << "\",\"passed\":"
            << (item.passed ? "true" : "false") << '}';
    }
    output << "\n],\"total\":" << checks.size() << ",\"failed\":" << failed
        << ",\"passed\":" << (failed ? "false" : "true") << "}\n";
    std::printf("Z-order no-op policy: %zu checks, %zu failed; no HWND or capture used\n", checks.size(), failed);
    return output && !failed ? 0 : 1;
}
