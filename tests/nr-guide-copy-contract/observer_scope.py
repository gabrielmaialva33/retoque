"""Production observer scope with only its platform activation gate replaced by off."""


def off_scope(root, function):
    header = (root / 'retoque/dlssnr/DlssNr_PresentDepthDx12.h').read_text()
    source = (root / 'retoque/dlssnr/DlssNr_PresentDepthDx12.cpp').read_text()
    return ('namespace DlssNr::PresentDepthDx12 {\n'
            'bool Active() { return false; } thread_local unsigned internal=0;\n'
            + function(header, 'class InternalScope\n') + ';\n'
            + function(source, 'InternalScope::InternalScope()')
            + function(source, 'InternalScope::~InternalScope()') + '}\n')
