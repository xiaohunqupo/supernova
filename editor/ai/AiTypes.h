#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace doriax::editor::ai {

using Json = nlohmann::json;

enum class ProviderId {
    OpenAI,
    Anthropic,
    Gemini,
    OpenAICompatible
};

enum class ChatRole {
    System,
    User,
    Assistant,
    Tool
};

enum class ApprovalMode {
    PreviewThenApprove,
    SafeAutoRun,
    FullAgent
};

struct Settings {
    ProviderId provider = ProviderId::OpenAI;
    std::string model = "gpt-4.1";
    std::string customEndpoint;
    ApprovalMode approvalMode = ApprovalMode::PreviewThenApprove;
    int maxOutputTokens = 1200;
};

struct ToolCall {
    std::string id;
    std::string name;
    Json arguments = Json::object();
};

struct ChatMessage {
    ChatRole role = ChatRole::User;
    std::string content;

    // Set on assistant messages that requested one or more tool calls. Kept in the
    // history so follow-up requests can present a well-formed tool round-trip.
    std::vector<ToolCall> toolCalls;

    // Set on tool-result messages (role == Tool).
    std::string toolCallId;
    std::string toolName;
    bool toolSuccess = true;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    Json parameters = Json::object();
    bool readOnly = true;
};

struct ProviderRequest {
    Settings settings;
    std::string apiKey;
    std::vector<ChatMessage> messages;
    std::vector<ToolDefinition> tools;
    std::string systemPrompt;
};

struct ProviderResponse {
    std::string text;
    std::vector<ToolCall> toolCalls;
    std::string error;
};

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    long timeoutSeconds = 90;
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;
};

struct ActionResult {
    bool success = false;
    std::string message;
    Json data = Json::object();
};

struct ActionProposal {
    uint64_t id = 0;
    std::string toolCallId;
    std::string toolName;
    Json arguments = Json::object();
    std::string description;
    bool readOnly = true;
    bool executed = false;
    bool executing = false;
    ActionResult result;
};

std::string toString(ProviderId provider);
ProviderId providerFromString(const std::string& value);
std::string defaultModelForProvider(ProviderId provider);

std::string toString(ApprovalMode mode);
ApprovalMode approvalModeFromString(const std::string& value);

} // namespace doriax::editor::ai
