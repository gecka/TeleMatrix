//! Adds an rpath to Qt's FFmpeg on macOS, for cargo-linked binaries only.
//!
//! macOS links the universal libav that official Qt ships rather than a system
//! one (see third_party/ffmpeg-7.1/README.md). Those dylibs carry `@rpath/...`
//! install names, where Homebrew's carried absolute paths — so anything cargo
//! links directly (test, bench and example binaries) now has nothing to resolve
//! them against and aborts at launch:
//!
//!     dyld: Library not loaded: @rpath/libavformat.61.dylib
//!
//! `cargo:rustc-link-arg` covers exactly those targets and NOT the staticlib, so
//! the shipped app is untouched: it resolves libav through the copies
//! macdeployqt places in the bundle, via its own `@executable_path/../Frameworks`
//! rpath. Adding Qt's build-machine path there would be wrong.
//!
//! This is deliberately not the FFI-header build script that
//! `rust/cbindgen_runner` exists to avoid — see that crate's docs. A build script
//! is the right tool here because cargo re-applies its link flags on every link,
//! which is the behaviour needed, rather than only when the crate recompiles.

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    // Build scripts compile for the host, so `cfg!(target_os)` would describe the
    // wrong machine when cross-compiling the x86_64 slice of a universal build.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        return;
    }

    // cargo_metadata(false): ffmpeg-sys-next already emits the link-lib and
    // link-search directives. Only the rpath is wanted from this probe.
    let probe = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("libavcodec");

    let Ok(libavcodec) = probe else {
        // Not fatal: ffmpeg-sys-next's own probe reports the real, actionable
        // error a moment later. Failing here would only mask it.
        return;
    };

    for path in libavcodec.link_paths {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", path.display());
    }
}
