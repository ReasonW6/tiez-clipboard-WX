// Isolated Windows Graphics Capture / D3D11 optical feasibility probe.
// Capture is restricted to the animated window created by this process.
// References: docs/WINDOWS-GLASS-RESEARCH.md. No production Tauri code is used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include "glass_capture_probe_vs.generated.h"
#include "glass_capture_probe_ps.generated.h"
#include <dxgi1_2.h>
#include <dcomp.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
constexpr UINT_PTR kFrameTimer = 1;
constexpr int kGlassWidth = 460;
constexpr int kGlassHeight = 540;
std::ofstream evidence;
HWND sourceWindow = nullptr;
HWND glassWindow = nullptr;
ULONGLONG started = 0;
bool closing = false;
bool refractionEnabled = true;
bool dispersionEnabled = true;
bool controlsLayoutEnabled = false;
bool sourceAnimationEnabled = true;
ULONGLONG animationPausedAt = 0;
ULONGLONG animationPausedDuration = 0;

double source_seconds() {
    const auto time = sourceAnimationEnabled ? GetTickCount64() : animationPausedAt;
    return (time - started - animationPausedDuration) / 1000.0;
}

void update_labels() {
    const std::wstring state = L"refraction=" + std::wstring(refractionEnabled ? L"ON" : L"OFF") +
        L" | dispersion=" + std::wstring(dispersionEnabled ? L"ON" : L"OFF") +
        L" | source=" + std::wstring(sourceAnimationEnabled ? L"MOVING" : L"PAUSED") +
        L" | layout=" + std::wstring(controlsLayoutEnabled ? L"CONTROLS" : L"PANEL");
    SetWindowTextW(sourceWindow, (L"TieZ Glass Capture QA | owned source | " + state).c_str());
    SetWindowTextW(glassWindow, (L"TieZ Glass Capture QA | optical output | " + state).c_str());
    InvalidateRect(sourceWindow, nullptr, FALSE);
}

void log(const std::string& fields) {
    evidence << "{\"elapsed_ms\":" << (GetTickCount64() - started) << ',' << fields << "}\n";
    evidence.flush();
}

void log_error(const char* phase, HRESULT hr) {
    std::ostringstream line;
    line << "\"event\":\"error\",\"phase\":\"" << phase << "\",\"hresult\":\"0x"
         << std::hex << static_cast<unsigned long>(hr) << "\"";
    log(line.str());
}

void verify_affinity(HWND window, const char* role) {
    const BOOL setOk = SetWindowDisplayAffinity(window, WDA_NONE);
    const DWORD setError = setOk ? 0 : GetLastError();
    DWORD value = 0xffffffff;
    const BOOL getOk = GetWindowDisplayAffinity(window, &value);
    log("\"event\":\"display_affinity\",\"role\":\"" + std::string(role) +
        "\",\"set_ok\":" + std::to_string(setOk != FALSE) +
        ",\"set_error\":" + std::to_string(setError) +
        ",\"get_ok\":" + std::to_string(getOk != FALSE) +
        ",\"value\":" + std::to_string(value));
    if (!setOk || !getOk || value != WDA_NONE) throw hresult_error(E_ACCESSDENIED);
}

constexpr char kShader[] = R"hlsl(
#include "glass_probe_layout.hlsli"
cbuffer Parameters : register(b0) {
    float2 glassSize;
    float2 sourceSize;
    float2 sourceOffset;
    float2 sourceScale;
    float refractionOn;
    float dispersionOn;
    float controlsLayout;
    float reserved;
};
Texture2D<float4> sourceImage : register(t0);
SamplerState linearClamp : register(s0);
struct Vertex { float4 position : SV_POSITION; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v;
    float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float4 PS(Vertex input) : SV_TARGET {
    float2 pixel = input.position.xy;
    GlassOptics optics = GlassProbeEvaluate(pixel, glassSize, controlsLayout);
    if (optics.coverage <= 0.0) return 0;
    float2 basePixel = sourceOffset + pixel * sourceScale;
    float2 greenOffset = optics.offsetG * refractionOn;
    float2 redOffset = lerp(optics.offsetG, optics.offsetR, dispersionOn) * refractionOn;
    float2 blueOffset = lerp(optics.offsetG, optics.offsetB, dispersionOn) * refractionOn;
    float3 color;
    color.r = sourceImage.Sample(linearClamp, (basePixel + redOffset) / sourceSize).r;
    color.g = sourceImage.Sample(linearClamp, (basePixel + greenOffset) / sourceSize).g;
    color.b = sourceImage.Sample(linearClamp, (basePixel + blueOffset) / sourceSize).b;
    // Undecorated source first, then exactly one geometry-derived highlight.
    color = saturate(color + optics.highlight);
    return float4(color * optics.coverage, optics.coverage);
}
)hlsl";

