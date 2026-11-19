"""Generate a host TU: production bodies are sliced verbatim, never rewritten."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]

def read(path):
    return (ROOT / path).read_text()

def between(text, start, end):
    assert text.count(start) == 1, start
    return text[text.index(start):text.index(end, text.index(start))]

def record(text, name):
    return re.search(r'^(?:struct|enum class) ' + name + r'\b[^\n]*\n\{.*?^\};', text, re.M | re.S).group() + '\n'

def generate(out):
    menu = read('retoque/dlssnr/DlssNr_Menu.cpp')
    common = read('OptiScaler/menu/menu_common.cpp')
    config = read('OptiScaler/Config.h')
    theme = between(common, 'void MenuCommon::ApplyThemeStyle()', 'static double lastTime')
    names = set(re.findall(r'(?:config|conf)->(\w+)|&Config::(\w+)', menu + theme))
    names = {n for pair in names for n in pair if n}
    names.update(['FontSize', 'UseHQFont', 'MenuBGColorA', 'DlssNrGpuTiming', 'DlssNrGpuTimingInterval', 'DlssNrPasses', 'DlssNrIndividualPassSettings'])
    fields = []
    for name in sorted(names):
        m = re.search(r'CustomOptional<[^;]+>\s+' + name + r'\s*(?:\{[^;]*\})?;', config)
        if m:
            fields.append(m.group())
    types = record(config, 'Scaler')
    gpu = read('retoque/dlssnr/DlssNr_GpuTiming.h')
    types += 'namespace DlssNr::GpuTiming {\n' + between(gpu, 'struct Metadata', '// Collection never waits') + '\n}\n'
    feature = read('retoque/dlssnr/DlssNrFeature_Dx12.h')
    types += 'namespace DlssNr {\n'
    types += record(read('retoque/dlssnr/DlssNr_PresentColour.h'), 'PresentColour')
    for name in ['CalibrationReading', 'ExposureStatus', 'PresentHdrStatus', 'PeripheryState', 'DetailMeasureStatus', 'PulseStatus', 'PairCaptureStatus']:
        types += record(feature, name)
    types += '}\nnamespace DlssNr::ReplayRecorder {\n' + record(read('retoque/dlssnr/DlssNr_ReplayRecorderDx12.h'), 'Status') + '}\n'
    scopes = read('OptiScaler/menu/menu_common.h')
    # Copy the two exact RAII classes used by the panel, without Windows menu declarations.
    types += between(scopes, 'class ScopedIndent', 'class ScopedID')
    types += between(scopes, 'class ScopedCollapsingHeader', 'template <typename T> struct MenuOption')
    (out / 'types.generated.h').write_text(types)
    (out / 'config.generated.h').write_text('\n'.join(fields))
    generated = '#include "pch.h"\n#include "facade.h"\n' + theme
    generated += '\n#line 49 "retoque/dlssnr/DlssNr_Menu.cpp"\n' + menu[menu.index('namespace DlssNr\n{'):]
    generated += '\n#include "preview.h"\n'
    (out / 'menu.generated.cpp').write_text(generated)

if __name__ == '__main__':
    generate(Path(sys.argv[1]))
