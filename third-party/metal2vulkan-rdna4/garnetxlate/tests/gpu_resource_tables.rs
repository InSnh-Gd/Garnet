use std::path::PathBuf;

fn validate(name: &str, spv: &[u8]) -> String {
    let directory =
        std::env::temp_dir().join(format!("rdna4-tables-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("shader.spv");
    std::fs::write(&path, spv).unwrap();
    let result = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let result = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(result.status.success());
    std::fs::remove_dir_all(directory).unwrap();
    String::from_utf8(result.stdout).unwrap()
}

fn sampler_usage_source(operations: &str) -> String {
    include_str!("fixtures/argument_sampler_usage.ll").replace("  ; SAMPLE_OPERATIONS", operations)
}

fn linked_sampler_usage_sources(source: &str) -> (String, String) {
    let helper = source
        .split("\n!air.kernel =")
        .next()
        .expect("sampler fixture has kernel metadata")
        .replace(
            "define void @sampler_usage(",
            "define void @sampler_helper(",
        );
    let entry = r#"
define void @sampler_usage(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {
entry:
  call void @sampler_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i)
  ret void
}
declare void @sampler_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2), ptr addrspace(1), i32) section "air.externally_defined"
!air.kernel = !{!0}
!air.visible_function_references = !{!4}
!0 = !{ptr @sampler_usage, !1, !2}
!1 = !{}
!2 = !{!3, !5, !6}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!5 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!6 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
!4 = !{!"air.visible_function_reference", ptr @sampler_helper.MTL_VISIBLE_FN_REF, !"sampler_helper"}
"#
    .to_string();
    (entry, helper)
}

fn assemble_old_sampler_bindings(ordinary: bool, comparison: bool) -> Vec<u8> {
    let ordinary_declaration = ordinary.then_some(
        "OpDecorate %ordinary_sampler_var DescriptorSet 0\n\
         OpDecorate %ordinary_sampler_var Binding 160\n",
    );
    let comparison_declaration = comparison.then_some(
        "OpDecorate %comparison_sampler_var DescriptorSet 0\n\
         OpDecorate %comparison_sampler_var Binding 736\n",
    );
    let ordinary_variable =
        ordinary.then_some("%ordinary_sampler_var = OpVariable %sampler_ptr UniformConstant\n");
    let comparison_variable =
        comparison.then_some("%comparison_sampler_var = OpVariable %sampler_ptr UniformConstant\n");
    let ordinary_body = ordinary.then_some(
        "%ordinary_sampler = OpLoad %sampler %ordinary_sampler_var\n\
         %ordinary_image = OpLoad %color_image_type %color_image\n\
         %ordinary_sampled = OpSampledImage %sampled_color_type %ordinary_image %ordinary_sampler\n\
         %ordinary_result = OpImageSampleExplicitLod %v4float %ordinary_sampled %coord Lod %float_0\n",
    );
    let comparison_body = comparison.then_some(
        "%comparison_sampler = OpLoad %sampler %comparison_sampler_var\n\
         %comparison_image = OpLoad %depth_image_type %depth_image\n\
         %comparison_sampled = OpSampledImage %sampled_depth_type %comparison_image %comparison_sampler\n\
         %comparison_result = OpImageSampleDrefExplicitLod %float %comparison_sampled %coord %float_0 Lod %float_0\n",
    );
    let interface = format!(
        "%color_image %depth_image{}{}",
        if ordinary {
            " %ordinary_sampler_var"
        } else {
            ""
        },
        if comparison {
            " %comparison_sampler_var"
        } else {
            ""
        },
    );
    let source = format!(
        r#"OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" {interface}
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %color_image DescriptorSet 0
OpDecorate %color_image Binding 32
OpDecorate %depth_image DescriptorSet 0
OpDecorate %depth_image Binding 33
{ordinary_declaration}{comparison_declaration}%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%float_0 = OpConstant %float 0
%coord = OpConstantComposite %v2float %float_0 %float_0
%color_image_type = OpTypeImage %float 2D 0 0 0 1 Unknown
%depth_image_type = OpTypeImage %float 2D 1 0 0 1 Unknown
%color_image_ptr = OpTypePointer UniformConstant %color_image_type
%depth_image_ptr = OpTypePointer UniformConstant %depth_image_type
%sampler = OpTypeSampler
%sampler_ptr = OpTypePointer UniformConstant %sampler
%sampled_color_type = OpTypeSampledImage %color_image_type
%sampled_depth_type = OpTypeSampledImage %depth_image_type
%color_image = OpVariable %color_image_ptr UniformConstant
%depth_image = OpVariable %depth_image_ptr UniformConstant
{ordinary_variable}{comparison_variable}%main = OpFunction %void None %fn
%entry = OpLabel
{ordinary_body}{comparison_body}OpReturn
OpFunctionEnd
"#,
        ordinary_declaration = ordinary_declaration.unwrap_or(""),
        comparison_declaration = comparison_declaration.unwrap_or(""),
        ordinary_variable = ordinary_variable.unwrap_or(""),
        comparison_variable = comparison_variable.unwrap_or(""),
        ordinary_body = ordinary_body.unwrap_or(""),
        comparison_body = comparison_body.unwrap_or(""),
        interface = interface,
    );
    let directory = std::env::temp_dir().join(format!(
        "rdna4-old-sampler-{}-{ordinary}-{comparison}",
        std::process::id()
    ));
    std::fs::create_dir_all(&directory).unwrap();
    let input = directory.join("input.spvasm");
    let binary = directory.join("output.spv");
    std::fs::write(&input, source).unwrap();
    let result = std::process::Command::new("spirv-as")
        .args(["--target-env", "vulkan1.2"])
        .arg(&input)
        .arg("-o")
        .arg(&binary)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let bytes = std::fs::read(binary).unwrap();
    std::fs::remove_dir_all(directory).unwrap();
    bytes
}

fn old_sampler_metadata(ordinary: bool, comparison: bool) -> serde_json::Value {
    let primary = if ordinary { 160 } else { 736 };
    let mut row = serde_json::json!({
        "kind": "EmbeddedArgBufferSampler",
        "metal_index": 0,
        "descriptor": {"set": 0, "binding": primary, "count": 1},
        "embedded_source": {
            "buffer_index": 3,
            "field_offset": 0,
            "field_ordinal": 0,
            "argument_index": 0
        }
    });
    if ordinary && comparison {
        row["comparison_descriptor"] = serde_json::json!({"set": 0, "binding": 736, "count": 1});
    }
    serde_json::json!({
        "reflection_version": 62,
        "stage": "Kernel",
        "entry_point": "main",
        "bindings": [row]
    })
}

