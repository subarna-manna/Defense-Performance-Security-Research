// security/zero_trust/device_attestation.rs
// TPM 2.0 Remote Attestation (Challenge/Response model)
// Depends on: tss-esapi (~7.x), base64, sha2, serde (for serialization)

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use tss_esapi::{
    Context, Tcti,
    structures::{PcrSelectionList, PcrSelection, HashingAlgorithm, PcrHandle},
    tss2_esys::{ESYS_TR, TPM2B_NONCE, TPMT_SIGNATURE, TPMT_PUBLIC},
    utils::{self, PublicKey},
    Error as TssError,
};
use sha2::{Sha256, Digest};
use base64::{engine::general_purpose, Engine as _};
use serde::{Serialize, Deserialize};

lazy_static::lazy_static! {
    static ref TRUSTED_PCRS: Arc<Mutex<HashMap<u32, String>>> = Arc::new(Mutex::new(HashMap::new()));
    // In production: golden PCR values per firmware/OS version, loaded from secure config
}

#[derive(Serialize, Deserialize)]
pub struct AttestationEvidence {
    pub device_id: String,
    pub nonce: String,                // base64 challenge from verifier
    pub quote: String,                // base64 TPM Quote
    pub signature: String,            // base64 TPMT_SIGNATURE
    pub ak_pub: String,               // base64 TPMT_PUBLIC (Attestation Key)
    pub pcr_values: HashMap<u32, String>, // selected PCRs (e.g. 0-7 boot chain)
}

/// Called once at verifier startup (or per-policy load)
pub fn load_golden_pcrs(golden: HashMap<u32, String>) {
    let mut map = TRUSTED_PCRS.lock().unwrap();
    *map = golden;
}

/// Verifier side: Validate TPM attestation evidence
/// Returns Ok(()) if valid, Err(reason) otherwise
pub fn verify_attestation(
    evidence: &AttestationEvidence,
    expected_ak_pub: Option<&str>,   // optional: known AK public for device
) -> Result<(), String> {
    // 1. Decode inputs
    let nonce_bytes = general_purpose::STANDARD.decode(&evidence.nonce)
        .map_err(|e| format!("Invalid nonce: {}", e))?;
    let quote_bytes = general_purpose::STANDARD.decode(&evidence.quote)
        .map_err(|e| format!("Invalid quote: {}", e))?;
    let sig_bytes = general_purpose::STANDARD.decode(&evidence.signature)
        .map_err(|e| format!("Invalid signature: {}", e))?;
    let ak_pub_bytes = general_purpose::STANDARD.decode(&evidence.ak_pub)
        .map_err(|e| format!("Invalid AK pub: {}", e))?;

    // 2. Reconstruct TPM structures (simplified; real code uses tss-esapi parsers)
    let nonce: TPM2B_NONCE = utils::Marshal::from_bytes(&nonce_bytes)
        .map_err(|e| format!("Nonce parse error: {:?}", e))?;
    // ... similarly for quote, sig, ak_pub ...

    // 3. Verify quote signature using AK public key
    // (use tss-esapi Context::verify_signature or crypto lib)
    // Example placeholder:
    // if !verify_quote_signature(&ak_pub, &quote_bytes, &sig_bytes) { return Err("Signature invalid"); }

    // 4. Check quote includes the nonce (freshness)
    // Parse quote → check extraData field matches nonce hash

    // 5. Extract & verify PCRs against golden
    let mut hasher = Sha256::new();
    for (pcr_idx, value_b64) in &evidence.pcr_values {
        let value = general_purpose::STANDARD.decode(value_b64)
            .map_err(|e| format!("Invalid PCR {}: {}", pcr_idx, e))?;
        let expected = TRUSTED_PCRS.lock().unwrap().get(pcr_idx)
            .ok_or(format!("No golden value for PCR {}", pcr_idx))?;
        let expected_bytes = hex::decode(expected).map_err(|e| e.to_string())?;

        if value != expected_bytes {
            return Err(format!("PCR {} mismatch", pcr_idx));
        }
        hasher.update(&value);
    }

    // 6. Optional: check AK pub matches expected (for known devices)
    if let Some(expected) = expected_ak_pub {
        if *expected != evidence.ak_pub {
            return Err("AK public key mismatch".to_string());
        }
    }

    Ok(())
}

