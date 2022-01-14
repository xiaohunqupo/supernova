//
// (c) 2021 Eduardo Doria.
//

#ifndef TIMEDACTION_H
#define TIMEDACTION_H

#include "Action.h"
#include "Ease.h"

namespace Supernova{
    class TimedAction: public Action{

    protected:
        void setAction(float duration, bool loop);

    public:
        TimedAction(Scene* scene);

        void setFunction(std::function<float(float)> function);
        void setFunction(sol::function);

        void setFunctionType(int functionType);
    };
}

#endif //TIMEDACTION_H