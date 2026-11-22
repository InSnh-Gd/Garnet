use metal2vulkan::passes::Stage;

fn source(shape: &str, access: &str, body: &str, declarations: &str) -> String {
    format!(
        r#"target triple = "air64-apple-macosx"
define void @probe(ptr addrspace(1) %tex, ptr addrspace(2) %sampler, ptr addrspace(1) %out) {{
entry:
{body}
  ret void
}}
{declarations}
!air.kernel = !{{!0}}
!0 = !{{ptr @probe, !1, !2}}
!1 = !{{}}
!2 = !{{!3, !4, !5}}
!3 = !{{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.{access}", !"air.arg_type_name", !"{shape}"}}
!4 = !{{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1}}
!5 = !{{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4*"}}
"#
    )
}

fn translate(source: &str, label: &str) -> Result<Vec<u8>, String> {
    let tmp = std::env::temp_dir().join(format!("image-legality-{label}-{}", std::process::id()));
    std::fs::create_dir_all(&tmp).unwrap();
    let result = metal2vulkan::translate_sanitized_native(source, Stage::Kernel, &tmp)
        .map_err(|e| e.to_string());
    std::fs::remove_dir_all(&tmp).unwrap();
    result
}

fn assembly(bytes: &[u8], label: &str) -> String {
    let tmp =
        std::env::temp_dir().join(format!("image-legality-val-{label}-{}", std::process::id()));
    std::fs::create_dir_all(&tmp).unwrap();
    let path = tmp.join("shader.spv");
    std::fs::write(&path, bytes).unwrap();
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
    std::fs::remove_dir_all(&tmp).unwrap();
    assert!(disassembly.status.success());
    String::from_utf8(disassembly.stdout).unwrap()
}

#[test]
fn direct_depth_array_size_and_depth_ms_sample_count_are_dispatched() {
    for (shape, intrinsic, opcode) in [
        (
            "depth2d_array<float, sample>",
            "air.get_array_size_depth_2d_array",
            "OpImageQuerySizeLod",
        ),
        (
            "depth2d_ms<float, read>",
            "air.get_num_samples_depth_2d_ms",
            "OpImageQuerySamples",
        ),
    ] {
        let ll = source(shape, if shape.contains("_ms") { "read" } else { "sample" },
            &format!("  %n = call i32 @{intrinsic}(ptr addrspace(1) %tex)\n  store i32 %n, ptr addrspace(1) %out"),
            &format!("declare i32 @{intrinsic}(ptr addrspace(1))"));
        let bytes = translate(&ll, intrinsic).unwrap();
        assert!(assembly(&bytes, intrinsic).contains(opcode));
    }
}

#[test]
fn one_dimensional_read_and_width_require_zero_lod() {
    for suffix in ["1d", "1d_array"] {
        let layer = if suffix.ends_with("array") {
            "i32 1, "
        } else {
            ""
        };
        let layer_ty = if layer.is_empty() { "" } else { "i32, " };
        let shape = format!("texture{suffix}<float, read>");
        for lod in [0, 1] {
            let read = source(&shape, "read", &format!("  %r = call {{ <4 x float>, i8 }} @air.read_texture_{suffix}.v4f32(ptr addrspace(1) %tex, i32 3, {layer}i32 {lod}, i32 1)\n  %c = extractvalue {{ <4 x float>, i8 }} %r, 0\n  store <4 x float> %c, ptr addrspace(1) %out"),
                &format!("declare {{ <4 x float>, i8 }} @air.read_texture_{suffix}.v4f32(ptr addrspace(1), i32, {layer_ty}i32, i32)"));
            let width = source(&shape, "read", &format!("  %n = call i32 @air.get_width_texture_{suffix}(ptr addrspace(1) %tex, i32 {lod})\n  store i32 %n, ptr addrspace(1) %out"),
                &format!("declare i32 @air.get_width_texture_{suffix}(ptr addrspace(1), i32)"));
            for (label, ll) in [
                (format!("read-{suffix}-{lod}"), read),
                (format!("width-{suffix}-{lod}"), width),
            ] {
                let result = translate(&ll, &label);
                if lod == 0 {
                    assembly(&result.unwrap(), &label);
                } else {
                    assert!(result
                        .expect_err("operation must be rejected")
                        .contains("1D texture requires zero LOD"));
                }
            }
        }
    }
}

#[test]
fn one_dimensional_mip_count_is_zero() {
    for suffix in ["1d", "1d_array"] {
        let ll = source(&format!("texture{suffix}<float, sample>"), "sample",
            &format!("  %n = call i32 @air.get_num_mip_levels_texture_{suffix}(ptr addrspace(1) %tex)\n  store i32 %n, ptr addrspace(1) %out"),
            &format!("declare i32 @air.get_num_mip_levels_texture_{suffix}(ptr addrspace(1))"));
        let asm = assembly(&translate(&ll, suffix).unwrap(), suffix);
        assert!(!asm.contains("OpImageQueryLevels"), "{asm}");
        assert!(
            asm.lines()
                .any(|line| line.contains("OpCopyObject %uint %uint_0")),
            "{asm}"
        );
    }
}

fn cube_gradient(arrayed: bool, lanes: u32) -> String {
    let suffix = if arrayed { "cube_array" } else { "cube" };
    let layer = if arrayed { "i32 1, " } else { "" };
    let layer_ty = if arrayed { "i32, " } else { "" };
    let derivative = if lanes == 3 {
        "<float 0.125, float 0.25, float 0.5>"
    } else {
        "<float 0.125, float 0.25>"
    };
    source(&format!("texture{suffix}<float, sample>"), "sample",
        &format!("  %r = call {{ <4 x float>, i8 }} @air.sample_texture_{suffix}_grad.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %sampler, <3 x float> <float 0.5, float 1.0, float 0.25>, {layer}<{lanes} x float> {derivative}, <{lanes} x float> {derivative}, float 0.0, i32 0)\n  %c = extractvalue {{ <4 x float>, i8 }} %r, 0\n  store <4 x float> %c, ptr addrspace(1) %out"),
        &format!("declare {{ <4 x float>, i8 }} @air.sample_texture_{suffix}_grad.v4f32(ptr addrspace(1), ptr addrspace(2), <3 x float>, {layer_ty}<{lanes} x float>, <{lanes} x float>, float, i32)"))
}

#[test]
fn cube_and_cube_array_gradients_have_three_spatial_derivatives() {
    for arrayed in [false, true] {
        let label = if arrayed { "cube-array" } else { "cube" };
        let asm = assembly(
            &translate(&cube_gradient(arrayed, 3), label).unwrap(),
            label,
        );
        assert!(
            asm.lines()
                .any(|line| line.contains("OpImageSampleExplicitLod") && line.contains("Grad")),
            "{asm}"
        );
        assert!(!asm.contains("ConstOffset"), "{asm}");
        let grad = asm
            .lines()
            .find(|line| line.contains("OpImageSampleExplicitLod") && line.contains("Grad"))
            .unwrap();
        for derivative in grad.split_whitespace().rev().take(2) {
            assert!(
                asm.lines().any(|line| line
                    .trim_start()
                    .starts_with(&format!("{derivative} = OpConstantComposite %v3float"))),
                "{asm}"
            );
        }
        assert!(
            translate(&cube_gradient(arrayed, 2), &format!("{label}-bad"))
                .expect_err("operation must be rejected")
                .contains("3-component float derivatives")
        );
    }
}

#[test]
fn macos_depth_comparison_rejects_nonzero_level_bias_and_gradients() {
    let original = include_str!("fixtures/argument_image_sampling_queries.ll");
    let anchor = "%coord1, float 0.5, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.0, float 0.0, i32 0)";
    assert!(original.contains(anchor));
    for (tail, expected) in [
        ("%coord1, float 0.5, i1 true, <2 x i32> zeroinitializer, i1 true, float 1.0, float 0.0, i32 0)", "level(0)"),
        ("%coord1, float 0.5, i1 true, <2 x i32> zeroinitializer, i1 false, float 1.0, float 0.0, i32 0)", "bias"),
    ] {
        let ll = original.replace(anchor, tail);
        assert!(indirect(&ll).expect_err("operation must be rejected").contains(expected));
    }
    let ll = original.replace(
        "air.sample_compare_depth_2d.f32",
        "air.sample_compare_depth_2d_grad.f32",
    );
    assert!(indirect(&ll)
        .expect_err("operation must be rejected")
        .contains("gradient"));
}

#[test]
fn cube_sample_has_no_offset_overload() {
    let ll = cube_gradient(false, 3)
        .replace("cube_grad.v4f32", "cube.v4f32")
        .replace(
            "<3 x float> <float 0.125, float 0.25, float 0.5>, <3 x float> <float 0.125, float 0.25, float 0.5>, float 0.0, i32 0)",
            "i1 true, <2 x i32> <i32 1, i32 -1>, i1 true, float 0.0, float 0.0, i32 0)",
        )
        .replace(
            "<3 x float>, <3 x float>, float, i32)",
            "i1, <2 x i32>, i1, float, float, i32)",
        );
    assert!(translate(&ll, "cube-offset")
        .expect_err("operation must be rejected")
        .contains("cube sample has no offset"));
}

#[test]
fn indirect_one_dimensional_query_lod_is_checked() {
    let ll = include_str!("fixtures/argument_image_sampling_queries.ll");
    let ll = ll.replace("%image_1d, i32 0)", "%image_1d, i32 %mip)");
    assert!(indirect(&ll)
        .expect_err("nonzero/dynamic 1D LOD must fail")
        .contains("1D texture requires zero LOD"));
}

#[test]
fn depth_cube_gradient_keeps_float3_derivatives() {
    let ll = cube_gradient(true, 3)
        .replace(
            "texturecube_array<float, sample>",
            "depthcube_array<float, sample>",
        )
        .replace(
            "air.sample_texture_cube_array_grad.v4f32",
            "air.sample_depth_cube_array_grad.f32",
        )
        .replace("{ <4 x float>, i8 }", "{ float, i8 }")
        .replace(
            "ptr addrspace(2) %sampler, <3 x float>",
            "ptr addrspace(2) %sampler, i32 0, <3 x float>",
        )
        .replace(
            "ptr addrspace(2), <3 x float>",
            "ptr addrspace(2), i32, <3 x float>",
        )
        .replace("store <4 x float> %c", "store float %c");
    let asm = assembly(
        &translate(&ll, "depth-cube-grad").unwrap(),
        "depth-cube-grad",
    );
    assert!(
        asm.lines()
            .any(|line| line.contains("OpImageSampleExplicitLod") && line.contains("Grad")),
        "{asm}"
    );
}

#[test]
fn multisample_reads_preserve_component_type_and_sample_operand() {
    for (suffix, element) in [("v4f32", "float"), ("u.v4i32", "i32"), ("s.v4i32", "i32")] {
        let scalar = if suffix == "u.v4i32" {
            "uint"
        } else if element == "i32" {
            "int"
        } else {
            "float"
        };
        let ll = source(&format!("texture2d_ms<{scalar}, read>"), "read",
            &format!("  %r = call {{ <4 x {element}>, i8 }} @air.read_texture_2d_ms.{suffix}(ptr addrspace(1) %tex, <2 x i32> <i32 2, i32 1>, i32 2, i32 1)\n  %c = extractvalue {{ <4 x {element}>, i8 }} %r, 0\n  store <4 x {element}> %c, ptr addrspace(1) %out"),
            &format!("declare {{ <4 x {element}>, i8 }} @air.read_texture_2d_ms.{suffix}(ptr addrspace(1), <2 x i32>, i32, i32)"));
        let asm = assembly(
            &translate(&ll, suffix).unwrap_or_else(|error| panic!("{suffix}: {error}")),
            suffix,
        );
        assert!(
            asm.contains(&format!("OpTypeImage %{scalar} 2D 0 0 1 1")),
            "{asm}"
        );
        assert!(
            asm.lines()
                .any(|line| line.contains("OpImageFetch") && line.contains("Sample %uint_2")),
            "{asm}"
        );
        assert!(
            !asm.lines()
                .any(|line| line.contains("OpImageFetch") && line.contains("Lod")),
            "{asm}"
        );
    }
}

#[test]
fn multisample_storage_write_is_refused() {
    let original = include_str!("fixtures/argument_image_sampling_queries.ll");
    let ll = original.replace(
        "air.write_texture_2d.v4f32",
        "air.write_texture_2d_ms.v4f32",
    );
    let error = indirect(&ll).expect_err("multisample write must be refused");
    assert!(
        error.contains("multisampled storage") || error.contains("conflicting image types"),
        "{error}"
    );
}

fn indirect(source: &str) -> Result<(Vec<u8>, Vec<u8>), String> {
    let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [8, 1, 1])?;
    garnetxlate::lower_gpu_arguments(&spv, &metadata)
}

#[test]
fn indirect_multisample_reads_keep_ms_type_and_selected_sample() {
    for (suffix, element, component) in [
        ("v4f32", "float", "float"),
        ("u.v4i32", "i32", "uint"),
        ("s.v4i32", "i32", "int"),
    ] {
        let ll = format!(
            r#"target triple = "air64-apple-macosx"
%Root = type {{ [2 x %Record] }}
%Record = type {{ [2 x ptr addrspace(1)] }}
define void @probe(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {{
entry:
  %ix = and i32 %i, 1
  %index = zext i32 %ix to i64
  %slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 1, i32 0, i64 %index
  %token = load i64, ptr addrspace(2) %slot, align 8
  %image = inttoptr i64 %token to ptr addrspace(1)
  %read = call {{ <4 x {element}>, i8 }} @air.read_texture_2d_ms.{suffix}(ptr addrspace(1) %image, <2 x i32> <i32 2, i32 1>, i32 2, i32 1)
  %rgba = extractvalue {{ <4 x {element}>, i8 }} %read, 0
  store <4 x {element}> %rgba, ptr addrspace(1) %out, align 16
  ret void
}}
declare {{ <4 x {element}>, i8 }} @air.read_texture_2d_ms.{suffix}(ptr addrspace(1), <2 x i32>, i32, i32)
!air.kernel = !{{!0}}
!0 = !{{ptr @probe, !1, !2}}
!1 = !{{}}
!2 = !{{!3, !4, !5}}
!3 = !{{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}}
!4 = !{{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"{element}4"}}
!5 = !{{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}}
"#
        );
        let (spv, metadata) = indirect(&ll).unwrap();
        let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
        assert_eq!(metadata["argument_resource_tables"]["version"], 9);
        let asm = assembly(&spv, &format!("indirect-ms-{suffix}"));
        assert!(
            asm.contains(&format!("OpTypeImage %{component} 2D 0 0 1 1")),
            "{asm}"
        );
        assert!(
            asm.lines()
                .any(|line| line.contains("OpImageFetch") && line.contains("Sample %uint_2")),
            "{asm}"
        );
    }
}

#[test]
fn abi9_image_matrix_preserves_the_query_tail_contract() {
    let (spv, metadata) =
        indirect(include_str!("fixtures/argument_image_sampling_queries.ll")).unwrap();
    let metadata: serde_json::Value = serde_json::from_slice(&metadata).unwrap();
    assert_eq!(metadata["argument_resource_tables"]["version"], 9);
    assembly(&spv, "abi8-image-matrix");
}
