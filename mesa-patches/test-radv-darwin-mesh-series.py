#!/usr/bin/env python3
"""Verify Mesa RADV Darwin patch 0029 (GFX12 meshShader on GFX ring 0).

Checks:
  1. Contiguous 0001..0029 patch series and unified-diff hunk count integrity.
  2. Clean `git apply --check` and `patch -p1 --fuzz=0` replay of 0029 against
     exact post-0028 `src/amd/vulkan/radv_physical_device.c` context (or full
     `0001..0029` replay on a pinned Mesa tarball via `--mesa-archive`), with
     zero `.rej`/`.orig` files.
  3. Compilation and execution of the real patched `radv_mesh_enabled()`,
     `radv_taskmesh_enabled()`, extension/feature/stage-mask wiring, and
     conservative `meshAndTaskShaderDerivatives` gating under
     `-std=gnu11 -Wall -Wextra -Werror` (`-O2` and `-fsanitize=address,undefined`).
"""

from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile


PATCH_RE = re.compile(r"^(\d{4})-.+\.patch$")
TARGET_REL = "src/amd/vulkan/radv_physical_device.c"

# Exact post-0028 line blocks from mesa@f5cb8ee032adabef599ae892ec4e42d796b84da9
# after applying patches 0001..0028 (start_line_1based, lines).
POST_0028_BLOCKS: list[tuple[int, list[str]]] = [
    (
        72,
        [
            "   /* SQTT / SPM interfere with the register states for perf counters. */",
            "   return pdev->info.gfx_level >= GFX10_3 && pdev->info.gfx_level <= GFX12 &&",
            "          !(instance->vk.trace_mode & RADV_TRACE_MODE_RGP);",
            "}",
            "",
            "static bool",
            "radv_taskmesh_enabled(const struct radv_physical_device *pdev)",
            "{",
            "   const struct radv_instance *instance = radv_physical_device_instance(pdev);",
            "",
            "   if (RADV_DEBUG(instance, NO_MESH_SHADER))",
            "      return false;",
            "",
            "   return pdev->use_ngg && !pdev->use_llvm && pdev->info.gfx_level >= GFX10_3 && radv_compute_queue_enabled(pdev) &&",
            "          !pdev->info.has_taskmesh_indirect0_bug;",
            "}",
            "",
            "bool",
            "radv_spm_trace_enabled(const struct radv_physical_device *pdev)",
            "{",
            "   const struct radv_instance *instance = radv_physical_device_instance(pdev);",
        ],
    ),
    (
        931,
        [
            "      .EXT_legacy_vertex_attributes = !pdev->use_llvm,",
            "      .EXT_line_rasterization = true,",
            "      .EXT_load_store_op_none = true,",
            "      .EXT_map_memory_placed = true,",
            "      .EXT_memory_budget = true,",
            "      .EXT_memory_priority = true,",
            "      .EXT_mesh_shader = radv_taskmesh_enabled(pdev),",
            "      .EXT_multi_draw = true,",
            "      .EXT_multisampled_render_to_single_sampled = radv_msrtss_enabled(pdev),",
            "      .EXT_mutable_descriptor_type = true, /* Trivial promotion from VALVE. */",
            "      .EXT_nested_command_buffer = true,",
            "      .EXT_non_seamless_cube_map = true,",
            "      .EXT_pci_bus_info = true,",
        ],
    ),
    (
        1034,
        [
            "static void",
            "radv_physical_device_get_features(const struct radv_physical_device *pdev, struct vk_features *features)",
            "{",
            "   const struct radv_instance *instance = radv_physical_device_instance(pdev);",
            "   bool taskmesh_en = radv_taskmesh_enabled(pdev);",
            "   bool has_perf_query = radv_perf_query_supported(pdev);",
            "   bool has_shader_image_float_minmax = pdev->info.gfx_level <= GFX7 || pdev->info.gfx_level == GFX10 ||",
            "                                        pdev->info.gfx_level == GFX10_3 || pdev->info.gfx_level >= GFX12;",
            "   bool has_fragment_shader_interlock = radv_has_pops(pdev);",
            "   const bool enable_sparse = radv_sparse_enabled(pdev);",
            "",
            "   *features = (struct vk_features){",
            "      /* Vulkan 1.0 */",
            "      .robustBufferAccess = true,",
        ],
    ),
    (
        1390,
        [
            "      /* VK_EXT_image_view_min_lod */",
            "      .minLod = true,",
            "",
            "      /* VK_EXT_mesh_shader */",
            "      .meshShader = taskmesh_en,",
            "      .taskShader = taskmesh_en,",
            "      .multiviewMeshShader = taskmesh_en,",
            "      .primitiveFragmentShadingRateMeshShader = taskmesh_en,",
            "      .meshShaderQueries = false,",
            "",
            "      /* VK_EXT_depth_clip_control */",
            "      .depthClipControl = true,",
        ],
    ),
    (
        1817,
        [
            "   const bool has_fp16 = radv_shader_fp16_enabled(pdev);",
            "   const bool enable_sparse = radv_sparse_enabled(pdev);",
            "",
            "   VkShaderStageFlags taskmesh_stages =",
            "      radv_taskmesh_enabled(pdev) ? VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT : 0;",
            "   VkShaderStageFlags rt_stages = radv_enable_rt(pdev) ? RADV_RT_STAGE_BITS : 0;",
            "",
            "   bool accel_dot = pdev->info.compiler_info.has_accelerated_dot_product;",
            "   bool gfx11plus = pdev->info.gfx_level >= GFX11;",
        ],
    ),
    (
        2263,
        [
            "      /* VK_EXT_mesh_shader */",
            "      .maxTaskWorkGroupTotalCount = 4194304, /* 2^22 min required */",
            "      .maxTaskWorkGroupCount = {65535, 65535, 65535},",
            "      .maxTaskWorkGroupInvocations = 1024,",
            "      .maxTaskWorkGroupSize = {1024, 1024, 1024},",
            "      .maxTaskPayloadSize = 16384, /* 16K min required */",
            "      .maxTaskSharedMemorySize = 65536,",
            "      .maxTaskPayloadAndSharedMemorySize = 65536,",
            "",
            "      .maxMeshWorkGroupTotalCount = 4194304, /* 2^22 min required */",
            "      .maxMeshWorkGroupCount = {65535, 65535, 65535},",
            "      .maxMeshWorkGroupInvocations = 256, /* Max NGG HW limit */",
            "      .maxMeshWorkGroupSize = {256, 256, 256},",
            "      .maxMeshOutputMemorySize = 32 * 1024,                   /* 32K min required */",
            "      .maxMeshSharedMemorySize = 28672,                       /* 28K min required */",
            "      .maxMeshPayloadAndSharedMemorySize = 16384 + 28672,     /* 28K min required */",
            "      .maxMeshPayloadAndOutputMemorySize = 16384 + 32 * 1024, /* 47K min required */",
            "      .maxMeshOutputComponents = 128,                         /* 32x vec4 min required */",
            "      .maxMeshOutputVertices = 256,",
            "      .maxMeshOutputPrimitives = 256,",
            "      .maxMeshOutputLayers = 8,",
            "      .maxMeshMultiviewViewCount = AC_MULTIVIEW_MAX_VIEWS,",
            "      .meshOutputPerVertexGranularity = 1,",
            "      .meshOutputPerPrimitiveGranularity = 1,",
            "",
            "      .maxPreferredTaskWorkGroupInvocations = 64,",
            "      .maxPreferredMeshWorkGroupInvocations = 128,",
            "      .prefersLocalInvocationVertexOutput = true,",
            "      .prefersLocalInvocationPrimitiveOutput = true,",
            "      .prefersCompactVertexOutput = true,",
            "      .prefersCompactPrimitiveOutput = false,",
        ],
    ),
    (
        2374,
        [
            "      .pipelineBinaryPrecompiledInternalCache = false,",
            "      .pipelineBinaryCompressedData = false,",
            "",
            "      /* VK_KHR_compute_shader_derivatives */",
            "      .meshAndTaskShaderDerivatives = radv_taskmesh_enabled(pdev),",
            "",
            "      /* VK_EXT_device_generated_commands */",
            "      .maxIndirectPipelineCount = 4096,",
        ],
    ),
]

