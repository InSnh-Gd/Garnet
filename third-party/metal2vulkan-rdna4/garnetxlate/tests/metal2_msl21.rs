// SPDX-License-Identifier: LGPL-3.0-or-later
//! Offline ABI tests for Metal 2 / MSL 2.1 translator additions (`0069`):
//! dual-source blending `[[index(1)]]`, `air.simd_active_threads_mask.i64`,
//! `simd_shuffle_rotate_up` / `quad_shuffle_rotate_up`, and `[[raster_order_group(n)]]`.
use garnetxlate::{garnetx_free, garnetx_translate};
use std::os::raw::c_char;

struct Out {
    rc: i32,
    msg: String,
    spv: Vec<u8>,
}

fn translate_ffi(ll: &str) -> Out {
    let mut spv: *mut u8 = std::ptr::null_mut();
    let mut spv_len = 0usize;
    let mut meta: *mut u8 = std::ptr::null_mut();
    let mut meta_len = 0usize;
    let mut err = [0 as c_char; 512];
    let rc = unsafe {
        garnetx_translate(
            ll.as_ptr(),
            ll.len(),
            std::ptr::null(),
            &mut spv,
            &mut spv_len,
            &mut meta,
            &mut meta_len,
            err.as_mut_ptr(),
            err.len(),
        )
    };
    let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }
        .to_string_lossy()
        .into_owned();
    let mut spv_bytes = Vec::new();
    if rc == 0 {
        unsafe {
            spv_bytes = std::slice::from_raw_parts(spv, spv_len).to_vec();
            garnetx_free(spv, spv_len);
            garnetx_free(meta, meta_len);
        }
    } else {
        assert!(
            spv.is_null() && meta.is_null() && spv_len == 0 && meta_len == 0,
            "outputs must be zeroed on failure"
        );
    }
    Out {
        rc,
        msg,
        spv: spv_bytes,
    }
}

#[test]
fn dual_source_fragment_output_emits_location_and_index_decorations() {
    let dual_ll = r#"
target triple = "spirv-unknown-vulkan1.2"
%DualOut = type { <4 x float>, <4 x float> }

define %DualOut @f() {
entry:
  %a = insertvalue %DualOut poison, <4 x float> <float 0.5, float 0.5, float 0.5, float 1.0>, 0
  %b = insertvalue %DualOut %a, <4 x float> <float 0.4, float 0.2, float 0.0, float 1.0>, 1
  ret %DualOut %b
}

!air.fragment = !{!0}
!0 = !{ptr @f, !1, !4}
!1 = !{!2, !3}
!2 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"c0"}
!3 = !{!"air.render_target", i32 0, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"c1"}
!4 = !{}
"#;
    let out = translate_ffi(dual_ll);
    assert_eq!(out.rc, 0, "dual-source translation failed: {}", out.msg);
    let asm = metal2vulkan::disassemble(&out.spv).expect("disassemble");
    assert_eq!(
        asm.matches("Location 0").count(),
        2,
        "expected both dual-source outputs at Location 0:\n{asm}"
    );
    assert!(
        asm.contains("Index 1"),
        "expected second output decorated Index 1:\n{asm}"
    );
}

