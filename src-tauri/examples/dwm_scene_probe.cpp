// Isolated, owned-fixture-only experiment. This is not a production capture backend.
// Private ABI declarations are researched from ADeltaX's original API notes;
// implementation below is original. Display affinity stays zero on every window;
// only the covered helper uses the private shared-scene host's live-preview flag.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dispatcherqueue.h>
#include <windows.ui.composition.interop.h>
#include "probe_gaussian_blur.h"
#include "dwm_scene_probe_vs.generated.h"
#include "dwm_scene_probe_ps.generated.h"
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#ifdef TIEZ_PROBE_ROUNDED
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#endif
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#ifdef TIEZ_PROBE_WEBVIEW
#include "probe_webview.h"
#endif

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
#ifdef TIEZ_PROBE_WEBVIEW
constexpr UINT webviewFailedMessage = WM_APP + 18;
UINT_PTR nextWebViewGeneration = 0;
constexpr ULONGLONG automaticExitMs = 180000;
#else
constexpr ULONGLONG automaticExitMs = 120000;
#endif
constexpr int width = 320, height = 360;
#ifdef TIEZ_PROBE_ROUNDED
constexpr bool roundedRegionEnabled = true;
constexpr int roundedRadiusPx = 24;
#else
constexpr bool roundedRegionEnabled = false;
constexpr int roundedRadiusPx = 0;
#endif
constexpr int fixtureX = 30, fixtureY = 90;
constexpr int outputX = 750;
constexpr int baseWidth = 1100, baseHeight = 490;
int layoutX = 0, layoutY = 0;
HWND base = nullptr, acrylic = nullptr, helper = nullptr, cover = nullptr, output = nullptr;
RECT sourceRect{}, outputRect{}, outputHomeRect{};
ULONGLONG started = 0, animationTick = 0;
bool closing = false, paused = false, mirror = false, acrylicOn = true;
bool controlsOptics = false;
std::ofstream evidence;

void log(const std::string& fields) {
    evidence << "{\"elapsed_ms\":" << GetTickCount64() - started << ',' << fields << "}\n";
    evidence.flush();
}
void hr_log(const char* phase, HRESULT hr) {
    std::ostringstream stream;
    stream << "\"event\":\"api\",\"phase\":\"" << phase << "\",\"hresult\":\"0x"
        << std::hex << static_cast<unsigned long>(hr) << "\"";
    log(stream.str());
}
void require(HRESULT hr, const char* phase) {
    hr_log(phase, hr);
    check_hresult(hr);
}
bool is_own(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}
#ifdef TIEZ_PROBE_ROUNDED
struct ProbeRegion {
    HRGN value;
    explicit ProbeRegion(HRGN region) : value(region) { check_bool(value != nullptr); }
    ProbeRegion(const ProbeRegion&) = delete;
    ProbeRegion& operator=(const ProbeRegion&) = delete;
    ~ProbeRegion() { if (value) DeleteObject(value); }
};
HRGN create_expected_region() {
    // GDI omits the right/bottom outline pixels; retain the full capture extent
    // so horizontal mirroring cannot sample a clipped last column as black.
    return CreateRoundRectRgn(0, 0, width + 1, height + 1, roundedRadiusPx * 2, roundedRadiusPx * 2);
}
void set_rounded_region(HWND hwnd) {
    if (!is_own(hwnd) || IsWindowVisible(hwnd)) throw hresult_error(E_ACCESSDENIED);
    RECT client{};
    check_bool(GetClientRect(hwnd, &client));
    if (client.left != 0 || client.top != 0 || client.right != width || client.bottom != height)
        throw hresult_error(E_BOUNDS);
    ProbeRegion region(create_expected_region());
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowRgn(hwnd, region.value, FALSE)) {
        const DWORD error = GetLastError();
        throw hresult_error(error ? HRESULT_FROM_WIN32(error) : E_FAIL);
    }
    // SetWindowRgn takes ownership only on success; never delete that HRGN.
    region.value = nullptr;
}
void validate_rounded_regions() {
    ProbeRegion expected(create_expected_region());
    RECT bounds{};
    const RECT captureBounds{0, 0, width, height};
    if (GetRgnBox(expected.value, &bounds) == ERROR || !EqualRect(&bounds, &captureBounds))
        throw hresult_error(E_BOUNDS);
    for (const HWND hwnd : {helper, cover, output}) {
        ProbeRegion actual(CreateRectRgn(0, 0, 0, 0));
        const int kind = GetWindowRgn(hwnd, actual.value);
        if (kind == ERROR || !EqualRgn(actual.value, expected.value)) {
            log("\"event\":\"scope_stop\",\"reason\":\"rounded_region_mismatch\",\"window\":\"" +
                std::string(hwnd == helper ? "helper" : hwnd == cover ? "cover" : "output") +
                "\",\"region_kind\":" + std::to_string(kind) +
                ",\"rounded_radius_px\":" + std::to_string(roundedRadiusPx));
            throw hresult_error(E_ACCESSDENIED);
        }
    }
}
#endif
void validate_affinity(HWND hwnd) {
    DWORD value = 0xffffffff;
    const BOOL ok = GetWindowDisplayAffinity(hwnd, &value);
    log("\"event\":\"affinity\",\"owned\":" + std::to_string(is_own(hwnd)) +
        ",\"get_ok\":" + std::to_string(ok != FALSE) + ",\"value\":" + std::to_string(value));
    if (!ok || value != WDA_NONE || !is_own(hwnd)) throw hresult_error(E_ACCESSDENIED);
}

std::string json_string(const std::string& value) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 0x20) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += char(c);
    }
    return result + '"';
}
void log_scope_window(const char* event, HWND hwnd, const RECT& bounds) {
    wchar_t className[256]{};
    const int count = GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const DWORD classError = count ? ERROR_SUCCESS : GetLastError();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::ostringstream stream;
    stream << "\"event\":" << json_string(event) << ",\"class_name\":"
        << json_string(winrt::to_string(hstring(className, static_cast<uint32_t>(count))))
        << ",\"class_read_ok\":" << (count != 0) << ",\"class_read_error\":" << classError
        << ",\"pid\":" << pid << ",\"owned\":" << (pid == GetCurrentProcessId())
        << ",\"visible\":" << (IsWindowVisible(hwnd) != FALSE)
        << ",\"iconic\":" << (IsIconic(hwnd) != FALSE)
        << ",\"style\":" << static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE))
        << ",\"extended_style\":" << static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE))
        << ",\"bounds_relative_to_source\":[" << static_cast<long long>(bounds.left) - sourceRect.left
        << ',' << static_cast<long long>(bounds.top) - sourceRect.top
        << ',' << static_cast<long long>(bounds.right) - sourceRect.left
        << ',' << static_cast<long long>(bounds.bottom) - sourceRect.top
        << "],\"source_rect\":[" << sourceRect.left << ',' << sourceRect.top
        << ',' << sourceRect.right << ',' << sourceRect.bottom << ']';
    log(stream.str());
}
bool is_capture_cursor_overlay(HWND hwnd) {
    wchar_t className[256]{};
    const int count = GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    constexpr DWORD requiredStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    return count > 0 && std::wstring(className, static_cast<size_t>(count)) == L"CodexComputerUseCursorOverlay" &&
        (style & requiredStyle) == requiredStyle;
}

