// SPDX-License-Identifier: LGPL-3.0-or-later
//! Device capability policy checks and GPU argument resource table lowering.

use crate::source_proof::verify_uniform_control_proof;
use metal2vulkan::passes::QueryLodReplay;
use serde_json::Value;

/// Lower an already translated stage, including one built from linked AIR dependencies.
/// The original ABI and cached static bindings remain usable by existing consumers.
pub fn lower_gpu_arguments(
    spv: &[u8],
    metadata_bytes: &[u8],
) -> Result<(Vec<u8>, Vec<u8>), String> {
    let metadata: Value =
        serde_json::from_slice(metadata_bytes).map_err(|error| error.to_string())?;
    let normalized = normalized_sampler_coordinates(&metadata)?;
    lower_gpu_arguments_with_table_version(
        spv,
        metadata_bytes,
        Some(if normalized { 8 } else { 7 }),
    )
}

fn has_image_query_table(spv: &[u8]) -> bool {
    let words = spv
        .chunks_exact(4)
        .map(|word| u32::from_le_bytes(word.try_into().unwrap()))
        .collect::<Vec<_>>();
    let mut cursor = 5;
    while cursor < words.len() {
        let count = (words[cursor] >> 16) as usize;
        if count == 0 || count > words.len() - cursor {
            return false;
        }
        // OpDecorate target Binding 673 identifies the query metadata interface.
        if count == 4
            && words[cursor] & 0xffff == 71
            && words[cursor + 2] == 33
            && words[cursor + 3] == 673
        {
            return true;
        }
        cursor += count;
    }
    false
}

fn normalized_sampler_coordinates(metadata: &Value) -> Result<bool, String> {
    let normalized = match metadata.get("normalized_sampler_coordinates") {
        None => false,
        Some(Value::Bool(normalized)) => *normalized,
        Some(_) => {
            return Err("normalized_sampler_coordinates metadata must be a boolean".into());
        }
    };
    if normalized
        && metadata
            .get("trimmed_from_reflection_version")
            .and_then(Value::as_u64)
            .is_none_or(|version| !matches!(version, 64..=66))
    {
        return Err(
            "normalized sampler coordinates require trimmed reflection version 64..66".into(),
        );
    }
    Ok(normalized)
}

fn validate_argument_buffer_roots(metadata: &Value) -> Result<(), String> {
    let Some(roots) = metadata.get("argument_buffer_roots") else {
        return Ok(());
    };
    let invalid = || "invalid declared argument-buffer roots".to_string();
    let roots = roots.as_array().ok_or_else(invalid)?;
    if metadata["trimmed_from_reflection_version"] != 66 {
        return Err(invalid());
    }
    let mut previous = None;
    for root in roots {
        let root = root
            .as_u64()
            .filter(|root| *root < 32)
            .ok_or_else(invalid)?;
        if previous.is_some_and(|previous| previous >= root)
            || !metadata["bindings"].as_array().is_some_and(|bindings| {
                bindings.iter().any(|binding| {
                    binding["kind"] == "Buffer"
                        && binding["metal_index"] == root
                        && binding["param_index"].as_u64().is_some()
                        && binding["descriptor"]["set"] == 0
                        && binding["descriptor"]["binding"] == root
                        && binding["descriptor"]["count"] == 1
                })
            })
        {
            return Err(invalid());
        }
        previous = Some(root);
    }
    Ok(())
}

pub(crate) fn lower_gpu_arguments_with_table_version(
    spv: &[u8],
    metadata_bytes: &[u8],
    requested_table_version: Option<u32>,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    lower_gpu_arguments_with_table_version_and_replay(
        spv,
        metadata_bytes,
        requested_table_version,
        QueryLodReplay::UniformOnly,
    )
}

