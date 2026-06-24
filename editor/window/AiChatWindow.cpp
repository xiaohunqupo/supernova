#include "AiChatWindow.h"

#include "App.h"
#include "AppSettings.h"
#include "ai/EditorActionExecutor.h"
#include "ai/SecretStore.h"
#include "external/IconsFontAwesome6.h"
#include "window/Widgets.h"
#include "imgui.h"

#include <algorithm>
#include <string>
#include <vector>

namespace doriax::editor {

namespace {

const ImVec4 kUserColor(0.55f, 0.75f, 1.0f, 1.0f);
const ImVec4 kAssistantColor(0.72f, 0.86f, 0.68f, 1.0f);
const ImVec4 kSuccessColor(0.55f, 0.85f, 0.55f, 1.0f);
const ImVec4 kErrorColor(0.95f, 0.55f, 0.55f, 1.0f);
const ImVec4 kCodeColor(0.82f, 0.85f, 0.72f, 1.0f);

// Light Markdown pass for assistant text: turns "- "/"* "/"+ " bullets into a
// bullet glyph and renders fenced ``` code blocks ``` in a monospace font. Other
// Markdown (bold, headings, inline code) is left as plain text on purpose.
void appendMarkdownParagraphs(const std::string& content, ImU32 textColor, ImU32 codeColor,
                              ImFont* codeFont,
                              std::vector<SelectableTextView::Paragraph>& out) {
    const size_t n = content.size();
    size_t pos = 0;
    bool inCode = false;

    while (pos <= n) {
        size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, (nl == std::string::npos ? n : nl) - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t firstNonSpace = line.find_first_not_of(" \t");
        bool isFence = (firstNonSpace != std::string::npos &&
                        line.compare(firstNonSpace, 3, "```") == 0);

        if (isFence) {
            inCode = !inCode; // swallow the fence line itself
        } else if (inCode) {
            out.push_back({line, codeColor, codeFont});
        } else {
            // Base indent so list items sit in from the message text. Existing
            // leading whitespace (nested lists) is preserved on top of it.
            static const std::string kListIndent = "    ";
            size_t i = (firstNonSpace == std::string::npos) ? line.size() : firstNonSpace;

            bool bullet = (i < line.size()) &&
                          (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
                          (i + 1 < line.size()) && (line[i + 1] == ' ' || line[i + 1] == '\t');

            bool ordered = false;
            if (!bullet && i < line.size() && line[i] >= '0' && line[i] <= '9') {
                size_t j = i;
                while (j < line.size() && line[j] >= '0' && line[j] <= '9') ++j;
                ordered = (j < line.size()) && (line[j] == '.' || line[j] == ')') &&
                          (j + 1 < line.size()) && (line[j + 1] == ' ' || line[j + 1] == '\t');
            }

            if (bullet) {
                size_t rest = i + 1;
                while (rest < line.size() && (line[rest] == ' ' || line[rest] == '\t')) ++rest;
                out.push_back({kListIndent + line.substr(0, i) + "\xE2\x80\xA2 " + line.substr(rest),
                               textColor, nullptr});
            } else if (ordered) {
                out.push_back({kListIndent + line, textColor, nullptr});
            } else {
                out.push_back({line, textColor, nullptr});
            }
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

} // namespace

AiChatWindow::AiChatWindow(Project* project, ResourcesWindow* resourcesWindow)
    : project(project)
    , resourcesWindow(resourcesWindow) {
    service.setSettings(AppSettings::getAiSettings());
}

void AiChatWindow::show() {
    if (!windowOpen) {
        return;
    }

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        windowFocused = false;
        ImGui::End();
        settingsWindow.show();
        return;
    }

    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // Drive the agent loop: auto-run eligible proposals, then let the service
    // send any follow-up request triggered by completed tool results.
    if (!service.isBusy()) {
        autoRunProposals();
        service.update();
    }

    drawHeader();
    ImGui::Separator();

    const ImGuiStyle& style = ImGui::GetStyle();
    float inputHeight = ImGui::GetTextLineHeight() * 3.0f + style.FramePadding.y * 2.0f;
    float composerHeight = inputHeight + style.ItemSpacing.y * 2.0f + 2.0f;

    std::vector<ai::ActionProposal> proposals = service.getProposals();
    int pending = 0;
    for (const auto& proposal : proposals) {
        if (!proposal.executed) ++pending;
    }

    float availY = ImGui::GetContentRegionAvail().y;
    float trayHeight = 0.0f;
    if (pending > 0) {
        float cardHeight = ImGui::GetFrameHeightWithSpacing()
                         + ImGui::GetTextLineHeightWithSpacing() * 2.0f + 14.0f;
        trayHeight = std::min(static_cast<float>(pending), 3.0f) * cardHeight;
        trayHeight = std::min(trayHeight, availY * 0.5f);
    }

    float transcriptHeight = availY - composerHeight - trayHeight
                           - (trayHeight > 0.0f ? style.ItemSpacing.y : 0.0f);
    if (transcriptHeight < 60.0f) {
        transcriptHeight = 60.0f;
    }

    drawTranscript(transcriptHeight);
    if (pending > 0) {
        drawProposals(proposals, trayHeight);
    }
    drawComposer(inputHeight);

    ImGui::End();

    // Rendered at the root level so the modal floats above the chat window.
    settingsWindow.show();
}

void AiChatWindow::setOpen(bool open) {
    if (open && !windowOpen) {
        focusRequested = true;
    }
    windowOpen = open;
}

bool AiChatWindow::isOpen() const {
    return windowOpen;
}

bool AiChatWindow::isFocused() const {
    return windowFocused;
}

void AiChatWindow::drawHeader() {
    ai::Settings settings = service.getSettings();
    const ImGuiStyle& style = ImGui::GetStyle();

    float height = ImGui::GetFrameHeight();
    ImVec2 buttonSize(height, height);
    float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(ICON_FA_ROBOT "  %s", settings.model.c_str());

    ImGui::SameLine();
    ImGui::SetCursorPosX(rightEdge - height * 2.0f - style.ItemSpacing.x);
    ImGui::BeginDisabled(service.isBusy());
    if (Widgets::iconButton("##AiNewChat", ICON_FA_PLUS, buttonSize)) {
        service.clearConversation();
        inputBuffer.fill('\0');
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("New chat");

    ImGui::SameLine();
    if (Widgets::iconButton("##AiSettings", ICON_FA_GEAR, buttonSize)) {
        settingsWindow.open(&service);
    }
    ImGui::SetItemTooltip("AI settings");
}

void AiChatWindow::drawTranscript(float height) {
    std::vector<ai::ChatMessage> messages = service.getMessages();
    bool busy = service.isBusy();

    if (messages.empty() && !busy) {
        ImGui::BeginChild("##AiEmpty", ImVec2(0, height), 0, ImGuiWindowFlags_NoNav);
        ImGui::Spacing();
        ImGui::TextDisabled("Ask about your project, scene, or resources,");
        ImGui::TextDisabled("or request an editor action such as");
        ImGui::TextDisabled("\"add a point light above the player\".");
        if (!ai::SecretStore::hasSessionApiKey(service.getSettings().provider)) {
            ImGui::Spacing();
            if (ImGui::Button(ICON_FA_KEY " Set API key")) {
                settingsWindow.open(&service);
            }
        }
        ImGui::EndChild();
        return;
    }

    const ImU32 userCol = ImGui::GetColorU32(kUserColor);
    const ImU32 assistantCol = ImGui::GetColorU32(kAssistantColor);
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 okCol = ImGui::GetColorU32(kSuccessColor);
    const ImU32 errCol = ImGui::GetColorU32(kErrorColor);
    const ImU32 codeCol = ImGui::GetColorU32(kCodeColor);
    ImFont* codeFont = App::getCodeFont();

    std::vector<SelectableTextView::Paragraph> paragraphs;
    bool first = true;
    auto spacer = [&]() {
        if (!first) paragraphs.push_back({"", textCol});
        first = false;
    };

    for (const auto& message : messages) {
        switch (message.role) {
            case ai::ChatRole::System:
                break;
            case ai::ChatRole::User:
                spacer();
                paragraphs.push_back({"You", userCol});
                paragraphs.push_back({message.content, textCol});
                break;
            case ai::ChatRole::Assistant:
                if (!message.content.empty()) {
                    spacer();
                    paragraphs.push_back({"Assistant", assistantCol});
                    appendMarkdownParagraphs(message.content, textCol, codeCol, codeFont, paragraphs);
                }
                break;
            case ai::ChatRole::Tool: {
                spacer();
                std::string firstLine = message.content.substr(0, message.content.find('\n'));
                std::string header = std::string(message.toolSuccess ? ICON_FA_CIRCLE_CHECK
                                                                      : ICON_FA_CIRCLE_XMARK)
                                   + "  " + message.toolName;
                if (!firstLine.empty()) {
                    header += " - " + firstLine;
                }
                paragraphs.push_back({header, message.toolSuccess ? okCol : errCol});
                break;
            }
        }
    }

    if (busy) {
        spacer();
        paragraphs.push_back({std::string(ICON_FA_SPINNER) + "  Thinking...", dimCol});
    }

    // Tighten line spacing so the blank line separating turns reads as a single
    // gap rather than a double one.
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(style.ItemSpacing.x, std::max(2.0f, style.ItemSpacing.y * 0.4f)));
    transcript.draw("##AiTranscript", ImVec2(0, height), paragraphs, scrollToBottom);
    ImGui::PopStyleVar();
    scrollToBottom = false;
}

void AiChatWindow::drawProposals(const std::vector<ai::ActionProposal>& proposals, float height) {
    ImGui::BeginChild("##AiProposals", ImVec2(0, height), ImGuiChildFlags_Border);

    for (const auto& proposal : proposals) {
        if (proposal.executed) {
            continue;
        }
        ImGui::PushID(static_cast<int>(proposal.id));
        ImGui::TextWrapped(ICON_FA_WRENCH "  %s", proposal.description.c_str());

        if (proposal.executing) {
            ImGui::TextDisabled("Running...");
        } else {
            if (ImGui::Button(ICON_FA_CHECK " Approve")) {
                executeProposal(proposal.id);
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK " Dismiss")) {
                service.removeProposal(proposal.id);
                scrollToBottom = true;
            }
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    ImGui::EndChild();
}

void AiChatWindow::drawComposer(float inputHeight) {
    ImGui::Separator();

    const ImGuiStyle& style = ImGui::GetStyle();
    float buttonWidth = ImGui::GetFrameHeight();
    float inputWidth = ImGui::GetContentRegionAvail().x - buttonWidth - style.ItemSpacing.x;

    bool submitted = false;
    if (refocusInput) {
        ImGui::SetKeyboardFocusHere();
        refocusInput = false;
    }
    if (ImGui::InputTextMultiline("##AiInput", inputBuffer.data(), inputBuffer.size(),
                                  ImVec2(inputWidth, inputHeight),
                                  ImGuiInputTextFlags_EnterReturnsTrue |
                                  ImGuiInputTextFlags_CtrlEnterForNewLine)) {
        submitted = true;
    }

    ImGui::SameLine();
    if (service.isBusy()) {
        if (Widgets::iconButton("##AiStop", ICON_FA_STOP, ImVec2(buttonWidth, inputHeight))) {
            service.cancel();
        }
        ImGui::SetItemTooltip("Stop");
    } else {
        bool hasText = inputBuffer[0] != '\0';
        ImGui::BeginDisabled(!hasText);
        if (Widgets::iconButton("##AiSend", ICON_FA_PAPER_PLANE, ImVec2(buttonWidth, inputHeight))) {
            submitted = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Send  (Enter - Ctrl+Enter for newline)");
    }

    if (submitted) {
        submitMessage();
    }
}

void AiChatWindow::submitMessage() {
    if (service.isBusy() || inputBuffer[0] == '\0') {
        return;
    }
    if (service.sendUserMessage(inputBuffer.data())) {
        inputBuffer.fill('\0');
        scrollToBottom = true;
        refocusInput = true;
    }
}

void AiChatWindow::executeProposal(uint64_t proposalId) {
    ai::EditorActionExecutor executor(project, resourcesWindow, &service.getHttpClient());
    service.executeProposal(proposalId, executor);
    scrollToBottom = true;
}

void AiChatWindow::autoRunProposals() {
    ai::ApprovalMode mode = service.getSettings().approvalMode;
    if (mode == ai::ApprovalMode::PreviewThenApprove) {
        return;
    }

    for (const auto& proposal : service.getProposals()) {
        if (proposal.executed || proposal.executing) {
            continue;
        }
        bool eligible = (mode == ai::ApprovalMode::FullAgent) ||
                        (mode == ai::ApprovalMode::SafeAutoRun && proposal.readOnly);
        if (eligible) {
            executeProposal(proposal.id);
            break; // one per frame keeps the UI responsive
        }
    }
}

} // namespace doriax::editor
