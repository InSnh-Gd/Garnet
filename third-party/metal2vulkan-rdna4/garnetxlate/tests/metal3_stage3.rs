// SPDX-License-Identifier: LGPL-3.0-or-later
//! Translator Stage 3 integration tests via garnetxlate C FFI:
//! - Task 6.1: Object & Mesh shaders (`SPV_EXT_mesh_shader`, >=16 KiB `TaskPayloadWorkgroupEXT`,
//!   Point/Line/Triangle topologies, `PerPrimitiveEXT`, `CullPrimitiveEXT`, `Layer`, `ViewportIndex`,
//!   and paired fragment `PerPrimitiveEXT` inputs)
//! - Task 7.2: Hardware Ray Query (`SPV_KHR_ray_query`, Descriptor & 64-bit BDA acceleration
//!   structures across Compute/Vertex/Fragment, and runtime `MTLIntersectionFunctionTable` bounded
//!   `OpSwitch` dispatch for custom bounding-box and triangle any-hit callbacks)

use garnetxlate::{
    garnetx_abi_version, garnetx_detect_stage, garnetx_free, garnetx_translate,
    garnetx_translate_linked_libraries, garnetx_translate_stage3,
    garnetx_translate_with_capabilities, RDNA4xBackendCapabilities, RDNA4xDep,
    RDNA4xDynamicLibrary, RDNA4xIntersectionFunction, GARNETX_CAP_MESH_EMULATED_PAYLOAD,
    GARNETX_CAP_RT_IFT_ABI_V2, GARNETX_CAP_RT_POSITION_FETCH, GARNETX_IFT_KIND_BOUNDING_BOX,
    GARNETX_IFT_KIND_TRIANGLE_ANY_HIT,
    GARNETX_MESH_GRID_PARAMS_BINDING, GARNETX_MESH_INDIRECT_BINDING, GARNETX_MESH_PAYLOAD_BINDING,
    GARNETX_STAGE_FRAGMENT, GARNETX_STAGE_KERNEL, GARNETX_STAGE_MESH, GARNETX_STAGE_OBJECT,
    GARNETX_STAGE_VERTEX, RC_BAD_ARGS, RC_OK, RC_REFUSED,
};
use serde_json::Value;
use std::os::raw::c_char;

struct Out {
    rc: i32,
    msg: String,
    spv: Vec<u8>,
    meta: Vec<u8>,
}

