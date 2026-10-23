#pragma once
// Test-only stand-in for OptiScaler/pch.h: Windows, Vulkan and the standard headers the guide resample and the harness
// need, and nothing of the DLL (spdlog, Config, State, hooks). DlssNr_GuideMatch_Vk.cpp logs nothing.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
