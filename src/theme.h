#ifndef MD_THEME_H
#define MD_THEME_H

#include <SDL3/SDL.h>
#include "highlight.h"

typedef struct Theme {
    SDL_Color bg;
    SDL_Color text;
    SDL_Color muted;
    SDL_Color heading;
    SDL_Color rule;
    SDL_Color code_bg;        /* inline code, table stripes, search bar */
    SDL_Color code_text;
    SDL_Color block_bg;       /* fenced / indented code blocks */
    SDL_Color block_text;
    SDL_Color block_label;    /* the language name above a block */
    SDL_Color quote_bar;
    SDL_Color accent;
    SDL_Color syntax[HL_COUNT];
} Theme;

/* Code blocks use Catppuccin (https://catppuccin.com/palette/): Latte in
 * the light theme, Mocha in the dark one. Roles follow catppuccin/nvim's
 * lua/catppuccin/groups/treesitter.lua (see class_for in highlight.c). */
#define CTP(hex) {(Uint8)((hex) >> 16), (Uint8)(((hex) >> 8) & 0xFF), (Uint8)((hex) & 0xFF), 0xFF}
#define LATTE_FLAMINGO 0xDD7878
#define LATTE_PINK 0xEA76CB
#define LATTE_MAUVE 0x8839EF
#define LATTE_RED 0xD20F39
#define LATTE_MAROON 0xE64553
#define LATTE_PEACH 0xFE640B
#define LATTE_YELLOW 0xDF8E1D
#define LATTE_GREEN 0x40A02B
#define LATTE_TEAL 0x179299
#define LATTE_SKY 0x04A5E5
#define LATTE_SAPPHIRE 0x209FB5
#define LATTE_BLUE 0x1E66F5
#define LATTE_LAVENDER 0x7287FD
#define LATTE_TEXT 0x4C4F69
#define LATTE_OVERLAY2 0x7C7F93
#define LATTE_OVERLAY1 0x8C8FA1
#define LATTE_BASE 0xEFF1F5
#define LATTE_MANTLE 0xE6E9EF

#define MOCHA_FLAMINGO 0xF2CDCD
#define MOCHA_PINK 0xF5C2E7
#define MOCHA_MAUVE 0xCBA6F7
#define MOCHA_RED 0xF38BA8
#define MOCHA_MAROON 0xEBA0AC
#define MOCHA_PEACH 0xFAB387
#define MOCHA_YELLOW 0xF9E2AF
#define MOCHA_GREEN 0xA6E3A1
#define MOCHA_TEAL 0x94E2D5
#define MOCHA_SKY 0x89DCEB
#define MOCHA_SAPPHIRE 0x74C7EC
#define MOCHA_BLUE 0x89B4FA
#define MOCHA_LAVENDER 0xB4BEFE
#define MOCHA_TEXT 0xCDD6F4
#define MOCHA_OVERLAY2 0x9399B2
#define MOCHA_OVERLAY1 0x7F849C
#define MOCHA_BASE 0x1E1E2E
#define MOCHA_MANTLE 0x181825

#define CATPPUCCIN_CODE(F) \
    .block_bg    = CTP(F##_BASE), \
    .block_text  = CTP(F##_TEXT), \
    .block_label = CTP(F##_OVERLAY1), \
    .syntax = { \
        [HL_KEYWORD]          = CTP(F##_MAUVE), \
        [HL_STRING]           = CTP(F##_GREEN), \
        [HL_COMMENT]          = CTP(F##_OVERLAY2), \
        [HL_NUMBER]           = CTP(F##_PEACH), \
        [HL_CONSTANT]         = CTP(F##_PEACH), \
        [HL_FUNCTION]         = CTP(F##_BLUE), \
        [HL_TYPE]             = CTP(F##_YELLOW), \
        [HL_VARIABLE_BUILTIN] = CTP(F##_RED), \
        [HL_OPERATOR]         = CTP(F##_SKY), \
        [HL_PUNCTUATION]      = CTP(F##_OVERLAY2), \
        [HL_PROPERTY]         = CTP(F##_LAVENDER), \
        [HL_TAG]              = CTP(F##_BLUE), \
        [HL_ATTRIBUTE]        = CTP(F##_YELLOW), \
        [HL_PARAMETER]        = CTP(F##_MAROON), \
        [HL_FUNCTION_BUILTIN] = CTP(F##_PEACH), \
        [HL_SPECIAL]          = CTP(F##_PINK), \
        [HL_CHARACTER]        = CTP(F##_TEAL), \
        [HL_LABEL]            = CTP(F##_SAPPHIRE), \
        [HL_SYMBOL]           = CTP(F##_FLAMINGO), \
    }

static const Theme THEME_LIGHT = {
    .bg        = {0xFF, 0xFF, 0xFF, 0xFF},
    .text      = {0x1F, 0x23, 0x28, 0xFF},
    .muted     = {0x59, 0x63, 0x6E, 0xFF},
    .heading   = {0x1F, 0x23, 0x28, 0xFF},
    .rule      = {0xD1, 0xD9, 0xE0, 0xFF},
    .code_bg   = {0xF6, 0xF8, 0xFA, 0xFF},
    .code_text = {0x1F, 0x23, 0x28, 0xFF},
    .quote_bar = {0xD0, 0xD7, 0xDE, 0xFF},
    .accent    = {0x09, 0x69, 0xDA, 0xFF},
    CATPPUCCIN_CODE(LATTE),
};

static const Theme THEME_DARK = {
    .bg        = {0x0D, 0x11, 0x17, 0xFF},
    .text      = {0xE6, 0xED, 0xF3, 0xFF},
    .muted     = {0x9D, 0xA7, 0xB3, 0xFF},
    .heading   = {0xF0, 0xF6, 0xFC, 0xFF},
    .rule      = {0x30, 0x36, 0x3D, 0xFF},
    .code_bg   = {0x16, 0x1B, 0x22, 0xFF},
    .code_text = {0xE6, 0xED, 0xF3, 0xFF},
    .quote_bar = {0x3D, 0x44, 0x4D, 0xFF},
    .accent    = {0x47, 0x8B, 0xE6, 0xFF},
    CATPPUCCIN_CODE(MOCHA),
};

#endif
