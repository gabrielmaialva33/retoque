"""Compile the actual production scope constructor/destructor and admission against a fake prepare/provider."""
from pathlib import Path
import subprocess
from source_contracts import body
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent

def sources():
    source=(ROOT/'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
    header=(ROOT/'retoque/dlssnr/DlssNrFeature_Vk.h').read_text()
    start=header.index('class ScopedPreUpscaleVk\n')
    end=header.index('\n};',start)+3
    return header[start:end]+'\nstd::atomic_flag g_preBorrow=ATOMIC_FLAG_INIT;\n'+'\n'.join(
        body(source,name) for name in ('PreResource','ScopedPreUpscaleVk::ScopedPreUpscaleVk','ScopedPreUpscaleVk::~ScopedPreUpscaleVk'))

def run(folder, mutants=True):
    production=sources()
    fixture=(HERE/'scope_driver.cpp').read_text()
    changes={
      'aside admitted':('picture.role != Pictures::Role::Main','false && picture.role != Pictures::Role::Main'),
      'layout unchecked':('== (uint32_t) layout','!= 999999u + (uint32_t)layout'),
      'formatless feature unchecked':('!coverage.formatless ||',''),
      'format flags2 unchecked':('!coverage.formatFlags2','false'),
      'subrect admitted':('if (baseX || baseY)','if (false && (baseX || baseY))'),
      'exclusive ownership guessed':('!image->concurrent ||',''),
      'view incarnation unchecked':('!facts->CurrentView(view, identity)','false && !facts->CurrentView(view, identity)'),
      'explicit restore skipped':('void Complete() noexcept { _colour.Restore(); }','void Complete() noexcept {}'),
      'busy recording admitted':('admission.outstanding = PreRecordingBusy();','admission.outstanding = false;'),
      'physical API ignored':('capabilities.apiVersion < VK_API_VERSION_1_3', 'false && capabilities.apiVersion < VK_API_VERSION_1_3'),
      'post fallback':('_outcome = VkPre::Outcome::Rejected;', '_outcome = VkPre::Outcome::Unselected;'),
    }
    variants=[('baseline',production)]+([(name,production.replace(old,new,1)) for name,(old,new) in changes.items()] if mutants else [])
    for name,text in variants:
        if name!='baseline': assert text!=production,name
        case=folder/'scope_driver.cpp'; case.write_text(fixture.replace('// PRODUCTION_SCOPE',text))
        out=folder/'scope_driver'
        command=['g++','-std=c++20','-O0','-Wall','-Wextra','-Wno-address','-Werror',
                 '-I'+str(ROOT/'retoque'),'-I'+str(ROOT/'external/vulkan/include'),
                 '-I'+str(ROOT/'external/nvngx_dlss_sdk'),str(case),'-o',str(out)]
        result=subprocess.run(command,capture_output=True,text=True)
        assert result.returncode==0,name+': '+result.stderr
        result=subprocess.run([str(out)],capture_output=True,text=True)
        if name=='baseline': assert result.returncode==0,result.stderr
        else: assert result.returncode!=0 and 'Assertion' in result.stderr,'survived: '+name
    print(f'PASS: extracted production scope with fake prepare/provider, 23 scenarios; {len(variants)-1} compiling mutants')

if __name__ == '__main__':
    import tempfile
    (HERE/'artifacts').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=HERE/'artifacts', prefix='scope-') as tmp:
        run(Path(tmp))
