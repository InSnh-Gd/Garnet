use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;

const LARGE_BUFFER_BDA: &str = include_str!("fixtures/large_buffer_bda.ll");
const LARGE_BUFFER_ATOMIC: &str = include_str!("fixtures/large_buffer_atomic.ll");
const LARGE_BUFFER_ADDRSPACE2_DATA: &str = include_str!("fixtures/large_buffer_addrspace2_data.ll");
const LARGE_BUFFER_ADDRSPACE2_DEVICE_POINTERS: &str =
    include_str!("fixtures/large_buffer_addrspace2_device_pointers.ll");
const LARGE_BUFFER_CROSS_ROOT_PHI: &str = include_str!("fixtures/large_buffer_cross_root_phi.ll");
const LARGE_BUFFER_CROSS_ROOT_SELECT: &str =
    include_str!("fixtures/large_buffer_cross_root_select.ll");
const LARGE_BUFFER_INTEGER_ROUNDTRIP: &str =
    include_str!("fixtures/large_buffer_integer_roundtrip.ll");
const LARGE_BUFFER_MIXED_POINTER_SELECT: &str =
    include_str!("fixtures/large_buffer_mixed_pointer_select.ll");
const LARGE_BUFFER_LOOP_PHI: &str = include_str!("fixtures/large_buffer_loop_phi.ll");
const LARGE_BUFFER_NESTED_MIXED_POINTER_SELECT: &str =
    include_str!("fixtures/large_buffer_nested_mixed_pointer_select.ll");
const LARGE_BUFFER_POINTER_MERGE: &str = include_str!("fixtures/large_buffer_pointer_merge.ll");
const LARGE_BUFFER_SCALED_CONSTANT: &str = include_str!("fixtures/large_buffer_scaled_constant.ll");
const LARGE_BUFFER_SCALED_DYNAMIC: &str = include_str!("fixtures/large_buffer_scaled_dynamic.ll");
const LARGE_BUFFER_SCALED_DYNAMIC_NO_LAYOUT: &str =
    include_str!("fixtures/large_buffer_scaled_dynamic_no_layout.ll");
const LARGE_BUFFER_CHAINED_CONSTANT: &str =
    include_str!("fixtures/large_buffer_chained_constant.ll");
const LARGE_BUFFER_INBOUNDS_SINGLETON: &str =
    include_str!("fixtures/large_buffer_inbounds_singleton.ll");
const LARGE_BUFFER_INBOUNDS_ZERO_ARRAY: &str =
    include_str!("fixtures/large_buffer_inbounds_zero_array.ll");
const LARGE_BUFFER_WRAPPING_I32: &str = include_str!("fixtures/large_buffer_wrapping_i32.ll");
const LARGE_BUFFER_NULLABLE_MERGE: &str = include_str!("fixtures/large_buffer_nullable_merge.ll");
const LARGE_BUFFER_UNALIGNED_SCALAR: &str =
    include_str!("fixtures/large_buffer_unaligned_scalar.ll");
const LARGE_BUFFER_VEC3_TAIL: &str = include_str!("fixtures/large_buffer_vec3_tail.ll");
const ORDINARY_I32: &str = include_str!("fixtures/ordinary_i32.ll");
const LEGACY_BDA_V1: &str = include_str!("fixtures/legacy_bda_v1.ll");

fn evidence_dir(label: &str) -> PathBuf {
    std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            std::env::temp_dir().join(format!("rdna4-{label}-{}", std::process::id()))
        })
}

fn validate_and_disassemble(label: &str, spv: &[u8]) -> (String, PathBuf) {
    let out = evidence_dir(label);
    std::fs::create_dir_all(&out).unwrap();
    let spv_path = out.join(format!("{label}.spv"));
    std::fs::write(&spv_path, spv).unwrap();

    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&spv_path)
        .output()
        .unwrap();
    std::fs::write(
        out.join(format!("{label}-spirv-val.log")),
        format!(
            "status={}\n{}{}",
            validation.status,
            String::from_utf8_lossy(&validation.stdout),
            String::from_utf8_lossy(&validation.stderr)
        ),
    )
    .unwrap();
    assert!(
        validation.status.success(),
        "spirv-val failed: {}",
        String::from_utf8_lossy(&validation.stderr)
    );

    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&spv_path)
        .output()
        .unwrap();
    assert!(
        disassembly.status.success(),
        "spirv-dis failed: {}",
        String::from_utf8_lossy(&disassembly.stderr)
    );
    let text = String::from_utf8(disassembly.stdout).unwrap();
    std::fs::write(out.join(format!("{label}.spvasm")), &text).unwrap();
    (text, out)
}

fn has_aligned_load(text: &str, ty: &str, alignment: u32) -> bool {
    text.lines().any(|line| {
        line.contains("OpLoad")
            && line.contains(ty)
            && line.contains(&format!("Aligned {alignment}"))
    })
}

