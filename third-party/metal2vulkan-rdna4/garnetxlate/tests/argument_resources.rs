#[test]
fn real_argument_texture_sampler_air_translates_with_bound_resources() {
    let source = include_str!("fixtures/argument_resources.ll");
    let (spv, metadata) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let reflection: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    let resources = reflection["bindings"].as_array().unwrap();
    let texture = resources
        .iter()
        .find(|binding| binding["kind"] == "EmbeddedArgBufferTexture")
        .unwrap();
    assert_eq!(
        texture["descriptor"],
        serde_json::json!({"set": 0, "binding": 32, "count": 1})
    );
    assert_eq!(
        texture["embedded_source"],
        serde_json::json!({
            "buffer_param_index": 0,
            "buffer_index": 3,
            "field_offset": 0,
            "field_ordinal": 0,
            "argument_index": 2,
            "resource_buffer_index": null
        })
    );
    let sampler = resources
        .iter()
        .find(|binding| binding["kind"] == "EmbeddedArgBufferSampler")
        .unwrap();
    assert_eq!(
        sampler["descriptor"],
        serde_json::json!({"set": 0, "binding": 160, "count": 1})
    );
    assert_eq!(
        sampler["embedded_source"],
        serde_json::json!({
            "buffer_param_index": 0,
            "buffer_index": 3,
            "field_offset": 8,
            "field_ordinal": 1,
            "argument_index": 7,
            "resource_buffer_index": null
        })
    );
    assert_eq!(reflection["entry_point"], "argument_resources");
    let out = std::env::var_os("ARGUMENT_RESOURCES_EVIDENCE")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            std::env::temp_dir().join(format!("rdna4-resources-{}", std::process::id()))
        });
    std::fs::create_dir_all(&out).unwrap();
    std::fs::write(out.join("resources.spv"), &spv).unwrap();
    std::fs::write(
        out.join("resources.meta.json"),
        serde_json::to_vec_pretty(&reflection).unwrap(),
    )
    .unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(out.join("resources.spv"))
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "spirv-val failed: {}",
        String::from_utf8_lossy(&validation.stderr)
    );
    if std::env::var_os("ARGUMENT_RESOURCES_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}
