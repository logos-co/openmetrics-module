//! Global module state: the running HTTP server plus its configuration.
//!
//! The module is a `staticlib` whose entry points are free `extern "C"`
//! functions, so the server handle lives in a process-global mutex.

use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

use serde::Deserialize;
use tiny_http::Server;

use crate::server;

/// Configuration passed to `start`.
#[derive(Debug, Deserialize)]
struct Config {
    port: u16,
    #[serde(default)]
    modules: Vec<String>,
}

/// State held while the server is running.
struct Running {
    server: Arc<Server>,
    handle: Option<JoinHandle<()>>,
    port: u16,
    modules: Vec<String>,
}

static STATE: Mutex<Option<Running>> = Mutex::new(None);

/// Parse the config JSON and start the HTTP server on a background thread.
pub fn start(config_json: &str) -> Result<(), String> {
    let cfg: Config = serde_json::from_str(config_json)
        .map_err(|e| format!("invalid config json: {e}"))?;

    let mut guard = STATE.lock().map_err(|_| "state lock poisoned".to_string())?;
    if guard.is_some() {
        return Err("server already running".into());
    }

    let server = Server::http(("0.0.0.0", cfg.port))
        .map_err(|e| format!("failed to bind port {}: {e}", cfg.port))?;
    let server = Arc::new(server);

    let thread_server = Arc::clone(&server);
    let thread_modules = cfg.modules.clone();
    let handle = std::thread::Builder::new()
        .name("prometheus-metrics-http".into())
        .spawn(move || server::serve(thread_server, thread_modules))
        .map_err(|e| format!("failed to spawn http thread: {e}"))?;

    *guard = Some(Running {
        server,
        handle: Some(handle),
        port: cfg.port,
        modules: cfg.modules,
    });
    Ok(())
}

/// Stop the HTTP server and join its thread. Returns false if not running.
pub fn stop() -> bool {
    let mut guard = match STATE.lock() {
        Ok(g) => g,
        Err(_) => return false,
    };

    if let Some(mut running) = guard.take() {
        // Break out of `incoming_requests()` so the serve loop returns.
        running.server.unblock();
        if let Some(handle) = running.handle.take() {
            let _ = handle.join();
        }
        true
    } else {
        false
    }
}

/// JSON snapshot of the current state for `get_info`.
pub fn info() -> String {
    let guard = match STATE.lock() {
        Ok(g) => g,
        Err(_) => return "{\"running\":false,\"error\":\"state lock poisoned\"}".into(),
    };

    match &*guard {
        Some(r) => serde_json::json!({
            "running": true,
            "port": r.port,
            "modules": r.modules,
        })
        .to_string(),
        None => serde_json::json!({ "running": false }).to_string(),
    }
}

#[cfg(test)]
mod tests {
    use std::io::{Read, Write};
    use std::net::TcpStream;
    use std::time::Duration;

    #[test]
    fn server_starts_serves_health_and_stops() {
        let cfg = r#"{"port":19099,"modules":[]}"#;
        assert!(super::start(cfg).is_ok(), "start should succeed");

        // Give the accept loop a moment to bind.
        std::thread::sleep(Duration::from_millis(300));

        let mut stream = TcpStream::connect("127.0.0.1:19099").expect("connect");
        stream
            .write_all(b"GET /health HTTP/1.0\r\nHost: localhost\r\n\r\n")
            .expect("write");
        let mut resp = String::new();
        stream.read_to_string(&mut resp).expect("read");
        assert!(resp.contains("200"), "expected 200, got: {resp}");
        assert!(resp.contains("ok"), "expected body 'ok', got: {resp}");

        assert!(super::stop(), "stop should return true");
        assert!(!super::stop(), "second stop should return false");
    }
}