// Enumerates identifiers/geometry and blocker class names, never titles, text or pixels.
// The private API's include list is reported ineffective; do not use it as isolation.
struct Scope {
    std::vector<HWND> exclude;
    bool reachedBase = false;
    bool reachedOutput = false;
    bool reachedCover = false;
    bool outputAboveCover = false;
    bool coverAboveHelper = false;
    bool foreignAboveSource = false;
};
BOOL CALLBACK enumerate(HWND hwnd, LPARAM parameter) {
    auto& scope = *reinterpret_cast<Scope*>(parameter);
    if (hwnd == base) scope.reachedBase = true;
    if (hwnd == output) scope.reachedOutput = true;
    if (hwnd == cover) {
        scope.reachedCover = true;
        scope.outputAboveCover = scope.reachedOutput;
    }
    if (hwnd == helper) scope.coverAboveHelper = scope.reachedCover;
    if (!is_own(hwnd)) {
        scope.exclude.push_back(hwnd);
        if (!scope.reachedBase && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
            RECT bounds{}, intersection{};
            if (GetWindowRect(hwnd, &bounds) && IntersectRect(&intersection, &bounds, &sourceRect)) {
                DWORD cloaked = 0;
                DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
                if (!cloaked) {
                    // This observed transparent QA marker remains excluded from
                    // the private scene; it is not an external application pane.
                    if (is_capture_cursor_overlay(hwnd)) {
                        static std::vector<HWND> reported;
                        if (std::find(reported.begin(), reported.end(), hwnd) == reported.end()) {
                            reported.push_back(hwnd);
                            log_scope_window("scope_excluded_cursor_overlay", hwnd, bounds);
                        }
                    } else {
                        scope.foreignAboveSource = true;
                        log_scope_window("scope_blocker", hwnd, bounds);
                    }
                }
            }
        }
    }
    return TRUE;
}
Scope ensure_scope() {
    if (!IsWindowVisible(base) || IsIconic(base)) throw hresult_error(E_ACCESSDENIED);
    RECT bounds{};
    check_bool(GetWindowRect(base, &bounds));
    if (bounds.left > sourceRect.left || bounds.top > sourceRect.top ||
        bounds.right < sourceRect.right || bounds.bottom < sourceRect.bottom ||
        (GetWindowLongPtrW(base, GWL_EXSTYLE) & WS_EX_LAYERED)) throw hresult_error(E_ACCESSDENIED);
    const DWORD helperStyle = static_cast<DWORD>(GetWindowLongPtrW(helper, GWL_EXSTYLE));
    const DWORD coverStyle = static_cast<DWORD>(GetWindowLongPtrW(cover, GWL_EXSTYLE));
    if (!(helperStyle & WS_EX_TOOLWINDOW) || !(coverStyle & WS_EX_TOOLWINDOW)) {
        log("\"event\":\"scope_stop\",\"reason\":\"helper_cover_toolwindow_missing\",\"helper_extended_style\":" +
            std::to_string(helperStyle) + ",\"cover_extended_style\":" + std::to_string(coverStyle));
        throw hresult_error(E_ACCESSDENIED);
    }
    RECT helperBounds{}, coverBounds{}, outputBounds{};
    if (!is_own(helper) || !is_own(cover) || !is_own(output) ||
        !IsWindowVisible(helper) || !IsWindowVisible(cover) || !IsWindowVisible(output) ||
        IsIconic(helper) || IsIconic(cover) || IsIconic(output) ||
        GetWindow(helper, GW_OWNER) != nullptr || GetWindow(output, GW_OWNER) != nullptr ||
        GetWindow(cover, GW_OWNER) != helper ||
        (GetWindowLongPtrW(cover, GWL_EXSTYLE) & WS_EX_LAYERED) ||
        !GetWindowRect(helper, &helperBounds) || !GetWindowRect(cover, &coverBounds) ||
        !GetWindowRect(output, &outputBounds) || !EqualRect(&helperBounds, &outputRect) ||
        !EqualRect(&coverBounds, &outputRect) || !EqualRect(&outputBounds, &outputRect)) {
        log("\"event\":\"scope_stop\",\"reason\":\"coincident_owned_group_invalid\"");
        throw hresult_error(E_ACCESSDENIED);
    }
#ifdef TIEZ_PROBE_ROUNDED
    validate_rounded_regions();
#endif
    Scope scope;
    check_bool(EnumWindows(enumerate, reinterpret_cast<LPARAM>(&scope)));
    if (!scope.outputAboveCover || !scope.coverAboveHelper) {
        log("\"event\":\"scope_stop\",\"reason\":\"output_cover_helper_order_invalid\"");
        throw hresult_error(E_ACCESSDENIED);
    }
    if (!scope.reachedBase || scope.foreignAboveSource) {
        log("\"event\":\"scope_stop\",\"reason\":\"foreign_window_above_owned_fixture\",\"reached_base\":" +
            std::to_string(scope.reachedBase) + ",\"foreign_above_source\":" + std::to_string(scope.foreignAboveSource) +
            ",\"source_rect\":[" + std::to_string(sourceRect.left) + ',' + std::to_string(sourceRect.top) + ',' +
            std::to_string(sourceRect.right) + ',' + std::to_string(sourceRect.bottom) + ']');
        throw hresult_error(E_ACCESSDENIED);
    }
    scope.exclude.push_back(helper);
    scope.exclude.push_back(cover);
    scope.exclude.push_back(output);
    return scope;
}

// Windows 11 variant only. Older ordinal signatures are deliberately unsupported.
using CreateScene = HRESULT(WINAPI*)(HWND, void*, void**, HTHUMBNAIL*);
using UpdateScene = HRESULT(WINAPI*)(HTHUMBNAIL, HWND*, DWORD, HWND*, DWORD, RECT*, SIZE*, DWORD);

void set_helper_livepreview(bool enabled) {
    if (!is_own(helper)) throw hresult_error(E_ACCESSDENIED);
    struct AttributeData { int attribute; void* data; SIZE_T size; };
    using CompositionAttribute = BOOL(WINAPI*)(HWND, AttributeData*);
    const HMODULE user = GetModuleHandleW(L"user32.dll");
    const auto set = reinterpret_cast<CompositionAttribute>(GetProcAddress(user, "SetWindowCompositionAttribute"));
    const auto get = reinterpret_cast<CompositionAttribute>(GetProcAddress(user, "GetWindowCompositionAttribute"));
    if (!set) throw hresult_error(E_NOINTERFACE);
    BOOL value = enabled ? TRUE : FALSE;
    AttributeData data{0xD, &value, sizeof(value)};
    SetLastError(ERROR_SUCCESS);
    const BOOL setOk = set(helper, &data);
    const DWORD error = setOk ? ERROR_SUCCESS : GetLastError();
    value = FALSE;
    const BOOL getOk = get ? get(helper, &data) : FALSE;
    log("\"event\":\"helper_livepreview\",\"owned_helper_only\":true,\"requested\":" +
        std::to_string(enabled) + ",\"set_ok\":" + std::to_string(setOk != FALSE) +
        ",\"set_error\":" + std::to_string(error) + ",\"get_ok\":" +
        std::to_string(getOk != FALSE) + ",\"readback\":" + std::to_string(value != FALSE));
    if (!setOk || (getOk && bool(value) != enabled)) throw hresult_error(E_FAIL);
}

