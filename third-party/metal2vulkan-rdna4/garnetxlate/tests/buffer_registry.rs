use serde_json::Value;

fn source(stage: &str, helper: bool, constant_write: bool) -> String {
    let child_space = if constant_write { 2 } else { 1 };
    let child_load = format!(
        "  %slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %selected\n  %child = load ptr addrspace({child_space}), ptr addrspace(2) %slot, align 8"
    );
    let access = if constant_write {
        "  store i32 7, ptr addrspace(2) %child, align 4\n  %value = add i32 0, 7".to_string()
    } else if helper {
        "  %value = call i32 @read_child(ptr addrspace(1) %child, i64 %offset)".to_string()
    } else {
        "  %address = getelementptr i32, ptr addrspace(1) %child, i64 %offset\n  %value = load i32, ptr addrspace(1) %address, align 4".to_string()
    };
    let (result, params, output, outputs, position) = if stage == "kernel" {
        ("void", ", ptr addrspace(1) %out, i32 %index", "  store i32 %value, ptr addrspace(1) %out, align 4\n  ret void", "", "!4 = !{i32 1, !\"air.buffer\", !\"air.location_index\", i32 0, i32 1, !\"air.write\", !\"air.address_space\", i32 1, !\"air.arg_type_name\", !\"uint\"}\n!5 = !{i32 2, !\"air.thread_position_in_grid\", !\"air.arg_type_name\", !\"uint\"}")
    } else {
        ("<4 x float>", ", <4 x float> %position", "  %float = uitofp i32 %value to float\n  %rgba = insertelement <4 x float> zeroinitializer, float %float, i32 0\n  ret <4 x float> %rgba", if stage == "fragment" { "!6 = !{!\"air.render_target\", i32 0, i32 0, !\"air.arg_type_name\", !\"float4\"}" } else { "!6 = !{!\"air.position\", !\"air.arg_type_name\", !\"float4\"}" }, if stage == "fragment" { "!4 = !{i32 1, !\"air.position\", !\"air.arg_type_name\", !\"float4\"}" } else { "!4 = !{i32 1, !\"air.vertex_id\", !\"air.arg_type_name\", !\"uint\"}" })
    };
    let params = if stage == "vertex" {
        ", i32 %index"
    } else {
        params
    };
    let index = if stage == "fragment" {
        "  %x = extractelement <4 x float> %position, i32 0\n  %index = fptoui float %x to i32"
    } else {
        ""
    };
    let inputs = if stage == "kernel" {
        "!3, !4, !5"
    } else {
        "!3, !4"
    };
    let outputs_list = if stage == "kernel" { "" } else { "!6" };
    let helper_body = if helper {
        "define internal i32 @read_child(ptr addrspace(1) %data, i64 %i) {\nentry:\n  %p = getelementptr i32, ptr addrspace(1) %data, i64 %i\n  %v = load i32, ptr addrspace(1) %p, align 4\n  ret i32 %v\n}"
    } else {
        ""
    };
    format!(
        r#"target triple = "air64-apple-macosx"
%Root = type {{ [2 x ptr addrspace({child_space})] }}
define {result} @consume(ptr addrspace(2) %args{params}) {{
entry:
{index}
  %choice = and i32 %index, 1
  %selected = zext i32 %choice to i64
  %wide = zext i32 %index to i64
  %offset = add i64 %wide, 1073741824
{child_load}
{access}
{output}
}}
{helper_body}
!air.{stage} = !{{!0}}
!0 = !{{ptr @consume, !1, !2}}
!1 = !{{{outputs_list}}}
!2 = !{{{inputs}}}
!3 = !{{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}}
{position}
{outputs}
"#
    )
}

#[test]
fn nested_addresses_use_a_bounded_registry_in_each_stage_and_helper() {
    for stage in ["kernel", "vertex", "fragment"] {
        for helper in [false, true] {
            let source = source(stage, helper, false);
            let (bytes, metadata) = garnetxlate::translate_text(&source, [1, 1, 1])
                .unwrap_or_else(|error| panic!("{stage} helper={helper}: {error}"));
            let metadata: Value = serde_json::from_slice(&metadata).unwrap();
            assert_eq!(metadata["buffer_address_table_version"], 3, "{metadata}");
            assert_eq!(metadata["buffer_address_table_bytes"], 800);
            assert_eq!(metadata["buffer_address_table_row_stride_bytes"], 24);
            assert_eq!(metadata["buffer_address_table_dynamic_extent"], true);
            metal2vulkan::tools::spirv_val_bytes(&bytes, &std::env::temp_dir()).unwrap();
            let assembly = metal2vulkan::disassemble(&bytes).unwrap();
            for required in [
                "OpArrayLength",
                "OpLoopMerge",
                "OpFunctionCall",
                "OpUGreaterThanEqual",
                "OpULessThanEqual",
                "OpBitwiseAnd",
                "OpConvertUToPtr",
                "OpIAdd",
                "PhysicalStorageBuffer64",
            ] {
                assert!(
                    assembly.contains(required),
                    "{stage} helper={helper} missing {required}: {assembly}"
                );
            }
            assert!(
                assembly
                    .lines()
                    .any(|line| line.contains("OpLoad") && line.contains("Aligned 4")),
                "{assembly}"
            );
        }
    }
}

#[test]
fn writing_through_a_loaded_constant_pointer_is_refused() {
    let error = garnetxlate::translate_text(&source("kernel", false, true), [1, 1, 1]).unwrap_err();
    assert!(
        error.contains("cannot write through a constant-space buffer pointer"),
        "{error}"
    );
}
