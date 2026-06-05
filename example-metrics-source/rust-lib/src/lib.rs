//! metrics_demo — a tiny example module that exposes `collectMetrics()` for the
//! `prometheus_metrics` scraper. Pure Rust, no dependencies.
//!
//! The c-ffi code generator strips the `metrics_demo_` prefix, so
//! `metrics_demo_collectMetrics` becomes the module method `collectMetrics`
//! (the exact name the scraper calls). The function name is intentionally
//! camelCase after the prefix to match that convention.

use std::ffi::CString;
use std::os::raw::c_char;
use std::sync::atomic::{AtomicU64, Ordering};

/// Counts how many times we've been scraped (demonstrates a counter).
static SCRAPES: AtomicU64 = AtomicU64::new(0);

/// Return a `{"metrics": [...]}` payload with prometheus-like fields.
/// The returned C string must be freed with `metrics_demo_free_string`.
#[no_mangle]
#[allow(non_snake_case)]
pub extern "C" fn metrics_demo_collectMetrics() -> *mut c_char {
    let scrapes = SCRAPES.fetch_add(1, Ordering::Relaxed) + 1;

    let payload = format!(
        concat!(
            "{{\"metrics\":[",
            "{{\"name\":\"demo_scrapes_total\",\"type\":\"counter\",",
            "\"help\":\"Times collectMetrics was called\",\"value\":{scrapes}}},",
            "{{\"name\":\"demo_temperature_celsius\",\"type\":\"gauge\",",
            "\"help\":\"A fake temperature reading\",\"value\":21,",
            "\"labels\":{{\"sensor\":\"core\"}}}}",
            "]}}"
        ),
        scrapes = scrapes
    );

    CString::new(payload)
        .unwrap_or_else(|_| CString::new("{\"metrics\":[]}").unwrap())
        .into_raw()
}

/// Free a string returned by `metrics_demo_collectMetrics`.
#[no_mangle]
pub extern "C" fn metrics_demo_free_string(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        drop(CString::from_raw(ptr));
    }
}
