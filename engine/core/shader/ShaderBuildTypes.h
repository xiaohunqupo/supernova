//
// (c) 2026 Eduardo Doria.
//

#ifndef SHADERBUILDTYPES_H
#define SHADERBUILDTYPES_H

#include <cstdint>
#include "Engine.h"
#include "ShaderData.h"

namespace doriax{

    typedef uint64_t ShaderKey;

    struct ShaderBuildResult {
        ShaderData data;
        ResourceLoadState state;

        ShaderBuildResult() : state(ResourceLoadState::NotStarted) {}
        ShaderBuildResult(const ShaderData& shaderData, ResourceLoadState buildState)
            : data(shaderData), state(buildState) {}

        explicit operator bool() const {
            return state == ResourceLoadState::Finished;
        }
    };

}

#endif // SHADERBUILDTYPES_H