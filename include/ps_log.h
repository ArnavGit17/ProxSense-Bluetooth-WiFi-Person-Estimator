/**
 * @file ps_log.h
 * @brief Lightweight structured logging for ProxSense.
 *
 * Design goals:
 *   - Zero external dependencies (no syslog by default, though it can be added).
 *   - Compile-time log level filtering to avoid overhead in production builds.
 *   - Thread-safe output (atomic writes via fprintf to stderr).
 *   - Timestamps on every log line.
 *   - Source location (file:line) on DEBUG and higher levels.
 *
 * Usage:
 *   PS_LOG_INFO("Scanner opened on hci%d", dev_id);
 *   PS_LOG_WARN("RSSI unavailable for %s", mac_str);
 *   PS_LOG_ERROR("Failed to open socket: %s", strerror(errno));
 *   PS_LOG_DEBUG("Processing observation from %s", mac_str);
 *
 * Compile-time level: define PS_LOG_LEVEL_MAX before including this header
 * or on the compiler command line.  Messages above the max are compiled out
 * entirely (zero runtime cost).
 */

#ifndef PS_LOG_H
#define PS_LOG_H

#include <stdio.h>
#include <time.h>

/* Log levels — ordered from most to least severe. */
typedef enum ps_log_level {
    PS_LOG_LEVEL_ERROR = 0,
    PS_LOG_LEVEL_WARN  = 1,
    PS_LOG_LEVEL_INFO  = 2,
    PS_LOG_LEVEL_DEBUG = 3
} ps_log_level_t;

/* Default maximum level if not set by the build system. */
#ifndef PS_LOG_LEVEL_MAX
#  ifdef NDEBUG
#    define PS_LOG_LEVEL_MAX PS_LOG_LEVEL_INFO
#  else
#    define PS_LOG_LEVEL_MAX PS_LOG_LEVEL_DEBUG
#  endif
#endif

/* Runtime log level — can be lowered from the CLI via --quiet / --verbose. */
extern ps_log_level_t ps_log_runtime_level;

/**
 * @brief Set the runtime log level (used by CLI argument parsing).
 */
void ps_log_set_level(ps_log_level_t level);

/**
 * @brief Internal log function — do not call directly; use macros below.
 */
void ps_log_write(ps_log_level_t level,
                  const char    *file,
                  int            line,
                  const char    *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* =========================================================================
 * Public logging macros
 *
 * The compile-time guard ensures that log calls above PS_LOG_LEVEL_MAX are
 * removed entirely by the preprocessor — no function call, no format string
 * evaluation, no branch.
 * ========================================================================= */

/*
 * C11 requires at least one argument for the '...' in a variadic macro.
 * The GCC/Clang ##__VA_ARGS__ extension removes the preceding comma when
 * __VA_ARGS__ is empty, allowing zero-argument calls like PS_LOG_INFO("msg").
 * The project uses GCC/Clang in Linux builds, so we suppress the pedantic
 * warning specifically for this extension rather than redesigning the API.
 */
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wvariadic-macros"
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif

#define PS_LOG(level, fmt, ...) \
    do { \
        if ((level) <= PS_LOG_LEVEL_MAX && (level) <= ps_log_runtime_level) { \
            ps_log_write((level), __FILE__, __LINE__, (fmt), ##__VA_ARGS__); \
        } \
    } while (0)

#define PS_LOG_ERROR(fmt, ...) PS_LOG(PS_LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define PS_LOG_WARN(fmt, ...)  PS_LOG(PS_LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define PS_LOG_INFO(fmt, ...)  PS_LOG(PS_LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define PS_LOG_DEBUG(fmt, ...) PS_LOG(PS_LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

#endif /* PS_LOG_H */
