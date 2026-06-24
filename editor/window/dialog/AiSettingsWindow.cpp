#include "AiSettingsWindow.h"

#include "AppSettings.h"
#include "ai/AiService.h"
#include "ai/AiTypes.h"
#include "ai/SecretStore.h"
#include "external/IconsFontAwesome6.h"
#include "window/Widgets.h"

#include <algorithm>
#include <cstdio>

namespace doriax::editor {

namespace {

const char* kProviderLabels[] = {"OpenAI", "Anthropic", "Gemini", "OpenAI-compatible"};
const ai::ProviderId kProviderValues[] = {
    ai::ProviderId::OpenAI,
    ai::ProviderId::Anthropic,
    ai::ProviderId::Gemini,
    ai::ProviderId::OpenAICompatible
};
constexpr int kProviderCount = IM_ARRAYSIZE(kProviderValues);

const char* kApprovalLabels[] = {"Preview then approve", "Auto-run read-only", "Full agent"};
const ai::ApprovalMode kApprovalValues[] = {
    ai::ApprovalMode::PreviewThenApprove,
    ai::ApprovalMode::SafeAutoRun,
    ai::ApprovalMode::FullAgent
};
constexpr int kApprovalCount = IM_ARRAYSIZE(kApprovalValues);

const char* kApprovalHelp[] = {
    "Every tool call waits for your approval.",
    "Read-only tools run automatically; changes still need approval.",
    "All tool calls run automatically. Use with care."
};

int providerToIndex(ai::ProviderId provider) {
    for (int i = 0; i < kProviderCount; ++i) {
        if (kProviderValues[i] == provider) return i;
    }
    return 0;
}

int approvalToIndex(ai::ApprovalMode mode) {
    for (int i = 0; i < kApprovalCount; ++i) {
        if (kApprovalValues[i] == mode) return i;
    }
    return 0;
}

} // namespace

void AiSettingsWindow::open(ai::AiService* service) {
    m_isOpen = true;
    m_service = service;

    ai::Settings settings = AppSettings::getAiSettings();
    m_providerIndex = providerToIndex(settings.provider);
    m_approvalIndex = approvalToIndex(settings.approvalMode);
    m_maxOutputTokens = settings.maxOutputTokens;
    std::snprintf(m_modelBuffer.data(), m_modelBuffer.size(), "%s", settings.model.c_str());
    std::snprintf(m_endpointBuffer.data(), m_endpointBuffer.size(), "%s", settings.customEndpoint.c_str());
    m_apiKeyBuffer.fill('\0');
    refreshKeyState();
}

void AiSettingsWindow::refreshKeyState() {
    m_keySet = ai::SecretStore::hasSessionApiKey(kProviderValues[m_providerIndex]);
}

void AiSettingsWindow::apply() {
    ai::Settings settings = AppSettings::getAiSettings();
    settings.provider = kProviderValues[m_providerIndex];
    settings.approvalMode = kApprovalValues[m_approvalIndex];
    settings.model = m_modelBuffer.data();
    settings.customEndpoint = m_endpointBuffer.data();
    settings.maxOutputTokens = std::clamp(m_maxOutputTokens, 256, 16000);
    if (settings.model.empty()) {
        settings.model = ai::defaultModelForProvider(settings.provider);
    }

    AppSettings::setAiSettings(settings);
    if (m_service) {
        m_service->setSettings(settings);
    }

    // The API key is kept in memory for this session only and never persisted.
    if (m_apiKeyBuffer[0] != '\0') {
        ai::SecretStore::setSessionApiKey(settings.provider, m_apiKeyBuffer.data());
        m_apiKeyBuffer.fill('\0');
    }
}

void AiSettingsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("AI Settings##AiSettingsModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(520, 0),
        ImVec2(520, ImGui::GetMainViewport()->WorkSize.y * 0.9f)
    );

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::BeginPopupModal("AI Settings##AiSettingsModal", &m_isOpen, flags)) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            drawSettings();
        }
        ImGui::EndPopup();
    }
}

void AiSettingsWindow::drawSettings() {
    const bool isCompatible = kProviderValues[m_providerIndex] == ai::ProviderId::OpenAICompatible;

    ImGui::BeginTable("ai_settings", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp);
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

    // Provider
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Provider");
    ImGui::TableNextColumn();
    {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##Provider", &m_providerIndex, kProviderLabels, kProviderCount)) {
            // Switch to the provider's default model and reflect its key state.
            std::snprintf(m_modelBuffer.data(), m_modelBuffer.size(), "%s",
                          ai::defaultModelForProvider(kProviderValues[m_providerIndex]).c_str());
            refreshKeyState();
        }
    }

    // Model
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Model");
    ImGui::TableNextColumn();
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##Model", m_modelBuffer.data(), m_modelBuffer.size());
    }

    // Custom endpoint (OpenAI-compatible only)
    if (isCompatible) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Endpoint");
        ImGui::SetItemTooltip("Chat Completions URL. Leave empty to use OpenRouter.");
        ImGui::TableNextColumn();
        {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##Endpoint", "https://openrouter.ai/api/v1/chat/completions",
                                     m_endpointBuffer.data(), m_endpointBuffer.size());
        }
    }

    // API key
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("API Key");
    ImGui::SetItemTooltip("Stored in memory for this session only. Never written to disk.");
    ImGui::TableNextColumn();
    {
        float clearWidth = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##ApiKey", m_keySet ? "session key set - type to replace" : "paste API key",
                                 m_apiKeyBuffer.data(), m_apiKeyBuffer.size(), ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_keySet && m_apiKeyBuffer[0] == '\0');
        if (Widgets::iconButton("##ClearKey", ICON_FA_TRASH, ImVec2(clearWidth, ImGui::GetFrameHeight()))) {
            ai::SecretStore::clearSessionApiKey(kProviderValues[m_providerIndex]);
            m_apiKeyBuffer.fill('\0');
            refreshKeyState();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Clear the session key for this provider");
    }

    // Approval mode
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Approval");
    ImGui::TableNextColumn();
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##Approval", &m_approvalIndex, kApprovalLabels, kApprovalCount);
        ImGui::TextDisabled("%s", kApprovalHelp[m_approvalIndex]);
    }

    // Max output tokens
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Max Output Tokens");
    ImGui::TableNextColumn();
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##MaxOutput", &m_maxOutputTokens, 256, 1024);
        m_maxOutputTokens = std::clamp(m_maxOutputTokens, 256, 16000);
    }

    ImGui::EndTable();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float windowWidth = ImGui::GetWindowSize().x;
    float buttonsWidth = 250;
    ImGui::SetCursorPosX((windowWidth - buttonsWidth) * 0.5f);

    if (ImGui::Button("OK", ImVec2(120, 0))) {
        apply();
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
}

} // namespace doriax::editor
