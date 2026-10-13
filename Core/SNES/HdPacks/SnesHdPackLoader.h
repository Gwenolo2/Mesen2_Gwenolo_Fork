#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "SNES/HdPacks/SnesHdData.h"
#include "SnesHdPackConditions.h"


// ---------------------------------------------------------------------------
// Pixel buffer for a loaded sprite sheet
// ---------------------------------------------------------------------------
struct SnesHdSheetImage {
    std::vector<uint32_t> Pixels; // RGBA, row-major
    int Width  = 0;
    int Height = 0;
};

class SnesHdPackLoader {
public:
    // -----------------------------------------------------------------------
    // Static API – called from the PPU
    // -----------------------------------------------------------------------

    /// Load a pack from a snes_hires.txt file. Fills outData and populates
    /// the internal pixel caches.  Returns false on failure.
	 static bool LoadHdPack(const std::string& definitionFile, std::shared_ptr<SnesHdPackData>& outData);
   
	 
	 /// Clear all loaded data (call when a game is unloaded).
    static void Clear();

    // Fonction de recherche rapide
	 static SnesHdPackTileInfo* GetHdTile(uint64_t hash, uint32_t palette, SnesBpp bpp, bool isLarge, bool isSprite, bool isHFlip = false, bool isVFlip = false, int x = 0, int y = 0);

    /// Return the sheet pixel buffer for a given image index.
	 static SnesHdPackBitmapInfo* GetSheet(uint32_t imageIndex);

    /// Whether a pack is currently loaded and enabled.
    static bool IsLoaded() { return _isLoaded; }


	 // Pointeur global vers notre pack actif
	 static std::shared_ptr<SnesHdPackData> ActivePack;

	 //static void ProcessDisableTileTag(const std::vector<std::string>& tokens, const std::vector<SnesHdPackCondition*>& conditions, std::shared_ptr<SnesHdPackData>& outData, int currentLine);
	 static void ProcessDisableTileTag(const std::vector<std::string>& tokens, const std::vector<SnesHdPackCondition*>& conditions, std::shared_ptr<SnesHdPackData>& outData, const std::string& lineLocation);



	 static HdPackConditionOperator ParseConditionOperator(std::string& opString);

    // -----------------------------------------------------------------------
    // Internal – used by the instance created in LoadHdPack
    // -----------------------------------------------------------------------
private:
    SnesHdPackData* _data = nullptr;
    std::string _hdPackFolder;
    uint32_t _currentLine = 0;
    int _errorCount       = 0;

    bool LoadPack(const std::string& definitionFile);
    void ProcessScaleTag(const std::string& content);
    bool ProcessImgTag(const std::string& content);
	 void ProcessTileTag(const std::vector<std::string>& tokens, const std::vector<SnesHdPackCondition*>& conditions);
    void LogError(const std::string& msg);


	 

    // -----------------------------------------------------------------------
    // Static storage – shared across the life-time of the emulator session
    // -----------------------------------------------------------------------
    static bool _isLoaded;

    // Fast look-up:  (MemoryHash << 32 | PaletteIndex) → SnesHdTile
    // We flatten key.Bpp / IsLargeTile / IsSprite because they are NOT
    // written to snes_hires.txt; we only match by hash+palette.
    struct FlatKey {
        uint64_t MemoryHash;
        uint32_t PaletteIndex;
        bool operator==(const FlatKey& o) const noexcept {
            return MemoryHash == o.MemoryHash && PaletteIndex == o.PaletteIndex;
        }
    };
    struct FlatKeyHash {
        size_t operator()(const FlatKey& k) const noexcept {
            return std::hash<uint64_t>{}(k.MemoryHash) ^ (std::hash<uint32_t>{}(k.PaletteIndex) << 1);
        }
    };

	 
};
