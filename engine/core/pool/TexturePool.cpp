//
// (c) 2026 Eduardo Doria.
//

#include "TexturePool.h"

#include "Engine.h"
#include "Log.h"

using namespace doriax;

textures_t& TexturePool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static textures_t* map = new textures_t();
    return *map;
};

std::shared_ptr<TextureRender> TexturePool::get(const std::string& id){
	auto& map = getMap();
	auto it = map.find(id);

    if (it != map.end() && it->second && it->second->isCreated()){
        return it->second;
    }

	return NULL;
}

std::shared_ptr<TextureRender> TexturePool::get(const std::string& id, TextureType type, const std::shared_ptr<std::array<TextureData,6>> &data, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV){
	auto& map = getMap();
	auto it = map.find(id);
	std::shared_ptr<TextureRender> shared = (it != map.end()) ? it->second : nullptr;

	if (shared && shared->isCreated()){
		return shared;
	}

	if (!data){
		return NULL;
	}

	int numFaces = (type == TextureType::TEXTURE_CUBE) ? 6 : 1;

	void* data_array[6];
	size_t size_array[6];

	for (int f = 0; f < numFaces; f++){
		data_array[f] = data->at(f).getData();
		size_array[f] = (size_t)data->at(f).getSize();
		if (!data_array[f] || size_array[f] == 0){
			// Pixel data was released or never populated. Skip the upload so we
			// don't trigger Sokol's VALIDATE_IMAGEDATA_NODATA. The caller will
			// fall back and Texture::load() will refresh the data on a later try.
			Log::error("Texture data is empty: %s", id.c_str());
			return NULL;
		}
	}

	if (!shared){
		shared = std::make_shared<TextureRender>();
	}

	if (!shared->createTexture(id, data->at(0).getWidth(), data->at(0).getHeight(), data->at(0).getColorFormat(), type, numFaces, data_array, size_array, minFilter, magFilter, wrapU, wrapV)){
		shared->destroyTexture();
		if (it != map.end()){
			map.erase(it);
		}
		return NULL;
	}
	//Log::debug("Create texture %s", id.c_str());

	map[id] = shared;

	return shared;
}

void TexturePool::remove(const std::string& id){
	auto& map = getMap();
	auto it = map.find(id);
	if (it != map.end()){
		if (!it->second || it->second.use_count() <= 1){
			if (it->second){
				it->second->destroyTexture();
			}
			//Log::debug("Remove texture %s", id.c_str());
			map.erase(it);
		}
	}else{
		if (Engine::isViewLoaded()){
			Log::debug("Trying to destroy a non existent texture: %s", id.c_str());
		}
	}
}

void TexturePool::clear(){
	for (auto& it: getMap()) {
		if (it.second)
			it.second->destroyTexture();
	}
	//Log::debug("Remove all textures");
	getMap().clear();
}
