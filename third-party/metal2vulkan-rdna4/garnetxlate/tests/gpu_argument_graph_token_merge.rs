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