/// Attester side example (on device): Generate attestation evidence
/// This would run in a daemon/service on the endpoint device
pub fn generate_attestation(
    device_id: &str,
    nonce_b64: &str,           // received from verifier
    tcti_name: &str,           // e.g. "device:/dev/tpmrm0" or "mssim:port=2321"
) -> Result<AttestationEvidence, TssError> {
    let tcti = Tcti::from_name(tcti_name)?;
    let mut ctx = Context::new(tcti)?;

    let nonce_bytes = general_purpose::STANDARD.decode(nonce_b64)?;
    let nonce: TPM2B_NONCE = utils::Marshal::from_bytes(&nonce_bytes)?;

    // Assume AK handle already created/persisted (EK → AK cert chain done earlier)
    let ak_handle: ESYS_TR = ctx.tr_from_name("persistent:0x81000001")?; // example

    // Select PCRs to quote (e.g. boot chain PCRs 0-7, SHA256)
    let pcr_sel = PcrSelectionList::new()
        .with_selection(PcrSelection::new(HashingAlgorithm::Sha256, &[0,1,2,3,4,5,6,7])?);

    // Get quote
    let (quote, sig) = ctx.quote(
        ak_handle,
        &nonce,
        &pcr_sel,
        None, // no qualifying data beyond nonce
    )?;

    // Read current PCR values
    let mut pcr_values = HashMap::new();
    for i in 0..=7 {
        let val = ctx.pcr_read(&PcrHandle::new(i as u32)?)?;
        pcr_values.insert(i, general_purpose::STANDARD.encode(val.value()));
    }

    // Get AK public (for verifier to validate sig)
    let ak_pub = ctx.read_public(ak_handle)?.public_area;

    Ok(AttestationEvidence {
        device_id: device_id.to_string(),
        nonce: nonce_b64.to_string(),
        quote: general_purpose::STANDARD.encode(quote.to_bytes()?),
        signature: general_purpose::STANDARD.encode(sig.to_bytes()?),
        ak_pub: general_purpose::STANDARD.encode(ak_pub.to_bytes()?),
        pcr_values,
    })
}






// device_attestation.rs - Enrollment extensions

use tss_esapi::{
    Context, Tcti,
    structures::{Auth, Public, Template, PcrSelectionList, HashingAlgorithm},
    tss2_esys::{ESYS_TR, TPM2B_PUBLIC, TPM2B_PRIVATE},
    utils::{self, PublicKey},
    enums::{Hierarchy, SessionType},
    Error as TssError,
};
use base64::Engine;
use std::path::Path;

// Assume TCTI from config (e.g., "device:/dev/tpmrm0")
fn get_context(tcti_name: &str) -> Result<Context, TssError> {
    let tcti = Tcti::from_name(tcti_name)?;
    Ok(Context::new(tcti)?)
}

/// Step 1-2: Load/Create EK and get pub/cert
pub fn get_or_create_ek(tcti_name: &str) -> Result<(ESYS_TR, String, String), TssError> {  // handle, ek_pub_b64, ek_cert_b64
    let mut ctx = get_context(tcti_name)?;

    // Load persistent EK (common factory handle)
    let ek_handle = ctx.tr_from_name("persistent:0x81010001")?;  // or create if not

    // If create needed:
    // let ek_handle = ctx.create_primary(Hierarchy::Endorsement, &Auth::Empty, &Template::rsa2048_ek(), None, None)?.key_handle;

    let ek_pub = ctx.read_public(ek_handle)?;
    let ek_pub_bytes = ek_pub.public_area.to_bytes()?;  // or marshal
    let ek_pub_b64 = base64::engine::general_purpose::STANDARD.encode(ek_pub_bytes);

    // Retrieve EK cert (from NV index 0x1c00002 or manufacturer)
    // Example: Read NV (simplified; use actual size/index from TPM)
    let ek_cert_bytes = ctx.nv_read(Hierarchy::Owner, 0x1c00002, 1024)?;  // adjust index/size
    let ek_cert_b64 = base64::engine::general_purpose::STANDARD.encode(ek_cert_bytes);

    Ok((ek_handle, ek_pub_b64, ek_cert_b64))
}

