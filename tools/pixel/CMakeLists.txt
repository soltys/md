# pixel: PICO-8 palette pixel art editor for md's icons and artwork.
# Builds into its own single pixel.exe; md.exe does not depend on it.
add_executable(pixel WIN32 pixel.c)
target_link_libraries(pixel PRIVATE
    SDL3::SDL3-static
    SDL3_image::SDL3_image-static)

if(WIN32)
    # Same DPI-awareness/UTF-8 manifest as md.exe.
    target_sources(pixel PRIVATE "${PROJECT_SOURCE_DIR}/src/md.manifest")
endif()

if(MSVC)
    target_compile_options(pixel PRIVATE /W4 /utf-8 /permissive-
        $<$<CONFIG:Release,MinSizeRel>:/GS->)
    target_compile_definitions(pixel PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_options(pixel PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(pixel PRIVATE -Wall -Wextra -Wconversion -Wno-sign-conversion)
endif()
