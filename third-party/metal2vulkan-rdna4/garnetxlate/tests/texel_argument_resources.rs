use std::path::PathBuf;

#[test]
fn dynamic_texture_buffer_arguments_lower_to_texel_descriptors() {
    let source = include_str!("fixtures/argument_resource_texture_buffer_graph.ll");
    let (spv, metadata) = garnetxlate::translate_text_gpu_arguments(source, [32, 1, 1])
        .expect("lower dynamic texture-buffer argument array");
    let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(metadata["argument_resource_tables"]["version"], 10);
    assert_eq!(
        metadata["argument_resource_tables"]["categories"],
        serde_json::json!([3])
    );
    let table: metal2vulkan::reflect::ArgumentResourceTables =
        serde_json::from_value(metadata["argument_resource_tables"].clone()).unwrap();
    assert_eq!(table.version, 10);
    assert!(table.null_shapes.is_empty());
    assert_eq!(
        metadata["bindings"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|binding| binding["descriptor"]
                == serde_json::json!({"set":2,"binding":1,"count":0}))
            .count(),
        1
    );

    let directory = std::env::temp_dir().join(format!("rdna4-texel-graph-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path: PathBuf = directory.join("shader.spv");
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
    let text = String::from_utf8(disassembly.stdout).unwrap();
    assert!(
        text.contains("DescriptorSet 2") && text.contains("Binding 1"),
        "{text}"
    );
    assert!(
        text.contains("OpImageFetch") && text.contains("Buffer"),
        "{text}"
    );
    std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn dynamic_storage_texture_buffer_arguments_use_storage_texel_set() {
    let source = include_str!("fixtures/argument_resource_texture_buffer_graph.ll")
        .replace("texture_buffer<float, read>", "texture_buffer<float, read_write>")
        .replace(
            "i32 %index, i32 1)",
            "i32 %index, i32 3)",
        )
        .replace(
            "  %rgba = extractvalue",
            "  call void @air.write_texture_buffer_1d.v4f32(ptr addrspace(1) %texture, i32 %index, <4 x float> zeroinitializer, i32 3)\n  %rgba = extractvalue",
        )
        .replace(
            "declare { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1), i32, i32)",
            "declare { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1), i32, i32)\ndeclare void @air.write_texture_buffer_1d.v4f32(ptr addrspace(1), i32, <4 x float>, i32)",
        );
    let (spv, metadata) = garnetxlate::translate_text_gpu_arguments(&source, [32, 1, 1])
        .expect("lower dynamic storage texture-buffer argument array");
    let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(metadata["argument_resource_tables"]["version"], 10);
    assert_eq!(
        metadata["argument_resource_tables"]["categories"],
        serde_json::json!([4])
    );
    let table: metal2vulkan::reflect::ArgumentResourceTables =
        serde_json::from_value(metadata["argument_resource_tables"].clone()).unwrap();
    assert_eq!(table.version, 10);
    assert!(table.null_shapes.is_empty());
    assert_eq!(
        metadata["bindings"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|binding| binding["descriptor"]
                == serde_json::json!({"set":3,"binding":1,"count":0}))
            .count(),
        1
    );

    let directory =
        std::env::temp_dir().join(format!("rdna4-storage-texel-graph-{}", std::process::id()));
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
    let text = String::from_utf8(disassembly.stdout).unwrap();
    assert!(
        text.contains("DescriptorSet 3") && text.contains("Binding 1"),
        "{text}"
    );
    assert!(
        text.contains("OpImageRead") && text.contains("OpImageWrite") && text.contains("Buffer"),
        "{text}"
    );
    let read = text
        .lines()
        .find(|line| line.contains("OpImageRead"))
        .unwrap();
    assert!(
        !read.contains("Lod"),
        "texture-buffer reads have no LOD operand: {read}"
    );
    std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn dynamic_texture_buffer_normalized_production_path() {
    let uniform_source =
        include_str!("fixtures/argument_resource_texture_buffer_graph.ll").to_string();
    let storage_source = uniform_source
        .replace("texture_buffer<float, read>", "texture_buffer<float, read_write>")
        .replace("i32 %index, i32 1)", "i32 %index, i32 3)")
        .replace(
            "  %rgba = extractvalue",
            "  call void @air.write_texture_buffer_1d.v4f32(ptr addrspace(1) %texture, i32 %index, <4 x float> zeroinitializer, i32 3)\n  %rgba = extractvalue",
        )
        .replace(
            "declare { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1), i32, i32)",
            "declare { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1), i32, i32)\ndeclare void @air.write_texture_buffer_1d.v4f32(ptr addrspace(1), i32, <4 x float>, i32)",
        );

    let directory =
        std::env::temp_dir().join(format!("rdna4-texel-norm-prod-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();

    for (label, source, expected_category) in [
        ("uniform", uniform_source, 3),
        ("storage", storage_source, 4),
    ] {
        let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [32, 1, 1])
            .expect("translate dynamic texture-buffer on production path");
        let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata)
            .expect("lower dynamic texture-buffer with normalized metadata");
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(
            metadata["argument_resource_tables"]["version"], 10,
            "{label}"
        );
        assert_eq!(metadata["normalized_sampler_coordinates"], true, "{label}");
        assert_eq!(
            metadata["argument_resource_tables"]["categories"],
            serde_json::json!([expected_category]),
            "{label}"
        );
        let path = directory.join(format!("{label}.spv"));
        std::fs::write(&path, spv).unwrap();
        let validation = std::process::Command::new("spirv-val")
            .args(["--target-env", "vulkan1.2"])
            .arg(&path)
            .output()
            .unwrap();
        assert!(
            validation.status.success(),
            "{label}: {}",
            String::from_utf8_lossy(&validation.stderr)
        );
    }

    std::fs::remove_dir_all(directory).unwrap();
}