fn call_plain(ll: &str, local: Option<[u32; 3]>) -> Out {
    let mut spv: *mut u8 = std::ptr::null_mut();
    let mut spv_len = 0usize;
    let mut meta: *mut u8 = std::ptr::null_mut();
    let mut meta_len = 0usize;
    let mut err = [0 as c_char; 512];
    let local_ptr = local.as_ref().map_or(std::ptr::null(), |l| l.as_ptr());
    let rc = unsafe {
        garnetx_translate(
            ll.as_ptr(),
            ll.len(),
            local_ptr,
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

fn call_stage3(
    ll: &str,
    local: Option<[u32; 3]>,
    deps: &[(&str, &str)],
    dynlibs: &[(&str, &str)],
    vis_fns: &[(&str, &str)],
    ift_fns: &[(&str, u32, &str)],
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
    let rc = unsafe {
        garnetx_translate_stage3(
            ll.as_ptr(),
            ll.len(),
            local_ptr,
            c_deps.as_ptr(),
            c_deps.len(),
            c_libs.as_ptr(),
            c_libs.len(),
            c_vis.as_ptr(),
            c_vis.len(),
            c_ift.as_ptr(),
            c_ift.len(),
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

fn detect_stage(ll: &str) -> (i32, u32) {
    let mut stage = u32::MAX;
    let mut err = [0 as c_char; 256];
    let rc = unsafe {
        garnetx_detect_stage(
            ll.as_ptr(),
            ll.len(),
            &mut stage,
            err.as_mut_ptr(),
            err.len(),
        )
    };
    (rc, stage)
}

fn spv_asm(spv: &[u8]) -> String {
    metal2vulkan::disassemble(spv).expect("disassemble SPIR-V")
}

const OBJECT_16K_LL: &str = r#"
%struct.Payload16K = type { [4096 x i32] }

define void @obj_main(ptr addrspace(7) %payload, ptr addrspace(1) %props, i32 %tid) {
entry:
  %p0 = getelementptr inbounds %struct.Payload16K, ptr addrspace(7) %payload, i32 0, i32 0, i32 0
  store i32 %tid, ptr addrspace(7) %p0, align 4
  %plast = getelementptr inbounds %struct.Payload16K, ptr addrspace(7) %payload, i32 0, i32 0, i32 4095
  store i32 305419896, ptr addrspace(7) %plast, align 4
  call void @air.set_mesh_properties(ptr addrspace(1) %props, <3 x i32> <i32 2, i32 1, i32 1>)
  ret void
}

declare void @air.set_mesh_properties(ptr addrspace(1), <3 x i32>)

!air.object = !{!0}
!0 = !{ptr @obj_main, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.payload", !"air.arg_type_size", i32 16384, !"air.arg_type_align_size", i32 4}
!4 = !{i32 1, !"air.mesh_grid_properties"}
!5 = !{i32 2, !"air.thread_index_in_threadgroup"}
"#;

const MESH_TRI_PAYLOAD_LL: &str = r#"
%struct.Payload16K = type { [4096 x i32] }
%struct.VertOut = type { <4 x float>, <4 x float> }
%struct.PrimOut = type { <4 x float>, i1, i32, i32 }

define void @mesh_main(ptr addrspace(1) %mesh, ptr addrspace(7) %payload, i32 %tid) {
entry:
  %plast = getelementptr inbounds %struct.Payload16K, ptr addrspace(7) %payload, i32 0, i32 0, i32 4095
  %tag = load i32, ptr addrspace(7) %plast, align 4
  %tag_f = sitofp i32 %tag to float
  %pos = insertelement <4 x float> <float 0.0, float 0.5, float 0.0, float 1.0>, float %tag_f, i32 0
  %v0 = insertvalue %struct.VertOut poison, <4 x float> %pos, 0
  %v1 = insertvalue %struct.VertOut %v0, <4 x float> <float 1.0, float 0.0, float 0.0, float 1.0>, 1
  call void @air.set_mesh_vertex(ptr addrspace(1) %mesh, i32 %tid, %struct.VertOut %v1)
  %p0 = insertvalue %struct.PrimOut poison, <4 x float> <float 0.0, float 1.0, float 0.0, float 1.0>, 0
  %p1 = insertvalue %struct.PrimOut %p0, i1 false, 1
  %p2 = insertvalue %struct.PrimOut %p1, i32 2, 2
  %p3 = insertvalue %struct.PrimOut %p2, i32 1, 3
  call void @air.set_mesh_primitive(ptr addrspace(1) %mesh, i32 0, %struct.PrimOut %p3)
  call void @air.set_mesh_index(ptr addrspace(1) %mesh, i32 0, i32 0)
  call void @air.set_mesh_index(ptr addrspace(1) %mesh, i32 1, i32 1)
  call void @air.set_mesh_index(ptr addrspace(1) %mesh, i32 2, i32 2)
  call void @air.set_mesh_primitive_count(ptr addrspace(1) %mesh, i32 1)
  ret void
}

declare void @air.set_mesh_vertex(ptr addrspace(1), i32, %struct.VertOut)
declare void @air.set_mesh_primitive(ptr addrspace(1), i32, %struct.PrimOut)
declare void @air.set_mesh_index(ptr addrspace(1), i32, i32)
declare void @air.set_mesh_primitive_count(ptr addrspace(1), i32)

!air.mesh = !{!0}
!0 = !{ptr @mesh_main, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.mesh", !"air.mesh_topology", !"triangle", !"air.max_vertices", i32 3, !"air.max_primitives", i32 1, !"air.vertex_output", !6, !"air.primitive_output", !7}
!4 = !{i32 1, !"air.payload", !"air.arg_type_size", i32 16384, !"air.arg_type_align_size", i32 4}
!5 = !{i32 2, !"air.thread_index_in_threadgroup"}
!6 = !{!8, !9}
!7 = !{!10, !11, !12, !13}
!8 = !{!"air.position", !"air.arg_type_name", !"float4"}
!9 = !{!"air.vertex_output", !"air.location_index", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"vcolor"}
!10 = !{!"air.primitive_output", !"air.location_index", i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"pcolor"}
!11 = !{!"air.primitive_culled", !"air.arg_type_name", !"bool"}
!12 = !{!"air.render_target_array_index", !"air.arg_type_name", !"uint"}
!13 = !{!"air.viewport_array_index", !"air.arg_type_name", !"uint"}
"#;

const FRAG_PER_PRIM_LL: &str = r#"
define <4 x float> @frag_main(<4 x float> %vcol, <4 x float> %pcol) {
entry:
  %sum = fadd <4 x float> %vcol, %pcol
  ret <4 x float> %sum
}

!air.fragment = !{!0}
!0 = !{ptr @frag_main, !1, !2}
!1 = !{!5}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.fragment_input", !"air.location_index", i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 1, !"air.fragment_input", !"air.location_index", i32 1, !"air.per_primitive", !"air.arg_type_name", !"float4"}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
"#;

const MESH_POINT_LL: &str = r#"
%struct.V = type { <4 x float> }
define void @mesh_pt(ptr addrspace(1) %mesh) {
entry:
  %v0 = insertvalue %struct.V poison, <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, 0
  call void @air.set_mesh_vertex(ptr addrspace(1) %mesh, i32 0, %struct.V %v0)
  call void @air.set_mesh_index(ptr addrspace(1) %mesh, i32 0, i32 0)
  call void @air.set_mesh_primitive_count(ptr addrspace(1) %mesh, i32 1)
  ret void
}
declare void @air.set_mesh_vertex(ptr addrspace(1), i32, %struct.V)
declare void @air.set_mesh_index(ptr addrspace(1), i32, i32)
declare void @air.set_mesh_primitive_count(ptr addrspace(1), i32)
!air.mesh = !{!0}
!0 = !{ptr @mesh_pt, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.mesh", !"air.mesh_topology", !"point", !"air.max_vertices", i32 4, !"air.max_primitives", i32 4, !"air.vertex_output", !4}
!4 = !{!5}
!5 = !{!"air.position", !"air.arg_type_name", !"float4"}
"#;

const MESH_LINE_LL: &str = r#"
%struct.V = type { <4 x float> }
define void @mesh_ln(ptr addrspace(1) %mesh) {
entry:
  %v0 = insertvalue %struct.V poison, <4 x float> <float -0.5, float 0.0, float 0.0, float 1.0>, 0
  %v1 = insertvalue %struct.V poison, <4 x float> <float 0.5, float 0.0, float 0.0, float 1.0>, 0
  call void @air.set_mesh_vertex(ptr addrspace(1) %mesh, i32 0, %struct.V %v0)
  call void @air.set_mesh_vertex(ptr addrspace(1) %mesh, i32 1, %struct.V %v1)
  call void @air.set_mesh_index(ptr addrspace(1) %mesh, i32 0, i32 0)
  call void @air.set_mesh_index(ptr addrspace(1) %mesh, i32 1, i32 1)
  call void @air.set_mesh_primitive_count(ptr addrspace(1) %mesh, i32 1)
  ret void
}
declare void @air.set_mesh_vertex(ptr addrspace(1), i32, %struct.V)
declare void @air.set_mesh_index(ptr addrspace(1), i32, i32)
declare void @air.set_mesh_primitive_count(ptr addrspace(1), i32)
!air.mesh = !{!0}
!0 = !{ptr @mesh_ln, !1, !2}
!1 = !{}
!2 = !{!3}
!3 = !{i32 0, !"air.mesh", !"air.mesh_topology", !"line", !"air.max_vertices", i32 4, !"air.max_primitives", i32 2, !"air.vertex_output", !4}
!4 = !{!5}
!5 = !{!"air.position", !"air.arg_type_name", !"float4"}
"#;

#[test]
fn abi3_compat_and_stage_detection() {
    assert_eq!(garnetx_abi_version(), 3);
    assert_eq!(detect_stage(OBJECT_16K_LL), (RC_OK, GARNETX_STAGE_OBJECT));
    assert_eq!(detect_stage(MESH_TRI_PAYLOAD_LL), (RC_OK, GARNETX_STAGE_MESH));
    assert_eq!(detect_stage(FRAG_PER_PRIM_LL), (RC_OK, GARNETX_STAGE_FRAGMENT));

    let vert = r#"
define <4 x float> @vmain(i32 %vid) {
entry:
  ret <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>
}
!air.vertex = !{!0}
!0 = !{ptr @vmain, !1, !2}
!1 = !{!4}
!2 = !{!3}
!3 = !{i32 0, !"air.vertex_id"}
!4 = !{!"air.position", !"air.arg_type_name", !"float4"}
"#;
    assert_eq!(detect_stage(vert), (RC_OK, GARNETX_STAGE_VERTEX));

    let kern = r#"
define void @kmain(ptr addrspace(1) %out, i32 %tid) {
entry:
  %p = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %tid
  store i32 %tid, ptr addrspace(1) %p, align 4
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @kmain, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_name", !"uint"}
!4 = !{i32 1, !"air.thread_index_in_threadgroup"}
"#;
    assert_eq!(detect_stage(kern), (RC_OK, GARNETX_STAGE_KERNEL));

    // Plain kernel reflection must not gain spurious Stage 3 keys.
    let kout = call_plain(kern, None);
    assert_eq!(kout.rc, RC_OK, "{}", kout.msg);
    let kmeta: Value = serde_json::from_slice(&kout.meta).unwrap();
    assert!(kmeta.get("object").is_none());
    assert!(kmeta.get("mesh").is_none());
    assert!(kmeta.get("varyings").is_none());
    assert!(kmeta.get("acceleration_structures").is_none());
    assert!(kmeta.get("intersection_function_tables").is_none());

    // Unknown stage metadata -> RC_REFUSED.
    let (rc, _) = detect_stage("define void @f() { ret void }\n");
    assert_eq!(rc, RC_REFUSED);
}

#[test]
fn object_mesh_and_fragment_per_primitive_end_to_end_ffi() {
    let obj = call_plain(OBJECT_16K_LL, Some([32, 1, 1]));
    assert_eq!(obj.rc, RC_OK, "object failed: {}", obj.msg);
    let obj_asm = spv_asm(&obj.spv);
    assert!(obj_asm.contains("OpCapability MeshShadingEXT"), "{obj_asm}");
    assert!(obj_asm.contains("OpExtension \"SPV_EXT_mesh_shader\""), "{obj_asm}");
    assert!(obj_asm.contains("OpEntryPoint TaskEXT"), "{obj_asm}");
    assert!(obj_asm.contains("TaskPayloadWorkgroupEXT"), "{obj_asm}");
    assert!(obj_asm.contains("OpEmitMeshTasksEXT"), "{obj_asm}");
    assert!(obj_asm.contains("LocalSize 32 1 1"), "{obj_asm}");
    let obj_meta: Value = serde_json::from_slice(&obj.meta).unwrap();
    assert_eq!(obj_meta["stage"], "Object");
    assert_eq!(obj_meta["object"]["has_payload"], true);
    assert_eq!(obj_meta["object"]["payload_size"], 16384);

    let mesh = call_plain(MESH_TRI_PAYLOAD_LL, Some([3, 1, 1]));
    assert_eq!(mesh.rc, RC_OK, "mesh failed: {}", mesh.msg);
    let mesh_asm = spv_asm(&mesh.spv);
    assert!(mesh_asm.contains("OpEntryPoint MeshEXT"), "{mesh_asm}");
    assert!(mesh_asm.contains("OutputVertices 3"), "{mesh_asm}");
    assert!(mesh_asm.contains("OutputPrimitivesEXT 1"), "{mesh_asm}");
    assert!(mesh_asm.contains("OutputTrianglesEXT"), "{mesh_asm}");
    assert!(mesh_asm.contains("OpSetMeshOutputsEXT"), "{mesh_asm}");
    assert!(mesh_asm.contains("PerPrimitiveEXT"), "{mesh_asm}");
    assert!(mesh_asm.contains("CullPrimitiveEXT"), "{mesh_asm}");
    assert!(mesh_asm.contains("BuiltIn Layer"), "{mesh_asm}");
    assert!(mesh_asm.contains("BuiltIn ViewportIndex"), "{mesh_asm}");
    assert!(mesh_asm.contains("TaskPayloadWorkgroupEXT"), "{mesh_asm}");
    let mesh_meta: Value = serde_json::from_slice(&mesh.meta).unwrap();
    assert_eq!(mesh_meta["stage"], "Mesh");
    assert_eq!(mesh_meta["mesh"]["topology"], "Triangle");
    assert_eq!(mesh_meta["mesh"]["max_vertices"], 3);
    assert_eq!(mesh_meta["mesh"]["max_primitives"], 1);
    assert_eq!(mesh_meta["mesh"]["has_payload"], true);
    assert_eq!(mesh_meta["mesh"]["payload_size"], 16384);
    assert_eq!(mesh_meta["mesh"]["writes_cull_primitive"], true);
    assert_eq!(mesh_meta["mesh"]["writes_layer"], true);
    assert_eq!(mesh_meta["mesh"]["writes_viewport_index"], true);

    let frag = call_plain(FRAG_PER_PRIM_LL, None);
    assert_eq!(frag.rc, RC_OK, "frag failed: {}", frag.msg);
    let frag_asm = spv_asm(&frag.spv);
    assert!(frag_asm.contains("OpCapability MeshShadingEXT"), "{frag_asm}");
    assert!(frag_asm.contains("PerPrimitiveEXT"), "{frag_asm}");
    let frag_meta: Value = serde_json::from_slice(&frag.meta).unwrap();
    let frag_vars = frag_meta["varyings"].as_array().expect("frag varyings");
    assert_eq!(
        mesh_meta["mesh"]["vertex_outputs"][0]["location"],
        frag_vars[0]["location"]
    );
    assert_eq!(
        mesh_meta["mesh"]["primitive_outputs"][0]["location"],
        frag_vars[1]["location"]
    );
    assert_eq!(frag_vars[1]["per_primitive"], true);
}

#[test]
fn mesh_point_and_line_topologies_via_ffi() {
    let pt = call_plain(MESH_POINT_LL, Some([4, 1, 1]));
    assert_eq!(pt.rc, RC_OK, "mesh point failed: {}", pt.msg);
    let pt_asm = spv_asm(&pt.spv);
    assert!(pt_asm.contains("OutputPoints"), "{pt_asm}");
    assert!(pt_asm.contains("PrimitivePointIndicesEXT"), "{pt_asm}");
    let pt_meta: Value = serde_json::from_slice(&pt.meta).unwrap();
    assert_eq!(pt_meta["mesh"]["topology"], "Point");

    let ln = call_plain(MESH_LINE_LL, Some([4, 1, 1]));
    assert_eq!(ln.rc, RC_OK, "mesh line failed: {}", ln.msg);
    let ln_asm = spv_asm(&ln.spv);
    assert!(ln_asm.contains("OutputLinesEXT"), "{ln_asm}");
    assert!(ln_asm.contains("PrimitiveLineIndicesEXT"), "{ln_asm}");
    let ln_meta: Value = serde_json::from_slice(&ln.meta).unwrap();
    assert_eq!(ln_meta["mesh"]["topology"], "Line");
}

#[test]
fn hardware_ray_query_descriptor_and_bda_as_across_stages() {
    let compute_desc = r#"
define void @rq_comp(ptr addrspace(1) %as, ptr addrspace(1) %out, i32 %tid) {
entry:
  %any = icmp ne i32 %tid, 0
  %hit = call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float> } @air.intersect.instancing.triangle_data.world_space_data(<3 x float> <float 0.0, float 0.0, float -2.0>, <3 x float> <float 0.0, float 0.0, float 1.0>, float 0.001, float 100.0, ptr addrspace(1) %as, i32 255, ptr addrspace(1) null, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 -1, i32 -1, i1 %any, i1 false)
  %t = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float> } %hit, 1
  %dst = getelementptr inbounds float, ptr addrspace(1) %out, i32 %tid
  store float %t, ptr addrspace(1) %dst, align 4
  ret void
}
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float>, <3 x float> } @air.intersect.instancing.triangle_data.world_space_data(<3 x float>, <3 x float>, float, float, ptr addrspace(1), i32, ptr addrspace(1), ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)
!air.kernel = !{!0}
!0 = !{ptr @rq_comp, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing>"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!5 = !{i32 2, !"air.thread_index_in_threadgroup"}
"#;
    let out = call_plain(compute_desc, Some([32, 1, 1]));
    assert_eq!(out.rc, RC_OK, "rq_comp failed: {}", out.msg);
    let asm = spv_asm(&out.spv);
    assert!(asm.contains("OpCapability RayQueryKHR"), "{asm}");
    assert!(asm.contains("OpExtension \"SPV_KHR_ray_query\""), "{asm}");
    assert!(asm.contains("OpTypeAccelerationStructureKHR"), "{asm}");
    assert!(asm.contains("OpRayQueryInitializeKHR"), "{asm}");
    assert!(asm.contains("OpRayQueryProceedKHR"), "{asm}");
    assert!(asm.contains("OpRayQueryTerminateKHR"), "{asm}");
    assert!(asm.contains("OpRayQueryGetIntersectionObjectToWorldKHR"), "{asm}");
    let meta: Value = serde_json::from_slice(&out.meta).unwrap();
    let accels = meta["acceleration_structures"].as_array().expect("accels");
    assert_eq!(accels.len(), 1);
    assert_eq!(accels[0]["kind"], "Instance");
    assert_eq!(accels[0]["binding_mode"], "Descriptor");
    assert_eq!(accels[0]["metal_index"], 0);
    assert!(accels[0]["descriptor"]["binding"].as_u64().is_some());

    // Argument-buffer 64-bit BDA acceleration structure
    let compute_bda = r#"
%struct.SceneArgs = type { ptr addrspace(1) }
define void @rq_bda(ptr addrspace(1) %args, ptr addrspace(1) %out, i32 %tid) {
entry:
  %p = getelementptr inbounds %struct.SceneArgs, ptr addrspace(1) %args, i32 0, i32 0
  %as = load ptr addrspace(1), ptr addrspace(1) %p, align 8
  %hit = call { i32, float, i32, i32, ptr addrspace(1), i32, i32 } @air.intersect.instancing(<3 x float> zeroinitializer, <3 x float> <float 0.0, float 0.0, float 1.0>, float 0.001, float 100.0, ptr addrspace(1) %as, i32 255, ptr addrspace(1) null, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 -1, i32 -1, i1 false, i1 false)
  %kind = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32 } %hit, 0
  %dst = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %tid
  store i32 %kind, ptr addrspace(1) %dst, align 4
  ret void
}
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32 } @air.intersect.instancing(<3 x float>, <3 x float>, float, float, ptr addrspace(1), i32, ptr addrspace(1), ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)
!air.kernel = !{!0}
!0 = !{ptr @rq_bda, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.struct_type_info", !6}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"uint"}
!5 = !{i32 2, !"air.thread_index_in_threadgroup"}
!6 = !{i32 0, i32 8, i32 0, !"acceleration_structure<instancing>", !"tlas", !"air.indirect_argument", !7}
!7 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing>"}
"#;
    let bda_out = call_plain(compute_bda, Some([32, 1, 1]));
    assert_eq!(bda_out.rc, RC_OK, "rq_bda failed: {}", bda_out.msg);
    let bda_asm = spv_asm(&bda_out.spv);
    assert!(
        bda_asm.contains("OpConvertUToAccelerationStructureKHR"),
        "{bda_asm}"
    );
    let bda_meta: Value = serde_json::from_slice(&bda_out.meta).unwrap();
    let bda_as = &bda_meta["acceleration_structures"][0];
    assert_eq!(bda_as["binding_mode"], "BufferDeviceAddress");
    assert_eq!(bda_as["argument_buffer_index"], 0);
    assert_eq!(bda_as["byte_offset"], 0);
    assert_eq!(bda_as["bda_abi"], "u64_device_address");
}

#[test]
fn runtime_intersection_function_table_compute_and_fragment_ffi() {
    let bbox_ll = r#"
define { i1, float } @sphere_bbox(<3 x float> %org, <3 x float> %dir, float %tmin, float %tmax) {
entry:
  %r0 = insertvalue { i1, float } poison, i1 true, 0
  %r1 = insertvalue { i1, float } %r0, float 4.25, 1
  ret { i1, float } %r1
}
"#;
    let anyhit_ll = r#"
define i1 @cutout_anyhit(<3 x float> %org, <3 x float> %dir, float %tmin, float %t, <2 x float> %bary, i1 %front) {
entry:
  %u = extractelement <2 x float> %bary, i32 0
  %ok = fcmp ogt float %u, 5.000000e-01
  ret i1 %ok
}
"#;
    let comp_ift = r#"
define void @rq_comp_ift(ptr addrspace(1) %as, ptr addrspace(1) %ift, ptr addrspace(1) %out, i32 %tid) {
entry:
  %hit = call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.intersection_function_buffer.instancing.triangle_data(<3 x float> zeroinitializer, <3 x float> <float 0.0, float 0.0, float 1.0>, float 0.01, float 100.0, ptr addrspace(1) %as, i32 255, ptr addrspace(1) %ift, i64 4, i64 8, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 -1, i32 -1, i1 false, i1 false, i32 0, i32 0)
  %t = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %hit, 1
  %dst = getelementptr inbounds float, ptr addrspace(1) %out, i32 %tid
  store float %t, ptr addrspace(1) %dst, align 4
  ret void
}
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.intersection_function_buffer.instancing.triangle_data(<3 x float>, <3 x float>, float, float, ptr addrspace(1), i32, ptr addrspace(1), i64, i64, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1, i32, i32)
!air.kernel = !{!0}
!0 = !{ptr @rq_comp_ift, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read"}
!4 = !{i32 1, !"air.intersection_function_table", !"air.location_index", i32 1, i32 1, !"air.read"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!6 = !{i32 3, !"air.thread_index_in_threadgroup"}
"#;
    let frag_ift = r#"
define <4 x float> @rq_frag_ift(<4 x float> %uv, ptr addrspace(1) %as, ptr addrspace(1) %ift) {
entry:
  %hit = call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.intersection_function_buffer.instancing.triangle_data(<3 x float> zeroinitializer, <3 x float> <float 0.0, float 0.0, float 1.0>, float 0.01, float 100.0, ptr addrspace(1) %as, i32 255, ptr addrspace(1) %ift, i64 4, i64 8, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 -1, i32 -1, i1 false, i1 false, i32 0, i32 0)
  %t = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %hit, 1
  %c = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %t, i32 0
  ret <4 x float> %c
}
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.intersection_function_buffer.instancing.triangle_data(<3 x float>, <3 x float>, float, float, ptr addrspace(1), i32, ptr addrspace(1), i64, i64, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1, i32, i32)
!air.fragment = !{!0}
!0 = !{ptr @rq_frag_ift, !1, !2}
!1 = !{!6}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.fragment_input", !"air.location_index", i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 1, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read"}
!5 = !{i32 2, !"air.intersection_function_table", !"air.location_index", i32 1, i32 1, !"air.read"}
!6 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
"#;

    let fns = [
        ("sphere_bbox", GARNETX_IFT_KIND_BOUNDING_BOX, bbox_ll),
        ("cutout_anyhit", GARNETX_IFT_KIND_TRIANGLE_ANY_HIT, anyhit_ll),
    ];

    for (label, shader) in [("compute", comp_ift), ("fragment", frag_ift)] {
        let out = call_stage3(shader, Some([32, 1, 1]), &[], &[], &[], &fns);
        assert_eq!(out.rc, RC_OK, "{label} IFT failed: {}", out.msg);
        let asm = spv_asm(&out.spv);
        assert!(asm.contains("OpSwitch"), "{label}: {asm}");
        assert!(asm.contains("OpRayQueryGenerateIntersectionKHR"), "{label}: {asm}");
        assert!(asm.contains("OpRayQueryConfirmIntersectionKHR"), "{label}: {asm}");
        assert!(
            asm.contains("OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR"),
            "{label}: {asm}"
        );
        let meta: Value = serde_json::from_slice(&out.meta).unwrap();
        let ift = &meta["intersection_function_tables"];
        assert_eq!(ift["abi_version"], 1, "{label}");
        assert_eq!(ift["header_words"], 1, "{label}");
        assert_eq!(ift["tables"][0]["metal_index"], 1, "{label}");
        assert!(ift["tables"][0]["descriptor"]["binding"].as_u64().is_some());
        let handles = ift["handles"].as_array().expect("ift handles");
        assert_eq!(handles.len(), 2);
        assert_eq!(handles[0]["symbol"], "cutout_anyhit");
        assert_eq!(handles[0]["handle_id"], 1);
        assert_eq!(handles[0]["kind"], "TriangleAnyHit");
        assert_eq!(handles[1]["symbol"], "sphere_bbox");
        assert_eq!(handles[1]["handle_id"], 2);
        assert_eq!(handles[1]["kind"], "BoundingBox");
    }

    // Invalid kind -> RC_BAD_ARGS.
    let bad = call_stage3(comp_ift, None, &[], &[], &[], &[("sphere_bbox", 99, bbox_ll)]);
    assert_eq!(bad.rc, RC_BAD_ARGS);
}

#[test]
fn ray_query_with_inline_helper_dylib_vft_ift_and_same_library_ift_ffi() {
    // 1. Ray query + 1-block internal helper + MTLDynamicLibrary + VFT + IFT together.
    let dylib_ll = r#"
define float @ext_mul(float %x) {
entry:
  %r = fmul float %x, 2.000000e+00
  ret float %r
}
"#;
    let vis_ll = r#"
define float @vis_add(float %x) {
entry:
  %r = fadd float %x, 1.000000e+00
  ret float %r
}
"#;
    let bbox_ll = r#"
define { i1, float } @sphere_bbox(<3 x float> %org, <3 x float> %dir, float %tmin, float %tmax) {
entry:
  %r0 = insertvalue { i1, float } poison, i1 true, 0
  %r1 = insertvalue { i1, float } %r0, float 4.250000e+00, 1
  ret { i1, float } %r1
}
"#;
    let combo_ll = r#"
define internal float @local_bias(float %x) {
entry:
  %r = fadd float %x, 5.000000e-01
  ret float %r
}

define void @rq_combo(ptr addrspace(1) %as, ptr addrspace(1) %ift, ptr addrspace(1) %vft, ptr addrspace(1) %out, i32 %tid) {
entry:
  %hit = call { i32, float, i32, i32, ptr addrspace(1), i32, i32 } @air.intersect.intersection_function_buffer.instancing(<3 x float> zeroinitializer, <3 x float> <float 0.0, float 0.0, float 1.0>, float 0.01, float 100.0, ptr addrspace(1) %as, i32 255, ptr addrspace(1) %ift, i64 4, i64 8, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 -1, i32 -1, i1 false, i1 false, i32 0, i32 0)
  %t = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32 } %hit, 1
  %s0 = call float @local_bias(float %t)
  %s1 = call float @ext_mul(float %s0)
  %vfn = call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) %vft, i32 0)
  %s2 = call float %vfn(float %s1)
  %dst = getelementptr inbounds float, ptr addrspace(1) %out, i32 %tid
  store float %s2, ptr addrspace(1) %dst, align 4
  ret void
}
declare float @ext_mul(float)
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1), i32)
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32 } @air.intersect.intersection_function_buffer.instancing(<3 x float>, <3 x float>, float, float, ptr addrspace(1), i32, ptr addrspace(1), i64, i64, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1, i32, i32)
!air.kernel = !{!0}
!0 = !{ptr @rq_combo, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6, !7}
!3 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read"}
!4 = !{i32 1, !"air.intersection_function_table", !"air.location_index", i32 1, i32 1, !"air.read"}
!5 = !{i32 2, !"air.visible_function_table", !"air.location_index", i32 2, i32 1, !"air.read"}
!6 = !{i32 3, !"air.buffer", !"air.location_index", i32 3, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!7 = !{i32 4, !"air.thread_index_in_threadgroup"}
"#;
    let combo = call_stage3(
        combo_ll,
        Some([32, 1, 1]),
        &[],
        &[("ext.metallib", dylib_ll)],
        &[("vis_add", vis_ll)],
        &[("sphere_bbox", GARNETX_IFT_KIND_BOUNDING_BOX, bbox_ll)],
    );
    assert_eq!(combo.rc, RC_OK, "combo failed: {}", combo.msg);
    let combo_asm = spv_asm(&combo.spv);
    assert!(combo_asm.contains("OpRayQueryProceedKHR"), "{combo_asm}");
    assert!(combo_asm.contains("OpRayQueryGenerateIntersectionKHR"), "{combo_asm}");
    let combo_meta: Value = serde_json::from_slice(&combo.meta).unwrap();
    assert!(combo_meta["visible_function_tables"].is_object());
    assert!(combo_meta["intersection_function_tables"].is_object());

    // 2. Same-library whole-module IFT (entry + [[intersection]] in one MTLLibrary) + conflict check.
    let whole_lib = r#"
define { i1, float } @sphere_hit(<3 x float> %org, <3 x float> %dir, float %tmin, float %tmax) {
entry:
  %r0 = insertvalue { i1, float } poison, i1 true, 0
  %r1 = insertvalue { i1, float } %r0, float 4.000000e+00, 1
  ret { i1, float } %r1
}

define void @rt_k(ptr addrspace(1) %as, ptr addrspace(1) %ift, ptr addrspace(1) %out, i32 %tid) {
entry:
  %hit = call { i32, float, i32, i32, ptr addrspace(1), i32, i32 } @air.intersect.intersection_function_buffer.instancing(<3 x float> zeroinitializer, <3 x float> <float 0.0, float 0.0, float 1.0>, float 0.01, float 100.0, ptr addrspace(1) %as, i32 255, ptr addrspace(1) %ift, i64 4, i64 8, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 -1, i32 -1, i1 false, i1 false, i32 0, i32 0)
  %t = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32 } %hit, 1
  %dst = getelementptr inbounds float, ptr addrspace(1) %out, i32 %tid
  store float %t, ptr addrspace(1) %dst, align 4
  ret void
}
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32 } @air.intersect.intersection_function_buffer.instancing(<3 x float>, <3 x float>, float, float, ptr addrspace(1), i32, ptr addrspace(1), i64, i64, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1, i32, i32)
!air.kernel = !{!0}
!0 = !{ptr @rt_k, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read"}
!4 = !{i32 1, !"air.intersection_function_table", !"air.location_index", i32 1, i32 1, !"air.read"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!6 = !{i32 3, !"air.thread_index_in_threadgroup"}
"#;
    let same = call_stage3(
        whole_lib,
        Some([32, 1, 1]),
        &[],
        &[],
        &[],
        &[("sphere_hit", GARNETX_IFT_KIND_BOUNDING_BOX, whole_lib)],
    );
    assert_eq!(same.rc, RC_OK, "same-library IFT failed: {}", same.msg);

    let conflict_hit = r#"
define { i1, float } @sphere_hit(<3 x float> %org, <3 x float> %dir, float %tmin, float %tmax) {
entry:
  %r0 = insertvalue { i1, float } poison, i1 false, 0
  %r1 = insertvalue { i1, float } %r0, float 9.000000e+00, 1
  ret { i1, float } %r1
}
"#;
    let conflict = call_stage3(
        whole_lib,
        Some([32, 1, 1]),
        &[],
        &[],
        &[],
        &[("sphere_hit", GARNETX_IFT_KIND_BOUNDING_BOX, conflict_hit)],
    );
    assert_eq!(conflict.rc, RC_REFUSED);
    assert!(conflict.msg.contains("conflicts"), "{}", conflict.msg);
}

fn call_stage3_with_caps(
    ll: &str,
    local: Option<[u32; 3]>,
    vis_fns: &[(&str, &str)],
    caps: Option<RDNA4xBackendCapabilities>,
) -> Out {
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
            c_vis.as_ptr(),
            c_vis.len(),
            std::ptr::null(),
            0,
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

#[test]
fn apple_air_pull_model_perspective_and_no_perspective_ffi() {
    let ll = r#"
define <4 x float> @fs_apple_pull(<2 x float> %np_uv, <2 x float> %p_uv, <2 x half> %np_h) {
entry:
  %c = call <2 x float> @air.interpolate_center_no_perspective.v2f32(<2 x float> %np_uv)
  %ce = call <2 x float> @air.interpolate_centroid_no_perspective.v2f32(<2 x float> %np_uv)
  %sa = call <2 x float> @air.interpolate_sample_perspective.v2f32(<2 x float> %p_uv, i32 0)
  %of = call <2 x half> @air.interpolate_offset_no_perspective.v2f16(<2 x half> %np_h, <2 x float> <float 0.75, float 0.25>)
  %of32 = fpext <2 x half> %of to <2 x float>
  %s0 = fadd <2 x float> %c, %ce
  %s1 = fadd <2 x float> %sa, %of32
  %x = extractelement <2 x float> %s0, i32 0
  %y = extractelement <2 x float> %s0, i32 1
  %z = extractelement <2 x float> %s1, i32 0
  %w = extractelement <2 x float> %s1, i32 1
  %r0 = insertelement <4 x float> poison, float %x, i32 0
  %r1 = insertelement <4 x float> %r0, float %y, i32 1
  %r2 = insertelement <4 x float> %r1, float %z, i32 2
  %r3 = insertelement <4 x float> %r2, float %w, i32 3
  ret <4 x float> %r3
}
declare <2 x float> @air.interpolate_center_no_perspective.v2f32(<2 x float>)
declare <2 x float> @air.interpolate_centroid_no_perspective.v2f32(<2 x float>)
declare <2 x float> @air.interpolate_sample_perspective.v2f32(<2 x float>, i32)
declare <2 x half> @air.interpolate_offset_no_perspective.v2f16(<2 x half>, <2 x float>)

!air.fragment = !{!0}
!0 = !{ptr @fs_apple_pull, !1, !2}
!1 = !{!3}
!2 = !{!4, !5, !6}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.fragment_input", !"air.location_index", i32 0, i32 1, !"air.pull_mode", !"air.arg_type_name", !"interpolant<float2, interpolation::no_perspective>"}
!5 = !{i32 1, !"air.fragment_input", !"air.location_index", i32 1, i32 1, !"air.pull_mode", !"air.arg_type_name", !"interpolant<float2, interpolation::perspective>"}
!6 = !{i32 2, !"air.fragment_input", !"air.location_index", i32 2, i32 1, !"air.pull_mode", !"air.arg_type_name", !"interpolant<half2, interpolation::no_perspective>"}
"#;
    let out = call_plain(ll, None);
    assert_eq!(out.rc, RC_OK, "apple pull-model FFI failed: {}", out.msg);
    let asm = spv_asm(&out.spv);
    assert!(asm.contains("OpCapability InterpolationFunction"), "{asm}");
    assert!(asm.contains("NoPerspective"), "{asm}");
    assert!(asm.contains("InterpolateAtCentroid"), "{asm}");
    assert!(asm.contains("InterpolateAtSample"), "{asm}");
    assert!(asm.contains("InterpolateAtOffset"), "{asm}");
    assert!(asm.contains("OpFConvert"), "{asm}");
}

#[test]
fn fragment_vft_constant_vector_literal_regression_ffi() {
    let red_ll = r#"
define <4 x float> @color_red(<4 x float> %base) {
entry:
  %r = insertelement <4 x float> %base, float 1.000000e+00, i32 0
  ret <4 x float> %r
}
"#;
    let green_ll = r#"
define <4 x float> @color_green(<4 x float> %base) {
entry:
  %r = insertelement <4 x float> %base, float 1.000000e+00, i32 1
  ret <4 x float> %r
}
"#;
    let vft_fs_ll = r#"
define <4 x float> @vft_fs(ptr addrspace(1) %table, ptr addrspace(2) %slot_buf) {
entry:
  %slot = load i32, ptr addrspace(2) %slot_buf, align 4
  %fp = tail call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) %table, i32 %slot)
  %col = tail call <4 x float> %fp(<4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>)
  ret <4 x float> %col
}
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1), i32)

!air.fragment = !{!0}
!0 = !{ptr @vft_fs, !1, !2}
!1 = !{!3}
!2 = !{!4, !5}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.visible_function_table", !"air.location_index", i32 0, i32 1, !"air.read"}
!5 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"uint"}
"#;
    let vis = [("color_red", red_ll), ("color_green", green_ll)];
    // Stage 3 path
    let out3 = call_stage3_with_caps(vft_fs_ll, None, &vis, None);
    assert_eq!(out3.rc, RC_OK, "vft_fs stage3 failed: {}", out3.msg);
    let asm3 = spv_asm(&out3.spv);
    assert!(asm3.contains("OpSwitch"), "{asm3}");
    let meta3: Value = serde_json::from_slice(&out3.meta).unwrap();
    let handles = meta3["visible_function_tables"]["handles"]
        .as_array()
        .expect("vft handles");
    assert_eq!(handles.len(), 2);
    assert_eq!(handles[0]["symbol"], "color_green");
    assert_eq!(handles[1]["symbol"], "color_red");

    // Stage 2 path (`garnetx_translate_linked_libraries`)
    let c_vis: Vec<RDNA4xDep> = vis
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
    let rc2 = unsafe {
        garnetx_translate_linked_libraries(
            vft_fs_ll.as_ptr(),
            vft_fs_ll.len(),
            std::ptr::null(),
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
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
    assert_eq!(rc2, RC_OK);
    unsafe {
        garnetx_free(spv, spv_len);
        garnetx_free(meta, meta_len);
    }
}

#[test]
fn emulated_object_and_mesh_payload_ffi() {
    assert_eq!(garnetx_abi_version(), 3);
    assert_eq!(GARNETX_CAP_MESH_EMULATED_PAYLOAD, 1 << 8);
    assert_eq!(GARNETX_MESH_GRID_PARAMS_BINDING, 673);
    assert_eq!(GARNETX_MESH_PAYLOAD_BINDING, 674);
    assert_eq!(GARNETX_MESH_INDIRECT_BINDING, 675);

    let emul_caps = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: GARNETX_CAP_MESH_EMULATED_PAYLOAD,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };

    // 1. Emulated Object via garnetx_translate_stage3
    let obj = call_stage3_with_caps(OBJECT_16K_LL, Some([32, 1, 1]), &[], Some(emul_caps));
    assert_eq!(obj.rc, RC_OK, "emulated object failed: {}", obj.msg);
    let obj_asm = spv_asm(&obj.spv);
    assert!(obj_asm.contains("OpEntryPoint GLCompute"), "{obj_asm}");
    assert!(!obj_asm.contains("MeshShadingEXT"), "{obj_asm}");
    assert!(!obj_asm.contains("OpEmitMeshTasksEXT"), "{obj_asm}");
    assert!(!obj_asm.contains("TaskPayloadWorkgroupEXT"), "{obj_asm}");
    assert!(obj_asm.contains("Binding 673"), "{obj_asm}");
    assert!(obj_asm.contains("Binding 674"), "{obj_asm}");
    assert!(obj_asm.contains("Binding 675"), "{obj_asm}");
    let obj_meta: Value = serde_json::from_slice(&obj.meta).unwrap();
    assert_eq!(obj_meta["stage"], "Object");
    assert_eq!(obj_meta["object"]["emulated_payload"], true);
    assert_eq!(obj_meta["object"]["payload_stride"], 16384);
    let obj_bindings = obj_meta["bindings"].as_array().unwrap();
    assert!(obj_bindings
        .iter()
        .any(|b| b["kind"] == "MeshEmulatedGridParams" && b["descriptor"]["binding"] == 673));
    assert!(obj_bindings
        .iter()
        .any(|b| b["kind"] == "MeshEmulatedPayload" && b["descriptor"]["binding"] == 674));
    assert!(obj_bindings
        .iter()
        .any(|b| b["kind"] == "MeshEmulatedIndirect" && b["descriptor"]["binding"] == 675));

    // 2. Emulated Mesh with payload via garnetx_translate_with_capabilities
    let mut spv: *mut u8 = std::ptr::null_mut();
    let mut spv_len = 0usize;
    let mut meta: *mut u8 = std::ptr::null_mut();
    let mut meta_len = 0usize;
    let mut err = [0 as c_char; 512];
    let local = [32u32, 1, 1];
    let rc = unsafe {
        garnetx_translate_with_capabilities(
            MESH_TRI_PAYLOAD_LL.as_ptr(),
            MESH_TRI_PAYLOAD_LL.len(),
            local.as_ptr(),
            &mut spv,
            &mut spv_len,
            &mut meta,
            &mut meta_len,
            err.as_mut_ptr(),
            err.len(),
            &emul_caps,
        )
    };
    assert_eq!(rc, RC_OK, "emulated mesh failed: {}", unsafe {
        std::ffi::CStr::from_ptr(err.as_ptr()).to_string_lossy()
    });
    let mesh_spv = unsafe { std::slice::from_raw_parts(spv, spv_len).to_vec() };
    let mesh_meta_bytes = unsafe { std::slice::from_raw_parts(meta, meta_len).to_vec() };
    unsafe {
        garnetx_free(spv, spv_len);
        garnetx_free(meta, meta_len);
    }
    let mesh_asm = spv_asm(&mesh_spv);
    assert!(mesh_asm.contains("OpEntryPoint MeshEXT"), "{mesh_asm}");
    assert!(mesh_asm.contains("OpCapability DrawParameters"), "{mesh_asm}");
    assert!(mesh_asm.contains("BuiltIn DrawIndex"), "{mesh_asm}");
    assert!(
        mesh_asm.contains("OpPhi") && mesh_asm.contains("BuiltIn LocalInvocationIndex"),
        "array-first emulated Mesh payload must preserve LocalInvocationIndex-seeded OpPhi:\n{mesh_asm}"
    );
    assert!(!mesh_asm.contains("TaskPayloadWorkgroupEXT"), "{mesh_asm}");
    assert!(mesh_asm.contains("Binding 673"), "{mesh_asm}");
    assert!(mesh_asm.contains("Binding 674"), "{mesh_asm}");
    assert!(!mesh_asm.contains("Binding 675"), "{mesh_asm}");
    let mesh_meta: Value = serde_json::from_slice(&mesh_meta_bytes).unwrap();
    assert_eq!(mesh_meta["stage"], "Mesh");
    assert_eq!(mesh_meta["mesh"]["emulated_payload"], true);
    assert_eq!(mesh_meta["mesh"]["payload_stride"], 16384);
    let mesh_bindings = mesh_meta["bindings"].as_array().unwrap();
    assert!(mesh_bindings
        .iter()
        .any(|b| b["kind"] == "MeshEmulatedGridParams" && b["descriptor"]["binding"] == 673));
    assert!(mesh_bindings
        .iter()
        .any(|b| b["kind"] == "MeshEmulatedPayload" && b["descriptor"]["binding"] == 674));

    // 3. Mesh-only (no payload) with GARNETX_CAP_MESH_EMULATED_PAYLOAD stays pure hardware MeshEXT.
    let mesh_only = call_stage3_with_caps(MESH_POINT_LL, Some([4, 1, 1]), &[], Some(emul_caps));
    assert_eq!(mesh_only.rc, RC_OK, "mesh-only failed: {}", mesh_only.msg);
    let mo_asm = spv_asm(&mesh_only.spv);
    assert!(mo_asm.contains("OpEntryPoint MeshEXT"), "{mo_asm}");
    assert!(!mo_asm.contains("DrawParameters"), "{mo_asm}");
    assert!(!mo_asm.contains("Binding 673"), "{mo_asm}");
    assert!(!mo_asm.contains("Binding 674"), "{mo_asm}");
    let mo_meta: Value = serde_json::from_slice(&mesh_only.meta).unwrap();
    assert!(mo_meta["mesh"].get("emulated_payload").is_none());
    assert!(mo_meta["mesh"].get("payload_stride").is_none());

    // 4. Native mode (flags = 0) stays 100% unchanged TaskEXT + TaskPayloadWorkgroupEXT.
    let native_caps = RDNA4xBackendCapabilities {
        flags: 0,
        ..emul_caps
    };
    let nat_obj = call_stage3_with_caps(OBJECT_16K_LL, Some([32, 1, 1]), &[], Some(native_caps));
    assert_eq!(nat_obj.rc, RC_OK, "native object failed: {}", nat_obj.msg);
    let nat_asm = spv_asm(&nat_obj.spv);
    assert!(nat_asm.contains("OpEntryPoint TaskEXT"), "{nat_asm}");
    assert!(nat_asm.contains("TaskPayloadWorkgroupEXT"), "{nat_asm}");
    assert!(nat_asm.contains("OpEmitMeshTasksEXT"), "{nat_asm}");
    let nat_meta: Value = serde_json::from_slice(&nat_obj.meta).unwrap();
    assert!(nat_meta["object"].get("emulated_payload").is_none());
    assert!(nat_meta["object"].get("payload_stride").is_none());

    // 5. Unknown capability flag bit still rejected with RC_BAD_ARGS.
    let bad_caps = RDNA4xBackendCapabilities {
        flags: 1 << 30,
        ..emul_caps
    };
    let bad = call_stage3_with_caps(OBJECT_16K_LL, Some([32, 1, 1]), &[], Some(bad_caps));
    assert_eq!(bad.rc, RC_BAD_ARGS);

    // 6. Binding 674 collision (emulated payload + dynamic sampler) must return RC_REFUSED,
    //    while payload-less emulated Object + dynamic sampler succeeds with StageResourceMetadata.
    let obj_payload_sampler = r#"
%struct.Payload = type { [16 x i32] }
%struct._texture_2d_t = type opaque
%struct._sampler_t = type opaque
define void @obj_sp(ptr addrspace(6) %payload, ptr addrspace(1) %mgp, ptr addrspace(1) %tex, ptr addrspace(2) %samp) {
entry:
  %c = call <4 x float> @air.sample_texture_2d.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %samp, <2 x float> <float 0.5, float 0.5>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %x = extractelement <4 x float> %c, i32 0
  %xi = fptosi float %x to i32
  %p0 = getelementptr inbounds %struct.Payload, ptr addrspace(6) %payload, i32 0, i32 0, i32 0
  store i32 %xi, ptr addrspace(6) %p0, align 4
  call void @air.set_mesh_properties(ptr addrspace(1) %mgp, <3 x i32> <i32 1, i32 1, i32 1>)
  ret void
}
declare <4 x float> @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare void @air.set_mesh_properties(ptr addrspace(1), <3 x i32>)
!air.object = !{!0}
!0 = !{ptr @obj_sp, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.payload", !"air.arg_type_size", i32 64, !"air.arg_type_name", !"Payload"}
!4 = !{i32 1, !"air.mesh_grid_properties"}
!5 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, access::sample>"}
!6 = !{i32 3, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler"}
"#;
    let collide = call_stage3_with_caps(obj_payload_sampler, Some([4, 1, 1]), &[], Some(emul_caps));
    assert_eq!(collide.rc, RC_REFUSED, "expected RC_REFUSED on binding 674 collision");
    assert!(collide.msg.contains("674"), "{}", collide.msg);

    let obj_nopayload_sampler = r#"
%struct._texture_2d_t = type opaque
%struct._sampler_t = type opaque
define void @obj_s_nopay(ptr addrspace(1) %mgp, ptr addrspace(1) %tex, ptr addrspace(2) %samp) {
entry:
  %c = call <4 x float> @air.sample_texture_2d.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %samp, <2 x float> <float 0.5, float 0.5>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %x = extractelement <4 x float> %c, i32 0
  %gt = fcmp ogt float %x, 0.0
  %cnt = select i1 %gt, i32 1, i32 0
  %g = insertelement <3 x i32> <i32 1, i32 1, i32 1>, i32 %cnt, i32 0
  call void @air.set_mesh_properties(ptr addrspace(1) %mgp, <3 x i32> %g)
  ret void
}
declare <4 x float> @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare void @air.set_mesh_properties(ptr addrspace(1), <3 x i32>)
!air.object = !{!0}
!0 = !{ptr @obj_s_nopay, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.mesh_grid_properties"}
!4 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, access::sample>"}
!5 = !{i32 2, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler"}
"#;
    let nopay = call_stage3_with_caps(obj_nopayload_sampler, Some([4, 1, 1]), &[], Some(emul_caps));
    assert_eq!(nopay.rc, RC_OK, "payload-less emulated object + sampler failed: {}", nopay.msg);
    let nopay_meta: Value = serde_json::from_slice(&nopay.meta).unwrap();
    assert_eq!(nopay_meta["object"]["emulated_payload"], true);
    assert_eq!(nopay_meta["object"]["has_payload"], false);
    assert!(nopay_meta["object"].get("payload_stride").is_none());
    let nb = nopay_meta["bindings"].as_array().unwrap();
    assert!(nb.iter().any(|b| b["kind"] == "MeshEmulatedGridParams" && b["descriptor"]["binding"] == 673));
    assert!(nb.iter().any(|b| b["kind"] == "StageResourceMetadata" && b["descriptor"]["binding"] == 674));
    assert!(!nb.iter().any(|b| b["kind"] == "MeshEmulatedPayload"));
    assert!(nb.iter().any(|b| b["kind"] == "MeshEmulatedIndirect" && b["descriptor"]["binding"] == 675));
}

#[test]
fn apple_air_v28_live_fixes_0079_ffi() {
    let emul_caps = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: GARNETX_CAP_MESH_EMULATED_PAYLOAD,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };

    // 1. Real Apple AIR mesh_only_per_prim + obj_payload_16k + mesh_payload_16k + helper_add_blue.MTL_VISIBLE_FN_REF.
    let obj_16k = r#"
%struct.Payload16K = type { [4096 x i32] }
define void @obj_payload_16k(ptr addrspace(6) nocapture writeonly align 4 dereferenceable(16384) %payload, ptr addrspace(3) %mgp.0, ptr addrspace(2) nocapture readonly "air-buffer-no-alias" %cfg, i32 %tid, i32 %tgid) {
entry:
  %is0 = icmp eq i32 %tid, 0
  br i1 %is0, label %w, label %done
w:
  %c = load <4 x i32>, ptr addrspace(2) %cfg, align 16
  %cx = extractelement <4 x i32> %c, i32 0
  %cy = extractelement <4 x i32> %c, i32 1
  %cz = extractelement <4 x i32> %c, i32 2
  %w0 = add i32 %cx, %tgid
  %p0 = getelementptr inbounds %struct.Payload16K, ptr addrspace(6) %payload, i32 0, i32 0, i32 0
  store i32 %w0, ptr addrspace(6) %p0, align 4
  %p4095 = getelementptr inbounds %struct.Payload16K, ptr addrspace(6) %payload, i32 0, i32 0, i32 4095
  store i32 %cy, ptr addrspace(6) %p4095, align 4
  %g = insertelement <3 x i32> <i32 poison, i32 1, i32 1>, i32 %cz, i32 0
  tail call void @air.set_threadgroups_per_grid_mesh_properties(ptr addrspace(3) nocapture %mgp.0, <3 x i32> %g)
  br label %done
done:
  ret void
}
declare void @air.set_threadgroups_per_grid_mesh_properties(ptr addrspace(3) nocapture, <3 x i32>)
!air.object = !{!14}
!14 = !{ptr @obj_payload_16k, !15, !16, !23}
!15 = !{}
!16 = !{!17, !19, !20, !21, !22}
!17 = !{i32 0, !"air.payload", !"air.struct_type_info", !18, !"air.arg_type_size", i32 16384, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"Payload16K", !"air.arg_name", !"payload"}
!18 = !{i32 0, i32 16384, i32 4, !"uint", !"words", i32 4096}
!19 = !{i32 1, !"air.mesh_grid_properties", !"air.arg_type_name", !"mesh_grid_properties", !"air.arg_name", !"mgp"}
!20 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"cfg"}
!21 = !{i32 3, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint"}
!22 = !{i32 4, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint"}
!23 = !{!"air.max_mesh_work_groups", i32 4}
"#;
    let obj_emul = call_stage3_with_caps(obj_16k, Some([32, 1, 1]), &[], Some(emul_caps));
    assert_eq!(obj_emul.rc, RC_OK, "apple AIR obj_payload_16k failed: {}", obj_emul.msg);
    let obj_meta: Value = serde_json::from_slice(&obj_emul.meta).unwrap();
    assert_eq!(obj_meta["object"]["emulated_payload"], true);
    assert_eq!(obj_meta["object"]["max_total_threadgroups_per_mesh_grid"], 4);

    let mesh_helper = r#"
define void @mesh_with_private_helper(ptr addrspace(7) %out.coerce, i32 %tid) {
entry:
  %is0 = icmp eq i32 %tid, 0
  br i1 %is0, label %emit, label %done
emit:
  %c = tail call <4 x float> @helper_add_blue.MTL_VISIBLE_FN_REF(<4 x float> <float 5.0e-01, float 2.5e-01, float 0.0, float 1.0>)
  tail call void @air.set_position_mesh(ptr addrspace(7) nocapture %out.coerce, i32 0, <4 x float> <float -1.0, float -1.0, float 0.0, float 1.0>)
  tail call void @air.set_position_mesh(ptr addrspace(7) nocapture %out.coerce, i32 1, <4 x float> <float 1.0, float -1.0, float 0.0, float 1.0>)
  tail call void @air.set_position_mesh(ptr addrspace(7) nocapture %out.coerce, i32 2, <4 x float> <float 0.0, float 1.0, float 0.0, float 1.0>)
  tail call void @air.set_vertex_data_mesh.v4f32(ptr addrspace(7) nocapture %out.coerce, i32 0, i32 0, <4 x float> %c)
  tail call void @air.set_vertex_data_mesh.v4f32(ptr addrspace(7) nocapture %out.coerce, i32 0, i32 1, <4 x float> %c)
  tail call void @air.set_vertex_data_mesh.v4f32(ptr addrspace(7) nocapture %out.coerce, i32 0, i32 2, <4 x float> %c)
  tail call void @air.set_index_mesh(ptr addrspace(7) nocapture %out.coerce, i32 0, i8 0)
  tail call void @air.set_index_mesh(ptr addrspace(7) nocapture %out.coerce, i32 1, i8 1)
  tail call void @air.set_index_mesh(ptr addrspace(7) nocapture %out.coerce, i32 2, i8 2)
  tail call void @air.set_primitive_data_mesh.v4f32(ptr addrspace(7) nocapture %out.coerce, i32 0, i32 0, <4 x float> <float 0.0, float 5.0e-01, float 2.5e-01, float 0.0>)
  tail call void @air.set_primitive_count_mesh(ptr addrspace(7) nocapture %out.coerce, i32 1)
  br label %done
done:
  ret void
}
declare void @air.set_position_mesh(ptr addrspace(7) nocapture, i32, <4 x float>)
declare void @air.set_vertex_data_mesh.v4f32(ptr addrspace(7) nocapture, i32, i32, <4 x float>)
declare void @air.set_primitive_data_mesh.v4f32(ptr addrspace(7) nocapture, i32, i32, <4 x float>)
declare void @air.set_index_mesh(ptr addrspace(7) nocapture, i32, i8)
declare void @air.set_primitive_count_mesh(ptr addrspace(7) nocapture, i32)
declare <4 x float> @helper_add_blue.MTL_VISIBLE_FN_REF(<4 x float>) section "air.externally_defined"
!air.mesh = !{!14}
!14 = !{ptr @mesh_with_private_helper, !15, !16}
!15 = !{}
!16 = !{!17, !24}
!17 = !{i32 0, !"air.mesh", !18, !"air.arg_type_name", !"mesh<VertOut, PrimOut, 4, 2, triangle>", !"air.arg_name", !"out"}
!18 = !{!"air.mesh_type_info", !19, !22, i32 4, i32 2, !"air.triangle"}
!19 = !{!20, !21}
!20 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!21 = !{!"air.mesh_vertex_data", i32 0, !"user(locn0)", !"air.arg_type_name", !"float4", !"air.arg_name", !"vcolor"}
!22 = !{!23}
!23 = !{!"air.mesh_primitive_data", i32 0, !"user(locn1)", !"air.arg_type_name", !"float4", !"air.arg_name", !"pcolor"}
!24 = !{i32 1, !"air.thread_index_in_threadgroup", !"air.arg_type_name", !"uint"}
"#;
    let helper_ll = r#"
define <4 x float> @helper_add_blue(<4 x float> %c) {
entry:
  %r = insertelement <4 x float> %c, float 1.0, i32 2
  ret <4 x float> %r
}
"#;
    let (spv_mh, meta_mh) = garnetxlate::translate_bytes_stage3(
        mesh_helper.as_bytes(),
        [32, 1, 1],
        &[("helper_add_blue".to_string(), helper_ll.as_bytes().to_vec())],
        &[],
        &[],
        &[],
        Some(emul_caps),
    )
    .expect("mesh_with_private_helper via stage3");
    let mh_asm = spv_asm(&spv_mh);
    assert!(mh_asm.contains("OpEntryPoint MeshEXT"), "{mh_asm}");
    assert!(mh_asm.contains("PerPrimitiveEXT"), "{mh_asm}");
    let mh_meta: Value = serde_json::from_slice(&meta_mh).unwrap();
    assert_eq!(mh_meta["mesh"]["vertex_outputs"][0]["location"], 0);
    assert_eq!(mh_meta["mesh"]["primitive_outputs"][0]["location"], 1);
    assert_eq!(mh_meta["mesh"]["primitive_outputs"][0]["per_primitive"], true);

    // 2. Real Apple AIR trace_ift + aabb_sphere (<{ i1, float }>) + tri_anyhit_filter (i1) via FFI.
    let trace_ift = r#"
define void @trace_ift(ptr addrspace(1) %rays, ptr addrspace(1) %out, ptr addrspace(1) readonly %scene.coerce, ptr addrspace(1) readonly %ift.coerce, i32 %tid) {
entry:
  %hit = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float> <float 0.0, float 0.0, float 0.0>, <3 x float> <float 0.0, float 0.0, float -1.0>, float 1.000000e-03, float 1.000000e+02, ptr addrspace(1) readonly %scene.coerce, i32 255, ptr addrspace(1) readonly %ift.coerce, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 0, i32 0, i32 0, i1 false, i1 false)
  %ht = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %hit, 0
  %dst = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %tid
  store i32 %ht, ptr addrspace(1) %dst, align 4
  ret void
}
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float>, <3 x float>, float, float, ptr addrspace(1), i32, ptr addrspace(1), ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)
!air.kernel = !{!0}
!0 = !{ptr @trace_ift, !1, !2}
!1 = !{}
!2 = !{!19, !20, !21, !22, !23}
!19 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_name", !"uint"}
!20 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"uint"}
!21 = !{i32 2, !"air.instance_acceleration_structure", !"air.location_index", i32 2, i32 1, !"air.read"}
!22 = !{i32 3, !"air.intersection_function_table", !"air.location_index", i32 3, i32 1, !"air.read", !"air.arg_type_name", !"intersection_function_table<instancing, triangle_data>", !"air.arg_name", !"ift"}
!23 = !{i32 4, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
"#;
    let aabb_sphere = r#"
define <{ i1, float }> @aabb_sphere(<3 x float> %origin, <3 x float> %dir, float %tmin, float %tmax) {
entry:
  %r0 = insertvalue <{ i1, float }> poison, i1 true, 0
  %r1 = insertvalue <{ i1, float }> %r0, float 3.500000e+00, 1
  ret <{ i1, float }> %r1
}
!air.intersection = !{!14}
!14 = !{ptr @aabb_sphere, !15, !18, !"air.bounding_box", !"air.instancing"}
!15 = !{}
!18 = !{!19, !20, !21, !22}
!19 = !{i32 0, !"air.origin", !"air.arg_type_name", !"float3"}
!20 = !{i32 1, !"air.direction", !"air.arg_type_name", !"float3"}
!21 = !{i32 2, !"air.min_distance", !"air.arg_type_name", !"float"}
!22 = !{i32 3, !"air.max_distance", !"air.arg_type_name", !"float"}
"#;
    let tri_anyhit = r#"
define i1 @tri_anyhit_filter(<2 x float> %bary) {
entry:
  %bx = extractelement <2 x float> %bary, i32 0
  %ok = fcmp oge float %bx, 3.000000e-01
  ret i1 %ok
}
!air.intersection = !{!14}
!14 = !{ptr @tri_anyhit_filter, !15, !17, !"air.triangle", !"air.instancing", !"air.triangle_data"}
!15 = !{}
!17 = !{!18}
!18 = !{i32 0, !"air.barycentric_coord", !"air.arg_type_name", !"float2"}
"#;
    let rt_res = call_stage3(
        trace_ift,
        Some([4, 1, 1]),
        &[],
        &[],
        &[],
        &[
            ("aabb_sphere", GARNETX_IFT_KIND_BOUNDING_BOX, aabb_sphere),
            ("tri_anyhit_filter", GARNETX_IFT_KIND_TRIANGLE_ANY_HIT, tri_anyhit),
        ],
    );
    assert_eq!(rt_res.rc, RC_OK, "apple AIR trace_ift failed: {}", rt_res.msg);
    let rt_asm = spv_asm(&rt_res.spv);
    assert!(rt_asm.contains("OpSwitch"), "{rt_asm}");
    assert!(rt_asm.contains("OpRayQueryGenerateIntersectionKHR"), "{rt_asm}");
    assert!(rt_asm.contains("OpRayQueryConfirmIntersectionKHR"), "{rt_asm}");
}

#[test]
fn apple_air_v28_void_atomic_rmw_0080_ffi() {
    // 1. Exact Apple air64_v28 `k_u64_minmax` shape: void-returning `air.atomic.global.{min,max}.u.i64`.
    let u64_minmax = r#"
target triple = "air64_v28-apple-macosx26.7.0"
%"struct.metal::_atomic" = type { i64 }
define void @k_u64_minmax(ptr addrspace(1) nocapture noundef "air-buffer-no-alias" %slots, i32 noundef %gid) {
entry:
  %mul = mul i32 %gid, 37
  %add = add i32 %mul, 13
  %and = and i32 %add, 255
  %conv = zext i32 %and to i64
  %shl = shl nuw nsw i64 %conv, 36
  %xor = xor i32 %gid, 90
  %conv1 = zext i32 %xor to i64
  %shl2 = shl nuw nsw i64 %conv1, 8
  %conv3 = zext i32 %gid to i64
  %or = or i64 %shl2, %conv3
  %or4 = or i64 %or, %shl
  %__s.i24 = getelementptr inbounds %"struct.metal::_atomic", ptr addrspace(1) %slots, i64 0, i32 0
  tail call void @air.atomic.global.min.u.i64(ptr addrspace(1) nocapture %__s.i24, i64 %or4, i32 0, i32 2, i1 true)
  %__s.i26 = getelementptr inbounds %"struct.metal::_atomic", ptr addrspace(1) %slots, i64 1, i32 0
  tail call void @air.atomic.global.max.u.i64(ptr addrspace(1) nocapture %__s.i26, i64 %or4, i32 0, i32 2, i1 true)
  ret void
}
declare void @air.atomic.global.min.u.i64(ptr addrspace(1) nocapture, i64, i32, i32, i1)
declare void @air.atomic.global.max.u.i64(ptr addrspace(1) nocapture, i64, i32, i32, i1)
!air.kernel = !{!14}
!14 = !{ptr @k_u64_minmax, !15, !16}
!15 = !{}
!16 = !{!17, !19}
!17 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !18, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"metal::_atomic", !"air.arg_name", !"slots"}
!18 = !{i32 0, i32 8, i32 0, !"ulong", !"__s"}
!19 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
"#;
    let out_u64 = call_plain(u64_minmax, Some([64, 1, 1]));
    assert_eq!(out_u64.rc, RC_OK, "void u64 minmax failed: {}", out_u64.msg);
    let asm_u64 = spv_asm(&out_u64.spv);
    assert!(asm_u64.contains("OpCapability Int64"), "{asm_u64}");
    assert!(asm_u64.contains("OpCapability Int64Atomics"), "{asm_u64}");
    assert!(asm_u64.contains("OpAtomicUMin"), "{asm_u64}");
    assert!(asm_u64.contains("OpAtomicUMax"), "{asm_u64}");
    assert!(!asm_u64.contains("air.atomic."), "{asm_u64}");

    // 2. Void-returning signed i64 min/max and add.u.i64/sub.s.i64/xchg.i64.
    let s64_add = r#"
target triple = "air64_v28-apple-macosx26.7.0"
define void @k_s64_void(ptr addrspace(1) %slots, i32 %gid) {
entry:
  %v = sext i32 %gid to i64
  %p0 = getelementptr inbounds i64, ptr addrspace(1) %slots, i64 0
  %p1 = getelementptr inbounds i64, ptr addrspace(1) %slots, i64 1
  %p2 = getelementptr inbounds i64, ptr addrspace(1) %slots, i64 2
  tail call void @air.atomic.global.min.s.i64(ptr addrspace(1) %p0, i64 %v, i32 0, i32 2, i1 true)
  tail call void @air.atomic.global.max.s.i64(ptr addrspace(1) %p1, i64 %v, i32 0, i32 2, i1 true)
  tail call void @air.atomic.global.add.u.i64(ptr addrspace(1) %p2, i64 1, i32 0, i32 2, i1 true)
  ret void
}
declare void @air.atomic.global.min.s.i64(ptr addrspace(1), i64, i32, i32, i1)
declare void @air.atomic.global.max.s.i64(ptr addrspace(1), i64, i32, i32, i1)
declare void @air.atomic.global.add.u.i64(ptr addrspace(1), i64, i32, i32, i1)
!air.kernel = !{!0}
!0 = !{ptr @k_s64_void, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"long", !"air.arg_name", !"slots"}
!4 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
"#;
    let out_s64 = call_plain(s64_add, Some([32, 1, 1]));
    assert_eq!(out_s64.rc, RC_OK, "void s64/add failed: {}", out_s64.msg);
    let asm_s64 = spv_asm(&out_s64.spv);
    assert!(asm_s64.contains("OpAtomicSMin"), "{asm_s64}");
    assert!(asm_s64.contains("OpAtomicSMax"), "{asm_s64}");
    assert!(asm_s64.contains("OpAtomicIAdd"), "{asm_s64}");
    assert!(!asm_s64.contains("air.atomic."), "{asm_s64}");

    // 3. Void-returning f32 native add/sub/xchg and CAS min/max.
    let f32_void = r#"
target triple = "air64_v28-apple-macosx26.7.0"
define void @k_f32_void(ptr addrspace(1) %bf, ptr addrspace(1) %bf_cas, i32 %gid) {
entry:
  %v = sitofp i32 %gid to float
  %idx = zext i32 %gid to i64
  %pf = getelementptr inbounds float, ptr addrspace(1) %bf, i64 %idx
  %pc = getelementptr inbounds float, ptr addrspace(1) %bf_cas, i64 %idx
  tail call void @air.atomic.global.add.f.f32(ptr addrspace(1) %pf, float %v, i32 0, i32 2, i1 true)
  tail call void @air.atomic.global.sub.f32(ptr addrspace(1) %pf, float 1.0, i32 0, i32 2, i1 true)
  tail call void @air.atomic.global.min.f.f32(ptr addrspace(1) %pc, float %v, i32 0, i32 2, i1 true)
  tail call void @air.atomic.global.max.f.f32(ptr addrspace(1) %pc, float %v, i32 0, i32 2, i1 true)
  ret void
}
declare void @air.atomic.global.add.f.f32(ptr addrspace(1), float, i32, i32, i1)
declare void @air.atomic.global.sub.f32(ptr addrspace(1), float, i32, i32, i1)
declare void @air.atomic.global.min.f.f32(ptr addrspace(1), float, i32, i32, i1)
declare void @air.atomic.global.max.f.f32(ptr addrspace(1), float, i32, i32, i1)
!air.kernel = !{!0}
!0 = !{ptr @k_f32_void, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"bf"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"bf_cas"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"gid"}
"#;
    let out_f32 = call_plain(f32_void, Some([32, 1, 1]));
    assert_eq!(out_f32.rc, RC_OK, "void f32 atomic failed: {}", out_f32.msg);
    let asm_f32 = spv_asm(&out_f32.spv);
    assert!(asm_f32.contains("OpCapability AtomicFloat32AddEXT"), "{asm_f32}");
    assert!(asm_f32.contains("OpAtomicFAddEXT"), "{asm_f32}");
    assert!(asm_f32.contains("OpAtomicCompareExchange"), "{asm_f32}");
    assert!(!asm_f32.contains("air.atomic."), "{asm_f32}");
}

#[test]
fn wave1_rt_position_fetch_cap_intersection_query_and_ray_data_payload_ffi() {
    assert_eq!(GARNETX_CAP_RT_POSITION_FETCH, 1 << 9);
    let rt_caps = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: GARNETX_CAP_RT_POSITION_FETCH,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };

    // 1. Metal 3 intersection_query with null-checked primitive_data load (exact Apple Clang 21 shape).
    let iq_ll = r#"
target triple = "air64_v28-apple-macosx26.7.0"
define void @k_iq_prim_null(ptr addrspace(1) %scene.coerce, ptr addrspace(1) %out) {
entry:
  %0 = tail call ptr @air.allocate_intersection_query.instancing.triangle_data()
  tail call void @air.reset_intersection_query.instancing.triangle_data(ptr %0, <3 x float> <float 5.0e-01, float 5.0e-01, float 0.0>, <3 x float> <float 0.0, float 0.0, float -1.0>, float 1.0e-03, float 1.0e+02, ptr addrspace(1) readonly %scene.coerce, i32 255, i32 1, i32 2, i32 0, i32 0, i32 0, i32 3, i32 0, i32 0, i32 0, i1 false, i1 false)
  %1 = tail call i1 @air.next_intersection_query.instancing.triangle_data(ptr %0)
  br i1 %1, label %while.body, label %while.end
while.body:
  %2 = tail call i32 @air.get_candidate_intersection_type_intersection_query.instancing.triangle_data(ptr nocapture readonly %0)
  %is_tri = icmp eq i32 %2, 1
  br i1 %is_tri, label %tri, label %next_it
tri:
  %cpd = tail call ptr addrspace(1) @air.get_candidate_primitive_data_intersection_query.instancing.triangle_data(ptr nocapture readonly %0)
  %is_null = icmp eq ptr addrspace(1) %cpd, null
  br i1 %is_null, label %commit, label %read_pd
read_pd:
  %color = bitcast ptr addrspace(1) %cpd to ptr addrspace(1)
  %v = load <4 x float>, ptr addrspace(1) %color, align 16
  %vx = extractelement <4 x float> %v, i64 0
  %ok = fcmp oge float %vx, 0.0
  br i1 %ok, label %commit, label %next_it
commit:
  tail call void @air.commit_triangle_intersection_intersection_query.instancing.triangle_data(ptr %0)
  br label %next_it
next_it:
  %more = tail call i1 @air.next_intersection_query.instancing.triangle_data(ptr %0)
  br i1 %more, label %while.body, label %while.end
while.end:
  %t = tail call float @air.get_committed_distance_intersection_query.instancing.triangle_data(ptr nocapture readonly %0)
  %ff = tail call i1 @air.is_committed_triangle_front_facing_intersection_query.instancing.triangle_data(ptr nocapture readonly %0)
  %ff_f = select i1 %ff, float %t, float -1.0
  store float %ff_f, ptr addrspace(1) %out, align 4
  tail call void @air.deallocate_intersection_query.instancing.triangle_data(ptr %0)
  ret void
}
declare ptr @air.allocate_intersection_query.instancing.triangle_data()
declare void @air.deallocate_intersection_query.instancing.triangle_data(ptr)
declare void @air.reset_intersection_query.instancing.triangle_data(ptr, <3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)
declare i1 @air.next_intersection_query.instancing.triangle_data(ptr)
declare i32 @air.get_candidate_intersection_type_intersection_query.instancing.triangle_data(ptr nocapture readonly)
declare ptr addrspace(1) @air.get_candidate_primitive_data_intersection_query.instancing.triangle_data(ptr nocapture readonly)
declare void @air.commit_triangle_intersection_intersection_query.instancing.triangle_data(ptr)
declare float @air.get_committed_distance_intersection_query.instancing.triangle_data(ptr nocapture readonly)
declare i1 @air.is_committed_triangle_front_facing_intersection_query.instancing.triangle_data(ptr nocapture readonly)
!air.kernel = !{!0}
!0 = !{ptr @k_iq_prim_null, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
"#;
    let out_iq = call_stage3_with_caps(iq_ll, Some([1, 1, 1]), &[], Some(rt_caps));
    assert_eq!(out_iq.rc, RC_OK, "intersection_query with GARNETX_CAP_RT_POSITION_FETCH failed: {}", out_iq.msg);
    let asm_iq = spv_asm(&out_iq.spv);
    assert!(asm_iq.contains("OpRayQueryInitializeKHR"), "{asm_iq}");
    assert!(asm_iq.contains("OpRayQueryProceedKHR"), "{asm_iq}");
    assert!(asm_iq.contains("OpRayQueryConfirmIntersectionKHR"), "{asm_iq}");

    // 2. Unknown capability bit still fails cleanly with RC_BAD_ARGS.
    let bad_caps = RDNA4xBackendCapabilities {
        flags: 1 << 30,
        ..rt_caps
    };
    let out_bad = call_stage3_with_caps(iq_ll, Some([1, 1, 1]), &[], Some(bad_caps));
    assert_eq!(out_bad.rc, RC_BAD_ARGS, "unknown capability flag must fail with RC_BAD_ARGS");
}

#[test]
fn wave2_bindless30_flat_and_nested_apple_air_lower_to_abi10() {
    let rt_caps = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: GARNETX_CAP_RT_POSITION_FETCH | GARNETX_CAP_RT_IFT_ABI_V2,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };

    // Exact Apple Clang 21 AIR for encoder-less Metal 3 flat bindless struct array:
    // struct Material { texture2d<float> albedo; sampler smp; float4 tint; };
    // kernel void k_bindless_flat(device const Material *mats [[buffer(0)]], ...)
    let flat_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
%struct.Material = type { %"struct.metal::texture2d", %"struct.metal::sampler", <4 x float> }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::sampler" = type { ptr addrspace(2) }
define void @k_bindless_flat(ptr addrspace(1) nocapture readonly "air-buffer-no-alias" %mats, ptr addrspace(1) nocapture writeonly "air-buffer-no-alias" %out, i32 %tid) local_unnamed_addr {
entry:
  %idxprom = zext i32 %tid to i64
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.Material, ptr addrspace(1) %mats, i64 %idxprom, i32 1, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(1) %agg.tmp.sroa.0.0..sroa_idx, align 8
  %t.i = getelementptr inbounds %struct.Material, ptr addrspace(1) %mats, i64 %idxprom, i32 0, i32 0
  %0 = load ptr addrspace(1), ptr addrspace(1) %t.i, align 8
  %1 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly %0, ptr addrspace(2) nocapture readonly %agg.tmp.sroa.0.0.copyload, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0)
  %2 = extractvalue { <4 x float>, i8 } %1, 0
  %tint = getelementptr inbounds %struct.Material, ptr addrspace(1) %mats, i64 %idxprom, i32 2
  %3 = load <4 x float>, ptr addrspace(1) %tint, align 16
  %mul = fmul fast <4 x float> %2, %3
  %arrayidx7 = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idxprom
  store <4 x float> %mul, ptr addrspace(1) %arrayidx7, align 16
  ret void
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32)
!air.kernel = !{!14}
!14 = !{ptr @k_bindless_flat, !15, !16}
!15 = !{}
!16 = !{!17, !22, !23}
!17 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.struct_type_info", !18, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"Material", !"air.arg_name", !"mats"}
!18 = !{i32 0, i32 8, i32 0, !"texture2d<float, sample>", !"albedo", !"air.indirect_argument", !19, i32 8, i32 8, i32 0, !"sampler", !"smp", !"air.indirect_argument", !20, i32 16, i32 16, i32 0, !"float4", !"tint", !"air.indirect_argument", !21}
!19 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"albedo"}
!20 = !{i32 1, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"smp"}
!21 = !{i32 2, !"air.indirect_constant", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"tint"}
!22 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!23 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;
    let out_flat = call_stage3_with_caps(flat_ll, Some([1, 1, 1]), &[], Some(rt_caps));
    assert_eq!(out_flat.rc, RC_OK, "flat bindless translate failed: {}", out_flat.msg);
    let (spv_flat, meta_flat) =
        garnetxlate::lower_gpu_arguments_with_capabilities(&out_flat.spv, &out_flat.meta, rt_caps)
            .expect("flat bindless lower_gpu_arguments failed");
    let meta_flat_json: Value = serde_json::from_slice(&meta_flat).unwrap();
    let cats_flat = meta_flat_json["argument_resource_tables"]["categories"]
        .as_array()
        .expect("flat bindless must emit argument_resource_tables.categories");
    assert!(cats_flat.iter().any(|v| v == 0), "missing texture category 0: {meta_flat_json}");
    assert!(cats_flat.iter().any(|v| v == 2), "missing sampler category 2: {meta_flat_json}");
    let asm_flat = spv_asm(&spv_flat);
    assert!(asm_flat.contains("OpCapability RuntimeDescriptorArray"), "{asm_flat}");
    assert!(asm_flat.contains("DescriptorSet 2"), "{asm_flat}");
    assert!(asm_flat.contains("DescriptorSet 4"), "{asm_flat}");

    // Exact Apple Clang 21 AIR for nested bindless scene + RT TLAS + material pointer deref:
    let nested_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
