#pragma once

struct ID3D11DeviceContext;
struct NVSDK_NGX_Parameter;
namespace DlssNr
{
// Called only after a successful native D3D11 DLSS evaluate, before wrapper/game effects.
void EvaluateNativeDx11Scene(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params);
const char* NativeDx11SceneStatus();
void ShutdownNativeDx11Scene();
} // namespace DlssNr
