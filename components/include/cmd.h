/* Command line over the console: commands registered at compile time (CMD), line editing (backspace,
 * Ctrl-U, Ctrl-C), quotes in arguments, `help` built in. cmd_poll() never blocks: call it from the main loop. */
#pragma once
#include "ndf.h"

typedef int (*cmd_fn_t)(int argc, char **argv);
typedef struct { const char *name, *help; cmd_fn_t fn; } cmd_t;

#define CMD(name_, help_, fn_) \
    static const cmd_t __cmd_##fn_ __attribute__((used, aligned(4), section(".ndf_reg.cmd." #name_))) = { #name_, help_, fn_ }

int  cmd_exec(char *line);        /* tokenises line in place, runs it; command's return, -1 unknown, 0 empty */
void cmd_poll(void);              /* reads console bytes, runs a command on Enter, prints the prompt */
void cmd_prompt(const char *p);   /* default "> " */
