// The private scene bridge uses the Windows 11 ABI researched in the isolated
// dwm_scene_probe. No capture exclusion is applied to the application's HWND.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "glass_renderer.h"
#include <windows.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include "glass_background_surface.h"
#include "glass_capture_region.h"
#include "glass_zorder_noop.h"
#include "glass_renderer_vs.h"
#include "glass_renderer_blur_ps.h"
#include "glass_renderer_optics_ps.h"
#include "glass_renderer_prefix_init_ps.h"
#include "glass_renderer_prefix_glass_ps.h"
#include "glass_renderer_prefix_track_ps.h"
#include "glass_renderer_prefix_finish_ps.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
constexpr UINT_PTR watchdogId = 1;
constexpr UINT_PTR resizeWatchdogId = 0x54475A48;
constexpr UINT_PTR scopeRecoveryTimerId = 0x54475A49;
constexpr UINT_PTR zOrderNoopTimerId = 0x54475A4A;
constexpr UINT_PTR presentRetryTimerId = 0x54475A4B;
constexpr UINT_PTR initialAlphaRetryTimerId = 0x54475A4C;
constexpr UINT_PTR subclassId = 0x54475A47;
constexpr size_t maxControls = 64;
constexpr wchar_t helperClass[] = L"TieZ.NativeGlass.Helper.v1";
const UINT frameMessage = RegisterWindowMessageW(L"TieZ.NativeGlass.Frame.v1");
const UINT reconcileMessage = RegisterWindowMessageW(L"TieZ.NativeGlass.Reconcile.v1");
std::atomic<UINT_PTR> nextGeneration{1};

using CreateScene = HRESULT(WINAPI*)(HWND, void*, void**, HTHUMBNAIL*);
using UpdateScene = HRESULT(WINAPI*)(HTHUMBNAIL, HWND*, DWORD, HWND*, DWORD, RECT*, SIZE*, DWORD);

struct Dispatch {
    std::atomic<bool> alive{true};
    std::atomic<bool> frameQueued{false};
    std::atomic<bool> scopeQueued{false};
    std::atomic<bool> captureClosed{false};
    HWND window = nullptr;
    UINT_PTR generation = 0;
    std::mutex retargetMutex;
    uint64_t requestedRetarget = 0, completedRetarget = 0;
    HRESULT retargetResult = S_OK;
    bool retargetWorkerRunning = false;
};
std::mutex hookMutex;
std::unordered_map<HWINEVENTHOOK, std::weak_ptr<Dispatch>> hookDispatch;

void queue(const std::shared_ptr<Dispatch>& dispatch, bool frame) noexcept {
    if (!dispatch || !dispatch->alive.load()) return;
    auto& queued = frame ? dispatch->frameQueued : dispatch->scopeQueued;
    if (queued.exchange(true)) return;
    if (!PostMessageW(dispatch->window, frame ? frameMessage : reconcileMessage,
        dispatch->generation, 0)) queued.store(false);
}

bool complete_retarget_flush(const std::shared_ptr<Dispatch>& dispatch, uint64_t revision, HRESULT result) {
    std::lock_guard<std::mutex> lock(dispatch->retargetMutex);
    if (!dispatch->alive.load()) {
        dispatch->retargetWorkerRunning = false;
        return false;
    }
    if (revision > dispatch->completedRetarget) {
        dispatch->completedRetarget = revision;
        dispatch->retargetResult = result;
    }
    const bool again = dispatch->requestedRetarget > revision;
    if (!again) dispatch->retargetWorkerRunning = false;
    return again;
}

void queue_retarget_flush(const std::shared_ptr<Dispatch>& dispatch, uint64_t revision) {
    if (!dispatch || !dispatch->alive.load()) throw hresult_error(E_ABORT);
    std::lock_guard<std::mutex> lock(dispatch->retargetMutex);
    dispatch->requestedRetarget = revision;
    if (dispatch->retargetWorkerRunning) return;
    dispatch->retargetWorkerRunning = true;
    try {
        std::thread([dispatch] {
            bool again;
            do {
                uint64_t target;
                {
                    std::lock_guard<std::mutex> guard(dispatch->retargetMutex);
                    if (!dispatch->alive.load()) {
                        dispatch->retargetWorkerRunning = false;
                        return;
                    }
                    target = dispatch->requestedRetarget;
                }
                // Only the compositor fence runs here. The immediate context,
                // frame pool and renderer remain owned by the window thread.
                again = complete_retarget_flush(dispatch, target, DwmFlush());
                queue(dispatch, true);
            } while (again);
        }).detach();
    } catch (...) {
        dispatch->retargetWorkerRunning = false;
        throw;
    }
}

void CALLBACK scene_event(HWINEVENTHOOK hook, DWORD, HWND, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW && object != OBJID_CLIENT) return;
    std::shared_ptr<Dispatch> dispatch;
    {
        std::lock_guard<std::mutex> lock(hookMutex);
        const auto found = hookDispatch.find(hook);
        if (found != hookDispatch.end()) dispatch = found->second.lock();
    }
    queue(dispatch, false);
}

bool own_window(HWND window) noexcept {
    DWORD process = 0;
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}

