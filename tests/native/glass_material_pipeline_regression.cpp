// Full material regression; real mip generation, two SRVs and production passes.
#define wmain isolated_math_regression_entrypoint
#include "glass_gpu_regression.cpp"
#undef wmain
#include "glass_prefix_gpu.h"

struct MaterialGpu : PrefixGpu {
    ComPtr<ID3D11PixelShader> pressureProbe, surfaceLodProbe;
    explicit MaterialGpu(const std::filesystem::path& shader) : PrefixGpu(shader) {
        ComPtr<ID3DBlob> code,error;
        HRESULT result=D3DCompileFromFile(shader.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "PressureProbePS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,&code,&error);
        if(FAILED(result)&&error)std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());
        require(result,"production HLSLI pressure probe");
        require(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&pressureProbe),"pressure probe shader");
        code.Reset(); error.Reset();
        result=D3DCompileFromFile(shader.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "SurfaceLodProbePS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,&code,&error);
        if(FAILED(result)&&error)std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());
        require(result,"production surface LOD probe");
        require(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&surfaceLodProbe),"surface LOD probe shader");
    }
    Pixels floatProbe(const State& state, ID3D11PixelShader* probe) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=Width;desc.Height=Height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        Texture output,unused;
        require(device->CreateTexture2D(&desc,nullptr,&output.texture),"float pressure target");
        require(device->CreateRenderTargetView(output.texture.Get(),nullptr,&output.target),"float pressure RTV");
        draw(unused,output,state,probe);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;require(device->CreateTexture2D(&desc,nullptr,&staging),"float pressure staging");
        context->CopyResource(staging.Get(),output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"float pressure readback");
        Pixels pixels;pixels.reserve(Width*Height);
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
            auto p=reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*16);
            pixels.push_back({p[0],p[1],p[2],p[3]});
        }
        context->Unmap(staging.Get(),0);return pixels;
    }
    Pixels pressureCoordinates(const State& state) { return floatProbe(state,pressureProbe.Get()); }
    Pixels surfaceLod(const State& state) { return floatProbe(state,surfaceLodProbe.Get()); }
    Texture mipChain(const Texture& source) {
        D3D11_TEXTURE2D_DESC desc{}; source.texture->GetDesc(&desc);
        desc.MipLevels = 0; desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        Texture result;
        require(device->CreateTexture2D(&desc, nullptr, &result.texture), "mip-chain texture");
        require(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.source), "mip-chain SRV");
        context->CopySubresourceRegion(result.texture.Get(), 0, 0, 0, 0, source.texture.Get(), 0, nullptr);
        context->GenerateMips(result.source.Get());
        return result;
    }
    Pixel mipPixel(const Texture& texture, unsigned mipLevel) {
        D3D11_TEXTURE2D_DESC desc{}; texture.texture->GetDesc(&desc);
        desc.Width = std::max(1u, desc.Width >> mipLevel); desc.Height = std::max(1u, desc.Height >> mipLevel);
        desc.MipLevels = 1; desc.BindFlags = desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(device->CreateTexture2D(&desc, nullptr, &staging), "mip readback texture");
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.texture.Get(), mipLevel, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "mip readback");
        auto p = static_cast<const uint8_t*>(mapped.pData) + (desc.Height / 2) * mapped.RowPitch + (desc.Width / 2) * 4;
        Pixel result{p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, p[3] / 255.f};
        context->Unmap(staging.Get(), 0); return result;
    }
    struct Frame { Pixels output, base; };
    // Binding 0 is the real window base. Binding 1 can deliberately be wrong
    // or lack mips to prove that the acceptance gates detect those regressions.
    Frame material(const Pixels& pixels, State state, unsigned binding = 0, bool layered = true) {
        auto scene = texture(&pixels), pyramid = mipChain(scene);
        auto horizontal = texture(), tinted = texture(), output = texture();
        state.direction[0] = 1; state.direction[1] = 0; state.options[2] = 0;
        draw(scene, horizontal, state, blur.Get());
        state.direction[0] = 0; state.direction[1] = 1; state.options[2] = 1;
        draw(horizontal, tinted, state, blur.Get());
        if (layered) {
            return {compositePrefix(binding == 1 ? tinted : scene, tinted, state, binding != 2), read(tinted)};
        }
        state.options[2] = layered ? 1.f : 0.f;
        auto second = binding == 1 ? tinted.source.Get() : binding == 2 ? scene.source.Get() : pyramid.source.Get();
        context->PSSetShaderResources(1, 1, &second);
        draw(tinted, output, state, optics.Get());
        ID3D11ShaderResourceView* none = nullptr; context->PSSetShaderResources(1, 1, &none);
        return {read(output), read(tinted)};
    }
};