HOST_HARNESS_C = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum amd_gfx_level {
   GFX10 = 10,
   GFX10_3 = 11,
   GFX11 = 12,
   GFX12 = 13,
};

enum amd_ip_type {
   AMD_IP_GFX = 0,
   AMD_IP_COMPUTE = 1,
   AMD_IP_MAX = 2,
};

typedef uint32_t VkShaderStageFlags;
#define VK_SHADER_STAGE_TASK_BIT_EXT (0x00000040u)
#define VK_SHADER_STAGE_MESH_BIT_EXT (0x00000080u)

#define RADV_DEBUG_NO_MESH_SHADER (1ull << 0)
#define RADV_DEBUG(instance, flag) (((instance)->debug_flags & RADV_DEBUG_##flag) != 0)
#define RADV_QUEUE_DISABLE_COMPUTE (1u << 1)

struct radv_instance {
   uint64_t debug_flags;
   uint32_t queue_disable_flags;
};

struct radeon_ip_info {
   uint32_t num_queues;
};

struct radeon_info {
   enum amd_gfx_level gfx_level;
   bool has_taskmesh_indirect0_bug;
   struct radeon_ip_info ip[AMD_IP_MAX];
};

struct radv_physical_device {
   const struct radv_instance *instance;
   bool use_ngg;
   bool use_llvm;
   struct radeon_info info;
};

