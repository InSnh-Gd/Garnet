// SPDX-License-Identifier: LGPL-3.0-or-later
//! AIR text sanitization, linked-function specialization, translation, and reflection trimming.

use crate::device_caps::lower_gpu_arguments_with_table_version;
use crate::source_proof::uniform_control_proof_bytes;
use metal2vulkan::passes::{QueryLodReplay, Stage, TransformOptions};
use metal2vulkan::tools::sanitize_ll_text_with_datalayout;
use serde_json::Value;
use std::path::Path;

/// Pure (no unsafe, no C types) core: text in, SPIR-V and trimmed meta JSON out.
pub fn translate_bytes(ll: &[u8], local: [u32; 3]) -> Result<(Vec<u8>, Vec<u8>), String> {
    #[cfg(feature = "test-hooks")]
    {
        if ll.starts_with(b";RDNA4X-TEST-PANIC") {
            panic!("rdna4x test hook panic");
        }
        if ll.starts_with(b";RDNA4X-TEST-EXIT") {
            // A deliberate process exit mid-translation (crash-guard fixture).
            std::process::exit(77);
        }
    }
    let text = std::str::from_utf8(ll).map_err(|e| format!("LLVM text is not UTF-8: {e}"))?;
    translate_text_abi8(text, local)
}

/// [`translate_bytes`] with authored linked dependencies (`garnetx_translate_linked`).
pub fn translate_bytes_linked(
    ll: &[u8],
    local: [u32; 3],
    deps: &[(String, Vec<u8>)],
) -> Result<(Vec<u8>, Vec<u8>), String> {
    #[cfg(feature = "test-hooks")]
    {
        if ll.starts_with(b";RDNA4X-TEST-PANIC") {
            panic!("rdna4x test hook panic");
        }
    }
    let text = std::str::from_utf8(ll).map_err(|e| format!("LLVM text is not UTF-8: {e}"))?;
    let mut texts = Vec::with_capacity(deps.len());
    for (sym, bytes) in deps {
        let t = std::str::from_utf8(bytes)
            .map_err(|e| format!("linked dependency {sym:?}: LLVM text is not UTF-8: {e}"))?;
        texts.push((sym.as_str(), t));
    }
    translate_text_linked_abi8(text, local, &texts)
}

/// `stage` is read from the `!air.*` metadata lines exactly as `metal2vulkan::detect_stage` does.
pub fn detect_stage_text(san_ll: &str) -> Result<Stage, String> {
    if san_ll.contains("!air.vertex =") {
        Ok(Stage::Vertex)
    } else if san_ll.contains("!air.fragment =") {
        Ok(Stage::Fragment)
    } else if san_ll.contains("!air.object =") {
        Ok(Stage::Object)
    } else if san_ll.contains("!air.mesh =") {
        Ok(Stage::Mesh)
    } else if san_ll.contains("!air.kernel =") {
        Ok(Stage::Kernel)
    } else {
        Err(
            "metal2vulkan: no !air.vertex/!air.fragment/!air.kernel/!air.object/!air.mesh stage metadata in module"
                .to_string(),
        )
    }
}

pub fn translate_text(ll_text: &str, local: [u32; 3]) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with(ll_text, local, None, false)
}

/// Opt-in ABI 8 translation used by the C entry points.
///
/// The ordinary Rust entry points retain the historical unnormalized-coordinate behavior for
/// existing callers. Native code crosses the C boundary and therefore selects the new contract
/// explicitly here, keeping old cached metadata and direct library consumers on their prior
/// lowering.
pub(crate) fn translate_text_abi8(ll_text: &str, local: [u32; 3]) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with(ll_text, local, None, true)
}

/// Translate with GPU-visible opaque argument tokens instead of encoding-time descriptors.
/// The consumer must implement the versioned table contract in the returned metadata.
pub fn translate_text_gpu_arguments(
    ll_text: &str,
    local: [u32; 3],
) -> Result<(Vec<u8>, Vec<u8>), String> {
    let (spv, metadata_bytes) = translate_text(ll_text, local)?;
    lower_gpu_arguments_with_table_version(&spv, &metadata_bytes, Some(7))
}