fn assemble_abi8_flat_embedded_sampler() -> Vec<u8> {
    let source = r#"OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %color_image %sampler_var %flags_var
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %color_image DescriptorSet 0
OpDecorate %color_image Binding 32
OpDecorate %sampler_var DescriptorSet 0
OpDecorate %sampler_var Binding 160
OpDecorate %flags_var DescriptorSet 0
OpDecorate %flags_var Binding 674
OpDecorate %flags_array ArrayStride 4
OpMemberDecorate %flags_struct 0 Offset 0
OpDecorate %flags_struct Block
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%uint_0 = OpConstant %uint 0
%uint_1 = OpConstant %uint 1
%uint_2 = OpConstant %uint 2
%float = OpTypeFloat 32
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%float_0 = OpConstant %float 0
%float_half = OpConstant %float 0.5
%float_quarter = OpConstant %float 0.25
%pixel_coord = OpConstantComposite %v2float %float_half %float_half
%normalized_coord = OpConstantComposite %v2float %float_quarter %float_quarter
%color_image_type = OpTypeImage %float 2D 0 0 0 1 Unknown
%color_image_ptr = OpTypePointer UniformConstant %color_image_type
%sampled_color_type = OpTypeSampledImage %color_image_type
%sampler = OpTypeSampler
%sampler_ptr = OpTypePointer UniformConstant %sampler
%flags_array = OpTypeRuntimeArray %uint
%flags_struct = OpTypeStruct %flags_array
%flags_struct_ptr = OpTypePointer StorageBuffer %flags_struct
%uint_ptr = OpTypePointer StorageBuffer %uint
%bool = OpTypeBool
%color_image = OpVariable %color_image_ptr UniformConstant
%sampler_var = OpVariable %sampler_ptr UniformConstant
%flags_var = OpVariable %flags_struct_ptr StorageBuffer
%main = OpFunction %void None %fn
%entry = OpLabel
%flag_ptr = OpAccessChain %uint_ptr %flags_var %uint_0 %uint_0
%flag_word = OpLoad %uint %flag_ptr
%is_pixel = OpINotEqual %bool %flag_word %uint_0
%coord = OpSelect %v2float %is_pixel %pixel_coord %normalized_coord
%color = OpLoad %color_image_type %color_image
%sampler_value = OpLoad %sampler %sampler_var
%sampled = OpSampledImage %sampled_color_type %color %sampler_value
%result = OpImageSampleExplicitLod %v4float %sampled %coord Lod %float_0
OpReturn
OpFunctionEnd
"#;
    let directory = std::env::temp_dir().join(format!(
        "rdna4-abi8-flat-embedded-{}-{}",
        std::process::id(),
        source.len()
    ));
    std::fs::create_dir_all(&directory).unwrap();
    let input = directory.join("input.spvasm");
    let binary = directory.join("output.spv");
    std::fs::write(&input, source).unwrap();
    let result = std::process::Command::new("spirv-as")
        .args(["--target-env", "vulkan1.2"])
        .arg(&input)
        .arg("-o")
        .arg(&binary)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let bytes = std::fs::read(binary).unwrap();
    std::fs::remove_dir_all(directory).unwrap();
    bytes
}

const ORDINARY_SAMPLE: &str = r#"  %ordinary = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %color, ptr addrspace(2) %sampler, <2 x float> <float 0.25, float 0.75>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %ordinary4 = extractvalue { <4 x float>, i8 } %ordinary, 0
  %ordinary_value = extractelement <4 x float> %ordinary4, i32 0
  store float %ordinary_value, ptr addrspace(1) %out, align 4"#;

const COMPARISON_SAMPLE: &str = r#"  %comparison = call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) %depth, ptr addrspace(2) %sampler, i32 1, <2 x float> <float 0.25, float 0.75>, float 0.5, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.0, float 0.0, i32 0)
  %comparison_value = extractvalue { float, i8 } %comparison, 0
  %out_one = getelementptr float, ptr addrspace(1) %out, i64 1
  store float %comparison_value, ptr addrspace(1) %out_one, align 4"#;

#[test]
fn repeated_flat_comparisons_keep_one_binding_through_linking_and_postpass() {
    let source = include_str!("fixtures/repeated_flat_comparison.ll");
    let direct = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let linked = garnetxlate::translate_text_linked(source, [1, 1, 1], &[]).unwrap();
    assert_eq!(direct, linked);
    let assembly = validate("repeated-flat-static", &direct.0);
    assert_eq!(
        assembly
            .lines()
            .filter(|line| line.ends_with(" Binding 736"))
            .count(),
        1
    );
    let lowered = garnetxlate::lower_gpu_arguments(&direct.0, &direct.1).unwrap();
    assert_eq!(
        garnetxlate::lower_gpu_arguments(&linked.0, &linked.1).unwrap(),
        lowered
    );
    let assembly = validate("repeated-flat-token", &lowered.0);
    assert_eq!(assembly.matches("OpImageSampleDrefExplicitLod").count(), 2);
    assert!(assembly.contains("OpImageSampleExplicitLod"));
    let metadata: serde_json::Value = serde_json::from_slice(&lowered.1).unwrap();
    assert_eq!(
        metadata["argument_resource_tables"]["comparison_sampler"],
        true
    );
}

#[test]
fn scalar_tokens_use_their_nonzero_field_offsets() {
    let (spv, metadata) = garnetxlate::translate_text_gpu_arguments(
        include_str!("fixtures/argument_scalar_resources.ll"),
        [32, 1, 1],
    )
    .unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(
        metadata["argument_resource_tables"]["roots"],
        serde_json::json!([{"buffer_index":2,"binding":706}])
    );
    let assembly = validate("scalar", &spv);
    assert!(assembly.contains("Binding 706"));
    assert!(assembly.contains("OpCapability SampledImageArrayNonUniformIndexing"));
}

#[test]
fn nested_pointer_graph_metadata_survives_trim_linking_and_postpass() {
    let source = include_str!("fixtures/argument_resource_graph.ll");
    let result = garnetxlate::translate_text(source, [8, 1, 1]).unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&result.1).unwrap();
    assert_eq!(
        metadata["argument_resource_tables"],
        serde_json::json!({
            "version": 9, "table_binding": 672, "categories": [0, 2],
            "comparison_sampler": false, "null_shapes": [1]
        })
    );
    assert_eq!(
        garnetxlate::lower_gpu_arguments(&result.0, &result.1).unwrap(),
        result
    );
    assert_eq!(
        garnetxlate::translate_text_linked(source, [8, 1, 1], &[]).unwrap(),
        result
    );
    let assembly = validate("pointer-graph", &result.0);
    assert!(assembly.contains("OpImageSample") && assembly.contains("OpConvertUToPtr"));
    assert!(assembly.contains("DescriptorSet 2") && assembly.contains("DescriptorSet 4"));
    assert!(!assembly.contains("Binding 707"));
}

