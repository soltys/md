# Third-party code, all fetched at configure time and linked statically.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ---------------------------------------------------------------- SDL3 ----
set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
# Subsystems a document viewer never touches: less code to load and relocate.
set(SDL_AUDIO         OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA        OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK      OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC        OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI        OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR        OFF CACHE BOOL "" FORCE)
set(SDL_POWER         OFF CACHE BOOL "" FORCE)
set(SDL_TRAY          OFF CACHE BOOL "" FORCE)
set(SDL_GPU           OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN        OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_VULKAN OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.20
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_SUBMODULES ""
    SYSTEM)

# ------------------------------------------------------------ SDL3_ttf ----
# Vendored FreeType + HarfBuzz (shaping, emoji sequences) + plutosvg (the
# OT-SVG colour glyphs in Noto Color Emoji). Upstream keeps those as git
# submodules of SDL_ttf; FetchContent pulls them in with the checkout.
set(SDLTTF_VENDORED  ON  CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ  ON  CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG  ON  CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES   OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        release-3.2.2
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_SUBMODULES external/freetype external/harfbuzz external/plutosvg external/plutovg
    SYSTEM)

# ---------------------------------------------------------- SDL3_image ----
# Only formats with built-in decoders (stb_image, nanosvg, ...), so nothing
# else has to be fetched or linked.
set(SDLIMAGE_VENDORED     OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BACKEND_STB  ON  CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_LIBPNG   OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF         OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL          OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF          OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP         OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES      OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS        OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL      OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG        release-3.4.8
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_SUBMODULES ""   # vendored codecs not needed (built-in decoders only)
    SYSTEM)

FetchContent_MakeAvailable(SDL3 SDL3_ttf SDL3_image)

# --------------------------------------------------------- tree-sitter ----
# SOURCE_SUBDIR points at a directory without a CMakeLists.txt, so these are
# only downloaded; we compile the few C files we need ourselves.
FetchContent_Declare(tree_sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG        v0.27.2
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES ""
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  do-not-add)

FetchContent_Declare(tree_sitter_markdown
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-markdown.git
    GIT_TAG        v0.5.4
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES ""
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  do-not-add)

FetchContent_MakeAvailable(tree_sitter tree_sitter_markdown)

add_library(md_tree_sitter STATIC "${tree_sitter_SOURCE_DIR}/lib/src/lib.c")
target_include_directories(md_tree_sitter
    PUBLIC  "${tree_sitter_SOURCE_DIR}/lib/include"
    PRIVATE "${tree_sitter_SOURCE_DIR}/lib/src")

set(_tsmd "${tree_sitter_markdown_SOURCE_DIR}")
add_library(md_ts_markdown STATIC
    "${_tsmd}/tree-sitter-markdown/src/parser.c"
    "${_tsmd}/tree-sitter-markdown/src/scanner.c")
target_include_directories(md_ts_markdown PRIVATE "${_tsmd}/tree-sitter-markdown/src")

add_library(md_ts_markdown_inline STATIC
    "${_tsmd}/tree-sitter-markdown-inline/src/parser.c"
    "${_tsmd}/tree-sitter-markdown-inline/src/scanner.c")
target_include_directories(md_ts_markdown_inline PRIVATE "${_tsmd}/tree-sitter-markdown-inline/src")

foreach(_t md_tree_sitter md_ts_markdown md_ts_markdown_inline)
    if(MSVC)
        target_compile_options(${_t} PRIVATE /W0 /utf-8)
        target_compile_definitions(${_t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${_t} PRIVATE -w)
    endif()
endforeach()
