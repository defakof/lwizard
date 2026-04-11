# LWizard

LWizard is a native C++ plugin for [Mod Organizer 2](https://github.com/ModOrganizer2/modorganizer) 2.5.2, focused on Baldur's Gate 3 workflows.

It currently provides:

- a Tools submenu with the main LWizard dialog and an Unpack mod helper
- BG3 localization scanning for the MO2 Content column
- persistent scan caching keyed by language and file fingerprints
- translation/base pairing metadata with Content-column tooltips
- linked-row highlighting in the MO2 mod list

## What it does

### Tools menu

LWizard registers two `IPluginTool` entries under the `LWizard/` submenu:

- `LWizard/Menu` opens the main dialog
- `LWizard/Unpack mod` extracts `.pak` archives from a selected mod with Divine

The main dialog currently has:

- a **Settings** tab with the target localization language, an optional "cache only current language" switch, and a **Scan mods** button
- a **Logs** tab showing the plugin log buffer

Language changes are saved immediately through MO2 plugin settings.

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

The Nexus availability and outdated states are still placeholders for a later pipeline.

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

When `.pak` inspection is needed, LWizard uses Norbyte's `Divine.exe` (`list-package`, `extract-single-file`, `convert-loca`).

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

## Requirements

| Component | Notes |
|-----------|-------|
| MO2 | 2.5.2 portable (or equivalent) |
| Compiler | MSVC x64, currently configured for Visual Studio 18 2026 with toolset v145 |
| Qt | 6.7.3 `msvc2022_64` |
| vcpkg | `VCPKG_ROOT` configured |
| mo2-uibase | Built from `modorganizer-uibase` tag `v2.5.2` to match MO2's `uibase.dll` |

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

The default install prefix in `CMakePresets.json` points at `../Mod Organizer`, so install will deploy plugin DLLs into the local MO2 portable tree.

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
