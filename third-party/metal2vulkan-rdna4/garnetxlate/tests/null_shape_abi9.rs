use serde_json::Value;
use std::process::Command;

#[test]
fn direct_and_graph_stage_metadata_use_the_exact_native_binding_schema() {
    for source in [
        include_str!("fixtures/abi8/sampler-direct.ll"),
        include_str!("fixtures/argument_resource_graph.ll"),
    ] {
        let (_, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1]).unwrap();
        let metadata: Value = serde_json::from_slice(&metadata).unwrap();
        let binding = metadata["bindings"]
            .as_array()
            .unwrap()
            .iter()
            .find(|binding| binding["kind"] == "StageResourceMetadata")
            .unwrap();
        assert_eq!(
            binding,
            &serde_json::json!({
                "kind": "StageResourceMetadata", "metal_index": 0,
                "descriptor": {"set":0,"binding":674,"count":1},
                "declared_size":384, "extent":{"Object":{"bytes":384}},
                "access":"ReadOnly"
            })
        );
    }
}

fn validate(bytes: &[u8], label: &str) -> String {
    let dir = std::env::temp_dir().join(format!(
        "null_abi9_wrapper_{}_{}",
        std::process::id(),
        label
    ));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("shader.spv");
    std::fs::write(&path, bytes).unwrap();
    let validation = Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "{}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = Command::new("spirv-dis").arg(&path).output().unwrap();
    String::from_utf8(disassembly.stdout).unwrap()
}

#[test]
fn modern_cached_fields_get_registry_and_remap_metadata() {
    let source = include_str!("fixtures/argument_resource_arrays.ll");
    let (before, metadata) = garnetxlate::translate_text(source, [1, 1, 1]).expect("translate");
    let (after, metadata) =
        garnetxlate::lower_gpu_arguments(&before, &metadata).expect("ABI9 postpass");
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(metadata["trimmed_from_reflection_version"], 66);
    assert_eq!(metadata["normalized_sampler_coordinates"], false);
    assert_eq!(metadata["argument_resource_tables"]["version"], 9);
    assert!(!metadata["argument_resource_tables"]["null_shapes"]
        .as_array()
        .unwrap()
        .is_empty());
    assert!(metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .any(
            |binding| binding["kind"] == "StageResourceMetadata" && binding["declared_size"] == 384
        ));
    let spv = validate(&after, "fields");
    assert!(spv.contains("Binding 674") && spv.contains("Binding 672"));
}

#[test]
fn graph_registry_preserves_explicit_coordinate_option() {
    let source = include_str!("fixtures/argument_resource_graph.ll");
    for normalized in [false, true] {
        let (bytes, metadata) = if normalized {
            garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1])
        } else {
            garnetxlate::translate_text(source, [1, 1, 1])
        }
        .expect("graph translates");
        let metadata: Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(metadata["normalized_sampler_coordinates"], normalized);
        assert_eq!(metadata["argument_resource_tables"]["version"], 9);
        assert_eq!(
            metadata["argument_resource_tables"]["null_shapes"],
            serde_json::json!([1])
        );
        validate(
            &bytes,
            if normalized {
                "graph_normalized"
            } else {
                "graph_legacy"
            },
        );
    }
}

#[test]
fn completed_cached_tables_validate_registry_before_idempotent_return() {
    let source = include_str!("fixtures/argument_resource_graph.ll");
    let (bytes, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1]).unwrap();
    let value: Value = serde_json::from_slice(&metadata).unwrap();
    let (again, same_metadata) = garnetxlate::lower_gpu_arguments(&bytes, &metadata).unwrap();
    assert_eq!(again, bytes);
    assert_eq!(same_metadata, metadata);
    for (field, invalid, expected) in [
        (
            "version",
            serde_json::json!(11),
            "unknown argument_resource_tables version",
        ),
        (
            "null_shapes",
            serde_json::json!([1, 1]),
            "strictly ascending",
        ),
        ("null_shapes", serde_json::json!([1024]), "reserved"),
    ] {
        let mut broken = value.clone();
        broken["argument_resource_tables"][field] = invalid;
        let error = garnetxlate::lower_gpu_arguments(&bytes, &serde_json::to_vec(&broken).unwrap())
            .unwrap_err();
        assert!(error.contains(expected), "{error}");
    }
    let mut missing = value;
    missing["argument_resource_tables"]
        .as_object_mut()
        .unwrap()
        .remove("null_shapes");
    let error = garnetxlate::lower_gpu_arguments(&bytes, &serde_json::to_vec(&missing).unwrap())
        .unwrap_err();
    assert!(error.contains("missing field `null_shapes`"), "{error}");
}

#[test]
fn cached_lowering_relocates_an_earlier_static_shape_and_adds_token_base() {
    let source = include_str!("fixtures/argument_resource_arrays.ll").replacen("  ret void", "  %null3d = call ptr addrspace(1) @air.get_null_texture_3d()\n  %width3d = call i32 @air.get_width_texture_3d(ptr addrspace(1) %null3d, i32 0)\n  store i32 %width3d, ptr addrspace(1) %out\n  ret void", 1)
        + "\ndeclare ptr addrspace(1) @air.get_null_texture_3d()\ndeclare i32 @air.get_width_texture_3d(ptr addrspace(1), i32)\n";
    let (before, metadata) =
        garnetxlate::translate_text(&source, [1, 1, 1]).expect("static-null stage translates");
    let initial: Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(
        initial["argument_resource_tables"]["null_shapes"],
        serde_json::json!([2])
    );
    let before_text = validate(&before, "before_cached_reloc");
    assert!(!before_text.contains("Binding 672"));
    let (after, metadata) =
        garnetxlate::lower_gpu_arguments(&before, &metadata).expect("cached shape union");
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(
        metadata["argument_resource_tables"]["null_shapes"],
        serde_json::json!([1, 2])
    );
    let after_text = validate(&after, "after_cached_reloc");
    assert!(
        after_text.contains("Binding 672") && after_text.contains("%uint_33"),
        "{after_text}"
    );
    let metadata_load = after_text
        .lines()
        .find_map(|line| {
            let (pointer, instruction) = line.split_once(" = ")?;
            if !(instruction.contains("OpAccessChain %_ptr_StorageBuffer_uint")
                && instruction.ends_with("%uint_0 %uint_33"))
            {
                return None;
            }
            after_text.lines().find_map(|line| {
                let (loaded, instruction) = line.split_once(" = ")?;
                (instruction == format!("OpLoad %uint {}", pointer.trim()))
                    .then_some(loaded.trim().to_string())
            })
        })
        .expect("3D remap moved from global slot0 to slot1");
    assert!(
        after_text.lines().any(|line| line.contains("OpIAdd %uint")
            && line.split_whitespace().any(|word| word == metadata_load)),
        "old static remap must add the new token count: {after_text}"
    );
}
