#include "CodeEditor.h"

#include "Backend.h"
#include "util/ProjectUtils.h"
#include "command/type/PropertyCmd.h"
#include "Out.h"
#include "yaml-cpp/yaml.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <cctype>

using namespace doriax;

editor::CodeEditor::CodeEditor(Project* project) : isFileChangePopupOpen(false), windowFocused(false), lastFocused(nullptr) {
    this->project = project;
}

editor::CodeEditor::~CodeEditor() {
}

fs::path editor::CodeEditor::resolveFilepath(const fs::path& relPath) const {
    if (relPath.is_absolute()) return relPath;
    return project->getProjectPath() / relPath;
}

std::string editor::CodeEditor::toRelativePath(const std::string& filepath) const {
    fs::path inputPath(filepath);
    if (inputPath.is_relative()) return filepath;
    fs::path projectPath = project->getProjectPath();
    if (!projectPath.empty()) {
        std::error_code ec;
        fs::path relPath = fs::relative(inputPath, projectPath, ec);
        if (!ec && !relPath.empty()) {
            return relPath.string();
        }
    }
    return filepath;
}

bool editor::CodeEditor::loadFileContent(EditorInstance& instance) {
    try {
        fs::path fullPath = resolveFilepath(instance.filepath);
        std::ifstream file(fullPath);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();

            instance.editor->SetText(buffer.str());
            instance.savedUndoIndex = instance.editor->GetUndoIndex();
            instance.lastWriteTime = fs::last_write_time(fullPath);
            instance.isModified = false;

            return true;
        }
    } catch (const std::exception& e) {
        // Handle file access errors
    }
    return false;
}

void editor::CodeEditor::checkFileChanges(EditorInstance& instance) {
    try {
        fs::path fullPath = resolveFilepath(instance.filepath);
        auto currentWriteTime = fs::last_write_time(fullPath);
        if (currentWriteTime != instance.lastWriteTime) {
            // Read the file to check if content actually changed
            std::ifstream file(fullPath);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                file.close();
                if (buffer.str() == instance.editor->GetText()) {
                    // Content is the same (e.g. we just saved), update timestamp only
                    instance.lastWriteTime = currentWriteTime;
                    return;
                }
            }
            if (instance.isModified) {
                // Check if this file is already in the queue
                auto it = std::find_if(changedFilesQueue.begin(), changedFilesQueue.end(),
                    [&](const PendingFileChange& change) {
                        return change.filepath == instance.filepath;
                    });

                // Only add to queue if not already present
                if (it == changedFilesQueue.end()) {
                    changedFilesQueue.push_back({instance.filepath, currentWriteTime});
                }
            } else {
                // If no unsaved changes, silently reload the file
                loadFileContent(instance);
                updateScriptProperties(instance);
            }
        }
    } catch (const std::exception& e) {
        // Handle file access errors
    }
}

void editor::CodeEditor::handleFileChangePopup() {
    if (changedFilesQueue.empty()) {
        return;
    }

    if (!isFileChangePopupOpen) {
        ImGui::OpenPopup("Files Changed###FilesChanged");
        isFileChangePopupOpen = true;
    }

    // Center popup
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Files Changed###FilesChanged", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (changedFilesQueue.size() == 1) {
            ImGui::Text("The file '%s' has been modified externally.", 
                changedFilesQueue[0].filepath.filename().string().c_str());
        } else {
            ImGui::Text("%zu files have been modified externally:", changedFilesQueue.size());

            // Display up to 10 files
            int fileCount = 0;
            for (const auto& change : changedFilesQueue) {
                if (fileCount < 10) {
                    ImGui::BulletText("%s", change.filepath.filename().string().c_str());
                }
                fileCount++;
            }

            // If there are more files, show a message
            if (fileCount > 10) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "And %zu more files...", changedFilesQueue.size() - 10);
            }
        }

        ImGui::Text("Do you want to reload the modified files?");
        ImGui::Text("Warning: You have unsaved changes that will be lost.");
        ImGui::Separator();

        float buttonWidth = 120.0f;
        float windowWidth = ImGui::GetWindowSize().x;

        ImGui::SetCursorPosX((windowWidth - buttonWidth * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

        if (ImGui::Button("Yes", ImVec2(buttonWidth, 0))) {
            // Reload all files in the queue
            for (const auto& change : changedFilesQueue) {
                auto it = editors.find(change.filepath.string());
                if (it != editors.end()) {
                    loadFileContent(it->second);
                    updateScriptProperties(it->second);
                }
            }
            changedFilesQueue.clear();
            isFileChangePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(buttonWidth, 0))) {
            // Update timestamps without reloading
            for (const auto& change : changedFilesQueue) {
                auto it = editors.find(change.filepath.string());
                if (it != editors.end()) {
                    it->second.lastWriteTime = change.newWriteTime;
                }
            }
            changedFilesQueue.clear();
            isFileChangePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        isFileChangePopupOpen = false;
    }
}

