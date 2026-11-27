// SPDX-License-Identifier: LGPL-3.0-or-later
//! Integration tests for Metal 3 motion-blur ray-tracing translation via `garnetxlate`:
//! - `GARNETX_CAP_RT_MOTION_BLUR` + `GARNETX_CAP_RT_PRIMITIVE_DATA` + `GARNETX_CAP_RT_IFT_ABI_V2`
//! - Apple `air64_v28` fixtures (`@k_motion_inst`, `@k_motion_prim`, `@k_motion_both`)
//! - `intersection_query<primitive_motion>` and `intersection_query<instancing, primitive_motion, instance_motion>`
//! - `intersector<instancing, primitive_motion, instance_motion>` with triangle IFT + world transforms
//! - Gating check (`GARNETX_CAP_RT_MOTION_BLUR` unset vs set)
//! - Validates every emitted SPIR-V module with `spirv-val --target-env vulkan1.3`.

use garnetxlate::{
    garnetx_free, garnetx_translate_stage3, RDNA4xBackendCapabilities, RDNA4xIntersectionFunction,
    GARNETX_CAP_RT_CURVES, GARNETX_CAP_RT_IFT_ABI_V2, GARNETX_CAP_RT_MOTION_BLUR,
    GARNETX_CAP_RT_POSITION_FETCH, GARNETX_CAP_RT_PRIMITIVE_DATA, GARNETX_IFT_KIND_TRIANGLE_ANY_HIT,
    GARNETX_RT_PRIM_DATA_BINDING, RC_OK,
};
use serde_json::Value;
use std::os::raw::c_char;

struct Out {
    rc: i32,
    msg: String,
    spv: Vec<u8>,
    meta: Vec<u8>,
}

