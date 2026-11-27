// SPDX-License-Identifier: LGPL-3.0-or-later
//! Metal 3 modularity C FFI: dynamic library symbol enumeration/linking (Task 3.2) and runtime
//! visible function table bounded-switch dispatch + metadata (Task 3.3).

use crate::air_spirv::{
    detect_stage_text, patch_storage_formats, trim_meta_with_buffer_address_mode,
};
use crate::query_lod::{
    gate_query_lod, gate_query_metadata, source_language_version, stamp_query_policy,
    RDNA4xBackendCapabilities,
};
use crate::source_proof::uniform_control_proof_bytes;
use crate::{
    finish, leak, put_err, RDNA4xDep, MAX_DEPS, MAX_LL_BYTES, RC_BAD_ARGS, RC_OK, RC_PANIC,
    RC_REFUSED,
};
use metal2vulkan::linked_functions::{
    exported_symbols, visible_function_reference_symbols, DynamicLibraryModule,
    IntersectionFunctionKind, LinkedFunctionLinkage, LinkedFunctionReference,
    LinkedIntersectionFunction, RuntimeIntersectionFunctionTableInfo,
    RuntimeVisibleFunctionTableInfo,
};
use metal2vulkan::passes::{QueryLodReplay, Stage, TransformOptions};
use metal2vulkan::tools::sanitize_ll_text_with_datalayout;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

pub const GARNETX_STAGE_VERTEX: u32 = 0;
pub const GARNETX_STAGE_FRAGMENT: u32 = 1;
pub const GARNETX_STAGE_KERNEL: u32 = 2;
pub const GARNETX_STAGE_OBJECT: u32 = 3;
pub const GARNETX_STAGE_MESH: u32 = 4;

pub const GARNETX_IFT_KIND_BOUNDING_BOX: u32 = 0;
pub const GARNETX_IFT_KIND_TRIANGLE_ANY_HIT: u32 = 1;
pub const GARNETX_IFT_KIND_CURVE: u32 = 2;

/// One preloaded dynamic library (`MTLDynamicLibrary` / `binaryFunctions`) as LLVM AIR text.
#[repr(C)]
pub struct RDNA4xDynamicLibrary {
    pub name: *const u8,
    pub name_len: usize,
    pub ll: *const u8,
    pub ll_len: usize,
}

/// One pipeline-linked Metal 3 intersection function (`[[intersection(bounding_box|triangle)]]`)
/// for runtime `MTLIntersectionFunctionTable` dispatch inside `OpRayQueryProceedKHR`.
#[repr(C)]
pub struct RDNA4xIntersectionFunction {
    pub symbol: *const u8,
    pub symbol_len: usize,
    pub ll: *const u8,
    pub ll_len: usize,
    /// `GARNETX_IFT_KIND_BOUNDING_BOX` (0) or `GARNETX_IFT_KIND_TRIANGLE_ANY_HIT` (1).
    pub kind: u32,
}

