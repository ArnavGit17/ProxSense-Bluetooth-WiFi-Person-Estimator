/**
 * @file ps_log.c
 * @brief Logging implementation.
 *
 * Phase 0: complete (logging is needed from the very first build to diagnose
 * issues; it has no hardware dependencies).
 *
 * Thread safety: fprintf to stderr is atomic on POSIX for reasonably short
 * writes (< PIPE_BUF bytes).  This is sufficient for Phase 0.
 */

#include "ps_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/* Default to INFO at runtime unless the binary is built with -DDEBUG. */
ps_log_level_t ps_log_runtime_level =
#ifdef DEBUG
    PS_LOG_LEVEL_DEBUG;
#else
    PS_LOG_LEVEL_INFO;
#endif

void ps_log_set_level(ps_log_level_t level)
{
    ps_log_runtime_level = level;
}

void ps_log_write(ps_log_level_t level,
                  const char    *file,
                  int            line,
                  const char    *fmt, ...)
{
    static const char * const level_labels[] = {
        [PS_LOG_LEVEL_ERROR] = "ERROR",
        [PS_LOG_LEVEL_WARN]  = "WARN ",
        [PS_LOG_LEVEL_INFO]  = "INFO ",
        [PS_LOG_LEVEL_DEBUG] = "DEBUG"
    };

    /* Get current wall-clock time for the log timestamp. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    char time_buf[24]; /* "HH:MM:SS.mmm\0" = 13 chars, 24 is safe */
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_info);

    /* Print the log prefix: timestamp, level, file:line (debug only). */
    if (level <= PS_LOG_LEVEL_WARN) {
        /* For warnings and errors, show file:line to aid debugging. */
        fprintf(stderr, "[%s.%03ld] [%s] (%s:%d) ",
                time_buf, ts.tv_nsec / 1000000L,
                level_labels[level], file, line);
    } else {
        fprintf(stderr, "[%s.%03ld] [%s] ",
                time_buf, ts.tv_nsec / 1000000L,
                level_labels[level]);
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
}
