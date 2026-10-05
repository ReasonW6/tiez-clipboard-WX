// Offscreen float32 coordinates from the actual production GlassEvaluate.
#define wmain existing_glass_regression_entrypoint
#include "glass_gpu_regression.cpp"
#undef wmain

struct ProjectionGpu : Gpu {
    ComPtr<ID3D11PixelShader> probe;
    Texture output;
    ComPtr<ID3D11Texture2D> staging;
    explicit ProjectionGpu(const std::filesystem::path& shader) : Gpu(shader) {
        ComPtr<ID3DBlob> code, error;
        HRESULT result = D3DCompileFromFile(shader.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "ProjectionProbePS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS
                | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
        if (FAILED(result) && error)
            std::cerr.write(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize());
        require(result, "projection probe compilation");
        require(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &probe), "projection probe");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = Width; desc.Height = Height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        require(device->CreateTexture2D(&desc, nullptr, &output.texture), "float projection target");
        require(device->CreateRenderTargetView(output.texture.Get(), nullptr, &output.target), "float projection RTV");
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        require(device->CreateTexture2D(&desc, nullptr, &staging), "float projection staging");
    }
    Pixels coordinates(const State& state, unsigned channel) {
        State probeState = state; probeState.trackMeta[3] = float(channel);
        Texture unused;
        draw(unused, output, probeState, probe.Get());
        context->CopyResource(staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "projection readback");
        Pixels pixels; pixels.reserve(Width * Height);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            auto p = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 16);
            pixels.push_back({p[0], p[1], p[2], p[3]});
        }
        context->Unmap(staging.Get(), 0);
        return pixels;
    }
};

struct CoordinateMetrics {
    unsigned nonfinite = 0, opaque = 0, quads = 0, platform = 0;
    float minDet = 1, maxOutward = 0, maxTangential = 0, maxOvershoot = 0, maxPlatformOffset = 0, maxOffset = 0, maxSourceOutside = 0;
    float maxShoulderTangential = 0, maxShoulderOvershoot = 0, maxCoreOracleError = 0;
};

CoordinateMetrics inspect(const Pixels& pixels, const State& state, unsigned channel = 1) {
    CoordinateMetrics m;
    const auto& c = state.controls[0];
    const float r = std::clamp(c.shape[0], 0.f, .5f * std::min(c.rect[2], c.rect[3]));
    float press = c.shape[1] * c.shape[1] * (3 - 2 * c.shape[1]);
    float shoulder = std::min(r, state.direction[1] * (1 + .12f * press) * 1.5f);
    const double restHeight=std::min(double(state.direction[1]),std::min(double(r),1.5*state.direction[1])*.75);
    const double height=std::min(double(state.direction[1])*(1+.12*press),shoulder*.75*(1+.12*press));
    const double focal=std::max(6*restHeight,.3*std::max(c.rect[2],c.rect[3]));
    const double baseIor=std::clamp(double(state.direction[0]),1.0,2.5);
    const double index=baseIor+(int(channel)-1)*std::min(.16,.28*(baseIor-1));
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
        auto i = y * Width + x;
        const auto& p = pixels[i];
        for (float value : p) if (!std::isfinite(value)) ++m.nonfinite;
        if (p[2] != 1) continue;
        ++m.opaque;
        float dx = p[0] - (x + .5f), dy = p[1] - (y + .5f), length = std::hypot(dx, dy);
        m.maxOffset = std::max(m.maxOffset, length);
        const float sourceNearX = std::clamp(p[0], c.rect[0]+r, c.rect[0]+c.rect[2]-r);
        const float sourceNearY = std::clamp(p[1], c.rect[1]+r, c.rect[1]+c.rect[3]-r);
        m.maxSourceOutside = std::max(m.maxSourceOutside, std::hypot(p[0]-sourceNearX,p[1]-sourceNearY)-r);
        // Independent nearest point on the inset rectangle identifies radial
        // direction without reimplementing Snell or the surface profile.
        float nearestX = std::clamp(x + .5f, c.rect[0] + r, c.rect[0] + c.rect[2] - r);
        float nearestY = std::clamp(y + .5f, c.rect[1] + r, c.rect[1] + c.rect[3] - r);
        float nx = x + .5f - nearestX, ny = y + .5f - nearestY, radial = std::hypot(nx, ny);
        float depth = r - radial;
        const double t=shoulder>0?std::clamp(double(depth/shoulder),0.0,1.0):0;
        const double bodyWindow=t*t*(3-2*t);
        const double vx=(x+.5-c.rect[0]-.5*c.rect[2])/focal,vy=(y+.5-c.rect[1]-.5*c.rect[3])/focal;
        const double factor=index<=1?0:1/std::sqrt(index*index+(index*index-1)*(vx*vx+vy*vy))-1;
        const float bodyX=float(bodyWindow*height*vx*factor),bodyY=float(bodyWindow*height*vy*factor);
        const float shoulderX=dx-bodyX,shoulderY=dy-bodyY;
        if (radial > .0001f) {
            nx /= radial; ny /= radial;
            m.maxOutward = std::max(m.maxOutward, dx * nx + dy * ny);
            m.maxTangential = std::max(m.maxTangential, std::abs(dx * ny - dy * nx));
            m.maxOvershoot = std::max(m.maxOvershoot, length - radial);
            m.maxShoulderTangential=std::max(m.maxShoulderTangential,std::abs(shoulderX*ny-shoulderY*nx));
            m.maxShoulderOvershoot=std::max(m.maxShoulderOvershoot,std::hypot(shoulderX,shoulderY)-radial);
        }
        if (depth >= shoulder + .001f || radial <= .0001f) {
            ++m.platform;
            m.maxPlatformOffset = std::max(m.maxPlatformOffset, length);
            m.maxCoreOracleError=std::max({m.maxCoreOracleError,std::abs(shoulderX),std::abs(shoulderY)});
        }
        if (x + 1 < Width && y + 1 < Height && pixels[i + 1][2] == 1
            && pixels[i + Width][2] == 1 && pixels[i + Width + 1][2] == 1) {
            ++m.quads;
            float ax = pixels[i + 1][0] - p[0], ay = pixels[i + 1][1] - p[1];
            float bx = pixels[i + Width][0] - p[0], by = pixels[i + Width][1] - p[1];
            float cx = pixels[i + Width + 1][0] - pixels[i + Width][0];
            float cy = pixels[i + Width + 1][1] - pixels[i + Width][1];
            float ex = pixels[i + Width + 1][0] - pixels[i + 1][0];
            float ey = pixels[i + Width + 1][1] - pixels[i + 1][1];
            // Both triangles, not only one corner of each pixel quad.
            m.minDet = std::min({m.minDet, ax * by - ay * bx, cx * ey - cy * ex});
        }
    }
    return m;
}

