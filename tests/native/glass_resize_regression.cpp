// No HWNDs, capture, desktop pixels, or UI are used by this regression.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../../src-tauri/native/glass_renderer.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace tiez_glass_detail;
namespace {
int checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int world_pixel(int x, int y) { return 100000 + y * 200 + x; }

void intersection_case(PixelRect old_crop, PixelRect new_crop) {
    const uint32_t width = static_cast<uint32_t>(new_crop.right - new_crop.left);
    const uint32_t height = static_cast<uint32_t>(new_crop.bottom - new_crop.top);
    constexpr int neutral = -1;
    std::vector<int> output(static_cast<size_t>(width) * height, neutral);
    const auto copy = resize_intersection(old_crop, new_crop);
    for (uint32_t y = 0; y < copy.height; ++y)
        for (uint32_t x = 0; x < copy.width; ++x)
            output[static_cast<size_t>(y + copy.destination_y) * width + x + copy.destination_x] =
                world_pixel(old_crop.left + static_cast<int>(copy.source_x + x),
                    old_crop.top + static_cast<int>(copy.source_y + y));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const int screen_x = new_crop.left + static_cast<int>(x);
            const int screen_y = new_crop.top + static_cast<int>(y);
            const bool covered = screen_x >= old_crop.left && screen_x < old_crop.right &&
                screen_y >= old_crop.top && screen_y < old_crop.bottom;
            require(output[static_cast<size_t>(y) * width + x] ==
                (covered ? world_pixel(screen_x, screen_y) : neutral),
                "resize keeps screen pixels 1:1 and leaves uncovered bands neutral");
        }
    }
}

void presentation_case(int interrupted_step) {
    PresentationEpoch epoch;
    const uint64_t snapshot = epoch.value;
    bool mounted = false, published = false, cancelled = false;
    int performed = 0;
    auto cancel = [&] { mounted = false; cancelled = true; };
    // Run the production fence with injected invalidations from the blocking
    // Present/SetContent/SetRoot/Commit calls. No Windows messages are needed.
    for (int step = 0; step < 4; ++step) {
        if (!epoch.perform(snapshot, [&] {
            ++performed;
            if (step == 2) mounted = true;
            if (step == interrupted_step) epoch.invalidate();
        }, cancel)) break;
        if (step == 3) published = true;
    }
    if (interrupted_step < 4) {
        require(cancelled && !mounted && !published,
            "in-call invalidation cancels a pending mount and cannot publish ready/resizing");
        require(performed == interrupted_step + 1,
            "no following publication operation runs after an invalidation");
        bool staleExecuted = false;
        require(!epoch.perform(snapshot, [&] { staleExecuted = true; mounted = true; }, cancel) && !staleExecuted,
            "old render snapshot cannot remount on a subsequent attempt");
        require(epoch.perform(epoch.value, [&] { mounted = true; }, cancel) && mounted,
            "a newly validated render epoch may mount normally");
    } else {
        require(!cancelled && mounted && published && performed == 4,
            "uninterrupted rendering still mounts and publishes");
    }
}

