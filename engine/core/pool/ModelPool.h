//
// (c) 2026 Eduardo Doria.
//

#ifndef MODELPOOL_H
#define MODELPOOL_H

#include "Export.h"
#include <map>
#include <memory>
#include <string>

namespace tinygltf { class Model; }

namespace doriax{

    typedef std::map< std::string, std::shared_ptr<tinygltf::Model> > models_t;

    class DORIAX_API ModelPool{
    private:
        static models_t& getMap();

    public:
        static std::shared_ptr<tinygltf::Model> get(const std::string& id);
        static std::shared_ptr<tinygltf::Model> add(const std::string& id, std::shared_ptr<tinygltf::Model> model);
        static void remove(const std::string& id);

        // necessary for engine shutdown
        static void clear();
    };
}

#endif /* MODELPOOL_H */