%struct.BindlessScene = type { ptr addrspace(1), %"struct.metal::raytracing::_acceleration_structure", %"struct.metal::texture2d", %"struct.metal::sampler" }
%"struct.metal::raytracing::_acceleration_structure" = type { ptr addrspace(1) }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::sampler" = type { ptr addrspace(2) }
%struct.Material = type { %"struct.metal::texture2d", %"struct.metal::sampler", <4 x float> }
define void @k_bindless_nested(ptr addrspace(2) nocapture readonly align 8 dereferenceable(32) "air-buffer-no-alias" %scene, ptr addrspace(1) nocapture writeonly "air-buffer-no-alias" %out, i32 %tid) local_unnamed_addr {
entry:
  %agg.tmp3.sroa.0.0..sroa_idx = getelementptr inbounds %struct.BindlessScene, ptr addrspace(2) %scene, i64 0, i32 1, i32 0
  %agg.tmp3.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(2) %agg.tmp3.sroa.0.0..sroa_idx, align 8
  %0 = tail call ptr addrspace(1) @air.get_null_intersection_function_table()
  %1 = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float> <float 5.000000e-01, float 5.000000e-01, float 0.000000e+00>, <3 x float> <float 0.000000e+00, float 0.000000e+00, float -1.000000e+00>, float 0x3F50624DE0000000, float 1.000000e+02, ptr addrspace(1) readonly %agg.tmp3.sroa.0.0.copyload, i32 255, ptr addrspace(1) readonly %0, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 0, i32 0, i32 0, i1 false, i1 false)
  %2 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %1, 0
  %cmp.not = icmp eq i32 %2, 0
  br i1 %cmp.not, label %cond.false, label %cond.true
