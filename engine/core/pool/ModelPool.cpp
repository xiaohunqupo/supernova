//
// (c) 2026 Eduardo Doria.
//

#include "ModelPool.h"
#include "ObjModelData.h"

#include "tiny_gltf.h"

using namespace doriax;

models_t& ModelPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static models_t* map = new models_t();
    return *map;
}

obj_models_t& ModelPool::getObjMap(){
    static obj_models_t* map = new obj_models_t();
    return *map;
}

std::shared_ptr<tinygltf::Model> ModelPool::getGLTF(const std::string& id){
    auto it = getMap().find(id);
    if (it == getMap().end()){
        return nullptr;
    }

    if (it->second.use_count() > 0){
        return it->second;
    }

    return nullptr;
}

std::shared_ptr<tinygltf::Model> ModelPool::addGLTF(const std::string& id, std::shared_ptr<tinygltf::Model> model){
    auto& shared = getMap()[id];

    if (shared.use_count() > 0){
        return shared;
    }

    shared = model;
    return shared;
}

std::shared_ptr<ObjModelData> ModelPool::getObj(const std::string& id){
    auto it = getObjMap().find(id);
    if (it == getObjMap().end()){
        return nullptr;
    }

    if (it->second.use_count() > 0){
        return it->second;
    }

    return nullptr;
}

std::shared_ptr<ObjModelData> ModelPool::addObj(const std::string& id, std::shared_ptr<ObjModelData> model){
    auto& shared = getObjMap()[id];

    if (shared.use_count() > 0){
        return shared;
    }

    shared = model;
    return shared;
}

void ModelPool::removeGLTF(const std::string& id){
    if (getMap().count(id)){
        auto& shared = getMap()[id];
        if (shared.use_count() <= 1){
            getMap().erase(id);
        }
    }
}

void ModelPool::removeObj(const std::string& id){
    if (getObjMap().count(id)){
        auto& shared = getObjMap()[id];
        if (shared.use_count() <= 1){
            getObjMap().erase(id);
        }
    }
}

void ModelPool::clear(){
    getMap().clear();
    getObjMap().clear();
}

void ModelPool::clearUnused(){
    auto& map = getMap();
    for (auto it = map.begin(); it != map.end();){
        if (!it->second || it->second.use_count() <= 1){
            it = map.erase(it);
        }else{
            ++it;
        }
    }
    auto& objMap = getObjMap();
    for (auto it = objMap.begin(); it != objMap.end();){
        if (!it->second || it->second.use_count() <= 1){
            it = objMap.erase(it);
        }else{
            ++it;
        }
    }
}
