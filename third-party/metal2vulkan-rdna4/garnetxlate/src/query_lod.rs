//! Capability-aware ABI 3 entry points and LOD extension policy.

use super::*;

/// Capability flag enabling GFX-ring emulated Object+Mesh payload mode (`GARNETX_CAP_MESH_EMULATED_PAYLOAD`).
pub const GARNETX_CAP_MESH_EMULATED_PAYLOAD: u32 = 1 << 8;
/// Capability flag advertising device-enabled `VK_KHR_ray_tracing_position_fetch` (`GARNETX_CAP_RT_POSITION_FETCH`).
pub const GARNETX_CAP_RT_POSITION_FETCH: u32 = 1 << 9;
/// Capability flag enabling IFT backing buffer ABI v2 (66-word header with `attached_bda[31]`).
pub const GARNETX_CAP_RT_IFT_ABI_V2: u32 = 1 << 10;
/// Capability flag enabling RT `primitiveDataBuffer` GPU lookup via binding 676.
pub const GARNETX_CAP_RT_PRIMITIVE_DATA: u32 = 1 << 11;
/// Capability flag indicating native Vulkan `shaderInt64` arithmetic (skip u32-half i64 emulation).
pub const GARNETX_CAP_NATIVE_INT64: u32 = 1 << 12;

/// Set-0 `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` bindings for emulated Object+Mesh stages and RT prim data.
pub const GARNETX_MESH_GRID_PARAMS_BINDING: u32 =
    metal2vulkan::reflect::MESH_EMULATED_GRID_PARAMS_BINDING;
pub const GARNETX_MESH_PAYLOAD_BINDING: u32 = metal2vulkan::reflect::MESH_EMULATED_PAYLOAD_BINDING;
pub const GARNETX_MESH_INDIRECT_BINDING: u32 =
    metal2vulkan::reflect::MESH_EMULATED_INDIRECT_BINDING;
pub const GARNETX_RT_PRIM_DATA_BINDING: u32 = metal2vulkan::reflect::RT_PRIM_DATA_BINDING;

const KNOWN_CAPABILITY_FLAGS: u32 = 3
    | GARNETX_CAP_MESH_EMULATED_PAYLOAD
    | GARNETX_CAP_RT_POSITION_FETCH
    | GARNETX_CAP_RT_IFT_ABI_V2
    | GARNETX_CAP_RT_PRIMITIVE_DATA
    | GARNETX_CAP_NATIVE_INT64;

/// Caller-proven language/device support and actual Vulkan subgroup properties (ABI 3).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RDNA4xBackendCapabilities {
    pub struct_size: u32,
    pub version: u32,
    /// Bit 0: actual platform compiler exposes query LOD; bit 1: actual device support;
    /// bit 8: `GARNETX_CAP_MESH_EMULATED_PAYLOAD` (GFX-ring emulated Object+Mesh mode).
    pub flags: u32,
    pub subgroup_size: u32,
    pub subgroup_supported_stages: u32,
    pub subgroup_supported_operations: u32,
}

impl RDNA4xBackendCapabilities {
    pub(crate) fn validate(self) -> Result<Self, String> {
        if self.struct_size != 24
            || self.version != 1
            || self.flags & !KNOWN_CAPABILITY_FLAGS != 0
        {
            return Err("invalid ABI 3 backend capabilities".into());
        }
        Ok(self)
    }

    pub(crate) fn emulated_mesh_payload(self) -> bool {
        self.flags & GARNETX_CAP_MESH_EMULATED_PAYLOAD != 0
    }

    pub(crate) fn ift_abi_v2(self) -> bool {
        self.flags & GARNETX_CAP_RT_IFT_ABI_V2 != 0
    }

    pub(crate) fn rt_primitive_data(self) -> bool {
        self.flags & GARNETX_CAP_RT_PRIMITIVE_DATA != 0
    }

    pub(crate) fn native_int64(self) -> bool {
        self.flags & GARNETX_CAP_NATIVE_INT64 != 0
    }

    pub(crate) fn replay(self) -> QueryLodReplay {
        if self.subgroup_size >= 4
            && self.subgroup_supported_stages & 0x10 != 0
            && self.subgroup_supported_operations & 0x80 != 0
        {
            QueryLodReplay::QuadBroadcast
        } else {
            QueryLodReplay::DescriptorEnumeration
        }
    }
}

