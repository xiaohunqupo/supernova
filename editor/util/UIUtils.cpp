#include "UIUtils.h"
#include "App.h"
#include "external/IconsFontAwesome6.h"
#include "window/widget/InputTextContextMenu.h"
#include "imgui_internal.h"

namespace doriax::editor {

bool UIUtils::searchInput(const char* id, std::string hint, char* buffer, size_t bufferSize, bool autoFocus, bool* matchCase, float fixedWidth) {
    ImGui::BeginGroup();

    ImGuiStyle& style = ImGui::GetStyle();

    // Calculate icon button width
    ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_MAGNIFYING_GLASS);
    float buttonWidth = iconSize.x + style.FramePadding.x * 2.0f;

    // Calculate input field width
    float totalWidth = (fixedWidth > 0.0f) ? fixedWidth : ImGui::GetContentRegionAvail().x;
    float inputWidth = totalWidth - buttonWidth;
    if (inputWidth < 50.0f) inputWidth = 50.0f; // Minimum width

    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));
    ImGui::PushItemWidth(inputWidth);

    if (autoFocus) {
        ImGui::SetKeyboardFocusHere();
    }

    bool changed = false;
    if (hint.empty()) {
        changed = ImGui::InputText(id, buffer, bufferSize);
    } else {
        changed = ImGui::InputTextWithHint(id, hint.c_str(), buffer, bufferSize);
    }
    ImGuiID inputId = ImGui::GetItemID();
    ImRect inputRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::PopItemWidth();
    ImGui::PopStyleColor();

    changed = InputTextContextMenu::drawForLastItem(buffer, bufferSize) || changed;

    // Button on the same line with no spacing
    ImGui::SameLine(0.0f, 0.0f);

    // Style the button to match the input field
    ImVec4 bgColor = style.Colors[ImGuiCol_FrameBg];
    ImVec4 bgHoveredColor = style.Colors[ImGuiCol_FrameBgHovered];
    ImVec4 bgActiveColor = style.Colors[ImGuiCol_FrameBgActive];

    // Determine icon text color based on match case
    bool isMatchCase = (matchCase != nullptr) && (*matchCase);
    ImVec4 iconTextColor = isMatchCase ? style.Colors[ImGuiCol_Text] : style.Colors[ImGuiCol_TextDisabled];

    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, bgColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgHoveredColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bgActiveColor);
    ImGui::PushStyleColor(ImGuiCol_Text, iconTextColor);

    // Create unique popup ID based on the input ID
    std::string popupId = std::string("SearchOptions") + id;
    std::string buttonId = std::string(ICON_FA_MAGNIFYING_GLASS) + "##SearchButton" + id;

    if (ImGui::Button(buttonId.c_str(), ImVec2(buttonWidth, 0.0f))) {
        ImGui::OpenPopup(popupId.c_str());
    }
    ImGuiID iconButtonId = ImGui::GetItemID();
    ImRect iconButtonRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::PopStyleColor(4);
    ImGui::PopStyleColor();

    ImRect searchRect = inputRect;
    searchRect.Add(iconButtonRect);
    ImGui::RenderNavCursor(searchRect, inputId);
    ImGui::RenderNavCursor(searchRect, iconButtonId);

    // Context menu for search options
    if (ImGui::BeginPopup(popupId.c_str())) {
        if (matchCase) {
            ImGui::Checkbox(ICON_FA_SPELL_CHECK "  Match Case", matchCase);
        } else {
            // Show disabled checkbox if matchCase is not provided
            bool dummyMatchCase = false;
            ImGui::BeginDisabled();
            ImGui::Text("No options available");
            ImGui::EndDisabled();
        }

        ImGui::EndPopup();
    }

    ImGui::EndGroup();

    return changed;
}

}
