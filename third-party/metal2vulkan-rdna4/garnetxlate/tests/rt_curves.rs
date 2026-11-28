// SPDX-License-Identifier: LGPL-3.0-or-later
//! Wave 2.3A integration tests for Metal 3 curve ray-tracing translation via `garnetxlate`:
//! - `GARNETX_CAP_RT_CURVES` + `GARNETX_CAP_RT_PRIMITIVE_DATA` + `GARNETX_IFT_KIND_CURVE`
//! - `intersection_query<instancing, curve_data>` (`@k_curve_iq` from Wave 0.1 AIR fixture)
//! - `intersector<instancing, curve_data>` + `[[intersection(curve, instancing, curve_data)]]`
//!   (`@k_curve_intersector` + `@curve_anyhit` from Wave 0.1 AIR fixture)
//! - Validates emitted SPIR-V with `spirv-val --target-env vulkan1.3`.

use garnetxlate::{
    garnetx_free, garnetx_translate_stage3, RDNA4xBackendCapabilities, RDNA4xIntersectionFunction,
    GARNETX_CAP_RT_CURVES, GARNETX_CAP_RT_IFT_ABI_V2, GARNETX_CAP_RT_MOTION_BLUR,
    GARNETX_CAP_RT_POSITION_FETCH, GARNETX_CAP_RT_PRIMITIVE_DATA, GARNETX_IFT_KIND_CURVE,
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

const CURVE_ANYHIT_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define i1 @curve_anyhit(float noundef %dist, float noundef %param, i32 noundef %prim) local_unnamed_addr {
entry:
  %cmp = fcmp fast ogt float %param, 2.500000e-01
  %cmp1 = fcmp fast ogt float %dist, 0.000000e+00
  %or.cond = select i1 %cmp, i1 %cmp1, i1 false
  %and = and i32 %prim, 1
  %cmp2 = icmp eq i32 %and, 0
  %spec.select = and i1 %or.cond, %cmp2
  ret i1 %spec.select
}

!air.version = !{!9}
!air.language_version = !{!10}
!air.intersection = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @curve_anyhit, !15, !17, !"air.curve", !"air.instancing", !"air.curve_data"}
!15 = !{!16}
!16 = !{!"air.accept_intersection", !"air.arg_type_name", !"bool"}
!17 = !{!18, !19, !20}
!18 = !{i32 0, !"air.distance", !"air.arg_type_name", !"float", !"air.arg_name", !"dist"}
!19 = !{i32 1, !"air.curve_parameter", !"air.arg_type_name", !"float", !"air.arg_name", !"param"}
!20 = !{i32 2, !"air.primitive_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"prim"}
"#;

const K_CURVE_IQ_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define void @k_curve_iq(ptr addrspace(1) %tlas.coerce, ptr addrspace(1) nocapture noundef writeonly "air-buffer-no-alias" %out, i32 noundef %tid) local_unnamed_addr {
entry:
  %0 = tail call ptr @air.allocate_intersection_query.instancing.curve_data()
  tail call void @air.reset_intersection_query.instancing.curve_data(ptr %0, <3 x float> <float 0.000000e+00, float 0.000000e+00, float -5.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 0x3F50624DE0000000, float 1.000000e+02, ptr addrspace(1) readonly %tlas.coerce, i32 -1, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false, i1 false)
  %1 = tail call i1 @air.next_intersection_query.instancing.curve_data(ptr %0)
  br i1 %1, label %while.body, label %while.end

while.body:
  %2 = tail call i32 @air.get_candidate_intersection_type_intersection_query.instancing.curve_data(ptr nocapture readonly %0)
  %cmp = icmp eq i32 %2, 3
  br i1 %cmp, label %if.then, label %if.end13

if.then:
  %3 = tail call fast float @air.get_candidate_curve_parameter_intersection_query.instancing.curve_data(ptr nocapture readonly %0)
  %cmp11 = fcmp fast ogt float %3, 0x3FB99999A0000000
  br i1 %cmp11, label %if.then12, label %if.end13

if.then12:
  tail call void @air.commit_curve_intersection_intersection_query.instancing.curve_data(ptr %0)
  br label %if.end13

if.end13:
  %4 = tail call i1 @air.next_intersection_query.instancing.curve_data(ptr %0)
  br i1 %4, label %while.body, label %while.end

while.end:
  %5 = tail call fast float @air.get_committed_distance_intersection_query.instancing.curve_data(ptr nocapture readonly %0)
  %vecinit15 = insertelement <4 x float> undef, float %5, i64 0
  %6 = tail call fast float @air.get_committed_curve_parameter_intersection_query.instancing.curve_data(ptr nocapture readonly %0)
  %vecinit17 = insertelement <4 x float> %vecinit15, float %6, i64 1
  %7 = tail call i32 @air.get_committed_primitive_id_intersection_query.instancing.curve_data(ptr nocapture readonly %0)
  %8 = tail call fast float @air.convert.f.f32.u.i32(i32 %7)
  %vecinit19 = insertelement <4 x float> %vecinit17, float %8, i64 2
  %9 = tail call i32 @air.get_committed_intersection_type_intersection_query.instancing.curve_data(ptr nocapture readonly %0)
  %10 = tail call fast float @air.convert.f.f32.u.i32(i32 %9)
  %vecinit21 = insertelement <4 x float> %vecinit19, float %10, i64 3
  %idxprom = zext i32 %tid to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idxprom
  store <4 x float> %vecinit21, ptr addrspace(1) %arrayidx, align 16
  tail call void @air.deallocate_intersection_query.instancing.curve_data(ptr %0)
  ret void
}

declare void @air.deallocate_intersection_query.instancing.curve_data(ptr) local_unnamed_addr
declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr
declare i32 @air.get_committed_intersection_type_intersection_query.instancing.curve_data(ptr nocapture readonly) local_unnamed_addr
declare i32 @air.get_committed_primitive_id_intersection_query.instancing.curve_data(ptr nocapture readonly) local_unnamed_addr
declare float @air.get_committed_curve_parameter_intersection_query.instancing.curve_data(ptr nocapture readonly) local_unnamed_addr
declare float @air.get_committed_distance_intersection_query.instancing.curve_data(ptr nocapture readonly) local_unnamed_addr
declare i1 @air.next_intersection_query.instancing.curve_data(ptr) local_unnamed_addr
declare void @air.commit_curve_intersection_intersection_query.instancing.curve_data(ptr) local_unnamed_addr
declare float @air.get_candidate_curve_parameter_intersection_query.instancing.curve_data(ptr nocapture readonly) local_unnamed_addr
declare i32 @air.get_candidate_intersection_type_intersection_query.instancing.curve_data(ptr nocapture readonly) local_unnamed_addr
declare void @air.reset_intersection_query.instancing.curve_data(ptr, <3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1) local_unnamed_addr
declare ptr @air.allocate_intersection_query.instancing.curve_data() local_unnamed_addr

!air.version = !{!9}
!air.language_version = !{!10}
!air.kernel = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @k_curve_iq, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19}
!17 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing>", !"air.arg_name", !"tlas"}
!18 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!19 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;