pub(crate) fn gate_query_lod(
    spv: &[u8],
    capabilities: RDNA4xBackendCapabilities,
) -> Result<bool, String> {
    if spv.len() < 20 || !spv.len().is_multiple_of(4) || spv[..4] != [3, 2, 35, 7] {
        return Err("invalid query LOD SPIR-V".into());
    }
    let words = spv
        .chunks_exact(4)
        .map(|word| u32::from_le_bytes(word.try_into().unwrap()))
        .collect::<Vec<_>>();
    let mut cursor = 5;
    let mut has_query = false;
    while cursor < words.len() {
        let count = (words[cursor] >> 16) as usize;
        if count == 0 || count > words.len() - cursor {
            return Err("invalid query LOD SPIR-V instruction".into());
        }
        // SPIR-V core opcode 105 is OpImageQueryLod.
        if words[cursor] & 0xffff == 105 {
            has_query = true;
            if (capabilities.flags & 3) != 3 {
                return Err(
                    "query texture LOD extension requires actual language and device support"
                        .into(),
                );
            }
        }
        cursor += count;
    }
    Ok(has_query)
}

/// ABI 3 capability-aware counterpart; the ABI 2 function keeps its original signature.
///
/// # Safety
/// All pointers must be valid for their stated lengths; capabilities points to a readable 24-byte record.
#[no_mangle]
pub unsafe extern "C" fn garnetx_translate_with_capabilities(
    ll: *const u8,
    ll_len: usize,
    local_xyz: *const u32,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
    capabilities: *const RDNA4xBackendCapabilities,
) -> c_int {
    if !spv.is_null() {
        *spv = std::ptr::null_mut();
    }
    if !spv_len.is_null() {
        *spv_len = 0;
    }
    if !meta.is_null() {
        *meta = std::ptr::null_mut();
    }
    if !meta_len.is_null() {
        *meta_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    let capabilities = match capabilities
        .as_ref()
        .copied()
        .and_then(|caps| caps.validate().ok())
    {
        Some(capabilities) => capabilities,
        None => {
            put_err(err, err_len, "invalid ABI 3 backend capabilities");
            return RC_BAD_ARGS;
        }
    };
    if ll.is_null()
        || ll_len == 0
        || ll_len > MAX_LL_BYTES
        || spv.is_null()
        || spv_len.is_null()
        || meta.is_null()
        || meta_len.is_null()
    {
        put_err(err, err_len, "bad arguments");
        return RC_BAD_ARGS;
    }
    let local = if local_xyz.is_null() {
        [64u32, 1, 1]
    } else {
        let l = [*local_xyz, *local_xyz.add(1), *local_xyz.add(2)];
        if l.contains(&0) {
            put_err(err, err_len, "bad arguments: --local has a zero dimension");
            return RC_BAD_ARGS;
        }
        l
    };
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        translate_bytes_with_capabilities(bytes, local, None, capabilities)
    }));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}

