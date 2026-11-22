//! Authored MSL 2.1 cube operations captured by test-argument-image-operations.m.

use serde_json::Value;

#[test]
fn indirect_cube_array_read_preserves_face_slice_and_explicit_mip() {
    let source = include_str!("fixtures/indirect_cube_array_read.ll");
    for lod in [0, 1, 2] {
        let source = source.replace(
            "i32 2, i32 1, i32 0, i32 0)",
            &format!("i32 2, i32 1, i32 {lod}, i32 0)"),
        );
        let (bytes, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1])
            .expect("sampled cube-array reads must translate");
        assert_eq!(
            garnetxlate::translate_bytes_linked(source.as_bytes(), [1, 1, 1], &[]).unwrap(),
            (bytes.clone(), metadata.clone()),
            "linked reads must use the same contract"
        );
        let reflection: Value = serde_json::from_slice(&metadata).unwrap();
        let samplers = reflection["bindings"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|binding| binding.get("static_sampler"))
            .filter(|state| !state.is_null())
            .collect::<Vec<_>>();
        assert_eq!(samplers.len(), 1, "one exact synthesized read sampler");
        for field in ["min_filter", "mag_filter", "mip_filter"] {
            assert_eq!(samplers[0][field], "Nearest");
        }
        assert_eq!(samplers[0]["lod_bias"], 0.0);
        assert!(samplers[0]["lod_max_clamp"].as_f64().unwrap() >= 2.0);
        let (bytes, _) = garnetxlate::lower_gpu_arguments(&bytes, &metadata)
            .expect("cached cube-array reads must preserve their shape");
        let directory = std::env::temp_dir().join(format!(
            "rdna4-cube-array-read-{}-{lod}",
            std::process::id()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("shader.spv");
        std::fs::write(&path, bytes).unwrap();
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
        std::fs::remove_dir_all(directory).unwrap();
    }
}

#[test]
fn indirect_cube_read_sample_gradient_query_and_write_share_the_shape_contract() {
    let source = include_str!("fixtures/indirect_cube_operations.ll");
    let (bytes, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1])
        .expect("legal MSL 2.1 cube writes must translate");
    let (bytes, metadata) = garnetxlate::lower_gpu_arguments(&bytes, &metadata)
        .expect("cached cube operations must use the same shape registry");
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    let shapes = metadata["argument_resource_tables"]["null_shapes"]
        .as_array()
        .unwrap();
    assert!(shapes.contains(&Value::from(3)), "sampled cube dummy");
    assert!(shapes.contains(&Value::from(0x103)), "storage cube dummy");
    let directory = std::env::temp_dir().join(format!("rdna4-cube-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("shader.spv");
    std::fs::write(&path, bytes).unwrap();
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
    let text = String::from_utf8(result.stdout).unwrap();
    assert!(text.contains("OpImageWrite"));
    assert!(text.contains("OpImageSampleExplicitLod"));
    assert!(text.contains("OpImageQuerySizeLod"));
    std::fs::remove_dir_all(directory).unwrap();
}
