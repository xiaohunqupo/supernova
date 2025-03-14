//
// (c) 2024 Eduardo Doria.
//

#ifndef ACTION_COMPONENT_H
#define ACTION_COMPONENT_H

#include "Engine.h"

namespace Supernova{

    enum class ActionState{
        Running,
        Paused,
        Stopped
    };

    struct SUPERNOVA_API ActionComponent{
        ActionState state = ActionState::Stopped;
        float timecount = 0;

        bool startTrigger = false;
        bool stopTrigger = false;
        bool pauseTrigger = false;

        bool ownedTarget = false;

        Entity target = NULL_ENTITY;

        float speed = 1;

        FunctionSubscribe<void()> onStart;
        FunctionSubscribe<void()> onPause;
        FunctionSubscribe<void()> onStop;

        FunctionSubscribe<void()> onStep;
    };

    
}

#endif //ACTION_COMPONENT_H