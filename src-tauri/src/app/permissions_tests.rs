use serde_json::{json, Value};
use tauri::test::{get_ipc_response, mock_builder, MockRuntime, INVOKE_KEY};
use tauri::{App, WebviewWindow, WebviewWindowBuilder};

fn test_app() -> App<MockRuntime> {
    let mut context = tauri::generate_context!();
    context.config_mut().app.windows.clear();
    mock_builder().build(context).unwrap()
}

fn test_window(app: &App<MockRuntime>, label: &str) -> WebviewWindow<MockRuntime> {
    // MockRuntime never opens WebView2; keep even its directory checks in the workspace.
    WebviewWindowBuilder::new(app, label, Default::default())
        .data_directory(std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target"))
        .build()
        .unwrap()
}

fn request(
    window: &WebviewWindow<MockRuntime>,
    command: &str,
    body: Value,
) -> Result<Value, Value> {
    get_ipc_response(
        window,
        tauri::webview::InvokeRequest {
            cmd: command.into(),
            callback: tauri::ipc::CallbackFn(0),
            error: tauri::ipc::CallbackFn(1),
            url: window.url().unwrap(),
            body: tauri::ipc::InvokeBody::Json(body),
            headers: Default::default(),
            invoke_key: INVOKE_KEY.into(),
        },
    )
    .map(|response| response.deserialize::<Value>().unwrap())
}

#[test]
fn review_native_theme_query_is_allowed_in_local_windows() {
    let app = test_app();
    for label in ["main", "compact-preview"] {
        let window = test_window(&app, label);
        let response = request(&window, "plugin:window|theme", json!({"label": label}));
        assert_eq!(
            response,
            Ok(json!("light")),
            "Theme lookup failed for {label}"
        );
    }
}

#[test]
fn review_preview_can_send_events_to_main_window() {
    let app = test_app();
    let _main = test_window(&app, "main");
    let preview = test_window(&app, "compact-preview");
    let response = request(
        &preview,
        "plugin:event|emit_to",
        json!({
            "target": {"kind": "AnyLabel", "label": "main"},
            "event": "compact-preview-mounted", "payload": true
        }),
    );
    assert_eq!(response, Ok(Value::Null));
}

#[test]
fn review_unlisted_window_cannot_query_native_theme() {
    let app = test_app();
    let window = test_window(&app, "unlisted");
    assert!(request(&window, "plugin:window|theme", json!({"label": "unlisted"})).is_err());
}
