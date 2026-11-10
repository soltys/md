/* Command line: md [options] [file.md], --help and --version.
 *
 * md.exe is a GUI-subsystem program, so it has no console of its own. Text
 * goes to stdout when that is redirected (md --help > help.txt, | more),
 * otherwise to the console md was started from, and when there is none
 * (Explorer, a shortcut) into a message box. */
#include "cli.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <stdio.h>
#endif

#ifndef MD_VERSION
#define MD_VERSION "0.0.0-dev"
#endif

static const char HELP[] =
"md " MD_VERSION " - a fast Markdown viewer with a Neovim editor\n"
"\n"
"USAGE\n"
"    md [OPTIONS] [FILE]\n"
"    mdv [OPTIONS] [FILE]      (the same program, installed under a second name)\n"
"\n"
"    md opens FILE as rendered Markdown. Without FILE it shows an empty window:\n"
"    open a file with Ctrl+O, Ctrl+Shift+N, or by dropping it on the window.\n"
"\n"
"ARGUMENTS\n"
"    FILE                Markdown file to open (.md, .markdown, .mdown, .mkd,\n"
"                        .mkdn, or any text file). Relative paths are relative to\n"
"                        the current directory; links and images inside the file\n"
"                        are resolved relative to the file itself.\n"
"\n"
"OPTIONS\n"
"    -e, --edit          Start with the editor (Neovim) open beside the preview\n"
"                        and keyboard focus in the editor. Neovim is launched\n"
"                        while the window is being created, so this costs no\n"
"                        extra time. Without -e md starts as a viewer and only\n"
"                        launches Neovim when the editor is first asked for.\n"
"    -l, --light         Use the light theme, whatever the Windows setting.\n"
"    -d, --dark          Use the dark theme, whatever the Windows setting.\n"
"    --theme=THEME       light, dark or system (the default: follow Windows'\n"
"                        light / dark app mode, also when it changes while md\n"
"                        runs). '--theme dark' works too. The last one wins.\n"
"    -h, --help, /?      Show this help and exit.\n"
"    -V, --version       Show the version and exit.\n"
"    --                  Treat everything after this as a file name, even if it\n"
"                        starts with '-'.\n"
"\n"
"    Options and FILE can come in any order: md -e notes.md, md notes.md -e.\n"
"\n"
"WINDOW LAYOUT\n"
"    md starts as a viewer: only the rendered Markdown. With the editor shown,\n"
"    the window is split between Neovim and the preview.\n"
"\n"
"    Ctrl+Shift+S        Editor beside the preview on / off (off = viewer only)\n"
"    Ctrl+Shift+E        Editor only on / off\n"
"    Ctrl+Shift+O        Split side by side / stacked (top and bottom)\n"
"    Ctrl+Shift+X        Swap which side (or half) the editor is on\n"
"    Ctrl+Shift+Tab      Move keyboard focus between editor and preview\n"
"    F1                  Show / hide the list of shortcuts in the window\n"
"\n"
"    Shortcuts use Ctrl+Shift so they never clash with AltGr letters (AltGr is\n"
"    Ctrl+Alt on Windows) or with Neovim's own Ctrl mappings. Clicking a pane\n"
"    also gives it focus.\n"
"\n"
"FILES\n"
"    Ctrl+Shift+N        Open a file (file dialog); works in both panes\n"
"    Ctrl+O              Open a file (when the preview has focus)\n"
"    Drag and drop       Drop a file on the window to open it\n"
"    Ctrl+Q              Quit (when the preview has focus)\n"
"    F2                  Copy the full path of the open file to the clipboard\n"
"    F5                  Reload the file from disk, keeping the scroll position.\n"
"                        With the editor running Neovim re-reads it, unless the\n"
"                        buffer has unsaved changes (then nothing is reloaded)\n"
"    F8                  Switch between the light and dark theme (md then\n"
"                        stops following the Windows setting until restarted)\n"
"    F12                 Save the whole rendered document as a PNG image (asks\n"
"                        where). The image is as wide as the preview pane, at\n"
"                        the display scale, in the current light / dark theme\n"
"\n"
"    With the editor running, files are opened in Neovim and the preview\n"
"    follows its buffer. Closing the window asks Neovim to quit, so unsaved\n"
"    changes are never lost silently: Neovim refuses and tells you why.\n"
"\n"
"PREVIEW: SCROLLING (Vim-style keys work too)\n"
"    Up / Down, j / k            One line up / down (5j: five lines)\n"
"    PgUp / PgDn                 One page up / down\n"
"    Space / Shift+Space         One page down / up\n"
"    Ctrl+d / Ctrl+u             Half a page down / up\n"
"    Ctrl+f / Ctrl+b             One page down / up\n"
"    Home / End, gg / G          Start / end of the document\n"
"    42G or 42gg                 Jump to source line 42\n"
"    Mouse wheel                 Scroll the pane under the mouse\n"
"\n"
"PREVIEW: SEARCH\n"
"    /                   Search forward; the first match is shown as you type\n"
"    ?                   Search backward\n"
"    Enter / Esc         Keep the match / cancel and go back\n"
"    n / N               Next / previous match (wraps around, with a message)\n"
"\n"
"    Search is case-insensitive unless the text contains a capital letter.\n"
"    The current match is selected, so Ctrl+C copies it.\n"
"\n"
"PREVIEW: SELECTING AND COPYING\n"
"    Mouse drag          Select text (dragging past the edge scrolls)\n"
"    Shift+click         Extend the selection\n"
"    Ctrl+A              Select everything\n"
"    Ctrl+C              Copy as rich text: pasting into Word, Outlook, Teams or\n"
"                        a browser keeps headings, lists, links, tables and code\n"
"                        colours; plain-text programs get the plain text\n"
"    Ctrl+Shift+C        Copy as plain text only\n"
"\n"
"PREVIEW: LINKS\n"
"    Click               Open the link. Web and mailto: links open in the\n"
"                        browser / mail program. Links to Markdown files open in\n"
"                        md (in Neovim when the editor runs). Other local files\n"
"                        open with their usual program. %20-style escapes and\n"
"                        file:// links are understood; #anchors are ignored.\n"
"\n"
"EDITOR\n"
"    The editor pane is a real Neovim (nvim --embed). Every key except md's\n"
"    Ctrl+Shift shortcuts and F1, F2, F5 and F12 goes to Neovim, including the mouse.\n"
"\n"
"    - The preview re-renders from the buffer as you type, before you save.\n"
"    - Scrolling is kept in sync both ways: scroll the editor and the preview\n"
"      follows; scroll the preview and the editor follows. At the end of the\n"
"      file both show the end.\n"
"    - Neovim must be on PATH (version 0.12 or newer). If it cannot be\n"
"      started, md stays a viewer and says why.\n"
"    - md runs Neovim with its own configuration, embedded in md.exe and\n"
"      written to %APPDATA%\\md\\init.lua (nvim -u <that file>): line numbers,\n"
"      soft wrap, 2-space indent, no swap files, y / d / p on the Windows\n"
"      clipboard (win32yank.exe from the Neovim release, else PowerShell)\n"
"      and the catppuccin-mocha\n"
"      colour scheme, installed on first run with vim.pack (needs git and a\n"
"      network connection once; without it the default colours are used).\n"
"      Your own Neovim configuration is not loaded.\n"
"\n"
"RENDERING\n"
"    Headings (ATX and setext), paragraphs, hard line breaks, bold, italic,\n"
"    strikethrough, inline code, links (inline, reference, autolinks, e-mail),\n"
"    images (PNG, JPG, GIF, BMP, SVG, QOI, ... loaded in the background; remote\n"
"    images show their alt text), bulleted / numbered / nested / task lists,\n"
"    nested block quotes, tables with column alignment, horizontal rules, HTML\n"
"    entities, emoji (Noto Color Emoji, including skin tones, flags and ZWJ\n"
"    sequences), and fenced code blocks with syntax highlighting for C, C++,\n"
"    C#, Python, JavaScript, JSON, Bash, Rust and Lua. HTML blocks are shown as\n"
"    source. The colours follow the Windows light / dark theme; code blocks\n"
"    use Catppuccin colours: Latte when light, Mocha when dark.\n"
"\n"
"    YAML front matter (between --- lines at the top of the file) is shown as\n"
"    a table of its top-level keys; lists become code pills, quotes and\n"
"    comments are dropped. TOML front matter (+++) is hidden.\n"
"\n"
"    Fonts (Roboto, Roboto Mono Nerd Font, Noto Color Emoji, Noto Sans Math\n"
"    and Symbols 2 for arrows and symbols) are built into md.exe;\n"
"    nothing has to be installed besides Neovim for the editor.\n"
"\n"
"EXIT STATUS\n"
"    0   md was closed normally, or --help / --version was shown\n"
"    1   md could not start (the window could not be created)\n"
"    2   unknown option\n"
"\n"
"EXAMPLES\n"
"    md README.md                View a file\n"
"    md -e notes.md              Edit a file with the live preview beside it\n"
"    md --dark README.md         View a file in the dark theme\n"
"    mdv docs\\features.md        Same as md\n"
"    md --help | more            Read this help page by page\n"
"    md -- -strange-name.md      Open a file whose name starts with '-'\n"
"\n"
"MORE\n"
"    docs/features.md in the source tree is a tour of everything md renders;\n"
"    open it in md. Source: https://github.com/soltys/md\n";