fn call_stage3_ift_with_caps(
    ll: &str,
    local: Option<[u32; 3]>,
    ift_fns: &[(&str, u32, &str)],
    caps: Option<RDNA4xBackendCapabilities>,
) -> Out {
    let c_ift: Vec<RDNA4xIntersectionFunction> = ift_fns
        .iter()
        .map(|(s, k, t)| RDNA4xIntersectionFunction {
            symbol: s.as_ptr(),
            symbol_len: s.len(),
            ll: t.as_ptr(),
            ll_len: t.len(),
            kind: *k,
        })
        .collect();
    let mut spv: *mut u8 = std::ptr::null_mut();
    let mut spv_len = 0usize;
    let mut meta: *mut u8 = std::ptr::null_mut();
    let mut meta_len = 0usize;
    let mut err = [0 as c_char; 512];
    let local_ptr = local.as_ref().map_or(std::ptr::null(), |l| l.as_ptr());
    let caps_ptr = caps.as_ref().map_or(std::ptr::null(), |c| c as *const _);
    let rc = unsafe {
        garnetx_translate_stage3(
            ll.as_ptr(),
            ll.len(),
            local_ptr,
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
            c_ift.as_ptr(),
            c_ift.len(),
            caps_ptr,
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
    let (mut s, mut m) = (Vec::new(), Vec::new());
    if rc == RC_OK {
        unsafe {
            s = std::slice::from_raw_parts(spv, spv_len).to_vec();
            m = std::slice::from_raw_parts(meta, meta_len).to_vec();
            garnetx_free(spv, spv_len);
            garnetx_free(meta, meta_len);
        }
    } else {
        assert!(spv.is_null() && meta.is_null() && spv_len == 0 && meta_len == 0);
    }
    Out {
        rc,
        msg,
        spv: s,
        meta: m,
    }
}

fn spv_asm(spv: &[u8]) -> String {
    metal2vulkan::disassemble(spv).expect("disassemble SPIR-V")
}

fn assert_spirv_val(label: &str, spv: &[u8]) {
    use std::io::Write;
    let mut child = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.3", "-"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .expect("spirv-val must be available in PATH");
    child.stdin.as_mut().unwrap().write_all(spv).unwrap();
    let out = child.wait_with_output().unwrap();
    assert!(
        out.status.success(),
        "spirv-val rejected {label} SPIR-V:\n{}\n--- ASM ---\n{}",
        String::from_utf8_lossy(&out.stderr),
        spv_asm(spv)
    );
}

fn assert_has_rt_prim_data_binding(meta: &Value, label: &str) {
    let binding = meta["bindings"]
        .as_array()
        .and_then(|arr| arr.iter().find(|b| b["kind"] == "RTPrimDataTable"))
        .unwrap_or_else(|| panic!("{label}: missing RTPrimDataTable binding in {meta}"));
    assert_eq!(binding["descriptor"]["binding"], GARNETX_RT_PRIM_DATA_BINDING, "{label}: {binding}");
    assert_eq!(binding["declared_size"], 264, "{label}: {binding}");
}

fn motion_caps(flags: u32) -> RDNA4xBackendCapabilities {
    RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    }
}

const K_MOTION_INST_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define void @k_motion_inst(ptr addrspace(1) %tlas.coerce, ptr addrspace(1) nocapture noundef writeonly "air-buffer-no-alias" %out, ptr addrspace(2) nocapture noundef readonly align 4 dereferenceable(4) "air-buffer-no-alias" %tm, i32 noundef %tid) local_unnamed_addr {
entry:
  %0 = load float, ptr addrspace(2) %tm, align 4
  %1 = tail call ptr addrspace(1) @air.get_null_intersection_function_table()
  %2 = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data.instance_motion(<3 x float> <float 0.000000e+00, float 0.000000e+00, float -5.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 0x3F50624DE0000000, float 1.000000e+02, ptr addrspace(1) readonly %tlas.coerce, i32 -1, float %0, ptr addrspace(1) readonly %1, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false, i1 false)
  %3 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %2, 1
  %4 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %2, 2
  %5 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %2, 7
  %vecinit7 = insertelement <4 x float> undef, float %3, i64 0
  %6 = extractelement <2 x float> %5, i64 0
  %vecinit8 = insertelement <4 x float> %vecinit7, float %6, i64 1
  %7 = extractelement <2 x float> %5, i64 1
  %vecinit11 = insertelement <4 x float> %vecinit8, float %7, i64 2
  %8 = tail call fast float @air.convert.f.f32.u.i32(i32 %4)
  %vecinit12 = insertelement <4 x float> %vecinit11, float %8, i64 3
  %idxprom = zext i32 %tid to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idxprom
  store <4 x float> %vecinit12, ptr addrspace(1) %arrayidx, align 16
  ret void
}

declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data.instance_motion(<3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, float, ptr addrspace(1) readonly, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1) local_unnamed_addr
declare ptr addrspace(1) @air.get_null_intersection_function_table() local_unnamed_addr

!air.version = !{!9}
!air.language_version = !{!10}
!air.kernel = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @k_motion_inst, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing, instance_motion>", !"air.arg_name", !"tlas"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!19 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"tm"}
!20 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;

const K_MOTION_PRIM_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define void @k_motion_prim(ptr addrspace(1) %blas.coerce, ptr addrspace(1) nocapture noundef writeonly "air-buffer-no-alias" %out, ptr addrspace(2) nocapture noundef readonly align 4 dereferenceable(4) "air-buffer-no-alias" %tm, i32 noundef %tid) local_unnamed_addr {
entry:
  %0 = load float, ptr addrspace(2) %tm, align 4
  %1 = tail call ptr addrspace(1) @air.get_null_intersection_function_table()
  %2 = tail call { i32, float, i32, i32, ptr addrspace(1), <2 x float>, i1 } @air.intersect.triangle_data.primitive_motion(<3 x float> <float 0.000000e+00, float 0.000000e+00, float -5.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 0x3F50624DE0000000, float 1.000000e+02, ptr addrspace(1) readonly %blas.coerce, float %0, ptr addrspace(1) readonly %1, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false)
  %3 = extractvalue { i32, float, i32, i32, ptr addrspace(1), <2 x float>, i1 } %2, 1
  %4 = extractvalue { i32, float, i32, i32, ptr addrspace(1), <2 x float>, i1 } %2, 2
  %5 = extractvalue { i32, float, i32, i32, ptr addrspace(1), <2 x float>, i1 } %2, 5
  %vecinit7 = insertelement <4 x float> undef, float %3, i64 0
  %6 = extractelement <2 x float> %5, i64 0
  %vecinit8 = insertelement <4 x float> %vecinit7, float %6, i64 1
  %7 = extractelement <2 x float> %5, i64 1
  %vecinit11 = insertelement <4 x float> %vecinit8, float %7, i64 2
  %8 = tail call fast float @air.convert.f.f32.u.i32(i32 %4)
  %vecinit12 = insertelement <4 x float> %vecinit11, float %8, i64 3
  %idxprom = zext i32 %tid to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idxprom
  store <4 x float> %vecinit12, ptr addrspace(1) %arrayidx, align 16
  ret void
}

declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr
declare { i32, float, i32, i32, ptr addrspace(1), <2 x float>, i1 } @air.intersect.triangle_data.primitive_motion(<3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, float, ptr addrspace(1) readonly, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1) local_unnamed_addr
declare ptr addrspace(1) @air.get_null_intersection_function_table() local_unnamed_addr

!air.version = !{!9}
!air.language_version = !{!10}
!air.kernel = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @k_motion_prim, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.primitive_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<primitive_motion>", !"air.arg_name", !"blas"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!19 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"tm"}
!20 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;

const K_MOTION_BOTH_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define void @k_motion_both(ptr addrspace(1) %tlas.coerce, ptr addrspace(1) nocapture noundef writeonly "air-buffer-no-alias" %out, ptr addrspace(2) nocapture noundef readonly align 4 dereferenceable(4) "air-buffer-no-alias" %tm, i32 noundef %tid) local_unnamed_addr {
entry:
  %0 = load float, ptr addrspace(2) %tm, align 4
  %1 = tail call ptr addrspace(1) @air.get_null_intersection_function_table()
  %2 = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data.primitive_motion.instance_motion(<3 x float> <float 0.000000e+00, float 0.000000e+00, float -5.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 0x3F50624DE0000000, float 1.000000e+02, ptr addrspace(1) readonly %tlas.coerce, i32 -1, float %0, ptr addrspace(1) readonly %1, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false, i1 false)
  %3 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %2, 1
  %4 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %2, 2
  %5 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %2, 7
  %vecinit7 = insertelement <4 x float> undef, float %3, i64 0
  %6 = extractelement <2 x float> %5, i64 0
  %vecinit8 = insertelement <4 x float> %vecinit7, float %6, i64 1
  %7 = extractelement <2 x float> %5, i64 1
  %vecinit11 = insertelement <4 x float> %vecinit8, float %7, i64 2
  %8 = tail call fast float @air.convert.f.f32.u.i32(i32 %4)
  %vecinit12 = insertelement <4 x float> %vecinit11, float %8, i64 3
  %idxprom = zext i32 %tid to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idxprom
  store <4 x float> %vecinit12, ptr addrspace(1) %arrayidx, align 16
  ret void
}

declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data.primitive_motion.instance_motion(<3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, float, ptr addrspace(1) readonly, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1) local_unnamed_addr
declare ptr addrspace(1) @air.get_null_intersection_function_table() local_unnamed_addr

!air.version = !{!9}
!air.language_version = !{!10}
!air.kernel = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @k_motion_both, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing, primitive_motion, instance_motion>", !"air.arg_name", !"tlas"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!19 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"tm"}
!20 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;

const K_MOTION_IQ_PRIM_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define void @k_motion_iq_prim(ptr addrspace(1) %blas.coerce, ptr addrspace(1) nocapture noundef writeonly "air-buffer-no-alias" %out, ptr addrspace(2) nocapture noundef readonly align 4 dereferenceable(4) "air-buffer-no-alias" %tm, i32 noundef %tid) local_unnamed_addr {
entry:
  %0 = load float, ptr addrspace(2) %tm, align 4
  %q = tail call ptr @air.allocate_intersection_query.triangle_data.primitive_motion()
  tail call void @air.reset_intersection_query.triangle_data.primitive_motion(ptr %q, <3 x float> <float 0.000000e+00, float 0.000000e+00, float -5.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 1.000000e-03, float 1.000000e+02, ptr addrspace(1) readonly %blas.coerce, float %0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false)
  br label %loop

loop:
  %more = tail call i1 @air.next_intersection_query.triangle_data.primitive_motion(ptr %q)
  br i1 %more, label %cand, label %done

cand:
  %ct = tail call i32 @air.get_candidate_intersection_type_intersection_query.triangle_data.primitive_motion(ptr %q)
  %is_tri = icmp eq i32 %ct, 1
  br i1 %is_tri, label %do_commit, label %loop

do_commit:
  tail call void @air.commit_triangle_intersection_intersection_query.triangle_data.primitive_motion(ptr %q)
  br label %loop

done:
  %cm = tail call i32 @air.get_committed_intersection_type_intersection_query.triangle_data.primitive_motion(ptr %q)
  %dist = tail call float @air.get_committed_distance_intersection_query.triangle_data.primitive_motion(ptr %q)
  %bary = tail call <2 x float> @air.get_committed_triangle_barycentric_coord_intersection_query.triangle_data.primitive_motion(ptr %q)
  %ff = tail call i1 @air.is_committed_triangle_front_facing_intersection_query.triangle_data.primitive_motion(ptr %q)
  tail call void @air.deallocate_intersection_query.triangle_data.primitive_motion(ptr %q)
  %bx = extractelement <2 x float> %bary, i64 0
  %ff_f = select i1 %ff, float 1.000000e+00, float 0.000000e+00
  %cm_f = sitofp i32 %cm to float
  %v0 = insertelement <4 x float> poison, float %dist, i64 0
  %v1 = insertelement <4 x float> %v0, float %bx, i64 1
  %v2 = insertelement <4 x float> %v1, float %ff_f, i64 2
  %v3 = insertelement <4 x float> %v2, float %cm_f, i64 3
  %idx = zext i32 %tid to i64
  %dst = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idx
  store <4 x float> %v3, ptr addrspace(1) %dst, align 16
  ret void
}

declare ptr @air.allocate_intersection_query.triangle_data.primitive_motion()
declare void @air.reset_intersection_query.triangle_data.primitive_motion(ptr, <3 x float>, <3 x float>, float, float, ptr addrspace(1), float, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1)
declare i1 @air.next_intersection_query.triangle_data.primitive_motion(ptr)
declare i32 @air.get_candidate_intersection_type_intersection_query.triangle_data.primitive_motion(ptr)
declare void @air.commit_triangle_intersection_intersection_query.triangle_data.primitive_motion(ptr)
declare i32 @air.get_committed_intersection_type_intersection_query.triangle_data.primitive_motion(ptr)
declare float @air.get_committed_distance_intersection_query.triangle_data.primitive_motion(ptr)
declare <2 x float> @air.get_committed_triangle_barycentric_coord_intersection_query.triangle_data.primitive_motion(ptr)
declare i1 @air.is_committed_triangle_front_facing_intersection_query.triangle_data.primitive_motion(ptr)
declare void @air.deallocate_intersection_query.triangle_data.primitive_motion(ptr)

!air.version = !{!9}
!air.language_version = !{!10}
!air.kernel = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @k_motion_iq_prim, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.primitive_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<primitive_motion>", !"air.arg_name", !"blas"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!19 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"tm"}
!20 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;

const K_MOTION_IQ_INST_WORLD_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define void @k_motion_iq_inst_world(ptr addrspace(1) %tlas.coerce, ptr addrspace(1) nocapture noundef writeonly "air-buffer-no-alias" %out, ptr addrspace(2) nocapture noundef readonly align 4 dereferenceable(4) "air-buffer-no-alias" %tm, i32 noundef %tid) local_unnamed_addr {
entry:
  %0 = load float, ptr addrspace(2) %tm, align 4
  %q = tail call ptr @air.allocate_intersection_query.instancing.triangle_data.primitive_motion.instance_motion()
  tail call void @air.reset_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr %q, <3 x float> <float 0.000000e+00, float 0.000000e+00, float -5.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 1.000000e-03, float 1.000000e+02, ptr addrspace(1) readonly %tlas.coerce, i32 -1, float %0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false, i1 false)
  br label %loop

loop:
  %more = tail call i1 @air.next_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr %q)
  br i1 %more, label %cand, label %done

