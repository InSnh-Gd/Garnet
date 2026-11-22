# RDNA4 translator C ABI

ABI 3 retains the ABI 2 function signatures and adds three capability-aware exports:

- `garnetx_translate_with_capabilities`
- `garnetx_translate_linked_with_capabilities`
- `garnetx_lower_argument_resources_with_capabilities`

Each new export takes its ABI 2 arguments followed by a pointer to this 24-byte record:

```c
struct rdna4x_backend_capabilities {
    uint32_t struct_size;                    /* 24 */
    uint32_t version;                        /* 1 */
    uint32_t flags;
    uint32_t subgroup_size;
    uint32_t subgroup_supported_stages;      /* VkShaderStageFlags */
    uint32_t subgroup_supported_operations;  /* VkSubgroupFeatureFlags */
};
```

Flag bit 0 means the actual platform compiler exposes the optional query-LOD language
extension. Bit 1 means the actual backend device supports that extension. Unknown flag bits,
record sizes, or record versions are bad arguments. Query shaders require both bits, a fragment
entry, and explicit `!air.language_version` metadata for Metal 2.2 or newer on macOS. The query
functions are outside the fixed Metal 2.0/2.1 contract. Other shaders do not require these bits.

The backend chooses quad broadcast only when the actual subgroup properties report fragment
support, `VK_SUBGROUP_FEATURE_QUAD_BIT`, and a subgroup size of at least four. Otherwise it uses
uniform descriptor enumeration bounded by the producer-validated resource table and actual
SSBO allocation length. Fragment quad operations do not require
`subgroupQuadOperationsInAllStages`. Both paths require the same derivative-uniform dynamic
instance proof, including helper calls and termination. They preserve `OpImageQueryLod` semantics.

The table producer must validate the ABI header, counts, offsets and descriptor allocations,
provide every required typed dummy, keep ordinary and comparison sampler representations distinct,
and keep this internal table immutable during the invocation. ABI 8/9 image query rows contain
eight words; words 6 and 7 identify the query shape tag and multisampling. Replay filters these
rows before loading descriptors of the query's type. ABI 9 obtains the typed dummy index from
the canonical null-shape registry and stage binding 674's ordinal relocation. Historical ABI 8
cached inputs retain their live-image-count plus shape-tag calculation. The query row tag is
independent of the ABI 9 registry ordinal.

ABI 10 adds uniform/storage texel-buffer token categories 3 and 4. Their offset/count pairs occupy
header words 7..10, leaving words 0..6 and ABI 9 null-shape metadata unchanged. The translator
emits this interface for indirect texture-buffer graphs, but the current native consumer does not
accept ABI 10; translation and SPIR-V validation are not live runtime support or GPU evidence.

Metadata records `source_language_version`, `query_lod_policy` (currently 1), and
`query_lod_replay`. Cached lowering refuses a query module whose language proof is missing or
whose already-applied policy/backend differs. It preserves the existing SPIR-V-bound uniform
control proof and publishes binding 673 when replay adds the query interface. Cache both SPIR-V
and metadata atomically; include the ABI, replay policy, all six normalized capability fields and
the translator identity in cache and postpass identities. A changed replay strategy requires
translation from AIR rather than replaying an already-lowered cache entry.

The historical Rust helpers and ABI 2 exports keep their previous capability-free interfaces.
Native ABI 3 consumers must use the new exports on direct, linked, and cached paths. The public
`supportsQueryTextureLOD` property must reflect actual language/device support.

Focused SPIR-V checks do not establish GPU query values, edge/helper behavior, mip selection,
sampler clamps, or precision. Those need execution against the corresponding reference oracle.

`src/query_lod.rs` owns capability validation, the ABI 3 exports and language/cache policy.
The product's `resources/lod_query/` modules separate query analysis, coordinates, quad replay
and descriptor enumeration. They use the shared fragment control/value proof, including readonly
buffer aliases and pure helper results; no independent LOD uniformity analysis is maintained.

`src/source_proof.rs` owns the serialized fragment source facts. Proof version 1
retains the existing entry/value uniformity contract. Version 2 also records each
implicit sample's original spatial coordinate, before sampler coordinate-domain
conversion, array-layer composition and pixel offsets. Both versions bind their
IDs to the exact SPIR-V SHA-256. Cached lowering validates entries, uniqueness,
sample opcodes, function ownership and coordinate dominance before using them.
It refuses a normalized nonuniform implicit sample without its coordinate fact;
an older cache cannot silently regain the former derivative-of-normalized-data
behavior. The native cache and postpass identities must change with this policy.

For the normalized sampler contract, derivatives use the source coordinate.
Image-size queries and sampler flags affect the sample coordinate at its original
site, not the derivative basis. Pixel sampler representations clamp to mip zero
and require equal min/mag filters, so those gradients cannot alter their mip or
filter selection. Source-provided gradients and explicit levels retain their own
operands. Array layers remain part of the sampling coordinate alone.