/// Step 6: Create AK under EK
pub fn create_ak(
    ctx: &mut Context,
    ek_handle: ESYS_TR,
) -> Result<(ESYS_TR, String), TssError> {
    // Restricted signing template (RSA2048, SHA256)
    let ak_template = Template::new()
        .with_public_area(Public::rsa_restricted_signing_sha256());

    let (ak_private, ak_public) = ctx.create(
        ek_handle,
        &Auth::Empty,
        &ak_template,
        None,
        None,
    )?;

    // Make persistent (optional)
    // ctx.evict_control(Hierarchy::Owner, ak_handle, 0x81000002)?; // example handle

    let ak_pub_bytes = ak_public.public_area.to_bytes()?;
    let ak_pub_b64 = base64::engine::general_purpose::STANDARD.encode(ak_pub_bytes);

    Ok((ak_handle, ak_pub_b64))
}

/// Step 7: Activate Credential (unwrap secret blob)
pub fn activate_credential(
    ctx: &mut Context,
    ak_handle: ESYS_TR,
    ek_handle: ESYS_TR,
    credential_blob_b64: &str,
) -> Result<String, TssError> {
    let blob_bytes = base64::engine::general_purpose::STANDARD.decode(credential_blob_b64)?;

    // Parse TPM2B to structs (use utils or manual)
    // Simplified: Assume blob is TPM2B_ATTEST or credential blob
    let (cert_info, secret) = ctx.activate_credential(
        ak_handle,
        ek_handle,
        &blob_bytes.into(),  // TPM2B_CREDENTIAL_BLOB
        &TPM2B_PUBLIC::default(),  // from public
    )?;

    // secret is decrypted value - prove by hashing/sending back
    let proof = hex::encode(secret.value());  // or base64

    Ok(proof)
}

// // || ------------ Cloud Side (Registrar - Pseudocode) ------------ || 

// // In Rust web service (e.g., axum)
// async fn enroll_init(body: EnrollRequest) -> Result<EnrollChallenge> {
//     // Verify ek_cert chain (use rustls or openssl crate)
//     // Store ek_pub, ek_cert for device_id
//     let secret = rand::random::<[u8; 32]>();  // ephemeral
//     let credential_blob = make_credential(&ek_pub, &secret, &ak_name_placeholder);  // impl with TPM2_MakeCredential logic or lib
//     Ok(EnrollChallenge { credential_blob: base64::encode(&credential_blob), registration_token: "..." })
// }

// async fn enroll_complete(body: CompleteRequest) -> Result<()> {
//     // Verify proof == hash(secret)
//     // Store ak_pub as trusted for device
//     Ok(())
// }







// // security/zero_trust/device_attestation.rs
// // Zero Trust Device Attestation
// // Ensures device firmware integrity before granting network access
// // Uses a simple hash-based check against a trusted registry (expandable to TPM/SPDM/RA-TLS)

// use std::collections::HashMap;
// use std::sync::{Arc, Mutex};
// use lazy_static::lazy_static;

// lazy_static! {
//     // In production: Load from secure config / HSM-backed store at startup
//     static ref TRUSTED_DEVICE_HASHES: Arc<Mutex<HashMap<String, String>>> = Arc::new(Mutex::new(HashMap::new()));
// }