constexpr char shader[] = R"hlsl(
#include "glass_probe_layout.hlsli"
Texture2D<float4> scene : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Options : register(b0) { uint mirrorX; uint opticalControls; float2 sceneSize; };
struct Vertex { float4 position : SV_POSITION; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v;
    float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float4 sampleScene(float2 position) {
    if (mirrorX != 0) position.x = sceneSize.x - position.x;
    return scene.Sample(linearClamp, position / sceneSize);
}
float4 PS(Vertex input) : SV_TARGET {
    int2 p = int2(input.position.xy);
    if (mirrorX != 0) p.x = int(sceneSize.x) - 1 - p.x;
    float3 original = scene.Load(int3(p, 0)).rgb;
    if (opticalControls == 0) {
        return float4(original, 1.0);
    }
    float2 pixel = input.position.xy;
    GlassOptics optics = GlassProbeEvaluate(pixel, sceneSize, 1.0);
    if (optics.coverage <= 0.0) return float4(original, 1.0);
    float4 green = sampleScene(pixel + optics.offsetG);
    float3 color;
    color.r = sampleScene(pixel + optics.offsetR).r;
    color.g = green.g;
    color.b = sampleScene(pixel + optics.offsetB).b;
    // Preserve the full opaque scene and apply the shared optical coverage once.
    color = min(saturate(color + optics.highlight * green.a), green.a.xxx);
    return float4(lerp(original, color, optics.coverage), 1.0);
}
)hlsl";

struct ShaderOptions {
    UINT mirrorX;
    UINT opticalControls;
    float sceneSize[2];
};
static_assert(sizeof(ShaderOptions) == 16);

void destroy_group_window(HWND& hwnd, const char* phase) {
    if (IsWindow(hwnd) && !DestroyWindow(hwnd)) {
        hr_log(phase, HRESULT_FROM_WIN32(GetLastError()));
    }
    if (!IsWindow(hwnd)) hwnd = nullptr;
}
void destroy_helper_group() {
    // The owned cover is destroyed with its helper owner, never removed first.
    destroy_group_window(helper, "destroy_helper");
    destroy_group_window(cover, "destroy_cover");
}

struct Probe {
#ifdef TIEZ_PROBE_WEBVIEW
    const UINT_PTR webViewGeneration = ++nextWebViewGeneration;
    std::unique_ptr<ProbeWebView> foreground;
#endif
    HMODULE dwmModule = nullptr;
    HTHUMBNAIL thumbnail = nullptr;
    CreateScene createScene = nullptr;
    UpdateScene updateScene = nullptr;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDCompositionDesktopDevice> composition;
    com_ptr<IDCompositionTarget> helperTarget, outputTarget;
    com_ptr<IDCompositionVisual2> shared;
    com_ptr<IDCompositionVisual2> outputVisual;
    com_ptr<IDXGISwapChain1> swap;
    com_ptr<ID3D11Texture2D> retained, readback, outputReadback;
    com_ptr<ID3D11ShaderResourceView> view;
    com_ptr<ID3D11VertexShader> vertex;
    com_ptr<ID3D11PixelShader> pixel;
    com_ptr<ID3D11SamplerState> sampler;
    com_ptr<ID3D11Buffer> options;
    IDirect3DDevice captureDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    unsigned long long frames = 0, validated = 0, outputAlphaChecks = 0;
    ULONGLONG lastReport = 0, lastFrame = 0, captureStarted = 0, lastAlphaReport = 0;
    bool valid = false;
    bool renderDirty = false;
    bool helperPreviewExcluded = false;
    std::vector<HWND> configuredExclusions;
    bool sceneConfigured = false;
    ULONGLONG lastValidFrame = 0;

    void check_output_alpha(ID3D11Texture2D* candidate, const char* phase) {
        context->CopyResource(outputReadback.get(), candidate);
        D3D11_MAPPED_SUBRESOURCE data{};
        check_hresult(context->Map(outputReadback.get(), 0, D3D11_MAP_READ, 0, &data));
        unsigned int nonOpaque = 0, minimum = 255, maximum = 0;
        for (int y = 0; y < height; ++y) {
            const auto* row = static_cast<const unsigned char*>(data.pData) + data.RowPitch * y;
            for (int x = 0; x < width; ++x) {
                const unsigned int alpha = row[x * 4 + 3];
                minimum = std::min(minimum, alpha);
                maximum = std::max(maximum, alpha);
                if (alpha != 255) ++nonOpaque;
            }
        }
        context->Unmap(outputReadback.get(), 0);
        ++outputAlphaChecks;
        const ULONGLONG now = GetTickCount64();
        if (nonOpaque || outputAlphaChecks <= 2 || now - lastAlphaReport >= 1000) {
            lastAlphaReport = now;
            log("\"event\":\"output_alpha_readback\",\"phase\":" + json_string(phase) +
                ",\"checks\":" + std::to_string(outputAlphaChecks) + ",\"pixels_checked\":" +
                std::to_string(width * height) + ",\"non_opaque_pixels\":" + std::to_string(nonOpaque) +
                ",\"minimum_alpha\":" + std::to_string(minimum) + ",\"maximum_alpha\":" +
                std::to_string(maximum) + ",\"controls_optics\":" + std::to_string(controlsOptics) +
                ",\"source\":\"actual_swapchain_backbuffer\",\"before_present\":true");
        }
        if (nonOpaque) throw hresult_error(E_FAIL);
    }

    void update_scene() {
        auto scope = ensure_scope();
        std::sort(scope.exclude.begin(), scope.exclude.end(), [](HWND a, HWND b) {
            return reinterpret_cast<UINT_PTR>(a) < reinterpret_cast<UINT_PTR>(b);
        });
        if (sceneConfigured && scope.exclude == configuredExclusions) return;
        RECT crop = sourceRect;
        SIZE size{width, height};
        const HRESULT hr = updateScene(thumbnail, nullptr, 0, scope.exclude.data(),
            static_cast<DWORD>(scope.exclude.size()), &crop, &size, 1);
        if (FAILED(hr)) require(hr, "update_shared_scene");
        if (!EqualRect(&crop, &sourceRect) || size.cx != width || size.cy != height) {
            log("\"event\":\"scope_stop\",\"reason\":\"private_api_changed_crop\"");
            throw hresult_error(E_ACCESSDENIED);
        }
        configuredExclusions = std::move(scope.exclude);
        sceneConfigured = true;
        log("\"event\":\"scene_configuration_updated\",\"excluded_windows\":" + std::to_string(configuredExclusions.size()));
    }

