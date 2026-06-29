#pragma once

#include "Project.h"
#include "ai/AiService.h"
#include "ai/ConversationStore.h"
#include "ai/ModelCatalog.h"
#include "window/dialog/AiSettingsWindow.h"
#include "window/widget/SelectableTextView.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace doriax::editor {

class ResourcesWindow;

// Clean, Copilot-style chat surface. Provider/key details live in
// AiSettingsWindow; quick conversation controls live with the composer.
class AiChatWindow {
private:
    Project* project;
    ResourcesWindow* resourcesWindow;
    ai::AiService service;
    ai::ConversationStore conversationStore;
    ai::ModelCatalog modelCatalog;
    AiSettingsWindow settingsWindow;
    SelectableTextView transcript;

    bool windowOpen = true;
    bool focusRequested = false;
    bool windowFocused = false;
    bool isWindowVisible = false;
    bool hasNotification = false;
    bool syncedInitialSettings = false;
    bool scrollToBottom = false;
    bool refocusInput = false;
    bool customModelPopupRequested = false;
    std::array<char, 8192> inputBuffer{};
    std::array<char, 256> customModelBuffer{};
    std::vector<int> inputSoftWraps;
    std::string inputDisplayText;
    std::string currentConversationId;
    std::string autoLoadedProjectPath;
    size_t lastSavedMessageCount = 0;
    size_t lastObservedMessageCount = 0;
    bool autoLoadedLatestConversation = false;

    void drawHeader();
    void drawHistoryPopup();
    void drawTranscript(float height);
    void drawProposals(const std::vector<ai::ActionProposal>& proposals, float height);
    void drawComposer(float inputHeight);
    void drawComposerControls();
    void drawEditableSettingLabel(const std::string& value, float width,
                                  const char* editId, const char* popupId,
                                  const ImVec4* textColor = nullptr);
    void drawModelPopup();
    void drawCustomModelPopup();
    void drawApprovalPopup();
    void applySettings(const ai::Settings& settings);
    bool canSendMessage(std::string* reason = nullptr) const;
    void submitMessage();
    void executeProposal(uint64_t proposalId);
    void autoRunProposals();
    void updateMessageNotification();
    void loadLatestConversationForCurrentProject();
    void persistConversation();
    void loadConversation(const std::string& id);
    void prefetchModels();

public:
    static constexpr const char* WINDOW_NAME = "AI Chat";

    AiChatWindow(Project* project, ResourcesWindow* resourcesWindow);

    void show();
    void setOpen(bool open);
    bool isOpen() const;
    bool isFocused() const;

    // Clears the in-memory chat. Used by the header New Chat button and when
    // the editor starts a fresh project at the same path (e.g. File > New Project).
    void startNewChat();
};

} // namespace doriax::editor