/// ABI 8 GPU-argument lowering with an explicit replay strategy for the existing fragment
/// `calculate_*_lod_texture_2d` extension.
///
/// `QuadBroadcast` is valid only after the caller proves fragment
/// `VK_SUBGROUP_FEATURE_QUAD_BIT` support and a subgroup size of at least four. Without that
/// evidence, use `DescriptorEnumeration`; the default `UniformOnly` refuses a nonuniform tuple.
pub fn translate_text_gpu_arguments_abi8_with_query_lod_replay(
    ll_text: &str,
    local: [u32; 3],
    replay: QueryLodReplay,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with_options(ll_text, local, None, true, replay)
}

/// The authored dependencies the entry reaches, transitively, each sanitised: the exact input of
/// `metal2vulkan::specialize_linked_module`. A symbol authored twice with different modules is an error;
/// a reference nobody authored is left for `specialize_linked_module` to refuse by name.
pub fn linkage_for(
    san_entry: &str,
    deps: &[(&str, &str)],
) -> Result<metal2vulkan::linked_functions::LinkedFunctionLinkage, String> {
    use metal2vulkan::linked_functions::{LinkedFunctionLinkage, LinkedFunctionReference};
    use std::collections::{BTreeMap, BTreeSet};
    let mut sanitized: BTreeMap<&str, String> = BTreeMap::new();
    for (sym, ll) in deps {
        let (s, _) = sanitize_ll_text_with_datalayout(ll);
        match sanitized.get(sym) {
            Some(prev) if *prev != s => {
                return Err(format!(
                    "linked function {sym:?} is authored twice with different modules"
                ));
            }
            _ => {
                sanitized.insert(*sym, s);
            }
        }
    }
    let mut need: Vec<String> =
        metal2vulkan::linked_functions::visible_function_reference_symbols(san_entry)?;
    need.extend(metal2vulkan::linked_functions::external_symbol_references(san_entry)?);
    let mut done: BTreeSet<String> = BTreeSet::new();
    let mut linkage = LinkedFunctionLinkage::default();
    while let Some(sym) = need.pop() {
        if !done.insert(sym.clone()) {
            continue;
        }
        let Some(module) = sanitized.get(sym.as_str()) else {
            continue; // specialize_linked_module names it
        };
        need.extend(metal2vulkan::linked_functions::visible_function_reference_symbols(module)?);
        need.extend(metal2vulkan::linked_functions::external_symbol_references(module)?);
        linkage.visible_references.push(LinkedFunctionReference {
            symbol: sym,
            module_ll: module.clone(),
        });
    }
    Ok(linkage)
}

/// [`translate_text`] with authored dependencies: (logical function name, LLVM text of its module).
pub fn translate_text_linked(
    ll_text: &str,
    local: [u32; 3],
    deps: &[(&str, &str)],
) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with(ll_text, local, Some(deps), false)
}

pub(crate) fn translate_text_linked_abi8(
    ll_text: &str,
    local: [u32; 3],
    deps: &[(&str, &str)],
) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with(ll_text, local, Some(deps), true)
}

/// `deps == None`: the unlinked path, exactly as before ABI 2 (an entry that names a visible function is refused by the translator). `Some`: the linked path.
pub(crate) fn translate_text_with(
    ll_text: &str,
    local: [u32; 3],
    deps: Option<&[(&str, &str)]>,
    normalized_sampler_coordinates: bool,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with_options(
        ll_text,
        local,
        deps,
        normalized_sampler_coordinates,
        QueryLodReplay::UniformOnly,
    )
}

pub(crate) fn translate_text_with_options(
    ll_text: &str,
    local: [u32; 3],
    deps: Option<&[(&str, &str)]>,
    normalized_sampler_coordinates: bool,
    query_lod_replay: QueryLodReplay,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    translate_text_with_mesh_options(
        ll_text,
        local,
        deps,
        normalized_sampler_coordinates,
        query_lod_replay,
        false,
    )
}

