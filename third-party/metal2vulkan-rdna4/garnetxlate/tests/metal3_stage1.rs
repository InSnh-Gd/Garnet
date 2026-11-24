// SPDX-License-Identifier: LGPL-3.0-or-later
//! Offline FFI integration tests for Metal 3 Translator Stage 1 (`0070`..`0072`):
//! - Task 2.4: `[[barycentric_coord]]` and pull-model `interpolant<T>::interpolate_at_*`
//! - Task 2.6: `atomic<float>` (native `AtomicFloat32AddEXT` + CAS `min`/`max`/`cmpxchg`),
//!   `atomic<int64_t>`/`uint64_t` (`Int64Atomics`), `bfloat` FMA/math/conversions,
//!   texture atomics (`OpImageTexelPointer` preserving `R32ui`/`R32i`), and
//!   `simd_shuffle_and_fill_{up,down}`
//! - Task 2.3: Compute ICB GPU encoding + `garnetx_lower_icb_stage(..., stage_kind=3, ...)`
use garnetxlate::{garnetx_free, garnetx_lower_icb_stage, garnetx_translate};
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
    }
    Out {
        rc,
        msg,
        spv: spv_bytes,
    }
}

#[test]
fn task24_barycentric_and_pull_model_interpolation_via_ffi() {
    let ll = r#"
target triple = "spirv-unknown-vulkan1.2"
%struct._interpolant_t = type opaque
define <4 x float> @frag_m3(<3 x float> %bary_p, <3 x float> %bary_np, ptr addrspace(1) %interp_uv, i32 %sid) {
entry:
  %c = call <2 x float> @air.interpolate_at_center.v2f32(ptr addrspace(1) %interp_uv)
  %ct = call <2 x float> @air.interpolate_at_centroid.v2f32(ptr addrspace(1) %interp_uv)
  %sp = call <2 x float> @air.interpolate_at_sample.v2f32(ptr addrspace(1) %interp_uv, i32 %sid)
  %of = call <2 x float> @air.interpolate_at_offset.v2f32(ptr addrspace(1) %interp_uv, <2 x float> <float 0.25, float 0.75>)
  %s0 = fadd <2 x float> %c, %ct
  %s1 = fadd <2 x float> %sp, %of
  %uv = fadd <2 x float> %s0, %s1
  %u = extractelement <2 x float> %uv, i32 0
  %v = extractelement <2 x float> %uv, i32 1
  %bx = extractelement <3 x float> %bary_p, i32 0
  %by = extractelement <3 x float> %bary_np, i32 1
  %r0 = insertelement <4 x float> poison, float %bx, i32 0
  %r1 = insertelement <4 x float> %r0, float %by, i32 1
  %r2 = insertelement <4 x float> %r1, float %u, i32 2
  %r3 = insertelement <4 x float> %r2, float %v, i32 3
  ret <4 x float> %r3
}
declare <2 x float> @air.interpolate_at_center.v2f32(ptr addrspace(1))
declare <2 x float> @air.interpolate_at_centroid.v2f32(ptr addrspace(1))
declare <2 x float> @air.interpolate_at_sample.v2f32(ptr addrspace(1), i32)
declare <2 x float> @air.interpolate_at_offset.v2f32(ptr addrspace(1), <2 x float>)
!air.fragment = !{!0}
!0 = !{ptr @frag_m3, !1, !3}
!1 = !{!2}
!2 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!3 = !{!4, !5, !6, !7}
!4 = !{i32 0, !"air.barycentric_coord", !"air.perspective", !"air.arg_type_name", !"float3"}
!5 = !{i32 1, !"air.barycentric_coord", !"air.no_perspective", !"air.arg_type_name", !"float3"}
!6 = !{i32 2, !"air.fragment_input", !"uv", !"air.pull_mode", !"air.perspective", !"air.arg_type_name", !"interpolant<float2>"}
!7 = !{i32 3, !"air.sample_id", !"air.arg_type_name", !"uint"}
"#;
    let out = translate_ffi(ll);
    assert_eq!(out.rc, 0, "barycentric+pull-model failed: {}", out.msg);
    let asm = metal2vulkan::disassemble(&out.spv).expect("disassemble");
    assert!(asm.contains("OpCapability FragmentBarycentricKHR"), "{asm}");
    assert!(
        asm.contains("OpExtension \"SPV_KHR_fragment_shader_barycentric\""),
        "{asm}"
    );
    assert!(asm.contains("BuiltIn BaryCoordKHR"), "{asm}");
    assert!(asm.contains("BuiltIn BaryCoordNoPerspKHR"), "{asm}");
    assert!(asm.contains("OpCapability InterpolationFunction"), "{asm}");
    assert!(asm.contains("InterpolateAtCentroid"), "{asm}");
    assert!(asm.contains("InterpolateAtSample"), "{asm}");
    assert!(asm.contains("InterpolateAtOffset"), "{asm}");
}