#[test]
fn simd_active_threads_mask_and_rotate_up_intrinsics_lower_via_ffi() {
    let ll = r#"
target triple = "spirv-unknown-vulkan1.2"
define void @k(i32 %v, ptr addrspace(1) %out) {
entry:
  %mask = tail call i64 @air.simd_active_threads_mask.i64()
  %srot = tail call i32 @air.simd_shuffle_rotate_up.u.i32(i32 %v, i16 1)
  %qrot = tail call i32 @air.quad_shuffle_rotate_up.u.i32(i32 %srot, i16 1)
  %lo = trunc i64 %mask to i32
  %sum = add i32 %lo, %qrot
  store i32 %sum, ptr addrspace(1) %out, align 4
  ret void
}

declare i64 @air.simd_active_threads_mask.i64()
declare i32 @air.simd_shuffle_rotate_up.u.i32(i32, i16)
declare i32 @air.quad_shuffle_rotate_up.u.i32(i32, i16)

!air.kernel = !{!0}
!0 = !{ptr @k, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
"#;
    let out = translate_ffi(ll);
    assert_eq!(out.rc, 0, "simd translation failed: {}", out.msg);
    let asm = metal2vulkan::disassemble(&out.spv).expect("disassemble");
    assert!(asm.contains("OpCapability GroupNonUniformBallot"), "{asm}");
    assert!(asm.contains("OpCapability GroupNonUniformShuffle"), "{asm}");
    assert!(asm.contains("OpGroupNonUniformBallot"), "{asm}");
    assert!(asm.contains("OpGroupNonUniformShuffle"), "{asm}");
    assert!(asm.contains("OpISub"), "{asm}");
    assert!(!asm.contains("OpFunctionCall"), "{asm}");
}

#[test]
fn raster_order_group_color_and_void_fragments_emit_interlock_and_reject_oob() {
    let make_rog_ll = |ret_color: bool, group: u32| {
        if ret_color {
            format!(
                r#"
target triple = "spirv-unknown-vulkan1.2"
define <4 x float> @f(ptr addrspace(1) %buf) {{
entry:
  %v = load i32, ptr addrspace(1) %buf, align 4
  %n = add i32 %v, 1
  store i32 %n, ptr addrspace(1) %buf, align 4
  ret <4 x float> <float 0.0, float 1.0, float 0.0, float 1.0>
}}

!air.fragment = !{{!0}}
!0 = !{{ptr @f, !1, !2}}
!1 = !{{!3}}
!2 = !{{!4}}
!3 = !{{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}}
!4 = !{{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.raster_order_group", i32 {group}, !"air.address_space", i32 1, !"air.arg_type_name", !"uint"}}
"#
            )
        } else {
            format!(
                r#"
target triple = "spirv-unknown-vulkan1.2"
define void @f(ptr addrspace(1) %buf) {{
entry:
  %v = load i32, ptr addrspace(1) %buf, align 4
  %n = add i32 %v, 1
  store i32 %n, ptr addrspace(1) %buf, align 4
  ret void
}}

!air.fragment = !{{!0}}
!0 = !{{ptr @f, !1, !2}}
!1 = !{{}}
!2 = !{{!3}}
!3 = !{{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.raster_order_group", i32 {group}, !"air.address_space", i32 1, !"air.arg_type_name", !"uint"}}
"#
            )
        }
    };

    for (label, ret_color) in [("color", true), ("void", false)] {
        let out = translate_ffi(&make_rog_ll(ret_color, 0));
        assert_eq!(out.rc, 0, "rog {label} failed: {}", out.msg);
        let asm = metal2vulkan::disassemble(&out.spv).expect("disassemble");
        assert!(asm.contains("FragmentShaderPixelInterlockEXT"), "{label}:\n{asm}");
        assert!(asm.contains("SPV_EXT_fragment_shader_interlock"), "{label}:\n{asm}");
        assert!(asm.contains("PixelInterlockOrderedEXT"), "{label}:\n{asm}");
        assert!(asm.contains("OpBeginInvocationInterlockEXT"), "{label}:\n{asm}");
        assert!(asm.contains("OpEndInvocationInterlockEXT"), "{label}:\n{asm}");
    }

    let oob = translate_ffi(&make_rog_ll(true, 8));
    assert_ne!(oob.rc, 0, "raster_order_group(8) must be rejected");
    assert!(
        oob.msg.contains("raster_order_group(8)"),
        "expected error naming raster_order_group(8), got: {}",
        oob.msg
    );
}

#[test]
fn air_get_num_samples_requires_and_uses_raster_sample_count_cap() {
    use garnetxlate::{
        garnetx_translate_with_capabilities, translate_bytes_stage3, RDNA4xBackendCapabilities,
        GARNETX_CAP_RASTER_SAMPLES_SHIFT, RC_BAD_ARGS, RC_OK, RC_REFUSED,
    };
    let msaa_ll = r#"
target triple = "spirv-unknown-vulkan1.2"
define <4 x float> @f() {
entry:
  %ns = tail call i32 @air.get_num_samples.i32()
  %f = sitofp i32 %ns to float
  %v0 = insertelement <4 x float> poison, float %f, i32 0
  %v1 = insertelement <4 x float> %v0, float 0.0, i32 1
  %v2 = insertelement <4 x float> %v1, float 0.0, i32 2
  %v3 = insertelement <4 x float> %v2, float 1.0, i32 3
  ret <4 x float> %v3
}

declare i32 @air.get_num_samples.i32()

!air.fragment = !{!0}
!0 = !{ptr @f, !1, !2}
!1 = !{!3}
!2 = !{}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
"#;
    let run_with_flags = |flags: u32| -> Out {
        let caps = RDNA4xBackendCapabilities {
            struct_size: 24,
            version: 1,
            flags,
            subgroup_size: 32,
            subgroup_supported_stages: 0x10,
            subgroup_supported_operations: 0x80,
        };
        let mut spv: *mut u8 = std::ptr::null_mut();
        let mut spv_len = 0usize;
        let mut meta: *mut u8 = std::ptr::null_mut();
        let mut meta_len = 0usize;
        let mut err = [0 as c_char; 512];
        let rc = unsafe {
            garnetx_translate_with_capabilities(
                msaa_ll.as_ptr(),
                msaa_ll.len(),
                std::ptr::null(),
                &mut spv,
                &mut spv_len,
                &mut meta,
                &mut meta_len,
                err.as_mut_ptr(),
                err.len(),
                &caps,
            )
        };
        let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let mut spv_bytes = Vec::new();
        if rc == RC_OK {
            unsafe {
                spv_bytes = std::slice::from_raw_parts(spv, spv_len).to_vec();
                garnetx_free(spv, spv_len);
                garnetx_free(meta, meta_len);
            }
        }
        Out {
            rc,
            msg,
            spv: spv_bytes,
        }
    };

    let refused = run_with_flags(0);
    assert_eq!(refused.rc, RC_REFUSED, "unset sample count must refuse: {}", refused.msg);
    assert!(
        refused.msg.contains("raster_sample_count"),
        "expected raster_sample_count refusal, got: {}",
        refused.msg
    );

    let bad = run_with_flags(3u32 << GARNETX_CAP_RASTER_SAMPLES_SHIFT);
    assert_eq!(bad.rc, RC_BAD_ARGS, "non-power-of-two sample count must be rejected");

    let ok4 = run_with_flags(4u32 << GARNETX_CAP_RASTER_SAMPLES_SHIFT);
    assert_eq!(ok4.rc, RC_OK, "4x MSAA translation failed: {}", ok4.msg);
    let asm = metal2vulkan::disassemble(&ok4.spv).expect("disassemble");
    assert!(
        asm.lines().any(|l| l.contains("OpConstant") && l.trim_end().ends_with(" 4")),
        "{asm}"
    );
    assert!(!asm.contains("OpFunctionCall"), "{asm}");

    let stage3_caps = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: 4u32 << GARNETX_CAP_RASTER_SAMPLES_SHIFT,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };
    let (s3_spv, _) =
        translate_bytes_stage3(msaa_ll.as_bytes(), [64, 1, 1], &[], &[], &[], &[], Some(stage3_caps))
            .expect("stage3 4x MSAA translation");
    let s3_asm = metal2vulkan::disassemble(&s3_spv).expect("disassemble stage3");
    assert!(
        s3_asm.lines().any(|l| l.contains("OpConstant") && l.trim_end().ends_with(" 4")),
        "{s3_asm}"
    );
}
