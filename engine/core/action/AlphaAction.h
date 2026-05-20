//
// (c) 2026 Eduardo Doria.
//

#ifndef ALPHAACTION_H
#define ALPHAACTION_H

#include "TimedAction.h"

namespace doriax{
    class DORIAX_API AlphaAction: public TimedAction{

    public:
        AlphaAction(Scene* scene);
        AlphaAction(Scene* scene, Entity entity);

        void setAction(float startAlpha, float endAlpha, float duration, bool loop=false);
    };
}

#endif //ALPHAACTION_H