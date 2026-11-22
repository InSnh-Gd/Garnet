use serde_json::Value;
use std::path::PathBuf;

const POINTER_KERNEL: &str = r#"
target triple = "air64_v28-apple-macosx26.5.0"
%PointerArgs = type { ptr addrspace(1), i32 }
define void @gpu_pointer(ptr addrspace(1) %args, ptr addrspace(1) %observed) {
entry:
  %slot = getelementptr inbounds %PointerArgs, ptr addrspace(1) %args, i64 0, i32 0
  %words = load ptr addrspace(1), ptr addrspace(1) %slot, align 8
  %old = load i32, ptr addrspace(1) %words, align 4
  store i32 %old, ptr addrspace(1) %observed, align 4
  %bias_slot = getelementptr inbounds %PointerArgs, ptr addrspace(1) %args, i64 0, i32 1
  %bias = load i32, ptr addrspace(1) %bias_slot, align 4
  %flipped = xor i32 %old, 2781060721
  %new = add i32 %flipped, %bias
  store i32 %new, ptr addrspace(1) %words, align 4
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @gpu_pointer, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 1, !"air.struct_type_info", !5, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"PointerArgs", !"air.arg_name", !"args"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 7, i32 1, !"air.write", !"air.arg_type_name", !"uint", !"air.arg_name", !"observed"}
!5 = !{i32 0, i32 8, i32 0, !"uint", !"words", !"air.indirect_argument", !6, i32 8, i32 4, i32 0, !"uint", !"bias", !"air.indirect_argument", !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"words"}
!7 = !{i32 1, !"air.indirect_constant", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"uint", !"air.arg_name", !"bias"}
"#;

