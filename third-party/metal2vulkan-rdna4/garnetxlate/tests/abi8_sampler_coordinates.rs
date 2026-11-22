use serde_json::Value;
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT_TEMP: AtomicUsize = AtomicUsize::new(0);

fn translate(source: &str) -> (Vec<u8>, Value, String) {
    let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1]).unwrap();
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    let assembly = validate_spv("rdna4-abi8-sampler", &spv);
    (spv, metadata, assembly)
}

fn validate_spv(prefix: &str, spv: &[u8]) -> String {
    let directory = std::env::temp_dir().join(format!(
        "{prefix}-{}-{}",
        std::process::id(),
        NEXT_TEMP.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("shader.spv");
    std::fs::write(&path, spv).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "{}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let assembly = String::from_utf8(disassembly.stdout).unwrap();
    std::fs::remove_dir_all(directory).unwrap();
    assembly
}

fn binding<'a>(metadata: &'a Value, kind: &str) -> &'a Value {
    metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .find(|binding| binding["kind"] == kind)
        .unwrap_or_else(|| panic!("missing {kind}: {metadata}"))
}

fn c_translate(source: &str, linked: bool) -> (Vec<u8>, Vec<u8>) {
    let mut spv = std::ptr::null_mut();
    let mut spv_len = 0usize;
    let mut metadata = std::ptr::null_mut();
    let mut metadata_len = 0usize;
    let mut error = [0 as std::os::raw::c_char; 1024];
    let rc = unsafe {
        if linked {
            garnetxlate::garnetx_translate_linked(
                source.as_ptr(),
                source.len(),
                std::ptr::null(),
                std::ptr::null(),
                0,
                &mut spv,
                &mut spv_len,
                &mut metadata,
                &mut metadata_len,
                error.as_mut_ptr(),
                error.len(),
            )
        } else {
            garnetxlate::garnetx_translate(
                source.as_ptr(),
                source.len(),
                std::ptr::null(),
                &mut spv,
                &mut spv_len,
                &mut metadata,
                &mut metadata_len,
                error.as_mut_ptr(),
                error.len(),
            )
        }
    };
    assert_eq!(
        rc,
        garnetxlate::RC_OK,
        "{}",
        unsafe { std::ffi::CStr::from_ptr(error.as_ptr()) }.to_string_lossy()
    );
    unsafe {
        let spv_bytes = std::slice::from_raw_parts(spv, spv_len).to_vec();
        let metadata_bytes = std::slice::from_raw_parts(metadata, metadata_len).to_vec();
        garnetxlate::garnetx_free(spv, spv_len);
        garnetxlate::garnetx_free(metadata, metadata_len);
        (spv_bytes, metadata_bytes)
    }
}

fn token_uses(assembly: &str, id: &str) -> usize {
    assembly
        .lines()
        .filter_map(|line| line.split_once(" = ").map(|(_, operands)| operands))
        .map(|operands| {
            operands
                .split_whitespace()
                .filter(|token| token.trim_end_matches(',') == id)
                .count()
        })
        .sum()
}

#[test]
fn abi8_trim_meta_preserves_direct_sampler_flag_extent() {
    let metadata = serde_json::json!({
        "reflection_version": 64,
        "normalized_sampler_coordinates": true,
        "stage": "Fragment",
        "entry_point": "main",
        "bindings": [{
            "kind": "SamplerCoordinateFlags",
            "metal_index": 0,
            "descriptor": {"set": 0, "binding": 674, "count": 1},
            "declared_size": 128,
            "extent": {"Object": {"bytes": 128}},
            "access": "ReadOnly",
            "trim_me": true
        }]
    });
    let trimmed: Value = serde_json::from_slice(&garnetxlate::trim_meta(&metadata, 0)).unwrap();
    assert_eq!(
        trimmed["bindings"][0],
        serde_json::json!({
            "kind": "SamplerCoordinateFlags",
            "metal_index": 0,
            "descriptor": {"set": 0, "binding": 674, "count": 1},
            "declared_size": 128,
            "extent": {"Object": {"bytes": 128}},
            "access": "ReadOnly"
        })
    );
    assert_eq!(trimmed["normalized_sampler_coordinates"], true);
}

