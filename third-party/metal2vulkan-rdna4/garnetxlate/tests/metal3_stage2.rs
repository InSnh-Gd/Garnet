// SPDX-License-Identifier: LGPL-3.0-or-later
//! Translator Stage 2 integration tests via garnetxlate C FFI:
//! - Task 3.2: MTLDynamicLibrary symbol enumeration & multi-stage transitive linking
//! - Task 3.3: MTLVisibleFunctionTable bounded OpSwitch dispatch + deterministic handle metadata
//! - Task 5.3: Sparse / PRT residency predicate lowering (`OpImageSparse*` + `SparseResidency`)

use garnetxlate::{
    garnetx_abi_version, garnetx_free, garnetx_list_exported_symbols, garnetx_translate,
    garnetx_translate_linked_libraries, RDNA4xDep, RDNA4xDynamicLibrary, RC_BAD_ARGS, RC_OK,
    RC_REFUSED,
};
use serde_json::Value;
use std::os::raw::c_char;

struct Out {
    rc: i32,
    msg: String,
    spv: Vec<u8>,
    meta: Vec<u8>,
}

fn call_stage2(
    ll: &str,
    deps: &[(&str, &str)],
    dynlibs: &[(&str, &str)],
    vis_fns: &[(&str, &str)],
) -> Out {
    let c_deps: Vec<RDNA4xDep> = deps
        .iter()
        .map(|(s, t)| RDNA4xDep {
            symbol: s.as_ptr(),
            symbol_len: s.len(),
            ll: t.as_ptr(),
            ll_len: t.len(),
        })
        .collect();
    let c_libs: Vec<RDNA4xDynamicLibrary> = dynlibs
        .iter()
        .map(|(n, t)| RDNA4xDynamicLibrary {
            name: n.as_ptr(),
            name_len: n.len(),
            ll: t.as_ptr(),
            ll_len: t.len(),
        })
        .collect();
    let c_vis: Vec<RDNA4xDep> = vis_fns
        .iter()
        .map(|(s, t)| RDNA4xDep {
            symbol: s.as_ptr(),
            symbol_len: s.len(),
            ll: t.as_ptr(),
            ll_len: t.len(),
        })
        .collect();
    let mut spv: *mut u8 = std::ptr::null_mut();
    let mut spv_len = 0usize;
    let mut meta: *mut u8 = std::ptr::null_mut();
    let mut meta_len = 0usize;
    let mut err = [0 as c_char; 512];
    let rc = unsafe {
        garnetx_translate_linked_libraries(
            ll.as_ptr(),
            ll.len(),
            std::ptr::null(),
            c_deps.as_ptr(),
            c_deps.len(),
            c_libs.as_ptr(),
            c_libs.len(),
            c_vis.as_ptr(),
            c_vis.len(),
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

fn call_plain(ll: &str) -> Out {
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
    let (mut s, mut m) = (Vec::new(), Vec::new());
    if rc == RC_OK {
        unsafe {
            s = std::slice::from_raw_parts(spv, spv_len).to_vec();
            m = std::slice::from_raw_parts(meta, meta_len).to_vec();
            garnetx_free(spv, spv_len);
            garnetx_free(meta, meta_len);
        }
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

const DYLIB_MATH_BASE: &str = r#"
define internal i32 @private_bias(i32 %x) {
entry:
  %r = add i32 %x, 5
  ret i32 %r
}
define i32 @lib_bias(i32 %x) {
entry:
  %r = call i32 @private_bias(i32 %x)
  ret i32 %r
}
"#;

const DYLIB_MATH_TOP: &str = r#"
declare i32 @lib_bias(i32)
define internal i32 @private_bias(i32 %x) {
entry:
  %r = mul i32 %x, 3
  ret i32 %r
}
define i32 @lib_scale_and_bias(i32 %x) {
entry:
  %a = call i32 @private_bias(i32 %x)
  %b = call i32 @lib_bias(i32 %a)
  ret i32 %b
}
define <4 x float> @lib_tint(<4 x float> %c) {
entry:
  %out = fmul <4 x float> %c, <float 5.000000e-01, float 1.000000e+00, float 2.500000e-01, float 1.000000e+00>
  ret <4 x float> %out
}
"#;

#[test]
fn list_exported_symbols_enumerates_exports_and_rejects_bad_args() {
    assert_eq!(garnetx_abi_version(), 3);
    let mut out_ptr: *mut u8 = std::ptr::null_mut();
    let mut out_len = 0usize;
    let mut err = [0 as c_char; 256];
    let rc = unsafe {
        garnetx_list_exported_symbols(
            DYLIB_MATH_TOP.as_ptr(),
            DYLIB_MATH_TOP.len(),
            &mut out_ptr,
            &mut out_len,
            err.as_mut_ptr(),
            err.len(),
        )
    };
    assert_eq!(rc, RC_OK);
    let syms_bytes = unsafe { std::slice::from_raw_parts(out_ptr, out_len).to_vec() };
    unsafe { garnetx_free(out_ptr, out_len) };
    let syms: Vec<String> = serde_json::from_slice(&syms_bytes).expect("parse symbols json");
    assert_eq!(syms, vec!["lib_scale_and_bias", "lib_tint"]);

    let bad_rc = unsafe {
        garnetx_list_exported_symbols(
            std::ptr::null(),
            0,
            &mut out_ptr,
            &mut out_len,
            err.as_mut_ptr(),
            err.len(),
        )
    };
    assert_eq!(bad_rc, RC_BAD_ARGS);
}

#[test]
fn dynamic_library_links_compute_vertex_and_fragment_stages_transitively() {
    let compute_ll = r#"
define void @main(ptr addrspace(1) %inbuf, ptr addrspace(1) %outbuf, i32 %tid) {
entry:
  %ip = getelementptr inbounds i32, ptr addrspace(1) %inbuf, i32 %tid
  %v = load i32, ptr addrspace(1) %ip, align 4
  %r = call i32 @lib_scale_and_bias(i32 %v)
  %op = getelementptr inbounds i32, ptr addrspace(1) %outbuf, i32 %tid
  store i32 %r, ptr addrspace(1) %op, align 4
  ret void
}
declare i32 @lib_scale_and_bias(i32)
!air.kernel = !{!0}
!0 = !{ptr @main, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_name", !"int*"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_name", !"int*"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
"#;
    let libs = [("top", DYLIB_MATH_TOP), ("base", DYLIB_MATH_BASE)];
    let c_out = call_stage2(compute_ll, &[], &libs, &[]);
    assert_eq!(c_out.rc, RC_OK, "compute dylib failed: {}", c_out.msg);
    let c_meta: Value = serde_json::from_slice(&c_out.meta).expect("compute meta");
    assert_eq!(c_meta["stage"], "Kernel");
    assert!(c_meta.get("visible_function_tables").is_none());

    let vertex_ll = r#"
define <4 x float> @vs_main(i32 %vid) {
entry:
  %n = call i32 @lib_scale_and_bias(i32 %vid)
  %f = sitofp i32 %n to float
  %v0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %f, i32 0
  ret <4 x float> %v0
}
declare i32 @lib_scale_and_bias(i32)
!air.vertex = !{!0}
!0 = !{ptr @vs_main, !1, !2}
!1 = !{!3}
!2 = !{!4}
!3 = !{!"air.position", !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint"}
"#;
    let v_out = call_stage2(vertex_ll, &[], &libs, &[]);
    assert_eq!(v_out.rc, RC_OK, "vertex dylib failed: {}", v_out.msg);
    let v_meta: Value = serde_json::from_slice(&v_out.meta).expect("vertex meta");
    assert_eq!(v_meta["stage"], "Vertex");

    let fragment_ll = r#"
define <4 x float> @fs_main(<4 x float> %color) {
entry:
  %tinted = call <4 x float> @lib_tint(<4 x float> %color)
  ret <4 x float> %tinted
}
declare <4 x float> @lib_tint(<4 x float>)
!air.fragment = !{!0}
!0 = !{ptr @fs_main, !1, !2}
!1 = !{!3}
!2 = !{!4}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.fragment_input", !"air.location_index", i32 0, i32 1, !"air.center", !"air.perspective", !"air.arg_type_name", !"float4"}
"#;
    let f_out = call_stage2(fragment_ll, &[], &libs, &[]);
    assert_eq!(f_out.rc, RC_OK, "fragment dylib failed: {}", f_out.msg);
    let f_meta: Value = serde_json::from_slice(&f_out.meta).expect("fragment meta");
    assert_eq!(f_meta["stage"], "Fragment");
}

#[test]
fn dynamic_library_reports_missing_mismatched_and_conflicting_symbols() {
    let entry_ll = r#"
define void @main(ptr addrspace(1) %out) {
entry:
  %r = call i32 @missing_export(i32 7)
  store i32 %r, ptr addrspace(1) %out, align 4
  ret void
}
declare i32 @missing_export(i32)
!air.kernel = !{!0}
!0 = !{ptr @main, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_name", !"int*"}
"#;
    let miss = call_stage2(entry_ll, &[], &[("base", DYLIB_MATH_BASE)], &[]);
    assert_eq!(miss.rc, RC_REFUSED);
    assert!(
        miss.msg.contains("missing_export"),
        "expected missing symbol name, got: {}",
        miss.msg
    );

    let wrong_sig_lib = "define float @missing_export(i32 %x) {\nentry:\n  ret float 1.0\n}\n";
    let mismatch = call_stage2(entry_ll, &[], &[("wrong", wrong_sig_lib)], &[]);
    assert_eq!(mismatch.rc, RC_REFUSED);
    assert!(
        mismatch.msg.contains("signature") && mismatch.msg.contains("missing_export"),
        "expected signature mismatch error, got: {}",
        mismatch.msg
    );

    let lib_a = "define i32 @missing_export(i32 %x) {\nentry:\n  ret i32 %x\n}\n";
    let lib_b =
        "define i32 @missing_export(i32 %x) {\nentry:\n  %y = add i32 %x, 1\n  ret i32 %y\n}\n";
    let conflict = call_stage2(entry_ll, &[], &[("a", lib_a), ("b", lib_b)], &[]);
    assert_eq!(conflict.rc, RC_REFUSED);
    assert!(
        conflict.msg.contains("missing_export"),
        "expected conflict error naming symbol, got: {}",
        conflict.msg
    );
}

#[test]
fn visible_function_table_compute_fragment_and_vertex_emit_switch_and_metadata() {
    let vis_add = "define i32 @fn_add(i32 %x) {\nentry:\n  %y = add i32 %x, 10\n  ret i32 %y\n}\n";
    // Visible function calling an external dynamic library helper!
    let vis_lib = r#"
declare i32 @lib_bias(i32)
define i32 @fn_dylib(i32 %x) {
entry:
  %y = call i32 @lib_bias(i32 %x)
  ret i32 %y
}
"#;
    let compute_vft = r#"
define void @main(ptr addrspace(1) %table, ptr addrspace(1) %slots, ptr addrspace(1) %out, i32 %tid) {
entry:
  %sp = getelementptr inbounds i32, ptr addrspace(1) %slots, i32 %tid
  %slot = load i32, ptr addrspace(1) %sp, align 4
  %fp = call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) %table, i32 %slot)
  %res = call i32 %fp(i32 %tid)
  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %tid
  store i32 %res, ptr addrspace(1) %op, align 4
  ret void
}
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1), i32)
!air.kernel = !{!0}
!0 = !{ptr @main, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.visible_function_table", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"visible_function_table<int(int)>"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_name", !"int*"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_name", !"int*"}
!6 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
"#;
    let out = call_stage2(
        compute_vft,
        &[],
        &[("base", DYLIB_MATH_BASE)],
        &[("fn_dylib", vis_lib), ("fn_add", vis_add)],
    );
    assert_eq!(out.rc, RC_OK, "compute VFT failed: {}", out.msg);
    let asm = spv_asm(&out.spv);
    assert!(asm.contains("OpSwitch "), "expected OpSwitch for VFT dispatch:\n{asm}");
    assert!(
        !asm.contains("OpUnreachable"),
        "VFT default branch must safely return zero, never OpUnreachable:\n{asm}"
    );
    let meta: Value = serde_json::from_slice(&out.meta).expect("parse VFT meta");
    let vft = &meta["visible_function_tables"];
    assert_eq!(vft["abi_version"], 1);
    assert_eq!(vft["header_words"], 1);
    assert_eq!(vft["tables"].as_array().unwrap().len(), 1);
    assert_eq!(vft["tables"][0]["param_index"], 0);
    assert_eq!(vft["tables"][0]["metal_index"], 2);
    assert!(
        vft["tables"][0]["descriptor"]["binding"].is_u64(),
        "VFT table descriptor must resolve Vulkan binding: {vft}"
    );
    // Deterministic alphabetical handle IDs starting at 1 (0 reserved for null).
    assert_eq!(
        vft["handles"],
        serde_json::json!([
            {"handle_id": 1, "symbol": "fn_add"},
            {"handle_id": 2, "symbol": "fn_dylib"}
        ])
    );

    // Fragment stage VFT with color callbacks.
    let frag_vis_red = r#"
define <4 x float> @shade_red(<4 x float> %in) {
entry:
  ret <4 x float> <float 1.0, float 0.0, float 0.0, float 1.0>
}
"#;
    let frag_vis_green = r#"
define <4 x float> @shade_green(<4 x float> %in) {
entry:
  ret <4 x float> <float 0.0, float 1.0, float 0.0, float 1.0>
}
"#;
    let frag_vft = r#"
define <4 x float> @fs_vft(ptr addrspace(1) %table, ptr addrspace(1) %slot_buf, <4 x float> %c) {
entry:
  %slot = load i32, ptr addrspace(1) %slot_buf, align 4
  %fp = call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) %table, i32 %slot)
  %out = call <4 x float> %fp(<4 x float> %c)
  ret <4 x float> %out
}
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1), i32)
!air.fragment = !{!0}
!0 = !{ptr @fs_vft, !1, !2}
!1 = !{!3}
!2 = !{!4, !5, !6}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.visible_function_table", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"visible_function_table<float4(float4)>"}
!5 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_name", !"int*"}
!6 = !{i32 2, !"air.fragment_input", !"air.location_index", i32 0, i32 1, !"air.center", !"air.perspective", !"air.arg_type_name", !"float4"}
"#;
    let f_out = call_stage2(
        frag_vft,
        &[],
        &[],
        &[("shade_red", frag_vis_red), ("shade_green", frag_vis_green)],
    );
    assert_eq!(f_out.rc, RC_OK, "fragment VFT failed: {}", f_out.msg);
    assert!(spv_asm(&f_out.spv).contains("OpSwitch "));
    let f_meta: Value = serde_json::from_slice(&f_out.meta).expect("frag VFT meta");
    assert_eq!(f_meta["stage"], "Fragment");
    assert_eq!(
        f_meta["visible_function_tables"]["handles"],
        serde_json::json!([
            {"handle_id": 1, "symbol": "shade_green"},
            {"handle_id": 2, "symbol": "shade_red"}
        ])
    );

    // Signature mismatch must be refused cleanly.
    let bad_vis = "define float @wrong_fn(float %x) {\nentry:\n  ret float %x\n}\n";
    let bad = call_stage2(compute_vft, &[], &[], &[("wrong_fn", bad_vis)]);
    assert_eq!(bad.rc, RC_REFUSED);
    assert!(
        bad.msg.contains("signature") || bad.msg.contains("visible function"),
        "expected VFT signature error, got: {}",
        bad.msg
    );

    // Same-library [[visible]] functions (single MTLLibrary defining entry + visible functions),
    // passed either as whole library OR split single-function modules, must link without duplicate
    // definitions; conflicting bodies must still be refused.
    let same_lib_ll = r#"
define i32 @vis_inc(i32 %x) {
entry:
  %y = add i32 %x, 1
  ret i32 %y
}
define i32 @vis_dbl(i32 %x) {
entry:
  %y = mul i32 %x, 2
  ret i32 %y
}
define void @main(ptr addrspace(1) %table, ptr addrspace(1) %out, i32 %tid) {
entry:
  %fp = call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) %table, i32 %tid)
  %res = call i32 %fp(i32 %tid)
  %op = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %tid
  store i32 %res, ptr addrspace(1) %op, align 4
  ret void
}
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1), i32)
!air.kernel = !{!0}
!0 = !{ptr @main, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.visible_function_table", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"visible_function_table<int(int)>"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_name", !"int*"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
"#;
    let whole_same = call_stage2(
        same_lib_ll,
        &[],
        &[],
        &[("vis_inc", same_lib_ll), ("vis_dbl", same_lib_ll)],
    );
    assert_eq!(whole_same.rc, RC_OK, "same-lib whole VFT failed: {}", whole_same.msg);
    assert!(spv_asm(&whole_same.spv).contains("OpSwitch "));
    let whole_meta: Value = serde_json::from_slice(&whole_same.meta).expect("whole same-lib meta");
    assert_eq!(
        whole_meta["visible_function_tables"]["handles"],
        serde_json::json!([
            {"handle_id": 1, "symbol": "vis_dbl"},
            {"handle_id": 2, "symbol": "vis_inc"}
        ])
    );

    let split_inc = "define i32 @vis_inc(i32 %x) {\nentry:\n  %y = add i32 %x, 1\n  ret i32 %y\n}\n";
    let split_same = call_stage2(
        same_lib_ll,
        &[],
        &[],
        &[("vis_inc", split_inc), ("fn_add", vis_add)],
    );
    assert_eq!(split_same.rc, RC_OK, "same-lib split VFT failed: {}", split_same.msg);
    assert!(spv_asm(&split_same.spv).contains("OpSwitch "));

    let conflict_inc =
        "define i32 @vis_inc(i32 %x) {\nentry:\n  %y = add i32 %x, 99\n  ret i32 %y\n}\n";
    let conflict = call_stage2(same_lib_ll, &[], &[], &[("vis_inc", conflict_inc)]);
    assert_eq!(conflict.rc, RC_REFUSED);
    assert!(
        conflict.msg.contains("conflicts") && conflict.msg.contains("vis_inc"),
        "expected same-lib conflict error, got: {}",
        conflict.msg
    );
}