fn aligned_operation_count(text: &str, opcode: &str, alignment: u32) -> usize {
    text.lines()
        .filter(|line| line.contains(opcode) && line.contains(&format!("Aligned {alignment}")))
        .count()
}

fn long_constants(text: &str) -> HashMap<&str, u64> {
    text.lines()
        .filter_map(|line| {
            let words = line.split_whitespace().collect::<Vec<_>>();
            match words.as_slice() {
                [id, "=", "OpConstant", "%ulong", value] => Some((*id, value.parse().ok()?)),
                _ => None,
            }
        })
        .collect()
}

fn long_comparison_constants(text: &str, opcode: &str) -> Vec<u64> {
    let constants = long_constants(text);
    text.lines()
        .filter(|line| line.contains(opcode))
        .filter_map(|line| {
            let rhs = line.split_whitespace().last()?;
            constants.get(rhs).copied()
        })
        .collect()
}

fn has_buffer_address_table(meta: &Value) -> bool {
    meta["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .any(|binding| binding["kind"] == "BufferAddressTable")
}

fn assert_registry_metadata(meta: &Value) {
    assert_eq!(meta["buffer_address_table_version"], 3, "{meta}");
    assert_eq!(meta["buffer_address_table_bytes"], 800);
    assert_eq!(meta["buffer_address_table_row_stride_bytes"], 24);
    assert_eq!(meta["buffer_address_table_dynamic_extent"], true);
}

fn assert_registry_guard_selected(text: &str) {
    let calls = text
        .lines()
        .filter_map(|line| {
            let words = line.split_whitespace().collect::<Vec<_>>();
            (words.get(2) == Some(&"OpFunctionCall") && words.get(3) == Some(&"%bool"))
                .then(|| words[0])
        })
        .collect::<HashSet<_>>();
    let guards = text
        .lines()
        .filter_map(|line| {
            let words = line.split_whitespace().collect::<Vec<_>>();
            (words.get(2) == Some(&"OpSelect")
                && words.get(3) == Some(&"%bool")
                && words.last().is_some_and(|id| calls.contains(id)))
            .then(|| words[0])
        })
        .collect::<HashSet<_>>();
    assert!(
        !guards.is_empty(),
        "mixed pointer lost its selected registry guard:\n{text}"
    );
    assert!(
        text.lines().any(|line| {
            let words = line.split_whitespace().collect::<Vec<_>>();
            words.first() == Some(&"OpBranchConditional")
                && words.get(1).is_some_and(|id| guards.contains(id))
        }),
        "selected registry result does not guard the access:\n{text}"
    );
}

#[derive(Default, Debug)]
struct GuardedAccessStats {
    selection_merges: usize,
    greater_equal: usize,
    less_equal: usize,
    access_matches: usize,
    guarded_loads: usize,
    guarded_stores: usize,
    guarded_atomics: usize,
    unguarded_loads: usize,
    unguarded_stores: usize,
    unguarded_atomics: usize,
}

fn u32_constants(text: &str) -> HashMap<&str, u32> {
    text.lines()
        .filter_map(|line| {
            let words = line.split_whitespace().collect::<Vec<_>>();
            match words.as_slice() {
                [id, "=", "OpConstant", "%uint", value] => Some((*id, value.parse().ok()?)),
                _ => None,
            }
        })
        .collect()
}

fn access_chain_rows(text: &str) -> HashSet<(u32, u32)> {
    let constants = u32_constants(text);
    text.lines()
        .filter(|line| line.contains("OpAccessChain"))
        .filter_map(|line| {
            let words = line.split_whitespace().collect::<Vec<_>>();
            let row = constants.get(words.get(words.len().checked_sub(2)?)?)?;
            let component = constants.get(words.last()?)?;
            Some((*row, *component))
        })
        .collect()
}

fn guarded_access_stats(text: &str) -> GuardedAccessStats {
    let mut stats = GuardedAccessStats::default();
    let mut current_label = "";
    let mut pending_guard = false;
    let mut pending_access_mask = false;
    let mut guarded_labels = HashSet::new();
    for line in text.lines() {
        if line.contains("OpLabel") {
            current_label = line.split_whitespace().next().unwrap_or("");
            continue;
        }
        stats.selection_merges += line.matches("OpSelectionMerge").count();
        stats.greater_equal += line.matches("OpUGreaterThanEqual").count();
        stats.less_equal += line.matches("OpULessThanEqual").count();
        if pending_access_mask && line.contains("OpIEqual") {
            stats.access_matches += 1;
        }
        pending_access_mask = line.contains("OpBitwiseAnd");
        if line.contains("OpBranchConditional") {
            if pending_guard {
                if let Some(label) = line.split_whitespace().nth(2) {
                    guarded_labels.insert(label);
                }
            }
            pending_guard = false;
        } else if line.contains("OpSelectionMerge") {
            pending_guard = true;
        }
        if line.contains("Aligned") {
            let guarded = guarded_labels.contains(current_label);
            if line.contains("OpLoad") {
                if guarded {
                    stats.guarded_loads += 1;
                } else {
                    stats.unguarded_loads += 1;
                }
            } else if line.contains("OpStore") {
                if guarded {
                    stats.guarded_stores += 1;
                } else {
                    stats.unguarded_stores += 1;
                }
            }
        }
        if line.contains("OpAtomic") {
            if guarded_labels.contains(current_label) {
                stats.guarded_atomics += 1;
            } else {
                stats.unguarded_atomics += 1;
            }
        }
    }
    stats
}

#[test]
fn runtime_i64_getelementptr_uses_64_bit_physical_addresses_and_registry_metadata() {
    // The runtime control buffer contains:
    //   0x00000000fffffffc, 0x0000000100000000,
    //   0x0000000380000000, 0x00000001ffffffff
    // so one invocation exercises the last aligned word before 4 GiB, the first word at 4 GiB,
    // a distant access, and an offset below 8 GiB that still requires the high address half.
    let (spv, raw_meta) = garnetxlate::translate_text(LARGE_BUFFER_BDA, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_registry_metadata(&meta);

    let (text, out) = validate_and_disassemble("large-buffer-bda", &spv);
    assert!(text.lines().any(|line| line.contains("OpCapability Int64")));
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert!(text.lines().any(|line| line.contains("OpIAdd %ulong")));
    assert!(text.lines().any(|line| line.contains("OpConvertUToPtr")));
    assert!(has_aligned_load(&text, "%ulong", 8));
    assert!(has_aligned_load(&text, "%uint", 4));
    assert!(text
        .lines()
        .any(|line| line.contains("OpStore") && line.contains("Aligned 4")));
    let rows = access_chain_rows(&text);
    for location in 0..3 {
        for field in [0, 32, 64] {
            for component in 0..2 {
                assert!(
                    rows.contains(&(field + location, component)),
                    "missing address-table row {} component {component}:\n{text}",
                    field + location
                );
            }
        }
    }
    assert!(
        text.contains("OpFunctionCall") && text.contains("OpArrayLength"),
        "loaded addresses must use the bounded registry:\n{text}"
    );
    let guards = guarded_access_stats(&text);
    // Eleven direct-root reads (five offsets, four read_at calls, one cell, one nested pointer load)
    // and four output stores retain direct-row guards. The loaded child address also requires
    // a declared-registry match before its target read.
    assert_eq!(guards.selection_merges, 19, "guard count:\n{text}");
    // Each access checks both `address >= base` and `length >= access_size`.
    assert_eq!(guards.greater_equal, 33, "lower-bound guard count:\n{text}");
    assert_eq!(guards.less_equal, 18, "range guard count:\n{text}");
    assert_eq!(guards.access_matches, 16, "access guard count:\n{text}");
    assert_eq!(guards.guarded_loads, 12, "guarded loads:\n{text}");
    assert_eq!(guards.guarded_stores, 4, "guarded stores:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");
    assert_eq!(guards.unguarded_stores, 0, "unguarded stores:\n{text}");
    assert!(
        !text.contains("OpUConvert %uint"),
        "a 64-bit buffer address or offset was narrowed to 32 bits:\n{text}"
    );

    std::fs::write(out.join("large-buffer-bda.meta.json"), &raw_meta).unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn scaled_constant_getelementptr_byte_offset_selects_large_buffer_addressing() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_SCALED_CONSTANT, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-scaled-constant", &spv);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert!(text.lines().any(|line| line.contains("OpConvertUToPtr")));
    assert!(has_aligned_load(&text, "%uint", 4));
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.selection_merges, 1, "guard count:\n{text}");
    assert_eq!(guards.guarded_loads, 1, "guarded loads:\n{text}");
    assert!(
        !text.contains("OpUConvert %uint"),
        "a scaled constant byte offset was narrowed to 32 bits:\n{text}"
    );

    std::fs::write(
        out.join("large-buffer-scaled-constant.meta.json"),
        &raw_meta,
    )
    .unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn scaled_runtime_i32_getelementptr_uses_64_bit_physical_addresses() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_SCALED_DYNAMIC, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-scaled-dynamic", &spv);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert!(
        text.lines().any(|line| line.contains("OpSConvert %ulong")),
        "the signed i32 index was not widened before byte scaling:\n{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpIMul %ulong")),
        "the runtime index was not scaled to a 64-bit byte offset:\n{text}"
    );
    assert!(text.lines().any(|line| line.contains("OpConvertUToPtr")));
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 1, "guarded loads:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");
    assert!(
        !text.contains("OpUConvert %uint"),
        "a scaled runtime byte offset was narrowed to 32 bits:\n{text}"
    );

    std::fs::write(out.join("large-buffer-scaled-dynamic.meta.json"), &raw_meta).unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn scalar_dynamic_getelementptr_uses_default_abi_without_datalayout() {
    for (label, source) in [
        (
            "large-buffer-scaled-dynamic-layout-control",
            LARGE_BUFFER_SCALED_DYNAMIC,
        ),
        (
            "large-buffer-scaled-dynamic-no-layout",
            LARGE_BUFFER_SCALED_DYNAMIC_NO_LAYOUT,
        ),
    ] {
        let (spv, raw_meta) = garnetxlate::translate_text(source, [64, 1, 1]).unwrap();
        let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
        assert!(has_buffer_address_table(&meta), "{label}");
        assert_eq!(meta["buffer_address_table_version"], 2, "{label}");

        let (text, out) = validate_and_disassemble(label, &spv);
        assert!(
            text.lines()
                .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")),
            "{label}: scalar GEP did not select BDA:\n{text}"
        );
        assert!(
            text.lines().any(|line| line.contains("OpSConvert %ulong")),
            "{label}: the signed scalar index was not widened:\n{text}"
        );
        assert!(
            text.lines().any(|line| line.contains("OpIMul %ulong")),
            "{label}: default scalar stride was not applied in 64-bit arithmetic:\n{text}"
        );
        let guards = guarded_access_stats(&text);
        assert_eq!(guards.guarded_loads, 1, "{label}: guarded loads:\n{text}");
        assert_eq!(
            guards.unguarded_loads, 0,
            "{label}: scalar load escaped BDA bounds:\n{text}"
        );

        std::fs::write(out.join(format!("{label}.meta.json")), &raw_meta).unwrap();
        if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
            std::fs::remove_dir_all(out).unwrap();
        }
    }
}