cond.true:
  %3 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %1, 5
  %mats = getelementptr inbounds %struct.BindlessScene, ptr addrspace(2) %scene, i64 0, i32 0
  %4 = load ptr addrspace(1), ptr addrspace(2) %mats, align 8
  %idxprom = zext i32 %3 to i64
  %agg.tmp4.sroa.0.0..sroa_idx = getelementptr inbounds %struct.Material, ptr addrspace(1) %4, i64 %idxprom, i32 1, i32 0
  %agg.tmp4.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(1) %agg.tmp4.sroa.0.0..sroa_idx, align 8
  %t.i = getelementptr inbounds %struct.Material, ptr addrspace(1) %4, i64 %idxprom, i32 0, i32 0
  %5 = load ptr addrspace(1), ptr addrspace(1) %t.i, align 8
  %6 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly %5, ptr addrspace(2) nocapture readonly %agg.tmp4.sroa.0.0.copyload, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0)
  br label %cond.end
cond.false:
  %agg.tmp12.sroa.0.0..sroa_idx = getelementptr inbounds %struct.BindlessScene, ptr addrspace(2) %scene, i64 0, i32 3, i32 0
  %agg.tmp12.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(2) %agg.tmp12.sroa.0.0..sroa_idx, align 8
  %t.i56 = getelementptr inbounds %struct.BindlessScene, ptr addrspace(2) %scene, i64 0, i32 2, i32 0
  %7 = load ptr addrspace(1), ptr addrspace(2) %t.i56, align 8
  %8 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly %7, ptr addrspace(2) nocapture readonly %agg.tmp12.sroa.0.0.copyload, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0)
  br label %cond.end