HRESULT last_error() noexcept {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

void require_bool(BOOL value) { if (!value) throw hresult_error(last_error()); }

struct Region {
    HRGN value = nullptr;
    explicit Region(HRGN region = nullptr) : value(region) {}
    ~Region() { if (value) DeleteObject(value); }
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
};

struct Thumbnail {
    HTHUMBNAIL value = nullptr;
    explicit Thumbnail(HTHUMBNAIL thumbnail = nullptr) : value(thumbnail) {}
    ~Thumbnail() { if (value) DwmUnregisterThumbnail(value); }
    Thumbnail(const Thumbnail&) = delete;
    Thumbnail& operator=(const Thumbnail&) = delete;
    HTHUMBNAIL release() { const auto result = value; value = nullptr; return result; }
};

struct MissingHostRegion {};
struct HostStateChanged { const char* reason; };
struct HostTranslationPending {};
struct SceneTemporarilyUnavailable { const char* reason; };

struct PopupProcess {
    HANDLE handle = nullptr;
    FILETIME created{};
    ~PopupProcess() { if (handle) CloseHandle(handle); }
};

bool equal_path(const std::wstring& first, const std::wstring& second) noexcept {
    return CompareStringOrdinal(first.c_str(), static_cast<int>(first.size()),
        second.c_str(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

bool webview_image_name(const std::wstring& image) {
    const size_t filename = image.find_last_of(L'\\');
    return filename != std::wstring::npos && equal_path(image.substr(filename + 1), L"msedgewebview2.exe");
}

bool cursor_overlay(HWND window) noexcept {
    wchar_t name[128]{};
    const int count = GetClassNameW(window, name, static_cast<int>(std::size(name)));
    constexpr DWORD required = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    return count && wcscmp(name, L"CodexComputerUseCursorOverlay") == 0 && (style & required) == required;
}

void helper_live_preview(HWND helper, bool value) {
    struct Attribute { int attribute; void* data; SIZE_T size; };
    using SetAttribute = BOOL(WINAPI*)(HWND, Attribute*);
    const auto set = reinterpret_cast<SetAttribute>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!set || !own_window(helper)) throw hresult_error(E_NOINTERFACE);
    BOOL enabled = value ? TRUE : FALSE;
    Attribute attribute{0xD, &enabled, sizeof(enabled)};
    require_bool(set(helper, &attribute));
}

struct ShaderControl { float rectangle[4]; float shape[4]; };
struct ShaderTrack {
    float rectangle[4];
    float shape[4]; // Radius, progress, owning control index, reserved.
    float active[4];
    float inactive[4];
};
struct ShaderState {
    float size[2];
    float direction[2];
    float tint[4];
    float options[4];
    ShaderControl controls[maxControls];
    float trackMeta[4];
    ShaderTrack tracks[maxControls];
};
static_assert(sizeof(ShaderControl) == 32);
static_assert(sizeof(ShaderTrack) == 64);
static_assert(sizeof(ShaderState) == 6208);
static_assert(offsetof(ShaderState, controls) == 48);
static_assert(offsetof(ShaderState, trackMeta) == 2096);
static_assert(offsetof(ShaderState, tracks) == 2112);
static_assert(sizeof(TiezGlassTrack) == 56);
static_assert(sizeof(TiezGlassControl) == 84);
static_assert(offsetof(TiezGlassControl, material) == 24);
static_assert(offsetof(TiezGlassControl, track) == 28);

LRESULT CALLBACK helper_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK host_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void register_helper_class() {
    static const bool registered = [] {
        WNDCLASSEXW type{sizeof(type)};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = helper_proc;
        type.lpszClassName = helperClass;
        type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        const ATOM atom = RegisterClassExW(&type);
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw hresult_error(last_error());
        return true;
    }();
    (void)registered;
}
} // namespace

struct TiezGlass {
    HWND host = nullptr, qaSource = nullptr, helper = nullptr, cover = nullptr;
    const DWORD thread = GetCurrentThreadId();
    const UINT_PTR identity = nextGeneration.fetch_add(1);
    void* callbackContext = nullptr;
    TiezGlassStateCallback callback = nullptr;
    int32_t reportedState = -1;
    HRESULT reportedError = S_OK;
    uint64_t reportedGeometryRevision = 0;
    const char* phase = "construct";
    bool active = true, hasState = false, tearingDown = false, synchronizing = false, failed = false;
    bool reconcileDeferred = false, frameDeferred = false;
    bool subclassInstalled = false, originalRegionSaved = false, hadOriginalRegion = false;
    bool hostRegionInstalled = false, helperExcluded = false, sceneConfigured = false;
    bool hostRegionRecoveryPending = false, hostRegionRecoveryUsed = false;
    bool scopeRecoveryPending = false, scopeRecoveryTimerArmed = false;
    bool qaColoredAuxiliary = false, qaCoincidentAuxiliary = false;
    mutable uint64_t regionQueryRetries = 0;
    uint64_t displayEpoch = 0, configuredDisplayEpoch = 0;
    uint64_t cropRevision = 0, acceptedCropRevision = 0, rejectedEarlyFrames = 0;
    int64_t earliestRetargetFrame = 0;
    bool validTexture = false, nativeStarted = false, baseDirty = true, solidOnlyOutput = false;
    bool awaitingMoveCapture = false;
    bool retargetFlushPending = false;
    bool initialAlphaPending = false;
    uint64_t initialCaptureAt = 0;
    struct PendingPresent {
        uint64_t epoch = 0, parameters = 0;
        bool active = false, backgroundOnly = false, forced = false, announce = false;
    } pendingPresent;
    bool foregroundSwapMounted = false, viewportResizePending = false;
    uint64_t parameterRevision = 0;
    RECT pendingViewportClient{}, pendingViewportOuter{};
    bool resizeBackground = false;
    tiez_glass_detail::ResizeSnapshot lastCapture{};
    tiez_glass_detail::ResizeLease resizeLease{};
    tiez_glass_detail::PresentationEpoch presentationEpoch{};
    tiez_glass_detail::ResizeSizeGate resizeSizeGate{};
    tiez_glass_detail::ZOrderNoopGate zOrderNoop{};
    uint64_t zOrderNoopEpoch = 0;
    UINT_PTR zOrderNoopCapture = 0;
    bool zOrderNoopRenderDeferred = false;
    std::vector<HWND> captureExclusions;
    RECT bounds{}, hostBounds{}, auxiliaryBounds{}, virtualDesktopBounds{}, configuredSceneCrop{};
    RECT captureBounds{}, filteredCaptureBounds{}, backgroundSurfaceBounds{};
    UINT width = 0, height = 0, configuredDpi = 96;
    UINT captureWidth = 0, captureHeight = 0;
    DWORD configuredFrameStyle = 0, configuredFrameExtendedStyle = 0;
    float scale = 1.0f;
    Region originalRegion, expectedHostRegion;
    TiezGlassState parameters{};
    std::vector<TiezGlassControl> controls;
    std::unordered_map<DWORD, std::unique_ptr<PopupProcess>> qaPopupProcesses;
    std::vector<HWND> exclusions;
    HMODULE dwm = nullptr;
    HTHUMBNAIL thumbnail = nullptr;
    CreateScene createScene = nullptr;
    UpdateScene updateScene = nullptr;
    std::shared_ptr<Dispatch> dispatch;
    std::vector<HWINEVENTHOOK> hooks;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDCompositionDesktopDevice> composition;
    com_ptr<IDCompositionTarget> hostTarget, helperTarget;
    com_ptr<IDCompositionVisual2> rootVisual, solidVisual, backgroundVisual, hostVisual, sharedVisual;
    com_ptr<IDCompositionSurface> solidSurface, backgroundSurface;
    uint32_t solidPixel = 0;
    com_ptr<IDXGISwapChain1> swap;
    com_ptr<ID3D11Texture2D> scene, horizontal, tinted, initialAlpha;
    com_ptr<ID3D11Texture2D> capturedScene;
    com_ptr<ID3D11Texture2D> captureTexture, filteredCaptureSource;
    com_ptr<ID3D11ShaderResourceView> captureView, filteredCaptureSourceView;
    bool filteredCaptureValid = false;
    uint64_t captureRevision = 0, filteredCaptureRevision = 0;
    float filteredCaptureSigma = -1.0f, filteredCaptureTint[4]{};
    com_ptr<ID3D11ShaderResourceView> sceneView, horizontalView, tintedView;
    com_ptr<ID3D11RenderTargetView> horizontalTarget, tintedTarget;
    com_ptr<ID3D11VertexShader> vertex;
    com_ptr<ID3D11PixelShader> blur, optics;
    com_ptr<ID3D11PixelShader> prefixInit, prefixGlass, prefixTrack, prefixFinish;
    struct OpticalSurface {
        com_ptr<ID3D11Texture2D> texture;
        com_ptr<ID3D11ShaderResourceView> view;
        com_ptr<ID3D11RenderTargetView> target;
    };
    OpticalSurface initialPrefix, prefixColors[2], prefixCoverage[2];
    OpticalSurface captureHorizontal, captureFiltered;
    struct BackgroundOutput {
        com_ptr<IDCompositionSurface> surface;
        RECT crop{};
    };
    bool initialPrefixValid = false;
    com_ptr<ID3D11Buffer> constants;
    com_ptr<ID3D11Buffer> gaussianConstants;
    float gaussianSigma = -1.0f;
    com_ptr<ID3D11SamplerState> sampler;
    com_ptr<ID3D11RasterizerState> scissorRasterizer;
    IDirect3DDevice captureDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::event_token frameToken{}, closedToken{};
    bool hasFrameToken = false, hasClosedToken = false;

    struct SynchronizationScope {
        TiezGlass& renderer;
        const bool previous;
        explicit SynchronizationScope(TiezGlass& value) : renderer(value), previous(value.synchronizing) {
            renderer.synchronizing = true;
        }
        ~SynchronizationScope() {
            renderer.synchronizing = previous;
            if (!previous && renderer.reconcileDeferred) {
                renderer.reconcileDeferred = false;
                renderer.request_reconcile();
            }
            if (!previous && renderer.frameDeferred) {
                renderer.frameDeferred = false;
                queue(renderer.dispatch, true);
            }
        }
    };

    TiezGlass(HWND window, HWND fixture, void* user, TiezGlassStateCallback stateCallback)
        : host(window), qaSource(fixture), callbackContext(user), callback(stateCallback) {
        if (fixture) {
            wchar_t mode[32]{};
            const DWORD length = GetEnvironmentVariableW(L"TIEZ_MATERIAL_QA_AUXILIARY", mode, static_cast<DWORD>(std::size(mode)));
            if (length && length < std::size(mode)) {
                qaColoredAuxiliary = wcscmp(mode, L"colors") == 0 || wcscmp(mode, L"offscreen") == 0;
                qaCoincidentAuxiliary = wcscmp(mode, L"colors") == 0;
            }
        }
    }

    void check_thread() const {
        if (GetCurrentThreadId() != thread) throw hresult_error(RPC_E_WRONG_THREAD);
    }

    void report(int32_t state, HRESULT error = S_OK, uint64_t geometryRevision = 0) noexcept {
        if (failed && state != 2) return;
        const uint64_t submittedRevision = state == 1 ? geometryRevision : 0;
        if (reportedState == state && reportedError == error && reportedGeometryRevision == submittedRevision) return;
        reportedState = state;
        reportedError = error;
        reportedGeometryRevision = submittedRevision;
        trace_output("state");
        if (callback && !tearingDown) callback(callbackContext, state, static_cast<int32_t>(error), submittedRevision);
    }

    void report_frame_submission(uint64_t parametersAtStart, bool backgroundOnly) {
        if (parametersAtStart == parameterRevision)
            report(backgroundOnly ? 3 : 1, S_OK, backgroundOnly ? 0 : parameters.geometry_revision);
        else { baseDirty = true; frameDeferred = true; }
    }

    void trace_output(const char* event, IDXGISwapChain1* previous = nullptr,
        IDCompositionSurface* previousBackground = nullptr) const noexcept {
        if (!qaSource) return;
        FILETIME utc{};
        GetSystemTimePreciseAsFileTime(&utc);
        ULARGE_INTEGER stamp{};
        stamp.LowPart = utc.dwLowDateTime;
        stamp.HighPart = utc.dwHighDateTime;
        constexpr uint64_t unixEpoch100ns = 116444736000000000ULL;
        const uint64_t timeMs = stamp.QuadPart >= unixEpoch100ns ? (stamp.QuadPart - unixEpoch100ns) / 10000 : 0;
        // Fixture-only lifecycle evidence: no window titles or sampled pixels.
        std::fprintf(stderr,
            "[native-glass-output] renderer=%llu event=%s time_ms=%llu tick_ms=%llu epoch=%llu "
            "state=%d hr=0x%08lX native_started=%d captured=%d resize_background=%d "
            "swap=%p previous_swap=%p background=%p previous_background=%p root=%p solid=%p solid_only=%d phase=%s\n",
            static_cast<unsigned long long>(identity), event, static_cast<unsigned long long>(timeMs),
            static_cast<unsigned long long>(GetTickCount64()), static_cast<unsigned long long>(presentationEpoch.value),
            reportedState, static_cast<unsigned long>(reportedError), nativeStarted, validTexture, resizeBackground,
            static_cast<void*>(swap.get()), static_cast<void*>(previous), static_cast<void*>(backgroundSurface.get()),
            static_cast<void*>(previousBackground), static_cast<void*>(rootVisual.get()),
            static_cast<void*>(solidSurface.get()), solidOnlyOutput, phase);
        std::fflush(stderr);
    }

    void trace_host_layout(const char* event) const noexcept {
        if (!qaSource || !own_window(host)) return;
        RECT outer{}, client{}, mask{}, expected{};
        POINT origin{};
        GetWindowRect(host, &outer);
        GetClientRect(host, &client);
        ClientToScreen(host, &origin);
        const int kind = GetWindowRgnBox(host, &mask);
        if (expectedHostRegion.value) GetRgnBox(expectedHostRegion.value, &expected);
        std::fprintf(stderr,
            "[native-glass-layout] renderer=%llu event=%s tick_ms=%llu epoch=%llu syncing=%d await_size=%d "
            "size_generation=%llu positioned=%d target=[%u,%u] "
            "client=[%ld,%ld,%ld,%ld] origin=[%ld,%ld] outer=[%ld,%ld,%ld,%ld] "
            "region_kind=%d region=[%ld,%ld,%ld,%ld] expected=[%ld,%ld,%ld,%ld]\n",
            static_cast<unsigned long long>(identity), event, static_cast<unsigned long long>(GetTickCount64()),
            static_cast<unsigned long long>(presentationEpoch.value), synchronizing, resizeSizeGate.waiting,
            static_cast<unsigned long long>(resizeSizeGate.generation), resizeSizeGate.position_known,
            resizeSizeGate.target_width, resizeSizeGate.target_height,
            client.left, client.top, client.right, client.bottom, origin.x, origin.y,
            outer.left, outer.top, outer.right, outer.bottom, kind, mask.left, mask.top, mask.right, mask.bottom,
            expected.left, expected.top, expected.right, expected.bottom);
        std::fflush(stderr);
    }

    void diagnose(HRESULT error) const noexcept {
        if (!qaSource) return;
        RECT outer{}, client{};
        POINT origin{};
        const BOOL outerOk = GetWindowRect(host, &outer);
        const BOOL clientOk = GetClientRect(host, &client) && ClientToScreen(host, &origin);
        if (clientOk) OffsetRect(&client, origin.x, origin.y);
        DWORD affinity = 0xffffffff;
        SetLastError(ERROR_SUCCESS);
        const BOOL affinityOk = GetWindowDisplayAffinity(host, &affinity);
        const DWORD affinityError = affinityOk ? ERROR_SUCCESS : GetLastError();
        // Opt-in fixture diagnostics contain only our windows' geometry and
        // flags. Never enumerate external titles, text, images or data here.
        std::fprintf(stderr,
            "[native-glass] phase=%s hr=0x%08lX own=%d owner=%d visible=%d iconic=%d "
            "style=0x%08lX exstyle=0x%08lX outer_ok=%d outer=[%ld,%ld,%ld,%ld] "
            "client_ok=%d client=[%ld,%ld,%ld,%ld] crop=[%ld,%ld,%ld,%ld] size=[%u,%u] "
            "fixture_own=%d helper_own=%d helper_owner=%d cover_own=%d cover_owner_matches=%d "
            "affinity_get=%d affinity_error=%lu affinity=0x%08lX\n",
            phase, static_cast<unsigned long>(error), own_window(host), GetWindow(host, GW_OWNER) != nullptr,
            IsWindowVisible(host) != FALSE, IsIconic(host) != FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(host, GWL_STYLE)),
            static_cast<DWORD>(GetWindowLongPtrW(host, GWL_EXSTYLE)),
            outerOk != FALSE, outer.left, outer.top, outer.right, outer.bottom,
            clientOk != FALSE, client.left, client.top, client.right, client.bottom,
            bounds.left, bounds.top, bounds.right, bounds.bottom, width, height,
            own_window(qaSource), own_window(helper), helper && GetWindow(helper, GW_OWNER) != nullptr,
            own_window(cover), cover && GetWindow(cover, GW_OWNER) == helper,
            affinityOk != FALSE, affinityError, affinity);
        std::fflush(stderr);
    }

    bool query_host_region(Region& result) const {
        check_thread();
        if (!own_window(host) || GetWindowThreadProcessId(host, nullptr) != thread)
            throw hresult_error(E_ACCESSDENIED);
        result.value = CreateRectRgn(0, 0, 0, 0);
        if (!result.value) throw hresult_error(last_error());
        SetLastError(ERROR_SUCCESS);
        const int firstRead = GetWindowRgn(host, result.value);
        const DWORD firstError = GetLastError();
        if (firstRead != ERROR) return true;

        // Live QA observed a valid destination failing with error 6 while a
        // fresh destination immediately returned the complete host mask. One
        // bounded read retry is safe; the caller still compares the whole mask.
        const DWORD firstType = GetObjectType(result.value);
        Region retry(CreateRectRgn(0, 0, 0, 0));
        if (!retry.value) throw hresult_error(last_error());
        SetLastError(ERROR_SUCCESS);
        const int secondRead = GetWindowRgn(host, retry.value);
        const DWORD secondError = GetLastError();
        const DWORD secondType = GetObjectType(retry.value);
        if (secondRead != ERROR && firstType == OBJ_REGION && secondType == OBJ_REGION) {
            std::swap(result.value, retry.value);
            ++regionQueryRetries;
            if (qaSource && (regionQueryRetries & (regionQueryRetries - 1)) == 0) {
                std::fprintf(stderr,
                    "[native-glass-region-query] renderer=%llu recovered_reads=%llu first_error=%lu second_read=%d\n",
                    static_cast<unsigned long long>(identity), static_cast<unsigned long long>(regionQueryRetries),
                    firstError, secondRead);
                std::fflush(stderr);
            }
            return true;
        }

        RECT sourceExtent{};
        SetLastError(ERROR_SUCCESS);
        const int sourceBox = GetWindowRgnBox(host, &sourceExtent);
        const DWORD sourceError = GetLastError();
        // ERROR also means no window region. Independent source/destination
        // checks distinguish the observed missing mask from a bad query handle.
        const bool missing = firstType == OBJ_REGION && secondType == OBJ_REGION &&
            secondRead == ERROR && sourceBox == ERROR && sourceError == ERROR_SUCCESS;
        if (qaSource) {
            std::fprintf(stderr,
                "[native-glass-region-query] renderer=%llu first_read=%d first_error=%lu first_type=%lu "
                "second_read=%d second_error=%lu second_type=%lu source_box=%d source_error=%lu "
                "source_extent=[%ld,%ld,%ld,%ld] missing=%d\n",
                static_cast<unsigned long long>(identity), firstRead, firstError, firstType,
                secondRead, secondError, secondType, sourceBox, sourceError,
                sourceExtent.left, sourceExtent.top, sourceExtent.right, sourceExtent.bottom, missing);
            std::fflush(stderr);
        }
        if (!missing) throw hresult_error(E_FAIL);
        return false;
    }

    void save_region() {
        hadOriginalRegion = query_host_region(originalRegion);
        originalRegionSaved = true;
    }

    HRGN client_region() const {
        Region region;
        if (!query_host_region(region)) {
            if (hostRegionInstalled && expectedHostRegion.value) throw MissingHostRegion{};
            throw hresult_error(E_FAIL);
        }
        // Bounding-box equality alone cannot prove the same rounded silhouette.
        if (!hostRegionInstalled || !expectedHostRegion.value ||
            !EqualRgn(region.value, expectedHostRegion.value)) throw hresult_error(E_ACCESSDENIED);
        Region clip(CreateRectRgn(0, 0, static_cast<int>(width), static_cast<int>(height)));
        if (!clip.value) throw hresult_error(last_error());
        // Window regions use outer-window coordinates; DComp and capture use
        // client coordinates, excluding Tauri's invisible resize margins.
        if (OffsetRgn(region.value, hostBounds.left - bounds.left, hostBounds.top - bounds.top) == ERROR ||
            CombineRgn(region.value, region.value, clip.value, RGN_AND) == ERROR)
            throw hresult_error(E_FAIL);
        RECT extent{};
        const RECT expected{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        if (GetRgnBox(region.value, &extent) == ERROR || !EqualRect(&extent, &expected))
            throw hresult_error(E_BOUNDS);
        const HRGN result = region.value;
        region.value = nullptr;
        return result;
    }

    void install_host_region(const RECT& client, const RECT& outer, UINT dpi) {
        Region region;
        const LONG clientWidth = client.right - client.left, clientHeight = client.bottom - client.top;
        if (!dpi || clientWidth < 1 || clientHeight < 1 || clientWidth > 8192 || clientHeight > 8192 ||
            client.left < outer.left || client.top < outer.top || client.right > outer.right || client.bottom > outer.bottom)
            throw hresult_error(E_BOUNDS);
        const int radius = std::max(1, static_cast<int>(std::lround(16.0f * dpi / 96.0f)));
        // Restrict visible pixels to the client body. The public resize
        // handles in the WebView supply resizing inside that silhouette.
        region.value = CreateRoundRectRgn(0, 0, static_cast<int>(clientWidth) + 1,
            static_cast<int>(clientHeight) + 1, radius * 2, radius * 2);
        if (!region.value) throw hresult_error(last_error());
        if (OffsetRgn(region.value, client.left - outer.left,
            client.top - outer.top) == ERROR) throw hresult_error(E_FAIL);
        const bool changedShape = !expectedHostRegion.value || !EqualRgn(region.value, expectedHostRegion.value);
        Region current;
        const bool present = query_host_region(current);
        if (hostRegionInstalled) {
            // Preserve a nonempty region changed by another participant.
            // Only our exact prior mask, or a missing mask, is replaceable.
            if (present && !EqualRgn(current.value, expectedHostRegion.value))
                throw hresult_error(E_ACCESSDENIED);
            if (!changedShape && !present && hostRegionRecoveryUsed)
                throw hresult_error(E_FAIL);
        } else if (present != hadOriginalRegion ||
            (present && !EqualRgn(current.value, originalRegion.value))) {
            throw hresult_error(E_ACCESSDENIED);
        }
        // WM_WINDOWPOSCHANGED may already have installed this exact mask.
        // Avoid another non-client recalculation during capture rebuild.
        if (hostRegionInstalled && present && !changedShape && !hostRegionRecoveryPending) return;
        Region remembered(CreateRectRgn(0, 0, 0, 0));
        if (!remembered.value) throw hresult_error(last_error());
        if (CombineRgn(remembered.value, region.value, nullptr, RGN_COPY) == ERROR)
            throw hresult_error(E_FAIL);
        trace_host_layout("region.before");
        require_bool(SetWindowRgn(host, region.value, FALSE));
        region.value = nullptr;
        if (changedShape) hostRegionRecoveryUsed = false;
        if (hostRegionRecoveryPending || (hostRegionInstalled && !changedShape && !present))
            hostRegionRecoveryUsed = true;
        hostRegionInstalled = true;
        std::swap(expectedHostRegion.value, remembered.value);
        Region verified;
        // Reinstalling is only successful after an exact readback; a second
        // missing mask here fails instead of queuing another recovery.
        if (!query_host_region(verified) || !EqualRgn(verified.value, expectedHostRegion.value))
            throw hresult_error(E_FAIL);
        hostRegionRecoveryPending = false;
        trace_host_layout("region.after");
    }

    void install_region(HWND window) {
        if (window == host) {
            install_host_region(bounds, hostBounds, configuredDpi);
            return;
        }
        Region region(auxiliary_region());
        require_bool(SetWindowRgn(window, region.value, FALSE));
        region.value = nullptr;
    }

    void recover_missing_region() noexcept {
        if (synchronizing || tearingDown || !hostRegionInstalled || !expectedHostRegion.value ||
            hostRegionRecoveryPending || hostRegionRecoveryUsed) {
            fail(E_FAIL);
            return;
        }
        hostRegionRecoveryPending = true;
        // Unwind the current frame before stopping capture. No old texture or
        // auxiliary window survives into the queued, fully checked rebuild.
        suspend();
        if (qaSource) {
            std::fprintf(stderr, "[native-glass-region-recovery] renderer=%llu queued=1 size=[%u,%u]\n",
                static_cast<unsigned long long>(identity), width, height);
            std::fflush(stderr);
        }
        if (!PostMessageW(host, reconcileMessage, identity, 0)) fail(last_error());
    }

    void suspend_for_host_change(const char* reason) noexcept {
        if (failed || tearingDown) return;
        // A move/minimize can win the race with an already queued frame. Unwind
        // that frame before retiring its helpers, without making an ordinary
        // window transition a terminal renderer failure.
        suspend();
        const bool resume = active && hasState && own_window(host) &&
            IsWindowVisible(host) && !IsIconic(host);
        if (qaSource) {
            std::fprintf(stderr,
                "[native-glass-host-transition] renderer=%llu reason=%s suspended=1 requeue=%d\n",
                static_cast<unsigned long long>(identity), reason, resume);
            std::fflush(stderr);
        }
        // Hidden/minimized windows wait for the real SHOW/WINDOWPOS message.
        // Visible geometry changes queue one rebuild with a fresh snapshot.
        if (resume && !PostMessageW(host, reconcileMessage, identity, 0)) fail(last_error());
    }

    void request_reconcile() noexcept {
        if (failed || tearingDown) return;
        if (synchronizing || resizeSizeGate.waiting) {
            reconcileDeferred = true;
            return;
        }
        if (!PostMessageW(host, reconcileMessage, identity, 0)) fail(last_error());
    }

    void stop_scope_recovery_timer() noexcept {
        if (scopeRecoveryTimerArmed && host) KillTimer(host, scopeRecoveryTimerId);
        scopeRecoveryTimerArmed = false;
    }

    void arm_scope_recovery_timer() {
        if (!scopeRecoveryTimerArmed && active && hasState && owns_host() &&
            IsWindowVisible(host) && !IsIconic(host)) {
            // This host timer survives stop_capture(), which revokes the old
            // capture's event hooks and Dispatch. Never wait for that dead
            // capture's FrameArrived to discover that an obstruction is gone.
            require_bool(SetTimer(host, scopeRecoveryTimerId, 250, nullptr) != 0);
            scopeRecoveryTimerArmed = true;
        }
    }

    void suspend_for_scene(const char* reason) noexcept {
        if (failed || tearingDown) return;
        scopeRecoveryPending = true;
        suspend();
        if (qaSource) {
            std::fprintf(stderr, "[native-glass-scope-recovery] renderer=%llu reason=%s suspended=1 sampled_output=0\n",
                static_cast<unsigned long long>(identity), reason);
            std::fflush(stderr);
        }
        try { arm_scope_recovery_timer(); }
        catch (...) { fail(winrt::to_hresult()); }
    }

    void retry_scene_scope() {
        if (!scopeRecoveryPending || failed || tearingDown || !active || !hasState) {
            stop_scope_recovery_timer();
            return;
        }
        if (synchronizing || resizeSizeGate.waiting) return;
        if (!owns_host()) throw hresult_error(E_ACCESSDENIED);
        if (!IsWindowVisible(host) || IsIconic(host)) {
            stop_scope_recovery_timer();
            return;
        }
        check_hresult(device->GetDeviceRemovedReason());
        const RECT currentCrop = client_bounds();
        ensure_fixture(&currentCrop);
        Scope scope{this, &currentCrop};
        phase = "scope.recovery_preflight";
        require_bool(EnumWindows(enumerate, reinterpret_cast<LPARAM>(&scope)));
        if (!scope.reachedHost || (qaSource && !scope.reachedSource)) throw hresult_error(E_ACCESSDENIED);
        if (qaSource && scope.blocked) return;
        // This only authorizes trying a fresh session. begin_capture and every
        // new frame still validate ownership, masks, crop, order and exclusions.
        // No previous raw texture, frame timestamp or output is revived.
        scopeRecoveryPending = false;
        stop_scope_recovery_timer();
        trace_output("scope.retry_fresh_capture");
        request_reconcile();
    }

    void display_changed() noexcept {
        ++displayEpoch;
        forget_resize_background();
        detach_output();
        report(0);
        // Display changes can reenter while COM is committing or capture is
        // being rebuilt. Hide auxiliary pixels immediately, but unwind before
        // releasing resources that the interrupted operation may still use.
        if (IsWindow(helper)) ShowWindow(helper, SW_HIDE);
        if (IsWindow(cover)) ShowWindow(cover, SW_HIDE);
        if (failed || tearingDown) return;
        if (!synchronizing) suspend();
        request_reconcile();
    }

    void restore_region() noexcept {
        if (!hostRegionInstalled || !originalRegionSaved || !IsWindow(host)) return;
        try {
            Region current;
            if (query_host_region(current) && !EqualRgn(current.value, expectedHostRegion.value)) return;
            bool restored = false;
            if (hadOriginalRegion) {
                Region copy(CreateRectRgn(0, 0, 0, 0));
                if (copy.value && CombineRgn(copy.value, originalRegion.value, nullptr, RGN_COPY) != ERROR &&
                    SetWindowRgn(host, copy.value, TRUE)) {
                    copy.value = nullptr;
                    restored = true;
                }
            } else {
                restored = SetWindowRgn(host, nullptr, TRUE) != 0;
            }
            if (restored) hostRegionInstalled = false;
            else if (qaSource) {
                std::fprintf(stderr, "[native-glass-region-restore] renderer=%llu failed=1\n",
                    static_cast<unsigned long long>(identity));
                std::fflush(stderr);
            }
        } catch (...) { /* Do not replace an unreadable or externally changed mask. */ }
    }
    void initialize() {
        phase = "initialize.host_identity";
        check_thread();
        if (!frameMessage || !reconcileMessage || !own_window(host) ||
            GetWindowThreadProcessId(host, nullptr) != thread || GetAncestor(host, GA_ROOT) != host ||
            GetWindow(host, GW_OWNER)) throw hresult_error(E_INVALIDARG);
        phase = "initialize.fixture_identity";
        if (qaSource && (!own_window(qaSource) || qaSource == host ||
            GetAncestor(qaSource, GA_ROOT) != qaSource)) throw hresult_error(E_ACCESSDENIED);
        DWORD_PTR existing = 0;
        phase = "initialize.duplicate_subclass";
        if (GetWindowSubclass(host, host_proc, subclassId, &existing))
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        DWORD affinity = 0;
        phase = "initialize.display_affinity";
        require_bool(GetWindowDisplayAffinity(host, &affinity));
        if (affinity != WDA_NONE) throw hresult_error(E_ACCESSDENIED);
        phase = "initialize.helper_class";
        register_helper_class();
        phase = "initialize.save_region";
        save_region();
        phase = "initialize.host_subclass";
        require_bool(SetWindowSubclass(host, host_proc, subclassId, reinterpret_cast<DWORD_PTR>(this)));
        subclassInstalled = true;
        phase = "initialize.load_dwm";
        dwm = LoadLibraryW(L"dwmapi.dll");
        if (!dwm) throw hresult_error(last_error());
        createScene = reinterpret_cast<CreateScene>(GetProcAddress(dwm, MAKEINTRESOURCEA(163)));
        updateScene = reinterpret_cast<UpdateScene>(GetProcAddress(dwm, MAKEINTRESOURCEA(164)));
        phase = "initialize.scene_api_support";
        if (!createScene || !updateScene || !GraphicsCaptureSession::IsSupported())
            throw hresult_error(E_NOINTERFACE);
        phase = "initialize.d3d_device";
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.put(), nullptr, context.put()));
        phase = "initialize.scene_mip_support";
        UINT formatSupport = 0;
        check_hresult(device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &formatSupport));
        constexpr UINT requiredFormatSupport = D3D11_FORMAT_SUPPORT_MIP_AUTOGEN |
            D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        if ((formatSupport & requiredFormatSupport) != requiredFormatSupport)
            throw hresult_error(DXGI_ERROR_UNSUPPORTED);
        const auto dxgi = device.as<IDXGIDevice>();
        phase = "initialize.dcomp_device";
        check_hresult(DCompositionCreateDevice3(dxgi.get(), __uuidof(IDCompositionDesktopDevice),
            composition.put_void()));
        // The existing WUC target at this same layer must be released by Rust.
        phase = "initialize.host_dcomp_target";
        check_hresult(composition->CreateTargetForHwnd(host, FALSE, hostTarget.put()));
        phase = "initialize.host_visual";
        check_hresult(composition->CreateVisual(hostVisual.put()));
        phase = "initialize.background_visual";
        check_hresult(composition->CreateVisual(backgroundVisual.put()));
        phase = "initialize.root_visual";
        check_hresult(composition->CreateVisual(rootVisual.put()));
        phase = "initialize.solid_visual";
        check_hresult(composition->CreateVisual(solidVisual.put()));
        // One source-free pixel covers every supported client size before any
        // resize begins. HWND clipping limits visibility; no 8192-square texture
        // is allocated and no resize-time transform/Commit is necessary.
        const float extent = static_cast<float>(tiez_glass_detail::solid_background_extent);
        const D2D_MATRIX_3X2_F solidTransform{extent, 0, 0, extent, 0, 0};
        check_hresult(solidVisual->SetTransform(solidTransform));
        check_hresult(solidVisual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR));
        check_hresult(rootVisual->AddVisual(solidVisual.get(), FALSE, nullptr));
        check_hresult(rootVisual->AddVisual(backgroundVisual.get(), TRUE, solidVisual.get()));
        check_hresult(rootVisual->AddVisual(hostVisual.get(), TRUE, backgroundVisual.get()));
        phase = "initialize.vertex_shader";
        check_hresult(device->CreateVertexShader(g_glass_renderer_vs, sizeof(g_glass_renderer_vs), nullptr, vertex.put()));
        phase = "initialize.blur_shader";
        check_hresult(device->CreatePixelShader(g_glass_renderer_blur_ps, sizeof(g_glass_renderer_blur_ps), nullptr, blur.put()));
        phase = "initialize.optics_shader";
        check_hresult(device->CreatePixelShader(g_glass_renderer_optics_ps, sizeof(g_glass_renderer_optics_ps), nullptr, optics.put()));
        phase = "initialize.prefix_shaders";
        check_hresult(device->CreatePixelShader(g_glass_renderer_prefix_init_ps, sizeof(g_glass_renderer_prefix_init_ps), nullptr, prefixInit.put()));
        check_hresult(device->CreatePixelShader(g_glass_renderer_prefix_glass_ps, sizeof(g_glass_renderer_prefix_glass_ps), nullptr, prefixGlass.put()));
        check_hresult(device->CreatePixelShader(g_glass_renderer_prefix_track_ps, sizeof(g_glass_renderer_prefix_track_ps), nullptr, prefixTrack.put()));
        check_hresult(device->CreatePixelShader(g_glass_renderer_prefix_finish_ps, sizeof(g_glass_renderer_prefix_finish_ps), nullptr, prefixFinish.put()));
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(ShaderState);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        phase = "initialize.constant_buffer";
        check_hresult(device->CreateBuffer(&buffer, nullptr, constants.put()));
        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        phase = "initialize.sampler";
        check_hresult(device->CreateSamplerState(&sampling, sampler.put()));
        com_ptr<IInspectable> inspectable;
        phase = "initialize.winrt_device";
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        captureDevice = inspectable.as<IDirect3DDevice>();
        phase = "initialize.complete";
        report(0);
    }

    RECT client_bounds() const {
        RECT client{};
        require_bool(GetClientRect(host, &client));
        POINT origin{client.left, client.top};
        require_bool(ClientToScreen(host, &origin));
        OffsetRect(&client, origin.x, origin.y);
        return client;
    }

    bool project_webview_popup(HWND window) {
        if (!qaSource) return false;
        // This exception is scoped to the observed nonactivating WebView2
        // tooltip in the isolated QA process. Class/name alone is insufficient.
        constexpr DWORD popupFlags = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        constexpr DWORD popupExtendedFlags = WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_TOPMOST;
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const DWORD extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        if ((style & popupFlags) != popupFlags || (extended & popupExtendedFlags) != popupExtendedFlags) return false;
        wchar_t name[128]{};
        if (!GetClassNameW(window, name, static_cast<int>(std::size(name))) ||
            wcscmp(name, L"Chrome_WidgetWin_1") != 0) return false;
        DWORD process = 0;
        if (!GetWindowThreadProcessId(window, &process) || !process || process == GetCurrentProcessId()) return false;
        const auto cached = qaPopupProcesses.find(process);
        if (cached != qaPopupProcesses.end()) {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (WaitForSingleObject(cached->second->handle, 0) == WAIT_TIMEOUT &&
                GetProcessTimes(cached->second->handle, &created, &exited, &kernel, &user) &&
                CompareFileTime(&created, &cached->second->created) == 0) return true;
            qaPopupProcesses.erase(cached);
        }
        auto proof = std::make_unique<PopupProcess>();
        proof->handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process);
        if (!proof->handle || WaitForSingleObject(proof->handle, 0) != WAIT_TIMEOUT) return false;
        FILETIME exited{}, kernel{}, user{}, parentCreated{};
        if (!GetProcessTimes(proof->handle, &proof->created, &exited, &kernel, &user) ||
            !GetProcessTimes(GetCurrentProcess(), &parentCreated, &exited, &kernel, &user) ||
            CompareFileTime(&proof->created, &parentCreated) < 0) return false;
        std::wstring image(32768, L'\0');
        DWORD imageLength = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(proof->handle, 0, image.data(), &imageLength)) return false;
        image.resize(imageLength);
        if (!webview_image_name(image)) return false;
        const HANDLE rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (rawSnapshot == INVALID_HANDLE_VALUE) return false;
        winrt::handle snapshot{rawSnapshot};
        PROCESSENTRY32W entry{sizeof(entry)};
        bool directChild = false;
        if (Process32FirstW(snapshot.get(), &entry)) {
            do {
                if (entry.th32ProcessID == process) {
                    directChild = entry.th32ParentProcessID == GetCurrentProcessId();
                    break;
                }
            } while (Process32NextW(snapshot.get(), &entry));
        }
        // Pinning and rechecking the live process prevents snapshot/PID reuse
        // races. The creation-time ordering also rejects a reused parent PID.
        if (!directChild || WaitForSingleObject(proof->handle, 0) != WAIT_TIMEOUT) return false;
        const FILETIME created = proof->created;
        qaPopupProcesses.emplace(process, std::move(proof));
        std::fprintf(stderr,
            "[native-glass-project-popup] hwnd=%p pid=%lu direct_parent=%lu image=msedgewebview2.exe "
            "created=0x%08lX%08lX class=Chrome_WidgetWin_1 nonactivating=1 excluded_from_scene=1\n",
            static_cast<void*>(window), process, GetCurrentProcessId(), created.dwHighDateTime, created.dwLowDateTime);
        std::fflush(stderr);
        return true;
    }

    struct Scope {
        TiezGlass* renderer;
        const RECT* crop = nullptr;
        std::vector<HWND> exclude;
        bool reachedHost = false, reachedCover = false, reachedHelper = false, reachedSource = false;
        bool correctOrder = true, blocked = false;
        bool hostAboveCover = false, coverAboveHelper = false;
        LONG nextOrder = 0, hostOrder = -1, coverOrder = -1, helperOrder = -1, sourceOrder = -1;
        HWND firstBlocker = nullptr;
        HWND previousWindow = nullptr;
        tiez_glass_detail::ZOrderSnapshot order{};
        DWORD blockerProcess = 0, blockerStyle = 0, blockerExtendedStyle = 0, blockerCloaked = 0;
        DWORD blockerClassError = 0;
        int blockerRegionType = -1;
        DWORD blockerRegionError = 0;
        HRESULT blockerCloakResult = S_OK;
        RECT blockerBounds{}, blockerRegionBounds{};
        bool blockerRootIsHost = false, blockerRootIsFixture = false, blockerOwnerIsHost = false;
        wchar_t blockerClass[128]{};
    };

    static BOOL CALLBACK enumerate(HWND window, LPARAM data) {
        auto& scope = *reinterpret_cast<Scope*>(data);
        auto& renderer = *scope.renderer;
        const LONG order = scope.nextOrder++;
        if (window == renderer.host) {
            scope.reachedHost = true;
            scope.hostOrder = order;
            scope.order.host = window;
            scope.order.previous = scope.previousWindow;
            scope.order.topmost = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            scope.order.previous_topmost = scope.previousWindow &&
                (GetWindowLongPtrW(scope.previousWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        }
        if (scope.previousWindow == renderer.host) scope.order.next = window;
        scope.previousWindow = window;
        if (window == renderer.cover) {
            scope.hostAboveCover = scope.reachedHost;
            scope.correctOrder = scope.correctOrder && scope.reachedHost;
            scope.reachedCover = true;
            scope.coverOrder = order;
        }
        if (window == renderer.helper) {
            scope.coverAboveHelper = scope.reachedCover;
            scope.correctOrder = scope.correctOrder && scope.reachedCover;
            scope.reachedHelper = true;
            scope.helperOrder = order;
        }
        if (window == renderer.qaSource) {
            scope.reachedSource = true;
            scope.sourceOrder = order;
        }
        const bool own = own_window(window);
        const bool group = window == renderer.host || window == renderer.helper || window == renderer.cover;
        const bool fixture = renderer.qaSource && (window == renderer.qaSource ||
            GetAncestor(window, GA_ROOTOWNER) == renderer.qaSource);
        // In production the scene represents windows below this unowned host.
        // The private API can treat owner groups together; include their roots
        // in exclusions, never change somebody else's owner relationships.
        if (group || (renderer.qaSource ? !fixture : !scope.reachedHost)) {
            scope.exclude.push_back(window);
            const HWND owner = GetAncestor(window, GA_ROOTOWNER);
            if (owner && owner != renderer.qaSource) scope.exclude.push_back(owner);
        }
        if (renderer.qaSource && !own && !scope.reachedSource && IsWindowVisible(window) &&
            !IsIconic(window) && !cursor_overlay(window)) {
            RECT rectangle{}, intersection{};
            DWORD cloaked = 0;
            if (GetWindowRect(window, &rectangle) &&
                IntersectRect(&intersection, &rectangle, scope.crop ? scope.crop : &renderer.bounds)) {
                const HRESULT cloakResult = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
                if (FAILED(cloakResult) || !cloaked) {
                    if (renderer.project_webview_popup(window)) {
                        // Verified foreground still never enters the sampled
                        // scene; only the false foreign-blocker result changes.
                        scope.exclude.push_back(window);
                        const HWND root = GetAncestor(window, GA_ROOTOWNER);
                        if (root && root != renderer.qaSource) scope.exclude.push_back(root);
                        return TRUE;
                    }
                    scope.blocked = true;
                    if (!scope.firstBlocker) {
                        scope.firstBlocker = window;
                        scope.blockerBounds = rectangle;
                        scope.blockerCloaked = cloaked;
                        scope.blockerCloakResult = cloakResult;
                        GetWindowThreadProcessId(window, &scope.blockerProcess);
                        scope.blockerStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
                        scope.blockerExtendedStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
                        const HWND root = GetAncestor(window, GA_ROOTOWNER);
                        scope.blockerRootIsHost = root == renderer.host;
                        scope.blockerRootIsFixture = root == renderer.qaSource;
                        scope.blockerOwnerIsHost = GetWindow(window, GW_OWNER) == renderer.host;
                        // Diagnostic only: ERROR is not an empty region. Keep
                        // the existing block decision regardless of this read.
                        SetLastError(ERROR_SUCCESS);
                        scope.blockerRegionType = GetWindowRgnBox(window, &scope.blockerRegionBounds);
                        scope.blockerRegionError = GetLastError();
                        SetLastError(ERROR_SUCCESS);
                        if (!GetClassNameW(window, scope.blockerClass, static_cast<int>(std::size(scope.blockerClass))))
                            scope.blockerClassError = GetLastError();
                    }
                }
            }
        }
        return TRUE;
    }

    void diagnose_scope(const Scope& scope) const noexcept {
        if (!qaSource) return;
        std::fprintf(stderr,
            "[native-glass-scope] reached_host=%d reached_cover=%d reached_helper=%d reached_source=%d "
            "correct_order=%d host_above_cover=%d cover_above_helper=%d blocked=%d "
            "host_z=%ld cover_z=%ld helper_z=%ld source_z=%ld enumerated=%ld "
            "host=%p cover=%p helper=%p source=%p\n",
            scope.reachedHost, scope.reachedCover, scope.reachedHelper, scope.reachedSource,
            scope.correctOrder, scope.hostAboveCover, scope.coverAboveHelper, scope.blocked,
            scope.hostOrder, scope.coverOrder, scope.helperOrder, scope.sourceOrder, scope.nextOrder,
            static_cast<void*>(host), static_cast<void*>(cover), static_cast<void*>(helper), static_cast<void*>(qaSource));
        if (scope.firstBlocker) {
            char className[512]{};
            WideCharToMultiByte(CP_UTF8, 0, scope.blockerClass, -1, className, static_cast<int>(sizeof(className)), nullptr, nullptr);
            for (char& character : className)
                if (character && (static_cast<unsigned char>(character) < 0x20 || character == '"')) character = '?';
            // Only metadata of the first rejected overlapping window; no
            // titles, UI text, process command lines or screen contents.
            std::fprintf(stderr,
                "[native-glass-blocker] hwnd=%p pid=%lu class=\"%s\" class_error=%lu "
                "style=0x%08lX exstyle=0x%08lX rect=[%ld,%ld,%ld,%ld] "
                "cloak_hr=0x%08lX cloaked=%lu root_is_host=%d root_is_fixture=%d owner_is_host=%d "
                "region_box_type=%d region_box_error=%lu region_box_window=[%ld,%ld,%ld,%ld]\n",
                static_cast<void*>(scope.firstBlocker), scope.blockerProcess, className, scope.blockerClassError,
                scope.blockerStyle, scope.blockerExtendedStyle, scope.blockerBounds.left, scope.blockerBounds.top,
                scope.blockerBounds.right, scope.blockerBounds.bottom, static_cast<unsigned long>(scope.blockerCloakResult),
                scope.blockerCloaked, scope.blockerRootIsHost, scope.blockerRootIsFixture, scope.blockerOwnerIsHost,
                scope.blockerRegionType, scope.blockerRegionError, scope.blockerRegionBounds.left,
                scope.blockerRegionBounds.top, scope.blockerRegionBounds.right, scope.blockerRegionBounds.bottom);
        }
        std::fflush(stderr);
    }

    RECT read_virtual_desktop() const {
        const int64_t left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int64_t desktopWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN), desktopHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const int64_t right = left + desktopWidth, bottom = top + desktopHeight;
        if (desktopWidth <= 0 || desktopHeight <= 0 ||
            right > std::numeric_limits<LONG>::max() || bottom > std::numeric_limits<LONG>::max())
            throw hresult_error(E_BOUNDS);
        return {static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right), static_cast<LONG>(bottom)};
    }

    bool can_translate(const RECT& client, const RECT& outer, UINT dpi, const RECT& desktop) const {
        const bool eligible = !qaCoincidentAuxiliary && session && pool && dispatch && dispatch->alive.load() &&
            !dispatch->captureClosed.load() && validTexture && nativeStarted &&
            active && !failed && displayEpoch == configuredDisplayEpoch && dpi && dpi == configuredDpi &&
            EqualRect(&desktop, &virtualDesktopBounds) &&
            client.right - client.left == static_cast<LONG>(width) &&
            client.bottom - client.top == static_cast<LONG>(height) &&
            outer.right - outer.left == hostBounds.right - hostBounds.left &&
            outer.bottom - outer.top == hostBounds.bottom - hostBounds.top &&
            client.left - outer.left == bounds.left - hostBounds.left &&
            client.top - outer.top == bounds.top - hostBounds.top;
        if (!eligible) return false;
        if (capture_supports_client(client)) return true;
        const RECT next = capture_bounds_for(client);
        return next.right - next.left == static_cast<LONG>(captureWidth) &&
            next.bottom - next.top == static_cast<LONG>(captureHeight);
    }

    bool position_change_can_retarget(const WINDOWPOS& position) const {
        RECT actualOuter{};
        return !qaCoincidentAuxiliary && session && validTexture && nativeStarted && active && !failed &&
            IsWindowVisible(host) && !IsIconic(host) && (position.flags & SWP_NOZORDER) &&
            !(position.flags & (SWP_HIDEWINDOW | SWP_FRAMECHANGED)) &&
            GetWindowRect(host, &actualOuter) &&
            tiez_glass_detail::capture_viewport_can_translate(pixel_rect(hostBounds), pixel_rect(actualOuter),
                position.cx, position.cy, (position.flags & SWP_NOSIZE) != 0,
                viewportResizePending || resizeSizeGate.waiting);
    }

    void ensure_host_snapshot() {
        phase = "scope.host_identity_owner";
        if (!own_window(host) || GetWindowThreadProcessId(host, nullptr) != thread ||
            GetAncestor(host, GA_ROOT) != host || GetWindow(host, GW_OWNER))
            throw hresult_error(E_ACCESSDENIED);
        phase = "scope.host_visibility";
        if (!IsWindowVisible(host) || IsIconic(host)) throw HostStateChanged{"visibility"};
        const RECT client = client_bounds();
        RECT outer{};
        phase = "scope.host_outer_rect";
        require_bool(GetWindowRect(host, &outer));
        const UINT dpi = GetDpiForWindow(host);
        if (dpi && dpi != configuredDpi) throw HostStateChanged{"dpi"};
        const RECT desktop = read_virtual_desktop();
        if (displayEpoch != configuredDisplayEpoch || !EqualRect(&desktop, &virtualDesktopBounds))
            throw HostStateChanged{"display_layout"};
        if (!EqualRect(&client, &bounds) || !EqualRect(&outer, &hostBounds)) {
            if (can_translate(client, outer, dpi, desktop)) throw HostTranslationPending{};
            throw HostStateChanged{"geometry"};
        }
    }

    bool auxiliary_outside_desktop() const {
        RECT intersection{};
        return !IntersectRect(&intersection, &virtualDesktopBounds, &auxiliaryBounds);
    }

    HRGN auxiliary_region() const {
        // Validate the host mask independently of the rectangular capture.
        Region hostMask(client_region());
        if (qaCoincidentAuxiliary) {
            const HRGN result = hostMask.value;
            hostMask.value = nullptr;
            return result;
        }
        const HRGN result = CreateRectRgn(0, 0, static_cast<int>(captureWidth), static_cast<int>(captureHeight));
        if (!result) throw hresult_error(last_error());
        return result;
    }

    void validate_auxiliary_windows(bool visible) {
        ensure_host_snapshot();
        phase = "scope.auxiliary_placement";
        if (qaCoincidentAuxiliary ? !EqualRect(&auxiliaryBounds, &bounds) : !auxiliary_outside_desktop())
            throw hresult_error(E_ACCESSDENIED);
        phase = "scope.host_region_in_client_coordinates";
        Region expected(auxiliary_region());
        for (HWND window : {helper, cover}) {
            phase = window == helper ? "scope.helper_geometry_region" : "scope.cover_geometry_region";
            RECT rectangle{};
            Region actual(CreateRectRgn(0, 0, 0, 0));
            if (!own_window(window) || (IsWindowVisible(window) != FALSE) != visible || IsIconic(window) ||
                !GetWindowRect(window, &rectangle) || !EqualRect(&rectangle, &auxiliaryBounds) ||
                !actual.value || GetWindowRgn(window, actual.value) == ERROR ||
                !EqualRgn(expected.value, actual.value)) throw hresult_error(E_ACCESSDENIED);
        }
        phase = "scope.helper_cover_owners";
        if (GetWindow(helper, GW_OWNER) || GetWindow(cover, GW_OWNER) != helper)
            throw hresult_error(E_ACCESSDENIED);
        ensure_host_snapshot();
    }

    void ensure_fixture(const RECT* crop = nullptr) {
        if (qaSource) {
            const RECT& sampled = crop ? *crop : captureBounds;
            phase = "scope.fixture_coverage";
            RECT fixture{};
            DWORD cloaked = 0;
            if (!own_window(qaSource) || GetAncestor(qaSource, GA_ROOT) != qaSource ||
                !IsWindowVisible(qaSource) || IsIconic(qaSource) ||
                (GetWindowLongPtrW(qaSource, GWL_EXSTYLE) & WS_EX_LAYERED) ||
                !GetWindowRect(qaSource, &fixture) || fixture.left > sampled.left ||
                fixture.top > sampled.top || fixture.right < sampled.right || fixture.bottom < sampled.bottom ||
                FAILED(DwmGetWindowAttribute(qaSource, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked)
                throw hresult_error(E_ACCESSDENIED);
            phase = "scope.fixture_region";
            Region fixtureRegion(CreateRectRgn(0, 0, 0, 0));
            if (!fixtureRegion.value) throw hresult_error(last_error());
            const int kind = GetWindowRgn(qaSource, fixtureRegion.value);
            if (kind != ERROR) {
                RECT fixtureCrop{sampled.left - fixture.left, sampled.top - fixture.top,
                    sampled.right - fixture.left, sampled.bottom - fixture.top};
                Region required(CreateRectRgnIndirect(&fixtureCrop));
                Region uncovered(CreateRectRgn(0, 0, 0, 0));
                if (!required.value || !uncovered.value ||
                    CombineRgn(uncovered.value, required.value, fixtureRegion.value, RGN_DIFF) != NULLREGION)
                    throw hresult_error(E_ACCESSDENIED);
            }
        }
    }

    std::vector<HWND> ensure_scope(tiez_glass_detail::ZOrderSnapshot* order = nullptr) {
        ensure_host_snapshot();
        ensure_fixture();
        validate_auxiliary_windows(true);
        Scope scope{this, &captureBounds};
        phase = "scope.enumerate_z_order";
        require_bool(EnumWindows(enumerate, reinterpret_cast<LPARAM>(&scope)));
        phase = "scope.z_order_and_blockers";
        if (!scope.reachedHost || !scope.reachedCover || !scope.reachedHelper ||
            (qaSource && !scope.reachedSource)) {
            diagnose_scope(scope);
            throw hresult_error(E_ACCESSDENIED);
        }
        if (!scope.correctOrder || (qaSource && scope.blocked)) {
            diagnose_scope(scope);
            throw SceneTemporarilyUnavailable{scope.blocked ? "fixture_obstructed" : "scene_order_changed"};
        }
        std::sort(scope.exclude.begin(), scope.exclude.end(), [](HWND a, HWND b) {
            return reinterpret_cast<UINT_PTR>(a) < reinterpret_cast<UINT_PTR>(b);
        });
        scope.exclude.erase(std::unique(scope.exclude.begin(), scope.exclude.end()), scope.exclude.end());
        if (order) *order = scope.order;
        return scope.exclude;
    }

    static bool EqualRectValue(const RECT& a, const RECT& b) noexcept { return EqualRect(&a, &b) != FALSE; }

    static tiez_glass_detail::PixelRect pixel_rect(const RECT& rectangle) noexcept {
        return {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
    }

    static RECT native_rect(const tiez_glass_detail::PixelRect& rectangle) noexcept {
        return {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
    }

    RECT available_capture_bounds() const {
        RECT available = virtualDesktopBounds;
        if (qaSource) {
            RECT fixture{};
            POINT origin{};
            require_bool(GetClientRect(qaSource, &fixture));
            require_bool(ClientToScreen(qaSource, &origin));
            OffsetRect(&fixture, origin.x, origin.y);
            RECT clipped{};
            if (!IntersectRect(&clipped, &available, &fixture)) throw hresult_error(E_ACCESSDENIED);
            available = clipped;
        }
        return available;
    }

    float capture_sigma_for(const RECT& client) const {
        float materialScale = scale;
        const auto clientWidth = static_cast<UINT>(client.right - client.left);
        const auto clientHeight = static_cast<UINT>(client.bottom - client.top);
        if (tiez_glass_detail::optics_ready(true, clientWidth, clientHeight,
            parameters.viewport_width, parameters.viewport_height, scale)) {
            materialScale = std::max(scale, std::min(clientWidth / parameters.viewport_width,
                clientHeight / parameters.viewport_height));
        }
        // Cover the normal clarity range without resizing capture for every
        // slider update. Larger validated blur requests reserve their own halo.
        return std::max(16.0f, parameters.blur_radius) * materialScale;
    }

    RECT capture_bounds_for(const RECT& client) const {
        if (qaCoincidentAuxiliary) return client;
        const RECT available = available_capture_bounds();
        tiez_glass_detail::PixelRect result{};
        if (!tiez_glass_detail::capture_region(pixel_rect(client), pixel_rect(available),
            tiez_glass_detail::capture_padding(scale, capture_sigma_for(client)), result)) throw hresult_error(E_BOUNDS);
        return native_rect(result);
    }

    bool capture_supports_client(const RECT& client) const {
        return !qaCoincidentAuxiliary && tiez_glass_detail::capture_supports(pixel_rect(captureBounds),
            pixel_rect(client), pixel_rect(available_capture_bounds()), capture_sigma_for(client));
    }

    tiez_glass_detail::ResizeContext resize_context() const {
        constexpr DWORD frameStyle = WS_CAPTION | WS_THICKFRAME | WS_CHILD | WS_POPUP | WS_MINIMIZE | WS_MAXIMIZE;
        constexpr DWORD frameExtendedStyle = WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
            WS_EX_DLGMODALFRAME | WS_EX_LAYOUTRTL;
        return {reinterpret_cast<uintptr_t>(qaSource),
            reinterpret_cast<uintptr_t>(qaSource ? GetWindow(qaSource, GW_OWNER) : nullptr),
            reinterpret_cast<uintptr_t>(MonitorFromWindow(host, MONITOR_DEFAULTTONEAREST)),
            GetDpiForWindow(host), displayEpoch, pixel_rect(read_virtual_desktop()),
            static_cast<DWORD>(GetWindowLongPtrW(host, GWL_STYLE)) & frameStyle,
            static_cast<DWORD>(GetWindowLongPtrW(host, GWL_EXSTYLE)) & frameExtendedStyle};
    }

    std::vector<HWND> external_exclusions(const std::vector<HWND>& windows) const {
        std::vector<HWND> result;
        for (const HWND window : windows)
            if (window != helper && window != cover) result.push_back(window);
        return result;
    }

    bool reusable_background(const std::vector<HWND>& scope) {
        if (qaSource && capturedScene && !tiez_glass_detail::same_rect(lastCapture.crop, pixel_rect(captureBounds))) {
            const RECT cachedCrop = native_rect(lastCapture.crop);
            ensure_fixture(&cachedCrop);
            Scope cached{this, &cachedCrop};
            require_bool(EnumWindows(enumerate, reinterpret_cast<LPARAM>(&cached)));
            if (!cached.reachedHost || !cached.reachedSource || cached.blocked) return false;
        }
        Region current;
        const bool correctRegion = query_host_region(current) && expectedHostRegion.value &&
            EqualRgn(current.value, expectedHostRegion.value);
        return capturedScene && (validTexture || resizeLease.active) &&
            own_window(host) && GetWindowThreadProcessId(host, nullptr) == thread &&
            GetAncestor(host, GA_ROOT) == host && !GetWindow(host, GW_OWNER) &&
            (!qaSource || own_window(qaSource)) &&
            tiez_glass_detail::reusable_resize(lastCapture, resize_context(), resizeLease, GetTickCount64(),
                active && !failed && IsWindowVisible(host) && !IsIconic(host),
                captureExclusions == external_exclusions(scope), correctRegion,
                SUCCEEDED(device->GetDeviceRemovedReason()));
    }

    void cancel_noop_z_order() noexcept {
        if (zOrderNoop.waiting && host) KillTimer(host, zOrderNoopTimerId);
        zOrderNoop.cancel();
        zOrderNoopRenderDeferred = false;
    }

    bool noop_z_order_source(tiez_glass_detail::ZOrderSnapshot& order) {
        if (!session || !pool || !dispatch || !dispatch->alive.load() || dispatch->captureClosed.load() ||
            !active || failed || tearingDown || !validTexture || !nativeStarted || !sceneConfigured ||
            reportedState != 1 || solidOnlyOutput ||
            resizeBackground || resizeLease.active || resizeSizeGate.waiting || scopeRecoveryPending)
            return false;
        check_hresult(device->GetDeviceRemovedReason());
        try {
            const auto current = ensure_scope(&order);
            return current == exclusions && reusable_background(current);
        } catch (const HostTranslationPending&) {
            // A queued translation may normally retarget the live capture.
            // It invalidates this stricter unchanged-geometry transaction.
            throw HostStateChanged{"z_order_geometry"};
        }
    }

    uint64_t begin_noop_z_order(const WINDOWPOS& position) {
        if (synchronizing || zOrderNoop.waiting ||
            !tiez_glass_detail::pure_z_order_request(position, host)) return 0;
        tiez_glass_detail::ZOrderSnapshot order;
        if (!noop_z_order_source(order)) return 0;
        const uint64_t token = zOrderNoop.begin(position, order, GetTickCount64());
        if (!token) return 0;
        zOrderNoopEpoch = presentationEpoch.value;
        zOrderNoopCapture = dispatch->generation;
        require_bool(SetTimer(host, zOrderNoopTimerId,
            static_cast<UINT>(tiez_glass_detail::ZOrderNoopGate::lifetime_ms), nullptr) != 0);
        trace_output("z_order.noop_begin");
        return token;
    }

    bool noop_z_order_still_valid(uint64_t token, tiez_glass_detail::ZOrderSnapshot& order) {
        if (!zOrderNoop.current(token) || !zOrderNoop.within_deadline(GetTickCount64()) ||
            presentationEpoch.value != zOrderNoopEpoch || !dispatch ||
            dispatch->generation != zOrderNoopCapture || !noop_z_order_source(order)) return false;
        // Scope validation can dispatch window messages. An interrupted check
        // must not authorize a later transaction that reused these resources.
        return zOrderNoop.current(token) && zOrderNoop.within_deadline(GetTickCount64()) &&
            presentationEpoch.value == zOrderNoopEpoch &&
            dispatch && dispatch->generation == zOrderNoopCapture &&
            tiez_glass_detail::same_z_order(zOrderNoop.order, order);
    }

    void withdraw_noop_z_order() noexcept {
        trace_output("z_order.noop_withdraw");
        suspend();
        request_reconcile();
    }

    void noop_z_order_default_returned(uint64_t token, const WINDOWPOS& position) {
        if (!zOrderNoop.current(token)) return;
        tiez_glass_detail::ZOrderSnapshot order;
        if (!noop_z_order_still_valid(token, order) ||
            !zOrderNoop.after_default(token, position, order, GetTickCount64())) {
            withdraw_noop_z_order();
            return;
        }
        // An actual no-op need not produce CHANGED. This queued check runs
        // after this CHANGING returns, without extending the fixed deadline.
        request_reconcile();
    }

    bool finish_noop_z_order(const WINDOWPOS* position = nullptr) {
        const uint64_t token = zOrderNoop.generation;
        tiez_glass_detail::ZOrderSnapshot order;
        if (!noop_z_order_still_valid(token, order) ||
            !zOrderNoop.can_finish(token, order, GetTickCount64()) ||
            (position && !tiez_glass_detail::z_order_request_is_noop(*position, order, true))) {
            withdraw_noop_z_order();
            return false;
        }
        const bool renderLatestControls = zOrderNoopRenderDeferred;
        cancel_noop_z_order();
        reconcileDeferred = frameDeferred = false;
        // No helper, private scene, crop or exclusion changed. Existing queued
        // frames still belong to that same configuration; unlike a real
        // reorder, this needs neither a replacement source nor a QPC retarget.
        // Retaining the mounted output does not refresh lastCapture's time.
        trace_output("z_order.noop_retained");
        // A static capture may have no later frame. Apply any DOM/material
        // update deferred by this gate without stamping it as a new capture.
        if (renderLatestControls) render();
        queue(dispatch, true);
        return true;
    }

    void wait_for_noop_z_order() {
        tiez_glass_detail::ZOrderSnapshot order;
        if (!noop_z_order_still_valid(zOrderNoop.generation, order)) withdraw_noop_z_order();
        else frameDeferred = true;
    }

    void expire_noop_z_order() noexcept {
        if (zOrderNoop.waiting && !zOrderNoop.within_deadline(GetTickCount64())) withdraw_noop_z_order();
    }

    bool owns_host() const noexcept {
        DWORD_PTR installed = 0;
        return own_window(host) && GetWindowThreadProcessId(host, nullptr) == thread &&
            GetAncestor(host, GA_ROOT) == host && !GetWindow(host, GW_OWNER) && subclassInstalled &&
            GetWindowSubclass(host, host_proc, subclassId, &installed) &&
            installed == reinterpret_cast<DWORD_PTR>(this);
    }

    bool owns_nonclient_activation(HWND window) const noexcept {
        if (window != host || !hostRegionInstalled || !expectedHostRegion.value) return false;
        try {
            const bool ownHost = owns_host();
            if (!ownHost) return false;
            const auto currentFrame = resize_context();
            const bool sameFrame = currentFrame.dpi == configuredDpi && currentFrame.dpi != 0 &&
                currentFrame.frame_style == configuredFrameStyle &&
                currentFrame.frame_extended_style == configuredFrameExtendedStyle;
            RECT outer{};
            if (!GetWindowRect(host, &outer)) return false;
            const RECT client = client_bounds();
            // An undecorated Tao frame has at most the thin top resize inset,
            // never a native caption-height area above its client.
            const bool borderless = tiez_glass_detail::borderless_client_insets(
                pixel_rect(client), pixel_rect(outer), configuredDpi) &&
                !(currentFrame.frame_style & (WS_CHILD | WS_MINIMIZE | WS_MAXIMIZE)) &&
                !(currentFrame.frame_extended_style & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
            Region current(CreateRectRgn(0, 0, 0, 0));
            const bool ownedMask = current.value && GetWindowRgn(host, current.value) != ERROR &&
                EqualRgn(current.value, expectedHostRegion.value);
            return tiez_glass_detail::suppress_owned_nonclient_activation(active, tearingDown, ownHost,
                IsWindowVisible(host) && !IsIconic(host), sameFrame, borderless, ownedMask);
        } catch (...) { return false; }
    }

    bool owns_resize_nonclient_paint(HWND window) const noexcept {
        if (!active || !nativeStarted || !resizeBackground || failed || tearingDown ||
            !capturedScene || !resizeLease.active || !hostRegionInstalled || !expectedHostRegion.value)
            return false;
        try {
            const bool ownHost = window == host && own_window(host) &&
                GetWindowThreadProcessId(host, nullptr) == thread && GetAncestor(host, GA_ROOT) == host &&
                !GetWindow(host, GW_OWNER);
            Region current(CreateRectRgn(0, 0, 0, 0));
            const bool ownedMask = current.value && GetWindowRgn(host, current.value) != ERROR &&
                EqualRgn(current.value, expectedHostRegion.value);
            // This is only a read-only paint decision during half-completed
            // sizing messages. Do not enumerate/reconcile scope, repair a mask,
            // render, or extend the lease from here. Existing invalidation paths
            // retain sole authority over the sampled output's lifetime.
            const bool reusable = device && tiez_glass_detail::reusable_resize(lastCapture,
                resize_context(), resizeLease, GetTickCount64(), IsWindowVisible(host) && !IsIconic(host),
                true, ownedMask, SUCCEEDED(device->GetDeviceRemovedReason()));
            return tiez_glass_detail::suppress_resize_nonclient_paint(ownHost, active, nativeStarted,
                resizeBackground, failed, tearingDown, reusable, ownedMask);
        } catch (...) { return false; }
    }

    std::vector<HWND> pending_resize_scope(const RECT& client) {
        const RECT checked = capturedScene ? native_rect(tiez_glass_detail::union_rect(
            pixel_rect(client), lastCapture.crop)) : client;
        ensure_fixture(&checked);
        Scope scope{this, &checked};
        require_bool(EnumWindows(enumerate, reinterpret_cast<LPARAM>(&scope)));
        if (!scope.reachedHost || (qaSource && (!scope.reachedSource || scope.blocked)))
            throw hresult_error(E_ACCESSDENIED);
        std::sort(scope.exclude.begin(), scope.exclude.end(), [](HWND a, HWND b) {
            return reinterpret_cast<UINT_PTR>(a) < reinterpret_cast<UINT_PTR>(b);
        });
        scope.exclude.erase(std::unique(scope.exclude.begin(), scope.exclude.end()), scope.exclude.end());
        return scope.exclude;
    }

    void refresh_resize_region(const WINDOWPOS& position) {
        if (synchronizing || !resizeBackground || (session && !viewportResizePending) || !capturedScene ||
            ((position.flags & SWP_NOSIZE) && !resizeSizeGate.waiting)) return;
        SynchronizationScope synchronization(*this);
        const RECT client = client_bounds();
        RECT outer{};
        require_bool(GetWindowRect(host, &outer));
        if (!reusable_background(pending_resize_scope(client))) {
            cancel_presentation();
            return;
        }
        if (viewportResizePending && (!(position.flags & SWP_NOZORDER) ||
            (position.flags & (SWP_HIDEWINDOW | SWP_FRAMECHANGED)) || !capture_supports_client(client))) {
            suspend();
            return;
        }
        resizeSizeGate.position_changed(static_cast<UINT>(client.right - client.left),
            static_cast<UINT>(client.bottom - client.top));
        // Clip the newly exposed non-client margins before forwarding the
        // WINDOWPOSCHANGED chain. The capture/output snapshot remains old;
        // only reconciliation after WM_SIZE may replace its dimensions.
        phase = "resize.synchronous_host_region";
        install_host_region(client, outer, configuredDpi);
        align_resize_background(client);
        trace_host_layout("resize.region_current");
        if (viewportResizePending) {
            pendingViewportClient = client;
            pendingViewportOuter = outer;
        }
        // Constraints can cancel a requested resize after capture was stopped.
        // In that case USER32 need not send WM_SIZE. This also applies to the
        // rebuild path, which has no retained session or pending viewport flag.
        if ((position.flags & SWP_NOSIZE) &&
            client.right - client.left == static_cast<LONG>(width) &&
            client.bottom - client.top == static_cast<LONG>(height)) {
            resizeSizeGate.accept(resizeSizeGate.generation, width, height, width, height);
        }
    }

    bool predict_resize_region(const WINDOWPOS& position, const RECT& client, const RECT& outer) {
        tiez_glass_detail::PixelRect predictedClient{}, predictedOuter{};
        if (!tiez_glass_detail::predict_resize_client(pixel_rect(client), pixel_rect(outer),
            (position.flags & SWP_NOMOVE) ? outer.left : position.x,
            (position.flags & SWP_NOMOVE) ? outer.top : position.y, position.cx, position.cy,
            predictedClient, predictedOuter)) return false;
        const RECT nextClient{predictedClient.left, predictedClient.top, predictedClient.right, predictedClient.bottom};
        const RECT nextOuter{predictedOuter.left, predictedOuter.top, predictedOuter.right, predictedOuter.bottom};
        RECT observedOuter{};
        require_bool(GetWindowRect(host, &observedOuter));
        const RECT observedClient = client_bounds();
        if (!EqualRect(&client, &observedClient) || !EqualRect(&outer, &observedOuter) ||
            MonitorFromRect(&nextOuter, MONITOR_DEFAULTTONEAREST) != MonitorFromWindow(host, MONITOR_DEFAULTTONEAREST) ||
            !reusable_background(pending_resize_scope(nextClient))) return false;
        // The next NC paint can occur before POSITIONCHANGED. Same DPI/frame
        // properties let the measured client insets predict its owned mask.
        // This clips a shrinking window slightly early; an enlarged mask is
        // still clipped by the old HWND until Windows applies the new bounds.
        phase = "resize.predicted_host_region";
        install_host_region(nextClient, nextOuter, configuredDpi);
        trace_host_layout("resize.region_predicted");
        // No future crop or source dimensions become authoritative here.
        // POSITIONCHANGED must still validate/correct this prediction, and its
        // actual WM_SIZE must still pass the generation gate before rebuilding.
        require_bool(GetWindowRect(host, &observedOuter));
        const RECT verifiedClient = client_bounds();
        if (!EqualRect(&client, &verifiedClient) || !EqualRect(&outer, &observedOuter))
            throw HostStateChanged{"predicted_region_geometry"};
        return reusable_background(pending_resize_scope(nextClient));
    }

    void resize_size_processed(WPARAM sizeType, LPARAM size, uint64_t messageGeneration) noexcept {
        if (!resizeSizeGate.waiting || sizeType == SIZE_MINIMIZED || !own_window(host)) return;
        RECT client{};
        if (!GetClientRect(host, &client)) return;
        if (resizeSizeGate.accept(messageGeneration, LOWORD(size), HIWORD(size),
            static_cast<UINT>(client.right - client.left), static_cast<UINT>(client.bottom - client.top))) {
            trace_host_layout("resize.size_processed");
            request_reconcile();
        }
    }

    void forget_resize_background() noexcept {
        capturedScene = nullptr;
        filteredCaptureSource = nullptr;
        filteredCaptureSourceView = nullptr;
        filteredCaptureValid = false;
        captureExclusions.clear();
        viewportResizePending = false;
        resizeBackground = false;
        resizeLease = {};
        resizeSizeGate.cancel();
        if (host) KillTimer(host, resizeWatchdogId);
    }

    bool can_retain_solid() const noexcept {
        return tiez_glass_detail::retain_solid_on_withdrawal(active, tearingDown, owns_host(),
            IsWindowVisible(host) && !IsIconic(host), device && SUCCEEDED(device->GetDeviceRemovedReason()),
            solidSurface && solidVisual && rootVisual && hostTarget);
    }

    void detach_output(bool retainSolid = true) noexcept {
        initialAlphaPending = false;
        KillTimer(helper, initialAlphaRetryTimerId);
        pendingPresent = {};
        awaitingMoveCapture = false;
        retargetFlushPending = false;
        KillTimer(host, presentRetryTimerId);
        cancel_noop_z_order();
        presentationEpoch.invalidate();
        nativeStarted = false;
        foregroundSwapMounted = false;
        // The old swap chain contains optics as well as captured pixels. Both
        // sampled layers must leave in the same batch; only the parameter-only
        // solid may remain while the asynchronous frontend fallback catches up.
        const HRESULT clearBackground = backgroundVisual ? backgroundVisual->SetContent(nullptr) : S_OK;
        const HRESULT clearForeground = hostVisual ? hostVisual->SetContent(nullptr) : S_OK;
        solidOnlyOutput = retainSolid && can_retain_solid() &&
            SUCCEEDED(clearBackground) && SUCCEEDED(clearForeground);
        if (!solidOnlyOutput && solidVisual) solidVisual->SetContent(nullptr);
        HRESULT mounted = hostTarget ? hostTarget->SetRoot(solidOnlyOutput ? rootVisual.get() : nullptr) : S_OK;
        if (FAILED(mounted) || (composition && FAILED(composition->Commit()))) {
            solidOnlyOutput = false;
            if (hostTarget) hostTarget->SetRoot(nullptr);
            if (composition) composition->Commit();
        }
        if (!solidOnlyOutput) { solidSurface = nullptr; solidPixel = 0; }
        backgroundSurface = nullptr;
        backgroundSurfaceBounds = {};
        trace_output(solidOnlyOutput ? "detach.solid_only" : "detach.all");
    }

    bool update_solid_surface() {
        if (!active || tearingDown || !owns_host() || !IsWindowVisible(host) || IsIconic(host)) {
            detach_output(false);
            return false;
        }
        const HRESULT health = device ? device->GetDeviceRemovedReason() : E_UNEXPECTED;
        if (FAILED(health)) {
            detach_output(false);
            check_hresult(health);
        }
        const uint32_t pixel = tiez_glass_detail::solid_tint_pixel(
            parameters.tint_rgba[0], parameters.tint_rgba[1], parameters.tint_rgba[2]);
        if (solidSurface && solidPixel == pixel) return true;
        const uint64_t outputEpoch = presentationEpoch.value;
        const auto previousSolid = solidSurface;
        const auto owner = composition.as<IDCompositionDevice>();
        const auto drawingContext = context;
        const auto solid = solidVisual;
        const auto cancelled = [&] { cancel_presentation(); };
        com_ptr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = descriptor.Height = descriptor.MipLevels = descriptor.ArraySize = 1;
        descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        descriptor.SampleDesc.Count = 1;
        descriptor.Usage = D3D11_USAGE_IMMUTABLE;
        descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{&pixel, sizeof(pixel), sizeof(pixel)};
        phase = "output.solid_candidate";
        check_hresult(device->CreateTexture2D(&descriptor, &initial, texture.put()));
        com_ptr<IDCompositionSurface> candidate;
        if (!presentationEpoch.perform(outputEpoch, [&] {
            candidate = tiez_glass_detail::make_background_surface(owner.get(), drawingContext.get(), texture.get());
        }, cancelled)) return false;
        if (!active || tearingDown || !owns_host() || !IsWindowVisible(host) || IsIconic(host)) {
            detach_output(false);
            return false;
        }
        if (!presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(solid->SetContent(candidate.get())); }, cancelled)) return false;
        if (!nativeStarted && !presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(hostTarget->SetRoot(rootVisual.get())); }, cancelled)) return false;
        if (!presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(composition->Commit()); }, cancelled)) return false;
        if (!presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(composition->WaitForCommitCompletion()); }, cancelled)) return false;
        solidSurface = candidate;
        solidPixel = pixel;
        solidOnlyOutput = !nativeStarted;
        trace_output("solid.commit");
        return true;
    }

    void cancel_presentation() noexcept {
        trace_output("epoch.cancel");
        forget_resize_background();
        validTexture = false;
        detach_output();
        report(0);
        request_reconcile();
    }

    void arm_resize_watchdog() {
        const uint64_t now = GetTickCount64();
        const uint64_t age = resizeLease.active && now >= resizeLease.started_at ? now - resizeLease.started_at :
            tiez_glass_detail::resize_cache_lifetime_ms;
        const UINT remaining = static_cast<UINT>(age < tiez_glass_detail::resize_cache_lifetime_ms ?
            tiez_glass_detail::resize_cache_lifetime_ms - age : 1);
        require_bool(SetTimer(host, resizeWatchdogId, remaining, nullptr) != 0);
    }

    void expire_resize_background() {
        if (synchronizing) return;
        if (!resizeBackground) { KillTimer(host, resizeWatchdogId); return; }
        const uint64_t now = GetTickCount64();
        if (resizeLease.active && now >= resizeLease.started_at &&
            now - resizeLease.started_at < tiez_glass_detail::resize_cache_lifetime_ms) return;
        // A cancelled/missing size message must not leave a static scene in
        // fallback forever. Retire the old session and explicitly restart from
        // the current HWND snapshot; no future frame or input is guaranteed.
        suspend();
        request_reconcile();
    }

    bool retain_resize_capture(const WINDOWPOS& position, const RECT& client, const RECT& outer) const {
        if (!session || !pool || !dispatch || !dispatch->alive.load() || dispatch->captureClosed.load() ||
            !validTexture || !nativeStarted || !capturedScene ||
            !tiez_glass_detail::same_rect(lastCapture.crop, pixel_rect(captureBounds))) return false;
        tiez_glass_detail::PixelRect nextClient{}, nextOuter{};
        if (!tiez_glass_detail::predict_resize_client(pixel_rect(client), pixel_rect(outer),
            (position.flags & SWP_NOMOVE) ? outer.left : position.x,
            (position.flags & SWP_NOMOVE) ? outer.top : position.y,
            position.cx, position.cy, nextClient, nextOuter)) return false;
        return capture_supports_client(native_rect(nextClient));
    }

    bool prepare_resize(const WINDOWPOS& position) {
        if (synchronizing || tearingDown || failed || qaCoincidentAuxiliary ||
            (position.flags & SWP_NOSIZE) || !(position.flags & SWP_NOZORDER) ||
            (position.flags & (SWP_HIDEWINDOW | SWP_FRAMECHANGED))) return false;
        RECT currentOuter{};
        require_bool(GetWindowRect(host, &currentOuter));
        // The active HWND may already be B while the retained capture is A.
        // Compare this request with the actual window, so B -> A is a resize.
        if (position.cx == currentOuter.right - currentOuter.left &&
            position.cy == currentOuter.bottom - currentOuter.top) return false;
        // A static WGC source need not produce new frames while idle. Its last
        // verified raw image can begin a new lease, but repeated resize messages
        // cannot renew it. Only frame_arrived replaces the source during resize.
        if (!session || viewportResizePending) {
            if (!resizeBackground || !capturedScene) return false;
            SynchronizationScope synchronization(*this);
            const RECT currentClient = client_bounds();
            if (!reusable_background(pending_resize_scope(currentClient))) return false;
            resizeSizeGate.begin();
            viewportResizePending = retain_resize_capture(position, currentClient, currentOuter);
            if (!viewportResizePending && session) stop_capture(true);
            return predict_resize_region(position, currentClient, currentOuter);
        }
        SynchronizationScope synchronization(*this);
        const auto scope = ensure_scope();
        if (!reusable_background(scope)) return false;
        // Remove the old controls before Windows changes the physical size.
        // A cached final OpticsPS image must never survive into a resize.
        trace_output("resize.prepare");
        render(true);
        if (foregroundSwapMounted) {
            // A nonblocking Present can be busy. Hide its old controls with a
            // current background tree transaction before the HWND size changes.
            pendingPresent = {};
            KillTimer(host, presentRetryTimerId);
            if (!publish_background_surface(presentationEpoch.value, "resize.backpressure", false)) return false;
            resizeBackground = true;
            resizeLease.begin(GetTickCount64());
            arm_resize_watchdog();
            report(3);
        }
        check_hresult(DwmFlush());
        if (!reusable_background(ensure_scope())) return false;
        trace_output("resize.background_presented");
        const RECT currentClient = client_bounds();
        resizeSizeGate.begin();
        viewportResizePending = retain_resize_capture(position, currentClient, currentOuter);
        if (!viewportResizePending) stop_capture(true);
        return predict_resize_region(position, currentClient, currentOuter);
    }

    void update_scene() {
        auto next = ensure_scope();
        const bool scopeChanged = sceneConfigured && exclusions != next;
        if (capturedScene && captureExclusions != external_exclusions(next)) {
            const bool wasResizing = resizeBackground;
            forget_resize_background();
            if (wasResizing) {
                validTexture = false;
                detach_output();
                report(0);
            }
        }
        if (sceneConfigured && exclusions == next && EqualRect(&configuredSceneCrop, &captureBounds)) return;
        if (scopeChanged) {
            // Pending frames were rendered against the previous exclusions.
            // They cannot refresh the cache under the new scope's metadata.
            earliestRetargetFrame = retarget_time_lower_bound();
            ++cropRevision;
            validTexture = false;
            detach_output();
            report(0);
        }
        RECT crop = captureBounds;
        SIZE size{static_cast<LONG>(captureWidth), static_cast<LONG>(captureHeight)};
        phase = "scene.update_private_api";
        check_hresult(updateScene(thumbnail, nullptr, 0, next.data(), static_cast<DWORD>(next.size()),
            &crop, &size, 1));
        phase = "scene.exact_returned_crop";
        if (!EqualRect(&crop, &captureBounds) || size.cx != static_cast<LONG>(captureWidth) ||
            size.cy != static_cast<LONG>(captureHeight)) throw hresult_error(E_ACCESSDENIED);
        exclusions = std::move(next);
        configuredSceneCrop = captureBounds;
        sceneConfigured = true;
    }

    static int64_t retarget_time_lower_bound() {
        LARGE_INTEGER counter{}, frequency{};
        require_bool(QueryPerformanceCounter(&counter));
        require_bool(QueryPerformanceFrequency(&frequency));
        if (counter.QuadPart < 1 || frequency.QuadPart < 1) throw hresult_error(E_FAIL);
        // SystemRelativeTime is compositor QPC time in 100 ns units. Retain
        // the cross-thread one-QPC-tick ambiguity instead of rejecting it.
        constexpr int64_t ticksPerSecond = 10000000;
        const int64_t count = counter.QuadPart - 1;
        const int64_t seconds = count / frequency.QuadPart;
        const int64_t remainder = count % frequency.QuadPart;
        if (seconds > std::numeric_limits<int64_t>::max() / ticksPerSecond ||
            remainder > std::numeric_limits<int64_t>::max() / ticksPerSecond)
            throw hresult_error(E_BOUNDS);
        const int64_t whole = seconds * ticksPerSecond;
        const int64_t fraction = remainder * ticksPerSecond / frequency.QuadPart;
        if (whole > std::numeric_limits<int64_t>::max() - fraction) throw hresult_error(E_BOUNDS);
        return whole + fraction;
    }

    void replace_shared_scene() {
        auto next = ensure_scope();
        Thumbnail candidateThumbnail;
        com_ptr<IDCompositionVisual2> candidate;
        phase = "move.create_private_scene";
        check_hresult(createScene(helper, composition.get(), candidate.put_void(), &candidateThumbnail.value));
        bool rootChanged = false;
        try {
            RECT crop = captureBounds;
            SIZE size{static_cast<LONG>(captureWidth), static_cast<LONG>(captureHeight)};
            phase = "move.configure_new_scene";
            check_hresult(updateScene(candidateThumbnail.value, nullptr, 0, next.data(), static_cast<DWORD>(next.size()),
                &crop, &size, 1));
            if (!EqualRect(&crop, &captureBounds) || size.cx != static_cast<LONG>(captureWidth) ||
                size.cy != static_cast<LONG>(captureHeight)) throw hresult_error(E_ACCESSDENIED);
            const D2D_RECT_F clip{0, 0, static_cast<float>(captureWidth), static_cast<float>(captureHeight)};
            check_hresult(candidate->SetClip(clip));
            ensure_scope();
            phase = "move.replace_scene_root";
            check_hresult(helperTarget->SetRoot(candidate.get()));
            rootChanged = true;
            check_hresult(composition->Commit());
            check_hresult(composition->WaitForCommitCompletion());
            ensure_scope();
            phase = "move.await_compositor_flush";
            queue_retarget_flush(dispatch, cropRevision);
            retargetFlushPending = true;
        } catch (...) {
            if (rootChanged) {
                // Keep the previous scene alive until its replacement commits.
                // A failed rollback is terminal, not a queued translation.
                HRESULT rollback = helperTarget->SetRoot(sharedVisual.get());
                if (SUCCEEDED(rollback)) rollback = composition->Commit();
                if (SUCCEEDED(rollback)) rollback = composition->WaitForCommitCompletion();
                if (FAILED(rollback)) {
                    phase = "move.rollback_scene_root";
                    throw hresult_error(rollback);
                }
            }
            throw;
        }
        Thumbnail previous(thumbnail);
        sharedVisual = std::move(candidate);
        thumbnail = candidateThumbnail.release();
        exclusions = std::move(next);
        configuredSceneCrop = captureBounds;
        sceneConfigured = true;
        if (qaSource) {
            std::fprintf(stderr,
                "[native-glass-scene-replace] revision=%llu previous=%p current=%p helper=%p pool=%p session=%p\n",
                static_cast<unsigned long long>(cropRevision), static_cast<void*>(previous.value),
                static_cast<void*>(thumbnail), static_cast<void*>(helper), get_abi(pool), get_abi(session));
            std::fflush(stderr);
        }
    }

    void translate_capture(const RECT& client, const RECT& outer) {
        phase = "move.validate_owned_region";
        Region current;
        if (!query_host_region(current)) throw MissingHostRegion{};
        if (!expectedHostRegion.value || !EqualRgn(current.value, expectedHostRegion.value))
            throw hresult_error(E_ACCESSDENIED);
        const RECT previousBounds = bounds, previousHostBounds = hostBounds, previousCaptureBounds = captureBounds;
        const int64_t previousEarliestFrame = earliestRetargetFrame;
        const uint64_t previousRevision = cropRevision;
        const bool previousFlushPending = retargetFlushPending;
        uint32_t discarded = 0;
        try {
            bounds = client;
            hostBounds = outer;
            if (!capture_supports_client(client)) captureBounds = capture_bounds_for(client);
            ensure_scope();
            if (!EqualRect(&captureBounds, &previousCaptureBounds)) {
                while (auto frame = pool.TryGetNextFrame()) { frame.Close(); ++discarded; }
                earliestRetargetFrame = retarget_time_lower_bound();
                ++cropRevision;
                replace_shared_scene();
            }
        } catch (...) {
            // The old root remains authoritative after rollback. In particular,
            // A -> B -> C -> B must not mistake an uncommitted B for its source.
            bounds = previousBounds;
            hostBounds = previousHostBounds;
            captureBounds = previousCaptureBounds;
            earliestRetargetFrame = previousEarliestFrame;
            cropRevision = previousRevision;
            retargetFlushPending = previousFlushPending;
            throw;
        }
        if (qaSource) {
            std::fprintf(stderr,
                "[native-glass-retarget] revision=%llu earliest_100ns=%lld discarded=%u "
                "crop=[%ld,%ld,%ld,%ld] retained_capture=1\n",
                static_cast<unsigned long long>(cropRevision), static_cast<long long>(earliestRetargetFrame), discarded,
                bounds.left, bounds.top, bounds.right, bounds.bottom);
            std::fflush(stderr);
        }
        if (reusable_background(ensure_scope()) && copy_captured_viewport()) {
            // A static reserve can already cover the new viewport. Reprojection
            // does not stamp an old source as a newly arrived capture.
            awaitingMoveCapture = false;
            render();
        } else {
            // Retain the last submitted image during the asynchronous move.
            // Its old pixels are never stamped as a capture at the new origin
            // or used for new optics. Withdrawing the tree here caused flashes
            // every time dragging crossed the real capture reserve.
            awaitingMoveCapture = true;
            pendingPresent = {};
            KillTimer(host, presentRetryTimerId);
        }
        // A static scene may have produced its sole updated frame during the
        // commit/flush. Consume that queued frame too; do not demand a later
        // repaint, restart WGC, or invent a static-scene timeout.
        queue(dispatch, true);
    }

    void make_texture(com_ptr<ID3D11Texture2D>& texture, com_ptr<ID3D11ShaderResourceView>& view,
        com_ptr<ID3D11RenderTargetView>* target, bool mipChain = false,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM, UINT textureWidth = 0, UINT textureHeight = 0) {
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = textureWidth ? textureWidth : width;
        descriptor.Height = textureHeight ? textureHeight : height;
        descriptor.MipLevels = mipChain ? 0 : 1;
        descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
        descriptor.Format = format;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE | ((target || mipChain) ? D3D11_BIND_RENDER_TARGET : 0);
        descriptor.MiscFlags = mipChain ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        check_hresult(device->CreateTexture2D(&descriptor, nullptr, texture.put()));
        check_hresult(device->CreateShaderResourceView(texture.get(), nullptr, view.put()));
        if (target) check_hresult(device->CreateRenderTargetView(texture.get(), nullptr, target->put()));
    }

    bool copy_captured_viewport() {
        if (!capturedScene) return false;
        tiez_glass_detail::ResizeCopy copy{};
        if (!tiez_glass_detail::capture_viewport_copy(lastCapture.crop, pixel_rect(bounds), copy)) return false;
        D3D11_TEXTURE2D_DESC description{};
        capturedScene->GetDesc(&description);
        if (description.Width != static_cast<UINT>(lastCapture.crop.right - lastCapture.crop.left) ||
            description.Height != static_cast<UINT>(lastCapture.crop.bottom - lastCapture.crop.top) ||
            description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.ArraySize != 1 ||
            description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0) throw hresult_error(E_BOUNDS);
        const D3D11_BOX source{copy.source_x, copy.source_y, 0,
            copy.source_x + copy.width, copy.source_y + copy.height, 1};
        context->CopySubresourceRegion(scene.get(), 0, 0, 0, 0, capturedScene.get(), 0, &source);
        context->GenerateMips(sceneView.get());
        baseDirty = true;
        return true;
    }

    void align_resize_background(const RECT& client) {
        if (!backgroundSurface || !resizeBackground || !nativeStarted) return;
        const uint64_t epoch = presentationEpoch.value;
        const auto surface = backgroundSurface;
        const auto visual = backgroundVisual;
        const auto cancelled = [&] { cancel_presentation(); };
        if (!reusable_background(pending_resize_scope(client))) { cancel_presentation(); return; }
        if (!presentationEpoch.perform(epoch, [&] {
            check_hresult(visual->SetOffsetX(static_cast<float>(int64_t(backgroundSurfaceBounds.left) - client.left)));
            check_hresult(visual->SetOffsetY(static_cast<float>(int64_t(backgroundSurfaceBounds.top) - client.top)));
        }, cancelled)) return;
        if (!presentationEpoch.perform(epoch, [&] { check_hresult(composition->Commit()); }, cancelled)) return;
        if (!presentationEpoch.perform(epoch, [&] { check_hresult(composition->WaitForCommitCompletion()); }, cancelled)) return;
        if (!reusable_background(pending_resize_scope(client))) { cancel_presentation(); return; }
        trace_output("resize.background_screen_aligned");
    }

    void bind_gaussian_kernel(float physicalSigma) {
        const float sigma = std::clamp(physicalSigma, 0.0f, 128.0f);
        if (sigma <= 4.0f) return;
        if (!gaussianConstants || gaussianSigma != sigma) {
            struct Kernel { float meta[4]{}; float pairs[192][4]{}; } kernel;
            const int radius = static_cast<int>(std::ceil(3.0f * sigma));
            float sum = 1.0f;
            size_t count = 0;
            for (int tap = 1; tap <= radius; tap += 2) {
                const float d1 = float(tap) / sigma, d2 = float(tap + 1) / sigma;
                const float w1 = std::exp(-0.5f * d1 * d1);
                const float w2 = tap + 1 <= radius ? std::exp(-0.5f * d2 * d2) : 0.0f;
                const float weight = w1 + w2;
                kernel.pairs[count][0] = float(tap) + w2 / weight;
                kernel.pairs[count++][1] = weight;
                sum += 2.0f * weight;
            }
            kernel.meta[0] = float(count); kernel.meta[1] = 1.0f / sum; kernel.meta[2] = sigma;
            for (size_t tap = 0; tap < count; ++tap) kernel.pairs[tap][1] /= sum;
            if (!gaussianConstants) {
                D3D11_BUFFER_DESC buffer{};
                buffer.ByteWidth = sizeof(Kernel); buffer.Usage = D3D11_USAGE_DEFAULT;
                buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                check_hresult(device->CreateBuffer(&buffer, nullptr, gaussianConstants.put()));
            }
            context->UpdateSubresource(gaussianConstants.get(), 0, nullptr, &kernel, 0, 0);
            gaussianSigma = sigma;
        }
        ID3D11Buffer* buffer = gaussianConstants.get();
        context->PSSetConstantBuffers(1, 1, &buffer);
    }

    bool render_capture_background(const ShaderState& settings) {
        const bool cached = filteredCaptureValid && filteredCaptureSource == capturedScene &&
            filteredCaptureRevision == captureRevision && filteredCaptureSigma == settings.options[0] &&
            std::equal(std::begin(filteredCaptureTint), std::end(filteredCaptureTint), std::begin(settings.tint)) &&
            EqualRectValue(filteredCaptureBounds, native_rect(lastCapture.crop));
        filteredCaptureValid = false;
        tiez_glass_detail::ResizeCopy copy{};
        if (!capturedScene || !tiez_glass_detail::capture_viewport_copy(lastCapture.crop, pixel_rect(bounds), copy))
            return false;
        D3D11_TEXTURE2D_DESC sourceDescription{}, allocated{};
        capturedScene->GetDesc(&sourceDescription);
        if (sourceDescription.Width != static_cast<UINT>(lastCapture.crop.right - lastCapture.crop.left) ||
            sourceDescription.Height != static_cast<UINT>(lastCapture.crop.bottom - lastCapture.crop.top) ||
            sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM || sourceDescription.ArraySize != 1 ||
            sourceDescription.SampleDesc.Count != 1 || sourceDescription.SampleDesc.Quality != 0)
            throw hresult_error(E_BOUNDS);
        if (captureFiltered.texture) captureFiltered.texture->GetDesc(&allocated);
        if (allocated.Width != sourceDescription.Width || allocated.Height != sourceDescription.Height) {
            captureHorizontal = {};
            captureFiltered = {};
            // Preserve the RGB/coverage ratio in faint Gaussian tails until
            // the vertical pass normalizes it. BGRA8 rounds them separately.
            make_texture(captureHorizontal.texture, captureHorizontal.view, &captureHorizontal.target,
                false, DXGI_FORMAT_R16G16B16A16_FLOAT, sourceDescription.Width, sourceDescription.Height);
            make_texture(captureFiltered.texture, captureFiltered.view, &captureFiltered.target,
                false, DXGI_FORMAT_B8G8R8A8_UNORM, sourceDescription.Width, sourceDescription.Height);
        }
        if (filteredCaptureSource != capturedScene) {
            filteredCaptureSourceView = nullptr;
            check_hresult(device->CreateShaderResourceView(capturedScene.get(), nullptr, filteredCaptureSourceView.put()));
            filteredCaptureSource = capturedScene;
        }
        if (!cached) {
            bind_gaussian_kernel(settings.options[0]);
            ShaderState full = settings;
            full.size[0] = static_cast<float>(sourceDescription.Width);
            full.size[1] = static_cast<float>(sourceDescription.Height);
            full.options[1] = full.trackMeta[0] = 0;
            full.direction[0] = 1.0f;
            full.direction[1] = 0.0f;
            full.options[2] = 0.0f;
            bind_pass_pipeline(sourceDescription.Width, sourceDescription.Height);
            draw_pass(filteredCaptureSourceView.get(), captureHorizontal.target.get(), blur.get(), full);
            full.direction[0] = 0.0f;
            full.direction[1] = 1.0f;
            full.options[2] = 1.0f;
            draw_pass(captureHorizontal.view.get(), captureFiltered.target.get(), blur.get(), full);
            filteredCaptureRevision = captureRevision;
            filteredCaptureSigma = settings.options[0];
            std::copy_n(settings.tint, 4, filteredCaptureTint);
        }
        const D3D11_BOX viewport{copy.source_x, copy.source_y, 0,
            copy.source_x + copy.width, copy.source_y + copy.height, 1};
        context->CopySubresourceRegion(tinted.get(), 0, 0, 0, 0, captureFiltered.texture.get(), 0, &viewport);
        filteredCaptureBounds = native_rect(lastCapture.crop);
        filteredCaptureValid = true;
        bind_pass_pipeline();
        return true;
    }

    bool prepare_background_surface(uint64_t outputEpoch, BackgroundOutput& candidate) {
        const auto cancelled = [&] { cancel_presentation(); };
        if (!presentationEpoch.perform(outputEpoch, [] {}, cancelled)) return false;
        if (!reusable_background(ensure_scope())) { cancel_presentation(); return false; }
        // Never draw into a bound surface: EndDraw can become visible on any
        // later Commit, even when this call is cancelled by a reentrant event.
        // The source is the opaque material pass, never the controls/optics.
        const auto source = filteredCaptureValid ? captureFiltered.texture : tinted;
        const RECT sourceBounds = filteredCaptureValid ? filteredCaptureBounds : bounds;
        const auto surfaceDevice = composition.as<IDCompositionDevice>();
        phase = "output.background_surface_candidate";
        if (!presentationEpoch.perform(outputEpoch, [&] {
            candidate.surface = tiez_glass_detail::make_background_surface(surfaceDevice.get(), context.get(), source.get());
        }, cancelled)) return false;
        // BeginDraw/EndDraw may pump messages. Validate the source's scope and
        // bounded resize lease again while the new surface is still unbound.
        if (!reusable_background(ensure_scope())) { cancel_presentation(); return false; }
        candidate.crop = sourceBounds;
        trace_output("background.candidate_unbound");
        return true;
    }

    bool mount_output(uint64_t outputEpoch, const BackgroundOutput& candidateBackground, bool showForeground = true) {
        // Retain both the previous base and all participating visuals until
        // the replacement batch has completed. The root itself stays stable.
        const auto previousBackground = backgroundSurface;
        const auto root = rootVisual, background = backgroundVisual, foreground = hostVisual;
        const auto cancelled = [&] { cancel_presentation(); };
        if (!presentationEpoch.perform(outputEpoch, [] {}, cancelled)) return false;
        const auto scope = ensure_scope();
        if (candidateBackground.surface && !reusable_background(scope)) { cancel_presentation(); return false; }
        if (!presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(background->SetContent(candidateBackground.surface.get())); }, cancelled)) return false;
        if (!presentationEpoch.perform(outputEpoch, [&] {
            check_hresult(background->SetOffsetX(candidateBackground.surface ?
                static_cast<float>(int64_t(candidateBackground.crop.left) - bounds.left) : 0.0f));
            check_hresult(background->SetOffsetY(candidateBackground.surface ?
                static_cast<float>(int64_t(candidateBackground.crop.top) - bounds.top) : 0.0f));
        }, cancelled)) return false;
        if (!presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(foreground->SetContent(showForeground ? swap.get() : nullptr)); }, cancelled)) return false;
        if (!presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(hostTarget->SetRoot(root.get())); }, cancelled)) return false;
        // The fresh surface's EndDraw and both visual contents share this
        // device/Commit. A swap Present alone has no such tree transaction.
        if (!presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(composition->Commit()); }, cancelled)) return false;
        if (!presentationEpoch.perform(outputEpoch,
            [&] { check_hresult(composition->WaitForCommitCompletion()); }, cancelled)) return false;
        const auto verifiedScope = ensure_scope();
        if (candidateBackground.surface && !reusable_background(verifiedScope)) { cancel_presentation(); return false; }
        backgroundSurface = candidateBackground.surface;
        backgroundSurfaceBounds = candidateBackground.crop;
        foregroundSwapMounted = showForeground;
        nativeStarted = true;
        solidOnlyOutput = false;
        trace_output("background.commit_complete", nullptr, previousBackground.get());
        return true;
    }

    bool publish_background_surface(uint64_t outputEpoch, const char* reason, bool showForeground = true) {
        LARGE_INTEGER started{}, prepared{}, completed{}, frequency{};
        if (qaSource) QueryPerformanceCounter(&started);
        BackgroundOutput candidate;
        if (!prepare_background_surface(outputEpoch, candidate)) return false;
        if (qaSource) QueryPerformanceCounter(&prepared);
        if (!mount_output(outputEpoch, candidate, showForeground)) return false;
        if (qaSource && QueryPerformanceCounter(&completed) && QueryPerformanceFrequency(&frequency) &&
            frequency.QuadPart > 0 && started.QuadPart > 0 && prepared.QuadPart >= started.QuadPart &&
            completed.QuadPart >= prepared.QuadPart) {
            // Fixture-only CPU wall time. Copy queues GPU work; neither these
            // QPC intervals nor WaitForCommitCompletion prove display latency.
            const double milliseconds = 1000.0 / static_cast<double>(frequency.QuadPart);
            std::fprintf(stderr,
                "[native-glass-background-timing] renderer=%llu reason=%s qpc_frequency=%lld "
                "copy_validate_ms=%.3f mount_wait_ms=%.3f total_ms=%.3f size=[%u,%u] crop_revision=%llu\n",
                static_cast<unsigned long long>(identity), reason, static_cast<long long>(frequency.QuadPart),
                static_cast<double>(prepared.QuadPart - started.QuadPart) * milliseconds,
                static_cast<double>(completed.QuadPart - prepared.QuadPart) * milliseconds,
                static_cast<double>(completed.QuadPart - started.QuadPart) * milliseconds, width, height,
                static_cast<unsigned long long>(cropRevision));
            std::fflush(stderr);
        }
        return true;
    }

    void configure_output() {
        pendingPresent = {};
        KillTimer(host, presentRetryTimerId);
        const bool retained = resizeBackground && reusable_background(ensure_scope());
        if (resizeBackground && !retained) {
            forget_resize_background();
            detach_output();
            report(0);
        }
        const uint64_t outputEpoch = presentationEpoch.value;
        // SetContent does not keep the application's swap-chain object alive.
        // Retain the mounted chain until the candidate's commit has completed;
        // replacing the member early otherwise destroys the old presentation.
        const auto previousSwap = swap;
        trace_output("candidate.begin", previousSwap.get());
        // Keep the source alive across COM calls. A reentrant invalidation may
        // clear the member cache; the epoch fence still forbids mounting it.
        const auto retainedScene = capturedScene;
        const auto retainedCrop = lastCapture.crop;
        phase = "output.create_candidate_swapchain";
        // ResizeBuffers invalidates the surface still mounted by DComp. Build
        // a complete replacement first; the mounted background stays intact.
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
        com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 descriptor{};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        descriptor.SampleDesc.Count = 1;
        descriptor.BufferCount = 2;
        descriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        descriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        descriptor.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        descriptor.Scaling = DXGI_SCALING_STRETCH;
        com_ptr<IDXGISwapChain1> candidate;
        check_hresult(factory->CreateSwapChainForComposition(device.get(), &descriptor, nullptr, candidate.put()));
        context->ClearState();
        sceneView = horizontalView = tintedView = nullptr;
        horizontalTarget = tintedTarget = nullptr;
        scene = horizontal = tinted = nullptr;
        swap = std::move(candidate);
        phase = "output.scene_texture";
        make_texture(scene, sceneView, nullptr, true);
        phase = "output.horizontal_texture";
        make_texture(horizontal, horizontalView, &horizontalTarget, false, DXGI_FORMAT_R16G16B16A16_FLOAT);
        phase = "output.tinted_texture";
        make_texture(tinted, tintedView, &tintedTarget);
        phase = "output.optical_prefix_textures";
        initialPrefix = {};
        initialPrefixValid = false;
        make_texture(initialPrefix.texture, initialPrefix.view, &initialPrefix.target, true, DXGI_FORMAT_R16G16B16A16_FLOAT);
        for (auto& surface : prefixColors) {
            surface = {};
            make_texture(surface.texture, surface.view, &surface.target, true, DXGI_FORMAT_R16G16B16A16_FLOAT);
        }
        for (auto& surface : prefixCoverage) {
            surface = {};
            make_texture(surface.texture, surface.view, &surface.target, false, DXGI_FORMAT_R16_FLOAT);
        }
        baseDirty = true;
        // A five-pixel, startup-only readback rejects the transparent/empty
        // first frames observed during the native probe. Black RGB is valid.
        // This does not claim general scene-content validation.
        initialAlpha = nullptr;
        initialAlphaPending = false;
        D3D11_TEXTURE2D_DESC alphaDescriptor{};
        alphaDescriptor.Width = 5;
        alphaDescriptor.Height = 1;
        alphaDescriptor.MipLevels = alphaDescriptor.ArraySize = alphaDescriptor.SampleDesc.Count = 1;
        alphaDescriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        alphaDescriptor.Usage = D3D11_USAGE_STAGING;
        alphaDescriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        phase = "output.initial_alpha_texture";
        check_hresult(device->CreateTexture2D(&alphaDescriptor, nullptr, initialAlpha.put()));
        const float opaque[]{parameters.tint_rgba[0], parameters.tint_rgba[1], parameters.tint_rgba[2], 1.0f};
        if (retained) {
            phase = "output.resize_raw_intersection";
            com_ptr<ID3D11RenderTargetView> rawTarget;
            check_hresult(device->CreateRenderTargetView(scene.get(), nullptr, rawTarget.put()));
            // Unknown bands receive an estimate from the trusted raw source's
            // final 1x1 mip. Normalize premultiplied coverage, but do not tint
            // here: the normal material passes apply tint exactly once later.
            D3D11_TEXTURE2D_DESC capturedDescriptor{};
            retainedScene->GetDesc(&capturedDescriptor);
            D3D11_SHADER_RESOURCE_VIEW_DESC averageDescriptor{};
            averageDescriptor.Format = capturedDescriptor.Format;
            averageDescriptor.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            averageDescriptor.Texture2D.MostDetailedMip = capturedDescriptor.MipLevels - 1;
            averageDescriptor.Texture2D.MipLevels = 1;
            com_ptr<ID3D11ShaderResourceView> averageView;
            check_hresult(device->CreateShaderResourceView(retainedScene.get(), &averageDescriptor, averageView.put()));
            ShaderState average{};
            average.size[0] = static_cast<float>(width);
            average.size[1] = static_cast<float>(height);
            std::copy_n(parameters.tint_rgba, 3, average.tint);
            average.options[2] = 1.0f;
            bind_pass_pipeline();
            draw_pass(averageView.get(), rawTarget.get(), blur.get(), average);
            const auto copy = tiez_glass_detail::resize_intersection(retainedCrop, pixel_rect(bounds));
            if (copy.width && copy.height) {
                const D3D11_BOX source{copy.source_x, copy.source_y, 0,
                    copy.source_x + copy.width, copy.source_y + copy.height, 1};
                context->CopySubresourceRegion(scene.get(), 0, copy.destination_x, copy.destination_y, 0,
                    retainedScene.get(), 0, &source);
            }
            context->GenerateMips(sceneView.get());
            // This composite is never stored as a capture or used for optics.
            render(true, false);
        } else {
            com_ptr<ID3D11Texture2D> buffer;
            phase = "output.placeholder_target";
            check_hresult(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
            com_ptr<ID3D11RenderTargetView> target;
            check_hresult(device->CreateRenderTargetView(buffer.get(), nullptr, target.put()));
            context->ClearRenderTargetView(target.get(), opaque);
            phase = "output.placeholder_present";
            check_hresult(swap->Present(0, 0));
        }
        if (!presentationEpoch.perform(outputEpoch, [] {}, [&] { cancel_presentation(); })) return;
        trace_output("candidate.present_returned", previousSwap.get());
        phase = "output.candidate_commit";
        if (retained) {
            if (!publish_background_surface(outputEpoch, "resize.configure", false)) return;
        } else if (!mount_output(outputEpoch, {})) return;
        if (retained) { arm_resize_watchdog(); report(3); }
        trace_output("candidate.commit_complete", previousSwap.get());
    }

    HWND make_helper(HWND owner) {
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, helperClass, L"",
            WS_POPUP, auxiliaryBounds.left, auxiliaryBounds.top, static_cast<int>(captureWidth), static_cast<int>(captureHeight),
            owner, nullptr, GetModuleHandleW(nullptr), this);
        if (!window) throw hresult_error(last_error());
        return window;
    }

    void add_hook(DWORD minimum, DWORD maximum) {
        const HWINEVENTHOOK hook = SetWinEventHook(minimum, maximum, nullptr, scene_event,
            0, 0, WINEVENT_OUTOFCONTEXT);
        if (!hook) throw hresult_error(last_error());
        hooks.push_back(hook);
        std::lock_guard<std::mutex> lock(hookMutex);
        hookDispatch.emplace(hook, dispatch);
    }

    void begin_capture() {
        phase = "capture.active_visibility";
        if (!active || !hasState || !host || !IsWindowVisible(host) || IsIconic(host)) {
            report(0);
            return;
        }
        if (!resizeBackground) report(0);
        phase = "capture.client_dimensions";
        bounds = client_bounds();
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
            bounds.right - bounds.left > 8192 || bounds.bottom - bounds.top > 8192)
            throw hresult_error(E_BOUNDS);
        width = static_cast<UINT>(bounds.right - bounds.left);
        height = static_cast<UINT>(bounds.bottom - bounds.top);
        phase = "capture.outer_and_client_dimensions";
        require_bool(GetWindowRect(host, &hostBounds));
        if (hostBounds.left > bounds.left || hostBounds.top > bounds.top ||
            hostBounds.right < bounds.right || hostBounds.bottom < bounds.bottom ||
            hostBounds.right - hostBounds.left > 16384 || hostBounds.bottom - hostBounds.top > 16384)
            throw hresult_error(E_BOUNDS);
        configuredDpi = GetDpiForWindow(host);
        if (!configuredDpi) configuredDpi = 96;
        scale = static_cast<float>(configuredDpi) / 96.0f;
        virtualDesktopBounds = read_virtual_desktop();
        configuredDisplayEpoch = displayEpoch;
        captureBounds = capture_bounds_for(bounds);
        ensure_fixture(&captureBounds);
        captureWidth = static_cast<UINT>(captureBounds.right - captureBounds.left);
        captureHeight = static_cast<UINT>(captureBounds.bottom - captureBounds.top);
        auxiliaryBounds = captureBounds;
        if (!qaCoincidentAuxiliary) {
            // The private scene samples captureBounds. Auxiliary windows stay
            // off every display, so independent HWND animations cannot expose
            // their pixels around the host during minimization/restoration.
            const int64_t left = static_cast<int64_t>(virtualDesktopBounds.left) - captureWidth - 32;
            const int64_t top = virtualDesktopBounds.top;
            if (left < std::numeric_limits<LONG>::min() || top + captureHeight > std::numeric_limits<LONG>::max())
                throw hresult_error(E_BOUNDS);
            auxiliaryBounds = {static_cast<LONG>(left), static_cast<LONG>(top),
                static_cast<LONG>(left + captureWidth), static_cast<LONG>(top + captureHeight)};
            if (!auxiliary_outside_desktop()) throw hresult_error(E_ACCESSDENIED);
        }
        if (qaColoredAuxiliary) {
            std::fprintf(stderr,
                "[native-glass-auxiliary-qa] mode=%s helper_color=00FFFF cover_color=FF00FF "
                "source=[%ld,%ld,%ld,%ld] auxiliary=[%ld,%ld,%ld,%ld]\n",
                qaCoincidentAuxiliary ? "colors" : "offscreen",
                bounds.left, bounds.top, bounds.right, bounds.bottom,
                auxiliaryBounds.left, auxiliaryBounds.top, auxiliaryBounds.right, auxiliaryBounds.bottom);
            std::fflush(stderr);
        }
        // An allowed status is queried without prompting. Refuse capture when
        // a visible system border could escape the host's rounded silhouette.
        using namespace winrt::Windows::Security::Authorization::AppCapabilityAccess;
        phase = "capture.existing_borderless_access";
        if (AppCapability::Create(L"graphicsCaptureWithoutBorder").CheckAccess() !=
            AppCapabilityAccessStatus::Allowed) throw hresult_error(E_ACCESSDENIED);
        ensure_host_snapshot();
        if (resizeBackground) {
            Region previous;
            // install_region may recover a missing owned mask. That recovery
            // must not retroactively authorize pixels cached before the loss.
            if (!query_host_region(previous) || !expectedHostRegion.value ||
                !EqualRgn(previous.value, expectedHostRegion.value)) {
                forget_resize_background();
                detach_output();
                report(0);
            }
        }
        phase = "capture.host_region";
        install_region(host);
        // Region changes may trigger non-client recalculation. Confirm the
        // client/outer snapshot before any auxiliary window becomes visible.
        ensure_host_snapshot();
        const auto frame = resize_context();
        configuredFrameStyle = frame.frame_style;
        configuredFrameExtendedStyle = frame.frame_extended_style;
        phase = "capture.create_helper";
        helper = make_helper(nullptr);
        phase = "capture.create_cover";
        cover = make_helper(helper);
        phase = "capture.helper_region";
        install_region(helper);
        phase = "capture.cover_region";
        install_region(cover);
        // CreateWindowEx/DPI handling can adjust requested geometry. Validate
        // the actual hidden windows before showing even one auxiliary pixel.
        validate_auxiliary_windows(false);
        constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW;
        phase = "capture.position_shields";
        HDWP positions = BeginDeferWindowPos(2);
        if (!positions) throw hresult_error(last_error());
        positions = DeferWindowPos(positions, helper, host, 0, 0, 0, 0, flags);
        if (!positions) throw hresult_error(last_error());
        positions = DeferWindowPos(positions, cover, host, 0, 0, 0, 0, flags);
        if (!positions) throw hresult_error(last_error());
        require_bool(EndDeferWindowPos(positions));
        phase = "capture.paint_cover";
        require_bool(UpdateWindow(cover));
        phase = "capture.shields_flush";
        check_hresult(DwmFlush());
        ensure_scope();
        configure_output();
        ensure_scope();
        captureTexture = nullptr;
        captureView = nullptr;
        make_texture(captureTexture, captureView, nullptr, true, DXGI_FORMAT_B8G8R8A8_UNORM,
            captureWidth, captureHeight);
        phase = "capture.helper_livepreview";
        helper_live_preview(helper, true);
        helperExcluded = true;
        phase = "capture.create_private_scene";
        check_hresult(createScene(helper, composition.get(), sharedVisual.put_void(), &thumbnail));
        update_scene();
        const D2D_RECT_F clip{0, 0, static_cast<float>(captureWidth), static_cast<float>(captureHeight)};
        phase = "capture.scene_clip";
        check_hresult(sharedVisual->SetClip(clip));
        phase = "capture.helper_dcomp_target";
        check_hresult(composition->CreateTargetForHwnd(helper, TRUE, helperTarget.put()));
        phase = "capture.helper_visual_commit";
        check_hresult(helperTarget->SetRoot(sharedVisual.get()));
        check_hresult(composition->Commit());
        check_hresult(DwmFlush());
        phase = "capture.wgc_item";
        const auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(helper, guid_of<GraphicsCaptureItem>(), put_abi(item)));
        const auto size = item.Size();
        phase = "capture.wgc_item_dimensions";
        if (size.Width != static_cast<int>(captureWidth) || size.Height != static_cast<int>(captureHeight))
            throw hresult_error(E_BOUNDS);
        phase = "capture.wgc_frame_pool";
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(captureDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        phase = "capture.wgc_session";
        session = pool.CreateCaptureSession(item);
        phase = "capture.wgc_cursor_preference";
        session.IsCursorCaptureEnabled(false);
        phase = "capture.wgc_border_preference";
        session.IsBorderRequired(false);
        dispatch = std::make_shared<Dispatch>();
        dispatch->window = helper;
        dispatch->generation = nextGeneration.fetch_add(1);
        std::weak_ptr<Dispatch> weak = dispatch;
        frameToken = pool.FrameArrived([weak](auto const&, auto const&) {
            if (auto signal = weak.lock()) queue(signal, true);
        });
        hasFrameToken = true;
        closedToken = item.Closed([weak](auto const&, auto const&) {
            if (auto signal = weak.lock()) {
                signal->captureClosed.store(true);
                queue(signal, false);
            }
        });
        hasClosedToken = true;
        phase = "capture.window_event_hooks";
        add_hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
        add_hook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE);
        add_hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE);
        ensure_scope();
        phase = "capture.startup_watchdog";
        require_bool(SetTimer(helper, watchdogId, 5000, nullptr) != 0);
        phase = "capture.start";
        session.StartCapture();
        phase = "capture.await_first_frame";
    }

    void stop_capture(bool preserveResize = false) noexcept {
        initialAlphaPending = false;
        KillTimer(helper, initialAlphaRetryTimerId);
        pendingPresent = {};
        awaitingMoveCapture = false;
        retargetFlushPending = false;
        KillTimer(host, presentRetryTimerId);
        cancel_noop_z_order();
        viewportResizePending = false;
        trace_output(preserveResize ? "capture.stop_preserving" : "capture.stop");
        if (!preserveResize) {
            forget_resize_background();
            validTexture = false;
            detach_output();
        }
        // Remove the sampled helper before its cover or the app's opaque root.
        // In-flight callbacks own only Dispatch, never this renderer/context.
        if (dispatch) dispatch->alive.store(false);
        if (helper) KillTimer(helper, watchdogId);
        if (IsWindow(helper)) ShowWindow(helper, SW_HIDE);
        if (IsWindow(cover)) ShowWindow(cover, SW_HIDE);
        for (const HWINEVENTHOOK hook : hooks) {
            {
                std::lock_guard<std::mutex> lock(hookMutex);
                hookDispatch.erase(hook);
            }
            UnhookWinEvent(hook);
        }
        hooks.clear();
        try { if (pool && hasFrameToken) pool.FrameArrived(frameToken); } catch (...) {}
        try { if (item && hasClosedToken) item.Closed(closedToken); } catch (...) {}
        hasFrameToken = hasClosedToken = false;
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        session = nullptr;
        pool = nullptr;
        item = nullptr;
        captureTexture = nullptr;
        captureView = nullptr;
        if (helperTarget) helperTarget->SetRoot(nullptr);
        if (composition) {
            composition->Commit();
            composition->WaitForCommitCompletion();
        }
        sharedVisual = nullptr;
        helperTarget = nullptr;
        if (thumbnail) DwmUnregisterThumbnail(thumbnail);
        thumbnail = nullptr;
        if (helperExcluded && IsWindow(helper)) {
            try { helper_live_preview(helper, false); } catch (...) {}
        }
        helperExcluded = false;
        if (IsWindow(helper)) DestroyWindow(helper);
        helper = nullptr;
        if (IsWindow(cover)) DestroyWindow(cover);
        cover = nullptr;
        dispatch.reset();
        validTexture = false;
        frameDeferred = false;
        earliestRetargetFrame = 0;
        sceneConfigured = false;
        exclusions.clear();
        trace_output("capture.stopped");
    }

    void suspend() noexcept {
        if (tearingDown) return;
        cancel_noop_z_order();
        if (synchronizing) {
            forget_resize_background();
            validTexture = false;
            detach_output();
            report(0);
            reconcileDeferred = true;
            return;
        }
        synchronizing = true;
        stop_capture();
        report(0);
        synchronizing = false;
    }

    void fail(HRESULT error) noexcept {
        if (synchronizing || tearingDown || failed) return;
        failed = true;
        scopeRecoveryPending = false;
        stop_scope_recovery_timer();
        diagnose(error);
        synchronizing = true;
        // Withdraw captured content before potentially blocking capture teardown.
        // A healthy device may retain only the source-independent solid.
        stop_capture();
        report(2, error);
        synchronizing = false;
    }

    bool finish_retained_viewport(const RECT& client, const RECT& outer) {
        if (!session || !pool || !dispatch || !dispatch->alive.load() || dispatch->captureClosed.load() ||
            !resizeSizeGate.position_known || !EqualRect(&client, &pendingViewportClient) ||
            !EqualRect(&outer, &pendingViewportOuter) || !capture_supports_client(client) ||
            !reusable_background(pending_resize_scope(client))) {
            suspend();
            request_reconcile();
            return false;
        }
        const UINT nextWidth = static_cast<UINT>(client.right - client.left);
        const UINT nextHeight = static_cast<UINT>(client.bottom - client.top);
        const bool dimensionsChanged = nextWidth != width || nextHeight != height;
        bounds = client;
        hostBounds = outer;
        width = nextWidth;
        height = nextHeight;
        viewportResizePending = false;
        ensure_scope();
        if (dimensionsChanged) configure_output();
        if (!copy_captured_viewport()) throw hresult_error(E_BOUNDS);
        validTexture = true;
        render();
        trace_output("resize.capture_retained");
        // The source window, pool, crop and generation did not change. Frames
        // queued during the viewport handoff still belong to this same source.
        queue(dispatch, true);
        return true;
    }

    void reconcile() {
        check_thread();
        if (tearingDown || failed) return;
        if (synchronizing || resizeSizeGate.waiting) {
            reconcileDeferred = true;
            return;
        }
        if (!host || !IsWindow(host) || !active || !hasState || !IsWindowVisible(host) || IsIconic(host)) {
            stop_scope_recovery_timer();
            suspend();
            return;
        }
        if (scopeRecoveryPending) {
            arm_scope_recovery_timer();
            return;
        }
        if (zOrderNoop.waiting) {
            if (!zOrderNoop.default_returned) {
                wait_for_noop_z_order();
                return;
            }
            if (!finish_noop_z_order()) return;
        }
        SynchronizationScope synchronization(*this);
        if (!update_solid_surface()) return;
        {
            RECT currentOuter{};
            require_bool(GetWindowRect(host, &currentOuter));
            const RECT currentClient = client_bounds();
            const UINT currentDpi = GetDpiForWindow(host);
            const bool moved = !EqualRect(&currentClient, &bounds) || !EqualRect(&currentOuter, &hostBounds);
            if (viewportResizePending) {
                finish_retained_viewport(currentClient, currentOuter);
            } else if (moved && can_translate(currentClient, currentOuter, currentDpi, read_virtual_desktop())) {
                translate_capture(currentClient, currentOuter);
            } else if (!session || moved ||
                (currentDpi && currentDpi != configuredDpi)) {
                // Only prepare_resize may retain a verified raw background.
                stop_capture(resizeBackground);
                begin_capture();
            } else {
                if (dispatch && dispatch->captureClosed.load())
                    throw hresult_error(HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
                update_scene();
            }
        }
    }

    void draw_pass(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
        ID3D11PixelShader* shader, const ShaderState& settings, ID3D11ShaderResourceView* secondarySource = nullptr) {
        context->UpdateSubresource(constants.get(), 0, nullptr, &settings, 0, 0);
        context->OMSetRenderTargets(1, &target, nullptr);
        context->PSSetShader(shader, nullptr, 0);
        ID3D11ShaderResourceView* sources[]{source, secondarySource};
        context->PSSetShaderResources(0, 2, sources);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none[2]{};
        context->PSSetShaderResources(0, 2, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void bind_pass_pipeline(UINT targetWidth = 0, UINT targetHeight = 0) {
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(targetWidth ? targetWidth : width),
            static_cast<float>(targetHeight ? targetHeight : height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.get(), nullptr, 0);
        ID3D11SamplerState* sample = sampler.get();
        context->PSSetSamplers(0, 1, &sample);
        ID3D11Buffer* buffer = constants.get();
        context->PSSetConstantBuffers(0, 1, &buffer);
    }

    void draw_prefix_node(ID3D11ShaderResourceView* source, ID3D11ShaderResourceView* coverage,
        const OpticalSurface& colorOutput, const OpticalSurface& coverageOutput,
        ID3D11PixelShader* shader, const ShaderState& settings) {
        context->UpdateSubresource(constants.get(), 0, nullptr, &settings, 0, 0);
        ID3D11RenderTargetView* targets[]{colorOutput.target.get(), coverageOutput.target.get()};
        ID3D11ShaderResourceView* sources[]{source, nullptr, coverage};
        context->OMSetRenderTargets(2, targets, nullptr);
        context->PSSetShaderResources(0, 3, sources);
        context->PSSetShader(shader, nullptr, 0);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none[3]{};
        context->PSSetShaderResources(0, 3, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->GenerateMips(colorOutput.view.get());
    }

    void ensure_scissor_rasterizer() {
        if (scissorRasterizer) return;
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.ScissorEnable = TRUE;
        check_hresult(device->CreateRasterizerState(&rasterizer, scissorRasterizer.put()));
    }

    RECT draw_bounded_prefix(ID3D11PixelShader* shader, const ShaderState& settings, const float* rectangle) {
        const RECT dirty{
            static_cast<LONG>(std::clamp(std::floor(rectangle[0] - 1.0f), 0.0f, float(width))),
            static_cast<LONG>(std::clamp(std::floor(rectangle[1] - 1.0f), 0.0f, float(height))),
            static_cast<LONG>(std::clamp(std::ceil(rectangle[0] + rectangle[2] + 1.0f), 0.0f, float(width))),
            static_cast<LONG>(std::clamp(std::ceil(rectangle[1] + rectangle[3] + 1.0f), 0.0f, float(height)))};
        if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) return dirty;
        context->RSSetState(scissorRasterizer.get());
        context->RSSetScissorRects(1, &dirty);
        context->UpdateSubresource(constants.get(), 0, nullptr, &settings, 0, 0);
        ID3D11RenderTargetView* targets[]{prefixColors[1].target.get(), prefixCoverage[1].target.get()};
        ID3D11ShaderResourceView* sources[]{prefixColors[0].view.get(), nullptr, prefixCoverage[0].view.get()};
        context->OMSetRenderTargets(2, targets, nullptr);
        context->PSSetShaderResources(0, 3, sources);
        context->PSSetShader(shader, nullptr, 0);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none[3]{};
        context->PSSetShaderResources(0, 3, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->RSSetState(nullptr);
        const D3D11_BOX copy{static_cast<UINT>(dirty.left), static_cast<UINT>(dirty.top), 0,
            static_cast<UINT>(dirty.right), static_cast<UINT>(dirty.bottom), 1};
        // Only the initialized scratch rectangle is copied back. All pixels
        // outside it keep the complete prior prefix, including coverage.
        context->CopySubresourceRegion(prefixColors[0].texture.get(), 0, copy.left, copy.top, 0,
            prefixColors[1].texture.get(), 0, &copy);
        context->CopySubresourceRegion(prefixCoverage[0].texture.get(), 0, copy.left, copy.top, 0,
            prefixCoverage[1].texture.get(), 0, &copy);
        return dirty;
    }

    void render_prefix(ID3D11RenderTargetView* output, const ShaderState& settings, bool sourceChanged) {
        if (sourceChanged || !initialPrefixValid) {
            draw_prefix_node(sceneView.get(), nullptr, initialPrefix, prefixCoverage[0], prefixInit.get(), settings);
            initialPrefixValid = true;
        } else {
            const float zero[4]{};
            context->ClearRenderTargetView(prefixCoverage[0].target.get(), zero);
        }
        ensure_scissor_rasterizer();
        context->CopyResource(prefixColors[0].texture.get(), initialPrefix.texture.get());
        bool mipDirty = false;
        // Each track is painted immediately before its own thumb. Later glass
        // samples that completed prefix; no layer samples its own old output.
        for (size_t control = 0; control < controls.size(); ++control) {
            ShaderState parameters = settings;
            parameters.controls[0] = settings.controls[control];
            for (size_t track = 0; track < static_cast<size_t>(settings.trackMeta[0]); ++track) {
                if (static_cast<size_t>(settings.tracks[track].shape[2]) == control) {
                    parameters.tracks[0] = settings.tracks[track];
                    const RECT changed = draw_bounded_prefix(prefixTrack.get(), parameters, parameters.tracks[0].rectangle);
                    mipDirty = mipDirty || (changed.right > changed.left && changed.bottom > changed.top);
                }
            }
            if (mipDirty) context->GenerateMips(prefixColors[0].view.get());
            const RECT dirty = draw_bounded_prefix(prefixGlass.get(), parameters, parameters.controls[0].rectangle);
            mipDirty = dirty.right > dirty.left && dirty.bottom > dirty.top;
        }
        ID3D11ShaderResourceView* finalSources[]{initialPrefix.view.get(), prefixCoverage[0].view.get(), tintedView.get()};
        context->PSSetShaderResources(1, 3, finalSources);
        draw_pass(prefixColors[0].view.get(), output, prefixFinish.get(), settings, initialPrefix.view.get());
        ID3D11ShaderResourceView* none[3]{};
        context->PSSetShaderResources(1, 3, none);
    }

    bool submit_pending_present() {
        if (!pendingPresent.active) return true;
        const auto pending = pendingPresent;
        const auto cancelled = [&] { cancel_presentation(); };
        HRESULT result = S_FALSE;
        if (!presentationEpoch.perform(pending.epoch, [&] {
            result = swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
            if (result != DXGI_ERROR_WAS_STILL_DRAWING) check_hresult(result);
        }, cancelled)) return false;
        // Busy/occluded are backpressure, not source or device failures. Keep
        // the mounted image and this already drawn buffer; retry without Draw.
        if (result != S_OK) {
            require_bool(SetTimer(host, presentRetryTimerId,
                result == DXGI_ERROR_WAS_STILL_DRAWING ? 8 : 100, nullptr) != 0);
            return false;
        }
        KillTimer(host, presentRetryTimerId);
        pendingPresent = {};
        if (pending.announce) {
            const bool foregroundChanged = foregroundSwapMounted == pending.backgroundOnly;
            if (foregroundChanged || tiez_glass_detail::refresh_background_surface(true, pending.forced, nativeStarted)) {
                if (pending.backgroundOnly) {
                    if (!publish_background_surface(pending.epoch, "resize.prepare", false)) return false;
                } else if (!mount_output(pending.epoch, {})) return false;
            }
            resizeBackground = pending.backgroundOnly;
            if (pending.backgroundOnly) {
                resizeLease.begin(GetTickCount64());
                arm_resize_watchdog();
            } else {
                resizeLease = {};
                KillTimer(host, resizeWatchdogId);
            }
            report_frame_submission(pending.parameters, pending.backgroundOnly);
        }
        if (baseDirty) frameDeferred = true;
        return true;
    }

    void render(bool forceBackground = false, bool announce = true) {
        if (awaitingMoveCapture) return;
        if (viewportResizePending) { frameDeferred = true; return; }
        if (zOrderNoop.waiting) {
            zOrderNoopRenderDeferred = true;
            wait_for_noop_z_order();
            return;
        }
        if (resizeBackground) expire_resize_background();
        if (failed || (!validTexture && !resizeBackground) || !swap) return;
        const uint64_t outputEpoch = presentationEpoch.value;
        const uint64_t parametersAtStart = parameterRevision;
        SynchronizationScope synchronization(*this);
        if (!submit_pending_present()) return;
        ensure_scope();
        const bool backgroundOnly = tiez_glass_detail::background_only(forceBackground, validTexture,
            width, height, parameters.viewport_width, parameters.viewport_height, scale);
        // A new physical surface must not scale controls from an older DOM
        // viewport. Until both snapshots match, present only desktop material.
        ShaderState settings{};
        settings.size[0] = static_cast<float>(width);
        settings.size[1] = static_cast<float>(height);
        std::copy(std::begin(parameters.tint_rgba), std::end(parameters.tint_rgba), settings.tint);
        const float scaleX = backgroundOnly ? scale : static_cast<float>(width) / parameters.viewport_width;
        const float scaleY = backgroundOnly ? scale : static_cast<float>(height) / parameters.viewport_height;
        const float coordinateScale = std::min(scaleX, scaleY);
        if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0f || scaleY <= 0.0f)
            throw hresult_error(E_INVALIDARG);
        settings.options[0] = parameters.blur_radius * coordinateScale;
        settings.options[1] = backgroundOnly ? 0.0f : static_cast<float>(controls.size());
        settings.options[3] = coordinateScale;
        size_t trackCount = 0;
        for (size_t i = 0; !backgroundOnly && i < controls.size(); ++i) {
            const auto& control = controls[i];
            auto& output = settings.controls[i];
            output.rectangle[0] = control.x * scaleX;
            output.rectangle[1] = control.y * scaleY;
            output.rectangle[2] = control.width * scaleX;
            output.rectangle[3] = control.height * scaleY;
            output.shape[0] = control.radius * coordinateScale;
            output.shape[1] = control.pressure;
            output.shape[2] = static_cast<float>(control.material);
            if (control.track.width > 0.0f) {
                const auto& track = control.track;
                auto& outputTrack = settings.tracks[trackCount++];
                outputTrack.rectangle[0] = track.x * scaleX;
                outputTrack.rectangle[1] = track.y * scaleY;
                outputTrack.rectangle[2] = track.width * scaleX;
                outputTrack.rectangle[3] = track.height * scaleY;
                outputTrack.shape[0] = track.radius * coordinateScale;
                outputTrack.shape[1] = track.progress;
                outputTrack.shape[2] = static_cast<float>(i);
                std::copy(std::begin(track.active_rgba), std::end(track.active_rgba), outputTrack.active);
                std::copy(std::begin(track.inactive_rgba), std::end(track.inactive_rgba), outputTrack.inactive);
                for (float value : outputTrack.rectangle)
                    if (!std::isfinite(value)) throw hresult_error(E_INVALIDARG);
                if (!std::isfinite(outputTrack.shape[0]) ||
                    !std::isfinite(outputTrack.rectangle[0] + outputTrack.rectangle[2]) ||
                    !std::isfinite(outputTrack.rectangle[1] + outputTrack.rectangle[3]))
                    throw hresult_error(E_INVALIDARG);
            }
            for (float value : output.rectangle)
                if (!std::isfinite(value)) throw hresult_error(E_INVALIDARG);
            if (!std::isfinite(output.shape[0]) ||
                !std::isfinite(output.rectangle[0] + output.rectangle[2]) ||
                !std::isfinite(output.rectangle[1] + output.rectangle[3])) throw hresult_error(E_INVALIDARG);
        }
        settings.trackMeta[0] = static_cast<float>(trackCount);
        if (!std::isfinite(settings.options[0])) throw hresult_error(E_INVALIDARG);
        bind_pass_pipeline();
        // Scroll, press and track geometry do not change the desktop material.
        // Reuse its filtered texture until the source or material changes.
        const bool backgroundRecomputed = baseDirty;
        if (baseDirty) {
            // A background-only resize can consume baseDirty before the DOM
            // catches up. The optical prefix must still be rebuilt afterwards.
            initialPrefixValid = false;
            if (!render_capture_background(settings)) {
                settings.direction[0] = 1.0f;
                draw_pass(sceneView.get(), horizontalTarget.get(), blur.get(), settings);
                settings.direction[0] = 0.0f;
                settings.direction[1] = 1.0f;
                settings.options[2] = 1.0f;
                draw_pass(horizontalView.get(), tintedTarget.get(), blur.get(), settings);
            }
            baseDirty = parametersAtStart != parameterRevision;
        }
        com_ptr<ID3D11Texture2D> finalTexture;
        check_hresult(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), finalTexture.put_void()));
        if (backgroundOnly) {
            // Direct copy deliberately bypasses OpticsPS and its old controls.
            context->CopyResource(finalTexture.get(), tinted.get());
        } else {
            com_ptr<ID3D11RenderTargetView> finalTarget;
            check_hresult(device->CreateRenderTargetView(finalTexture.get(), nullptr, finalTarget.put()));
            settings.options[2] = 1.0f;
            render_prefix(finalTarget.get(), settings, backgroundRecomputed);
        }
        pendingPresent = {outputEpoch, parametersAtStart, true, backgroundOnly, forceBackground, announce};
        submit_pending_present();
    }

    bool initial_alpha_ready() {
        if (!initialAlphaPending) return false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(initialAlpha.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            require_bool(SetTimer(helper, initialAlphaRetryTimerId, 8, nullptr) != 0);
            return false;
        }
        check_hresult(result);
        bool opaque = true;
        const auto* pixels = static_cast<const unsigned char*>(mapped.pData);
        for (UINT index = 0; index < 5; ++index) opaque = opaque && pixels[index * 4 + 3] >= 250;
        context->Unmap(initialAlpha.get(), 0);
        initialAlphaPending = false;
        KillTimer(helper, initialAlphaRetryTimerId);
        return opaque;
    }

    bool accept_copied_capture(uint64_t epoch, uint64_t capturedAt) {
        return presentationEpoch.perform(epoch, [&] {
            ++captureRevision;
            capturedScene = captureTexture;
            lastCapture = {resize_context(), pixel_rect(captureBounds), capturedAt};
            if (!copy_captured_viewport()) throw hresult_error(E_BOUNDS);
            validTexture = true;
            awaitingMoveCapture = false;
            resizeLease.refresh_from_capture(capturedAt);
            captureExclusions = external_exclusions(exclusions);
            baseDirty = true;
        }, [&] { cancel_presentation(); });
    }

    bool retarget_ready() {
        if (!retargetFlushPending) return true;
        if (!dispatch) return false;
        std::lock_guard<std::mutex> lock(dispatch->retargetMutex);
        if (dispatch->completedRetarget < cropRevision) return false;
        check_hresult(dispatch->retargetResult);
        retargetFlushPending = false;
        return true;
    }

    void frame_arrived() {
        if (!session || !pool || tearingDown || failed) return;
        if (viewportResizePending || resizeSizeGate.waiting) { frameDeferred = true; return; }
        if (zOrderNoop.waiting) { wait_for_noop_z_order(); return; }
        if (synchronizing) {
            frameDeferred = true;
            return;
        }
        SynchronizationScope synchronization(*this);
        update_scene();
        // Commit completion does not prove the new crop was displayed. Keep
        // its queued WGC frame until the compositor fence completes, including
        // a static scene's sole update, without blocking window movement.
        if (!retarget_ready()) return;
        const uint64_t frameEpoch = presentationEpoch.value;
        Direct3D11CaptureFrame newest{nullptr};
        int64_t newestTime = 0;
        while (auto frame = pool.TryGetNextFrame()) {
            const int64_t rendered = frame.SystemRelativeTime().count();
            if (earliestRetargetFrame && rendered < earliestRetargetFrame) {
                ++rejectedEarlyFrames;
                frame.Close();
                continue;
            }
            if (!newest || rendered >= newestTime) {
                if (newest) newest.Close();
                newestTime = rendered;
                newest = std::move(frame);
            } else frame.Close();
        }
        if (!newest) {
            if (initialAlphaPending && initial_alpha_ready()) {
                if (!accept_copied_capture(frameEpoch, initialCaptureAt)) return;
                KillTimer(helper, watchdogId);
            }
            // Deferred control/material updates must not wait for a static
            // background to generate another WGC frame. No capture time is
            // renewed by rendering the already verified source.
            render();
            return;
        }
        struct ReturnFrame {
            Direct3D11CaptureFrame& frame;
            ~ReturnFrame() { try { frame.Close(); } catch (...) {} }
        } returnFrame{newest};
        const auto size = newest.ContentSize();
        if (size.Width != static_cast<int>(captureWidth) || size.Height != static_cast<int>(captureHeight))
            throw hresult_error(E_BOUNDS);
        const auto access = newest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        D3D11_TEXTURE2D_DESC descriptor{};
        texture->GetDesc(&descriptor);
        if (descriptor.Width != captureWidth || descriptor.Height != captureHeight || descriptor.ArraySize != 1 ||
            descriptor.SampleDesc.Count != 1 || descriptor.SampleDesc.Quality != 0 ||
            descriptor.Format != DXGI_FORMAT_B8G8R8A8_UNORM) throw hresult_error(E_BOUNDS);
        if (ensure_scope() != exclusions) {
            update_scene();
            return;
        }
        if (!validTexture) {
            if (initialAlphaPending) {
                if (initial_alpha_ready()) {
                    if (!accept_copied_capture(frameEpoch, initialCaptureAt)) return;
                    render();
                    KillTimer(helper, watchdogId);
                }
                return;
            }
            const std::array<POINT, 5> locations{{
                {static_cast<LONG>(width / 2), static_cast<LONG>(height / 2)},
                {static_cast<LONG>(width / 4), static_cast<LONG>(height / 4)},
                {static_cast<LONG>(width * 3 / 4), static_cast<LONG>(height / 4)},
                {static_cast<LONG>(width / 4), static_cast<LONG>(height * 3 / 4)},
                {static_cast<LONG>(width * 3 / 4), static_cast<LONG>(height * 3 / 4)}
            }};
            for (UINT index = 0; index < locations.size(); ++index) {
                const auto point = locations[index];
                const UINT x = static_cast<UINT>(point.x + bounds.left - captureBounds.left);
                const UINT y = static_cast<UINT>(point.y + bounds.top - captureBounds.top);
                const D3D11_BOX source{x, y, 0, x + 1, y + 1, 1};
                context->CopySubresourceRegion(initialAlpha.get(), 0, index, 0, 0, texture.get(), 0, &source);
            }
            // Own the candidate before returning its WGC frame to the pool.
            // A pending GPU readback is retried without blocking the UI thread.
            context->CopySubresourceRegion(captureTexture.get(), 0, 0, 0, 0, texture.get(), 0, nullptr);
            context->GenerateMips(captureView.get());
            initialCaptureAt = GetTickCount64();
            initialAlphaPending = true;
            if (initial_alpha_ready()) {
                if (!accept_copied_capture(frameEpoch, initialCaptureAt)) return;
                render();
                KillTimer(helper, watchdogId);
            }
            return;
        }
        context->CopySubresourceRegion(captureTexture.get(), 0, 0, 0, 0, texture.get(), 0, nullptr);
        context->GenerateMips(captureView.get());
        if (!accept_copied_capture(frameEpoch, GetTickCount64())) return;
        render();
        if (qaSource && cropRevision != acceptedCropRevision) {
            std::fprintf(stderr,
                "[native-glass-retarget-frame] revision=%llu rendered_100ns=%lld earliest_100ns=%lld rejected_early=%llu\n",
                static_cast<unsigned long long>(cropRevision), static_cast<long long>(newestTime),
                static_cast<long long>(earliestRetargetFrame), static_cast<unsigned long long>(rejectedEarlyFrames));
            std::fflush(stderr);
        }
        acceptedCropRevision = cropRevision;
        KillTimer(helper, watchdogId);
    }

    void update(const TiezGlassState& state) {
        phase = "update.validate_parameters";
        check_thread();
        if (!std::isfinite(state.viewport_width) || !std::isfinite(state.viewport_height) ||
            state.viewport_width < 1.0f || state.viewport_height < 1.0f ||
            !std::isfinite(state.blur_radius) || state.blur_radius < 0.0f || state.blur_radius > 128.0f ||
            state.control_count > maxControls || (state.control_count && !state.controls))
            throw hresult_error(E_INVALIDARG);
        for (float channel : state.tint_rgba)
            if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) throw hresult_error(E_INVALIDARG);
        std::vector<TiezGlassControl> copied;
        if (state.control_count) copied.assign(state.controls, state.controls + state.control_count);
        for (const auto& control : copied) {
            for (float value : {control.x, control.y, control.width, control.height, control.radius, control.pressure})
                if (!std::isfinite(value)) throw hresult_error(E_INVALIDARG);
            if (control.width < 0.0f || control.height < 0.0f || control.radius < 0.0f ||
                control.pressure < 0.0f || control.pressure > 1.0f || control.material > 2u) throw hresult_error(E_INVALIDARG);
            const auto& track = control.track;
            for (float value : {track.x, track.y, track.width, track.height, track.radius, track.progress})
                if (!std::isfinite(value)) throw hresult_error(E_INVALIDARG);
            for (float channel : track.active_rgba)
                if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) throw hresult_error(E_INVALIDARG);
            for (float channel : track.inactive_rgba)
                if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) throw hresult_error(E_INVALIDARG);
            const bool trackEnabled = track.width > 0.0f && track.height > 0.0f;
            if ((!trackEnabled && (track.width != 0.0f || track.height != 0.0f)) ||
                track.radius < 0.0f || track.radius > 0.5f * std::min(track.width, track.height) ||
                track.progress < 0.0f || track.progress > 1.0f) throw hresult_error(E_INVALIDARG);
        }
        baseDirty = baseDirty || parameters.viewport_width != state.viewport_width ||
            parameters.viewport_height != state.viewport_height || parameters.blur_radius != state.blur_radius ||
            !std::equal(std::begin(parameters.tint_rgba), std::end(parameters.tint_rgba), std::begin(state.tint_rgba));
        parameters = state;
        parameters.controls = nullptr;
        controls = std::move(copied);
        hasState = true;
        ++parameterRevision;
        if (synchronizing || viewportResizePending) {
            baseDirty = frameDeferred = true;
            request_reconcile();
            return;
        }
        // Return geometry/material IPC without rendering or waiting for vsync.
        // The coalesced frame notification draws the newest state, also when
        // an unchanged desktop produces no further WGC frames.
        request_reconcile();
        queue(dispatch, true);
    }

    ~TiezGlass() {
        tearingDown = true;
        scopeRecoveryPending = false;
        stop_scope_recovery_timer();
        callback = nullptr;
        callbackContext = nullptr;
        if (subclassInstalled && IsWindow(host)) RemoveWindowSubclass(host, host_proc, subclassId);
        subclassInstalled = false;
        stop_capture();
        if (composition) {
            composition->WaitForCommitCompletion();
        }
        restore_region();
        hostTarget = nullptr;
        rootVisual = nullptr;
        solidVisual = nullptr;
        solidSurface = nullptr;
        backgroundVisual = nullptr;
        hostVisual = nullptr;
        swap = nullptr;
        composition = nullptr;
        if (dwm) FreeLibrary(dwm);
    }
};

