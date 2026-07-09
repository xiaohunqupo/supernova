#include "AiService.h"

#include "AiProvider.h"
#include "EditorActionExecutor.h"
#include "EditorActionRegistry.h"
#include "Out.h"
#include "SecretStore.h"

#include <algorithm>
#include <cctype>
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

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool looksLikeOllamaEndpoint(const Settings& settings) {
    if (settings.provider != ProviderId::OpenAICompatible) {
        return false;
    }
    std::string endpoint = lowercase(settings.customEndpoint);
    return endpoint.find("ollama") != std::string::npos ||
           endpoint.find(":11434") != std::string::npos;
}

std::string pluralTokens(int count) {
    return count == 1 ? "token" : "tokens";
}

bool hasTokenUsage(const ProviderResponse& response) {
    return response.promptTokens >= 0 ||
           response.completionTokens >= 0 ||
           response.totalTokens >= 0;
}

bool promptUsedNearlyWholeContext(const ProviderResponse& response) {
    if (response.promptTokens < 0 || response.totalTokens <= 0) {
        return false;
    }
    int remaining = response.totalTokens - response.promptTokens;
    int smallHeadroom = std::max(32, response.totalTokens / 20);
    return remaining >= 0 && remaining <= smallHeadroom;
}

std::string tokenUsageSummary(const ProviderResponse& response) {
    std::ostringstream out;
    bool wrote = false;
    if (response.promptTokens >= 0) {
        out << response.promptTokens << " prompt";
        wrote = true;
    }
    if (response.completionTokens >= 0) {
        if (wrote) out << " + ";
        out << response.completionTokens << " output";
        wrote = true;
    }
    if (response.totalTokens >= 0) {
        if (wrote) out << " = ";
        out << response.totalTokens << " total";
    }
    return out.str();
}

// A context window comfortably above what this request already consumed, rounded
// up to a tidy boundary, so the suggestion leaves room for both prompt and reply.
int suggestContextTokens(const ProviderResponse& response) {
    int used = std::max(response.totalTokens, response.promptTokens);
    int target = used > 0 ? used * 2 : 8192;
    for (int boundary : {8192, 16384, 32768, 65536, 131072}) {
        if (target <= boundary) return boundary;
    }
    return target;
}

// Advice for a token limit that is the provider/model context window rather than
// Doriax's output cap. Ollama gets a concrete num_ctx suggestion because a small
// context window is a common culprit.
std::string contextRemedy(const ProviderRequest& request, const ProviderResponse& response) {
    if (!looksLikeOllamaEndpoint(request.settings)) {
        return "Use a larger-context model or reduce the conversation/project context and try again.";
    }
    int suggested = suggestContextTokens(response);
    std::ostringstream out;
    out << "For Ollama, raise the model context length to at least " << suggested
        << " (\"/set parameter num_ctx " << suggested << "\" in the Ollama session, or "
           "\"PARAMETER num_ctx " << suggested << "\" in the Modelfile)";
    if (response.totalTokens > 0) {
        out << "; this response stopped after " << response.totalTokens << " total tokens";
    }
    out << ". You can also use a larger-context model or reduce the conversation/project context, then try again.";
    return out.str();
}

