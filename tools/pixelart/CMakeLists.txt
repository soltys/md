# pixelart: text sprite sheets (PICO-8 palette) -> PNG assets.
# A console tool; md.exe does not depend on it.
add_executable(pixelart pixelart.c)
target_link_libraries(pixelart PRIVATE
    SDL3::SDL3-static
    SDL3_image::SDL3_image-static)

if(MSVC)
    target_compile_options(pixelart PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(pixelart PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_options(pixelart PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(pixelart PRIVATE -Wall -Wextra -Wconversion -Wno-sign-conversion)
endif()

# `cmake --build --preset release --target art` regenerates art/png from the
# sheets in art/. The PNGs are committed, so a normal build never runs this.
file(GLOB MD_ART_SHEETS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/art/*.txt")
add_custom_target(art
    COMMAND pixelart -o "${PROJECT_SOURCE_DIR}/art/png" ${MD_ART_SHEETS}
    DEPENDS pixelart ${MD_ART_SHEETS}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Generating PNG assets from art/*.txt"
    VERBATIM)