struct vk_device_extension_table {
   bool EXT_mesh_shader;
};

struct vk_features {
   bool meshShader;
   bool taskShader;
   bool multiviewMeshShader;
   bool primitiveFragmentShadingRateMeshShader;
   bool meshShaderQueries;
};

struct vk_properties {
   bool meshAndTaskShaderDerivatives;
};

static const struct radv_instance *
radv_physical_device_instance(const struct radv_physical_device *pdev)
{
   return pdev->instance;
}

static bool
radv_compute_queue_enabled(const struct radv_physical_device *pdev)
{
   const struct radv_instance *instance = radv_physical_device_instance(pdev);
   return !(instance->queue_disable_flags & RADV_QUEUE_DISABLE_COMPUTE) &&
          pdev->info.ip[AMD_IP_COMPUTE].num_queues > 0;
}

#include "radv_mesh_helpers.inc"

static struct vk_device_extension_table
eval_extensions(const struct radv_physical_device *pdev)
{
   return (struct vk_device_extension_table){
#include "radv_mesh_ext.inc"
   };
}

static struct vk_features
eval_features(const struct radv_physical_device *pdev)
{
#include "radv_mesh_feat_vars.inc"
   return (struct vk_features){
#include "radv_mesh_feat_fields.inc"
   };
}

static VkShaderStageFlags
eval_taskmesh_stages(const struct radv_physical_device *pdev)
{
#include "radv_mesh_stages.inc"
   return taskmesh_stages;
}

static struct vk_properties
eval_properties(const struct radv_physical_device *pdev)
{
   return (struct vk_properties){
#include "radv_mesh_deriv.inc"
   };
}

static unsigned g_checks, g_failures;
#define CHECK(c)                                                                                   \
   do {                                                                                            \
      g_checks++;                                                                                  \
      if (!(c)) {                                                                                  \
         g_failures++;                                                                             \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                              \
      }                                                                                            \
   } while (0)

static struct radv_physical_device
make_pdev(const struct radv_instance *inst, enum amd_gfx_level gfx_level, bool use_ngg,
          bool use_llvm, uint32_t compute_queues, bool indirect0_bug)
{
   struct radv_physical_device pdev = {
      .instance = inst,
      .use_ngg = use_ngg,
      .use_llvm = use_llvm,
      .info =
         {
            .gfx_level = gfx_level,
            .has_taskmesh_indirect0_bug = indirect0_bug,
            .ip = {[AMD_IP_GFX] = {.num_queues = 1}, [AMD_IP_COMPUTE] = {.num_queues = compute_queues}},
         },
   };
   return pdev;
}

static void
check_case(const struct radv_physical_device *pdev, bool exp_mesh, bool exp_task)
{
   const bool mesh = radv_mesh_enabled(pdev);
   const bool task = radv_taskmesh_enabled(pdev);
   const struct vk_device_extension_table ext = eval_extensions(pdev);
   const struct vk_features feat = eval_features(pdev);
   const VkShaderStageFlags stages = eval_taskmesh_stages(pdev);
   const struct vk_properties props = eval_properties(pdev);

   CHECK(mesh == exp_mesh);
   CHECK(task == exp_task);
   CHECK(ext.EXT_mesh_shader == exp_mesh);
   CHECK(feat.meshShader == exp_mesh);
   CHECK(feat.taskShader == exp_task);
   CHECK(feat.multiviewMeshShader == exp_mesh);
   CHECK(feat.primitiveFragmentShadingRateMeshShader == exp_mesh);
   CHECK(feat.meshShaderQueries == false);

   const VkShaderStageFlags exp_stages =
      (exp_mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : 0u) |
      (exp_task ? VK_SHADER_STAGE_TASK_BIT_EXT : 0u);
   CHECK(stages == exp_stages);
   CHECK(props.meshAndTaskShaderDerivatives == exp_task);
}

