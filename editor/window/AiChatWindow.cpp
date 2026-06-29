#include "AiChatWindow.h"

#include "App.h"
#include "AppSettings.h"
#include "ai/EditorActionExecutor.h"
#include "ai/SecretStore.h"
#include "external/IconsFontAwesome6.h"
#include "window/Widgets.h"
#include "window/widget/InputTextContextMenu.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace doriax::editor {

namespace {

const ImVec4 kUserColor(0.55f, 0.75f, 1.0f, 1.0f);
const ImVec4 kAssistantColor(0.72f, 0.86f, 0.68f, 1.0f);
const ImVec4 kSuccessColor(0.55f, 0.85f, 0.55f, 1.0f);
const ImVec4 kErrorColor(0.95f, 0.55f, 0.55f, 1.0f);
const ImVec4 kCodeColor(0.82f, 0.85f, 0.72f, 1.0f);

// ImGui does not soft-wrap editable multiline inputs. The composer inserts
// generated line breaks for display, tracks them separately from user-entered
// newlines, and reflows them whenever the input width changes.
struct PromptWrapContext {
    float maxLineWidth = 0.0f;
    std::vector<int>* softWraps = nullptr;
    std::string* displayText = nullptr;
};

struct PromptWrapResult {
    std::string displayText;
    std::vector<int> softWrapDisplayOffsets;
    std::vector<int> softWrapRawOffsets;
};

bool isPromptWrapSpace(char c) {
    return c == ' ' || c == '\t';
}

bool isUtf8Continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xc0) == 0x80;
}

int nextCodepointStart(const char* text, int len, int pos) {
    if (pos >= len) {
        return len;
    }
    ++pos;
    while (pos < len && isUtf8Continuation(text[pos])) {
        ++pos;
    }
    return pos;
}

float promptLineWidth(const char* text, int start, int end) {
    if (end <= start) {
        return 0.0f;
    }
    return ImGui::CalcTextSize(text + start, text + end, false).x;
}

int findPromptWrapOffset(const std::string& text, int lineStart, int lineEnd,
                         float maxLineWidth) {
    int bestOffset = lineStart;
    int bestSpaceOffset = -1;
    const int len = static_cast<int>(text.size());
    const char* raw = text.c_str();

    for (int pos = lineStart; pos < lineEnd;) {
        int next = nextCodepointStart(raw, len, pos);
        if (next <= pos || next > lineEnd) {
            next = std::min(pos + 1, lineEnd);
        }

        if (promptLineWidth(raw, lineStart, next) > maxLineWidth) {
            if (bestSpaceOffset > lineStart) {
                return bestSpaceOffset;
            }
            return bestOffset > lineStart ? bestOffset : next;
        }

        if (isPromptWrapSpace(raw[pos])) {
            bestSpaceOffset = next;
        }
        bestOffset = next;
        pos = next;
    }

    return -1;
}

void sanitizePromptSoftWraps(std::vector<int>& softWraps, const std::string& displayText) {
    std::sort(softWraps.begin(), softWraps.end());
    softWraps.erase(std::unique(softWraps.begin(), softWraps.end()), softWraps.end());

    std::vector<int> valid;
    valid.reserve(softWraps.size());
    for (int pos : softWraps) {
        if (pos >= 0 && pos < static_cast<int>(displayText.size()) &&
            displayText[static_cast<size_t>(pos)] == '\n') {
            valid.push_back(pos);
        }
    }
    softWraps.swap(valid);
}

void adjustPromptSoftWrapsForEdit(const std::string& oldDisplayText,
                                  const std::string& currentDisplayText,
                                  std::vector<int>& softWraps) {
    if (oldDisplayText.empty() || oldDisplayText == currentDisplayText) {
        sanitizePromptSoftWraps(softWraps, currentDisplayText);
        return;
    }

    int prefix = 0;
    const int oldLen = static_cast<int>(oldDisplayText.size());
    const int newLen = static_cast<int>(currentDisplayText.size());
    while (prefix < oldLen && prefix < newLen &&
           oldDisplayText[static_cast<size_t>(prefix)] ==
               currentDisplayText[static_cast<size_t>(prefix)]) {
        ++prefix;
    }

    int suffix = 0;
    while (oldLen - suffix > prefix && newLen - suffix > prefix &&
           oldDisplayText[static_cast<size_t>(oldLen - suffix - 1)] ==
               currentDisplayText[static_cast<size_t>(newLen - suffix - 1)]) {
        ++suffix;
    }

    const int oldChangeEnd = oldLen - suffix;
    const int newChangeEnd = newLen - suffix;
    const int delta = (newChangeEnd - prefix) - (oldChangeEnd - prefix);

    std::vector<int> adjusted;
    adjusted.reserve(softWraps.size());
    for (int pos : softWraps) {
        if (pos < prefix) {
            adjusted.push_back(pos);
        } else if (pos >= oldChangeEnd) {
            adjusted.push_back(pos + delta);
        }
    }

    softWraps.swap(adjusted);
    sanitizePromptSoftWraps(softWraps, currentDisplayText);
}

