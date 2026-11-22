#[test]
fn nonuniform_image_sampler_and_pointer_arrays_keep_their_coordinates() {
    let (spv, metadata) = garnetxlate::translate_text(
        include_str!("fixtures/argument_resource_arrays.ll"),
        [32, 1, 1],
    )
    .unwrap();
    let reflection: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    let bindings = reflection["bindings"].as_array().unwrap();
    for (kind, offset, argument, binding) in [
        ("EmbeddedArgBufferTexture", 0, 2, 32),
        ("EmbeddedArgBufferSampler", 16, 7, 160),
    ] {
        let resource = bindings.iter().find(|r| r["kind"] == kind).unwrap();
        assert_eq!(resource["descriptor"]["count"], 2);
        assert_eq!(resource["descriptor"]["binding"], binding);
        assert_eq!(resource["embedded_source"]["buffer_index"], 3);
        assert_eq!(resource["embedded_source"]["field_offset"], offset);
        assert_eq!(resource["embedded_source"]["argument_index"], argument);
    }
    let out = std::env::var_os("ARGUMENT_RESOURCE_ARRAYS_EVIDENCE")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            std::env::temp_dir().join(format!("rdna4-resource-arrays-{}", std::process::id()))
        });
    std::fs::create_dir_all(&out).unwrap();
    std::fs::write(out.join("arrays.spv"), &spv).unwrap();
    std::fs::write(out.join("arrays.meta.json"), &metadata).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(out.join("arrays.spv"))
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "{}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(out.join("arrays.spv"))
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let assembly = String::from_utf8(disassembly.stdout).unwrap();
    assert!(
        assembly.contains("OpCapability ShaderNonUniform"),
        "{assembly}"
    );
    assert!(
        assembly.contains("OpCapability SampledImageArrayNonUniformIndexing"),
        "{assembly}"
    );
    let mut samples = 0;
    for line in assembly
        .lines()
        .filter(|line| line.contains("= OpSampledImage "))
    {
        let id = line.split_whitespace().next().unwrap();
        assert!(
            assembly.contains(&format!("OpDecorate {id} NonUniform")),
            "{assembly}"
        );
        samples += 1;
    }
    assert!(samples > 0);
    std::fs::write(out.join("arrays.spvasm"), assembly).unwrap();
    if std::env::var_os("ARGUMENT_RESOURCE_ARRAYS_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn fixed_array_indices_do_not_require_nonuniform_features() {
    let source = include_str!("fixtures/argument_resource_arrays.ll")
        .replace("i64 %conv2", "i64 1")
        .replace("i64 %conv", "i64 0");
    let (spv, _) = garnetxlate::translate_text(&source, [32, 1, 1]).unwrap();
    let words = spv
        .chunks_exact(4)
        .map(|word| u32::from_le_bytes(word.try_into().unwrap()))
        .collect::<Vec<_>>();
    let mut at = 5;
    while at < words.len() {
        let count = (words[at] >> 16) as usize;
        assert!(count > 0);
        if words[at] & 0xffff == 17 {
            assert_ne!(
                words[at + 1],
                5301,
                "fixed indices acquired ShaderNonUniform"
            );
        }
        at += count;
    }
}