cond.end:
  %.pn = phi { <4 x float>, i8 } [ %6, %cond.true ], [ %8, %cond.false ]
  %cond = extractvalue { <4 x float>, i8 } %.pn, 0
  %idxprom15 = zext i32 %tid to i64
  %arrayidx16 = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %idxprom15
  store <4 x float> %cond, ptr addrspace(1) %arrayidx16, align 16
  ret void
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, ptr addrspace(1) readonly, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)
declare ptr addrspace(1) @air.get_null_intersection_function_table()
!air.kernel = !{!14}
!14 = !{ptr @k_bindless_nested, !15, !16}
!15 = !{}
!16 = !{!17, !27, !28}
!17 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 32, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !18, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"BindlessScene", !"air.arg_name", !"scene"}
!18 = !{i32 0, i32 8, i32 0, !"Material", !"mats", !"air.indirect_argument", !19, i32 8, i32 8, i32 0, !"acceleration_structure<instancing>", !"tlas", !"air.indirect_argument", !24, i32 16, i32 8, i32 0, !"texture2d<float, sample>", !"envMap", !"air.indirect_argument", !25, i32 24, i32 8, i32 0, !"sampler", !"envSmp", !"air.indirect_argument", !26}
!19 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.struct_type_info", !20, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"Material", !"air.arg_name", !"mats"}
!20 = !{i32 0, i32 8, i32 0, !"texture2d<float, sample>", !"albedo", !"air.indirect_argument", !21, i32 8, i32 8, i32 0, !"sampler", !"smp", !"air.indirect_argument", !22, i32 16, i32 16, i32 0, !"float4", !"tint", !"air.indirect_argument", !23}
!21 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"albedo"}
!22 = !{i32 1, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"smp"}
!23 = !{i32 2, !"air.indirect_constant", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"tint"}
!24 = !{i32 1, !"air.instance_acceleration_structure", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing>", !"air.arg_name", !"tlas"}
!25 = !{i32 2, !"air.texture", !"air.location_index", i32 2, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"envMap"}
!26 = !{i32 3, !"air.sampler", !"air.location_index", i32 3, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"envSmp"}
!27 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!28 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;
    let out_nested = call_stage3_with_caps(nested_ll, Some([1, 1, 1]), &[], Some(rt_caps));
    assert_eq!(out_nested.rc, RC_OK, "nested bindless translate failed: {}", out_nested.msg);
    let (spv_nested, meta_nested) = garnetxlate::lower_gpu_arguments_with_capabilities(
        &out_nested.spv,
        &out_nested.meta,
        rt_caps,
    )
    .expect("nested bindless lower_gpu_arguments failed");
    let meta_nested_json: Value = serde_json::from_slice(&meta_nested).unwrap();
    let cats_nested = meta_nested_json["argument_resource_tables"]["categories"]
        .as_array()
        .expect("nested bindless must emit argument_resource_tables.categories");
    assert!(cats_nested.iter().any(|v| v == 0), "missing texture category 0: {meta_nested_json}");
    assert!(cats_nested.iter().any(|v| v == 2), "missing sampler category 2: {meta_nested_json}");
    let asm_nested = spv_asm(&spv_nested);
    assert!(asm_nested.contains("OpConvertUToAccelerationStructureKHR"), "{asm_nested}");
    assert!(asm_nested.contains("OpRayQueryInitializeKHR"), "{asm_nested}");
    assert!(asm_nested.contains("DescriptorSet 2"), "{asm_nested}");
    assert!(asm_nested.contains("DescriptorSet 4"), "{asm_nested}");
    for (label, bin) in [("flat", &spv_flat), ("nested", &spv_nested)] {
        if let Ok(mut child) = std::process::Command::new("spirv-val")
            .args(["--target-env", "vulkan1.3", "-"])
            .stdin(std::process::Stdio::piped())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
        {
            use std::io::Write;
            child.stdin.as_mut().unwrap().write_all(bin).unwrap();
            let out = child.wait_with_output().unwrap();
            assert!(
                out.status.success(),
                "spirv-val rejected {label} bindless SPIR-V:\n{}",
                String::from_utf8_lossy(&out.stderr),
            );
        }
    }
}

