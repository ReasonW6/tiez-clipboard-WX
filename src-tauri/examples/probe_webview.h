// Owned-fixture QA foreground only. No production WebView or material backend.
#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>

class ProbeWebView final {
public:
    struct Callbacks {
        // Ready means the local document navigated successfully, not visual QA.
        std::function<void()> ready;
        std::function<void(const char* phase, HRESULT)> error;
        // Called for initial key-down only, after marking the key handled.
        // PostMessage to the base dispatcher here; do not synchronously destroy
        // windows, run a modal loop, or call capture/COM work from this callback.
        std::function<void(UINT virtualKey, LPARAM keyData)> accelerator;
    };

    ProbeWebView() = default;
    ProbeWebView(const ProbeWebView&) = delete;
    ProbeWebView& operator=(const ProbeWebView&) = delete;
    ~ProbeWebView() { Close(); }

    // All calls/destruction belong to the supplied HWND's existing STA thread.
    // userDataFolder must be a dedicated test-profile path, never user app data.
    // The caller owns profile cleanup after the browser process has exited.
    HRESULT Initialize(HWND parent, RECT clientBounds, const std::wstring& userDataFolder,
        Callbacks callbacks);
    HRESULT Resize(RECT clientBounds);
    HRESULT NotifyParentMoved();
    void Close() noexcept;

private:
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    static constexpr wchar_t identityProperty[] = L"TieZ.OwnedSceneProbe.WebView.State";
    struct State {
        HWND parent = nullptr;
        RECT bounds{};
        bool closed = false, ready = false;
        Callbacks callbacks;
        std::wstring userDataFolder;
        ComPtr<ICoreWebView2Environment> environment;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;

        bool Live() const noexcept {
            // IsWindow alone cannot distinguish a recycled HWND. Window
            // properties disappear with its destruction; each state has its
            // own identity and old asynchronous callbacks hold only weak_ptrs.
            return !closed && IsWindow(parent) &&
                GetPropW(parent, identityProperty) == reinterpret_cast<HANDLE>(const_cast<State*>(this));
        }
        void Close() noexcept {
            if (closed) return;
            closed = true;
            callbacks = {};
            if (GetPropW(parent, identityProperty) == reinterpret_cast<HANDLE>(this))
                RemovePropW(parent, identityProperty);
            parent = nullptr;
            // Close explicitly releases registered event handlers. All handlers
            // capture weak state, so no controller/event-handler cycle exists.
            if (controller) controller->Close();
            webview.Reset();
            controller.Reset();
            environment.Reset();
        }
        void Fail(const char* phase, HRESULT hr) noexcept {
            auto report = std::move(callbacks.error);
            Close();
            if (report) { try { report(phase, hr); } catch (...) {} }
        }
        bool Check(HRESULT hr, const char* phase) noexcept {
            if (SUCCEEDED(hr)) return true;
            Fail(phase, hr);
            return false;
        }
    };
    std::shared_ptr<State> state_;
    static void Configure(const std::shared_ptr<State>& state);