#define MD_STR2(x) #x
#define MD_STR(x) MD_STR2(x)
#define MD_VER3(a, b, c) MD_STR(a) "." MD_STR(b) "." MD_STR(c)

static const char VERSION[] =
"md " MD_VERSION "\n"
"SDL " MD_VER3(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION)
", SDL_ttf " MD_VER3(SDL_TTF_MAJOR_VERSION, SDL_TTF_MINOR_VERSION, SDL_TTF_MICRO_VERSION)
", SDL_image " MD_VER3(SDL_IMAGE_MAJOR_VERSION, SDL_IMAGE_MINOR_VERSION, SDL_IMAGE_MICRO_VERSION)
", tree-sitter\n";

static void print(const char *text, bool error)
{
#ifdef _WIN32
    /* 1. Redirected to a file or pipe: write the UTF-8 bytes as they are. */
    HANDLE h = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD type = h && h != INVALID_HANDLE_VALUE ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
        DWORD written;
        WriteFile(h, text, (DWORD)SDL_strlen(text), &written, NULL);
        return;
    }
    /* 2. Started from a console (cmd, PowerShell, Windows Terminal). The
     *    shell does not wait for a GUI program, so start on a fresh line. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);
        if (con != INVALID_HANDLE_VALUE) {
            int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
            WCHAR *wide = SDL_malloc(sizeof(WCHAR) * (size_t)(n + 2));
            if (wide) {
                wide[0] = L'\n';
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wide + 1, n);
                DWORD written;
                WriteConsoleW(con, wide, (DWORD)(n > 0 ? n : 1), &written, NULL);
                SDL_free(wide);
            }
            CloseHandle(con);
            FreeConsole();
            return;
        }
        FreeConsole();
    }
    /* 3. No console at all: a message box. */
    SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION,
                             "md", text, NULL);