#[test]
fn wave2_ift_abi_v2_attached_buffers_argbuf_texture_and_nested_vft() {
    assert_eq!(GARNETX_CAP_RT_IFT_ABI_V2, 1 << 10);
    let caps_v2 = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: GARNETX_CAP_RT_POSITION_FETCH | GARNETX_CAP_RT_IFT_ABI_V2,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };

    let entry_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
define void @k_ift_entry(ptr addrspace(1) %scene.coerce, ptr addrspace(1) %ift.coerce, ptr addrspace(1) nocapture writeonly "air-buffer-no-alias" %out) local_unnamed_addr {
entry:
  %0 = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float> zeroinitializer, <3 x float> <float 0.000000e+00, float 0.000000e+00, float -1.000000e+00>, float 0x3F50624DE0000000, float 1.000000e+02, ptr addrspace(1) readonly %scene.coerce, i32 255, ptr addrspace(1) readonly %ift.coerce, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 0, i32 0, i32 0, i1 false, i1 false)
  %1 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %0, 1
  store float %1, ptr addrspace(1) %out, align 4
  ret void
}
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, ptr addrspace(1) readonly, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)
!air.kernel = !{!14}
!14 = !{ptr @k_ift_entry, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19}
!17 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing>", !"air.arg_name", !"scene"}
!18 = !{i32 1, !"air.intersection_function_table", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"intersection_function_table<instancing, triangle_data>", !"air.arg_name", !"ift"}
!19 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
"#;

    let cb_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
