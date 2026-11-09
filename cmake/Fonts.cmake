# Fonts embedded into md.exe. Everything is pinned by hash.
include(FetchContent)

# Roboto (classic v3) static instances, hinted build (best on Windows).
FetchContent_Declare(roboto
    URL      https://github.com/googlefonts/roboto-3-classic/releases/download/v3.016/Roboto_v3.016.zip
    URL_HASH SHA256=1653dbe12f248da8fb0b9920db7b9496cd677ed3981154f6f15285c8bd4e334f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR do-not-add)

# Roboto Mono (variable, default instance = Regular 400) from google/fonts.
set(_gf https://raw.githubusercontent.com/google/fonts/5bb28733cec313b63fbd415b574b5ae35c8aa8b8/ofl)
FetchContent_Declare(roboto_mono
    URL      ${_gf}/robotomono/RobotoMono%5Bwght%5D.ttf
    URL_HASH SHA256=66a80e79d17e4c7cabd162e2916578a4cc08fd19eef6e2a643305eae9c567b2b
    DOWNLOAD_NAME RobotoMono.ttf
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_Declare(roboto_mono_italic
    URL      ${_gf}/robotomono/RobotoMono-Italic%5Bwght%5D.ttf
    URL_HASH SHA256=49ac343bb7b070071e53f0a8a501d68d140ed98ebd0a43b6b8fb96cf22f09ff7
    DOWNLOAD_NAME RobotoMono-Italic.ttf
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR do-not-add)

# Noto Color Emoji, Google Fonts build: scalable OT-SVG colour glyphs, which
# SDL_ttf renders through plutosvg at any size.
FetchContent_Declare(noto_emoji
    URL      ${_gf}/notocoloremoji/NotoColorEmoji-Regular.ttf
    URL_HASH SHA256=4d82a18d8d95f60ba883ce242bbadbf84a576e987745dd7ba38d71c67cff2d73
    DOWNLOAD_NAME NotoColorEmoji.ttf
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR do-not-add)

FetchContent_MakeAvailable(roboto roboto_mono roboto_mono_italic noto_emoji)

set(_r "${roboto_SOURCE_DIR}/hinted/static")
set(MD_FONT_ROBOTO_REGULAR     "${_r}/Roboto-Regular.ttf")
set(MD_FONT_ROBOTO_ITALIC      "${_r}/Roboto-Italic.ttf")
set(MD_FONT_ROBOTO_MEDIUM      "${_r}/Roboto-Medium.ttf")
set(MD_FONT_ROBOTO_BOLD        "${_r}/Roboto-Bold.ttf")
set(MD_FONT_ROBOTO_BOLD_ITALIC "${_r}/Roboto-BoldItalic.ttf")
set(MD_FONT_ROBOTO_LIGHT       "${_r}/Roboto-Light.ttf")
set(MD_FONT_ROBOTO_MONO        "${roboto_mono_SOURCE_DIR}/RobotoMono.ttf")
set(MD_FONT_ROBOTO_MONO_ITALIC "${roboto_mono_italic_SOURCE_DIR}/RobotoMono-Italic.ttf")
set(MD_FONT_NOTO_EMOJI         "${noto_emoji_SOURCE_DIR}/NotoColorEmoji.ttf")

foreach(_f REGULAR ITALIC MEDIUM BOLD BOLD_ITALIC LIGHT MONO MONO_ITALIC)
    if(NOT EXISTS "${MD_FONT_ROBOTO_${_f}}")
        message(FATAL_ERROR "Missing font: ${MD_FONT_ROBOTO_${_f}}")
    endif()
endforeach()
if(NOT EXISTS "${MD_FONT_NOTO_EMOJI}")
    message(FATAL_ERROR "Missing font: ${MD_FONT_NOTO_EMOJI}")
endif()
