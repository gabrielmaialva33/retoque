// The three swapchain slots the window host reads, as OptiScaler's State has them.
#pragma once
struct IDXGISwapChain;
struct State
{
    IDXGISwapChain* currentSwapchain = nullptr;
    IDXGISwapChain* currentWrappedSwapchain = nullptr;
    IDXGISwapChain* currentFGSwapchain = nullptr;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
