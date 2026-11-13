#include "pch.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "Gameboy/HdPacks/GbHdPackLoader.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/MessageManager.h"

bool GbHdPackLoader::Load(const std::string& packFolder, GbHdPackData& outData)
{
	outData = GbHdPackData();
	outData.FolderPath = packFolder;

	std::ifstream file(FolderUtilities::CombinePath(packFolder, "gb_hires.txt"));
	if(!file.good()) {
		return false;
	}

	int ignoredLines = 0;
	int duplicates = 0;
	std::string line;
	while(std::getline(file, line)) {
		if(!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		size_t first = line.find_first_not_of(" \t");
		if(first == std::string::npos) {
			continue;
		}
		line = line.substr(first);
		if(line[0] == '#') {
			continue;
		}

		if(line.compare(0, 7, "<scale>") == 0) {
			long s = strtol(line.c_str() + 7, nullptr, 10);
			if(s >= 1 && s <= 10) {
				outData.Scale = (uint32_t)s;
			} else {
				ignoredLines++;
			}
		} else if(line.compare(0, 5, "<img>") == 0) {
			outData.Images.push_back(line.substr(5));
		} else if(line.compare(0, 5, "<tile") == 0) {
			size_t close = line.find('>');
			if(close == std::string::npos) {
				ignoredLines++;
				continue;
			}
			std::string tag = line.substr(0, close + 1);
			uint8_t flags = 0;
			if(tag == "<tile>") {
				flags = 0;
			} else if(tag == "<tile_hflip>") {
				flags = 2;
			} else if(tag == "<tile_vflip>") {
				flags = 4;
			} else if(tag == "<tile_hvflip>") {
				flags = 6;
			} else {
				ignoredLines++;
				continue;
			}

			// Corps : hash,palette,image,x,y   # commentaire
			std::string body = line.substr(close + 1);
			size_t comment = body.find('#');
			if(comment != std::string::npos) {
				body = body.substr(0, comment);
			}
			std::vector<std::string> fields;
			std::stringstream ss(body);
			std::string field;
			while(std::getline(ss, field, ',')) {
				fields.push_back(field);
			}
			if(fields.size() != 5) {
				ignoredLines++;
				continue;
			}

			uint64_t hash = strtoull(fields[0].c_str(), nullptr, 16);
			uint32_t palette = (uint32_t)strtoul(fields[1].c_str(), nullptr, 10);
			uint32_t imageIndex = (uint32_t)strtoul(fields[2].c_str(), nullptr, 10);
			if(imageIndex >= outData.Images.size()) {
				// L'<img> correspondant n'a pas encore été lu : tile invalide
				ignoredLines++;
				continue;
			}

			GbHdTileRef ref;
			ref.ImageIndex = imageIndex;
			ref.X = (uint32_t)strtoul(fields[3].c_str(), nullptr, 10);
			ref.Y = (uint32_t)strtoul(fields[4].c_str(), nullptr, 10);

			auto key = std::make_tuple(hash, palette, flags);
			if(outData.Tiles.count(key)) {
				duplicates++;
			}
			outData.Tiles[key] = ref;
		}
	}

	int missingImages = 0;
	for(const std::string& img : outData.Images) {
		std::ifstream imgFile(FolderUtilities::CombinePath(packFolder, img), std::ios::binary);
		if(!imgFile.good()) {
			missingImages++;
		}
	}

	outData.IsLoaded = true;
	MessageManager::Log("[HD-GB-LOAD] Pack : " + packFolder);
	MessageManager::Log("[HD-GB-LOAD] scale " + std::to_string(outData.Scale) + ", " + std::to_string(outData.Images.size()) + " images, " + std::to_string(outData.Tiles.size()) + " tiles, " + std::to_string(duplicates) + " doublons, " + std::to_string(ignoredLines) + " lignes ignorées, " + std::to_string(missingImages) + " images absentes");
	return true;
}