#else
    fputs(text, error ? stderr : stdout);
    fflush(error ? stderr : stdout);
#endif
}

static bool is(const char *arg, const char *a, const char *b)
{
    return SDL_strcmp(arg, a) == 0 || (b && SDL_strcmp(arg, b) == 0);
}

int cli_parse(int argc, char **argv, CliOptions *out)
{
    SDL_zerop(out);
    bool only_files = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!only_files && a[0] == '-' && a[1] != '\0') {
            const char *theme = NULL;
            if (is(a, "-e", "--edit")) out->edit = true;
            else if (is(a, "-l", "--light")) out->theme = CLI_THEME_LIGHT;
            else if (is(a, "-d", "--dark")) out->theme = CLI_THEME_DARK;
            else if (SDL_strncmp(a, "--theme=", 8) == 0) theme = a + 8;
            else if (is(a, "--theme", NULL)) theme = i + 1 < argc ? argv[++i] : "";
            else if (is(a, "-h", "--help")) { print(HELP, false); return 0; }
            else if (is(a, "-V", "--version")) { print(VERSION, false); return 0; }
            else if (is(a, "--", NULL)) only_files = true;
            else {
                char *msg = NULL;
                SDL_asprintf(&msg, "md: unknown option '%s'\nTry 'md --help' for the list of options.\n", a);
                print(msg ? msg : "md: unknown option\n", true);
                SDL_free(msg);
                return 2;
            }
            if (theme) {
                if (SDL_strcasecmp(theme, "light") == 0) out->theme = CLI_THEME_LIGHT;
                else if (SDL_strcasecmp(theme, "dark") == 0) out->theme = CLI_THEME_DARK;
                else if (SDL_strcasecmp(theme, "system") == 0 || SDL_strcasecmp(theme, "auto") == 0)
                    out->theme = CLI_THEME_SYSTEM;
                else {
                    char *msg = NULL;
                    SDL_asprintf(&msg, "md: unknown theme '%s' (use light, dark or system)\n", theme);
                    print(msg ? msg : "md: unknown theme\n", true);
                    SDL_free(msg);
                    return 2;
                }
            }
        } else if (!only_files && (is(a, "/?", "/h") || is(a, "/help", NULL))) {
            print(HELP, false);
            return 0;
        } else if (!out->file) {
            out->file = a;
        }
    }
    return -1;
}
