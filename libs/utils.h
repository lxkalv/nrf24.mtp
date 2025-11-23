#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

/* Initialise/close file logging.
 * If log_init() fails it returns -1 and logging to file is disabled,
 * but console logging still works.
 */
int  log_init(const char *path);
void log_close(void);

/* Simple coloured logging helpers, similar to utils.py */

void ERROR(const char *fmt, ...);
void WARN (const char *fmt, ...);
void INFO (const char *fmt, ...);
void SUCC (const char *fmt, ...);

#endif /* UTILS_H */