std::string editor::CodeEditor::getWindowTitle(const EditorInstance& instance) const {
    std::string filename = instance.filepath.filename().string();
    return filename + (instance.isModified ? " *" : "") + "###" + instance.filepath.string();
}

void editor::CodeEditor::updateScriptProperties(const EditorInstance& instance){
    // Update script properties if this is a script file
    for (auto& sceneProject : project->getScenes()) {
        if (!sceneProject.scene)
            continue;

        for (Entity entity : sceneProject.entities) {
            ScriptComponent* scriptComponent = sceneProject.scene->findComponent<ScriptComponent>(entity);
            if (!scriptComponent)
                continue;

            bool found = false;
            // Check all scripts in the component
            for (const auto& scriptEntry : scriptComponent->scripts) {
                bool isCppScript =
                    (scriptEntry.type == ScriptType::SUBCLASS ||
                    scriptEntry.type == ScriptType::SCRIPT_CLASS);

                bool isLuaScript =
                    (scriptEntry.type == ScriptType::SCRIPT_LUA);

                // For C++ scripts we compare headerPath, for Lua we compare .lua path
                // instance.filepath is already project-relative
                bool matchesFile = false;
                if (isCppScript && !scriptEntry.headerPath.empty()) {
                    matchesFile = (scriptEntry.headerPath == instance.filepath.string());
                } else if (isLuaScript && !scriptEntry.path.empty()) {
                    matchesFile = (scriptEntry.path == instance.filepath.string());
                }

                if (matchesFile) {
                    std::vector<ScriptEntry> newScripts = scriptComponent->scripts;

                    project->updateScriptProperties(&sceneProject, entity, newScripts);
                    PropertyCmd<std::vector<ScriptEntry>> propertyCmd(project, sceneProject.id, entity, ComponentType::ScriptComponent, "scripts", newScripts);
                    propertyCmd.execute();

                    found = true;
                    break; // Found matching script, no need to check others
                }
            }
            if (found)
            break; // No need to scan more entities in this scene
        }
    }
}

