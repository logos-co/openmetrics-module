//! Collect metrics from the configured modules over Logos IPC.
//!
//! Fan out to all modules **concurrently**: dispatch every `collectMetrics`
//! call first (async, non-blocking), then gather the results. Total scrape
//! latency is the slowest single module, not the sum.

use std::time::Duration;

use logos_rust_sdk::LogosModuleSDK;
use serde_json::Value;

/// Per-scrape timeout for a single module's `collectMetrics` reply.
const CALL_TIMEOUT: Duration = Duration::from_secs(5);

/// The metrics method every scrapeable module implements.
const METRICS_METHOD: &str = "collectMetrics";

/// A module's parsed metrics payload (`{"metrics": [...]}`).
pub struct ModuleMetrics {
    pub module: String,
    pub payload: Value,
}

/// Query every configured module's `collectMetrics()` and return the parsed
/// payloads. Modules that don't implement the method (or error / time out) are
/// skipped silently so one bad module never breaks a scrape.
pub fn collect(modules: &[String]) -> Vec<ModuleMetrics> {
    let sdk = LogosModuleSDK::new();

    // 1. Dispatch every call up front; each returns a receiver without blocking.
    let pending: Vec<(String, _)> = modules
        .iter()
        .filter_map(|name| {
            let proxy = sdk.plugin(name);
            proxy
                .call_no_params(METRICS_METHOD)
                .ok()
                .map(|rx| (name.clone(), rx))
        })
        .collect();

    // 2. Gather as the module-process event loop services each call.
    pending
        .into_iter()
        .filter_map(|(name, rx)| match rx.recv_timeout(CALL_TIMEOUT) {
            Ok(result) if result.success => serde_json::from_str::<Value>(&result.message)
                .ok()
                .map(|payload| ModuleMetrics {
                    module: name,
                    payload,
                }),
            _ => None,
        })
        .collect()
}
