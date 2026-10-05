// Offscreen production-shader regression. Creates no HWND and reads no desktop.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
#ifndef TIEZ_GPU_WIDTH
#define TIEZ_GPU_WIDTH 256
#define TIEZ_GPU_HEIGHT 160
#endif
constexpr unsigned Width = TIEZ_GPU_WIDTH, Height = TIEZ_GPU_HEIGHT;
using Pixel = std::array<float, 4>;
using Pixels = std::vector<Pixel>;
struct Control { float rect[4]; float shape[4]; };
struct Track {
    float rect[4]{24, 76, 208, 8};
    float shape[4]{4, .5f, 0, 0};
    float active[4]{.1f, .4f, .9f, .6f};
    float inactive[4]{.8f, .2f, .1f, .35f};
};
struct State {
    float size[2]{float(Width), float(Height)}, direction[2]{};
    float tint[4]{}, options[4]{0, 0, 0, 1};
    Control controls[64]{};
    float trackMeta[4]{};
    Track tracks[64]{};
};
static_assert(sizeof(Track) == 64 && offsetof(Track, shape) == 16
    && offsetof(Track, active) == 32 && offsetof(Track, inactive) == 48);
static_assert(sizeof(State) == 6208 && offsetof(State, trackMeta) == 2096
    && offsetof(State, tracks) == 2112);

