#pragma once

#include "AiTypes.h"
#include "HttpClient.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace doriax::editor::ai {

class EditorActionExecutor;

class AiService {
public:
    AiService();
    ~AiService();

    void setSettings(const Settings& settings);
    Settings getSettings() const;

    bool sendUserMessage(const std::string& text);
    void cancel();
    bool isBusy() const;

    // Drives the agent loop: reaps a finished worker and, when the conversation
    // ends on unanswered tool results, automatically sends a follow-up request
    // so the model can react to them. Call once per frame.
    void update();

    std::vector<ChatMessage> getMessages() const;
    std::vector<ActionProposal> getProposals() const;
    void clearConversation();

    // Replaces the in-memory conversation (e.g. when loading saved history).
    // Ignored while a request is in flight.
    void loadConversation(std::vector<ChatMessage> newMessages);

    ActionResult executeProposal(uint64_t proposalId, EditorActionExecutor& executor);
    void removeProposal(uint64_t proposalId);

    const HttpClient& getHttpClient() const { return httpClient; }

private:
    static constexpr int kMaxToolRounds = 8;

    mutable std::mutex mutex;
    Settings settings;
    std::vector<ChatMessage> messages;
    std::vector<ActionProposal> proposals;
    std::thread worker;
    std::atomic<bool> busy{false};
    std::atomic<bool> cancelRequested{false};
    uint64_t nextProposalId = 1;
    int toolRounds = 0;
    HttpClient httpClient;

    std::string buildSystemPrompt() const;
    ProviderRequest buildRequestSnapshotLocked() const;
    void dispatchRequest(ProviderRequest request);
    void runProviderRequest(ProviderRequest request);
    bool needsContinuationLocked() const;
    void appendAssistantMessageLocked(const std::string& text);
    void addToolCallProposalLocked(const ToolCall& call);
};

} // namespace doriax::editor::ai
