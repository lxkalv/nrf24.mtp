#include "utils.h"

#include <stdio.h>
#include <stdarg.h>

/* ANSI colour codes */
#define COLOR_RED   "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE  "\033[34m"
#define COLOR_RESET "\033[0m"

static void vlog_with_prefix(const char *prefix,
                             const char *color,
                             FILE *stream,
                             const char *fmt,
                             va_list ap)
{
    fprintf(stream, "%s[%s]:%s ", color, prefix, COLOR_RESET);
    vfprintf(stream, fmt, ap);
    fprintf(stream, "\n");
    fflush(stream);
}

void ERROR(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_with_prefix("~ERR", COLOR_RED, stderr, fmt, ap);
    va_end(ap);
}

void WARN(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_with_prefix("WARN", COLOR_YELLOW, stderr, fmt, ap);
    va_end(ap);
}

void INFO(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_with_prefix("INFO", COLOR_BLUE, stdout, fmt, ap);
    va_end(ap);
}

void SUCC(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_with_prefix("SUCC", COLOR_GREEN, stdout, fmt, ap);
    va_end(ap);
}