void editor::CodeEditor::insertLuaEntityProperty(EditorInstance& instance, Entity entity, uint32_t entitySceneId) {
    // Resolve the source scene
    SceneProject* sourceSceneProject = project->getScene(entitySceneId);
    if (!sourceSceneProject || !sourceSceneProject->scene) {
        sourceSceneProject = project->getSelectedScene();
    }
    if (!sourceSceneProject || !sourceSceneProject->scene) return;

    Scene* scene = sourceSceneProject->scene;
    if (!scene->isEntityCreated(entity)) return;

    // Get entity info
    std::string entityName = scene->getEntityName(entity);
    std::string entityType = editor::ProjectUtils::getEntityTypeName(scene, entity);

    // Check if the entity has a Lua script — use its className as a more specific type
    ScriptComponent* entityScriptComp = scene->findComponent<ScriptComponent>(entity);
    if (entityScriptComp) {
        for (const auto& se : entityScriptComp->scripts) {
            if (se.type == ScriptType::SCRIPT_LUA && se.enabled && !se.className.empty()) {
                entityType = se.className;
                break;
            }
        }
    }

    // Generate variable name: sanitize entity name to camelCase
    std::string varName;
    bool nextUpper = false;
    for (char c : entityName) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            if (varName.empty()) {
                varName += (char)std::tolower(static_cast<unsigned char>(c));
            } else if (nextUpper) {
                varName += (char)std::toupper(static_cast<unsigned char>(c));
                nextUpper = false;
            } else {
                varName += c;
            }
        } else {
            if (!varName.empty()) nextUpper = true;
        }
    }
    if (varName.empty()) varName = "entity";

    // Avoid duplicate property names: check existing text for same variable name
    std::string text = instance.editor->GetText();
    {
        std::string baseName = varName;
        int suffix = 2;
        while (text.find("name = \"" + varName + "\"") != std::string::npos) {
            varName = baseName + std::to_string(suffix++);
        }
    }

    // Generate display name from varName
    std::string displayName;
    for (size_t i = 0; i < varName.size(); i++) {
        if (i == 0) {
            displayName += (char)std::toupper(static_cast<unsigned char>(varName[i]));
        } else if (std::isupper(static_cast<unsigned char>(varName[i]))) {
            displayName += ' ';
            displayName += varName[i];
        } else {
            displayName += varName[i];
        }
    }

    // Build the Lua property entry text
    std::string propEntry =
        "        {\n"
        "            name = \"" + varName + "\",\n"
        "            displayName = \"" + displayName + "\",\n"
        "            type = \"" + entityType + "\",\n"
        "            default = nil\n"
        "        }";

    // Find the properties table and insert at the end
    // Look for "properties = {" or "properties={" pattern
    std::regex propsRegex(R"(properties\s*=\s*\{)");
    std::smatch match;
    if (!std::regex_search(text, match, propsRegex)) {
        Out::error("Could not find 'properties = {' table in Lua script");
        return;
    }

    // Find the matching closing brace by counting braces
    size_t openPos = match.position() + match.length();
    int depth = 1;
    size_t closePos = std::string::npos;
    for (size_t i = openPos; i < text.size(); i++) {
        if (text[i] == '{') depth++;
        else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                closePos = i;
                break;
            }
        }
    }
    if (closePos == std::string::npos) {
        Out::error("Could not find closing '}' for properties table");
        return;
    }

    // Check if there are existing entries (look for '{' between open and close)
    bool hasEntries = false;
    for (size_t i = openPos; i < closePos; i++) {
        if (text[i] == '{') { hasEntries = true; break; }
    }

    // Build insertion text
    std::string insertion;
    if (hasEntries) {
        insertion = ",\n" + propEntry;
    } else {
        insertion = "\n" + propEntry + "\n    ";
    }

    // Insert before the closing brace of properties table
    std::string newText = text.substr(0, closePos) + insertion + text.substr(closePos);

    // Update editor content
    instance.editor->SetText(newText);

    // Save the file (triggers updateScriptProperties which re-parses the Lua file)
    save(instance);

    // Now link the entity to the newly created property
    SceneProject* selectedScene = project->getSelectedScene();
    if (!selectedScene || !selectedScene->scene) return;

    uint32_t storedSceneId = (entitySceneId != selectedScene->id) ? entitySceneId : 0;

    for (Entity sceneEntity : selectedScene->entities) {
        ScriptComponent* scriptComp = selectedScene->scene->findComponent<ScriptComponent>(sceneEntity);
        if (!scriptComp) continue;

        for (size_t si = 0; si < scriptComp->scripts.size(); si++) {
            auto& scriptEntry = scriptComp->scripts[si];
            if (scriptEntry.type != ScriptType::SCRIPT_LUA) continue;
            if (scriptEntry.path != instance.filepath.string()) continue;

            // Find the newly added property by name
            for (size_t pi = 0; pi < scriptEntry.properties.size(); pi++) {
                if (scriptEntry.properties[pi].name == varName) {
                    // Set EntityReference value
                    std::vector<ScriptEntry> newScripts = scriptComp->scripts;
                    newScripts[si].properties[pi].value = EntityReference{entity, storedSceneId};

                    PropertyCmd<std::vector<ScriptEntry>> propCmd(
                        project, selectedScene->id, sceneEntity,
                        ComponentType::ScriptComponent, "scripts", newScripts);
                    propCmd.execute();
                    return;
                }
            }
        }
    }
}

