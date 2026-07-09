#include "ConversationStore.h"

#include "Project.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>

namespace fs = std::filesystem;

namespace doriax::editor::ai {

namespace {

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Json messageToJson(const ChatMessage& message) {
    Json out;
    out["role"] = toString(message.role);
    out["content"] = message.content;
    if (!message.toolCallId.empty()) out["tool_call_id"] = message.toolCallId;
    if (!message.toolName.empty()) out["tool_name"] = message.toolName;
    if (!message.toolDescription.empty()) out["tool_description"] = message.toolDescription;
    if (message.role == ChatRole::Tool) out["tool_success"] = message.toolSuccess;
    if (!message.toolCalls.empty()) {
        Json calls = Json::array();
        for (const ToolCall& call : message.toolCalls) {
            Json entry = {{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}};
            if (!call.thoughtSignature.empty()) {
                entry["thought_signature"] = call.thoughtSignature;
            }
            calls.push_back(entry);
        }
        out["tool_calls"] = calls;
    }
    if (!message.thinkingBlocks.empty()) {
        out["thinking_blocks"] = message.thinkingBlocks;
    }
    return out;
}

ChatMessage messageFromJson(const Json& node) {
    ChatMessage message;
    message.role = chatRoleFromString(node.value("role", "user"));
    message.content = node.value("content", "");
    message.toolCallId = node.value("tool_call_id", "");
    message.toolName = node.value("tool_name", "");
    message.toolDescription = node.value("tool_description", "");
    message.toolSuccess = node.value("tool_success", true);
    if (node.contains("tool_calls") && node["tool_calls"].is_array()) {
        for (const Json& callNode : node["tool_calls"]) {
            ToolCall call;
            call.id = callNode.value("id", "");
            call.name = callNode.value("name", "");
            call.arguments = callNode.value("arguments", Json::object());
            call.thoughtSignature = callNode.value("thought_signature",
                callNode.value("thoughtSignature", ""));
            message.toolCalls.push_back(call);
        }
    }
    if (node.contains("thinking_blocks") && node["thinking_blocks"].is_array()) {
        for (const Json& block : node["thinking_blocks"]) {
            if (block.is_object()) {
                message.thinkingBlocks.push_back(block);
            }
        }
    }
    return message;
}

} // namespace

ConversationStore::ConversationStore(Project* project)
    : project(project) {
}

fs::path ConversationStore::conversationsDir() const {
    return project->getProjectInternalPath() / "ai" / "conversations";
}

std::string ConversationStore::newId() {
    static std::atomic<uint64_t> counter{0};
    return std::to_string(nowSeconds()) + "-" + std::to_string(counter.fetch_add(1));
}

void ConversationStore::save(const std::string& id, const std::string& title,
                             const std::vector<ChatMessage>& messages) {
    if (id.empty()) return;

    fs::path dir = conversationsDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return;

    Json root;
    root["id"] = id;
    root["title"] = title;
    root["updated_at"] = nowSeconds();
    root["messages"] = Json::array();
    for (const ChatMessage& message : messages) {
        root["messages"].push_back(messageToJson(message));
    }

    std::ofstream out(dir / (id + ".json"), std::ios::trunc);
    if (out) {
        out << root.dump(2);
    }
}

bool ConversationStore::load(const std::string& id, std::vector<ChatMessage>& outMessages) const {
    std::ifstream in(conversationsDir() / (id + ".json"));
    if (!in) return false;

    Json root = Json::parse(in, nullptr, false);
    if (!root.is_object() || !root.contains("messages") || !root["messages"].is_array()) {
        return false;
    }

    outMessages.clear();
    for (const Json& node : root["messages"]) {
        outMessages.push_back(messageFromJson(node));
    }
    return true;
}

std::vector<ConversationMeta> ConversationStore::list() const {
    std::vector<ConversationMeta> result;
    fs::path dir = conversationsDir();
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return result;
    }

    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") {
            continue;
        }
        std::ifstream in(it->path());
        if (!in) continue;
        Json root = Json::parse(in, nullptr, false);
        if (!root.is_object()) continue;

        ConversationMeta meta;
        meta.id = root.value("id", it->path().stem().string());
        meta.title = root.value("title", std::string("Conversation"));
        meta.updatedAt = root.value("updated_at", static_cast<int64_t>(0));
        result.push_back(std::move(meta));
    }

    std::sort(result.begin(), result.end(), [](const ConversationMeta& a, const ConversationMeta& b) {
        return a.updatedAt > b.updatedAt;
    });
    return result;
}

void ConversationStore::remove(const std::string& id) {
    if (id.empty()) return;
    std::error_code ec;
    fs::remove(conversationsDir() / (id + ".json"), ec);
}

} // namespace doriax::editor::ai