#[test]
fn ordinary_buffer_pointer_read_write() {
    let out = std::env::var_os("GPU_POINTER_EVIDENCE")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            std::env::temp_dir().join(format!("rdna4-pointer-{}", std::process::id()))
        });
    std::fs::create_dir_all(&out).unwrap();
    std::fs::write(out.join("pointer.ll"), POINTER_KERNEL).unwrap();
    let (spv, meta) = garnetxlate::translate_text(POINTER_KERNEL, [1, 1, 1]).unwrap();
    std::fs::write(out.join("pointer.spv"), &spv).unwrap();
    std::fs::write(out.join("pointer.meta.json"), &meta).unwrap();
    let metadata: Value = serde_json::from_slice(&meta).unwrap();
    let bindings = metadata["bindings"].as_array().unwrap();
    assert!(bindings.iter().any(|b| b["kind"] == "BufferAddressTable"));
    let embedded = bindings
        .iter()
        .find(|b| b["kind"] == "EmbeddedArgBufferBuffer")
        .unwrap();
    assert_eq!(
        embedded["embedded_source"],
        serde_json::json!({
            "buffer_param_index": 0, "buffer_index": 3, "field_offset": 0,
            "field_ordinal": 0, "argument_index": 0, "resource_buffer_index": 0
        })
    );
    assert_eq!(
        metadata["argument_buffer_fields"],
        serde_json::json!([
            {"buffer_param_index": 0, "buffer_index": 3, "field_offset": 0,
             "field_ordinal": 0, "argument_index": 0, "resource_buffer_index": 0},
            {"buffer_param_index": 0, "buffer_index": 3, "field_offset": 8,
             "field_ordinal": 1, "argument_index": 1, "resource_buffer_index": null}
        ])
    );
    for (param, location) in [(0, 3), (1, 7)] {
        assert!(bindings
            .iter()
            .any(|b| (b["kind"] == "Buffer" || b["kind"] == "ArgumentBuffer")
                && b["param_index"] == param
                && b["metal_index"] == location));
    }
    let status = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(out.join("pointer.spv"))
        .output()
        .unwrap();
    std::fs::write(
        out.join("spirv-val.log"),
        format!(
            "status={}\n{}{}",
            status.status,
            String::from_utf8_lossy(&status.stdout),
            String::from_utf8_lossy(&status.stderr)
        ),
    )
    .unwrap();
    assert!(
        status.status.success(),
        "{}",
        String::from_utf8_lossy(&status.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(out.join("pointer.spv"))
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    std::fs::write(out.join("pointer.spvasm"), &disassembly.stdout).unwrap();
    let assembly = String::from_utf8(disassembly.stdout).unwrap();
    assert!(assembly.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450"));
    assert!(assembly.contains("OpConvertUToPtr"));
    assert!(assembly
        .lines()
        .any(|line| line.contains("OpLoad") && line.contains("Aligned 4")));
    assert!(assembly
        .lines()
        .any(|line| line.contains("OpStore") && line.contains("Aligned 4")));
    if std::env::var_os("GPU_POINTER_EVIDENCE").is_none() {
        std::fs::remove_dir_all(out).unwrap();
    }
}

#[test]
fn ordinary_metadata_does_not_gain_empty_argument_buffer_fields() {
    let input = serde_json::json!({
        "bindings": [{"kind": "Buffer", "metal_index": 7, "embedded_source": null}],
        "argument_buffer_fields": []
    });
    let output: Value = serde_json::from_slice(&garnetxlate::trim_meta(&input, 0)).unwrap();
    assert!(output.get("argument_buffer_fields").is_none());
    assert!(output["bindings"][0].get("embedded_source").is_none());
}

#[test]
fn gpu_producer_copies_and_offsets_a_device_pointer_without_truncating() {
    let source = include_str!("fixtures/gpu_produced_arguments.ll");
    let (spv, meta) = garnetxlate::translate_text(source, [1, 1, 1]).unwrap();
    let metadata: Value = serde_json::from_slice(&meta).unwrap();
    for location in [3, 5] {
        assert!(metadata["bindings"]
            .as_array()
            .unwrap()
            .iter()
            .any(|binding| {
                (binding["kind"] == "ArgumentBuffer" || binding["kind"] == "Buffer")
                    && binding["metal_index"] == location
            }));
    }
    let out = std::env::temp_dir().join(format!("rdna4-gpu-producer-{}", std::process::id()));
    std::fs::create_dir_all(&out).unwrap();
    let path = out.join("producer.spv");
    std::fs::write(&path, spv).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "{}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let text = String::from_utf8(disassembly.stdout).unwrap();
    assert!(text.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450"));
    // The raw buffer path stores an address as two u32 words, including carry.
    let instructions = text
        .lines()
        .map(|line| line.split_whitespace().collect::<Vec<_>>())
        .collect::<Vec<_>>();
    let stored_values = instructions
        .iter()
        .filter_map(|words| match words.as_slice() {
            ["OpStore", _, value, ..] => Some(*value),
            _ => None,
        })
        .collect::<Vec<_>>();
    let mut halves = std::collections::HashMap::<&str, u8>::new();
    for words in &instructions {
        if let [id, "=", "OpUConvert", "%uint", address] = words.as_slice() {
            if !stored_values.contains(id) {
                continue;
            }
            let high = instructions
                .iter()
                .find_map(|shift| match shift.as_slice() {
                    [shift_id, "=", "OpShiftRightLogical", "%ulong", base, "%ulong_32"]
                        if shift_id == address =>
                    {
                        Some(*base)
                    }
                    _ => None,
                });
            *halves.entry(high.unwrap_or(address)).or_default() |=
                if high.is_some() { 2 } else { 1 };
        }
    }
    assert!(
        halves.values().any(|mask| *mask == 3),
        "pointer store lost an address half:\n{text}"
    );
    assert!(
        text.contains("OpULessThan"),
        "split address addition lost carry:\n{text}"
    );
    if let Some(evidence) = std::env::var_os("GPU_POINTER_EVIDENCE") {
        let evidence = PathBuf::from(evidence);
        std::fs::create_dir_all(&evidence).unwrap();
        std::fs::copy(&path, evidence.join("producer.spv")).unwrap();
        std::fs::write(evidence.join("producer.spvasm"), text).unwrap();
        std::fs::write(evidence.join("producer.meta.json"), meta).unwrap();
    }
    std::fs::remove_dir_all(out).unwrap();
}

#[test]
fn device_to_device_texture_and_sampler_copies_keep_both_token_words() {
    let source = include_str!("fixtures/argument_device_resource_copy.ll");
    let (spv, _) = garnetxlate::translate_text(source, [2, 1, 1]).unwrap();
    let out = std::env::temp_dir().join(format!("rdna4-resource-copy-{}", std::process::id()));
    std::fs::create_dir_all(&out).unwrap();
    let path = out.join("copy.spv");
    std::fs::write(&path, spv).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "{}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let text = String::from_utf8(disassembly.stdout).unwrap();
    assert!(
        text.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450"),
        "resource copying fell back from 64-bit device addressing:\n{text}"
    );
    let stores = text.lines().filter(|line| line.contains("OpStore")).count();
    assert!(stores >= 6, "resource copy dropped a token word:\n{text}");
    if let Some(evidence) = std::env::var_os("GPU_POINTER_EVIDENCE") {
        let evidence = PathBuf::from(evidence);
        std::fs::create_dir_all(&evidence).unwrap();
        std::fs::copy(&path, evidence.join("resource-copy.spv")).unwrap();
        std::fs::write(evidence.join("resource-copy.spvasm"), text).unwrap();
    }
    std::fs::remove_dir_all(out).unwrap();
}

#[test]
fn integer_resource_copies_do_not_hide_constant_pointer_addressing() {
    let source = include_str!("fixtures/argument_device_resource_copy.ll")
        .replace("%texture = load ptr addrspace(1)", "%texture = load i64")
        .replace("store ptr addrspace(1) %texture,", "store i64 %texture,")
        .replace("%sampler = load ptr addrspace(2)", "%sampler = load i64")
        .replace("store ptr addrspace(2) %sampler,", "store i64 %sampler,")
        .replace("ptr addrspace(2) %data, i64 %wide", "ptr addrspace(2) %data, i64 5")
        .replace("ptr addrspace(1) %observed, i64 %wide", "ptr addrspace(1) %observed, i64 1")
        .replace(
            "  store ptr addrspace(2) %data,",
            "  %offset_data = getelementptr i32, ptr addrspace(2) %data, i64 4\n  store ptr addrspace(2) %offset_data,",
        );
    let (spv, _) = garnetxlate::translate_text(&source, [2, 1, 1]).unwrap();
    let out = std::env::temp_dir().join(format!("rdna4-constant-copy-{}", std::process::id()));
    std::fs::create_dir_all(&out).unwrap();
    let path = out.join("copy.spv");
    std::fs::write(&path, spv).unwrap();
    let validation = std::process::Command::new("spirv-val")
        .args(["--target-env", "vulkan1.2"])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        validation.status.success(),
        "{}",
        String::from_utf8_lossy(&validation.stderr)
    );
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let text = String::from_utf8(disassembly.stdout).unwrap();
    assert!(
        text.contains("OpMemoryModel PhysicalStorageBuffer64 GLSL450"),
        "{text}"
    );
    assert!(text.contains("OpConvertUToPtr"), "{text}");
    std::fs::remove_dir_all(out).unwrap();
}

#[test]
fn thread_to_device_sampler_stores_are_rejected() {
    let input = include_str!("fixtures/argument_sampler_thread_store.ll");
    let constant = input.replace(
        "store ptr addrspace(2) %input,",
        "store ptr addrspace(2) @__air_sampler_state,",
    ) + "\n@__air_sampler_state = internal addrspace(2) constant [2 x i64] [i64 34901797601017929, i64 0], align 8\n!air.sampler_states = !{!8}\n!8 = !{!\"air.sampler_state\", ptr addrspace(2) @__air_sampler_state}\n";
    for (name, source) in [("parameter", input), ("constexpr", constant.as_str())] {
        let error = garnetxlate::translate_text(source, [1, 1, 1])
            .expect_err("thread sampler became an encoded device sampler");
        assert!(!error.is_empty(), "{name} rejection had no reason");
        if let Some(evidence) = std::env::var_os("GPU_POINTER_EVIDENCE") {
            let evidence = PathBuf::from(evidence);
            std::fs::create_dir_all(&evidence).unwrap();
            std::fs::write(
                evidence.join(format!("sampler-{name}-rejection.txt")),
                error,
            )
            .unwrap();
        }
    }
}