/// C FFI: detect the Metal shader stage from AIR text (`GARNETX_STAGE_*`).
///
/// # Safety
/// `ll` must be readable for `ll_len` bytes; `out_stage_kind` must be writable.
#[no_mangle]
pub unsafe extern "C" fn garnetx_detect_stage(
    ll: *const u8,
    ll_len: usize,
    out_stage_kind: *mut u32,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    if ll.is_null() || ll_len == 0 || ll_len > MAX_LL_BYTES || out_stage_kind.is_null() {
        put_err(err, err_len, "bad arguments");
        return RC_BAD_ARGS;
    }
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| -> Result<u32, String> {
        let text = std::str::from_utf8(bytes).map_err(|e| format!("LLVM text is not UTF-8: {e}"))?;
        let (san, _) = sanitize_ll_text_with_datalayout(text);
        let stage = detect_stage_text(&san)?;
        Ok(match stage {
            Stage::Vertex => GARNETX_STAGE_VERTEX,
            Stage::Fragment => GARNETX_STAGE_FRAGMENT,
            Stage::Kernel => GARNETX_STAGE_KERNEL,
            Stage::Object => GARNETX_STAGE_OBJECT,
            Stage::Mesh => GARNETX_STAGE_MESH,
        })
    }));
    match outcome {
        Ok(Ok(kind)) => {
            *out_stage_kind = kind;
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

/// Enumerate exported (non-internal, non-private, non-intrinsic) symbols in an AIR module.
pub fn list_exported_symbols(ll: &[u8]) -> Result<Vec<String>, String> {
    let text = std::str::from_utf8(ll).map_err(|e| format!("LLVM text is not UTF-8: {e}"))?;
    let (san, _) = sanitize_ll_text_with_datalayout(text);
    exported_symbols(&san)
}

/// C FFI: enumerate exported symbols of a dynamic library AIR module as a UTF-8 JSON array.
/// Release `*out_json` with `garnetx_free(*out_json, *out_json_len)`.
///
/// # Safety
/// `ll` must be readable for `ll_len` bytes; output pointers must be writable.
#[no_mangle]
pub unsafe extern "C" fn garnetx_list_exported_symbols(
    ll: *const u8,
    ll_len: usize,
    out_json: *mut *mut u8,
    out_json_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if !out_json.is_null() {
        *out_json = std::ptr::null_mut();
    }
    if !out_json_len.is_null() {
        *out_json_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    if ll.is_null()
        || ll_len == 0
        || ll_len > MAX_LL_BYTES
        || out_json.is_null()
        || out_json_len.is_null()
    {
        put_err(err, err_len, "bad arguments");
        return RC_BAD_ARGS;
    }
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| -> Result<Vec<u8>, String> {
        let syms = list_exported_symbols(bytes)?;
        serde_json::to_vec(&syms).map_err(|e| format!("symbols json: {e}"))
    }));
    match outcome {
        Ok(Ok(json_bytes)) => {
            let (p, n) = leak(json_bytes);
            *out_json = p;
            *out_json_len = n;
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

fn enrich_vft_metadata(vft: &RuntimeVisibleFunctionTableInfo, reflection_json: &Value) -> Value {
    let bindings = reflection_json.get("bindings").and_then(Value::as_array);
    let tables: Vec<Value> = vft
        .tables
        .iter()
        .map(|tbl| {
            let descriptor = bindings.and_then(|bs| {
                bs.iter()
                    .find(|b| {
                        b.get("kind").and_then(Value::as_str) == Some("Buffer")
                            && b.get("param_index").and_then(Value::as_u64)
                                == Some(tbl.param_index as u64)
                    })
                    .or_else(|| {
                        bs.iter().find(|b| {
                            b.get("kind").and_then(Value::as_str) == Some("Buffer")
                                && b.get("metal_index").and_then(Value::as_u64)
                                    == Some(tbl.metal_index as u64)
                        })
                    })
                    .and_then(|b| b.get("descriptor"))
                    .cloned()
            });
            json!({
                "param_index": tbl.param_index,
                "metal_index": tbl.metal_index,
                "descriptor": descriptor,
            })
        })
        .collect();
    let handles: Vec<Value> = vft
        .handles
        .iter()
        .map(|h| {
            json!({
                "handle_id": h.handle_id,
                "symbol": h.symbol,
            })
        })
        .collect();
    json!({
        "abi_version": vft.abi_version,
        "header_words": vft.header_words,
        "tables": tables,
        "handles": handles,
    })
}

fn enrich_ift_metadata(
    ift: &RuntimeIntersectionFunctionTableInfo,
    reflection_json: &Value,
) -> Value {
    let bindings = reflection_json.get("bindings").and_then(Value::as_array);
    let tables: Vec<Value> = ift
        .tables
        .iter()
        .map(|tbl| {
            let descriptor = bindings.and_then(|bs| {
                bs.iter()
                    .find(|b| {
                        b.get("kind").and_then(Value::as_str) == Some("Buffer")
                            && b.get("param_index").and_then(Value::as_u64)
                                == Some(tbl.param_index as u64)
                    })
                    .or_else(|| {
                        bs.iter().find(|b| {
                            b.get("kind").and_then(Value::as_str) == Some("Buffer")
                                && b.get("metal_index").and_then(Value::as_u64)
                                    == Some(tbl.metal_index as u64)
                        })
                    })
                    .and_then(|b| b.get("descriptor"))
                    .cloned()
            });
            json!({
                "param_index": tbl.param_index,
                "metal_index": tbl.metal_index,
                "descriptor": descriptor,
            })
        })
        .collect();
    let handles: Vec<Value> = ift
        .handles
        .iter()
        .map(|h| {
            json!({
                "handle_id": h.handle_id,
                "symbol": h.symbol,
                "kind": match h.kind {
                    IntersectionFunctionKind::BoundingBox => "BoundingBox",
                    IntersectionFunctionKind::TriangleAnyHit => "TriangleAnyHit",
                    IntersectionFunctionKind::Curve => "Curve",
                },
            })
        })
        .collect();
    json!({
        "abi_version": ift.abi_version,
        "header_words": ift.header_words,
        "tables": tables,
        "handles": handles,
    })
}

/// Pure-Rust Metal 3 modularity translation supporting direct visible-function deps, preloaded
/// dynamic libraries (`MTLDynamicLibrary`), and runtime `MTLVisibleFunctionTable` dispatch across
/// compute, vertex, fragment, object, and mesh stages.
pub fn translate_bytes_stage2(
    ll: &[u8],
    local: [u32; 3],
    deps: &[(String, Vec<u8>)],
    dynlibs: &[(String, Vec<u8>)],
    visible_functions: &[(String, Vec<u8>)],
    capabilities: Option<RDNA4xBackendCapabilities>,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_bytes_stage3(
        ll,
        local,
        deps,
        dynlibs,
        visible_functions,
        &[],
        capabilities,
    )
}

/// Pure-Rust Metal 3 Stage 3 translation: all Stage 2 modularity plus Object/Mesh shaders,
/// hardware `SPV_KHR_ray_query`, and runtime `MTLIntersectionFunctionTable` dispatch across
/// compute and render (vertex/fragment/object/mesh) stages.
pub fn translate_bytes_stage3(
    ll: &[u8],
    local: [u32; 3],
    deps: &[(String, Vec<u8>)],
    dynlibs: &[(String, Vec<u8>)],
    visible_functions: &[(String, Vec<u8>)],
    intersection_functions: &[(String, IntersectionFunctionKind, Vec<u8>)],
    capabilities: Option<RDNA4xBackendCapabilities>,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    let caps = capabilities.map(RDNA4xBackendCapabilities::validate).transpose()?;
    let entry_text =
        std::str::from_utf8(ll).map_err(|e| format!("LLVM text is not UTF-8: {e}"))?;
    let (san_entry, datalayout) = sanitize_ll_text_with_datalayout(entry_text);
    let stage = detect_stage_text(&san_entry)?;

    let mut linkage = LinkedFunctionLinkage::default();

    // 1. Dynamic libraries (`preloadedLibraries` / `binaryFunctions`).
    for (name, bytes) in dynlibs {
        let lib_text = std::str::from_utf8(bytes)
            .map_err(|e| format!("dynamic library {name:?}: LLVM text is not UTF-8: {e}"))?;
        let (san_lib, _) = sanitize_ll_text_with_datalayout(lib_text);
        linkage.dynamic_libraries.push(DynamicLibraryModule {
            name: name.clone(),
            module_ll: san_lib,
        });
    }

    // 2. Direct `.MTL_VISIBLE_FN_REF` dependencies (transitively reachable).
    let mut sanitized_deps: BTreeMap<&str, String> = BTreeMap::new();
    for (sym, bytes) in deps {
        let dep_text = std::str::from_utf8(bytes)
            .map_err(|e| format!("linked dependency {sym:?}: LLVM text is not UTF-8: {e}"))?;
        let (san_dep, _) = sanitize_ll_text_with_datalayout(dep_text);
        match sanitized_deps.get(sym.as_str()) {
            Some(prev) if *prev != san_dep => {
                return Err(format!(
                    "linked function {sym:?} is authored twice with different modules"
                ));
            }
            _ => {
                sanitized_deps.insert(sym.as_str(), san_dep);
            }
        }
    }
    let mut need: Vec<String> = visible_function_reference_symbols(&san_entry)?;
    need.extend(metal2vulkan::linked_functions::external_symbol_references(&san_entry)?);
    let mut done: BTreeSet<String> = BTreeSet::new();
    while let Some(sym) = need.pop() {
        if !done.insert(sym.clone()) {
            continue;
        }
        let Some(module) = sanitized_deps.get(sym.as_str()) else {
            continue;
        };
        need.extend(visible_function_reference_symbols(module)?);
        need.extend(metal2vulkan::linked_functions::external_symbol_references(module)?);
        linkage.visible_references.push(LinkedFunctionReference {
            symbol: sym,
            module_ll: module.clone(),
        });
    }

    // 3. Runtime `MTLVisibleFunctionTable` linked visible functions (`[[visible]]`).
    let mut seen_vis: BTreeMap<&str, String> = BTreeMap::new();
    for (sym, bytes) in visible_functions {
        let vis_text = std::str::from_utf8(bytes)
            .map_err(|e| format!("visible function {sym:?}: LLVM text is not UTF-8: {e}"))?;
        let (san_vis, _) = sanitize_ll_text_with_datalayout(vis_text);
        match seen_vis.get(sym.as_str()) {
            Some(prev) if *prev != san_vis => {
                return Err(format!(
                    "visible function {sym:?} is authored twice with different modules"
                ));
            }
            Some(_) => continue,
            None => {
                seen_vis.insert(sym.as_str(), san_vis.clone());
                linkage.runtime_visible_functions.push(LinkedFunctionReference {
                    symbol: sym.clone(),
                    module_ll: san_vis,
                });
            }
        }
    }
    if !visible_functions.is_empty()
        || san_entry.contains("air.visible_function_table")
        || san_entry.contains("@air.get_function_pointer_visible_function_table")
        || intersection_functions.iter().any(|(_, _, b)| {
            std::str::from_utf8(b).is_ok_and(|s| s.contains("visible_function_table"))
        })
    {
        linkage.runtime_visible_tables = true;
    }

    // 4. Runtime `MTLIntersectionFunctionTable` linked intersection functions.
    let mut seen_ift: BTreeMap<&str, (IntersectionFunctionKind, String)> = BTreeMap::new();
    for (sym, kind, bytes) in intersection_functions {
        let ift_text = std::str::from_utf8(bytes)
            .map_err(|e| format!("intersection function {sym:?}: LLVM text is not UTF-8: {e}"))?;
        let (san_ift, _) = sanitize_ll_text_with_datalayout(ift_text);
        match seen_ift.get(sym.as_str()) {
            Some((prev_kind, prev_ll)) if *prev_kind != *kind || *prev_ll != san_ift => {
                return Err(format!(
                    "intersection function {sym:?} is authored twice with conflicting definitions"
                ));
            }
            Some(_) => continue,
            None => {
                seen_ift.insert(sym.as_str(), (*kind, san_ift.clone()));
                linkage
                    .runtime_intersection_functions
                    .push(LinkedIntersectionFunction {
                        symbol: sym.clone(),
                        kind: *kind,
                        module_ll: san_ift,
                    });
            }
        }
    }
    if !intersection_functions.is_empty()
        || san_entry.contains("air.intersection_function_table")
        || san_entry.contains("@air.intersect.intersection_function_buffer")
    {
        linkage.runtime_intersection_tables = true;
    }
    linkage.runtime_intersection_abi_v2 =
        caps.is_some_and(RDNA4xBackendCapabilities::ift_abi_v2);
    linkage.runtime_rt_primitive_data =
        caps.is_some_and(RDNA4xBackendCapabilities::rt_primitive_data);
    linkage.runtime_rt_curves =
        caps.is_some_and(RDNA4xBackendCapabilities::rt_curves);

    let (san_ll, vft_info, ift_info) =
        metal2vulkan::specialize_linked_module_with_tables(&san_entry, stage, &linkage)?;

    let replay = caps.map_or(QueryLodReplay::UniformOnly, RDNA4xBackendCapabilities::replay);
    let mut options = TransformOptions {
        kernel_local_size: local,
        kernel_dispatch: None,
        raster_sample_count: caps.and_then(RDNA4xBackendCapabilities::raster_sample_count),
        vertex_amplification_count: 1,
        ..TransformOptions::default()
    };
    options = options
        .with_normalized_sampler_coordinates()
        .with_query_lod_replay(replay)
        .with_hardware_ray_query();
    if caps.is_some_and(RDNA4xBackendCapabilities::emulated_mesh_payload) {
        options = options.with_emulated_mesh_payload();
    }
    if caps.is_some_and(RDNA4xBackendCapabilities::rt_primitive_data) {
        options = options.with_rt_primitive_data();
    }
    if caps.is_some_and(RDNA4xBackendCapabilities::rt_curves) {
        options = options.with_rt_curves();
    }
    if caps.is_some_and(RDNA4xBackendCapabilities::native_int64) {
        options = options.with_native_int64();
    }

    let tmp = Path::new("");
    let (spv, reflection, buffer_address_mode, uniform_control_proof) =
        metal2vulkan::translate_sanitized_native_reflected_unvalidated_with_uniform_control_proof(
            &san_ll,
            stage,
            tmp,
            options,
            datalayout.as_deref(),
        )?;
    if spv.len() < 20 || spv[..4] != [0x03, 0x02, 0x23, 0x07] {
        return Err("bad SPIR-V magic".to_string());
    }

    let reflection_text = serde_json::to_string(&reflection).map_err(|e| format!("meta: {e}"))?;
    let mut reflection_json: Value =
        serde_json::from_str(&reflection_text).map_err(|e| format!("meta: {e}"))?;
    let (spv, patched) = patch_storage_formats(&spv, &reflection_json);
    if !uniform_control_proof.entries.is_empty()
        && (!uniform_control_proof.values.is_empty()
            || !uniform_control_proof.sampling_coordinates.is_empty())
    {
        reflection_json["uniform_control_proof"] =
            uniform_control_proof_bytes(&uniform_control_proof, &spv);
    }
    if let Some(ref vft) = vft_info {
        reflection_json["visible_function_tables"] = enrich_vft_metadata(vft, &reflection_json);
    }
    if let Some(ref ift) = ift_info {
        reflection_json["intersection_function_tables"] = enrich_ift_metadata(ift, &reflection_json);
    }
    let meta = trim_meta_with_buffer_address_mode(&reflection_json, patched, buffer_address_mode);

    if let Some(caps) = caps {
        let has_query = gate_query_lod(&spv, caps)?;
        let mut metadata: Value = serde_json::from_slice(&meta).map_err(|e| e.to_string())?;
        if let Some(version) = source_language_version(entry_text) {
            metadata["source_language_version"] = json!(version);
        }
        if has_query {
            gate_query_metadata(&metadata)?;
        }
        stamp_query_policy(&mut metadata, caps);
        let meta = serde_json::to_vec(&metadata).map_err(|e| e.to_string())?;
        return Ok((spv, meta));
    }

    Ok((spv, meta))
}

unsafe fn read_dep_array(
    ptr: *const RDNA4xDep,
    count: usize,
    total: &mut usize,
    label: &str,
) -> Result<Vec<(String, Vec<u8>)>, String> {
    if count > MAX_DEPS || (count > 0 && ptr.is_null()) {
        return Err(format!("bad arguments: invalid {label} array"));
    }
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let item = &*ptr.add(i);
        if item.symbol.is_null() || item.symbol_len == 0 || item.ll.is_null() || item.ll_len == 0 {
            return Err(format!("bad arguments: empty {label} entry"));
        }
        *total = total.saturating_add(item.ll_len);
        if *total > MAX_LL_BYTES {
            return Err(format!("bad arguments: {label} modules too large"));
        }
        let sym = std::str::from_utf8(std::slice::from_raw_parts(item.symbol, item.symbol_len))
            .map_err(|_| format!("bad arguments: {label} symbol is not UTF-8"))?
            .to_string();
        out.push((
            sym,
            std::slice::from_raw_parts(item.ll, item.ll_len).to_vec(),
        ));
    }
    Ok(out)
}

unsafe fn read_dynlib_array(
    ptr: *const RDNA4xDynamicLibrary,
    count: usize,
    total: &mut usize,
) -> Result<Vec<(String, Vec<u8>)>, String> {
    if count > MAX_DEPS || (count > 0 && ptr.is_null()) {
        return Err("bad arguments: invalid dynamic library array".into());
    }
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let item = &*ptr.add(i);
        if item.ll.is_null() || item.ll_len == 0 || (item.name_len > 0 && item.name.is_null()) {
            return Err("bad arguments: empty dynamic library entry".into());
        }
        *total = total.saturating_add(item.ll_len);
        if *total > MAX_LL_BYTES {
            return Err("bad arguments: dynamic libraries too large".into());
        }
        let name = if item.name_len == 0 {
            format!("dylib_{i}")
        } else {
            std::str::from_utf8(std::slice::from_raw_parts(item.name, item.name_len))
                .map_err(|_| "bad arguments: dynamic library name is not UTF-8")?
                .to_string()
        };
        out.push((
            name,
            std::slice::from_raw_parts(item.ll, item.ll_len).to_vec(),
        ));
    }
    Ok(out)
}

/// C FFI: translate AIR text with Metal 3 dynamic libraries (`MTLDynamicLibrary`), direct linked
/// functions (`.MTL_VISIBLE_FN_REF`), and runtime `MTLVisibleFunctionTable` bounded-switch
/// dispatch across compute, vertex, and fragment stages.
///
/// `capabilities` may be NULL. Outputs and return codes follow `garnetx_translate`.
///
/// # Safety
/// All pointers must be valid for their stated lengths; output pointers must be writable.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn garnetx_translate_linked_libraries(
    ll: *const u8,
    ll_len: usize,
    local_xyz: *const u32,
    deps: *const RDNA4xDep,
    ndeps: usize,
    dynlibs: *const RDNA4xDynamicLibrary,
    ndynlibs: usize,
    visible_fns: *const RDNA4xDep,
    nvisible_fns: usize,
    capabilities: *const RDNA4xBackendCapabilities,
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
    let caps = if capabilities.is_null() {
        None
    } else {
        match (*capabilities).validate() {
            Ok(valid) => Some(valid),
            Err(msg) => {
                put_err(err, err_len, &msg);
                return RC_BAD_ARGS;
            }
        }
    };
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
    let owned_deps = match read_dep_array(deps, ndeps, &mut total, "dependency") {
        Ok(v) => v,
        Err(msg) => {
            put_err(err, err_len, &msg);
            return RC_BAD_ARGS;
        }
    };
    let owned_dynlibs = match read_dynlib_array(dynlibs, ndynlibs, &mut total) {
        Ok(v) => v,
        Err(msg) => {
            put_err(err, err_len, &msg);
            return RC_BAD_ARGS;
        }
    };
    let owned_vis = match read_dep_array(visible_fns, nvisible_fns, &mut total, "visible function")
    {
        Ok(v) => v,
        Err(msg) => {
            put_err(err, err_len, &msg);
            return RC_BAD_ARGS;
        }
    };
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        translate_bytes_stage2(
            bytes,
            local,
            &owned_deps,
            &owned_dynlibs,
            &owned_vis,
            caps,
        )
    }));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}

