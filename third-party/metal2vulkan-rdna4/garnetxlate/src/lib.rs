// SPDX-License-Identifier: LGPL-3.0-or-later
//! garnetxlate: the Garnet in-process shader translator.
//!
//! One C-ABI entry point turns the text form of an Apple AIR module (already disassembled by the
//! caller with the operating system's own LLVM) into Vulkan SPIR-V plus the trimmed reflection the
//! Garnet Metal bundle reads. It is the in-process equivalent of the daemon's pipeline
//! (`rdna4-llvm-dis`, `metal2vulkan --emit-meta [--local NAME=X,Y,Z]`, `patch_storage_formats`, `trim`),
//! minus every file and every process: a sandboxed application cannot start `spirv-val` or write a
//! scratch directory, and this code never tries to.
//!
//! The external `spirv-val` gate is skipped (see `metal2vulkan::Validation`). The daemon already runs
//! with an always-pass validator on the machine this serves, so the bytes are unchanged; the two
//! structural rejects that follow the validator still run.
//!
//! Every exported function is wrapped in `catch_unwind` and returns a code, never unwinds across the
//! C boundary. Return codes of `garnetx_translate`: 0 ok, 1 refused (the translator said no), 2 panic
//! caught, 3 bad arguments.

mod icb_stage;
pub use icb_stage::garnetx_lower_icb_stage;
mod stage2_link;
pub use stage2_link::{
    garnetx_detect_stage, garnetx_list_exported_symbols, garnetx_translate_linked_libraries,
    garnetx_translate_stage3, list_exported_symbols, translate_bytes_stage2,
    translate_bytes_stage3, RDNA4xDynamicLibrary, RDNA4xIntersectionFunction,
    GARNETX_IFT_KIND_BOUNDING_BOX, GARNETX_IFT_KIND_CURVE, GARNETX_IFT_KIND_TRIANGLE_ANY_HIT,
    GARNETX_STAGE_FRAGMENT, GARNETX_STAGE_KERNEL, GARNETX_STAGE_MESH, GARNETX_STAGE_OBJECT,
    GARNETX_STAGE_VERTEX,
};
mod source_proof;

mod query_lod;
pub use query_lod::{
    garnetx_lower_argument_resources_with_capabilities,
    garnetx_translate_linked_with_capabilities,
    garnetx_translate_with_capabilities,
    lower_gpu_arguments_with_capabilities,
    RDNA4xBackendCapabilities,
    GARNETX_CAP_MESH_EMULATED_PAYLOAD,
    GARNETX_CAP_NATIVE_INT64,
    GARNETX_CAP_RASTER_SAMPLES_MASK,
    GARNETX_CAP_RASTER_SAMPLES_SHIFT,
    GARNETX_CAP_RT_CURVES,
    GARNETX_CAP_RT_IFT_ABI_V2,
    GARNETX_CAP_RT_MOTION_BLUR,
    GARNETX_CAP_RT_POSITION_FETCH,
    GARNETX_CAP_RT_PRIMITIVE_DATA,
    GARNETX_MESH_GRID_PARAMS_BINDING,
    GARNETX_MESH_INDIRECT_BINDING,
    GARNETX_MESH_PAYLOAD_BINDING,
    GARNETX_RT_PRIM_DATA_BINDING,
};

mod air_spirv;
pub use air_spirv::{
    detect_stage_text, linkage_for, patch_storage_formats, translate_bytes,
    translate_bytes_linked, translate_text, translate_text_gpu_arguments,
    translate_text_gpu_arguments_abi8_with_query_lod_replay, translate_text_linked, trim_meta,
    trim_meta_with_buffer_address_mode,
};
pub(crate) use air_spirv::translate_text_with_rt_options;

mod device_caps;
pub use device_caps::lower_gpu_arguments;
pub(crate) use device_caps::lower_gpu_arguments_with_table_version_and_replay;

pub(crate) use metal2vulkan::passes::QueryLodReplay;
use metal2vulkan::reflect::ShaderReflection;
use serde_json::Value;
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};

/// Version of the C ABI below. The caller refuses a library whose version it does not know.
/// Version 3 adds capability-aware exports; version 2 exports retain their signatures.
/// Version 2 adds `garnetx_translate_linked` (direct visible-function references resolved to the exact authored
/// dependencies before translation); `garnetx_translate` is unchanged.
pub const ABI_VERSION: u32 = 3;
/// Largest LLVM text accepted (the caller caps it too).
pub const MAX_LL_BYTES: usize = 64 * 1024 * 1024;
/// Largest SPIR-V binary accepted by post-translation rewrite exports (matches RDNA4X_MAX_SPV).
pub const MAX_SPV_BYTES: usize = 64 * 1024 * 1024;

