#pragma once

// Synthetic input against the real layout helper and Dear ImGui. No OS input or Config access.
namespace Preview
{
inline void VerifyLayoutInput()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(420, 600);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    const char* labels[] = {
        "A long checkbox caption that must wrap across lines and keep its original identity",
        "A long checkbox caption that must wrap across lines and keep its original identity##suffix",
        "A translated long checkbox caption that must wrap across lines and keep its original identity###stable"
    };
    for (const char* label : labels)
    {
        bool value = false;
        ImVec2 box, caption;
        ImGuiID expected = 0;
        auto frame = [&](ImVec2 mouse, bool down, bool disabled)
        {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(300, 500));
            ImGui::Begin("layout-input", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
            const auto start = ImGui::GetCursorScreenPos();
            box = ImVec2(start.x + 5, start.y + 5);
            caption = ImVec2(start.x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + 5, start.y + 5);
            expected = ImGui::GetID(label);
            ImGui::BeginDisabled(disabled);
            {
                DlssNr::MenuLayout::WrapText wrap;
                DlssNr::MenuLayout::Checkbox(label, &value);
            }
            ImGui::EndDisabled();
            ImGui::End();
            ImGui::Render();
            for (auto* texture : *ImGui::GetDrawData()->Textures)
                if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
                {
                    texture->SetTexID(1);
                    texture->SetStatus(ImTextureStatus_OK);
                }
        };
        frame(ImVec2(-100, -100), false, false);
        frame(box, false, false);
        frame(box, true, false);
        if (GImGui->ActiveId != expected)
            throw std::runtime_error("Wrapped checkbox changed its active ImGui ID");
        frame(box, false, false);
        if (!value)
            throw std::runtime_error("Wrapped checkbox did not toggle on box release");
        frame(caption, true, false);
        frame(caption, false, false);
        if (value)
            throw std::runtime_error("Wrapped checkbox caption did not toggle exactly once");
        frame(caption, true, true);
        frame(caption, false, true);
        frame(box, true, true);
        frame(box, false, true);
        if (value)
            throw std::runtime_error("Disabled wrapped checkbox accepted a click");
    }
    ImGui::DestroyContext();
}
} // namespace Preview
