"""Production observer scope with only its platform activation gate replaced by off."""


def off_scope(root, function):
    header = (root / 'retoque/dlssnr/DlssNr_PresentDepthDx12.h').read_text()
    source = (root / 'retoque/dlssnr/DlssNr_PresentDepthDx12.cpp').read_text()
    return ('namespace DlssNr::PresentDepthDx12 {\n'
            'bool Active() { return false; } thread_local unsigned internal=0;\n'
            + function(header, 'class InternalScope\n') + ';\n'
            + function(source, 'InternalScope::InternalScope()')
            + function(source, 'InternalScope::~InternalScope()') + '}\n')


def mask_inspection(main):
    """Dispatch's mask-inspection gate as shipped: the production header and the pass's own observer and helpers.

    The fixture's Config leaves [DlssNr] MaskInspection unset, so the real gate runs and stays off
    (tests/nr-mask-inspection covers it on).
    """
    start = main.index('static DlssNr::MaskInspection::Observer g_maskInspection;')
    last = 'static void MaskInspectionSay(const char* line) { LOG_INFO("{}", line); }\n'
    return ('#include <optional>\n#include <string>\n#include "retoque/dlssnr/DlssNr_MaskInspection.h"\n'
            '#include "retoque/dlssnr/DlssNr_PassSettings.h"\n'
            'static long long NowMs() { return 0; }\n'
            + main[start:main.index(last) + len(last)])
