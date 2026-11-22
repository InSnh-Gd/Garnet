#!/usr/bin/env python3
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

backend = Path(__file__).with_name('rdna4_backend_impl.inc').read_text()
features = re.search(r'VkPhysicalDeviceFeatures ef = \{[^}]+\};', backend)[0]
create_info = re.search(r'VkDeviceCreateInfo dci = \{[^}]+\};', backend)[0]
test = r'''
#include <stdio.h>
#include <vulkan/vulkan.h>

static VkBool32 blend_requested(VkBool32 supported) {
    VkPhysicalDeviceFeatures pf = { .independentBlend = supported };
    struct { int supported; } gatherCaps = {0};
    VkPhysicalDeviceVulkan11Features enabled11 = {0};
    VkDeviceQueueCreateInfo dq = {0};
    const uint32_t devExtCount = 0;
    const char **devExts = NULL;
    FEATURES
    CREATE_INFO
    return dci.pEnabledFeatures->independentBlend;
}

int main(void) {
    if (blend_requested(VK_FALSE)) {
        puts("FAIL unsupported independentBlend requested");
        return 1;
    }
    if (!blend_requested(VK_TRUE)) {
        puts("FAIL independentBlend missing: IconServices pipeline with attachment 0 disabled and attachment 1 additive green-only violates pAttachments-00605");
        return 1;
    }
    puts("PASS IconServices independent attachment state enabled on supported backend only");
    return 0;
}
'''.replace('FEATURES\n', features + '\n').replace('CREATE_INFO\n', create_info + '\n')

with tempfile.TemporaryDirectory() as work:
    exe = Path(work) / 'test-independent-blend'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    *sys.argv[1:], '-x', 'c', '-', '-o', str(exe)],
                   input=test, text=True, check=True)
    sys.exit(subprocess.run([str(exe)], check=False).returncode)
