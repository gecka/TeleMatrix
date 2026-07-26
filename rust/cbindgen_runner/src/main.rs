//! Standalone cbindgen runner — regenerates the C FFI header (`protocol_ffi.h`)
//! for the C++ build, decoupled from the main crate's compilation.
//!
//! Why a separate binary instead of `build.rs`: a `build.rs` runs cbindgen only
//! when cargo recompiles the crate. When the crate is up-to-date (common after a
//! `cmake` clean/reconfigure that wipes the generated header), `cargo build`
//! no-ops and the header is never recreated — the C++ build then fails with
//! `'protocol_ffi.h' file not found`. CMake invokes this via `cargo run`, which
//! ALWAYS executes the binary, so the header is regenerated whenever the build
//! step fires (including when the output is merely missing).
//!
//! Inputs (set by CMake):
//!   TELEMATRIX_CRATE_DIR    — path to the telematrix-protocol crate to scan
//!   TELEMATRIX_BINDINGS_DIR — directory to write `protocol_ffi.h` into

use std::env;
use std::path::PathBuf;

fn main() {
    let crate_dir = env::var("TELEMATRIX_CRATE_DIR")
        .expect("TELEMATRIX_CRATE_DIR must point at the telematrix-protocol crate");
    let output_dir = PathBuf::from(
        env::var("TELEMATRIX_BINDINGS_DIR")
            .expect("TELEMATRIX_BINDINGS_DIR must be set to the bindings output directory"),
    );
    std::fs::create_dir_all(&output_dir).expect("unable to create bindings output directory");

    let config_path = PathBuf::from(&crate_dir).join("cbindgen.toml");
    let config = cbindgen::Config::from_file(&config_path).unwrap_or_default();

    let output = output_dir.join("protocol_ffi.h");
    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("unable to generate C bindings")
        .write_to_file(&output);

    eprintln!("cbindgen-runner: wrote {}", output.display());
}