    void toggle_output_position() {
        ensure_scope();
        const bool overSource = !EqualRect(&outputRect, &sourceRect);
        const RECT destination = overSource ? sourceRect : outputHomeRect;
        constexpr UINT positionFlags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW;
        HDWP position = BeginDeferWindowPos(3);
        check_bool(position != nullptr);
        position = DeferWindowPos(position, helper, output, destination.left, destination.top,
            width, height, positionFlags);
        check_bool(position != nullptr);
        position = DeferWindowPos(position, cover, output, destination.left, destination.top,
            width, height, positionFlags);
        check_bool(position != nullptr);
        position = DeferWindowPos(position, output, HWND_TOPMOST, destination.left, destination.top,
            width, height, positionFlags);
        check_bool(position != nullptr);
        check_bool(EndDeferWindowPos(position));
        outputRect = destination;
#ifdef TIEZ_PROBE_WEBVIEW
        if (foreground) require(foreground->NotifyParentMoved(), "webview_parent_moved");
#endif
        require(DwmFlush(), "moved_group_dwm_flush");
        ensure_scope();
        // The source crop and all exclusion rules remain fixed; only the owned
        // output group moves. Refresh even when the HWND exclusion set is equal.
        sceneConfigured = false;
        update_scene();
        renderDirty = true;
        log("\"event\":\"output_group_moved\",\"over_source\":" + std::to_string(overSource) +
            ",\"output_rect\":[" + std::to_string(outputRect.left) + ',' + std::to_string(outputRect.top) + ',' +
            std::to_string(outputRect.right) + ',' + std::to_string(outputRect.bottom) +
            "],\"source_rect_unchanged\":true,\"rectangles_equal\":true,\"z_order\":\"output>cover>helper\","
            "\"scene_refresh_completed\":true,\"frames\":" + std::to_string(frames) +
            ",\"validated\":" + std::to_string(validated));
        InvalidateRect(base, nullptr, FALSE);
    }

    void initialize() {
        dwmModule = LoadLibraryW(L"dwmapi.dll");
        check_bool(dwmModule != nullptr);
        createScene = reinterpret_cast<CreateScene>(GetProcAddress(dwmModule, MAKEINTRESOURCEA(163)));
        updateScene = reinterpret_cast<UpdateScene>(GetProcAddress(dwmModule, MAKEINTRESOURCEA(164)));
        if (!createScene || !updateScene) throw hresult_error(E_NOINTERFACE);
        require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.put(), nullptr, context.put()), "d3d_device");
        auto dxgi = device.as<IDXGIDevice>();
        require(DCompositionCreateDevice3(dxgi.get(), __uuidof(IDCompositionDesktopDevice),
            composition.put_void()), "dcomp_device");
        com_ptr<IDXGIAdapter> adapter;
        require(dxgi->GetAdapter(adapter.put()), "adapter");
        com_ptr<IDXGIFactory2> factory;
        require(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()), "factory");
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = width; swapDesc.Height = height;
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1; swapDesc.BufferCount = 2;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        require(factory->CreateSwapChainForComposition(device.get(), &swapDesc, nullptr, swap.put()), "output_swap");
#ifdef TIEZ_PROBE_WEBVIEW
        require(composition->CreateTargetForHwnd(output, FALSE, outputTarget.put()), "output_target_below_children");
#else
        require(composition->CreateTargetForHwnd(output, TRUE, outputTarget.put()), "output_target");
