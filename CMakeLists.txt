cmake_minimum_required(VERSION 4.0)

# Static CRT everywhere (our code and every fetched dependency) so the result
# is a single self-contained md.exe. Must be set before project().
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(md VERSION 0.1.0 LANGUAGES C CXX) # CXX only for HarfBuzz; app code is C

if(NOT MSVC)
    message(WARNING "md targets the MSVC toolchain. Other compilers are only "
                    "supported for quick local sanity builds.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Whole-program optimisation for Release builds (applies to dependencies too).
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(Fonts)
include(Grammars)

add_executable(md WIN32
    src/main.c
    src/app.h
    src/assets.c
    src/assets.h
    src/asset_ids.h
    src/fonts.c
    src/fonts.h
    src/document.c
    src/document.h
    src/view.c
    src/view.h
    src/theme.h
    src/nvim.c
    src/nvim.h
    src/msgpack.c
    src/msgpack.h
    src/editor.c
    src/editor.h
    src/emoji.c
    src/emoji.h
    src/images.c
    src/images.h
    src/highlight.c
    src/highlight.h
    src/regex.c
    src/regex.h
    ${MD_QUERIES_SOURCE}
)

# Neovim config for the editor pane, embedded like the fonts.
set(MD_NVIM_INIT "${CMAKE_CURRENT_SOURCE_DIR}/nvim/init.lua")

target_include_directories(md PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(md PRIVATE
    SDL3::SDL3-static
    SDL3_ttf::SDL3_ttf-static
    SDL3_image::SDL3_image-static
    md_tree_sitter
    md_ts_markdown
    md_ts_markdown_inline
    ${MD_GRAMMAR_LIBS}
)

if(WIN32)
    # Fonts are compiled into the executable as RCDATA resources. At runtime
    # they are reached through LockResource(), i.e. straight out of the mapped
    # image with no copy and no file I/O.
    configure_file(src/assets.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/assets.rc" @ONLY)
    # rc.exe does not track RCDATA inputs; rebuild when any of them changes.
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/assets.rc" PROPERTIES
        OBJECT_DEPENDS "${MD_FONT_ROBOTO_REGULAR};${MD_FONT_ROBOTO_ITALIC};${MD_FONT_ROBOTO_MEDIUM};${MD_FONT_ROBOTO_BOLD};${MD_FONT_ROBOTO_BOLD_ITALIC};${MD_FONT_ROBOTO_LIGHT};${MD_FONT_ROBOTO_MONO};${MD_FONT_ROBOTO_MONO_ITALIC};${MD_FONT_NOTO_EMOJI};${MD_NVIM_INIT};${MD_FONT_ROBOTO_MONO_BOLD};${MD_FONT_ROBOTO_MONO_BOLD_ITALIC}")
    target_sources(md PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/assets.rc"
        src/md.manifest)
else()
    # Dev-only fallback: load the fonts from the download directory.
    target_compile_definitions(md PRIVATE
        MD_ASSET_ROBOTO_REGULAR="${MD_FONT_ROBOTO_REGULAR}"
        MD_ASSET_ROBOTO_ITALIC="${MD_FONT_ROBOTO_ITALIC}"
        MD_ASSET_ROBOTO_MEDIUM="${MD_FONT_ROBOTO_MEDIUM}"
        MD_ASSET_ROBOTO_BOLD="${MD_FONT_ROBOTO_BOLD}"
        MD_ASSET_ROBOTO_BOLD_ITALIC="${MD_FONT_ROBOTO_BOLD_ITALIC}"
        MD_ASSET_ROBOTO_LIGHT="${MD_FONT_ROBOTO_LIGHT}"
        MD_ASSET_ROBOTO_MONO="${MD_FONT_ROBOTO_MONO}"
        MD_ASSET_ROBOTO_MONO_ITALIC="${MD_FONT_ROBOTO_MONO_ITALIC}"
        MD_ASSET_NOTO_EMOJI="${MD_FONT_NOTO_EMOJI}"
        MD_ASSET_NVIM_INIT="${MD_NVIM_INIT}"
        MD_ASSET_ROBOTO_MONO_BOLD="${MD_FONT_ROBOTO_MONO_BOLD}"
        MD_ASSET_ROBOTO_MONO_BOLD_ITALIC="${MD_FONT_ROBOTO_MONO_BOLD_ITALIC}")
endif()

if(MSVC)
    target_compile_options(md PRIVATE /W4 /utf-8 /permissive- /Gy /Gw /Oi
        $<$<CONFIG:Release,MinSizeRel>:/GS->)
    target_compile_definitions(md PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_options(md PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(md PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
