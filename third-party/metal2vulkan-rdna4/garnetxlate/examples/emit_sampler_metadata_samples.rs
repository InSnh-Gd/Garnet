use std::path::Path;

const STATIC_NEVER: u64 = 34_901_797_601_017_929;

const ORDINARY_SAMPLE: &str = r#"  %ordinary = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %color, ptr addrspace(2) %sampler, <2 x float> <float 0.25, float 0.75>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %ordinary4 = extractvalue { <4 x float>, i8 } %ordinary, 0
  %ordinary_value = extractelement <4 x float> %ordinary4, i32 0
  store float %ordinary_value, ptr addrspace(1) %out, align 4"#;

const COMPARISON_SAMPLE: &str = r#"  %comparison = call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) %depth, ptr addrspace(2) %sampler, i32 1, <2 x float> <float 0.25, float 0.75>, float 0.5, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.0, float 0.0, i32 0)
  %comparison_value = extractvalue { float, i8 } %comparison, 0
  %out_one = getelementptr float, ptr addrspace(1) %out, i64 1
  store float %comparison_value, ptr addrspace(1) %out_one, align 4"#;

fn standalone_source(ordinary: bool, comparison: bool, static_words: Option<u64>) -> String {
    let (static_state, sampler_argument, sampler_metadata) = match static_words {
        Some(words) => (
            format!(
                "@__air_sampler_state = internal addrspace(2) constant [2 x i64] [i64 {words}, i64 0], align 8\n"
            ),
            ", ",
            "",
        ),
        None => (
            String::new(),
            ", ptr addrspace(2) %sampler, ",
            r#"!5 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1}"#,
        ),
    };
    let (parameter_metadata, out_index, position_index, static_metadata) = if static_words.is_some()
    {
        (
            "!3, !4, !6, !7",
            2,
            3,
            "\n!air.sampler_states = !{!8}\n!8 = !{!\"air.sampler_state\", ptr addrspace(2) @__air_sampler_state}",
        )
    } else {
        ("!3, !4, !5, !6, !7", 3, 4, "")
    };
    let ordinary_ir = if ordinary {
        format!(
            r#"  %ordinary = call {{ <4 x float>, i8 }} @air.sample_texture_2d.v4f32(ptr addrspace(1) %color, ptr addrspace(2) {sampler}, <2 x float> <float 0.25, float 0.75>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %ordinary4 = extractvalue {{ <4 x float>, i8 }} %ordinary, 0
  %ordinary_value = extractelement <4 x float> %ordinary4, i32 0
"#,
            sampler = if static_words.is_some() {
                "@__air_sampler_state"
            } else {
                "%sampler"
            }
        )
    } else {
        String::new()
    };
    let comparison_ir = if comparison {
        format!(
            r#"  %comparison = call {{ float, i8 }} @air.sample_compare_depth_2d.f32(ptr addrspace(1) %depth, ptr addrspace(2) {sampler}, i32 1, <2 x float> <float 0.25, float 0.75>, float 0.5, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.0, float 0.0, i32 0)
  %comparison_value = extractvalue {{ float, i8 }} %comparison, 0
"#,
            sampler = if static_words.is_some() {
                "@__air_sampler_state"
            } else {
                "%sampler"
            }
        )
    } else {
        String::new()
    };
    format!(
        r#"target triple = "air64-apple-macosx"
{static_state}define void @sampler_usage(ptr addrspace(1) %color, ptr addrspace(1) %depth{sampler_argument}ptr addrspace(1) %out, i32 %i) {{
entry:
{ordinary_ir}{comparison_ir}  store float 0.0, ptr addrspace(1) %out, align 4
  ret void
}}
declare {{ <4 x float>, i8 }} @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare {{ float, i8 }} @air.sample_compare_depth_2d.f32(ptr addrspace(1), ptr addrspace(2), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32)
!air.kernel = !{{!0}}
!0 = !{{ptr @sampler_usage, !1, !2}}
!1 = !{{}}
!2 = !{{{parameter_metadata}}}
!3 = !{{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>"}}
!4 = !{{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float, sample>"}}
{sampler_metadata}
!6 = !{{i32 {out_index}, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float*"}}
!7 = !{{i32 {position_index}, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}}{static_metadata}
"#
    )
}

fn write(path: &Path, spv: &[u8], metadata: &[u8]) {
    std::fs::write(path.with_extension("spv"), spv).unwrap();
    std::fs::write(path.with_extension("json"), metadata).unwrap();
}

fn main() {
    let out = std::env::args().nth(1).expect("output directory");
    let abi8 = std::env::args().nth(2).as_deref() == Some("abi8");
    let out = Path::new(&out);
    std::fs::create_dir_all(out).unwrap();

    let translate_stage = |source: &str| {
        if abi8 {
            garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1])
        } else {
            garnetxlate::translate_text(source, [1, 1, 1])
        }
    };
    let translate_gpu = |source: &str| {
        if abi8 {
            let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [8, 1, 1])?;
            garnetxlate::lower_gpu_arguments(&spv, &metadata)
        } else {
            garnetxlate::translate_text_gpu_arguments(source, [8, 1, 1])
        }
    };

    let (spv, metadata) = translate_stage(&standalone_source(true, true, None)).unwrap();
    write(&out.join("direct-pair"), &spv, &metadata);

    let (spv, metadata) =
        translate_stage(&standalone_source(false, true, Some(STATIC_NEVER))).unwrap();
    write(&out.join("static-compare-only"), &spv, &metadata);

    let fixture = include_str!("../tests/fixtures/argument_sampler_usage.ll");
    for (name, operations) in [
        ("gpu-normal-only", ORDINARY_SAMPLE.to_string()),
        (
            "gpu-mixed",
            format!("{ORDINARY_SAMPLE}\n{COMPARISON_SAMPLE}"),
        ),
        ("gpu-compare-only", COMPARISON_SAMPLE.to_string()),
    ] {
        let source = fixture.replace("  ; SAMPLE_OPERATIONS", &operations);
        let (spv, metadata) = translate_gpu(&source).unwrap();
        write(&out.join(name), &spv, &metadata);
    }
}