#ifndef TIEZ_PROJECTION_HELPER_ONLY
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: glass_projection CURRENT UNBOUNDED REPORT_JSON");
        ProjectionGpu gpu(argv[1]), unbounded(argv[2]); Report report;
        Control controls[] = {{{113,69,30,22},{11,0,0,0}}, {{24,24,208,112},{16,0,0,0}},
            {{24,24,208,112},{20,0,0,0}}, {{16,16,224,128},{64,0,0,0}}};
        const char* names[] = {"capsule_r11", "box_r16", "box_r20", "narrow_shoulder_r64"};
        const char* channels[] = {"R", "G", "B"};
        // Five IORs include identity, production, dispersion's kink, the
        // numerical worst-case neighborhood, and the accepted clamp endpoint.
        const float iors[] = {1.f, 1.52f, 1.f + .16f / .28f, 2.15f, 2.5f};
        constexpr float CoordinateTolerance = .0001f;
        for (unsigned geometry = 0; geometry < 4; ++geometry) for (float press : {0.f,1.f})
        for (float phase : {0.f,.25f,.5f,.75f}) for (float ior : iors) {
            State state; state.options[1] = 1; state.controls[0] = controls[geometry];
            state.controls[0].shape[1] = press;
            state.controls[0].rect[0] += phase;
            // Distinct X/Y phases also exercise nonsymmetric corner samples.
            state.controls[0].rect[1] += phase == 0 ? 0 : 1 - phase;
            state.direction[0] = ior; state.direction[1] = 26;
            for (unsigned channel = 0; channel < 3; ++channel) {
                auto m = inspect(gpu.coordinates(state, channel), state, channel);
                std::string name = std::string(names[geometry]) + "_press" + std::to_string(int(press))
                    + "_phase" + std::to_string(phase) + "_ior" + std::to_string(ior) + "_" + channels[channel];
                report.check(name + "_all_values_finite", m.nonfinite, 0);
                report.check(name + "_nonempty_opaque_and_quads", std::min(m.opaque, m.quads), 10, true);
                report.check(name + "_pixel_triangles_preserve_orientation", m.minDet, .00001, true);
                report.check(name + "_offset_points_inward", m.maxOutward, CoordinateTolerance);
                report.check(name + "_shoulder_offset_follows_boundary_normal", m.maxShoulderTangential, CoordinateTolerance);
                report.check(name + "_shoulder_does_not_cross_corner_center", m.maxShoulderOvershoot, CoordinateTolerance);
                report.check(name + "_flat_platform_matches_slab_oracle", m.maxCoreOracleError, CoordinateTolerance);
                report.check(name + "_source_stays_inside_surface", m.maxSourceOutside, CoordinateTolerance);
                // A capsule has only a line of flat core; arbitrary pixel phases
                // need not hit it. Box fixtures must exercise actual platforms.
                if (geometry != 0) report.check(name + "_platform_has_samples", m.platform, 1, true);
                if (ior == 1) report.check(name + "_unit_ior_is_identity", m.maxOffset, 0);
                else report.check(name + "_refraction_is_nonzero", m.maxOffset, .1, true);
            }
        }
        State mutation; mutation.options[1] = 1; mutation.controls[0] = controls[2];
        mutation.controls[0].shape[1] = 1; mutation.controls[0].rect[0] += .25f; mutation.controls[0].rect[1] += .75f;
        mutation.direction[0] = 2.5f; mutation.direction[1] = 26;
        auto rejected = inspect(unbounded.coordinates(mutation, 2), mutation);
        report.check("negative_unbounded_projection_fails_same_orientation_gate", rejected.minDet, 0);
        return report.write(argv[3], unsigned(gpu.level)) ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
#endif