namespace {
LRESULT CALLBACK helper_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* renderer = reinterpret_cast<TiezGlass*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        renderer = static_cast<TiezGlass*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
    }
    if (message == WM_ERASEBKGND && renderer && renderer->qaColoredAuxiliary) {
        RECT rectangle{};
        if (GetClientRect(window, &rectangle)) {
            const COLORREF color = window == renderer->cover ? RGB(255, 0, 255) : RGB(0, 255, 255);
            const HBRUSH brush = CreateSolidBrush(color);
            if (brush) {
                const int painted = FillRect(reinterpret_cast<HDC>(wparam), &rectangle, brush);
                DeleteObject(brush);
                if (painted) return 1;
            }
        }
    }
    if (renderer && window == renderer->helper && !renderer->tearingDown) {
        try {
            const auto signal = renderer->dispatch;
            if (signal && signal->alive.load() && wparam == signal->generation) {
                if (message == frameMessage) {
                    signal->frameQueued.store(false);
                    renderer->frame_arrived();
                    return 0;
                }
                if (message == reconcileMessage) {
                    signal->scopeQueued.store(false);
                    renderer->reconcile();
                    return 0;
                }
            }
            if (message == WM_TIMER && wparam == initialAlphaRetryTimerId) {
                queue(signal, true);
                return 0;
            }
            if (message == WM_TIMER && wparam == watchdogId && !renderer->validTexture) {
                renderer->fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
                return 0;
            }
        } catch (const SceneTemporarilyUnavailable& change) { renderer->suspend_for_scene(change.reason); return 0; }
        catch (const HostTranslationPending&) { renderer->request_reconcile(); return 0; }
        catch (const HostStateChanged& change) { renderer->suspend_for_host_change(change.reason); return 0; }
        catch (const MissingHostRegion&) { renderer->recover_missing_region(); return 0; }
        catch (...) { renderer->fail(winrt::to_hresult()); return 0; }
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_NCDESTROY) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK host_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR data) {
    auto* renderer = reinterpret_cast<TiezGlass*>(data);
    if (!renderer || renderer->tearingDown) return DefSubclassProc(window, message, wparam, lparam);
    const UINT_PTR rendererIdentity = renderer->identity;
    uint64_t noopZOrderGeneration = 0;
    // A WM_SIZE already on the stack before POSITIONCHANGED (or from an older
    // resize) cannot complete a later resize just because its numbers match.
    const uint64_t resizeMessageGeneration = message == WM_SIZE && renderer->resizeSizeGate.position_known
        ? renderer->resizeSizeGate.generation : 0;
    try {
        if (renderer->qaSource) {
            if (message == WM_NCCALCSIZE) renderer->trace_host_layout("message.nccalcsize");
            else if (message == WM_NCPAINT) renderer->trace_host_layout("message.ncpaint");
            else if (message == WM_ERASEBKGND) renderer->trace_host_layout("message.erase_background");
            else if (message == WM_SIZE) renderer->trace_host_layout("message.size_begin");
            else if (message == WM_NCACTIVATE) renderer->trace_host_layout("message.ncactivate");
        }
        if (message == WM_NCACTIVATE && renderer->owns_nonclient_activation(window)) {
            // Preserve Tao's activation callbacks and wParam. The documented
            // -1 only asks DefWindowProc to omit native caption repainting.
            lparam = -1;
            renderer->trace_host_layout("activation.ncpaint_suppressed");
        }
        if (message == WM_NCPAINT && renderer->owns_resize_nonclient_paint(window)) {
            // Tao keeps caption styles for its undecorated shadow frame. Its
            // default NC paint can expose those buttons in a newly widened
            // client before WebView/DComp cover it. The owned client mask needs
            // no default frame painting during this verified resize interval.
            renderer->trace_host_layout("resize.ncpaint_suppressed");
            return 0;
        }
        if (message == WM_TIMER && wparam == resizeWatchdogId) {
            renderer->expire_resize_background();
            return 0;
        }
        if (message == WM_TIMER && wparam == scopeRecoveryTimerId) {
            renderer->retry_scene_scope();
            return 0;
        }
        if (message == WM_TIMER && wparam == zOrderNoopTimerId) {
            renderer->expire_noop_z_order();
            return 0;
        }
        if (message == WM_TIMER && wparam == presentRetryTimerId) {
            if (!renderer->synchronizing) {
                TiezGlass::SynchronizationScope synchronization(*renderer);
                renderer->submit_pending_present();
            }
            return 0;
        }
        if (message == WM_DISPLAYCHANGE) renderer->display_changed();
        if (message == WM_DPICHANGED || (message == WM_SHOWWINDOW && !wparam)) {
            renderer->forget_resize_background();
            renderer->detach_output();
            renderer->report(0);
            renderer->suspend();
        }
        if (message == WM_WINDOWPOSCHANGING && !renderer->synchronizing) {
            const auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
            // Reentrant requests cannot inherit the outer request's proof or
            // deadline, even if they happen to ask for the same final order.
            if (renderer->zOrderNoop.waiting) renderer->withdraw_noop_z_order();
            if ((position->flags & (SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER)) !=
                (SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER) ||
                (position->flags & (SWP_HIDEWINDOW | SWP_FRAMECHANGED))) {
                noopZOrderGeneration = renderer->begin_noop_z_order(*position);
                if (!noopZOrderGeneration && !renderer->position_change_can_retarget(*position) &&
                    !renderer->prepare_resize(*position))
                    renderer->suspend();
            }
        }
        if (message == WM_WINDOWPOSCHANGED || message == WM_DPICHANGED || message == WM_SHOWWINDOW) {
            if (!renderer->synchronizing) {
                if (message == WM_WINDOWPOSCHANGED)
                    renderer->refresh_resize_region(*reinterpret_cast<WINDOWPOS*>(lparam));
                // SWP_NOSENDCHANGING can skip the pre-move message. Hide the
                // helpers synchronously here, before the queued rebuild, too.
                if (message == WM_WINDOWPOSCHANGED && renderer->session) {
                    const auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
                    const bool orderRetained = renderer->zOrderNoop.waiting &&
                        renderer->finish_noop_z_order(position);
                    RECT outer{};
                    require_bool(GetWindowRect(window, &outer));
                    const RECT client = renderer->client_bounds();
                    const bool resizeRetained = renderer->viewportResizePending &&
                        renderer->resizeSizeGate.position_known &&
                        TiezGlass::EqualRectValue(client, renderer->pendingViewportClient) &&
                        TiezGlass::EqualRectValue(outer, renderer->pendingViewportOuter) &&
                        (position->flags & SWP_NOZORDER);
                    const bool retained = renderer->position_change_can_retarget(*position) &&
                        renderer->can_translate(client, outer, GetDpiForWindow(window), renderer->read_virtual_desktop());
                    if (!IsWindowVisible(window) || IsIconic(window) ||
                        ((!TiezGlass::EqualRectValue(client, renderer->bounds) ||
                          !TiezGlass::EqualRectValue(outer, renderer->hostBounds)) && !retained && !resizeRetained) ||
                        (!(position->flags & SWP_NOZORDER) && !orderRetained) ||
                        (position->flags & SWP_FRAMECHANGED)) renderer->suspend();
                }
            }
            renderer->request_reconcile();
        }
        if (message == reconcileMessage && wparam == renderer->identity) {
            renderer->reconcile();
            return 0;
        }
        if (message == WM_NCDESTROY) {
            renderer->suspend();
            renderer->detach_output(false);
            renderer->host = nullptr;
            renderer->subclassInstalled = false;
            RemoveWindowSubclass(window, host_proc, subclassId);
        }
    } catch (const SceneTemporarilyUnavailable& change) { renderer->suspend_for_scene(change.reason); }
    catch (const HostTranslationPending&) { renderer->request_reconcile(); }
    catch (const HostStateChanged& change) { renderer->suspend_for_host_change(change.reason); }
    catch (const MissingHostRegion&) { renderer->recover_missing_region(); }
    catch (...) { renderer->fail(winrt::to_hresult()); }
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if ((message == WM_SIZE && resizeMessageGeneration) || noopZOrderGeneration) {
        // Wry's SetBounds and user callbacks may pump STA messages and retire
        // this renderer. Reacquire the installed subclass before dereferencing
        // anything; identity also rejects HWND/heap-address reuse.
        DWORD_PTR current = 0;
        if (GetWindowSubclass(window, host_proc, subclassId, &current) && current == data) {
            auto* live = reinterpret_cast<TiezGlass*>(current);
            if (live->identity == rendererIdentity && live->host == window && !live->tearingDown) {
                if (message == WM_SIZE) live->resize_size_processed(wparam, lparam, resizeMessageGeneration);
                else {
                    try {
                        // Downstream procedures may rewrite WINDOWPOS. Recheck
                        // their final request before returning it to USER32.
                        live->noop_z_order_default_returned(noopZOrderGeneration,
                            *reinterpret_cast<WINDOWPOS*>(lparam));
                    } catch (const SceneTemporarilyUnavailable& change) { live->suspend_for_scene(change.reason); }
                    catch (const HostTranslationPending&) { live->suspend_for_host_change("z_order_geometry"); }
                    catch (const HostStateChanged& change) { live->suspend_for_host_change(change.reason); }
                    catch (const MissingHostRegion&) { live->recover_missing_region(); }
                    catch (...) { live->fail(winrt::to_hresult()); }
                }
            }
        }
    }
    return result;
}
} // namespace