std::string buildTruncationMessage(const ProviderRequest& request, const ProviderResponse& response) {
    const bool hasCompletionUsage = response.completionTokens >= 0;
    // The model reached (within rounding of) Doriax's own output cap, so the stop
    // is the editor's "Max output tokens" setting, not a provider/context limit.
    const bool reachedDoriaxCap =
        hasCompletionUsage && response.completionTokens + 1 >= request.settings.maxOutputTokens;
    const bool stoppedBeforeDoriaxCap = hasCompletionUsage && !reachedDoriaxCap;
    // Only trust the "context full" reading when the stop was not Doriax's cap;
    // otherwise a small cap plus a largish prompt can look like a full window.
    const bool contextLikelyFull = promptUsedNearlyWholeContext(response) && !reachedDoriaxCap;

    std::ostringstream out;
    if (contextLikelyFull) {
        out << "The provider stopped at a token limit";
        if (!response.stopReason.empty()) {
            out << " (" << response.stopReason << ")";
        }
        if (response.completionTokens >= 0) {
            out << " after " << response.completionTokens << " output "
                << pluralTokens(response.completionTokens);
        }
        out << " because the prompt used nearly the whole context window";
        if (hasTokenUsage(response)) {
            out << " (" << tokenUsageSummary(response) << " tokens)";
        }
        out << ". This is not Doriax's \"Max output tokens\" cap ("
            << request.settings.maxOutputTokens << "). " << contextRemedy(request, response);
        return out.str();
    }

    if (stoppedBeforeDoriaxCap) {
        out << "The provider stopped at its own token limit";
        if (!response.stopReason.empty()) {
            out << " (" << response.stopReason << ")";
        }
        out << " after " << response.completionTokens << " output "
            << pluralTokens(response.completionTokens)
            << ", before Doriax's configured max of "
            << request.settings.maxOutputTokens << ".";
        if (hasTokenUsage(response)) {
            out << " Usage: " << tokenUsageSummary(response) << " tokens.";
        }
        out << " " << contextRemedy(request, response);
        return out.str();
    }

    out << "The response hit the max output token limit ("
        << request.settings.maxOutputTokens
        << ") and was cut off before completing.";
    if (hasTokenUsage(response)) {
        out << " Usage: " << tokenUsageSummary(response) << " tokens.";
    }
    out << " Increase \"Max output tokens\" in AI settings (gear icon) and try again - "
           "writing a whole file such as a shader needs a higher limit.";
    return out.str();
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
        turnActive = true;
        turnFailed = false;
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
    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int maxRounds = std::max(1, settings.maxToolRounds);
        if (!needsContinuationLocked() || toolRounds >= maxRounds) {
            if (needsContinuationLocked() && toolRounds >= maxRounds) {
                appendAssistantMessageLocked(
                    "Reached the tool-step limit for this request. Send another message to continue.");
                finishTurnLocked(false);
            } else if (turnActive && !hasPendingProposalsLocked()) {
                // Agent turn settled (final reply, or waiting is over with no more work).
                finishTurnLocked(!turnFailed);
            }
            return;
        }
        ++toolRounds;
        request = buildRequestSnapshotLocked();
        dispatch = true;
    }
    if (dispatch) {
        dispatchRequest(std::move(request));
    }
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
    turnActive = false;
    turnFailed = false;
}

