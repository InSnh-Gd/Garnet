use serde_json::Value;
use std::path::PathBuf;

const VERTEX: &str = include_str!("fixtures/large_buffer_vertex.ll");
const FRAGMENT: &str = include_str!("fixtures/large_buffer_fragment.ll");
const INDIRECT_ARGUMENT_GRAPH: &str = include_str!("fixtures/argument_lod_graph.ll");
const ARGUMENT_RESOURCE_ARRAYS: &str = include_str!("fixtures/argument_resource_arrays.ll");

fn evidence_dir() -> PathBuf {
    std::env::var_os("RDNA4X_BDA_STAGE_EVIDENCE")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            std::env::temp_dir().join(format!("rdna4-bda-stages-{}", std::process::id()))
        })
}

fn translate_and_validate(label: &str, source: &str) -> (Value, String, PathBuf) {
    let (spv, metadata) = garnetxlate::translate_text(source, [1, 1, 1])
        .unwrap_or_else(|error| panic!("{label} translation failed: {error}"));
    let parsed: Value = serde_json::from_slice(&metadata).unwrap();
    let out = evidence_dir();
    std::fs::create_dir_all(&out).unwrap();
    let path = out.join(format!("{label}.spv"));
    std::fs::write(&path, &spv).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
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
        "{label} won't validate: {}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let text = String::from_utf8(disassembly.stdout).unwrap();
    std::fs::write(out.join(format!("{label}.spvasm")), &text).unwrap();
    std::fs::write(
        out.join(format!("{label}.meta.json")),
        serde_json::to_vec_pretty(&parsed).unwrap(),
    )
    .unwrap();
    (parsed, text, out)
}

fn has_buffer_address_table(meta: &Value) -> bool {
    meta["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .any(|binding| binding["kind"] == "BufferAddressTable")
}

fn buffer_address_table_bindings(meta: &Value) -> Vec<&Value> {
    meta["bindings"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|binding| binding["kind"] == "BufferAddressTable")
        .collect()
}

fn assert_readonly_physical_access_without_writes(text: &str) {
    assert!(
        text.lines()
            .any(|line| line.contains("OpUGreaterThanEqual")),
        "missing lower-bound guard:\n{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpULessThanEqual")),
        "missing range guard:\n{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpIEqual %bool")),
        "missing read/write access-mask guard:\n{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpSelectionMerge")),
        "physical access is not guarded by structured control flow:\n{text}"
    );
    assert_eq!(
        text.lines()
            .filter(|line| line.contains("OpLoad") && line.contains("Aligned"))
            .count(),
        1,
        "expected exactly one physical buffer load:\n{text}"
    );
    assert_eq!(
        text.lines()
            .filter(|line| line.contains("OpStore") && line.contains("Aligned"))
            .count(),
        0,
        "a read-only buffer received a physical store:\n{text}"
    );
}

#[test]
fn vertex_runtime_i64_byte_offset_keeps_physical_addresses_and_v2_table() {
    let (meta, text, _) = translate_and_validate("large-buffer-vertex", VERTEX);
    let tables = buffer_address_table_bindings(&meta);
    assert_eq!(
        tables.len(),
        1,
        "only the dynamically indexed data root should be physicalized: {meta:#}"
    );
    assert_eq!(tables[0]["access"], "ReadOnly", "{meta:#}");
    assert_eq!(meta["buffer_address_table_version"], 2, "{meta:#}");
    assert!(
        text.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450"),
        "{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpIAdd %ulong")),
        "{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpConvertUToPtr")),
        "{text}"
    );
    assert!(!text.contains("OpUConvert %uint"), "{text}");
    assert_readonly_physical_access_without_writes(&text);
}

#[test]
fn fragment_runtime_i64_byte_offset_keeps_physical_addresses_and_v2_table() {
    let (meta, text, _) = translate_and_validate("large-buffer-fragment", FRAGMENT);
    let tables = buffer_address_table_bindings(&meta);
    assert_eq!(
        tables.len(),
        1,
        "only the dynamically indexed data root should be physicalized: {meta:#}"
    );
    assert_eq!(tables[0]["access"], "ReadOnly", "{meta:#}");
    assert_eq!(meta["buffer_address_table_version"], 2, "{meta:#}");
    assert!(
        text.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450"),
        "{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpIAdd %ulong")),
        "{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpConvertUToPtr")),
        "{text}"
    );
    assert!(!text.contains("OpUConvert %uint"), "{text}");
    assert_readonly_physical_access_without_writes(&text);
}