pub const RC_OK: c_int = 0;
pub const RC_REFUSED: c_int = 1;
pub const RC_PANIC: c_int = 2;
pub const RC_BAD_ARGS: c_int = 3;

/// The ABI version.
#[no_mangle]
pub extern "C" fn garnetx_abi_version() -> u32 {
    ABI_VERSION
}

/// Free a buffer returned through `garnetx_translate`. `len` must be the length it reported.
///
/// # Safety
/// `ptr`/`len` must come from `garnetx_translate` and be freed once.
#[no_mangle]
pub unsafe extern "C" fn garnetx_free(ptr: *mut u8, len: usize) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !ptr.is_null() {
            drop(Vec::from_raw_parts(ptr, len, len));
        }
    }));
}

/// Translate AIR text.
///
/// `ll`/`ll_len`: the module text as `llvm-dis` prints it. `local_xyz`: NULL, or three non-zero
/// `u32`s, the nominal kernel threadgroup size (the daemon's `--local NAME=X,Y,Z`). On success
/// `*spv`/`*spv_len` and `*meta`/`*meta_len` hold buffers to release with `garnetx_free`. On a non-zero
/// return `err` (capacity `err_len`) holds a NUL-terminated message and the outputs are zeroed.
///
/// # Safety
/// All pointers must be valid for their stated lengths; the out pointers must be writable.
#[no_mangle]
pub unsafe extern "C" fn garnetx_translate(
    ll: *const u8,
    ll_len: usize,
    local_xyz: *const u32,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if !spv.is_null() {
        *spv = std::ptr::null_mut();
    }
    if !spv_len.is_null() {
        *spv_len = 0;
    }
    if !meta.is_null() {
        *meta = std::ptr::null_mut();
    }
    if !meta_len.is_null() {
        *meta_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    if ll.is_null()
        || ll_len == 0
        || ll_len > MAX_LL_BYTES
        || spv.is_null()
        || spv_len.is_null()
        || meta.is_null()
        || meta_len.is_null()
    {
        put_err(err, err_len, "bad arguments");
        return RC_BAD_ARGS;
    }
    let local = if local_xyz.is_null() {
        [64u32, 1, 1]
    } else {
        let l = [*local_xyz, *local_xyz.add(1), *local_xyz.add(2)];
        if l.contains(&0) {
            put_err(err, err_len, "bad arguments: --local has a zero dimension");
            return RC_BAD_ARGS;
        }
        l
    };
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| translate_bytes(bytes, local)));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}

/// One authored linked dependency: the logical Metal function name and the LLVM text of the AIR
/// module that defines it (as `llvm-dis` prints it; this library sanitises it).
#[repr(C)]
pub struct RDNA4xDep {
    pub symbol: *const u8,
    pub symbol_len: usize,
    pub ll: *const u8,
    pub ll_len: usize,
}

/// Largest number of dependencies one call accepts, and the largest total LLVM text of the entry plus them.
pub const MAX_DEPS: usize = 256;

/// Translate AIR text whose direct visible-function references (`.MTL_VISIBLE_FN_REF`) are resolved
/// to the exact authored dependencies in `deps` first (the portable alternative to Metal's runtime
/// function linker; see `metal2vulkan::specialize_linked_module`). Everything else is as
/// `garnetx_translate`. Each dependency is sanitised like the entry. Dependencies the entry does not
/// reach (transitively) are ignored, so a caller may pass every function the pipeline links; a
/// reference with no authored dependency is a refusal (code 1) naming it. An entry that references
/// nothing is translated exactly as `garnetx_translate` would.
///
/// # Safety
/// All pointers must be valid for their stated lengths (`deps` for `ndeps` entries); the out pointers must be writable.
#[no_mangle]
pub unsafe extern "C" fn garnetx_translate_linked(
    ll: *const u8,
    ll_len: usize,
    local_xyz: *const u32,
    deps: *const RDNA4xDep,
    ndeps: usize,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if !spv.is_null() {
        *spv = std::ptr::null_mut();
    }
    if !spv_len.is_null() {
        *spv_len = 0;
    }
    if !meta.is_null() {
        *meta = std::ptr::null_mut();
    }
    if !meta_len.is_null() {
        *meta_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    if ll.is_null()
        || ll_len == 0
        || ll_len > MAX_LL_BYTES
        || spv.is_null()
        || spv_len.is_null()
        || meta.is_null()
        || meta_len.is_null()
        || (ndeps > 0 && deps.is_null())
        || ndeps > MAX_DEPS
    {
        put_err(err, err_len, "bad arguments");
        return RC_BAD_ARGS;
    }
    let local = if local_xyz.is_null() {
        [64u32, 1, 1]
    } else {
        let l = [*local_xyz, *local_xyz.add(1), *local_xyz.add(2)];
        if l.contains(&0) {
            put_err(err, err_len, "bad arguments: --local has a zero dimension");
            return RC_BAD_ARGS;
        }
        l
    };
    let mut total = ll_len;
    let mut owned: Vec<(String, Vec<u8>)> = Vec::with_capacity(ndeps);
    for i in 0..ndeps {
        let d = &*deps.add(i);
        if d.symbol.is_null() || d.symbol_len == 0 || d.ll.is_null() || d.ll_len == 0 {
            put_err(err, err_len, "bad arguments: empty dependency");
            return RC_BAD_ARGS;
        }
        total = total.saturating_add(d.ll_len);
        if total > MAX_LL_BYTES {
            put_err(err, err_len, "bad arguments: dependencies too large");
            return RC_BAD_ARGS;
        }
        let sym = match std::str::from_utf8(std::slice::from_raw_parts(d.symbol, d.symbol_len)) {
            Ok(x) => x.to_string(),
            Err(_) => {
                put_err(
                    err,
                    err_len,
                    "bad arguments: dependency symbol is not UTF-8",
                );
                return RC_BAD_ARGS;
            }
        };
        owned.push((sym, std::slice::from_raw_parts(d.ll, d.ll_len).to_vec()));
    }
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        translate_bytes_linked(bytes, local, &owned)
    }));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}

