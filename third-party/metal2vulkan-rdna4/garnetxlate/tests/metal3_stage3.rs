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
    GARNETX_IFT_KIND_BOUNDING_BOX, GARNETX_IFT_KIND_TRIANGLE_ANY_HIT,
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
        flags: 1 << 9,
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
