/* REPL: commands live in the .ndf_reg.cmd.* sections (sorted by name by the linker). */
#include "cmd.h"
#include "console.h"
#include <string.h>

extern const cmd_t __cmd_start[], __cmd_end[];
static char line[128];
static int len;
static const char *prompt = "> ";

void cmd_prompt(const char *p) { prompt = p; }

int cmd_exec(char *s)
{
    char *argv[16]; int argc = 0;
    while (*s && argc < 16) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        char q = (*s == '"' || *s == '\'') ? *s++ : 0;        /* quoted argument */
        argv[argc++] = s;
        while (*s && (q ? *s != q : (*s != ' ' && *s != '\t'))) s++;
        if (*s) *s++ = 0;
    }
    if (!argc) return 0;
    if (!strcmp(argv[0], "help")) {
        for (const cmd_t *c = __cmd_start; c < __cmd_end; c++) printf("  %-12s %s\n", c->name, c->help);
        return 0;
    }
    for (const cmd_t *c = __cmd_start; c < __cmd_end; c++) if (!strcmp(c->name, argv[0])) return c->fn(argc, argv);
    printf("unknown command: %s (help lists them)\n", argv[0]);
    return -1;
}

void cmd_poll(void)
{
    int ch;
    while ((ch = console_getc()) >= 0) {
        if (ch == '\r' || ch == '\n') {
            if (ch == '\n' && !len) continue;                    /* the \n of a \r\n pair */
            console_write("\r\n", 2);
            line[len] = 0; len = 0;
            cmd_exec(line);
            console_write(prompt, strlen(prompt));
        } else if (ch == 8 || ch == 127) {                       /* backspace */
            if (len) { len--; console_write("\b \b", 3); }
        } else if (ch == 21 || ch == 3) {                        /* Ctrl-U / Ctrl-C: drop the line */
            len = 0; console_write("^C\r\n", 4); console_write(prompt, strlen(prompt));
        } else if (ch >= ' ' && len < (int)sizeof line - 1) {
            line[len++] = (char)ch; char c = (char)ch; console_write(&c, 1);
        }
    }
}