cand:
  %ct = tail call i32 @air.get_candidate_intersection_type_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr %q)
  %is_tri = icmp eq i32 %ct, 1
  br i1 %is_tri, label %do_commit, label %loop

do_commit:
  tail call void @air.commit_triangle_intersection_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr %q)
  br label %loop

done:
  %dist = tail call float @air.get_committed_distance_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr %q)
  %iid = tail call i32 @air.get_committed_instance_id_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr %q)
  %o2w = tail call { <3 x float>, <3 x float>, <3 x float>, <3 x float> } @air.get_committed_object_to_world_transform_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr %q)
  tail call void @air.deallocate_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr %q)
  %c3 = extractvalue { <3 x float>, <3 x float>, <3 x float>, <3 x float> } %o2w, 3
  %tx = extractelement <3 x float> %c3, i64 0
  %iid_f = uitofp i32 %iid to float
  %v0 = insertelement <4 x float> poison, float %dist, i64 0
  %v1 = insertelement <4 x float> %v0, float %tx, i64 1
  %v2 = insertelement <4 x float> %v1, float %iid_f, i64 2
  %v3 = insertelement <4 x float> %v2, float 0.000000e+00, i64 3
  %idx = zext i32 %tid to i64
  %dst = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idx
  store <4 x float> %v3, ptr addrspace(1) %dst, align 16
  ret void
}