#[test]
fn defined_helpers_keep_distinct_checked_cursors_in_vertex_and_fragment() {
    for (stage, source) in [("vertex", VERTEX), ("fragment", FRAGMENT)] {
        let source = source.replace(
            "  %value = load i32, ptr addrspace(1) %source, align 4",
            r#"  %first = call i32 @read_checked(ptr addrspace(1) %source)
  %next = getelementptr i8, ptr addrspace(1) %source, i64 4
  %second = call i32 @read_checked(ptr addrspace(1) %next)
  %value = add i32 %first, %second"#,
        ) + r#"
define i32 @read_checked(ptr addrspace(1) %p) {
entry:
  %value = load i32, ptr addrspace(1) %p, align 4
  ret i32 %value
}
"#;
        let (meta, text, _) = translate_and_validate(&format!("checked-helper-{stage}"), &source);
        assert_eq!(meta["buffer_address_table_version"], 2, "{meta:#}");
        let tables = buffer_address_table_bindings(&meta);
        assert_eq!(tables.len(), 1, "{meta:#}");
        assert_eq!(tables[0]["access"], "ReadOnly", "{meta:#}");
        assert!(text.contains("OpIAdd %ulong"), "{text}");
        assert!(!text.contains("OpUConvert %uint"), "{text}");
        assert_eq!(
            text.lines()
                .filter(|line| line.contains("OpLoad") && line.contains("Aligned"))
                .count(),
            2,
            "{text}"
        );
        assert!(
            !text
                .lines()
                .any(|line| line.contains("OpStore") && line.contains("Aligned")),
            "{text}"
        );
        for guard in [
            "OpUGreaterThanEqual",
            "OpULessThanEqual",
            "OpSelectionMerge",
        ] {
            assert!(text.contains(guard), "missing {guard}:\n{text}");
        }
    }
}

#[test]
fn indirect_argument_buffer_in_fragment_is_not_physicalized() {
    let (spv, raw_meta) = garnetxlate::translate_text(INDIRECT_ARGUMENT_GRAPH, [1, 1, 1])
        .expect("fragment argument graph translation");
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert!(
        !has_buffer_address_table(&meta),
        "an air.indirect_buffer descriptor aggregate became a BDA root: {meta:#}"
    );
    assert!(
        meta.get("buffer_address_table_version").is_none(),
        "indirect argument buffer claimed a BDA table contract: {meta:#}"
    );

    let out = evidence_dir();
    std::fs::create_dir_all(&out).unwrap();
    let path = out.join("indirect-argument-fragment.spv");
    std::fs::write(&path, &spv).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "indirect argument graph won't validate: {}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let text = String::from_utf8(disassembly.stdout).unwrap();
    std::fs::write(out.join("indirect-argument-fragment.spvasm"), &text).unwrap();
    std::fs::write(
        out.join("indirect-argument-fragment.meta.json"),
        serde_json::to_vec_pretty(&meta).unwrap(),
    )
    .unwrap();
    assert!(
        text.contains("OpMemoryModel Logical GLSL450"),
        "indirect argument graph changed memory model:\n{text}"
    );
    assert!(
        !text.contains("PhysicalStorageBuffer"),
        "indirect argument graph gained a physical storage pointer:\n{text}"
    );
}

#[test]
fn unknown_gpu_resource_token_falls_back_to_dummy_index_zero() {
    let (spv, raw_meta) =
        garnetxlate::translate_text_gpu_arguments(ARGUMENT_RESOURCE_ARRAYS, [32, 1, 1])
            .expect("GPU argument resource table translation");
    let meta: Value = serde_json::from_slice(&raw_meta).unwrap();
    assert_eq!(meta["argument_resource_tables"]["version"], 9, "{meta:#}");

    let out = evidence_dir();
    std::fs::create_dir_all(&out).unwrap();
    let path = out.join("unknown-token.spv");
    std::fs::write(&path, &spv).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "GPU token table won't validate: {}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let text = String::from_utf8(disassembly.stdout).unwrap();
    std::fs::write(out.join("unknown-token.spvasm"), &text).unwrap();
    std::fs::write(
        out.join("unknown-token.meta.json"),
        serde_json::to_vec_pretty(&meta).unwrap(),
    )
    .unwrap();

    assert!(text.contains("Binding 672"), "{text}");
    assert!(
        text.lines().any(|line| line.contains("OpLoopMerge")),
        "{text}"
    );
    assert!(
        text.lines().any(|line| line.contains("OpULessThan %bool")),
        "lookup has no bounded candidate check:\n{text}"
    );
    // The binary search materializes two dummy fallbacks: `in_range ? low : 0` before the final
    // comparison and `exact_match ? candidate : 0` after it. The last operand proves an unknown
    // token selects descriptor-array index zero rather than becoming an address.
    let dummy_fallbacks = text
        .lines()
        .filter(|line| line.contains("OpSelect %uint") && line.trim_end().ends_with("%uint_0"))
        .count();
    assert_eq!(dummy_fallbacks, 2, "unknown-token fallback:\n{text}");
}
