#pragma once
#include "SNES/HdPacks/SnesHdData.h"
#include "Shared/SettingTypes.h"
#include <cstdint>
#include <sstream>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <tuple>
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
  static void RequestFrameGrab();
  static bool CanCapture(uint32_t frameCount);
  static bool _grabRequested;
  static bool _grabActive;
  static uint32_t _grabFrame;
  static uint32_t _lastFrame;



  // added by Gwenolo
  static uint32_t CalculateMode7Hash(uint8_t *vram, uint16_t *cgram);

  // Le "hook" statique appelé depuis SnesPpu.cpp
  static void ProcessMode7(uint16_t *vram, uint16_t *cgram);

  // --- HD PACK SNES : objets composes (une entree OAM = un objet) ---
  struct SnesObjMember {
    int X = 0;                    // position d'affichage dans l'objet (pixels SNES)
    int Y = 0;
    uint64_t Hash = 0;            // hash 31 des 32 octets 4bpp (meme calcul que le loader)
    uint32_t Palette = 8;         // palette D8 (8 a 15 pour les sprites)
    bool HFlip = false;
    bool VFlip = false;
    uint16_t VramAddr = 0;        // adresse VRAM de la tile (source, sans flip)
    std::vector<uint32_t> Pixels; // 8x8 ARGB, orientation source (sans flip)
  };
  struct SnesObjEntry {
    int W = 0;
    int H = 0;
    int Count = 0;                // nombre d'images ou l'objet est apparu
    std::vector<SnesObjMember> Members;
  };
  static constexpr int SnesMinObjectFrames = 3;
  static constexpr size_t MaxSnesObjectsWritten = 300;
  static constexpr size_t MaxSnesObjectKinds = 1500;
  inline static std::unordered_map<std::string, SnesObjEntry> _snesObjects;
  inline static std::map<std::string, int> _snesObjSizes;
  inline static int _snesObjOffscreen = 0;
  inline static int _snesObjCapped = 0;
  inline static int _snesObjSnapshots = 0;
  inline static int _snesObjMerged = 0;   // objets formes de plusieurs entrees OAM (vues)
  inline static int _snesObjSplit = 0;    // groupes trop grands, decoupes en entrees
  static constexpr int SnesMaxClusterPx = 256; // taille max d'un objet compose (px SNES)
  static constexpr int SnesClusterGapPx = 8;   // ecart max entre deux entrees OAM pour les regrouper
  static void ProcessObjects(const uint8_t *oam, const uint16_t *vram, const uint16_t *cgram,
                             uint8_t oamMode, uint16_t oamBase, uint16_t oamGap);
  static void WriteObjectFiles();

  // --- HD PACK SNES : ecrans de fond (equivalent de GbHdPackBuilder::SnapshotBackground) ---
  struct SnesHdBgLayerInfo {
    uint16_t Tilemap = 0;
    uint16_t Chr = 0;
    uint16_t HScroll = 0;
    uint16_t VScroll = 0;
    bool DoubleWidth = false;
    bool DoubleHeight = false;
    bool LargeTiles = false;
  };
  struct SnesScreenCell {
    int Col = 0;
    int Row = 0;
    uint64_t Hash = 0;
    uint32_t Palette = 0;
    uint8_t Bpp = 4;
    bool HFlip = false;
    bool VFlip = false;
  };
  struct SnesScreenInfo {
    int Layer = 0;                  // calque BG (0 = BG1 ... 3 = BG4)
    int Count = 0;
    std::vector<SnesScreenCell> Cells; // 32x28 cases de 8 pixels
  };
  static constexpr int SnesMaxScreenKinds = 800;
  static constexpr size_t SnesMaxBgTiles = 16384;
  inline static std::map<uint64_t, SnesScreenInfo> _snesScreens;
  inline static std::map<std::tuple<uint64_t, uint32_t, uint8_t>, std::vector<uint32_t>> _snesBgTiles;
  inline static size_t _snesScreenFrames = 0;
  inline static int _snesScreenCapped = 0;
  inline static int _snesScreenSkipped = 0; // calques 16x16 ou mode 7 non releves
  static void ProcessScreens(const uint16_t* vram, const uint16_t* cgram, uint8_t bgMode,
                             uint8_t mainLayers, const SnesHdBgLayerInfo* layers);
  static void WriteScreenFiles();

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


  static uint32_t _sheetWidth;
  static uint32_t _sheetHeight;

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