#[test]
fn abi8_trim_meta_matches_the_shared_native_allowlist_fixture() {
    let fixture: Value =
        serde_json::from_str(include_str!("fixtures/reflection-trim-abi8.json")).unwrap();
    let actual: Value = serde_json::from_slice(&garnetxlate::trim_meta_with_buffer_address_mode(
        &fixture["input"],
        0,
        metal2vulkan::BufferAddressTableMode::AddressLengthAccessV2,
    ))
    .unwrap();
    assert_eq!(
        actual, fixture["expected"],
        "Rust and native/Python trims must preserve the same ABI 8 reflection facts"
    );
}

#[test]
fn abi8_opt_in_and_legacy_paths_report_distinct_contract_versions() {
    let source = include_str!("fixtures/abi8/sampler-direct.ll");
    let (_, legacy) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let legacy: Value = serde_json::from_slice(&legacy).unwrap();
    assert_eq!(legacy["trimmed_from_reflection_version"], 66);
    assert_eq!(legacy["normalized_sampler_coordinates"], false);

    let (_, legacy_linked) = garnetxlate::translate_text_linked(source, [1, 1, 1], &[]).unwrap();
    let legacy_linked: Value = serde_json::from_slice(&legacy_linked).unwrap();
    assert_eq!(legacy_linked["trimmed_from_reflection_version"], 66);
    assert_eq!(legacy_linked["normalized_sampler_coordinates"], false);

    let (_, current) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1]).unwrap();
    let current: Value = serde_json::from_slice(&current).unwrap();
    assert_eq!(current["trimmed_from_reflection_version"], 66);
    assert_eq!(current["normalized_sampler_coordinates"], true);

    let (_, current_linked) =
        garnetxlate::translate_bytes_linked(source.as_bytes(), [1, 1, 1], &[]).unwrap();
    let current_linked: Value = serde_json::from_slice(&current_linked).unwrap();
    assert_eq!(current_linked["trimmed_from_reflection_version"], 66);
    assert_eq!(current_linked["normalized_sampler_coordinates"], true);

    for linked in [false, true] {
        let (_, c_metadata) = c_translate(source, linked);
        let c_metadata: Value = serde_json::from_slice(&c_metadata).unwrap();
        assert_eq!(
            c_metadata["normalized_sampler_coordinates"], true,
            "C ABI linked={linked} must select the same ABI 8 contract"
        );
    }
}

#[test]
fn abi8_direct_sampler_uses_a_stage_local_flag_word_and_normalized_sample() {
    let (_spv, metadata, assembly) = translate(include_str!("fixtures/abi8/sampler-direct.ll"));
    assert_eq!(metadata["trimmed_from_reflection_version"], 66);
    assert_eq!(metadata["normalized_sampler_coordinates"], true);
    let flags = binding(&metadata, "StageResourceMetadata");
    assert_eq!(
        flags["descriptor"],
        serde_json::json!({"set": 0, "binding": 674, "count": 1})
    );
    assert_eq!(flags["declared_size"], 384);
    assert_eq!(
        flags["extent"],
        serde_json::json!({"Object": {"bytes": 384}})
    );
    assert_eq!(flags["access"], "ReadOnly");

    assert!(assembly.contains("Binding 674"), "{assembly}");
    assert!(assembly.contains("OpImageSampleExplicitLod"), "{assembly}");
    assert!(
        assembly.contains("OpImageSampleDrefExplicitLod"),
        "{assembly}"
    );
}

#[test]
fn abi8_keeps_static_pixel_coordinates_as_compile_time_state() {
    let (_spv, metadata, _assembly) =
        translate(include_str!("fixtures/abi8/sampler-constexpr-never.ll"));
    let static_sampler = metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .find(|binding| binding["kind"] == "StaticSampler")
        .unwrap_or_else(|| panic!("missing static sampler: {metadata}"));
    assert_eq!(
        static_sampler["static_sampler"]["coordinates"], "Normalized",
        "{metadata}"
    );
    assert!(!metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .any(|binding| binding["kind"] == "SamplerCoordinateFlags"));
}

