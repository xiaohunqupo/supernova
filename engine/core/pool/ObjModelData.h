//
// (c) 2026 Eduardo Doria.
//

#ifndef OBJMODELDATA_H
#define OBJMODELDATA_H

#include <vector>
#include "tiny_obj_loader.h"

namespace doriax{

    struct ObjModelData {
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
    };

}

#endif /* OBJMODELDATA_H */
