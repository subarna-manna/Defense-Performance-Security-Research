// security/zero_trust/policy_engine.rs
// Policy Engine
// Enforces least-privilege access decisions based on loaded policies
// Simple rule-based (expandable to OPA/WASM integration)

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::Path;
use std::sync::{Arc, Mutex};

#[derive(Serialize, Deserialize, Debug)]
struct PolicyRule {
    subject: String,       // e.g., device_id, user_id
    action: String,        // e.g., "read", "write", "access"
    resource: String,      // e.g., "/api/data", "backend-service"
    effect: String,        // "allow" or "deny"
    conditions: Option<HashMap<String, String>>, // e.g., {"time": "day", "attested": "true"}
}

lazy_static::lazy_static! {
    static ref POLICIES: Arc<Mutex<Vec<PolicyRule>>> = Arc::new(Mutex::new(Vec::new()));
}

/// Load policies from JSON file (called at startup)
pub fn load_policies(policy_file: &Path) {
    let data = fs::read_to_string(policy_file).expect("Failed to read policy file");
    let rules: Vec<PolicyRule> = serde_json::from_str(&data).expect("Invalid policy JSON");
    let mut policies = POLICIES.lock().unwrap();
    *policies = rules;
    println!("Loaded {} policy rules", policies.len());
}

/// Evaluate if an action is allowed
/// Returns true if allowed by policy (deny-by-default)
pub fn is_allowed(
    subject: &str,
    action: &str,
    resource: &str,
    context: &HashMap<String, String>,
) -> bool {
    let policies = POLICIES.lock().unwrap();

    // Deny by default (zero-trust)
    for rule in policies.iter() {
        if rule.subject == subject || rule.subject == "*" {
            if rule.action == action || rule.action == "*" {
                if rule.resource == resource || rule.resource == "*" {
                    // Check conditions (simple key-value match)
                    let mut conditions_met = true;
                    if let Some(conds) = &rule.conditions {
                        for (k, v) in conds {
                            if context.get(k) != Some(v) {
                                conditions_met = false;
                                break;
                            }
                        }
                    }

                    if conditions_met {
                        return rule.effect == "allow";
                    }
                }
            }
        }
    }

    // Explicit deny or no match → deny
    false
}

// Example usage:
// let mut ctx = HashMap::new();
// ctx.insert("attested".to_string(), "true".to_string());
// is_allowed("device-001", "read", "/data/mission", &ctx);





// --------------- Integration & Usage Notes ---------------
// These Rust components are designed for zero-trust enforcement in your Defense Private Cloud:
// 1. Startup Sequence (e.g., in a Rust service or lib init):

// init_trusted_devices(/* from config or HSM */);
// load_policies(Path::new("/etc/defense_cloud/policies.json"));


// 2. Before granting access (e.g., in an API server or auth gateway)

// if !attest("device-001", "sha256:abc123...") {
//     // Reject connection
// }
// let mut ctx = HashMap::new();
// ctx.insert("attested".to_string(), "true".to_string());
// if !is_allowed("device-001", "access", "load-balancer", &ctx) {
//     // Deny
// }

