#pragma once
// Test-only stand-in for OptiScaler/pch.h on the host: the Vulkan and standard headers DlssNr_MotionProbe_Vk.cpp needs,
// and nothing of the DLL. The pass logs nothing; its Vulkan calls land in cases.cpp's fakes.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