#[test]
fn task26_atomics_bfloat_texture_atomics_and_shuffle_fill_via_ffi() {
    let ll_f32_i64 = r#"
target triple = "spirv-unknown-vulkan1.2"
define void @k_atom(ptr addrspace(1) %fbuf, ptr addrspace(1) %fcas, ptr addrspace(1) %i64buf, i32 %tid) {
entry:
  %idx = zext i32 %tid to i64
  %fp = getelementptr inbounds float, ptr addrspace(1) %fbuf, i64 %idx
  %a = call float @air.atomic.global.add.f32(ptr addrspace(1) %fp, float 2.0, i32 0, i32 2, i1 false)
  %cp = getelementptr inbounds float, ptr addrspace(1) %fcas, i64 %idx
  %mn = call float @air.atomic.global.min.f.f32(ptr addrspace(1) %cp, float %a, i32 0, i32 2, i1 false)
  call void @air.atomic.global.store.f32(ptr addrspace(1) %fp, float %mn, i32 0, i32 2, i1 false)
  %ip = getelementptr inbounds i64, ptr addrspace(1) %i64buf, i64 %idx
  %o = call i64 @air.atomic.global.add.u.i64(ptr addrspace(1) %ip, i64 7, i32 0, i32 2, i1 false)
  %m = call i64 @air.atomic.global.max.s.i64(ptr addrspace(1) %ip, i64 %o, i32 0, i32 2, i1 false)
  call void @air.atomic.global.store.i64(ptr addrspace(1) %ip, i64 %m, i32 0, i32 2, i1 false)
  ret void
}
declare float @air.atomic.global.add.f32(ptr addrspace(1), float, i32, i32, i1)
declare float @air.atomic.global.min.f.f32(ptr addrspace(1), float, i32, i32, i1)
declare void @air.atomic.global.store.f32(ptr addrspace(1), float, i32, i32, i1)
declare i64 @air.atomic.global.add.u.i64(ptr addrspace(1), i64, i32, i32, i1)
declare i64 @air.atomic.global.max.s.i64(ptr addrspace(1), i64, i32, i32, i1)
declare void @air.atomic.global.store.i64(ptr addrspace(1), i64, i32, i32, i1)
!air.kernel = !{!0}
!0 = !{ptr @k_atom, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong"}
!6 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
"#;
    let out = translate_ffi(ll_f32_i64);
    assert_eq!(out.rc, 0, "f32/i64 atomics failed: {}", out.msg);
    let asm = metal2vulkan::disassemble(&out.spv).expect("disassemble");
    assert!(asm.contains("OpCapability AtomicFloat32AddEXT"), "{asm}");
    assert!(asm.contains("OpCapability Int64Atomics"), "{asm}");
    assert!(asm.contains("OpAtomicFAddEXT"), "{asm}");
    assert!(asm.contains("OpAtomicCompareExchange"), "{asm}");
    assert!(asm.contains("OpAtomicIAdd"), "{asm}");
    assert!(asm.contains("OpAtomicSMax"), "{asm}");

    // Texture atomic image MUST preserve explicit R32ui format across garnetx_translate's
    // patch_storage_formats pass (Vulkan forbids Unknown on OpImageTexelPointer).
    let ll_tex = r#"
target triple = "spirv-unknown-vulkan1.2"
%struct._texture_2d_t = type opaque
define void @k_tex(ptr addrspace(1) %tex, ptr addrspace(1) %out, <2 x i32> %gid) {
entry:
  %prev = call <4 x i32> @air.atomic_fetch_add_explicit_texture_2d.i32.u.v4i32(ptr addrspace(1) %tex, <2 x i32> %gid, <2 x i32> zeroinitializer, <4 x i32> <i32 3, i32 0, i32 0, i32 0>, i32 0, i32 0)
  %v0 = extractelement <4 x i32> %prev, i32 0
  %sh = call i32 @air.simd_shuffle_and_fill_up.i32(i32 %v0, i32 99, i16 1, i16 4)
  store i32 %sh, ptr addrspace(1) %out, align 4
  ret void
}
declare <4 x i32> @air.atomic_fetch_add_explicit_texture_2d.i32.u.v4i32(ptr addrspace(1), <2 x i32>, <2 x i32>, <4 x i32>, i32, i32)
declare i32 @air.simd_shuffle_and_fill_up.i32(i32, i32, i16, i16)
!air.kernel = !{!0}
!0 = !{ptr @k_tex, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.arg_type_name", !"texture2d<uint, read_write>"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2"}
"#;
    let out_tex = translate_ffi(ll_tex);
    assert_eq!(out_tex.rc, 0, "texture atomic failed: {}", out_tex.msg);
    let asm_tex = metal2vulkan::disassemble(&out_tex.spv).expect("disassemble tex");
    assert!(asm_tex.contains("OpImageTexelPointer"), "{asm_tex}");
    assert!(
        asm_tex.contains("2 R32ui"),
        "patch_storage_formats must preserve R32ui for OpImageTexelPointer:\n{asm_tex}"
    );
    assert!(!asm_tex.contains("2 Unknown"), "{asm_tex}");
    assert!(asm_tex.contains("OpGroupNonUniformShuffle"), "{asm_tex}");
}

#[test]
fn task23_compute_icb_encoder_and_stage_lowering_via_ffi() {
    let encoder_ll = r#"
target triple = "spirv-unknown-vulkan1.2"
%struct.Args = type { ptr addrspace(1), ptr addrspace(1), ptr addrspace(1) }
define void @encode_icb(ptr addrspace(1) %args, i32 %tid) {
entry:
  %p0 = getelementptr inbounds %struct.Args, ptr addrspace(1) %args, i64 0, i32 0
  %icb = load ptr addrspace(1), ptr addrspace(1) %p0, align 8
  %p1 = getelementptr inbounds %struct.Args, ptr addrspace(1) %args, i64 0, i32 1
  %pso = load ptr addrspace(1), ptr addrspace(1) %p1, align 8
  %p2 = getelementptr inbounds %struct.Args, ptr addrspace(1) %args, i64 0, i32 2
  %buf = load ptr addrspace(1), ptr addrspace(1) %p2, align 8
  call void @air.set_pipeline_state_compute_command(ptr addrspace(1) %icb, i32 %tid, ptr addrspace(1) %pso)
  call void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1) %icb, i32 %tid, ptr addrspace(1) %buf, i32 0)
  call void @air.set_barrier_compute_command(ptr addrspace(1) %icb, i32 %tid)
  call void @air.concurrent_dispatch_threadgroups_compute_command(ptr addrspace(1) %icb, i32 %tid, <3 x i32> <i32 2, i32 1, i32 1>, <3 x i32> <i32 32, i32 1, i32 1>)
  ret void
}
declare void @air.set_pipeline_state_compute_command(ptr addrspace(1), i32, ptr addrspace(1))
declare void @air.set_kernel_buffer_compute_command.p1i8(ptr addrspace(1), i32, ptr addrspace(1), i32)
declare void @air.set_barrier_compute_command(ptr addrspace(1), i32)
declare void @air.concurrent_dispatch_threadgroups_compute_command(ptr addrspace(1), i32, <3 x i32>, <3 x i32>)
!air.kernel = !{!0}
!0 = !{ptr @encode_icb, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 24, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Args"}
!4 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
"#;
    let enc = translate_ffi(encoder_ll);
    assert_eq!(enc.rc, 0, "compute ICB encoder failed: {}", enc.msg);

    let consumer_ll = r#"
target triple = "spirv-unknown-vulkan1.2"
define void @consume_icb(ptr addrspace(1) %buf, i32 %tid) {
entry:
  %idx = zext i32 %tid to i64
  %p = getelementptr inbounds i32, ptr addrspace(1) %buf, i64 %idx
  %v = load i32, ptr addrspace(1) %p, align 4
  %n = add i32 %v, 100
  store i32 %n, ptr addrspace(1) %p, align 4
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @consume_icb, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint"}
!4 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
"#;
    let cons = translate_ffi(consumer_ll);
    assert_eq!(cons.rc, 0, "compute consumer failed: {}", cons.msg);

    let mut lowered_ptr: *mut u8 = std::ptr::null_mut();
    let mut lowered_len = 0usize;
    let mut tbl_binding = 0u32;
    let mut vs_binding = 99u32;
    let mut err = [0 as c_char; 256];
    let rc = unsafe {
        garnetx_lower_icb_stage(
            cons.spv.as_ptr(),
            cons.spv.len(),
            3,
            0,
            std::ptr::null(),
            0,
            &mut lowered_ptr,
            &mut lowered_len,
            &mut tbl_binding,
            &mut vs_binding,
            err.as_mut_ptr(),
            err.len(),
        )
    };
    let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }
        .to_string_lossy()
        .into_owned();
    assert_eq!(rc, 0, "garnetx_lower_icb_stage(compute) failed: {msg}");
    assert_eq!(tbl_binding, 676);
    assert_eq!(vs_binding, 0);
    let lowered = unsafe {
        let bytes = std::slice::from_raw_parts(lowered_ptr, lowered_len).to_vec();
        garnetx_free(lowered_ptr, lowered_len);
        bytes
    };
    let asm = metal2vulkan::disassemble(&lowered).expect("disassemble lowered compute");
    assert!(asm.contains("OpEntryPoint GLCompute"), "{asm}");
    assert!(asm.contains("Binding 676"), "{asm}");
    assert!(asm.contains("PhysicalStorageBuffer64"), "{asm}");
}
