//
// (c) 2026 Eduardo Doria.
//

#include "SokolFramebuffer.h"
#include "Log.h"
#include "SokolCmdQueue.h"
#include "Engine.h"

using namespace doriax;

SokolFramebuffer::SokolFramebuffer(){
    for (int i = 0; i < 6; i++){
        colorAttachmentViews[i].id = SG_INVALID_ID;
    }
    depthAttachmentView.id = SG_INVALID_ID;
}

SokolFramebuffer::SokolFramebuffer(const SokolFramebuffer& rhs){
    for (int i = 0; i < 6; i++){
        colorAttachmentViews[i] = rhs.colorAttachmentViews[i];
    }
    depthAttachmentView = rhs.depthAttachmentView;
    colorTexture = rhs.colorTexture;
    depthTexture = rhs.depthTexture;
}

SokolFramebuffer& SokolFramebuffer::operator=(const SokolFramebuffer& rhs){
    for (int i = 0; i < 6; i++){
        colorAttachmentViews[i] = rhs.colorAttachmentViews[i];
    }
    depthAttachmentView = rhs.depthAttachmentView;
    colorTexture = rhs.colorTexture;
    depthTexture = rhs.depthTexture;

    return *this;
}

bool SokolFramebuffer::createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap){
    if ((textureType != TextureType::TEXTURE_2D) && (textureType != TextureType::TEXTURE_CUBE)){
        Log::error("Framebuffer texture type must be 2D or CUBE");
        return false;
    }

    destroyFramebuffer();

    bool colorCreated = colorTexture.createFramebufferTexture(textureType, false, shadowMap, width, height, minFilter, magFilter, wrapU, wrapV);
    bool depthCreated = depthTexture.createFramebufferTexture(TextureType::TEXTURE_2D, true, shadowMap, width, height, minFilter, magFilter, wrapU, wrapV);

    if (!colorCreated || !depthCreated) {
        destroyFramebuffer();
        return false;
    }

    size_t faces = (textureType == TextureType::TEXTURE_CUBE)? 6 : 1;

    for (int i = 0; i < faces; i++){
        sg_view_desc color_view_desc = {0};
        color_view_desc.color_attachment.image = colorTexture.backend.get();
        color_view_desc.color_attachment.slice = i;
        color_view_desc.label = "framebuffer-color-attachment-view";

        if (Engine::isAsyncThread()){
            colorAttachmentViews[i] = SokolCmdQueue::add_command_make_view(color_view_desc);
        }else{
            colorAttachmentViews[i] = sg_make_view(color_view_desc);
        }
    }

    sg_view_desc depth_view_desc = {0};
    depth_view_desc.depth_stencil_attachment.image = depthTexture.backend.get();
    depth_view_desc.label = "framebuffer-depth-attachment-view";

    if (Engine::isAsyncThread()){
        depthAttachmentView = SokolCmdQueue::add_command_make_view(depth_view_desc);
    }else{
        depthAttachmentView = sg_make_view(depth_view_desc);
    }

    bool created = isCreated();
    if (!created) {
        destroyFramebuffer();
    }

    return created;
}

void SokolFramebuffer::destroyFramebuffer(){
    if (sg_isvalid()){
        for (int i = 0; i < 6; i++){
            if (colorAttachmentViews[i].id == SG_INVALID_ID) {
                continue;
            }

            if (Engine::isAsyncThread()){
                SokolCmdQueue::add_command_destroy_view(colorAttachmentViews[i]);
            }else{
                sg_destroy_view(colorAttachmentViews[i]);
            }
        }

        if (depthAttachmentView.id != SG_INVALID_ID){
            if (Engine::isAsyncThread()){
                SokolCmdQueue::add_command_destroy_view(depthAttachmentView);
            }else{
                sg_destroy_view(depthAttachmentView);
            }
        }
    }

    colorTexture.destroyTexture();
    depthTexture.destroyTexture();

    for (int i = 0; i < 6; i++){
        colorAttachmentViews[i].id = SG_INVALID_ID;
    }
    depthAttachmentView.id = SG_INVALID_ID;
}

bool SokolFramebuffer::isCreated(){
    if (!sg_isvalid()) {
        return false;
    }

    bool hasAttachment = false;
    for (int i = 0; i < 6; i++) {
        if (colorAttachmentViews[i].id == SG_INVALID_ID) {
            continue;
        }

        hasAttachment = true;
        if (sg_query_view_state(colorAttachmentViews[i]) != SG_RESOURCESTATE_VALID) {
            return false;
        }
    }

    if (depthAttachmentView.id != SG_INVALID_ID) {
        if (sg_query_view_state(depthAttachmentView) != SG_RESOURCESTATE_VALID) {
            return false;
        }
    }

    return hasAttachment;
}

TextureRender& SokolFramebuffer::getColorTexture(){
    return colorTexture;
}

TextureRender& SokolFramebuffer::getDepthTexture(){
    return depthTexture;
}

const void* SokolFramebuffer::getD3D11HandlerColorRTV() const{
    if (colorAttachmentViews[0].id != SG_INVALID_ID && sg_isvalid()){
        sg_d3d11_view_info info = sg_d3d11_query_view_info(colorAttachmentViews[0]);

        return info.rtv;
    }

    return nullptr;
}

const void* SokolFramebuffer::getD3D11HandlerDSV() const{
    if (depthAttachmentView.id != SG_INVALID_ID && sg_isvalid()){
        sg_d3d11_view_info info = sg_d3d11_query_view_info(depthAttachmentView);

        return info.dsv;
    }

    return nullptr;
}

sg_attachments SokolFramebuffer::get(size_t face){
    sg_attachments attachments = {};

    attachments.colors[0] = colorAttachmentViews[face];
    attachments.depth_stencil = depthAttachmentView;

    return attachments;
}
