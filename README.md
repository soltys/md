# md

A Markdown viewer/editor for Windows built for the fastest possible cold start.

* C11, compiled with MSVC only, built with CMake
* SDL3 + SDL3_ttf (FreeType, HarfBuzz, plutosvg) + SDL3_image for rendering
* tree-sitter + tree-sitter-markdown for parsing
* Roboto (Regular, Italic, Medium, Bold, Bold Italic, Light, Mono, Mono Italic)
  and Noto Color Emoji embedded in the executable
* Ships as a single `md.exe`: every dependency and the C runtime are linked
  statically, fonts are Win32 resources read in place from the mapped image

## Building

Requires **CMake 4.0 or newer** and **Visual Studio 2026** (workload
"Desktop development with C++"). All dependencies are downloaded by CMake
`FetchContent` during configure, so the first configure takes a while.

Everything is driven by `CMakePresets.json`. The quickest way is the default
workflow, which configures with the `Visual Studio 18 2026` generator (x64)
and builds Release:

```bat
cmake --workflow --preset default
build\vs2026\Release\md.exe README.md
```

Or step by step:

```bat
cmake --preset vs2026
cmake --build --preset release      :: or: --preset debug
```

`build\vs2026\md.sln` can also be opened in Visual Studio, or open the folder
directly and pick a preset there.

For Ninja builds, run from a *Developer Command Prompt for VS 2026*:

```bat
cmake --preset ninja-release
cmake --build --preset ninja-release
build\ninja-release\md.exe README.md
```

| Configure preset | Generator | Output |
| --- | --- | --- |
| `vs2026` (default) | Visual Studio 18 2026, x64 | `build/vs2026/<Config>/md.exe` |
| `ninja-release` | Ninja, MSVC `cl` | `build/ninja-release/md.exe` |
| `ninja-debug` | Ninja, MSVC `cl` | `build/ninja-debug/md.exe` |

## Usage

```
md.exe file.md
```

You can also drop a file on the window or press **Ctrl+O**.

| Key | Action |
| --- | --- |
| Ctrl+O | Open a file |
| Ctrl+Q / Ctrl+W | Quit |
| Up / Down, PgUp / PgDn, Space, Home / End, mouse wheel | Scroll |

## Neovim config

`nvim/init.lua` is the editor config for the embedded Neovim pane (Neovim
0.12+). It sets a few prose-friendly options and installs the
[catppuccin](https://github.com/catppuccin/nvim) colorscheme through the
built-in `vim.pack`, using the Mocha flavour with brighter line numbers. To try it on
its own without touching your personal config:

```bat
set NVIM_APPNAME=md-nvim
nvim -u nvim\init.lua README.md
```

## How startup is kept fast

1. Before `SDL_Init`, a font thread starts initialising FreeType/HarfBuzz and
   opening the embedded fonts, and a loader thread reads and parses the file.
2. The main thread creates the window and renderer, presents one frame in the
   background colour and only then shows the window, so there is no flash.
3. The main thread waits for the fonts only when it is about to draw text.
4. Fonts are never copied: FreeType reads them straight from the executable's
   resource section, and only the pages it touches get loaded.
5. The window is DPI-aware through the manifest, so it opens at the right size.

## Roadmap

1. ✅ Skeleton, build, embedded fonts, fast first frame
2. Full Markdown viewer: inline formatting, links, images, tables, emoji sequences
3. Embedded Neovim editor (`nvim --embed --clean`) with horizontal/vertical
   split (Ctrl+Alt+O) and split on/off (Ctrl+Alt+S)
4. Syntax highlighting in code blocks, scroll sync, startup polish