extern "C" int32_t tiez_glass_create(void* host, void* qa_source, void* context,
    TiezGlassStateCallback on_state, TiezGlass** result) noexcept {
    if (!result) return E_POINTER;
    *result = nullptr;
    try {
        auto renderer = std::make_unique<TiezGlass>(static_cast<HWND>(host),
            static_cast<HWND>(qa_source), context, on_state);
        try { renderer->initialize(); }
        catch (...) { renderer->diagnose(winrt::to_hresult()); throw; }
        *result = renderer.release();
        return S_OK;
    } catch (...) { return static_cast<int32_t>(winrt::to_hresult()); }
}

extern "C" int32_t tiez_glass_update(TiezGlass* glass, const TiezGlassState* state) noexcept {
    if (!glass || !state) return E_POINTER;
    try { glass->update(*state); return S_OK; }
    catch (const SceneTemporarilyUnavailable& change) { glass->suspend_for_scene(change.reason); return S_OK; }
    catch (const HostTranslationPending&) { glass->request_reconcile(); return S_OK; }
    catch (const HostStateChanged& change) { glass->suspend_for_host_change(change.reason); return S_OK; }
    catch (const MissingHostRegion&) { glass->recover_missing_region(); return S_OK; }
    catch (...) {
        const HRESULT error = winrt::to_hresult();
        if (GetCurrentThreadId() == glass->thread) glass->fail(error);
        return static_cast<int32_t>(error);
    }
}

extern "C" int32_t tiez_glass_set_active(TiezGlass* glass, bool active) noexcept {
    if (!glass) return E_POINTER;
    try {
        glass->check_thread();
        glass->active = active;
        if (active) glass->reconcile();
        else {
            glass->scopeRecoveryPending = false;
            glass->stop_scope_recovery_timer();
            glass->suspend();
        }
        return S_OK;
    } catch (const SceneTemporarilyUnavailable& change) {
        glass->suspend_for_scene(change.reason);
        return S_OK;
    } catch (const HostTranslationPending&) {
        glass->request_reconcile();
        return S_OK;
    } catch (const HostStateChanged& change) {
        glass->suspend_for_host_change(change.reason);
        return S_OK;
    } catch (const MissingHostRegion&) {
        glass->recover_missing_region();
        return S_OK;
    } catch (...) {
        const HRESULT error = winrt::to_hresult();
        if (GetCurrentThreadId() == glass->thread) glass->fail(error);
        return static_cast<int32_t>(error);
    }
}

extern "C" void tiez_glass_destroy(TiezGlass* glass) noexcept {
    // The caller owns the UI-thread contract; deletion is synchronous and all
    // callback dispatch is disconnected before any resource is released.
    delete glass;
}