#[test]
fn abi8_static_pixel_sampler_uses_normalized_ordinary_and_compare_backends() {
    let source = include_str!("fixtures/abi8/sampler-constexpr-never.ll")
        .replace("34901797601017929", "34901797601050697");
    let (_spv, metadata, assembly) = translate(&source);
    let static_sampler = metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .find(|binding| binding["kind"] == "StaticSampler")
        .unwrap_or_else(|| panic!("missing static sampler: {metadata}"));
    assert_eq!(
        static_sampler["static_sampler"]["coordinates"], "Pixel",
        "{metadata}"
    );
    assert!(!metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .any(|binding| binding["kind"] == "SamplerCoordinateFlags"));
    assert!(assembly.contains("OpImageSampleExplicitLod"), "{assembly}");
    assert!(
        assembly.contains("OpImageSampleDrefExplicitLod"),
        "{assembly}"
    );
}

#[test]
fn abi8_mixed_direct_sampler_reports_both_backend_representations() {
    let (_spv, metadata, _assembly) = translate(include_str!("fixtures/abi8/sampler-mixed.ll"));
    let samplers = metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|binding| {
            matches!(
                binding["kind"].as_str(),
                Some("Sampler") | Some("EmbeddedArgBufferSampler")
            )
        })
        .collect::<Vec<_>>();
    assert!(!samplers.is_empty(), "{metadata}");
    assert!(samplers.iter().any(|binding| {
        binding["descriptor"]["binding"] == 160
            && binding["comparison_descriptor"]["binding"] == 736
    }));
}

#[test]
fn abi8_fragment_indirect_sampler_keeps_the_flag_descriptor_stage_local() {
    let source = include_str!("fixtures/abi8/sampler-fragment-indirect.ll");
    let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1]).unwrap();
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(metadata["stage"], "Fragment");
    assert_eq!(metadata["normalized_sampler_coordinates"], true);
    let flags = binding(&metadata, "StageResourceMetadata");
    assert_eq!(
        flags["descriptor"],
        serde_json::json!({"set": 0, "binding": 674, "count": 1}),
        "producer metadata is stage-local; native relocates fragment set0 to segment set1"
    );
    assert_eq!(flags["declared_size"], 384);
    assert_eq!(
        flags["extent"],
        serde_json::json!({"Object": {"bytes": 384}})
    );
    let assembly = validate_spv("rdna4-abi8-fragment-indirect", &spv);
    assert!(assembly.contains("DescriptorSet 0"), "{assembly}");
    assert!(assembly.contains("Binding 674"), "{assembly}");
}

#[test]
fn modern_gpu_sampler_keeps_null_remap_and_reads_flags_from_table() {
    let source = include_str!("fixtures/abi8/sampler-fragment-indirect.ll");
    let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1]).unwrap();
    let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(metadata["argument_resource_tables"]["version"], 9);
    assert!(
        !metadata["bindings"]
            .as_array()
            .unwrap()
            .iter()
            .any(|binding| binding["kind"] == "SamplerCoordinateFlags"),
        "a table stage must not also expose the direct flag SSBO: {metadata}"
    );
    let assembly = validate_spv("rdna4-abi8-fragment-table-only", &spv);
    assert!(assembly.contains("Binding 674"), "{assembly}");
    assert!(assembly.contains("Binding 672"), "{assembly}");
}