void editor::CodeEditor::insertCppEntityProperty(EditorInstance& instance, Entity entity, uint32_t entitySceneId) {
    // Resolve the source scene
    SceneProject* sourceSceneProject = project->getScene(entitySceneId);
    if (!sourceSceneProject || !sourceSceneProject->scene) {
        sourceSceneProject = project->getSelectedScene();
    }
    if (!sourceSceneProject || !sourceSceneProject->scene) return;

    Scene* scene = sourceSceneProject->scene;
    if (!scene->isEntityCreated(entity)) return;

    // Get entity info
    std::string entityName = scene->getEntityName(entity);
    std::string entityType = editor::ProjectUtils::getEntityTypeName(scene, entity);

    // Check if the entity has a C++ SUBCLASS script — use its class name as a more specific type
    bool isSubclassType = false;
    std::string subclassHeaderFile;
    ScriptComponent* entityScriptComp = scene->findComponent<ScriptComponent>(entity);
    if (entityScriptComp) {
        for (const auto& se : entityScriptComp->scripts) {
            if (se.type == ScriptType::SUBCLASS && !se.className.empty()) {
                entityType = se.className;
                isSubclassType = true;
                if (!se.headerPath.empty()) {
                    subclassHeaderFile = fs::path(se.headerPath).filename().string();
                }
                break;
            }
        }
    }

    // Generate variable name: sanitize entity name to camelCase
    std::string varName;
    bool nextUpper = false;
    for (char c : entityName) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            if (varName.empty()) {
                varName += (char)std::tolower(static_cast<unsigned char>(c));
            } else if (nextUpper) {
                varName += (char)std::toupper(static_cast<unsigned char>(c));
                nextUpper = false;
            } else {
                varName += c;
            }
        } else {
            if (!varName.empty()) nextUpper = true;
        }
    }
    if (varName.empty()) varName = "entity";

    // Determine header path: if currently viewing .cpp, derive .h path
    fs::path headerPath = instance.filepath;
    std::string ext = headerPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    bool isSource = (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c");
    if (isSource) {
        headerPath.replace_extension(".h");
    }

    // Get header text: either from open editor or from disk
    std::string headerText;
    EditorInstance* headerInstance = nullptr;
    std::string headerKey = headerPath.string();
    auto headerIt = editors.find(headerKey);
    if (headerIt != editors.end()) {
        headerInstance = &headerIt->second;
        headerText = headerInstance->editor->GetText();
    } else {
        // Read from disk
        fs::path fullHeaderPath = resolveFilepath(headerPath);
        std::ifstream file(fullHeaderPath);
        if (!file.is_open()) {
            Out::error("Could not open header file: %s", headerPath.string().c_str());
            return;
        }
        headerText = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
    }

    // Avoid duplicate variable names in header
    {
        std::string baseName = varName;
        int suffix = 2;
        // Check for existing "Type* varName" patterns
        while (headerText.find("* " + varName + " ") != std::string::npos ||
               headerText.find("* " + varName + "=") != std::string::npos ||
               headerText.find("*" + varName + " ") != std::string::npos ||
               headerText.find("*" + varName + "=") != std::string::npos) {
            varName = baseName + std::to_string(suffix++);
        }
    }

    // Generate display name from varName (camelCase -> "Camel Case")
    std::string displayName;
    for (size_t i = 0; i < varName.size(); i++) {
        if (i == 0) {
            displayName += (char)std::toupper(static_cast<unsigned char>(varName[i]));
        } else if (std::isupper(static_cast<unsigned char>(varName[i]))) {
            displayName += ' ';
            displayName += varName[i];
        } else {
            displayName += varName[i];
        }
    }

    // Build the SPROPERTY + member declaration
    std::string typeDecl = isSubclassType ? entityType : ("doriax::" + entityType);
    std::string propCode =
        "\n"
        "    SPROPERTY(\"" + displayName + "\")\n"
        "    " + typeDecl + "* " + varName + " = nullptr;\n";

    // If using a subclass type, ensure the header includes it
    if (isSubclassType && !subclassHeaderFile.empty()) {
        std::string includeDirective = "#include \"" + subclassHeaderFile + "\"";
        if (headerText.find(includeDirective) == std::string::npos) {
            // Find the last #include line and insert after it
            size_t lastInclude = std::string::npos;
            size_t searchPos = 0;
            while (true) {
                size_t found = headerText.find("#include", searchPos);
                if (found == std::string::npos) break;
                lastInclude = found;
                searchPos = found + 1;
            }
            if (lastInclude != std::string::npos) {
                size_t endOfLine = headerText.find('\n', lastInclude);
                if (endOfLine != std::string::npos) {
                    headerText.insert(endOfLine + 1, includeDirective + "\n");
                }
            }
        }
    }

    // Find where to insert: before the constructor declaration
    // Look for "ClassName(" that is a constructor (preceded by spaces, on its own line)
    // Strategy: find the last SPROPERTY block, insert after it. If none, insert before constructor.
    size_t insertPos = std::string::npos;

    // Find last SPROPERTY declaration (the variable line after it ends with ";")
    std::regex spropertyRegex(R"(SPROPERTY\s*\([^)]*\)\s*\n\s*[\w:*<>]+\s+\w+\s*=[^;]*;)");
    std::sregex_iterator it(headerText.begin(), headerText.end(), spropertyRegex);
    std::sregex_iterator endIt;
    size_t lastSpropertyEnd = std::string::npos;
    for (; it != endIt; ++it) {
        lastSpropertyEnd = it->position() + it->length();
    }

    if (lastSpropertyEnd != std::string::npos) {
        // Insert after the last SPROPERTY block (skip to next line)
        size_t nextLine = headerText.find('\n', lastSpropertyEnd);
        if (nextLine != std::string::npos) {
            insertPos = nextLine + 1;
        } else {
            insertPos = lastSpropertyEnd;
        }
    }

    if (insertPos == std::string::npos) {
        // Fallback: find constructor pattern "    ClassName(" — a line with identifier followed by (
        // Look for first line with pattern "    SomeName(doriax::" which is the constructor
        std::regex ctorRegex(R"(\n(\s+)\w+\s*\()");
        std::smatch ctorMatch;
        if (std::regex_search(headerText, ctorMatch, ctorRegex)) {
            insertPos = ctorMatch.position() + 1; // After the newline
        }
    }

    if (insertPos == std::string::npos) {
        Out::error("Could not find insertion point in header file");
        return;
    }

    // Ensure there's a blank line before the new property if needed
    std::string newHeaderText = headerText.substr(0, insertPos) + propCode + headerText.substr(insertPos);

    // Update header: either editor or disk
    if (headerInstance) {
        headerInstance->editor->SetText(newHeaderText);
        save(*headerInstance);
    } else {
        // Write directly to disk
        fs::path fullHeaderPath = resolveFilepath(headerPath);
        std::ofstream file(fullHeaderPath, std::ios::trunc);
        if (!file.is_open()) {
            Out::error("Could not write header file: %s", headerPath.string().c_str());
            return;
        }
        file << newHeaderText;
        file.close();

        // Trigger property update by notifying the project
        // We need to create a temporary instance-like context for updateScriptProperties
        // Instead, just call project->updateScriptProperties for matching scripts
    }

    // If we're viewing the source file, also reload it if it changed externally
    if (isSource && headerInstance == nullptr) {
        // The header was written to disk; if there's an open editor for it, reload
        // (already handled above since we checked editors.find)
    }

    // Now link the entity to the newly created property
    SceneProject* selectedScene = project->getSelectedScene();
    if (!selectedScene || !selectedScene->scene) return;

    uint32_t storedSceneId = (entitySceneId != selectedScene->id) ? entitySceneId : 0;

    // First, trigger updateScriptProperties for all entities referencing this header
    for (auto& sceneProject : project->getScenes()) {
        if (!sceneProject.scene) continue;

        for (Entity sceneEntity : sceneProject.entities) {
            ScriptComponent* scriptComp = sceneProject.scene->findComponent<ScriptComponent>(sceneEntity);
            if (!scriptComp) continue;

            for (size_t si = 0; si < scriptComp->scripts.size(); si++) {
                auto& scriptEntry = scriptComp->scripts[si];
                if (scriptEntry.type != ScriptType::SUBCLASS && scriptEntry.type != ScriptType::SCRIPT_CLASS) continue;
                if (scriptEntry.headerPath != headerPath.string()) continue;

                // Update properties from the modified header
                std::vector<ScriptEntry> newScripts = scriptComp->scripts;
                project->updateScriptProperties(&sceneProject, sceneEntity, newScripts);

                // Find the newly added property by name and set EntityReference
                for (size_t nsi = 0; nsi < newScripts.size(); nsi++) {
                    if (newScripts[nsi].headerPath != headerPath.string()) continue;
                    for (size_t pi = 0; pi < newScripts[nsi].properties.size(); pi++) {
                        if (newScripts[nsi].properties[pi].name == varName) {
                            newScripts[nsi].properties[pi].value = EntityReference{entity, storedSceneId};
                            break;
                        }
                    }
                }

                PropertyCmd<std::vector<ScriptEntry>> propCmd(
                    project, sceneProject.id, sceneEntity,
                    ComponentType::ScriptComponent, "scripts", newScripts);
                propCmd.execute();
            }
        }
    }
}