#[test]
fn under_aligned_scalar_access_is_lowered_to_byte_physical_accesses() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_UNALIGNED_SCALAR, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-unaligned-scalar", &spv);
    assert!(
        text.lines()
            .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")),
        "runtime byte GEP did not select BDA:\n{text}"
    );
    assert_eq!(
        text.lines()
            .filter(|line| {
                line.contains("OpLoad") && line.contains("%uchar") && line.contains("Aligned 1")
            })
            .count(),
        4,
        "the align-1 i32 load was not lowered to four byte loads:\n{text}"
    );
    assert_eq!(
        aligned_operation_count(&text, "OpStore", 1),
        4,
        "the align-1 i32 store was not lowered to four byte stores:\n{text}"
    );
    assert!(
        !text
            .lines()
            .any(|line| line.contains("OpLoad %uint") && line.contains("Aligned 4")),
        "the unaligned i32 load claimed four-byte alignment:\n{text}"
    );
    assert_eq!(
        aligned_operation_count(&text, "OpStore", 4),
        0,
        "the unaligned i32 store claimed four-byte alignment:\n{text}"
    );

    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 4, "guarded byte loads:\n{text}");
    assert_eq!(guards.guarded_stores, 4, "guarded byte stores:\n{text}");
    assert_eq!(guards.selection_merges, 2, "guard count:\n{text}");
    assert!(
        guards.less_equal >= 2,
        "the scalar accesses lost their bounds guards:\n{text}"
    );

    std::fs::write(
        out.join("large-buffer-unaligned-scalar.meta.json"),
        &raw_meta,
    )
    .unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn vec3_tail_guard_uses_the_twelve_byte_store_size_not_the_sixteen_byte_stride() {
    let (spv, raw_meta) = garnetxlate::translate_text(LARGE_BUFFER_VEC3_TAIL, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-vec3-tail", &spv);
    assert!(
        text.lines()
            .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")),
        "runtime vector GEP did not select BDA:\n{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpIMul %ulong")),
        "the vec3 allocation stride was not applied in 64-bit arithmetic:\n{text}"
    );
    assert!(
        long_constants(&text).values().any(|value| *value == 16),
        "the vec3 GEP lost its 16-byte allocation stride:\n{text}"
    );
    let bounds_sizes = long_comparison_constants(&text, "OpUGreaterThanEqual");
    assert!(
        bounds_sizes.iter().filter(|value| **value == 12).count() >= 2,
        "the vec3 load/store guards did not use the 12-byte store size: \
         {bounds_sizes:?}\n{text}"
    );
    assert!(
        !bounds_sizes.contains(&16),
        "a vec3 guard used the 16-byte allocation size instead of its 12-byte store size: \
         {bounds_sizes:?}\n{text}"
    );

    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 1, "guarded vec3 load:\n{text}");
    assert_eq!(guards.guarded_stores, 1, "guarded vec3 store:\n{text}");
    assert_eq!(guards.selection_merges, 2, "guard count:\n{text}");

    std::fs::write(out.join("large-buffer-vec3-tail.meta.json"), &raw_meta).unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn nullable_select_phi_and_helper_keep_large_buffer_bounds() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_NULLABLE_MERGE, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_registry_metadata(&meta);

    let (text, out) = validate_and_disassemble("large-buffer-nullable-merge", &spv);
    assert!(
        text.lines()
            .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")),
        "nullable large-buffer merge stayed Logical:\n{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpSelect %ulong")),
        "nullable select did not merge a complete 64-bit address:\n{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpPhi %ulong")),
        "nullable phi did not merge a complete 64-bit address:\n{text}"
    );
    assert!(
        text.contains("OpFunctionCall") && text.contains("OpArrayLength"),
        "nullable child address lost its registry lookup:\n{text}"
    );
    assert_no_narrowed_i64(&text);

    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 3, "guarded loads:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");
    assert!(
        guards.greater_equal >= 6,
        "nullable loads lost lower-bound checks:\n{text}"
    );
    assert!(
        guards.less_equal >= 3,
        "nullable loads lost range checks:\n{text}"
    );
    assert!(
        guards.access_matches >= 3,
        "nullable loads lost access-permission checks:\n{text}"
    );

    std::fs::write(out.join("large-buffer-nullable-merge.meta.json"), &raw_meta).unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn addrspace2_data_buffers_with_dynamic_scaled_offsets_select_bda() {
    for (label, source, version) in [
        (
            "large-buffer-addrspace2-data",
            LARGE_BUFFER_ADDRSPACE2_DATA,
            2,
        ),
        (
            "large-buffer-addrspace2-device-pointers",
            LARGE_BUFFER_ADDRSPACE2_DEVICE_POINTERS,
            3,
        ),
    ] {
        let (spv, raw_meta) = garnetxlate::translate_text(source, [64, 1, 1]).unwrap();
        let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
        assert!(has_buffer_address_table(&meta), "{label}");
        assert_eq!(meta["buffer_address_table_version"], version, "{label}");

        let (text, out) = validate_and_disassemble(label, &spv);
        assert!(
            text.lines()
                .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")),
            "{label}"
        );
        assert!(
            text.lines().any(|line| line.contains("OpSConvert %ulong")),
            "{label}: the signed i32 index was not widened:\n{text}"
        );
        assert!(
            text.lines().any(|line| line.contains("OpIMul %ulong")),
            "{label}: the data-buffer index was not scaled in 64-bit arithmetic:\n{text}"
        );
        let guards = guarded_access_stats(&text);
        assert!(
            guards.guarded_loads >= 1,
            "{label}: the addrspace(2) data-buffer load was not guarded:\n{text}"
        );
        assert_eq!(
            guards.unguarded_stores, 0,
            "{label}: an output store escaped bounds checks:\n{text}"
        );

        std::fs::write(out.join(format!("{label}.meta.json")), &raw_meta).unwrap();
        if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
            std::fs::remove_dir_all(out).unwrap();
        }
    }
}

