#include "AiService.h"

#include "AiProvider.h"
#include "EditorActionExecutor.h"
#include "EditorActionRegistry.h"
#include "Out.h"
#include "SecretStore.h"

#include <algorithm>
#include <memory>
#include <sstream>

namespace doriax::editor::ai {

namespace {

// Turns a provider HTTP failure into a short, readable line. The detail is the
// provider's own error.message (already extracted by the response parsers),
// not the raw JSON envelope.
std::string humanizeProviderError(long status, const std::string& detail) {
    std::string label;
    if (status == 400) label = "Bad request";
    else if (status == 401 || status == 403) label = "Authentication failed - check your API key";
    else if (status == 404) label = "Not found - check the model name or endpoint";
    else if (status == 429) label = "Rate limit or quota exceeded";
    else if (status >= 500) label = "The provider is unavailable, try again later";
    else label = "Request failed";

    std::string out = label + " (HTTP " + std::to_string(status) + ")";
    if (!detail.empty()) {
        out += ": " + detail;
    }
    return out;
}

} // namespace

AiService::AiService() = default;

AiService::~AiService() {
    cancel();
    if (worker.joinable()) {
        worker.join();
    }
}

void AiService::setSettings(const Settings& newSettings) {
    std::lock_guard<std::mutex> lock(mutex);
    settings = newSettings;
    if (settings.model.empty()) {
        settings.model = defaultModelForProvider(settings.provider);
    }
}

Settings AiService::getSettings() const {
    std::lock_guard<std::mutex> lock(mutex);
    return settings;
}

bool AiService::sendUserMessage(const std::string& text) {
    if (text.empty() || busy.load()) {
        return false;
    }

    ProviderRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex);
        messages.push_back({ChatRole::User, text});
        toolRounds = 0;
        request = buildRequestSnapshotLocked();
    }
    dispatchRequest(std::move(request));
    return true;
}

void AiService::cancel() {
    cancelRequested.store(true);
}

bool AiService::isBusy() const {
    return busy.load();
}

void AiService::update() {
    if (busy.load()) {
        return;
    }
    // Reap the worker that produced the latest response before reusing it.
    if (worker.joinable()) {
        worker.join();
    }

    ProviderRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!needsContinuationLocked() || toolRounds >= kMaxToolRounds) {
            if (needsContinuationLocked() && toolRounds >= kMaxToolRounds) {
                appendAssistantMessageLocked(
                    "Reached the tool-step limit for this request. Send another message to continue.");
            }
            return;
        }
        ++toolRounds;
        request = buildRequestSnapshotLocked();
    }
    dispatchRequest(std::move(request));
}

std::vector<ChatMessage> AiService::getMessages() const {
    std::lock_guard<std::mutex> lock(mutex);
    return messages;
}

std::vector<ActionProposal> AiService::getProposals() const {
    std::lock_guard<std::mutex> lock(mutex);
    return proposals;
}

void AiService::clearConversation() {
    if (busy.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    messages.clear();
    proposals.clear();
    toolRounds = 0;
}

void AiService::loadConversation(std::vector<ChatMessage> newMessages) {
    if (busy.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    messages = std::move(newMessages);
    proposals.clear();
    toolRounds = 0;
}

ActionResult AiService::executeProposal(uint64_t proposalId, EditorActionExecutor& executor) {
    ActionProposal proposal;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& item : proposals) {
            if (item.id == proposalId) {
                if (item.executed) {
                    return item.result;
                }
                item.executing = true;
                proposal = item;
                break;
            }
        }
    }

    if (proposal.id == 0) {
        return {false, "Action proposal not found.", Json::object()};
    }

    ActionResult result = executor.execute(proposal.toolName, proposal.arguments, &cancelRequested);

    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& item : proposals) {
            if (item.id == proposalId) {
                item.executing = false;
                item.executed = true;
                item.result = result;
                break;
            }
        }

        ChatMessage toolMessage;
        toolMessage.role = ChatRole::Tool;
        toolMessage.toolCallId = proposal.toolCallId;
        toolMessage.toolName = proposal.toolName;
        toolMessage.toolSuccess = result.success;
        toolMessage.content = result.success ? result.message : ("Error: " + result.message);
        if (!result.data.empty()) {
            toolMessage.content += "\n" + result.data.dump(2);
        }
        messages.push_back(toolMessage);
    }

    if (result.success) {
        Out::success("AI action completed: %s", proposal.description.c_str());
    } else {
        Out::error("AI action failed: %s", result.message.c_str());
    }

    return result;
}

void AiService::removeProposal(uint64_t proposalId) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& proposal : proposals) {
        if (proposal.id != proposalId || proposal.executed) {
            continue;
        }
        // Resolve the call with a dismissal result rather than dropping it, so
        // the assistant's tool call stays answered and the next request that
        // replays the history remains well-formed.
        proposal.executing = false;
        proposal.executed = true;
        proposal.result = {false, "Dismissed by the user.", Json::object()};

        ChatMessage toolMessage;
        toolMessage.role = ChatRole::Tool;
        toolMessage.toolCallId = proposal.toolCallId;
        toolMessage.toolName = proposal.toolName;
        toolMessage.toolSuccess = false;
        toolMessage.content = "The user dismissed this action.";
        messages.push_back(toolMessage);
        break;
    }
}

