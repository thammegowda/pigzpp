// Locate and link the pigzpp C-ABI shared library (libpigzppc), built with
//   cmake -DPIGZPP_BUILD_CAPI=ON -S . -B build && cmake --build build --target pigzppc
//
// Override the search directory with PIGZPP_BUILD_DIR if the build lives elsewhere.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let build_dir = std::env::var_os("PIGZPP_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("../../build"));
    let build_dir = build_dir.canonicalize().unwrap_or(build_dir);

    println!("cargo:rustc-link-search=native={}", build_dir.display());
    println!("cargo:rustc-link-lib=dylib=pigzppc");
    // rpath so downstream binaries find libpigzppc.so without LD_LIBRARY_PATH.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", build_dir.display());
    println!("cargo:rerun-if-env-changed=PIGZPP_BUILD_DIR");
}
