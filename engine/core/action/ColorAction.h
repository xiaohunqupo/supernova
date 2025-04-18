//
// (c) 2024 Eduardo Doria.
//

#ifndef COLORACTION_H
#define COLORACTION_H

#include "TimedAction.h"

namespace Supernova{
    class SUPERNOVA_API ColorAction: public TimedAction{

    public:
        ColorAction(Scene* scene);

        void setAction(Vector3 startColor, Vector3 endColor, float duration, bool loop=false);
        void setAction(Vector4 startColor, Vector4 endColor, float duration, bool loop=false);
    };
}

#endif //COLORACTION_H