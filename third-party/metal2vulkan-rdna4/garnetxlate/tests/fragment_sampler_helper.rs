use serde_json::Value;

const SOURCE: &[u8] = include_bytes!("../../tests/fixtures/fragment_sampler_helper.ll");

#[test]
fn token_only_sampling_helpers_do_not_require_a_buffer_address_table() {
    let (spv, metadata) = garnetxlate::translate_bytes(SOURCE, [1, 1, 1]).unwrap();
    metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
    let meta: Value = serde_json::from_slice(&metadata).unwrap();
    assert!(
        meta["bindings"]
            .as_array()
            .unwrap()
            .iter()
            .all(|binding| { binding["kind"] != "BufferAddressTable" }),
        "opaque image/sampler helper arguments are tokens, not buffer addresses: {meta}"
    );
    let assembly = metal2vulkan::disassemble(&spv).unwrap();
    assert!(assembly.contains("OpImageSampleImplicitLod"));
    assert!(!assembly.contains("PhysicalStorageBuffer"));
    assert!(meta["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .any(|binding| { binding["kind"] == "EmbeddedArgBufferSampler" }));
}

#[test]
fn cached_sampler_flags_dominate_both_uniform_helper_branches() {
    for linked in [false, true] {
        let (spv, metadata) = if linked {
            garnetxlate::translate_bytes_linked(SOURCE, [1, 1, 1], &[]).unwrap()
        } else {
            garnetxlate::translate_bytes(SOURCE, [1, 1, 1]).unwrap()
        };
        let (spv, _) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
        metal2vulkan::tools::spirv_val_bytes(&spv, &std::env::temp_dir()).unwrap();
        let assembly = metal2vulkan::disassemble(&spv).unwrap();
        assert_eq!(assembly.matches("OpImageSampleExplicitLod").count(), 2);
        assert_eq!(assembly.matches("OpDPdx ").count(), 2);
        assert_eq!(assembly.matches("OpDPdy ").count(), 2);
    }
}
