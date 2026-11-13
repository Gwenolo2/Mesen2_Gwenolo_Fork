#include "pch.h"
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <algorithm>
#include <sstream>
#include <fstream>
#include "Gameboy/HdPacks/GbHdPackBuilder.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Shared/MessageManager.h"

bool GbHdPackBuilder::_isActive = false;
HdPackBuilderOptions GbHdPackBuilder::_options = {};
std::string GbHdPackBuilder::_saveFolder = "";
uint32_t GbHdPackBuilder::_sheetWidth = 16;
int GbHdPackBuilder::_scale = 4;
std::mutex GbHdPackBuilder::_mutex;
std::set<std::tuple<uint64_t, uint32_t, uint8_t>> GbHdPackBuilder::_processed;
std::map<uint32_t, std::vector<GbHdPackBuilder::Tile>> GbHdPackBuilder::_paletteTiles;
uint64_t GbHdPackBuilder::_totalCalls = 0;
std::vector<std::vector<GbOamTile>> GbHdPackBuilder::_frameSnapshots;
std::map<std::tuple<uint64_t, uint32_t, uint8_t>, uint64_t> GbHdPackBuilder::_spriteOrder;
std::vector<GbHdPackBuilder::GbObjectInfo> GbHdPackBuilder::_objects;
std::map<std::tuple<uint64_t, uint32_t, uint8_t>, std::string> GbHdPackBuilder::_spriteAtlasRef;
std::map<uint64_t, GbHdPackBuilder::GbScreenInfo> GbHdPackBuilder::_screens;
size_t GbHdPackBuilder::_screenFrames = 0;
std::map<std::tuple<uint64_t, uint32_t, uint8_t>, std::string> GbHdPackBuilder::_bgAtlasRef;
std::map<uint32_t, uint64_t> GbHdPackBuilder::_callsPerPalette;

bool GbHdPackBuilder::IsActive()
{
	return _isActive;
}

void GbHdPackBuilder::StartRecording(HdPackBuilderOptions options)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_processed.clear();
	_paletteTiles.clear();
	_options = options;
	_saveFolder = options.SaveFolder ? options.SaveFolder : "";
	_scale = (int)options.Scale;
	if(_scale < 1 || _scale > 10) {
		_scale = 4;
	}
	_sheetWidth = (options.SheetWidth >= 1 && options.SheetWidth <= 256) ? options.SheetWidth : 16;
	_totalCalls = 0;
	_callsPerPalette.clear();
	_frameSnapshots.clear();
	_spriteOrder.clear();
	_objects.clear();
	_spriteAtlasRef.clear();
	_screens.clear();
	_screenFrames = 0;
	_bgAtlasRef.clear();
	_isActive = true;
	MessageManager::Log("[HD-GB] Enregistrement démarré : scale " + std::to_string(_scale) + ", dossier " + _saveFolder);
}

void GbHdPackBuilder::StopRecording()
{
	std::lock_guard<std::mutex> lock(_mutex);
	if(!_isActive) {
		return;
	}
	_isActive = false;
	LogSummary();
	if(_options.GenerateGbObjects) {
		BuildObjects();
	} else {
			MessageManager::Log("[HD-GB] Objets composes desactives : aucun PNG ni .mesen par objet");
	}
	WriteOutput();
	if(_options.GenerateGbObjects) {
		WriteObjectFiles();
	}
	WriteScreenFiles();
	_screens.clear();
	_screenFrames = 0;
	_bgAtlasRef.clear();
	_objects.clear();
	_spriteAtlasRef.clear();
	_frameSnapshots.clear();
	_spriteOrder.clear();
	_processed.clear();
	_paletteTiles.clear();
}

