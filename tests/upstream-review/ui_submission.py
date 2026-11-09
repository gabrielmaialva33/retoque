import ast
from support import source,function,execute
fixture=ast.parse(source('tests/fg-command-objects/fence_cases.py'))
pre=next(ast.literal_eval(n.value) for n in fixture.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='unit' for t in n.targets))
pre='#pragma GCC diagnostic ignored "-Wsign-compare"\n'+pre
pre=pre.replace('int Signal(ID3D12Fence*,UINT64){return signal;}','int Signal(ID3D12Fence* f,UINT64){assert(f);return signal;}')
pre=pre.replace(' bool WaitForFenceValue(',''' ID3D12Fence* _uiFence=nullptr; HANDLE _uiFenceEvent=(void*)1; UINT64 _uiFenceValue=0;
 Allocator* _uiCommandAllocator[4]{}; ID3D12GraphicsCommandList* _uiCommandList[4]{};
 UINT64 _uiAllocatorFenceValues[4]{};bool _uiCommandListResetted[4]{};
 bool SubmitUICommandList(UINT);bool WaitForUIAllocator(UINT);ID3D12GraphicsCommandList* GetUICommandList(int);
 void Provider(int);
 bool WaitForFenceValue(''')
base=source('OptiScaler/framegen/IFGFeature_Dx12.cpp')
methods=''.join(function(base,x) for x in ('bool IFGFeature_Dx12::WaitForFenceValue(','bool IFGFeature_Dx12::SubmitUICommandList(','bool IFGFeature_Dx12::WaitForUIAllocator(','ID3D12GraphicsCommandList* IFGFeature_Dx12::GetUICommandList('))
post=r'''
int main(){
 IFGFeature_Dx12 fg;Queue queue;ID3D12Fence fence;Allocator allocator;ID3D12GraphicsCommandList list;
 fg._gameCommandQueue=&queue;fg._uiFence=&fence;fg._uiCommandAllocator[0]=&allocator;fg._uiCommandList[0]=&list;
 fg._uiCommandListResetted[0]=true;fg._uiAllocatorFenceValues[0]=1;queue.signal=-1;
 fg.Provider(0);fence.completed=2; // Later queue/slot advanced the shared fence without covering old work.
 assert(fg._uiAllocatorFenceValues[0]==UINT64_MAX);
 assert(fg.GetUICommandList(0)==nullptr && allocator.resets==0);
 fg._uiCommandListResetted[0]=true;fg._uiFence=nullptr;fg.Provider(0); // Missing fence never dereferenced.
}
'''
failed=[]
for folder,cls,method in [('ffx','FSRFG_Dx12','Deactivate'),('ffx','FSRFG_Dx12','Present'),('dlssg','DLSSG_Dx12','Present'),('xefg','XeFG_Dx12','Deactivate'),('xefg','XeFG_Dx12','Present'),('reprojection','Reprojection_Dx12','Present')]:
    s=source(f'OptiScaler/framegen/{folder}/{cls}.cpp')
    part=function(s,('bool ' if method=='Present' else 'void ')+cls+'::'+method+'(')
    if 'if (_uiCommandListResetted[fIndex])' in part: block=function(part,'if (_uiCommandListResetted[fIndex])')
    else:
        assert 'SubmitUICommandList(fIndex);' in part
        block='SubmitUICommandList(fIndex);'
    try:execute('4-ui-'+cls+'-'+method,pre+methods+'void IFGFeature_Dx12::Provider(int fIndex){'+block+'}'+post)
    except SystemExit:failed.append(cls+'-'+method)
if failed:raise SystemExit('FAIL: '+', '.join(failed))
