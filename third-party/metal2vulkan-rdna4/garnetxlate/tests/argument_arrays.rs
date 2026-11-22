use serde_json::Value;

#[test]
fn wrapped_pointer_and_constant_arrays_keep_every_argument_coordinate() {
    let source = std::env::var_os("ARGUMENT_ARRAYS_SOURCE")
        .map(|path| std::fs::read_to_string(path).unwrap())
        .unwrap_or_else(|| include_str!("fixtures/argument_arrays.ll").to_string());
    let (spv, metadata) = garnetxlate::translate_text(&source, [1, 1, 1]).unwrap();
    let metadata: Value = serde_json::from_slice(&metadata).unwrap();
    let out = std::env::var_os("ARGUMENT_ARRAYS_EVIDENCE")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            std::env::temp_dir().join(format!("rdna4-arrays-{}", std::process::id()))
        });
    std::fs::create_dir_all(&out).unwrap();
    std::fs::write(out.join("arrays.spv"), &spv).unwrap();
    std::fs::write(
        out.join("arrays.meta.json"),
        serde_json::to_vec_pretty(&metadata).unwrap(),
    )
    .unwrap();
    let coordinates = metadata["argument_buffer_fields"]
        .as_array()
        .unwrap()
        .iter()
        .map(|field| {
            assert_eq!(field["buffer_param_index"], 0);
            assert_eq!(field["buffer_index"], 3);
            (
                field["argument_index"].as_u64().unwrap(),
                field["field_offset"].as_u64().unwrap(),
                field["field_ordinal"].as_u64().unwrap(),
                field["resource_buffer_index"].as_u64(),
            )
        })
        .collect::<Vec<_>>();
    assert_eq!(
        coordinates,
        vec![
            (2, 0, 0, Some(2)),
            (5, 8, 1, Some(5)),
            (6, 16, 1, Some(6)),
            (11, 24, 2, None),
            (16, 28, 3, None),
            (17, 32, 3, None),
            (18, 36, 3, None)
        ]
    );
    let resources = metadata["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|binding| binding["kind"] == "EmbeddedArgBufferBuffer")
        .map(|binding| {
            assert!(binding["descriptor"].is_null());
            binding["embedded_source"]["argument_index"]
                .as_u64()
                .unwrap()
        })
        .collect::<Vec<_>>();
    assert_eq!(resources, vec![2, 5, 6]);
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(out.join("arrays.spv"))
        .output()
        .unwrap();
    std::fs::write(
        out.join("validation.log"),
        format!(
            "status={}\n{}{}",
            validation.status,
            String::from_utf8_lossy(&validation.stdout),
            String::from_utf8_lossy(&validation.stderr)
        ),
    )
    .unwrap();
    assert!(
        validation.status.success(),
        "{}",
        String::from_utf8_lossy(&validation.stderr)
    );
    if std::env::var_os("ARGUMENT_ARRAYS_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}
