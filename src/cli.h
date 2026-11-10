#ifndef MD_CLI_H
#define MD_CLI_H

#include <stdbool.h>

enum { CLI_THEME_SYSTEM, CLI_THEME_LIGHT, CLI_THEME_DARK };

typedef struct CliOptions {
    const char *file;   /* Markdown file to open, or NULL */
    bool edit;          /* -e / --edit: open with the editor */
    int theme;          /* CLI_THEME_*: --light / --dark / --theme */
} CliOptions;

/* Parses the command line. Returns -1 to go on and start md, or an exit
 * code when md should stop right away (--help, --version, a bad option);
 * the text has already been printed then. */
int cli_parse(int argc, char **argv, CliOptions *out);

#endif