#[test]
fn gpu_sampler_usage_selects_both_representations_for_one_token() {
    let source = sampler_usage_source(&format!("{ORDINARY_SAMPLE}\n{COMPARISON_SAMPLE}"));
    let direct = garnetxlate::translate_text(&source, [8, 1, 1]).unwrap();
    let (linked_entry, linked_helper) = linked_sampler_usage_sources(&source);
    let linked = garnetxlate::translate_text_linked(
        &linked_entry,
        [8, 1, 1],
        &[("sampler_helper", &linked_helper)],
    )
    .unwrap();
    let direct_metadata: serde_json::Value = serde_json::from_slice(&direct.1).unwrap();
    let linked_metadata: serde_json::Value = serde_json::from_slice(&linked.1).unwrap();
    assert_eq!(
        direct_metadata["argument_resource_tables"], linked_metadata["argument_resource_tables"],
        "linked AIR must preserve the argument-table contract"
    );
    let postpass = garnetxlate::lower_gpu_arguments(&direct.0, &direct.1).unwrap();
    let linked_postpass = garnetxlate::lower_gpu_arguments(&linked.0, &linked.1).unwrap();
    assert_eq!(
        garnetxlate::lower_gpu_arguments(&postpass.0, &postpass.1).unwrap(),
        postpass,
        "a warm cached postpass must be idempotent"
    );
    assert_eq!(
        garnetxlate::lower_gpu_arguments(&linked_postpass.0, &linked_postpass.1).unwrap(),
        linked_postpass,
        "a warm linked postpass must be idempotent"
    );
    let direct_postpass_metadata: serde_json::Value = serde_json::from_slice(&postpass.1).unwrap();
    let linked_postpass_metadata: serde_json::Value =
        serde_json::from_slice(&linked_postpass.1).unwrap();
    assert_eq!(
        direct_postpass_metadata["argument_resource_tables"],
        linked_postpass_metadata["argument_resource_tables"],
        "direct and linked postpasses must preserve one table contract"
    );
    let result = garnetxlate::translate_text_gpu_arguments(&source, [8, 1, 1]).unwrap();
    assert_eq!(
        postpass, result,
        "direct and cached entry points must agree"
    );
    let metadata: serde_json::Value = serde_json::from_slice(&result.1).unwrap();
    assert_eq!(
        metadata["argument_resource_tables"],
        serde_json::json!({
            "version": 9,
            "table_binding": 672,
            "categories": [0, 2],
            "comparison_sampler": true, "null_shapes": [1,65]
        })
    );
    assert_eq!(
        garnetxlate::translate_text_gpu_arguments(&source, [8, 1, 1]).unwrap(),
        result
    );
    for (name, module) in [
        ("sampler-usage-direct", &postpass),
        ("sampler-usage-linked", &linked_postpass),
        ("sampler-usage-cached", &result),
    ] {
        let assembly = validate(name, &module.0);
        assert!(assembly.contains("OpImageSampleExplicitLod"), "{assembly}");
        assert!(
            assembly.contains("OpImageSampleDrefExplicitLod"),
            "{assembly}"
        );
        assert!(
            assembly.contains("DescriptorSet 4") && assembly.contains("Binding 0"),
            "{assembly}"
        );
    }
    let assembly = validate("sampler-usage-cached-offset", &result.0);
    let word_five_loads = assembly
        .lines()
        .filter_map(|line| {
            let (pointer, instruction) = line.split_once(" = ")?;
            let pointer = pointer.trim();
            if !instruction.starts_with("OpAccessChain")
                || !instruction.ends_with("%uint_0 %uint_5")
            {
                return None;
            }
            assembly.lines().find_map(|line| {
                let (value, instruction) = line.split_once(" = ")?;
                (instruction == format!("OpLoad %uint {pointer}")).then_some(value.trim())
            })
        })
        .collect::<Vec<_>>();
    assert!(
        !word_five_loads.is_empty()
            && assembly.lines().any(|line| {
                line.contains("OpIAdd %uint")
                    && word_five_loads.iter().any(|value| line.ends_with(value))
            }),
        "comparison sampler offset must add a word-5 sampler count load\n{assembly}"
    );

    for (field, value) in [
        ("comparison_sampler", serde_json::json!(false)),
        ("categories", serde_json::json!([0])),
        ("table_binding", serde_json::json!(671)),
        ("version", serde_json::json!(8)),
    ] {
        let mut corrupted: serde_json::Value = serde_json::from_slice(&result.1).unwrap();
        if field == "version" {
            corrupted["argument_resource_tables"]
                .as_object_mut()
                .unwrap()
                .remove("null_shapes");
        }
        corrupted["argument_resource_tables"][field] = value;
        let error =
            garnetxlate::lower_gpu_arguments(&result.0, &serde_json::to_vec(&corrupted).unwrap())
                .unwrap_err();
        assert!(error.contains("metadata disagrees"), "{field}: {error}");
    }
}

#[test]
fn gpu_sampler_usage_keeps_normal_and_comparison_only_tables_truthful() {
    for (name, operations, comparison) in [
        ("normal-only", ORDINARY_SAMPLE, false),
        ("comparison-only", COMPARISON_SAMPLE, true),
    ] {
        let source = sampler_usage_source(operations);
        let (spv, metadata) = garnetxlate::translate_text_gpu_arguments(&source, [8, 1, 1]).unwrap();
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(
            metadata["argument_resource_tables"]["comparison_sampler"], comparison,
            "{name}: {metadata}"
        );
        assert_eq!(
            metadata["argument_resource_tables"]["categories"],
            serde_json::json!([0, 2])
        );
        let assembly = validate(name, &spv);
        assert_eq!(
            assembly.contains("OpImageSampleDrefExplicitLod"),
            comparison,
            "{name}:\n{assembly}"
        );
        if comparison {
            assert!(assembly.contains("OpIAdd %uint"), "{name}:\n{assembly}");
        }
    }
}

#[test]
fn cached_sampler_metadata_migrates_pairs_and_comparison_only_rows_to_abi_seven() {
    for (name, ordinary, comparison) in [
        ("normal", true, false),
        ("pair", true, true),
        ("comparison", false, true),
    ] {
        let spv = assemble_old_sampler_bindings(ordinary, comparison);
        let metadata = old_sampler_metadata(ordinary, comparison);
        let metadata = garnetxlate::trim_meta(&metadata, 0);
        let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(
            metadata["argument_resource_tables"]["version"], 7,
            "{name}: {metadata}"
        );
        assert_eq!(
            metadata["argument_resource_tables"]["comparison_sampler"], comparison,
            "{name}: {metadata}"
        );
        assert_eq!(
            metadata["argument_resource_tables"]["categories"],
            serde_json::json!([2]),
            "{name}: {metadata}"
        );
        assert_eq!(
            metadata["bindings"][0]["descriptor"],
            serde_json::json!({"set": 4, "binding": 0, "count": 0}),
            "{name}: {metadata}"
        );
        assert_eq!(
            metadata["bindings"][0].get("comparison_descriptor"),
            (ordinary && comparison)
                .then(|| serde_json::json!({"set": 4, "binding": 0, "count": 0}))
                .as_ref(),
            "{name}: {metadata}"
        );
        let assembly = validate(&format!("old-sampler-{name}"), &spv);
        assert!(assembly.contains("DescriptorSet 4"), "{name}: {assembly}");
        assert_eq!(
            assembly.contains("OpImageSampleDrefExplicitLod"),
            comparison,
            "{name}: {assembly}"
        );
    }
}

