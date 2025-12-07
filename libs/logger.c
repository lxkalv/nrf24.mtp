#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>  /* gettimeofday for milliseconds */
#include <sys/stat.h>   /* mkdir */
#include <sys/types.h>

/* ANSI color codes for console output */
#define LOGGER_COLOR_RED    "\x1b[31m"
#define LOGGER_COLOR_GREEN  "\x1b[32m"
#define LOGGER_COLOR_YELLOW "\x1b[33m"
#define LOGGER_COLOR_BLUE   "\x1b[34m"
#define LOGGER_COLOR_RESET  "\x1b[0m"

/* Internal state */
static FILE *logger_fp = NULL;
static logger_level_t logger_min_level = LOGGER_LEVEL_DEBUG;

static int ensure_dir(const char *path)
{
    if (!path || !*path) {
        return -1;
    }

    int rc = mkdir(path, 0755);
    if (rc == 0 || errno == EEXIST) {
        return 0;
    }
    fprintf(stderr, "[LOGGER WARN]: Could not create directory '%s': %s\n",
            path, strerror(errno));
    return -1;
}

/* Forward declaration of internal helper */
static void logger_vlog(logger_level_t level,
                        const char *color_code,
                        const char *plain_prefix,
                        const char *fmt,
                        va_list args);

/* Small helper: get current local time with milliseconds as
 * "YYYY-MM-DD HH:MM:SS.mmm"
 */
static void logger_time_iso8601(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return;
    }

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        /* Fallback */
        snprintf(buf, buf_len, "0000-00-00 00:00:00.000");
        return;
    }

    time_t sec = tv.tv_sec;
    struct tm *tm_ptr = NULL;

#if defined(_WIN32) && !defined(__MINGW32__)
    /* Windows secure version */
    struct tm tm_info;
    if (localtime_s(&tm_info, &sec) != 0) {
        snprintf(buf, buf_len, "0000-00-00 00:00:00.000");
        return;
    }
    tm_ptr = &tm_info;
#else
    tm_ptr = localtime(&sec);
    if (tm_ptr == NULL) {
        snprintf(buf, buf_len, "0000-00-00 00:00:00.000");
        return;
    }