int promptDisplayPosToRawPos(int displayPos, const std::vector<int>& softWraps) {
    int removed = 0;
    for (int wrapPos : softWraps) {
        if (wrapPos >= displayPos) {
            break;
        }
        ++removed;
    }
    return std::max(0, displayPos - removed);
}

int promptRawPosToDisplayPos(int rawPos, const std::vector<int>& softWrapRawOffsets) {
    int inserted = 0;
    for (int wrapRawPos : softWrapRawOffsets) {
        if (wrapRawPos > rawPos) {
            break;
        }
        ++inserted;
    }
    return std::max(0, rawPos + inserted);
}

std::string stripPromptSoftWraps(const char* text, int len,
                                 const std::vector<int>& softWraps) {
    std::string raw;
    raw.reserve(static_cast<size_t>(std::max(0, len)));

    size_t wrapIndex = 0;
    for (int i = 0; i < len; ++i) {
        bool skipSoftWrap = wrapIndex < softWraps.size() && softWraps[wrapIndex] == i &&
                            text[i] == '\n';
        if (skipSoftWrap) {
            ++wrapIndex;
            continue;
        }
        raw.push_back(text[i]);
        while (wrapIndex < softWraps.size() && softWraps[wrapIndex] <= i) {
            ++wrapIndex;
        }
    }
    return raw;
}

std::string stripPromptSoftWrapsRange(const char* text, int start, int end,
                                      const std::vector<int>& softWraps) {
    if (!text || end <= start) {
        return {};
    }

    std::string raw;
    raw.reserve(static_cast<size_t>(end - start));
    auto wrapIt = std::lower_bound(softWraps.begin(), softWraps.end(), start);
    for (int i = start; i < end; ++i) {
        bool skipSoftWrap = wrapIt != softWraps.end() && *wrapIt == i && text[i] == '\n';
        if (skipSoftWrap) {
            ++wrapIt;
            continue;
        }
        raw.push_back(text[i]);
        while (wrapIt != softWraps.end() && *wrapIt <= i) {
            ++wrapIt;
        }
    }
    return raw;
}

std::string copyPromptContextMenuRange(const char* buffer, int start, int end,
                                       void* userData) {
    auto* context = static_cast<PromptWrapContext*>(userData);
    if (!context || !context->softWraps) {
        return std::string(buffer + start, buffer + end);
    }
    return stripPromptSoftWrapsRange(buffer, start, end, *context->softWraps);
}

PromptWrapResult rewrapPromptText(const std::string& rawText, float maxLineWidth,
                                  size_t maxDisplayBytes) {
    PromptWrapResult result;
    result.displayText.reserve(rawText.size());

    if (rawText.empty() || maxLineWidth <= ImGui::GetFontSize() * 4.0f) {
        result.displayText = rawText;
        return result;
    }

    const int len = static_cast<int>(rawText.size());
    int copyPos = 0;
    int hardLineStart = 0;

    while (hardLineStart < len) {
        int hardLineEnd = hardLineStart;
        while (hardLineEnd < len && rawText[static_cast<size_t>(hardLineEnd)] != '\n') {
            ++hardLineEnd;
        }

        int visualLineStart = hardLineStart;
        while (visualLineStart < hardLineEnd) {
            int wrapAt = findPromptWrapOffset(rawText, visualLineStart, hardLineEnd,
                                              maxLineWidth);
            if (wrapAt <= visualLineStart || wrapAt >= hardLineEnd) {
                break;
            }
            if (rawText.size() + result.softWrapDisplayOffsets.size() + 1 > maxDisplayBytes) {
                break;
            }

            result.displayText.append(rawText.data() + copyPos,
                                      static_cast<size_t>(wrapAt - copyPos));
            result.softWrapDisplayOffsets.push_back(
                static_cast<int>(result.displayText.size()));
            result.softWrapRawOffsets.push_back(wrapAt);
            result.displayText.push_back('\n');
            copyPos = wrapAt;
            visualLineStart = wrapAt;
        }

        if (hardLineEnd >= len) {
            break;
        }

        result.displayText.append(rawText.data() + copyPos,
                                  static_cast<size_t>(hardLineEnd + 1 - copyPos));
        copyPos = hardLineEnd + 1;
        hardLineStart = hardLineEnd + 1;
    }

    if (copyPos < len) {
        result.displayText.append(rawText.data() + copyPos,
                                  static_cast<size_t>(len - copyPos));
    }

    return result;
}

