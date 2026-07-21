#pragma once

#include <string>
#include "imgui.h"

namespace doriax::editor {
    class UIUtils {
    public:
        // Draw a search input field with a magnifying glass icon button
        // Returns true if the input text changed
        // If fixedWidth > 0, uses that width; otherwise uses available content region width
        static bool searchInput(const char* id, std::string hint, char* buffer, size_t bufferSize, bool autoFocus = false, bool* matchCase = nullptr, float fixedWidth = 0.0f);

        // Draw a float slider that turns into a text-input field when double-clicked, so an exact
        // value can be typed. Typed values are not clamped to [minValue, maxValue] (only dragging is),
        // so values beyond the slider range can be entered. Drag to adjust; a plain click does not
        // seek the value (this keeps double-click from nudging it). Returns true while it is changing.
        static bool sliderFloatInput(const char* id, float* value, float minValue, float maxValue, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
    };
}