#[test]
fn sampler_coordinate_contract_rejects_malformed_metadata() {
    let spv = assemble_old_sampler_bindings(true, false);
    for (marker, version, expected) in [
        (Some(serde_json::json!("true")), 64, "must be a boolean"),
        (
            Some(serde_json::json!(true)),
            63,
            "require trimmed reflection version 64",
        ),
    ] {
        let mut metadata = old_sampler_metadata(true, false);
        metadata["reflection_version"] = serde_json::json!(version);
        metadata["normalized_sampler_coordinates"] = marker.unwrap();
        let metadata = garnetxlate::trim_meta(&metadata, 0);
        let error = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap_err();
        assert!(error.contains(expected), "{error}");
    }

    for marker in [None, Some(serde_json::json!(false))] {
        let mut metadata = old_sampler_metadata(true, false);
        metadata["reflection_version"] = serde_json::json!(64);
        if let Some(marker) = marker {
            metadata["normalized_sampler_coordinates"] = marker;
        }
        let metadata = garnetxlate::trim_meta(&metadata, 0);
        let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(metadata["argument_resource_tables"]["version"], 7);
        validate("legacy-coordinate-contract", &spv);
    }

    let mut legacy = old_sampler_metadata(true, false);
    legacy["reflection_version"] = serde_json::json!(64);
    legacy["normalized_sampler_coordinates"] = serde_json::json!(false);
    let legacy = garnetxlate::trim_meta(&legacy, 0);
    let (legacy_spv, legacy_table) = garnetxlate::lower_gpu_arguments(&spv, &legacy).unwrap();
    let legacy_table: serde_json::Value = serde_json::from_slice(&legacy_table).unwrap();
    assert_eq!(legacy_table["argument_resource_tables"]["version"], 7);
    let mut mislabeled = legacy_table.clone();
    mislabeled["normalized_sampler_coordinates"] = serde_json::json!(true);
    let error =
        garnetxlate::lower_gpu_arguments(&legacy_spv, &serde_json::to_vec(&mislabeled).unwrap())
            .unwrap_err();
    assert!(error.contains("table metadata disagrees"), "{error}");

    let mut current = old_sampler_metadata(true, false);
    current["reflection_version"] = serde_json::json!(64);
    current["normalized_sampler_coordinates"] = serde_json::json!(true);
    let current = garnetxlate::trim_meta(&current, 0);
    let (current_spv, current_table) = garnetxlate::lower_gpu_arguments(&spv, &current).unwrap();
    let current_table: serde_json::Value = serde_json::from_slice(&current_table).unwrap();
    assert_eq!(current_table["argument_resource_tables"]["version"], 8);
    let mut mislabeled = current_table.clone();
    mislabeled["normalized_sampler_coordinates"] = serde_json::json!(false);
    let error =
        garnetxlate::lower_gpu_arguments(&current_spv, &serde_json::to_vec(&mislabeled).unwrap())
            .unwrap_err();
    assert!(error.contains("table metadata disagrees"), "{error}");
    let mut wrong_version = current_table;
    wrong_version["argument_resource_tables"]["version"] = serde_json::json!(9);
    wrong_version["argument_resource_tables"]["null_shapes"] = serde_json::json!([]);
    let error =
        garnetxlate::lower_gpu_arguments(&current_spv, &serde_json::to_vec(&wrong_version).unwrap())
            .unwrap_err();
    assert!(error.contains("table metadata disagrees"), "{error}");
}

#[test]
fn abi8_flat_embedded_sampler_postpass_replaces_the_direct_flag_word() {
    let spv = assemble_abi8_flat_embedded_sampler();
    let original = validate("abi8-flat-embedded-before-postpass", &spv);
    let flags_var = original
        .lines()
        .find_map(|line| {
            let tokens = line.split_whitespace().collect::<Vec<_>>();
            matches!(tokens.as_slice(), ["OpDecorate", _, "Binding", "674"]).then(|| tokens[1])
        })
        .expect("fixture must declare the direct ABI 8 flag descriptor");
    let direct_pointers = original
        .lines()
        .filter_map(|line| {
            let (pointer, instruction) = line.split_once(" = ")?;
            let operands = instruction.split_whitespace().collect::<Vec<_>>();
            (instruction.starts_with("OpAccessChain") && operands.get(2) == Some(&flags_var))
                .then_some(pointer.trim())
        })
        .collect::<Vec<_>>();
    assert!(original.lines().any(|line| {
        let Some((_, instruction)) = line.split_once(" = ") else {
            return false;
        };
        instruction
            .strip_prefix("OpLoad %uint ")
            .is_some_and(|pointer| direct_pointers.contains(&pointer))
    }));
    let mut metadata = old_sampler_metadata(true, false);
    metadata["reflection_version"] = serde_json::json!(64);
    metadata["normalized_sampler_coordinates"] = serde_json::json!(true);
    let metadata = garnetxlate::trim_meta(&metadata, 0);
    let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(metadata["argument_resource_tables"]["version"], 8);
    assert_eq!(
        metadata["argument_resource_tables"]["categories"],
        serde_json::json!([2])
    );
    let assembly = validate("abi8-flat-embedded-postpass", &spv);

    assert!(
        !assembly.lines().any(|line| {
            let tokens = line.split_whitespace().collect::<Vec<_>>();
            matches!(tokens.as_slice(), ["OpDecorate", _, "Binding", "674"])
        }),
        "the replaced direct 674 descriptor and its loads must be retired:\n{assembly}"
    );

    let word_loads = |word: &str| {
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
    let word_four = word_loads("4");
    let word_five = word_loads("5");
    assert!(!word_four.is_empty() && !word_five.is_empty(), "{assembly}");
    let shifted = assembly.lines().find_map(|line| {
        let (value, instruction) = line.split_once(" = ")?;
        (instruction.starts_with("OpShiftLeftLogical %uint")
            && word_five
                .iter()
                .any(|word| instruction.split_whitespace().any(|token| token == *word)))
        .then_some(value.trim())
    });
    assert!(
        shifted.is_some_and(|shifted| assembly.lines().any(|line| {
            line.contains("OpIAdd %uint")
                && word_four
                    .iter()
                    .any(|word| line.ends_with(&format!("{word} {shifted}")))
        })),
        "postpass must compute flags at word4 + 2*word5:\n{assembly}"
    );
}

#[test]
fn graph_static_sampler_state_survives_trim_linking_and_postpass() {
    let source = include_str!("fixtures/argument_graph_static_sampler.ll");
    let result = garnetxlate::translate_text(source, [8, 1, 1]).unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&result.1).unwrap();
    let samplers = metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|binding| binding["kind"] == "StaticSampler")
        .collect::<Vec<_>>();
    assert_eq!(
        samplers.len(),
        1,
        "graph dropped its constexpr sampler state"
    );
    assert_eq!(
        samplers[0]["static_sampler"]["raw_words"],
        serde_json::json!([34901797601026121u64, 0])
    );
    assert_eq!(samplers[0]["static_sampler"]["mip_filter"], "Nearest");
    let assembly = validate("graph-static-sampler", &result.0);
    let binding = samplers[0]["descriptor"]["binding"].as_u64().unwrap();
    assert_eq!(binding, 160);
    assert!(
        assembly.contains(&format!("Binding {binding}")),
        "{assembly}"
    );
    assert_eq!(
        garnetxlate::translate_text_linked(source, [8, 1, 1], &[]).unwrap(),
        result
    );
    assert_eq!(
        garnetxlate::lower_gpu_arguments(&result.0, &result.1).unwrap(),
        result
    );
}

