#pragma once

#include "imgui.h"

#include <array>

namespace doriax::editor {

namespace ai { class AiService; }

// Modal dialog that owns all AI assistant configuration (provider, model,
// endpoint, approval mode, output budget and the session API key) so the chat
// window itself can stay clean and Copilot-like.
class AiSettingsWindow {
private:
    bool m_isOpen = false;
    ai::AiService* m_service = nullptr;

    int m_providerIndex = 0;
    int m_approvalIndex = 0;
    int m_maxOutputTokens = 1200;
    std::array<char, 256> m_modelBuffer{};
    std::array<char, 512> m_endpointBuffer{};
    std::array<char, 512> m_apiKeyBuffer{};
    bool m_keySet = false;

    void drawSettings();
    void refreshKeyState();
    void apply();

public:
    AiSettingsWindow() = default;
    ~AiSettingsWindow() = default;

    void open(ai::AiService* service);
    void show();
    bool isOpen() const { return m_isOpen; }
};

} // namespace doriax::editor
