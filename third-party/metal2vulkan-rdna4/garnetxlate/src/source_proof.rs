//! Source fragment facts bound to the exact cached SPIR-V identity.

use metal2vulkan::{
    SourceSampleCoordinate, UniformControlEntry, UniformControlProof, UniformControlValue,
};
use serde_json::Value;
use sha2::{Digest, Sha256};

pub(crate) fn uniform_control_proof_bytes(proof: &UniformControlProof, spv: &[u8]) -> Value {
    serde_json::json!({
        "version": if proof.sampling_coordinates.is_empty() { 1 } else { 2 },
        "spv_sha256": sha256_hex(spv),
        "entries": proof.entries.iter().map(|entry| {
            serde_json::json!({"function": entry.function, "name": entry.name})
        }).collect::<Vec<_>>(),
        "values": proof.values.iter().map(|fact| {
            serde_json::json!({"function": fact.function, "value": fact.value})
        }).collect::<Vec<_>>(),
        "sampling_coordinates": proof.sampling_coordinates.iter().map(|fact| {
            serde_json::json!({"sample": fact.sample, "coordinate": fact.coordinate})
        }).collect::<Vec<_>>(),
    })
}

fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut text = String::with_capacity(digest.len() * 2);
    for byte in digest {
        use std::fmt::Write as _;
        write!(&mut text, "{byte:02x}").expect("writing to a String cannot fail");
    }
    text
}

fn proof_u32(value: &Value, what: &str) -> Result<u32, String> {
    value
        .as_u64()
        .and_then(|value| u32::try_from(value).ok())
        .ok_or_else(|| format!("invalid uniform control proof {what}"))
}

pub(crate) fn verify_uniform_control_proof(
    metadata: &Value,
    spv: &[u8],
) -> Result<Option<UniformControlProof>, String> {
    let Some(proof) = metadata.get("uniform_control_proof") else {
        return Ok(None);
    };
    let version = proof.get("version").and_then(Value::as_u64);
    if !matches!(version, Some(1 | 2)) {
        return Err("unsupported uniform control proof version".into());
    }
    let claimed_hash = proof
        .get("spv_sha256")
        .and_then(Value::as_str)
        .ok_or("uniform control proof has no SPIR-V hash")?;
    if claimed_hash != sha256_hex(spv) {
        return Err("uniform control proof does not match the cached SPIR-V bytes".into());
    }

    let mut entries = proof
        .get("entries")
        .and_then(Value::as_array)
        .ok_or("uniform control proof has no entry list")?
        .iter()
        .map(|entry| {
            Ok(UniformControlEntry {
                function: proof_u32(
                    entry
                        .get("function")
                        .ok_or("uniform control proof entry has no function")?,
                    "entry function",
                )?,
                name: entry
                    .get("name")
                    .and_then(Value::as_str)
                    .ok_or("uniform control proof entry has no name")?
                    .to_string(),
            })
        })
        .collect::<Result<Vec<_>, String>>()?;
    let entry_count = entries.len();
    entries.sort_by(|left, right| {
        (left.function, left.name.as_str()).cmp(&(right.function, right.name.as_str()))
    });
    entries.dedup();
    if entries.len() != entry_count {
        return Err("uniform control proof repeats an entry".into());
    }

    let mut values = proof
        .get("values")
        .and_then(Value::as_array)
        .ok_or("uniform control proof has no value list")?
        .iter()
        .map(|fact| {
            Ok(UniformControlValue {
                function: proof_u32(
                    fact.get("function")
                        .ok_or("uniform control proof value has no function")?,
                    "value function",
                )?,
                value: proof_u32(
                    fact.get("value")
                        .ok_or("uniform control proof value has no id")?,
                    "value id",
                )?,
            })
        })
        .collect::<Result<Vec<_>, String>>()?;
    let value_count = values.len();
    values.sort_unstable_by_key(|fact| (fact.function, fact.value));
    values.dedup();
    if values.len() != value_count {
        return Err("uniform control proof repeats a value".into());
    }
    let mut sampling_coordinates = if version == Some(2) {
        proof
            .get("sampling_coordinates")
            .and_then(Value::as_array)
            .ok_or("source proof has no sampling coordinate list")?
            .iter()
            .map(|fact| {
                Ok(SourceSampleCoordinate {
                    sample: proof_u32(
                        fact.get("sample")
                            .ok_or("sampling coordinate has no sample id")?,
                        "coordinate sample",
                    )?,
                    coordinate: proof_u32(
                        fact.get("coordinate")
                            .ok_or("sampling coordinate has no source id")?,
                        "coordinate source",
                    )?,
                })
            })
            .collect::<Result<Vec<_>, String>>()?
    } else {
        if proof
            .get("sampling_coordinates")
            .and_then(Value::as_array)
            .is_some_and(|facts| !facts.is_empty())
        {
            return Err("version 1 source proof cannot carry sampling coordinates".into());
        }
        Vec::new()
    };
    sampling_coordinates.sort_unstable_by_key(|fact| (fact.sample, fact.coordinate));
    if sampling_coordinates
        .windows(2)
        .any(|facts| facts[0].sample == facts[1].sample)
    {
        return Err("source proof repeats a sampling coordinate".into());
    }
    Ok(Some(UniformControlProof {
        entries,
        values,
        sampling_coordinates,
    }))
}
