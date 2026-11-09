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

Requires Visual Studio 2022 (Desktop development with C++) and CMake 3.24+.
All dependencies are downloaded by CMake `FetchContent` during configure.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\md.exe README.md
```

Or with Ninja from a *Developer Command Prompt*:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

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