void require(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(result));
}
struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
};
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> optics, blur;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    D3D_FEATURE_LEVEL level{};
    unsigned reflectedBufferBytes{}, reflectedTrackMetaOffset{}, reflectedTrackOffset{}, reflectedTrackCount{}, reflectedTrackStride{};
    explicit Gpu(const std::filesystem::path& shader, D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_WARP) {
        require(D3D11CreateDevice(nullptr, driver, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, &level, &context), "D3D11CreateDevice");
        auto compile = [&](const char* entry, const char* profile) {
            ComPtr<ID3DBlob> code, error;
            HRESULT result = D3DCompileFromFile(shader.c_str(), nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS |
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
            if (FAILED(result) && error)
                std::cerr.write(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize());
            require(result, entry);
            return code;
        };
        auto vs = compile("VSMain", "vs_5_0");
        auto ps = compile("OpticsPS", "ps_5_0");
        auto bs = compile("BlurPS", "ps_5_0");
        ComPtr<ID3D11ShaderReflection> reflection;
        require(D3DReflect(ps->GetBufferPointer(), ps->GetBufferSize(),
            __uuidof(ID3D11ShaderReflection), &reflection), "shader reflection");
        auto reflectedBuffer = reflection->GetConstantBufferByName("Parameters");
        D3D11_SHADER_BUFFER_DESC reflectedDesc{};
        require(reflectedBuffer->GetDesc(&reflectedDesc), "Parameters reflection");
        reflectedBufferBytes = reflectedDesc.Size;
        D3D11_SHADER_VARIABLE_DESC reflectedMeta{}, reflectedTrack{};
        require(reflectedBuffer->GetVariableByName("trackMeta")->GetDesc(&reflectedMeta), "trackMeta reflection");
        auto reflectedTracks = reflectedBuffer->GetVariableByName("tracks");
        require(reflectedTracks->GetDesc(&reflectedTrack), "tracks reflection");
        D3D11_SHADER_TYPE_DESC reflectedTrackType{};
        require(reflectedTracks->GetType()->GetDesc(&reflectedTrackType), "track array reflection");
        reflectedTrackMetaOffset = reflectedMeta.StartOffset;
        reflectedTrackOffset = reflectedTrack.StartOffset;
        reflectedTrackCount = reflectedTrackType.Elements;
        reflectedTrackStride = reflectedTrackCount ? reflectedTrack.Size / reflectedTrackCount : 0;
        if (reflectedBufferBytes != sizeof(State) || reflectedTrackMetaOffset != offsetof(State, trackMeta)
            || reflectedTrackOffset != offsetof(State, tracks) || reflectedTrackCount != 64 || reflectedTrackStride != sizeof(Track))
            throw std::runtime_error("Compiled shader and test constant-buffer layouts differ");
        require(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex), "vertex");
        require(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &optics), "optics");
        require(device->CreatePixelShader(bs->GetBufferPointer(), bs->GetBufferSize(), nullptr, &blur), "blur");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(State); buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        require(device->CreateBuffer(&buffer, nullptr, &constants), "constants");
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        require(device->CreateSamplerState(&sample, &sampler), "sampler");
    }
    Texture texture(const Pixels* pixels = nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = Width; desc.Height = Height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        // Captured scenes and final output use BGRA8; blur intermediates and
        // optical prefixes use floating-point auxiliary textures.
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        std::vector<std::array<uint8_t, 4>> bytes;
        if (pixels) {
            bytes.reserve(pixels->size());
            auto byte = [](float value) { return uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 255)); };
            for (auto& p : *pixels) bytes.push_back({byte(p[2]), byte(p[1]), byte(p[0]), byte(p[3])});
        }
        D3D11_SUBRESOURCE_DATA data{bytes.data(), Width * 4, 0};
        Texture result;
        require(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &result.texture), "texture");
        require(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.source), "source");
        require(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.target), "target");
        return result;
    }
    Texture auxiliary(DXGI_FORMAT format, bool mip = false) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = Width; d.Height = Height;
        d.MipLevels = mip ? 0 : 1; d.ArraySize = d.SampleDesc.Count = 1; d.Format = format;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = mip ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        Texture result;
        require(device->CreateTexture2D(&d, nullptr, &result.texture), "auxiliary texture");
        require(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.source), "auxiliary SRV");
        require(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.target), "auxiliary RTV");
        return result;
    }
    void draw(const Texture& source, Texture& output, const State& state, ID3D11PixelShader* ps) {
        context->UpdateSubresource(constants.Get(), 0, nullptr, &state, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, float(Width), float(Height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        auto cb = constants.Get(); auto ss = sampler.Get();
        auto srv = source.source.Get(); auto rtv = output.target.Get();
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetSamplers(0, 1, &ss);
        context->PSSetShaderResources(0, 1, &srv);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    Pixels read(const Texture& output) {
        D3D11_TEXTURE2D_DESC desc{}; output.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(device->CreateTexture2D(&desc, nullptr, &staging), "readback texture");
        context->CopyResource(staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback map");
        Pixels result; result.reserve(Width * Height);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            auto p = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            result.push_back({p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, p[3] / 255.f});
        }
        context->Unmap(staging.Get(), 0); return result;
    }
    Pixels render(const Pixels& pixels, const State& state) {
        auto input = texture(&pixels), output = texture();
        draw(input, output, state, optics.Get()); return read(output);
    }
    Pixels renderBlur(const Pixels& pixels, State state) {
        ID3D11Buffer* noKernel = nullptr;
        context->PSSetConstantBuffers(1, 1, &noKernel);
        auto input = texture(&pixels), horizontal = auxiliary(DXGI_FORMAT_R16G16B16A16_FLOAT), output = texture();
        state.direction[0] = 1; state.direction[1] = 0; state.options[2] = 0;
        draw(input, horizontal, state, blur.Get());
        state.direction[0] = 0; state.direction[1] = 1; state.options[2] = 1;
        draw(horizontal, output, state, blur.Get()); return read(output);
    }
    Pixels sampleOracle(const Pixels& pixels,const Pixels& rg,const Pixels& blue) {
        // CPU-derived coordinates, sampled by the same D3D sampler. This keeps
        // subtexel interpolation precision out of the geometry/color oracle.
        const char* source=R"(
Texture2D<float4> SourceTex:register(t0);
Texture2D<float4> CoordinatesRG:register(t1);
Texture2D<float4> CoordinatesB:register(t2);
SamplerState LinearSampler:register(s0);
float4 OraclePS(float4 pixel:SV_Position):SV_Target {
    int3 at=int3(pixel.xy,0);
    float4 rg=CoordinatesRG.Load(at);
    float2 b=CoordinatesB.Load(at).xy;
    float3 color=float3(SourceTex.SampleLevel(LinearSampler,rg.xy,0).r,
        SourceTex.SampleLevel(LinearSampler,rg.zw,0).g,SourceTex.SampleLevel(LinearSampler,b,0).b);
    return float4(color+(1.0-color)*0.042579995,1.0);
})";
        ComPtr<ID3DBlob> code,error;ComPtr<ID3D11PixelShader> shader;
        const auto hr=D3DCompile(source,std::char_traits<char>::length(source),nullptr,nullptr,nullptr,
            "OraclePS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&code,&error);
        if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());
        require(hr,"independent sampling shader");
        require(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader),"sampling oracle");
        auto coordinateTexture=[&](const Pixels& values) {
            D3D11_TEXTURE2D_DESC d{};d.Width=Width;d.Height=Height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_IMMUTABLE;
            D3D11_SUBRESOURCE_DATA data{values.data(),Width*16,0};Texture result;
            require(device->CreateTexture2D(&d,&data,&result.texture),"oracle coordinates");
            require(device->CreateShaderResourceView(result.texture.Get(),nullptr,&result.source),"oracle coordinates SRV");
            return result;
        };
        auto input=texture(&pixels),output=texture(),r=coordinateTexture(rg),b=coordinateTexture(blue);
        ID3D11ShaderResourceView* views[]{r.source.Get(),b.source.Get()};context->PSSetShaderResources(1,2,views);
        State state;draw(input,output,state,shader.Get());
        ID3D11ShaderResourceView* none[2]{};context->PSSetShaderResources(1,2,none);
        return read(output);
    }
    double timedDrawAndReadback(const Pixels& pixels, const State& state, unsigned frames) {
        auto input = texture(&pixels), output = texture();
        for (unsigned warmup = 0; warmup < 2; ++warmup) {
            draw(input, output, state, optics.Get()); read(output);
        }
        const auto start = std::chrono::steady_clock::now();
        for (unsigned frame = 0; frame < frames; ++frame) {
            draw(input, output, state, optics.Get()); read(output);
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
    }
};

float delta(const Pixel& a, const Pixel& b, unsigned channels = 3) {
    float value = 0; for (unsigned c = 0; c < channels; ++c) value = std::max(value, std::abs(a[c] - b[c]));
    return value;
}
float maxDelta(const Pixels& a, const Pixels& b, unsigned channels = 3) {
    float value = 0; for (size_t i = 0; i < a.size(); ++i) value = std::max(value, delta(a[i], b[i], channels));
    return value;
}
float alphaError(const Pixels& pixels) {
    float value = 0; for (auto& p : pixels) value = std::max(value, std::abs(1 - p[3])); return value;
}
float chroma(const Pixel& pixel) {
    return std::max({pixel[0], pixel[1], pixel[2]}) - std::min({pixel[0], pixel[1], pixel[2]});
}

// Independent capsule oracle: distance to a finite horizontal center segment.
// This avoids copying the shader's rounded-box SDF or any refraction equations.
float capsuleDistance(float x, float y, const Track& track) {
    float radius = track.rect[3] * .5f;
    float nearestX = std::clamp(x, track.rect[0] + radius,
        track.rect[0] + track.rect[2] - radius);
    return std::hypot(x - nearestX, y - track.rect[1] - radius) - radius;
}
Pixel composite(const Pixel& background, const float* rgba) {
    Pixel result{};
    for (unsigned c = 0; c < 3; ++c) result[c] = background[c] * (1 - rgba[3]) + rgba[c] * rgba[3];
    result[3] = 1; return result;
}
// Closed-form plane-slab oracle for the guaranteed flat interior fixtures.
// Uses neither the shader's refract/normalize nor its boundary-normal field.
std::array<float,2> bodySample(float x,float y,const Control& control,unsigned channel) {
    const double radius=control.shape[0],height=std::min(26.0,.75*std::min(radius,39.0));
    const double focal=std::max(6*height,.3*std::max(control.rect[2],control.rect[3]));
    const double n=1.52+(int(channel)-1)*(.28*.52);
    const double dx=(x-control.rect[0]-.5*control.rect[2])/focal,dy=(y-control.rect[1]-.5*control.rect[3])/focal;
    const double nearX=std::clamp(double(x),double(control.rect[0])+radius,control.rect[0]+control.rect[2]-radius);
    const double nearY=std::clamp(double(y),double(control.rect[1])+radius,control.rect[1]+control.rect[3]-radius);
    const double t=std::clamp((radius-std::hypot(x-nearX,y-nearY))/radius,0.0,1.0);
    const double factor=height*t*t*(3-2*t)*(1/std::sqrt(n*n+(n*n-1)*(dx*dx+dy*dy))-1);
    return {float(x+factor*dx),float(y+factor*dy)};
}
State threeTrackState() {
    State state; state.options[1] = 3; state.trackMeta[0] = 3;
    for (unsigned index = 0; index < 3; ++index) {
        auto& track = state.tracks[index];
        track.rect[1] = 32.f + 44.f * index;
        track.shape[1] = .25f * (index + 1); track.shape[2] = float(index);
        const float centerX = track.rect[0] + track.rect[2] * track.shape[1];
        state.controls[index] = {{centerX - 15, track.rect[1] - 7, 30, 22}, {11, 0, 0, 0}};
        for (unsigned channel = 0; channel < 3; ++channel) {
            track.active[channel] = channel == index ? .9f : .1f;
            track.inactive[channel] = channel == (index + 1) % 3 ? .7f : .2f;
        }
    }
    return state;
}
State gridTrackState() {
    State state; state.trackMeta[0] = 64;
    for (unsigned index = 0; index < 64; ++index) {
        auto& track = state.tracks[index];
        track.rect[0] = 4.f + 32.f * (index % 8); track.rect[1] = 6.f + 20.f * (index / 8);
        track.rect[2] = 24; track.shape[1] = .5f * (index % 3); track.shape[2] = float(index);
        for (unsigned channel = 0; channel < 3; ++channel) {
            track.active[channel] = channel == index % 3 ? .9f : .1f;
            track.inactive[channel] = channel == (index + 1) % 3 ? .7f : .2f;
        }
    }
    return state;
}
struct Result { std::string name; bool passed; double metric; double limit; bool atLeast; };
struct Report {
    std::vector<Result> results;
    struct Timing { unsigned tracks, frames; double millisecondsPerFrame; };
    std::vector<Timing> timings;
    void check(std::string name, double metric, double limit, bool atLeast = false) {
        bool passed = atLeast ? metric >= limit : metric <= limit;
        std::cout << (passed ? "PASS " : "FAIL ") << name << " metric=" << metric << " limit=" << limit << '\n';
        results.push_back({std::move(name), passed, metric, limit, atLeast});
    }
    bool write(const std::filesystem::path& path, unsigned featureLevel) {
        bool passed = std::all_of(results.begin(), results.end(), [](auto& r) { return r.passed; });
        std::ofstream out(path);
        out << std::setprecision(9) << "{\n  \"passed\": " << (passed ? "true" : "false")
            << ",\n  \"backend\": \"D3D11 WARP; no HWND, capture or desktop access\","
            << "\n  \"surfaceFormat\": \"B8G8R8A8_UNORM\","
            << "\n  \"blurIntermediateFormat\": \"R16G16B16A16_FLOAT\",\n  \"featureLevel\": " << featureLevel
            << ",\n  \"constantBufferBytes\": " << sizeof(State)
            << ",\n  \"resolution\": [" << Width << ',' << Height << "],\n  \"checks\": [\n";
        for (size_t i = 0; i < results.size(); ++i) {
            auto& r = results[i];
            out << "    {\"name\": \"" << r.name << "\", \"passed\": " << (r.passed ? "true" : "false")
                << ", \"metric\": " << r.metric << ", \"comparison\": \"" << (r.atLeast ? ">=" : "<=")
                << "\", \"limit\": " << r.limit << '}'
                << (i + 1 == results.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"warpTiming\": {\n    \"scope\": \"OpticsPS draw plus synchronous BGRA8 readback; synthetic 256x160, one fixed control, two warmups; not hardware FPS or acceptance\",\n    \"samples\": [";
        for (size_t i = 0; i < timings.size(); ++i) {
            const auto& sample = timings[i];
            out << (i ? "," : "") << "{\"tracks\":" << sample.tracks << ",\"frames\":" << sample.frames
                << ",\"millisecondsPerFrame\":" << sample.millisecondsPerFrame << '}';
        }
        out << "]\n  }\n}\n"; return passed;
    }
};

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: glass_gpu_regression SHADER_PATH REPORT_JSON");
        Gpu gpu(argv[1]); Report report;
        report.check("compiled_shader_constant_buffer_bytes", gpu.reflectedBufferBytes == 6208 ? 0 : 1, 0);
        report.check("compiled_shader_track_meta_offset", gpu.reflectedTrackMetaOffset == 2096 ? 0 : 1, 0);
        report.check("compiled_shader_track_offset", gpu.reflectedTrackOffset == 2112 ? 0 : 1, 0);
        report.check("compiled_shader_track_array_count", gpu.reflectedTrackCount == 64 ? 0 : 1, 0);
        report.check("compiled_shader_track_array_stride", gpu.reflectedTrackStride == 64 ? 0 : 1, 0);
        constexpr float OneByte = 1.01f / 255;
        // The weak environment reflection follows the independent coordinate
        // and input-color reference; it cannot conceal a shifted track.
        const auto reflectedFace = [](Pixel source) {
            for (unsigned channel = 0; channel < 3; ++channel)
                source[channel] += (1.f - source[channel]) * .042579995f;
            return source;
        };
        Pixels background(Width * Height);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x)
            background[y * Width + x] = {float(40 + x / 2) / 255, float(70 + y / 2) / 255, float(120 + (x + y) / 8) / 255, 1};
        State state;
        report.check("disabled_track_preserves_every_background_pixel", maxDelta(background, gpu.render(background, state), 4), 0);

        // A deliberately inactive owner is valid only in this isolated shader test.
        // The production bridge requires its track to belong to a live control.
        state.trackMeta[0] = 1;
        for (float progress : {0.f, .5f, 1.f}) {
            state.tracks[0].shape[1] = progress;
            auto actual = gpu.render(background, state);
            float interior = 0, exterior = 0, boundaryEnvelope = 0;
            unsigned interiorCount = 0, boundaryCount = 0;
            for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
                auto i = y * Width + x;
                float distance = capsuleDistance(x + .5f, y + .5f, state.tracks[0]);
                bool active = progress > 0 && (progress >= 1 || x + .5f < state.tracks[0].rect[0] + state.tracks[0].rect[2] * progress);
                auto expected = composite(background[i], active ? state.tracks[0].active : state.tracks[0].inactive);
                if (distance <= -.5f) { interior = std::max(interior, delta(expected, actual[i])); ++interiorCount; }
                else if (distance >= .5f) exterior = std::max(exterior, delta(background[i], actual[i]));
                else {
                    ++boundaryCount;
                    // Antialias pixels must stay inside the convex interval of one
                    // source-over operation, rather than accumulating two tints.
                    for (unsigned c = 0; c < 3; ++c)
                        boundaryEnvelope = std::max(boundaryEnvelope, std::max(std::min(background[i][c], expected[c]) - actual[i][c], actual[i][c] - std::max(background[i][c], expected[c])));
                }
            }
            auto label = std::to_string(int(progress * 100));
            report.check("track_" + label + "_straight_progress_and_single_alpha_interior", interior, OneByte);
            report.check("track_" + label + "_rounded_capsule_exterior", exterior, 0);
            report.check("track_" + label + "_antialias_convex_color_bounds", boundaryEnvelope, OneByte);
            report.check("track_" + label + "_opaque_output", alphaError(actual), 0);
            report.check("track_" + label + "_interior_sample_count", interiorCount, 1400, true);
            report.check("track_" + label + "_boundary_sample_count", boundaryCount, 8, true);
        }
        // Track with both tints alpha zero must preserve the source, even with
        // lens evaluation enabled; the track's presence must not add alpha.
        State clear = state; clear.tracks[0].active[3] = clear.tracks[0].inactive[3] = 0;
        report.check("transparent_track_preserves_background", maxDelta(background, gpu.render(background, clear), 4), 0);

        Pixels flat(Width * Height, Pixel{90 / 255.f, 110 / 255.f, 130 / 255.f, 1});
        State lens; lens.trackMeta[0] = 1; lens.tracks[0].shape[1] = .5f;
        lens.options[1] = 1;
        lens.controls[0] = {{98, 60, 60, 40}, {20, 0, 0, 0}};
        State noTrack = lens; noTrack.trackMeta[0] = 0;
        auto lensed = gpu.render(flat, lens), baseline = gpu.render(flat, noTrack);
        State noLens = lens; noLens.options[1] = 0;
        auto straight = gpu.render(flat, noLens);
        unsigned displacedPixels = 0; float strongestDisplacement = 0, centerError = 0;
        for (unsigned y = 61; y < 99; ++y) for (unsigned x = 99; x < 157; ++x) {
            if (y >= 76 && y < 84) continue;
            auto difference = delta(lensed[y * Width + x], baseline[y * Width + x]);
            if (difference > .02f) ++displacedPixels;
            strongestDisplacement = std::max(strongestDisplacement, difference);
        }
        for (unsigned y : {79u, 80u}) for (unsigned x = 121; x < 135; ++x) {
            Pixel expected{};expected[3]=1;
            const auto& track=lens.tracks[0];
            for(unsigned channel=0;channel<3;++channel) {
                const auto q=bodySample(x+.5f,y+.5f,lens.controls[0],channel);
                // This inner strip remains fully inside the bar. Only the
                // active/inactive split moves; account for premultiplied colors.
                const float active=std::clamp(track.rect[0]+track.rect[2]*track.shape[1]-q[0]+.5f,0.f,1.f);
                const float alpha=active*track.active[3]+(1-active)*track.inactive[3];
                const float base=std::round(flat[0][channel]*255)/255;
                expected[channel]=base*(1-alpha)+active*track.active[channel]*track.active[3]
                    +(1-active)*track.inactive[channel]*track.inactive[3];
            }
            centerError=std::max(centerError,delta(lensed[y*Width+x],reflectedFace(expected)));
        }
        report.check("thumb_shoulder_moves_track_outside_original_bar", displacedPixels, 12, true);
        report.check("thumb_shoulder_has_nontrivial_displacement_signal", strongestDisplacement, .04, true);
        report.check("thumb_interior_track_matches_slab_projection", centerError, OneByte);
        report.check("thumb_output_alpha_is_one", alphaError(lensed), 0);

        State duplicate = lens; duplicate.controls[1] = duplicate.controls[0]; duplicate.options[1] = 2;
        report.check("coincident_controls_do_not_accumulate_tint_or_alpha", maxDelta(lensed, gpu.render(flat, duplicate), 4), 0);
        State clearLens = lens; clearLens.tracks[0].active[3] = clearLens.tracks[0].inactive[3] = 0;
        report.check("transparent_track_under_thumb_is_identity", maxDelta(baseline, gpu.render(flat, clearLens), 4), 0);

        // Actual GPU negative control: first render the lens without a track,
        // then run the production pixel shader with the straight track and no
        // lens over that result. This reproduces an upper DOM track's ordering.
        auto afterOptics = gpu.render(baseline, noLens);
        unsigned negativeDisplacement = 0;
        for (unsigned y = 61; y < 99; ++y) for (unsigned x = 99; x < 157; ++x) {
            if (y >= 76 && y < 84) continue;
            if (delta(afterOptics[y * Width + x], baseline[y * Width + x]) > .02f) ++negativeDisplacement;
        }
        report.check("negative_post_optics_track_is_rejected_by_displacement_gate", negativeDisplacement, 0);
        report.check("negative_post_optics_track_differs_from_current_pipeline", maxDelta(lensed, afterOptics), .04, true);

        // Three independent settings tracks share one draw. Each real-sized
        // thumb must bend its own track, including the second and third slots.
        {
            auto multi = threeTrackState();
            auto noTracks = multi; noTracks.trackMeta[0] = 0;
            auto noControls = multi; noControls.options[1] = 0;
            const auto actual = gpu.render(flat, multi), underlay = gpu.render(flat, noTracks);
            const auto wrongOrder = gpu.render(underlay, noControls);
            const auto straightTracks = gpu.render(background, noControls);
            report.check("three_tracks_and_thumbs_output_alpha_is_one", alphaError(actual), 0);
            for (unsigned index = 0; index < 3; ++index) {
                const auto& track = multi.tracks[index]; const auto& control = multi.controls[index];
                unsigned displaced = 0, incorrectDisplacement = 0; float peak = 0, primitiveError = 0;
                for (unsigned y = unsigned(control.rect[1]) + 1; y < unsigned(control.rect[1] + control.rect[3]) - 1; ++y)
                for (unsigned x = unsigned(control.rect[0]) + 1; x < unsigned(control.rect[0] + control.rect[2]) - 1; ++x) {
                    if (y >= unsigned(track.rect[1]) && y < unsigned(track.rect[1] + track.rect[3])) continue;
                    const auto i = y * Width + x;
                    const auto difference = delta(actual[i], underlay[i]);
                    displaced += difference > .02f; peak = std::max(peak, difference);
                    incorrectDisplacement += delta(wrongOrder[i], underlay[i]) > .02f;
                }
                for (unsigned x = 30; x < 226; ++x) {
                    const auto i = unsigned(track.rect[1] + 4) * Width + x;
                    const bool active = x + .5f < track.rect[0] + track.rect[2] * track.shape[1];
                    primitiveError = std::max(primitiveError, delta(straightTracks[i], composite(background[i], active ? track.active : track.inactive)));
                }
                const auto prefix = "three_tracks_slot_" + std::to_string(index);
                report.check(prefix + "_keeps_own_color_and_progress", primitiveError, OneByte);
                report.check(prefix + "_thumb_moves_track_outside_bar", displaced, 6, true);
                report.check(prefix + "_thumb_displacement_signal", peak, .04, true);
                report.check(prefix + "_post_optics_negative_has_no_displacement", incorrectDisplacement, 0);
                auto muted = multi; muted.tracks[index].active[3] = muted.tracks[index].inactive[3] = 0;
                auto withoutOne = gpu.render(flat, muted);
                float otherError = 0, ownDifference = 0;
                for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
                    const auto i = y * Width + x;
                    if (y + .5f >= control.rect[1] && y + .5f < control.rect[1] + control.rect[3])
                        ownDifference = std::max(ownDifference, delta(actual[i], withoutOne[i]));
                    else otherError = std::max(otherError, delta(actual[i], withoutOne[i], 4));
                }
                report.check(prefix + "_has_independent_visible_content", ownDifference, .04, true);
                report.check(prefix + "_muting_leaves_other_tracks_exact", otherError, 0);
            }
            auto transparent = multi;
            for (auto& track : transparent.tracks) track.active[3] = track.inactive[3] = 0;
            report.check("three_transparent_tracks_preserve_lenses_exactly", maxDelta(underlay, gpu.render(flat, transparent), 4), 0);
        }
        {
            auto grid = gridTrackState(); const auto actual = gpu.render(background, grid);
            float independentColors = 0;
            for (const auto& track : grid.tracks) for (unsigned offset : {6u, 18u}) {
                const auto x = unsigned(track.rect[0]) + offset, y = unsigned(track.rect[1]) + 4;
                const bool active = track.shape[1] > 0 && (track.shape[1] >= 1 || x + .5f < track.rect[0] + track.rect[2] * track.shape[1]);
                independentColors = std::max(independentColors, delta(actual[y * Width + x],
                    composite(background[y * Width + x], active ? track.active : track.inactive)));
            }
            report.check("all_64_tracks_keep_independent_colors_and_progress", independentColors, OneByte);
            report.check("all_64_tracks_output_alpha_is_one", alphaError(actual), 0);
            auto first63 = grid; first63.trackMeta[0] = 63;
            const auto truncated = gpu.render(background, first63);
            const auto& last = grid.tracks[63];
            float priorError = 0, lastSignal = 0;
            for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
                const auto i = y * Width + x;
                if (capsuleDistance(x + .5f, y + .5f, last) < .5f)
                    lastSignal = std::max(lastSignal, delta(actual[i], truncated[i]));
                else priorError = std::max(priorError, delta(actual[i], truncated[i], 4));
            }
            report.check("slot_63_is_drawn_when_count_is_64", lastSignal, .04, true);
            report.check("changing_count_63_to_64_leaves_previous_slots_exact", priorError, 0);
            auto none = grid; none.trackMeta[0] = 0;
            report.check("zero_count_ignores_all_64_populated_slots", maxDelta(background, gpu.render(background, none), 4), 0);
            auto transparent = grid;
            for (auto& track : transparent.tracks) track.active[3] = track.inactive[3] = 0;
            report.check("all_64_transparent_tracks_preserve_background_exactly", maxDelta(background, gpu.render(background, transparent), 4), 0);

            // One fixed control and one fixed populated array isolate loop count
            // in this bounded software-renderer measurement. Readback forces
            // completion; these numbers include readback and are not GPU FPS.
            grid.controls[0] = {{24,24,208,112},{20,0,0,0}}; grid.options[1] = 1;
            for (unsigned count : {3u, 64u}) {
                grid.trackMeta[0] = float(count);
                constexpr unsigned frames = 12;
                report.timings.push_back({count, frames, gpu.timedDrawAndReadback(background, grid, frames)});
            }
        }
        {
            // Exercise both final array slots together. Earlier valid controls
            // and their tracks coincide in a remote corner, leaving the last
            // thumb's neighborhood free of any other lens or track.
            State full; full.options[1] = 64; full.trackMeta[0] = 64;
            for (unsigned index = 0; index < 63; ++index) {
                full.controls[index] = {{4,4,30,22},{11,0,0,0}};
                full.tracks[index].rect[0] = 4; full.tracks[index].rect[1] = 11;
                full.tracks[index].rect[2] = 24; full.tracks[index].shape[2] = float(index);
            }
            full.controls[63] = {{113,69,30,22},{11,0,0,0}};
            full.tracks[63].shape[2] = 63;
            const auto actual = gpu.render(flat, full);
            auto missingTrack = full; missingTrack.trackMeta[0] = 63;
            const auto finalSlotBaseline = gpu.render(flat, missingTrack);
            auto missingControl = full; missingControl.options[1] = 63;
            auto missingBoth = missingControl; missingBoth.trackMeta[0] = 63;
            const auto finalSlotStraight = gpu.render(flat, missingControl), withoutBoth = gpu.render(flat, missingBoth);
            unsigned displaced = 0, incorrectDisplacement = 0; float peak = 0;
            for (unsigned y = 70; y < 90; ++y) for (unsigned x = 114; x < 142; ++x) {
                if (y >= 76 && y < 84) continue;
                const auto i = y * Width + x; const auto difference = delta(actual[i], finalSlotBaseline[i]);
                displaced += difference > .02f; peak = std::max(peak, difference);
                incorrectDisplacement += delta(finalSlotStraight[i], withoutBoth[i]) > .02f;
            }
            report.check("all_64_controls_and_tracks_last_thumb_displaces_last_track", displaced, 6, true);
            report.check("all_64_controls_and_tracks_last_thumb_has_visible_signal", peak, .04, true);
            report.check("missing_control_63_negative_cannot_displace_track_63", incorrectDisplacement, 0);
            report.check("all_64_controls_and_tracks_output_alpha_is_one", alphaError(actual), 0);
        }
        {
            State overlap; overlap.trackMeta[0] = 2;
            overlap.tracks[0].shape[1] = overlap.tracks[1].shape[1] = 1;
            for (unsigned channel = 0; channel < 4; ++channel)
                overlap.tracks[1].active[channel] = overlap.tracks[0].inactive[channel];
            const auto actual = gpu.render(background, overlap);
            auto reversed = overlap; std::swap(reversed.tracks[0], reversed.tracks[1]);
            const auto reverseFrame = gpu.render(background, reversed);
            float orderError = 0, reverseSignal = 0;
            for (unsigned y = 77; y < 83; ++y) for (unsigned x = 30; x < 226; ++x) {
                const auto i = y * Width + x;
                const auto expected = composite(composite(background[i], overlap.tracks[0].active), overlap.tracks[1].active);
                orderError = std::max(orderError, delta(actual[i], expected));
                reverseSignal = std::max(reverseSignal, delta(actual[i], reverseFrame[i]));
            }
            report.check("overlapping_tracks_apply_source_over_in_array_order_once", orderError, OneByte);
            report.check("overlapping_track_order_is_visibly_observable", reverseSignal, .04, true);
            report.check("overlapping_tracks_output_alpha_is_one", alphaError(actual), 0);
        }

        // Neutral sharp input isolates wavelength-dependent sampling from tint.
        // Use the actual production optics shader, not a copy of its equations.
        // Four straight shoulder patches stay away from rounded corners; the
        // inner rectangle is independently known to be beyond the 20 px radius.
        State dispersion;
        dispersion.options[1] = 1;
        dispersion.controls[0] = {{24, 24, 208, 112}, {20, 0, 0, 0}};
        Pixels oracleRG(Width*Height),oracleB(Width*Height);
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x) {
            const auto r=bodySample(x+.5f,y+.5f,dispersion.controls[0],0);
            const auto g=bodySample(x+.5f,y+.5f,dispersion.controls[0],1);
            const auto b=bodySample(x+.5f,y+.5f,dispersion.controls[0],2);
            oracleRG[y*Width+x]={r[0]/Width,r[1]/Height,g[0]/Width,g[1]/Height};
            oracleB[y*Width+x]={b[0]/Width,b[1]/Height,0,0};
        }
        auto shoulder = [](unsigned x, unsigned y) {
            return ((x >= 25 && x <= 43 || x >= 212 && x <= 230) && y >= 48 && y <= 111)
                || ((y >= 25 && y <= 43 || y >= 116 && y <= 134) && x >= 48 && x <= 207);
        };
        auto center = [](unsigned x, unsigned y) { return x >= 48 && x <= 207 && y >= 48 && y <= 111; };
        constexpr unsigned glyphs[4][7] = {
            {14, 17, 16, 23, 17, 17, 14}, // G
            {16, 16, 16, 16, 16, 16, 31}, // L
            {14, 17, 17, 31, 17, 17, 17}, // A
            {15, 16, 16, 14, 1, 1, 30}   // S
        };
        for (unsigned fixture = 0; fixture < 2; ++fixture) {
            Pixels source(Width * Height), inverse(source.size());
            for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
                unsigned column = x / 2 % 6, row = y / 2 % 9;
                bool ink = fixture == 0 ? (x % 8 < 2 || y % 12 < 2)
                    : (column < 5 && row < 7 && (glyphs[x / 12 % 4][row] & (1u << (4 - column))) != 0);
                float value = ink ? 0.f : 1.f;
                source[y * Width + x] = {value, value, value, 1};
                inverse[y * Width + x] = {1 - value, 1 - value, 1 - value, 1};
            }
            auto result = gpu.render(source, dispersion), inverted = gpu.render(inverse, dispersion);
            auto oracle=gpu.sampleOracle(source,oracleRG,oracleB);
            float peak = 0, centerChroma = 0, centerOracleError = 0, polarityError = 0, exteriorChroma = 0;
            unsigned visibleCount = 0;
            for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
                auto i = y * Width + x;
                if (shoulder(x, y)) {
                    peak = std::max(peak, chroma(result[i]));
                    if (chroma(result[i]) >= 32.f / 255) ++visibleCount;
                    // Reversing edge contrast must reverse R/B polarity. A
                    // decorative colored rim independent of the source fails.
                    polarityError = std::max(polarityError,
                        std::abs((result[i][0] - result[i][2]) + (inverted[i][0] - inverted[i][2])));
                }
                if (center(x, y)) {
                    centerChroma = std::max(centerChroma, chroma(result[i]));
                    centerOracleError=std::max(centerOracleError,delta(result[i],oracle[i]));
                    polarityError=std::max(polarityError,std::abs((result[i][0]-result[i][2])+(inverted[i][0]-inverted[i][2])));
                }
                if (x < 24 || x >= 232 || y < 24 || y >= 136)
                    exteriorChroma = std::max(exteriorChroma, chroma(result[i]));
            }
            std::string label = fixture == 0 ? "neutral_lines" : "neutral_glyphs";
            // On an 8-bit source, 64 levels at one edge plus 64 shoulder pixels
            // above 32 levels establish visible separation beyond quantization
            // and reject the previous sub-0.1-pixel dispersion coefficient.
            report.check(label + "_shoulder_peak_rgb_separation", peak, 64.f / 255, true);
            report.check(label + "_shoulder_visible_chroma_pixels", visibleCount, 64, true);
            report.check(label + "_contrast_reversal_reverses_chroma", polarityError, 2 * OneByte);
            report.check(label + "_interior_resolves_source_dependent_dispersion", centerChroma, 32.f/255, true);
            report.check(label + "_interior_matches_slab_color_oracle", centerOracleError, OneByte);
            report.check(label + "_outside_control_stays_neutral", exteriorChroma, 0);
            report.check(label + "_opaque_output", alphaError(result), 0);
        }
        // Area between monotone R/B edge responses measures their equivalent
        // spatial separation in pixels without fitting Snell's equation.
        Pixels edge(Width * Height);
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            float value = x >= 33 ? 1.f : 0.f;
            edge[y * Width + x] = {value, value, value, 1};
        }
        auto edgeResult = gpu.render(edge, dispersion);
        float edgeSeparation = 0;
        for (unsigned x = 25; x <= 43; ++x)
            edgeSeparation += std::abs(edgeResult[80 * Width + x][0] - edgeResult[80 * Width + x][2]);
        report.check("neutral_edge_equivalent_rb_separation_pixels_minimum", edgeSeparation, .5, true);
        // The fixed receiver deliberately resolves a wider spectral band than
        // the former local-height model. Bound it to one fifth of this 20 px
        // shoulder; spatially uniform inputs must still stay neutral.
        report.check("neutral_edge_equivalent_rb_separation_pixels_bounded", edgeSeparation, 4.0);
        for (float value : {0.f, 128.f / 255, 1.f}) {
            auto uniform = gpu.render(Pixels(Width * Height, Pixel{value, value, value, 1}), dispersion);
            float generatedChroma = 0;
            for (auto& pixel : uniform) generatedChroma = std::max(generatedChroma, chroma(pixel));
            std::string label = "uniform_" + std::to_string(int(std::lround(value * 255)));
            report.check(label + "_glass_never_generates_rainbow", generatedChroma, 0);
            report.check(label + "_glass_output_alpha_is_one", alphaError(uniform), 0);
        }

        // Full two-pass production blur with nonzero trailing track fields.
        // A half-plane contains premultiplied white and the other half no
        // coverage: normalized convolution must produce white wherever coverage
        // reaches, including the formerly blackened transparent boundary.
        Pixels halfPlane(Width * Height, Pixel{0, 0, 0, 0});
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width / 2; ++x)
            halfPlane[y * Width + x] = {1, 1, 1, 1};
        for (float sigma : {0.f, 1.5f, 16.f, 42.f}) for (float tint : {0.f, .74f}) {
            State blurState = lens; blurState.options[0] = sigma;
            blurState.tint[0] = 37 / 255.f; blurState.tint[1] = 73 / 255.f;
            blurState.tint[2] = 109 / 255.f; blurState.tint[3] = tint;
            auto result = gpu.renderBlur(halfPlane, blurState);
            Pixel white = composite(Pixel{1, 1, 1, 1}, blurState.tint);
            float error = 0;
            // Both the opaque side and first transparent-side pixel have
            // coverage for nonzero blur. sigma zero checks the opaque side.
            for (unsigned x : {120u, 127u, sigma > 0 ? 128u : 127u})
                error = std::max(error, delta(result[80 * Width + x], white));
            auto label = std::to_string(sigma) + "_tint_" + std::to_string(tint);
            report.check("normalized_blur_" + label + "_no_dark_halo", error, OneByte);
            report.check("normalized_blur_" + label + "_opaque_output", alphaError(result), 0);
        }
        Pixels empty(Width * Height, Pixel{0, 0, 0, 0});
        State emptyState = lens; emptyState.options[0] = 42;
        emptyState.tint[0] = 37 / 255.f; emptyState.tint[1] = 73 / 255.f; emptyState.tint[2] = 109 / 255.f;
        auto emptyResult = gpu.renderBlur(empty, emptyState);
        report.check("zero_coverage_blur_returns_exact_tint", maxDelta(emptyResult, Pixels(Width * Height, Pixel{37 / 255.f, 73 / 255.f, 109 / 255.f, 1}), 4), 0);
        return report.write(argv[2], gpu.level) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
