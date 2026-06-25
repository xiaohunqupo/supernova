#include "AiEngineApiContext.h"

#include "engine_api_suggestions.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace doriax::editor::ai {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::string> tokenize(const std::string& value) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return !needle.empty() && haystack.find(needle) != std::string::npos;
}

struct ScoredSymbol {
    int score = 0;
    Json data;
};

std::unordered_map<std::string, std::string> buildInheritanceMap() {
    std::unordered_map<std::string, std::string> inheritance;
    for (const auto& symbol : doriax::editor::getEngineAPISymbols()) {
        const std::string kind = symbol.kind ? symbol.kind : "";
        if (kind != "Class") continue;

        const std::string name = lower(symbol.name ? symbol.name : "");
        const std::string detail = symbol.detail ? symbol.detail : "";
        const size_t basePos = detail.find(" : ");
        if (name.empty() || basePos == std::string::npos) continue;

        inheritance[name] = lower(detail.substr(basePos + 3));
    }
    return inheritance;
}

bool isSameOrBaseClass(const std::string& candidateBase,
                       const std::string& className,
                       const std::unordered_map<std::string, std::string>& inheritance) {
    if (candidateBase.empty() || className.empty()) return false;
    std::string current = className;
    for (int depth = 0; depth < 32; ++depth) {
        if (current == candidateBase) return true;
        auto it = inheritance.find(current);
        if (it == inheritance.end()) break;
        current = it->second;
    }
    return false;
}

std::string luaCallHint(const std::string& detail, const std::string& parent) {
    const std::string detailLower = lower(detail);
    const std::string parentLower = lower(parent);
    if (detailLower.find(":setcolor()") != std::string::npos &&
        (parentLower == "mesh" || parentLower == "shape" || parentLower == "image" ||
         parentLower == "text" || parentLower == "polygon" || parentLower == "skybox")) {
        return "Lua: local shape = Shape(self.scene, self.entity); "
               "shape:setColor(1.0, 0.0, 0.0, 1.0) or shape:setColor(Vector4(1.0, 0.0, 0.0, 1.0)). "
               "C++ cpp_subclass on Mesh: setColor(1.0f, 0.0f, 0.0f, 1.0f) in constructor or onUpdate; include \"Mesh.h\" (not <core/Mesh.h>) and unregister engine events in the destructor.";
    }
    if (parentLower == "shape" && detailLower.find(":setcolor()") == std::string::npos &&
        detailLower.find("shape") != std::string::npos && detailLower.find("constructor") != std::string::npos) {
        return "Construct with Shape(self.scene, self.entity) for an existing entity.";
    }
    return {};
}

} // namespace

Json AiEngineApiContext::search(const std::string& query,
                                const std::string& parentFilter,
                                int maxResults) {
    maxResults = std::max(1, std::min(50, maxResults));
    const std::string queryLower = lower(query);
    const std::string parentLower = lower(parentFilter);
    const std::vector<std::string> tokens = tokenize(query);
    const auto inheritance = buildInheritanceMap();

    std::vector<ScoredSymbol> scored;
    for (const auto& symbol : doriax::editor::getEngineAPISymbols()) {
        const std::string name = symbol.name ? symbol.name : "";
        const std::string kind = symbol.kind ? symbol.kind : "";
        const std::string detail = symbol.detail ? symbol.detail : "";
        const std::string parent = symbol.parent ? symbol.parent : "";

        const std::string nameLower = lower(name);
        const std::string kindLower = lower(kind);
        const std::string detailLower = lower(detail);
        const std::string parentSymbolLower = lower(parent);

        if (!parentLower.empty() &&
            nameLower != parentLower &&
            !isSameOrBaseClass(parentSymbolLower, parentLower, inheritance)) {
            continue;
        }

        int score = 0;
        if (nameLower == queryLower) score += 120;
        if (parentSymbolLower == queryLower) score += 90;
        if (detailLower == queryLower) score += 70;
        if (contains(nameLower, queryLower)) score += 55;
        if (contains(parentSymbolLower, queryLower)) score += 45;
        if (contains(detailLower, queryLower)) score += 30;
        if (contains(kindLower, queryLower)) score += 10;

        int matchedTokens = 0;
        for (const std::string& token : tokens) {
            if (contains(nameLower, token)) {
                score += 18;
                matchedTokens++;
            } else if (contains(parentSymbolLower, token)) {
                score += 14;
                matchedTokens++;
            } else if (isSameOrBaseClass(parentSymbolLower, token, inheritance)) {
                score += 12;
                matchedTokens++;
            } else if (contains(detailLower, token)) {
                score += 10;
                matchedTokens++;
            } else if (contains(kindLower, token)) {
                score += 4;
                matchedTokens++;
            }
        }
        if (!tokens.empty() && matchedTokens == static_cast<int>(tokens.size())) {
            score += 35;
        }
        if (!parentLower.empty()) {
            score += 20;
        }

        if (score <= 0) {
            continue;
        }

        scored.push_back({
            score,
            Json{
                {"name", name},
                {"kind", kind},
                {"parent", parent},
                {"detail", detail}
            }
        });
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredSymbol& a, const ScoredSymbol& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.data.value("detail", "") < b.data.value("detail", "");
    });

    Json results = Json::array();
    for (const ScoredSymbol& item : scored) {
        if (static_cast<int>(results.size()) >= maxResults) break;
        Json entry = item.data;
        entry["score"] = item.score;
        const std::string hint = luaCallHint(entry.value("detail", ""), entry.value("parent", ""));
        if (!hint.empty()) {
            entry["lua_call_hint"] = hint;
        }
        results.push_back(std::move(entry));
    }

    return Json{
        {"source", "engine_api_suggestions.h generated from engine/core Lua bindings and headers"},
        {"query", query},
        {"parent", parentFilter},
        {"results", results},
        {"usage_note", "Lua instance methods use ':' as shown in detail. Construct wrappers with signatures returned here, e.g. Shape(self.scene, self.entity). For color on Mesh/Shape, Lua uses setColor with RGBA floats or Vector4; C++ cpp_subclass scripts inherit Mesh and call setColor(1,0,0,1) with flat headers like \"Mesh.h\". Register engine callbacks in the constructor and unregister them in the destructor. Read lua_call_hint when present."}
    };
}

} // namespace doriax::editor::ai
