fn validate_and_disassemble(spv: &[u8], label: &str) -> String {
    let directory =
        std::env::temp_dir().join(format!("rdna4-token-merge-{label}-{}", std::process::id()));
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
    assert!(
        disassembly.status.success(),
        "{}",
        String::from_utf8_lossy(&disassembly.stderr)
    );
    std::fs::remove_dir_all(directory).unwrap();
    String::from_utf8(disassembly.stdout).unwrap()
}

fn entry_function_body(assembly: &str) -> &str {
    let entry_id = assembly
        .lines()
        .find(|line| line.trim_start().starts_with("OpEntryPoint "))
        .and_then(|line| line.split_whitespace().nth(2))
        .unwrap_or_else(|| panic!("entry point missing:\n{assembly}"));
    let function = format!("{entry_id} = OpFunction");
    let start = assembly
        .find(&function)
        .unwrap_or_else(|| panic!("entry function missing:\n{assembly}"));
    let rest = &assembly[start..];
    let end = rest
        .find("OpFunctionEnd")
        .unwrap_or_else(|| panic!("entry function end missing:\n{assembly}"));
    &rest[..end]
}

fn has_call_consuming_merge_pair(entry: &str, opcode: &str, ty: &str) -> bool {
    let mut defs = Vec::<(&str, Option<&str>)>::new();
    for line in entry.lines() {
        let fields = line.split_whitespace().collect::<Vec<_>>();
        if fields.get(1) == Some(&"=")
            && fields.get(2) == Some(&opcode)
            && fields.get(3) == Some(&ty)
        {
            defs.push((fields[0], fields.get(4).copied()));
        }
    }
    entry.lines().any(|line| {
        let fields = line.split_whitespace().collect::<Vec<_>>();
        if !fields.contains(&"OpFunctionCall") || fields.len() < 6 {
            return false;
        }
        let call_operands = &fields[5..];
        let used = defs
            .iter()
            .filter(|(result, _)| call_operands.contains(result))
            .collect::<Vec<_>>();
        if opcode == "OpSelect" {
            used.iter().enumerate().any(|(index, (_, condition))| {
                used[index + 1..]
                    .iter()
                    .any(|(_, other)| condition.is_some() && condition == other)
            })
        } else {
            used.len() >= 2
        }
    })
}

fn has_uint_select_pair_with_one_condition(entry: &str) -> bool {
    let mut conditions = Vec::<&str>::new();
    for line in entry.lines() {
        let fields = line.split_whitespace().collect::<Vec<_>>();
        if fields.get(1) == Some(&"=")
            && fields.get(2) == Some(&"OpSelect")
            && fields.get(3) == Some(&"%uint")
        {
            let condition = fields[4];
            if conditions.contains(&condition) {
                return true;
            }
            conditions.push(condition);
        }
    }
    false
}

#[test]
fn token_select_uses_two_uint_words() {
    let source = include_str!("fixtures/argument_graph_token_select.ll");
    let (spv, metadata) = garnetxlate::translate_text_gpu_arguments(source, [1, 1, 1]).unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(
        metadata["argument_resource_tables"]["categories"],
        serde_json::json!([0, 2])
    );
    let assembly = validate_and_disassemble(&spv, "select");
    let entry = entry_function_body(&assembly);
    assert!(
        has_call_consuming_merge_pair(entry, "OpSelect", "%uint"),
        "{assembly}"
    );
    assert!(assembly.contains("OpSampledImage"), "{assembly}");
    assert!(assembly.contains("DescriptorSet 2"), "{assembly}");
}

#[test]
fn token_loop_fixed_point_materializes_texture_phi() {
    let source = include_str!("fixtures/argument_graph_token_loop.ll");
    let (spv, _) = garnetxlate::translate_text_gpu_arguments(source, [1, 1, 1]).unwrap();
    let assembly = validate_and_disassemble(&spv, "loop");
    let entry = entry_function_body(&assembly);
    assert!(has_uint_select_pair_with_one_condition(entry), "{assembly}");
    assert!(
        has_call_consuming_merge_pair(entry, "OpPhi", "%uint"),
        "{assembly}"
    );
    assert!(assembly.contains("OpSampledImage"), "{assembly}");
}

#[test]
fn token_helper_parameter_and_return_are_inlined_before_payload_merge() {
    let source = include_str!("fixtures/argument_graph_token_helper.ll");
    let (spv, _) = garnetxlate::translate_text_gpu_arguments(source, [1, 1, 1]).unwrap();
    let assembly = validate_and_disassemble(&spv, "helper");
    assert!(!assembly.contains("pick_texture"), "{assembly}");
    let entry = entry_function_body(&assembly);
    assert!(
        has_call_consuming_merge_pair(entry, "OpSelect", "%uint"),
        "{assembly}"
    );
    assert!(assembly.contains("OpSampledImage"), "{assembly}");
}

