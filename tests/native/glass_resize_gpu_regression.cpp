// Real production BlurPS, mip generation, and BGRA8 copies; no HWND or capture.
#define wmain isolated_glass_regression_entrypoint
#include "glass_gpu_regression.cpp"
#undef wmain
#include "../../src-tauri/native/glass_renderer.h"

namespace {
constexpr unsigned CapturedWidth = Width - 64;
struct ResizeGpu : Gpu {
    using Gpu::Gpu;
    Texture captured(Pixel color, bool transparentCorners = false) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = CapturedWidth; desc.Height = Height;
        desc.MipLevels = 0; desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        Texture result;
        require(device->CreateTexture2D(&desc, nullptr, &result.texture), "captured mip chain");
        require(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.source), "captured SRV");
        auto byte = [](float value) { return uint8_t(std::lround(value * 255)); };
        std::vector<std::array<uint8_t, 4>> pixels(CapturedWidth * Height,
            {byte(color[2]), byte(color[1]), byte(color[0]), byte(color[3])});
        if (transparentCorners)
            for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < CapturedWidth; ++x)
                if ((x < 12 || x >= CapturedWidth - 12) && (y < 12 || y >= Height - 12))
                    pixels[y * CapturedWidth + x] = {0, 0, 0, 0};
        context->UpdateSubresource(result.texture.Get(), 0, nullptr, pixels.data(), CapturedWidth * 4, 0);
        context->GenerateMips(result.source.Get());
        return result;
    }
    struct Frame { Pixels raw, output; };
    Frame resize(Texture& capturedTexture, State material, unsigned negative = 0, Pixel premultiplied = {}) {
        auto raw = texture(), horizontal = texture(), output = texture();
        D3D11_TEXTURE2D_DESC desc{};
        capturedTexture.texture->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC averageDesc{};
        averageDesc.Format = desc.Format;
        averageDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        averageDesc.Texture2D.MostDetailedMip = desc.MipLevels - 1;
        averageDesc.Texture2D.MipLevels = 1;
        Texture averageSource;
        require(device->CreateShaderResourceView(capturedTexture.texture.Get(), &averageDesc,
            &averageSource.source), "last raw mip SRV");
        State average;
        std::copy_n(material.tint, 3, average.tint);
        average.options[2] = 1;
        if (negative == 2) average.tint[3] = material.tint[3];
        draw(averageSource, raw, average, blur.Get());
        if (negative == 1) {
            const float tint[]{material.tint[0], material.tint[1], material.tint[2], 1};
            context->ClearRenderTargetView(raw.target.Get(), tint);
        } else if (negative == 3) {
            const float unnormalized[]{premultiplied[0], premultiplied[1], premultiplied[2], 1};
            context->ClearRenderTargetView(raw.target.Get(), unnormalized);
        }
        const auto copy = tiez_glass_detail::resize_intersection(
            {0, 0, int(CapturedWidth), int(Height)}, {0, 0, int(Width), int(Height)});
        const D3D11_BOX intersection{copy.source_x, copy.source_y, 0,
            copy.source_x + copy.width, copy.source_y + copy.height, 1};
        context->CopySubresourceRegion(raw.texture.Get(), 0, copy.destination_x, copy.destination_y, 0,
            capturedTexture.texture.Get(), 0, &intersection);
        material.direction[0] = 1; material.direction[1] = 0; material.options[2] = 0;
        draw(raw, horizontal, material, blur.Get());
        material.direction[0] = 0; material.direction[1] = 1; material.options[2] = 1;
        draw(horizontal, output, material, blur.Get());
        return {read(raw), read(output)};
    }
};

State material_state(bool dark, float opacity) {
    State material;
    material.options[0] = 1.5f + 14.5f * opacity / 100;
    const float light[]{245, 248, 252}, night[]{23, 31, 43};
    for (unsigned c = 0; c < 3; ++c) material.tint[c] = (dark ? night[c] : light[c]) / 255;
    material.tint[3] = .74f + .16f * opacity / 100;
    return material;
}

float band_error(const Pixels& output, Pixel expected) {
    float error = 0;
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = CapturedWidth; x < Width; ++x)
        error = std::max(error, delta(output[y * Width + x], expected, 4));
    return error;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: glass_resize_gpu_regression SHADER_PATH REPORT_JSON");
        ResizeGpu gpu(argv[1]); Report report;
        const Pixel gray{139.f / 255, 139.f / 255, 139.f / 255, 1};
        const Pixel color{50.f / 255, 130.f / 255, 210.f / 255, 1};
        const Pixel translucent{32.f / 255, 74.f / 255, 108.f / 255, 128.f / 255};
        const Pixel straight{32.f / 128, 74.f / 128, 108.f / 128, 1};
        for (unsigned kind = 0; kind < 4; ++kind) {
            const Pixel rawColor = kind == 0 ? gray : kind == 1 ? color : translucent;
            const Pixel opaqueColor = kind < 2 ? rawColor : straight;
            const bool corners = kind == 3;
            auto capture = gpu.captured(rawColor, corners);
            for (bool dark : {false, true}) for (float opacity : {0.f, 50.f, 100.f}) {
                const auto state = material_state(dark, opacity);
                const auto frame = gpu.resize(capture, state);
                const std::string name = "source" + std::to_string(kind) + (dark ? "_dark_" : "_light_") +
                    "opacity" + std::to_string(int(opacity));
                report.check(name + "_unknown_band_estimates_raw_before_tint",
                    band_error(frame.raw, opaqueColor), 3.01 / 255);
                report.check(name + "_unknown_band_receives_one_tint",
                    band_error(frame.output, composite(opaqueColor, state.tint)), 1.01 / 255);
                report.check(name + "_final_coverage_has_no_black_alpha_edge", alphaError(frame.output), 0);
                float knownError = 0;
                for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < CapturedWidth; ++x) {
                    Pixel expected = rawColor;
                    if (corners && (x < 12 || x >= CapturedWidth - 12) && (y < 12 || y >= Height - 12))
                        expected = {0, 0, 0, 0};
                    knownError = std::max(knownError, delta(frame.raw[y * Width + x], expected, 4));
                }
                report.check(name + "_known_intersection_copies_all_channels_exactly", knownError, 0);
                if (!corners) {
                    const Pixels completeSource(Width * Height, rawColor);
                    report.check(name + "_no_extra_seam_vs_complete_source",
                        maxDelta(frame.output, gpu.renderBlur(completeSource, state), 4), 1.01 / 255);
                }
            }
        }
        auto grayCapture = gpu.captured(gray);
        const auto material = material_state(false, 50);
        const auto correct = gpu.resize(grayCapture, material);
        report.check("negative_pure_tint_fill_produces_detectable_white_band",
            maxDelta(correct.output, gpu.resize(grayCapture, material, 1).output) * 255, 8, true);
        report.check("negative_double_tint_fill_is_detectable",
            maxDelta(correct.output, gpu.resize(grayCapture, material, 2).output) * 255, 8, true);
        auto alphaCapture = gpu.captured(translucent);
        const auto alphaCorrect = gpu.resize(alphaCapture, material);
        report.check("negative_unnormalized_premultiplication_produces_detectable_dark_band",
            maxDelta(alphaCorrect.output, gpu.resize(alphaCapture, material, 3, translucent).output) * 255, 8, true);
        return report.write(argv[2], gpu.level) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