const K_CURVE_INTERSECTOR_LL: &str = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

define void @k_curve_intersector(ptr addrspace(1) %tlas.coerce, ptr addrspace(1) %ift.coerce, ptr addrspace(1) nocapture noundef writeonly "air-buffer-no-alias" %out, i32 noundef %tid) local_unnamed_addr {
entry:
  %0 = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, float } @air.intersect.instancing.curve_data(<3 x float> <float 0.000000e+00, float 0.000000e+00, float -5.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float 0x3F50624DE0000000, float 1.000000e+02, ptr addrspace(1) readonly %tlas.coerce, i32 255, ptr addrspace(1) readonly %ift.coerce, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 -1, i32 -1, i32 0, i1 false, i1 false)
  %1 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, float } %0, 0
  %2 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, float } %0, 1
  %3 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, float } %0, 2
  %4 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, float } %0, 7
  %vecinit9 = insertelement <4 x float> undef, float %2, i64 0
  %vecinit10 = insertelement <4 x float> %vecinit9, float %4, i64 1
  %5 = tail call fast float @air.convert.f.f32.u.i32(i32 %3)
  %vecinit11 = insertelement <4 x float> %vecinit10, float %5, i64 2
  %6 = tail call fast float @air.convert.f.f32.u.i32(i32 %1)
  %vecinit12 = insertelement <4 x float> %vecinit11, float %6, i64 3
  %idxprom = zext i32 %tid to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idxprom
  store <4 x float> %vecinit12, ptr addrspace(1) %arrayidx, align 16
  ret void
}

declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, float } @air.intersect.instancing.curve_data(<3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, ptr addrspace(1) readonly, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1) local_unnamed_addr

!air.version = !{!9}
!air.language_version = !{!10}
!air.kernel = !{!14}