float interiorDepth(unsigned x, unsigned y, const Control& control) {
    float radius = control.shape[0];
    float qx = std::abs(x + .5f - control.rect[0] - control.rect[2] / 2) - (control.rect[2] / 2 - radius);
    float qy = std::abs(y + .5f - control.rect[1] - control.rect[3] / 2) - (control.rect[3] / 2 - radius);
    return radius - std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) - std::min(std::max(qx, qy), 0.f);
}
State materialState(Control control, bool dark, float opacity) {
    State state; state.controls[0] = control; state.options[1] = 1;
    state.options[0] = 1.5f + 14.5f * opacity / 100;
    const float light[] = {245,248,252}, night[] = {23,31,43};
    for (unsigned c = 0; c < 3; ++c) state.tint[c] = (dark ? night[c] : light[c]) / 255;
    state.tint[3] = .74f + .16f * opacity / 100;
    return state;
}
Pixels detailSource(bool glyphs, unsigned phase) {
    constexpr unsigned letters[4][7] = {{14,17,16,23,17,17,14}, {16,16,16,16,16,16,31},
        {14,17,17,31,17,17,17}, {15,16,16,14,1,1,30}};
    Pixels pixels(Width * Height);
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
        unsigned shifted = x + phase, column = shifted / 2 % 6, row = y / 2 % 9;
        bool ink = glyphs ? column < 5 && row < 7 && (letters[shifted / 12 % 4][row] & (1u << (4 - column))) != 0
            : shifted % 8 < 2 || y % 12 < 2;
        float value = ink ? 0.f : 1.f; pixels[y * Width + x] = {value,value,value,1};
    }
    return pixels;
}
struct Signal { float peak = 0, areaPercent = 0, refractionPeak = 0, outsideError = 0; unsigned visible = 0, samples = 0; };
Signal signal(const MaterialGpu::Frame& frame, const MaterialGpu::Frame& zeroDispersion,
    const MaterialGpu::Frame& zeroOffsets, Control control) {
    Signal result;
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
        auto i = y * Width + x; float depth = interiorDepth(x, y, control);
        if (depth >= 1 && depth < control.shape[0] - 1) {
            Pixel residual{};
            for (unsigned c = 0; c < 3; ++c) residual[c] = frame.output[i][c] - zeroDispersion.output[i][c];
            float spread = chroma(residual) * 255;
            result.peak = std::max(result.peak, spread);
            result.visible += spread >= 15.99f; result.samples++;
            result.refractionPeak = std::max(result.refractionPeak,
                std::abs(frame.output[i][1] - zeroOffsets.output[i][1]) * 255);
        }
        if (depth < -1) result.outsideError = std::max(result.outsideError, delta(frame.output[i], frame.base[i], 4));
    }
    result.areaPercent = 100.f * result.visible / std::max(result.samples, 1u);
    return result;
}
float linearChannel(float value) { return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f); }
float luminance(const Pixel& p) { return .2126f * linearChannel(p[0]) + .7152f * linearChannel(p[1]) + .0722f * linearChannel(p[2]); }
float contrast(const Pixel& a, const Pixel& b) { float l = luminance(a), r = luminance(b); return (std::max(l,r) + .05f) / (std::min(l,r) + .05f); }
float centerContrast(const Pixels& pixels, unsigned centerX = 128) {
    float low = 1, high = 0;
    for (unsigned y = 76; y < 84; ++y) for (unsigned x = centerX - 12; x < centerX + 12; ++x) {
        low = std::min(low, pixels[y * Width + x][1]); high = std::max(high, pixels[y * Width + x][1]);
    }
    return (high - low) * 255;
}

