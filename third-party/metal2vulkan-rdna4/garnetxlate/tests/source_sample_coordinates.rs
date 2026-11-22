use serde_json::Value;

fn source(arrayed: bool) -> String {
    let suffix = if arrayed { "2d_array" } else { "2d" };
    let layer = if arrayed { "i32 %layer," } else { "" };
    let layer_type = if arrayed { "i32," } else { "" };
    format!(
        r#"
target triple = "air64-apple-macosx"
%Args = type {{ [2 x ptr addrspace(1)], [2 x ptr addrspace(2)] }}
define <4 x float> @main(ptr addrspace(2) %args, <4 x float> %position, ptr addrspace(2) %layers) {{
entry:
  %x = extractelement <4 x float> %position, i32 0
  %pixel = call i32 @air.convert.u.i32.f.f32(float %x)
  %quad = and i32 %pixel, 2
  %condition = icmp ne i32 %quad, 0
  %index32 = and i32 %pixel, 1
  %index = zext i32 %index32 to i64
  %image_slot = getelementptr %Args, ptr addrspace(2) %args, i64 0, i32 0, i64 %index
  %sampler_slot = getelementptr %Args, ptr addrspace(2) %args, i64 0, i32 1, i64 %index
  %image = load ptr addrspace(1), ptr addrspace(2) %image_slot, align 8
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  %xy = shufflevector <4 x float> %position, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  br i1 %condition, label %arm, label %skip
arm:
  %layer = load i32, ptr addrspace(2) %layers, align 4
  %coord = fmul <2 x float> %xy, <float 0.4, float 0.4>
  %sample = call {{ <4 x float>, i8 }} @air.sample_texture_{suffix}.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, {layer} i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %rgba = extractvalue {{ <4 x float>, i8 }} %sample, 0
  br label %merge
skip:
  br label %merge
merge:
  %color = phi <4 x float> [ %rgba, %arm ], [ zeroinitializer, %skip ]
  ret <4 x float> %color
}}
declare {{ <4 x float>, i8 }} @air.sample_texture_{suffix}.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, {layer_type} i1, <2 x i32>, i1, float, float, i32)
declare i32 @air.convert.u.i32.f.f32(float)
!air.fragment = !{{!0}}
!0 = !{{ptr @main, !1, !2}}
!1 = !{{!6}}
!2 = !{{!3, !4, !5}}
!3 = !{{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Args"}}
!4 = !{{i32 1, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4"}}
!5 = !{{i32 2, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"uint"}}
!6 = !{{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}}
"#
    )
}

#[test]
fn guarded_coordinate_reads_reconverge_without_an_unconditional_access_promise() {
    let original = "%coord = fmul <2 x float> %xy, <float 0.4, float 0.4>";
    let text = source(false).replace(original,
        "%scale_ptr = getelementptr i8, ptr addrspace(2) %layers, i64 8\n  \
         %scale = load float, ptr addrspace(2) %scale_ptr, align 4\n  \
         %scale0 = insertelement <2 x float> poison, float %scale, i32 0\n  \
         %scale2 = shufflevector <2 x float> %scale0, <2 x float> poison, <2 x i32> zeroinitializer\n  \
         %coord = fmul <2 x float> %xy, %scale2");
    assert!(!text.contains("dereferenceable"));
    let looped = text.replace("entry:\n", "entry:\n  br label %loop_header\nloop_header:\n  %iteration = phi i32 [ 0, %entry ], [ %next, %merge ]\n  %acc = phi <4 x float> [ zeroinitializer, %entry ], [ %total, %merge ]\n  %again = icmp ult i32 %iteration, 2\n  br i1 %again, label %loop_body, label %exit\nloop_body:\n")
        .replace("  ret <4 x float> %color", "  %total = fadd <4 x float> %acc, %color\n  %next = add i32 %iteration, 1\n  br label %loop_header\nexit:\n  ret <4 x float> %acc");
    for (spv, meta) in [
        garnetxlate::translate_text(&text, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text(&looped, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text_linked(
            include_str!("fixtures/guarded_coordinate_entry.ll"),
            [1, 1, 1],
            &[(
                "scale_helper",
                include_str!("fixtures/guarded_coordinate_helper.ll"),
            )],
        )
        .unwrap(),
    ] {
        let (spv, meta) = garnetxlate::lower_gpu_arguments(&spv, &meta).unwrap();
        metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
        assert_eq!(
            garnetxlate::lower_gpu_arguments(&spv, &meta).unwrap(),
            (spv.clone(), meta)
        );
        let assembly = metal2vulkan::disassemble(&spv).unwrap();
        assert_eq!(assembly.matches("OpDPdx ").count(), 1, "{assembly}");
        let guard = assembly.find("OpBranchConditional").unwrap();
        let derivative = assembly.find("OpDPdx ").unwrap();
        let sample = assembly.find("OpImageSampleExplicitLod ").unwrap();
        assert!(guard < derivative && derivative < sample, "{assembly}");
        assert!(assembly[guard..derivative].contains("OpPhi "), "{assembly}");
    }
}

#[test]
fn ordered_coordinate_prefix_keeps_the_store_before_the_read_on_all_entry_paths() {
    let entry = include_str!("fixtures/ordered_coordinate_entry.ll");
    let helper = include_str!("fixtures/guarded_coordinate_helper.ll")
        .replace(
            "ptr addrspace(2) %buffer)",
            "ptr addrspace(2) %buffer, i64 %offset)",
        )
        .replace("i64 8", "i64 %offset");
    let direct = entry.replace(
        "%scale = call float @scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2) %layers, i64 %byte_offset)",
        "%scale_ptr = getelementptr i8, ptr addrspace(2) %layers, i64 %byte_offset\n  %scale = load float, ptr addrspace(2) %scale_ptr, align 4",
    );
    // Keep buffer metadata following the visible-function declaration while removing the unused link.
    let direct = direct
        .lines()
        .filter(|line| {
            !line.contains("@scale_helper")
                && !line.starts_with("!air.visible_function_references")
                && !line.starts_with("!7 =")
        })
        .collect::<Vec<_>>()
        .join("\n");
    for (spv, metadata) in [
        garnetxlate::translate_text(&direct, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text_linked(entry, [1, 1, 1], &[("scale_helper", &helper)]).unwrap(),
    ] {
        let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
        metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
        assert_eq!(
            garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap(),
            (spv.clone(), metadata)
        );
        let assembly = metal2vulkan::disassemble(&spv).unwrap();
        assert_eq!(assembly.matches("OpDPdx ").count(), 1, "{assembly}");
        assert!(
            assembly.find("OpBranchConditional").unwrap() < assembly.find("OpStore ").unwrap(),
            "{assembly}"
        );
        assert!(
            assembly.find("OpStore ").unwrap() < assembly.find("OpDPdx ").unwrap(),
            "{assembly}"
        );
    }
}

#[test]
fn linear_coordinate_regions_survive_direct_linked_and_cached_paths() {
    let entry = include_str!("fixtures/ordered_coordinate_entry.ll")
        .replace(
            "label %arm, label %skip",
            "label %read_prologue, label %skip",
        )
        .replace(
            "arm:\n",
            "read_prologue:\n  %selected_offset = add i64 %byte_offset, 0\n  br label %arm\narm:\n",
        )
        .replace(
            "%layers, i64 %byte_offset)",
            "%layers, i64 %selected_offset)",
        );
    let helper = include_str!("fixtures/guarded_coordinate_helper.ll")
        .replace(
            "ptr addrspace(2) %buffer)",
            "ptr addrspace(2) %buffer, i64 %offset)",
        )
        .replace("i64 8", "i64 %offset");
    let direct = entry.replace(
        "%scale = call float @scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2) %layers, i64 %selected_offset)",
        "%scale_ptr = getelementptr i8, ptr addrspace(2) %layers, i64 %selected_offset\n  %scale = load float, ptr addrspace(2) %scale_ptr, align 4",
    );
    let direct = direct
        .lines()
        .filter(|line| {
            !line.contains("@scale_helper")
                && !line.starts_with("!air.visible_function_references")
                && !line.starts_with("!7 =")
        })
        .collect::<Vec<_>>()
        .join("\n");
    let looped = direct.replace("entry:\n", "entry:\n  br label %loop_header\nloop_header:\n  %iteration = phi i32 [ 0, %entry ], [ %next, %merge ]\n  %acc = phi <4 x float> [ zeroinitializer, %entry ], [ %total, %merge ]\n  %again = icmp ult i32 %iteration, 2\n  br i1 %again, label %loop_body, label %exit\nloop_body:\n")
        .replace("  ret <4 x float> %color", "  %total = fadd <4 x float> %acc, %color\n  %next = add i32 %iteration, 1\n  br label %loop_header\nexit:\n  ret <4 x float> %acc");
    for (spv, metadata) in [
        garnetxlate::translate_text(&direct, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text(&looped, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text_linked(&entry, [1, 1, 1], &[("scale_helper", &helper)]).unwrap(),
    ] {
        let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
        metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
        assert_eq!(
            garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap(),
            (spv.clone(), metadata)
        );
        let assembly = metal2vulkan::disassemble(&spv).unwrap();
        assert_eq!(assembly.matches("OpDPdx ").count(), 1, "{assembly}");
        assert!(
            assembly.find("OpBranchConditional").unwrap() < assembly.find("OpStore ").unwrap(),
            "{assembly}"
        );
        assert!(
            assembly.find("OpStore ").unwrap() < assembly.find("OpDPdx ").unwrap(),
            "{assembly}"
        );
    }
}

#[test]
fn single_predecessor_coordinate_identities_survive_linked_and_cached_paths() {
    let entry = include_str!("fixtures/ordered_coordinate_entry.ll")
        .replace("arm:\n", "arm:\n  br label %read\nread:\n  %offset = phi i64 [ %byte_offset, %arm ]\n  %base = phi ptr addrspace(2) [ %layers, %arm ]\n")
        .replace("%layers, i64 %byte_offset)", "%base, i64 %offset)")
        .replace("[ %rgba, %arm ]", "[ %rgba, %read ]");
    let helper = include_str!("fixtures/guarded_coordinate_helper.ll")
        .replace(
            "ptr addrspace(2) %buffer)",
            "ptr addrspace(2) %buffer, i64 %offset)",
        )
        .replace("i64 8", "i64 %offset");
    let (spv, metadata) =
        garnetxlate::translate_text_linked(&entry, [1, 1, 1], &[("scale_helper", &helper)]).unwrap();
    let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
    metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
    assert_eq!(
        garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap(),
        (spv.clone(), metadata)
    );
    let assembly = metal2vulkan::disassemble(&spv).unwrap();
    assert_eq!(assembly.matches("OpDPdx ").count(), 1, "{assembly}");
    assert!(
        assembly.find("OpBranchConditional").unwrap() < assembly.find("OpStore ").unwrap(),
        "{assembly}"
    );
    assert!(
        assembly.find("OpStore ").unwrap() < assembly.find("OpDPdx ").unwrap(),
        "{assembly}"
    );
}

#[test]
fn dynamic_guarded_coordinate_reads_survive_direct_linked_and_cached_paths() {
    let entry = include_str!("fixtures/guarded_coordinate_entry.ll").replace(
        "%condition = icmp",
        "%quad64 = zext i32 %quad to i64\n  %bytes = mul i64 %quad64, 4\n  %offset = add i64 %bytes, 8\n  %condition = icmp",
    ).replace(
        "ptr addrspace(2) %layers)",
        "ptr addrspace(2) %layers, i64 %offset)",
    ).replace(
        "@scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2))",
        "@scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2), i64)",
    );
    let helper = include_str!("fixtures/guarded_coordinate_helper.ll")
        .replace(
            "ptr addrspace(2) %buffer)",
            "ptr addrspace(2) %buffer, i64 %offset)",
        )
        .replace("i64 8", "i64 %offset");
    let direct = entry.replace(
        "%scale = call float @scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2) %layers, i64 %offset)",
        "%scale_ptr = getelementptr i8, ptr addrspace(2) %layers, i64 %offset\n  %scale = load float, ptr addrspace(2) %scale_ptr, align 4",
    );
    let direct = direct
        .split("\ndeclare float @scale_helper")
        .next()
        .unwrap();
    let early = direct
        .replace("%layers) {", "%layers, ptr addrspace(2) %control) {")
        .replace("entry:\n", "entry:\n  %state = load i32, ptr addrspace(2) %control, align 4\n  %empty = icmp eq i32 %state, 0\n  br i1 %empty, label %early_return, label %body\nearly_return:\n  ret <4 x float> zeroinitializer\nbody:\n")
        .replace("!2 = !{!3, !4, !5}", "!2 = !{!3, !4, !5, !8}")
        + "\n!8 = !{i32 3, !\"air.buffer\", !\"air.location_index\", i32 2, i32 1, !\"air.read\", !\"air.address_space\", i32 2, !\"air.arg_type_name\", !\"uint\"}\n";
    for (spv, meta) in [
        garnetxlate::translate_text(direct, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text(&early, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text_linked(&entry, [1, 1, 1], &[("scale_helper", &helper)]).unwrap(),
    ] {
        let (spv, meta) = garnetxlate::lower_gpu_arguments(&spv, &meta).unwrap();
        metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
        assert_eq!(
            garnetxlate::lower_gpu_arguments(&spv, &meta).unwrap(),
            (spv.clone(), meta)
        );
        let assembly = metal2vulkan::disassemble(&spv).unwrap();
        assert_eq!(assembly.matches("OpDPdx ").count(), 1, "{assembly}");
        assert_eq!(assembly.matches("OpDPdy ").count(), 1, "{assembly}");
        assert!(assembly.contains("OpTypeInt 64 0"), "{assembly}");
        assert!(
            assembly.find("OpBranchConditional").unwrap() < assembly.find("OpDPdx ").unwrap(),
            "{assembly}"
        );
    }
}

#[test]
fn bounded_constant_coordinate_reads_survive_direct_linked_and_cached_paths() {
    let original = "%coord = fmul <2 x float> %xy, <float 0.4, float 0.4>";
    let text = source(false)
        .replace("ptr addrspace(2) %layers)", "ptr addrspace(2) readonly align 4 dereferenceable(16) %layers)")
        .replace(original,
            "%scale_ptr = getelementptr i8, ptr addrspace(2) %layers, i64 8\n  \
             %scale = load float, ptr addrspace(2) %scale_ptr, align 4\n  \
             %scale0 = insertelement <2 x float> poison, float %scale, i32 0\n  \
             %scale2 = shufflevector <2 x float> %scale0, <2 x float> poison, <2 x i32> zeroinitializer\n  \
             %coord = fmul <2 x float> %xy, %scale2");
    let linked = text.replace(
        "%scale_ptr = getelementptr i8, ptr addrspace(2) %layers, i64 8\n  %scale = load float, ptr addrspace(2) %scale_ptr, align 4",
        "%scale = call float @scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2) %layers)") +
        "\ndeclare float @scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2)) section \"air.externally_defined\"\n\
         !air.visible_function_references = !{!7}\n\
         !7 = !{!\"air.visible_function_reference\", ptr @scale_helper.MTL_VISIBLE_FN_REF, !\"scale_helper\"}\n";
    let helper = "define float @scale_helper(ptr addrspace(2) %buffer) {\nentry:\n\
                  %p = getelementptr i32, ptr addrspace(2) %buffer, i64 2\n\
                  %bits = load i32, ptr addrspace(2) %p, align 4\n\
                  %value = bitcast i32 %bits to float\n\
                  ret float %value\n}\n";
    let reinterpreted = "define float @scale_helper(ptr addrspace(2) %buffer) {\nentry:\n\
                        %p = getelementptr i8, ptr addrspace(2) %buffer, i64 8\n\
                        %value = load float, ptr addrspace(2) %p, align 4\n\
                        ret float %value\n}\n";
    assert!(linked.contains("%scale = call float @scale_helper"));
    for (spv, metadata) in [
        garnetxlate::translate_text(&text, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text_linked(&linked, [1, 1, 1], &[("scale_helper", helper)]).unwrap(),
        garnetxlate::translate_text_linked(&linked, [1, 1, 1], &[("scale_helper", reinterpreted)])
            .unwrap(),
    ] {
        let (lowered, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
        metal2vulkan::tools::spirv_val_bytes(&lowered, &std::env::temp_dir()).unwrap();
        assert_eq!(
            garnetxlate::lower_gpu_arguments(&lowered, &metadata).unwrap(),
            (lowered.clone(), metadata)
        );
        let assembly = metal2vulkan::disassemble(&lowered).unwrap();
        let branch = assembly.find("OpBranchConditional").unwrap();
        assert!(assembly.find("OpDPdx ").unwrap() < branch, "{assembly}");
        assert!(
            assembly.find("OpImageSampleExplicitLod ").unwrap() > branch,
            "{assembly}"
        );
    }
}

#[test]
fn normalized_token_sampling_hoists_only_the_source_spatial_coordinate() {
    for arrayed in [false, true] {
        for linked in [false, true] {
            let text = source(arrayed);
            let (spv, meta) = if linked {
                garnetxlate::translate_bytes_linked(text.as_bytes(), [1, 1, 1], &[])
            } else {
                garnetxlate::translate_bytes(text.as_bytes(), [1, 1, 1])
            }
            .unwrap_or_else(|error| panic!("arrayed={arrayed} linked={linked}: {error}"));
            metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
            let lowered = if serde_json::from_slice::<Value>(&meta).unwrap()
                ["argument_buffer_fields"]
                .as_array()
                .is_some_and(|fields| !fields.is_empty())
            {
                garnetxlate::lower_gpu_arguments(&spv, &meta).unwrap().0
            } else {
                spv
            };
            metal2vulkan::tools::spirv_val_bytes(&lowered, &std::env::temp_dir()).unwrap();
            let assembly = metal2vulkan::disassemble(&lowered).unwrap();
            assert_eq!(assembly.matches("OpDPdx ").count(), 1, "{assembly}");
            assert_eq!(assembly.matches("OpDPdy ").count(), 1, "{assembly}");
            let first_branch = assembly.find("OpBranchConditional").unwrap();
            assert!(
                assembly.find("OpDPdx ").unwrap() < first_branch,
                "{assembly}"
            );
            assert!(
                assembly.find("OpImageSampleExplicitLod ").unwrap() > first_branch,
                "{assembly}"
            );
        }
    }
}

#[test]
fn affine_coordinate_math_survives_direct_linked_and_cached_paths() {
    let arithmetic = "%coord = call <2 x float> @air.fma.v2f32(<2 x float> %xy, <2 x float> <float 0.4, float 0.4>, <2 x float> <float 0.125, float 0.625>)";
    let original = "%coord = fmul <2 x float> %xy, <float 0.4, float 0.4>";
    let declaration =
        "\ndeclare <2 x float> @air.fma.v2f32(<2 x float>, <2 x float>, <2 x float>)\n";
    let direct = source(false).replace(original, arithmetic) + declaration;
    let linked = source(false).replace(original,
        "%coord = call <2 x float> @affine_helper.MTL_VISIBLE_FN_REF(<2 x float> %xy)")
        + "\ndeclare <2 x float> @affine_helper.MTL_VISIBLE_FN_REF(<2 x float>) section \"air.externally_defined\"\n\
           !air.visible_function_references = !{!7}\n\
           !7 = !{!\"air.visible_function_reference\", ptr @affine_helper.MTL_VISIBLE_FN_REF, !\"affine_helper\"}\n";
    let helper = format!("define <2 x float> @affine_helper(<2 x float> %xy) {{\nentry:\n  {arithmetic}\n  ret <2 x float> %coord\n}}\n{declaration}");
    for (spv, metadata) in [
        garnetxlate::translate_text(&direct, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text_linked(&linked, [1, 1, 1], &[("affine_helper", &helper)])
            .unwrap(),
    ] {
        let (lowered, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
        metal2vulkan::tools::spirv_val_bytes(&lowered, &std::env::temp_dir()).unwrap();
        let repeated = garnetxlate::lower_gpu_arguments(&lowered, &metadata).unwrap();
        assert_eq!(repeated.0, lowered);
        assert_eq!(repeated.1, metadata);
        let assembly = metal2vulkan::disassemble(&lowered).unwrap();
        let branch = assembly.find("OpBranchConditional").unwrap();
        assert_eq!(assembly.matches("OpDPdx ").count(), 1, "{assembly}");
        assert_eq!(assembly.matches("OpDPdy ").count(), 1, "{assembly}");
        assert!(assembly.find("OpDPdx ").unwrap() < branch, "{assembly}");
        assert!(
            assembly.find("OpImageSampleExplicitLod ").unwrap() > branch,
            "{assembly}"
        );
    }
}

#[test]
fn flat_cached_coordinate_facts_survive_serialization_and_reject_corruption() {
    let source = include_bytes!("../../tests/fixtures/fragment_sampler_helper.ll");
    let (spv, meta) = garnetxlate::translate_bytes(source, [1, 1, 1]).unwrap();
    let metadata: Value = serde_json::from_slice(&meta).unwrap();
    let proof = &metadata["uniform_control_proof"];
    assert_eq!(proof["version"], 2);
    assert_eq!(proof["sampling_coordinates"].as_array().unwrap().len(), 2);
    for mutation in [
        "duplicate",
        "unknown",
        "non_sample",
        "cross_function",
        "missing",
    ] {
        let mut metadata = metadata.clone();
        let function_id = metadata["uniform_control_proof"]["entries"][0]["function"].clone();
        let facts = metadata["uniform_control_proof"]["sampling_coordinates"]
            .as_array_mut()
            .unwrap();
        match mutation {
            "duplicate" => facts.push(facts[0].clone()),
            "unknown" => facts[0]["coordinate"] = Value::from(u32::MAX),
            "non_sample" => facts[0]["sample"] = facts[0]["coordinate"].clone(),
            "cross_function" => facts[0]["coordinate"] = function_id,
            "missing" => {
                metadata["uniform_control_proof"]
                    .as_object_mut()
                    .unwrap()
                    .remove("sampling_coordinates");
            }
            _ => unreachable!(),
        }
        let error = garnetxlate::lower_gpu_arguments(&spv, &serde_json::to_vec(&metadata).unwrap())
            .unwrap_err();
        assert!(error.contains("coordinate"), "{mutation}: {error}");
    }
}

#[test]
fn depth_and_comparison_samples_keep_their_source_spatial_coordinate() {
    for arrayed in [false, true] {
        for comparison in [false, true] {
            let suffix = if arrayed { "2d_array" } else { "2d" };
            let family = if comparison {
                "sample_compare_depth"
            } else {
                "sample_depth"
            };
            let layer = if arrayed { "i32 %layer," } else { "" };
            let layer_type = if arrayed { "i32," } else { "" };
            let mut text = source(arrayed)
                .replace(&format!("sample_texture_{suffix}.v4f32"), &format!("{family}_{suffix}.f32"))
                .replace("%sample = call { <4 x float>, i8 }", "%sample = call { float, i8 }")
                .replace("declare { <4 x float>, i8 }", "declare { float, i8 }")
                .replace("ptr addrspace(2) %sampler, <2 x float>", "ptr addrspace(2) %sampler, i32 0, <2 x float>")
                .replace("ptr addrspace(2), <2 x float>", "ptr addrspace(2), i32, <2 x float>")
                .replace("%rgba = extractvalue { <4 x float>, i8 } %sample, 0", "%depth = extractvalue { float, i8 } %sample, 0\n  %rgba = insertelement <4 x float> zeroinitializer, float %depth, i32 0");
            if comparison {
                text = text
                    .replace(
                        &format!("%coord, {layer} i1"),
                        &format!("%coord, {layer} float 0.5, i1"),
                    )
                    .replace(
                        &format!("<2 x float>, {layer_type} i1"),
                        &format!("<2 x float>, {layer_type} float, i1"),
                    );
            }
            for guarded in [false, true] {
                let text = if guarded {
                    text.replace("%coord = fmul <2 x float> %xy, <float 0.4, float 0.4>",
                "%factor = load float, ptr addrspace(2) %layers, align 4\n  %factor0 = insertelement <2 x float> poison, float %factor, i32 0\n  %factor2 = shufflevector <2 x float> %factor0, <2 x float> poison, <2 x i32> zeroinitializer\n  %coord = fmul <2 x float> %xy, %factor2")
                } else {
                    text.clone()
                };
                let (spv, metadata) = garnetxlate::translate_bytes(text.as_bytes(), [1, 1, 1])
                    .unwrap_or_else(|error| panic!("{family} arrayed={arrayed}: {error}"));
                let (spv, _) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
                metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
                let assembly = metal2vulkan::disassemble(&spv).unwrap();
                assert_eq!(assembly.matches("OpDPdx ").count(), 1, "{assembly}");
                assert!(
                    (assembly.find("OpDPdx ").unwrap()
                        < assembly.find("OpBranchConditional").unwrap())
                        != guarded,
                    "{assembly}"
                );
                assert!(
                    assembly.contains(if comparison {
                        "OpImageSampleDrefExplicitLod"
                    } else {
                        "OpImageSampleExplicitLod"
                    }),
                    "{assembly}"
                );
            }
        }
    }
}