std::vector<fs::path> editor::CodeEditor::getOpenPaths() const{
    std::vector<fs::path> openPaths;
    for (auto it = editors.begin(); it != editors.end(); ++it) {
        const auto& instance = it->second;

        openPaths.push_back(instance.filepath);
    }

    return openPaths;
}

bool editor::CodeEditor::isFocused() const {
    return windowFocused;
}

bool editor::CodeEditor::save(EditorInstance& instance) {
    try {
        fs::path fullPath = resolveFilepath(instance.filepath);
        std::ofstream file(fullPath);
        if (!file.is_open()) {
            return false;
        }

        std::string text = instance.editor->GetText();
        file << text;
        file.close();

        instance.savedUndoIndex = instance.editor->GetUndoIndex();
        instance.isModified = false;
        instance.lastWriteTime = fs::last_write_time(fullPath);

        updateScriptProperties(instance);

        return true;
    } catch (const std::exception& e) {
        // Handle file save errors
        return false;
    }
}

void editor::CodeEditor::saveLastFocused(){
    if (lastFocused){
        save(*lastFocused);
    }
}

bool editor::CodeEditor::save(const std::string& filepath) {
    std::string key = toRelativePath(filepath);
    auto it = editors.find(key);
    if (it == editors.end()) {
        return false;
    }

    return save(it->second);
}

