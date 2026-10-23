#pragma once
#include <imgui/imgui.h>
// Host fake: the two blur calls the Vulkan overlay makes.
class MenuCommon
{
  public:
    static void SetBackgroundBlur(ImTextureID, ImVec2 = ImVec2(1.0f, 1.0f)) {}
    static bool BackgroundBlurUsed() { return false; }
};
