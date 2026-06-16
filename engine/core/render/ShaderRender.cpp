//
// (c) 2026 Eduardo Doria.
//

#include "ShaderRender.h"

#include "SystemRender.h"
#include "Engine.h"

using namespace doriax;

ShaderRender::ShaderRender(){ }

ShaderRender::ShaderRender(const ShaderRender& rhs) : backend(rhs.backend), shaderData(rhs.shaderData) { }

ShaderRender& ShaderRender::operator=(const ShaderRender& rhs) { 
    backend = rhs.backend;
    shaderData = rhs.shaderData;
    return *this; 
}

ShaderRender::~ShaderRender(){
    //Cannot destroy because its a handle
}

bool ShaderRender::createShader(ShaderData& shaderData){
    if (Engine::isViewLoaded() && !isCreated()) {
        this->shaderData = shaderData;

        bool ret = backend.createShader(this->shaderData);

        // Release the source via the command stream so it runs after MAKE_SHADER.
        // A frame-counted scheduleCleanup() could fire before the shader is built,
        // leaving an empty source (GL_SHADER_COMPILATION_FAILED) on the async path.
        SystemRender::addQueueCommand(ShaderRender::cleanupShader, &(this->shaderData));
        
        return ret;
    }else{
        return false;
    }
}

void ShaderRender::destroyShader(){
    backend.destroyShader();
}

bool ShaderRender::isCreated(){
    return backend.isCreated();
}

void ShaderRender::cleanupShader(void* data){
    //Keep only reflection info
    static_cast<ShaderData*>(data)->releaseSourceData();
}