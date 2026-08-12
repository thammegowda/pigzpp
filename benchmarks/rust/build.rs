// Add an rpath to libpigzppc.so so the benchmark binary runs without
// LD_LIBRARY_PATH. The linker search + lib come from the pigzpp crate's
// build.rs; rpath must be set on the final binary's own build script.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let build_dir = std::env::var_os("PIGZPP_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("../../build"));
    let build_dir = build_dir.canonicalize().unwrap_or(build_dir);
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", build_dir.display());
    println!("cargo:rerun-if-env-changed=PIGZPP_BUILD_DIR");
}
