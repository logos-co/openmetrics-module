//! Render collected module metrics as Prometheus exposition text (v0.0.4).
//!
//! Metrics are dynamic — defined by other modules at runtime — so we format the
//! text directly rather than going through a statically-typed registry. Samples
//! are grouped by metric name into families so each `# HELP` / `# TYPE` is
//! emitted once, with all samples of that family contiguous (as Prometheus
//! expects). Every sample gets a `module="<name>"` label identifying its source.

use std::collections::BTreeMap;

use serde_json::Value;

use crate::collector::ModuleMetrics;

struct Family {
    mtype: String,
    help: String,
    samples: Vec<String>,
}

/// Convert the collected metrics into a Prometheus exposition payload.
pub fn to_prometheus_text(collected: &[ModuleMetrics]) -> String {
    let mut families: BTreeMap<String, Family> = BTreeMap::new();

    for module_metrics in collected {
        let metrics = match module_metrics
            .payload
            .get("metrics")
            .and_then(Value::as_array)
        {
            Some(arr) => arr,
            None => continue,
        };

        for metric in metrics {
            let name = match metric.get("name").and_then(Value::as_str) {
                Some(n) if !n.is_empty() => n,
                _ => continue,
            };
            let value = match render_value(metric.get("value")) {
                Some(v) => v,
                None => continue,
            };
            let mtype = sanitize_type(metric.get("type").and_then(Value::as_str));
            let help = metric
                .get("help")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();

            let labels = render_labels(&module_metrics.module, metric.get("labels"));
            let sample = format!("{name}{{{labels}}} {value}");

            let family = families.entry(name.to_string()).or_insert_with(|| Family {
                mtype,
                help,
                samples: Vec::new(),
            });
            family.samples.push(sample);
        }
    }

    let mut out = String::new();
    for (name, family) in &families {
        if !family.help.is_empty() {
            out.push_str(&format!("# HELP {name} {}\n", escape_help(&family.help)));
        }
        out.push_str(&format!("# TYPE {name} {}\n", family.mtype));
        for sample in &family.samples {
            out.push_str(sample);
            out.push('\n');
        }
    }
    out
}

/// Build the label set, always leading with `module="<name>"`, then any
/// string-valued labels the module supplied.
fn render_labels(module: &str, labels: Option<&Value>) -> String {
    let mut parts = vec![format!("module=\"{}\"", escape_label(module))];
    if let Some(obj) = labels.and_then(Value::as_object) {
        for (key, value) in obj {
            let v = match value {
                Value::String(s) => s.clone(),
                other => other.to_string(),
            };
            parts.push(format!("{}=\"{}\"", key, escape_label(&v)));
        }
    }
    parts.join(",")
}

/// Render a metric value as a Prometheus number. Bools map to 1/0; numeric
/// strings pass through. Anything else is rejected (the sample is skipped).
fn render_value(value: Option<&Value>) -> Option<String> {
    match value {
        Some(Value::Number(n)) => Some(n.to_string()),
        Some(Value::Bool(b)) => Some(if *b { "1".into() } else { "0".into() }),
        Some(Value::String(s)) if s.parse::<f64>().is_ok() => Some(s.clone()),
        _ => None,
    }
}

/// Restrict the type to the Prometheus-known set; unknown/missing → "untyped".
fn sanitize_type(mtype: Option<&str>) -> String {
    match mtype {
        Some("counter") => "counter",
        Some("gauge") => "gauge",
        Some("histogram") => "histogram",
        Some("summary") => "summary",
        _ => "untyped",
    }
    .to_string()
}

/// Escape a `# HELP` description: backslash and newline only.
fn escape_help(s: &str) -> String {
    s.replace('\\', "\\\\").replace('\n', "\\n")
}

/// Escape a label value: backslash, double-quote, and newline.
fn escape_label(s: &str) -> String {
    s.replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('\n', "\\n")
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn mm(module: &str, payload: serde_json::Value) -> ModuleMetrics {
        ModuleMetrics {
            module: module.to_string(),
            payload,
        }
    }

    #[test]
    fn renders_counter_and_gauge_with_module_label() {
        let collected = vec![mm(
            "storage_module",
            json!({"metrics": [
                {"name": "storage_blocks_total", "type": "counter", "help": "Total blocks stored", "value": 42},
                {"name": "storage_peers_connected", "type": "gauge", "help": "Connected peers", "value": 7, "labels": {"protocol": "libp2p"}}
            ]}),
        )];

        let text = to_prometheus_text(&collected);

        assert!(text.contains("# HELP storage_blocks_total Total blocks stored\n"));
        assert!(text.contains("# TYPE storage_blocks_total counter\n"));
        assert!(text.contains("storage_blocks_total{module=\"storage_module\"} 42\n"));
        assert!(text.contains("# TYPE storage_peers_connected gauge\n"));
        assert!(text.contains(
            "storage_peers_connected{module=\"storage_module\",protocol=\"libp2p\"} 7\n"
        ));
    }

    #[test]
    fn groups_same_metric_from_multiple_modules_under_one_family() {
        let collected = vec![
            mm(
                "node_a",
                json!({"metrics": [{"name": "up", "type": "gauge", "help": "up", "value": 1}]}),
            ),
            mm(
                "node_b",
                json!({"metrics": [{"name": "up", "type": "gauge", "help": "up", "value": 1}]}),
            ),
        ];

        let text = to_prometheus_text(&collected);

        // HELP/TYPE emitted exactly once for the shared family.
        assert_eq!(text.matches("# TYPE up gauge").count(), 1);
        assert_eq!(text.matches("# HELP up up").count(), 1);
        assert!(text.contains("up{module=\"node_a\"} 1\n"));
        assert!(text.contains("up{module=\"node_b\"} 1\n"));
    }

    #[test]
    fn skips_invalid_entries_but_keeps_valid_ones() {
        let collected = vec![mm(
            "m",
            json!({"metrics": [
                {"type": "counter", "value": 1},                 // missing name → skipped
                {"name": "no_value", "type": "counter"},          // missing value → skipped
                {"name": "good", "type": "counter", "value": 5}   // kept
            ]}),
        )];

        let text = to_prometheus_text(&collected);

        assert!(text.contains("good{module=\"m\"} 5\n"));
        assert!(!text.contains("no_value"));
    }

    #[test]
    fn unknown_type_becomes_untyped_and_bool_maps_to_number() {
        let collected = vec![mm(
            "m",
            json!({"metrics": [{"name": "flag", "type": "weird", "value": true}]}),
        )];

        let text = to_prometheus_text(&collected);

        assert!(text.contains("# TYPE flag untyped\n"));
        assert!(text.contains("flag{module=\"m\"} 1\n"));
    }

    #[test]
    fn escapes_label_values() {
        let collected = vec![mm(
            "m",
            json!({"metrics": [{"name": "x", "type": "gauge", "value": 1, "labels": {"path": "a\"b\\c"}}]}),
        )];

        let text = to_prometheus_text(&collected);

        assert!(text.contains("path=\"a\\\"b\\\\c\""));
    }

    #[test]
    fn empty_input_yields_empty_output() {
        assert_eq!(to_prometheus_text(&[]), "");
    }
}
