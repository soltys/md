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

/* Code blocks use Catppuccin Mocha (https://catppuccin.com/palette/) in
 * both themes, matching the editor pane. Roles follow catppuccin/nvim's
 * lua/catppuccin/groups/treesitter.lua (see class_for in highlight.c). */
#define MOCHA(hex) {(Uint8)((hex) >> 16), (Uint8)(((hex) >> 8) & 0xFF), (Uint8)((hex) & 0xFF), 0xFF}
#define MOCHA_CODE \
    .block_bg    = MOCHA(0x1E1E2E), /* base */ \
    .block_text  = MOCHA(0xCDD6F4), /* text */ \
    .block_label = MOCHA(0x7F849C), /* overlay1 */ \
    .syntax = { \
        [HL_KEYWORD]          = MOCHA(0xCBA6F7), /* mauve */ \
        [HL_STRING]           = MOCHA(0xA6E3A1), /* green */ \
        [HL_COMMENT]          = MOCHA(0x9399B2), /* overlay2 */ \
        [HL_NUMBER]           = MOCHA(0xFAB387), /* peach */ \
        [HL_CONSTANT]         = MOCHA(0xFAB387), /* peach */ \
        [HL_FUNCTION]         = MOCHA(0x89B4FA), /* blue */ \
        [HL_TYPE]             = MOCHA(0xF9E2AF), /* yellow */ \
        [HL_VARIABLE_BUILTIN] = MOCHA(0xF38BA8), /* red */ \
        [HL_OPERATOR]         = MOCHA(0x89DCEB), /* sky */ \
        [HL_PUNCTUATION]      = MOCHA(0x9399B2), /* overlay2 */ \
        [HL_PROPERTY]         = MOCHA(0xB4BEFE), /* lavender */ \
        [HL_TAG]              = MOCHA(0x89B4FA), /* blue */ \
        [HL_ATTRIBUTE]        = MOCHA(0xF9E2AF), /* yellow */ \
        [HL_PARAMETER]        = MOCHA(0xEBA0AC), /* maroon */ \
        [HL_FUNCTION_BUILTIN] = MOCHA(0xFAB387), /* peach */ \
        [HL_SPECIAL]          = MOCHA(0xF5C2E7), /* pink */ \
        [HL_CHARACTER]        = MOCHA(0x94E2D5), /* teal */ \
        [HL_LABEL]            = MOCHA(0x74C7EC), /* sapphire */ \
        [HL_SYMBOL]           = MOCHA(0xF2CDCD), /* flamingo */ \
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
    MOCHA_CODE,
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
    MOCHA_CODE,
};

#endif
