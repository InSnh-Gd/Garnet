use std::collections::{BTreeSet, HashMap};

const ENTRY: &str = include_str!("fixtures/constant_helper_entry.ll");
const HELPER: &str = include_str!("fixtures/constant_helper_body.ll");

fn constant_offset(id: &str, definitions: &HashMap<String, Vec<String>>) -> u64 {
    let Some(words) = definitions.get(id) else {
        return 0;
    };
    match words[0].as_str() {
        "OpConstant" => words[2].parse().unwrap(),
        "OpIAdd" => {
            constant_offset(&words[2], definitions) + constant_offset(&words[3], definitions)
        }
        _ => 0,
    }
}

fn validate_addresses(label: &str, entry: &str, helper: &str, expected: &[u64]) {
    let (spirv, _) = garnetxlate::translate_text_linked(entry, [1, 1, 1], &[("read_tail", helper)])
        .expect("a constant-buffer cursor must reach the helper as an address");
    let directory =
        std::env::temp_dir().join(format!("constant-helper-{}-{label}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("helper.spv");
    std::fs::write(&path, spirv).unwrap();
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
        .arg("--raw-id")
        .arg(&path)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let text = String::from_utf8(disassembly.stdout).unwrap();
    let definitions = text
        .lines()
        .filter_map(|line| {
            let words = line
                .split_whitespace()
                .map(str::to_string)
                .collect::<Vec<_>>();
            (words.get(1).map(String::as_str) == Some("="))
                .then(|| (words[0].clone(), words[2..].to_vec()))
        })
        .collect::<HashMap<_, _>>();
    let offsets = definitions
        .values()
        .filter(|words| words[0] == "OpConvertUToPtr")
        .map(|words| constant_offset(&words[2], &definitions))
        .collect::<BTreeSet<_>>();
    assert_eq!(offsets, expected.iter().copied().collect(), "{text}");
    assert!(
        definitions.values().any(|words| words[0] == "OpIMul"
            && words[2..]
                .iter()
                .any(|id| constant_offset(id, &definitions) == 4)),
        "{text}"
    );
    std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn linked_constant_and_device_helpers_preserve_cursor_bytes() {
    for space in [1, 2] {
        let entry = ENTRY
            .replace("addrspace(2)", &format!("addrspace({space})"))
            .replace(
                "!\"air.address_space\", i32 2",
                &format!("!\"air.address_space\", i32 {space}"),
            );
        let helper = HELPER.replace("addrspace(2)", &format!("addrspace({space})"));
        validate_addresses(&format!("space{space}"), &entry, &helper, &[56, 60, 72]);
    }
}

#[test]
fn repeated_constant_helper_calls_preserve_distinct_cursors() {
    let entry = ENTRY.replace("  ret <4 x float> %color", "  %tail2 = getelementptr inbounds %Globals, ptr addrspace(2) %globals, i64 2, i32 0, i64 0, i64 0\n  %color2 = call <4 x float> @read_tail.MTL_VISIBLE_FN_REF(ptr addrspace(2) %tail2)\n  %sum = fadd <4 x float> %color, %color2\n  ret <4 x float> %sum");
    validate_addresses("repeated", &entry, HELPER, &[56, 60, 72, 112, 116, 128]);
}
