#include "Out.h"
#include "EditorHost.h"
#include <cstdarg>
#include <iostream>

using namespace doriax::editor;

OutputWindow* Out::outputWindow = nullptr;

void Out::logMessage(LogType type, const std::string& message, const char* fallbackPrefix, std::ostream& fallbackStream) {
    if (Out::getOutputWindow()) {
        getEditorHost().enqueueMainThreadTask([type, message]() {
            if (OutputWindow* window = Out::getOutputWindow()) {
                window->addLog(type, message);
            }
        });
    } else {
        fallbackStream << fallbackPrefix << message << std::endl;
    }
}

void Out::setOutputWindow(OutputWindow* outputWindow) {
    Out::outputWindow = outputWindow;
}

OutputWindow* Out::getOutputWindow() {
    return outputWindow;
}

void Out::info(const std::string& message) {
    logMessage(LogType::Info, message, "[INFO] ", std::cout);
}

void Out::success(const std::string& message) {
    logMessage(LogType::Success, message, "[SUCCESS] ", std::cout);
}

void Out::warning(const std::string& message) {
    logMessage(LogType::Warning, message, "[WARNING] ", std::cout);
}

void Out::error(const std::string& message) {
    logMessage(LogType::Error, message, "[ERROR] ", std::cerr);
}

void Out::build(const std::string& message) {
    logMessage(LogType::Build, message, "[BUILD] ", std::cerr);
}

void Out::editor_assert(bool condition, const std::string& message) {
    if (!condition) {
        error("Assertion failed: " + message);
        #ifdef _DEBUG
            // Break into debugger if in debug mode
            #if defined(_MSC_VER)
                __debugbreak();
            #elif defined(__GNUC__)
                __builtin_trap();
            #endif
        #endif
    }
}
