use std::collections::{HashMap, HashSet};

const THREAD: &str = include_str!("fixtures/argument_sampler_thread_store.ll");
const DEVICE: &str = include_str!("fixtures/argument_device_resource_copy.ll");
const RULE: &str = "MSL 2.1 section 2.12.1: thread/constexpr sampler";

fn translate_via_c_abi8(source: &str) -> Vec<u8> {
    let mut spirv = std::ptr::null_mut();
    let mut metadata = std::ptr::null_mut();
    let mut spirv_len = 0;
    let mut metadata_len = 0;
    let mut error = [0i8; 512];
    let local = [2, 1, 1];
    let status = unsafe {
        garnetxlate::garnetx_translate(
            source.as_ptr(),
            source.len(),
            local.as_ptr(),
            &mut spirv,
            &mut spirv_len,
            &mut metadata,
            &mut metadata_len,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    assert_eq!(
        status,
        garnetxlate::RC_OK,
        "{}",
        unsafe { std::ffi::CStr::from_ptr(error.as_ptr()) }.to_string_lossy()
    );
    assert!(!spirv.is_null() && !metadata.is_null());
    let bytes = unsafe { std::slice::from_raw_parts(spirv, spirv_len) }.to_vec();
    let reflected: serde_json::Value =
        serde_json::from_slice(unsafe { std::slice::from_raw_parts(metadata, metadata_len) })
            .unwrap();
    assert_eq!(reflected["normalized_sampler_coordinates"], true);
    unsafe {
        garnetxlate::garnetx_free(spirv, spirv_len);
        garnetxlate::garnetx_free(metadata, metadata_len);
    }
    bytes
}

fn illegal_sources() -> Vec<(&'static str, String)> {
    let constexpr = THREAD.replace("ptr addrspace(2) %input, ptr addrspace(1) %slot", "ptr addrspace(2) @__air_sampler_state, ptr addrspace(1) %slot")
        + "\n@__air_sampler_state = internal addrspace(2) constant [2 x i64] [i64 34901797601017929, i64 0], align 8\n!air.sampler_states = !{!8}\n!8 = !{!\"air.sampler_state\", ptr addrspace(2) @__air_sampler_state}\n";
    let freeze = THREAD
        .replace(
            "  store ptr",
            "  %forward = freeze ptr addrspace(2) %input\n  store ptr",
        )
        .replace(
            "store ptr addrspace(2) %input",
            "store ptr addrspace(2) %forward",
        );
    let select = THREAD.replace("  store ptr", "  %forward = select i1 true, ptr addrspace(2) %input, ptr addrspace(2) %input\n  store ptr")
        .replace("store ptr addrspace(2) %input", "store ptr addrspace(2) %forward");
    let phi = THREAD.replace("  store ptr", "  br i1 true, label %left, label %right\nleft:\n  br label %merge\nright:\n  br label %merge\nmerge:\n  %forward = phi ptr addrspace(2) [ %input, %left ], [ %input, %right ]\n  store ptr")
        .replace("store ptr addrspace(2) %input", "store ptr addrspace(2) %forward");
    let helper = THREAD.replace("  store ptr addrspace(2) %input, ptr addrspace(1) %slot, align 8", "  call void @sink(ptr addrspace(1) %slot, ptr addrspace(2) %input)")
        + "\ndefine internal void @sink(ptr addrspace(1) %dst, ptr addrspace(2) %s) {\nentry:\n  store ptr addrspace(2) %s, ptr addrspace(1) %dst, align 8\n  ret void\n}\n";
    let returned = THREAD.replace("  store ptr", "  %forward = call ptr addrspace(2) @identity(ptr addrspace(2) %input)\n  store ptr")
        .replace("store ptr addrspace(2) %input", "store ptr addrspace(2) %forward")
        + "\ndefine internal ptr addrspace(2) @identity(ptr addrspace(2) %s) {\nentry:\n  ret ptr addrspace(2) %s\n}\n";
    let spilled = THREAD.replace("  store ptr", "  %local = alloca ptr addrspace(2), align 8\n  store ptr addrspace(2) %input, ptr %local, align 8\n  %forward = load ptr addrspace(2), ptr %local, align 8\n  store ptr")
        .replace("store ptr addrspace(2) %input, ptr addrspace(1)", "store ptr addrspace(2) %forward, ptr addrspace(1)");
    let integer = THREAD.replace(
        "  store ptr addrspace(2) %input,",
        "  %forward = ptrtoint ptr addrspace(2) %input to i64\n  store i64 %forward,",
    );
    let static_gep = constexpr.replace("  store ptr", "  %forward = getelementptr [2 x i64], ptr addrspace(2) @__air_sampler_state, i64 0, i64 0\n  store ptr")
        .replace("store ptr addrspace(2) @__air_sampler_state,", "store ptr addrspace(2) %forward,");
    vec![
        ("parameter", THREAD.into()),
        ("constexpr", constexpr),
        ("freeze", freeze),
        ("select", select),
        ("phi", phi),
        ("helper-store", helper),
        ("helper-return", returned),
        ("thread-spill", spilled),
        ("integer-token", integer),
        ("constexpr-gep", static_gep),
    ]
}

#[test]
fn illegal_sampler_origins_have_the_source_rule_error() {
    for (name, source) in illegal_sources() {
        let error = garnetxlate::translate_text(&source, [1, 1, 1]).unwrap_err();
        assert!(error.contains(RULE), "{name}: {error}");
        if let Some(evidence) = std::env::var_os("SAMPLER_COPY_EVIDENCE") {
            let evidence = std::path::PathBuf::from(evidence);
            std::fs::create_dir_all(&evidence).unwrap();
            std::fs::write(evidence.join(format!("illegal-{name}.ll")), source).unwrap();
            std::fs::write(evidence.join(format!("illegal-{name}.txt")), error).unwrap();
        }
    }
}

#[test]
fn c_abi_refusal_publishes_neither_spirv_nor_reflection() {
    for (name, source) in illegal_sources() {
        let mut spirv = std::ptr::dangling_mut();
        let mut metadata = std::ptr::dangling_mut();
        let mut spirv_len = 123;
        let mut metadata_len = 456;
        let mut error = [0i8; 512];
        let local = [1, 1, 1];
        let status = unsafe {
            garnetxlate::garnetx_translate(
                source.as_ptr(),
                source.len(),
                local.as_ptr(),
                &mut spirv,
                &mut spirv_len,
                &mut metadata,
                &mut metadata_len,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        assert_eq!(status, garnetxlate::RC_REFUSED, "{name}");
        assert!(spirv.is_null() && metadata.is_null(), "{name}");
        assert_eq!((spirv_len, metadata_len), (0, 0), "{name}");
        let error = unsafe { std::ffi::CStr::from_ptr(error.as_ptr()) }
            .to_str()
            .unwrap();
        assert!(error.contains(RULE), "{name}: {error}");
    }
}

#[test]
fn argument_buffer_sampler_copy_preserves_each_loaded_token_word() {
    for address_space in [1, 2] {
        let mut source = DEVICE.replace(
            "ptr addrspace(2) %source",
            &format!("ptr addrspace({address_space}) %source"),
        );
        for field in ["texture_source", "sampler_source", "data_source"] {
            source = source.replace(
                &format!("ptr addrspace(2) %{field}"),
                &format!("ptr addrspace({address_space}) %{field}"),
            );
        }
        source = source.replace(
            "!\"air.read\", !\"air.address_space\", i32 2, !\"air.struct_type_info\"",
            &format!("!\"air.read\", !\"air.address_space\", i32 {address_space}, !\"air.struct_type_info\""),
        );
        let spirv = translate_via_c_abi8(&source);
        let directory = std::env::temp_dir().join(format!(
            "rdna4-sampler-copy-{}-{address_space}",
            std::process::id()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("copy.spv");
        std::fs::write(&path, spirv).unwrap();
        let validated = std::process::Command::new("spirv-val")
            .args(["--target-env", "vulkan1.2"])
            .arg(&path)
            .output()
            .unwrap();
        assert!(
            validated.status.success(),
            "{}",
            String::from_utf8_lossy(&validated.stderr)
        );
        let disassembled = std::process::Command::new("spirv-dis")
            .arg(&path)
            .output()
            .unwrap();
        assert!(disassembled.status.success());
        let text = String::from_utf8(disassembled.stdout).unwrap();
        let mut definitions = HashMap::<String, Vec<String>>::new();
        let mut stores = HashSet::new();
        for line in text.lines() {
            let words = line.split_whitespace().collect::<Vec<_>>();
            if words.get(1) == Some(&"=") {
                definitions.insert(
                    words[0].into(),
                    words[2..].iter().map(|word| (*word).into()).collect(),
                );
            }
            if words.first() == Some(&"OpStore") {
                stores.insert(words[2].to_owned());
            }
        }
        let mut halves = HashMap::<String, u8>::new();
        for stored in stores {
            let Some(conversion) = definitions.get(&stored) else {
                continue;
            };
            if conversion.first().map(String::as_str) != Some("OpUConvert") {
                continue;
            }
            let source = &conversion[2];
            let Some(definition) = definitions.get(source) else {
                continue;
            };
            match definition[0].as_str() {
                "OpBitwiseOr" => *halves.entry(source.clone()).or_default() |= 1,
                "OpShiftRightLogical" if definition[3] == "%ulong_32" => {
                    *halves.entry(definition[2].clone()).or_default() |= 2;
                }
                _ => {}
            }
        }
        assert!(text.contains("PhysicalStorageBuffer64"), "{text}");
        assert!(
            halves.values().filter(|mask| **mask == 3).count() >= 3,
            "texture/sampler copy must store both halves of each token: {text}"
        );
        if let Some(evidence) = std::env::var_os("SAMPLER_COPY_EVIDENCE") {
            let evidence = std::path::PathBuf::from(evidence);
            std::fs::create_dir_all(&evidence).unwrap();
            std::fs::copy(&path, evidence.join(format!("copy-as{address_space}.spv"))).unwrap();
            std::fs::write(
                evidence.join(format!("copy-as{address_space}.spvasm")),
                text,
            )
            .unwrap();
            std::fs::write(evidence.join(format!("copy-as{address_space}.ll")), source).unwrap();
        }
        std::fs::remove_dir_all(directory).unwrap();
    }
}
