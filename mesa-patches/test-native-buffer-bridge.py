#!/usr/bin/env python3
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('mesa', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
mesa = args.mesa.resolve()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parent
build = out / 'mesa-build'


def run(cmd, cwd=None):
    print('+', shlex.join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), cwd=cwd, check=True)


if not (build / 'build.ninja').exists():
    run(['meson', 'setup', build, mesa, '-Dvulkan-drivers=amd', '-Dgallium-drivers=',
         '-Dplatforms=', '-Dllvm=disabled', '-Damd-use-llvm=false', '-Dglx=disabled',
         '-Degl=disabled', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled',
         '-Dvideo-codecs=', '-Dbuild-tests=false'])
headers = ['src/amd/common/amdgfxregs.h', 'src/amd/common/sid_tables.h',
           'src/amd/compiler/aco_opcodes.h', 'src/amd/vulkan/radv_entrypoints.h',
           'src/amd/vulkan/radv_drirc.h', 'src/amd/vulkan/radv_tracepoints.h',
           'src/compiler/builtin_types.h', 'src/compiler/nir/nir_opcodes.h',
           'src/compiler/nir/nir_intrinsics.h', 'src/compiler/nir/nir_intrinsics_indices.h',
           'src/compiler/nir/nir_builder_opcodes.h', 'src/compiler/spirv/spirv_info.h',
           'src/util/format/u_format_gen.h', 'src/util/shader_stats.h',
           'src/vulkan/util/vk_dispatch_table.h', 'src/vulkan/util/vk_enum_defines.h',
           'src/vulkan/util/vk_enum_to_str.h', 'src/vulkan/util/vk_extensions.h',
           'src/vulkan/util/vk_struct_type_cast.h',
           'src/vulkan/runtime/vk_physical_device_features.h',
           'src/vulkan/runtime/vk_physical_device_properties.h',
           'src/vulkan/runtime/vk_cmd_queue.h', 'src/vulkan/runtime/vk_format_info.h',
           'src/vulkan/runtime/vk_common_entrypoints.h',
           'src/vulkan/runtime/vk_cmd_enqueue_entrypoints.h',
           'src/vulkan/runtime/vk_dispatch_trampolines.h',
           'src/vulkan/wsi/wsi_common_entrypoints.h']
run(['ninja', '-C', build, *headers])
for gfx in ('gfx11', 'gfx12'):
    cmd = ['python3', 'parse_cp_pm4_table_data_json.py', 'cp_pm4_table_data_gfx11.json',
           'pm4_it_opcodes_gfx11.h', 'cp_pm4_table_data_gfx12.json',
           'pm4_it_opcodes_gfx12.h', gfx, 'packets_h']
    print('+ cwd', mesa / 'src/amd/packets', shlex.join(cmd), flush=True)
    with (build / f'src/amd/common/amd_cp_packets_{gfx}.h').open('w') as stream:
        subprocess.run(cmd, cwd=mesa / 'src/amd/packets', stdout=stream, check=True)
commands = json.loads(subprocess.check_output(['ninja', '-C', str(build), '-t', 'compdb', 'c_COMPILER']))
(out / 'compile_commands.json').write_text(json.dumps(commands, indent=2) + '\n')
entry = next(c for c in commands if c['file'].endswith('/radv_amdgpu_bo.c'))
flags = shlex.split(entry['command'])
compiler = flags.pop(0)
for option in ('-o', '-MQ', '-MF'):
    i = flags.index(option)
    del flags[i:i+2]
flags.remove('-MD')
flags.remove('-c')
flags.pop()
flags += ['-idirafter', str(mesa / 'include/darwin'), '-g', '-O1', '-UNDEBUG',
          '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie',
          '-ffunction-sections', '-fdata-sections', '-fdiagnostics-color=never']
objects = []
for source, name in ((mesa / 'src/amd/vulkan/winsys/amdgpu/radv_darwin_scanout.c', 'radv-bridge'),
                     (repo / 'gfx1200_radv_buffer_test.c', 'radv-test'),
                     (mesa / 'src/amd/common/darwin/ac_darwin_kext.c', 'kext')):
    obj = out / f'{name}.o'
    run([compiler, *flags, '-c', source, '-o', obj], build)
    objects.append(obj)
run([compiler, '-no-pie', '-fsanitize=address,undefined', '-Wl,--gc-sections', *objects, '-lm',
     '-pthread', '-o', out / 'radv-buffer-test'])
run(['clang', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
     '-I' + str(mesa / 'include'), '-I' + str(mesa / 'include/darwin'),
     '-I' + str(mesa / 'src/amd/common/darwin'), repo / 'gfx1200_buffer_bridge_test.c',
     mesa / 'src/amd/common/darwin/ac_darwin_kext.c', '-lm', '-o', out / 'buffer-test'])
os.environ['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
os.environ['UBSAN_OPTIONS'] = 'halt_on_error=1'
run([out / 'buffer-test'])
run([out / 'radv-buffer-test'])
