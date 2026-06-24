#pragma once

#include "AiTypes.h"

#include <map>
#include <mutex>
#include <string>

namespace doriax::editor::ai {

class SecretStore {
public:
    static void setSessionApiKey(ProviderId provider, const std::string& key);
    static std::string getSessionApiKey(ProviderId provider);
    static bool hasSessionApiKey(ProviderId provider);
    static void clearSessionApiKey(ProviderId provider);
};

} // namespace doriax::editor::ai