bool rewrapPromptBuffer(char* buffer, int bufferSize, float maxLineWidth,
                        std::vector<int>& softWraps, std::string& displayText,
                        int* cursorPos = nullptr, int* selectionStart = nullptr,
                        int* selectionEnd = nullptr, int* textLen = nullptr) {
    if (!buffer || bufferSize <= 0) {
        return false;
    }

    int len = textLen ? *textLen : static_cast<int>(std::strlen(buffer));
    len = std::clamp(len, 0, bufferSize - 1);
    std::string currentDisplay(buffer, buffer + len);
    adjustPromptSoftWrapsForEdit(displayText, currentDisplay, softWraps);

    int rawCursor = cursorPos ? promptDisplayPosToRawPos(*cursorPos, softWraps) : 0;
    int rawSelectionStart = selectionStart ? promptDisplayPosToRawPos(*selectionStart, softWraps) : 0;
    int rawSelectionEnd = selectionEnd ? promptDisplayPosToRawPos(*selectionEnd, softWraps) : 0;

    std::string rawText = stripPromptSoftWraps(currentDisplay.c_str(),
                                               static_cast<int>(currentDisplay.size()),
                                               softWraps);
    PromptWrapResult wrapped = rewrapPromptText(rawText, maxLineWidth,
                                                static_cast<size_t>(bufferSize - 1));

    const bool changed = wrapped.displayText != currentDisplay;
    if (changed) {
        std::memcpy(buffer, wrapped.displayText.c_str(), wrapped.displayText.size());
        buffer[wrapped.displayText.size()] = '\0';
        if (textLen) {
            *textLen = static_cast<int>(wrapped.displayText.size());
        }
        if (cursorPos) {
            *cursorPos = std::clamp(
                promptRawPosToDisplayPos(rawCursor, wrapped.softWrapRawOffsets),
                0, static_cast<int>(wrapped.displayText.size()));
        }
        if (selectionStart) {
            *selectionStart = std::clamp(
                promptRawPosToDisplayPos(rawSelectionStart, wrapped.softWrapRawOffsets),
                0, static_cast<int>(wrapped.displayText.size()));
        }
        if (selectionEnd) {
            *selectionEnd = std::clamp(
                promptRawPosToDisplayPos(rawSelectionEnd, wrapped.softWrapRawOffsets),
                0, static_cast<int>(wrapped.displayText.size()));
        }
    }

    softWraps = std::move(wrapped.softWrapDisplayOffsets);
    displayText = std::move(wrapped.displayText);
    return changed;
}

void rewrapPromptAfterContextMenuEdit(char* buffer, size_t bufferSize,
                                      int* cursorPos, int* selectionStart,
                                      int* selectionEnd, void* userData) {
    auto* context = static_cast<PromptWrapContext*>(userData);
    if (!context || !context->softWraps || !context->displayText) {
        return;
    }

    int textLen = static_cast<int>(std::strlen(buffer));
    rewrapPromptBuffer(buffer, static_cast<int>(bufferSize), context->maxLineWidth,
                       *context->softWraps, *context->displayText,
                       cursorPos, selectionStart, selectionEnd, &textLen);
}

int promptWrapCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit &&
        data->EventFlag != ImGuiInputTextFlags_CallbackAlways) {
        return 0;
    }

    auto* context = static_cast<PromptWrapContext*>(data->UserData);
    if (!context || !context->softWraps || !context->displayText) {
        return 0;
    }

    if (rewrapPromptBuffer(data->Buf, data->BufSize, context->maxLineWidth,
                           *context->softWraps, *context->displayText,
                           &data->CursorPos, &data->SelectionStart,
                           &data->SelectionEnd, &data->BufTextLen)) {
        data->BufDirty = true;
    }

    return 0;
}

const char* providerDisplayName(ai::ProviderId provider) {
    switch (provider) {
        case ai::ProviderId::OpenAI: return "OpenAI";
        case ai::ProviderId::Anthropic: return "Anthropic";
        case ai::ProviderId::Gemini: return "Gemini";
        case ai::ProviderId::DeepSeek: return "DeepSeek";
        case ai::ProviderId::OpenAICompatible: return "OpenAI-compatible";
    }
    return "OpenAI";
}

const char* kApprovalLabels[] = {"Preview then approve", "Auto-run read-only", "Full agent"};
const ai::ApprovalMode kApprovalValues[] = {
    ai::ApprovalMode::PreviewThenApprove,
    ai::ApprovalMode::SafeAutoRun,
    ai::ApprovalMode::FullAgent
};
constexpr int kApprovalCount = IM_ARRAYSIZE(kApprovalValues);

int approvalToIndex(ai::ApprovalMode mode) {
    for (int i = 0; i < kApprovalCount; ++i) {
        if (kApprovalValues[i] == mode) return i;
    }
    return 0;
}

const char* approvalDisplayName(ai::ApprovalMode mode) {
    return kApprovalLabels[approvalToIndex(mode)];
}