void editor::CodeEditor::saveAll() {
    for (auto& [filepath, instance] : editors) {
        if (instance.isModified) {
            save(instance);
        }
    }
}

void editor::CodeEditor::undoLastFocused() {
    if (lastFocused && lastFocused->editor) {
        lastFocused->editor->Undo();
        lastFocused->isModified = lastFocused->editor->GetUndoIndex() != lastFocused->savedUndoIndex;
    }
}

void editor::CodeEditor::redoLastFocused() {
    if (lastFocused && lastFocused->editor) {
        lastFocused->editor->Redo();
        lastFocused->isModified = lastFocused->editor->GetUndoIndex() != lastFocused->savedUndoIndex;
    }
}

bool editor::CodeEditor::canUndoLastFocused() const {
    return lastFocused && lastFocused->editor && lastFocused->editor->CanUndo();
}

bool editor::CodeEditor::canRedoLastFocused() const {
    return lastFocused && lastFocused->editor && lastFocused->editor->CanRedo();
}

bool editor::CodeEditor::hasUnsavedChanges() const {
    for (const auto& [filepath, instance] : editors) {
        if (instance.isModified) {
            return true;
        }
    }
    return false;
}

bool editor::CodeEditor::hasLastFocusedUnsavedChanges() const {
    if (lastFocused){
        return lastFocused->isModified;
    }
    return false;
}

