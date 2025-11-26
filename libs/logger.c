#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <errno.h>

/* ANSI color codes for console output */
#define LOGGER_COLOR_RED    "\x1b[31m"
#define LOGGER_COLOR_GREEN  "\x1b[32m"
#define LOGGER_COLOR_YELLOW "\x1b[33m"
#define LOGGER_COLOR_BLUE   "\x1b[34m"
#define LOGGER_COLOR_RESET  "\x1b[0m"

/* Internal state */
static FILE *logger_fp = NULL;
static logger_level_t logger_min_level = LOGGER_LEVEL_DEBUG;

/* Forward declaration of internal helper */
static void logger_vlog(logger_level_t level,
                        const char *color_code,
                        const char *plain_prefix,
                        const char *fmt,
                        va_list args);

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int logger_init(const char *file_path)
{
    /* If there is already an open log file, close it first */
    if (logger_fp != NULL) {
        fflush(logger_fp);
        fclose(logger_fp);
        logger_fp = NULL;
    }

    if (file_path == NULL) {
        /* Console-only logging */
        return 0;
    }

    logger_fp = fopen(file_path, "a");
    if (!logger_fp) {
        /* Not fatal: we still log to console */
        fprintf(stderr,
                "[LOGGER WARN]: Could not open log file '%s': %s\n",
                file_path, strerror(errno));
        return -1;
    }

    return 0;
}

void logger_close(void)
{
    if (logger_fp != NULL) {
        fflush(logger_fp);
        fclose(logger_fp);
        logger_fp = NULL;
    }
}

void logger_set_level(logger_level_t level)
{
    logger_min_level = level;
}

void logger_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_vlog(LOGGER_LEVEL_ERROR,
                LOGGER_COLOR_RED,
                "[ERRO]:",
                fmt, args);
    va_end(args);
}

void logger_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_vlog(LOGGER_LEVEL_WARN,
                LOGGER_COLOR_YELLOW,
                "[WARN]:",
                fmt, args);
    va_end(args);
}

void logger_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_vlog(LOGGER_LEVEL_INFO,
                LOGGER_COLOR_BLUE,
                "[INFO]:",
                fmt, args);
    va_end(args);
}

void logger_succ(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    /* Treat success as INFO level */
    logger_vlog(LOGGER_LEVEL_INFO,
                LOGGER_COLOR_GREEN,
                "[SUCC]:",
                fmt, args);
    va_end(args);
}

void logger_timestamp(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) {
        /* Fallback string on error */
        snprintf(buf, buf_len, "0000-00-00_00-00-00");
        return;
    }

    struct tm *tm_ptr = NULL;

#if defined(_WIN32) && !defined(__MINGW32__)
    /* Microsoft secure version */
    struct tm tm_info;
    if (localtime_s(&tm_info, &now) != 0) {
        snprintf(buf, buf_len, "0000-00-00_00-00-00");
        return;
    }
    tm_ptr = &tm_info;
#else
    tm_ptr = localtime(&now);
    if (tm_ptr == NULL) {
        snprintf(buf, buf_len, "0000-00-00_00-00-00");
        return;
    }
#endif

    /* Format: YYYY-MM-DD_HH-MM-SS */
    if (strftime(buf, buf_len, "%Y-%m-%d_%H-%M-%S", tm_ptr) == 0) {
        /* strftime returns 0 if the buffer is too small */
        snprintf(buf, buf_len, "0000-00-00_00-00-00");
    }
}


/* ------------------------------------------------------------------------- */
/* Internal helper                                                           */
/* ------------------------------------------------------------------------- */

static void logger_vlog(logger_level_t level,
                        const char *color_code,
                        const char *plain_prefix,
                        const char *fmt,
                        va_list args)
{
    if (level < logger_min_level) {
        return; /* log level filtered out */
    }

    /* Format the user message into a buffer */
    char msg_buf[1024];
    int  n = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);

    if (n < 0) {
        /* Formatting error; just print something minimal */
        fprintf(stdout,
                "%s[ERR ]:%s <formatting error>\n",
                LOGGER_COLOR_RED,
                LOGGER_COLOR_RESET);
        fflush(stdout);
        if (logger_fp) {
            fprintf(logger_fp, "[ERR ]: <formatting error>\n");
            fflush(logger_fp);
        }
        return;
    }

    msg_buf[sizeof(msg_buf) - 1] = '\0';

    /* Console output:
     * color_code + prefix + reset, then a space and the message (uncolored).
     */
    fprintf(stdout, "%s%s%s %s\n",
            color_code, plain_prefix, LOGGER_COLOR_RESET, msg_buf);
    fflush(stdout);

    /* File output without color codes */
    if (logger_fp) {
        fprintf(logger_fp, "%s %s\n", plain_prefix, msg_buf);
        fflush(logger_fp);
    }
}
