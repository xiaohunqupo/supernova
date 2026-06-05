//
// (c) 2026 Eduardo Doria.
//

#ifndef PROGRESSBAR_COMPONENT_H
#define PROGRESSBAR_COMPONENT_H

#include "ecs/Entity.h"

namespace doriax{

    enum class ProgressbarType{
        VERTICAL,
        HORIZONTAL
    };

    struct DORIAX_API ProgressbarComponent{
        Entity fill = NULL_ENTITY;
        ProgressbarType type = ProgressbarType::HORIZONTAL;

        float value = 0; // from 0 to 1

        bool needUpdateProgressbar = true;
    };

}

#endif //PROGRESSBAR_COMPONENT_H