void editor::CodeEditor::openFile(const std::string& filepath) {
    std::string key = toRelativePath(filepath);

    auto it = editors.find(key);
    if (it != editors.end()) {
        // File already open - set this window as focused for the next frame
        ImGui::SetWindowFocus(getWindowTitle(it->second).c_str());
        return;
    }

    auto& instance = editors[key];
    instance.filepath = key;
    instance.editor = std::make_unique<CustomTextEditor>();

    // Detect language from extension and filename
    std::string ext = instance.filepath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    std::string filename = instance.filepath.filename().string();

    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" ||
        ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".h") {
        instance.languageType = SyntaxLanguage::Cpp;
        instance.editor->SetLanguage(SyntaxLanguage::Cpp);
    } else if (ext == ".lua") {
        instance.languageType = SyntaxLanguage::Lua;
        instance.editor->SetLanguage(SyntaxLanguage::Lua);
    } else if (ext == ".cmake" || filename == "CMakeLists.txt") {
        instance.languageType = SyntaxLanguage::CMake;
        instance.editor->SetLanguage(SyntaxLanguage::CMake);
    } else {
        // Default to plain text for all other files
        instance.languageType = SyntaxLanguage::None;
        instance.editor->SetLanguage(SyntaxLanguage::None);
    }

    instance.editor->SetTabSize(4);
    instance.editor->SetAutoIndent(true);
    instance.editor->SetHighlightCurrentLine(true);
    instance.editor->SetMatchBrackets(true);
    instance.editor->SetAutoComplete(true);

    // Load the file content
    if (!loadFileContent(instance)) {
        // If loading fails, remove the instance
        if (lastFocused == &instance) {
            lastFocused = nullptr;
        }
        editors.erase(key);
        return;
    }

    project->addTab(TabType::CODE_EDITOR, key);

    Backend::getApp().addNewCodeWindowToDock(instance.filepath);
}

void editor::CodeEditor::closeFile(const std::string& filepath) {
    std::string key = toRelativePath(filepath);
    if (auto it = editors.find(key); it != editors.end()) {
        if (lastFocused == &it->second) {
            lastFocused = nullptr;
        }

        project->removeTab(TabType::CODE_EDITOR, key);

        editors.erase(it);
    }
}

bool editor::CodeEditor::isFileOpen(const std::string& filepath) const {
    std::string key = toRelativePath(filepath);
    return editors.find(key) != editors.end();
}

void editor::CodeEditor::setText(const std::string& filepath, const std::string& text) {
    std::string key = toRelativePath(filepath);
    if (auto it = editors.find(key); it != editors.end()) {
        it->second.editor->SetText(text);
        it->second.savedUndoIndex = it->second.editor->GetUndoIndex();
        it->second.isModified = false;
        try {
            it->second.lastWriteTime = fs::last_write_time(resolveFilepath(key));
        } catch (const std::exception& e) {
            // Handle file access errors
        }
    }
}

std::string editor::CodeEditor::getText(const std::string& filepath) const {
    std::string key = toRelativePath(filepath);
    if (auto it = editors.find(key); it != editors.end()) {
        return it->second.editor->GetText();
    }
    return "";
}

bool editor::CodeEditor::handleFileRename(const fs::path& oldPath, const fs::path& newPath) {
    std::string oldKey = toRelativePath(oldPath.string());
    std::string newKey = toRelativePath(newPath.string());

    auto it = editors.find(oldKey);
    if (it == editors.end()) {
        return false;
    }

    // Create new instance with the same editor content
    EditorInstance newInstance;
    newInstance.editor = std::move(it->second.editor);
    newInstance.isOpen = it->second.isOpen;
    newInstance.filepath = newKey;
    newInstance.isModified = it->second.isModified;
    newInstance.lastCheckTime = it->second.lastCheckTime;
    newInstance.hasExternalChanges = it->second.hasExternalChanges;
    newInstance.savedUndoIndex = it->second.savedUndoIndex;

    try {
        newInstance.lastWriteTime = fs::last_write_time(resolveFilepath(newKey));
    } catch (const std::exception& e) {
        return false;
    }

    // Update lastFocused pointer if it was pointing to the old instance
    if (lastFocused == &it->second) {
        editors[newKey] = std::move(newInstance);
        lastFocused = &editors[newKey];
        editors.erase(it);
    } else {
        editors.erase(it);
        editors[newKey] = std::move(newInstance);
    }

    // Update any pending file changes
    auto changeIt = std::remove_if(changedFilesQueue.begin(), changedFilesQueue.end(),
        [&](const PendingFileChange& change) {
            return change.filepath == fs::path(oldKey);
        });
    changedFilesQueue.erase(changeIt, changedFilesQueue.end());

    project->removeTab(TabType::CODE_EDITOR, oldKey);
    project->addTab(TabType::CODE_EDITOR, newKey);

    Backend::getApp().addNewCodeWindowToDock(fs::path(newKey));

    return true;
}

