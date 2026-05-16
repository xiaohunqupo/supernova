//
// (c) 2026 Eduardo Doria.
//

#include "ModelPool.h"

#include "tiny_gltf.h"

using namespace doriax;

models_t& ModelPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static models_t* map = new models_t();
    return *map;
}

std::shared_ptr<tinygltf::Model> ModelPool::get(const std::string& id){
    auto it = getMap().find(id);
    if (it == getMap().end()){
        return nullptr;
    }

    if (it->second.use_count() > 0){
        return it->second;
    }

    return nullptr;
}

std::shared_ptr<tinygltf::Model> ModelPool::add(const std::string& id, std::shared_ptr<tinygltf::Model> model){
    auto& shared = getMap()[id];

    if (shared.use_count() > 0){
        return shared;
    }

    shared = model;
    return shared;
}

void ModelPool::remove(const std::string& id){
    if (getMap().count(id)){
        auto& shared = getMap()[id];
        if (shared.use_count() <= 1){
            getMap().erase(id);
        }
    }
}

void ModelPool::clear(){
    getMap().clear();
}
