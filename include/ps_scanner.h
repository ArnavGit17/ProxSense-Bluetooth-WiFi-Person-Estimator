/**
 * @file ps_scanner.h
 * @brief Abstract scanner interface (vtable pattern).
 *
 * Every radio back-end (Bluetooth, Wi-Fi, or future technologies) implements
 * this interface.  The Observation Manager calls through this interface and
 * has no knowledge of BlueZ, nl80211, or any other concrete technology.
 *
 * This is a deliberate inversion of control: the estimator pipeline remains
 * stable regardless of which scan back-ends are available at runtime.
 *
 * Lifetime contract:
 *   1. Caller allocates a ps_scanner_t and fills it (or calls back-end's init).
 *   2. Caller calls ops->open() to initialise hardware/socket.
 *   3. Caller calls ops->read_next() in a loop to receive observations.
 *   4. Caller calls ops->close() to release all resources.
 *   5. The scanner does NOT free the ps_scanner_t itself (caller owns it).
 */

#ifndef PS_SCANNER_H
#define PS_SCANNER_H

#include "ps_types.h"
#include "ps_observation.h"
#include <stdbool.h>

/* Forward declaration — concrete back-ends define their own private context. */
typedef struct ps_scanner ps_scanner_t;

/* =========================================================================
 * Scanner operations vtable
 *
 * Every field is a function pointer.  A back-end that does not support a
 * given operation sets the pointer to NULL; callers must check before use.
 * ========================================================================= */
typedef struct ps_scanner_ops {
    /**
     * @brief Open and initialise the scanner.
     * @param scanner  The scanner instance.
     * @return PS_OK, PS_ERR_NO_HARDWARE, PS_ERR_PERMISSION, or PS_ERR_IO.
     *
     * On failure this function must leave no partial resources open.
     */
    ps_status_t (*open)(ps_scanner_t *scanner);

    /**
     * @brief Read the next observation, blocking for up to timeout_ms.
     * @param scanner     The scanner instance.
     * @param out         Caller-supplied observation to fill.
     * @param timeout_ms  Max time to wait in milliseconds; 0 = non-blocking.
     * @return PS_OK on new observation, PS_ERR_TIMEOUT if none arrived,
     *         PS_ERR_SCANNER_FAILED on unrecoverable error.
     *
     * On PS_ERR_TIMEOUT, *out is unchanged.
     */
    ps_status_t (*read_next)(ps_scanner_t *scanner,
                             ps_observation_t *out,
                             int timeout_ms);

    /**
     * @brief Close the scanner and free internal resources.
     * @param scanner  The scanner instance.
     *
     * Always succeeds; errors are logged internally.
     * After close() the scanner may not be used without another open().
     */
    void (*close)(ps_scanner_t *scanner);

    /**
     * @brief Return a human-readable description of this back-end.
     * Used in diagnostics and --help output.
     */
    const char *(*describe)(const ps_scanner_t *scanner);
} ps_scanner_ops_t;

/* =========================================================================
 * ps_scanner_t — the scanner instance handle
 *
 * The ops pointer is set by the back-end's init function.
 * The priv pointer is an opaque back-end private context (heap-allocated by
 * the back-end's init function and freed in close()).
 * ========================================================================= */
struct ps_scanner {
    const ps_scanner_ops_t *ops;   /**< vtable — set once during init, const thereafter */
    void                   *priv;  /**< back-end private state (owned by back-end)       */
    ps_source_t             source;/**< Which radio technology this scanner represents   */
    bool                    is_open;/**< True after open(), False after close()          */
};

/* =========================================================================
 * Convenience wrappers (null-check the vtable entries)
 * ========================================================================= */

static inline ps_status_t ps_scanner_open(ps_scanner_t *s)
{
    if (!s || !s->ops || !s->ops->open) return PS_ERR_INVALID_ARG;
    return s->ops->open(s);
}

static inline ps_status_t ps_scanner_read_next(ps_scanner_t *s,
                                                ps_observation_t *out,
                                                int timeout_ms)
{
    if (!s || !s->ops || !s->ops->read_next || !out) return PS_ERR_INVALID_ARG;
    return s->ops->read_next(s, out, timeout_ms);
}

static inline void ps_scanner_close(ps_scanner_t *s)
{
    if (s && s->ops && s->ops->close) s->ops->close(s);
}

static inline const char *ps_scanner_describe(const ps_scanner_t *s)
{
    if (!s || !s->ops || !s->ops->describe) return "(no description)";
    return s->ops->describe(s);
}

#endif /* PS_SCANNER_H */