declare ptr @air.allocate_intersection_query.instancing.triangle_data.primitive_motion.instance_motion()
declare void @air.reset_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr, <3 x float>, <3 x float>, float, float, ptr addrspace(1), i32, float, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)
declare i1 @air.next_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr)
declare i32 @air.get_candidate_intersection_type_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr)
declare void @air.commit_triangle_intersection_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr)
declare float @air.get_committed_distance_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr)
declare i32 @air.get_committed_instance_id_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr)
declare { <3 x float>, <3 x float>, <3 x float>, <3 x float> } @air.get_committed_object_to_world_transform_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr)
declare void @air.deallocate_intersection_query.instancing.triangle_data.primitive_motion.instance_motion(ptr)

!air.version = !{!9}
!air.language_version = !{!10}
!air.kernel = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @k_motion_iq_inst_world, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing, primitive_motion, instance_motion>", !"air.arg_name", !"tlas"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!19 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"tm"}
!20 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;

const TRI_ANYHIT_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define i1 @tri_anyhit(float noundef %dist, <2 x float> noundef %bary, i1 noundef %ff) local_unnamed_addr {
entry:
  %bx = extractelement <2 x float> %bary, i64 0
  %c0 = fcmp ogt float %bx, 1.000000e-01
  %c1 = fcmp ogt float %dist, 0.000000e+00
  %c2 = select i1 %ff, i1 %c0, i1 false
  %ok = and i1 %c1, %c2
  ret i1 %ok
}