#[test]
fn chained_constant_getelementptrs_accumulate_their_byte_offsets() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_CHAINED_CONSTANT, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-chained-constant", &spv);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert!(
        text.lines()
            .any(|line| line.contains("OpConstant %ulong 4295032832")),
        "the chained byte offsets were not accumulated in 64-bit arithmetic:\n{text}"
    );
    assert!(text.lines().any(|line| line.contains("OpConvertUToPtr")));
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 1, "guarded loads:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");
    assert!(
        !text.contains("OpUConvert %uint"),
        "the accumulated byte offset was narrowed to 32 bits:\n{text}"
    );

    std::fs::write(
        out.join("large-buffer-chained-constant.meta.json"),
        &raw_meta,
    )
    .unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn inbounds_array_type_extent_does_not_bound_the_underlying_object() {
    for (label, source) in [
        (
            "large-buffer-inbounds-singleton",
            LARGE_BUFFER_INBOUNDS_SINGLETON,
        ),
        (
            "large-buffer-inbounds-zero-array",
            LARGE_BUFFER_INBOUNDS_ZERO_ARRAY,
        ),
    ] {
        let (spv, raw_meta) = garnetxlate::translate_text(source, [64, 1, 1]).unwrap();
        let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
        assert!(has_buffer_address_table(&meta), "{label}");
        assert_eq!(meta["buffer_address_table_version"], 2, "{label}");

        let (text, out) = validate_and_disassemble(label, &spv);
        assert!(text
            .lines()
            .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
        assert!(
            text.lines().any(|line| line.contains("OpSConvert %ulong")),
            "{label}: the signed array index was not widened:\n{text}"
        );
        assert!(
            text.lines().any(|line| line.contains("OpIMul %ulong")),
            "{label}: the array index was not scaled in 64-bit arithmetic:\n{text}"
        );
        assert!(
            !text.contains("OpUConvert %uint"),
            "{label}: the reachable byte offset was narrowed to 32 bits:\n{text}"
        );

        std::fs::write(out.join(format!("{label}.meta.json")), &raw_meta).unwrap();
        if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
            std::fs::remove_dir_all(out).unwrap();
        }
    }
}

