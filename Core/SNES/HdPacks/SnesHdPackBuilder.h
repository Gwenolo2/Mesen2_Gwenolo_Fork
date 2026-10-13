#pragma once
#include "SNES/HdPacks/SnesHdData.h"
#include "Shared/SettingTypes.h"
#include <cstdint>
#include <sstream>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>




struct HdMetadataItem
{
	std::string Type;        // "Sprite", "Background", "Mode7Tile", "Mode7Map"
	std::string Filename;    // Chemin du PNG généré
	std::string Hash;        // Hash CRC32 ou autre de la tuile
	int GrabX = 0;           // Coordonnée X à l'écran ou dans la VRAM lors du dump
	int GrabY = 0;           // Coordonnée Y à l'écran ou dans la VRAM lors du dump
	uint32_t GrabFrame = 0;  // Frame d'apparition
	std::string SnesHiresLine;
};

//--------------------------------------------------------------
struct SnesHdTileImage {
  uint64_t MemoryHash;
  std::vector<uint32_t> Pixels;
  bool IsSprite;

  // data related to context when the tile is grabbed to memory prior to flush it to PNG File
  uint32_t FrameSeen;
  int ScreenX;
  int ScreenY;

  bool IsHFlip = false;
  bool IsVFlip = false;

  // On va stocker l'index VRAM (ou l'adresse) de cette tuile pour les trier
  uint16_t VramTileIndex;
};
//-------------------------------------------------------------
class SnesHdPackBuilder {

	struct PngReference
	{
		int ImageIndex;
		int X;
		int Y;
	};

	struct PreMappedTile
	{
		uint64_t MemoryHash;
		uint32_t PaletteIndex;
		int ImageIndex;
		int X;
		int Y;
		bool IsHFlip;
		bool IsVFlip;
	};

	// --- À la place de std::vector<std::string> _smartRebuildImages; ---
	struct SmartRebuildImage
	{
		std::string OriginalAbsPath;
		std::string OriginalRelativePath;
		std::string RebuildRelativePath;
	};

public:


  static std::unordered_map<uint64_t, PngReference> _smartRebuildMap;

  //static std::vector<std::string> _smartRebuildImages;
  static std::vector<SmartRebuildImage> _smartRebuildImages;

  static std::map<int, std::set<std::pair<int, int>>> _usedSmartBlocks;

  static std::vector<PreMappedTile> _preMappedTiles;

  static void ProcessTile(const uint8_t *tileData, size_t dataSize,
                          uint32_t paletteIndex, SnesBpp bpp, bool isLargeTile,
                          const uint16_t *cgram, bool isSprite, uint16_t vramTileIndex,
								  int screenX, int screenY, uint32_t frameCount, 
								  bool hMirror = false, bool vMirror = false);

  static bool IsActive();
  static void StartRecording(HdPackBuilderOptions options);
  static void StopRecording();



  // added by Gwenolo
  static uint32_t CalculateMode7Hash(uint8_t *vram, uint16_t *cgram);

  // Le "hook" statique appelé depuis SnesPpu.cpp
  static void ProcessMode7(uint16_t *vram, uint16_t *cgram);

  // --- VARIABLES POUR LES INFOS ROM ---
  inline static std::string _romInfoTxt = "";
  inline static std::string _romInfoJson = "";

  inline static std::string _SNesHDPackVersion = "SNES_101";




private:
  static bool _isActive;
  static HdPackBuilderOptions _options;
  static std::string _saveFolder;
  static std::unordered_set<SnesHdTileKey> _processedTiles;
  static std::unordered_map<uint32_t, std::vector<SnesHdTileImage>>
      _paletteTiles;

  // to collect meta data in Json files when building the snes_hires.txt
  inline static std::vector<HdMetadataItem> _metadataCollection;




  static void DumpMode7Texture(uint16_t *vram, uint16_t *cgram, uint32_t hash,
                               uint32_t scale);


  static void DumpMode7Tileset(uint16_t* vram, uint16_t* cgram, uint32_t hash, uint32_t scale);

  // Stocke les hash des textures Mode 7 déjà extraites pour éviter les doublons
  static std::unordered_set<uint32_t> _m7TexturesDone;
  static std::unordered_set<uint32_t> _m7TilesetsDone;

  //std::unordered_set<uint32_t> _dumpedM7Tiles;

  // --- CACHE POUR LES TUILES Mode 7 INDIVIDUELLES ---
  static std::unordered_set<uint32_t> _m7SingleTilesDone;

  // Stocke les lignes de texte qui devront être écrites dans hires.txt
  static std::stringstream _m7Rows;


  static void SaveTilePng(const uint8_t *tileData, size_t dataSize,
                          uint64_t memoryHash, uint32_t paletteIndex,
                          SnesBpp bpp, bool isLargeTile, const uint16_t *cgram,
                          bool isSprite, uint16_t vramTileIndex,
								  int screenX, int screenY, uint32_t frameCount, 
								  bool hMirror, bool vMirror);
};
