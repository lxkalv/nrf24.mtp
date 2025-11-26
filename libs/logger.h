#ifndef LOGGER_H
#define LOGGER_H

#include <stddef.h>  /* size_t */

/* Optional log levels if we want to extend later */
typedef enum {
    LOGGER_LEVEL_DEBUG = 0,
    LOGGER_LEVEL_INFO,
    LOGGER_LEVEL_WARN,
    LOGGER_LEVEL_ERROR
} logger_level_t;

/**
 * Initialize the logger.
 *
 * If file_path is non-NULL, the logger will append to that file.
 * If opening the file fails, the logger will still work for console output,
 * and logger_init() will return non-zero.
 *
 * Calling logger_init() multiple times will close any previously open log file
 * and attempt to open the new one.
 */
int  logger_init(const char *file_path);

/**
 * Close the logger and flush/close the log file if it is open.
 * Safe to call even if logger_init() failed or was never called.
 */
void logger_close(void);

/**
 * Optional: set minimum log level to output.
 * For now, this is implemented but not strictly necessary;
 * default level is LOGGER_LEVEL_DEBUG (everything printed).
 */
void logger_set_level(logger_level_t level);

/**
 * Logging functions (printf-style).
 *
 * These print to stdout with a colored prefix and also write to the
 * log file (if opened) without colors. They always append a newline.
 */
void logger_error(const char *fmt, ...);
void logger_warn (const char *fmt, ...);
void logger_info (const char *fmt, ...);
void logger_succ (const char *fmt, ...);

/**
 * Write current local time into buf in the format:
 *   "YYYY-MM-DD_HH-MM-SS"
 *
 * buf_len must be > 0. If something fails, a fallback string is written.
 */
void logger_timestamp(char *buf, size_t buf_len);

#endif /* LOGGER_H */