// /// Initialize trusted device registry (called once at service startup)
// pub fn init_trusted_devices(trusted_list: HashMap<String, String>) {
//     let mut registry = TRUSTED_DEVICE_HASHES.lock().unwrap();
//     *registry = trusted_list;
//     // Example: registry.insert("device-001".to_string(), "sha256:abc123...".to_string());
// }

// /// Attest a device by ID and provided firmware hash
// /// Returns true if the device is trusted (hash matches expected)
// pub fn attest(device_id: &str, provided_hash: &str) -> bool {
//     let registry = TRUSTED_DEVICE_HASHES.lock().unwrap();

//     if let Some(expected_hash) = registry.get(device_id) {
//         if expected_hash == provided_hash {
//             println!("Attestation success for device: {}", device_id);
//             return true;
//         } else {
//             println!("Attestation failed: hash mismatch for {}", device_id);
//             return false;
//         }
//     }

//     println!("Attestation failed: unknown device {}", device_id);
//     false
// }

// /// Add a new trusted device (admin-only, secured endpoint in production)
// pub fn register_device(device_id: String, firmware_hash: String) {
//     let mut registry = TRUSTED_DEVICE_HASHES.lock().unwrap();
//     registry.insert(device_id, firmware_hash);
// }





















// // security/zero_trust/device_attestation.rs
// // TPM 2.0 Remote Attestation (Challenge/Response model)
// // Depends on: tss-esapi (~7.x), base64, sha2, serde (for serialization)

// use std::collections::HashMap;
// use std::sync::{Arc, Mutex};
// use tss_esapi::{
//     Context, Tcti,
//     structures::{PcrSelectionList, PcrSelection, HashingAlgorithm, PcrHandle},
//     tss2_esys::{ESYS_TR, TPM2B_NONCE, TPMT_SIGNATURE, TPMT_PUBLIC},
//     utils::{self, PublicKey},
//     Error as TssError,
// };
// use sha2::{Sha256, Digest};
// use base64::{engine::general_purpose, Engine as _};
// use serde::{Serialize, Deserialize};

// lazy_static::lazy_static! {
//     static ref TRUSTED_PCRS: Arc<Mutex<HashMap<u32, String>>> = Arc::new(Mutex::new(HashMap::new()));
//     // In production: golden PCR values per firmware/OS version, loaded from secure config
// }

// #[derive(Serialize, Deserialize)]
// pub struct AttestationEvidence {
//     pub device_id: String,
//     pub nonce: String,                // base64 challenge from verifier
//     pub quote: String,                // base64 TPM Quote
//     pub signature: String,            // base64 TPMT_SIGNATURE
//     pub ak_pub: String,               // base64 TPMT_PUBLIC (Attestation Key)
//     pub pcr_values: HashMap<u32, String>, // selected PCRs (e.g. 0-7 boot chain)
// }

// /// Called once at verifier startup (or per-policy load)
// pub fn load_golden_pcrs(golden: HashMap<u32, String>) {
//     let mut map = TRUSTED_PCRS.lock().unwrap();
//     *map = golden;
// }

// /// Verifier side: Validate TPM attestation evidence
// /// Returns Ok(()) if valid, Err(reason) otherwise
// pub fn verify_attestation(
//     evidence: &AttestationEvidence,
//     expected_ak_pub: Option<&str>,   // optional: known AK public for device
// ) -> Result<(), String> {
//     // 1. Decode inputs
//     let nonce_bytes = general_purpose::STANDARD.decode(&evidence.nonce)
//         .map_err(|e| format!("Invalid nonce: {}", e))?;
//     let quote_bytes = general_purpose::STANDARD.decode(&evidence.quote)
//         .map_err(|e| format!("Invalid quote: {}", e))?;
//     let sig_bytes = general_purpose::STANDARD.decode(&evidence.signature)
//         .map_err(|e| format!("Invalid signature: {}", e))?;
//     let ak_pub_bytes = general_purpose::STANDARD.decode(&evidence.ak_pub)
//         .map_err(|e| format!("Invalid AK pub: {}", e))?;