std::string AiService::buildSystemPrompt() const {
    std::ostringstream prompt;
    prompt
        << "You are Doriax Editor Assistant, an AI helper embedded inside the Doriax game editor.\n"
        << "You answer questions and drive the editor through the provided tools. The engine layer is not aware of you.\n"
        << "Call tools only when they help the user's explicit goal, and never invent tool names.\n"
        << "Read-only tools inspect project context. Mutating, file-writing, download, and import tools may require user approval before they run.\n"
        << "Use read_resource_file before editing existing scripts or materials. Use inspect_scene for scene-level settings.\n"
        << "After a tool returns, use its result to decide the next step and report concise progress to the user.\n"
        << "Prefer project-relative paths. Never request arbitrary shell commands. Never ask for secrets in chat.\n"
        << "For external assets, use curated sources only and preserve license/author/source attribution.\n"
        << "For scripts and engine API code, do not invent Doriax APIs. Use search_engine_api when you need classes, methods, or signatures.\n"
        << "When you create a script for requested behavior, write the complete script with update_script_file instead of asking the user to edit it manually.\n"
        << "Doriax Lua scripts are plain returned module tables: local Name = { properties = {} }; function Name:init() ... end; return Name.\n"
        << "Lua script instances receive self.scene and self.entity; self.entity is a numeric Entity id, not an object. Never call self.entity:... or self.entity....\n"
        << "They do not use Dori.Script, on_start, get_entity, get_component/getComponent, or set_property/setProperty.\n"
        << "Do not put editor property paths such as submeshes[0].material.baseColorFactor inside Lua scripts; use runtime wrappers and APIs from search_engine_api instead.\n";
    return prompt.str();
}

ProviderRequest AiService::buildRequestSnapshotLocked() const {
    ProviderRequest request;
    request.settings = settings;
    request.apiKey = SecretStore::getApiKey(settings.provider);
    request.messages = messages;
    request.tools = EditorActionRegistry::tools();
    request.systemPrompt = buildSystemPrompt();
    return request;
}

void AiService::dispatchRequest(ProviderRequest request) {
    // dispatchRequest only runs when no request is in flight, but the previous
    // worker may have finished without being reaped yet (update() usually joins
    // it). Join it here before reassigning, otherwise the std::thread move-assign
    // onto a still-joinable handle would call std::terminate.
    if (worker.joinable()) {
        worker.join();
    }
    cancelRequested.store(false);
    busy.store(true);
    worker = std::thread(&AiService::runProviderRequest, this, std::move(request));
}

bool AiService::needsContinuationLocked() const {
    // Continue only when the conversation ends on tool results the model has
    // not yet responded to, and nothing is still awaiting user approval.
    if (messages.empty() || messages.back().role != ChatRole::Tool) {
        return false;
    }
    for (const auto& proposal : proposals) {
        if (!proposal.executed) {
            return false;
        }
    }
    return true;
}

void AiService::runProviderRequest(ProviderRequest request) {
    if (request.apiKey.empty()) {
        std::lock_guard<std::mutex> lock(mutex);
        appendAssistantMessageLocked("No API key set for " + toString(request.settings.provider) +
                                     ". Open AI settings to add one before sending requests.");
        busy.store(false);
        return;
    }

    ProviderResponse parsed;
    try {
        std::unique_ptr<Provider> provider = createProvider(request.settings.provider);
        HttpRequest httpRequest = provider->buildRequest(request);
        HttpResponse httpResponse = httpClient.send(httpRequest, &cancelRequested);

        if (cancelRequested.load()) {
            std::lock_guard<std::mutex> lock(mutex);
            appendAssistantMessageLocked("Request cancelled.");
            busy.store(false);
            return;
        }

        if (!httpResponse.error.empty()) {
            parsed.error = "Connection error: " + httpResponse.error;
        } else if (httpResponse.status < 200 || httpResponse.status >= 300) {
            // Reuse the provider parser to pull error.message out of the body.
            ProviderResponse errorBody = provider->parseResponse(httpResponse.body);
            parsed.error = humanizeProviderError(httpResponse.status, errorBody.error);
        } else {
            parsed = provider->parseResponse(httpResponse.body);
        }
    } catch (const std::exception& e) {
        parsed.error = e.what();
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!parsed.error.empty()) {
            appendAssistantMessageLocked(parsed.error);
        } else if (parsed.text.empty() && parsed.toolCalls.empty()) {
            appendAssistantMessageLocked("The provider returned no text or tool calls.");
        } else {
            ChatMessage assistant;
            assistant.role = ChatRole::Assistant;
            assistant.content = parsed.text;
            assistant.toolCalls = parsed.toolCalls;
            messages.push_back(assistant);
            for (const ToolCall& call : parsed.toolCalls) {
                addToolCallProposalLocked(call);
            }
        }
    }

    busy.store(false);
}

void AiService::appendAssistantMessageLocked(const std::string& text) {
    messages.push_back({ChatRole::Assistant, text});
}

void AiService::addToolCallProposalLocked(const ToolCall& call) {
    ActionProposal proposal;
    proposal.id = nextProposalId++;
    proposal.toolCallId = call.id;
    proposal.toolName = call.name;
    proposal.arguments = call.arguments;
    proposal.readOnly = EditorActionRegistry::isReadOnly(call.name);
    proposal.description = EditorActionRegistry::describe(call.name, call.arguments);

    ValidationResult validation = EditorActionRegistry::validate(call.name, call.arguments);
    if (!validation.ok) {
        // Resolve invalid calls immediately and feed the error back so the
        // tool round-trip stays well-formed and the model can recover.
        proposal.executed = true;
        proposal.result = {false, validation.error, Json::object()};

        ChatMessage toolMessage;
        toolMessage.role = ChatRole::Tool;
        toolMessage.toolCallId = call.id;
        toolMessage.toolName = call.name;
        toolMessage.toolSuccess = false;
        toolMessage.content = "Error: " + validation.error;
        messages.push_back(toolMessage);
    }
    proposals.push_back(proposal);
}

} // namespace doriax::editor::ai