#[test]
fn integer_texture_tokens_keep_read_write_and_query_types() {
    let original = include_str!("fixtures/argument_resource_texel_graph.ll");
    for storage in [false, true] {
        let source = if storage {
            original.to_string()
        } else {
            original
                .lines()
                .filter(|line| !line.trim_start().starts_with("call void @air.write_"))
                .collect::<Vec<_>>()
                .join("\n")
                .replace("i32 0, i32 3)", "i32 0, i32 1)")
        };
        let (spv, metadata) = garnetxlate::translate_text(&source, [8, 1, 1]).unwrap();
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(
            metadata["argument_resource_tables"]["categories"],
            serde_json::json!([u32::from(storage)])
        );
        let assembly = validate(if storage { "texel-rw" } else { "texel-read" }, &spv);
        assert!(assembly.contains("Binding 673"), "{assembly}");
        assert_eq!(metadata["argument_resource_tables"]["version"], 9);
        assert_eq!(
            metadata["argument_resource_tables"]["comparison_sampler"],
            false
        );
        assert!(
            assembly.contains(if storage {
                "OpImageRead"
            } else {
                "OpImageFetch"
            }),
            "{assembly}"
        );
        assert_eq!(assembly.contains("OpImageWrite"), storage);
        assert!(
            assembly
                .lines()
                .any(|line| line.contains("OpShiftRightLogical %ulong")
                    && line.ends_with("%ulong_32")),
            "{assembly}"
        );
        assert!(assembly.contains(if storage {
            "DescriptorSet 3"
        } else {
            "DescriptorSet 2"
        }));
    }
}

#[test]
fn invalid_texture_access_and_conflicting_graph_types_are_refused() {
    let original = include_str!("fixtures/argument_resource_texel_graph.ll");
    for (source, expected) in [
        (
            original.replace("i32 0, i32 3)", "i32 0, i32 7)"),
            "invalid GPU argument image access",
        ),
        (
            original.replacen("i32 0, i32 3)", "i32 0, i32 1)", 1),
            "conflicting image types",
        ),
        (
            original.replace("get_height_texture_2d", "get_height_texture_3d"),
            "shape disagrees",
        ),
    ] {
        let error = garnetxlate::translate_text(&source, [8, 1, 1]).unwrap_err();
        assert!(error.contains(expected), "{error}");
    }
}

#[test]
fn nested_integer_image_queries_use_the_actual_component_type() {
    let original = include_str!("fixtures/argument_resource_texel_graph.ll");
    for unsigned in [false, true] {
        let source = original
            .replace("uitofp i32 %size to float", "add i32 %size, 0")
            .replace("fadd float", "add float")
            .replace("float", "i32")
            .replace("v4f32", if unsigned { "u.v4i32" } else { "v4i32" });
        let source = source.replace("define void @texel_graph(", "define internal void @texel_helper(")
            .replace("declare ptr addrspace(2)", "define void @texel_graph(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {\nentry:\n call void @texel_helper(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i)\n ret void\n}\ndeclare ptr addrspace(2)");
        let (spv, _) = garnetxlate::translate_text(&source, [8, 1, 1]).unwrap();
        let assembly = validate(if unsigned { "texel-uint" } else { "texel-int" }, &spv);
        assert!(
            assembly.contains(if unsigned {
                "OpTypeImage %uint 2D"
            } else {
                "OpTypeImage %int 2D"
            }),
            "{assembly}"
        );
        assert!(
            assembly.contains("Binding 673")
                && assembly.contains("OpImageRead")
                && assembly.contains("OpImageWrite")
        );
    }
}

#[test]
fn query_only_graph_has_no_image_type_or_descriptor_array() {
    let source = include_str!("fixtures/argument_resource_query_graph.ll");
    let result = garnetxlate::translate_text(source, [8, 1, 1]).unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&result.1).unwrap();
    assert_eq!(
        metadata["argument_resource_tables"],
        serde_json::json!({
            "version":7, "table_binding":672, "image_query_binding":673,
            "categories":[], "comparison_sampler":false
        })
    );
    assert_eq!(
        garnetxlate::lower_gpu_arguments(&result.0, &result.1).unwrap(),
        result
    );
    assert_eq!(
        garnetxlate::translate_text_linked(source, [8, 1, 1], &[]).unwrap(),
        result
    );
    let assembly = validate("image-query-only", &result.0);
    assert!(assembly.contains("Binding 672") && assembly.contains("Binding 673"));
    assert!(!assembly.contains("OpTypeImage") && !assembly.contains("OpImageQuery"));
    assert!(!assembly.contains("OpCapability RuntimeDescriptorArray"));
    assert!(assembly.contains("OpULessThan") && assembly.contains("OpShiftRightLogical"));
}

#[test]
fn dynamic_texture_nullness_checks_both_token_words_without_descriptors() {
    let original = include_str!("fixtures/argument_resource_texel_graph.ll");
    let start = original.find("  %sampler =").unwrap();
    let end = original[start..].find("  ret void").unwrap() + start;
    let source = format!("{}  %null = call i1 @air.is_null_texture_2d(ptr addrspace(1) %image)\n  %result = select i1 %null, i32 1, i32 0\n  store i32 %result, ptr addrspace(1) %out, align 4\n{}\ndeclare i1 @air.is_null_texture_2d(ptr addrspace(1))\n", &original[..start], &original[end..]);
    let (spv, metadata) = garnetxlate::translate_text(&source, [8, 1, 1]).unwrap();
    let assembly = validate("dynamic-texture-null", &spv);
    let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    assert!(metadata.get("argument_resource_tables").is_none());
    assert!(
        assembly.contains("OpBitwiseOr") && assembly.contains("OpIEqual"),
        "{assembly}"
    );
    assert!(!assembly.contains("OpTypeImage"), "{assembly}");
    assert!(
        assembly
            .lines()
            .any(|line| line.contains("OpShiftRightLogical %ulong") && line.ends_with("%ulong_32")),
        "{assembly}"
    );
}

