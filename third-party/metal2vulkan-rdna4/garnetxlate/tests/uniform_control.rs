use metal2vulkan::passes::TransformOptions;
use serde_json::Value;
use sha2::{Digest, Sha256};

fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut text = String::with_capacity(digest.len() * 2);
    for byte in digest {
        use std::fmt::Write as _;
        write!(&mut text, "{byte:02x}").unwrap();
    }
    text
}

#[test]
fn helper_uniform_facts_survive_final_remap_and_bind_the_serialized_spirv() {
    let directory = std::env::temp_dir().join(format!(
        "rdna4-uniform-control-proof-{}",
        std::process::id()
    ));
    std::fs::create_dir_all(&directory).unwrap();
    let source = include_str!("fixtures/uniform_helper_graph.ll");
    let (sanitized, datalayout) = metal2vulkan::tools::sanitize_ll_text_with_datalayout(source);
    let (spv, reflection, _, proof) =
        metal2vulkan::translate_sanitized_native_reflected_unvalidated_with_uniform_control_proof(
            &sanitized,
            metal2vulkan::passes::Stage::Fragment,
            &directory,
            TransformOptions::default(),
            datalayout.as_deref(),
        )
        .unwrap();
    assert_eq!(proof.entries.len(), 1);
    assert_eq!(proof.entries[0].name, "main");
    assert!(!proof.values.is_empty(), "helper clones lost their roots");

    let assembly = metal2vulkan::disassemble(&spv).unwrap();
    for entry in &proof.entries {
        assert!(
            assembly.contains(&format!("%{} = OpFunction", entry.function)),
            "entry function %{} is absent:\n{assembly}",
            entry.function
        );
    }
    for fact in &proof.values {
        assert!(
            assembly.contains(&format!("%{} =", fact.value)),
            "uniform value %{} is absent:\n{assembly}",
            fact.value
        );
        assert!(
            proof
                .entries
                .iter()
                .any(|entry| entry.function == fact.function),
            "inlined helper fact still names non-entry function %{}:\n{assembly}",
            fact.function
        );
    }

    let (metadata_spv, metadata) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    assert_eq!(metadata_spv, spv);
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    let serialized = metadata
        .get("uniform_control_proof")
        .expect("uniform proof missing from trimmed metadata");
    assert_eq!(serialized["version"], 1);
    assert_eq!(serialized["spv_sha256"], sha256_hex(&spv));
    assert_eq!(
        serialized["entries"].as_array().unwrap().len(),
        proof.entries.len()
    );
    assert_eq!(
        serialized["values"].as_array().unwrap().len(),
        proof.values.len()
    );
    assert!(reflection.entry_point.is_some());
    std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn cached_lowering_refuses_metadata_replayed_against_different_spirv() {
    let source = include_str!("fixtures/uniform_helper_graph.ll");
    let (mut spv, metadata) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let metadata_value: Value = serde_json::from_slice(&metadata).unwrap();
    assert!(metadata_value.get("uniform_control_proof").is_some());

    let last = spv.len() - 1;
    spv[last] ^= 0x80;
    let error = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap_err();
    assert!(
        error.contains("does not match the cached SPIR-V bytes"),
        "{error}"
    );
}

#[test]
fn cached_lowering_rejects_malformed_uniform_proofs() {
    let source = include_str!("fixtures/uniform_helper_graph.ll");
    let (spv, metadata) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let original: Value = serde_json::from_slice(&metadata).unwrap();
    for (case, expected) in [
        ("version", "unsupported uniform control proof version"),
        ("entry", "uniform control proof repeats an entry"),
        ("value", "uniform control proof repeats a value"),
        ("range", "invalid uniform control proof value id"),
    ] {
        let mut metadata = original.clone();
        let proof = &mut metadata["uniform_control_proof"];
        match case {
            "version" => proof["version"] = serde_json::json!(99),
            "entry" => {
                let duplicate = proof["entries"][0].clone();
                proof["entries"].as_array_mut().unwrap().push(duplicate);
            }
            "value" => {
                let duplicate = proof["values"][0].clone();
                proof["values"].as_array_mut().unwrap().push(duplicate);
            }
            "range" => proof["values"][0]["value"] = serde_json::json!(u64::MAX),
            _ => unreachable!(),
        }
        let error = garnetxlate::lower_gpu_arguments(&spv, &serde_json::to_vec(&metadata).unwrap())
            .unwrap_err();
        assert!(error.contains(expected), "{case}: {error}");
    }
}

#[test]
fn constant_buffer_uniformity_requires_readonly_provenance_and_uniform_indices() {
    for (space, varying, physical, expected) in [
        (2, false, false, true),
        (2, true, false, false),
        (1, false, false, false),
        (2, false, true, true),
        (2, true, true, false),
        (1, false, true, false),
    ] {
        let width = if physical { 64 } else { 32 };
        let access = if space == 2 {
            "air.read"
        } else {
            "air.read_write"
        };
        let index = if varying {
            format!("  %px = extractelement <4 x float> %position, i32 0\n  %index = fptoui float %px to i{width}")
        } else {
            format!(
                "  %index = add i{width} {}, 0",
                if physical { 4294967296u64 } else { 0 }
            )
        };
        let source = format!(
            r#"
target triple = "air64-apple-macosx"
define <4 x float> @constant_bound(ptr addrspace({space}) %bounds, <4 x float> %position) {{
entry:
{index}
  %element = getelementptr inbounds i32, ptr addrspace({space}) %bounds, i{width} %index
  %bound = load i32, ptr addrspace({space}) %element, align 4
  %value = uitofp i32 %bound to float
  %result = insertelement <4 x float> zeroinitializer, float %value, i32 0
  ret <4 x float> %result
}}
!air.fragment = !{{!0}}
!0 = !{{ptr @constant_bound, !1, !2}}
!1 = !{{!3}}
!2 = !{{!4, !5}}
!3 = !{{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}}
!4 = !{{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"{access}", !"air.address_space", i32 {space}, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"bounds"}}
!5 = !{{i32 1, !"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}}
"#
        );
        let (spv, metadata) = garnetxlate::translate_text(&source, [1, 1, 1]).unwrap();
        let assembly = metal2vulkan::disassemble(&spv).unwrap();
        let lines = assembly
            .lines()
            .map(|line| line.split_whitespace().collect::<Vec<_>>())
            .collect::<Vec<_>>();
        // A scaled i32 index can also select BDA to preserve its full byte range.
        let actual_physical = lines.iter().any(|tokens| {
            tokens.get(2) == Some(&"OpTypePointer")
                && tokens.get(3) == Some(&"PhysicalStorageBuffer")
        });
        assert!(!physical || actual_physical, "{assembly}");
        let storage = if actual_physical {
            "PhysicalStorageBuffer"
        } else {
            "StorageBuffer"
        };
        let physical_types = lines
            .iter()
            .filter(|tokens| {
                tokens.get(2) == Some(&"OpTypePointer") && tokens.get(3) == Some(&storage)
            })
            .map(|tokens| tokens[0])
            .collect::<std::collections::HashSet<_>>();
        let physical_pointers = lines
            .iter()
            .filter(|tokens| tokens.get(3).is_some_and(|ty| physical_types.contains(ty)))
            .map(|tokens| tokens[0])
            .collect::<std::collections::HashSet<_>>();
        let loads = lines
            .iter()
            .filter(|tokens| {
                tokens.get(2) == Some(&"OpLoad")
                    && tokens
                        .get(4)
                        .is_some_and(|pointer| physical_pointers.contains(pointer))
            })
            .map(|tokens| tokens[0].trim_start_matches('%').parse::<u64>().unwrap())
            .collect::<Vec<_>>();
        assert!(
            !loads.is_empty(),
            "the source must exercise actual {storage} buffer loads"
        );
        let metadata: Value = serde_json::from_slice(&metadata).unwrap();
        let proven = metadata["uniform_control_proof"]["values"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(|fact| fact["value"].as_u64())
            .collect::<std::collections::HashSet<_>>();
        assert_eq!(
            loads.iter().any(|load| proven.contains(load)),
            expected,
            "space={space}, varying={varying}, physical={physical} metadata={metadata}\n{assembly}"
        );
    }
}