%struct.AlphaArgBuf = type { %"struct.metal::texture2d", %"struct.metal::sampler" }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::sampler" = type { ptr addrspace(2) }
%struct.PrimRec = type { <4 x float> }
define <{ i1, float }> @ift_full_cb(<3 x float> %orig, <3 x float> %dir, float %tmin, float %tmax, i32 %pid, ptr addrspace(1) nocapture readonly "air-buffer-no-alias" %spheres, ptr addrspace(2) nocapture readonly align 8 dereferenceable(16) "air-buffer-no-alias" %alphaMat, ptr addrspace(1) nocapture readonly %vft.coerce, ptr addrspace(1) readonly "air-buffer-no-alias" %pdata) local_unnamed_addr {
entry:
  %0 = tail call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) nocapture readonly %vft.coerce, i32 0)
  %1 = bitcast ptr %0 to ptr
  %idxprom = zext i32 %pid to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(1) %spheres, i64 %idxprom
  %2 = load <4 x float>, ptr addrspace(1) %arrayidx, align 16
  %3 = extractelement <4 x float> %2, i64 3
  %call1 = tail call fast float %1(float %3)
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.AlphaArgBuf, ptr addrspace(2) %alphaMat, i64 0, i32 1, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(2) %agg.tmp.sroa.0.0..sroa_idx, align 8
  %t.i13 = getelementptr inbounds %struct.AlphaArgBuf, ptr addrspace(2) %alphaMat, i64 0, i32 0, i32 0
  %4 = load ptr addrspace(1), ptr addrspace(2) %t.i13, align 8
  %5 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly %4, ptr addrspace(2) nocapture readonly %agg.tmp.sroa.0.0.copyload, <2 x float> <float 5.000000e-01, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0)
  %tobool.not = icmp eq ptr addrspace(1) %pdata, null
  br i1 %tobool.not, label %cond.end, label %cond.true
cond.true:
  %info = getelementptr inbounds %struct.PrimRec, ptr addrspace(1) %pdata, i64 0, i32 0
  %6 = load <4 x float>, ptr addrspace(1) %info, align 16
  %7 = extractelement <4 x float> %6, i64 0
  br label %cond.end
cond.end:
  %cond = phi fast float [ %7, %cond.true ], [ 0.000000e+00, %entry ]
  %8 = extractvalue { <4 x float>, i8 } %5, 0
  %9 = extractelement <4 x float> %8, i64 0
  %cmp = fcmp fast ogt float %9, 5.000000e-01
  %add = fadd fast float %call1, %tmin
  %add4 = fadd fast float %add, %cond
  %10 = insertvalue <{ i1, float }> undef, i1 %cmp, 0
  %11 = insertvalue <{ i1, float }> %10, float %add4, 1
  ret <{ i1, float }> %11
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) nocapture readonly, ptr addrspace(2) nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) nocapture readonly, i32)
!air.intersection = !{!14}
!14 = !{ptr @ift_full_cb, !15, !18, !"air.bounding_box", !"air.instancing"}
!15 = !{!16, !17}
!16 = !{!"air.accept_intersection", !"air.arg_type_name", !"bool", !"air.arg_name", !"accept"}
!17 = !{!"air.distance", !"air.arg_type_name", !"float", !"air.arg_name", !"dist"}
!18 = !{!19, !20, !21, !22, !23, !24, !25, !29, !30}
!19 = !{i32 0, !"air.origin", !"air.arg_type_name", !"float3", !"air.arg_name", !"orig", !"air.arg_unused"}
!20 = !{i32 1, !"air.direction", !"air.arg_type_name", !"float3", !"air.arg_name", !"dir", !"air.arg_unused"}
!21 = !{i32 2, !"air.min_distance", !"air.arg_type_name", !"float", !"air.arg_name", !"tmin"}
!22 = !{i32 3, !"air.max_distance", !"air.arg_type_name", !"float", !"air.arg_name", !"tmax", !"air.arg_unused"}
!23 = !{i32 4, !"air.primitive_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"pid"}
!24 = !{i32 5, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"spheres"}
!25 = !{i32 6, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !26, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"AlphaArgBuf", !"air.arg_name", !"alphaMat"}
!26 = !{i32 0, i32 8, i32 0, !"texture2d<float, sample>", !"alphaTex", !"air.indirect_argument", !27, i32 8, i32 8, i32 0, !"sampler", !"alphaSmp", !"air.indirect_argument", !28}
!27 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"alphaTex"}
!28 = !{i32 1, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"alphaSmp"}
!29 = !{i32 7, !"air.visible_function_table", !"air.location_index", i32 2, i32 1, !"air.read", !"air.arg_type_name", !"visible_function_table", !"air.arg_name", !"vft"}
!30 = !{i32 8, !"air.primitive_data", !"air.arg_type_name", !"PrimRec", !"air.arg_name", !"pdata"}
"#;

    let vis_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
define float @eval_radius(float %base) local_unnamed_addr {
entry:
  %mul = fmul fast float %base, 5.000000e-01
  ret float %mul
}
!air.visible = !{!14}
!14 = !{ptr @eval_radius, !15, !17}
!15 = !{!16}
!16 = !{!"air.visible_output", !"air.arg_type_name", !"float"}
!17 = !{!18}
!18 = !{i32 0, !"air.visible_input", !"air.arg_type_name", !"float", !"air.arg_name", !"base"}
"#;

    let (spv, meta) = garnetxlate::translate_bytes_stage3(
        entry_ll.as_bytes(),
        [1, 1, 1],
        &[],
        &[],
        &[("eval_radius".to_string(), vis_ll.as_bytes().to_vec())],
        &[(
            "ift_full_cb".to_string(),
            metal2vulkan::linked_functions::IntersectionFunctionKind::BoundingBox,
            cb_ll.as_bytes().to_vec(),
        )],
        Some(caps_v2),
    )
    .expect("IFT ABI v2 full callback translate_bytes_stage3 failed");
    let (spv, meta) =
        garnetxlate::lower_gpu_arguments_with_capabilities(&spv, &meta, caps_v2)
            .expect("IFT ABI v2 lower_gpu_arguments failed");
    let meta_json: Value = serde_json::from_slice(&meta).unwrap();
    assert_eq!(meta_json["intersection_function_tables"]["abi_version"], 2, "{meta_json}");
    assert_eq!(meta_json["intersection_function_tables"]["header_words"], 66, "{meta_json}");
    assert_eq!(meta_json["visible_function_tables"]["handles"][0]["symbol"], "eval_radius", "{meta_json}");
    let cats = meta_json["argument_resource_tables"]["categories"]
        .as_array()
        .expect("IFT callback AlphaArgBuf must trigger argument_resource_tables");
    assert!(cats.iter().any(|v| v == 0), "missing texture category 0: {meta_json}");
    assert!(cats.iter().any(|v| v == 2), "missing sampler category 2: {meta_json}");
    let asm = spv_asm(&spv);
    assert!(asm.contains("OpRayQueryProceedKHR"), "{asm}");
    assert!(asm.contains("OpRayQueryGenerateIntersectionKHR"), "{asm}");
    assert!(asm.contains("DescriptorSet 2"), "{asm}");
    assert!(asm.contains("DescriptorSet 4"), "{asm}");
    assert!(asm.contains("Binding 640"), "missing BufferAddressTable binding 640: {asm}");

    // Validate generated SPIR-V with spirv-val when available on PATH.
    if let Ok(mut child) = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.3", "-"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
    {
        use std::io::Write;
        child.stdin.as_mut().unwrap().write_all(&spv).unwrap();
        let out = child.wait_with_output().unwrap();
        assert!(
            out.status.success(),
            "spirv-val rejected IFT ABI v2 SPIR-V:\n{}\n--- ASM ---\n{}",
            String::from_utf8_lossy(&out.stderr),
            asm
        );
    }

    // Also verify ABI v1 compatibility when GARNETX_CAP_RT_IFT_ABI_V2 is NOT set.
    let caps_v1 = RDNA4xBackendCapabilities {
        flags: GARNETX_CAP_RT_POSITION_FETCH,
        ..caps_v2
    };
    let (_, meta_v1) = garnetxlate::translate_bytes_stage3(
        entry_ll.as_bytes(),
        [1, 1, 1],
        &[],
        &[],
        &[("eval_radius".to_string(), vis_ll.as_bytes().to_vec())],
        &[(
            "ift_full_cb".to_string(),
            metal2vulkan::linked_functions::IntersectionFunctionKind::BoundingBox,
            cb_ll.as_bytes().to_vec(),
        )],
        Some(caps_v1),
    )
    .expect("IFT ABI v1 compat failed");
    let meta_v1_json: Value = serde_json::from_slice(&meta_v1).unwrap();
    assert_eq!(meta_v1_json["intersection_function_tables"]["abi_version"], 1, "{meta_v1_json}");
    assert_eq!(meta_v1_json["intersection_function_tables"]["header_words"], 1, "{meta_v1_json}");
}