#[allow(clippy::type_complexity)]
unsafe fn finish(
    outcome: std::thread::Result<Result<(Vec<u8>, Vec<u8>), String>>,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    match outcome {
        Ok(Ok((s, m))) => {
            let (sp, sl) = leak(s);
            let (mp, ml) = leak(m);
            *spv = sp;
            *spv_len = sl;
            *meta = mp;
            *meta_len = ml;
            RC_OK
        }
        Ok(Err(e)) => {
            put_err(err, err_len, &e);
            RC_REFUSED
        }
        Err(p) => {
            let why = if let Some(s) = p.downcast_ref::<&str>() {
                (*s).to_string()
            } else if let Some(s) = p.downcast_ref::<String>() {
                s.clone()
            } else {
                "unknown panic".to_string()
            };
            put_err(err, err_len, &format!("panic: {why}"));
            RC_PANIC
        }
    }
}

fn leak(mut v: Vec<u8>) -> (*mut u8, usize) {
    v.shrink_to_fit();
    let len = v.len();
    let ptr = v.as_mut_ptr();
    std::mem::forget(v);
    (ptr, len)
}

pub(crate) unsafe fn put_err(dst: *mut c_char, cap: usize, msg: &str) {
    if dst.is_null() || cap == 0 {
        return;
    }
    let n = msg.len().min(cap - 1);
    // Cut on a character boundary so the message stays valid UTF-8.
    let mut n = n;
    while n > 0 && !msg.is_char_boundary(n) {
        n -= 1;
    }
    std::ptr::copy_nonoverlapping(msg.as_ptr(), dst as *mut u8, n);
    *dst.add(n) = 0;
}


/// Optional ABI-2 extension: lower cached SPIR-V and its matching metadata for GPU resource tokens.
/// Outputs and return codes follow [`garnetx_translate`]. Consumers opt in by resolving this symbol
/// and implementing the returned versioned descriptor-table contract.
///
/// # Safety
/// Inputs must be readable for their lengths; outputs must be writable and not alias the inputs.
#[no_mangle]
pub unsafe extern "C" fn garnetx_lower_argument_resources(
    input_spv: *const u8,
    input_spv_len: usize,
    input_meta: *const u8,
    input_meta_len: usize,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if !spv.is_null() {
        *spv = std::ptr::null_mut();
    }
    if !spv_len.is_null() {
        *spv_len = 0;
    }
    if !meta.is_null() {
        *meta = std::ptr::null_mut();
    }
    if !meta_len.is_null() {
        *meta_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    if input_spv.is_null()
        || !(20..=MAX_SPV_BYTES).contains(&input_spv_len)
        || !input_spv_len.is_multiple_of(4)
        || input_meta.is_null()
        || input_meta_len == 0
        || input_meta_len > 8 * 1024 * 1024
        || spv.is_null()
        || spv_len.is_null()
        || meta.is_null()
        || meta_len.is_null()
    {
        put_err(err, err_len, "bad GPU argument lowering inputs");
        return RC_BAD_ARGS;
    }
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        lower_gpu_arguments(
            std::slice::from_raw_parts(input_spv, input_spv_len),
            std::slice::from_raw_parts(input_meta, input_meta_len),
        )
    }));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}


/// Convenience for tests and tools: the reflection type is re-exported so a dependent can inspect it.
pub type Reflection = ShaderReflection;
