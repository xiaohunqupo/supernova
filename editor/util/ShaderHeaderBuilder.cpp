//
// (c) 2026 Eduardo Doria.
//

#include "ShaderHeaderBuilder.h"
#include "FileUtils.h"

#include <algorithm>

using namespace doriax::editor;

std::string ShaderHeaderBuilder::buildShaderHeader(const std::string& suffix, std::vector<HeaderShader> shaders) {
    std::sort(shaders.begin(), shaders.end(), [](const HeaderShader& a, const HeaderShader& b) {
        return a.name < b.name;
    });

    std::string content;
    content += "#ifndef SHADER_" + suffix + "_h\n";
    content += "#define SHADER_" + suffix + "_h\n\n";
    content += "#include <string>\n\n";

    constexpr size_t chunkSize = 100;
    for (const auto& shader : shaders) {
        content += "static const std::string " + shader.name + " = ";
        if (shader.encodedData.empty()) {
            content += "\"\"";
        } else {
            for (size_t offset = 0; offset < shader.encodedData.size(); offset += chunkSize) {
                content += "\n    \"" + shader.encodedData.substr(offset, chunkSize) + "\"";
            }
        }
        content += ";\n\n";
    }

    content += "std::string getBase64Shader(std::string name) {";
    bool first = true;
    for (const auto& shader : shaders) {
        content += "\n";
        content += first ? "    if (name == \"" : "    } else if (name == \"";
        content += shader.name + "\") {\n";
        content += "        return " + shader.name + ";";
        first = false;
    }
    if (!first) {
        content += "\n    }";
    }
    content += "\n    return \"\";\n}\n\n";
    content += "#endif //SHADER_" + suffix + "_h\n";

    return content;
}

bool ShaderHeaderBuilder::writeShaderHeader(const std::filesystem::path& outputPath, const std::string& suffix, const std::vector<HeaderShader>& shaders, std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    if (ec) {
        err = "Failed to create shader header directory: " + ec.message();
        return false;
    }

    FileUtils::writeIfChanged(outputPath, buildShaderHeader(suffix, shaders));
    return true;
}
