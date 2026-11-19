#pragma once
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <algorithm>
#include <string>

namespace DlssNr::MenuLayout
{
struct WrapText
{
    WrapText() { ImGui::PushTextWrapPos(0.0f); }
    ~WrapText() { ImGui::PopTextWrapPos(); }
};

// Keep the native checkbox and keyboard navigation. A wrapped caption also toggles the same item.
inline bool Checkbox(const char* label, bool* value)
{
    const float captionWidth =
        ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x;
    if (ImGui::CalcTextSize(label, nullptr, true).x <= captionWidth)
        return ImGui::Checkbox(label, value);
    const ImGuiID id = ImGui::GetID(label);
    ImGui::BeginGroup();
    ImGui::PushOverrideID(id);
    bool changed = ImGui::Checkbox("", value);
    ImGui::PopID();
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImGui::TextWrapped("%.*s", static_cast<int>(end - label), label);
    if (ImGui::IsItemClicked() && !(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled))
    {
        *value = !*value;
        ImGui::MarkItemEdited(id);
        changed = true;
    }
    ImGui::EndGroup();
    return changed;
}

// A temporary lives through the widget call, including its interaction queries. Empty-label hashing
// with the original ID as seed preserves bare, ## and ### identities in both translated and English UI.
// Do not use for BeginCombo: a successful call changes the current window before the temporary dies.
class FieldLabel
{
    const char* label_;
    bool stacked_;

  public:
    explicit FieldLabel(const char* label) : label_(label)
    {
        const float available = ImGui::GetContentRegionAvail().x;
        stacked_ = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::CalcItemWidth() +
                       ImGui::GetStyle().ItemInnerSpacing.x + 120.0f >
                   available;
        if (stacked_)
        {
            const ImGuiID id = ImGui::GetID(label);
            const char* end = ImGui::FindRenderedTextEnd(label);
            ImGui::TextWrapped("%.*s", static_cast<int>(end - label), label);
            ImGui::PushOverrideID(id);
            ImGui::PushItemWidth(std::max(40.0f, std::min(ImGui::CalcItemWidth(), available - 85.0f)));
        }
    }
    ~FieldLabel()
    {
        if (stacked_)
        {
            ImGui::PopItemWidth();
            ImGui::PopID();
        }
    }
    operator const char*() const { return stacked_ ? "" : label_; }
    FieldLabel(const FieldLabel&) = delete;
    FieldLabel& operator=(const FieldLabel&) = delete;
};
} // namespace DlssNr::MenuLayout
