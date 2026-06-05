//! The HTTP server: a `tiny_http` accept loop serving `/metrics` and `/health`.
//!
//! Runs on a background thread. `state::stop` calls `Server::unblock`, which
//! makes `incoming_requests()` terminate so this loop returns and the thread
//! joins cleanly.

use std::sync::Arc;

use tiny_http::{Header, Response, Server};

use crate::{collector, formatter};

/// Serve requests until the server is unblocked (via `state::stop`).
pub fn serve(server: Arc<Server>, modules: Vec<String>) {
    for request in server.incoming_requests() {
        match request.url() {
            "/metrics" => {
                let collected = collector::collect(&modules);
                let body = formatter::to_prometheus_text(&collected);
                let response = Response::from_string(body).with_header(content_type());
                let _ = request.respond(response);
            }
            "/health" => {
                let _ = request.respond(Response::from_string("ok\n"));
            }
            _ => {
                let _ = request.respond(Response::from_string("not found\n").with_status_code(404));
            }
        }
    }
}

/// `Content-Type` for the Prometheus text exposition format.
fn content_type() -> Header {
    Header::from_bytes(
        &b"Content-Type"[..],
        &b"text/plain; version=0.0.4; charset=utf-8"[..],
    )
    .expect("static content-type header is valid")
}
