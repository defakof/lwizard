# LWizard

**LWizard** is a native C++ plugin for [Mod Organizer 2](https://github.com/ModOrganizer2/modorganizer) (**2.5.2**), aimed at **Baldur’s Gate 3** workflows. It extends MO2 with game-specific tooling (for example, localization-related **ModDataContent** integration and BG3-oriented helpers).

## What this plugin does

These behaviors match the current source code (not a roadmap).

- **Tools menu (`IPluginTool`) — submenu**  
  MO2 groups tools whose `displayName()` contains `/` (e.g. **LWizard/Menu**, **LWizard/Unpack mod**). **Menu** opens the main dialog: a **Settings** tab (target localization language, **Save**, **Scan mods**) and a **Logs** tab that streams the plugin’s own log buffer (with **Clear**). **Unpack mod** lets you pick a mod and run **Divine** `extract-package` on every `.pak` under that mod, extracting beside each archive (destination = the folder containing the `.pak`, with `--use-package-name`).

- **Content column (`ModDataContent`)**  
  After startup, once the UI is ready, the plugin registers a **ModDataContent** feature for Baldur’s Gate 3 (via the managed game, or the game id `baldursgate3`). It defines five content types with distinct icons: embedded translation, separate installed translation, available on Nexus, installed but outdated, and none found. **Only the “embedded in mod data” path is implemented today:** detection looks for `Localization/<language>/` on disk or inside `.pak` files. **Separate-mod / Nexus / outdated states are not implemented yet** — the scanner does not assign those categories, so in practice you will see either the embedded icon or the “not available” icon after a scan.

- **Localization scan**  
  **Scan mods** runs on a **background thread** over **valid** mods only. For each mod it checks unpacked folders and `.pak` files. Reading `.pak` contents uses **Norbyte’s LSLib** command-line tool **`Divine.exe`** (`list-package` for BG3). If `Divine.exe` is missing, the plugin tries to **download** the official LSLib release ZIP (pinned version in code) with **PowerShell**, extract the `Tools` binaries into **`plugins/lwizard/`**, and use that copy. It also picks up `Divine.exe` from the unofficial BG3 MO2 plugin layout (`basic_games/.../baldursgate3/tools/`) or any path under **`plugins/`** if already present.

- **Language setting**  
  The plugin exposes an MO2 setting **“Localization language to scan for in BG3 mods”** (and the dialog mirrors it). The same value drives scanning and cache keys.

- **Cache and refresh**  
  Per-mod results are **cached** after a scan. The cache is **cleared** when MO2 performs a refresh cycle and when the language setting changes, so the Content column does not show LWizard icons until you run **Scan mods** again. When a scan finishes, the plugin triggers a **soft MO2 refresh** so the Content column can re-query the new cache.

## Requirements

| Component | Notes |
|-----------|--------|
| **MO2** | 2.5.2 portable (or equivalent); `uibase.dll` at runtime |
| **Compiler** | **MSVC** x64 — this tree targets **Visual Studio 18 2026** with toolset **v145** (see `CMakePresets.json`) |
| **Qt** | **6.7.3** `msvc2022_64` — set **`QT_ROOT`** to the kit root (e.g. `D:\Qt\6.7.3\msvc2022_64`) |
| **vcpkg** | **`VCPKG_ROOT`** set; on **PATH** |
| **mo2-uibase** | Headers + `uibase.lib` from building [modorganizer-uibase](https://github.com/ModOrganizer2/modorganizer-uibase) at tag **`v2.5.2`** (must match MO2’s `uibase.dll`) |

Install the built uibase CMake package so this project can resolve it. By default, **`CMakeLists.txt`** and the **`vs18-buildtools`** preset expect:

- **`../uibase_install/lib/cmake/mo2-uibase`** (relative to this folder), and  
- **`CMAKE_PREFIX_PATH`** including **`../uibase_install/lib`** and **`$env:QT_ROOT`**.

Adjust `mo2-uibase_DIR` or `CMAKE_PREFIX_PATH` if your layout differs.

## Build

From a **Developer PowerShell** (or environment where MSVC and CMake are available):

```powershell
$env:QT_ROOT = "D:\Qt\6.7.3\msvc2022_64"   # your Qt 6.7.3 kit
$env:VCPKG_ROOT = "C:\vcpkg"               # your vcpkg root

cd lwizard
cmake --preset vs18-buildtools
cmake --build vsbuild --preset vs18-buildtools
cmake --install vsbuild --config RelWithDebInfo
```

The **`vs18-buildtools`** preset sets `CMAKE_INSTALL_PREFIX` to **`../Mod Organizer`** by default. Change `CMakePresets.json` (or override at configure time) so `cmake --install` deploys **`lwizard.dll`** and **`lwizard_unpack.dll`** into your MO2 **`plugins/`** tree.

## Repository layout

| Path | Purpose |
|------|---------|
| `src/` | C++ sources and headers |
| `resources/` | Qt resources (e.g. `lwizard.qrc`) |
| `CMakeLists.txt` | Plugin target, `mo2_configure_plugin` / `mo2_install_plugin` |
| `CMakePresets.json` | `vs18-buildtools` + vcpkg toolchain |
| `vcpkg.json` | **`mo2-cmake`** (MO2 CMake helpers) |

Build trees **`vsbuild/`** and **`build/`** are gitignored.

## References

- MO2 plugin API (**uibase**): [modorganizer-uibase](https://github.com/ModOrganizer2/modorganizer-uibase)  
- MO2 wiki — [Writing Mod Organizer Plugins](https://github.com/ModOrganizer2/modorganizer/wiki/Writing-Mod-Organizer-Plugins)  
- Full toolchain and workspace notes (sibling docs in the parent project): **`project_context.md`** and **`setup_checklist.md`**

## License

Add a `LICENSE` file if you publish this repository publicly.
