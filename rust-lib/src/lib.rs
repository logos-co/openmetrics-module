//! prometheus_metrics — a Logos module that serves a Prometheus `/metrics`
//! endpoint by querying a configured set of modules' `collectMetrics()` methods.
//!
//! It is a pure passthrough: you `start` it with a config JSON listing the
//! modules to query, it stands up an HTTP server, and on each scrape it fans
//! out concurrently to those modules, gathers their `LogosMap` metric payloads,
//! and renders Prometheus exposition text. It does not discover loaded modules
//! or touch any platform/core API.
//!
//! The Qt plugin glue is auto-generated from `include/prometheus_metrics.h` by
//! `logos-cpp-generator --from-c-header`. The codegen strips the
//! `prometheus_metrics_` prefix, so these `extern "C"` functions become the
//! module methods `start`, `stop`, and `get_info`.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

mod collector;
mod formatter;
mod server;
mod state;

/// Start the HTTP server. `config_json` is `{"port": <u16>, "modules": [<name>...]}`.
/// Returns 1 on success, 0 on failure (bad config / already running / bind error).
#[no_mangle]
pub extern "C" fn prometheus_metrics_start(config_json: *const c_char) -> i64 {
    let config = if config_json.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(config_json) }
            .to_string_lossy()
            .into_owned()
    };

    match state::start(&config) {
        Ok(()) => 1,
        Err(e) => {
            eprintln!("prometheus_metrics: start failed: {e}");
            0
        }
    }
}

/// Stop the HTTP server. Returns 1 if it was running and is now stopped, 0 if
/// it was not running.
#[no_mangle]
pub extern "C" fn prometheus_metrics_stop() -> i64 {
    if state::stop() {
        1
    } else {
        0
    }
}

/// Return a JSON string describing current state:
/// `{"running": bool, "port": <int>, "modules": [...]}`.
/// The caller must free the returned pointer with `prometheus_metrics_free_string`.
#[no_mangle]
pub extern "C" fn prometheus_metrics_get_info() -> *mut c_char {
    let info = state::info();
    CString::new(info)
        .unwrap_or_else(|_| CString::new("{}").unwrap())
        .into_raw()
}

/// Free a string previously returned by `prometheus_metrics_get_info`.
#[no_mangle]
pub extern "C" fn prometheus_metrics_free_string(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        drop(CString::from_raw(ptr));
    }
}

/// Test-only stubs for the `logos_sdk_*` symbols that `logos-rust-sdk` imports
/// from `liblogos_module_client`. In a real build those are resolved at the
/// final plugin link; under `cargo test` the harness links the whole crate, so
/// we define no-ops here. None of the unit tests exercise IPC.
#[cfg(test)]
mod test_stubs {
    use std::os::raw::{c_char, c_int, c_void};

    type LogosSdkCallback = extern "C" fn(c_int, *const c_char, *mut c_void);

    #[no_mangle]
    extern "C" fn logos_sdk_call_method_sync(
        _plugin: *const c_char,
        _method: *const c_char,
        _params: *const c_char,
    ) -> *mut c_char {
        std::ptr::null_mut()
    }

    #[no_mangle]
    extern "C" fn logos_sdk_free_string(_s: *mut c_char) {}

    #[no_mangle]
    extern "C" fn logos_sdk_call_method_async(
        _plugin: *const c_char,
        _method: *const c_char,
        _params: *const c_char,
        _callback: LogosSdkCallback,
        _user_data: *mut c_void,
    ) {
    }

    #[no_mangle]
    extern "C" fn logos_sdk_register_event(
        _plugin: *const c_char,
        _event: *const c_char,
        _callback: LogosSdkCallback,
        _user_data: *mut c_void,
    ) {
    }

    #[no_mangle]
    extern "C" fn logos_sdk_shutdown() {}
}
