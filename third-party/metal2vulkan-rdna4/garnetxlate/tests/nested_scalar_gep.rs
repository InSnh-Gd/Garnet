use std::collections::HashMap;

const SOURCE: &str = include_str!("fixtures/nested_scalar_gep.ll");

fn validate_paths(label: &str, spirv: &[u8], expected: Vec<Vec<Option<u32>>>) {
    let directory =
        std::env::temp_dir().join(format!("nested-scalar-gep-{}-{label}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("selected.spv");
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
    let folded = directory.join("folded.spv");
    let optimization = std::process::Command::new("spirv-opt")
        .arg("--ccp")
        .arg(&path)
        .arg("-o")
        .arg(&folded)
        .output()
        .unwrap();
    assert!(optimization.status.success());
    let disassembly = std::process::Command::new("spirv-dis")
        .arg(&folded)
        .output()
        .unwrap();
    assert!(disassembly.status.success());
    let text = String::from_utf8(disassembly.stdout).unwrap();
    let constants = text
        .lines()
        .filter_map(|line| {
            let words = line.split_whitespace().collect::<Vec<_>>();
            (words.get(2) == Some(&"OpConstant"))
                .then(|| (words[0], words[4].parse::<u32>().unwrap()))
        })
        .collect::<HashMap<_, _>>();
    let paths = text
        .lines()
        .filter(|line| {
            line.contains("= OpAccessChain ") || line.contains("= OpInBoundsAccessChain ")
        })
        .map(|line| {
            line.split_whitespace()
                .skip(5)
                .map(|index| constants.get(index).copied())
                .collect::<Vec<_>>()
        })
        .collect::<Vec<_>>();
    assert_eq!(paths, expected, "{text}");
    std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn selected_nested_array_constant_offset_preserves_byte_address() {
    for space in [1, 2] {
        for offset in [0, 1, 2] {
            let source = SOURCE
                .replace("addrspace(1)", &format!("addrspace({space})"))
                .replace("%selected, i32 1", &format!("%selected, i32 {offset}"));
            let (spirv, _) = garnetxlate::translate_text(&source, [1, 1, 1]).unwrap();
            validate_paths(
                &format!("space{space}-offset{offset}"),
                &spirv,
                vec![vec![Some(0), Some(0), Some(1), Some(offset)]; 2],
            );
        }
    }
}

#[test]
fn selected_nested_array_dynamic_offset_preserves_byte_address() {
    let source = SOURCE
        .replace(
            "  %next = getelementptr",
            "  %bounded = and i32 %index, 3\n  %next = getelementptr",
        )
        .replace("%selected, i32 1", "%selected, i32 %bounded");
    let (spirv, _) = garnetxlate::translate_text(&source, [1, 1, 1]).unwrap();
    validate_paths(
        "dynamic",
        &spirv,
        vec![vec![Some(0), Some(0), Some(1), None]; 2],
    );
}

#[test]
fn linked_selected_nested_array_offset_preserves_byte_address() {
    let helper = SOURCE
        .split("!air.vertex")
        .next()
        .unwrap()
        .replace("@nested_scalar_gep", "@walk");
    let body = SOURCE
        .split("entry:")
        .nth(1)
        .unwrap()
        .split("\n}")
        .next()
        .unwrap();
    let entry = SOURCE.replace(body, "\n  %out = call <4 x float> @walk.MTL_VISIBLE_FN_REF(ptr addrspace(1) %data, ptr addrspace(1) %other, i32 %index)\n  ret <4 x float> %out")
        + "\ndeclare <4 x float> @walk.MTL_VISIBLE_FN_REF(ptr addrspace(1), ptr addrspace(1), i32) section \"air.externally_defined\"\n!air.visible_function_references = !{!7}\n!7 = !{!\"air.visible_function_reference\", ptr @walk.MTL_VISIBLE_FN_REF, !\"walk\"}\n";
    let (spirv, _) =
        garnetxlate::translate_text_linked(&entry, [1, 1, 1], &[("walk", &helper)]).unwrap();
    validate_paths(
        "linked",
        &spirv,
        vec![vec![Some(0), Some(0), Some(1), Some(1)]; 2],
    );
}