void solid_fallback_cases() {
    require(solid_tint_pixel(0.0f, 0.0f, 0.0f) == 0xff000000u,
        "black fallback is opaque rather than an empty premultiplied pixel");
    require(solid_tint_pixel(1.0f, 1.0f, 1.0f) == 0xffffffffu,
        "white fallback retains full channel range and opacity");
    require(solid_tint_pixel(1.0f, 0.0f, 0.0f) == 0xffff0000u &&
        solid_tint_pixel(0.0f, 1.0f, 0.0f) == 0xff00ff00u &&
        solid_tint_pixel(0.0f, 0.0f, 1.0f) == 0xff0000ffu,
        "individual color primaries occupy the Windows BGRA byte positions");
    require(solid_tint_pixel(0.1f, 0.2f, 0.3f) == 0xff1a334du &&
        solid_tint_pixel(0.5f, 0.5f, 0.5f) == 0xff808080u,
        "fractional material colors round to the nearest byte without truncation");
    require(solid_tint_pixel(std::nextafter(0.5f, 0.0f), 0.0f, 0.0f) == 0xff7f0000u &&
        solid_tint_pixel(std::nextafter(0.5f, 1.0f), 0.0f, 0.0f) == 0xff800000u,
        "neighboring representable values around the half-byte threshold remain distinct");
    for (uint32_t channel = 0; channel < 256; ++channel) {
        const float normalized = static_cast<float>(channel) / 255.0f;
        require(solid_tint_pixel(normalized, 0.0f, 1.0f) == (0xff0000ffu | (channel << 16)),
            "all red byte values round-trip without changing alpha or other channels");
        require(solid_tint_pixel(1.0f, normalized, 0.0f) == (0xffff0000u | (channel << 8)),
            "all green byte values round-trip without changing alpha or other channels");
        require(solid_tint_pixel(0.0f, 1.0f, normalized) == (0xff00ff00u | channel),
            "all blue byte values round-trip without changing alpha or other channels");
    }

    require(solid_background_extent == 8192u,
        "the one-pixel fallback covers the renderer's full supported client extent");
    const PixelRect client{458, 441, 883, 1030}, outer{450, 440, 891, 1038};
    PixelRect maximumClient{}, maximumOuter{};
    require(predict_resize_client(client, outer, 450, 440, 8208, 8201, maximumClient, maximumOuter) &&
        maximumClient.right - maximumClient.left == static_cast<int32_t>(solid_background_extent) &&
        maximumClient.bottom - maximumClient.top == static_cast<int32_t>(solid_background_extent),
        "the largest accepted client in both dimensions fits the solid visual including measured frame insets");
    for (const POINT dimensions : {POINT{8209, 8201}, POINT{8208, 8202}}) {
        const PixelRect beforeClient = maximumClient, beforeOuter = maximumOuter;
        require(!predict_resize_client(client, outer, 450, 440, dimensions.x, dimensions.y,
            maximumClient, maximumOuter) && same_rect(maximumClient, beforeClient) && same_rect(maximumOuter, beforeOuter),
            "one pixel beyond either fallback extent is rejected without publishing new bounds");
    }
    require(borderless_client_insets(client, outer, 96),
        "the measured 96-DPI one-pixel top inset qualifies for owned activation handling");
    require(borderless_client_insets({0, 0, 1, 1}, {0, 0, 1, 1}, 96),
        "a nonempty borderless client with no frame insets remains eligible");
    require(!borderless_client_insets({458, 463, 883, 1030}, outer, 96),
        "a normal caption-height top inset cannot masquerade as a borderless frame");
    require(!borderless_client_insets({458, 442, 883, 1030}, outer, 96),
        "two top pixels exceed the 96-DPI borderless allowance");
    require(borderless_client_insets({12, 2, 650, 883}, {0, 0, 662, 895}, 144) &&
        !borderless_client_insets({12, 3, 650, 883}, {0, 0, 662, 895}, 144),
        "144-DPI rounding accepts the two-pixel thin top edge but not a third pixel");
    require(borderless_client_insets({16, 2, 866, 1180}, {0, 0, 882, 1196}, 192) &&
        !borderless_client_insets({16, 3, 866, 1180}, {0, 0, 882, 1196}, 192),
        "192-DPI borderless allowance includes exactly two top pixels");
    require(!borderless_client_insets(client, outer, 0),
        "unknown DPI never grants the activation paint exception");
    require(borderless_client_insets({-742, -599, -317, -10}, {-750, -600, -309, -2}, 96),
        "negative desktop coordinates retain the same valid borderless insets");
    require(borderless_client_insets(maximumClient, maximumOuter, 96),
        "the exact 8192-square supported client remains covered by the activation guard");
    require(!borderless_client_insets({0, 0, 8193, 8192}, {0, 0, 8193, 8192}, 96) &&
        !borderless_client_insets({0, 0, 8192, 8193}, {0, 0, 8192, 8193}, 96),
        "the activation guard rejects either client dimension beyond the solid fallback coverage");
    for (const PixelRect outside : {PixelRect{449, 441, 883, 1030}, PixelRect{458, 439, 883, 1030},
        PixelRect{458, 441, 892, 1030}, PixelRect{458, 441, 883, 1039}}) {
        require(!borderless_client_insets(outside, outer, 96),
            "a client outside any measured outer-frame edge cannot suppress activation painting");
    }
    for (const PixelRect empty : {PixelRect{458, 441, 458, 1030}, PixelRect{458, 441, 883, 441},
        PixelRect{883, 441, 458, 1030}, PixelRect{458, 1030, 883, 441}}) {
        require(!borderless_client_insets(empty, outer, 96),
            "empty or inverted client dimensions cannot suppress activation painting");
    }
    require(borderless_client_insets({INT32_MIN, INT32_MIN + 1, INT32_MIN + 1, INT32_MIN + 2},
        {INT32_MIN, INT32_MIN, INT32_MIN + 1, INT32_MIN + 2}, 96) &&
        borderless_client_insets({INT32_MAX - 1, INT32_MAX - 1, INT32_MAX, INT32_MAX},
            {INT32_MAX - 1, INT32_MAX - 2, INT32_MAX, INT32_MAX}, 96),
        "valid one-pixel clients at both signed coordinate extremes avoid overflow");
    require(!borderless_client_insets({INT32_MIN, 0, INT32_MAX, 1},
        {INT32_MIN, 0, INT32_MAX, 1}, 96) &&
        !borderless_client_insets({0, INT32_MIN, 1, INT32_MAX}, {0, INT32_MIN, 1, INT32_MAX}, 96),
        "overflow-sized client extents cannot wrap into an apparently small covered client");
    require(!borderless_client_insets({0, INT32_MAX - 1, 1, INT32_MAX},
        {0, INT32_MIN, 1, INT32_MAX}, UINT32_MAX),
        "extreme top offset and DPI arithmetic cannot wrap into the thin-edge allowance");

    // Enumerate the contract's complete state spaces. Only the named safe state
    // may retain a source-free surface or suppress default activation painting.
    constexpr unsigned retainedState = 0b111101; // active, !teardown, own, visible, healthy, solid
    for (unsigned state = 0; state < 64; ++state) {
        require(retain_solid_on_withdrawal((state & 1) != 0, (state & 2) != 0,
            (state & 4) != 0, (state & 8) != 0, (state & 16) != 0, (state & 32) != 0) ==
            (state == retainedState),
            "inactive, teardown, foreign, invisible, device-lost or absent-solid states cannot retain fallback");
    }
    constexpr unsigned activationState = 0b1111101; // active, !teardown, own, visible, frame, borderless, exact mask
    for (unsigned state = 0; state < 128; ++state) {
        require(suppress_owned_nonclient_activation((state & 1) != 0, (state & 2) != 0,
            (state & 4) != 0, (state & 8) != 0, (state & 16) != 0,
            (state & 32) != 0, (state & 64) != 0) == (state == activationState),
            "activation suppression requires the exact visible owned borderless frame even when several guards fail");
    }
    const ResizeContext context{10, 0, 20, 144, 3, {-1920, -300, 2560, 1440}};
    const ResizeSnapshot sampled{context, {-25, 30, 85, 100}, 1000};
    const ResizeLease expired{1000, true};
    require(!reusable_resize(sampled, context, expired, 2500, true, true, true, true) &&
        retain_solid_on_withdrawal(true, false, true, true, true, true) &&
        suppress_owned_nonclient_activation(true, false, true, true, true, true, true),
        "expired sampled pixels are withdrawn while source-free fallback and owned activation guard remain eligible");
    require(!reusable_resize(sampled, context, ResizeLease{}, 1100, true, false, true, true) &&
        retain_solid_on_withdrawal(true, false, true, true, true, true) &&
        suppress_owned_nonclient_activation(true, false, true, true, true, true, true),
        "source scope rejection does not make a pure parameter color or exact owned mask depend on raw capture readiness");
}
}

