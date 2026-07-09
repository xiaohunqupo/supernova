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
    DeepSeek,
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
    // Large enough to write a whole file (scripts, forked shaders) in one tool call; a low
    // cap truncates the call mid-JSON and the partial call is dropped (looks like an empty
    // reply). Output tokens are only billed when generated, so a high cap is safe.
    int maxOutputTokens = 8192;
};

struct ToolCall {
    std::string id;
    std::string name;
    Json arguments = Json::object();
    // Opaque Gemini thought signature (REST: thoughtSignature). Required on the
    // first functionCall part of each model step for Gemini 3 / thinking models;
    // must be echoed verbatim on the next request.
    std::string thoughtSignature;
};

struct ChatMessage {
    ChatRole role = ChatRole::User;
    std::string content;

    // Set on assistant messages that requested one or more tool calls. Kept in the
    // history so follow-up requests can present a well-formed tool round-trip.
    std::vector<ToolCall> toolCalls;

    // Anthropic thinking / redacted_thinking content blocks (with signature).
    // Claude Sonnet 5+ enables adaptive thinking by default; these must be echoed
    // verbatim ahead of tool_use when continuing a tool-use turn.
    std::vector<Json> thinkingBlocks;

    // Set on tool-result messages (role == Tool).
    std::string toolCallId;
    std::string toolName;
    // Human-readable label for the transcript (from EditorActionRegistry::describe).
    std::string toolDescription;
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
    // Anthropic thinking / redacted_thinking blocks from the model turn.
    std::vector<Json> thinkingBlocks;
    std::string error;
    // Provider stop reason such as finish_reason "length", stop_reason "max_tokens",
    // or Gemini finishReason "MAX_TOKENS".
    std::string stopReason;
    // Token usage when returned by the provider. A value of -1 means unknown.
    int promptTokens = -1;
    int completionTokens = -1;
    int totalTokens = -1;
    // True when the model stopped because it hit a token limit; the reply is likely incomplete.
    bool truncated = false;
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

std::string toString(ChatRole role);
ChatRole chatRoleFromString(const std::string& value);

std::string toString(ApprovalMode mode);
ApprovalMode approvalModeFromString(const std::string& value);

} // namespace doriax::editor::ai
