#ifndef CB_CLI_H
#define CB_CLI_H
#include <stdarg.h>
#include <stdio.h>

/* How a CLI command reports a failure: "error: ..." on stderr, then -1 for
 * the command to return. main() alone turns that into the exit status;
 * nothing below it exits. */
static inline int cli_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return -1;
}

#endif