!air.version = !{!9}
!air.language_version = !{!10}
!air.intersection = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @tri_anyhit, !15, !17, !"air.triangle", !"air.instancing"}
!15 = !{!16}
!16 = !{!"air.accept_intersection", !"air.arg_type_name", !"bool"}
!17 = !{!18, !19, !20}
!18 = !{i32 0, !"air.distance", !"air.arg_type_name", !"float", !"air.arg_name", !"dist"}
!19 = !{i32 1, !"air.barycentric_coord", !"air.arg_type_name", !"float2", !"air.arg_name", !"bary"}
!20 = !{i32 2, !"air.front_facing", !"air.arg_type_name", !"bool", !"air.arg_name", !"ff"}
"#;

const K_MOTION_BOTH_IFT_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define void @k_motion_both_ift(ptr addrspace(1) %tlas.coerce, ptr addrspace(1) %ift.coerce, ptr addrspace(1) nocapture noundef writeonly "air-buffer-no-alias" %out, ptr addrspace(2) nocapture noundef readonly align 4 dereferenceable(4) "air-buffer-no-alias" %tm, i32 noundef %tid) local_unnamed_addr {
entry:
  %0 = load float, ptr addrspace(2) %tm, align 4
  %1 = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data.primitive_motion.instance_motion(<3 x float> <float 0.000000e+00, float 0.000000e+00, float -5.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 1.000000e-03, float 1.000000e+02, ptr addrspace(1) readonly %tlas.coerce, i32 -1, float %0, ptr addrspace(1) readonly %ift.coerce, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false, i1 false)
  %d = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %1, 1
  %p = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %1, 2
  %pf = sitofp i32 %p to float
  %v0 = insertelement <4 x float> poison, float %d, i64 0
  %v1 = insertelement <4 x float> %v0, float %pf, i64 1
  %v2 = insertelement <4 x float> %v1, float 0.000000e+00, i64 2
  %v3 = insertelement <4 x float> %v2, float 1.000000e+00, i64 3
  %idx = zext i32 %tid to i64
  %dst = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idx
  store <4 x float> %v3, ptr addrspace(1) %dst, align 16
  ret void
}

declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data.primitive_motion.instance_motion(<3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, float, ptr addrspace(1) readonly, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)

