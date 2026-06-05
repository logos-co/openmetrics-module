#ifndef METRICS_DEMO_H
#define METRICS_DEMO_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The c-ffi code generator strips the `metrics_demo_` prefix, so
 * `metrics_demo_collectMetrics` becomes the module method `collectMetrics`.
 * `metrics_demo_free_string` is recognized as the string-freeing helper.
 */

/**
 * Return a {"metrics": [...]} payload of prometheus-like fields.
 * The returned C string must be freed with metrics_demo_free_string().
 */
char* metrics_demo_collectMetrics(void);

/** Free a string returned by metrics_demo_collectMetrics(). */
void metrics_demo_free_string(char* ptr);

#ifdef __cplusplus
}
#endif

#endif /* METRICS_DEMO_H */
