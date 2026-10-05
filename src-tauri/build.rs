fn main() {
    tauri_build::build();
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        build_glass_renderer();
        // Tauri embeds the app manifest only in its main binary. The native
        // material probe also needs Common Controls v6 to start WebView2.
        println!("cargo:rustc-link-arg-examples=/MANIFEST:EMBED");
        println!("cargo:rustc-link-arg-examples=/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'");
    }
}

fn build_glass_renderer() {
    use std::{env, path::PathBuf, process::Command};

    assert_eq!(
        env::var("CARGO_CFG_TARGET_ENV").as_deref(),
        Ok("msvc"),
        "The Windows glass renderer requires the MSVC toolchain"
    );
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo OUT_DIR"));
    let mut native = cc::Build::new();
    native
        .cpp(true)
        .std("c++17")
        .file("native/glass_renderer.cpp")
        .include("native")
        .include(&output)
        .define("UNICODE", None)
        .define("_UNICODE", None)
        .flag("/EHsc")
        .flag("/utf-8");
    let compiler = native.get_compiler();
    // Use the SDK selected by cc/MSVC, including clean shells used by CI.
    let mut candidates = Vec::new();
    for (key, value) in compiler.env() {
        if key.to_string_lossy().eq_ignore_ascii_case("PATH") {
            candidates.extend(env::split_paths(value).map(|path| path.join("fxc.exe")));
        }
        if key.to_string_lossy().eq_ignore_ascii_case("INCLUDE") {
            for include in env::split_paths(value) {
                for directory in include.ancestors() {
                    if directory
                        .file_name()
                        .is_some_and(|name| name.to_string_lossy().eq_ignore_ascii_case("include"))
                    {
                        if let (Some(sdk), Ok(relative)) =
                            (directory.parent(), include.strip_prefix(directory))
                        {
                            if let Some(version) = relative.components().next() {
                                candidates.push(
                                    sdk.join("bin")
                                        .join(version.as_os_str())
                                        .join("x64/fxc.exe"),
                                );
                            }
                        }
                    }
                }
            }
        }
    }
    if let Some(path) = env::var_os("PATH") {
        candidates.extend(env::split_paths(&path).map(|path| path.join("fxc.exe")));
    }
    let fxc = candidates
        .into_iter()
        .find(|path| path.is_file())
        .expect("Windows SDK fxc.exe is required to build the native glass shaders");
    for (entry, profile, name) in [
        ("VSMain", "vs_5_0", "vs"),
        ("BlurPS", "ps_5_0", "blur_ps"),
        ("OpticsPS", "ps_5_0", "optics_ps"),
        ("PrefixInitPS", "ps_5_0", "prefix_init_ps"),
        ("PrefixGlassPS", "ps_5_0", "prefix_glass_ps"),
        ("PrefixTrackPS", "ps_5_0", "prefix_track_ps"),
        ("PrefixFinishPS", "ps_5_0", "prefix_finish_ps"),
    ] {
        let result = Command::new(&fxc)
            .args([
                "/nologo", "/Ges", "/WX", "/O3", "/T", profile, "/E", entry, "/I", "native", "/Fh",
            ])
            .arg(output.join(format!("glass_renderer_{name}.h")))
            .arg("/Fo")
            .arg(output.join(format!("glass_renderer_{name}.cso")))
            .arg("/Vn")
            .arg(format!("g_glass_renderer_{name}"))
            .arg("native/glass_renderer.hlsl")
            .output()
            .expect("Run fxc.exe");
        assert!(
            result.status.success(),
            "Glass shader {entry} failed:\n{}\n{}",
            String::from_utf8_lossy(&result.stdout),
            String::from_utf8_lossy(&result.stderr)
        );
    }
    native.compile("tiez_glass_renderer");
    for library in [
        "windowsapp",
        "d3d11",
        "dxgi",
        "dxguid",
        "dcomp",
        "dwmapi",
        "user32",
        "gdi32",
        "comctl32",
    ] {
        // The production bin and material example include the bridge module
        // directly; Cargo otherwise attaches link-lib only to this package's
        // separate library target.
        println!("cargo:rustc-link-arg={library}.lib");
    }
    for source in [
        "native/glass_renderer.h",
        "native/glass_background_surface.h",
        "native/glass_capture_region.h",
        "native/glass_zorder_noop.h",
        "native/glass_renderer.cpp",
        "native/glass_renderer.hlsl",
        "native/glass_optics.hlsli",
        "native/glass_prefix.hlsli",
    ] {
        println!("cargo:rerun-if-changed={source}");
    }
}
