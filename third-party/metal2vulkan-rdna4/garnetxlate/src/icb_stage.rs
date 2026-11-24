// SPDX-License-Identifier: LGPL-3.0-or-later
//! C ABI entry point for lowering render and compute stage SPIR-V to ICB dynamic BDA bindings.
//! `stage_kind`: 0 = Vertex, 1 = Fragment, 2 = TessellationEvaluation, 3 = Compute (`GLCompute`).

use crate::{leak, put_err, RC_BAD_ARGS, RC_OK, RC_PANIC, RC_REFUSED};
use metal2vulkan::passes::icb_stage::{lower_icb_stage, IcbVertexAttr};
use std::ffi::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};

#[no_mangle]
pub unsafe extern "C" fn garnetx_lower_icb_stage(
    spv: *const u8,
    spv_len: usize,
    stage_kind: u32,
    stage_set: u32,
    vattrs: *const IcbVertexAttr,
    nvattrs: usize,
    out_spv: *mut *mut u8,
    out_spv_len: *mut usize,
    out_table_binding: *mut u32,
    out_vs_params_binding: *mut u32,
    err: *mut c_char,
    err_len: usize,
) -> c_int {
    if !out_spv.is_null() {
        *out_spv = std::ptr::null_mut();
    }
    if !out_spv_len.is_null() {
        *out_spv_len = 0;
    }
    if !out_table_binding.is_null() {
        *out_table_binding = 0;
    }
    if !out_vs_params_binding.is_null() {
        *out_vs_params_binding = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    if spv.is_null()
        || spv_len < 20
        || !spv_len.is_multiple_of(4)
        || (nvattrs > 0 && vattrs.is_null())
        || nvattrs > 32
        || out_spv.is_null()
        || out_spv_len.is_null()
        || out_table_binding.is_null()
        || out_vs_params_binding.is_null()
    {
        put_err(err, err_len, "bad arguments to garnetx_lower_icb_stage");
        return RC_BAD_ARGS;
    }
    let spv_slice = std::slice::from_raw_parts(spv, spv_len);
    let attrs_slice = if nvattrs > 0 {
        std::slice::from_raw_parts(vattrs, nvattrs)
    } else {
        &[]
    };
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        lower_icb_stage(spv_slice, stage_kind, stage_set, attrs_slice)
    }));
    match outcome {
        Ok(Ok((bytes, tbl_b, vs_b))) => {
            let (p, n) = leak(bytes);
            *out_spv = p;
            *out_spv_len = n;
            *out_table_binding = tbl_b;
            *out_vs_params_binding = vs_b;
            RC_OK
        }
        Ok(Err(e)) => {
            put_err(err, err_len, &e);
            RC_REFUSED
        }
        Err(p) => {
            let why = if let Some(s) = p.downcast_ref::<&str>() {
                (*s).to_string()
            } else if let Some(s) = p.downcast_ref::<String>() {
                s.clone()
            } else {
                "unknown panic".to_string()
            };
            put_err(err, err_len, &format!("panic: {why}"));
            RC_PANIC
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use metal2vulkan::passes::icb_stage::{lower_icb_stage, RDNA4_ICB_TABLE_BINDING};

    #[test]
    fn ffi_lower_icb_stage_rejects_bad_args() {
        let mut out_spv = std::ptr::null_mut();
        let mut out_len = 0usize;
        let mut tbl = 0u32;
        let mut vsp = 0u32;
        let mut err = [0i8; 128];
        let rc = unsafe {
            garnetx_lower_icb_stage(
                std::ptr::null(),
                0,
                0,
                0,
                std::ptr::null(),
                0,
                &mut out_spv,
                &mut out_len,
                &mut tbl,
                &mut vsp,
                err.as_mut_ptr(),
                err.len(),
            )
        };
        assert_eq!(rc, RC_BAD_ARGS);
        let _ = (lower_icb_stage, RDNA4_ICB_TABLE_BINDING);
    }
}