pub(crate) fn lower_gpu_arguments_with_table_version_and_replay(
    spv: &[u8],
    metadata_bytes: &[u8],
    requested_table_version: Option<u32>,
    replay: QueryLodReplay,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    if spv.len() < 20 || !spv.len().is_multiple_of(4) || spv[..4] != [0x03, 0x02, 0x23, 0x07] {
        return Err("invalid cached SPIR-V for GPU argument lowering".into());
    }
    let mut metadata: Value =
        serde_json::from_slice(metadata_bytes).map_err(|error| error.to_string())?;
    validate_argument_buffer_roots(&metadata)?;
    let normalized = normalized_sampler_coordinates(&metadata)?;
    let prior_table = metadata
        .get("argument_resource_tables")
        .map(|table| {
            serde_json::from_value::<metal2vulkan::reflect::ArgumentResourceTables>(table.clone())
                .map_err(|error| format!("invalid GPU argument table metadata: {error}"))
        })
        .transpose()?;
    let modern = metadata
        .get("trimmed_from_reflection_version")
        .and_then(Value::as_u64)
        .is_some_and(|version| matches!(version, 65 | 66));
    let has_flat_texel = metadata["bindings"].as_array().is_some_and(|bindings| {
        bindings.iter().any(|binding| {
            binding["kind"] == "EmbeddedArgBufferTexture"
                && binding["descriptor"]["set"] == 0
                && binding["texture_shape"]["dimension"] == "Buffer"
        })
    });
    let legacy_table = !normalized && prior_table.as_ref().is_some_and(|table| table.version == 7);
    let table_version = if modern && !legacy_table {
        if has_flat_texel {
            10
        } else {
            9
        }
    } else {
        requested_table_version.unwrap_or(7)
    };
    if !matches!(table_version, 9 | 10) && (table_version == 8) != normalized {
        return Err("GPU argument table version and sampler coordinate contract disagree".into());
    }
    if let Some(table) = metadata.get("argument_resource_tables") {
        let prior_version = table.get("version").and_then(Value::as_u64);
        if prior_version != Some(u64::from(table_version))
            && !(table_version == 10 && prior_version == Some(9))
        {
            return Err("GPU argument table metadata disagrees with sampler coordinates".into());
        }
        if metadata["bindings"].as_array().is_some_and(|bindings| {
            bindings
                .iter()
                .any(|binding| binding["kind"] == "SamplerCoordinateFlags")
        }) {
            return Err(
                "GPU argument table cannot share a stage with direct sampler-coordinate flags"
                    .into(),
            );
        }
        let flat_fields = metadata["bindings"].as_array().is_some_and(|bindings| {
            bindings.iter().any(|binding| {
                matches!(
                    binding["kind"].as_str(),
                    Some("EmbeddedArgBufferTexture" | "EmbeddedArgBufferSampler")
                ) && binding["descriptor"]["set"] == 0
            })
        });
        if !flat_fields {
            let table = prior_table
                .as_ref()
                .ok_or("missing GPU argument table metadata")?;
            metal2vulkan::passes::validate_argument_resource_tables(spv, table)?;
            return Ok((spv.to_vec(), metadata_bytes.to_vec()));
        }
    }
    let null_shapes = prior_table
        .as_ref()
        .map(|table| table.null_shapes.clone())
        .unwrap_or_default();
    let mut roots = std::collections::BTreeSet::new();
    if let Some(prior_roots) = metadata
        .get("argument_resource_tables")
        .and_then(|table| table.get("roots"))
    {
        for root in prior_roots.as_array().ok_or("invalid GPU argument roots")? {
            let buffer = root["buffer_index"]
                .as_u64()
                .filter(|index| *index < 32)
                .ok_or("invalid GPU argument root buffer")?;
            if root["binding"].as_u64() != Some(704 + buffer) {
                return Err("invalid GPU argument root binding".into());
            }
            roots.insert(buffer as u32);
        }
    }
    let bindings = metadata["bindings"]
        .as_array_mut()
        .ok_or("missing shader bindings")?;
    let mut fields = Vec::new();
    let mut categories = prior_table
        .as_ref()
        .map(|table| &table.categories)
        .into_iter()
        .flatten()
        .copied()
        .collect::<std::collections::BTreeSet<_>>();
    let mut comparison_sampler = prior_table
        .as_ref()
        .is_some_and(|table| table.comparison_sampler);
    for binding in bindings {
        if binding["kind"] != "EmbeddedArgBufferTexture"
            && binding["kind"] != "EmbeddedArgBufferSampler"
        {
            continue;
        }
        if binding["descriptor"]["set"] != 0 {
            continue;
        }
        let coordinate = |key: &str| {
            binding["embedded_source"][key]
                .as_u64()
                .and_then(|value| u32::try_from(value).ok())
                .ok_or("invalid GPU argument coordinate")
        };
        let owner = coordinate("buffer_index")?;
        let offset = coordinate("field_offset")?;
        let descriptor = binding["descriptor"]["binding"]
            .as_u64()
            .and_then(|value| u32::try_from(value).ok())
            .ok_or("invalid GPU argument descriptor")?;
        roots.insert(owner);
        let category: u32 = if binding["kind"] == "EmbeddedArgBufferSampler" {
            2
        } else if binding["texture_shape"]["dimension"] == "Buffer" {
            if binding["texture_shape"]["writable"] == true {
                4
            } else {
                3
            }
        } else if binding["texture_shape"]["writable"] == true {
            1
        } else {
            0
        };
        categories.insert(category);
        fields.push(metal2vulkan::passes::ArgumentResourceField {
            binding: descriptor,
            buffer_index: owner,
            byte_offset: offset,
        });
        if category == 2 {
            let comparison_descriptor = match binding.get("comparison_descriptor") {
                None => None,
                Some(descriptor) => Some(
                    descriptor
                        .get("binding")
                        .and_then(Value::as_u64)
                        .and_then(|binding| u32::try_from(binding).ok())
                        .ok_or("invalid GPU comparison sampler descriptor")?,
                ),
            };
            if let Some(comparison_descriptor) = comparison_descriptor {
                fields.push(metal2vulkan::passes::ArgumentResourceField {
                    binding: comparison_descriptor,
                    buffer_index: owner,
                    byte_offset: offset,
                });
            }
            let primary_is_comparison =
                descriptor >= metal2vulkan::reflect::COMPARISON_SAMPLER_BINDING_BASE;
            comparison_sampler |= comparison_descriptor.is_some() || primary_is_comparison;
        }
        let (set, descriptor_binding) = match category {
            3 => (2, 1),
            4 => (3, 1),
            category => (2 + category, 0),
        };
        binding["descriptor"] =
            serde_json::json!({"set": set, "binding": descriptor_binding, "count": 0});
        if category == 2 {
            if let Some(comparison) = binding.get_mut("comparison_descriptor") {
                *comparison = serde_json::json!({"set": 4, "binding": 0, "count": 0});
            }
        }
    }
    let uniform_control_proof = verify_uniform_control_proof(&metadata, spv)?;
    if fields.is_empty() {
        return Ok((spv.to_vec(), metadata_bytes.to_vec()));
    }
    let (spv, null_shapes, has_metadata) =
        metal2vulkan::passes::lower_argument_resource_tables_versioned_with_query_lod_replay(
            spv,
            &fields,
            table_version,
            normalized,
            &null_shapes,
            uniform_control_proof.as_ref(),
            replay,
        )?;
    metadata["argument_resource_tables"] = serde_json::json!({
        "version": table_version, "table_binding": 672, "categories": categories,
        "comparison_sampler": comparison_sampler,
        "roots": roots.into_iter().map(|buffer| serde_json::json!({"buffer_index": buffer, "binding": 704 + buffer})).collect::<Vec<_>>()
    });
    if let Some(binding) = prior_table
        .as_ref()
        .and_then(|table| table.image_query_binding)
    {
        metadata["argument_resource_tables"]["image_query_binding"] = serde_json::json!(binding);
    }
    let is_emulated_mesh_stage = metadata
        .get("object")
        .and_then(|o| o.get("emulated_payload"))
        .and_then(Value::as_bool)
        == Some(true)
        || metadata
            .get("mesh")
            .and_then(|m| m.get("emulated_payload"))
            .and_then(Value::as_bool)
            == Some(true);
    let has_emulated_payload_ssbo = metadata
        .get("bindings")
        .and_then(Value::as_array)
        .is_some_and(|bs| {
            bs.iter()
                .any(|b| b.get("kind").and_then(Value::as_str) == Some("MeshEmulatedPayload"))
        });
    if has_emulated_payload_ssbo && has_metadata {
        return Err(
            "GPU argument resource metadata (binding 674) collides with emulated Object/Mesh payload binding 674"
                .into(),
        );
    }
    if !is_emulated_mesh_stage && has_image_query_table(&spv) {
        metadata["argument_resource_tables"]["image_query_binding"] = serde_json::json!(673);
    }
    if table_version >= 9 {
        metadata["argument_resource_tables"]["null_shapes"] = serde_json::json!(null_shapes);
        if let Some(bindings) = metadata["bindings"].as_array_mut() {
            if !has_metadata {
                bindings.retain(|binding| binding["kind"] != "StageResourceMetadata");
            }
        }
        if has_metadata {
            let bindings = metadata["bindings"]
                .as_array_mut()
                .ok_or("missing shader bindings")?;
            if !bindings
                .iter()
                .any(|binding| binding["kind"] == "StageResourceMetadata")
            {
                bindings.push(serde_json::json!({"kind":"StageResourceMetadata", "metal_index":0, "descriptor":{"set":0,"binding":674,"count":1}, "declared_size":384, "extent":{"Object":{"bytes":384}}, "access":"ReadOnly"}));
            }
        }
    }
    if uniform_control_proof.is_some() {
        metadata
            .as_object_mut()
            .expect("metadata was indexed as an object")
            .remove("uniform_control_proof");
    }
    if table_version == 8 {
        if let Some(bindings) = metadata["bindings"].as_array_mut() {
            bindings.retain(|binding| binding["kind"] != "SamplerCoordinateFlags");
        }
    }
    Ok((
        spv,
        serde_json::to_vec(&metadata).map_err(|error| error.to_string())?,
    ))
}

