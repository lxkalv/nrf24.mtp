#include "utils.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ANSI colour codes for console */
#define COLOR_RED    "\033[31m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_RESET  "\033[0m"

/* Global logfile handle (optional) */
static FILE *log_fp = NULL;

/* ---------- public API ---------- */

int log_init(const char *path)
{
    if (!path) {
        return -1;
    }

    /* Close any previous logfile */
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }

    log_fp = fopen(path, "w");
    if (!log_fp) {
        /* silently fail; console logging will still work */
        return -1;
    }

    /* Small header line */
    fprintf(log_fp, "Log file: %s\n", path);
    fflush(log_fp);
    return 0;
}

void log_close(void)
{
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

/* ---------- internal helper ---------- */

static void vlog_with_prefix(const char *prefix,
                             const char *color,
                             FILE *stream,
                             const char *fmt,
                             va_list ap)
{
    /* We need a copy if we want to use the va_list twice */
    va_list ap_copy;
    va_copy(ap_copy, ap);

    /* Console (with colour) */
    fprintf(stream, "%s[%s]:%s ", color, prefix, COLOR_RESET);
    vfprintf(stream, fmt, ap);
    fprintf(stream, "\n");
    fflush(stream);

    /* Optional logfile (no colours) */
    if (log_fp) {
        fprintf(log_fp, "[%s]: ", prefix);
        vfprintf(log_fp, fmt, ap_copy);
        fprintf(log_fp, "\n");
        fflush(log_fp);
    }

    va_end(ap_copy);
}

/* ---------- public logging functions ---------- */

void ERROR(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_with_prefix("ERR ", COLOR_RED, stderr, fmt, ap);
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