#endif
        require(composition->CreateVisual(outputVisual.put()), "output_visual");
        require(outputVisual->SetContent(swap.get()), "output_content");
        require(outputTarget->SetRoot(outputVisual.get()), "output_root");
        require(composition->Commit(), "output_commit");
        require(device->CreateVertexShader(kDwmSceneProbeVS, sizeof(kDwmSceneProbeVS), nullptr, vertex.put()), "vertex_shader");
        require(device->CreatePixelShader(kDwmSceneProbePS, sizeof(kDwmSceneProbePS), nullptr, pixel.put()), "pixel_shader");
        log("\"event\":\"shader_loaded\",\"embedded_bytecode\":true,\"runtime_shader_file_access\":false,\"optics\":\"shared_glass_optics\"");
        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        require(device->CreateSamplerState(&sampling, sampler.put()), "linear_sampler");
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(ShaderOptions); constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        require(device->CreateBuffer(&constants, nullptr, options.put()), "constant_buffer");
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = width; texture.Height = height; texture.MipLevels = 1; texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM; texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT; texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        require(device->CreateTexture2D(&texture, nullptr, retained.put()), "sampleable_texture");
        require(device->CreateShaderResourceView(retained.get(), nullptr, view.put()), "scene_srv");
        texture.Usage = D3D11_USAGE_STAGING; texture.BindFlags = 0; texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        require(device->CreateTexture2D(&texture, nullptr, readback.put()), "sentinel_readback");
        require(device->CreateTexture2D(&texture, nullptr, outputReadback.put()), "output_alpha_readback_texture");

        // The output stays hidden until an opaque swapchain placeholder is ready.
        // The helper has no private content while this first shield is installed.
        com_ptr<ID3D11Texture2D> placeholder;
        require(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), placeholder.put_void()), "placeholder_buffer");
        com_ptr<ID3D11RenderTargetView> placeholderTarget;
        require(device->CreateRenderTargetView(placeholder.get(), nullptr, placeholderTarget.put()), "placeholder_target");
        const float placeholderColor[]{25.0f / 255.0f, 30.0f / 255.0f, 40.0f / 255.0f, 1.0f};
        context->ClearRenderTargetView(placeholderTarget.get(), placeholderColor);
        check_output_alpha(placeholder.get(), "opaque_placeholder");
        require(swap->Present(1, 0), "placeholder_present");
        require(composition->Commit(), "placeholder_commit");
        require(composition->WaitForCommitCompletion(), "placeholder_commit_completed");
        constexpr UINT positionFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW;
        check_bool(SetWindowPos(output, HWND_TOPMOST, 0, 0, 0, 0, positionFlags));
        require(DwmFlush(), "visible_placeholder_dwm_flush");
        // Both hidden windows enter below the already visible opaque output.
        HDWP position = BeginDeferWindowPos(3);
        check_bool(position != nullptr);
        position = DeferWindowPos(position, helper, output, 0, 0, 0, 0, positionFlags);
        check_bool(position != nullptr);
        position = DeferWindowPos(position, cover, output, 0, 0, 0, 0, positionFlags);
        check_bool(position != nullptr);
        position = DeferWindowPos(position, output, HWND_TOPMOST, 0, 0, 0, 0, positionFlags);
        check_bool(position != nullptr);
        check_bool(EndDeferWindowPos(position));
        UpdateWindow(cover);
        require(DwmFlush(), "opaque_shields_dwm_flush");
        ensure_scope();
        log("\"event\":\"coincident_group_ready\",\"rectangles_equal\":true,\"z_order\":\"output>cover>helper\","
            "\"output_owner\":\"none\",\"helper_owner\":\"none\",\"cover_owner\":\"helper\","
            "\"opaque_placeholder_verified\":true,\"private_scene_attached\":false,\"helper_extended_style\":" +
            std::to_string(static_cast<DWORD>(GetWindowLongPtrW(helper, GWL_EXSTYLE))) +
            ",\"cover_extended_style\":" + std::to_string(static_cast<DWORD>(GetWindowLongPtrW(cover, GWL_EXSTYLE))) +
            ",\"rounded_region\":" + std::to_string(roundedRegionEnabled) +
            ",\"rounded_radius_px\":" + std::to_string(roundedRadiusPx));

        // Only the covered helper needs the shared-scene host's private flag.
        set_helper_livepreview(true);
        helperPreviewExcluded = true;
        require(createScene(helper, composition.get(), shared.put_void(), &thumbnail), "create_shared_scene");
        update_scene();
        const D2D_RECT_F clip{0, 0, static_cast<float>(width), static_cast<float>(height)};
        require(shared->SetClip(clip), "scene_clip");
        require(composition->CreateTargetForHwnd(helper, TRUE, helperTarget.put()), "helper_target");
        ensure_scope();
        require(helperTarget->SetRoot(shared.get()), "helper_root");
        require(composition->Commit(), "scene_commit");
        require(DwmFlush(), "initial_dwm_flush");

        com_ptr<IInspectable> inspectable;
        require(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()), "winrt_device");
        captureDevice = inspectable.as<IDirect3DDevice>();
        const auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        require(interop->CreateForWindow(helper, guid_of<GraphicsCaptureItem>(), put_abi(item)), "capture_owned_helper");
        const auto size = item.Size();
        if (size.Width != width || size.Height != height) throw hresult_error(E_INVALIDARG);
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(captureDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
#ifdef TIEZ_PROBE_ROUNDED
        // Querying existing capability access cannot prompt. Never request new
        // access here; a session preference cannot establish a Borderless grant.
        try {
            using namespace winrt::Windows::Security::Authorization::AppCapabilityAccess;
            const auto access = AppCapability::Create(L"graphicsCaptureWithoutBorder").CheckAccess();
            log("\"event\":\"borderless_existing_access\",\"status\":" +
                std::to_string(static_cast<int>(access)) + ",\"permission_prompt_requested\":false");
            if (access == AppCapabilityAccessStatus::Allowed) {
                session.IsBorderRequired(false);
                log("\"event\":\"borderless_session_preference\",\"requested\":true,\"visual_result_unverified\":true");
            }
        } catch (const hresult_error& error) {
            hr_log("borderless_existing_access_or_preference", error.code());
        }
#endif
        // No GraphicsCaptureAccess.RequestAccessAsync or system setting changes.
        ensure_scope();
        session.StartCapture();
        captureStarted = GetTickCount64();
        log("\"event\":\"capture_started\",\"target\":\"owned_helper\",\"expected_width\":320,\"expected_height\":360,\"helper_covered\":true,\"helper_livepreview_excluded\":true,\"output_capture_exclusion_attributes\":false");
#ifdef TIEZ_PROBE_WEBVIEW
        wchar_t executable[MAX_PATH]{};
        check_bool(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0);
        const auto profile = std::filesystem::path(executable).parent_path() / L"webview-profile";
        foreground = std::make_unique<ProbeWebView>();
        ProbeWebView::Callbacks callbacks;
        callbacks.ready = [] { log("\"event\":\"webview_ready\",\"local_document\":true,\"background_alpha\":0,\"shader_below_children\":true"); };
        callbacks.error = [generation = webViewGeneration](const char* phase, HRESULT hr) {
            hr_log(phase, hr);
            PostMessageW(base, webviewFailedMessage, generation, 0);
        };
        callbacks.accelerator = [](UINT key, LPARAM keyData) {
            PostMessageW(base, WM_KEYDOWN, key, keyData);
        };
        require(foreground->Initialize(output, RECT{0, 0, width, height}, profile.wstring(),
            std::move(callbacks)), "webview_initialize");
        log("\"event\":\"webview_requested\",\"isolated_profile\":true");
#endif
    }

    bool sentinels_match(ID3D11Texture2D* candidate) {
        context->CopyResource(readback.get(), candidate);
        D3D11_MAPPED_SUBRESOURCE data{};
        check_hresult(context->Map(readback.get(), 0, D3D11_MAP_READ, 0, &data));
        const std::array<POINT, 4> locations{{{10, 10}, {width - 10, 10}, {10, height - 10}, {width - 10, height - 10}}};
        const std::array<COLORREF, 4> colors{{RGB(232, 44, 88), RGB(36, 210, 132), RGB(44, 104, 232), RGB(236, 192, 40)}};
        bool matches = true;
        std::ostringstream sampleLog;
        sampleLog << "\"event\":\"sentinel_samples\",\"rgba\":[";
        for (size_t i = 0; i < locations.size(); ++i) {
            const auto* bytes = static_cast<const unsigned char*>(data.pData) +
                data.RowPitch * locations[i].y + 4 * locations[i].x;
            const int error = std::abs(int(bytes[2]) - int(GetRValue(colors[i]))) +
                std::abs(int(bytes[1]) - int(GetGValue(colors[i]))) +
                std::abs(int(bytes[0]) - int(GetBValue(colors[i])));
            matches = matches && error < 60 && bytes[3] > 240;
            if (i) sampleLog << ',';
            sampleLog << '[' << int(bytes[2]) << ',' << int(bytes[1]) << ',' << int(bytes[0]) << ',' << int(bytes[3]) << ']';
        }
        context->Unmap(readback.get(), 0);
        if (!matches) log(sampleLog.str() + ']');
        return matches;
    }

    void tick() {
        update_scene();
        auto frame = pool.TryGetNextFrame();
        if (frame) {
            const auto size = frame.ContentSize();
            if (size.Width != width || size.Height != height) throw hresult_error(E_INVALIDARG);
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            com_ptr<ID3D11Texture2D> captured;
            check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), captured.put_void()));
            D3D11_TEXTURE2D_DESC desc{};
            captured->GetDesc(&desc);
            if (desc.Width != width || desc.Height != height || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) throw hresult_error(E_INVALIDARG);
            ensure_scope();
            ++frames; lastFrame = GetTickCount64();
            const bool candidateValid = sentinels_match(captured.get());
            if (!candidateValid) {
                log("\"event\":\"sentinel_failure\",\"frame\":" + std::to_string(frames) + ",\"presented\":false");
                // Never replace the verified cache with an incomplete frame.
                if ((lastValidFrame && lastFrame - lastValidFrame > 1000) ||
                    (!lastValidFrame && lastFrame - captureStarted > 5000)) throw hresult_error(E_FAIL);
                return;
            }
            context->CopyResource(retained.get(), captured.get());
            valid = true;
            lastValidFrame = lastFrame;
            ++validated;
            renderDirty = true;
        } else if (!frames && GetTickCount64() - captureStarted > 8000) {
            throw hresult_error(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        }
        // A frozen source need not deliver another WGC frame when only the
        // shader changes. Reuse a verified texture without accepting new pixels.
        if (!valid || !renderDirty) return;
        ensure_scope();
        const ShaderOptions flags{mirror ? 1u : 0u, controlsOptics ? 1u : 0u, {float(width), float(height)}};
        context->UpdateSubresource(options.get(), 0, nullptr, &flags, 0, 0);
        com_ptr<ID3D11Texture2D> targetTexture;
        check_hresult(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), targetTexture.put_void()));
        com_ptr<ID3D11RenderTargetView> rtv;
        check_hresult(device->CreateRenderTargetView(targetTexture.get(), nullptr, rtv.put()));
        ID3D11RenderTargetView* rawTarget = rtv.get();
        ID3D11ShaderResourceView* rawView = view.get();
        ID3D11Buffer* rawOptions = options.get();
        context->OMSetRenderTargets(1, &rawTarget, nullptr);
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.get(), nullptr, 0);
        context->PSSetShader(pixel.get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &rawView);
        context->PSSetConstantBuffers(0, 1, &rawOptions);
        ID3D11SamplerState* rawSampler = sampler.get();
        context->PSSetSamplers(0, 1, &rawSampler);
        context->Draw(3, 0);
        rawView = nullptr;
        context->PSSetShaderResources(0, 1, &rawView);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        check_output_alpha(targetTexture.get(), "validated_scene");
        check_hresult(swap->Present(1, 0));
        renderDirty = false;
        const ULONGLONG now = GetTickCount64();
        if (!frame || validated == 1 || now - lastReport >= 1000) {
            lastReport = now;
            log("\"event\":\"valid_frame\",\"frames\":" + std::to_string(frames) +
                ",\"validated\":" + std::to_string(validated) + ",\"mirror\":" + std::to_string(mirror) +
                ",\"acrylic\":" + std::to_string(acrylicOn) + ",\"cached_texture\":" +
                std::to_string(!frame) + ",\"controls_optics\":" + std::to_string(controlsOptics) +
                ",\"output_over_source\":" + std::to_string(EqualRect(&outputRect, &sourceRect) != FALSE) +
                ",\"sampleable\":true");
        }
    }

    ~Probe() {
#ifdef TIEZ_PROBE_WEBVIEW
        if (foreground) { foreground->Close(); foreground.reset(); }
#endif
        try { if (session) session.Close(); } catch (...) { hr_log("close_capture_session", E_FAIL); }
        try { if (pool) pool.Close(); } catch (...) { hr_log("close_capture_pool", E_FAIL); }
        log("\"event\":\"capture_stopped\"");
        if (helperTarget) helperTarget->SetRoot(nullptr);
        if (composition) {
            hr_log("detach_helper_commit", composition->Commit());
            hr_log("detach_helper_completed", composition->WaitForCommitCompletion());
        }
        shared = nullptr;
        if (thumbnail) { hr_log("unregister_scene", DwmUnregisterThumbnail(thumbnail)); thumbnail = nullptr; }
        if (helperPreviewExcluded) {
            try { set_helper_livepreview(false); } catch (...) { hr_log("helper_livepreview_restore", E_FAIL); }
        }
        helperTarget = nullptr;
        destroy_helper_group();
        log("\"event\":\"helper_group_destroyed\",\"helper_absent\":" + std::to_string(!IsWindow(helper)) +
            ",\"cover_absent\":" + std::to_string(!IsWindow(cover)) + ",\"output_still_visible\":" +
            std::to_string(IsWindowVisible(output) != FALSE));
        if (outputTarget) outputTarget->SetRoot(nullptr);
        if (composition) composition->Commit();
        if (dwmModule) FreeLibrary(dwmModule);
        log("\"event\":\"capture_closed\",\"frames\":" + std::to_string(frames) + ",\"validated\":" +
            std::to_string(validated) + ",\"output_alpha_checks\":" + std::to_string(outputAlphaChecks));
    }
};
std::unique_ptr<Probe> probe;