/// ABI 3 capability-aware counterpart; the ABI 2 function keeps its original signature.
///
/// # Safety
/// All pointers must be valid for their stated lengths; capabilities points to a readable 24-byte record.
#[no_mangle]
pub unsafe extern "C" fn garnetx_translate_linked_with_capabilities(
    ll: *const u8,
    ll_len: usize,
    local_xyz: *const u32,
    deps: *const RDNA4xDep,
    ndeps: usize,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
    capabilities: *const RDNA4xBackendCapabilities,
) -> c_int {
    if !spv.is_null() {
        *spv = std::ptr::null_mut();
    }
    if !spv_len.is_null() {
        *spv_len = 0;
    }
    if !meta.is_null() {
        *meta = std::ptr::null_mut();
    }
    if !meta_len.is_null() {
        *meta_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    let capabilities = match capabilities
        .as_ref()
        .copied()
        .and_then(|caps| caps.validate().ok())
    {
        Some(capabilities) => capabilities,
        None => {
            put_err(err, err_len, "invalid ABI 3 backend capabilities");
            return RC_BAD_ARGS;
        }
    };
    if ll.is_null()
        || ll_len == 0
        || ll_len > MAX_LL_BYTES
        || spv.is_null()
        || spv_len.is_null()
        || meta.is_null()
        || meta_len.is_null()
        || (ndeps > 0 && deps.is_null())
        || ndeps > MAX_DEPS
    {
        put_err(err, err_len, "bad arguments");
        return RC_BAD_ARGS;
    }
    let local = if local_xyz.is_null() {
        [64u32, 1, 1]
    } else {
        let l = [*local_xyz, *local_xyz.add(1), *local_xyz.add(2)];
        if l.contains(&0) {
            put_err(err, err_len, "bad arguments: --local has a zero dimension");
            return RC_BAD_ARGS;
        }
        l
    };
    let mut total = ll_len;
    let mut owned: Vec<(String, Vec<u8>)> = Vec::with_capacity(ndeps);
    for i in 0..ndeps {
        let d = &*deps.add(i);
        if d.symbol.is_null() || d.symbol_len == 0 || d.ll.is_null() || d.ll_len == 0 {
            put_err(err, err_len, "bad arguments: empty dependency");
            return RC_BAD_ARGS;
        }
        total = total.saturating_add(d.ll_len);
        if total > MAX_LL_BYTES {
            put_err(err, err_len, "bad arguments: dependencies too large");
            return RC_BAD_ARGS;
        }
        let sym = match std::str::from_utf8(std::slice::from_raw_parts(d.symbol, d.symbol_len)) {
            Ok(x) => x.to_string(),
            Err(_) => {
                put_err(
                    err,
                    err_len,
                    "bad arguments: dependency symbol is not UTF-8",
                );
                return RC_BAD_ARGS;
            }
        };
        owned.push((sym, std::slice::from_raw_parts(d.ll, d.ll_len).to_vec()));
    }
    let bytes = std::slice::from_raw_parts(ll, ll_len);
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        translate_bytes_with_capabilities(bytes, local, Some(&owned), capabilities)
    }));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}

/// ABI 3 capability-aware counterpart; the ABI 2 function keeps its original signature.
///
/// # Safety
/// All pointers must be valid for their stated lengths; capabilities points to a readable 24-byte record.
#[no_mangle]
pub unsafe extern "C" fn garnetx_lower_argument_resources_with_capabilities(
    input_spv: *const u8,
    input_spv_len: usize,
    input_meta: *const u8,
    input_meta_len: usize,
    spv: *mut *mut u8,
    spv_len: *mut usize,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    err: *mut c_char,
    err_len: usize,
    capabilities: *const RDNA4xBackendCapabilities,
) -> c_int {
    if !spv.is_null() {
        *spv = std::ptr::null_mut();
    }
    if !spv_len.is_null() {
        *spv_len = 0;
    }
    if !meta.is_null() {
        *meta = std::ptr::null_mut();
    }
    if !meta_len.is_null() {
        *meta_len = 0;
    }
    if !err.is_null() && err_len > 0 {
        *err = 0;
    }
    let capabilities = match capabilities
        .as_ref()
        .copied()
        .and_then(|caps| caps.validate().ok())
    {
        Some(capabilities) => capabilities,
        None => {
            put_err(err, err_len, "invalid ABI 3 backend capabilities");
            return RC_BAD_ARGS;
        }
    };
    if input_spv.is_null()
        || !(20..=crate::MAX_SPV_BYTES).contains(&input_spv_len)
        || !input_spv_len.is_multiple_of(4)
        || input_meta.is_null()
        || input_meta_len == 0
        || input_meta_len > 8 * 1024 * 1024
        || spv.is_null()
        || spv_len.is_null()
        || meta.is_null()
        || meta_len.is_null()
    {
        put_err(err, err_len, "bad GPU argument lowering inputs");
        return RC_BAD_ARGS;
    }
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        lower_gpu_arguments_with_capabilities(
            std::slice::from_raw_parts(input_spv, input_spv_len),
            std::slice::from_raw_parts(input_meta, input_meta_len),
            capabilities,
        )
    }));
    finish(outcome, spv, spv_len, meta, meta_len, err, err_len)
}

pub(crate) fn source_language_version(source: &str) -> Option<[u32; 3]> {
    let reference = source
        .lines()
        .find_map(|line| {
            let (key, value) = line.split_once('=')?;
            (key.trim() == "!air.language_version").then_some(value.trim())
        })?
        .strip_prefix("!{")?
        .strip_suffix('}')?
        .trim();
    let values = source
        .lines()
        .find_map(|line| {
            let (key, value) = line.split_once('=')?;
            (key.trim() == reference).then_some(value.trim())
        })?
        .strip_prefix("!{")?
        .strip_suffix('}')?;
    let mut fields = values.split(',').map(str::trim);
    if fields.next()? != "!\"Metal\"" {
        return None;
    }
    let mut version = [0; 3];
    for component in &mut version {
        *component = fields.next()?.strip_prefix("i32 ")?.trim().parse().ok()?;
    }
    fields.next().is_none().then_some(version)
}