#include "glass_surface_filter_regression.h"

void run_reading_plane_regression(MaterialGpu& gpu, Report& report) {
    constexpr float OneByte = 1.01f / 255;
    const Control plane{{8, 8, 240, 144}, {24, 0, 2, 0}};
    const Control child{{70, 56, 116, 48}, {24, 0, 0, 0}};
    const Pixels source(Width * Height, Pixel{.1f, .1f, .1f, 1});
    for (bool dark : {false, true}) for (float opacity : {0.f, 50.f, 100.f}) for (float dpi : {1.f, 1.5f, 2.f}) {
        auto state = materialState(plane, dark, opacity);
        state.options[3] = dpi;
        state.options[0] *= dpi;
        auto flat = gpu.material(source, state).output;
        auto diagnostic = gpu.material(source, state, 0, false).output;
        auto regularState = state;
        regularState.controls[0].shape[2] = 1;
        auto regular = gpu.material(source, regularState).output;
        float variation = 0, curvedVariation = 0;
        const auto center = Height / 2 * Width + Width / 2;
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            if (interiorDepth(x, y, plane) < 1.5f) continue;
            const auto index = y * Width + x;
            variation = std::max(variation, delta(flat[index], flat[center]));
            curvedVariation = std::max(curvedVariation, delta(regular[index], regular[center]));
        }
        const auto name = std::string("reading_") + (dark ? "dark_" : "light_")
            + std::to_string(int(opacity)) + "_dpi" + std::to_string(int(dpi * 100));
        report.check(name + "_uniform_source_has_no_convex_rim", variation, OneByte);
        report.check(name + "_regular_negative_retains_convex_rim", curvedVariation, OneByte, true);
        report.check(name + "_diagnostic_and_prefix_agree", maxDelta(flat, diagnostic, 4), OneByte);
        report.check(name + "_alpha_is_opaque", alphaError(flat), 0);
        state.controls[0].shape[1] = 1;
        report.check(name + "_noninteractive_plane_ignores_pressure", maxDelta(flat, gpu.material(source, state).output, 4), 0);
        state.controls[0].shape[1] = 0;
        state.controls[1] = child;
        state.options[1] = 2;
        const auto layered = gpu.material(source, state).output;
        float outsideChild = 0, childSignal = 0;
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            const auto index = y * Width + x;
            const float difference = delta(layered[index], flat[index]);
            if (interiorDepth(x, y, child) < -1) outsideChild = std::max(outsideChild, difference);
            else if (interiorDepth(x, y, child) > 1.5f) childSignal = std::max(childSignal, difference);
        }
        report.check(name + "_child_leaves_surrounding_reading_plane_unchanged", outsideChild, 0);
        report.check(name + "_child_retains_its_own_optical_shape", childSignal, OneByte, true);
    }
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 9) throw std::runtime_error("Usage: full_material CURRENT ZERO_DISPERSION ZERO_OFFSETS REJECTED_RIM_TINT FIXED_PRESSURE_CAP DEPTH_FILTER CONSTANT_LOD REPORT_JSON");
        MaterialGpu gpu(argv[1]), zeroDispersion(argv[2]), zeroOffsets(argv[3]), rejectedRimTint(argv[4]), fixedPressureCap(argv[5]), depthFilter(argv[6]), constantLod(argv[7]); Report report;
        constexpr float OneByte = 1.01f / 255;
        Pixels checker(Width * Height);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            float v = (x + y) % 2 ? 1.f : 0.f; checker[y * Width + x] = {v,v,v,1};
        }
        auto top = gpu.texture(&checker), mip = gpu.mipChain(top);
        D3D11_TEXTURE2D_DESC mipDesc{}; mip.texture->GetDesc(&mipDesc);
        report.check("raw_backdrop_has_complete_mip_chain", mipDesc.MipLevels, 9, true);
        for (unsigned level : {1u, 3u, 7u}) report.check("generated_checker_mip_" + std::to_string(level) + "_is_averaged",
            delta(gpu.mipPixel(mip, level), Pixel{.5f,.5f,.5f,1}, 4), OneByte);
        const char* names[] = {"search", "card", "thumb"};
        Control controls[] = {{{24,60,208,40},{20,0,0,0}}, {{24,24,208,112},{16,0,0,0}}, {{113,69,30,22},{11,0,0,0}}};
        std::ofstream measurements(std::filesystem::path(argv[8]).replace_extension("measurements.json"));
        measurements << std::setprecision(9) << "[\n"; unsigned records = 0;
        for (unsigned geometry = 0; geometry < 3; ++geometry) for (bool glyphs : {false,true})
        for (bool dark : {false,true}) for (float opacity : {0.f,50.f}) for (unsigned phase : {0u,1u,2u,3u}) {
            auto state = materialState(controls[geometry], dark, opacity); auto source = detailSource(glyphs, phase);
            auto actual = gpu.material(source, state), zDisp = zeroDispersion.material(source, state), zOffsets = zeroOffsets.material(source, state);
            auto current = signal(actual, zDisp, zOffsets, state.controls[0]);
            auto prior = signal(gpu.material(source,state,0,false), zeroDispersion.material(source,state,0,false), zeroOffsets.material(source,state,0,false),state.controls[0]);
            auto misbound = signal(gpu.material(source,state,1), zeroDispersion.material(source,state,1), zeroOffsets.material(source,state,1),state.controls[0]);
            std::string name = std::string(names[geometry]) + (glyphs ? "_glyphs_" : "_lines_") + (dark ? "dark_" : "light_")
                + "opacity" + std::to_string(int(opacity)) + "_phase" + std::to_string(phase);
            report.check(name + "_full_material_spectral_peak", current.peak, 15.99, true);
            report.check(name + "_full_material_visible_shoulder_percent", current.areaPercent, 3, true);
            // This mutation changes the input pipeline, not the optical model.
            // Stronger optics can make even a preblurred source cross 3%.
            // Require the independent raw source to add another full visible
            // area threshold, while retaining the absolute quality gates above.
            report.check(name + "_raw_source_adds_visible_shoulder_area", current.areaPercent - prior.areaPercent, 3, true);
            report.check(name + "_wrong_control_source_fails_same_area_gate", misbound.areaPercent < 3 ? 0 : 1, 0);
            report.check(name + "_window_base_unchanged_outside_controls", current.outsideError, 0);
            report.check(name + "_final_alpha_is_one", alphaError(actual.output), 0);
            if (records++) measurements << ",\n";
            measurements << "{\"case\":\"" << name << "\",\"currentPeak\":" << current.peak << ",\"currentAreaPercent\":" << current.areaPercent
                << ",\"priorPeak\":" << prior.peak << ",\"priorAreaPercent\":" << prior.areaPercent
                << ",\"misboundPeak\":" << misbound.peak << ",\"misboundAreaPercent\":" << misbound.areaPercent
                << ",\"currentRefractionPeak\":" << current.refractionPeak << ",\"priorRefractionPeak\":" << prior.refractionPeak << '}';
        }
        measurements << "\n]\n";
        for (bool dark : {false,true}) for (float opacity : {0.f,50.f,100.f}) for (unsigned geometry = 0; geometry < 3; ++geometry) for(bool regular:{false,true}) {
            auto state = materialState(controls[geometry],dark,opacity);
            state.controls[0].shape[2]=regular?1.f:0.f;
            std::string name = std::string(names[geometry]) + (dark ? "_dark_" : "_light_") + std::to_string(int(opacity))+(regular?"_regular":"_clear");
            float minimumContrast = 100, uniformDifference = 0, uniformBaseDifference = 0, rejectedRimDifference = 0, alpha = 0;
            // Use the production primary and secondary foreground colors. They
            // are measured against the material; they never enter its textures.
            Pixel primary = dark ? Pixel{246/255.f,248/255.f,252/255.f,1} : Pixel{23/255.f,33/255.f,46/255.f,1};
            Pixel secondary = dark ? Pixel{225/255.f,231/255.f,240/255.f,1} : Pixel{29/255.f,43/255.f,59/255.f,1};
            const Pixel detailInk[] = {
                dark ? Pixel{225/255.f,231/255.f,240/255.f,1} : Pixel{45/255.f,61/255.f,80/255.f,1},
                dark ? Pixel{224/255.f,234/255.f,255/255.f,1} : Pixel{23/255.f,63/255.f,122/255.f,1},
                dark ? Pixel{255/255.f,221/255.f,229/255.f,1} : Pixel{131/255.f,31/255.f,43/255.f,1}
            };
            float detailContrast[] = {100, 100, 100};
            for (float value : {0.f,.5f,1.f}) {
                Pixels uniform(Width*Height,Pixel{value,value,value,1});
                auto actual = gpu.material(uniform,state).output, neutral = zeroDispersion.material(uniform,state).output;
                auto prior = gpu.material(uniform,state,0,false).output;
                uniformBaseDifference = std::max(uniformBaseDifference,maxDelta(actual,prior));
                rejectedRimDifference = std::max(rejectedRimDifference,maxDelta(rejectedRimTint.material(uniform,state).output,prior));
                uniformDifference = std::max(uniformDifference,maxDelta(actual,neutral)); alpha = std::max(alpha,alphaError(actual));
                const float safeInset = std::min(12.f,controls[geometry].shape[0]*.75f);
                for (unsigned y=0;y<Height;++y) for (unsigned x=0;x<Width;++x) if(interiorDepth(x,y,controls[geometry])>=safeInset) {
                    minimumContrast = std::min({minimumContrast,contrast(primary,actual[y*Width+x]),contrast(secondary,actual[y*Width+x])});
                    for (unsigned role = 0; role < 3; ++role)
                        detailContrast[role] = std::min(detailContrast[role], contrast(detailInk[role], actual[y*Width+x]));
                }
            }
            report.check(name+"_uniform_sources_never_create_spectral_color",uniformDifference,OneByte);
            report.check(name+"_uniform_source_preserves_existing_tint_without_bevel",uniformBaseDifference,2*OneByte);
            report.check(name+"_rejected_rim_tint_fails_uniform_source_gate",rejectedRimDifference,16.f/255,true);
            report.check(name+"_label_safe_inset_contrast",minimumContrast,4.5,true);
            for (unsigned role = 0; role < 3; ++role) {
                const char* roles[] = {"muted", "link_and_quick_paste", "danger"};
                report.check(name+"_"+roles[role]+"_safe_inset_contrast",detailContrast[role],4.5,true);
            }
            report.check(name+"_uniform_output_alpha_is_one",alpha,0);
        }
        for (bool checkerCoverage : {false,true}) for(bool regular:{false,true}) {
            Pixels partial(Width*Height,Pixel{0,0,0,0});
            for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x)if(checkerCoverage ? (x+y)%2 : x<Width/2)partial[y*Width+x]={1,1,1,1};
            auto state=materialState(controls[1],false,0);state.tint[0]=state.tint[1]=state.tint[2]=1;
            state.controls[0].shape[2]=regular?1.f:0.f;
            auto partialFrame=gpu.material(partial,state).output, opaque=gpu.material(Pixels(Width*Height,Pixel{1,1,1,1}),state).output;
            report.check(std::string(checkerCoverage?"premultiplied_mips_checker_has_no_dark_halo":"premultiplied_mips_half_plane_has_no_dark_halo")+(regular?"_regular":"_clear"),maxDelta(partialFrame,opaque,4),2*OneByte);
        }
        // Track composition must remain below the three displaced samples in
        // the layered path, not merely in the isolated optical math tests.
        {
            Pixels flat(Width*Height,Pixel{90/255.f,110/255.f,130/255.f,1});
            auto trackState=materialState(controls[2],false,0);
            trackState.trackMeta[0]=1;
            auto withoutTrack=trackState;withoutTrack.trackMeta[0]=0;
            auto actual=gpu.material(flat,trackState).output, baseline=gpu.material(flat,withoutTrack).output;
            auto straightState=trackState;straightState.options[1]=0;straightState.options[2]=0;
            auto postOptics=gpu.render(baseline,straightState);
            unsigned displaced=0, incorrectlyOrdered=0;
            for(unsigned y=70;y<90;++y)for(unsigned x=114;x<142;++x){
                if(y>=76&&y<84)continue;
                auto i=y*Width+x;
                displaced+=delta(actual[i],baseline[i])>.02f;
                incorrectlyOrdered+=delta(postOptics[i],baseline[i])>.02f;
            }
            report.check("layered_thumb_displaces_its_underlying_track",displaced,6,true);
            report.check("post_optics_track_negative_has_no_displacement",incorrectlyOrdered,0);
            report.check("layered_track_output_alpha_is_one",alphaError(actual),0);
            auto transparentTrack=trackState;transparentTrack.tracks[0].active[3]=transparentTrack.tracks[0].inactive[3]=0;
            report.check("layered_transparent_track_preserves_material",maxDelta(baseline,gpu.material(flat,transparentTrack).output,4),0);
            auto duplicate=trackState;duplicate.controls[1]=duplicate.controls[0];duplicate.options[1]=2;
            const auto stacked=gpu.material(flat,duplicate).output;
            float outside=0;
            for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x)
                if(interiorDepth(x,y,duplicate.controls[1]) < -1)
                    outside=std::max(outside,delta(actual[y*Width+x],stacked[y*Width+x],4));
            report.check("layered_coincident_surface_does_not_repaint_track_outside_it",outside,0);
            report.check("layered_coincident_surface_samples_previous_optics",maxDelta(actual,stacked),.02,true);
        }
        // The three settings sliders must all reach the layered source path in
        // the same draw, with independent progress, color and ownership.
        for(bool dark:{false,true})for(float opacity:{0.f,50.f}){
            const Pixels flat(Width*Height,Pixel{90/255.f,110/255.f,130/255.f,1});
            auto multi=threeTrackState();const auto parameters=materialState(multi.controls[0],dark,opacity);
            multi.options[0]=parameters.options[0];
            std::copy(std::begin(parameters.tint),std::end(parameters.tint),std::begin(multi.tint));
            auto noTracks=multi;noTracks.trackMeta[0]=0;
            const auto actual=gpu.material(flat,multi).output,baseline=gpu.material(flat,noTracks).output;
            auto postState=multi;postState.options[1]=0;postState.options[2]=0;
            const auto postOptics=gpu.render(baseline,postState);
            const auto prefix=std::string("layered_three_")+(dark?"dark_":"light_")+std::to_string(int(opacity));
            report.check(prefix+"_output_alpha_is_one",alphaError(actual),0);
            for(unsigned index=0;index<3;++index){
                const auto& track=multi.tracks[index];const auto& control=multi.controls[index];
                unsigned displaced=0,incorrectDisplacement=0;float peak=0;
                for(unsigned y=unsigned(control.rect[1])+1;y<unsigned(control.rect[1]+control.rect[3])-1;++y)
                for(unsigned x=unsigned(control.rect[0])+1;x<unsigned(control.rect[0]+control.rect[2])-1;++x){
                    if(y>=unsigned(track.rect[1])&&y<unsigned(track.rect[1]+track.rect[3]))continue;
                    const auto i=y*Width+x;const auto difference=delta(actual[i],baseline[i]);
                    displaced+=difference>.02f;peak=std::max(peak,difference);
                    incorrectDisplacement+=delta(postOptics[i],baseline[i])>.02f;
                }
                const auto name=prefix+"_slot_"+std::to_string(index);
                report.check(name+"_moves_its_track_outside_bar",displaced,6,true);
                report.check(name+"_has_nontrivial_track_refraction",peak,.04,true);
                report.check(name+"_post_optics_negative_has_no_displacement",incorrectDisplacement,0);
                auto muted=multi;muted.tracks[index].active[3]=muted.tracks[index].inactive[3]=0;
                const auto withoutOne=gpu.material(flat,muted).output;
                float otherError=0,ownSignal=0;
                for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
                    const auto i=y*Width+x;
                    if(y+.5f>=control.rect[1]&&y+.5f<control.rect[1]+control.rect[3])
                        ownSignal=std::max(ownSignal,delta(actual[i],withoutOne[i]));
                    else otherError=std::max(otherError,delta(actual[i],withoutOne[i],4));
                }
                report.check(name+"_has_independent_content",ownSignal,.04,true);
                report.check(name+"_muting_keeps_other_tracks_exact",otherError,0);
            }
            auto transparent=multi;
            for(auto& track:transparent.tracks)track.active[3]=track.inactive[3]=0;
            report.check(prefix+"_transparent_tracks_preserve_material_exactly",maxDelta(baseline,gpu.material(flat,transparent).output,4),0);
            auto duplicate=multi;duplicate.controls[3]=duplicate.controls[0];duplicate.options[1]=4;
            const auto stacked=gpu.material(flat,duplicate).output;
            float outside=0;
            for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x)
                if(interiorDepth(x,y,duplicate.controls[3]) < -1)
                    outside=std::max(outside,delta(actual[y*Width+x],stacked[y*Width+x],4));
            report.check(prefix+"_coincident_surface_keeps_uncovered_tracks_exact",outside,0);
            report.check(prefix+"_coincident_surface_refracts_existing_track",maxDelta(actual,stacked),.02,true);
        }
        // A real second surface adds reflection, never a second global tint.
        // The zero-normal plateau has a closed-form Fresnel reference.
        for(bool dark:{false,true})for(float opacity:{0.f,50.f,100.f}){
            Control panel{{24,24,208,112},{24,0,0,0}};
            auto stacked=materialState(panel,dark,opacity);stacked.options[1]=2;stacked.controls[1]=panel;
            const Pixel raw{90/255.f,110/255.f,130/255.f,1};
            const auto image=gpu.material(Pixels(Width*Height,raw),stacked).output;
            const float f0=std::pow(.52f/2.52f,2.f);Pixel expected{0,0,0,1};
            for(unsigned c=0;c<3;++c){
                const float initial=raw[c]*(1-stacked.tint[3])+stacked.tint[c]*stacked.tint[3];
                expected[c]=1-(1-initial)*(1-f0)*(1-f0);
            }
            const auto name=std::string("prefix_stack_")+(dark?"dark_":"light_")+std::to_string(int(opacity));
            report.check(name+"_two_interfaces_one_global_tint",delta(image[80*Width+128],expected),1.01f/255);
            auto reading=materialState(Control{{8,8,240,144},{26,0,1,0}},dark,opacity);
            reading.options[1]=2;reading.controls[1]={{32,40,192,80},{20,0,0,0}};
            const Pixel inks[]={
                dark?Pixel{225/255.f,231/255.f,240/255.f,1}:Pixel{45/255.f,61/255.f,80/255.f,1},
                dark?Pixel{224/255.f,234/255.f,255/255.f,1}:Pixel{23/255.f,63/255.f,122/255.f,1},
                dark?Pixel{1,221/255.f,229/255.f,1}:Pixel{131/255.f,31/255.f,43/255.f,1}};
            float minimum=100;
            for(float value:{0.f,.5f,1.f}){
                const auto output=gpu.material(Pixels(Width*Height,Pixel{value,value,value,1}),reading).output;
                for(unsigned y=60;y<100;++y)for(unsigned x=52;x<204;++x)
                    for(const auto& ink:inks)minimum=std::min(minimum,contrast(ink,output[y*Width+x]));
            }
            report.check(name+"_reading_plane_and_front_control_text_contrast",minimum,4.5,true);
        }
        for(unsigned geometry=0;geometry<3;++geometry){
            auto rest=materialState(controls[geometry],false,0), pressed=rest;pressed.controls[0].shape[1]=1;
            auto before=gpu.pressureCoordinates(rest), after=gpu.pressureCoordinates(pressed), restored=gpu.pressureCoordinates(rest);
            auto oldBefore=fixedPressureCap.pressureCoordinates(rest), oldAfter=fixedPressureCap.pressureCoordinates(pressed);
            float displacement=0,oldDisplacement=0,coverageDifference=0,minJacobian=1;
            for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
                auto i=y*Width+x;
                displacement=std::max(displacement,std::hypot(after[i][0]-before[i][0],after[i][1]-before[i][1]));
                oldDisplacement=std::max(oldDisplacement,std::hypot(oldAfter[i][0]-oldBefore[i][0],oldAfter[i][1]-oldBefore[i][1]));
                coverageDifference=std::max(coverageDifference,std::abs(after[i][2]-before[i][2]));
                if(x+1<Width&&y+1<Height&&after[i][2]==1&&after[i+1][2]==1&&after[i+Width][2]==1&&after[i+Width+1][2]==1){
                    float jxx=after[i+1][0]-after[i][0],jyx=after[i+1][1]-after[i][1];
                    float jxy=after[i+Width][0]-after[i][0],jyy=after[i+Width][1]-after[i][1];
                    minJacobian=std::min(minJacobian,jxx*jyy-jxy*jyx);
                }
            }
            std::string prefix=std::string(names[geometry])+"_pressure_";
            report.check(prefix+"changes_actual_gpu_green_sample_position",displacement,.025f*controls[geometry].shape[0],true);
            report.check(prefix+"old_fixed_cap_has_no_gpu_displacement",oldDisplacement,0);
            report.check(prefix+"unpressed_geometry_matches_previous_cap",maxDelta(before,oldBefore,4),0);
            report.check(prefix+"coverage_mask_is_unchanged",coverageDifference,0);
            report.check(prefix+"release_restores_gpu_coordinate_bitmap",maxDelta(before,restored,4),0);
            report.check(prefix+"gpu_pixel_quads_do_not_fold",minJacobian,.15,true);
            for(bool dark:{false,true})for(float opacity:{0.f,50.f})for(bool glyphs:{false,true}){
                auto source=detailSource(glyphs,0);
                auto state=materialState(controls[geometry],dark,opacity), down=state;
                down.controls[0].shape[1]=1;
                auto upFrame=gpu.material(source,state).output, downFrame=gpu.material(source,down).output;
                auto released=gpu.material(source,state).output;
                auto oldUp=fixedPressureCap.material(source,state).output,oldDown=fixedPressureCap.material(source,down).output;
                float greenPeak=0,oldGreenPeak=0;unsigned changedPixels=0;
                const auto& control=controls[geometry];
                // In the lower/right shoulder the environment-light dot product
                // is nonpositive at both pressure endpoints. Exclude the 1 px
                // reflection band too: remaining G changes are source sampling.
                for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
                    float depth=interiorDepth(x,y,control);
                    if(x+.5f<control.rect[0]+control.rect[2]/2||y+.5f<control.rect[1]+control.rect[3]/2||depth<1.5f||depth>=control.shape[0]-1)continue;
                    auto i=y*Width+x;
                    float difference=std::abs(downFrame[i][1]-upFrame[i][1])*255;
                    greenPeak=std::max(greenPeak,difference);changedPixels+=difference>=3.99f;
                    oldGreenPeak=std::max(oldGreenPeak,std::abs(oldDown[i][1]-oldUp[i][1])*255);
                }
                const auto name=prefix+(dark?"dark_":"light_")+std::to_string(int(opacity))+(glyphs?"_glyphs":"_lines");
                report.check(name+"_full_pipeline_green_refraction_changes",greenPeak,3.99,true);
                report.check(name+"_green_change_covers_multiple_pixels",changedPixels,3,true);
                report.check(name+"_fixed_cap_fails_same_green_change_gate",oldGreenPeak,0);
                report.check(name+"_release_restores_material_bitmap",maxDelta(upFrame,released,4),0);
                report.check(name+"_static_material_matches_old_cap",maxDelta(upFrame,oldUp,4),0);
                report.check(name+"_pressed_output_alpha_is_one",alphaError(downFrame),0);
            }
        }
        run_surface_filter_regression(gpu,constantLod,depthFilter,report);
        run_reading_plane_regression(gpu,report);
        return report.write(argv[8],gpu.level)?0:1;
    } catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 2;}
}