!air.version = !{!9}
!air.language_version = !{!10}
!air.kernel = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @k_motion_both_ift, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20, !21}
!17 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing, primitive_motion, instance_motion>", !"air.arg_name", !"tlas"}
!18 = !{i32 1, !"air.intersection_function_table", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"intersection_function_table<triangle_data, instancing>", !"air.arg_name", !"ift"}
!19 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!20 = !{i32 3, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"tm"}
!21 = !{i32 4, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;

#[test]
fn stage3_motion_blur_air_fixtures_validate_under_motion_cap() {
    let caps = Some(motion_caps(
        GARNETX_CAP_RT_POSITION_FETCH
            | GARNETX_CAP_RT_MOTION_BLUR
            | GARNETX_CAP_RT_PRIMITIVE_DATA
            | GARNETX_CAP_RT_IFT_ABI_V2,
    ));

    for (fn_name, ll) in [
        ("k_motion_inst", K_MOTION_INST_LL),
        ("k_motion_prim", K_MOTION_PRIM_LL),
        ("k_motion_both", K_MOTION_BOTH_LL),
    ] {
        let out = call_stage3_ift_with_caps(ll, Some([64, 1, 1]), &[], caps);
        assert_eq!(out.rc, RC_OK, "{fn_name}: {}", out.msg);
        assert_spirv_val(fn_name, &out.spv);
        let asm = spv_asm(&out.spv);
        assert!(
            asm.contains("OpRayQueryProceedKHR"),
            "{fn_name}: missing OpRayQueryProceedKHR"
        );
        assert!(
            asm.contains("OpRayQueryGenerateIntersectionKHR"),
            "{fn_name}: missing OpRayQueryGenerateIntersectionKHR for motion blur"
        );
        if fn_name != "k_motion_prim" {
            assert!(
                asm.contains("OpConvertUToAccelerationStructureKHR"),
                "{fn_name}: missing OpConvertUToAccelerationStructureKHR"
            );
        }
        let meta: Value = serde_json::from_slice(&out.meta).expect("metadata JSON");
        assert_has_rt_prim_data_binding(&meta, fn_name);
    }
}

#[test]
fn stage3_motion_blur_gating_disables_motion_lowering_when_cap_unset() {
    let out_off = call_stage3_ift_with_caps(K_MOTION_PRIM_LL, Some([64, 1, 1]), &[], None);
    assert_eq!(out_off.rc, RC_OK, "gating off: {}", out_off.msg);
    assert_spirv_val("k_motion_prim_cap_off", &out_off.spv);
    let asm_off = spv_asm(&out_off.spv);
    assert!(
        !asm_off.contains("OpRayQueryGenerateIntersectionKHR"),
        "motion cap disabled should not emit motion-triangle OpRayQueryGenerateIntersectionKHR:\n{asm_off}"
    );
}

#[test]
fn stage3_motion_blur_intersection_query_and_ift_validate() {
    let caps = Some(motion_caps(
        GARNETX_CAP_RT_POSITION_FETCH
            | GARNETX_CAP_RT_MOTION_BLUR
            | GARNETX_CAP_RT_CURVES
            | GARNETX_CAP_RT_PRIMITIVE_DATA
            | GARNETX_CAP_RT_IFT_ABI_V2,
    ));

    let out_iq_prim = call_stage3_ift_with_caps(K_MOTION_IQ_PRIM_LL, Some([64, 1, 1]), &[], caps);
    assert_eq!(out_iq_prim.rc, RC_OK, "k_motion_iq_prim: {}", out_iq_prim.msg);
    assert_spirv_val("k_motion_iq_prim", &out_iq_prim.spv);
    let asm_iq_prim = spv_asm(&out_iq_prim.spv);
    assert!(asm_iq_prim.contains("OpRayQueryGenerateIntersectionKHR"));

    let out_iq_inst =
        call_stage3_ift_with_caps(K_MOTION_IQ_INST_WORLD_LL, Some([64, 1, 1]), &[], caps);
    assert_eq!(
        out_iq_inst.rc, RC_OK,
        "k_motion_iq_inst_world: {}",
        out_iq_inst.msg
    );
    assert_spirv_val("k_motion_iq_inst_world", &out_iq_inst.spv);
    let asm_iq_inst = spv_asm(&out_iq_inst.spv);
    assert!(asm_iq_inst.contains("OpRayQueryGenerateIntersectionKHR"));
    assert!(asm_iq_inst.contains("OpConvertUToAccelerationStructureKHR"));

    let ift_fns = [("tri_anyhit", GARNETX_IFT_KIND_TRIANGLE_ANY_HIT, TRI_ANYHIT_LL)];
    let out_ift =
        call_stage3_ift_with_caps(K_MOTION_BOTH_IFT_LL, Some([64, 1, 1]), &ift_fns, caps);
    assert_eq!(out_ift.rc, RC_OK, "k_motion_both_ift: {}", out_ift.msg);
    assert_spirv_val("k_motion_both_ift", &out_ift.spv);
    let asm_ift = spv_asm(&out_ift.spv);
    assert!(asm_ift.contains("OpRayQueryGenerateIntersectionKHR"));
    assert!(asm_ift.contains("OpRayQueryConfirmIntersectionKHR"));
}
