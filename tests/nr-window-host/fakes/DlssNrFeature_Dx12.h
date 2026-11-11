// The production present pass and its status, faked: cases.cpp scripts what one present does.
#pragma once
struct IDXGISwapChain3;
struct ID3D12CommandQueue;
namespace DlssNr
{
void RunPresentPass(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool fgHook, bool boundary = false);
unsigned long long PassesRecorded();
bool IsRunning();
const char* FailureReason();
const char* HookStatus();
} // namespace DlssNr
