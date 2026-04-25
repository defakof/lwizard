# LWizard

LWizard is a native C++ plugin for [Mod Organizer 2](https://github.com/ModOrganizer2/modorganizer), focused on Baldur's Gate 3 workflows.

It currently provides:

- a Tools submenu with the main LWizard dialog and an Unpack mod helper
- BG3 localization scanning for the MO2 Content column
- persistent scan caching keyed by language and file fingerprints
- translation/base pairing metadata with Content-column tooltips
- linked-row highlighting in the MO2 mod list
- a full **Translation** tab: load mod strings, edit translations, AI-assisted translation via Google Gemini, and export as `.pak` or MO2 mod
- a **Nexus Downloads** tab: discover translation packs on Nexus Mods and queue downloads into MO2

## What it does

### Tools menu

LWizard registers two `IPluginTool` entries under the `LWizard/` submenu:

- `LWizard/Menu` opens the main dialog
- `LWizard/Unpack mod` extracts `.pak` archives from a selected mod with Divine (LSLib)

The main dialog has four tabs:

- **Settings** — target localization language, optional "cache only current language" switch, **Scan mods** button; language changes saved immediately via MO2 plugin settings
- **Translation** — full mod string editor and AI translation pipeline (see below)
- **Nexus Downloads** — manual Nexus translation discovery, optional API key, one-click MO2 download queueing
- **Logs** — live plugin log buffer

### Content column integration

After MO2 finishes initializing its UI, LWizard registers a BG3 `ModDataContent` feature and contributes these Content-column states:

1. Embedded translation
2. Installed / redundant
3. Available on Nexus
4. Outdated
5. Not available
6. Translation mod

The implemented paths today are:

- embedded localization found directly in the mod
- translation-pack detection by UUID overlap across localization data
- fallback name matching for some XML-only translation packs
- base-mod classification when a separate translation mod is installed for it
- Nexus-backed availability updates for mods that scan as unavailable locally but have matching translation pages on Nexus for the active language

The outdated state is still a placeholder for a later pipeline.

### Translation tab

The Translation tab provides a self-contained workflow for creating localization mods:

1. Pick any mod from the list (search supported).
2. Choose source language (default: English) and target language (default: from plugin settings).
3. Click **Load Strings** — localization strings are extracted in a background thread from unpacked files or `.pak` archives. `.pak` reads are done in-process via `bg3rustpaklib` (no Divine subprocesses for scanning).
4. A three-column table shows **UUID | Original | Translation**. The Original column renders BG3 markup (`<LSTag>`, `<br>`, `<b>`/`<i>`/`<s>`) as HTML. The Translation column renders HTML when idle and switches to plain-text editing on double-click.
5. **Original = Translated** pre-fills empty translation cells with the source text as a starting point.
6. Translations auto-save to `plugins/lwizard/translations/<modName>_<lang>.json` on every cell edit.
7. **Export .pak** packs the result into a ready-to-use `.pak` file.
8. **Export as Mod** creates a complete MO2 mod under `mods/<modName> - <lang>/` with proper `meta.lsx` and packs the localization.

#### AI translation (Google Gemini)

Paste a [Google AI Studio](https://aistudio.google.com/) API key once; it is persisted across sessions.

- **Translate Selected Rows** — sends selected strings to Gemini in batches of 12 with the full existing-translation context as a consistency glossary. Handles HTTP 429 rate limiting automatically (parses retry delay, backs off, retries up to 4 times).
- **Copy Prompt to Clipboard** — builds a self-contained prompt for the selected rows and copies it to the clipboard. Paste into any AI chat (ChatGPT, Claude, Gemini web, etc.).
- **Import from Clipboard** — parses the AI's reply from the clipboard (raw JSON, fenced code block, or any text containing the `{"lwizard_translations":{...}}` envelope) and applies translations directly to the table.

Four models are available: Gemini 3 Flash, Gemini 2.5 Flash, Gemini 2.5 Flash Lite, Gemini 3.1 Flash Lite (free-tier quotas vary by model).

### Nexus Downloads tab

The Nexus Downloads tab adds a second translation-discovery path on top of the local scan:

1. It lists all mods in the current profile that have a Nexus mod ID.
2. **Scan All** or **Scan Selected** scrapes the original mod page on Nexus for translation links matching the currently selected target language.
3. Without an API key, LWizard still discovers matching translation mod pages and offers **Open Page** actions.
4. With a personal Nexus Mods API key, LWizard also fetches file metadata, shows version/date/size/category, and enables **Download** plus **Download All Latest** actions through MO2's download manager.
5. When translations are found, LWizard updates the Content-column cache so mods previously marked **Not available** can move to **Available on Nexus**.

Automatic discovery also runs when MO2 notices a newly installed mod with a Nexus ID. Those searches are queued and processed one at a time so install-time scans do not pile up.

The API key is stored in `plugins/lwizard/nexus_config.json` under the MO2 base path.

### Translation pairing UI

For paired translation mods and their base mods, LWizard patches MO2's existing `modList` view at runtime through Qt only:

- the Content cell keeps MO2's normal icon rendering
- hovering the Content cell shows a custom tooltip such as `Translation for: <base mod>` or `Separate translation installed: <translation mod>`
- selecting one side of a pair highlights both rows
- translation mods are tinted pastel blue
- original/base mods are tinted pastel magenta

Highlighting is selection-driven and clears when the selection clears.

## Localization scan behavior

The scan runs on a background thread over valid mods only.

For each mod, LWizard can inspect:

- unpacked `Localization/<language>/...` folders
- unpacked localization under `PAK_FILES/...`
- `.pak` archives discovered in the mod root and under `PAK_FILES/`

When `.pak` inspection is needed for scanning, LWizard uses an in-process Rust-backed reader (`bg3rustpaklib`) to list and read entries directly.
`Divine.exe` (LSLib) is still used for packaging/export workflows and for the standalone **Unpack mod** helper.

The scanner also builds embedded string maps so it can compare localization UUID sets across languages and across mods.

## Cache behavior

LWizard persists two caches through MO2:

- a per-mod scan cache with content state, fingerprint, and translation-pair metadata
- a per-mod embedded-strings cache used for UUID comparison

Important details:

- cache entries are keyed by the selected language
- entries are reused only when the localization fingerprint still matches disk
- missing mods are pruned from cache
- if "cache only current language" is enabled, other languages are removed from persistent storage
- old cache entries without translation-pair metadata are ignored and refreshed by the next scan
- discovered Nexus translation mod IDs are cached with the content state for the active language

## Requirements

| Component | Notes |
|-----------|-------|
| MO2 | Current 2.5.x portable layout (or equivalent) |
| Compiler | MSVC x64, currently configured for Visual Studio 18 2026 with toolset v145 |
| Qt | 6.7.3 `msvc2022_64` |
| Rust toolchain | Required to build `bg3rustpaklib` via `cargo` during CMake build |
| vcpkg | `VCPKG_ROOT` configured |
| mo2-uibase | Build/install `modorganizer-uibase` so `mo2-uibase_DIR` resolves (see `CMakeLists.txt`) |
| Nexus Mods API key | Optional; enables file metadata and direct downloads in the Nexus Downloads tab |

## Build

From a Developer PowerShell:

```powershell
$env:QT_ROOT = "D:\Qt\6.7.3\msvc2022_64"
$env:VCPKG_ROOT = "C:\vcpkg"

cd lwizard
cmake --preset vs18-buildtools
cmake --build --preset vs18-buildtools
cmake --install vsbuild --config RelWithDebInfo
```

Notes:

- The build invokes `cargo rustc --release --features ffi --crate-type=staticlib` for `../bg3rustpaklib` automatically.
- `CMakeLists.txt` expects `mo2-uibase` at `../uibase_install/lib/cmake/mo2-uibase`.
- The default install prefix in `CMakePresets.json` points at `../Mod Organizer`, so install deploys plugin DLLs into that local MO2 portable tree.

## Repository layout

| Path | Purpose |
|------|---------|
| `src/` | Plugin source files |
| `resources/` | Qt resources and icons |
| `CMakeLists.txt` | Targets and install rules |
| `CMakePresets.json` | Local configure/build presets |
| `vcpkg.json` | CMake helper dependency |

## References

- [modorganizer-uibase](https://github.com/ModOrganizer2/modorganizer-uibase)
- [Writing Mod Organizer Plugins](https://github.com/ModOrganizer2/modorganizer/wiki/Writing-Mod-Organizer-Plugins)

## License

Add a `LICENSE` file before publishing the repository publicly.