fn query_replay_name(replay: QueryLodReplay) -> &'static str {
    match replay {
        QueryLodReplay::UniformOnly => "uniform-only",
        QueryLodReplay::QuadBroadcast => "quad-broadcast",
        QueryLodReplay::DescriptorEnumeration => "descriptor-enumeration",
    }
}

pub(crate) fn gate_query_metadata(metadata: &Value) -> Result<(), String> {
    let version = metadata["source_language_version"]
        .as_array()
        .and_then(|components| {
            let [major, minor, patch] = components.as_slice() else {
                return None;
            };
            Some([major.as_u64()?, minor.as_u64()?, patch.as_u64()?])
        })
        .ok_or("query texture LOD extension requires explicit Metal language metadata")?;
    if version < [2, 2, 0] || metadata["stage"] != "Fragment" {
        return Err(
            "query texture LOD extension requires fragment Metal 2.2 or newer on macOS".into(),
        );
    }
    Ok(())
}

pub(crate) fn stamp_query_policy(metadata: &mut Value, capabilities: RDNA4xBackendCapabilities) {
    metadata["query_lod_policy"] = serde_json::json!(1);
    metadata["query_lod_replay"] = serde_json::json!(query_replay_name(capabilities.replay()));
}

fn translate_bytes_with_capabilities(
    ll: &[u8],
    local: [u32; 3],
    deps: Option<&[(String, Vec<u8>)]>,
    capabilities: RDNA4xBackendCapabilities,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    #[cfg(feature = "test-hooks")]
    {
        if ll.starts_with(b";RDNA4X-TEST-PANIC") {
            panic!("rdna4x test hook panic");
        }
        if deps.is_none() && ll.starts_with(b";RDNA4X-TEST-EXIT") {
            std::process::exit(77);
        }
    }
    let text = std::str::from_utf8(ll).map_err(|error| error.to_string())?;
    let dependencies = deps
        .map(|deps| {
            deps.iter()
                .map(|(symbol, bytes)| {
                    std::str::from_utf8(bytes)
                        .map(|text| (symbol.as_str(), text))
                        .map_err(|error| error.to_string())
                })
                .collect::<Result<Vec<_>, _>>()
        })
        .transpose()?;
    let (spv, meta) = translate_text_with_rt_options(
        text,
        local,
        dependencies.as_deref(),
        true,
        capabilities.replay(),
        capabilities.emulated_mesh_payload(),
        capabilities.rt_primitive_data(),
        capabilities.native_int64(),
    )?;
    let has_query = gate_query_lod(&spv, capabilities)?;
    let mut metadata: Value = serde_json::from_slice(&meta).map_err(|error| error.to_string())?;
    if let Some(version) = source_language_version(text) {
        metadata["source_language_version"] = serde_json::json!(version);
    }
    if has_query {
        gate_query_metadata(&metadata)?;
    }
    stamp_query_policy(&mut metadata, capabilities);
    Ok((
        spv,
        serde_json::to_vec(&metadata).map_err(|error| error.to_string())?,
    ))
}

pub fn lower_gpu_arguments_with_capabilities(
    spv: &[u8],
    metadata: &[u8],
    capabilities: RDNA4xBackendCapabilities,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    let capabilities = capabilities.validate()?;
    let has_query = gate_query_lod(spv, capabilities)?;
    let mut facts: Value = serde_json::from_slice(metadata).map_err(|error| error.to_string())?;
    if has_query {
        gate_query_metadata(&facts)?;
        if facts.get("argument_resource_tables").is_some()
            && (facts["query_lod_policy"] != 1
                || facts["query_lod_replay"] != query_replay_name(capabilities.replay()))
        {
            return Err(
                "cached query LOD replay policy or backend mismatch; retranslate AIR".into(),
            );
        }
    }
    stamp_query_policy(&mut facts, capabilities);
    let metadata = serde_json::to_vec(&facts).map_err(|error| error.to_string())?;
    lower_gpu_arguments_with_table_version_and_replay(
        spv,
        &metadata,
        Some(8),
        capabilities.replay(),
    )
}