void GbHdPackBuilder::ProcessTile(const uint8_t* tileData, uint32_t paletteId, const uint16_t* colors, bool isSprite, bool hFlip, bool vFlip)
{
	if(!_isActive || tileData == nullptr || colors == nullptr) {
		return;
	}

	// Hash identique au SNES, au NES et aux conditions : hash = hash*31 + octet
	uint64_t hash = 0;
	for(int i = 0; i < 16; i++) {
		hash = (hash * 31) + tileData[i];
	}

	uint8_t flags = (isSprite ? 1 : 0) | (hFlip ? 2 : 0) | (vFlip ? 4 : 0);

	std::lock_guard<std::mutex> lock(_mutex);
	if(!_isActive) {
		return;
	}
	_totalCalls++;
	_callsPerPalette[paletteId]++;
	if(!_processed.insert(std::make_tuple(hash, paletteId, flags)).second) {
		return; // déjà vue
	}

	// Rendu : pixels 8×8 retournés comme à l'écran, puis mis à l'échelle
	int tileSize = 8 * _scale;
	Tile tile;
	tile.Hash = hash;
	tile.Palette = paletteId;
	tile.IsSprite = isSprite;
	tile.HFlip = hFlip;
	tile.VFlip = vFlip;
	tile.Pixels.assign(tileSize * tileSize, 0);

	for(int y = 0; y < 8; y++) {
		int srcY = vFlip ? 7 - y : y;
		uint8_t lo = tileData[srcY * 2];
		uint8_t hi = tileData[srcY * 2 + 1];
		for(int x = 0; x < 8; x++) {
			int srcX = hFlip ? 7 - x : x;
			int bit = 7 - srcX;
			uint8_t c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);

			uint16_t bgr = colors[c];
			uint8_t r = (bgr & 0x1F);
			r = (r << 3) | (r >> 2);
			uint8_t g = ((bgr >> 5) & 0x1F);
			g = (g << 3) | (g >> 2);
			uint8_t b = ((bgr >> 10) & 0x1F);
			b = (b << 3) | (b >> 2);

			// Objets : la couleur 0 est transparente (comme le SNES)
			uint8_t a = (isSprite && c == 0) ? 0 : 255;
			uint32_t argb = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;

			for(int dy = 0; dy < _scale; dy++) {
				for(int dx = 0; dx < _scale; dx++) {
					tile.Pixels[(y * _scale + dy) * tileSize + (x * _scale + dx)] = argb;
				}
			}
		}
	}

	_paletteTiles[paletteId].push_back(std::move(tile));
}

