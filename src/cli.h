#ifndef MD_CLI_H
#define MD_CLI_H

#include <stdbool.h>

typedef struct CliOptions {
    const char *file;   /* Markdown file to open, or NULL */
    bool edit;          /* -e / --edit: open with the editor */
} CliOptions;

/* Parses the command line. Returns -1 to go on and start md, or an exit
 * code when md should stop right away (--help, --version, a bad option);
 * the text has already been printed then. */
int cli_parse(int argc, char **argv, CliOptions *out);

#endif
