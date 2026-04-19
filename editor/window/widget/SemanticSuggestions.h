#pragma once

#include "imgui.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cctype>

namespace doriax::editor {

    enum class SuggestionKind {
        Keyword,
        Type,
        Function,
        Variable,
        Snippet,
        Class,
        Interface,
        Module,
        Property,
        Method,
        Field,
        Constant,
        Enum,
        EnumMember,
        Operator,
        Text
    };

    struct SuggestionItem {
        std::string label;           // Display text
        std::string insertText;      // Text to insert
        std::string detail;          // Additional info (e.g., type signature)
        std::string documentation;   // Full documentation
        std::string parentType;      // The class/type this belongs to (if any)
        std::string typeInfo;        // Declared type (for variables/fields)
        SuggestionKind kind;         // Target kind
        int score = 0;               // Resulting score
    };

    struct SuggestionContext {
        std::string currentWord;     // Word being typed
        std::string lineContent;     // Full line content
        int cursorColumn;            // Cursor position in line
        int lineNumber;              // Current line number
        std::string previousWord;    // Word before current (for context)
        bool afterDot = false;               // Is cursor after a '.' (member access)
        bool afterArrow = false;             // Is cursor after '->' (pointer member access)
        bool afterDoubleColon = false;       // Is cursor after '::' (scope resolution)
        bool afterColon = false;             // Is cursor after ':' (Lua method access)
        std::string targetType;      // Inferred type for member completion filtering
        bool isCpp = false;          // True if editing C++ (filters out Lua-only properties)
    };

    class SemanticSuggestions {
    public:
        SemanticSuggestions();
        ~SemanticSuggestions();

        // Configuration
        void SetMinPrefixLength(int length) { minPrefixLength = length; }
        void SetMaxSuggestions(int max) { maxSuggestions = max; }
        void SetFuzzyMatching(bool enable) { fuzzyMatching = enable; }
        void SetCaseSensitive(bool sensitive) { caseSensitive = sensitive; }

        // Language setup
        void SetKeywords(const std::unordered_set<std::string>& keywords);
        void SetTypes(const std::unordered_set<std::string>& types);
        void SetBuiltinFunctions(const std::unordered_set<std::string>& functions);
        void AddSnippet(const std::string& trigger, const std::string& expansion, const std::string& description);
        void ClearSnippets();

        // Document context
        void UpdateDocumentWords(const std::vector<std::string>& lines);
        void AddSymbol(const std::string& name, SuggestionKind kind, const std::string& detail = "", const std::string& parentType = "", const std::string& typeInfo = "");
        void ClearSymbols();

        // Inheritance
        void SetClassParent(const std::string& className, const std::string& parentClass);
        void ClearInheritance();

        // Type lookup: find the declared type of a symbol by name (searches fields/properties/variables)
        std::string FindSymbolType(const std::string& name) const;

        // Core functionality
        std::vector<SuggestionItem> GetSuggestions(const SuggestionContext& context);

        // UI helpers
        static const char* GetKindIcon(SuggestionKind kind);
        static ImVec4 GetKindColor(SuggestionKind kind);
        static const char* GetKindName(SuggestionKind kind);

    private:
        // Settings
        int minPrefixLength;
        int maxSuggestions;
        bool fuzzyMatching;
        bool caseSensitive;

        // Word sources
        std::unordered_set<std::string> keywords;
        std::unordered_set<std::string> types;
        std::unordered_set<std::string> builtinFunctions;
        std::unordered_set<std::string> documentWords;
        std::vector<SuggestionItem> snippets;
        std::vector<SuggestionItem> symbols;

        // Inheritance map: className -> parentClassName
        std::unordered_map<std::string, std::string> classParentMap;

        // Check if targetType equals typeName or is a descendant of it
        bool isTypeOrAncestor(const std::string& targetType, const std::string& symbolParent) const;

        // Matching
        int calculateMatchScore(const std::string& candidate, const std::string& query) const;
        bool matchesQuery(const std::string& candidate, const std::string& query) const;
        bool fuzzyMatch(const std::string& candidate, const std::string& query, int& outScore) const;
        bool prefixMatch(const std::string& candidate, const std::string& query) const;
        bool substringMatch(const std::string& candidate, const std::string& query) const;

        // Helpers
        std::string toLower(const std::string& str) const;
        bool isWordChar(char c) const;
        void addCandidates(std::vector<SuggestionItem>& results, 
                          const std::unordered_set<std::string>& source,
                          SuggestionKind kind,
                          const std::string& query);
    };

} // namespace doriax::editor
