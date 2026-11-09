/* Resource ids shared by assets.rc (RCDATA) and the C code.
 * Kept free of C syntax so rc.exe can include it. */
#ifndef MD_ASSET_IDS_H
#define MD_ASSET_IDS_H

#define ASSET_ROBOTO_REGULAR     101
#define ASSET_ROBOTO_ITALIC      102
#define ASSET_ROBOTO_MEDIUM      103
#define ASSET_ROBOTO_BOLD        104
#define ASSET_ROBOTO_BOLD_ITALIC 105
#define ASSET_ROBOTO_LIGHT       106
#define ASSET_ROBOTO_MONO        107
#define ASSET_ROBOTO_MONO_ITALIC 108
#define ASSET_NOTO_EMOJI         109
#define ASSET_NVIM_INIT          110
#define ASSET_ROBOTO_MONO_BOLD   111
#define ASSET_ROBOTO_MONO_BOLD_ITALIC 112

/* Application icon (art/png/md-icon.ico, drawn in art/sprites.txt). It is the
 * first icon in the exe, so Explorer shows it and SDL uses it for the window
 * class (taskbar, title bar) without any code. */
#define MD_APP_ICON              1

#endif
