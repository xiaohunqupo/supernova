#include "ModelCatalog.h"

#include <algorithm>

namespace doriax::editor::ai {

namespace {

constexpr size_t kMaxModels = 60;

bool looksLikeOpenAiChatModel(const std::string& id) {
    // Exclude embeddings/audio/image/moderation endpoints; keep chat families.
    static const char* prefixes[] = {"gpt", "chatgpt", "o1", "o3", "o4"};
    for (const char* prefix : prefixes) {
        if (id.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

bool looksLikeMainGeminiModel(const std::string& id) {
    static const char* mainModels[] = {
        "gemini-3.5-flash",
        "gemini-3.1-flash-lite",
        "gemini-2.5-pro",
        "gemini-2.5-flash",
        "gemini-2.5-flash-lite"
    };
    for (const char* model : mainModels) {
        if (id == model) {
            return true;
        }
    }
    return false;
}

} // namespace

ModelCatalog::ModelCatalog() {
    worker = std::thread(&ModelCatalog::workerLoop, this);
}

ModelCatalog::~ModelCatalog() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
    }
    condition.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

std::vector<ModelInfo> ModelCatalog::models(ProviderId provider) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(provider);
    if (it != cache.end() && it->second.status == CatalogStatus::Loaded && !it->second.models.empty()) {
        return it->second.models;
    }
    return curatedModels(provider);
}

CatalogStatus ModelCatalog::status(ProviderId provider) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(provider);
    return it == cache.end() ? CatalogStatus::Idle : it->second.status;
}

void ModelCatalog::refresh(ProviderId provider, const std::string& apiKey, const std::string& endpoint) {
    if (apiKey.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        Entry& entry = cache[provider];
        if (entry.status == CatalogStatus::Loading) {
            return; // already in flight
        }
        entry.status = CatalogStatus::Loading;
        queue.push_back({provider, apiKey, endpoint});
    }
    condition.notify_one();
}

void ModelCatalog::workerLoop() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this] { return stop || !queue.empty(); });
            if (stop) {
                return;
            }
            request = queue.front();
            queue.pop_front();
        }

        std::vector<ModelInfo> fetched;
        bool ok = fetchModels(request, fetched);

        std::lock_guard<std::mutex> lock(mutex);
        Entry& entry = cache[request.provider];
        if (ok && !fetched.empty()) {
            entry.models = std::move(fetched);
            entry.status = CatalogStatus::Loaded;
        } else {
            entry.status = CatalogStatus::Error;
        }
    }
}

bool ModelCatalog::fetchModels(const Request& request, std::vector<ModelInfo>& out) {
    std::string url;
    std::vector<std::string> headers = {"Accept: application/json"};

    switch (request.provider) {
        case ProviderId::OpenAI:
            url = "https://api.openai.com/v1/models";
            headers.push_back("Authorization: Bearer " + request.apiKey);
            break;
        case ProviderId::Anthropic:
            url = "https://api.anthropic.com/v1/models?limit=100";
            headers.push_back("x-api-key: " + request.apiKey);
            headers.push_back("anthropic-version: 2023-06-01");
            break;
        case ProviderId::Gemini:
            url = "https://generativelanguage.googleapis.com/v1beta/models?pageSize=200&key=" +
                  HttpClient::urlEncode(request.apiKey);
            break;
        case ProviderId::DeepSeek:
            url = "https://api.deepseek.com/models";
            headers.push_back("Authorization: Bearer " + request.apiKey);
            break;
        case ProviderId::OpenAICompatible: {
            std::string base = request.endpoint.empty()
                ? "https://openrouter.ai/api/v1/chat/completions"
                : request.endpoint;
            size_t pos = base.find("/chat/completions");
            url = (pos != std::string::npos) ? base.substr(0, pos) + "/models" : base;
            headers.push_back("Authorization: Bearer " + request.apiKey);
            break;
        }
    }

    HttpResponse response = httpClient.get(url, headers);
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        return false;
    }

    Json root = Json::parse(response.body, nullptr, false);
    if (!root.is_object()) {
        return false;
    }

    if (request.provider == ProviderId::Gemini) {
        if (!root.contains("models") || !root["models"].is_array()) return false;
        for (const Json& model : root["models"]) {
            const Json& methods = model.value("supportedGenerationMethods", Json::array());
            bool generates = false;
            if (methods.is_array()) {
                for (const Json& m : methods) {
                    if (m.is_string() && m.get<std::string>() == "generateContent") { generates = true; break; }
                }
            }
            if (!generates) continue;
            std::string name = model.value("name", "");
            const std::string prefix = "models/";
            if (name.rfind(prefix, 0) == 0) name = name.substr(prefix.size());
            if (name.empty()) continue;
            std::string label = model.value("displayName", name);
            if (!looksLikeMainGeminiModel(name)) continue;
            out.push_back({name, label});
            if (out.size() >= kMaxModels) break;
        }
        return !out.empty();
    }

    // OpenAI, Anthropic and OpenAI-compatible all return { "data": [ ... ] }.
    if (!root.contains("data") || !root["data"].is_array()) {
        return false;
    }
    for (const Json& model : root["data"]) {
        std::string id = model.value("id", "");
        if (id.empty()) continue;
        if (request.provider == ProviderId::OpenAI && !looksLikeOpenAiChatModel(id)) {
            continue;
        }
        std::string label = model.value("display_name", model.value("name", id));
        out.push_back({id, label});
        if (out.size() >= kMaxModels) break;
    }
    return !out.empty();
}

std::vector<ModelInfo> ModelCatalog::curatedModels(ProviderId provider) {
    switch (provider) {
        case ProviderId::Anthropic:
            return {
                {"claude-sonnet-4-20250514", "Claude Sonnet 4"},
                {"claude-3-7-sonnet-20250219", "Claude 3.7 Sonnet"},
                {"claude-3-5-sonnet-20241022", "Claude 3.5 Sonnet"},
                {"claude-3-5-haiku-20241022", "Claude 3.5 Haiku"}
            };
        case ProviderId::Gemini:
            return {
                {"gemini-3.5-flash", "Gemini 3.5 Flash"},
                {"gemini-3.1-flash-lite", "Gemini 3.1 Flash-Lite"},
                {"gemini-2.5-flash", "Gemini 2.5 Flash"},
                {"gemini-2.5-pro", "Gemini 2.5 Pro"},
                {"gemini-2.5-flash-lite", "Gemini 2.5 Flash-Lite"}
            };
        case ProviderId::DeepSeek:
            return {
                {"deepseek-chat", "DeepSeek Chat (V3)"},
                {"deepseek-reasoner", "DeepSeek Reasoner (R1)"}
            };
        case ProviderId::OpenAICompatible:
            return {
                {"openai/gpt-4.1", "OpenAI GPT-4.1"},
                {"openai/gpt-4.1-mini", "OpenAI GPT-4.1 mini"},
                {"anthropic/claude-sonnet-4", "Anthropic Claude Sonnet 4"},
                {"google/gemini-2.5-flash", "Google Gemini 2.5 Flash"},
                {"meta-llama/llama-3.3-70b-instruct", "Llama 3.3 70B Instruct"}
            };
        case ProviderId::OpenAI:
        default:
            return {
                {"gpt-4.1", "GPT-4.1"},
                {"gpt-4.1-mini", "GPT-4.1 mini"},
                {"gpt-4o", "GPT-4o"},
                {"gpt-4o-mini", "GPT-4o mini"},
                {"o3-mini", "o3 mini"}
            };
    }
}

} // namespace doriax::editor::ai
