use std::env;
use std::path::{Path, PathBuf};

/// Dynamically find the Wine root directory based on the current working directory
/// This function handles different invocation contexts:
/// - When called from `make` (root Wine directory)
/// - When called from cargo-analyzer in IDE (rust/src_lib directory)
fn find_wine_root() -> PathBuf {
    let current_dir = env::current_dir().expect("Failed to get current directory");
    
    // Check if we're already in the Wine root directory
    if current_dir.join("rust").join("src_lib").join("build.rs").exists() {
        return current_dir;
    }
    
    // Check if we're in the rust directory
    if current_dir.join("src_lib").join("build.rs").exists() {
        return current_dir.join("..");
    }
    
    // Check if we're in the src_lib directory (most common case for IDE)
    if current_dir.join("build.rs").exists() {
        return current_dir.join("..").join("..");
    }
    
    // Fallback: try to find Wine root by looking for characteristic files
    let mut current = current_dir.as_path();
    for _ in 0..5 { // Limit search depth to prevent infinite loops
        if current.join("rust").join("src_lib").join("build.rs").exists() {
            return current.to_path_buf();
        }
        if let Some(parent) = current.parent() {
            current = parent;
        } else {
            break;
        }
    }
    
    panic!(
        "Could not find Wine root directory. Current directory: {}",
        current_dir.display()
    );
}

fn main() {
    println!("Testing path resolution from different directories...");
    
    let wine_root = find_wine_root();
    println!("Found Wine root: {}", wine_root.display());
    
    // Test that the paths exist
    let include_path = wine_root.join("include");
    let wine_include_path = include_path.join("wine");
    let debug_header = wine_include_path.join("debug.h");
    
    println!("Include path: {}", include_path.display());
    println!("Wine include path: {}", wine_include_path.display());
    println!("Debug header path: {}", debug_header.display());
    println!("Debug header exists: {}", debug_header.exists());
    
    // Test dlls paths
    let dlls_path = wine_root.join("dlls");
    let ntdll_path = dlls_path.join("ntdll").join("ntdll.dll.so");
    println!("Ntdll path: {}", ntdll_path.display());
    println!("Ntdll exists: {}", ntdll_path.exists());
}