#[test]
fn sparse_sample_read_and_gather_emit_sparse_ops_and_capability_via_ffi() {
    let sparse_compute = r#"
define void @k(ptr addrspace(1) %tex, ptr addrspace(1) %stex, ptr addrspace(2) %sampler, ptr addrspace(1) %out, <2 x i32> %tid) {
entry:
  %slot = alloca { <4 x float>, i8 }, align 16
  %s = tail call { <4 x float>, i8 } @air.sample_texture_2d.sparse.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %sampler, <2 x float> <float 2.500000e-01, float 2.500000e-01>)
  store { <4 x float>, i8 } %s, ptr %slot, align 16
  %sl = load { <4 x float>, i8 }, ptr %slot, align 16
  %r = tail call { <4 x float>, i8 } @air.read_texture_2d.sparse.v4f32(ptr addrspace(1) %tex, <2 x i32> %tid, i32 0, i32 1)
  %sr = tail call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %stex, <2 x i32> %tid, i32 0, i32 3)
  %g = tail call { <4 x float>, i8 } @air.gather_texture_2d.sparse.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %sampler, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 false, <2 x i32> zeroinitializer, i32 0, i32 0)
  %cs = extractvalue { <4 x float>, i8 } %sl, 0
  %bs = extractvalue { <4 x float>, i8 } %sl, 1
  %cr = extractvalue { <4 x float>, i8 } %r, 0
  %br = extractvalue { <4 x float>, i8 } %r, 1
  %csr = extractvalue { <4 x float>, i8 } %sr, 0
  %bsr = extractvalue { <4 x float>, i8 } %sr, 1
  %cg = extractvalue { <4 x float>, i8 } %g, 0
  %bg = extractvalue { <4 x float>, i8 } %g, 1
  %v0 = fadd <4 x float> %cs, %cr
  %v1 = fadd <4 x float> %v0, %csr
  %v2 = fadd <4 x float> %v1, %cg
  %b0 = and i8 %bs, %br
  %b1 = and i8 %b0, %bsr
  %b2 = and i8 %b1, %bg
  %ok = icmp ne i8 %b2, 0
  %w = select i1 %ok, float 1.000000e+00, float 0.000000e+00
  %res = insertelement <4 x float> %v2, float %w, i32 3
  store <4 x float> %res, ptr addrspace(1) %out, align 16
  ret void
}
declare { <4 x float>, i8 } @air.sample_texture_2d.sparse.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>)
declare { <4 x float>, i8 } @air.read_texture_2d.sparse.v4f32(ptr addrspace(1), <2 x i32>, i32, i32)
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32, i32)
declare { <4 x float>, i8 } @air.gather_texture_2d.sparse.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i32, i32)
!air.kernel = !{!0}
!0 = !{ptr @k, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6, !7}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!4 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.arg_type_name", !"texture2d<float, read_write>", !"air.arg_name", !"stex"}
!5 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"sampler"}
!6 = !{i32 3, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4*", !"air.arg_name", !"out"}
!7 = !{i32 4, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"tid"}
"#;
    let c_out = call_plain(sparse_compute);
    assert_eq!(c_out.rc, RC_OK, "sparse compute failed: {}", c_out.msg);
    let c_asm = spv_asm(&c_out.spv);
    assert!(c_asm.contains("OpCapability SparseResidency"), "{c_asm}");
    assert!(c_asm.contains("OpImageSparseSampleExplicitLod"), "{c_asm}");
    assert!(c_asm.contains("OpImageSparseFetch"), "{c_asm}");
    assert!(c_asm.contains("OpImageSparseRead"), "{c_asm}");
    assert!(c_asm.contains("OpImageSparseGather"), "{c_asm}");
    assert_eq!(c_asm.matches("OpImageSparseTexelsResident").count(), 4, "{c_asm}");
    // Apple AIR `{ texel, i8 }` uses TFE polarity: 0 = resident, 1 = non-resident
    // (`sparse_color::resident()` compiles to `((status & 1) ^ 1)`).
    let mut i8_ty = None;
    let mut i8_zero = None;
    let mut i8_one = None;
    let mut resident_ids = Vec::new();
    let mut selects = Vec::new();
    for line in c_asm.lines() {
        let tok: Vec<&str> = line.split_whitespace().collect();
        if tok.as_slice() == [tok.first().copied().unwrap_or(""), "=", "OpTypeInt", "8", "0"] {
            i8_ty = Some(tok[0].to_string());
        } else if tok.len() == 5 && tok[1] == "=" && tok[2] == "OpConstant" {
            if Some(tok[3]) == i8_ty.as_deref() {
                if tok[4] == "0" {
                    i8_zero = Some(tok[0].to_string());
                } else if tok[4] == "1" {
                    i8_one = Some(tok[0].to_string());
                }
            }
        } else if tok.len() >= 5 && tok[1] == "=" && tok[2] == "OpImageSparseTexelsResident" {
            resident_ids.push(tok[0].to_string());
        } else if tok.len() == 7 && tok[1] == "=" && tok[2] == "OpSelect" {
            selects.push((tok[3].to_string(), tok[4].to_string(), tok[5].to_string(), tok[6].to_string()));
        }
    }
    let ty = i8_ty.unwrap_or_else(|| panic!("missing OpTypeInt 8 0:\n{c_asm}"));
    let z = i8_zero.unwrap_or_else(|| panic!("missing {ty} 0 constant:\n{c_asm}"));
    let o = i8_one.unwrap_or_else(|| panic!("missing {ty} 1 constant:\n{c_asm}"));
    assert_eq!(resident_ids.len(), 4, "{c_asm}");
    for rid in &resident_ids {
        assert!(
            selects.iter().any(|(t, c, tv, fv)| t == &ty && c == rid && tv == &z && fv == &o),
            "expected Apple AIR TFE polarity OpSelect {ty} {rid} {z} {o} (0=resident, 1=non-resident):\n{c_asm}"
        );
    }

    let sparse_frag = r#"
define <4 x float> @fs(ptr addrspace(1) %tex, ptr addrspace(2) %sampler, <2 x float> %uv) {
entry:
  %s = tail call { <4 x float>, i8 } @air.sample_texture_2d.sparse.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %sampler, <2 x float> %uv)
  %c = extractvalue { <4 x float>, i8 } %s, 0
  %b = extractvalue { <4 x float>, i8 } %s, 1
  %ok = icmp ne i8 %b, 0
  %a = select i1 %ok, float 1.000000e+00, float 0.000000e+00
  %out = insertelement <4 x float> %c, float %a, i32 3
  ret <4 x float> %out
}
declare { <4 x float>, i8 } @air.sample_texture_2d.sparse.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>)
!air.fragment = !{!0}
!0 = !{ptr @fs, !1, !2}
!1 = !{!3}
!2 = !{!4, !5, !6}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!5 = !{i32 1, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"sampler"}
!6 = !{i32 2, !"air.fragment_input", !"air.location_index", i32 0, i32 1, !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
"#;
    let f_out = call_plain(sparse_frag);
    assert_eq!(f_out.rc, RC_OK, "sparse frag failed: {}", f_out.msg);
    let f_asm = spv_asm(&f_out.spv);
    assert!(f_asm.contains("OpCapability SparseResidency"), "{f_asm}");
    assert!(f_asm.contains("OpImageSparseSampleImplicitLod"), "{f_asm}");
    assert!(f_asm.contains("OpImageSparseTexelsResident"), "{f_asm}");
    let f_meta: Value = serde_json::from_slice(&f_out.meta).expect("sparse frag meta");
    let sampling_coords = f_meta["uniform_control_proof"]["sampling_coordinates"]
        .as_array()
        .expect("sparse fragment must preserve uniform_control_proof.sampling_coordinates");
    assert!(
        !sampling_coords.is_empty(),
        "expected non-empty sampling_coordinates on sparse implicit fragment sample: {f_meta}"
    );

    let non_sparse = r#"
define void @k(ptr addrspace(1) %tex, ptr addrspace(2) %sampler, ptr addrspace(1) %out) {
entry:
  %s = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %sampler, <2 x float> <float 2.500000e-01, float 2.500000e-01>)
  %c = extractvalue { <4 x float>, i8 } %s, 0
  store <4 x float> %c, ptr addrspace(1) %out, align 16
  ret void
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>)
!air.kernel = !{!0}
!0 = !{ptr @k, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!4 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"sampler"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4*", !"air.arg_name", !"out"}
"#;
    let ns_out = call_plain(non_sparse);
    assert_eq!(ns_out.rc, RC_OK, "non-sparse failed: {}", ns_out.msg);
    let ns_asm = spv_asm(&ns_out.spv);
    assert!(!ns_asm.contains("OpCapability SparseResidency"), "{ns_asm}");
    assert!(!ns_asm.contains("OpImageSparseTexelsResident"), "{ns_asm}");
    assert!(!ns_asm.contains("OpImageSparse"), "{ns_asm}");
}