    // Plain sharp foreground controls; the empty middle shows the real native
    // shader output. No CSS blur, backdrop-filter, image, external font, or URL.
    // The page makes no network/clipboard requests; this is not a policy for
    // service traffic initiated by the installed WebView2 Runtime itself.
    static constexpr wchar_t document[] = LR"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'none'; base-uri 'none'; form-action 'none'">
<title>Owned scene foreground QA</title>
<style>
* { box-sizing:border-box }
html,body { margin:0; width:100%; height:100%; background:transparent; overflow:hidden }
body { color:#fff; font:13px/1.4 'Segoe UI',sans-serif }
header { position:absolute; top:18px; left:18px; right:18px; pointer-events:none }
h1 { display:inline-block; margin:0; padding:5px 9px; border-radius:5px;
     background:#13233b; font-size:16px; font-weight:650 }
header p { margin:4px 0 0; text-shadow:0 1px 2px #000,0 0 3px #000 }
footer { position:absolute; left:18px; right:18px; bottom:18px }
label { display:inline-block; margin-bottom:4px; padding:2px 6px; border-radius:3px;
        background:#13233b; font-size:11px }
.controls { display:flex; gap:7px }
input,button { min-width:0; height:34px; border:1px solid #d3dfef; border-radius:6px;
               font:inherit; padding:5px 9px }
input { width:100%; color:#fff; background:#13233b }
button { flex:none; color:#13233b; background:#ffdf89; font-weight:600; cursor:pointer }
button:active { background:#efc75d }
:focus-visible { outline:3px solid #77cfff; outline-offset:2px }
output { display:inline-block; margin-top:5px; padding:2px 6px; border-radius:3px;
         background:#13233b; font-size:11px; font-variant-numeric:tabular-nums }
@media (max-height:280px) {
 header { top:10px; left:12px; right:12px }
 header p { display:none }
 footer { bottom:10px; left:12px; right:12px }
 input,button { height:28px; padding:3px 6px }
}
</style></head><body>
<header><h1>Foreground QA</h1><p>Live glass remains visible in the center</p></header>
<footer><label for="sample">Fictional test text</label>
<div class="controls"><input id="sample" value="Aurora sample 27" maxlength="40"
 autocomplete="off" spellcheck="false" aria-label="Fictional test text">
<button id="count" type="button">Count</button></div>
<output id="result" aria-live="polite">Clicks: 0</output></footer>
<script>
let clicks=0;
document.getElementById('count').addEventListener('click',()=>{
 document.getElementById('result').textContent='Clicks: '+(++clicks);
});
</script></body></html>)html";
};

inline HRESULT ProbeWebView::Initialize(HWND parent, RECT clientBounds,
    const std::wstring& userDataFolder, Callbacks callbacks) {
    if (state_) return E_UNEXPECTED;
    if (!IsWindow(parent) || GetWindowThreadProcessId(parent, nullptr) != GetCurrentThreadId() ||
        userDataFolder.empty() || !callbacks.accelerator ||
        clientBounds.right < clientBounds.left || clientBounds.bottom < clientBounds.top)
        return E_INVALIDARG;
    if (GetPropW(parent, identityProperty)) return E_UNEXPECTED;
    auto state = std::make_shared<State>();
    state->parent = parent;
    state->bounds = clientBounds;
    state->userDataFolder = userDataFolder;
    state->callbacks = std::move(callbacks);
    if (!SetPropW(parent, identityProperty, reinterpret_cast<HANDLE>(state.get())))
        return HRESULT_FROM_WIN32(GetLastError());
    state_ = state;
    const std::weak_ptr<State> weak = state;
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, state->userDataFolder.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                const auto current = weak.lock();
                if (!current || !current->Live()) return S_OK;
                if (!current->Check(FAILED(result) ? result : (environment ? S_OK : E_POINTER),
                    "webview_environment")) return S_OK;
                current->environment = environment;
                const HRESULT create = environment->CreateCoreWebView2Controller(current->parent,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [weak](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            const auto active = weak.lock();
                            if (!active || !active->Live()) {
                                if (controller) controller->Close();
                                return S_OK;
                            }
                            if (!active->Check(FAILED(controllerResult) ? controllerResult :
                                (controller ? S_OK : E_POINTER), "webview_controller")) return S_OK;
                            active->controller = controller;
                            try { Configure(active); }
                            catch (...) { active->Fail("webview_configure_exception", E_FAIL); }
                            return S_OK;
                        }).Get());
                current->Check(create, "webview_create_controller");
                return S_OK;
            }).Get());
    if (FAILED(hr)) Close();
    return hr;
}

inline void ProbeWebView::Configure(const std::shared_ptr<State>& state) {
    if (!state->Check(state->controller->put_IsVisible(FALSE), "webview_hide_initial")) return;
    ComPtr<ICoreWebView2Controller2> transparent;
    if (!state->Check(state->controller.As(&transparent), "webview_controller2")) return;
    if (!state->Check(transparent->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0}),
        "webview_transparent_background")) return;
    ComPtr<ICoreWebView2Controller4> dropControl;
    if (!state->Check(state->controller.As(&dropControl), "webview_controller4")) return;
    if (!state->Check(dropControl->put_AllowExternalDrop(FALSE), "webview_disable_external_drop")) return;
    if (!state->Check(state->controller->put_Bounds(state->bounds), "webview_bounds")) return;
    if (!state->Check(state->controller->get_CoreWebView2(&state->webview), "webview_core")) return;
    ComPtr<ICoreWebView2Settings> settings;
    if (!state->Check(state->webview->get_Settings(&settings), "webview_settings")) return;
    if (!state->Check(settings->put_AreDefaultContextMenusEnabled(FALSE), "webview_context_menu")) return;
    if (!state->Check(settings->put_AreDevToolsEnabled(FALSE), "webview_devtools")) return;
    if (!state->Check(settings->put_IsWebMessageEnabled(FALSE), "webview_disable_messages")) return;
    ComPtr<ICoreWebView2Settings3> settings3;
    if (!state->Check(settings.As(&settings3), "webview_settings3")) return;
    if (!state->Check(settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE), "webview_browser_keys")) return;
    const std::weak_ptr<State> weak = state;
    EventRegistrationToken token{};
    if (!state->Check(state->controller->add_AcceleratorKeyPressed(
        Microsoft::WRL::Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [weak](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                const auto active = weak.lock();
                if (!active || !active->Live()) return S_OK;
                UINT key = 0;
                HRESULT hr = args->get_VirtualKey(&key);
                if (FAILED(hr) || (key != VK_ESCAPE && (key < VK_F1 || key > VK_F6))) return hr;
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                if (FAILED(hr = args->get_KeyEventKind(&kind))) return hr;
                // Preserve native Alt combinations, especially Alt+F4 close.
                if (kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN ||
                    kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_UP) return S_OK;
                hr = args->put_Handled(TRUE);
                if (FAILED(hr)) return hr;
                if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) return S_OK;
                INT keyData = 0;
                if (FAILED(hr = args->get_KeyEventLParam(&keyData))) return hr;
                if ((static_cast<UINT>(keyData) & (1u << 30)) != 0) return S_OK;
                try {
                    auto forward = active->callbacks.accelerator;
                    if (forward) forward(key, static_cast<LPARAM>(keyData));
                }
                catch (...) { return E_FAIL; }
                return S_OK;
            }).Get(), &token), "webview_accelerators")) return;
    if (!state->Check(state->webview->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                const auto active = weak.lock();
                if (!active || !active->Live() || active->ready) return S_OK;
                BOOL succeeded = FALSE;
                if (!active->Check(args->get_IsSuccess(&succeeded), "webview_navigation_status")) return S_OK;
                if (!active->Check(succeeded ? S_OK : E_FAIL, "webview_navigation")) return S_OK;
                if (!active->Check(active->controller->put_IsVisible(TRUE), "webview_show")) return S_OK;
                active->ready = true;
                try {
                    auto ready = active->callbacks.ready;
                    if (ready) ready();
                }
                catch (...) { active->Fail("webview_ready_callback", E_FAIL); }
                return S_OK;
            }).Get(), &token), "webview_navigation_handler")) return;
    state->Check(state->webview->NavigateToString(document), "webview_navigate_to_string");
}

inline HRESULT ProbeWebView::Resize(RECT clientBounds) {
    if (!state_ || !state_->Live()) return E_UNEXPECTED;
    if (clientBounds.right < clientBounds.left || clientBounds.bottom < clientBounds.top) return E_INVALIDARG;
    state_->bounds = clientBounds;
    return state_->controller ? state_->controller->put_Bounds(clientBounds) : S_OK;
}

inline HRESULT ProbeWebView::NotifyParentMoved() {
    if (!state_ || !state_->Live()) return E_UNEXPECTED;
    return state_->controller ? state_->controller->NotifyParentWindowPositionChanged() : S_OK;
}

inline void ProbeWebView::Close() noexcept {
    // Call before DestroyWindow(output), including while creation is pending.
    auto state = std::move(state_);
    if (state) state->Close();
}
