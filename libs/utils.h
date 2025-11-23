#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

/* Simple coloured logging helpers, similar to utils.py */

void ERROR(const char *fmt, ...);
void WARN (const char *fmt, ...);
void INFO (const char *fmt, ...);
void SUCC (const char *fmt, ...);

#endif /* UTILS_H */