#[test]
fn null_texel_accesses_guard_descriptor_loads_and_return_zero() {
    let original = include_str!("fixtures/argument_resource_texel_graph.ll");
    for storage in [false, true] {
        let source = if storage {
            original.to_string()
        } else {
            original
                .lines()
                .filter(|line| !line.trim_start().starts_with("call void @air.write_"))
                .collect::<Vec<_>>()
                .join("\n")
                .replace("i32 0, i32 3)", "i32 0, i32 1)")
        };
        let (spv, _) = garnetxlate::translate_text(&source, [8, 1, 1]).unwrap();
        let assembly = validate(if storage { "null-write" } else { "null-read" }, &spv);
        let mut accesses = 0;
        for function in assembly.split(" = OpFunction ").skip(1) {
            let function = function.split("OpFunctionEnd").next().unwrap();
            if !function.contains("OpImageRead")
                && !function.contains("OpImageFetch")
                && !function.contains("OpImageWrite")
            {
                continue;
            }
            accesses += 1;
            let conditional = function
                .find("OpBranchConditional")
                .expect("null image access must branch before touching a descriptor");
            let load = function.find("OpLoad %").expect("guarded descriptor load");
            assert!(conditional < load, "{function}");
            assert!(
                function.contains("OpINotEqual") && function.contains("OpSelectionMerge"),
                "{function}"
            );
            if !function.contains("OpImageWrite") {
                assert!(function.contains("OpReturnValue"), "{function}");
            }
        }
        assert_eq!(accesses, if storage { 2 } else { 1 });
        assert!(assembly.contains("OpConstantNull"));
    }
}

#[test]
fn sampling_preserves_derivatives_and_bias_with_shape_correct_null_indices() {
    use metal2vulkan::passes::{lower_argument_resource_tables, ArgumentResourceField};
    for (kind, sampled_type, result_type, instruction) in [
        (
            "implicit",
            "float",
            "v4float",
            "%result = OpImageSampleImplicitLod %v4float %sampled %coordinate Bias %float1",
        ),
        (
            "explicit",
            "float",
            "v4float",
            "%result = OpImageSampleExplicitLod %v4float %sampled %coordinate Lod %float1",
        ),
        (
            "gather",
            "float",
            "v4float",
            "%result = OpImageGather %v4float %sampled %coordinate %uint0",
        ),
        (
            "gather-uint",
            "uint",
            "v4uint",
            "%result = OpImageGather %v4uint %sampled %coordinate %uint0",
        ),
        (
            "gather-int",
            "int",
            "v4int",
            "%result = OpImageGather %v4int %sampled %coordinate %uint0",
        ),
    ] {
        let source = format!("OpCapability Shader\nOpMemoryModel Logical GLSL450\nOpEntryPoint Fragment %main \"main\" %images %samplers\nOpExecutionMode %main OriginUpperLeft\nOpDecorate %images DescriptorSet 0\nOpDecorate %images Binding 32\nOpDecorate %samplers DescriptorSet 0\nOpDecorate %samplers Binding 160\n%void = OpTypeVoid\n%float = OpTypeFloat 32\n%uint = OpTypeInt 32 0\n%int = OpTypeInt 32 1\n%v4uint = OpTypeVector %uint 4\n%v4int = OpTypeVector %int 4\n%v2float = OpTypeVector %float 2\n%v4float = OpTypeVector %float 4\n%uint0 = OpConstant %uint 0\n%uint2 = OpConstant %uint 2\n%float1 = OpConstant %float 1\n%coordinate = OpConstantComposite %v2float %float1 %float1\n%image = OpTypeImage %{sampled_type} 2D 0 0 0 1 Unknown\n%array = OpTypeArray %image %uint2\n%arrayptr = OpTypePointer UniformConstant %array\n%imageptr = OpTypePointer UniformConstant %image\n%images = OpVariable %arrayptr UniformConstant\n%sampler = OpTypeSampler\n%samplerptr = OpTypePointer UniformConstant %sampler\n%samplers = OpVariable %samplerptr UniformConstant\n%sampledtype = OpTypeSampledImage %image\n%function = OpTypeFunction %void\n%main = OpFunction %void None %function\n%entry = OpLabel\n%pointer = OpAccessChain %imageptr %images %uint0\n%texture = OpLoad %image %pointer\n%s = OpLoad %sampler %samplers\n%sampled = OpSampledImage %sampledtype %texture %s\n{instruction}\nOpReturn\nOpFunctionEnd\n");
        let directory =
            std::env::temp_dir().join(format!("rdna4-null-sample-{}-{kind}", std::process::id()));
        std::fs::create_dir_all(&directory).unwrap();
        let input = directory.join("input.spvasm");
        let binary = directory.join("input.spv");
        std::fs::write(&input, source).unwrap();
        let assembled = std::process::Command::new("spirv-as")
            .args(["--target-env", "vulkan1.2"])
            .arg(input)
            .arg("-o")
            .arg(&binary)
            .output()
            .unwrap();
        assert!(
            assembled.status.success(),
            "{}",
            String::from_utf8_lossy(&assembled.stderr)
        );
        let spv = lower_argument_resource_tables(
            &std::fs::read(binary).unwrap(),
            &[ArgumentResourceField {
                binding: 32,
                buffer_index: 3,
                byte_offset: 0,
            }],
        )
        .unwrap();
        let assembly = validate(kind, &spv);
        let body = assembly
            .split(" = OpFunction ")
            .nth(1)
            .unwrap()
            .split("OpFunctionEnd")
            .next()
            .unwrap();
        assert!(!body.contains("OpBranchConditional"), "{body}");
        let sample = body
            .find(match kind {
                "implicit" => "OpImageSampleExplicitLod",
                "explicit" => "OpImageSampleExplicitLod",
                _ => "OpImageGather",
            })
            .unwrap();
        assert!(body[..sample].contains("OpSelect %uint"), "{body}");
        assert!(
            body[sample..].contains(&format!("OpSelect %{result_type}")),
            "{body}"
        );
        assert!(
            body.contains("OpIAdd %uint")
                && assembly.contains(&format!("OpConstantNull %{result_type}")),
            "{assembly}"
        );
        if kind == "implicit" {
            assert!(body[sample..].contains("Grad"), "{body}");
            assert!(body[..sample].contains("OpDPdx"), "{body}");
            assert!(body[..sample].contains("OpDPdy"), "{body}");
            assert!(
                body[..sample].contains("FClamp") && body[..sample].contains("Exp2"),
                "{body}"
            );
        }
        std::fs::remove_dir_all(directory).unwrap();
    }
}

