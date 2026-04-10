# LWizard

**LWizard** is a native C++ plugin for [Mod Organizer 2](https://github.com/ModOrganizer2/modorganizer) (**2.5.2**), aimed at **Baldur’s Gate 3** workflows. It extends MO2 with game-specific tooling (for example, localization-related **ModDataContent** integration and BG3-oriented helpers).

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

The **`vs18-buildtools`** preset sets `CMAKE_INSTALL_PREFIX` to **`../Mod Organizer`** by default. Change `CMakePresets.json` (or override at configure time) so `cmake --install` deploys `lwizard.dll` into your MO2 **`plugins/`** tree.

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