static void
test_named_profiles(void)
{
   struct radv_instance inst = {.debug_flags = 0, .queue_disable_flags = 0};

   /* 1. GFX12 Darwin (RX 9060 XT / Navi 44, patch 0011 zeroes compute queues): mesh ON, task OFF. */
   struct radv_physical_device darwin_gfx12 = make_pdev(&inst, GFX12, true, false, 0, false);
   check_case(&darwin_gfx12, true, false);

   /* 2. Non-Darwin GFX12 with async compute queue: both mesh and task ON. */
   struct radv_physical_device linux_gfx12 = make_pdev(&inst, GFX12, true, false, 4, false);
   check_case(&linux_gfx12, true, true);

   /* 3. GFX10_3 and GFX11 follow the same split when compute queues == 0 vs > 0. */
   struct radv_physical_device gfx103_nocomp = make_pdev(&inst, GFX10_3, true, false, 0, false);
   check_case(&gfx103_nocomp, true, false);
   struct radv_physical_device gfx11_comp = make_pdev(&inst, GFX11, true, false, 1, false);
   check_case(&gfx11_comp, true, true);

   /* 4. Pre-GFX10_3 (GFX10): both OFF regardless of compute queues. */
   struct radv_physical_device gfx10_comp = make_pdev(&inst, GFX10, true, false, 2, false);
   check_case(&gfx10_comp, false, false);

   /* 5. RADV_DEBUG=nomeshshader disables both mesh and task on Darwin and non-Darwin. */
   inst.debug_flags = RADV_DEBUG_NO_MESH_SHADER;
   check_case(&darwin_gfx12, false, false);
   check_case(&linux_gfx12, false, false);
   inst.debug_flags = 0;

   /* 6. has_taskmesh_indirect0_bug disables both mesh and task on Darwin and non-Darwin. */
   struct radv_physical_device bug_darwin = make_pdev(&inst, GFX12, true, false, 0, true);
   struct radv_physical_device bug_linux = make_pdev(&inst, GFX12, true, false, 4, true);
   check_case(&bug_darwin, false, false);
   check_case(&bug_linux, false, false);

   /* 7. Non-NGG or LLVM backend disables both. */
   struct radv_physical_device no_ngg = make_pdev(&inst, GFX12, false, false, 4, false);
   struct radv_physical_device llvm_on = make_pdev(&inst, GFX12, true, true, 4, false);
   check_case(&no_ngg, false, false);
   check_case(&llvm_on, false, false);

   /* 8. RADV_QUEUE_DISABLE_COMPUTE disables taskShader while keeping meshShader enabled. */
   inst.queue_disable_flags = RADV_QUEUE_DISABLE_COMPUTE;
   check_case(&linux_gfx12, true, false);
   inst.queue_disable_flags = 0;
}

static void
test_exhaustive_truth_table(void)
{
   for (unsigned mask = 0; mask < (1u << 6); mask++) {
      const bool use_ngg = (mask & (1u << 0)) != 0;
      const bool use_llvm = (mask & (1u << 1)) != 0;
      const enum amd_gfx_level gfx = (mask & (1u << 2)) ? GFX12 : GFX10;
      const uint32_t compute_q = (mask & (1u << 3)) ? 1u : 0u;
      const bool indirect0_bug = (mask & (1u << 4)) != 0;
      const bool debug_nomesh = (mask & (1u << 5)) != 0;

      struct radv_instance inst = {
         .debug_flags = debug_nomesh ? RADV_DEBUG_NO_MESH_SHADER : 0,
         .queue_disable_flags = 0,
      };
      struct radv_physical_device pdev =
         make_pdev(&inst, gfx, use_ngg, use_llvm, compute_q, indirect0_bug);

      const bool exp_mesh =
         !debug_nomesh && use_ngg && !use_llvm && gfx >= GFX10_3 && !indirect0_bug;
      const bool exp_task = exp_mesh && compute_q > 0;
      check_case(&pdev, exp_mesh, exp_task);

      /* Invariants: task implies mesh; compute queue present preserves mesh == task parity. */
      CHECK(!radv_taskmesh_enabled(&pdev) || radv_mesh_enabled(&pdev));
      if (compute_q > 0)
         CHECK(radv_mesh_enabled(&pdev) == radv_taskmesh_enabled(&pdev));
   }
}

