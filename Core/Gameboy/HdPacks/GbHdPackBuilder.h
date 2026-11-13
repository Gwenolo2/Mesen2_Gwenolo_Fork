#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "Shared/SettingTypes.h"
#include <unordered_map>
#include <utility>

// HD Pack Builder Game Boy / Game Boy Color (phase 1 : extraction des tiles).
// Modèle : SnesHdPackBuilder. Conventions de palette (decision D8, doc 30) :
//   fond DMG = valeur de BGP (0-255) ; objets DMG = 256 + valeur de OBP0/OBP1
//   fond GBC = 0-7 ; objets GBC = 8-15
// Sortie : gb_hires.txt (D7) + un atlas PNG par palette.
struct GbOamTile {
	int16_t X;        // coin haut-gauche à l'écran (pixels GB)
	int16_t Y;
	uint64_t Hash;
	uint32_t Palette;
	uint8_t Flags;    // 1 = objet, 2 = flip H, 4 = flip V
};

class GbHdPackBuilder {
public:
	static bool IsActive();
	static void StartRecording(HdPackBuilderOptions options);
	static void StopRecording();

	// tileData : 16 octets bruts de la tile en VRAM (non retournés).
	// colors : 4 couleurs BGR555 (couleur par index 0-3, déjà résolues par la palette).
	static void ProcessTile(const uint8_t* tileData, uint32_t paletteId, const uint16_t* colors, bool isSprite, bool hFlip, bool vFlip);

	// Relevé OAM d'une frame (appelé en fin de frame pendant l'enregistrement)
	static void SnapshotFrame(const uint8_t* oam, const uint8_t* vram, bool large, bool cgb, uint8_t obp0, uint8_t obp1);
	static void SnapshotBackground(const uint8_t* vram, bool mapHigh, bool tileUnsigned, int scx, int scy, bool cgb, uint8_t bgp);

	static constexpr size_t MaxSnapshots = 18000;
	static constexpr int MinObjectFrames = 3;

private:
	struct Tile {
		uint64_t Hash;
		uint32_t Palette;
		bool IsSprite;
		bool HFlip;
		bool VFlip;
		std::vector<uint32_t> Pixels; // ARGB, retournée comme à l'écran, à l'échelle
	};

	static bool _isActive;
	static HdPackBuilderOptions _options;
	static std::string _saveFolder;
	static uint32_t _sheetWidth;
	static int _scale;
	static std::mutex _mutex;
	static std::set<std::tuple<uint64_t, uint32_t, uint8_t>> _processed;
	static std::map<uint32_t, std::vector<Tile>> _paletteTiles;
	static uint64_t _totalCalls;
	static std::map<uint32_t, uint64_t> _callsPerPalette;

	static void LogSummary();
	static void BuildObjects();
	static std::vector<std::vector<GbOamTile>> _frameSnapshots;
	// Rang de chaque tile de sprite dans l'ordre des objets (tiles d'un même objet consécutives)
	static std::map<std::tuple<uint64_t, uint32_t, uint8_t>, uint64_t> _spriteOrder;

	struct GbObjectInfo {
		std::string Name;   // obj_N_xCOUNT (sans extension)
		int Count;
		std::vector<GbOamTile> Members;
	};
	static std::vector<GbObjectInfo> _objects;
	// Référence atlas par tile de sprite : "chemin.png,x,y" (en pixels HD)
	static std::map<std::tuple<uint64_t, uint32_t, uint8_t>, std::string> _spriteAtlasRef;
	static void WriteObjectFiles();

	struct GbScreenInfo {
		int Count = 0;
		std::vector<GbOamTile> Cells; // X = colonne, Y = ligne (cases de 8 pixels)
	};
	static std::map<uint64_t, GbScreenInfo> _screens;
	static size_t _screenFrames;
	static std::map<std::tuple<uint64_t, uint32_t, uint8_t>, std::string> _bgAtlasRef;
	static void WriteScreenFiles();

	static void WriteOutput();
};