int main() {
    solid_fallback_cases();
    for (int step = 0; step <= 4; ++step) presentation_case(step);
    ResizeSizeGate sizeGate;
    sizeGate.begin();
    require(!sizeGate.accept(sizeGate.generation, 425, 589, 425, 589) && sizeGate.waiting,
        "WM_SIZE pumped before actual POSITIONCHANGED cannot release the resize rebuild");
    sizeGate.position_changed(280, 589);
    require(!sizeGate.accept(sizeGate.generation, 280, 589, 425, 589) && sizeGate.waiting,
        "message dimensions must agree with the actual client before rebuilding");
    require(!sizeGate.accept(sizeGate.generation, 320, 589, 320, 589) && sizeGate.waiting,
        "matching message and actual sizes still need to match the observed resize target");
    require(sizeGate.accept(sizeGate.generation, 280, 589, 280, 589) && !sizeGate.waiting,
        "processed new WM_SIZE releases the rebuild gate");
    require(!sizeGate.accept(sizeGate.generation, 280, 589, 280, 589),
        "duplicate WM_SIZE does not re-release a completed gate");
    // A -> B finished WM_SIZE, but begin_capture has not replaced the A
    // snapshot yet. A second real resize returns to A in that same interval.
    const uint64_t previousMessageGeneration = sizeGate.generation;
    sizeGate.begin();
    require(!sizeGate.accept(sizeGate.generation, 280, 589, 280, 589) && sizeGate.waiting,
        "beginning another resize clears the previous POSITIONCHANGED observation");
    sizeGate.position_changed(425, 589);
    require(!sizeGate.accept(previousMessageGeneration, 425, 589, 425, 589) && sizeGate.waiting,
        "an older reentrant WM_SIZE cannot release a newer target even when its numbers match");
    require(sizeGate.accept(sizeGate.generation, 425, 589, 425, 589) && !sizeGate.waiting,
        "A-to-B-to-A accepts the actual second resize despite the unchanged capture snapshot");
    sizeGate.begin();
    sizeGate.position_changed(280, 589);
    const uint64_t cancelledGeneration = sizeGate.generation;
    sizeGate.cancel();
    require(!sizeGate.accept(cancelledGeneration, 280, 589, 280, 589),
        "safety cancellation cannot be reversed by a queued size message");
    const PixelRect originalOuter{450, 440, 891, 1038};
    const PixelRect originalClient{458, 441, 883, 1030};
    PixelRect predictedClient{}, predictedOuter{};
    sizeGate.begin();
    require(predict_resize_client(originalClient, originalOuter, 450, 440, 296, 598,
        predictedClient, predictedOuter), "measured client insets can predict a normal shrink");
    require(same_rect(predictedClient, {458, 441, 738, 1030}) &&
        same_rect(predictedOuter, {450, 440, 746, 1038}),
        "prediction preserves measured 8/1/8/8 pixel insets and the requested outer extent");
    require(!sizeGate.position_known &&
        !sizeGate.accept(sizeGate.generation, 280, 589, 280, 589) && sizeGate.waiting,
        "predicting a mask cannot unlock WM_SIZE before actual POSITIONCHANGED");
    sizeGate.position_changed(280, 589);
    require(sizeGate.accept(sizeGate.generation, 280, 589, 280, 589),
        "actual POSITIONCHANGED and processed WM_SIZE still unlock a predicted resize");
    const auto predictedCopy = resize_intersection(originalClient, predictedClient);
    require(predictedCopy.source_x == 0 && predictedCopy.source_y == 0 &&
        predictedCopy.destination_x == 0 && predictedCopy.destination_y == 0 &&
        predictedCopy.width == 280 && predictedCopy.height == 589,
        "prediction keeps the original screen-space source and a one-to-one intersection");
    PixelRect expandedClient{}, expandedOuter{};
    require(predict_resize_client(predictedClient, predictedOuter, 450, 440, 441, 598,
        expandedClient, expandedOuter) && same_rect(expandedClient, originalClient) &&
        same_rect(expandedOuter, originalOuter), "inverse expansion returns to the original geometry");
    require(predict_resize_client(originalClient, originalOuter, -650, -240, 496, 698,
        expandedClient, expandedOuter) && same_rect(expandedClient, {-642, -239, -162, 450}) &&
        same_rect(expandedOuter, {-650, -240, -154, 458}),
        "left/top resizing on negative desktop coordinates preserves the measured frame");
    require(predict_resize_client(originalClient, originalOuter, INT32_MIN, INT32_MIN, 17, 10,
        expandedClient, expandedOuter) && expandedClient.right - expandedClient.left == 1 &&
        expandedClient.bottom - expandedClient.top == 1,
        "valid minimum coordinates and a one-pixel client avoid signed overflow");
    for (const auto dimensions : {POINT{16, 598}, POINT{441, 9}, POINT{0, 598}, POINT{-1, 598},
        POINT{8209, 598}, POINT{441, 8202}, POINT{16385, 598}, POINT{441, 16385}}) {
        const PixelRect beforeClient = expandedClient, beforeOuter = expandedOuter;
        require(!predict_resize_client(originalClient, originalOuter, 450, 440, dimensions.x, dimensions.y,
            expandedClient, expandedOuter) && same_rect(beforeClient, expandedClient) &&
            same_rect(beforeOuter, expandedOuter), "invalid targets fail without publishing a partial prediction");
    }
    require(!predict_resize_client(originalClient, originalOuter, INT32_MAX - 440, 440, 441, 598,
        expandedClient, expandedOuter), "right-coordinate overflow rejects prediction");
    require(!predict_resize_client(originalClient, originalOuter, 450, INT32_MAX - 597, 441, 598,
        expandedClient, expandedOuter), "bottom-coordinate overflow rejects prediction");
    require(!predict_resize_client({449, 441, 883, 1030}, originalOuter, 450, 440, 441, 598,
        expandedClient, expandedOuter), "client outside the measured outer frame cannot be predicted");
    require(!predict_resize_client({458, 441, 458, 1030}, originalOuter, 450, 440, 441, 598,
        expandedClient, expandedOuter), "empty measured client cannot be predicted");
    const HRGN oldMask = CreateRoundRectRgn(8, 1, 8 + 425 + 1, 1 + 589 + 1, 32, 32);
    const HRGN newMask = CreateRoundRectRgn(predictedClient.left - predictedOuter.left,
        predictedClient.top - predictedOuter.top, predictedClient.right - predictedOuter.left + 1,
        predictedClient.bottom - predictedOuter.top + 1, 32, 32);
    require(oldMask && newMask, "native GDI test masks were created without any HWND");
    unsigned oldNonClientPixels = 0, newNonClientPixels = 0;
    for (int x = 8 + 280; x < 296; ++x) {
        oldNonClientPixels += PtInRegion(oldMask, x, 200) != FALSE;
        newNonClientPixels += PtInRegion(newMask, x, 200) != FALSE;
    }
    require(oldNonClientPixels == 8, "previous wide mask exposes all eight new right non-client pixels");
    require(newNonClientPixels == 0, "synchronous client mask excludes the complete observed white-strip region");
    RECT newExtent{};
    require(GetRgnBox(newMask, &newExtent) != ERROR && newExtent.left == 8 && newExtent.top == 1 &&
        newExtent.right == 288 && newExtent.bottom == 590,
        "new owned mask keeps the exact client extent and original non-client offsets");
    require(PtInRegion(newMask, 8 + 279, 200) != FALSE,
        "excluding the non-client strip does not clip the last client pixel");
    DeleteObject(oldMask);
    DeleteObject(newMask);
    const ResizeContext original{10, 0, 20, 144, 3, {-1920, -300, 2560, 1440}};
    const ResizeSnapshot capture{original, {-25, 30, 85, 100}, 1000};
    const ResizeLease active_lease{1000, true};
    auto reusable = [&](ResizeContext context, uint64_t now, bool visible = true,
        bool scope = true, bool region = true, bool device = true) {
        return reusable_resize(capture, context, active_lease, now, visible, scope, region, device);
    };
    require(reusable(original, 1000), "fresh real capture is reusable");
    require(reusable(original, 2499), "cache remains valid inside its lifetime");
    require(!reusable(original, 2500), "cache expires at its original deadline");
    require(!reusable(original, 999), "clock reversal cannot revive a cache");
    require(!reusable(original, 1100, false), "hidden or minimized host cannot inherit cache");
    require(!reusable(original, 1100, true, false), "different exclusions cannot inherit cache");
    require(!reusable(original, 1100, true, true, false), "changed or missing region cannot inherit cache");
    require(!reusable(original, 1100, true, true, true, false), "removed device cannot inherit cache");
    for (int changed = 0; changed < 8; ++changed) {
        auto current = original;
        switch (changed) {
        case 0: ++current.source; break;
        case 1: ++current.source_owner; break;
        case 2: ++current.monitor; break;
        case 3: current.dpi = 192; break;
        case 4: ++current.display_epoch; break;
        case 5: ++current.desktop.left; break;
        case 6: current.frame_style = WS_THICKFRAME; break;
        case 7: current.frame_extended_style = WS_EX_CLIENTEDGE; break;
        }
        require(!reusable(current, 1100), "source/owner/monitor/DPI/display/frame change rejects cached pixels");
    }
    for (uint64_t now = 1000; now < 2600; now += 10)
        require(reusable(original, now) == (now < 2500), "repeated resizes cannot renew capture lifetime");

    ResizeLease after_idle{};
    require(reusable_resize(capture, original, after_idle, 100000, true, true, true, true),
        "a long-idle but still valid real source can begin the first resize");
    after_idle.begin(100000);
    for (uint64_t now = 100100; now < 101600; now += 100) {
        after_idle.begin(now);
        require(after_idle.started_at == 100000,
            "repeated resize and output reconstruction do not restart the lease");
        require(reusable_resize(capture, original, after_idle, now, true, true, true, true) ==
            (now < 101500), "idle source has a bounded lease beginning at the first resize");
    }
    auto new_capture = capture;
    new_capture.captured_at = 101600;
    after_idle.refresh_from_capture(new_capture.captured_at);
    require(reusable_resize(new_capture, original, after_idle, 101601, true, true, true, true),
        "a newly validated real capture can replace and renew the resize source");
    require(!reusable_resize(capture, original, ResizeLease{}, 100000, true, false, true, true),
        "idle status never bypasses changed scope validation");

    // Both edges may move during left/top resizing; screen-space identity, not
    // UV stretching or edge clamp, defines the expected result independently.
    for (int dx : {-50, -10, 0, 25, 200})
        for (int dy : {-50, 0, 20, 100})
            for (int size : {40, 70, 120})
                intersection_case(capture.crop, {-25 + dx, 30 + dy, -25 + dx + size, 30 + dy + size});

    require(background_only(true, true, 900, 600, 600, 400, 1.5f),
        "pre-resize presentation suppresses old optics even while old viewport matches");
    require(background_only(false, false, 900, 600, 600, 400, 1.5f),
        "composite cached background cannot enable optics");
    require(background_only(false, true, 1050, 600, 600, 400, 1.5f),
        "fresh capture cannot enable optics from an older viewport");
    require(!background_only(false, true, 1050, 600, 700, 400, 1.5f),
        "matching fresh capture and viewport restore optics");
    require(optics_ready(true, 1001, 751, 800.8f, 600.8f, 1.25f),
        "fractional DIP geometry accepts physical pixel rounding");
    require(optics_ready(true, 1000, 751, 571, 429, 1.75f),
        "integer CSS viewport rounding remains valid at 175 percent DPI");
    require(optics_ready(true, 1001, 751, 501, 376, 2.0f),
        "half CSS pixel rounding remains valid at 200 percent DPI");
    require(optics_ready(true, 1000, 751, 572, 430, 1.75f),
        "upward CSS viewport rounding remains valid at 175 percent DPI");
    require(optics_ready(true, 572, 596, 382, 398, 1.5f),
        "WebView2's observed upward rounding at 150 percent DPI is accepted");
    require(!optics_ready(true, 1001, 751, 799, 599, 1.25f),
        "a viewport more than one CSS pixel stale does not pass the gate");
    require(!optics_ready(true, 1000, 750, 801, 600, 1.25f),
        "a whole CSS pixel of difference is stale geometry");
    require(!refresh_background_surface(true, false, true),
        "dynamic captures, material and control updates do not remount the foreground");
    require(refresh_background_surface(true, true, true),
        "trusted resize preparation explicitly publishes its current material base");
    require(refresh_background_surface(true, false, false),
        "remount after withdrawal still publishes output");
    require(!refresh_background_surface(false, true, true) &&
        !refresh_background_surface(false, true, false),
        "output configuration leaves its one candidate publication to configure_output");
    require(suppress_resize_nonclient_paint(true, true, true, true, false, false, true, true),
        "trusted native resize with an exact owned mask handles its own non-client paint");
    for (int disabled = 0; disabled < 8; ++disabled) {
        bool flags[]{true, true, true, true, false, false, true, true};
        flags[disabled] = !flags[disabled];
        require(!suppress_resize_nonclient_paint(flags[0], flags[1], flags[2], flags[3],
            flags[4], flags[5], flags[6], flags[7]),
            "foreign/inactive/unmounted/nonresize/failed/teardown/stale/unowned cases preserve default NC painting");
    }
    std::printf("PASS: %d native resize checks (synthetic pixels; no desktop capture).\n", checks);
    return 0;
}
