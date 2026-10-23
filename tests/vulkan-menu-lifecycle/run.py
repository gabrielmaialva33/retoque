#!/usr/bin/env python3
"""Production menu_overlay_vk.cpp against fake Vulkan/ImGui/Win32: the menu's swapchain lifecycle contract."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FAKES = HERE / 'fake_include'

overlay = (ROOT / 'OptiScaler/menu/menu_overlay_vk.cpp').read_text()
hooks = (ROOT / 'OptiScaler/hooks/Vulkan_Hooks.cpp').read_text()

# The surface hook's menu block, verbatim from hkvkCreateWin32SurfaceKHR: read-then-act on the
# menu's window, the act re-checking under the overlay's locks. Sliced from its opening `if` to the
# brace that closes it.
anchor = '        if (!MenuOverlayVk::OnOtherWindow(pCreateInfo->hwnd) &&\n'
if hooks.count(anchor) != 1:
    sys.exit('FAIL: the surface hook no longer filters with MenuOverlayVk::OnOtherWindow(pCreateInfo->hwnd) once')
start = hooks.index(anchor)
end = hooks.index('\n        }\n', start) + len('\n        }\n')
surface = hooks[start:end]
if 'MenuOverlayVk::DestroyVulkanObjects(false, pCreateInfo->hwnd))' not in surface or \
        '_hwnd = pCreateInfo->hwnd;' not in surface:
    sys.exit('FAIL: the surface hook block changed shape: ' + surface)

# The swapchain hook reaches the overlay only after a successful driver create. cases.cpp's
# GameCreateSwapchain models exactly that, so hold the production hook to it.
create = hooks[hooks.index('VALIDATE_HOOK(hkvkCreateSwapchainKHR,'):]
create = create[:create.index('\nVALIDATE_HOOK(', 1)]
call = create.index('MenuOverlayVk::CreateSwapchain(')
gate = create.rindex('if (result == VK_SUCCESS && device != VK_NULL_HANDLE && pCreateInfo != nullptr && *pSwapchain != '
                     'VK_NULL_HANDLE', 0, call)
body = create[create.index('{', gate) + 1:call]
depth = 0
for ch in body:
    depth += {'{': 1, '}': -1}.get(ch, 0)
    if depth < 0:
        break
if depth < 0 or create.count('MenuOverlayVk::CreateSwapchain(') != 1:
    sys.exit('FAIL: MenuOverlayVk::CreateSwapchain is no longer called once, inside the driver-success block')
print('PASS: source shape: the overlay sees only successful creates; the surface hook block is sliced verbatim', flush=True)

unit = overlay + '\n' + (HERE / 'fakes.cpp').read_text() + '\n'
unit += ('// ---- sliced from Vulkan_Hooks.cpp hkvkCreateWin32SurfaceKHR ----\n'
         'static HWND _hwnd = nullptr;\nstruct FakeSurfaceInfo { HWND hwnd; };\n'
         'static void SurfaceHookTail(const FakeSurfaceInfo* pCreateInfo)\n{\n' + surface + '}\n\n')
unit += (HERE / 'cases.cpp').read_text()

cxx = os.environ.get('CXX', 'g++')
with tempfile.TemporaryDirectory(prefix='retoque-vulkan-menu-lifecycle-') as tmp:
    cpp, exe = Path(tmp) / 'unit.cpp', Path(tmp) / 'unit'
    cpp.write_text(unit)
    subprocess.run([cxx, '-std=c++20', '-pthread', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
                    '-I', str(FAKES), '-I', str(ROOT / 'OptiScaler/menu'), '-I', str(ROOT / 'OptiScaler/include'),
                    '-I', str(ROOT / 'external/vulkan/include'), str(cpp), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], timeout=120)
    sys.exit(result.returncode)
