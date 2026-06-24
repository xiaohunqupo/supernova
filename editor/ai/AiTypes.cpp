#include "AiTypes.h"

namespace doriax::editor::ai {

std::string toString(ProviderId provider) {
    switch (provider) {
        case ProviderId::OpenAI: return "openai";
        case ProviderId::Anthropic: return "anthropic";
        case ProviderId::Gemini: return "gemini";
        case ProviderId::OpenAICompatible: return "openai_compatible";
    }
    return "openai";
}

ProviderId providerFromString(const std::string& value) {
    if (value == "anthropic") return ProviderId::Anthropic;
    if (value == "gemini") return ProviderId::Gemini;
    if (value == "openai_compatible") return ProviderId::OpenAICompatible;
    return ProviderId::OpenAI;
}

std::string defaultModelForProvider(ProviderId provider) {
    switch (provider) {
        case ProviderId::OpenAI: return "gpt-4.1";
        case ProviderId::Anthropic: return "claude-sonnet-4-20250514";
        case ProviderId::Gemini: return "gemini-2.5-flash"; // Flash has a free tier; Pro does not
        case ProviderId::OpenAICompatible: return "openai/gpt-4.1";
    }
    return "gpt-4.1";
}

std::string toString(ApprovalMode mode) {
    switch (mode) {
        case ApprovalMode::PreviewThenApprove: return "preview_then_approve";
        case ApprovalMode::SafeAutoRun: return "safe_auto_run";
        case ApprovalMode::FullAgent: return "full_agent";
    }
    return "preview_then_approve";
}

ApprovalMode approvalModeFromString(const std::string& value) {
    if (value == "safe_auto_run") return ApprovalMode::SafeAutoRun;
    if (value == "full_agent") return ApprovalMode::FullAgent;
    return ApprovalMode::PreviewThenApprove;
}

} // namespace doriax::editor::ai
