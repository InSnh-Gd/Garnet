#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/garnet-fragment-output.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${CPPFLAGS:-} "$source_dir/test-fragment-output.c" -o "$work/normalize"

spirv-as --target-env vulkan1.2 -o "$work/direct.spv" - <<'SPIRV'
OpCapability Shader
OpCapability Float16
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color %depth
OpExecutionMode %main OriginUpperLeft
OpExecutionMode %main DepthReplacing
OpName %color "color"
OpDecorate %color Location 2
OpDecorate %depth BuiltIn FragDepth
%void = OpTypeVoid
%entry = OpTypeFunction %void
%half = OpTypeFloat 16
%float = OpTypeFloat 32
%half_output = OpTypePointer Output %half
%float_output = OpTypePointer Output %float
%half_value = OpConstant %half 0.5
%float_value = OpConstant %float 0.5
%color = OpVariable %half_output Output
%depth = OpVariable %float_output Output
%main = OpFunction %void None %entry
%label = OpLabel
OpStore %color %half_value
OpStore %depth %float_value
OpReturn
OpFunctionEnd
SPIRV

"$work/normalize" "$work/direct.spv" "$work/widened.spv" widened
spirv-val --target-env vulkan1.2 "$work/widened.spv"
spirv-dis --no-header "$work/widened.spv" -o "$work/widened.spvasm"
test "$(awk '/OpFConvert .*%float /{converts++} END{print converts+0}' "$work/widened.spvasm")" = 1
"$work/normalize" "$work/widened.spv" "$work/second.spv" unchanged
cmp "$work/widened.spv" "$work/second.spv"

spirv-dis --no-header "$work/direct.spv" -o "$work/direct.spvasm"
awk '{print} /OpStore %color / {print "%readback = OpLoad %half %color"}' "$work/direct.spvasm" |
    spirv-as --target-env vulkan1.2 -o "$work/aliased.spv" -
spirv-val --target-env vulkan1.2 "$work/aliased.spv"
"$work/normalize" "$work/aliased.spv" "$work/aliased-result.spv" unchanged
cmp "$work/aliased.spv" "$work/aliased-result.spv"
echo 'FRAGMENT_OUTPUT_PASS direct store, depth, idempotence, output read'