!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 3, i32 2, i32 0}
!14 = !{ptr @k_curve_intersector, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing>", !"air.arg_name", !"tlas"}
!18 = !{i32 1, !"air.intersection_function_table", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.arg_type_name", !"intersection_function_table<instancing, curve_data>", !"air.arg_name", !"ift"}
!19 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!20 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;

#[test]
fn wave2_rt_curves_iq_and_intersector_with_curve_anyhit_ffi() {
    assert_eq!(GARNETX_CAP_RT_CURVES, 1 << 13);
    assert_eq!(GARNETX_CAP_RT_MOTION_BLUR, 1 << 14);
    assert_eq!(GARNETX_IFT_KIND_CURVE, 2);
    assert_eq!(GARNETX_RT_PRIM_DATA_BINDING, 676);

    let caps_curve = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: GARNETX_CAP_RT_POSITION_FETCH
            | GARNETX_CAP_RT_IFT_ABI_V2
            | GARNETX_CAP_RT_PRIMITIVE_DATA
            | GARNETX_CAP_RT_CURVES,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };

    // 1. intersection_query<instancing, curve_data>
    let out_iq = call_stage3_ift_with_caps(K_CURVE_IQ_LL, Some([1, 1, 1]), &[], Some(caps_curve));
    assert_eq!(out_iq.rc, RC_OK, "k_curve_iq failed: {}", out_iq.msg);
    let asm_iq = spv_asm(&out_iq.spv);
    assert!(asm_iq.contains("OpRayQueryInitializeKHR"), "{asm_iq}");
    assert!(asm_iq.contains("OpRayQueryProceedKHR"), "{asm_iq}");
    assert!(asm_iq.contains("OpRayQueryGenerateIntersectionKHR"), "{asm_iq}");
    assert!(!asm_iq.contains("_intersection_query."), "{asm_iq}");
    assert_spirv_val("k_curve_iq", &out_iq.spv);
    let meta_iq: Value = serde_json::from_slice(&out_iq.meta).expect("iq meta json");
    assert_has_rt_prim_data_binding(&meta_iq, "k_curve_iq");

    // 2. intersector<instancing, curve_data> + @curve_anyhit (GARNETX_IFT_KIND_CURVE)
    let out_it = call_stage3_ift_with_caps(
        K_CURVE_INTERSECTOR_LL,
        Some([1, 1, 1]),
        &[("curve_anyhit", GARNETX_IFT_KIND_CURVE, CURVE_ANYHIT_LL)],
        Some(caps_curve),
    );
    assert_eq!(
        out_it.rc, RC_OK,
        "k_curve_intersector + curve_anyhit failed: {}",
        out_it.msg
    );
    let asm_it = spv_asm(&out_it.spv);
    assert!(asm_it.contains("OpRayQueryInitializeKHR"), "{asm_it}");
    assert!(asm_it.contains("OpRayQueryProceedKHR"), "{asm_it}");
    assert!(asm_it.contains("OpRayQueryGenerateIntersectionKHR"), "{asm_it}");
    assert!(!asm_it.contains("air.intersect."), "{asm_it}");
    assert_spirv_val("k_curve_intersector_anyhit", &out_it.spv);
    let meta_it: Value = serde_json::from_slice(&out_it.meta).expect("intersector meta json");
    assert_has_rt_prim_data_binding(&meta_it, "k_curve_intersector");
    let ift_fns = meta_it["intersection_function_tables"]["handles"]
        .as_array()
        .unwrap_or_else(|| panic!("missing intersection_function_tables.handles in {meta_it}"));
    assert!(
        ift_fns
            .iter()
            .any(|f| f["symbol"] == "curve_anyhit" && f["kind"] == "Curve" && f["handle_id"] == 1),
        "expected Curve handle in intersection_function_tables.handles: {meta_it}"
    );

    // 3. GARNETX_CAP_RT_CURVES=0 + GARNETX_CAP_RT_PRIMITIVE_DATA=0 non-regression check.
    let caps_no_curve = RDNA4xBackendCapabilities {
        flags: GARNETX_CAP_RT_POSITION_FETCH | GARNETX_CAP_RT_IFT_ABI_V2,
        ..caps_curve
    };
    let out_off = call_stage3_ift_with_caps(K_CURVE_IQ_LL, Some([1, 1, 1]), &[], Some(caps_no_curve));
    assert_eq!(out_off.rc, RC_OK, "k_curve_iq with caps off failed: {}", out_off.msg);
    assert_spirv_val("k_curve_iq_off", &out_off.spv);
    let meta_off: Value = serde_json::from_slice(&out_off.meta).expect("iq_off meta json");
    let has_prim_tbl = meta_off["bindings"]
        .as_array()
        .is_some_and(|arr| arr.iter().any(|b| b["kind"] == "RTPrimDataTable"));
    assert!(
        !has_prim_tbl,
        "RTPrimDataTable must not be emitted when RT_CURVES and RT_PRIMITIVE_DATA are off: {meta_off}"
    );
}