int
main(void)
{
   test_named_profiles();
   test_exhaustive_truth_table();
   printf("radv_darwin_mesh_host_test: %u checks, %u failures\n", g_checks, g_failures);
   return g_failures != 0;
}
"""


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    stdin: Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    with stdin.open("rb") if stdin else nullcontext(None) as input_file:
        result = subprocess.run(
            command,
            cwd=cwd,
            stdin=input_file,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
        )
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.returncode:
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(command)}")
    return result


def patch_files(patch_dir: Path, last_patch: str) -> list[Path]:
    if not re.fullmatch(r"\d{4}", last_patch):
        raise SystemExit(f"invalid final patch number: {last_patch}")
    patches: list[tuple[str, Path]] = []
    for path in patch_dir.iterdir():
        match = PATCH_RE.match(path.name)
        if match and match.group(1) <= last_patch:
            patches.append((match.group(1), path))
    patches.sort()
    actual = [number for number, _ in patches]
    expected = [f"{number:04d}" for number in range(1, int(last_patch) + 1)]
    if actual != expected:
        raise SystemExit(
            f"patch series is not contiguous through {last_patch}: "
            f"expected {expected[0]}..{expected[-1]}, got {actual}"
        )
    return [path for _, path in patches]


def validate_patch_hunks(patch_path: Path, expected_files: set[str]) -> int:
    lines = patch_path.read_text(encoding="utf-8").splitlines()
    i = 0
    hunks = 0
    touched: set[str] = set()
    cur_file = "<none>"
    while i < len(lines):
        line = lines[i]
        if line.startswith("+++ "):
            cur_file = line[4:].split("\t")[0].strip()
            touched.add(cur_file[2:] if cur_file.startswith("b/") else cur_file)
        elif line.startswith("@@ "):
            m = re.match(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", line)
            if not m:
                raise SystemExit(f"{patch_path.name}:{i+1}: malformed hunk header: {line}")
            o_cnt = int(m.group(2)) if m.group(2) is not None else 1
            n_cnt = int(m.group(4)) if m.group(4) is not None else 1
            j = i + 1
            ao = an = 0
            while j < len(lines) and not lines[j].startswith(
                ("diff --git ", "--- ", "+++ ", "@@ ")
            ):
                if lines[j].startswith(" "):
                    ao += 1
                    an += 1
                elif lines[j].startswith("-"):
                    ao += 1
                elif lines[j].startswith("+"):
                    an += 1
                elif lines[j].startswith("\\"):
                    pass
                else:
                    raise SystemExit(
                        f"{patch_path.name}:{j+1}: invalid hunk line in {cur_file}: {lines[j]!r}"
                    )
                j += 1
            if ao != o_cnt or an != n_cnt:
                raise SystemExit(
                    f"{patch_path.name}:{i+1}: hunk count mismatch in {cur_file}: "
                    f"header -{o_cnt}/+{n_cnt} vs actual -{ao}/+{an}"
                )
            hunks += 1
            i = j
            continue
        i += 1
    if hunks == 0:
        raise SystemExit(f"{patch_path.name}: contains zero hunks")
    if touched != expected_files:
        raise SystemExit(
            f"{patch_path.name}: touched files mismatch: expected {sorted(expected_files)}, got {sorted(touched)}"
        )
    return hunks


def build_post_0028_anchor(source: Path) -> Path:
    lines: list[str] = []
    for start_line, block in POST_0028_BLOCKS:
        if len(lines) >= start_line:
            raise SystemExit(f"overlapping anchor block at line {start_line}")
        while len(lines) < start_line - 1:
            lines.append(f"/* pad line {len(lines) + 1} */")
        assert len(lines) == start_line - 1
        lines.extend(block)
    target = source / TARGET_REL
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return target


def replay_on_archive(
    archive: Path,
    patches: list[Path],
    dest: Path,
    expected_sha256: str | None,
) -> Path:
    archive_hash = digest(archive)
    if expected_sha256 and archive_hash != expected_sha256:
        raise SystemExit(
            f"archive hash mismatch: expected {expected_sha256}, got {archive_hash}"
        )
    dest.mkdir(parents=True)
    with tarfile.open(archive, "r:*") as tar:
        tar.extractall(dest, filter="data")
    roots = [p for p in dest.iterdir() if p.name != "__MACOSX"]
    if len(roots) != 1 or not roots[0].is_dir():
        raise SystemExit(f"archive must contain one top-level directory: {roots}")
    source = roots[0]
    for patch in patches:
        run(
            [
                "patch",
                "-d",
                str(source),
                "-p1",
                "--fuzz=0",
                "--force",
                "--batch",
                "--no-backup-if-mismatch",
            ],
            stdin=patch,
        )
    rejects = list(source.rglob("*.rej")) + list(source.rglob("*.orig"))
    if rejects:
        raise SystemExit(f"patch series left rejects/originals: {rejects}")
    return source


def replay_standalone(patch_0029: Path, source: Path) -> None:
    source.mkdir(parents=True)
    before_bytes = build_post_0028_anchor(source).read_bytes()

    # 1. Verify zero-fuzz strict application via `git apply --check` + `git apply`
    #    inside an isolated git tree (prevents walking up into an enclosing repo).
    git_tree = source.parent / "git-apply-check"
    git_tree.mkdir(parents=True)
    build_post_0028_anchor(git_tree)
    run(["git", "init", "-q"], cwd=git_tree)
    run(["git", "apply", "--check", str(patch_0029)], cwd=git_tree)
    run(["git", "apply", str(patch_0029)], cwd=git_tree)
    git_after = (git_tree / TARGET_REL).read_bytes()
    if git_after == before_bytes:
        raise SystemExit("git apply silently skipped target file")

    # 2. Apply via `patch -p1 --fuzz=0` (matching README.txt / build-radv.sh)
    #    and verify byte-for-byte parity with `git apply`.
    proc = run(
        [
            "patch",
            "-d",
            str(source),
            "-p1",
            "--fuzz=0",
            "--force",
            "--batch",
            "--no-backup-if-mismatch",
        ],
        stdin=patch_0029,
    )
    if "offset" in proc.stdout.lower() or "fuzz" in proc.stdout.lower():
        raise SystemExit(f"patch applied with nonzero offset/fuzz:\n{proc.stdout}")
    rejects = list(source.rglob("*.rej")) + list(source.rglob("*.orig"))
    if rejects:
        raise SystemExit(f"0029 left rejects/originals: {rejects}")
    patch_after = (source / TARGET_REL).read_bytes()
    if patch_after != git_after:
        raise SystemExit("patch -p1 --fuzz=0 and git apply produced different outputs")
    print(f"STANDALONE_REPLAY_PASS ({patch_0029.name}, git apply + patch --fuzz=0 exact 0-offset parity, 0 rejects)")


def extract_c_function(text: str, signature_prefix: str) -> str:
    idx = text.find(signature_prefix)
    if idx < 0:
        raise SystemExit(f"{signature_prefix!r} not found in patched {TARGET_REL}")
    brace = text.find("{", idx)
    depth = 0
    end = -1
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                end = pos + 1
                break
    if end < 0:
        raise SystemExit(f"unbalanced braces in {signature_prefix!r}")
    return text[idx:end]


def extract_regex(text: str, pattern: str, label: str) -> str:
    m = re.search(pattern, text, flags=re.M | re.S)
    if not m:
        raise SystemExit(f"failed to extract {label} from patched {TARGET_REL}")
    return m.group(0)


def extract_patched_fragments(pdev_c: Path, harness_dir: Path) -> None:
    txt = pdev_c.read_text(encoding="utf-8")

    mesh_fn = extract_c_function(txt, "static bool\nradv_mesh_enabled(")
    taskmesh_fn = extract_c_function(txt, "static bool\nradv_taskmesh_enabled(")
    (harness_dir / "radv_mesh_helpers.inc").write_text(
        mesh_fn + "\n\n" + taskmesh_fn + "\n", encoding="utf-8"
    )

    ext_line = extract_regex(txt, r"^\s*\.EXT_mesh_shader\s*=\s*[^,\n]+,", ".EXT_mesh_shader")
    (harness_dir / "radv_mesh_ext.inc").write_text(ext_line + "\n", encoding="utf-8")

    feat_vars = extract_regex(
        txt,
        r"^\s*bool mesh_en\s*=\s*[^;\n]+;\n\s*bool taskmesh_en\s*=\s*[^;\n]+;",
        "mesh_en/taskmesh_en feature locals",
    )
    (harness_dir / "radv_mesh_feat_vars.inc").write_text(feat_vars + "\n", encoding="utf-8")

    feat_fields = extract_regex(
        txt,
        r"/\* VK_EXT_mesh_shader \*/\n"
        r"\s*\.meshShader\s*=\s*[^,\n]+,\n"
        r"\s*\.taskShader\s*=\s*[^,\n]+,\n"
        r"\s*\.multiviewMeshShader\s*=\s*[^,\n]+,\n"
        r"\s*\.primitiveFragmentShadingRateMeshShader\s*=\s*[^,\n]+,\n"
        r"\s*\.meshShaderQueries\s*=\s*[^,\n]+,",
        "VK_EXT_mesh_shader feature fields",
    )
    (harness_dir / "radv_mesh_feat_fields.inc").write_text(feat_fields + "\n", encoding="utf-8")

    stages_decl = extract_regex(
        txt,
        r"^\s*VkShaderStageFlags taskmesh_stages\s*=\s*[^;]+;",
        "taskmesh_stages",
    )
    (harness_dir / "radv_mesh_stages.inc").write_text(stages_decl + "\n", encoding="utf-8")

    deriv_line = extract_regex(
        txt,
        r"^\s*\.meshAndTaskShaderDerivatives\s*=\s*[^,\n]+,",
        ".meshAndTaskShaderDerivatives",
    )
    if "radv_taskmesh_enabled(pdev)" not in deriv_line:
        raise SystemExit(
            f".meshAndTaskShaderDerivatives must remain gated on radv_taskmesh_enabled(pdev): {deriv_line}"
        )
    (harness_dir / "radv_mesh_deriv.inc").write_text(deriv_line + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--patches", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--last", default="0029")
    parser.add_argument("--mesa-archive", type=Path)
    parser.add_argument("--expected-archive-sha256")
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument(
        "--builds",
        default="opt,asan-ubsan",
        help="Comma-separated build profiles from {opt,asan-ubsan}",
    )
    args = parser.parse_args()

    out = args.output_dir.resolve()
    patch_dir = args.patches.resolve()

    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"output directory is not empty: {out}")
    out.mkdir(parents=True, exist_ok=True)

    patches = patch_files(patch_dir, args.last)
    p0029 = patches[-1]
    hunks = validate_patch_hunks(p0029, {TARGET_REL})
    print(f"HUNK_INTEGRITY_PASS {p0029.name} ({hunks} hunks, sha256={digest(p0029)})")

    if args.mesa_archive:
        source = replay_on_archive(
            args.mesa_archive.resolve(),
            patches,
            out / "extract",
            args.expected_archive_sha256,
        )
        mode = "full-mesa-archive"
    else:
        source = out / "reconstructed"
        replay_standalone(p0029, source)
        mode = "standalone-post-0028-anchor"

    harness_dir = out / "harness"
    harness_dir.mkdir()
    extract_patched_fragments(source / TARGET_REL, harness_dir)
    harness_c = harness_dir / "mesh_host_test.c"
    harness_c.write_text(HOST_HARNESS_C, encoding="utf-8")

    cc = shlex.split(args.cc)
    san_env = dict(os.environ)
    san_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    san_env["ASAN_OPTIONS"] = (
        "detect_leaks=0:halt_on_error=1"
        if sys.platform == "darwin"
        else "detect_leaks=1:halt_on_error=1"
    )

    all_builds = {
        "opt": (["-O2"], None),
        "asan-ubsan": (
            ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"],
            san_env,
        ),
    }
    selected = [b.strip() for b in args.builds.split(",") if b.strip()]
    if not selected or any(b not in all_builds for b in selected):
        raise SystemExit(f"invalid --builds {args.builds!r}; valid: {sorted(all_builds)}")

    for label in selected:
        opt_flags, run_env = all_builds[label]
        bin_path = str(harness_dir / f"mesh_host_test_{label}")
        run(
            cc
            + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
            + opt_flags
            + ["-I", str(harness_dir), str(harness_c), "-o", bin_path]
        )
        run([bin_path], env=run_env)

    print(f"MESH_SERIES_PASS mode={mode} cc={cc[0]} builds={','.join(selected)} patch={p0029.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