#[test]
fn constant_air_indices_still_load_gpu_selected_tokens() {
    let source = include_str!("fixtures/argument_resource_arrays.ll")
        .replace("i64 %conv2", "i64 1")
        .replace("i64 %conv", "i64 0");
    let (spv, _) = garnetxlate::translate_text_gpu_arguments(&source, [32, 1, 1]).unwrap();
    let assembly = validate("constant", &spv);
    assert!(assembly.contains("OpCapability SampledImageArrayNonUniformIndexing"));
    assert!(assembly.contains("Binding 707"));
}

#[test]
fn integer_air_gathers_preserve_component_types_through_dynamic_argument_arrays() {
    for (suffix, component, scalar, vector, shape) in [
        ("u", "uint", "uint", "v4uint", 17),
        ("s", "int", "int", "v4int", 33),
    ] {
        let source = include_str!("fixtures/argument_resource_arrays.ll")
            .lines()
            .map(|line| {
                let trimmed = line.trim_start();
                if trimmed.starts_with("%2 = tail call") {
                    format!("  %2 = call {{ <4 x i32>, i8 }} @air.gather_texture_2d.{suffix}.v4i32(ptr addrspace(1) %1, ptr addrspace(2) %agg.tmp.sroa.0.0.copyload, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i32 0, i32 0)")
                } else if trimmed.starts_with("%3 = extractvalue") || trimmed.starts_with("%4 = extractelement") {
                    line.replace("<4 x float>", "<4 x i32>")
                } else if trimmed.starts_with("%5 = tail call") {
                    "  %5 = add i32 %4, 0".to_string()
                } else {
                    line.to_string()
                }
            })
            .collect::<Vec<_>>()
            .join("\n")
            .replace("texture2d<float, sample>", &format!("texture2d<{component}, sample>"));
        let source = format!("{source}\ndeclare {{ <4 x i32>, i8 }} @air.gather_texture_2d.{suffix}.v4i32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i32, i32)\n");
        let (spv, metadata) = garnetxlate::translate_text_gpu_arguments(&source, [8, 1, 1]).unwrap();
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(
            metadata["argument_resource_tables"]["null_shapes"],
            serde_json::json!([shape])
        );
        let assembly = validate(&format!("integer-air-{suffix}"), &spv);
        assert!(
            assembly.contains(&format!("OpTypeImage %{scalar} 2D")),
            "{assembly}"
        );
        assert!(
            assembly.contains(&format!("OpImageGather %{vector}")),
            "{assembly}"
        );
        assert!(
            assembly.contains(&format!("OpConstantNull %{vector}")),
            "{assembly}"
        );
        assert!(assembly.contains("OpConstant %uint 32"), "{assembly}");
    }
}

#[test]
fn no_resource_tokens_preserves_the_existing_contract() {
    let source = include_str!("fixtures/gpu_produced_arguments.ll");
    assert_eq!(
        garnetxlate::translate_text(source, [1, 1, 1]),
        garnetxlate::translate_text_gpu_arguments(source, [1, 1, 1])
    );
}

#[test]
fn cached_static_texel_reads_get_the_same_null_guard() {
    let original = include_str!("fixtures/argument_resource_arrays.ll");
    let source = original.lines().map(|line| {
        if line.trim_start().starts_with("%2 = tail call") {
            "  %2 = call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %1, ptr addrspace(2) %agg.tmp.sroa.0.0.copyload, <2 x i32> zeroinitializer, <2 x i32> zeroinitializer, i32 0, i32 1)"
        } else { line }
    }).collect::<Vec<_>>().join("\n").replace("texture2d<float, sample>", "texture2d<float, read>").replace("!\"air.sample\"", "!\"air.read\"");
    let source = format!("{source}\ndeclare {{ <4 x float>, i8 }} @air.read_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32)\n");
    let (spv, meta) = garnetxlate::translate_text(&source, [8, 1, 1]).unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&meta).unwrap();
    assert!(metadata.get("argument_resource_tables").is_none());
    let (spv, _) = garnetxlate::lower_gpu_arguments(&spv, &meta)
        .unwrap_or_else(|error| panic!("{error}\n{metadata}\n{}", validate("static-before", &spv)));
    let assembly = validate("static-null-read", &spv);
    let function = assembly
        .split(" = OpFunction ")
        .skip(1)
        .find(|function| {
            let body = function.split("OpFunctionEnd").next().unwrap();
            body.contains("OpImageFetch") || body.contains("OpImageRead")
        })
        .unwrap()
        .split("OpFunctionEnd")
        .next()
        .unwrap();
    assert!(
        function.find("OpBranchConditional").unwrap() < function.find("OpLoad %").unwrap(),
        "{function}"
    );
    assert!(function.contains("OpPhi") && assembly.contains("OpConstantNull"));
}

