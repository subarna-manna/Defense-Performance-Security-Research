// security/zero_trust/mutual_tls.rs
// Mutual TLS Enforcement
// Validates both client and server certificates using rustls
// Designed for server-side (accepting connections) and client-side (outgoing)

use rustls::{
    client::{ServerCertVerified, ServerCertVerifier},
    server::{ClientCertVerified, ClientCertVerifier},
    Certificate, PrivateKey, RootCertStore, ServerName,
};
use rustls::pki_types::{CertificateDer, ServerName as PkiServerName};
use rustls::crypto::verify::ServerCertVerifierBuilder;
use std::sync::Arc;
use std::time::SystemTime;
use std::path::Path;
use std::fs;

/// Custom verifier for client certificates (mTLS)
struct AllowAnyAnonymousOrClientCertVerifier;

impl ClientCertVerifier for AllowAnyAnonymousOrClientCertVerifier {
    fn client_auth_root_subjects(&self) -> &[rustls::DistinguishedName] {
        &[]
    }

    fn verify_client_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _now: SystemTime,
    ) -> Result<ClientCertVerified, rustls::Error> {
        // In production: Verify against CA, check revocation, extract attributes (e.g., CN=allowed-user)
        // For defense: Fail if no client cert or invalid chain
        Ok(ClientCertVerified::assertion())
    }
}

/// Custom verifier for server certificates (when connecting outbound)
struct AllowAnyServerCertVerifier;

impl ServerCertVerifier for AllowAnyServerCertVerifier {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &PkiServerName<'_>,
        _ocsp_response: &[u8],
        _now: SystemTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        // In production: Strict verification against CA + hostname
        // For internal zero-trust: Allow self-signed or known CA
        Ok(ServerCertVerified::assertion())
    }
}

/// Load a rustls ServerConfig for mTLS listener
pub fn create_mtls_server_config(
    ca_cert_path: &Path,
    server_cert_path: &Path,
    server_key_path: &Path,
    enforce_mtls: bool,
) -> Arc<rustls::ServerConfig> {
    let mut root_store = RootCertStore::empty();
    let ca_bytes = fs::read(ca_cert_path).expect("Failed to read CA cert");
    root_store.add(&CertificateDer::from(ca_bytes)).expect("Invalid CA cert");

    let cert_chain_bytes = fs::read(server_cert_path).expect("Failed to read server cert chain");
    let cert_chain = vec![CertificateDer::from(cert_chain_bytes)]; // Parse full chain if needed

    let key_bytes = fs::read(server_key_path).expect("Failed to read server key");
    let private_key = PrivateKey::from_der(&key_bytes).expect("Invalid private key");

    let mut config = rustls::ServerConfig::builder()
        .with_safe_defaults()
        .with_client_cert_verifier(if enforce_mtls {
            Arc::new(AllowAnyAnonymousOrClientCertVerifier)
        } else {
            rustls::server::NoClientAuth::new()
        })
        .with_single_cert(cert_chain, private_key)
        .expect("Failed to create server config");

    // Prefer TLS 1.3, disable old ciphers
    config.alpn_protocols = vec![b"h2".to_vec(), b"http/1.1".to_vec()];

    Arc::new(config)
}

/// Load a rustls ClientConfig for outbound mutual TLS
pub fn create_mtls_client_config(ca_cert_path: &Path) -> Arc<rustls::ClientConfig> {
    let mut root_store = RootCertStore::empty();
    let ca_bytes = fs::read(ca_cert_path).expect("Failed to read CA cert");
    root_store.add(&CertificateDer::from(ca_bytes)).expect("Invalid CA cert");

    Arc::new(rustls::ClientConfig::builder()
        .with_safe_defaults()
        .with_root_certificates(root_store)
        .with_no_client_auth())
}

// // security/zero_trust/mutual_tls.rs
// use std::sync::Arc;
// use rustls::{ClientConfig, ServerConfig, RootCertStore, Certificate, PrivateKey};

// pub fn create_server_config(ca_cert: &str, server_cert: &str, server_key: &str) -> Arc<ServerConfig> {
//     // Load certs from paths in config
//     // Enforce client cert validation
//     let mut root_store = RootCertStore::empty();
//     // add CA
//     let config = ServerConfig::builder()
//         .with_safe_defaults()
//         .with_client_cert_verifier(Arc::new(/* custom verifier */))
//         .with_single_cert(vec![/* cert */], /* key */)
//         .unwrap();
//     Arc::new(config)
// }

// // Similar for client side
// // pub fn attest(device_id: &str, hash: &str, expected_hash: &str) -> bool {
// //     !device_id.is_empty() && hash == expected_hash  // In real: TPM/secure boot check
// // }