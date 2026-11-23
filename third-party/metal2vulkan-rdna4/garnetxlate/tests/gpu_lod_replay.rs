use metal2vulkan::passes::QueryLodReplay;

const NONUNIFORM: &str = include_str!("fixtures/argument_lod_graph_nonuniform.ll");

fn translate(source: &str, replay: QueryLodReplay) -> Result<Vec<u8>, String> {
    garnetxlate::translate_text_gpu_arguments_abi8_with_query_lod_replay(source, [1, 1, 1], replay)
        .map(|(spirv, _)| spirv)
}

fn disassemble(spirv: &[u8], name: &str) -> String {
    let directory =
        std::env::temp_dir().join(format!("rdna4-lod-replay-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("shader.spv");
    std::fs::write(&path, spirv).unwrap();
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
    String::from_utf8(disassembly.stdout).unwrap()
}

fn count(assembly: &str, needle: &str) -> usize {
    assembly
        .lines()
        .filter(|line| line.contains(needle))
        .count()
}

#[test]
fn uniform_only_rejects_a_varying_descriptor_tuple() {
    let error = translate(NONUNIFORM, QueryLodReplay::UniformOnly).unwrap_err();
    assert!(error.contains("nonuniform GPU argument data"), "{error}");
}

#[test]
fn quad_broadcast_replays_four_dynamic_tuples_without_lane_mask_inference() {
    let assembly = disassemble(
        &translate(NONUNIFORM, QueryLodReplay::QuadBroadcast).unwrap(),
        "quad-both",
    );
    assert!(assembly.contains("OpCapability GroupNonUniform"));
    assert!(assembly.contains("OpCapability GroupNonUniformQuad"));
    assert_eq!(count(&assembly, "OpGroupNonUniformQuadBroadcast"), 8);
    assert_eq!(count(&assembly, "OpImageQueryLod"), 4);
    assert!(!assembly.contains("SubgroupLocalInvocationId"));
    assert!(!assembly.contains("BitwiseAnd") || !assembly.contains("SubgroupLocalInvocationId"));
}

#[test]
fn quad_broadcast_sampler_only_uses_one_broadcast_per_source_lane() {
    let source = NONUNIFORM.replace(
        "%image_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group, i32 0, i64 %index",
        "%image_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 0, i32 0, i64 0",
    );
    let assembly = disassemble(
        &translate(&source, QueryLodReplay::QuadBroadcast).unwrap(),
        "quad-sampler",
    );
    assert_eq!(count(&assembly, "OpGroupNonUniformQuadBroadcast"), 4);
    assert_eq!(count(&assembly, "OpImageQueryLod"), 4);
}

#[test]
fn quad_broadcast_image_only_uses_one_broadcast_per_source_lane() {
    let source = NONUNIFORM.replace(
        "%sampler_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group, i32 1",
        "%sampler_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 0, i32 1",
    );
    let assembly = disassemble(
        &translate(&source, QueryLodReplay::QuadBroadcast).unwrap(),
        "quad-image",
    );
    assert_eq!(count(&assembly, "OpGroupNonUniformQuadBroadcast"), 4);
    assert_eq!(count(&assembly, "OpImageQueryLod"), 4);
}

#[test]
fn descriptor_enumeration_keeps_queries_in_uniform_bounded_loops() {
    let assembly = disassemble(
        &translate(NONUNIFORM, QueryLodReplay::DescriptorEnumeration).unwrap(),
        "enum-both",
    );
    assert!(assembly.contains("OpImageQueryLod"));
    assert!(!assembly.contains("GroupNonUniformQuadBroadcast"));
    assert!(!assembly.contains("OpCapability GroupNonUniformQuad"));
    assert!(assembly.contains("OpArrayLength"));
    assert!(!assembly.contains("OpConstant %uint 129"));
    assert!(!assembly.contains("OpConstant %uint 17"));
    assert!(
        count(&assembly, "OpLoopMerge") >= 3,
        "expected lookup plus image/sampler enumeration loops\n{assembly}"
    );
}

#[test]
fn descriptor_enumeration_rewrites_multiple_queries_in_one_block() {
    let needle = "  %out0 = insertelement <4 x float> zeroinitializer, float %lod, i32 0";
    let replacement = "\
  %lod2 = call float @air.calculate_unclamped_lod_texture_2d(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, i32 0)
  %sum = fadd float %lod, %lod2
  %out0 = insertelement <4 x float> zeroinitializer, float %sum, i32 0";
    let source = NONUNIFORM.replace(needle, replacement);
    let assembly = disassemble(
        &translate(&source, QueryLodReplay::DescriptorEnumeration).unwrap(),
        "enum-multiple",
    );
    assert_eq!(count(&assembly, "OpImageQueryLod"), 2);
    assert!(
        count(&assembly, "OpLoopMerge") >= 5,
        "expected two image/sampler enumeration loops plus each lookup loop\n{assembly}"
    );
}

fn capabilities() -> garnetxlate::RDNA4xBackendCapabilities {
    garnetxlate::RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: 3,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    }
}

fn with_language(source: &str, minor: u32) -> String {
    format!("{source}\n!air.language_version = !{{!901}}\n!901 = !{{!\"Metal\", i32 2, i32 {minor}, i32 0}}\n")
}

fn translate_c(
    source: &str,
    capabilities: Option<garnetxlate::RDNA4xBackendCapabilities>,
    linked: bool,
) -> Result<(Vec<u8>, Vec<u8>), (i32, String)> {
    let mut spv = std::ptr::null_mut();
    let mut spv_len = 0;
    let mut metadata = std::ptr::null_mut();
    let mut metadata_len = 0;
    let mut error = [0; 1024];
    let caps = capabilities.as_ref().map_or(std::ptr::null(), |caps| caps);
    let result = unsafe {
        if linked {
            garnetxlate::garnetx_translate_linked_with_capabilities(
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
                caps,
            )
        } else {
            garnetxlate::garnetx_translate_with_capabilities(
                source.as_ptr(),
                source.len(),
                std::ptr::null(),
                &mut spv,
                &mut spv_len,
                &mut metadata,
                &mut metadata_len,
                error.as_mut_ptr(),
                error.len(),
                caps,
            )
        }
    };
    if result != 0 {
        assert!(spv.is_null() && metadata.is_null() && spv_len == 0 && metadata_len == 0);
        let error = unsafe { std::ffi::CStr::from_ptr(error.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        return Err((result, error));
    }
    let output = unsafe {
        let output = (
            std::slice::from_raw_parts(spv, spv_len).to_vec(),
            std::slice::from_raw_parts(metadata, metadata_len).to_vec(),
        );
        garnetxlate::garnetx_free(spv, spv_len);
        garnetxlate::garnetx_free(metadata, metadata_len);
        output
    };
    Ok(output)
}

#[test]
fn abi3_uses_actual_fragment_quad_properties_and_enumerates_when_one_is_missing() {
    assert_eq!(
        std::mem::size_of::<garnetxlate::RDNA4xBackendCapabilities>(),
        24
    );
    for linked in [false, true] {
        for missing in 0..4 {
            let mut caps = capabilities();
            match missing {
                1 => caps.subgroup_size = 3,
                2 => caps.subgroup_supported_stages = 0x20,
                3 => caps.subgroup_supported_operations = 1,
                _ => {}
            }
            let (spv, metadata) =
                translate_c(&with_language(NONUNIFORM, 2), Some(caps), linked).unwrap();
            let assembly = disassemble(&spv, &format!("abi3-{linked}-{missing}"));
            assert_eq!(
                assembly.contains("OpGroupNonUniformQuadBroadcast"),
                missing == 0
            );
            let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
            assert_eq!(
                metadata["source_language_version"],
                serde_json::json!([2, 2, 0])
            );
            assert_eq!(metadata["query_lod_policy"], 1);
            assert_eq!(
                metadata["query_lod_replay"],
                if missing == 0 {
                    "quad-broadcast"
                } else {
                    "descriptor-enumeration"
                }
            );
        }
    }
}

#[test]
fn abi3_refuses_unproven_extension_language_or_device_support() {
    for flags in [0, 1, 2] {
        let mut caps = capabilities();
        caps.flags = flags;
        let (code, error) =
            translate_c(&with_language(NONUNIFORM, 2), Some(caps), false).unwrap_err();
        assert_eq!(code, garnetxlate::RC_REFUSED);
        assert!(
            error.contains("actual language and device support"),
            "{error}"
        );
    }
    for source in [NONUNIFORM.to_owned(), with_language(NONUNIFORM, 1)] {
        let (code, error) = translate_c(&source, Some(capabilities()), false).unwrap_err();
        assert_eq!(code, garnetxlate::RC_REFUSED);
        assert!(
            error.contains("language") || error.contains("Metal 2.2"),
            "{error}"
        );
    }
}

#[test]
fn abi3_bad_capabilities_clear_outputs_and_return_bad_arguments() {
    let mut caps = capabilities();
    caps.flags = 4;
    for caps in [
        None,
        Some(caps),
        Some(garnetxlate::RDNA4xBackendCapabilities {
            version: 2,
            ..capabilities()
        }),
        Some(garnetxlate::RDNA4xBackendCapabilities {
            struct_size: 20,
            ..capabilities()
        }),
    ] {
        let (code, error) = translate_c(NONUNIFORM, caps, false).unwrap_err();
        assert_eq!(code, garnetxlate::RC_BAD_ARGS);
        assert!(error.contains("capabilities"));
    }
}

#[test]
fn abi3_cached_query_checks_language_and_replay_policy_before_reuse() {
    let caps = capabilities();
    let (spv, metadata) = translate_c(&with_language(NONUNIFORM, 2), Some(caps), false).unwrap();
    let (lowered, _) =
        garnetxlate::lower_gpu_arguments_with_capabilities(&spv, &metadata, caps).unwrap();
    assert_eq!(spv, lowered);
    let fallback = garnetxlate::RDNA4xBackendCapabilities {
        subgroup_supported_operations: 0,
        ..caps
    };
    let error =
        garnetxlate::lower_gpu_arguments_with_capabilities(&spv, &metadata, fallback).unwrap_err();
    assert!(error.contains("backend mismatch"), "{error}");
    let mut metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    metadata["source_language_version"] = serde_json::json!([2, 1, 0]);
    let error = garnetxlate::lower_gpu_arguments_with_capabilities(
        &spv,
        &serde_json::to_vec(&metadata).unwrap(),
        caps,
    )
    .unwrap_err();
    assert!(error.contains("Metal 2.2"), "{error}");
}

fn query_in_branch(uniform: bool) -> String {
    let query = NONUNIFORM
        .lines()
        .find(|line| line.trim_start().starts_with("%lod ="))
        .unwrap();
    let assertion = if uniform {
        "  %same = call i1 @air.is_uniform.f32(float %py)\n  call void @llvm.assume(i1 %same)\n"
    } else {
        ""
    };
    let body = format!("{assertion}  %guard = fcmp ogt float %py, 0.0\n  br i1 %guard, label %query_body, label %query_merge\nquery_body:\n{}\n  br label %query_merge\nquery_merge:\n  %lod = phi float [ %queried, %query_body ], [ 0.0, %entry ]", query.replace("%lod =", "%queried ="));
    format!(
        "{}\ndeclare i1 @air.is_uniform.f32(float)\ndeclare void @llvm.assume(i1)\n",
        NONUNIFORM.replace(query, &body)
    )
}

#[test]
fn both_replay_paths_preserve_uniform_branch_instances_and_refuse_divergent_instances() {
    for replay in [
        QueryLodReplay::QuadBroadcast,
        QueryLodReplay::DescriptorEnumeration,
    ] {
        disassemble(
            &translate(&query_in_branch(true), replay).unwrap(),
            &format!("uniform-branch-{replay:?}"),
        );
        let error = translate(&query_in_branch(false), replay).unwrap_err();
        assert!(error.contains("derivative-uniform control"), "{error}");
    }
}

#[test]
fn both_replay_paths_keep_queries_inside_asserted_and_inferred_uniform_loops() {
    let query = NONUNIFORM
        .lines()
        .find(|line| line.trim_start().starts_with("%lod ="))
        .unwrap();
    let body = format!("  br label %query_loop\nquery_loop:\n  %iteration = phi i32 [ 0, %entry ], [ %next_iteration, %query_body ]\n  %accumulated = phi float [ 0.0, %entry ], [ %queried, %query_body ]\n  %same_iteration = call i1 @air.is_uniform.i32(i32 %iteration)\n  call void @llvm.assume(i1 %same_iteration)\n  %again = icmp ult i32 %iteration, 2\n  br i1 %again, label %query_body, label %query_exit\nquery_body:\n{}\n  %next_iteration = add i32 %iteration, 1\n  br label %query_loop\nquery_exit:\n  %lod = fadd float %accumulated, 0.0", query.replace("%lod =", "%queried ="));
    let source = format!(
        "{}\ndeclare i1 @air.is_uniform.i32(i32)\ndeclare void @llvm.assume(i1)\n",
        NONUNIFORM.replace(query, &body)
    );
    for asserted in [true, false] {
        let source = if asserted {
            source.clone()
        } else {
            source.replace("  %same_iteration = call i1 @air.is_uniform.i32(i32 %iteration)\n  call void @llvm.assume(i1 %same_iteration)\n", "")
        };
        for replay in [
            QueryLodReplay::QuadBroadcast,
            QueryLodReplay::DescriptorEnumeration,
        ] {
            let assembly = disassemble(
                &translate(&source, replay).unwrap(),
                &format!("uniform-loop-{asserted}-{replay:?}"),
            );
            assert!(count(&assembly, "OpLoopMerge") >= 2);
        }
    }
}

#[test]
fn both_replay_paths_specialize_bodied_query_helpers_before_common_instance_proof() {
    let query = NONUNIFORM
        .lines()
        .find(|line| line.trim_start().starts_with("%lod ="))
        .unwrap();
    let source = NONUNIFORM.replace(query, "  %lod = call float @query_helper(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord)");
    let source = format!("{source}\ndefine internal float @query_helper(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord) {{\nentry:\n{query}\n  ret float %lod\n}}\n");
    for replay in [
        QueryLodReplay::QuadBroadcast,
        QueryLodReplay::DescriptorEnumeration,
    ] {
        disassemble(
            &translate(&source, replay).unwrap(),
            &format!("helper-{replay:?}"),
        );
    }
}

fn cached_static_query() -> (Vec<u8>, Vec<u8>) {
    let source = r#"OpCapability Shader
OpCapability ImageQuery
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %coordinate %output %images %samplers
OpExecutionMode %main OriginUpperLeft
OpDecorate %coordinate Location 0
OpDecorate %output Location 0
OpDecorate %images DescriptorSet 0
OpDecorate %images Binding 32
OpDecorate %samplers DescriptorSet 0
OpDecorate %samplers Binding 160
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v2float = OpTypeVector %float 2
%two = OpConstant %uint 2
%one = OpConstant %uint 1
%image = OpTypeImage %float 2D 0 0 0 1 Unknown
%sampler = OpTypeSampler
%sampled = OpTypeSampledImage %image
%image_array = OpTypeArray %image %two
%sampler_array = OpTypeArray %sampler %two
%image_array_ptr = OpTypePointer UniformConstant %image_array
%sampler_array_ptr = OpTypePointer UniformConstant %sampler_array
%image_ptr = OpTypePointer UniformConstant %image
%sampler_ptr = OpTypePointer UniformConstant %sampler
%input_ptr = OpTypePointer Input %v2float
%output_ptr = OpTypePointer Output %v2float
%coordinate = OpVariable %input_ptr Input
%output = OpVariable %output_ptr Output
%images = OpVariable %image_array_ptr UniformConstant
%samplers = OpVariable %sampler_array_ptr UniformConstant
%main = OpFunction %void None %fn
%entry = OpLabel
%coord = OpLoad %v2float %coordinate
%x = OpCompositeExtract %float %coord 0
%pixel = OpConvertFToU %uint %x
%index = OpBitwiseAnd %uint %pixel %one
%image_at = OpAccessChain %image_ptr %images %index
%selected_image = OpLoad %image %image_at
%sampler_at = OpAccessChain %sampler_ptr %samplers %index
%selected_sampler = OpLoad %sampler %sampler_at
%combined = OpSampledImage %sampled %selected_image %selected_sampler
%lod = OpImageQueryLod %v2float %combined %coord
OpStore %output %lod
OpReturn
OpFunctionEnd
"#;
    let directory =
        std::env::temp_dir().join(format!("rdna4-lod-cache-input-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let input = directory.join("shader.spvasm");
    let output = directory.join("shader.spv");
    std::fs::write(&input, source).unwrap();
    let result = std::process::Command::new("spirv-as")
        .args(["--target-env", "vulkan1.2"])
        .arg(&input)
        .arg("-o")
        .arg(&output)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let spv = std::fs::read(output).unwrap();
    std::fs::remove_dir_all(directory).unwrap();
    let metadata = serde_json::json!({
        "stage": "Fragment", "trimmed_from_reflection_version": 64,
        "normalized_sampler_coordinates": true, "source_language_version": [2,2,0],
        "bindings": [
            {"kind":"EmbeddedArgBufferTexture", "descriptor":{"set":0,"binding":32}, "embedded_source":{"buffer_index":3,"field_offset":0}},
            {"kind":"EmbeddedArgBufferSampler", "descriptor":{"set":0,"binding":160}, "embedded_source":{"buffer_index":3,"field_offset":16}}
        ]
    });
    (spv, serde_json::to_vec(&metadata).unwrap())
}

#[test]
fn abi3_cached_postpass_replays_actual_physical_tokens_on_both_backends() {
    let (input, metadata) = cached_static_query();
    for quad in [true, false] {
        let caps = garnetxlate::RDNA4xBackendCapabilities {
            subgroup_supported_operations: if quad { 0x80 } else { 0 },
            ..capabilities()
        };
        let (spv, metadata) =
            garnetxlate::lower_gpu_arguments_with_capabilities(&input, &metadata, caps).unwrap();
        let assembly = disassemble(&spv, &format!("cached-{quad}"));
        assert_eq!(
            assembly.contains("OpGroupNonUniformQuadBroadcast"),
            quad,
            "{assembly}"
        );
        if quad {
            let definitions = assembly
                .lines()
                .filter_map(|line| {
                    let fields = line.split_whitespace().collect::<Vec<_>>();
                    (fields.get(1) == Some(&"=")).then(|| (fields[0], fields[2..].to_vec()))
                })
                .collect::<std::collections::HashMap<_, _>>();
            for fields in assembly
                .lines()
                .map(|line| line.split_whitespace().collect::<Vec<_>>())
                .filter(|fields| fields.get(2) == Some(&"OpGroupNonUniformQuadBroadcast"))
            {
                let mut pending = vec![fields[5]];
                let mut visited = std::collections::HashSet::new();
                let mut reaches_token_lookup = false;
                while let Some(id) = pending.pop() {
                    if !visited.insert(id) {
                        continue;
                    }
                    let Some(definition) = definitions.get(id) else {
                        continue;
                    };
                    reaches_token_lookup |= definition.first() == Some(&"OpFunctionCall");
                    pending.extend(
                        definition
                            .iter()
                            .skip(1)
                            .copied()
                            .filter(|operand| operand.starts_with('%')),
                    );
                }
                assert!(reaches_token_lookup, "quad replay used an argument array position instead of its GPU token lookup result: {fields:?}");
            }
        }
        for fields in assembly
            .lines()
            .map(|line| line.split_whitespace().collect::<Vec<_>>())
            .filter(|fields| fields.get(2) == Some(&"OpImageQueryLod"))
        {
            assert!(
                assembly.contains(&format!("OpDecorate {} NonUniform", fields[4])),
                "replayed sampled image lost its descriptor divergence annotation: {fields:?}"
            );
        }
        assert!(assembly.contains("Binding 673"));
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(
            metadata["argument_resource_tables"]["image_query_binding"],
            673
        );
    }
}

#[test]
fn both_replay_paths_refuse_a_partial_discard_before_quad_source_reads() {
    let query = NONUNIFORM
        .lines()
        .find(|line| line.trim_start().starts_with("%lod ="))
        .unwrap();
    let source = NONUNIFORM.replace(query, &format!("  %discard_guard = fcmp ogt float %py, 0.0\n  br i1 %discard_guard, label %discard_body, label %query_continue\ndiscard_body:\n  call void @air.discard_fragment()\n  br label %query_continue\nquery_continue:\n{query}"));
    let source = format!("{source}\ndeclare void @air.discard_fragment()\n");
    for replay in [
        QueryLodReplay::QuadBroadcast,
        QueryLodReplay::DescriptorEnumeration,
    ] {
        let error = translate(&source, replay).unwrap_err();
        assert!(error.contains("derivative-uniform control"), "{error}");
    }
}

#[test]
fn both_replay_paths_validate_clamped_and_unclamped_query_components() {
    for query in ["clamped", "unclamped"] {
        let source =
            NONUNIFORM.replace("calculate_unclamped_lod", &format!("calculate_{query}_lod"));
        for replay in [
            QueryLodReplay::QuadBroadcast,
            QueryLodReplay::DescriptorEnumeration,
        ] {
            let assembly = disassemble(
                &translate(&source, replay).unwrap(),
                &format!("{query}-{replay:?}"),
            );
            assert_eq!(
                count(&assembly, "OpImageQueryLod"),
                if replay == QueryLodReplay::QuadBroadcast {
                    4
                } else {
                    1
                }
            );
        }
    }
}

#[test]
fn embedded_query_only_argument_buffers_work_under_both_replay_paths() {
    let base = include_str!("fixtures/abi8/sampler-fragment-indirect.ll")
        .replace("!{!\"Metal\", i32 2, i32 1, i32 0}", "!{!\"Metal\", i32 2, i32 2, i32 0}")
        .replace(
            "  %3 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <2 x float> %mul, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #2\n  %4 = extractvalue { <4 x float>, i8 } %3, 0\n  ret <4 x float> %4",
            "  %clamped = tail call float @air.calculate_clamped_lod_texture_2d(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <2 x float> %mul, i32 0) #2\n  %unclamped = tail call float @air.calculate_unclamped_lod_texture_2d(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <2 x float> %mul, i32 0) #2\n  %v0 = insertelement <4 x float> zeroinitializer, float %clamped, i32 0\n  %v1 = insertelement <4 x float> %v0, float %unclamped, i32 1\n  ret <4 x float> %v1",
        )
        .replace(
            "declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #1",
            "declare float @air.calculate_clamped_lod_texture_2d(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i32) local_unnamed_addr #1\ndeclare float @air.calculate_unclamped_lod_texture_2d(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i32) local_unnamed_addr #1",
        );
    let divergent = base.replace(
        "  %0 = load i32, ptr addrspace(2) %flip, align 4, !tbaa !24, !alias.scope !28, !noalias !31",
        "  %px = extractelement <4 x float> %position, i32 0\n  %0 = fptoui float %px to i32",
    );
    for (label, source, expect_divergent) in
        [("uniform", &base, false), ("divergent", &divergent, true)]
    {
        for quad in [true, false] {
            let caps = garnetxlate::RDNA4xBackendCapabilities {
                subgroup_supported_operations: if quad { 0x80 } else { 0 },
                ..capabilities()
            };
            let (spv, metadata) = translate_c(source, Some(caps), false)
                .expect("query-only embedded argument buffer should translate");
            let (postpass_spv, postpass_meta) = garnetxlate::lower_gpu_arguments_with_capabilities(
                &spv, &metadata, caps,
            )
            .expect("query-only embedded argument buffer should lower through ABI 9+ postpass");
            let assembly = disassemble(
                &postpass_spv,
                &format!("embedded-query-only-{label}-{quad}"),
            );
            assert!(assembly.contains("DescriptorSet 2"), "{assembly}");
            assert!(assembly.contains("DescriptorSet 4"), "{assembly}");
            let queries = count(&assembly, "OpImageQueryLod");
            assert!(queries >= 2, "{assembly}");
            assert_eq!(
                count(&assembly, "OpImageQuerySizeLod"),
                queries,
                "postpass double-normalized or kept dead size query\n{assembly}"
            );
            assert_eq!(
                assembly.contains("OpGroupNonUniformQuadBroadcast"),
                expect_divergent && quad,
                "{assembly}"
            );
            let postpass_meta: serde_json::Value = serde_json::from_slice(&postpass_meta).unwrap();
            assert_eq!(postpass_meta["argument_resource_tables"]["version"], 9);
            assert_eq!(
                postpass_meta["argument_resource_tables"]["image_query_binding"],
                if expect_divergent {
                    serde_json::json!(673)
                } else {
                    serde_json::Value::Null
                }
            );
        }
    }
}
