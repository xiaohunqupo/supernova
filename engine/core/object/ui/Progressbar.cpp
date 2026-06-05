//
// (c) 2026 Eduardo Doria.
//

#include "Progressbar.h"

#include "subsystem/UISystem.h"

using namespace doriax;

Progressbar::Progressbar(Scene* scene): Image(scene){
    addComponent<ProgressbarComponent>();
}

Progressbar::Progressbar(Scene* scene, Entity entity): Image(scene, entity){
}

Progressbar::~Progressbar(){
}

Image Progressbar::getFillObject() const{
    ProgressbarComponent& progresscomp = getComponent<ProgressbarComponent>();

    return Image(scene, progresscomp.fill);
}

void Progressbar::setType(ProgressbarType type){
    ProgressbarComponent& progresscomp = getComponent<ProgressbarComponent>();

    progresscomp.type = type;
    progresscomp.needUpdateProgressbar = true;
}

ProgressbarType Progressbar::getType() const{
    ProgressbarComponent& progresscomp = getComponent<ProgressbarComponent>();

    return progresscomp.type;
}

void Progressbar::setValue(float value){
    ProgressbarComponent& progresscomp = getComponent<ProgressbarComponent>();

    progresscomp.value = value;
    progresscomp.needUpdateProgressbar = true;
}

float Progressbar::getValue() const{
    ProgressbarComponent& progresscomp = getComponent<ProgressbarComponent>();

    return progresscomp.value;
}

void Progressbar::setFillTexture(const std::string& path){
    getFillObject().setTexture(path);
}

void Progressbar::setFillTexture(Framebuffer* framebuffer){
    getFillObject().setTexture(framebuffer);
}

void Progressbar::setFillColor(Vector4 color){
    getFillObject().setColor(color);
}

void Progressbar::setFillColor(const float red, const float green, const float blue, const float alpha){
    getFillObject().setColor(red, green, blue, alpha);
}

void Progressbar::setFillColor(const float red, const float green, const float blue){
    getFillObject().setColor(red, green, blue);
}

void Progressbar::setFillAlpha(const float alpha){
    getFillObject().setAlpha(alpha);
}

Vector4 Progressbar::getFillColor() const{
    return getFillObject().getColor();
}

float Progressbar::getFillAlpha() const{
    return getFillObject().getAlpha();
}

void Progressbar::setFillPatchMargin(int margin){
    getFillObject().setPatchMargin(margin);
}

void Progressbar::setFillPatchMargin(int marginLeft, int marginRight, int marginTop, int marginBottom){
    getFillObject().setPatchMargin(marginLeft, marginRight, marginTop, marginBottom);
}
