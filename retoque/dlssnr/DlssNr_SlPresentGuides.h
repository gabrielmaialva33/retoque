#pragma once
#include <cstdint>
namespace sl
{
struct Constants;
struct ResourceTag;
} // namespace sl
struct ID3D12GraphicsCommandList;
namespace DlssNr::SlPresent
{
bool Enabled();
void Constants(const sl::Constants& values, uint32_t token, uint32_t viewport);
void Tags(uint32_t token, uint32_t viewport, const sl::ResourceTag* tags, uint32_t count, void* commandList,
          bool hasToken, bool d3d12);
void MarkerStart(uint32_t token);
void MarkerEnd();
// Only known, open DIRECT recordings outside render passes. First contact installs hooks and refuses.
bool CopyRecording(ID3D12GraphicsCommandList* list);
} // namespace DlssNr::SlPresent
