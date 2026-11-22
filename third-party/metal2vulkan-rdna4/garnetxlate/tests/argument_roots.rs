use serde_json::{json, Value};

#[test]
fn opaque_graph_retains_declared_argument_roots_across_all_paths() {
    let source = include_str!("fixtures/argument_resource_graph.ll");
    let linked = source.split("\n!air.kernel =").next().unwrap().replace(
        "define void @resource_graph(",
        "define void @resource_helper(",
    );
    let entry = source
        .replace(
            source.split("entry:").nth(1).unwrap().split("\n}").next().unwrap(),
            "\n  call void @resource_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i)\n  ret void",
        )
        + "\ndeclare void @resource_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2), ptr addrspace(1), i32) section \"air.externally_defined\"\n!air.visible_function_references = !{!6}\n!6 = !{!\"air.visible_function_reference\", ptr @resource_helper.MTL_VISIBLE_FN_REF, !\"resource_helper\"}\n";
    for (spv, metadata) in [
        garnetxlate::translate_text(source, [1, 1, 1]).unwrap(),
        garnetxlate::translate_text_linked(&entry, [1, 1, 1], &[("resource_helper", &linked)])
            .unwrap(),
    ] {
        let metadata: Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(metadata["argument_buffer_roots"], json!([3]));
        assert_eq!(metadata["trimmed_from_reflection_version"], 66);
        assert!(metadata["argument_buffer_fields"].is_null());
        assert!(metadata["argument_resource_tables"]["roots"].is_null());
        let bytes = serde_json::to_vec(&metadata).unwrap();
        let (_, cached) = garnetxlate::lower_gpu_arguments(&spv, &bytes).unwrap();
        assert_eq!(serde_json::from_slice::<Value>(&cached).unwrap(), metadata);
        for invalid in [
            json!([3, 3]),
            json!([32]),
            json!([7]),
            json!([-1]),
            json!([3.0]),
            json!("3"),
        ] {
            let mut bad = metadata.clone();
            bad["argument_buffer_roots"] = invalid;
            assert!(
                garnetxlate::lower_gpu_arguments(&spv, &serde_json::to_vec(&bad).unwrap()).is_err(),
                "{bad}"
            );
        }
        let mut legacy = metadata;
        legacy["trimmed_from_reflection_version"] = json!(65);
        assert!(
            garnetxlate::lower_gpu_arguments(&spv, &serde_json::to_vec(&legacy).unwrap()).is_err()
        );
    }
}