unsafe fn read_ift_array(
    ptr: *const RDNA4xIntersectionFunction,
    count: usize,
    total: &mut usize,
) -> Result<Vec<(String, IntersectionFunctionKind, Vec<u8>)>, String> {
    if count > MAX_DEPS || (count > 0 && ptr.is_null()) {
        return Err("bad arguments: invalid intersection function array".into());
    }
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let item = &*ptr.add(i);
        if item.symbol.is_null() || item.symbol_len == 0 || item.ll.is_null() || item.ll_len == 0 {
            return Err("bad arguments: empty intersection function entry".into());
        }
        let kind = match item.kind {
            GARNETX_IFT_KIND_BOUNDING_BOX => IntersectionFunctionKind::BoundingBox,
            GARNETX_IFT_KIND_TRIANGLE_ANY_HIT => IntersectionFunctionKind::TriangleAnyHit,
            GARNETX_IFT_KIND_CURVE => IntersectionFunctionKind::Curve,
            other => {
                return Err(format!(
                    "bad arguments: unknown intersection function kind {other}"
                ));
            }
        };
        *total = total.saturating_add(item.ll_len);
        if *total > MAX_LL_BYTES {
            return Err("bad arguments: intersection function modules too large".into());
        }
        let sym = std::str::from_utf8(std::slice::from_raw_parts(item.symbol, item.symbol_len))
            .map_err(|_| "bad arguments: intersection function symbol is not UTF-8")?
            .to_string();
        out.push((
            sym,
            kind,
            std::slice::from_raw_parts(item.ll, item.ll_len).to_vec(),
        ));
    }
    Ok(out)
}