#endif

    char date_part[32];
    if (strftime(date_part, sizeof(date_part), "%Y-%m-%d %H:%M:%S", tm_ptr) == 0) {
        snprintf(buf, buf_len, "0000-00-00 00:00:00.000");
        return;
    }

    int millis = (int)(tv.tv_usec / 1000);
    snprintf(buf, buf_len, "%s.%03d", date_part, millis);
}


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

    /* Build timestamped path inside "logs/<optional_dir>/<basename>" directory tree.
     *
     * Examples:
     *   file_path = "p2p_tx.log"
     *     -> logs/p2p_tx/p2p_tx-YYYY-MM-DD_HH-MM-SS.log
     *   file_path = "p2p/p2p_tx.log"
     *     -> logs/p2p/p2p_tx/p2p_tx-YYYY-MM-DD_HH-MM-SS.log
     */

    char ts[32];
    logger_timestamp(ts, sizeof(ts));  /* seconds precision, with '_' */

    /* Extract optional directory and basename (strip separators) */
    char normalized[512];
    snprintf(normalized, sizeof(normalized), "%s", file_path);
    for (size_t i = 0; normalized[i] != '\0'; ++i) {
        if (normalized[i] == '\\') {
            normalized[i] = '/';
        }
    }

    char dir_part[512] = {0};
    const char *base = normalized;
    char *last_sep = strrchr(normalized, '/');
    if (last_sep) {
        *last_sep = '\0';
        base = last_sep + 1;
        if (*normalized) {
            snprintf(dir_part, sizeof(dir_part), "%s", normalized);
        }
    }

    /* Strip extension from basename (e.g., ".log") */
    const char *dot = strrchr(base, '.');
    size_t base_len = dot ? (size_t)(dot - base) : strlen(base);

    const char *base_fallback = "general";
    if (base_len == 0) {
        base      = base_fallback;
        base_len  = strlen(base_fallback);
    }

    (void)ensure_dir("logs");

    char final_path[512];

    char base_prefix[256];
    size_t copy_len = base_len;
    const size_t max_final = sizeof(final_path) - 1;
    const size_t ts_len = strlen(ts);
    const size_t constants = strlen("logs/") + ts_len + 6; /* "logs/" + '/' + '-' + ".log" */

    if (constants >= max_final) {
        copy_len = 0;
    } else {
        size_t max_per_base = (max_final - constants) / 2; /* base appears twice */
        if (copy_len > max_per_base) {
            copy_len = max_per_base;
        }
    }

    if (copy_len >= sizeof(base_prefix)) {
        copy_len = sizeof(base_prefix) - 1;
    }
    memcpy(base_prefix, base, copy_len);
    base_prefix[copy_len] = '\0';

    char subdir[512];
    if (dir_part[0] != '\0') {
        char accum[512];
        size_t accum_len = (size_t)snprintf(accum, sizeof(accum), "logs");
        if (accum_len >= sizeof(accum)) {
            accum[sizeof(accum) - 1] = '\0';
            accum_len = strlen(accum);
        }

        const char *segment = dir_part;
        while (*segment) {
            while (*segment == '/') {
                ++segment;
            }
            if (!*segment) {
                break;
            }
            const char *next = strchr(segment, '/');
            size_t seg_len = next ? (size_t)(next - segment) : strlen(segment);
            if (seg_len == 0) {
                segment = next ? next + 1 : segment;
                continue;
            }
            if (accum_len + 1 + seg_len >= sizeof(accum)) {
                seg_len = sizeof(accum) - accum_len - 2;
                if ((int)seg_len <= 0) {
                    break;
                }
            }
            accum[accum_len++] = '/';
            memcpy(accum + accum_len, segment, seg_len);
            accum_len += seg_len;
            accum[accum_len] = '\0';
            (void)ensure_dir(accum);
            segment = next ? next + 1 : segment + seg_len;
        }

        if (accum_len == 0) {
            snprintf(accum, sizeof(accum), "logs");
        }

        (void)snprintf(subdir, sizeof(subdir), "%s/%s", accum, base_prefix);
    } else {
        (void)snprintf(subdir, sizeof(subdir),
                       "logs/%s",
                       base_prefix);
    }
    (void)ensure_dir(subdir);
    (void)snprintf(final_path, sizeof(final_path),
                   "%s/%s-%s.log",
                   subdir, base_prefix, ts);

    logger_fp = fopen(final_path, "w");
    if (!logger_fp) {
        /* Not fatal: we still log to console */
        fprintf(stderr,
                "[LOGGER WARN]: Could not open log file '%s' (requested '%s'): %s\n",
                final_path, file_path, strerror(errno));
        return -1;
    }

    /* Optional header line with creation timestamp (with milliseconds) */
    char human_ts[64];
    logger_time_iso8601(human_ts, sizeof(human_ts));
    fprintf(logger_fp,
            "[%s] Log file created (requested name '%s')\n",
            human_ts, file_path);
    fflush(logger_fp);

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

    /* Format: YYYY-MM-DD_HH-MM-SS (for filenames) */
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
                "%s[ERRO]:%s <formatting error>\n",
                LOGGER_COLOR_RED,
                LOGGER_COLOR_RESET);
        fflush(stdout);
        if (logger_fp) {
            char ts[64];
            logger_time_iso8601(ts, sizeof(ts));
            fprintf(logger_fp, "[%s] [ERRO]: <formatting error>\n", ts);
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

    /* File output: timestamp (with ms) + prefix + message (no colors) */
    if (logger_fp) {
        char ts[64];
        logger_time_iso8601(ts, sizeof(ts));
        fprintf(logger_fp, "[%s] %s %s\n", ts, plain_prefix, msg_buf);
        fflush(logger_fp);
    }
}
