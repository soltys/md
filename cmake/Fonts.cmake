# Fonts embedded into md.exe. Everything is pinned by hash.
include(FetchContent)

# Roboto (classic v3) static instances, hinted build (best on Windows).
FetchContent_Declare(roboto
    URL      https://github.com/googlefonts/roboto-3-classic/releases/download/v3.016/Roboto_v3.016.zip
    URL_HASH SHA256=1653dbe12f248da8fb0b9920db7b9496cd677ed3981154f6f15285c8bd4e334f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR do-not-add)

# Roboto Mono static instances (real Bold, so bold text keeps the grid).
set(_rm https://raw.githubusercontent.com/googlefonts/RobotoMono/7bf0b33178844cc7d0c9915b746ece1be7f46bea/fonts/ttf)
set(_rm_hash_Regular    af0bff7599c3df3831755c16e39b3c496df74b8c8d8a1161b14dc8461be17cb4)
set(_rm_hash_Italic     4549325cd2d10938d37d63eba2aaca7c2e16e48322dc767576eab45e512b6ad2)
set(_rm_hash_Bold       3ecf35e5e87accc7578b605d1f5f0bc30d88b195d6807bec8a0c57f6aa95c4db)
set(_rm_hash_BoldItalic a0f16567447311eaf42a35f6c50eb64b911694b42f1b01038e3b7e92c20f131d)
foreach(_s Regular Italic Bold BoldItalic)
    string(TOLOWER "roboto_mono_${_s}" _name)
    FetchContent_Declare(${_name}
        URL      ${_rm}/RobotoMono-${_s}.ttf
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

FetchContent_MakeAvailable(roboto noto_emoji)

set(_r "${roboto_SOURCE_DIR}/hinted/static")
set(MD_FONT_ROBOTO_REGULAR     "${_r}/Roboto-Regular.ttf")
set(MD_FONT_ROBOTO_ITALIC      "${_r}/Roboto-Italic.ttf")
set(MD_FONT_ROBOTO_MEDIUM      "${_r}/Roboto-Medium.ttf")
set(MD_FONT_ROBOTO_BOLD        "${_r}/Roboto-Bold.ttf")
set(MD_FONT_ROBOTO_BOLD_ITALIC "${_r}/Roboto-BoldItalic.ttf")
set(MD_FONT_ROBOTO_LIGHT       "${_r}/Roboto-Light.ttf")
set(MD_FONT_ROBOTO_MONO        "${roboto_mono_regular_SOURCE_DIR}/RobotoMono-Regular.ttf")
set(MD_FONT_ROBOTO_MONO_ITALIC "${roboto_mono_italic_SOURCE_DIR}/RobotoMono-Italic.ttf")
set(MD_FONT_ROBOTO_MONO_BOLD   "${roboto_mono_bold_SOURCE_DIR}/RobotoMono-Bold.ttf")
set(MD_FONT_ROBOTO_MONO_BOLD_ITALIC "${roboto_mono_bolditalic_SOURCE_DIR}/RobotoMono-BoldItalic.ttf")
set(MD_FONT_NOTO_EMOJI         "${noto_emoji_SOURCE_DIR}/NotoColorEmoji.ttf")

foreach(_f REGULAR ITALIC MEDIUM BOLD BOLD_ITALIC LIGHT MONO MONO_ITALIC MONO_BOLD MONO_BOLD_ITALIC)
    if(NOT EXISTS "${MD_FONT_ROBOTO_${_f}}")
        message(FATAL_ERROR "Missing font: ${MD_FONT_ROBOTO_${_f}}")
    endif()
endforeach()
if(NOT EXISTS "${MD_FONT_NOTO_EMOJI}")
    message(FATAL_ERROR "Missing font: ${MD_FONT_NOTO_EMOJI}")
endif()