#[test]
fn token_select_rejects_result_shape_conflicting_with_an_arm() {
    let source = include_str!("fixtures/argument_graph_token_select_shape_mismatch.ll");
    let error = garnetxlate::translate_text_gpu_arguments(source, [1, 1, 1]).unwrap_err();
    assert!(
        error.contains("select") && error.contains("merges incompatible image types"),
        "{error}"
    );
}

#[test]
fn token_phi_rejects_incompatible_incoming_image_types() {
    let source = include_str!("fixtures/argument_graph_token_phi_shape_mismatch.ll");
    let error = garnetxlate::translate_text_gpu_arguments(source, [1, 1, 1]).unwrap_err();
    assert!(
        error.contains("phi") && error.contains("merges incompatible image types"),
        "{error}"
    );
}

#[test]
fn token_lookup_deduplicates_across_switch_arms_and_dominators() {
    // Same runtime texture token sampled across a 4-arm switch (mirroring static_sampler_table)
    // plus a post-switch dominated sample must hoist & reuse a single binding-672 lookup call
    // for the texture token rather than emitting 5 redundant binary-search loops.
    let source = r#"
target triple = "air64-apple-macosx"
%Root = type { ptr addrspace(2) }
%Record = type { ptr addrspace(1), [4 x ptr addrspace(2)] }

define void @switch_lookup_cse(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {
entry:
  %slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0
  %base = load ptr addrspace(2), ptr addrspace(2) %slot, align 8
  %img_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 0
  %tok = load i64, ptr addrspace(2) %img_slot, align 8
  %image = inttoptr i64 %tok to ptr addrspace(1)
  %s0_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 1, i64 0
  %s1_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 1, i64 1
  %s2_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 1, i64 2
  %s3_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 1, i64 3
  %s0 = load ptr addrspace(2), ptr addrspace(2) %s0_slot, align 8
  %s1 = load ptr addrspace(2), ptr addrspace(2) %s1_slot, align 8
  %s2 = load ptr addrspace(2), ptr addrspace(2) %s2_slot, align 8
  %s3 = load ptr addrspace(2), ptr addrspace(2) %s3_slot, align 8
  %arm = and i32 %i, 3
  switch i32 %arm, label %case3 [
    i32 0, label %case0
    i32 1, label %case1
    i32 2, label %case2
  ]
case0:
  %r0 = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %s0, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %v0 = extractvalue { <4 x float>, i8 } %r0, 0
  %f0 = extractelement <4 x float> %v0, i32 0
  br label %done
case1:
  %r1 = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %s1, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %v1 = extractvalue { <4 x float>, i8 } %r1, 0
  %f1 = extractelement <4 x float> %v1, i32 0
  br label %done
case2:
  %r2 = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %s2, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %v2 = extractvalue { <4 x float>, i8 } %r2, 0
  %f2 = extractelement <4 x float> %v2, i32 0
  br label %done
case3:
  %r3 = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %s3, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %v3 = extractvalue { <4 x float>, i8 } %r3, 0
  %f3 = extractelement <4 x float> %v3, i32 0
  br label %done
done:
  %p = phi float [ %f0, %case0 ], [ %f1, %case1 ], [ %f2, %case2 ], [ %f3, %case3 ]
  %r4 = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %s0, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %v4 = extractvalue { <4 x float>, i8 } %r4, 0
  %f4 = extractelement <4 x float> %v4, i32 0
  %sum = fadd float %p, %f4
  store float %sum, ptr addrspace(1) %out, align 4
  ret void
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

!air.kernel = !{!0}
!0 = !{ptr @switch_lookup_cse, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
"#;
    let (spv, _) = garnetxlate::translate_text_gpu_arguments(source, [1, 1, 1]).unwrap();
    let assembly = validate_and_disassemble(&spv, "switch_lookup_cse");
    let entry = entry_function_body(&assembly);
    let lookup_calls: Vec<&str> = entry
        .lines()
        .filter(|l| l.contains("= OpFunctionCall %uint "))
        .collect();
    // 1 hoisted lookup for `%image` (down from 5) + 4 distinct sampler lookups (`%s0` reused too -> 5 total, not 10!).
    assert_eq!(
        lookup_calls.len(),
        5,
        "expected 1 hoisted texture lookup + 4 unique sampler lookups (not 10):\n{}",
        lookup_calls.join("\n")
    );
}