void GbHdPackBuilder::SnapshotFrame(const uint8_t* oam, const uint8_t* vram, bool large, bool cgb, uint8_t obp0, uint8_t obp1)
{
	if(!_isActive || oam == nullptr || vram == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lock(_mutex);
	if(!_isActive || _frameSnapshots.size() >= MaxSnapshots) {
		return;
	}

	std::vector<GbOamTile> frame;
	for(int i = 0; i < 40; i++) {
		uint8_t rawY = oam[i * 4];
		uint8_t rawX = oam[i * 4 + 1];
		uint8_t tile = oam[i * 4 + 2];
		uint8_t attr = oam[i * 4 + 3];
		if(rawY == 0 || rawY >= 160 || rawX == 0 || rawX >= 168) {
			continue; // objet masqué
		}
		int x = (int)rawX - 8;
		int y = (int)rawY - 16;
		bool hf = (attr & 0x20) != 0;
		bool vf = (attr & 0x40) != 0;
		uint32_t palette;
		int bank = 0;
		if(cgb) {
			palette = 8 + (attr & 0x07);
			bank = (attr & 0x08) ? 0x2000 : 0;
		} else {
			palette = 256 + ((attr & 0x10) ? obp1 : obp0);
		}
		uint8_t flags = 1 | (hf ? 2 : 0) | (vf ? 4 : 0);

		int count = large ? 2 : 1;
		uint8_t t0 = large ? (uint8_t)(tile & 0xFE) : tile;
		for(int k = 0; k < count; k++) {
			// 8×16 : la tile du haut est t0, celle du bas t0|1 ; avec flip V elles s'échangent
			int idx;
			int dy = large ? 8 * k : 0;
			if(!large) {
				idx = t0;
			} else if(!vf) {
				idx = t0 + k;
			} else {
				idx = t0 + (1 - k);
			}
			int base = idx * 16 + bank;
			uint64_t h = 0;
			for(int j = 0; j < 16; j++) {
				h = (h * 31) + vram[base + j];
			}
			GbOamTile entry;
			entry.X = (int16_t)x;
			entry.Y = (int16_t)(y + dy);
			entry.Hash = h;
			entry.Palette = palette;
			entry.Flags = flags;
			frame.push_back(entry);
		}
	}
	_frameSnapshots.push_back(std::move(frame));
}

void GbHdPackBuilder::BuildObjects()
{
	struct Obj {
		int Count = 0;
		std::vector<GbOamTile> Members; // coordonnées normalisées (coin haut-gauche = 0,0)
	};
	std::map<std::string, Obj> objects;

	for(const auto& frame : _frameSnapshots) {
		size_t n = frame.size();
		std::vector<bool> used(n, false);
		for(size_t s = 0; s < n; s++) {
			if(used[s]) continue;

			// Composante connexe : tiles voisines (même grille de 8 pixels)
			std::vector<size_t> stack{ s };
			std::vector<size_t> members;
			used[s] = true;
			while(!stack.empty()) {
				size_t cur = stack.back();
				stack.pop_back();
				members.push_back(cur);
				for(size_t o = 0; o < n; o++) {
					if(used[o]) continue;
					int dx = frame[o].X - frame[cur].X;
					int dy = frame[o].Y - frame[cur].Y;
					bool adjacent = (std::abs(dx) == 8 && dy == 0) || (dx == 0 && std::abs(dy) == 8);
					if(adjacent) {
						used[o] = true;
						stack.push_back(o);
					}
				}
			}

			if(members.size() < 2 || members.size() > 32) {
				continue; // pas un objet multi-tiles
			}

			int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
			for(size_t m : members) {
				minX = std::min(minX, (int)frame[m].X);
				minY = std::min(minY, (int)frame[m].Y);
				maxX = std::max(maxX, (int)frame[m].X);
				maxY = std::max(maxY, (int)frame[m].Y);
			}
			if(maxX - minX + 8 > 128 || maxY - minY + 8 > 128) {
				continue; // trop grand : probablement deux objets collés
			}

			// Signature indépendante de la position : ordre trié des (décalage, tile, palette, flips)
			std::vector<std::string> parts;
			std::vector<GbOamTile> normalized;
			for(size_t m : members) {
				GbOamTile t = frame[m];
				t.X = (int16_t)(frame[m].X - minX);
				t.Y = (int16_t)(frame[m].Y - minY);
				normalized.push_back(t);
				char hashStr[32];
				snprintf(hashStr, sizeof(hashStr), "%016llX", (unsigned long long)t.Hash);
				parts.push_back(std::to_string(t.X) + "," + std::to_string(t.Y) + "," + hashStr + "," + std::to_string(t.Palette) + "," + std::to_string(t.Flags));
			}
			std::sort(parts.begin(), parts.end());
			std::string signature;
			for(const auto& p : parts) {
				signature += p + ";";
			}

			Obj& obj = objects[signature];
			obj.Count++;
			if(obj.Members.empty()) {
				obj.Members = normalized;
			}
		}
	}

	// Objets retenus : vus sur au moins MinObjectFrames images, du plus fréquent au moins fréquent
	std::vector<const Obj*> kept;
	for(auto& entry : objects) {
		if(entry.second.Count >= MinObjectFrames) {
			kept.push_back(&entry.second);
		}
	}
	std::sort(kept.begin(), kept.end(), [](const Obj* a, const Obj* b) { return a->Count > b->Count; });
	if(kept.size() > 300) {
		kept.resize(300);
	}

	// Ordre des tiles de sprite : objet par objet (rang croissant), puis ligne et colonne dans l'objet
	uint64_t seq = 0;
	for(const Obj* obj : kept) {
		std::vector<GbOamTile> members = obj->Members;
		std::sort(members.begin(), members.end(), [](const GbOamTile& a, const GbOamTile& b) {
			return a.Y != b.Y ? a.Y < b.Y : a.X < b.X;
		});
		for(const GbOamTile& t : members) {
			auto key = std::make_tuple(t.Hash, t.Palette, t.Flags);
			if(_spriteOrder.find(key) == _spriteOrder.end()) {
				_spriteOrder[key] = seq++;
			}
		}
	}

	MessageManager::Log("[HD-GB] Objets multi-tiles : " + std::to_string(kept.size()) + " retenus (seuil " + std::to_string(MinObjectFrames) + " images) sur " + std::to_string(_frameSnapshots.size()) + " relevés");
	if(kept.empty()) {
		return;
	}

	// Index des tiles rendues pour la composition
	std::map<std::tuple<uint64_t, uint32_t, uint8_t>, const Tile*> index;
	for(auto& entry : _paletteTiles) {
		for(const Tile& t : entry.second) {
			uint8_t flags = 1 | (t.HFlip ? 2 : 0) | (t.VFlip ? 4 : 0);
			index[std::make_tuple(t.Hash, t.Palette, flags)] = &t;
		}
	}

	std::string tilesDir = FolderUtilities::CombinePath(_saveFolder, "Tiles");
	std::string sprDir = FolderUtilities::CombinePath(tilesDir, "Sprites");
	std::string objDir = FolderUtilities::CombinePath(sprDir, "Objects");
	FolderUtilities::CreateFolder(tilesDir);
	FolderUtilities::CreateFolder(sprDir);
	FolderUtilities::CreateFolder(objDir);

	int tileSize = 8 * _scale;
	int idx = 0;
	for(const Obj* obj : kept) {
		idx++;
		int w = 0, h = 0;
		for(const GbOamTile& t : obj->Members) {
			w = std::max(w, (int)t.X + 8);
			h = std::max(h, (int)t.Y + 8);
		}
		int canvasW = w * _scale;
		int canvasH = h * _scale;
		std::vector<uint32_t> canvas((size_t)canvasW * canvasH, 0);

		for(const GbOamTile& t : obj->Members) {
			auto it = index.find(std::make_tuple(t.Hash, t.Palette, t.Flags));
			if(it == index.end()) continue;
			const Tile* tile = it->second;
			int baseX = t.X * _scale;
			int baseY = t.Y * _scale;
			for(int y = 0; y < tileSize; y++) {
				for(int x = 0; x < tileSize; x++) {
					uint32_t px = tile->Pixels[y * tileSize + x];
					if((px >> 24) == 0) continue; // transparent
					canvas[(size_t)(baseY + y) * canvasW + (baseX + x)] = px;
				}
			}
		}

		std::string baseName = "obj_" + std::to_string(idx) + "_x" + std::to_string(obj->Count);
		std::string name = baseName + ".png";
		_objects.push_back(GbObjectInfo{ baseName, obj->Count, obj->Members });
		if(!PNGHelper::WritePNG(FolderUtilities::CombinePath(objDir, name), canvas.data(), canvasW, canvasH, 32)) {
			MessageManager::Log("[HD-GB] Échec d'écriture : " + name);
		}
	}
}

void GbHdPackBuilder::WriteObjectFiles()
{
	if(_objects.empty() || _saveFolder.empty()) {
		return;
	}
	std::string objDir = FolderUtilities::CombinePath(FolderUtilities::CombinePath(FolderUtilities::CombinePath(_saveFolder, "Tiles"), "Sprites"), "Objects");
	FolderUtilities::CreateFolder(objDir);

	for(const GbObjectInfo& obj : _objects) {
		std::ostringstream out;
		out << "# Objet " << obj.Name << " : vu sur " << obj.Count << " images\n";
		out << "# Image entière : Tiles/Sprites/Objects/" << obj.Name << ".png\n";
		out << "# Tiles de l'objet ; [x,y] = position dans l'objet, en pixels GB\n";
		for(const GbOamTile& t : obj.Members) {
			char hashStr[32];
			snprintf(hashStr, sizeof(hashStr), "%016llX", (unsigned long long)t.Hash);
			auto it = _spriteAtlasRef.find(std::make_tuple(t.Hash, t.Palette, t.Flags));
			if(it == _spriteAtlasRef.end()) {
				out << "# tile absente de l'atlas : " << hashStr << "\n";
				continue;
			}
			std::string tag = "<tile";
			if((t.Flags & 2) && (t.Flags & 4)) tag += "_hvflip";
			else if(t.Flags & 2) tag += "_hflip";
			else if(t.Flags & 4) tag += "_vflip";
			tag += ">";
			out << tag << hashStr << "," << t.Palette << "," << it->second
				<< "   # [" << t.X << "," << t.Y << "]\n";
		}
		FILE* f = fopen(FolderUtilities::CombinePath(objDir, obj.Name + ".mesen").c_str(), "wb");
		if(f) {
			std::string text = out.str();
			fwrite(text.data(), 1, text.size(), f);
			fclose(f);
		} else {
			MessageManager::Log("[HD-GB] Échec d'écriture : " + obj.Name + ".mesen");
		}
	}
	MessageManager::Log("[HD-GB] Fichiers .mesen écrits : " + std::to_string(_objects.size()));
}


void GbHdPackBuilder::SnapshotBackground(const uint8_t* vram, bool mapHigh, bool tileUnsigned, int scx, int scy, bool cgb, uint8_t bgp)
{
	if(!_isActive || vram == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lock(_mutex);
	if(!_isActive || _screenFrames >= MaxSnapshots) {
		return;
	}
	_screenFrames++;

	// Écran visible : 20×18 cases, à partir du scroll (en cases de 8 pixels)
	int mapBase = mapHigh ? 0x1C00 : 0x1800;
	int startCol = (scx >> 3) & 31;
	int startRow = (scy >> 3) & 31;
	std::vector<GbOamTile> cells;
	cells.reserve(20 * 18);
	uint64_t key = 1469598103934665603ULL;
	for(int row = 0; row < 18; row++) {
		for(int col = 0; col < 20; col++) {
			int mapAddr = mapBase + ((startRow + row) & 31) * 32 + ((startCol + col) & 31);
			uint8_t tileIdx = vram[mapAddr];
			uint8_t attr = cgb ? vram[mapAddr | 0x2000] : 0;
			int bank = (cgb && (attr & 0x08)) ? 0x2000 : 0;
			int base = tileUnsigned ? tileIdx * 16 : 0x1000 + (int8_t)tileIdx * 16;
			uint64_t h = 0;
			for(int j = 0; j < 16; j++) {
				h = (h * 31) + vram[base + bank + j];
			}
			uint32_t palette = cgb ? (uint32_t)(attr & 0x07) : (uint32_t)bgp;
			uint8_t flags = ((attr & 0x20) ? 2 : 0) | ((attr & 0x40) ? 4 : 0);

			GbOamTile cell;
			cell.X = (int16_t)col;
			cell.Y = (int16_t)row;
			cell.Hash = h;
			cell.Palette = palette;
			cell.Flags = flags;
			cells.push_back(cell);
			key = (key * 1099511628211ULL) ^ h ^ (palette * 7919ULL) ^ (flags * 104729ULL);
		}
	}

	auto it = _screens.find(key);
	if(it == _screens.end()) {
		GbScreenInfo info;
		info.Cells = cells;
		it = _screens.emplace(key, std::move(info)).first;
	}
	it->second.Count++;
}

void GbHdPackBuilder::WriteScreenFiles()
{
	if(_screens.empty() || _saveFolder.empty()) {
		return;
	}

	std::vector<const GbScreenInfo*> kept;
	for(auto& entry : _screens) {
		if(entry.second.Count >= MinObjectFrames) {
			kept.push_back(&entry.second);
		}
	}
	std::sort(kept.begin(), kept.end(), [](const GbScreenInfo* a, const GbScreenInfo* b) { return a->Count > b->Count; });
	if(kept.size() > 300) {
		kept.resize(300);
	}
	MessageManager::Log("[HD-GB] Écrans de fond : " + std::to_string(kept.size()) + " retenus (seuil " + std::to_string(MinObjectFrames) + " images) sur " + std::to_string(_screenFrames) + " relevés");
	if(kept.empty()) {
		return;
	}

	// Index des tiles (fond et sprites) pour la composition
	std::map<std::tuple<uint64_t, uint32_t, uint8_t>, const Tile*> index;
	for(auto& entry : _paletteTiles) {
		for(const Tile& t : entry.second) {
			uint8_t flags = (t.IsSprite ? 1 : 0) | (t.HFlip ? 2 : 0) | (t.VFlip ? 4 : 0);
			index[std::make_tuple(t.Hash, t.Palette, flags)] = &t;
		}
	}

	std::string tilesDir = FolderUtilities::CombinePath(_saveFolder, "Tiles");
	std::string bgDir = FolderUtilities::CombinePath(tilesDir, "Backgrounds");
	std::string scrDir = FolderUtilities::CombinePath(bgDir, "Screens");
	FolderUtilities::CreateFolder(tilesDir);
	FolderUtilities::CreateFolder(bgDir);
	FolderUtilities::CreateFolder(scrDir);

	int tileSize = 8 * _scale;
	int idx = 0;
	for(const GbScreenInfo* scr : kept) {
		idx++;
		std::string baseName = "ecran_" + std::to_string(idx) + "_x" + std::to_string(scr->Count);
		int canvasW = 20 * tileSize;
		int canvasH = 18 * tileSize;
		std::vector<uint32_t> canvas((size_t)canvasW * canvasH, 0xFF000000);

		std::ostringstream out;
		out << "# Écran " << baseName << " : vu sur " << scr->Count << " images\n";
		out << "# Image entière : Tiles/Backgrounds/Screens/" << baseName << ".png\n";
		out << "# Tiles de l'écran ; [col,row] = case de 8 pixels GB dans l'écran\n";

		for(const GbOamTile& c : scr->Cells) {
			int cx = c.X * tileSize;
			int cy = c.Y * tileSize;
			auto it = index.find(std::make_tuple(c.Hash, c.Palette, c.Flags));
			if(it != index.end()) {
				const Tile* tile = it->second;
				for(int y = 0; y < tileSize; y++) {
					for(int x = 0; x < tileSize; x++) {
						canvas[(size_t)(cy + y) * canvasW + (cx + x)] = tile->Pixels[y * tileSize + x];
					}
				}
			}

			char hashStr[32];
			snprintf(hashStr, sizeof(hashStr), "%016llX", (unsigned long long)c.Hash);
			auto ref = _bgAtlasRef.find(std::make_tuple(c.Hash, c.Palette, c.Flags));
			if(ref == _bgAtlasRef.end()) {
				out << "# tile absente de l'atlas : " << hashStr << "\n";
				continue;
			}
			std::string tag = "<tile";
			if((c.Flags & 2) && (c.Flags & 4)) tag += "_hvflip";
			else if(c.Flags & 2) tag += "_hflip";
			else if(c.Flags & 4) tag += "_vflip";
			tag += ">";
			out << tag << hashStr << "," << c.Palette << "," << ref->second
				<< "   # [" << c.X << "," << c.Y << "]\n";
		}

		if(!PNGHelper::WritePNG(FolderUtilities::CombinePath(scrDir, baseName + ".png"), canvas.data(), canvasW, canvasH, 32)) {
			MessageManager::Log("[HD-GB] Échec d'écriture : " + baseName + ".png");
		}
		FILE* f = fopen(FolderUtilities::CombinePath(scrDir, baseName + ".mesen").c_str(), "wb");
		if(f) {
			std::string text = out.str();
			fwrite(text.data(), 1, text.size(), f);
			fclose(f);
		} else {
			MessageManager::Log("[HD-GB] Échec d'écriture : " + baseName + ".mesen");
		}
	}
}


void GbHdPackBuilder::LogSummary()
{
	size_t unique = 0;
	for(auto& entry : _paletteTiles) {
		unique += entry.second.size();
	}
	MessageManager::Log("[HD-GB] Arrêt : " + std::to_string(_totalCalls) + " tiles vues (appels du hook), " + std::to_string(unique) + " uniques");
	for(auto& entry : _callsPerPalette) {
		size_t uniqueInPal = _paletteTiles.count(entry.first) ? _paletteTiles[entry.first].size() : 0;
		MessageManager::Log("[HD-GB]   palette " + std::to_string(entry.first) + " : " + std::to_string(entry.second) + " appels, " + std::to_string(uniqueInPal) + " uniques");
	}
}

void GbHdPackBuilder::WriteOutput()
{
	if(_saveFolder.empty() || _paletteTiles.empty()) {
		return;
	}

	int tileSize = 8 * _scale;
	int cols = (int)_sheetWidth;

	// Sous-dossiers comme le SNES : Tiles/Backgrounds et Tiles/Sprites
	// create_directory ne crée qu'un niveau : Tiles d'abord, puis les sous-dossiers
	std::string tilesDir = FolderUtilities::CombinePath(_saveFolder, "Tiles");
	std::string bgDir = FolderUtilities::CombinePath(tilesDir, "Backgrounds");
	std::string sprDir = FolderUtilities::CombinePath(tilesDir, "Sprites");
	FolderUtilities::CreateFolder(tilesDir);
	FolderUtilities::CreateFolder(bgDir);
	FolderUtilities::CreateFolder(sprDir);

	std::ostringstream hires;
	hires << "# HD Pack Game Boy généré par le HD Pack Builder (phase 1 : tiles seules)\n";
	hires << "<scale>" << _scale << "\n\n";

	int imageIndex = 0;

	// Deux passes : d'abord tous les décors, puis tous les sprites
	for(int pass = 0; pass < 2; pass++) {
		bool wantSprites = (pass == 1);
		hires << (wantSprites ? "# --- Sprites ---\n" : "# --- Backgrounds ---\n");

		for(auto& entry : _paletteTiles) {
			uint32_t paletteId = entry.first;
			std::vector<Tile>& tiles = entry.second;
			if(tiles.empty() || tiles[0].IsSprite != wantSprites) {
				continue;
			}

			int rows = ((int)tiles.size() + cols - 1) / cols;
			int canvasW = cols * tileSize;
			int canvasH = rows * tileSize;
			std::vector<uint32_t> canvas((size_t)canvasW * canvasH, 0);

			for(size_t i = 0; i < tiles.size(); i++) {
				int col = (int)(i % cols);
				int row = (int)(i / cols);
				int baseX = col * tileSize;
				int baseY = row * tileSize;
				for(int y = 0; y < tileSize; y++) {
					for(int x = 0; x < tileSize; x++) {
						canvas[(size_t)(baseY + y) * canvasW + (baseX + x)] = tiles[i].Pixels[y * tileSize + x];
					}
				}
			}

			std::string kind = wantSprites ? "obj" : "bg";
			std::string pngName = "gb_atlas_" + kind + "_" + std::to_string(paletteId) + ".png";
			std::string dir = wantSprites ? sprDir : bgDir;
			std::string relPath = std::string(wantSprites ? "Tiles/Sprites/" : "Tiles/Backgrounds/") + pngName;
			if(!PNGHelper::WritePNG(FolderUtilities::CombinePath(dir, pngName), canvas.data(), canvasW, canvasH, 32)) {
				MessageManager::Log("[HD-GB] Échec d'écriture : " + FolderUtilities::CombinePath(dir, pngName));
			}

			if(wantSprites) {
				// Tiles des objets d'abord, dans l'ordre des objets ; les autres gardent l'ordre de capture
				auto orderOf = [](const Tile& t) -> uint64_t {
					uint8_t flags = 1 | (t.HFlip ? 2 : 0) | (t.VFlip ? 4 : 0);
					auto it = _spriteOrder.find(std::make_tuple(t.Hash, t.Palette, flags));
					return it == _spriteOrder.end() ? (1ULL << 40) : it->second;
				};
				std::stable_sort(tiles.begin(), tiles.end(), [&](const Tile& a, const Tile& b) {
					return orderOf(a) < orderOf(b);
				});
			}

			hires << "<img>" << relPath << "\n";
			for(size_t i = 0; i < tiles.size(); i++) {
				int col = (int)(i % cols);
				int row = (int)(i / cols);
				const Tile& t = tiles[i];

				std::string tag = "<tile";
				if(t.HFlip && t.VFlip) tag += "_hvflip";
				else if(t.HFlip) tag += "_hflip";
				else if(t.VFlip) tag += "_vflip";
				tag += ">";

				char hashStr[32];
				snprintf(hashStr, sizeof(hashStr), "%016llX", (unsigned long long)t.Hash);

				hires << tag << hashStr << "," << paletteId << "," << imageIndex << ","
				      << (col * tileSize) << "," << (row * tileSize) << "\n";
				      if(!wantSprites) {
				      	uint8_t flags = (t.HFlip ? 2 : 0) | (t.VFlip ? 4 : 0);
				      	_bgAtlasRef[std::make_tuple(t.Hash, t.Palette, flags)] =
				      		relPath + "," + std::to_string(col * tileSize) + "," + std::to_string(row * tileSize);
				      }
				      if(wantSprites) {
				      	uint8_t flags = 1 | (t.HFlip ? 2 : 0) | (t.VFlip ? 4 : 0);
				      	_spriteAtlasRef[std::make_tuple(t.Hash, t.Palette, flags)] =
				      		relPath + "," + std::to_string(col * tileSize) + "," + std::to_string(row * tileSize);
				      }
			}
			hires << "\n";
			imageIndex++;
		}
		hires << "\n";
	}

	std::ofstream file(FolderUtilities::CombinePath(_saveFolder, "gb_hires.txt"));
	file << hires.str();
}