#[test]
fn wave2_bindless_vft_in_struct_and_spirv_val() {
    let rt_caps = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: GARNETX_CAP_RT_POSITION_FETCH | GARNETX_CAP_RT_IFT_ABI_V2,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };
    let entry_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
%struct.VftHolder = type { ptr addrspace(1) }
define void @k_bindless_vft(ptr addrspace(2) nocapture readonly align 8 dereferenceable(8) %holder, ptr addrspace(1) nocapture writeonly %out, i32 %tid) local_unnamed_addr {
entry:
  %vptr = getelementptr inbounds %struct.VftHolder, ptr addrspace(2) %holder, i64 0, i32 0
  %vft = load ptr addrspace(1), ptr addrspace(2) %vptr, align 8
  %fp = tail call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) nocapture readonly %vft, i32 0)
  %res = tail call fast float %fp(float 4.000000e+00)
  %idx = zext i32 %tid to i64
  %dst = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idx
  store float %res, ptr addrspace(1) %dst, align 4
  ret void
}
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) nocapture readonly, i32)
!air.kernel = !{!0}
!0 = !{ptr @k_bindless_vft, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !6, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"VftHolder", !"air.arg_name", !"holder"}
!6 = !{i32 0, i32 8, i32 0, !"visible_function_table", !"vft", !"air.indirect_argument", !7}
!7 = !{i32 0, !"air.visible_function_table", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"visible_function_table", !"air.arg_name", !"vft"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;
    let vis_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
define float @eval_radius(float %base) local_unnamed_addr {
entry:
  %mul = fmul fast float %base, 5.000000e-01
  ret float %mul
}
!air.visible = !{!14}
!14 = !{ptr @eval_radius, !15, !17}
!15 = !{!16}
!16 = !{!"air.visible_output", !"air.arg_type_name", !"float"}
!17 = !{!18}
!18 = !{i32 0, !"air.visible_input", !"air.arg_type_name", !"float", !"air.arg_name", !"base"}
"#;
    let (spv, meta) = garnetxlate::translate_bytes_stage3(
        entry_ll.as_bytes(),
        [1, 1, 1],
        &[],
        &[],
        &[("eval_radius".to_string(), vis_ll.as_bytes().to_vec())],
        &[],
        Some(rt_caps),
    )
    .expect("bindless VFT struct translate failed");
    let meta_json: Value = serde_json::from_slice(&meta).unwrap();
    assert_eq!(meta_json["visible_function_tables"]["handles"][0]["symbol"], "eval_radius", "{meta_json}");
    let asm = spv_asm(&spv);
    assert!(asm.contains("Binding 640"), "missing BDA table: {asm}");
    if let Ok(mut child) = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.3", "-"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
    {
        use std::io::Write;
        child.stdin.as_mut().unwrap().write_all(&spv).unwrap();
        let out = child.wait_with_output().unwrap();
        assert!(
            out.status.success(),
            "spirv-val rejected bindless VFT SPIR-V:\n{}\n--- ASM ---\n{}",
            String::from_utf8_lossy(&out.stderr),
            asm
        );
    }
}

#[test]
fn wave2_bindless_holder_ift_abi_v2_and_spirv_val() {
    let rt_caps = RDNA4xBackendCapabilities {
        struct_size: 24,
        version: 1,
        flags: GARNETX_CAP_RT_POSITION_FETCH | GARNETX_CAP_RT_IFT_ABI_V2,
        subgroup_size: 32,
        subgroup_supported_stages: 0x10,
        subgroup_supported_operations: 0x80,
    };
    let entry_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
%struct.IftBindlessHolder = type { %"struct.metal::raytracing::_acceleration_structure", %"struct.metal::raytracing::intersection_function_table" }
%"struct.metal::raytracing::_acceleration_structure" = type { ptr addrspace(1) }
%"struct.metal::raytracing::intersection_function_table" = type { ptr addrspace(1) }

define void @k_ift_via_bindless_holder(ptr addrspace(2) readonly align 8 captures(none) dereferenceable(16) "air-buffer-no-alias" %holder, ptr addrspace(1) readonly captures(none) "air-buffer-no-alias" %rays_orig, ptr addrspace(1) writeonly captures(none) "air-buffer-no-alias" %out_dist, i32 %tid) local_unnamed_addr {
entry:
  %idxprom = zext i32 %tid to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(1) %rays_orig, i64 %idxprom
  %0 = load <4 x float>, ptr addrspace(1) %arrayidx, align 16
  %1 = shufflevector <4 x float> %0, <4 x float> poison, <3 x i32> <i32 0, i32 1, i32 2>
  %agg.tmp1.sroa.0.0..sroa_idx = getelementptr inbounds %struct.IftBindlessHolder, ptr addrspace(2) %holder, i64 0, i32 0, i32 0
  %agg.tmp1.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(2) %agg.tmp1.sroa.0.0..sroa_idx, align 8
  %agg.tmp2.sroa.0.0..sroa_idx = getelementptr inbounds %struct.IftBindlessHolder, ptr addrspace(2) %holder, i64 0, i32 1, i32 0
  %agg.tmp2.sroa.0.0.copyload = load ptr addrspace(1), ptr addrspace(2) %agg.tmp2.sroa.0.0..sroa_idx, align 8
  %2 = tail call { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float> %1, <3 x float> <float 0.000000e+00, float 0.000000e+00, float -1.000000e+00>, float 0x3F50624DE0000000, float 1.000000e+02, ptr addrspace(1) readonly %agg.tmp1.sroa.0.0.copyload, i32 -1, ptr addrspace(1) readonly %agg.tmp2.sroa.0.0.copyload, ptr null, i64 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 3, i32 0, i32 0, i32 0, i1 false, i1 false)
  %3 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %2, 0
  %4 = extractvalue { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } %2, 1
  %cmp.not = icmp eq i32 %3, 0
  %cond = select fast i1 %cmp.not, float -1.000000e+00, float %4
  %arrayidx4 = getelementptr inbounds float, ptr addrspace(1) %out_dist, i64 %idxprom
  store float %cond, ptr addrspace(1) %arrayidx4, align 4
  ret void
}
declare { i32, float, i32, i32, ptr addrspace(1), i32, i32, <2 x float>, i1 } @air.intersect.instancing.triangle_data(<3 x float>, <3 x float>, float, float, ptr addrspace(1) readonly, i32, ptr addrspace(1) readonly, ptr, i64, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1)
!air.kernel = !{!14}
!14 = !{ptr @k_ift_via_bindless_holder, !15, !16}
!15 = !{}
!16 = !{!17, !21, !22, !23}
!17 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !18, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"IftBindlessHolder", !"air.arg_name", !"holder"}
!18 = !{i32 0, i32 8, i32 0, !"acceleration_structure<instancing>", !"scene", !"air.indirect_argument", !19, i32 8, i32 8, i32 0, !"intersection_function_table<instancing, triangle_data>", !"ift", !"air.indirect_argument", !20}
!19 = !{i32 0, !"air.instance_acceleration_structure", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"acceleration_structure<instancing>", !"air.arg_name", !"scene"}
!20 = !{i32 1, !"air.intersection_function_table", !"air.location_index", i32 1, i32 1, !"air.read", !"air.arg_type_name", !"intersection_function_table<instancing, triangle_data>", !"air.arg_name", !"ift"}
!21 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"rays_orig"}
!22 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out_dist"}
!23 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
"#;
    let sphere_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
define <{ i1, float }> @sphere_vft_cb(<3 x float> %orig, <3 x float> %dir, float %tmin, float %tmax, i32 %pid, ptr addrspace(1) readonly captures(none) "air-buffer-no-alias" %spheres, ptr addrspace(1) readonly captures(none) %vft.coerce) local_unnamed_addr {
entry:
  %idxprom = zext i32 %pid to i64
  %arrayidx = getelementptr inbounds <4 x float>, ptr addrspace(1) %spheres, i64 %idxprom
  %0 = load <4 x float>, ptr addrspace(1) %arrayidx, align 16
  %1 = tail call ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) readonly captures(none) %vft.coerce, i32 0)
  %2 = bitcast ptr %1 to ptr
  %3 = extractelement <4 x float> %0, i64 3
  %call1 = tail call fast float %2(float %3)
  %10 = insertvalue <{ i1, float }> undef, i1 true, 0
  %11 = insertvalue <{ i1, float }> %10, float %call1, 1
  ret <{ i1, float }> %11
}
declare ptr @air.get_function_pointer_visible_function_table(ptr addrspace(1) readonly captures(none), i32)
!air.intersection = !{!14}
!14 = !{ptr @sphere_vft_cb, !15, !18, !"air.bounding_box", !"air.instancing", !"air.triangle_data"}
!15 = !{!16, !17}
!16 = !{!"air.accept_intersection", !"air.arg_type_name", !"bool", !"air.arg_name", !"accept"}
!17 = !{!"air.distance", !"air.arg_type_name", !"float", !"air.arg_name", !"distance"}
!18 = !{!19, !20, !21, !22, !23, !24, !25}
!19 = !{i32 0, !"air.origin", !"air.arg_type_name", !"float3", !"air.arg_name", !"orig"}
!20 = !{i32 1, !"air.direction", !"air.arg_type_name", !"float3", !"air.arg_name", !"dir"}
!21 = !{i32 2, !"air.min_distance", !"air.arg_type_name", !"float", !"air.arg_name", !"tmin"}
!22 = !{i32 3, !"air.max_distance", !"air.arg_type_name", !"float", !"air.arg_name", !"tmax"}
!23 = !{i32 4, !"air.primitive_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"pid"}
!24 = !{i32 5, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"spheres"}
!25 = !{i32 6, !"air.visible_function_table", !"air.location_index", i32 2, i32 1, !"air.read", !"air.arg_type_name", !"visible_function_table", !"air.arg_name", !"vft"}
"#;
    let alpha_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
%struct.AlphaArgBuf = type { %"struct.metal::texture2d", %"struct.metal::sampler" }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::sampler" = type { ptr addrspace(2) }
define i1 @tri_alpha_bindless_cb(<2 x float> %bary, i32 %pid, ptr addrspace(2) readonly align 8 captures(none) dereferenceable(16) "air-buffer-no-alias" %mat) local_unnamed_addr {
entry:
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.AlphaArgBuf, ptr addrspace(2) %mat, i64 0, i32 1, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(2) %agg.tmp.sroa.0.0..sroa_idx, align 8
  %vecinit1 = insertelement <2 x float> %bary, float 5.000000e-01, i64 1
  %t.i = getelementptr inbounds %struct.AlphaArgBuf, ptr addrspace(2) %mat, i64 0, i32 0, i32 0
  %0 = load ptr addrspace(1), ptr addrspace(2) %t.i, align 8
  %1 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %0, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <2 x float> %vecinit1, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0)
  %2 = extractvalue { <4 x float>, i8 } %1, 0
  %3 = extractelement <4 x float> %2, i64 3
  %cmp = fcmp fast ogt float %3, 5.000000e-01
  ret i1 %cmp
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
!air.intersection = !{!14}
!14 = !{ptr @tri_alpha_bindless_cb, !15, !17, !"air.triangle", !"air.instancing", !"air.triangle_data"}
!15 = !{!16}
!16 = !{!"air.accept_intersection", !"air.arg_type_name", !"bool"}
!17 = !{!18, !19, !20}
!18 = !{i32 0, !"air.barycentric_coord", !"air.arg_type_name", !"float2", !"air.arg_name", !"bary"}
!19 = !{i32 1, !"air.primitive_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"pid"}
!20 = !{i32 2, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !21, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"AlphaBindlessBuf", !"air.arg_name", !"mat"}
!21 = !{i32 0, i32 8, i32 0, !"texture2d<float, sample>", !"alphaTex", !"air.indirect_argument", !22, i32 8, i32 8, i32 0, !"sampler", !"alphaSmp", !"air.indirect_argument", !23}
!22 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"alphaTex"}
!23 = !{i32 1, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"alphaSmp"}
"#;
    let vis_ll = r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"
define float @scale_radius_half(float %r) local_unnamed_addr {
entry:
  %mul = fmul fast float %r, 5.000000e-01
  ret float %mul
}
!air.visible = !{!14}
!14 = !{ptr @scale_radius_half, !15, !17}
!15 = !{!16}
!16 = !{!"air.visible_output", !"air.arg_type_name", !"float"}
!17 = !{!18}
!18 = !{i32 0, !"air.visible_input", !"air.arg_type_name", !"float", !"air.arg_name", !"r"}
"#;
    let (spv, meta) = garnetxlate::translate_bytes_stage3(
        entry_ll.as_bytes(),
        [1, 1, 1],
        &[],
        &[],
        &[("scale_radius_half".to_string(), vis_ll.as_bytes().to_vec())],
        &[
            (
                "sphere_vft_cb".to_string(),
                metal2vulkan::linked_functions::IntersectionFunctionKind::BoundingBox,
                sphere_ll.as_bytes().to_vec(),
            ),
            (
                "tri_alpha_bindless_cb".to_string(),
                metal2vulkan::linked_functions::IntersectionFunctionKind::TriangleAnyHit,
                alpha_ll.as_bytes().to_vec(),
            ),
        ],
        Some(rt_caps),
    )
    .expect("bindless holder IFT ABI v2 translate failed");
    let meta_json: Value = serde_json::from_slice(&meta).unwrap();
    assert_eq!(meta_json["intersection_function_tables"]["abi_version"], 2, "{meta_json}");
    let asm = spv_asm(&spv);
    assert!(asm.contains("Binding 640"), "missing BDA table: {asm}");
    if let Ok(mut child) = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.3", "-"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
    {
        use std::io::Write;
        child.stdin.as_mut().unwrap().write_all(&spv).unwrap();
        let out = child.wait_with_output().unwrap();
        assert!(
            out.status.success(),
            "spirv-val rejected bindless holder IFT ABI v2 SPIR-V:\n{}\n--- ASM ---\n{}",
            String::from_utf8_lossy(&out.stderr),
            asm
        );
    }
}