#[test]
fn null_fetch_guards_preserve_constant_image_operands() {
    use metal2vulkan::passes::{lower_argument_resource_tables, ArgumentResourceField};
    let directory = std::env::temp_dir().join(format!("rdna4-null-offset-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let source = directory.join("fetch.spvasm");
    let path = directory.join("fetch.spv");
    std::fs::write(&source, include_str!("fixtures/argument_null_fetch.spvasm")).unwrap();
    let status = std::process::Command::new("spirv-as")
        .args(["--target-env", "vulkan1.2"])
        .arg(source)
        .arg("-o")
        .arg(&path)
        .status()
        .unwrap();
    assert!(status.success());
    let spv = std::fs::read(path).unwrap();
    std::fs::remove_dir_all(directory).unwrap();
    let spv = lower_argument_resource_tables(
        &spv,
        &[ArgumentResourceField {
            binding: 32,
            buffer_index: 3,
            byte_offset: 0,
        }],
    )
    .unwrap();
    let assembly = validate("null-fetch-constant-offset", &spv);
    assert!(assembly.contains("Lod|ConstOffset"), "{assembly}");
    assert!(assembly.contains("OpSelectionMerge") && assembly.contains("OpConstantNull"));
}

#[test]
fn postpass_refuses_invalid_coordinates_before_rewriting() {
    use metal2vulkan::passes::{lower_argument_resource_tables, ArgumentResourceField};
    let (spv, _) = garnetxlate::translate_text(
        include_str!("fixtures/argument_resource_arrays.ll"),
        [32, 1, 1],
    )
    .unwrap();
    let valid = ArgumentResourceField {
        binding: 32,
        buffer_index: 3,
        byte_offset: 0,
    };
    for fields in [
        vec![ArgumentResourceField {
            buffer_index: 31,
            ..valid
        }],
        vec![ArgumentResourceField {
            byte_offset: 4,
            ..valid
        }],
        vec![ArgumentResourceField {
            binding: 999,
            ..valid
        }],
        vec![ArgumentResourceField {
            byte_offset: u32::MAX - 7,
            ..valid
        }],
        vec![valid, valid],
    ] {
        assert!(lower_argument_resource_tables(&spv, &fields).is_err());
    }
    let (lowered, _) = garnetxlate::translate_text_gpu_arguments(
        include_str!("fixtures/argument_resource_arrays.ll"),
        [32, 1, 1],
    )
    .unwrap();
    assert!(lower_argument_resource_tables(&lowered, &[valid])
        .unwrap_err()
        .contains("already occupied"));
}

#[test]
fn cached_linked_results_and_optional_c_abi_use_the_same_lowering() {
    let source = include_str!("fixtures/argument_resource_arrays.ll");
    let (cached_spv, cached_meta) =
        garnetxlate::translate_text_linked(source, [32, 1, 1], &[]).unwrap();
    let expected = garnetxlate::translate_text_gpu_arguments(source, [32, 1, 1]).unwrap();
    assert_eq!(
        garnetxlate::lower_gpu_arguments(&cached_spv, &cached_meta).unwrap(),
        expected
    );
    unsafe {
        let mut spv = std::ptr::null_mut();
        let mut meta = std::ptr::null_mut();
        let mut spv_len = 0;
        let mut meta_len = 0;
        let mut error = [0i8; 256];
        let rc = garnetxlate::garnetx_lower_argument_resources(
            cached_spv.as_ptr(),
            cached_spv.len(),
            cached_meta.as_ptr(),
            cached_meta.len(),
            &mut spv,
            &mut spv_len,
            &mut meta,
            &mut meta_len,
            error.as_mut_ptr(),
            error.len(),
        );
        assert_eq!(rc, garnetxlate::RC_OK);
        assert_eq!(std::slice::from_raw_parts(spv, spv_len), expected.0);
        assert_eq!(std::slice::from_raw_parts(meta, meta_len), expected.1);
        garnetxlate::garnetx_free(spv, spv_len);
        garnetxlate::garnetx_free(meta, meta_len);
        let rc = garnetxlate::garnetx_lower_argument_resources(
            std::ptr::null(),
            0,
            cached_meta.as_ptr(),
            cached_meta.len(),
            &mut spv,
            &mut spv_len,
            &mut meta,
            &mut meta_len,
            error.as_mut_ptr(),
            error.len(),
        );
        assert_eq!(rc, garnetxlate::RC_BAD_ARGS);
        assert!(spv.is_null() && meta.is_null());
        assert_eq!((spv_len, meta_len), (0, 0));
        assert_ne!(error[0], 0);
    }
}

#[test]
fn all_shader_stages_and_resource_categories_validate_with_multiple_roots() {
    use metal2vulkan::passes::{lower_argument_resource_tables, ArgumentResourceField};
    for stage in ["Vertex", "Fragment", "GLCompute"] {
        for (category, element, binding) in [
            (0, "OpTypeImage %float 2D 0 0 0 1 Unknown", 32),
            (1, "OpTypeImage %float 2D 0 0 0 2 Unknown", 480),
            (2, "OpTypeSampler", 160),
        ] {
            let mode = match stage {
                "Fragment" => "OpExecutionMode %main OriginUpperLeft",
                "GLCompute" => "OpExecutionMode %main LocalSize 1 1 1",
                _ => "",
            };
            let assembly = format!(
                r#"OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint {stage} %main "main" %resource %other
{mode}
OpDecorate %resource DescriptorSet 0
OpDecorate %resource Binding {binding}
OpDecorate %other DescriptorSet 0
OpDecorate %other Binding {other_binding}
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%element = {element}
%pointer = OpTypePointer UniformConstant %element
%resource = OpVariable %pointer UniformConstant
%other = OpVariable %pointer UniformConstant
%main = OpFunction %void None %fn
%entry = OpLabel
%loaded = OpLoad %element %resource
%second = OpLoad %element %other
OpReturn
OpFunctionEnd
"#,
                other_binding = binding + 1
            );
            let directory = std::env::temp_dir().join(format!(
                "rdna4-assemble-{}-{stage}-{category}",
                std::process::id()
            ));
            std::fs::create_dir_all(&directory).unwrap();
            let input = directory.join("input.spvasm");
            let binary = directory.join("input.spv");
            std::fs::write(&input, assembly).unwrap();
            let assembled = std::process::Command::new("spirv-as")
                .args(["--target-env", "vulkan1.2"])
                .arg(&input)
                .arg("-o")
                .arg(&binary)
                .output()
                .unwrap();
            assert!(
                assembled.status.success(),
                "{}",
                String::from_utf8_lossy(&assembled.stderr)
            );
            let spv = lower_argument_resource_tables(
                &std::fs::read(binary).unwrap(),
                &[
                    ArgumentResourceField {
                        binding,
                        buffer_index: 2,
                        byte_offset: 8,
                    },
                    ArgumentResourceField {
                        binding: binding + 1,
                        buffer_index: 6,
                        byte_offset: 24,
                    },
                ],
            )
            .unwrap();
            let output = validate(&format!("{stage}-{category}"), &spv);
            assert!(output.contains(&format!("DescriptorSet {}", 2 + category)));
            assert!(output.contains("Binding 706") && output.contains("Binding 710"));
            std::fs::remove_dir_all(directory).unwrap();
        }
    }
}

#[test]
fn gpu_token_loads_replace_static_texture_and_sampler_selection() {
    let source = include_str!("fixtures/argument_resource_arrays.ll");
    let (spv, meta) = garnetxlate::translate_text_gpu_arguments(source, [32, 1, 1]).unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&meta).unwrap();
    assert_eq!(metadata["argument_resource_tables"]["version"], 9);
    assert_eq!(
        metadata["argument_resource_tables"]["comparison_sampler"],
        false
    );
    assert_eq!(
        metadata["argument_resource_tables"]["roots"],
        serde_json::json!([{"buffer_index":3,"binding":707}])
    );
    let out = std::env::var_os("GPU_RESOURCE_TABLE_EVIDENCE")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            std::env::temp_dir().join(format!("rdna4-gpu-tables-{}", std::process::id()))
        });
    std::fs::create_dir_all(&out).unwrap();
    let path = out.join("tables.spv");
    std::fs::write(&path, &spv).unwrap();
    std::fs::write(out.join("tables.meta.json"), &meta).unwrap();
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
    std::fs::write(out.join("tables.spvasm"), &assembly).unwrap();
    assert!(
        assembly.contains("OpCapability RuntimeDescriptorArray"),
        "{assembly}"
    );
    assert!(
        assembly.contains("OpCapability SampledImageArrayNonUniformIndexing"),
        "{assembly}"
    );
    assert!(assembly.contains("OpLoopMerge"), "{assembly}");
    assert!(assembly.contains("Binding 672"), "{assembly}");
    assert!(assembly.contains("Binding 707"), "{assembly}");
    assert!(assembly.contains("DescriptorSet 2"), "{assembly}");
    assert!(assembly.contains("DescriptorSet 4"), "{assembly}");
    if std::env::var_os("GPU_RESOURCE_TABLE_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}