void fill(HDC dc, RECT rectangle, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rectangle, brush);
    DeleteObject(brush);
}
void label(HDC dc, int x, int y, const wchar_t* text) {
    TextOutW(dc, x, y, text, lstrlenW(text));
}
void paint_base(HDC destination) {
    RECT client{}; GetClientRect(base, &client);
    HDC dc = CreateCompatibleDC(destination);
    HBITMAP bitmap = CreateCompatibleBitmap(destination, client.right, client.bottom);
    check_bool(dc && bitmap);
    const HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    fill(dc, client, RGB(25, 30, 40));
    const int saved = SaveDC(dc);
    SetViewportOrgEx(dc, layoutX, layoutY, nullptr);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(240, 244, 250));
    label(dc, 30, 20, L"TieZ owned scene | F1 Start / stop | F2 Blur | F3 Mirror | F4 Freeze | F5 Controls | F6 Move A / C | Esc closes");
    label(dc, fixtureX, 60, L"A: actual native scene");
    label(dc, outputX, 60, L"C: WGC texture + custom shader");
    label(dc, 390, 110, probe ? L"F1: stop the output group" : L"F1: start the output group");
    label(dc, 390, 140, probe && EqualRect(&outputRect, &sourceRect) ?
        L"Output group overlaps A; source crop fixed" : L"F6: move the active output group to A");
    const int phase = int(animationTick % 96);
    for (int y = 0; y < height; y += 24) {
        for (int x = 0; x < width; x += 24) {
            const auto color = ((x / 24 + y / 24) % 2) ? RGB(236, 144, 62) : RGB(40, 112, 190);
            fill(dc, RECT{fixtureX + x, fixtureY + y, fixtureX + std::min(x + 24, width), fixtureY + std::min(y + 24, height)}, color);
        }
    }
    fill(dc, RECT{fixtureX + 25 + phase, fixtureY + 35, fixtureX + 45 + phase, fixtureY + height - 35}, RGB(245, 247, 250));
    fill(dc, RECT{fixtureX, fixtureY, fixtureX + 22, fixtureY + 22}, RGB(232, 44, 88));
    fill(dc, RECT{fixtureX + width - 22, fixtureY, fixtureX + width, fixtureY + 22}, RGB(36, 210, 132));
    fill(dc, RECT{fixtureX, fixtureY + height - 22, fixtureX + 22, fixtureY + height}, RGB(44, 104, 232));
    fill(dc, RECT{fixtureX + width - 22, fixtureY + height - 22, fixtureX + width, fixtureY + height}, RGB(236, 192, 40));
    const auto timeoutLabel = L"Owned full-screen fixture | Escape closes immediately | Automatic exit after " +
        std::to_wstring(automaticExitMs / 1000) + L" seconds";
    label(dc, 30, 463, timeoutLabel.c_str());
    RestoreDC(dc, saved);
    BitBlt(destination, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}