//     // 2. Reconstruct TPM structures (simplified; real code uses tss-esapi parsers)
//     let nonce: TPM2B_NONCE = utils::Marshal::from_bytes(&nonce_bytes)
//         .map_err(|e| format!("Nonce parse error: {:?}", e))?;
//     // ... similarly for quote, sig, ak_pub ...

//     // 3. Verify quote signature using AK public key
//     // (use tss-esapi Context::verify_signature or crypto lib)
//     // Example placeholder:
//     // if !verify_quote_signature(&ak_pub, &quote_bytes, &sig_bytes) { return Err("Signature invalid"); }

//     // 4. Check quote includes the nonce (freshness)
//     // Parse quote → check extraData field matches nonce hash

//     // 5. Extract & verify PCRs against golden
//     let mut hasher = Sha256::new();
//     for (pcr_idx, value_b64) in &evidence.pcr_values {
//         let value = general_purpose::STANDARD.decode(value_b64)
//             .map_err(|e| format!("Invalid PCR {}: {}", pcr_idx, e))?;
//         let expected = TRUSTED_PCRS.lock().unwrap().get(pcr_idx)
//             .ok_or(format!("No golden value for PCR {}", pcr_idx))?;
//         let expected_bytes = hex::decode(expected).map_err(|e| e.to_string())?;

//         if value != expected_bytes {
//             return Err(format!("PCR {} mismatch", pcr_idx));
//         }
//         hasher.update(&value);
//     }

//     // 6. Optional: check AK pub matches expected (for known devices)
//     if let Some(expected) = expected_ak_pub {
//         if *expected != evidence.ak_pub {
//             return Err("AK public key mismatch".to_string());
//         }
//     }

//     Ok(())
// }

// /// Attester side example (on device): Generate attestation evidence
// /// This would run in a daemon/service on the endpoint device
// pub fn generate_attestation(
//     device_id: &str,
//     nonce_b64: &str,           // received from verifier
//     tcti_name: &str,           // e.g. "device:/dev/tpmrm0" or "mssim:port=2321"
// ) -> Result<AttestationEvidence, TssError> {
//     let tcti = Tcti::from_name(tcti_name)?;
//     let mut ctx = Context::new(tcti)?;

//     let nonce_bytes = general_purpose::STANDARD.decode(nonce_b64)?;
//     let nonce: TPM2B_NONCE = utils::Marshal::from_bytes(&nonce_bytes)?;

//     // Assume AK handle already created/persisted (EK → AK cert chain done earlier)
//     let ak_handle: ESYS_TR = ctx.tr_from_name("persistent:0x81000001")?; // example

//     // Select PCRs to quote (e.g. boot chain PCRs 0-7, SHA256)
//     let pcr_sel = PcrSelectionList::new()
//         .with_selection(PcrSelection::new(HashingAlgorithm::Sha256, &[0,1,2,3,4,5,6,7])?);

//     // Get quote
//     let (quote, sig) = ctx.quote(
//         ak_handle,
//         &nonce,
//         &pcr_sel,
//         None, // no qualifying data beyond nonce
//     )?;

//     // Read current PCR values
//     let mut pcr_values = HashMap::new();
//     for i in 0..=7 {
//         let val = ctx.pcr_read(&PcrHandle::new(i as u32)?)?;
//         pcr_values.insert(i, general_purpose::STANDARD.encode(val.value()));
//     }

//     // Get AK public (for verifier to validate sig)
//     let ak_pub = ctx.read_public(ak_handle)?.public_area;

//     Ok(AttestationEvidence {
//         device_id: device_id.to_string(),
//         nonce: nonce_b64.to_string(),
//         quote: general_purpose::STANDARD.encode(quote.to_bytes()?),
//         signature: general_purpose::STANDARD.encode(sig.to_bytes()?),
//         ak_pub: general_purpose::STANDARD.encode(ak_pub.to_bytes()?),
//         pcr_values,
//     })
// }