std::string modelDisplayName(const ai::Settings& settings, const ai::ModelCatalog& catalog) {
    std::vector<ai::ProviderId> providers = ai::SecretStore::providersWithKeys();
    if (providers.empty()) {
        return "No model";
    }
    if (!ai::SecretStore::hasApiKey(settings.provider)) {
        return "Select model";
    }

    std::string current = settings.model.empty()
        ? ai::defaultModelForProvider(settings.provider)
        : settings.model;
    for (const auto& model : catalog.models(settings.provider)) {
        if (current == model.id) {
            return model.label;
        }
    }
    return ai::ModelCatalog::humanizeModelId(current);
}

std::string compactTitleText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool lastSpace = false;
    for (unsigned char c : text) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!lastSpace && !out.empty()) {
                out.push_back(' ');
                lastSpace = true;
            }
        } else {
            out.push_back(static_cast<char>(c));
            lastSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string conversationTitleFromMessages(const std::vector<ai::ChatMessage>& messages) {
    for (const auto& message : messages) {
        if (message.role != ai::ChatRole::User) {
            continue;
        }
        std::string title = compactTitleText(message.content);
        if (title.empty()) {
            continue;
        }
        constexpr size_t kMaxTitleChars = 72;
        if (title.size() > kMaxTitleChars) {
            title.resize(kMaxTitleChars);
            title += "...";
        }
        return title;
    }
    return messages.empty() ? "New conversation" : "AI conversation";
}

std::string elideToWidth(std::string text, float width) {
    if (width <= 0.0f) {
        return {};
    }
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        return text;
    }
    const std::string suffix = "...";
    while (!text.empty()) {
        text.pop_back();
        std::string candidate = text + suffix;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) {
            return candidate;
        }
    }
    return suffix;
}

int64_t currentEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void constrainNextPopupToViewport(float minWidth, float maxWidth, float maxHeightFraction) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float viewportMaxWidth = std::max(120.0f, viewport->WorkSize.x - 16.0f);
    float popupMaxWidth = std::min(maxWidth, viewportMaxWidth);
    float popupMinWidth = std::min(minWidth, popupMaxWidth);
    float popupMaxHeight = std::max(ImGui::GetFrameHeightWithSpacing() * 8.0f,
                                    viewport->WorkSize.y * maxHeightFraction);

    ImGui::SetNextWindowSizeConstraints(ImVec2(popupMinWidth, 0.0f),
                                        ImVec2(popupMaxWidth, popupMaxHeight));
}

void keepCurrentWindowInsideViewport() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    ImVec2 workMin = viewport->WorkPos;
    ImVec2 workMax(viewport->WorkPos.x + viewport->WorkSize.x,
                   viewport->WorkPos.y + viewport->WorkSize.y);

    if (pos.x + size.x > workMax.x) pos.x = workMax.x - size.x;
    if (pos.y + size.y > workMax.y) pos.y = workMax.y - size.y;
    if (pos.x < workMin.x) pos.x = workMin.x;
    if (pos.y < workMin.y) pos.y = workMin.y;

    ImGui::SetWindowPos(pos, ImGuiCond_Always);
}

std::string compactRelativeTime(int64_t updatedAt, int64_t now) {
    if (updatedAt <= 0) {
        return "";
    }

    int64_t seconds = std::max<int64_t>(0, now - updatedAt);
    if (seconds < 60) return "now";

    int64_t minutes = seconds / 60;
    if (minutes < 60) return std::to_string(minutes) + "m";

    int64_t hours = minutes / 60;
    if (hours < 24) return std::to_string(hours) + "h";

    int64_t days = hours / 24;
    if (days < 7) return std::to_string(days) + "d";

    int64_t weeks = days / 7;
    if (weeks < 5) return std::to_string(weeks) + "w";

    int64_t months = days / 30;
    if (months < 12) return std::to_string(std::max<int64_t>(1, months)) + "mo";

    int64_t years = days / 365;
    return std::to_string(std::max<int64_t>(1, years)) + "y";
}

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
    , resourcesWindow(resourcesWindow)
    , conversationStore(project) {
    service.setSettings(AppSettings::getAiSettings());
    prefetchModels();
}

void AiChatWindow::prefetchModels() {
    // Kick off a background /models fetch for every configured provider at
    // app open, so the picker is already populated by the time it is opened.
    const ai::Settings settings = AppSettings::getAiSettings();
    for (ai::ProviderId provider : ai::SecretStore::providersWithKeys()) {
        modelCatalog.refresh(provider, ai::SecretStore::getApiKey(provider), settings.customEndpoint);
    }
}