#[test]
fn abi8_flat_gpu_sampler_reads_coordinate_flags_from_the_table_tail() {
    let source = include_str!("fixtures/abi8/sampler-argument-flat.ll");
    let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1]).unwrap();
    let initial: Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(initial["trimmed_from_reflection_version"], 66);
    assert_eq!(initial["normalized_sampler_coordinates"], true);
    let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(metadata["argument_resource_tables"]["version"], 9);
    assert_eq!(
        metadata["argument_resource_tables"]["comparison_sampler"],
        true
    );
    assert_eq!(
        metadata["argument_resource_tables"]["categories"],
        serde_json::json!([0, 2])
    );

    let directory =
        std::env::temp_dir().join(format!("rdna4-abi8-gpu-flags-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("shader.spv");
    std::fs::write(&path, &spv).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "{}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    std::fs::remove_dir_all(directory).unwrap();
    let assembly = String::from_utf8(disassembly.stdout).unwrap();

    assert!(
        assembly.contains("Binding 674"),
        "ABI 9 null remaps retain the stage metadata descriptor:\n{assembly}"
    );
    let table_word_loads = |word: &str| {
        assembly
            .lines()
            .filter_map(|line| {
                let (pointer, instruction) = line.split_once(" = ")?;
                let pointer = pointer.trim();
                if !instruction.starts_with("OpAccessChain")
                    || !instruction.ends_with(&format!("%uint_0 %uint_{word}"))
                {
                    return None;
                }
                assembly.lines().find_map(|line| {
                    let (value, instruction) = line.split_once(" = ")?;
                    (instruction == format!("OpLoad %uint {pointer}")).then_some(value.trim())
                })
            })
            .collect::<Vec<_>>()
    };
    let word_four = table_word_loads("4");
    let word_five = table_word_loads("5");
    assert!(!word_four.is_empty() && !word_five.is_empty());
    let shifted = assembly
        .lines()
        .filter_map(|line| {
            let (value, instruction) = line.split_once(" = ")?;
            (instruction.starts_with("OpShiftLeftLogical %uint")
                && word_five
                    .iter()
                    .any(|word| instruction.split_whitespace().any(|token| token == *word)))
            .then_some(value.trim())
        })
        .collect::<Vec<_>>();
    let has_tail_base = assembly.lines().any(|line| {
        line.contains("OpIAdd %uint")
            && word_four.iter().any(|word| {
                shifted
                    .iter()
                    .any(|shift| line.ends_with(&format!("{word} {shift}")))
            })
    });
    assert!(
        !shifted.is_empty() && has_tail_base,
        "GPU flags must derive word4 + 2*word5 before adding the sampler token:\n{assembly}"
    );
    assert!(word_five.iter().any(|word| token_uses(&assembly, word) > 0));
}

#[test]
fn abi8_real_reference_air_normalizes_every_sampler_operation() {
    for (name, source, operation, operation_count) in [
        (
            "ordinary",
            include_str!("fixtures/abi8/reference-air/sampler_coordinates_ordinary.ll"),
            "OpImageSampleExplicitLod",
            2,
        ),
        (
            "compare",
            include_str!("fixtures/abi8/reference-air/sampler_coordinates_compare.ll"),
            "OpImageSampleDrefExplicitLod",
            6,
        ),
        (
            "gather",
            include_str!("fixtures/abi8/reference-air/sampler_coordinates_gather.ll"),
            "OpImageGather",
            2,
        ),
        (
            "gather_compare",
            include_str!("fixtures/abi8/reference-air/sampler_coordinates_gather_compare.ll"),
            "OpImageDrefGather",
            6,
        ),
    ] {
        let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1])
            .unwrap_or_else(|error| panic!("{name} reference AIR: {error}"));
        let initial: Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(
            initial["normalized_sampler_coordinates"], true,
            "{name}: {initial}"
        );
        let flags = binding(&initial, "StageResourceMetadata");
        assert_eq!(
            flags["descriptor"],
            serde_json::json!({"set": 0, "binding": 674, "count": 1}),
            "{name}: {initial}"
        );
        assert_eq!(
            flags["declared_size"], 384,
            "{name}: flags storage is a fixed 32-word ABI extent"
        );

        let assembly = validate_spv(&format!("rdna4-abi8-reference-{name}"), &spv);
        assert!(
            assembly.contains("Binding 674"),
            "{name}: direct runtime samplers must read the stage-local flag buffer:\n{assembly}"
        );
        assert_eq!(
            assembly.matches(operation).count(),
            operation_count,
            "{name}: expected {operation_count} {operation} operations:\n{assembly}"
        );

        // Every runtime sampler takes the flag-dependent coordinate path. The normalizer divides
        // by the queried image extent and selects between that value and the original coordinate.
        assert!(
            assembly.contains("OpImageQuerySizeLod")
                && assembly.contains("OpConvertUToF")
                && assembly.contains("OpFDiv")
                && assembly.contains("OpSelect"),
            "{name}: missing normalized-coordinate select:\n{assembly}"
        );
    }
}