struct SourceBackdrop {
    winrt::Windows::System::DispatcherQueueController queue{nullptr};
    winrt::Windows::UI::Composition::Compositor compositor{nullptr};
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};
    winrt::Windows::UI::Composition::CompositionEffectBrush brush{nullptr};
    winrt::Windows::UI::Composition::CompositionGraphicsDevice graphics{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface labels{nullptr};
    com_ptr<ID3D11Device> labelDevice;
    com_ptr<ID2D1Device> labelDrawingDevice;

    void initialize(HWND hwnd) {
        if (!is_own(hwnd) || hwnd != acrylic) throw hresult_error(E_ACCESSDENIED);
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
            require(CreateDispatcherQueueController(options,
                reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(put_abi(queue))), "source_dispatcher_queue");
        }
        const BOOL enabled = TRUE;
        require(DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enabled, sizeof(enabled)), "source_host_backdrop_enabled");
        compositor = winrt::Windows::UI::Composition::Compositor();
        hr_log("source_compositor", S_OK);
        auto desktop = compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        require(desktop->CreateDesktopWindowTarget(hwnd, FALSE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(put_abi(target))), "source_desktop_target");
        auto source = winrt::Windows::UI::Composition::CompositionEffectSourceParameter(L"Backdrop");
        auto implementation = Microsoft::WRL::Make<ProbeGaussianBlur>(
            source.as<winrt::Windows::Graphics::Effects::IGraphicsEffectSource>());
        if (!implementation) throw hresult_error(E_OUTOFMEMORY);
        winrt::Windows::Graphics::Effects::IGraphicsEffect effect{nullptr};
        require(implementation.CopyTo(guid_of<winrt::Windows::Graphics::Effects::IGraphicsEffect>(), put_abi(effect)), "source_effect_description");
        brush = compositor.CreateEffectFactory(effect, {L"Blur.BlurAmount"}).CreateBrush();
        brush.SetSourceParameter(L"Backdrop", compositor.CreateBackdropBrush());
        hr_log("source_gaussian_backdrop_brush", S_OK);
        auto background = compositor.CreateSpriteVisual();
        background.RelativeSizeAdjustment({1.0f, 1.0f});
        background.Brush(brush);
        auto root = compositor.CreateContainerVisual();
        root.RelativeSizeAdjustment({1.0f, 1.0f});
        root.Children().InsertAtBottom(background);

        // Draw labels into a separate transparent surface above the backdrop.
        // This HWND has no GDI redirection bitmap or opaque client fill.
        require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            labelDevice.put(), nullptr, nullptr), "source_label_d3d_device");
        auto dxgi = labelDevice.as<IDXGIDevice>();
        require(D2D1CreateDevice(dxgi.get(), nullptr, labelDrawingDevice.put()), "source_label_d2d_device");
        auto interop = compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        require(interop->CreateGraphicsDevice(labelDrawingDevice.get(),
            reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(put_abi(graphics))), "source_label_graphics_device");
        RECT bounds{}; check_bool(GetClientRect(hwnd, &bounds));
        labels = graphics.CreateDrawingSurface({float(bounds.right), float(bounds.bottom)},
            DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);
        auto surface = labels.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        com_ptr<ID2D1DeviceContext> context;
        POINT offset{};
        require(surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), context.put_void(), &offset), "source_label_begin_draw");
        try {
            context->Clear(D2D1::ColorF(0, 0.0f));
            context->SetTransform(D2D1::Matrix3x2F::Translation(float(offset.x), float(offset.y)));
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            com_ptr<ID2D1SolidColorBrush> panel, ink;
            require(context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), panel.put()), "source_label_panel_brush");
            require(context->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.11f, 0.16f, 1.0f), ink.put()), "source_label_ink");
            context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(10, 12, float(bounds.right - 10), 72), 6, 6), panel.get());
            com_ptr<IDWriteFactory> textFactory;
            require(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(textFactory.put())), "source_label_text_factory");
            com_ptr<IDWriteTextFormat> format;
            require(textFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-US", format.put()), "source_label_text_format");
            const wchar_t title[] = L"Composition BackdropBrush";
            const wchar_t subtitle[] = L"F2: Gaussian blur 12 / 0";
            context->DrawText(title, UINT32(std::size(title) - 1), format.get(), D2D1::RectF(16, 20, float(bounds.right - 16), 43), ink.get());
            context->DrawText(subtitle, UINT32(std::size(subtitle) - 1), format.get(), D2D1::RectF(16, 45, float(bounds.right - 16), 68), ink.get());
        } catch (...) { surface->EndDraw(); throw; }
        require(surface->EndDraw(), "source_label_end_draw");
        auto foreground = compositor.CreateSpriteVisual();
        foreground.RelativeSizeAdjustment({1.0f, 1.0f});
        foreground.Brush(compositor.CreateSurfaceBrush(labels));
        root.Children().InsertAtTop(foreground);
        target.Root(root);
        hr_log("source_visual_tree_ready", S_OK);
    }

    void set_blur(bool enabled) {
        brush.Properties().InsertScalar(L"Blur.BlurAmount", enabled ? 12.0f : 0.0f);
        log("\"event\":\"native_blur_fixture\",\"implementation\":\"CompositionBackdropBrush_GaussianBlur\",\"enabled\":" +
            std::to_string(enabled) + ",\"blur_amount\":" + (enabled ? "12" : "0") +
            ",\"owned_window_only\":true,\"foreground_unfiltered\":true");
    }
    ~SourceBackdrop() {
        try {
            if (target) target.Root(nullptr);
            if (compositor) compositor.Close();
            if (queue) queue.ShutdownQueueAsync();
        } catch (...) {}
    }
};
std::unique_ptr<SourceBackdrop> sourceBackdrop;