/// C FFI: translate AIR text with full Metal 3 Stage 3 support — dynamic libraries, direct
/// visible-function refs, runtime `MTLVisibleFunctionTable`, Object/Mesh shaders, hardware
/// `SPV_KHR_ray_query`, and runtime `MTLIntersectionFunctionTable` dispatch across compute and
/// render stages.
///
/// `capabilities` may be NULL. Outputs and return codes follow `garnetx_translate`.
///
/// # Safety
/// All pointers must be valid for their stated lengths; output pointers must be writable.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn garnetx_translate_stage3(
    ll: *const u8,
    ll_len: usize,
    local_xyz: *const u32,
    deps: *const RDNA4xDep,
    ndeps: usize,
    dynlibs: *const RDNA4xDynamicLibrary,
    ndynlibs: usize,
    visible_fns: *const RDNA4xDep,
    nvisible_fns: usize,
    intersection_fns: *const RDNA4xIntersectionFunction,
    nintersection_fns: usize,
    capabilities: *const RDNA4xBackendCapabilities,
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
    let caps = if capabilities.is_null() {
        None
    } else {
        match (*capabilities).validate() {
            Ok(valid) => Some(valid),
            Err(msg) => {
                put_err(err, err_len, &msg);
                return RC_BAD_ARGS;
            }
        }
    };
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
    let owned_deps = match read_dep_array(deps, ndeps, &mut total, "dependency") {
        Ok(v) => v,
        Err(msg) => {
            put_err(err, err_len, &msg);
            return RC_BAD_ARGS;
        }
    };
    let owned_dynlibs = match read_dynlib_array(dynlibs, ndynlibs, &mut total) {
        Ok(v) => v,
        Err(msg) => {
            put_err(err, err_len, &msg);
            return RC_BAD_ARGS;
        }
    };
    let owned_vis = match read_dep_array(visible_fns, nvisible_fns, &mut total, "visible function")
    {
        Ok(v) => v,
        Err(msg) => {
            put_err(err, err_len, &msg);
            return RC_BAD_ARGS;
        }
    };
    let owned_ift = match read_ift_array(intersection_fns, nintersection_fns, &mut total) {
        Ok(v) => v,
        Err(msg) => {
            put_err(err, err_len, &msg);
            return RC_BAD_ARGS;
        }
    };
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        translate_bytes_stage3(
            bytes,
            local,
            &owned_deps,
            &owned_dynlibs,
            &owned_vis,
            &owned_ift,
            caps,
        )
    }));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}
