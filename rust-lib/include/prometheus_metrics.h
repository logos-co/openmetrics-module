#ifndef PROMETHEUS_METRICS_H
#define PROMETHEUS_METRICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The logos c-ffi code generator derives module method names by stripping the
 * `prometheus_metrics_` prefix from these functions, so they become the module
 * methods `start`, `stop`, and `get_info`. `prometheus_metrics_free_string` is
 * recognized as the string-freeing helper and is not exposed as a method.
 */

/**
 * Start the HTTP server.
 * @param config_json  JSON: {"port": <u16>, "modules": ["<name>", ...]}
 * @return 1 on success, 0 on failure (bad config / already running / bind error).
 */
int64_t prometheus_metrics_start(const char* config_json);

/**
 * Stop the HTTP server.
 * @return 1 if it was running and is now stopped, 0 if it was not running.
 */
int64_t prometheus_metrics_stop(void);

/**
 * Current state as JSON: {"running": bool, "port": <int>, "modules": [...]}.
 * Returns a heap-allocated C string that must be freed with
 * prometheus_metrics_free_string().
 */
char* prometheus_metrics_get_info(void);

/** Free a string returned by prometheus_metrics_get_info(). */
void prometheus_metrics_free_string(char* ptr);

#ifdef __cplusplus
}
#endif

#endif /* PROMETHEUS_METRICS_H */