pub(crate) fn translate_text_with_mesh_options(
    ll_text: &str,
    local: [u32; 3],
    deps: Option<&[(&str, &str)]>,
    normalized_sampler_coordinates: bool,
    query_lod_replay: QueryLodReplay,
    emulated_mesh_payload: bool,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    let (san_entry, datalayout) = sanitize_ll_text_with_datalayout(ll_text);
    let stage = detect_stage_text(&san_entry)?;
    let san_ll = match deps {
        None => san_entry,
        Some(deps) => {
            let linkage = linkage_for(&san_entry, deps)?;
            if !san_entry.contains(".MTL_VISIBLE_FN_REF")
                && !san_entry.contains("!air.visible_function_references")
                && linkage.is_empty()
            {
                san_entry // nothing to resolve: byte-identical to the unlinked path
            } else {
                metal2vulkan::specialize_linked_module(&san_entry, stage, &linkage)?
            }
        }
    };
    // The same options the command-line tool builds: only the nominal local size and the Metal
    // default vertex-amplification count of 1 are set.
    let mut options = TransformOptions {
        kernel_local_size: local,
        kernel_dispatch: None,
        raster_sample_count: None,
        vertex_amplification_count: 1,
        ..TransformOptions::default()
    };
    if normalized_sampler_coordinates {
        options = options.with_normalized_sampler_coordinates();
    }
    options = options
        .with_query_lod_replay(query_lod_replay)
        .with_hardware_ray_query();
    if emulated_mesh_payload {
        options = options.with_emulated_mesh_payload();
    }
    // Never used on the Skip path (it reaches only the scratch-file helpers, which never run).
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
    // Text round trip, like the daemon's (the command-line tool writes the JSON, Python reads it):
    // `to_value` would widen `f32` fields to `f64` and print them with spurious digits.
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
    let meta = trim_meta_with_buffer_address_mode(&reflection_json, patched, buffer_address_mode);
    Ok((spv, meta))
}