struct Parameters {
    float glassSize[2];
    float sourceSize[2];
    float sourceOffset[2];
    float sourceScale[2];
    float refractionOn;
    float dispersionOn;
    float controlsLayout;
    float reserved;
};
static_assert(sizeof(Parameters) % 16 == 0);

struct Probe {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain1> swapChain;
    com_ptr<IDCompositionDevice> composition;
    com_ptr<IDCompositionTarget> target;
    com_ptr<IDCompositionVisual> visual;
    com_ptr<ID3D11VertexShader> vertex;
    com_ptr<ID3D11PixelShader> pixel;
    com_ptr<ID3D11SamplerState> sampler;
    com_ptr<ID3D11Buffer> constants;
    com_ptr<ID3D11Texture2D> sourceTexture;
    com_ptr<ID3D11ShaderResourceView> sourceView;
    IDirect3DDevice captureDevice{nullptr};
    GraphicsCaptureItem captureItem{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    UINT sourceWidth = 0, sourceHeight = 0;
    UINT outputWidth = 0, outputHeight = 0;
    unsigned long long frames = 0, drained = 0, paints = 0;
    ULONGLONG lastFrame = 0, lastReport = 0;
    LONGLONG lastSourceTimestamp = 0;
    RECT lastSourceBounds{};
    POINT lastOutputOrigin{};
    float lastSourceScaleX = 0, lastSourceScaleY = 0;

    void initialize() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual{};
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
            device.put(), &actual, context.put()));
        auto dxgi = device.as<IDXGIDevice>();
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(dxgi->GetAdapter(adapter.put()));
        DXGI_ADAPTER_DESC adapterDesc{};
        check_hresult(adapter->GetDesc(&adapterDesc));
        log("\"event\":\"device\",\"feature_level\":" + std::to_string(actual) +
            ",\"vendor_id\":" + std::to_string(adapterDesc.VendorId) +
            ",\"device_id\":" + std::to_string(adapterDesc.DeviceId));
        com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        RECT client{}; GetClientRect(glassWindow, &client);
        outputWidth = client.right; outputHeight = client.bottom;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = outputWidth; desc.Height = outputHeight;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check_hresult(factory->CreateSwapChainForComposition(device.get(), &desc, nullptr, swapChain.put()));
        check_hresult(DCompositionCreateDevice(dxgi.get(), __uuidof(IDCompositionDevice), composition.put_void()));
        check_hresult(composition->CreateTargetForHwnd(glassWindow, TRUE, target.put()));
        check_hresult(composition->CreateVisual(visual.put()));
        check_hresult(visual->SetContent(swapChain.get()));
        check_hresult(target->SetRoot(visual.get()));
        check_hresult(composition->Commit());

        check_hresult(device->CreateVertexShader(kGlassProbeVS, sizeof(kGlassProbeVS), nullptr, vertex.put()));
        check_hresult(device->CreatePixelShader(kGlassProbePS, sizeof(kGlassProbePS), nullptr, pixel.put()));
        log("\"event\":\"shader_loaded\",\"embedded_bytecode\":true,\"runtime_includes\":false,\"optics\":\"shared_glass_optics\"");
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        check_hresult(device->CreateSamplerState(&sample, sampler.put()));
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Parameters);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check_hresult(device->CreateBuffer(&buffer, nullptr, constants.put()));

        if (!GraphicsCaptureSession::IsSupported()) throw hresult_error(E_NOTIMPL);
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        DWORD owner = 0;
        GetWindowThreadProcessId(sourceWindow, &owner);
        if (owner != GetCurrentProcessId()) throw hresult_error(E_ACCESSDENIED);
        check_hresult(interop->CreateForWindow(sourceWindow, guid_of<GraphicsCaptureItem>(), put_abi(captureItem)));
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        captureDevice = inspectable.as<IDirect3DDevice>();
        const auto size = captureItem.Size();
        if (size.Width <= 0 || size.Height <= 0) throw hresult_error(E_INVALIDARG);
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(captureDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session = pool.CreateCaptureSession(captureItem);
        session.IsCursorCaptureEnabled(false);
        // Keep the system's capture indicator policy; do not request borderless capture.
        try {
            log("\"event\":\"capture_border\",\"is_border_required\":" +
                std::to_string(session.IsBorderRequired()) + ",\"borderless_access_requested\":false");
        } catch (hresult_error const& error) { log_error("read_capture_border", error.code()); }
        session.StartCapture();
        log("\"event\":\"capture_started\",\"source_is_owned\":true,\"pid\":" +
            std::to_string(owner) + ",\"capture_width\":" + std::to_string(size.Width) +
            ",\"capture_height\":" + std::to_string(size.Height));
    }

    void toggle_border() {
        try {
            const bool requested = !session.IsBorderRequired();
            session.IsBorderRequired(requested);
            log("\"event\":\"capture_border_setter\",\"requested_required\":" +
                std::to_string(requested) + ",\"readback_required\":" +
                std::to_string(session.IsBorderRequired()) +
                ",\"setter_ok\":true,\"borderless_access_requested\":false,\"visual_state_verified\":false");
        } catch (hresult_error const& error) { log_error("set_capture_border", error.code()); }
    }

    void update() {
        auto frame = pool.TryGetNextFrame();
        if (frame) {
            for (int i = 0; i < 4; ++i) {
                auto next = pool.TryGetNextFrame();
                if (!next) break;
                frame.Close(); frame = std::move(next); ++drained;
            }
            const auto content = frame.ContentSize();
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            com_ptr<ID3D11Texture2D> input;
            check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), input.put_void()));
            D3D11_TEXTURE2D_DESC desc{}; input->GetDesc(&desc);
            if (!sourceTexture || desc.Width != sourceWidth || desc.Height != sourceHeight) {
                sourceView = nullptr; sourceTexture = nullptr;
                sourceWidth = desc.Width; sourceHeight = desc.Height;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
                check_hresult(device->CreateTexture2D(&desc, nullptr, sourceTexture.put()));
                check_hresult(device->CreateShaderResourceView(sourceTexture.get(), nullptr, sourceView.put()));
                log("\"event\":\"source_texture\",\"width\":" + std::to_string(sourceWidth) +
                    ",\"height\":" + std::to_string(sourceHeight) +
                    ",\"content_width\":" + std::to_string(content.Width) +
                    ",\"content_height\":" + std::to_string(content.Height));
            }
            ID3D11ShaderResourceView* noView = nullptr;
            context->PSSetShaderResources(0, 1, &noView);
            context->CopyResource(sourceTexture.get(), input.get());
            lastSourceTimestamp = frame.SystemRelativeTime().count();
            frame.Close();
            ++frames; lastFrame = GetTickCount64();
            if (frames == 1) log("\"event\":\"first_frame\",\"source_timestamp_100ns\":" + std::to_string(lastSourceTimestamp));
            if (content.Width != static_cast<int>(sourceWidth) || content.Height != static_cast<int>(sourceHeight)) {
                pool.Recreate(captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
            }
        }
        if (sourceTexture) render();
        const auto now = GetTickCount64();
        if (now - lastReport >= 1000) {
            lastReport = now;
            DWORD sourceAffinity = 0xffffffff, outputAffinity = 0xffffffff;
            const BOOL sourceOk = GetWindowDisplayAffinity(sourceWindow, &sourceAffinity);
            const BOOL outputOk = GetWindowDisplayAffinity(glassWindow, &outputAffinity);
            log("\"event\":\"progress\",\"frames\":" + std::to_string(frames) +
                ",\"presented\":" + std::to_string(paints) + ",\"drained\":" + std::to_string(drained) +
                ",\"source_timestamp_100ns\":" + std::to_string(lastSourceTimestamp) +
                ",\"frame_age_ms\":" + std::to_string(lastFrame ? now - lastFrame : now - started) +
                ",\"source_affinity_ok\":" + std::to_string(sourceOk != FALSE) +
                ",\"source_affinity\":" + std::to_string(sourceAffinity) +
                ",\"output_affinity_ok\":" + std::to_string(outputOk != FALSE) +
                ",\"output_affinity\":" + std::to_string(outputAffinity) +
                ",\"refraction\":" + std::to_string(refractionEnabled) +
                ",\"dispersion\":" + std::to_string(dispersionEnabled) +
                ",\"controls_layout\":" + std::to_string(controlsLayoutEnabled) +
                ",\"source_animation\":" + std::to_string(sourceAnimationEnabled) +
                ",\"source_left\":" + std::to_string(lastSourceBounds.left) +
                ",\"source_top\":" + std::to_string(lastSourceBounds.top) +
                ",\"source_right\":" + std::to_string(lastSourceBounds.right) +
                ",\"source_bottom\":" + std::to_string(lastSourceBounds.bottom) +
                ",\"output_origin_x\":" + std::to_string(lastOutputOrigin.x) +
                ",\"output_origin_y\":" + std::to_string(lastOutputOrigin.y) +
                ",\"output_width\":" + std::to_string(outputWidth) +
                ",\"output_height\":" + std::to_string(outputHeight) +
                ",\"source_scale_x\":" + std::to_string(lastSourceScaleX) +
                ",\"source_scale_y\":" + std::to_string(lastSourceScaleY));
            if (!sourceOk || !outputOk || sourceAffinity != WDA_NONE || outputAffinity != WDA_NONE)
                throw hresult_error(E_ACCESSDENIED);
            if (!frames && now - started > 8000) throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
    }

    void render() {
        RECT client{}; GetClientRect(glassWindow, &client);
        if (client.right <= 0 || client.bottom <= 0) return;
        if (static_cast<UINT>(client.right) != outputWidth || static_cast<UINT>(client.bottom) != outputHeight) {
            context->OMSetRenderTargets(0, nullptr, nullptr);
            outputWidth = client.right; outputHeight = client.bottom;
            check_hresult(swapChain->ResizeBuffers(2, outputWidth, outputHeight, DXGI_FORMAT_UNKNOWN, 0));
        }
        RECT sourceBounds{};
        check_hresult(DwmGetWindowAttribute(sourceWindow, DWMWA_EXTENDED_FRAME_BOUNDS, &sourceBounds, sizeof(sourceBounds)));
        POINT origin{}; ClientToScreen(glassWindow, &origin);
        const float scaleX = static_cast<float>(sourceWidth) / (sourceBounds.right - sourceBounds.left);
        const float scaleY = static_cast<float>(sourceHeight) / (sourceBounds.bottom - sourceBounds.top);
        lastSourceBounds = sourceBounds; lastOutputOrigin = origin;
        lastSourceScaleX = scaleX; lastSourceScaleY = scaleY;
        Parameters params{{static_cast<float>(outputWidth), static_cast<float>(outputHeight)},
            {static_cast<float>(sourceWidth), static_cast<float>(sourceHeight)},
            {(origin.x - sourceBounds.left) * scaleX, (origin.y - sourceBounds.top) * scaleY},
            {scaleX, scaleY}, refractionEnabled ? 1.0f : 0.0f, dispersionEnabled ? 1.0f : 0.0f,
            controlsLayoutEnabled ? 1.0f : 0.0f, 0};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(context->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, &params, sizeof(params));
        context->Unmap(constants.get(), 0);
        com_ptr<ID3D11Texture2D> backBuffer;
        check_hresult(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
        com_ptr<ID3D11RenderTargetView> targetView;
        check_hresult(device->CreateRenderTargetView(backBuffer.get(), nullptr, targetView.put()));
        const float clear[4] = {0, 0, 0, 0};
        context->ClearRenderTargetView(targetView.get(), clear);
        auto* rtv = targetView.get(); context->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(outputWidth), static_cast<float>(outputHeight), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.get(), nullptr, 0);
        context->PSSetShader(pixel.get(), nullptr, 0);
        auto* srv = sourceView.get(); context->PSSetShaderResources(0, 1, &srv);
        auto* ss = sampler.get(); context->PSSetSamplers(0, 1, &ss);
        auto* cb = constants.get(); context->PSSetConstantBuffers(0, 1, &cb);
        context->Draw(3, 0);
        check_hresult(swapChain->Present(1, 0));
        ++paints;
    }

    ~Probe() {
        try { if (session) session.Close(); if (pool) pool.Close(); } catch (...) {}
        if (context) { context->ClearState(); context->Flush(); }
    }
};
std::unique_ptr<Probe> probe;

