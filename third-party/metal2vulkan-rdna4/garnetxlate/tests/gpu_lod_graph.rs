use metal2vulkan::passes::QueryLodReplay;

#[test]
fn lod_queries_resolve_both_dynamic_graph_operands() {
    let original = include_str!("fixtures/argument_lod_graph.ll");
    for (query, query_first) in [
        ("unclamped", false),
        ("clamped", false),
        ("unclamped", true),
        ("clamped", true),
    ] {
        let source = original.replace("calculate_unclamped_lod", &format!("calculate_{query}_lod"));
        let source = if query_first {
            let line = source
                .lines()
                .find(|line| line.trim_start().starts_with("%lod ="))
                .unwrap();
            source
                .replacen(line, "", 1)
                .replacen("  %sample =", &format!("{line}\n  %sample ="), 1)
        } else {
            source
        };
        let (spv, metadata) = garnetxlate::translate_text_gpu_arguments_abi8_with_query_lod_replay(
            &source,
            [1, 1, 1],
            QueryLodReplay::UniformOnly,
        )
        .unwrap();
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(
            metadata["argument_resource_tables"]["categories"],
            serde_json::json!([0, 2])
        );
        assert_eq!(
            metadata["argument_resource_tables"]["image_query_binding"],
            673
        );
        let directory =
            std::env::temp_dir().join(format!("rdna4-lod-graph-{}-{query}", std::process::id()));
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
        assert!(
            assembly.lines().any(|line| line.ends_with(" Binding 3")),
            "{assembly}"
        );
        for expected in [
            "Binding 672",
            "Binding 673",
            "DescriptorSet 2",
            "DescriptorSet 4",
            "OpImageQueryLod",
        ] {
            assert!(assembly.contains(expected), "{expected}\n{assembly}");
        }
    }
}

#[test]
fn lod_query_graph_rejects_a_storage_image() {
    let source = include_str!("fixtures/argument_lod_graph.ll")
        .lines()
        .map(|line| {
            if line.trim_start().starts_with("%sample =") {
                "  %sample = call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x i32> zeroinitializer, <2 x i32> zeroinitializer, i32 0, i32 3)"
            } else {
                line
            }
        })
        .collect::<Vec<_>>()
        .join("\n");
    let source = format!("{source}\ndeclare {{ <4 x float>, i8 }} @air.read_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32)\n");
    let error = garnetxlate::translate_text_gpu_arguments_abi8_with_query_lod_replay(
        &source,
        [1, 1, 1],
        QueryLodReplay::UniformOnly,
    )
    .unwrap_err();
    assert!(
        error.contains("requires a non-multisampled sampled texture"),
        "{error}"
    );
}

#[test]
fn lod_query_graph_without_a_component_type_is_rejected() {
    let source = include_str!("fixtures/argument_lod_graph.ll")
        .lines()
        .filter(|line| {
            !["%sample =", "%rgba =", "%red ="]
                .iter()
                .any(|prefix| line.trim_start().starts_with(prefix))
        })
        .collect::<Vec<_>>()
        .join("\n")
        .replace("float %red,", "float 0.0,");
    let error = garnetxlate::translate_text_gpu_arguments_abi8_with_query_lod_replay(
        &source,
        [1, 1, 1],
        QueryLodReplay::UniformOnly,
    )
    .unwrap_err();
    assert!(
        error.contains("no established component/access type"),
        "{error}"
    );
}
