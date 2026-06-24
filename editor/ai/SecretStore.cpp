#include "SecretStore.h"

namespace doriax::editor::ai {

namespace {
std::mutex g_mutex;
std::map<ProviderId, std::string> g_sessionKeys;
}

void SecretStore::setSessionApiKey(ProviderId provider, const std::string& key) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (key.empty()) {
        g_sessionKeys.erase(provider);
        return;
    }
    g_sessionKeys[provider] = key;
}

std::string SecretStore::getSessionApiKey(ProviderId provider) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sessionKeys.find(provider);
    return it == g_sessionKeys.end() ? std::string() : it->second;
}

bool SecretStore::hasSessionApiKey(ProviderId provider) {
    return !getSessionApiKey(provider).empty();
}

void SecretStore::clearSessionApiKey(ProviderId provider) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sessionKeys.erase(provider);
}

} // namespace doriax::editor::ai
