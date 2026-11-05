use std::{fs, path::Path};

fn main() {
    let src = Path::new("src");

    // cargo пересобирает при изменении любого C++ исходника или заголовка аллокатора
    println!("cargo:rerun-if-changed=build.rs");
    for entry in fs::read_dir(src).unwrap() {
        let p = entry.unwrap().path();
        if matches!(p.extension().and_then(|e| e.to_str()), Some("cpp" | "hpp" | "h")) {
            println!("cargo:rerun-if-changed={}", p.display());
        }
    }

    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .opt_level(2)
        .define("NDEBUG", None)
        .flag_if_supported("/Oi")
        .file(src.join("pmalloc_allocator.cpp"))
        .compile("pmalloc");
}
