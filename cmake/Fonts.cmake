# Fonts embedded into md.exe. Everything is pinned by hash.
include(FetchContent)

# Roboto (classic v3) static instances, hinted build (best on Windows).
FetchContent_Declare(roboto
    URL      https://github.com/googlefonts/roboto-3-classic/releases/download/v3.016/Roboto_v3.016.zip
    URL_HASH SHA256=1653dbe12f248da8fb0b9920db7b9496cd677ed3981154f6f15285c8bd4e334f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR do-not-add)

# Roboto Mono Nerd Font (nerd-fonts v3.4.0), "Mono" variant: the icons are
# patched in at one cell wide, which is what Neovim's grid expects. Used for
# the editor pane and for code in the preview.
set(_rm https://raw.githubusercontent.com/ryanoasis/nerd-fonts/v3.4.0/patched-fonts/RobotoMono)
set(_rm_dir_Regular    Regular)
set(_rm_dir_Italic     Italic)
set(_rm_dir_Bold       Bold)
set(_rm_dir_BoldItalic Bold-Italic)
set(_rm_hash_Regular    b34b5b95d78fb5788d52af55339adb2e2ca91279c2043c60d0fb1208b88c7533)
set(_rm_hash_Italic     accf83db01798984f0995fd79d6f4c0687b0dcc065d3520ee5d1c7a8997cb9b6)
set(_rm_hash_Bold       6ee21e0e6fae4efa1cf6294fa1e53a021d6a21c98bab4c2022790d0156e83082)
set(_rm_hash_BoldItalic ac090631d31b05ac8f8862666b8ebbdf3f775b1e1c3785b1e9c4200730d0b8b9)
foreach(_s Regular Italic Bold BoldItalic)
    string(TOLOWER "roboto_mono_nf_${_s}" _name)
    FetchContent_Declare(${_name}
        URL      ${_rm}/${_rm_dir_${_s}}/RobotoMonoNerdFontMono-${_s}.ttf
        URL_HASH SHA256=${_rm_hash_${_s}}
        DOWNLOAD_NO_EXTRACT TRUE
        SOURCE_SUBDIR do-not-add)
    FetchContent_MakeAvailable(${_name})
endforeach()

set(_gf https://raw.githubusercontent.com/google/fonts/5bb28733cec313b63fbd415b574b5ae35c8aa8b8/ofl)

# Noto Color Emoji, Google Fonts build: scalable OT-SVG colour glyphs, which
# SDL_ttf renders through plutosvg at any size.
FetchContent_Declare(noto_emoji
    URL      ${_gf}/notocoloremoji/NotoColorEmoji-Regular.ttf
    URL_HASH SHA256=4d82a18d8d95f60ba883ce242bbadbf84a576e987745dd7ba38d71c67cff2d73
    DOWNLOAD_NAME NotoColorEmoji.ttf
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR do-not-add)

# Symbol fallbacks for what Roboto lacks: arrows (→ ⇒ ⟶), math, check marks
# (✓ ✗), shapes, keyboard symbols (⌘ ⏎). Without them those show as boxes.
FetchContent_Declare(noto_math
    URL      ${_gf}/notosansmath/NotoSansMath-Regular.ttf
    URL_HASH SHA256=3f495fe933c06786e4d5f6d86b8ee70b6753a68ee3b9d87528726de0f6e2c47d
    DOWNLOAD_NAME NotoSansMath-Regular.ttf
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_Declare(noto_symbols2
    URL      ${_gf}/notosanssymbols2/NotoSansSymbols2-Regular.ttf
    URL_HASH SHA256=7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21
    DOWNLOAD_NAME NotoSansSymbols2-Regular.ttf
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR do-not-add)

# Roboto Serif (Commercial Type for Google, OFL): Roboto's proportions and
# shapes with serifs, the optional body font (F7, --serif, the "Aa" button).
# Static instances at the 14pt optical size, the one drawn for body text on
# screen; only the five faces a page uses are embedded (~1.4 MB of TTF,
# a fraction of that after zstd) and they are unpacked only when serif text
# is first shown.
FetchContent_Declare(roboto_serif
    URL      https://github.com/googlefonts/roboto-serif/releases/download/v1.008/RobotoSerifFonts-v1.008.zip
    URL_HASH SHA256=366f437312f40a0039d6719e6493d64828a982ac2734f9618f4c07c8684b6984
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR do-not-add)

FetchContent_MakeAvailable(roboto roboto_serif noto_emoji noto_math noto_symbols2)

set(_r "${roboto_SOURCE_DIR}/hinted/static")
set(MD_FONT_ROBOTO_REGULAR     "${_r}/Roboto-Regular.ttf")
set(MD_FONT_ROBOTO_ITALIC      "${_r}/Roboto-Italic.ttf")
set(MD_FONT_ROBOTO_MEDIUM      "${_r}/Roboto-Medium.ttf")
set(MD_FONT_ROBOTO_BOLD        "${_r}/Roboto-Bold.ttf")
set(MD_FONT_ROBOTO_BOLD_ITALIC "${_r}/Roboto-BoldItalic.ttf")
set(MD_FONT_ROBOTO_LIGHT       "${_r}/Roboto-Light.ttf")
set(MD_FONT_ROBOTO_MONO        "${roboto_mono_nf_regular_SOURCE_DIR}/RobotoMonoNerdFontMono-Regular.ttf")
set(MD_FONT_ROBOTO_MONO_ITALIC "${roboto_mono_nf_italic_SOURCE_DIR}/RobotoMonoNerdFontMono-Italic.ttf")
set(MD_FONT_ROBOTO_MONO_BOLD   "${roboto_mono_nf_bold_SOURCE_DIR}/RobotoMonoNerdFontMono-Bold.ttf")
set(MD_FONT_ROBOTO_MONO_BOLD_ITALIC "${roboto_mono_nf_bolditalic_SOURCE_DIR}/RobotoMonoNerdFontMono-BoldItalic.ttf")
set(_rs "${roboto_serif_SOURCE_DIR}/ttf")
set(MD_FONT_SERIF_REGULAR     "${_rs}/RobotoSerif14pt-Regular.ttf")
set(MD_FONT_SERIF_ITALIC      "${_rs}/RobotoSerif14pt-Italic.ttf")
set(MD_FONT_SERIF_MEDIUM      "${_rs}/RobotoSerif14pt-Medium.ttf")
set(MD_FONT_SERIF_BOLD        "${_rs}/RobotoSerif14pt-Bold.ttf")
set(MD_FONT_SERIF_BOLD_ITALIC "${_rs}/RobotoSerif14pt-BoldItalic.ttf")
set(MD_FONT_NOTO_EMOJI         "${noto_emoji_SOURCE_DIR}/NotoColorEmoji.ttf")
set(MD_FONT_NOTO_MATH          "${noto_math_SOURCE_DIR}/NotoSansMath-Regular.ttf")
set(MD_FONT_NOTO_SYMBOLS2      "${noto_symbols2_SOURCE_DIR}/NotoSansSymbols2-Regular.ttf")

foreach(_f REGULAR ITALIC MEDIUM BOLD BOLD_ITALIC LIGHT MONO MONO_ITALIC MONO_BOLD MONO_BOLD_ITALIC)
    if(NOT EXISTS "${MD_FONT_ROBOTO_${_f}}")
        message(FATAL_ERROR "Missing font: ${MD_FONT_ROBOTO_${_f}}")
    endif()
endforeach()
foreach(_f EMOJI MATH SYMBOLS2)
    if(NOT EXISTS "${MD_FONT_NOTO_${_f}}")
        message(FATAL_ERROR "Missing font: ${MD_FONT_NOTO_${_f}}")
    endif()
endforeach()
foreach(_f REGULAR ITALIC MEDIUM BOLD BOLD_ITALIC)
    if(NOT EXISTS "${MD_FONT_SERIF_${_f}}")
        message(FATAL_ERROR "Missing font: ${MD_FONT_SERIF_${_f}}")
    endif()
endforeach()

# Store the fonts zstd-compressed (38 MB of TTF -> about 13 MB). A smaller
# md.exe is quicker for Windows to map and, above all, for antivirus to scan:
# Defender reads the whole file on the first run of every new build, and it
# looks inside embedded fonts. CMake compresses them itself, once, at
# configure time; src/assets.c unpacks each font on first use, on a
# background thread where possible (the emoji font only for documents that
# contain emoji). The original sizes go into generated/asset_sizes.h, since
# CMake's zstd frames do not record them.
option(MD_COMPRESS_FONTS "Embed the fonts zstd-compressed (smaller md.exe)" ON)
set(_md_pack_dir "${CMAKE_CURRENT_BINARY_DIR}/packed-fonts")
set(_md_raw_sizes "")
foreach(_f ROBOTO_REGULAR ROBOTO_ITALIC ROBOTO_MEDIUM ROBOTO_BOLD ROBOTO_BOLD_ITALIC
           ROBOTO_LIGHT ROBOTO_MONO ROBOTO_MONO_ITALIC ROBOTO_MONO_BOLD
           ROBOTO_MONO_BOLD_ITALIC NOTO_EMOJI NOTO_MATH NOTO_SYMBOLS2
           SERIF_REGULAR SERIF_ITALIC SERIF_MEDIUM SERIF_BOLD SERIF_BOLD_ITALIC)
    if(NOT MD_COMPRESS_FONTS)
        break()
    endif()
    set(_src "${MD_FONT_${_f}}")
    get_filename_component(_name "${_src}" NAME)
    set(_dst "${_md_pack_dir}/${_name}.zst")
    if(NOT EXISTS "${_dst}" OR "${_src}" IS_NEWER_THAN "${_dst}")
        file(MAKE_DIRECTORY "${_md_pack_dir}")
        message(STATUS "Compressing ${_name} (zstd level 19, once)")
        file(ARCHIVE_CREATE OUTPUT "${_dst}" PATHS "${_src}"
             FORMAT raw COMPRESSION Zstd COMPRESSION_LEVEL 19)
    endif()
    file(SIZE "${_src}" _size)
    string(APPEND _md_raw_sizes "    { ASSET_${_f}, ${_size}u },\n")
    set(MD_FONT_${_f} "${_dst}")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/asset_sizes.h" CONTENT
"/* Generated by cmake/Fonts.cmake: unpacked size of each compressed asset. */
#include \"asset_ids.h\"
static const struct { int id; unsigned size; } asset_raw_sizes[] = {
@_md_raw_sizes@    { 0, 0u }
};
" @ONLY)