void AiService::loadConversation(std::vector<ChatMessage> newMessages) {
    if (busy.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    messages = std::move(newMessages);
    proposals.clear();
    toolRounds = 0;
    turnActive = false;
    turnFailed = false;
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
        toolMessage.toolDescription = proposal.description;
        toolMessage.toolSuccess = result.success;
        toolMessage.content = result.success ? result.message : ("Error: " + result.message);
        if (!result.data.empty()) {
            toolMessage.content += "\n" + result.data.dump(2);
        }
        messages.push_back(toolMessage);
    }

    if (!result.success) {
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
        toolMessage.toolDescription = proposal.description;
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
        << "2D scenes have a dedicated lighting model: create_entity light_2d adds a Light2DComponent (radius/falloff point light) and occluder_2d adds an Occluder2DComponent (shadow caster). Light2D adds on top of the scene's 2D ambient (set_scene_property ambient_light_2d_color/ambient_light_2d_intensity), so dim the ambient to make 2D lights visible. Occluder2D shape AUTO_QUAD derives its outline from a sibling mesh; POLYGON uses its own points. Enable a Light2D's shadows property to cast from occluders. Shadow edge smoothness is per scene: shadows_quality (3D) and shadows_2d_quality (2D) take int_value 0=none/1=low/2=medium/3=high. These are separate from the 3D Light/global_illumination path.\n"
        << "After a tool returns, use its result to decide the next step and report concise progress to the user.\n"
        << "Work efficiently within the tool-step budget: batch independent tool calls into a single step instead of one per step, and never repeat a search or read whose result you already have. Gather the API facts you need, then write the file.\n"
        << "Prefer project-relative paths. Never request arbitrary shell commands. Never ask for secrets in chat.\n"
        << "For external assets, use curated sources only and preserve license/author/source attribution.\n"
        << "For scripts and engine API code, the Doriax engine source under the editor's engine/ directory (read it with search_engine_source and read_engine_source) is the ONLY source of truth. Use ONLY classes, methods, properties, enums, macros, and constructor overloads you have confirmed exist in that source; if a symbol is not present there it does not exist in Doriax, so do not use it. Never invent APIs or carry over names, macros, or patterns from other engines or frameworks (e.g. Godot GDCLASS, Unreal GENERATED_BODY/UPROPERTY, Qt Q_OBJECT). search_engine_api is only a quick index into that same source; when a symbol is unfamiliar or you are unsure of its exact spelling or overloads, confirm it in the source before writing it (e.g. key codes are Input.KEY_* in Lua but D_KEY_* macros in C++, and Quaternion's axis-angle constructor takes the angle first: Quaternion(angle, axis)).\n"
        << "When you create a script for requested behavior, write the complete script with update_script_file instead of asking the user to edit it manually.\n"
        << "After creating or editing ANY script (C++ or Lua), verify it before telling the user it is ready: start the scene with control_play_mode (action=start), then call read_output_log to inspect the editor output. A C++ compile error appears as a build failure with compiler messages; a Lua or C++ runtime error appears as a 'Script crash in scene ...' entry. If the scene is still building, read_output_log again. If you find errors, stop play with control_play_mode (action=stop), fix the script with update_script_file, and verify again -- repeat until the log is clean, then stop play. Do not claim the script works without reading the log.\n"
        << "Doriax Lua scripts are plain returned module tables: local Name = { properties = {} }; function Name:init() ... end; return Name.\n"
        << "Editor-exposed Lua properties go in the properties array as { name = \"speed\", displayName = \"Speed\", type = \"float\", default = 5.0 } and are read at runtime as self.speed.\n"
        << "Per-frame/input Lua logic must be registered in :init() with the global RegisterEngineEvent(self, \"onUpdate\") plus a matching function Name:onUpdate() ... end (also onFixedUpdate, onDraw, onMouseDown, onKeyDown, onTouchStart, etc.); no manual unregister is needed.\n"
        << "For component/UI events in Lua use the global RegisterEvent(self, eventObject, \"method\"), e.g. RegisterEvent(self, Button(self.scene, self.entity):getButtonComponent().onPress, \"onPress\"). RegisterEngineEvent/RegisterEvent are Lua globals (not bindings); find them with search_engine_api.\n"
        << "Lua script instances receive self.scene and self.entity; self.entity is a numeric Entity id, not an object. Never call self.entity:... or self.entity....\n"
        << "They do not use Dori.Script, on_start, get_entity, get_component/getComponent, or set_property/setProperty.\n"
        << "Do not put editor property paths such as submeshes[0].material.baseColorFactor inside Lua scripts; use runtime wrappers and APIs from search_engine_api instead.\n"
        << "For Mesh/Shape color at runtime use Shape(self.scene, self.entity) then setColor(1,0,0,1) or setColor(Vector4(1,0,0,1)); do not call methods on self.entity directly.\n"
        << "Doriax C++ scripts use flat quoted headers from .doriax/engine-api (e.g. \"Mesh.h\", \"ScriptBase.h\", \"Engine.h\"). Never use #include <core/...>.\n"
        << "For mesh/cube behavior use cpp_subclass inheriting Mesh (or Shape) and call setColor() on this; do not use ScriptBase as if it had mesh APIs or onInit().\n"
        << "cpp_script_class inherits ScriptBase for general logic; register onUpdate with REGISTER_ENGINE_EVENT in the constructor and unregister with UNREGISTER_ENGINE_EVENT in the destructor.\n"
        << "A Doriax script class needs no class-declaration or reflection macro (no D_OBJECT/GDCLASS/GENERATED_BODY/Q_OBJECT): just inherit the base class and declare a (Scene*, Entity) constructor. Prefer editing the class skeleton create_script generated rather than rewriting it.\n"
        << "For component/UI events (button press, click, scrollbar change) use REGISTER_COMPONENT_EVENT(Component, event, method) or shortcuts REGISTER_UI_EVENT/REGISTER_BUTTON_EVENT/REGISTER_SCROLLBAR_EVENT/REGISTER_PANEL_EVENT in the constructor, paired with UNREGISTER_* in the destructor. These macros and DPROPERTY are C++ script macros (not Lua bindings); find them with search_engine_api.\n"
        << "Cameras orient by target, not by rotation: CameraComponent.useTarget defaults to true and Camera::setTarget re-enables it, so while it is on the view is lookAt(position, target, up) and the entity's rotation is ignored. Call disableTarget() first if you want the rotation to drive the view (forward = rotation * Vector3(0, 0, -1)). For a third-person or orbit camera, place the camera around the target with spherical coordinates -- offset = (cos(pitch)*sin(yaw), sin(pitch), cos(pitch)*cos(yaw)); position = target + offset*distance -- then call setTarget(target), so a positive pitch means the camera sits ABOVE the target. Do not derive the position by subtracting a rotated forward vector (target - forward*distance): rotations are right-handed, so a positive pitch tilts the VIEW up and that formula silently puts the camera below the target.\n"
        << "Engine angle arguments are in the engine's default unit, which is DEGREES unless Engine::setUseDegrees(false) was called: Quaternion(angle, axis), Quaternion::fromEulerAngles, Camera::rotateView and similar all pass their angle through Angle::defaultToRad, so never assume they take radians. Convert explicitly with Angle::degToDefault/Angle::radToDefault, and note that std::sin/std::cos always want radians (Angle::defaultToRad).\n"
        << "To print or log from scripts use the Log class: C++ Log::print(\"pos %f %f %f\", p.x, p.y, p.z) with #include \"Log.h\" (Log::warn/Log::error for severity); Lua Log.print(\"pos \" .. tostring(p)). Engine has no log method; never use Engine::log, printf, std::cout, or bare print().\n"
        << "Custom shaders are per-component on Mesh/UI/Points/Lines/Sky via the customShader property. To customize, call fork_shader (it copies the built-in <type>.vert/.frag into the project shaders folder and sets customShader in one undoable step). Never set customShader by hand to files that do not exist; the value is a base path with no extension (vert=base.vert, frag=base.frag) or two explicit paths joined by '|' (\"a.vert|b.frag\") when the entry points have different names. Both a .vert and a .frag are always required. Clear customShader (empty string) to reset to the built-in shader.\n"
        << "The forked .vert/.frag are complete, compiling engine shaders with a strict structure: a #version line, #include \"includes/...\" (resolved against the engine library), #ifdef variant guards, uniform blocks, texture and sampler declarations, in/out varyings, and a specific fragment output variable. Editing them with write_shader_file REQUIRES first calling read_resource_file on the forked file, then making minimal targeted changes that PRESERVE the #version, #include lines, uniform blocks, ALL original texture and sampler declarations (uniform sampler2D/samplerCube/textureN/samplerN and their #ifdef guards), in/out declarations, and the existing output variable name. Keep every original texture/sampler declaration even if your new code does not sample it - the engine binds them by fixed slot, so removing or renaming one breaks the texture bindings. Likewise keep the original main() code that READS the in varyings (v_position, v_normal, v_uv1, v_color, v_lightProjPos, etc.): if you delete a varying's usage the GLSL compiler strips that fragment input and the shader fails to cross-compile with a vertex/fragment interface mismatch (\"vertex shader output X does not exist in fragment shader inputs\"). Prefer to keep the original main() body that consumes the varyings and layer your effect on the final color (e.g. compute the original color, then posterize/override it), rather than deleting the existing computations. Only change the shading math (usually near the end of main()). Never rewrite a shader from scratch, drop the #version/#include lines, or invent uniform, sampler, texture, or output names - that produces compile errors (e.g. missing precision qualifier / wrong #version) or broken bindings. A stylized custom shader (e.g. toon/cel) may legitimately replace the lighting model and stop sampling the shadow or IBL maps; that is allowed - keep their declarations (the engine harmlessly skips the now-unused bindings), but briefly tell the user when your shader changes lighting behavior such as no longer receiving shadows, so they can ask to keep it. If a custom shader fails to compile, the object falls back to the built-in shader and the error appears in the output window; read that error and the forked source, then fix minimally.\n";
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

bool AiService::hasPendingProposalsLocked() const {
    for (const auto& proposal : proposals) {
        if (!proposal.executed) {
            return true;
        }
    }
    return false;
}

void AiService::finishTurnLocked(bool success) {
    if (!turnActive) {
        return;
    }
    turnActive = false;
    turnFailed = false;
    if (success) {
        Out::success("AI chat session completed");
    }
}

void AiService::runProviderRequest(ProviderRequest request) {
    if (request.apiKey.empty()) {
        std::lock_guard<std::mutex> lock(mutex);
        appendAssistantMessageLocked("No API key set for " + toString(request.settings.provider) +
                                     ". Open AI settings to add one before sending requests.");
        turnFailed = true;
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
            turnFailed = true;
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
            turnFailed = true;
        } else if (parsed.toolCalls.empty() && parsed.truncated) {
            // A truncated reply often drops an incomplete tool call mid-JSON, so
            // it arrives with no usable tool call. Use provider usage, when
            // available, to distinguish the editor's output cap from a full
            // provider/model context window.
            std::string message = buildTruncationMessage(request, parsed);
            if (!parsed.text.empty()) {
                message = parsed.text + "\n\n" + message;
            }
            appendAssistantMessageLocked(message);
            turnFailed = true;
        } else if (parsed.text.empty() && parsed.toolCalls.empty()) {
            appendAssistantMessageLocked("The provider returned no text or tool calls.");
            turnFailed = true;
        } else {
            ChatMessage assistant;
            assistant.role = ChatRole::Assistant;
            assistant.content = parsed.text;
            assistant.toolCalls = parsed.toolCalls;
            assistant.thinkingBlocks = parsed.thinkingBlocks;
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
        toolMessage.toolDescription = proposal.description;
        toolMessage.toolSuccess = false;
        toolMessage.content = "Error: " + validation.error;
        messages.push_back(toolMessage);
    }
    proposals.push_back(proposal);
}

} // namespace doriax::editor::ai
