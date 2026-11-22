fn validate_and_disassemble(spv: &[u8], label: &str) -> String {
    let directory =
        std::env::temp_dir().join(format!("rdna4-image-{label}-{}", std::process::id()));
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

#[test]
fn dynamic_image_matrix_preserves_types_lod_grad_queries_and_storage() {
    let (spv, metadata) = garnetxlate::translate_text_gpu_arguments(
        include_str!("fixtures/argument_image_sampling_queries.ll"),
        [8, 1, 1],
    )
    .unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(
        metadata["argument_resource_tables"]["categories"],
        serde_json::json!([0, 1, 2])
    );
    assert_eq!(
        metadata["argument_resource_tables"]["image_query_binding"],
        673
    );

    let assembly = validate_and_disassemble(&spv, "matrix");
    for expected in [
        "OpTypeImage %float 2D",
        "OpTypeImage %uint 2D",
        "OpTypeImage %int 2D",
        "OpImageSampleExplicitLod",
        "OpImageSampleDrefExplicitLod",
        "Grad",
        "ConstOffset",
        "OpImageFetch",
        "OpImageRead",
        "OpImageWrite",
        "Binding 672",
        "Binding 673",
        "DescriptorSet 0",
        "DescriptorSet 2",
        "DescriptorSet 3",
        "DescriptorSet 4",
    ] {
        assert!(
            assembly.contains(expected),
            "missing {expected}:\n{assembly}"
        );
    }
    assert!(
        assembly.matches("OpImageWrite").count() == 3,
        "one write per selected float/uint/sint token:\n{assembly}"
    );
    assert!(
        assembly
            .lines()
            .filter(|line| line.contains("OpImageSampleExplicitLod"))
            .any(|line| line.contains("Lod|ConstOffset")),
        "explicit level and nonzero offset must share one sample:\n{assembly}"
    );
    assert!(
        assembly
            .lines()
            .filter(|line| line.contains("OpImageSampleExplicitLod"))
            .any(|line| line.contains("Grad")),
        "gradient sample must retain Grad:\n{assembly}"
    );
    assert!(
        assembly
            .lines()
            .any(|line| { line.contains("OpShiftRightLogical %uint") && line.contains("OpLoad") })
            || (assembly.contains("OpShiftRightLogical %uint")
                && assembly.contains("OpULessThan %bool")),
        "query lowering must retain mip shifting and bounds checks:\n{assembly}"
    );
}

#[test]
fn storage_writes_require_the_macos_zero_lod_constant() {
    let original = include_str!("fixtures/argument_image_sampling_queries.ll");
    let write = "call void @air.write_texture_2d.v4f32(ptr addrspace(1) %storage_f, <2 x i32> zeroinitializer, <4 x float> %write_texel, i32 0, i32 3)";
    assert!(original.contains(write), "fixture write anchor changed");
    for (lod, expected) in [("i32 1", "zero LOD"), ("i32 %mip", "zero LOD")] {
        let source = original.replacen(
            write,
            &write.replace(
                "<4 x float> %write_texel, i32 0, i32 3",
                &format!("<4 x float> %write_texel, {lod}, i32 3"),
            ),
            1,
        );
        let error = garnetxlate::translate_text_gpu_arguments(&source, [8, 1, 1]).unwrap_err();
        assert!(error.contains(expected), "{error}");
    }
}

#[test]
fn storage_reads_reject_a_lod_the_spirv_op_cannot_carry() {
    let original = include_str!("fixtures/argument_image_sampling_queries.ll");
    let read = "call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %storage_f, <2 x i32> zeroinitializer, i32 0, i32 3)";
    assert!(original.contains(read), "fixture read anchor changed");
    for (lod, expected) in [
        ("i32 2", "storage image read requires zero LOD"),
        ("i32 %mip", "storage image read requires zero LOD"),
    ] {
        let source = original.replacen(
            read,
            &read.replace("i32 0, i32 3", &format!("{lod}, i32 3")),
            1,
        );
        let error = garnetxlate::translate_text_gpu_arguments(&source, [8, 1, 1]).unwrap_err();
        assert!(error.contains(expected), "{error}");
    }
}

#[test]
fn storage_accepts_constant_zero_initializer_lod() {
    let original = include_str!("fixtures/argument_image_sampling_queries.ll");
    let source = original
        .replace(
            "call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %storage_f, <2 x i32> zeroinitializer, i32 0, i32 3)",
            "call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %storage_f, <2 x i32> zeroinitializer, i32 zeroinitializer, i32 3)",
        )
        .replace(
            "call void @air.write_texture_2d.v4f32(ptr addrspace(1) %storage_f, <2 x i32> zeroinitializer, <4 x float> %write_texel, i32 0, i32 3)",
            "call void @air.write_texture_2d.v4f32(ptr addrspace(1) %storage_f, <2 x i32> zeroinitializer, <4 x float> %write_texel, i32 zeroinitializer, i32 3)",
        );
    let (spv, _) = garnetxlate::translate_text_gpu_arguments(&source, [8, 1, 1]).unwrap();
    let assembly = validate_and_disassemble(&spv, "storage-zero-initializer");
    assert!(assembly.contains("OpImageRead") && assembly.contains("OpImageWrite"));
}

#[test]
fn integer_gradient_samples_are_refused_without_emitting_an_invalid_instruction() {
    let original = include_str!("fixtures/argument_image_sampling_queries.ll");
    let source = original
        .replace(
            "%gradient_value = call float @sample_float_gradient(ptr addrspace(1) %image,",
            "%gradient_value = call float @sample_float_gradient(ptr addrspace(1) %image_u,",
        )
        .replace(
            "@air.sample_texture_2d_grad.v4f32",
            "@air.sample_texture_2d_grad.u.v4i32",
        )
        .replace(
            "declare { <4 x float>, i8 } @air.sample_texture_2d_grad.v4f32",
            "declare { <4 x float>, i8 } @air.sample_texture_2d_grad.u.v4i32",
        )
        .replace(
            "%grad_sample = call { <4 x float>, i8 }",
            "%grad_sample = call { <4 x i32>, i8 }",
        )
        .replace(
            "%grad_rgba = extractvalue { <4 x float>, i8 }",
            "%grad_rgba = extractvalue { <4 x i32>, i8 }",
        )
        .replace(
            "%green = extractelement <4 x float> %grad_rgba, i32 1",
            "%green_i = extractelement <4 x i32> %grad_rgba, i32 1\n  %green = sitofp i32 %green_i to float",
        );
    let error = garnetxlate::translate_text_gpu_arguments(&source, [8, 1, 1]).unwrap_err();
    assert!(
        error.contains("integer texture") && error.contains("gradient"),
        "{error}"
    );
}