void editor::CodeEditor::show() {
    // Get current time
    double currentTime = ImGui::GetTime();

    windowFocused = false;

    // Iterate through all open editors
    for (auto it = editors.begin(); it != editors.end();) {
        auto& instance = it->second;

        // Check for external file changes every second
        if (currentTime - instance.lastCheckTime >= 1.0) {
            checkFileChanges(instance);
            instance.lastCheckTime = currentTime;
        }

        // Create window title using filename
        std::string windowTitle = getWindowTitle(instance);

        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(windowTitle.c_str(), &instance.isOpen)) {

            instance.isModified = instance.editor->GetUndoIndex() != instance.savedUndoIndex;

            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                windowFocused = true;

                lastFocused = &instance;
            }

            int line, column;
            instance.editor->GetCursorPosition(line, column);

            ImGui::Text("%6d/%-6d %6d lines  | %s | %s", 
                line + 1, 
                column + 1,
                instance.editor->GetLineCount(),
                instance.editor->GetLanguageName(),
                instance.isModified ? "*" : " ");

            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // font needs to be monospace
            instance.editor->Render("TextEditor");
            ImGui::PopFont();

            // Accept entity drag-drop onto Lua and C++ script editors
            if ((instance.languageType == SyntaxLanguage::Lua || instance.languageType == SyntaxLanguage::Cpp) && ImGui::BeginDragDropTarget()) {
                // Check if this is a C++ source file (not a header)
                bool isCppSource = false;
                if (instance.languageType == SyntaxLanguage::Cpp) {
                    std::string dropExt = instance.filepath.extension().string();
                    std::transform(dropExt.begin(), dropExt.end(), dropExt.begin(), ::tolower);
                    isCppSource = (dropExt != ".h" && dropExt != ".hpp" && dropExt != ".hh" && dropExt != ".hxx");
                }

                // Single entity
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity")) {
                    if (isCppSource) {
                        Backend::getApp().registerAlert("Drop Entity", "Entity properties must be added to the header file (.h), not the source file (.cpp).");
                    } else if (payload->DataSize >= sizeof(EntityPayload)) {
                        const EntityPayload* ep = static_cast<const EntityPayload*>(payload->Data);
                        if (instance.languageType == SyntaxLanguage::Lua)
                            insertLuaEntityProperty(instance, ep->entity, ep->entitySceneId);
                        else
                            insertCppEntityProperty(instance, ep->entity, ep->entitySceneId);
                    }
                }
                // Bundle (multiple entities) — use first entity
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("bundle")) {
                    if (isCppSource) {
                        Backend::getApp().registerAlert("Drop Entity", "Entity properties must be added to the header file (.h), not the source file (.cpp).");
                    } else {
                        try {
                            std::string yamlStr(static_cast<const char*>(payload->Data), payload->DataSize);
                            YAML::Node bundleNode = YAML::Load(yamlStr);
                            if (bundleNode["members"] && bundleNode["members"].IsSequence() && bundleNode["members"].size() > 0) {
                                Entity firstEntity = bundleNode["members"][0]["entity"].as<Entity>();
                                SceneProject* sp = project->getSelectedScene();
                                uint32_t sceneId = sp ? sp->id : 0;
                                if (instance.languageType == SyntaxLanguage::Lua)
                                    insertLuaEntityProperty(instance, firstEntity, sceneId);
                                else
                                    insertCppEntityProperty(instance, firstEntity, sceneId);
                            }
                        } catch (...) {}
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::End();

        // If the window was closed, remove it from our editors map
        if (!instance.isOpen) {
            if (lastFocused == &instance) {
                lastFocused = nullptr;
            }

            project->removeTab(TabType::CODE_EDITOR, instance.filepath.string());

            it = editors.erase(it);
        } else {
            ++it;
        }
    }

    // Handle file change popup after all windows are rendered
    handleFileChangePopup();
}