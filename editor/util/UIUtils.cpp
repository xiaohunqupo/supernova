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

bool UIUtils::sliderFloatInput(const char* id, float* value, float minValue, float maxValue, const char* format, ImGuiSliderFlags flags) {
    // Only one widget can be text-edited at a time (it holds keyboard focus), so shared statics are
    // enough to remember which slider was double-clicked into an input field and its pre-click value.
    static ImGuiID editingId = 0;
    static bool focusPending = false;
    static float clickValue = 0.0f;
    static int editFrame = -1; // ImGui frame count when the edit target was last drawn

    const ImGuiID widgetId = ImGui::GetID(id);
    const int frame = ImGui::GetFrameCount();

    // If the edit target was not drawn on the previous frame, its input field was torn down without a
    // deactivation event (row collapsed, panel switched, or hidden while focus was still pending).
    // Cancel the edit and fall back to the slider; clearing focusPending too avoids stealing focus
    // when the row reappears.
    if (editingId == widgetId && frame - editFrame > 1) {
        editingId = 0;
        focusPending = false;
    }

    bool changed = false;

    if (editingId == widgetId) {
        editFrame = frame;
        // Double-clicked: type the exact value. InputFloat auto-selects its text on activation and
        // does not clamp, so values outside [minValue, maxValue] can be entered.
        if (focusPending) {
            ImGui::SetKeyboardFocusHere();
            focusPending = false;
        }
        // Strip any decorations (icons/labels) from the slider's display format so the text field
        // shows just the number, matching how ImGui's own Ctrl+Click temp-input behaves.
        char inputFormat[32];
        changed = ImGui::InputFloat(id, value, 0.0f, 0.0f, ImParseFormatTrimDecorations(format, inputFormat, IM_ARRAYSIZE(inputFormat)));
        if (ImGui::IsItemDeactivated()) {
            editingId = 0;
        }
    } else {
        const float before = *value;
        changed = ImGui::SliderFloat(id, value, minValue, maxValue, format, flags);

        // Remember the value from before the interaction started (sliders seek to the cursor on click).
        if (ImGui::IsItemActivated()) {
            clickValue = before;
        }

        // Suppress the slider's click-to-seek: a plain click (including the first click of a
        // double-click) must not move the value. Only dragging past the threshold adjusts it, so
        // double-clicking to open the text field never changes the value nor pushes an undo command.
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            *value = clickValue;
            changed = false;
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            editingId = widgetId;
            focusPending = true;
            editFrame = frame; // mark handled this frame so the teardown guard waits for a real gap
        }
    }

    return changed;
}

}
