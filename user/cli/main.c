#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cli_util.h"
#include "../loader/loader.h"

static void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

int parse_args(int argc, char **argv, const char **cmd, const char **arg)
{
    if (argc < 2)
        return -1;
    *cmd = argv[1];
    *arg = argc > 2 ? argv[2] : NULL;
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s <load|unload|status> [arg]\n", prog);
}

int main(int argc, char **argv)
{
    const char *cmd, *arg;
    if (parse_args(argc, argv, &cmd, &arg)) {
        usage(argv[0]);
        return 1;
    }

    if (!strcmp(cmd, "load")) {
        if (!arg) {
            usage(argv[0]);
            return 1;
        }
        if (module_load(arg))
            log_error("Failed to load module %s", arg);
        else
            log_info("Module %s loaded", arg);
    } else if (!strcmp(cmd, "unload")) {
        if (!arg) {
            usage(argv[0]);
            return 1;
        }
        if (module_unload(arg))
            log_error("Failed to unload module %s", arg);
        else
            log_info("Module %s unloaded", arg);
    } else if (!strcmp(cmd, "status")) {
        log_info("Status command not implemented");
    } else {
        usage(argv[0]);
        return 1;
    }

    return 0;
}
