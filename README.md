# TieZ Clipboard WX

A Windows clipboard manager for personal use, based on [jimuzhe/tiez-clipboard](https://github.com/jimuzhe/tiez-clipboard).

[简体中文](README.zh-CN.md)

This fork keeps clipboard history, text and rich text, images, file paths, search, tags, pinned items, sequential paste, external editing, privacy masking, emoji favorites and seven built-in themes. AI, cloud synchronization, MQTT, LAN transfer, the theme store and announcements have been removed.

Automatic updates use signed releases from [ReasonW6/tiez-clipboard-WX](https://github.com/ReasonW6/tiez-clipboard-WX/releases).

## Development

Requirements: Windows 10/11 x64, Node.js 22.12+ or 24, stable Rust MSVC, Visual Studio C++ Build Tools and WebView2.

    npm ci
    npm run tauri:dev

The dev script starts only the frontend on port 1420. Native clipboard functions require tauri:dev.

## Checks and builds

    npm test
    npm run build
    npm run test:rust
    npm run tauri:build
    npm run build:portable

The default build creates an NSIS installer without requiring an updater signing key. The release script creates a signed update installer; see [release instructions](docs/RELEASING.md).

The portable archive is written to artifacts/portable/tiez-portable.zip. Extract it before launching. Its adjacent data directory holds the database and attachments. Sensitive clipboard data remains protected by the current Windows account. Automatic updates use the setup edition; replace the portable executable manually to keep using portable storage.

## Code layout

- src/features: React components and feature state.
- src/shared/hooks: history, settings, keyboard navigation, clipboard events and updates.
- src/shared/config/themes.ts: built-in theme registry.
- src-tauri/src/main.rs: desktop initialization and Tauri command registration.
- src-tauri/src/services/clipboard: clipboard capture and processing pipeline.
- src-tauri/src/services/clipboard_ops.rs: clipboard writing and paste/focus handling.
- src-tauri/src/infrastructure/repository: SQLite access and migrations.

Migration 11 removes retired settings and sync metadata while preserving clipboard history, tags and attachments. An old store theme falls back to Mica. Retired browser tokens and cached store styles are cleaned on startup.

License: [GPL-3.0](LICENSE). Original project credits are retained in the repository history and license.

### Material themes and UI tests

Mica, Acrylic and Liquid Glass support light, dark and system appearance. Liquid Glass adds clarity (0–100%) and control blur (0–32px) sliders in Appearance settings. Windows supplies the desktop backdrop; the blur slider affects in-app navigation and controls, not the fixed Windows desktop blur radius. Windows 10 uses a solid fallback.

Design references: [Apple Materials](https://developer.apple.com/design/human-interface-guidelines/materials), [Microsoft Mica](https://learn.microsoft.com/en-us/windows/apps/design/style/mica) and [Acrylic](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic).

Run `npx playwright install chromium` once, then `npm run test:ui`. The test harness uses fictional data and intercepts every Tauri command; it never accesses the real clipboard or database. Set `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH` to use an existing Chromium executable. See [verification notes](docs/UI-VERIFICATION.md).
