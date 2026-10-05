// Production BlurPS regression. Offscreen WARP only; no HWND or desktop access.
#define wmain shared_optics_regression_entrypoint
#include "glass_gpu_regression.cpp"
#undef wmain

Pixels blurFixture(unsigned kind, unsigned phase = 0) {
    constexpr unsigned glyphs[4][7] = {
        {14,17,16,23,17,17,14}, {16,16,16,16,16,16,31},
        {14,17,17,31,17,17,17}, {15,16,16,14,1,1,30}
    };
    Pixels pixels(Width * Height);
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
        unsigned shifted = x + phase, column = shifted / 2 % 6, row = y / 2 % 9;
        bool ink = kind == 0 ? shifted % 4 == 0 : kind == 1 ? (y + phase) % 4 == 0
            : column < 5 && row < 7 && (glyphs[shifted / 12 % 4][row] & (1u << (4 - column))) != 0;
        float value = ink ? 0.f : 1.f;
        pixels[y * Width + x] = {value, value, value, 1};
    }
    return pixels;
}

Pixels whiteCoverage(bool rounded) {
    Pixels pixels(Width * Height, Pixel{0,0,0,0});
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
        float alpha = x < Width / 2 ? 1.f : 0.f;
        if (rounded) {
            constexpr float radius = 28.f;
            float qx = std::abs(x + .5f - Width / 2.f) - (Width / 2.f - 20.f - radius);
            float qy = std::abs(y + .5f - Height / 2.f) - (Height / 2.f - 20.f - radius);
            float distance = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f))
                + std::min(std::max(qx, qy), 0.f) - radius;
            alpha = std::clamp(.5f - distance, 0.f, 1.f);
        }
        pixels[y * Width + x] = {alpha, alpha, alpha, alpha};
    }
    return pixels;
}

float centerStripeRange(const Pixels& pixels) {
    // This x-only fixture is constant along y. The x interval stays more than
    // 3*32 px from the texture edge, so clamping cannot mimic a residual.
    float low = 1, high = 0;
    for (unsigned x = 112; x < 144; ++x) {
        float value = pixels[(Height / 2) * Width + x][1];
        low = std::min(low, value); high = std::max(high, value);
    }
    return high - low;
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 5) throw std::runtime_error("Usage: glass_blur CURRENT DENSE_REFERENCE LEGACY_SPARSE REPORT_JSON");
        Gpu current(argv[1]), dense(argv[2]), sparse(argv[3]); Report report;
        constexpr float OneByte = 1.01f / 255;
        const char* fixtures[] = {"vertical_4px_lines", "horizontal_4px_lines", "neutral_glyphs"};
        for (unsigned kind = 0; kind < 3; ++kind) for (unsigned phase : {0u, 1u}) {
            auto source = blurFixture(kind, phase);
            auto prefix = std::string(fixtures[kind]) + "_phase" + std::to_string(phase);
            for (float sigma : {0.f, 1.5f, 4.f}) {
                State state; state.options[0] = sigma;
                state.tint[0] = 245 / 255.f; state.tint[1] = 248 / 255.f; state.tint[2] = 252 / 255.f;
                state.tint[3] = .74f;
                report.check(prefix + "_sigma" + std::to_string(sigma) + "_retains_low_sigma_bitmap",
                    maxDelta(current.renderBlur(source, state), sparse.renderBlur(source, state), 4), 0);
            }
            for (float sigma : {4.001f, 4.3334f, 5.f, 8.75f, 16.f, 32.f, 48.f, 64.f, 96.f, 128.f}) {
                State state; state.options[0] = sigma;
                auto actual = current.renderBlur(source, state), reference = dense.renderBlur(source, state);
                auto name = prefix + "_sigma" + std::to_string(sigma);
                report.check(name + "_matches_dense_reference_full_bitmap", maxDelta(actual, reference, 4), OneByte);
                report.check(name + "_final_alpha_is_one", alphaError(actual), 0);
                if (sigma >= 16.f) {
                    // The old shader must fail the exact same one-byte gate.
                    // A zero/constant-output substitute also fails the positive
                    // full-image comparison, including the clamped boundaries.
                    auto rejected = sparse.renderBlur(source, state);
                    report.check(name + "_legacy_sparse_fails_same_dense_gate",
                        maxDelta(rejected, reference, 4) <= OneByte ? 1 : 0, 0);
                }
                if (kind == 0 && sigma <= 32.f) {
                    report.check(name + "_fine_stripes_have_no_visible_rebound", centerStripeRange(actual), OneByte);
                }
            }
        }
        for (bool rounded : {false, true}) for (float sigma : {1.5f, 8.75f, 16.f, 32.f, 64.f, 128.f}) {
            auto source = whiteCoverage(rounded);
            State state; state.options[0] = sigma;
            state.tint[0] = state.tint[1] = state.tint[2] = 1.f;
            for (float tint : {0.f, .74f}) {
                state.tint[3] = tint;
                auto actual = current.renderBlur(source, state);
                std::string name = std::string(rounded ? "rounded" : "half_plane") + "_sigma"
                    + std::to_string(sigma) + "_tint" + std::to_string(tint);
                report.check(name + "_premultiplied_white_never_darkens",
                    maxDelta(actual, Pixels(Width * Height, Pixel{1,1,1,1}), 4), 0);
                if (sigma == 1.5f) report.check(name + "_retains_low_sigma_coverage_bitmap",
                    maxDelta(actual, sparse.renderBlur(source, state), 4), 0);
            }
        }
        return report.write(argv[4], current.level) ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
