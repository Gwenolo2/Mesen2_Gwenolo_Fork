#pragma once
// Lecteur glTF 2.0 (.gltf / .glb) pour <m7model> : maillages, materiaux, textures (PNG / JPG),
// hierarchie de noeuds, squelettes (skins) et animations. Base sur cgltf (MIT, ThirdParty/cgltf).
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

struct SnesHdModelMesh;
struct SnesHdPackBitmapInfo;

std::shared_ptr<SnesHdModelMesh> SnesHdLoadGltfModel(const std::string& rootFolder, const std::string& file,
	std::vector<std::unique_ptr<SnesHdPackBitmapInfo>>& imageFileData,
	std::unordered_map<std::string, SnesHdPackBitmapInfo*>& loadedBitmaps, const std::string& lineLocation);