void paint_source(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT rect{}; GetClientRect(window, &rect);
    HDC buffered = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, rect.right), std::max(1L, rect.bottom));
    auto oldBitmap = SelectObject(buffered, bitmap);
    HBRUSH white = CreateSolidBrush(RGB(236, 242, 248));
    FillRect(buffered, &rect, white); DeleteObject(white);
    const double seconds = source_seconds();
    const int shift = static_cast<int>(seconds * 22) % 64;
    for (int y = -64; y < rect.bottom + 64; y += 64) {
        for (int x = -64; x < rect.right + 64; x += 64) {
            if (((x / 64 + y / 64) & 1) == 0) {
                RECT tile{x + shift, y, x + shift + 64, y + 64};
                HBRUSH brush = CreateSolidBrush(RGB(163, 188, 214));
                FillRect(buffered, &tile, brush); DeleteObject(brush);
            }
        }
    }
    HPEN line = CreatePen(PS_SOLID, 2, RGB(31, 67, 102));
    auto oldPen = SelectObject(buffered, line);
    for (int x = 28; x < rect.right; x += 48) { MoveToEx(buffered, x, 0, nullptr); LineTo(buffered, x, rect.bottom); }
    for (int y = 24; y < rect.bottom; y += 48) { MoveToEx(buffered, 0, y, nullptr); LineTo(buffered, rect.right, y); }
    SelectObject(buffered, oldPen); DeleteObject(line);
    RECT moving{static_cast<LONG>(rect.right * (0.5 + 0.4 * std::sin(seconds))), 0, 0, rect.bottom};
    moving.right = moving.left + 14;
    HBRUSH marker = CreateSolidBrush(RGB(237, 71, 105));
    FillRect(buffered, &moving, marker); DeleteObject(marker);
    SetBkMode(buffered, TRANSPARENT); SetTextColor(buffered, RGB(12, 35, 60));
    HFONT font = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto oldFont = SelectObject(buffered, font);
    RECT label{24, 20, rect.right - 24, 112};
    const std::wstring labelText =
        L"OWNED SOURCE | 48 px grid | refraction " + std::wstring(refractionEnabled ? L"ON" : L"OFF") +
        L" | dispersion " + std::wstring(dispersionEnabled ? L"ON" : L"OFF") +
        L" | " + std::wstring(sourceAnimationEnabled ? L"MOVING" : L"PAUSED") +
        L"\nF2: refraction   F3: dispersion   F4: pause/resume source\nF5: capture border   F7: panel / controls layout   Escape: close";
    DrawTextW(buffered, labelText.c_str(), -1, &label, DT_LEFT | DT_TOP);
    for (int y = 148; y < rect.bottom; y += 144) {
        RECT sample{40, y, rect.right - 40, y + 36};
        DrawTextW(buffered, L"Straight lines bend only near the glass boundary  0123456789", -1, &sample, DT_LEFT | DT_SINGLELINE);
    }
    SelectObject(buffered, oldFont); DeleteObject(font);
    BitBlt(dc, 0, 0, rect.right, rect.bottom, buffered, 0, 0, SRCCOPY);
    SelectObject(buffered, oldBitmap); DeleteObject(bitmap); DeleteDC(buffered);
    EndPaint(window, &paint);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
        if (window == sourceWindow) paint_source(window);
        else { PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint); }
        return 0;
    case WM_TIMER:
        if (window == sourceWindow && !closing) {
            if (sourceAnimationEnabled) InvalidateRect(sourceWindow, nullptr, FALSE);
            UpdateWindow(sourceWindow);
            if (probe) {
                try { probe->update(); }
                catch (hresult_error const& error) { log_error("capture_or_render", error.code()); PostMessageW(window, WM_CLOSE, 0, 0); }
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostMessageW(window, WM_CLOSE, 0, 0);
        if (wp == 'R' || wp == VK_F2) {
            refractionEnabled = !refractionEnabled;
            log("\"event\":\"toggle_refraction\",\"enabled\":" + std::to_string(refractionEnabled) + ",\"virtual_key\":" + std::to_string(wp));
            update_labels();
        }
        if (wp == 'C' || wp == VK_F3) {
            dispersionEnabled = !dispersionEnabled;
            log("\"event\":\"toggle_dispersion\",\"enabled\":" + std::to_string(dispersionEnabled) + ",\"virtual_key\":" + std::to_string(wp));
            update_labels();
        }
        if (wp == VK_F4) {
            const auto now = GetTickCount64();
            if (sourceAnimationEnabled) animationPausedAt = now;
            else animationPausedDuration += now - animationPausedAt;
            sourceAnimationEnabled = !sourceAnimationEnabled;
            log("\"event\":\"toggle_source_animation\",\"enabled\":" + std::to_string(sourceAnimationEnabled) +
                ",\"animation_seconds\":" + std::to_string(source_seconds()));
            update_labels();
        }
        if ((wp == 'B' || wp == VK_F5) && probe) probe->toggle_border();
        if (wp == VK_F7) {
            controlsLayoutEnabled = !controlsLayoutEnabled;
            log("\"event\":\"toggle_controls_layout\",\"enabled\":" + std::to_string(controlsLayoutEnabled));
            update_labels();
        }
        return 0;
    case WM_CLOSE:
        if (!closing) {
            closing = true; KillTimer(sourceWindow, kFrameTimer);
            log("\"event\":\"closed\",\"frames\":" + std::to_string(probe ? probe->frames : 0));
            probe.reset();
            if (IsWindow(glassWindow)) DestroyWindow(glassWindow);
            if (IsWindow(sourceWindow)) DestroyWindow(sourceWindow);
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    started = GetTickCount64();
    wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr, module, MAX_PATH);
    evidence.open(std::filesystem::path(module).parent_path() / "evidence.jsonl", std::ios::trunc);
    if (!evidence) return 2;
    log("\"event\":\"starting\",\"capture_scope\":\"owned_source_window_only\",\"desktop_capture\":false");
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        init_apartment(apartment_type::single_threaded);
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc; wc.hInstance = instance;
        wc.lpszClassName = L"TieZOwnedGlassCaptureQA";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        sourceWindow = CreateWindowExW(0, wc.lpszClassName,
            L"TieZ Glass Capture QA | owned animated source", WS_OVERLAPPEDWINDOW,
            150, 75, 1040, 790, nullptr, nullptr, instance, nullptr);
        glassWindow = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
            L"TieZ Glass Capture QA | WGC optical output | R / C / Esc", WS_OVERLAPPEDWINDOW,
            410, 175, kGlassWidth, kGlassHeight, nullptr, nullptr, instance, nullptr);
        if (!sourceWindow || !glassWindow) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        verify_affinity(sourceWindow, "source");
        verify_affinity(glassWindow, "output");
        update_labels();
        ShowWindow(sourceWindow, SW_SHOWNORMAL); UpdateWindow(sourceWindow);
        ShowWindow(glassWindow, SW_SHOWNORMAL); UpdateWindow(glassWindow);
        probe = std::make_unique<Probe>();
        probe->initialize();
        if (!SetTimer(sourceWindow, kFrameTimer, 16, nullptr)) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        probe.reset();
        log("\"event\":\"shutdown\"");
        uninit_apartment();
        return 0;
    } catch (hresult_error const& error) {
        log_error("initialize", error.code());
        probe.reset();
        if (IsWindow(glassWindow)) DestroyWindow(glassWindow);
        if (IsWindow(sourceWindow)) DestroyWindow(sourceWindow);
        return 1;
    } catch (...) {
        log_error("unexpected", E_FAIL);
        return 1;
    }
}