#[test]
fn wrapping_i32_arithmetic_that_can_overflow_uses_64_bit_addresses() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_WRAPPING_I32, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-wrapping-i32", &spv);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert!(
        text.lines().any(|line| line.contains("OpSConvert %ulong")),
        "the possibly wrapped i32 index was not widened:\n{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpIMul %ulong")),
        "the byte offset was not computed as 64-bit arithmetic:\n{text}"
    );
    assert!(
        !text.contains("OpUConvert %uint"),
        "a possibly wrapped byte offset was narrowed to 32 bits:\n{text}"
    );

    std::fs::write(out.join("large-buffer-wrapping-i32.meta.json"), &raw_meta).unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn checked_same_root_select_and_phi_merge_64_bit_addresses_and_keep_guards() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_POINTER_MERGE, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-pointer-merge", &spv);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert!(
        text.lines()
            .filter(|line| line.contains("OpSelect %ulong"))
            .count()
            >= 1,
        "same-root select did not merge complete 64-bit addresses:\n{text}"
    );
    assert!(
        text.lines()
            .filter(|line| line.contains("OpPhi %ulong"))
            .count()
            >= 4,
        "same-root phi did not merge complete 64-bit addresses:\n{text}"
    );
    let guards = guarded_access_stats(&text);
    assert!(guards.selection_merges >= 2, "guard count:\n{text}");
    assert_eq!(guards.guarded_loads, 2, "guarded loads:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");
    assert!(
        !text.contains("OpUConvert %uint"),
        "a selected/phi i64 address was narrowed to 32 bits:\n{text}"
    );

    std::fs::write(out.join("large-buffer-pointer-merge.meta.json"), &raw_meta).unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn runtime_i64_loop_backedge_inherits_checked_row_bounds() {
    let (spv, raw_meta) = garnetxlate::translate_text(LARGE_BUFFER_LOOP_PHI, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-loop-phi", &spv);
    assert!(
        text.lines().any(|line| line.contains("OpPhi %ulong")),
        "loop pointer was not lowered as a 64-bit address phi:\n{text}"
    );
    assert!(
        !text.lines().any(|line| {
            line.contains("OpPhi %bool") && line.contains(" %true ") && line.contains(" %false ")
        }),
        "loop backedge lost checked bounds and gated the row off:\n{text}"
    );
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 1, "guarded loads:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");
    assert!(
        !text.contains("OpUConvert %uint"),
        "a loop-carried i64 address was narrowed to 32 bits:\n{text}"
    );

    std::fs::write(out.join("large-buffer-loop-phi.meta.json"), &raw_meta).unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn ptrtoint_inttoptr_roundtrip_preserves_checked_physical_address_and_row() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_INTEGER_ROUNDTRIP, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-integer-roundtrip", &spv);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert!(
        !text
            .lines()
            .any(|line| line.contains("OpCopyObject %ulong %ulong_0")),
        "ptrtoint discarded the physical address:\n{text}"
    );
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 1, "guarded loads:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");
    assert!(guards.access_matches >= 1, "access guard:\n{text}");
    assert!(
        !text.contains("OpUConvert %uint"),
        "an integer pointer round trip narrowed the address to 32 bits:\n{text}"
    );

    std::fs::write(
        out.join("large-buffer-integer-roundtrip.meta.json"),
        &raw_meta,
    )
    .unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn integer_sub_select_phi_and_loop_preserve_the_selected_checked_row() {
    let source = include_str!("fixtures/large_buffer_integer_merges.ll");
    let (spv, raw_meta) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert_eq!(meta["buffer_address_table_version"], 2);
    let (text, out) = validate_and_disassemble("large-buffer-integer-merges", &spv);
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 3, "{guards:?}\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "{guards:?}\n{text}");
    assert!(guards.access_matches >= 3, "{guards:?}\n{text}");
    assert!(
        text.lines()
            .filter(|line| line.contains("OpSelect %ulong"))
            .count()
            >= 4,
        "{text}"
    );
    assert!(
        text.lines()
            .filter(|line| line.contains("OpPhi %ulong"))
            .count()
            >= 8,
        "{text}"
    );
    assert!(!text.contains("OpUConvert %uint"), "{text}");
    assert!(!text.contains("OpCopyObject %ulong %ulong_0"), "{text}");
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn memory_intrinsics_keep_runtime_byte_offsets_and_guard_each_physical_access() {
    let source = include_str!("fixtures/large_buffer_memops.ll");
    let (spv, _) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let (text, out) = validate_and_disassemble("large-buffer-memops", &spv);
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 8, "{guards:?}\n{text}");
    assert_eq!(guards.guarded_stores, 13, "{guards:?}\n{text}");
    assert_eq!(
        guards.unguarded_loads + guards.unguarded_stores,
        0,
        "{guards:?}\n{text}"
    );
    assert!(!text.contains("OpUDiv"), "{text}");
    assert!(!text.contains("OpCopyMemory"), "{text}");
    assert!(!text.contains("OpFunctionCall"), "{text}");
    assert_no_narrowed_i64(&text);
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

fn assert_no_narrowed_i64(text: &str) {
    let definitions = text
        .lines()
        .filter_map(|line| {
            let words = line.split_whitespace().collect::<Vec<_>>();
            (words.get(1) == Some(&"="))
                .then_some(words)
                .filter(|words| words.len() >= 4)
        })
        .map(|words| (words[0], words))
        .collect::<HashMap<_, _>>();
    for words in definitions
        .values()
        .filter(|words| words[2] == "OpUConvert" && words[3] == "%uint")
    {
        let source = &definitions[words[4]];
        assert_ne!(source[3], "%ulong", "narrowed a 64-bit offset:\n{text}");
    }
}

#[test]
fn partial_memcpy_through_a_local_first_byte_keeps_the_runtime_bda_cursor() {
    let source = include_str!("fixtures/large_buffer_memops.ll")
        .replace(
            "%offset = load",
            "%first = getelementptr [8 x i8], ptr %local, i64 0, i64 0\n  %offset = load",
        )
        .replace("ptr align 1 %local", "ptr align 1 %first")
        .replace("i64 8, i1 false", "i64 7, i1 false");
    let (spv, _) = garnetxlate::translate_text(&source, [1, 1, 1]).unwrap();
    let (text, out) = validate_and_disassemble("large-buffer-memops-first-byte", &spv);
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 7, "{guards:?}\n{text}");
    assert_eq!(guards.guarded_stores, 12, "{guards:?}\n{text}");
    assert_eq!(
        guards.unguarded_loads + guards.unguarded_stores,
        0,
        "{guards:?}\n{text}"
    );
    assert_no_narrowed_i64(&text);
    assert!(!text.contains("OpFunctionCall"), "{text}");
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn mutable_multidimensional_local_pointer_tables_load_runtime_addresses_and_bounds() {
    let source = include_str!("fixtures/large_buffer_local_tables.ll");
    let (spv, raw_meta) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert_registry_metadata(&meta);
    let (text, out) = validate_and_disassemble("large-buffer-local-tables", &spv);
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 8, "{guards:?}\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "{guards:?}\n{text}");
    assert_no_narrowed_i64(&text);
    assert!(
        text.lines()
            .filter(|line| line.contains("OpLoad %ulong"))
            .count()
            >= 8,
        "{text}"
    );
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn under_aligned_vectors_keep_their_lane_bytes_and_complete_access_guard() {
    let source = LARGE_BUFFER_UNALIGNED_SCALAR
        .replace("load i32,", "load <3 x i32>,")
        .replace("store i32 %value,", "store <3 x i32> %value,");
    let (spv, _) = garnetxlate::translate_text(&source, [1, 1, 1]).unwrap();
    let (text, out) = validate_and_disassemble("large-buffer-unaligned-vec3", &spv);
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 12, "{guards:?}\n{text}");
    assert_eq!(guards.guarded_stores, 12, "{guards:?}\n{text}");
    assert_eq!(
        guards.unguarded_loads + guards.unguarded_stores,
        0,
        "{guards:?}\n{text}"
    );
    assert_eq!(aligned_operation_count(&text, "OpLoad", 1), 12, "{text}");
    assert_eq!(aligned_operation_count(&text, "OpStore", 1), 12, "{text}");
    assert!(
        long_comparison_constants(&text, "OpUGreaterThanEqual").contains(&12),
        "{text}"
    );
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn checked_cross_root_pointer_merges_select_bounds_with_the_pointer() {
    for (name, source, merge_op) in [
        (
            "cross-root-select",
            LARGE_BUFFER_CROSS_ROOT_SELECT,
            "OpSelect %ulong",
        ),
        (
            "cross-root-phi",
            LARGE_BUFFER_CROSS_ROOT_PHI,
            "OpPhi %ulong",
        ),
    ] {
        let (spv, raw_meta) = garnetxlate::translate_text(source, [64, 1, 1]).unwrap();
        let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
        assert!(has_buffer_address_table(&meta));
        assert_eq!(meta["buffer_address_table_version"], 2);

        let (text, out) = validate_and_disassemble(&format!("large-buffer-{name}"), &spv);
        assert!(text
            .lines()
            .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
        assert!(
            text.lines().filter(|line| line.contains(merge_op)).count() >= 4,
            "cross-root {name} did not merge address, length, and access with the pointer:\n{text}"
        );
        let guards = guarded_access_stats(&text);
        assert_eq!(guards.guarded_loads, 1, "guarded loads:\n{text}");
        assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");
        assert!(
            !text.contains("OpUConvert %uint"),
            "a cross-root i64 address was narrowed to 32 bits:\n{text}"
        );

        std::fs::write(
            out.join(format!("large-buffer-{name}.meta.json")),
            &raw_meta,
        )
        .unwrap();
        if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
            std::fs::remove_dir_all(out).unwrap();
        }
    }
}

#[test]
fn mixed_pointer_select_uses_direct_bounds_or_registry_guard() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_MIXED_POINTER_SELECT, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_registry_metadata(&meta);

    let (text, out) = validate_and_disassemble("large-buffer-mixed-pointer-select", &spv);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert_registry_guard_selected(&text);
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 1, "guarded loads:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");

    std::fs::write(
        out.join("large-buffer-mixed-pointer-select.meta.json"),
        &raw_meta,
    )
    .unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn nested_checked_false_arm_keeps_its_bounds_gate() {
    let (spv, raw_meta) =
        garnetxlate::translate_text(LARGE_BUFFER_NESTED_MIXED_POINTER_SELECT, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_registry_metadata(&meta);

    let (text, out) = validate_and_disassemble("large-buffer-nested-mixed-pointer-select", &spv);
    assert_registry_guard_selected(&text);
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_loads, 1, "guarded loads:\n{text}");
    assert_eq!(guards.unguarded_loads, 0, "unguarded loads:\n{text}");

    std::fs::write(
        out.join("large-buffer-nested-mixed-pointer-select.meta.json"),
        &raw_meta,
    )
    .unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn ordinary_i32_getelementptr_stays_logical_and_omits_v2_metadata() {
    // The runtime index is explicitly masked to 1023, so the complete byte range is provable and
    // the pre-BDA Logical representation stays byte-identical.
    let first = garnetxlate::translate_text(ORDINARY_I32, [64, 1, 1]).unwrap();
    let second = garnetxlate::translate_text(ORDINARY_I32, [64, 1, 1]).unwrap();
    assert_eq!(first, second, "legacy translation is not deterministic");

    let meta: Value = serde_json::from_slice(&first.1).unwrap();
    assert!(!has_buffer_address_table(&meta));
    assert!(meta.get("buffer_address_table_version").is_none());
    assert!(meta.get("buffer_address_table_bytes").is_none());

    let (text, out) = validate_and_disassemble("ordinary-i32", &first.0);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel Logical GLSL450")));
    assert!(!text.contains("PhysicalStorageBufferAddresses"));
    assert!(!text.contains("OpConvertUToPtr"));
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn pointer_load_bda_requires_registry_metadata() {
    let (spv, raw_meta) = garnetxlate::translate_text(LEGACY_BDA_V1, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_registry_metadata(&meta);

    let (text, out) = validate_and_disassemble("legacy-bda-v1", &spv);
    assert!(text
        .lines()
        .any(|line| line.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450")));
    assert!(text.lines().any(|line| line.contains("OpConvertUToPtr")));
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn trim_meta_v2_states_the_table_contract_without_changing_legacy_output() {
    let input = serde_json::json!({
        "reflection_version": 56,
        "stage": "Kernel",
        "entry_point": "large_buffer_bda",
        "bindings": [{"kind": "BufferAddressTable", "metal_index": 0}]
    });
    let legacy: Value = serde_json::from_slice(&garnetxlate::trim_meta(&input, 0)).unwrap();
    assert!(legacy.get("buffer_address_table_version").is_none());
    assert!(legacy.get("buffer_address_table_bytes").is_none());

    let v2: Value = serde_json::from_slice(&garnetxlate::trim_meta_with_buffer_address_mode(
        &input,
        0,
        metal2vulkan::BufferAddressTableMode::AddressLengthAccessV2,
    ))
    .unwrap();
    assert_eq!(v2["buffer_address_table_version"], 2);
    assert_eq!(v2["buffer_address_table_bytes"], 768);
}

#[test]
fn atomic_through_a_checked_large_buffer_row_is_bounds_and_access_guarded() {
    let (spv, raw_meta) = garnetxlate::translate_text(LARGE_BUFFER_ATOMIC, [64, 1, 1]).unwrap();
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(has_buffer_address_table(&meta));
    assert_eq!(meta["buffer_address_table_version"], 2);

    let (text, out) = validate_and_disassemble("large-buffer-atomic", &spv);
    for opcode in [
        "OpAtomicLoad",
        "OpAtomicStore",
        "OpAtomicIAdd",
        "OpAtomicCompareExchange",
    ] {
        assert!(
            text.lines().any(|line| line.contains(opcode)),
            "missing {opcode}:\n{text}"
        );
    }
    let guards = guarded_access_stats(&text);
    assert_eq!(guards.guarded_atomics, 4, "guarded atomics:\n{text}");
    assert_eq!(guards.unguarded_atomics, 0, "unguarded atomics:\n{text}");
    assert_eq!(guards.access_matches, 4, "access guards:\n{text}");
    assert!(guards.greater_equal >= 1, "lower-bound guard:\n{text}");
    assert!(guards.less_equal >= 1, "range guard:\n{text}");
    for required in ["%ulong_1", "%ulong_2", "%ulong_3"] {
        assert!(
            text.lines().any(|line| {
                line.contains("OpIEqual %bool") && line.split_whitespace().last() == Some(required)
            }),
            "missing access mask {required}:\n{text}"
        );
    }
    assert_eq!(
        text.lines()
            .filter(|line| {
                line.contains("OpIEqual %bool")
                    && line.split_whitespace().last() == Some("%ulong_3")
            })
            .count(),
        2,
        "load-modify-write and cmpxchg must require read and write bits:\n{text}"
    );

    std::fs::write(out.join("large-buffer-atomic.meta.json"), &raw_meta).unwrap();
    if std::env::var_os("RDNA4X_LARGE_BUFFER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}