/// Port of `patch_storage_formats` (pc-translate.py): the runtime format of a storage image is not
/// known from AIR, so declare the image `Unknown` and add the StorageImage{Read,Write}WithoutFormat
/// capabilities the module's accesses need. Returns the bytes and the number of image types patched.
pub fn patch_storage_formats(b: &[u8], meta: &Value) -> (Vec<u8>, usize) {
    if !b.len().is_multiple_of(4) || b.len() < 20 {
        return (b.to_vec(), 0);
    }
    let mut w: Vec<u32> = b
        .chunks_exact(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect();
    let mut acc: Vec<&str> = Vec::new();
    if let Some(bs) = meta.get("bindings").and_then(Value::as_array) {
        for x in bs {
            if x.get("kind").and_then(Value::as_str) == Some("StorageImage") {
                if let Some(a) = x.get("access").and_then(Value::as_str) {
                    acc.push(a);
                } else {
                    acc.push("");
                }
            }
        }
    }
    // Vulkan forbids ImageFormat::Unknown (0) on any storage image targeted by
    // OpImageTexelPointer (texture atomics). Collect the OpTypeImage ids referenced by
    // OpImageTexelPointer so their explicit atomic format (R32ui/R32i/R64ui/R64i) stays intact.
    let mut ptr_pointee = std::collections::HashMap::<u32, u32>::new();
    let mut val_type = std::collections::HashMap::<u32, u32>::new();
    let mut texel_ptr_images = std::collections::HashSet::<u32>::new();
    {
        let mut scan = 5usize;
        while scan < w.len() {
            let op = w[scan] & 0xffff;
            let wc = (w[scan] >> 16) as usize;
            if wc == 0 || scan + wc > w.len() {
                break;
            }
            match op {
                32 if wc >= 4 => {
                    // OpTypePointer: [hdr, result_id, storage_class, pointee_type]
                    ptr_pointee.insert(w[scan + 1], w[scan + 3]);
                }
                55 | 59 | 65 | 66 | 67 | 70 | 83 if wc >= 3 => {
                    // OpFunctionParameter / OpVariable / Op*AccessChain / OpCopyObject
                    val_type.insert(w[scan + 2], w[scan + 1]);
                }
                60 if wc >= 4 => {
                    // OpImageTexelPointer: [hdr, result_type, result_id, image_ptr, coord, sample]
                    if let Some(&ptr_ty) = val_type.get(&w[scan + 3]) {
                        if let Some(&img_ty) = ptr_pointee.get(&ptr_ty) {
                            texel_ptr_images.insert(img_ty);
                        }
                    }
                }
                _ => {}
            }
            scan += wc;
        }
    }
    let has = |n: &str| acc.contains(&n);
    let acc_empty = acc.is_empty();
    let mut p = 5usize;
    let mut need: Vec<u32> = Vec::new();
    let mut have: Vec<u32> = Vec::new();
    let mut patched = 0usize;
    let mut lastcap: Option<usize> = None;
    while p < w.len() {
        let op = w[p] & 0xffff;
        let wc = (w[p] >> 16) as usize;
        if wc == 0 {
            // Malformed stream: stop rather than loop.
            break;
        }
        if op == 17 && p + 1 < w.len() {
            lastcap = Some(p + wc);
            have.push(w[p + 1]);
        }
        if op == 25
            && p + 8 < w.len()
            && w[p + 7] == 2
            && w[p + 8] != 0
            && !texel_ptr_images.contains(&w[p + 1])
        {
            w[p + 8] = 0;
            patched += 1;
            if (has("ReadOnly") || has("ReadWrite") || acc_empty) && !need.contains(&55) {
                need.push(55);
            }
            if (has("WriteOnly") || has("ReadWrite") || acc_empty) && !need.contains(&56) {
                need.push(56);
            }
        }
        p += wc;
    }
    if patched == 0 {
        return (b.to_vec(), 0);
    }
    if need.is_empty() {
        need = vec![55, 56];
    }
    need.sort_unstable();
    let mut ins: Vec<u32> = Vec::new();
    for c in need {
        if !have.contains(&c) {
            ins.push((2 << 16) | 17);
            ins.push(c);
        }
    }
    if let Some(at) = lastcap {
        if at <= w.len() {
            let tail = w.split_off(at);
            w.extend(ins);
            w.extend(tail);
        }
    }
    let mut out = Vec::with_capacity(w.len() * 4);
    for x in w {
        out.extend_from_slice(&x.to_le_bytes());
    }
    (out, patched)
}

/// Port of `trim` (pc-translate.py), serialised compactly in the daemon's key order.
pub fn trim_meta(m: &Value, patched: usize) -> Vec<u8> {
    trim_meta_with_buffer_address_mode(m, patched, metal2vulkan::BufferAddressTableMode::LegacyV1)
}

/// Trim reflection while carrying the finished shader's buffer-address table contract.
///
/// A v1 table contains only 32 addresses. Address/length/access mode emits a 768-byte table and
/// therefore states both fields explicitly; legacy output omits them so existing metadata stays
/// byte-identical.
pub fn trim_meta_with_buffer_address_mode(
    m: &Value,
    patched: usize,
    mode: metal2vulkan::BufferAddressTableMode,
) -> Vec<u8> {
    let get = |k: &str| m.get(k).cloned().unwrap_or(Value::Null);
    let mut bindings: Vec<Value> = Vec::new();
    if let Some(bs) = m.get("bindings").and_then(Value::as_array) {
        for x in bs {
            let mut o = serde_json::Map::new();
            if matches!(
                x.get("kind").and_then(Value::as_str),
                Some("SamplerCoordinateFlags" | "StageResourceMetadata")
            ) {
                for k in ["kind", "metal_index"] {
                    if let Some(v) = x.get(k) {
                        o.insert(k.to_string(), v.clone());
                    }
                }
                if let Some(descriptor) = x.get("descriptor") {
                    o.insert(
                        "descriptor".to_string(),
                        Value::Object(
                            ["set", "binding", "count"]
                                .into_iter()
                                .filter_map(|k| {
                                    descriptor
                                        .get(k)
                                        .map(|value| (k.to_string(), value.clone()))
                                })
                                .collect(),
                        ),
                    );
                }
                for k in ["declared_size", "access"] {
                    if let Some(v) = x.get(k) {
                        o.insert(k.to_string(), v.clone());
                    }
                }
                if let Some(bytes) = x
                    .get("extent")
                    .and_then(|extent| extent.get("Object"))
                    .and_then(|object| object.get("bytes"))
                {
                    o.insert(
                        "extent".to_string(),
                        serde_json::json!({"Object": {"bytes": bytes}}),
                    );
                }
                bindings.push(Value::Object(o));
                continue;
            }
            for k in [
                "kind",
                "metal_index",
                "descriptor",
                "comparison_descriptor",
                "param_index",
                "address_space",
                "access",
                "declared_size",
                "extent",
                "type_name",
                "static_sampler",
                "texture_shape",
            ] {
                if let Some(v) = x.get(k) {
                    o.insert(k.to_string(), v.clone());
                }
            }
            // The runtime cannot reconstruct the owner and byte offset of an indirect resource.
            if let Some(source) = x.get("embedded_source").filter(|source| !source.is_null()) {
                o.insert("embedded_source".to_string(), source.clone());
            }
            bindings.push(Value::Object(o));
        }
    }
    let mut out = String::from("{");
    let mut first = true;
    let mut put = |out: &mut String, k: &str, v: &Value| {
        if !first {
            out.push(',');
        }
        first = false;
        out.push_str(&serde_json::to_string(k).unwrap());
        out.push(':');
        out.push_str(&ordered(v));
    };
    put(
        &mut out,
        "trimmed_from_reflection_version",
        &get("reflection_version"),
    );
    if let Some(normalized) = m.get("normalized_sampler_coordinates") {
        put(&mut out, "normalized_sampler_coordinates", normalized);
    }
    put(&mut out, "stage", &get("stage"));
    put(&mut out, "entry_point", &get("entry_point"));
    put(&mut out, "bindings", &Value::Array(bindings));
    for k in ["buffer_address_table_version", "buffer_address_table_bytes"] {
        if let Some(v) = m.get(k) {
            put(&mut out, k, v);
        }
    }
    if mode == metal2vulkan::BufferAddressTableMode::RegistryV3 {
        put(&mut out, "buffer_address_table_version", &Value::from(3u32));
        put(&mut out, "buffer_address_table_bytes", &Value::from(800u32));
        put(
            &mut out,
            "buffer_address_table_row_stride_bytes",
            &Value::from(24u32),
        );
        put(
            &mut out,
            "buffer_address_table_dynamic_extent",
            &Value::from(true),
        );
    } else if mode == metal2vulkan::BufferAddressTableMode::AddressLengthAccessV2 {
        if m.get("buffer_address_table_version").is_none() {
            put(&mut out, "buffer_address_table_version", &Value::from(2u32));
        }
        if m.get("buffer_address_table_bytes").is_none() {
            put(&mut out, "buffer_address_table_bytes", &Value::from(768u32));
        }
    }
    for key in ["argument_buffer_fields", "argument_buffer_roots"] {
        if let Some(fields) = m
            .get(key)
            .filter(|fields| fields.as_array().is_some_and(|fields| !fields.is_empty()))
        {
            put(&mut out, key, fields);
        }
    }
    if let Some(tables) = m
        .get("argument_resource_tables")
        .filter(|tables| !tables.is_null())
    {
        put(&mut out, "argument_resource_tables", tables);
    }
    if let Some(tessellation) = m.get("tessellation").filter(|t| !t.is_null()) {
        put(&mut out, "tessellation", tessellation);
    }
    if let Some(object) = m.get("object").filter(|o| !o.is_null()) {
        put(&mut out, "object", object);
    }
    if let Some(mesh) = m.get("mesh").filter(|v| !v.is_null()) {
        put(&mut out, "mesh", mesh);
    }
    let has_mesh = m.get("mesh").is_some_and(|v| !v.is_null());
    if let Some(varyings) = m.get("varyings").filter(|v| {
        v.as_array().is_some_and(|arr| {
            !arr.is_empty()
                && (has_mesh
                    || arr
                        .iter()
                        .any(|item| item.get("per_primitive").and_then(Value::as_bool) == Some(true)))
        })
    }) {
        put(&mut out, "varyings", varyings);
    }
    if let Some(accel) = m
        .get("acceleration_structures")
        .filter(|a| a.as_array().is_some_and(|arr| !arr.is_empty()))
    {
        put(&mut out, "acceleration_structures", accel);
    }
    if let Some(vft) = m
        .get("visible_function_tables")
        .filter(|vft| !vft.is_null())
    {
        put(&mut out, "visible_function_tables", vft);
    }
    if let Some(ift) = m
        .get("intersection_function_tables")
        .filter(|ift| !ift.is_null())
    {
        put(&mut out, "intersection_function_tables", ift);
    }
    if let Some(proof) = m
        .get("uniform_control_proof")
        .filter(|proof| !proof.is_null())
    {
        put(&mut out, "uniform_control_proof", proof);
    }
    for k in [
        "vertex_attributes",
        "local_size",
        "max_work_group_size",
        "kernel_dispatch",
        "function_constants",
        "descriptor_layout",
        "runtime_sampler_specializations",
        "runtime_storage_image_specializations",
    ] {
        put(&mut out, k, &get(k));
    }
    if patched > 0 {
        put(
            &mut out,
            "storage_image_format_patch",
            &Value::String("Unknown + StorageImage{Read,Write}WithoutFormat".to_string()),
        );
    }
    out.push('}');
    out.into_bytes()
}

fn ordered(v: &Value) -> String {
    serde_json::to_string(v).unwrap_or_else(|_| "null".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use metal2vulkan::BufferAddressTableMode;
    use serde_json::json;

    #[test]
    fn trim_meta_preserves_non_null_tessellation_and_omits_null() {
        let base = json!({
            "reflection_version": 66,
            "stage": "Vertex",
            "entry_point": "main",
            "bindings": [],
            "vertex_attributes": [],
            "local_size": null,
            "max_work_group_size": null,
            "kernel_dispatch": null,
            "function_constants": [],
            "descriptor_layout": {"sets": []},
            "runtime_sampler_specializations": [],
            "runtime_storage_image_specializations": [],
            "tessellation": null
        });
        let trimmed_null = trim_meta_with_buffer_address_mode(
            &base,
            0,
            BufferAddressTableMode::LegacyV1,
        );
        let parsed_null: Value = serde_json::from_slice(&trimmed_null).unwrap();
        assert!(parsed_null.get("tessellation").is_none());

        let mut with_tess = base.clone();
        with_tess["stage"] = json!("TessellationEvaluation");
        with_tess["tessellation"] = json!({
            "domain": "Quad",
            "control_point_count": 4,
            "control_point_locations": [0, 1],
            "patch_input_locations": [2],
            "control_point_attributes": [{"location": 0, "type_name": "float2"}],
            "patch_attributes": [{"location": 2, "type_name": "float4"}],
            "instance_id": {"location": 3, "type_name": "uint"},
            "amplification_id": null,
            "amplification_count": null
        });
        let trimmed_tess = trim_meta_with_buffer_address_mode(
            &with_tess,
            0,
            BufferAddressTableMode::LegacyV1,
        );
        let parsed_tess: Value = serde_json::from_slice(&trimmed_tess).unwrap();
        assert_eq!(parsed_tess["tessellation"]["domain"], json!("Quad"));
        assert_eq!(parsed_tess["tessellation"]["control_point_count"], json!(4));
        assert_eq!(parsed_tess["tessellation"]["instance_id"]["location"], json!(3));
    }
}

