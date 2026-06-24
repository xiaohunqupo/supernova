#pragma once

#include "Project.h"
#include "ai/AiService.h"
#include "window/dialog/AiSettingsWindow.h"
#include "window/widget/SelectableTextView.h"

#include <array>
#include <cstdint>
#include <vector>

namespace doriax::editor {

class ResourcesWindow;

// Clean, Copilot-style chat surface. All configuration lives in AiSettingsWindow;
// this window only renders the conversation and the composer.
class AiChatWindow {
private:
    Project* project;
    ResourcesWindow* resourcesWindow;
    ai::AiService service;
    AiSettingsWindow settingsWindow;
    SelectableTextView transcript;

    bool windowOpen = true;
    bool focusRequested = false;
    bool windowFocused = false;
    bool scrollToBottom = false;
    bool refocusInput = false;
    std::array<char, 8192> inputBuffer{};

    void drawHeader();
    void drawTranscript(float height);
    void drawProposals(const std::vector<ai::ActionProposal>& proposals, float height);
    void drawComposer(float inputHeight);
    void submitMessage();
    void executeProposal(uint64_t proposalId);
    void autoRunProposals();

public:
    static constexpr const char* WINDOW_NAME = "AI Chat";

    AiChatWindow(Project* project, ResourcesWindow* resourcesWindow);

    void show();
    void setOpen(bool open);
    bool isOpen() const;
    bool isFocused() const;
};

} // namespace doriax::editor