void AiChatWindow::show() {
    if (!windowOpen) {
        isWindowVisible = false;
        return;
    }

    if (!syncedInitialSettings) {
        service.setSettings(AppSettings::getAiSettings());
        syncedInitialSettings = true;
    }
    loadLatestConversationForCurrentProject();

    if (!isWindowVisible) {
        updateMessageNotification();
    }

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (hasNotification) {
        App::pushTabNotificationStyle();
        windowFlags |= ImGuiWindowFlags_UnsavedDocument;
    }

    if (!ImGui::Begin(WINDOW_NAME, &windowOpen, windowFlags)) {
        if (hasNotification) App::popTabNotificationStyle();
        windowFocused = false;
        isWindowVisible = false;
        updateMessageNotification();
        ImGui::End();
        settingsWindow.show();
        return;
    }
    if (hasNotification) App::popTabNotificationStyle();

    isWindowVisible = true;
    hasNotification = false;
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // Drive the agent loop: auto-run eligible proposals, then let the service
    // send any follow-up request triggered by completed tool results.
    if (!service.isBusy()) {
        autoRunProposals();
        service.update();
        updateMessageNotification();
        persistConversation();
    }

    drawHeader();
    ImGui::Separator();

    const ImGuiStyle& style = ImGui::GetStyle();
    float inputHeight = ImGui::GetTextLineHeight() * 3.0f + style.FramePadding.y * 2.0f;
    float controlsHeight = ImGui::GetFrameHeight();
    float composerHeight = inputHeight + controlsHeight + style.ItemSpacing.y + 2.0f;

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
    if (!open) {
        isWindowVisible = false;
        windowFocused = false;
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
    std::vector<ai::ChatMessage> messages = service.getMessages();
    const ImGuiStyle& style = ImGui::GetStyle();

    float height = ImGui::GetFrameHeight();
    ImVec2 buttonSize(height, height);
    float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    float buttonsWidth = height * 3.0f + style.ItemSpacing.x * 2.0f;
    float titleWidth = std::max(20.0f, ImGui::GetContentRegionAvail().x - buttonsWidth - style.ItemSpacing.x);

    ImGui::AlignTextToFramePadding();
    std::string title = elideToWidth(std::string(ICON_FA_MESSAGE "  ") +
                                     conversationTitleFromMessages(messages), titleWidth);
    ImGui::TextDisabled("%s", title.c_str());

    ImGui::SameLine();
    ImGui::SetCursorPosX(rightEdge - buttonsWidth);
    ImGui::BeginDisabled(service.isBusy());
    if (Widgets::iconButton("##AiNewChat", ICON_FA_PLUS, buttonSize)) {
        startNewChat();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("New chat");

    ImGui::SameLine();
    if (Widgets::iconButton("##AiHistory", ICON_FA_CLOCK_ROTATE_LEFT, buttonSize)) {
        ImGui::OpenPopup("AI History##AiHistoryPopup");
    }
    ImGui::SetItemTooltip("History");
    drawHistoryPopup();

    ImGui::SameLine();
    if (Widgets::iconButton("##AiSettings", ICON_FA_GEAR, buttonSize)) {
        settingsWindow.open(&service);
    }
    ImGui::SetItemTooltip("AI settings");
}

void AiChatWindow::drawHistoryPopup() {
    if (!ImGui::BeginPopup("AI History##AiHistoryPopup")) {
        return;
    }

    ImGui::TextDisabled(ICON_FA_COMMENTS "  Conversations");
    ImGui::Separator();

    std::vector<ai::ConversationMeta> conversations = conversationStore.list();
    if (conversations.empty()) {
        ImGui::TextDisabled("No saved conversations yet");
        ImGui::EndPopup();
        return;
    }

    float trashWidth = ImGui::GetFrameHeight();
    float ageWidth = ImGui::CalcTextSize("999mo").x;
    float rowWidth = 360.0f;
    int64_t now = currentEpochSeconds();
    std::string toDelete;

    for (const auto& meta : conversations) {
        ImGui::PushID(meta.id.c_str());
        bool isCurrent = meta.id == currentConversationId;

        const ImGuiStyle& style = ImGui::GetStyle();
        float titleWidth = rowWidth - trashWidth - ageWidth - style.ItemSpacing.x * 2.0f;
        std::string label = elideToWidth(meta.title.empty() ? "Conversation" : meta.title,
                                         titleWidth);
        if (ImGui::Selectable(label.c_str(), isCurrent,
                              ImGuiSelectableFlags_AllowOverlap, ImVec2(titleWidth, 0))) {
            loadConversation(meta.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", compactRelativeTime(meta.updatedAt, now).c_str());
        ImGui::SameLine();
        if (Widgets::iconButton("##DeleteConversation", ICON_FA_TRASH, ImVec2(trashWidth, ImGui::GetFrameHeight()))) {
            toDelete = meta.id;
        }
        ImGui::SetItemTooltip("Delete conversation");
        ImGui::PopID();
    }

    if (!toDelete.empty()) {
        conversationStore.remove(toDelete);
        if (toDelete == currentConversationId) {
            service.clearConversation();
            currentConversationId.clear();
            lastSavedMessageCount = 0;
            lastObservedMessageCount = 0;
            hasNotification = false;
        }
    }

    ImGui::EndPopup();
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
        if (ai::SecretStore::providersWithKeys().empty()) {
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

    // Tighten transcript line spacing only; keep default ItemSpacing for the
    // right-click menu (same pattern as OutputWindow).
    const ImGuiStyle& style = ImGui::GetStyle();
    const float transcriptLineSpacing = std::max(2.0f, style.ItemSpacing.y * 0.4f);
    transcript.draw("##AiTranscript", ImVec2(0, height), paragraphs, scrollToBottom,
                    transcriptLineSpacing);
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
    PromptWrapContext wrapContext{
        std::max(0.0f, inputWidth - style.FramePadding.x * 2.0f),
        &inputSoftWraps,
        &inputDisplayText
    };
    if (ImGui::InputTextMultiline("##AiInput", inputBuffer.data(), inputBuffer.size(),
                                  ImVec2(inputWidth, inputHeight),
                                  ImGuiInputTextFlags_EnterReturnsTrue |
                                  ImGuiInputTextFlags_CtrlEnterForNewLine |
                                  ImGuiInputTextFlags_NoHorizontalScroll |
                                  ImGuiInputTextFlags_CallbackEdit |
                                  ImGuiInputTextFlags_CallbackAlways,
                                  promptWrapCallback, &wrapContext)) {
        submitted = true;
    }
    bool inputActive = ImGui::IsItemActive();
    InputTextContextMenu::Options inputMenuOptions;
    inputMenuOptions.userData = &wrapContext;
    inputMenuOptions.copyRange = copyPromptContextMenuRange;
    inputMenuOptions.afterEdit = rewrapPromptAfterContextMenuEdit;
    InputTextContextMenu::drawForLastItem(inputBuffer.data(), inputBuffer.size(),
                                          inputMenuOptions);

    if (!inputActive && !submitted) {
        rewrapPromptBuffer(inputBuffer.data(), static_cast<int>(inputBuffer.size()),
                           wrapContext.maxLineWidth, inputSoftWraps, inputDisplayText);
    }
    if (submitted) {
        submitMessage();
        submitted = false;
    }

    ImGui::SameLine();
    if (service.isBusy()) {
        if (Widgets::iconButton("##AiStop", ICON_FA_STOP, ImVec2(buttonWidth, inputHeight))) {
            service.cancel();
        }
        ImGui::SetItemTooltip("Stop");
    } else {
        std::string promptText = stripPromptSoftWraps(
            inputBuffer.data(), static_cast<int>(std::strlen(inputBuffer.data())),
            inputSoftWraps);
        bool hasText = !promptText.empty();
        std::string disabledReason;
        bool canSend = hasText && canSendMessage(&disabledReason);
        ImGui::BeginDisabled(!canSend);
        if (Widgets::iconButton("##AiSend", ICON_FA_PAPER_PLANE, ImVec2(buttonWidth, inputHeight))) {
            submitted = true;
        }
        ImGui::EndDisabled();
        if (hasText && !disabledReason.empty()) {
            ImGui::SetItemTooltip("%s", disabledReason.c_str());
        } else {
            ImGui::SetItemTooltip("Send  (Enter - Ctrl+Enter for newline)");
        }
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y);
    drawComposerControls();

    if (submitted) {
        submitMessage();
    }
}

void AiChatWindow::drawComposerControls() {
    const ImGuiStyle& style = ImGui::GetStyle();

    ai::Settings settings = service.getSettings();
    const bool busy = service.isBusy();

    float avail = ImGui::GetContentRegionAvail().x;
    float buttonWidth = ImGui::GetFrameHeight();
    float editGap = std::max(4.0f, style.ItemInnerSpacing.x);
    std::string modelText = modelDisplayName(settings, modelCatalog);
    float preferredModelWidth = ImGui::CalcTextSize(modelText.c_str()).x + buttonWidth + editGap;
    float modelWidth = std::min(std::max(70.0f, preferredModelWidth),
                                std::max(70.0f, avail * 0.62f));
    if (avail > 84.0f) {
        modelWidth = std::min(modelWidth, avail - 44.0f);
    }
    float approvalWidth = avail - modelWidth - style.ItemSpacing.x;
    approvalWidth = std::max(40.0f, approvalWidth);
    modelWidth = std::max(40.0f, modelWidth);
    float rowStartX = ImGui::GetCursorPosX();
    float rowRightX = rowStartX + avail;

    ImGui::BeginDisabled(busy);

    const ImVec4* approvalColor = settings.approvalMode == ai::ApprovalMode::FullAgent
        ? &App::ThemeColors::DisabledGreenText
        : nullptr;
    drawEditableSettingLabel(approvalDisplayName(settings.approvalMode), approvalWidth,
                             "##AiApprovalEdit", "AI Approval##AiApprovalPopup",
                             approvalColor);

    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(rowStartX, rowRightX - modelWidth));
    drawEditableSettingLabel(modelText, modelWidth,
                             "##AiModelEdit", "AI Model##AiModelPopup");

    ImGui::EndDisabled();

    drawModelPopup();
    drawCustomModelPopup();
    drawApprovalPopup();
}

void AiChatWindow::drawEditableSettingLabel(const std::string& value, float width,
                                           const char* editId, const char* popupId,
                                           const ImVec4* textColor) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* icon = ICON_FA_PEN_TO_SQUARE;
    float frameHeight = ImGui::GetFrameHeight();
    float iconWidth = ImGui::CalcTextSize(icon).x;
    float gap = std::max(4.0f, style.ItemInnerSpacing.x);
    float textWidth = std::max(1.0f, width - iconWidth - gap);

    std::string display = elideToWidth(value, textWidth);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(editId, ImVec2(width, frameHeight))) {
        ImGui::OpenPopup(popupId);
    }

    ImU32 valueColor = textColor
        ? ImGui::GetColorU32(*textColor)
        : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImU32 iconColor = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImVec2 textSize = ImGui::CalcTextSize(display.c_str());
    float textY = pos.y + (frameHeight - textSize.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, textY), valueColor, display.c_str());

    float iconX = std::min(pos.x + textSize.x + gap, pos.x + width - iconWidth);
    ImVec2 iconSize = ImGui::CalcTextSize(icon);
    float iconY = pos.y + (frameHeight - iconSize.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(iconX, iconY), iconColor, icon);
}

void AiChatWindow::drawModelPopup() {
    constrainNextPopupToViewport(260.0f, 420.0f, 0.62f);
    if (!ImGui::BeginPopup("AI Model##AiModelPopup")) {
        return;
    }

    ai::Settings settings = service.getSettings();
    std::vector<ai::ProviderId> providers = ai::SecretStore::providersWithKeys();

    if (providers.empty()) {
        ImGui::TextDisabled("No providers configured");
        if (ImGui::Selectable("Add an API key...")) {
            settingsWindow.open(&service);
            ImGui::CloseCurrentPopup();
        }
        keepCurrentWindowInsideViewport();
        ImGui::EndPopup();
        return;
    }

    std::string current = settings.model.empty()
        ? ai::defaultModelForProvider(settings.provider)
        : settings.model;

    // Only providers with a configured key are listed; their models are fetched
    // live from the provider (nothing is shown until the fetch lands).
    for (size_t pi = 0; pi < providers.size(); ++pi) {
        ai::ProviderId provider = providers[pi];
        if (modelCatalog.status(provider) == ai::CatalogStatus::Idle) {
            modelCatalog.refresh(provider, ai::SecretStore::getApiKey(provider), settings.customEndpoint);
        }

        if (pi > 0) {
            ImGui::Spacing();
        }
        ImGui::TextDisabled("%s", providerDisplayName(provider));
        ImGui::Separator();

        std::vector<ai::ModelInfo> models = modelCatalog.models(provider);
        for (const auto& model : models) {
            bool selected = (provider == settings.provider) && (current == model.id);
            ImGui::PushID(model.id.c_str());
            if (ImGui::Selectable(model.label.c_str(), selected)) {
                ai::Settings next = settings;
                next.provider = provider;
                next.model = model.id;
                applySettings(next);
                ImGui::CloseCurrentPopup();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        if (models.empty()) {
            ImGui::TextDisabled("  %s", modelCatalog.status(provider) == ai::CatalogStatus::Loading
                                            ? "Loading..."
                                            : "No models available");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Selectable(ICON_FA_ROTATE "  Refresh models")) {
        for (ai::ProviderId provider : providers) {
            modelCatalog.refresh(provider, ai::SecretStore::getApiKey(provider), settings.customEndpoint);
        }
    }

    if (ImGui::Selectable("Custom model...")) {
        std::snprintf(customModelBuffer.data(), customModelBuffer.size(), "%s", current.c_str());
        customModelPopupRequested = true;
        ImGui::CloseCurrentPopup();
    }

    keepCurrentWindowInsideViewport();
    ImGui::EndPopup();
}

void AiChatWindow::drawCustomModelPopup() {
    if (customModelPopupRequested) {
        ImGui::OpenPopup("Custom model##AiCustomModelPopup");
        customModelPopupRequested = false;
    }

    if (!ImGui::BeginPopupModal("Custom model##AiCustomModelPopup", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui::SetNextItemWidth(320.0f);
    ImGui::InputText("##AiCustomModelInput", customModelBuffer.data(), customModelBuffer.size());

    ImGui::BeginDisabled(customModelBuffer[0] == '\0');
    if (ImGui::Button("Use", ImVec2(90, 0))) {
        ai::Settings next = service.getSettings();
        next.model = customModelBuffer.data();
        applySettings(next);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void AiChatWindow::drawApprovalPopup() {
    if (ImGui::BeginPopup("AI Approval##AiApprovalPopup")) {
        ai::Settings settings = service.getSettings();
        int approvalIndex = approvalToIndex(settings.approvalMode);
        for (int i = 0; i < kApprovalCount; ++i) {
            bool selected = i == approvalIndex;
            if (ImGui::Selectable(kApprovalLabels[i], selected)) {
                settings.approvalMode = kApprovalValues[i];
                applySettings(settings);
                ImGui::CloseCurrentPopup();
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndPopup();
    }
}

void AiChatWindow::applySettings(const ai::Settings& settings) {
    ai::Settings next = settings;
    if (next.model.empty()) {
        next.model = ai::defaultModelForProvider(next.provider);
    }
    AppSettings::setAiSettings(next);
    service.setSettings(next);
}

bool AiChatWindow::canSendMessage(std::string* reason) const {
    ai::Settings settings = service.getSettings();
    if (ai::SecretStore::providersWithKeys().empty()) {
        if (reason) *reason = "Add an API key before sending.";
        return false;
    }
    if (!ai::SecretStore::hasApiKey(settings.provider)) {
        if (reason) *reason = "Select a configured model/provider before sending.";
        return false;
    }
    if (settings.model.empty()) {
        if (reason) *reason = "Select a model before sending.";
        return false;
    }
    return true;
}

void AiChatWindow::submitMessage() {
    std::string promptText = stripPromptSoftWraps(
        inputBuffer.data(), static_cast<int>(std::strlen(inputBuffer.data())),
        inputSoftWraps);
    if (service.isBusy() || promptText.empty() || !canSendMessage()) {
        return;
    }
    if (service.sendUserMessage(promptText)) {
        inputBuffer.fill('\0');
        inputSoftWraps.clear();
        inputDisplayText.clear();
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

void AiChatWindow::updateMessageNotification() {
    std::vector<ai::ChatMessage> messages = service.getMessages();
    if (messages.size() < lastObservedMessageCount) {
        lastObservedMessageCount = messages.size();
        return;
    }

    bool receivedIncomingMessage = false;
    for (size_t i = lastObservedMessageCount; i < messages.size(); ++i) {
        if (messages[i].role == ai::ChatRole::Assistant || messages[i].role == ai::ChatRole::Tool) {
            receivedIncomingMessage = true;
            break;
        }
    }

    if (receivedIncomingMessage && !isWindowVisible) {
        hasNotification = true;
    }
    lastObservedMessageCount = messages.size();
}

void AiChatWindow::startNewChat() {
    if (service.isBusy()) {
        return;
    }
    // The previous conversation was already persisted by the per-frame pump.
    service.clearConversation();
    currentConversationId.clear();
    lastSavedMessageCount = 0;
    lastObservedMessageCount = 0;
    hasNotification = false;
    inputBuffer.fill('\0');
    inputSoftWraps.clear();
    inputDisplayText.clear();
    scrollToBottom = false;
    // Mark the current path as handled so a fresh temp project at a reused path
    // does not auto-restore the previous chat on the next frame.
    autoLoadedProjectPath = project ? project->getProjectPath().string() : std::string();
    autoLoadedLatestConversation = true;
}

void AiChatWindow::loadLatestConversationForCurrentProject() {
    if (!project || service.isBusy()) {
        return;
    }

    const std::string projectPath = project->getProjectPath().string();
    if (projectPath != autoLoadedProjectPath) {
        autoLoadedProjectPath = projectPath;
        autoLoadedLatestConversation = false;
        service.clearConversation();
        currentConversationId.clear();
        lastSavedMessageCount = 0;
        lastObservedMessageCount = 0;
        hasNotification = false;
        scrollToBottom = false;
        inputBuffer.fill('\0');
        inputSoftWraps.clear();
        inputDisplayText.clear();
    }

    if (projectPath.empty() || autoLoadedLatestConversation) {
        return;
    }

    autoLoadedLatestConversation = true;
    std::vector<ai::ConversationMeta> conversations = conversationStore.list();
    if (!conversations.empty()) {
        loadConversation(conversations.front().id);
    }
}

void AiChatWindow::persistConversation() {
    std::vector<ai::ChatMessage> messages = service.getMessages();
    if (messages.size() == lastSavedMessageCount) {
        return; // nothing new since the last save
    }
    if (messages.empty()) {
        lastSavedMessageCount = 0;
        return;
    }
    if (currentConversationId.empty()) {
        currentConversationId = ai::ConversationStore::newId();
    }
    conversationStore.save(currentConversationId, conversationTitleFromMessages(messages), messages);
    lastSavedMessageCount = messages.size();
}

void AiChatWindow::loadConversation(const std::string& id) {
    if (service.isBusy()) {
        return;
    }
    std::vector<ai::ChatMessage> messages;
    if (!conversationStore.load(id, messages)) {
        return;
    }
    service.loadConversation(std::move(messages));
    currentConversationId = id;
    lastSavedMessageCount = service.getMessages().size();
    lastObservedMessageCount = lastSavedMessageCount;
    hasNotification = false;
    inputBuffer.fill('\0');
    inputSoftWraps.clear();
    inputDisplayText.clear();
    scrollToBottom = true;
}

} // namespace doriax::editor