void set_acrylic() {
    if (!is_own(acrylic)) throw hresult_error(E_ACCESSDENIED);
    if (!sourceBackdrop) {
        sourceBackdrop = std::make_unique<SourceBackdrop>();
        sourceBackdrop->initialize(acrylic);
    }
    sourceBackdrop->set_blur(acrylicOn);
}
void start_output_group();
void stop_output_group() {
    probe.reset();
    destroy_helper_group();
    destroy_group_window(output, "destroy_output");
    if (base) InvalidateRect(base, nullptr, FALSE);
    log("\"event\":\"output_group_stopped\",\"helper_absent\":" + std::to_string(!IsWindow(helper)) +
        ",\"cover_absent\":" + std::to_string(!IsWindow(cover)) + ",\"output_absent\":" +
        std::to_string(!IsWindow(output)) + ",\"source_visible\":" + std::to_string(IsWindowVisible(acrylic) != FALSE));
}
void shutdown() {
    if (closing) return;
    closing = true;
    if (base) KillTimer(base, 1);
    stop_output_group();
    sourceBackdrop.reset();
    for (const HWND hwnd : {acrylic, base}) if (IsWindow(hwnd)) DestroyWindow(hwnd);
    PostQuitMessage(0);
}
LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    try {
        switch (message) {
        case WM_CLOSE: shutdown(); return 0;
#ifdef TIEZ_PROBE_WEBVIEW
        case webviewFailedMessage:
            if (!probe || wparam != probe->webViewGeneration) return 0;
            stop_output_group();
            log("\"event\":\"awaiting_f1\",\"base_and_source_retained\":true,\"reason\":\"webview_failure\"");
            return 0;
#endif
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) shutdown();
            else if (wparam == VK_F1 && !(lparam & (1LL << 30))) {
                if (probe) stop_output_group(); else start_output_group();
            }
            else if (wparam == VK_F2) {
                acrylicOn = !acrylicOn; set_acrylic();
                if (probe) { probe->sceneConfigured = false; probe->renderDirty = true; }
                log("\"event\":\"material_toggle\",\"value\":" + std::to_string(acrylicOn) +
                    ",\"scene_refresh_requested\":true");
            }
            else if (wparam == VK_F3) {
                mirror = !mirror;
                if (probe) probe->renderDirty = true;
                log("\"event\":\"mirror_toggle\",\"value\":" + std::to_string(mirror));
            }
            else if (wparam == VK_F4) {
                paused = !paused;
                log("\"event\":\"freeze_toggle\",\"value\":" + std::to_string(paused));
            }
            else if (wparam == VK_F5) {
                controlsOptics = !controlsOptics;
                if (probe) probe->renderDirty = true;
                if (output) SetWindowTextW(output, controlsOptics ? L"TieZ DWM Scene QA | seven glass controls" :
                    L"TieZ DWM Scene QA | raw WGC scene");
                log("\"event\":\"controls_optics_toggle\",\"value\":" + std::to_string(controlsOptics) +
                    ",\"cached_texture_redraw\":true");
            }
            else if (wparam == VK_F6 && !(lparam & (1LL << 30)) && probe) {
                probe->toggle_output_position();
            }
            return 0;
        case WM_TIMER:
            if (hwnd == base) {
                if (GetTickCount64() - started >= automaticExitMs) {
                    log("\"event\":\"automatic_timeout\""); shutdown(); return 0;
                }
                if (!paused) { animationTick += 2; InvalidateRect(base, nullptr, FALSE); UpdateWindow(base); }
                if (probe) probe->tick();
            }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (hwnd == base) paint_base(dc);
            else if (hwnd == cover) {
                RECT bounds{}; GetClientRect(hwnd, &bounds);
                // Diagnostic only: any exposed pixel can be detected in a recording.
                fill(dc, bounds, RGB(255, 0, 255));
            }
            // The source backdrop and sharp labels are composition visuals;
            // painting a GDI background here would cover the backdrop brush.
            EndPaint(hwnd, &ps);
            return 0;
        }
        }
    } catch (const hresult_error& error) {
        hr_log("runtime_group_stop", error.code());
        stop_output_group();
        log("\"event\":\"awaiting_f1\",\"base_and_source_retained\":true,\"reason\":\"runtime_group_failure\"");
    } catch (...) {
        hr_log("unexpected_group_stop", E_FAIL);
        stop_output_group();
        log("\"event\":\"awaiting_f1\",\"base_and_source_retained\":true,\"reason\":\"unexpected_group_failure\"");
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
HWND make_window(HINSTANCE instance, const wchar_t* title, DWORD ex, HWND owner, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(ex | WS_EX_TOPMOST, L"TieZOwnedDwmSceneProbe", title, WS_POPUP,
        x, y, w, h, owner, nullptr, instance, nullptr);
    check_bool(hwnd != nullptr);
    const DWORD noRound = 1;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &noRound, sizeof(noRound));
    validate_affinity(hwnd);
    return hwnd;
}
void start_output_group() {
    if (probe || helper || cover || output) throw hresult_error(E_UNEXPECTED);
    outputRect = outputHomeRect;
    log("\"event\":\"output_group_start_requested\",\"source_visible\":" +
        std::to_string(IsWindowVisible(acrylic) != FALSE));
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    helper = make_window(instance, L"TieZ DWM Scene QA | private helper", WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, nullptr,
        outputRect.left, outputRect.top, width, height);
    cover = make_window(instance, L"TieZ DWM Scene QA | opaque helper cover", WS_EX_TOOLWINDOW, helper,
        outputRect.left, outputRect.top, width, height);
    output = make_window(instance, L"TieZ DWM Scene QA | WGC shader output", WS_EX_NOREDIRECTIONBITMAP, nullptr,
        outputRect.left, outputRect.top, width, height);
#ifdef TIEZ_PROBE_ROUNDED
    for (const HWND hwnd : {helper, cover, output}) set_rounded_region(hwnd);
    validate_rounded_regions();
#endif
    probe = std::make_unique<Probe>();
    probe->initialize();
    InvalidateRect(base, nullptr, FALSE);
    log("\"event\":\"output_group_started\"");
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    started = GetTickCount64();
    wchar_t executable[MAX_PATH]{}; GetModuleFileNameW(nullptr, executable, MAX_PATH);
    evidence.open(std::filesystem::path(executable).parent_path() / L"scene-evidence.jsonl", std::ios::trunc);
    try {
        const BOOL dpiSet = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const DWORD dpiError = dpiSet ? ERROR_SUCCESS : GetLastError();
        const BOOL dpiVerified = AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        log("\"event\":\"dpi_context\",\"set_ok\":" + std::to_string(dpiSet != FALSE) +
            ",\"set_error\":" + std::to_string(dpiError) + ",\"per_monitor_v2_verified\":" + std::to_string(dpiVerified != FALSE));
        if (!dpiVerified) throw hresult_error(E_ACCESSDENIED);
        init_apartment(apartment_type::single_threaded);
        // Refuse systems with the pre-Windows-11 ordinal ABI.
        using RtlGetVersionType = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto versionFunction = reinterpret_cast<RtlGetVersionType>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{}; version.dwOSVersionInfoSize = sizeof(version);
        if (!versionFunction || versionFunction(&version) != 0 || version.dwBuildNumber < 22621) throw hresult_error(E_NOTIMPL);
        log("\"event\":\"start\",\"pid\":" + std::to_string(GetCurrentProcessId()) +
            ",\"windows_build\":" + std::to_string(version.dwBuildNumber) + ",\"capture_scope\":\"owned_fixture_only\","
            "\"rounded_region\":" + std::to_string(roundedRegionEnabled) +
            ",\"rounded_radius_px\":" + std::to_string(roundedRadiusPx));
        const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{}; info.cbSize = sizeof(info); check_bool(GetMonitorInfoW(monitor, &info));
        const RECT display = info.rcMonitor;
        if (display.right - display.left < baseWidth || display.bottom - display.top < baseHeight) throw hresult_error(E_BOUNDS);
        layoutX = (display.right - display.left - baseWidth) / 2;
        layoutY = (display.bottom - display.top - baseHeight) / 2;
        const int x = display.left + layoutX;
        const int y = display.top + layoutY;
        sourceRect = RECT{x + fixtureX, y + fixtureY, x + fixtureX + width, y + fixtureY + height};
        outputHomeRect = RECT{x + outputX, y + fixtureY, x + outputX + width, y + fixtureY + height};
        outputRect = outputHomeRect;
        WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = window_proc;
        wc.lpszClassName = L"TieZOwnedDwmSceneProbe"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        check_bool(RegisterClassW(&wc));
        base = make_window(instance, L"TieZ DWM Scene QA | owned fixture", 0, nullptr,
            display.left, display.top, display.right - display.left, display.bottom - display.top);
        ShowWindow(base, SW_SHOW); UpdateWindow(base);
        acrylic = make_window(instance, L"TieZ DWM Scene QA | native acrylic", WS_EX_NOREDIRECTIONBITMAP, base,
            sourceRect.left + 45, sourceRect.top + 85, 230, 190);
        ShowWindow(acrylic, SW_SHOW); set_acrylic(); UpdateWindow(acrylic);
        check_hresult(DwmFlush());
        check_bool(SetTimer(base, 1, 33, nullptr) != 0);
        log("\"event\":\"timer_started\",\"period_ms\":33,\"automatic_exit_ms\":" + std::to_string(automaticExitMs));
        log("\"event\":\"awaiting_f1\",\"base_and_source_only\":true");
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    } catch (const hresult_error& error) { hr_log("initialization_stop", error.code()); shutdown(); }
      catch (...) { hr_log("initialization_unexpected_stop", E_FAIL); shutdown(); }
    log("\"event\":\"shutdown\"");
    return 0;
}
