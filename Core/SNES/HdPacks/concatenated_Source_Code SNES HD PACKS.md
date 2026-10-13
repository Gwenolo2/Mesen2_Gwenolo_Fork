# Concaténation du code source

Ce fichier contient le code source d'une partie du repository. Il présente le contenu dans un fichier MD la concaténation des fichiers : `*.cpp,*.h`.
Chaque section indique le nom du fichier dans le repo.

---

## ./SnesHdData.h

```h
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set> // Ajouté pour WatchedMemoryAddresses
#include <vector>
#include "Shared/MessageManager.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/SimpleLock.h"
#include "Shared/SettingTypes.h"
#include "Shared/pl_mpeg.h"

// ---------------------------------------------------------
// FORWARD DECLARATIONS (Pour éviter les dépendances circulaires)
// ---------------------------------------------------------
struct SnesHdPackCondition;
struct SnesHdPackTileInfo;
class SnesHdPackData;

class SnesPpu;
struct SnesPpuState;
struct SnesHdPixelData;


// Enumération pour le mode de lecture
enum class SnesHdVideoMode
{
	Overlay,
	Pause
};
//-------------------------------------------------------
// La structure qui va stocker les infos du tag <video>
struct SnesHdVideoInfo
{
	std::string Filename;
	SnesHdVideoMode Mode = SnesHdVideoMode::Overlay;

	uint16_t SkipButtonsMask = 0;

	bool PlayOnlyOnce = false;
	bool MuteAudio = false;

	//  PARAMÈTRES PIP (Picture-in-Picture) ---
	bool Loop = false;
	int X = 0;
	int Y = 0;


	uint8_t ZIndex = 99; // 99 par défaut (tout au-dessus)

	// ----------------------------------------------------

	// --- VARIABLES D'ÉTAT EN TEMPS RÉEL ---
	bool HasPlayed = false;
	bool IsPlaying = false;

	// --- VARIABLES DE LECTURE VIDÉO ---
	double TimeAccumulator = 0.0;
	std::vector<uint8_t> CurrentFramePixels; // Va stocker l'image convertie
	int VideoWidth = 0;
	int VideoHeight = 0;

	// LE LECTEUR VIDÉO :
	plm_t* VideoPlayer = nullptr;

	std::vector<SnesHdPackCondition*> Conditions;

	bool MatchesCondition();
};
// ---------------------------------------------------------
enum class SnesBpp { Bpp2, Bpp4, Bpp8, Mode7 };

// ---------------------------------------------------------

// La structure pour stocker les règles de Z-Index pour les layers SNES
struct SnesHdLayerOrderRule
{
	std::vector<SnesHdPackCondition*> Conditions;
	uint8_t ZIndexBg[4] = { 0, 0, 0, 0 };

	// ON NE MET QUE LA DÉCLARATION ICI 
	bool MatchesCondition(int x, int y, struct SnesHdPixelData* tile);
};

// ---------------------------------------------------------
struct SnesHdTileKey
{
	uint64_t MemoryHash;
	uint32_t PaletteHash;
	SnesBpp Bpp;
	bool IsLargeTile;
	bool IsSprite;

	// ==========================================
	// VARIABLES POUR LES SYMÉTRIES
	// ==========================================
	bool IsHFlip = false;
	bool IsVFlip = false;

	// L'opérateur de comparaison doit impérativement utiliser ces nouvelles variables !
	bool operator==(const SnesHdTileKey& other) const
	{
		return MemoryHash == other.MemoryHash &&
			PaletteHash == other.PaletteHash &&
			Bpp == other.Bpp &&
			IsLargeTile == other.IsLargeTile &&
			IsSprite == other.IsSprite &&
			IsHFlip == other.IsHFlip &&
			IsVFlip == other.IsVFlip;
	}
};
//------------------------------------------------------------------
struct SnesHdPackAddition
{
	uint64_t BaseMemoryHash = 0;
	uint32_t BasePaletteHash = 0;
	bool IgnoreBasePalette = false;

	int32_t OffsetX = 0;
	int32_t OffsetY = 0;

	uint64_t AddedMemoryHash = 0;
	uint32_t AddedPaletteHash = 0;

	// Liste des conditions spécifiques à cette addition
	std::vector<SnesHdPackCondition*> Conditions;
};
//-------------------------------------------------------------------
struct ActiveHdAddition
{
	uint64_t MemoryHash;
	uint32_t PaletteHash;
	int ScreenX;
	int ScreenY;

	// On sauvegarde la priorité et la couche de la tuile de base 
	// pour que l'addition passe derrière ou devant les bons décors !
	uint8_t Priority;
	uint8_t LayerIndex;

	// On propage l'état du miroir du sprite parent
	bool HFlip;
	bool VFlip;
};
// ---------------------------------------------------------
namespace std
{
	template <> struct hash<SnesHdTileKey>
	{
		size_t operator()(const SnesHdTileKey& k) const
		{
			size_t h1 = hash<uint64_t>()(k.MemoryHash);
			size_t h2 = hash<uint32_t>()(k.PaletteHash);
			size_t h3 = hash<int>()(static_cast<int>(k.Bpp));
			size_t h4 = hash<bool>()(k.IsLargeTile);
			size_t h5 = hash<bool>()(k.IsSprite);
			size_t h6 = hash<bool>()(k.IsHFlip); 
			size_t h7 = hash<bool>()(k.IsVFlip); 

			// On intègre les deux nouveaux hashs avec des décalages de 5 et 6
			return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4) ^ (h6 << 5) ^ (h7 << 6);
		}
	};
}

// ---------------------------------------------------------
// LE SCREEN INFO (Pour la surveillance de la RAM)
// ---------------------------------------------------------
class SnesPpu;

struct SnesHdScreenInfo
{
	std::unordered_map<uint32_t, uint8_t> WatchedAddressValues;
	uint32_t FrameNumber = 0;

	// AJOUT POUR LES CONDITIONS AVANCÉES SNES ---
	SnesPpu* Ppu = nullptr;

	// Historique du scroll pour chaque scanline (0-239) et chaque calque (0-3)
	int16_t ScanlineHScroll[240][4] = { 0 };
	int16_t ScanlineVScroll[240][4] = { 0 };
};


//------------------------------------------------------------------------
struct SnesHdPackBitmapInfo
{
private:
	bool _initDone = false;
	SimpleLock _lock;
public:
	std::string PngName;
	std::vector<uint8_t> FileData;
	std::vector<uint32_t> PixelData;
	uint32_t Width = 0;
	uint32_t Height = 0;

	void Init()
	{
		if(_initDone) return;
		auto lock = _lock.AcquireSafe();
		if(_initDone) return;

		if(PNGHelper::ReadPNG(FileData, PixelData, Width, Height)) {
			// PremultiplyAlpha() optionnel
		}
		FileData.clear();
		_initDone = true;
	}
};

// ---------------------------------------------------------
// Pour que TileInfo puisse l'utiliser dans ses conditions
// ---------------------------------------------------------

struct SnesHdPackTileInfo;

struct SnesHdPixelData
{
	SnesHdPackTileInfo* Tile = nullptr;
	uint8_t TileX = 0;
	uint8_t TileY = 0;

	bool HFlip = false;
	bool VFlip = false;
	uint16_t BaseColor = 0;// Couleur SNES originale

	//Pour mémoriser ce qu'il y a DERRIÈRE le sprite ou la tile !
	uint16_t UnderlyingColor = 0;

	uint32_t Color = 0;     // Couleur HD 32-bit (ARGB)
	bool HasData = false;   // Est-ce que ce pixel contient de la HD ?

	bool IsSprite = false;

	// Priority VARIABLES
	bool IsHighPriority = false;
	uint8_t SnesPriorityLevel = 0; // De 0 à 15 


	uint8_t OamIndex = 255; // 255 par défaut (ce qui signifie "pas un sprite")


	uint8_t SourceLayer;

	// LE CONTEXTE MATHÉMATIQUE MODE 7 ---
	bool IsMode7 = false;      // Vrai si on est sur la map géante
	int32_t M7XValue = 0;      // La coordonnée X exacte (en virgule fixe 8.8)
	int32_t M7YValue = 0;      // La coordonnée Y exacte (en virgule fixe 8.8)
	int16_t M7A = 0;           // Vecteur X Horizontal
	int16_t M7B = 0;           // Vecteur X Vertical
	int16_t M7C = 0;           // Vecteur Y Horizontal
	int16_t M7D = 0;           // Vecteur Y Vertical
	uint32_t M7MapHash = 0;    // Pour que le filtre retrouve la grande image

	// POUR LES TUILES M7 INDIVIDUELLES ---
	bool IsMode7Tile = false;  // Vrai si on est sur une pièce/bloc Mode 7
	uint32_t M7TileHash = 0;   // Le Hash de la tuile pour la retrouver

	// LA RÈGLE DE DÉBORDEMENT ---
	bool M7LargeMap = false;   // Vrai si le jeu interdit la répétition (Screen Over)



};
// =========================================================
enum class SnesHdPackConditionType
{
	HMirror,
	VMirror,
	FrameRange,
	MemoryCheck,
	MemoryCheckConstant,
	SpriteAtPosition,
	TileAtPosition,
	SpriteOnScreen,
	TileOnScreen,
	TileNearby,    
	SpriteNearby
};
//-----------------------------------------------------------------------
struct SnesHdPackCondition
{
protected:
	SnesHdScreenInfo* _screenInfo = nullptr;
	SnesHdPackData* _hdPack = nullptr;

public:
	std::string Name;

	virtual SnesHdPackConditionType GetConditionType() = 0;
	virtual std::string GetConditionName() = 0;
	virtual bool IsExcludedFromFile() { return Name.size() > 0 && Name[0] == '!'; }
	virtual std::string ToString() = 0;

	virtual ~SnesHdPackCondition() {}
	//--------------------------
	void Initialize(SnesHdScreenInfo* screenInfo, SnesHdPackData* hdPack)
	{
		_screenInfo = screenInfo;
		_hdPack = hdPack;
		_resultCache = -1;
	}
	//--------------------------
	bool CheckCondition(int x, int y, SnesHdPixelData* tile)
	{
		if(_resultCache >= 0) {
			return (bool)_resultCache;
		}

		bool result = InternalCheckCondition(x, y, tile);
		if(Name[0] == '!') {
			result = !result;
		}

		if(_useCache) {
			_resultCache = result ? 1 : 0;
		}
		return result;
	}
	//--------------------------
	void ResetCache()
	{
		_resultCache = -1;
	}

protected:
	int8_t _resultCache = -1;
	bool _useCache = false;

	virtual bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) = 0;
};



// =========================================================
// ---------------------------------------------------------
struct SnesHdPackTileInfo : public SnesHdTileKey
{
private:
	bool _needInit = true;
public:
	uint32_t X = 0;
	uint32_t Y = 0;
	uint32_t Width = 8;
	uint32_t Height = 8;
	bool BeyondPriority = false;

	bool IsHFlip = false;
	bool IsVFlip = false;

	// Flag for <disableTile> tag
	bool IsDisabled = false;

	//Option pour désactiver la lumière dynamique sur cette tuile
	bool IgnoreColorMath = false;

	SnesHdPackBitmapInfo* Bitmap = nullptr;
	std::vector<uint32_t> HdTileData;

	//----------  CONDITIONS  -------------------------
	//  La liste des conditions rattachées à cette tuile !
	std::vector<SnesHdPackCondition*> Conditions;

	bool MatchesCondition(int x, int y, SnesHdPixelData* tile)
	{
		for(SnesHdPackCondition* condition : Conditions) {
			if(!condition->CheckCondition(x, y, tile)) {
				return false; // Si une seule condition échoue, la tuile est rejetée !
			}
		}
		return true; // Toutes les conditions sont validées
	}
	// ---------------------------------



	bool NeedInit() { return _needInit; }

	void Init(uint32_t scale)
	{
		if(!_needInit) return;
		_needInit = false;
		Bitmap->Init();

		if(Bitmap->PixelData.empty()) {
			static int errCount = 0;
			if(errCount < 5) {
				MessageManager::Log("[HD TRACE ASYNC] FAILURE : " + Bitmap->PngName + " Pixels are empty !");
				errCount++;
			}
			return;
		}

		uint32_t scaledWidth = Width * scale;
		uint32_t scaledHeight = Height * scale;
		uint32_t bitmapOffset = (Y * Bitmap->Width) + X;

		uint32_t* pngData = Bitmap->PixelData.data();
		HdTileData.resize(scaledWidth * scaledHeight);

		if(Bitmap->PixelData.size() >= bitmapOffset + ((scaledHeight - 1) * Bitmap->Width) + scaledWidth) {
			for(uint32_t y = 0; y < scaledHeight; y++) {
				memcpy(HdTileData.data() + (y * scaledWidth), pngData + bitmapOffset, scaledWidth * sizeof(uint32_t));
				bitmapOffset += Bitmap->Width;
			}
		}
	}
};
//----------------------------------------------------------
struct SnesHdBackgroundInfo
{
	SnesHdPackBitmapInfo* Bitmap = nullptr;
	float Brightness = 1.0f;
	float ScrollXRatio = 0.0f;
	float ScrollYRatio = 0.0f;
	int ZIndex = 0;
	int OffsetX = 0;
	int OffsetY = 0;
	int MasterScrollLayer = 1; // De 1 à 4 (Pour BG1, BG2, BG3, BG4)

	// will the background be tileable?
	bool TileX = false;
	bool TileY = false;

	// --- CHAMPS POUR L'ANIMATION ---
	bool IsAnimated = false;
	std::vector<SnesHdPackBitmapInfo*> AnimFrames;
	int FramesBetweenSteps = 1;
	bool ReverseAnim = false;

	std::vector<SnesHdPackCondition*> Conditions;

	bool MatchesCondition()
	{
		for(auto* cond : Conditions) {
			if(!cond->CheckCondition(0, 0, nullptr)) return false;
		}
		return true;
	}
};

//----------------------------------------------------------------
struct SnesHdMode7Image
{
	SnesHdPackBitmapInfo* Bitmap = nullptr;
	uint32_t Scale = 1;
	
	bool IgnoreColorMath = false;
	bool UseBilinearFilter = false;

	std::vector<std::vector<uint32_t>> Mipmaps;

	//coordonates in pixel in PNG file for the tile
	int X = 0;
	int Y = 0;


	std::vector<SnesHdPackCondition*> Conditions;

	// Fonction qui vérifie si toutes les conditions de cette image sont remplies
	bool MatchesCondition()
	{
		for(auto cond : Conditions) {
			// On passe (0, 0, nullptr) car les conditions Mode 7 (comme l'heure, 
			// la mémoire ou la frame) ne dépendent généralement pas d'un pixel précis.
			if(!cond->CheckCondition(0, 0, nullptr)) {
				return false;
			}
		}
		return true; // Si aucune condition n'a échoué, on affiche l'image !
	}


	SnesHdPixelData GetPixel(int x, int y)
	{
		if(!Bitmap || Bitmap->PixelData.empty()) return {};

		// x et y sont les coordonnées SNES (0-1023)
		// On les multiplie par l'échelle pour taper dans l'image HD
		uint32_t sx = (uint32_t)x * Scale;
		uint32_t sy = (uint32_t)y * Scale;

		if(sx < Bitmap->Width && sy < Bitmap->Height) {
			SnesHdPixelData data;
			data.Color = Bitmap->PixelData[sy * Bitmap->Width + sx];
			data.HasData = true;
			return data;
		}
		return {};
	}
};
//----------------------------------------------------------
struct SnesHdMsuTrack
{
	std::string Filename;
	uint32_t LoopPoint = 0;

	// AJOUT DES CONDITIONS
	std::vector<SnesHdPackCondition*> Conditions;

	// Fonction qui vérifie si on doit jouer cette piste
	bool MatchesCondition()
	{
		for(auto* cond : Conditions) {
			// On passe (0,0,nullptr) car les conditions audio (mémoire, frames) n'ont pas besoin de pixel X/Y
			if(!cond->CheckCondition(0, 0, nullptr)) return false;
		}
		return true;
	}
};


// ---------------------------------------------------------
class SnesHdPackData
{
private:
	bool _cancelLoad = false;
public:
	uint32_t Scale = 1;
	bool DisableM7TextureDump = false;

	bool DisableOriginalTiles = false;
	bool DisableOriginalSprites = false;

	bool DisablePlainTiles = false;

	std::string RootFolder = "";

	bool DisableBg1 = false;
	bool DisableBg2 = false;
	bool DisableBg3 = false;
	bool DisableBg4 = false;

	// Booléens pour savoir si le fichier texte contient le tag
	bool HasDisableBg1Tag = false;
	bool HasDisableBg2Tag = false;
	bool HasDisableBg3Tag = false;
	bool HasDisableBg4Tag = false;


	// Booléen évalué à chaque frame
	bool IsColorMathDisabled = false;

	bool HasDisableColorMathTag = false; 


	bool HasDisableSubLayerTag = false;
	

	// Variable mise à jour à chaque frame
	bool IsSubLayerDisabled = false;


	// --- OVERSCAN ---
	bool HasOverscanConfig = false;
	OverscanDimensions Overscan = { 0, 0, 0, 0 };

	std::vector<std::unique_ptr<SnesHdPackBitmapInfo>> ImageFileData;
	std::unordered_map<SnesHdTileKey, std::vector<SnesHdPackTileInfo*>> TileByKey;

	// Stockage global des conditions et adresses surveillées
	std::vector<std::unique_ptr<SnesHdPackCondition>> Conditions;
	std::unordered_set<uint32_t> WatchedMemoryAddresses;
	
	// Stockage des conditions rattachées au tag <disableColorMath>
	std::vector<SnesHdPackCondition*> DisableColorMathConditions;


	std::vector<SnesHdPackCondition*> DisableBg1Conditions;
	std::vector<SnesHdPackCondition*> DisableBg2Conditions;
	std::vector<SnesHdPackCondition*> DisableBg3Conditions;
	std::vector<SnesHdPackCondition*> DisableBg4Conditions;
	std::vector<SnesHdPackCondition*> DisableSubLayerConditions;

	std::vector<SnesHdLayerOrderRule> LayerOrders;


	// La liste "maître" qui possède les objets pour nettoyer la RAM à la fin
	std::vector<std::unique_ptr<SnesHdPackTileInfo>> Tiles;

	SnesHdScreenInfo ScreenInfo;

	std::vector<std::string> PngFilePaths;


	// Le catalogue global des additions lues dans le fichier texte
	std::unordered_map<uint64_t, std::vector<SnesHdPackAddition>> Additions;

	// Liste des additions qui doivent être dessinées sur la frame en cours
	std::vector<ActiveHdAddition> ActiveAdditionsQueue;

	// Le dictionnaire qui fait le lien entre la Piste (1, 2, 3...) et l'audio HD
	// Avant : std::unordered_map<int, SnesHdMsuTrack> MsuTracks;
	// MAINTENANT : Un ID de piste pointe vers une liste de choix possibles !
	std::unordered_map<int, std::vector<SnesHdMsuTrack>> MsuTracks;

	// Note: il est utile de stocker le chemin de base du pack pour charger les fichiers audio
	std::string PackFolderPath;



	// Dictionnaire des Maps Mode 7 (Clé = Hash de la Tilemap)
	// -->Un Hash pointe désormais vers une liste d'images (pour la gestion des conditions)
	std::unordered_map<uint32_t, std::vector<std::unique_ptr<SnesHdMode7Image>>> Mode7Maps;

	// Nos backgrounds triés et prêts à l'emploi
	std::vector<std::unique_ptr<SnesHdBackgroundInfo>> Backgrounds;

	// MPEG1 videos
	std::vector<std::unique_ptr<SnesHdVideoInfo>> Videos;

	void LoadAsync()
	{
		for(auto& bitmap : ImageFileData) {
			bitmap->Init();
			if(_cancelLoad) return;
		}

		for(auto& entry : TileByKey) {
			for(auto* tile : entry.second) {
				tile->Init(Scale);
				if(_cancelLoad) return;
			}
		}
		MessageManager::Log("[HD TRACE SYNCHRON] SUCCESS : Hash of tile success !");
	}



	// --- MODE 7 FUNCTIONS  ---

	 // 1. Chercher une Map HD complète
	SnesHdMode7Image* GetMode7Image(uint32_t mapHash)
	{
		auto it = Mode7Maps.find(mapHash);
		if(it != Mode7Maps.end()) {
			// On parcourt toutes les images associées à ce Hash
			for(auto& img : it->second) {
				// Si l'image n'a pas de condition, ou si sa condition est remplie
				if(img->MatchesCondition()) {
					return img.get();
				}
			}
		}
		return nullptr; // Aucune image valide pour l'état actuel du jeu
	}



	// 2. Chercher un pixel dans une tuile HD (Mario Kart)
	SnesHdPixelData GetHdTilePixel(uint64_t tileHash, int x, int y)

	{
		SnesHdTileKey key;
		key.MemoryHash = tileHash;
		key.PaletteHash = 0;
		key.Bpp = SnesBpp::Mode7;
		key.IsLargeTile = false;
		key.IsSprite = false;

		auto it = TileByKey.find(key);
		if(it != TileByKey.end() && !it->second.empty()) {

			// --- AJOUT : On boucle sur toutes les tuiles partageant ce Hash ---
			for(SnesHdPackTileInfo* tile : it->second) {

				// On vérifie si la condition de cette tuile est valide
				if(tile->MatchesCondition(x, y, nullptr)) {

					uint32_t sx = (uint32_t)x * Scale;
					uint32_t sy = (uint32_t)y * Scale;

					if(!tile->HdTileData.empty()) {
						SnesHdPixelData data;
						data.Color = tile->HdTileData[sy * (tile->Width * Scale) + sx];
						data.HasData = true;
						data.Tile = tile;
						return data;
					}
				}
			}
		}
		return {};
	}


	//------------------------------------------------
	void CancelLoad() { _cancelLoad = true; }

	//------------------------------------------------
	// --- DESTRUCTEUR ---
	~SnesHdPackData()
	{
		// On nettoie les lecteurs vidéo proprement !
		for(auto& video : Videos) {
			if(video->VideoPlayer != nullptr) {
				plm_destroy(video->VideoPlayer);
				video->VideoPlayer = nullptr;
			}
		}
	}


};
```

---

## ./SnesHdPackBuilder.cpp

```cpp
#include "pch.h"
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <algorithm>

#include "SNES/HdPacks/SnesHdPackBuilder.h"
#include "Utilities/CRC32.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/PNGHelper.h"

#include "Utilities/StringUtilities.h"

#include <string>


#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

std::string GetCurrentDateTimeString()
{
	auto now = std::chrono::system_clock::now();
	std::time_t time = std::chrono::system_clock::to_time_t(now);
	std::tm tm_local;

#if defined(_WIN32)
	// Version sécurisée pour Windows / Visual Studio
	localtime_s(&tm_local, &time);
#else
	// Version sécurisée pour Mac / Linux (POSIX)
	localtime_r(&time, &tm_local);
#endif

	std::stringstream ss;
	ss << std::put_time(&tm_local, "%Y-%m-%d %H:%M");
	return ss.str();
}


/* ============================================================================
 * SNES MODE 7 HD PACK BUILDER - IMPLEMENTATION SUMMARY
 * ============================================================================
 * * This module implements the extraction (dumping) of Mode 7 graphics for
 * HD Pack creation. Unlike standard SNES backgrounds (which use bitplanes),
 * Mode 7 treats the first 32KB of VRAM as a 1024x1024 chunky pixel map
 * (1 byte = 1 pixel color index).
 * 
 * * KEY ARCHITECTURE & LOGIC:
 * 
 * * 1. PPU Hook & Detection:
 * - The extraction is triggered at the end of the frame (VBlank/NMI) in
 * SnesPpu::ProcessEndOfScanline.
 * - We only dump if Mode 7 was actually rendered during the frame
 * (using a tracking boolean `_mode7UsedThisFrame`), bypassing aggressive
 * mid-screen `ForcedBlank` optimizations used by games like Super Mario Kart.
 * 
 * * 2. Anti-Combinatorial Hashing (The "Coin" Problem):
 * - Games like Super Mario Kart draw dynamic elements (coins, ? blocks)
 * directly into the Mode 7 tilemap. Hashing the entire 32KB VRAM would
 * generate a new PNG every time a coin is collected or animated (millions of files).
 * - SOLUTION: We generate a "Track Signature Hash" using only the first 512
 * bytes of VRAM (the top-left corner of the map, usually static background
 * like water/grass). This uniquely identifies the track without being
 * affected by gameplay elements.
 * 
 * * 3. Dual-Output Generation:
 *  * To allow artists to properly redraw dynamic elements, two files are dumped per track:
 * - [m7_HASH.png]: The full 1024x1024 map (scaled). Artists use this to paint
 * a clean, static background track (erasing baked-in coins/items).
 * - [m7tiles_HASH.png]: A 16x16 grid containing all 256 unique 8x8 Mode 7 tiles.
 * Artists use this to provide HD versions of dynamic elements (coins).
 * The rendering engine will superimpose these HD tiles over the static map.
 * 
 * * 4. Static Class Design:
 * - The builder operates using static variables (e.g., `_m7TexturesDone`, `_m7Rows`).
 * - Tags (`<m7img>`, `<m7tiles>`) are buffered in a std::stringstream and written
 * to `snes_hires.txt` during StopRecording().
 * - Crucially, static buffers are cleared during StopRecording() to prevent
 * memory leaks and cross-game contamination.
 * ============================================================================
 */



// Définition des variables statiques
std::unordered_set<uint32_t> SnesHdPackBuilder::_m7TexturesDone;
std::stringstream SnesHdPackBuilder::_m7Rows;
std::unordered_set<uint32_t> SnesHdPackBuilder::_m7TilesetsDone;

bool SnesHdPackBuilder::_isActive = false;
HdPackBuilderOptions SnesHdPackBuilder::_options;
std::string SnesHdPackBuilder::_saveFolder = "";
std::unordered_set<SnesHdTileKey> SnesHdPackBuilder::_processedTiles;
std::unordered_map<uint32_t, std::vector<SnesHdTileImage>>
    SnesHdPackBuilder::_paletteTiles;


// ==========================================================
// Variables statiques pour le Smart Rebuild
// ==========================================================
std::unordered_map<uint64_t, SnesHdPackBuilder::PngReference> SnesHdPackBuilder::_smartRebuildMap;
//std::vector<std::string> SnesHdPackBuilder::_smartRebuildImages;
std::vector<SnesHdPackBuilder::PreMappedTile> SnesHdPackBuilder::_preMappedTiles;

std::vector<SnesHdPackBuilder::SmartRebuildImage> SnesHdPackBuilder::_smartRebuildImages;
std::map<int, std::set<std::pair<int, int>>> SnesHdPackBuilder::_usedSmartBlocks;

std::unordered_set<uint32_t> SnesHdPackBuilder::_m7SingleTilesDone;

bool SnesHdPackBuilder::IsActive() { return _isActive; }
//--------------------------------------------------------------------------------------------------------

void SnesHdPackBuilder::StartRecording(HdPackBuilderOptions options) {
  _options = options;
  _saveFolder = options.SaveFolder;
  _processedTiles.clear();
  _paletteTiles.clear();
  _isActive = true;

  // Log de confirmation
  std::string logPath =
	  FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
  std::ofstream log(logPath);
  if(log.is_open()) {
	  log << "Recording initialized.\n";
	  log << "Root: " << _saveFolder << "\n";
	  log.close();
  }


  _smartRebuildMap.clear();
  _smartRebuildImages.clear();
  _preMappedTiles.clear();

  // ==========================================================
	 // --- SMART REBUILD : PRÉ-CHARGEMENT DES PNG ---
	 // ==========================================================
	 // On utilise des noms de variables uniques pour ne pas entrer en conflit avec la suite de ta fonction
  std::string smartLogPath = FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
  std::ofstream smartLog(smartLogPath, std::ios::app);

  if(_options.BuildFromPngFolder && !_saveFolder.empty()) {
	  if(smartLog.is_open()) smartLog << "\n--- SMART REBUILD INITIALIZATION ---\n";
	  if(smartLog.is_open()) smartLog << "Scanning folder: " << _saveFolder << "\n";

	  if(std::filesystem::exists(_saveFolder)) {
		  int imageIndexOffset = 0;
		  // CORRECTION: uint32_t pour matcher avec width et height !
		  uint32_t tileSize = 8 * (_options.Scale > 0 ? _options.Scale : 1);

		  _usedSmartBlocks.clear(); // On remet le traqueur à zéro !

		  for(const auto& entry : std::filesystem::recursive_directory_iterator(_saveFolder)) {
			  if(entry.is_regular_file() && entry.path().extension() == ".png") {

				  std::string filePath = entry.path().string();

				  // ANTI-BOUCLE INFINE : On ignore les fichiers _REBUILD existants !
				  if(filePath.find("_REBUILD.png") != std::string::npos) {
					  continue;
				  }

				  std::string relativePath = std::filesystem::relative(entry.path(), _saveFolder).string();
				  std::replace(relativePath.begin(), relativePath.end(), '\\', '/');

				  // Préparation du futur nom "_REBUILD"
				  std::string rebuildPath = relativePath;
				  size_t dotPos = rebuildPath.find_last_of('.');
				  if(dotPos != std::string::npos) {
					  rebuildPath.insert(dotPos, "_REBUILD");
				  } else {
					  rebuildPath += "_REBUILD.png";
				  }

				  if(smartLog.is_open()) smartLog << "Found PNG: " << relativePath << "\n";

				  std::vector<uint32_t> pixels;
				  uint32_t width = 0, height = 0;

				  // 1. On charge le fichier binaire en mémoire (C++ Standard)
				  std::ifstream file(filePath, std::ios::binary | std::ios::ate);
				  if(file.is_open()) {
					  std::streamsize size = file.tellg();
					  file.seekg(0, std::ios::beg);
					  std::vector<uint8_t> fileBytes(size);

					  if(file.read((char*)fileBytes.data(), size)) {

						  // 2. On décode les octets du fichier en pixels uint32_t
						  if(PNGHelper::ReadPNG(fileBytes, pixels, width, height)) {

							  if(smartLog.is_open()) smartLog << "  -> Successfully read pixels. Dimensions: " << width << "x" << height << "\n";

							  // ON UTILISE LA NOUVELLE STRUCTURE
							  SmartRebuildImage imgInfo;
							  imgInfo.OriginalAbsPath = filePath;
							  imgInfo.OriginalRelativePath = relativePath;
							  imgInfo.RebuildRelativePath = rebuildPath;
							  _smartRebuildImages.push_back(imgInfo);

							  for(uint32_t y = 0; y + tileSize <= height; y += tileSize) {
								  for(uint32_t x = 0; x + tileSize <= width; x += tileSize) {

									  uint64_t pixelHash = 14695981039346656037ULL;
									  bool isEmpty = true;

									  for(uint32_t py = 0; py < tileSize; py++) {
										  for(uint32_t px = 0; px < tileSize; px++) {
											  uint32_t color = pixels[(y + py) * width + (x + px)];

											  if((color >> 24) == 0) {
												  color = 0x00000000;
											  } else {
												  isEmpty = false;
											  }

											  pixelHash ^= color;
											  pixelHash *= 1099511628211ULL;
										  }
									  }

									  if(!isEmpty && _smartRebuildMap.find(pixelHash) == _smartRebuildMap.end()) {
										  _smartRebuildMap[pixelHash] = { imageIndexOffset, (int)x, (int)y };
									  }
								  }
							  }
							  imageIndexOffset++;
						  } else {
							  if(smartLog.is_open()) smartLog << "  -> ERROR: Failed to decode PNG format.\n";
						  }
					  } else {
						  if(smartLog.is_open()) smartLog << "  -> ERROR: Failed to read file bytes.\n";
					  }
				  } else {
					  if(smartLog.is_open()) smartLog << "  -> ERROR: Failed to open file.\n";
				  }
			  }
		  }
		  if(smartLog.is_open()) smartLog << "Smart Rebuild Map populated with " << _smartRebuildMap.size() << " unique tiles.\n";
	  } else {
		  if(smartLog.is_open()) smartLog << "Folder does not exist!\n";
	  }
  } else {
	  if(smartLog.is_open()) smartLog << "Smart Rebuild skipped (Option disabled or SaveFolder empty).\n";
  }
  if(smartLog.is_open()) smartLog.close();
  // ==========================================================

  // ==========================================================
	// --- PRÉ-CHARGEMENT DU MODE 7 (UPDATE MODE) ---
	// ==========================================================
  if(_options.UpdateIfExists) {
	  std::string filePath = FolderUtilities::CombinePath(_saveFolder, "snes_hires.txt");
	  std::ifstream inFile(filePath);

	  if(inFile.is_open()) {
		  std::string line;
		  while(std::getline(inFile, line)) {

			  // 1. Détection des Textures M7
			  if(line.substr(0, 7) == "<m7img>") {
				  std::vector<std::string> tokens = StringUtilities::Split(line.substr(7), ',');
				  if(tokens.size() > 1) {
					  uint64_t hash = std::stoull(StringUtilities::Trim(tokens[1]), nullptr, 16);
					  _m7TexturesDone.insert(hash);
				  }
			  }
			  // 2. Détection des Tilesets M7
			  else if(line.substr(0, 9) == "<m7tiles>") {
				  std::vector<std::string> tokens = StringUtilities::Split(line.substr(9), ',');
				  if(tokens.size() > 1) {
					  uint64_t hash = std::stoull(StringUtilities::Trim(tokens[1]), nullptr, 16);
					  _m7TilesetsDone.insert(hash);
				  }
			  }
		  }
		  inFile.close();
	  }
  }



  if (!_saveFolder.empty()) {
    // 1. Créer le dossier racine du Pack (ex: Documents/Mesen/HdPacks/MonJeu)
    FolderUtilities::CreateFolder(_saveFolder);

    // 2. Créer le dossier Tiles
    std::string tilesPath = FolderUtilities::CombinePath(_saveFolder, "Tiles");
    FolderUtilities::CreateFolder(tilesPath);

    // 3. Créer les sous-dossiers spécifiques
    std::string bgPath = FolderUtilities::CombinePath(tilesPath, "Backgrounds");
    std::string sprPath = FolderUtilities::CombinePath(tilesPath, "Sprites");

    FolderUtilities::CreateFolder(bgPath);
    FolderUtilities::CreateFolder(sprPath);
  }


}
//--------------------------------------------------------------------------------------------------------

uint32_t SnesHdPackBuilder::CalculateMode7Hash(uint8_t *vram, uint16_t *cgram) {
  // Le Mode 7 utilise toujours la première moitié de la VRAM (0x0000 - 0x7FFF en mots, donc 64 Ko en octets)
  uint32_t vramHash = CRC32::GetCRC(vram, 0x10000);

  // Le Mode 7 utilise toute la CGRAM (256 couleurs = 512 octets)
  uint32_t cgramHash = CRC32::GetCRC((uint8_t *)cgram, 512);

  // Combiner les deux pour avoir un identifiant unique
  return vramHash ^ (cgramHash << 1);
}
//--------------------------------------------------------------------------------------------------------------
void SnesHdPackBuilder::ProcessMode7(uint16_t* vram, uint16_t* cgram)
{
	if(!_isActive || vram == nullptr || cgram == nullptr) {
		return;
	}

	// =====================================================================
	// 1. DUMP DE LA MAP GEANTE (m7_*.png)
	// =====================================================================
	if(!_options.DisableM7TextureDump) {
		// Hash basé sur la disposition de la carte (512 premiers octets)
		uint32_t mapHash = CRC32::GetCRC((uint8_t*)vram, 512);

		if(_m7TexturesDone.find(mapHash) == _m7TexturesDone.end()) {
			_m7TexturesDone.insert(mapHash);
			DumpMode7Texture(vram, cgram, mapHash, _options.Scale);
		}
	}

	// =====================================================================
	// 2. DUMP DU CATALOGUE DE TUILES (m7tiles_*.png)
	// =====================================================================
	// On isole UNIQUEMENT les pixels des 256 tuiles (les octets de poids fort)
	// Il y a 256 tuiles * 64 pixels = 16 384 pixels.
	uint8_t tilePixels[16384];
	for(int i = 0; i < 16384; i++) {
		// On décale de 8 bits pour ne récupérer que le dessin, ignorant la carte !
		tilePixels[i] = (uint8_t)(vram[i] >> 8);
	}

	uint32_t pixelHash = CRC32::GetCRC(tilePixels, 16384);

	// On inclut la CGRAM car F-Zero fait clignoter les couleurs des bordures
	uint32_t cgramHash = CRC32::GetCRC((uint8_t*)cgram, 512);

	uint32_t tilesetHash = pixelHash ^ (cgramHash << 1);

	if(_m7TilesetsDone.find(tilesetHash) == _m7TilesetsDone.end()) {
		_m7TilesetsDone.insert(tilesetHash);
		// On passe le tilesetHash pour que le fichier ait un nom unique à son graphisme
		DumpMode7Tileset(vram, cgram, tilesetHash, _options.Scale);





	}
}
//--------------------------------------------------------------------------------------

void SnesHdPackBuilder::DumpMode7Tileset(uint16_t* vram, uint16_t* cgram, uint32_t globalHash, uint32_t scale)
{
	std::string mode7Folder = FolderUtilities::CombinePath(_saveFolder, "Tiles/Mode7");
	FolderUtilities::CreateFolder(mode7Folder);

	int tileSize = 8 * scale;
	int sheetWidth = 16 * tileSize;
	int sheetHeight = 16 * tileSize;

	// Toile transparente 128x128 (multipliée par le scale)
	std::vector<uint32_t> sheetPixels(sheetWidth * sheetHeight, 0x00000000);
	bool isCanvasEmpty = true;
	std::vector<std::string> tempTags;

	// Un seul fichier PNG pour toute la scène !
	std::string hexMapHash = HexUtilities::ToHex(globalHash);
	std::string filename = "m7tiles_" + hexMapHash + ".png";
	std::string relativePath = "Tiles/Mode7/" + filename;

	for(int tileIndex = 0; tileIndex < 256; tileIndex++) {

		uint8_t rawPixels[64];
		bool isTotallyEmpty = true;

		for(int py = 0; py < 8; py++) {
			for(int px = 0; px < 8; px++) {
				uint16_t pixelWordIndex = tileIndex * 64 + py * 8 + px;
				uint8_t colorIndex = (uint8_t)(vram[pixelWordIndex] >> 8);
				rawPixels[py * 8 + px] = colorIndex;

				if(colorIndex != 0) isTotallyEmpty = false;
			}
		}

		if(isTotallyEmpty) continue;

		uint32_t tileHash = CRC32::GetCRC(rawPixels, 64);

		// Anti-Spam global : on ne dumpe la tuile qu'une seule fois dans tout le jeu
		if(_m7SingleTilesDone.find(tileHash) != _m7SingleTilesDone.end()) {
			continue;
		}
		_m7SingleTilesDone.insert(tileHash);

		// Calcul des coordonnées X et Y dans la grille de notre Atlas
		int tx = tileIndex % 16;
		int ty = tileIndex / 16;
		int pixelX = tx * tileSize;
		int pixelY = ty * tileSize;

		// 1. DESSIN DANS UN BUFFER TEMPORAIRE (Pour le Smart Rebuild)
		std::vector<uint32_t> localTile(tileSize * tileSize, 0x00000000);

		for(int py = 0; py < 8; py++) {
			for(int px = 0; px < 8; px++) {
				uint8_t colorIndex = rawPixels[py * 8 + px];

				if(colorIndex != 0) {
					uint16_t c = cgram[colorIndex];
					uint8_t r = (c & 0x1F); r = (r << 3) | (r >> 2);
					uint8_t g = ((c >> 5) & 0x1F); g = (g << 3) | (g >> 2);
					uint8_t b = ((c >> 10) & 0x1F); b = (b << 3) | (b >> 2);

					uint32_t finalColor = ((uint32_t)0xFF << 24) | (r << 16) | (g << 8) | b;

					for(uint32_t sy = 0; sy < scale; sy++) {
						for(uint32_t sx = 0; sx < scale; sx++) {
							localTile[(py * scale + sy) * tileSize + (px * scale + sx)] = finalColor;
						}
					}
				}
			}
		}

		// =========================================================================
		// --- 2. SMART REBUILD : VÉRIFICATION DU HASH DE PIXELS ---
		// =========================================================================
		bool isMatched = false;
		if(_options.BuildFromPngFolder) {
			uint64_t pixelHash = 14695981039346656037ULL;
			for(uint32_t color : localTile) {
				if((color >> 24) == 0) {
					color = 0x00000000; // Normalisation de la transparence
				}
				pixelHash ^= color;
				pixelHash *= 1099511628211ULL;
			}

			auto it = _smartRebuildMap.find(pixelHash);
			if(it != _smartRebuildMap.end()) {
				isMatched = true;

				_usedSmartBlocks[it->second.ImageIndex].insert({ it->second.X, it->second.Y });

				// CORRECTION POUR LE MODE 7 : On utilise directement le chemin Rebuild !
				std::string matchedPath = _smartRebuildImages[it->second.ImageIndex].RebuildRelativePath;
				int matchedX = it->second.X;
				int matchedY = it->second.Y;

				// On écrit le tag M7 directement pointant vers le dossier d'input
				std::string tag = "<m7tiles>" + matchedPath + "," + HexUtilities::ToHex(tileHash) + "," +
					std::to_string(scale) + "," + std::to_string(matchedX) + "," + std::to_string(matchedY);
				tempTags.push_back(tag);

				if(_options.GenerateHdPackMetadata) {
					HdMetadataItem metaItem;
					metaItem.Type = "Mode7Tile";
					metaItem.Filename = matchedPath;
					metaItem.Hash = HexUtilities::ToHex(tileHash);
					metaItem.GrabX = matchedX;
					metaItem.GrabY = matchedY;
					metaItem.GrabFrame = 0;
					metaItem.SnesHiresLine = tag;
					_metadataCollection.push_back(metaItem);
				}
			}
		}
		// =========================================================================

		// 3. SI AUCUN MATCH : COMPORTEMENT CLASSIQUE
		if(!isMatched) {
			// On copie le buffer temporaire dans l'atlas final
			for(int py = 0; py < tileSize; py++) {
				for(int px = 0; px < tileSize; px++) {
					uint32_t color = localTile[py * tileSize + px];
					if((color >> 24) != 0) {
						sheetPixels[(pixelY + py) * sheetWidth + (pixelX + px)] = color;
					}
				}
			}
			isCanvasEmpty = false;

			std::string tag = "<m7tiles>" + relativePath + "," + HexUtilities::ToHex(tileHash) + "," +
				std::to_string(scale) + "," + std::to_string(pixelX) + "," + std::to_string(pixelY);
			tempTags.push_back(tag);

			if(_options.GenerateHdPackMetadata) {
				HdMetadataItem metaItem;
				metaItem.Type = "Mode7Tile";
				metaItem.Filename = relativePath;
				metaItem.Hash = HexUtilities::ToHex(tileHash);
				metaItem.GrabX = pixelX;
				metaItem.GrabY = pixelY;
				metaItem.GrabFrame = 0;
				metaItem.SnesHiresLine = tag;
				_metadataCollection.push_back(metaItem);
			}
		}
	}

	// Sauvegarde de l'Atlas Mode 7 (si on a dessiné au moins une tuile classique dedans)
	if(!isCanvasEmpty) {
		std::string fullPath = FolderUtilities::CombinePath(_saveFolder, relativePath);
		PNGHelper::WritePNG(fullPath, sheetPixels.data(), sheetWidth, sheetHeight, 32);
	}

	// Quoi qu'il arrive, on sauvegarde la liste des tags générés pour cette map !
	if(!tempTags.empty()) {
		_m7Rows << "\n# Mode 7 Atlas for Map: " << hexMapHash << "\n";
		for(const auto& tag : tempTags) {
			_m7Rows << tag << "\n";
		}
	}
}

//-------------------------------------------------------------------------------------
void SnesHdPackBuilder::DumpMode7Texture(uint16_t *vram, uint16_t *cgram,
                                         uint32_t hash, uint32_t scale) {
  int baseWidth = 1024;
  int baseHeight = 1024;
  int scaledWidth = baseWidth * scale;
  int scaledHeight = baseHeight * scale;

  std::vector<uint32_t> pixels(scaledWidth * scaledHeight, 0);

  for (int ty = 0; ty < 128; ty++) {
    for (int tx = 0; tx < 128; tx++) {

      // On lit directement le mot de 16-bits
      uint16_t tilemapWord = vram[ty * 128 + tx];
      // L'index de la tuile est dans l'octet de poids faible
      uint8_t tileIndex = tilemapWord & 0xFF;

      for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {

          uint16_t pixelWord = vram[tileIndex * 64 + py * 8 + px];
          // La donnée du pixel est dans l'octet de poids fort
          uint8_t colorIndex = pixelWord >> 8;

          uint16_t c = cgram[colorIndex];

          uint8_t r = (c & 0x1F);
          r = (r << 3) | (r >> 2);
          uint8_t g = ((c >> 5) & 0x1F);
          g = (g << 3) | (g >> 2);
          uint8_t b = ((c >> 10) & 0x1F);
          b = (b << 3) | (b >> 2);

          uint8_t a = (colorIndex == 0) ? 0 : 255;
          uint32_t finalColor = (a << 24) | (r << 16) | (g << 8) | b;

          int baseX = (tx * 8 + px) * scale;
          int baseY = (ty * 8 + py) * scale;

          for (uint32_t sy = 0; sy < scale; sy++) {
            for (uint32_t sx = 0; sx < scale; sx++) {
              pixels[(baseY + sy) * scaledWidth + (baseX + sx)] = finalColor;
            }
          }
        }
      }
    }
  }

  // 1. On convertit le hash en Hexa une seule fois
  std::string hexHash = HexUtilities::ToHex(hash);

  // 2. On utilise hexHash pour le nom du fichier
  std::string filename = "m7_" + hexHash + ".png";
  std::string fullPath = FolderUtilities::CombinePath(_saveFolder, filename);
  PNGHelper::WritePNG(fullPath, pixels.data(), scaledWidth, scaledHeight, 32);

  // 3. Construction de la balise <m7img> dans une variable
  std::string m7imgTag = "<m7img>" + filename + "," + hexHash + "," + std::to_string(scale);

  // On l'écrit dans le flux du fichier texte
  _m7Rows << m7imgTag << "\n";

  // =========================================================================
  // --- COLLECTE DES MÉTADONNÉES MODE 7 TEXTURE GÉANTE ---
  // =========================================================================
  if(_options.GenerateHdPackMetadata) {
	  HdMetadataItem metaItem;
	  metaItem.Type = "Mode7Map";
	  metaItem.Filename = filename;
	  metaItem.Hash = hexHash;
	  metaItem.GrabX = 0;
	  metaItem.GrabY = 0;
	  metaItem.GrabFrame = 0;

	  // Maintenant la variable existe et contient la bonne ligne !
	  metaItem.SnesHiresLine = m7imgTag;

	  _metadataCollection.push_back(metaItem);
  }
}
//------------------------------------------------------------------------------------



void SnesHdPackBuilder::StopRecording()
{
	_isActive = false;

	// FORCE BRUTE POUR TESTER LE JSON !
	_options.GenerateHdPackMetadata = true;

	if(_saveFolder.empty())
		return;

	std::string logPath = FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
	std::ofstream log(logPath, std::ios::app);
	std::string filePath = FolderUtilities::CombinePath(_saveFolder, "snes_hires.txt");

	// =========================================================
	// 1. LECTURE DE L'EXISTANT (Mode Append)
	// =========================================================
	std::string existingContent = "";
	std::unordered_set<uint64_t> existingHashes;
	int startingImageIndex = 0;

	if(_options.UpdateIfExists) {
		std::ifstream inFile(filePath);
		if(inFile.is_open()) {
			std::string line;
			while(std::getline(inFile, line)) {
				existingContent += line + "\n"; // Sauvegarde pour la réécriture

				if(line.substr(0, 5) == "<img>") {
					startingImageIndex++;
				} else if(line.substr(0, 6) == "<tile>") {
					std::vector<std::string> tokens = StringUtilities::Split(line.substr(6), ',');
					if(!tokens.empty()) {
						uint64_t hash = std::stoull(tokens[0], nullptr, 16);
						existingHashes.insert(hash);
					}
				}
			}
			inFile.close();
			if(log.is_open()) log << "[HD Builder] Fichier existant: " << startingImageIndex << " img, " << existingHashes.size() << " tiles.\n";
		}
	}

	// =========================================================
	// 2. PRÉPARATION DES NOUVELLES TUILES
	// =========================================================
	struct TileDef
	{
		uint64_t MemoryHash;
		uint32_t PaletteIndex;
		int ImageIndex;
		int X;
		int Y;
		uint16_t VramTileIndex;

		bool IsHFlip;
		bool IsVFlip;
	};

	std::vector<TileDef> tileDefinitions;
	std::vector<std::string> imageList;

	// /!\ DÉCALAGE DE L'INDEX
	int currentImageIndex = startingImageIndex;
	int scale = _options.Scale;
	if(scale < 1) scale = 4;

	int tileSize = 8 * scale;
	int tilesPerRow = 16;
	int maxTilesPerSheet = 256;

	for(auto& entry : _paletteTiles) {
		uint32_t paletteIndex = entry.first;
		auto& tiles = entry.second;

		// Filtre anti-tuiles unies
		if(_options.IgnorePlainTiles) {
			tiles.erase(std::remove_if(tiles.begin(), tiles.end(), [](const auto& tile) {
				if(tile.Pixels.empty()) return false;
				uint32_t firstColor = tile.Pixels[0];
				for(size_t p = 1; p < tile.Pixels.size(); p++) {
					if(tile.Pixels[p] != firstColor) return false;
				}
				return true;
			}), tiles.end());
		}

		// Filtre anti-doublons (Append Mode)
		if(_options.UpdateIfExists && !tiles.empty()) {
			tiles.erase(std::remove_if(tiles.begin(), tiles.end(), [&](const auto& tile) {
				return existingHashes.find(tile.MemoryHash) != existingHashes.end();
			}), tiles.end());
		}

		if(tiles.empty()) continue;

		bool isSprite = tiles[0].IsSprite;
		std::string subFolder = isSprite ? "Sprites" : "Backgrounds";

		// ==========================================================
		// --- LE PACKING DYNAMIQUE ---
		// ==========================================================
		if(_options.GenerateSpritesPerFrame && isSprite) {
			std::map<uint32_t, std::vector<const SnesHdTileImage*>> tilesByFrame;
			for(const auto& tile : tiles) tilesByFrame[tile.FrameSeen].push_back(&tile);

			int canvasWidth = 256 * scale;
			int canvasHeight = 256 * scale;

			std::string relativeFolder = "Tiles/" + subFolder + "/Palette_" + std::to_string(paletteIndex);
			std::string dirPath = FolderUtilities::CombinePath(_saveFolder, relativeFolder);
			FolderUtilities::CreateFolder(dirPath);

			std::vector<uint32_t> currentCanvas(canvasWidth * canvasHeight, 0);
			std::vector<TileDef> currentTileDefs;
			std::set<std::pair<int, int>> occupiedCoords;
			bool isCanvasEmpty = true;
			int packIndex = 0;

			auto flushCanvas = [&]() {
				if(isCanvasEmpty) return;

				std::string fileName = relativeFolder + "/packed_" + std::to_string(packIndex) + ".png";
				std::string fullPath = FolderUtilities::CombinePath(_saveFolder, fileName);

				if(PNGHelper::WritePNG(fullPath, currentCanvas.data(), canvasWidth, canvasHeight, 32)) {
					if(log.is_open()) log << "Successfully wrote packed canvas: " << fileName << "\n";
				}

				imageList.push_back(fileName);
				tileDefinitions.insert(tileDefinitions.end(), currentTileDefs.begin(), currentTileDefs.end());

				std::fill(currentCanvas.begin(), currentCanvas.end(), 0);
				currentTileDefs.clear();
				occupiedCoords.clear();
				isCanvasEmpty = true;

				packIndex++;
				currentImageIndex++;
			};

			for(auto& frameEntry : tilesByFrame) {
				for(const auto* tilePtr : frameEntry.second) {
					const auto& tile = *tilePtr;
					int sx = tile.ScreenX;
					int sy = tile.ScreenY;

					std::pair<int, int> currentCoord = { sx, sy };

					if(occupiedCoords.find(currentCoord) != occupiedCoords.end()) flushCanvas();

					bool drewSomething = false;
					for(int py = 0; py < tileSize; py++) {
						for(int px = 0; px < tileSize; px++) {
							int destX = (sx * scale) + px;
							int destY = (sy * scale) + py;

							if(destX >= 0 && destX < canvasWidth && destY >= 0 && destY < canvasHeight) {
								uint32_t pixel = tile.Pixels[py * tileSize + px];
								if((pixel & 0xFF000000) != 0) {
									currentCanvas[destY * canvasWidth + destX] = pixel;
									drewSomething = true;
								}
							}
						}
					}

					if(drewSomething) {
						isCanvasEmpty = false;
						occupiedCoords.insert(currentCoord);
						currentTileDefs.push_back({ tile.MemoryHash, paletteIndex, currentImageIndex, sx * scale, sy * scale, tile.VramTileIndex });

						if(_options.GenerateHdPackMetadata) {
							HdMetadataItem metaItem;
							metaItem.Type = "Sprite";
							metaItem.Filename = relativeFolder + "/packed_" + std::to_string(packIndex) + ".png";
							metaItem.Hash = HexUtilities::ToHex(tile.MemoryHash);
							metaItem.GrabX = sx;
							metaItem.GrabY = sy;
							metaItem.GrabFrame = tile.FrameSeen;

							// GESTION DYNAMIQUE DU TAG
							std::string tileTag = "<tile>";
							if(tile.IsHFlip && tile.IsVFlip) tileTag = "<tile_hvflip>";
							else if(tile.IsHFlip) tileTag = "<tile_hflip>";
							else if(tile.IsVFlip) tileTag = "<tile_vflip>";

							std::stringstream ssLine;
							ssLine << tileTag << std::hex << std::uppercase << tile.MemoryHash << ","
								<< std::dec << paletteIndex << "," << currentImageIndex << "," << (sx * scale) << "," << (sy * scale);
							metaItem.SnesHiresLine = ssLine.str();
							_metadataCollection.push_back(metaItem);
						}
					}
				}
			}
			flushCanvas();
		}
		// ==========================================================
		// --- LA GRILLE CLASSIQUE ---
		// ==========================================================
		else {
			std::sort(tiles.begin(), tiles.end(), [](const SnesHdTileImage& a, const SnesHdTileImage& b) {
				if(a.FrameSeen != b.FrameSeen) return a.FrameSeen < b.FrameSeen;
				if(a.ScreenY != b.ScreenY) return a.ScreenY < b.ScreenY;
				return a.ScreenX < b.ScreenX;
			});

			for(int i = 0; i < (int)tiles.size(); i += maxTilesPerSheet) {
				int count = std::min(maxTilesPerSheet, (int)tiles.size() - i);
				int rows = (count + tilesPerRow - 1) / tilesPerRow;
				int sheetWidth = tilesPerRow * tileSize;
				int sheetHeight = rows * tileSize;

				std::vector<uint32_t> sheetPixels(sheetWidth * sheetHeight, 0);

				std::string dirPath = FolderUtilities::CombinePath(_saveFolder, "Tiles/" + subFolder);
				FolderUtilities::CreateFolder(dirPath);

				std::string pageSuffix = "_page_" + std::to_string(i / maxTilesPerSheet);
				std::string fileName = "Tiles/" + subFolder + "/pal_" + std::to_string(paletteIndex) + pageSuffix + ".png";

				for(int j = 0; j < count; j++) {
					const auto& tile = tiles[i + j];
					int tx = j % tilesPerRow;
					int ty = j / tilesPerRow;

					for(int py = 0; py < tileSize; py++) {
						for(int px = 0; px < tileSize; px++) {
							sheetPixels[(ty * tileSize + py) * sheetWidth + (tx * tileSize + px)] = tile.Pixels[py * tileSize + px];
						}
					}

					tileDefinitions.push_back({ tile.MemoryHash, paletteIndex, currentImageIndex, tx * tileSize, ty * tileSize, tile.VramTileIndex });

					if(_options.GenerateHdPackMetadata) {

						// GESTION DYNAMIQUE DU TAG TILE
						std::string tileTag = "<tile>";
						if(tile.IsHFlip && tile.IsVFlip) tileTag = "<tile_hvflip>";
						else if(tile.IsHFlip) tileTag = "<tile_hflip>";
						else if(tile.IsVFlip) tileTag = "<tile_vflip>";

						HdMetadataItem metaItem;
						metaItem.Type = isSprite ? "Sprite" : "Background";
						metaItem.Filename = fileName;
						metaItem.Hash = HexUtilities::ToHex(tile.MemoryHash);
						metaItem.GrabX = tile.ScreenX;
						metaItem.GrabY = tile.ScreenY;
						metaItem.GrabFrame = tile.FrameSeen;

						std::stringstream ssLine;
						ssLine << tileTag << std::hex << std::uppercase << tile.MemoryHash << ","
							<< std::dec << paletteIndex << "," << currentImageIndex << "," << (tx * tileSize) << "," << (ty * tileSize);
						metaItem.SnesHiresLine = ssLine.str();
						_metadataCollection.push_back(metaItem);
					}
				}

				std::string fullPath = FolderUtilities::CombinePath(_saveFolder, fileName);
				if(PNGHelper::WritePNG(fullPath, sheetPixels.data(), sheetWidth, sheetHeight, 32)) {
					if(log.is_open()) log << "Successfully wrote: " << fileName << "\n";
				}

				imageList.push_back(fileName);
				currentImageIndex++;
			}
		}
	}
	// ==========================================================
	// --- SMART REBUILD : GÉNÉRATION DES PNG "_REBUILD" ---
	// ==========================================================
	if(_options.BuildFromPngFolder) {
		// On rouvre le mouchard
		std::string smartLogPath = FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
		std::ofstream smartLog(smartLogPath, std::ios::app);
		if(smartLog.is_open()) smartLog << "\n--- STARTING PNG REBUILD GENERATION ---\n";

		uint32_t tileSize = 8 * (_options.Scale > 0 ? _options.Scale : 1);

		// On ne parcourt que les images qui ont eu au moins 1 tuile utilisée !
		for(auto const& [imgIndex, blocks] : _usedSmartBlocks) {

			SmartRebuildImage& imgInfo = _smartRebuildImages[imgIndex];
			if(smartLog.is_open()) smartLog << "Processing: " << imgInfo.OriginalRelativePath << "\n";
			if(smartLog.is_open()) smartLog << " -> Blocks mapped: " << blocks.size() << "\n";

			std::vector<uint32_t> origPixels;
			uint32_t w = 0, h = 0;

			// 1. On recharge l'originale en mémoire
			std::ifstream origFile(imgInfo.OriginalAbsPath, std::ios::binary | std::ios::ate);
			if(origFile.is_open()) {
				std::streamsize size = origFile.tellg();
				origFile.seekg(0, std::ios::beg);
				std::vector<uint8_t> fileBytes(size);

				if(origFile.read((char*)fileBytes.data(), size)) {
					if(PNGHelper::ReadPNG(fileBytes, origPixels, w, h)) {
						if(smartLog.is_open()) smartLog << " -> Original file reloaded ok. Dimensions: " << w << "x" << h << "\n";

						// 2. On crée une toile vierge (100% transparente) de la MÊME taille
						std::vector<uint32_t> rebuildPixels(w * h, 0x00000000);

						// 3. On copie UNIQUEMENT les blocs de pixels utilisés !
						for(auto const& block : blocks) {
							int startX = block.first;
							int startY = block.second;

							for(uint32_t py = 0; py < tileSize; py++) {
								for(uint32_t px = 0; px < tileSize; px++) {
									if(startY + py < h && startX + px < w) {
										rebuildPixels[(startY + py) * w + (startX + px)] = origPixels[(startY + py) * w + (startX + px)];
									}
								}
							}
						}

						// 4. On sauvegarde le PNG "Épuré"
						std::string outPath = FolderUtilities::CombinePath(_saveFolder, imgInfo.RebuildRelativePath);
						if(smartLog.is_open()) smartLog << " -> Attempting to save to: " << outPath << "\n";

						if(PNGHelper::WritePNG(outPath, rebuildPixels.data(), w, h, 32)) {
							if(smartLog.is_open()) smartLog << " -> SUCCESS! File written.\n";
						} else {
							if(smartLog.is_open()) smartLog << " -> ERROR: PNGHelper::WritePNG failed to write the file!\n";
						}
					} else {
						if(smartLog.is_open()) smartLog << " -> ERROR: PNGHelper::ReadPNG failed to decode original bytes.\n";
					}
				} else {
					if(smartLog.is_open()) smartLog << " -> ERROR: origFile.read failed.\n";
				}
			} else {
				if(smartLog.is_open()) smartLog << " -> ERROR: Could not open original file: " << imgInfo.OriginalAbsPath << "\n";
			}
		}
		if(smartLog.is_open()) smartLog.close();
	}

	// =========================================================
	// 3. ÉCRITURE DU FICHIER TEXTE snes_hires.txt (MERGE)
	// =========================================================
	std::ofstream file(filePath);
	if(!file.is_open()) return;

	std::string currentTimestamp = GetCurrentDateTimeString();

	auto writeBlockHeader = [&](const std::string& blockType) {
		if(_options.UpdateIfExists) { // If we are in Append mode
			file << "\n##############################\n";
			file << "###    HD PACK BUILDER: ADDITION in Append Mode on " << currentTimestamp << " [" << blockType << "]\n";
			file << "##############################\n";
		} else {
			file << "\n# === " << blockType << " ===\n";
		}
	};

	// A. Réinjection de l'existant
	if(!_options.UpdateIfExists || existingContent.empty()) {
		if(!_romInfoTxt.empty()) file << _romInfoTxt;
		file << "<scale>" << _options.Scale << "\n\n";
	} else {
		file << existingContent;
		// We remove the old "# === NOUVELLES TUILES (UPDATE) ===" line here
		// because the new header system handles it better.
	}

	// B. Nouvelles images
	if(!imageList.empty() || !_smartRebuildImages.empty()) {
		writeBlockHeader("IMAGES");

		// 1. Les spritesheets trouvées par le Smart Rebuild
		/*	for(const auto& img : _smartRebuildImages) {
			file << "<img>" << img << "\n";
		}
		*/
		// --- SMART REBUILD IMAGES ---
		for(size_t i = 0; i < _smartRebuildImages.size(); i++) {
			// Si on s'en est servi, on pointe le fichier texte vers l'image optimisée (_REBUILD)
			if(_usedSmartBlocks.find(i) != _usedSmartBlocks.end()) {
				file << "<img>" << _smartRebuildImages[i].RebuildRelativePath << "\n";
			}
			// Si l'image n'a pas du tout été utilisée, on laisse le chemin d'origine.
			else {
				file << "<img>" << _smartRebuildImages[i].OriginalRelativePath << "\n";
			}
		}
		// 2. Les nouvelles images créées par le Builder
		for(const auto& img : imageList) {
			file << "<img>" << img << "\n";
		}
		file << "\n";
	}

	int lastImageIndex = -1;

	// C. Nouvelles tuiles
	if(!tileDefinitions.empty() || !_preMappedTiles.empty()) {
		writeBlockHeader("TILES");

		// --- SMART REBUILD TILES ---
		if(!_preMappedTiles.empty()) {
			file << "\n# --- SMART REBUILD MAPPED TILES ---\n";
			for(const auto& td : _preMappedTiles) {
				file << "<tile>" << std::hex << std::uppercase << td.MemoryHash << ","
					<< std::dec << td.PaletteIndex << "," << td.ImageIndex << ","
					<< td.X << "," << td.Y << "\n";
			}
			file << "\n";
		}
		for(const auto& td : tileDefinitions) {
			if(td.ImageIndex != lastImageIndex) {
				lastImageIndex = td.ImageIndex;
				file << "\n# =========================================================================\n";

				int localImgIndex = td.ImageIndex - startingImageIndex;
				if(localImgIndex >= 0 && localImgIndex < imageList.size()) {
					file << "# Tiles from PNG file <img> n°" << td.ImageIndex << " : " << imageList[localImgIndex] << "\n";
				}
				file << "# =========================================================================\n\n";
			}

			std::string commentLine = "# Tile Index: $" + HexUtilities::ToHex(td.VramTileIndex / 32) + ", Palette: " + std::to_string(td.PaletteIndex);
			file << commentLine << "\n";

			//  Sélection du bon Tag d'écriture selon la symétrie !
			std::string tileTag = "<tile>";
			if(td.IsHFlip && td.IsVFlip) tileTag = "<tile_hvflip>";
			else if(td.IsHFlip) tileTag = "<tile_hflip>";
			else if(td.IsVFlip) tileTag = "<tile_vflip>";

			file << tileTag << std::hex << std::uppercase << td.MemoryHash << ","
				<< std::dec << td.PaletteIndex << "," << td.ImageIndex << "," << td.X << "," << td.Y << "\n\n";
		}
	}

	// D. Mode 7
	std::string m7Data = _m7Rows.str();
	if(!m7Data.empty()) {
		writeBlockHeader("MODE 7 TILES");
		file << m7Data;
	}

	file.close();

	// =========================================================
	// 4. ÉCRITURE DES MÉTADONNÉES JSON
	// =========================================================
	if(_options.GenerateHdPackMetadata && !_metadataCollection.empty()) {
		std::string jsonPath = FolderUtilities::CombinePath(_saveFolder, "hdpack_metadata.json");
		std::ofstream jsonFile(jsonPath);

		if(jsonFile.is_open()) {
			std::stringstream ss;
			ss << "{\n";
			if(!_romInfoJson.empty()) ss << _romInfoJson;
			ss << "{\n  \"generatedTiles\": [\n";

			for(size_t i = 0; i < _metadataCollection.size(); i++) {
				const auto& item = _metadataCollection[i];
				ss << "    {\n"
					<< "      \"type\": \"" << item.Type << "\",\n"
					<< "      \"filename\": \"" << item.Filename << "\",\n"
					<< "      \"hash\": \"" << item.Hash << "\",\n"
					<< "      \"grabX\": " << item.GrabX << ",\n"
					<< "      \"grabY\": " << item.GrabY << ",\n"
					<< "      \"grabFrame\": " << item.GrabFrame << ",\n"
					<< "      \"snesHiresLine\": \"" << item.SnesHiresLine << "\"\n"
					<< "    }";
				if(i < _metadataCollection.size() - 1) ss << ",";
				ss << "\n";
			}
			ss << "  ]\n}";
			jsonFile << ss.str();
			jsonFile.close();
		}
	}

	if(log.is_open()) log.close();

	// =========================================================
	// 5. NETTOYAGE DES BUFFERS
	// =========================================================
	_metadataCollection.clear();
	_processedTiles.clear();
	_paletteTiles.clear();
	_m7Rows.str("");
	_m7Rows.clear();
	_m7TexturesDone.clear();
	_m7TilesetsDone.clear();
}


//--------------------------------------------------------------------------------------------------------


void SnesHdPackBuilder::ProcessTile(const uint8_t* tileData, size_t dataSize,
                                    uint32_t paletteIndex, SnesBpp bpp,
                                    bool isLargeTile, const uint16_t *cgram,
                                    bool isSprite, uint16_t vramTileIndex,
												int screenX, int screenY, uint32_t frameCount, bool hMirror, bool vMirror) {
  if (!_isActive || dataSize == 0 || tileData == nullptr || cgram == nullptr) {
    return;
  }

  // =========================================================================
	// --- LE BOUCLIER ANTI-MIROIR ---
	// Si c'est un sprite, qu'il est en miroir horizontal, et que l'option est active : on rejette !
	// =========================================================================
  if(_options.PreventHMirroredSpritesGrabbing && isSprite && hMirror) {
	  return;
  }


  // Hash MemoryHash basique
  uint64_t hash = 0;
  for (size_t i = 0; i < dataSize; i++) {
    hash = (hash * 31) + tileData[i];
  }

  SnesHdTileKey key;
  key.MemoryHash = hash;
  key.PaletteHash = paletteIndex;
  key.Bpp = bpp;
  key.IsLargeTile = isLargeTile;
  key.IsSprite = isSprite;

  // On enregistre les symétries pour différencier la clé !
  key.IsHFlip = hMirror;
  key.IsVFlip = vMirror;

  if (_processedTiles.find(key) == _processedTiles.end()) {
    _processedTiles.insert(key);

    // Log occasionnel pour ne pas saturer
    /*if(_processedTiles.size() % 100 == 0) {
      std::string logPath =
          FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
      std::ofstream log(logPath, std::ios::app);
      if (log.is_open()) {
        log << "Processed 100 more tiles. Total: " << _processedTiles.size()
            << "\n";
        log.close();
      }
    }
	 */

    SaveTilePng(tileData, dataSize, hash, paletteIndex, bpp, isLargeTile, cgram,
                isSprite, vramTileIndex, screenX, screenY, frameCount, hMirror, vMirror);

	 

	 // Hash MemoryHash basique
	 uint64_t hash = 0;
	 for(size_t i = 0; i < dataSize; i++) {
		 hash = (hash * 31) + tileData[i];
	 }

	 // ==========================================================
	 // MOUCHARD 1 : LE BUILDER (Maintenant avec le Hash !)
	 // ==========================================================
	 /*std::string mouchardPath = FolderUtilities::CombinePath(_saveFolder, "mouchard_hash.txt");
	 std::ofstream logB(mouchardPath, std::ios::app);
	 if(logB.is_open()) {
		 logB << "=== [BUILDER] ===\n";
		 logB << "Hash Généré : " << std::hex << hash << std::dec << "\n";
		 logB << "VramTileIndex: " << vramTileIndex << " | BPP: " << (int)bpp << " | DataSize: " << dataSize << "\n";
		 logB << "Les 16 premiers octets lus : ";
		 for(size_t i = 0; i < 16 && i < dataSize; i++) {
			 logB << std::hex << (int)tileData[i] << " ";
		 }
		 logB << std::dec << "\n\n";
		 logB.close();
	 }
	 */
	 // ==========================================================


  } 
}


//---------------------------------------------------------------- 
void SnesHdPackBuilder::SaveTilePng(const uint8_t *tileData, size_t dataSize,
                                    uint64_t memoryHash, uint32_t paletteIndex,
                                    SnesBpp bpp, bool isLargeTile,
                                    const uint16_t *cgram, bool isSprite,uint16_t vramTileIndex,
												int screenX, int screenY, uint32_t frameCount, bool hMirror, bool vMirror) {

	 
	// =========================================================
	// FORCE BRUTE POUR TESTER LE JSON !
	// =========================================================
	_options.GenerateHdPackMetadata = true;
	// =========================================================

  int scale = _options.Scale;
  if (scale < 1)
    scale = 1;
  if (scale > 10)
    scale = 10;

  int tileSize = 8 * scale;
  std::vector<uint32_t> rgbaBuffer(tileSize * tileSize, 0);

  int actualBpp = 2;
  if (bpp == SnesBpp::Bpp4)
    actualBpp = 4;
  else if (bpp == SnesBpp::Bpp8)
    actualBpp = 8;

  int palOffset = 0;
  if (bpp == SnesBpp::Bpp2)
    palOffset = paletteIndex * 4;
  else if (bpp == SnesBpp::Bpp4)
    palOffset = paletteIndex * 16;
  else if (bpp == SnesBpp::Bpp8)
    palOffset = 0;

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      uint8_t colorIndex = 0;
      for (int bp = 0; bp < actualBpp; bp++) {
        int pairIdx = bp / 2;
        int bitIdx = bp % 2;
        int byteOffset = pairIdx * 16 + y * 2 + bitIdx;
        if (byteOffset < dataSize && (tileData[byteOffset] & (1 << (7 - x)))) {
          colorIndex |= (1 << bp);
        }
      }

      uint16_t c = cgram[palOffset + colorIndex];
      uint8_t r = (c & 0x1F);
      r = (r << 3) | (r >> 2);
      uint8_t g = ((c >> 5) & 0x1F);
      g = (g << 3) | (g >> 2);
      uint8_t b = ((c >> 10) & 0x1F);
      b = (b << 3) | (b >> 2);
      uint8_t a = (colorIndex == 0) ? 0 : 255;

      uint32_t finalColor = (a << 24) | (r << 16) | (g << 8) | b;

      for (int dy = 0; dy < scale; dy++) {
        for (int dx = 0; dx < scale; dx++) {
			  // Inversion des coordonnées X et Y si miroir actif
			  int finalHdX = hMirror ? (tileSize - 1 - (x * scale + dx)) : (x * scale + dx);
			  int finalHdY = vMirror ? (tileSize - 1 - (y * scale + dy)) : (y * scale + dy);
           rgbaBuffer[(y * scale + dy) * tileSize + (x * scale + dx)] =
              finalColor;
        }
      }
    }
  }
  // ==========================================================
	// --- SMART REBUILD : VÉRIFICATION DU HASH DE PIXELS ---
	// ==========================================================
  if(_options.BuildFromPngFolder) {
	  uint64_t pixelHash = 14695981039346656037ULL;
	  for(uint32_t color : rgbaBuffer) {

		  if((color >> 24) == 0) {
			  color = 0x00000000;
		  }

		  pixelHash ^= color;
		  pixelHash *= 1099511628211ULL;
	  }

	  auto it = _smartRebuildMap.find(pixelHash);
	  if(it != _smartRebuildMap.end()) {
		  _usedSmartBlocks[it->second.ImageIndex].insert({ it->second.X, it->second.Y });
		  
		  /*
		  // MOUCHARD DE MATCH (Limité à 20 pour la perf)
		  static int matchCount = 0;
		  if(matchCount < 20) {
			  matchCount++;
			  std::string logPath = FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
			  std::ofstream log(logPath, std::ios::app);
			  if(log.is_open()) {
				  log << "[MATCH " << matchCount << "] MemoryHash: " << std::hex << std::uppercase << memoryHash
					  << " -> Maps to ImgIndex " << std::dec << it->second.ImageIndex
					  << " at (" << it->second.X << "," << it->second.Y << ")\n";
			  }
		  }
		  */


		  PreMappedTile preMapped = {
				memoryHash,
				paletteIndex,
				it->second.ImageIndex,
				it->second.X,
				it->second.Y
		  };
		  _preMappedTiles.push_back(preMapped);
		  return;
	  }
  }
  // ==========================================================

  // Si on arrive ici, c'est que l'option est désactivée OU que la tuile n'existait pas dans le dossier.
  // On reprend le comportement classique !
  SnesHdTileImage img;
  img.MemoryHash = memoryHash;
  img.Pixels = std::move(rgbaBuffer);
  img.IsSprite = isSprite;
  img.VramTileIndex = vramTileIndex;

  // --- L'ENREGISTREMENT des infos de contexte à l'ecran et temporel ---
  img.FrameSeen = frameCount;
  img.ScreenX = screenX;
  img.ScreenY = screenY;

  //Sauvegarde des flags pour la génération du TXT !
  img.IsHFlip = hMirror;
  img.IsVFlip = vMirror;

  _paletteTiles[paletteIndex].push_back(std::move(img));


}
```

---

## ./SnesHdPackBuilder.h

```h
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
```

---

## ./SnesHdPackConditions.h

```h
#pragma once
#include "pch.h"
#include "SNES/HdPacks/SnesHdData.h" 
#include "Utilities/HexUtilities.h"
#include "Utilities/StringUtilities.h"
#include <fstream>
#include <sstream>


class SnesPpu;
struct SnesPpuState;

// ---------------------------------------------------------
// 1. CONDITIONS DE TUILES (Adaptées avec le Hash SNES)
// ---------------------------------------------------------
struct SnesHdPackBaseTileCondition : public SnesHdPackCondition
{
	int32_t TileX = 0;
	int32_t TileY = 0;

	// SNES : On utilise le Hash et le BPP plutôt que des octets bruts !
	uint64_t MemoryHash = 0;
	uint32_t PaletteHash = 0;
	bool IgnorePalette = false;

	int32_t PixelOffset = 0;

	void Initialize(int32_t x, int32_t y, uint64_t memoryHash, uint32_t paletteHash, bool ignorePalette)
	{
		TileX = x;
		TileY = y;
		PixelOffset = (y * 256) + x; // Attention : La SNES peut être en 512px, il faudra peut-être gérer ça plus tard
		MemoryHash = memoryHash;
		PaletteHash = paletteHash;
		IgnorePalette = ignorePalette;
	}

	string ToString() override
	{
		stringstream out;
		out << "<condition>" << Name << "," << GetConditionName() << ",";
		out << TileX << ",";
		out << TileY << ",";
		out << HexUtilities::ToHex(MemoryHash) << ",";
		out << HexUtilities::ToHex(PaletteHash, true);
		return out.str();
	}
};

enum class HdPackConditionOperator
{
	Invalid = -1,
	Equal = 0,
	NotEqual = 1,
	GreaterThan = 2,
	LowerThan = 3,
	LowerThanOrEqual = 4,
	GreaterThanOrEqual = 5,

	// In operator added by Gwenolo
	In = 6
};

// ---------------------------------------------------------
// 2. CONDITIONS MÉMOIRE (Mutualisées pour l'Architecture SNES)
// ---------------------------------------------------------

// Les marqueurs spatiaux à injecter dans les bits de poids fort (bits 24-31)
enum class SnesMemorySpace : uint32_t
{
	CPU = 0x00000000,
	VRAM = 0x10000000,
	OAM = 0x20000000,
	CGRAM = 0x30000000,
	ARAM = 0x40000000 // SPC700 Audio RAM
};

struct SnesHdPackBaseMemoryCondition : public SnesHdPackCondition
{
	uint32_t OperandA = 0; // Adresse cible (sans le marqueur)
	HdPackConditionOperator Operator = {};
	uint32_t OperandB = 0; // Valeur constante OU Adresse de comparaison
	uint8_t Mask = 0xFF;

	SnesMemorySpace Space = SnesMemorySpace::CPU;
	bool IsConstantCheck = true; // Permet de basculer entre CheckConstant et Check

	// added by Gwenolo 
	std::vector<uint8_t> OperandBList;

	void Initialize(uint32_t operandA, HdPackConditionOperator op, uint32_t operandB, uint8_t mask, SnesMemorySpace space, bool isConstant)
	{
		OperandA = operandA;
		Operator = op;
		OperandB = operandB;
		Mask = mask;
		Space = space;
		IsConstantCheck = isConstant;
	}

	void InitializeForInOperator(uint32_t operandA, HdPackConditionOperator op, const std::string& operandBStr, uint8_t mask, SnesMemorySpace space)
	{
		OperandA = operandA;
		Operator = op;
		Mask = mask;
		Space = space;
		IsConstantCheck = true; // L'opérateur IN n'a de sens qu'avec des constantes
		OperandBList.clear();

		std::vector<std::string> values = StringUtilities::Split(operandBStr, '|');
		for(const auto& val : values) {
			OperandBList.push_back(HexUtilities::FromHex(val));
		}
	}

	// Génération dynamique du nom pour le snes_hires.txt
	string GetConditionName() override
	{
		string prefix = "memory";
		switch(Space) {
			case SnesMemorySpace::VRAM:  prefix = "vramMemory"; break;
			case SnesMemorySpace::OAM:   prefix = "oamMemory"; break;
			case SnesMemorySpace::CGRAM: prefix = "cgramMemory"; break;
			case SnesMemorySpace::ARAM:  prefix = "aramMemory"; break;
			default: break; // CPU
		}
		return prefix + (IsConstantCheck ? "CheckConstant" : "Check");
	}

	string ToString() override
	{
		stringstream out;
		out << "<condition>" << Name << "," << GetConditionName() << ",";
		out << HexUtilities::ToHex(OperandA) << ",";

		switch(Operator) {
			case HdPackConditionOperator::Equal: out << "=="; break;
			case HdPackConditionOperator::NotEqual: out << "!="; break;
			case HdPackConditionOperator::GreaterThan: out << ">"; break;
			case HdPackConditionOperator::LowerThan: out << "<"; break;
			case HdPackConditionOperator::LowerThanOrEqual: out << "<="; break;
			case HdPackConditionOperator::GreaterThanOrEqual: out << ">="; break;
			case HdPackConditionOperator::In: out << "In"; break; // Added by Gwenolo
		}
		out << ",";

		if(Operator == HdPackConditionOperator::In) {
			for(size_t i = 0; i < OperandBList.size(); i++) {
				out << HexUtilities::ToHex(OperandBList[i]);
				if(i < OperandBList.size() - 1) out << "|";
			}
		} else {
			out << HexUtilities::ToHex(OperandB);
		}

		if(Mask != 0xFF) {
			out << "," << HexUtilities::ToHex(Mask);
		}

		return out.str();
	}
};

// ---------------------------------------------------------
// 3. CONDITIONS DE RENDU (Miroirs, Positions)
// ---------------------------------------------------------
struct SnesHdPackHorizontalMirroringCondition : public SnesHdPackCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::HMirror; }
	string GetConditionName() override { return "hmirror"; }
	string ToString() override { return ""; }
	bool IsExcludedFromFile() override { return true; }

	// SNES : On utilise notre SnesHdPixelData
	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		return tile && tile->HFlip;
	}
};

struct SnesHdPackVerticalMirroringCondition : public SnesHdPackCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::VMirror; }
	string GetConditionName() override { return "vmirror"; }
	string ToString() override { return ""; }
	bool IsExcludedFromFile() override { return true; }

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		return tile && tile->VFlip;
	}
};
// ---------------------------------------------------------
// 4. MEMORY CHECKS (Constantes et Variables)
// ---------------------------------------------------------

// --- VARIANTE 1 : COMPARAISON AVEC UNE CONSTANTE (ex: == 3F) ---
struct SnesHdPackMemoryCheckConstantCondition : public SnesHdPackBaseMemoryCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::MemoryCheckConstant; }
	SnesHdPackMemoryCheckConstantCondition() { _useCache = true; }

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		if(!_screenInfo) return false;

		// On injecte le marqueur d'espace mémoire dans l'adresse pour lire la bonne cachette
		uint32_t finalAddress = OperandA | (uint32_t)Space;
		uint8_t a = (uint8_t)(_screenInfo->WatchedAddressValues[finalAddress] & Mask);
		uint8_t b = OperandB; // Constante

		switch(Operator) {
			case HdPackConditionOperator::Equal: return a == b;
			case HdPackConditionOperator::NotEqual: return a != b;
			case HdPackConditionOperator::GreaterThan: return a > b;
			case HdPackConditionOperator::LowerThan: return a < b;
			case HdPackConditionOperator::LowerThanOrEqual: return a <= b;
			case HdPackConditionOperator::GreaterThanOrEqual: return a >= b;
			case HdPackConditionOperator::In:
			{
				for(uint8_t valB : OperandBList) {
					if(a == valB) return true;
				}
				return false;
			}
			default: return false;
		}
	}
};

// --- VARIANTE 2 : COMPARAISON ENTRE DEUX REGISTRES (ex: == 8000) ---
struct SnesHdPackMemoryCheckCondition : public SnesHdPackBaseMemoryCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::MemoryCheck; }
	SnesHdPackMemoryCheckCondition() { _useCache = true; }

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		if(!_screenInfo) return false;

		// Adresse A (Espace Mémoire cible)
		uint32_t finalAddressA = OperandA | (uint32_t)Space;
		uint8_t a = (uint8_t)(_screenInfo->WatchedAddressValues[finalAddressA] & Mask);

		// Adresse B (Ici OperandB est une adresse ! On assume qu'elle est dans le même Espace Mémoire)
		uint32_t finalAddressB = OperandB | (uint32_t)Space;
		uint8_t b = (uint8_t)(_screenInfo->WatchedAddressValues[finalAddressB] & Mask);

		switch(Operator) {
			case HdPackConditionOperator::Equal: return a == b;
			case HdPackConditionOperator::NotEqual: return a != b;
			case HdPackConditionOperator::GreaterThan: return a > b;
			case HdPackConditionOperator::LowerThan: return a < b;
			case HdPackConditionOperator::LowerThanOrEqual: return a <= b;
			case HdPackConditionOperator::GreaterThanOrEqual: return a >= b;
			case HdPackConditionOperator::In: return false; // In n'a pas de sens entre 2 registres
			default: return false;
		}
	}
};

// ---------------------------------------------------------
// 5. CONDITIONS DE Positions (TILES, SPRITES)
// ---------------------------------------------------------

struct SnesHdPackSpriteAtPositionCondition : public SnesHdPackBaseTileCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::SpriteAtPosition; }
	string GetConditionName() override { return "spriteAtPosition"; }

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override;


};

enum class SnesHdPackConditionScope { Screen, Layer };

//--------------------------------------------------------------------------------
struct SnesHdPackTileAtPositionCondition : public SnesHdPackBaseTileCondition
{
	uint8_t TargetLayers = 0xFF; // Bitmask for layers


	bool CheckAnyX = false;
	bool CheckAnyY = false;
	SnesHdPackConditionScope Scope = SnesHdPackConditionScope::Screen;

	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::TileAtPosition; }
	string GetConditionName() override { return "tileAtPosition"; }

	void InitializeLayers(const string& layerStr)
	{
		if(layerStr == "all" || layerStr.empty()) {
			TargetLayers = 0xFF;
		} else {
			TargetLayers = 0;
			auto layers = StringUtilities::Split(layerStr, '|');
			for(auto& l : layers) {
				int layerIndex = std::stoi(l) - 1;
				if(layerIndex >= 0 && layerIndex < 4) {
					TargetLayers |= (1 << layerIndex);
				}
			}
		}
	}

	//--------------------
	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override;
	//------------------------

};
//--------------------------------------------------------------------------------

struct SnesHdPackFrameRangeCondition : public SnesHdPackCondition
{
	uint32_t OperandA = 0; // Le Diviseur
	uint32_t OperandB = 0; // La Valeur de Comparaison

	SnesHdPackFrameRangeCondition() { _useCache = true; } // Si ta classe de base gère le cache

	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::FrameRange; } // Assure-toi que FrameRange existe dans l'enum !
	string GetConditionName() override { return "frameRange"; }

	void Initialize(uint32_t operandA, uint32_t operandB)
	{
		// Sécurité anti-crash : on interdit la division par zéro !
		OperandA = (operandA == 0) ? 1 : operandA;
		OperandB = operandB;
	}

	string ToString() override
	{
		stringstream out;
		out << "<condition>" << Name << "," << GetConditionName() << ",";
		out << OperandA << ",";
		out << OperandB;

		return out.str();
	}

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		// Si _screenInfo n'est pas dispo, on renvoie false
		if(!_screenInfo) return false;

		// La fameuse formule d'animation temporelle
		return (_screenInfo->FrameNumber % OperandA) >= OperandB;
	}
};
//--------------------------------------------------------------------------------

class SnesHdPackTileNearbyCondition : public SnesHdPackCondition
{
public:
	int OffsetX = 0;
	int OffsetY = 0;
	bool CheckAnyX = false;
	bool CheckAnyY = false;

	uint64_t MemoryHash = 0;
	uint32_t PaletteHash = 0;
	bool IgnorePalette = false;

	// On implémente les fonctions virtuelles pures de la classe mère
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::TileNearby; }
	std::string GetConditionName() override { return "tileNearby"; }

	std::string ToString() override
	{
		// Sert souvent pour le debug dans Mesen
		return "tileNearby condition: " + Name;
	}

	void Initialize(int offsetX, int offsetY, bool anyX, bool anyY, uint64_t memHash, uint32_t palHash, bool ignorePal)
	{
		OffsetX = offsetX;
		OffsetY = offsetY;
		CheckAnyX = anyX;
		CheckAnyY = anyY;
		MemoryHash = memHash;
		PaletteHash = palHash;
		IgnorePalette = ignorePal;

		// Les conditions basées sur la position relative ne peuvent généralement pas utiliser le cache global 
		// de la même manière qu'un MemoryCheck, car le résultat dépend des coordonnées X/Y évaluées.
		_useCache = false;
	}

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override;
};

//---------------------------------------------------------------------------------------------------------------------------------
class SnesHdPackSpriteNearbyCondition : public SnesHdPackCondition
{
public:
	int OffsetX = 0;
	int OffsetY = 0;
	bool CheckAnyX = false;
	bool CheckAnyY = false;

	uint64_t MemoryHash = 0;
	uint32_t PaletteHash = 0;
	bool IgnorePalette = false;

	// On implémente les fonctions virtuelles pures de la classe mère
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::SpriteNearby; }
	std::string GetConditionName() override { return "spriteNearby"; }

	std::string ToString() override
	{
		// Sert souvent pour le debug dans Mesen
		return "tileNearby condition: " + Name;
	}

	void Initialize(int offsetX, int offsetY, bool anyX, bool anyY, uint64_t memHash, uint32_t palHash, bool ignorePal)
	{
		OffsetX = offsetX;
		OffsetY = offsetY;
		CheckAnyX = anyX;
		CheckAnyY = anyY;
		MemoryHash = memHash;
		PaletteHash = palHash;
		IgnorePalette = ignorePal;

		// Les conditions basées sur la position relative ne peuvent généralement pas utiliser le cache global 
		// de la même manière qu'un MemoryCheck, car le résultat dépend des coordonnées X/Y évaluées.
		_useCache = false;
	}

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override;
};
```

---

## ./SnesHdPackLoader.cpp

```cpp
#include "pch.h"
#include <filesystem>
#include <random>
#include "SNES/HdPacks/SnesHdPackLoader.h"
#include "SNES/HdPacks/SnesHdPackConditions.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/VirtualFile.h"
#include <fstream>
#include <iostream>
#include <algorithm>

#include "SNES/SnesPpu.h"
#include "SNES/SnesPpuTypes.h"

// --- DÉCODEUR VIDÉO MPEG-1 ---
#define PL_MPEG_IMPLEMENTATION
#include "Shared/pl_mpeg.h" // Modifie le chemin si tu l'as mis dans un autre dossier
// -----------------------------


// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------


bool SnesHdPackLoader::_isLoaded = false;
std::shared_ptr<SnesHdPackData> SnesHdPackLoader::ActivePack = nullptr; // C'est lui le vrai patron maintenant !

#define logError(y) LogError(y)
#define checkConstraint(x, y) if(!(x)) { logError(y); return; }

// ---------------------------------------------------------------------------
// Static API
// ---------------------------------------------------------------------------
bool SnesHdPackLoader::LoadHdPack(const std::string& definitionFile, std::shared_ptr<SnesHdPackData>& outData)
{
	std::ifstream file(definitionFile);
	if(!file.is_open()) {
		MessageManager::Log("[SnesHDPack] Failed to open: " + definitionFile);
		return false;
	}

	outData = std::make_shared<SnesHdPackData>();
	std::string rootFolder = FolderUtilities::GetFolderName(definitionFile);

	outData->RootFolder = rootFolder;

	// Dictionnaire pour ne créer qu'un seul objet BitmapInfo par PNG
	std::unordered_map<std::string, SnesHdPackBitmapInfo*> loadedBitmaps;

	std::string lineContent;
	std::vector<std::string> imageCatalog;
	std::unordered_map<std::string, SnesHdPackCondition*> conditionsByName;

	outData->PackFolderPath = rootFolder;

	int currentLine = 0;

	while(std::getline(file, lineContent)) {
		currentLine++;

		// 1. Nettoyage de la ligne (Retours chariot, commentaires, espaces)
		lineContent.erase(std::remove(lineContent.begin(), lineContent.end(), '\r'), lineContent.end());
		size_t commentPos = lineContent.find('#');
		if(commentPos != std::string::npos) {
			lineContent = lineContent.substr(0, commentPos);
		}
		lineContent.erase(0, lineContent.find_first_not_of(" \t"));

		if(lineContent.empty()) {
			continue;
		}

		// --- PARSING ---


		// ====================================================================
		// 1. ÉTAPE COMMUNE : EXTRACTION DES CONDITIONS (Pour TOUS les tags !)
		// ====================================================================
		std::vector<SnesHdPackCondition*> currentConditions;

		if(lineContent.substr(0, 1) == "[") {
			size_t endOfCondition = lineContent.find_first_of(']', 1);
			if(endOfCondition != std::string::npos) {
				std::string conditionString = lineContent.substr(1, endOfCondition - 1);

				// Découpage par le caractère '&'
				std::vector<std::string> condNames = StringUtilities::Split(conditionString, '&');
				for(const std::string& name : condNames) {
					auto result = conditionsByName.find(name);
					if(result != conditionsByName.end()) {
						currentConditions.push_back(result->second);
					}
				}
				// On retire la partie [...] de la ligne pour le parsing qui suit
				lineContent = lineContent.substr(endOfCondition + 1);
			}
		}

		///   <IMG> TAGS
		if(lineContent.substr(0, 5) == "<img>") {
			imageCatalog.push_back(lineContent.substr(5));
		} else if(lineContent.substr(0, 7) == "<scale>") {
			try { outData->Scale = std::stoi(lineContent.substr(7)); } catch(...) {}

		}

		///   <TILE> TAGS AVEC GESTION DES SYMÉTRIES (HFLIP, VFLIP, HVFLIP)
		if(lineContent.substr(0, 6) == "<tile>" ||
			lineContent.substr(0, 12) == "<tile_hflip>" ||
			lineContent.substr(0, 12) == "<tile_vflip>" ||
			lineContent.substr(0, 13) == "<tile_hvflip>") {
			// 1. On détecte quel est le type exact de la balise
			bool reqHFlip = false;
			bool reqVFlip = false;
			std::string data = "";

			if(lineContent.substr(0, 13) == "<tile_hvflip>") {
				reqHFlip = true; reqVFlip = true;
				data = lineContent.substr(13);
			} else if(lineContent.substr(0, 12) == "<tile_hflip>") {
				reqHFlip = true;
				data = lineContent.substr(12);
			} else if(lineContent.substr(0, 12) == "<tile_vflip>") {
				reqVFlip = true;
				data = lineContent.substr(12);
			} else {
				data = lineContent.substr(6);
			}

			std::stringstream ss(data);
			std::string item;
			std::vector<std::string> tokens;

			while(std::getline(ss, item, ',')) {
				tokens.push_back(StringUtilities::Trim(item));
			}

			// On s'attend à 5 paramètres (Hash, Palette, ImageIndex_OU_NomFichier, X, Y)
			if(tokens.size() >= 5) {
				uint64_t memoryHash = 0;
				uint32_t paletteHash = 0;
				int x = 0;
				int y = 0;
				std::string pngPath = "";

				try {
					// Parsing des paramètres garantis numériques
					memoryHash = std::stoull(tokens[0], nullptr, 16);
					paletteHash = std::stoull(tokens[1]);
					x = std::stoi(tokens[3]);
					y = std::stoi(tokens[4]);

					// --- LA GESTION DU PARAMÈTRE IMAGE ---
					std::string imageParam = tokens[2];

					// 1. Est-ce que ce paramètre finit par ".png"  ?
					// Si oui, on sait directement que c'est un fichier.
					if(imageParam.length() >= 4 && imageParam.substr(imageParam.length() - 4) == ".png") {
						pngPath = imageParam;
					} else {
						// 2. Sinon, on essaie de le lire comme un Index numérique
						try {
							int imageIndex = std::stoi(imageParam);

							// Sécurité sur la taille du catalogue
							if(imageIndex >= 0 && (size_t)imageIndex < imageCatalog.size()) {
								pngPath = imageCatalog[imageIndex];
							} else {
								MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - <tile> : Image Index out of bounds : " + imageParam);
								continue;
							}
						} catch(...) {
							// Si std::stoi échoue et que ça ne finit pas par .png, on assume que c'est quand même un nom de fichier 
							// (l'utilisateur a peut-être oublié l'extension, ou c'est un chemin sans extension)
							pngPath = imageParam;
						}
					}

				} catch(const std::exception& e) {
					MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - <tile> : Parameter Conversion error.");
					continue;
				}

				// Si on n'a pas trouvé de chemin valide, on ignore cette ligne
				if(pngPath.empty()) {
					continue;
				}

				// ---  GESTION MULTIPLE DU MOT-CLÉ "Randomize" ---
				std::string randomToken = "Randomize";
				size_t randPos;

				// Tant qu'il reste le mot "Randomize" dans le chemin...
				while((randPos = pngPath.find(randomToken)) != std::string::npos) {

					// 1. On coupe le chemin en deux
					std::string parentRelative = pngPath.substr(0, randPos);
					std::string afterRandomize = pngPath.substr(randPos + randomToken.length());

					// 2. On construit le chemin absolu du dossier parent
					std::string parentFullPath = FolderUtilities::CombinePath(rootFolder, parentRelative);

					std::vector<std::string> subFolders;
					try {
						// 3. On scanne le dossier physique pour lister ses sous-dossiers
						for(const auto& entry : std::filesystem::directory_iterator(parentFullPath)) {
							if(entry.is_directory()) {
								subFolders.push_back(entry.path().filename().string());
							}
						}
					} catch(const std::exception& e) {
						MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - Error reading 'Randomize' Folder : " + parentFullPath);
						pngPath = ""; // On vide pour annuler le chargement
						break;
					}

					// 4. On tire un dossier au hasard
					if(!subFolders.empty()) {
						static std::random_device rd;
						static std::mt19937 gen(rd());
						std::uniform_int_distribution<> distrib(0, subFolders.size() - 1);

						std::string chosenFolder = subFolders[distrib(gen)];

						// 5. On met à jour pngPath. La boucle va repartir et chercher le prochain "Randomize" !
						pngPath = parentRelative + chosenFolder + afterRandomize;
					} else {
						MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - No SubFolder in : " + parentFullPath);
						pngPath = "";
						break;
					}
				}

				// Si la boucle a échoué en cours de route, on passe à la ligne suivante
				if(pngPath.empty()) {
					continue;
				}

				// 2. CREATION OU RECUPERATION DU BITMAP
				if(loadedBitmaps.find(pngPath) == loadedBitmaps.end()) {
					auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
					bitmap->PngName = pngPath;

					// On charge les octets bruts du fichier PNG en RAM
					std::string fullPath = FolderUtilities::CombinePath(rootFolder, pngPath);
					VirtualFile pngFile(fullPath);
					if(pngFile.IsValid()) {
						pngFile.ReadFile(bitmap->FileData);
					} else {
						MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - Png file not found : " + fullPath);
					}

					loadedBitmaps[pngPath] = bitmap.get();
					outData->ImageFileData.push_back(std::move(bitmap));
				}

				// 3. CONSTRUCTION DE LA CLEF
				SnesHdTileKey key;
				key.MemoryHash = memoryHash;
				key.PaletteHash = paletteHash;
				key.Bpp = SnesBpp::Bpp4; // Valeur par défaut
				key.IsLargeTile = false;
				key.IsSprite = false;

				key.IsHFlip = reqHFlip; 
				key.IsVFlip = reqVFlip;

				// 4. CONSTRUCTION DE LA TUILE
				SnesHdPackTileInfo* tile = new SnesHdPackTileInfo();
				tile->MemoryHash = key.MemoryHash;
				tile->PaletteHash = key.PaletteHash;
				tile->Bpp = key.Bpp;
				tile->IsLargeTile = key.IsLargeTile;
				tile->IsSprite = false;

				tile->IsHFlip = reqHFlip; 
				tile->IsVFlip = reqVFlip;

				tile->X = x;
				tile->Y = y;
				tile->Width = 8;
				tile->Height = 8;


				// =======================================================
				//  Lecture du 6ème paramètre optionnel
				// =======================================================
				if(tokens.size() >= 6) {
					std::string opt = StringUtilities::ToLower(StringUtilities::Trim(tokens[5]));
					if(opt == "1" || opt == "true" || opt == "ignore") {
						tile->IgnoreColorMath = true;
					}
				}
				// =======================================================
				// 
				// On attache enfin le bitmap !
				tile->Bitmap = loadedBitmaps[pngPath];

				tile->Conditions = currentConditions;

				//    "QUADRUPLE BADGE" ---

				// 1. On prepare la clé (on la reset par secu)
				key.IsLargeTile = false;

				// 2. On boucle sur les deux BPP les plus probables (2 et 4)
				std::vector<SnesBpp> bppsToTry = { SnesBpp::Bpp2, SnesBpp::Bpp4 };
				for(auto currentBpp : bppsToTry) {
					key.Bpp = currentBpp;

					// 3. Et pour chaque BPP, on boucle sur IsSprite
					key.IsSprite = false;
					outData->TileByKey[key].push_back(tile); // Enregistre BG + BPP courant

					key.IsSprite = true;
					outData->TileByKey[key].push_back(tile); // Enregistre OBJ + BPP courant
				}

			}
		}
		//-----------------------------------------------------------------------
		/// PARSING  <CONDITION> TAGS
		else if(lineContent.substr(0, 11) == "<condition>") {
			std::string data = lineContent.substr(11);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				std::string baseConditionName = tokens[0]; // On garde le nom original
				std::string conditionType = tokens[1];

				// Managing NOT Condition ( ! ) : On fait 2 tours ! Un normal (i=0), et un inversé (i=1)
				for(int inverted = 0; inverted < 2; inverted++) {

					// Au deuxième tour, on ajoute un "!" devant le nom
					std::string conditionName = (inverted == 1) ? "!" + baseConditionName : baseConditionName;


					std::unique_ptr<SnesHdPackCondition> condition;
					
					// ==========================================================
					// 1. CONDITIONS : MEMORY CHECKS (VRAM, OAM, CGRAM, ARAM, CPU)
					// ==========================================================
					if(conditionType.find("memoryCheck") != std::string::npos || conditionType.find("MemoryCheck") != std::string::npos) {

						// A. Détection du type de comparaison (Constante vs Registre)
						bool isConstant = (conditionType.find("Constant") != std::string::npos);

						// B. Détection de l'Espace Mémoire ciblé
						SnesMemorySpace space = SnesMemorySpace::CPU; // Par défaut
						if(conditionType.find("vram") == 0 || conditionType.find("ppu") == 0) space = SnesMemorySpace::VRAM;
						else if(conditionType.find("oam") == 0)   space = SnesMemorySpace::OAM;
						else if(conditionType.find("cgram") == 0) space = SnesMemorySpace::CGRAM;
						else if(conditionType.find("aram") == 0 || conditionType.find("spc") == 0) space = SnesMemorySpace::ARAM;

						// C. Extraction des paramètres communs
						uint32_t operandA = HexUtilities::FromHex(tokens[2]);
						HdPackConditionOperator op = ParseConditionOperator(tokens[3]);

						uint32_t mask = 0xFF;
						if(tokens.size() > 5) {
							mask = HexUtilities::FromHex(tokens[5]);
						}

						bool In_operator = (op == HdPackConditionOperator::In);

						// D. Instanciation de la bonne classe
						if(isConstant) {
							condition.reset(new SnesHdPackMemoryCheckConstantCondition());
							SnesHdPackMemoryCheckConstantCondition* condPtr = (SnesHdPackMemoryCheckConstantCondition*)condition.get();

							if(In_operator) {
								condPtr->InitializeForInOperator(operandA, op, tokens[4], (uint8_t)mask, space);
							} else {
								uint32_t operandB = HexUtilities::FromHex(tokens[4]);
								condPtr->Initialize(operandA, op, operandB, (uint8_t)mask, space, true);
							}
						} else {
							condition.reset(new SnesHdPackMemoryCheckCondition());
							SnesHdPackMemoryCheckCondition* condPtr = (SnesHdPackMemoryCheckCondition*)condition.get();

							uint32_t operandB = HexUtilities::FromHex(tokens[4]); // Ici Operand B est une adresse !
							condPtr->Initialize(operandA, op, operandB, (uint8_t)mask, space, false);
						}

						// E. Finalisation et Enregistrement
						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							// 1. Initialisation (attache ScreenInfo)
							condPtr->Initialize(&outData->ScreenInfo, outData.get());

							// 2. Ajout de l'adresse A à surveiller (AVEC le marqueur spatial !)
							uint32_t addrToWatchA = operandA | (uint32_t)space;
							outData->WatchedMemoryAddresses.emplace(addrToWatchA);

							// 3. Si c'est un check entre 2 registres, il faut aussi surveiller l'adresse B !
							if(!isConstant && !In_operator) {
								uint32_t addrToWatchB = HexUtilities::FromHex(tokens[4]) | (uint32_t)space;
								outData->WatchedMemoryAddresses.emplace(addrToWatchB);
							}

							// 4. Enregistrement dans le dictionnaire
							conditionsByName[conditionName] = condPtr;

							// 5. Déplacement final
							outData->Conditions.emplace_back(std::move(condition));

						} else {
							MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - Unknown or invalid condition type : " + conditionType);
						}
					}
					// ==========================================================
					// 2. CONDITION : FRAME RANGE 
					// ==========================================================
					else if(conditionType == "frameRange") {
						condition.reset(new SnesHdPackFrameRangeCondition());
						SnesHdPackFrameRangeCondition* frameCond = (SnesHdPackFrameRangeCondition*)condition.get();

						uint32_t opA = 1;
						uint32_t opB = 0;

						if(tokens.size() > 3) {
							opA = std::stoul(StringUtilities::Trim(tokens[2]));
							opB = std::stoul(StringUtilities::Trim(tokens[3]));
						}

						frameCond->Initialize(opA, opB);

						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							condPtr->Initialize(&outData->ScreenInfo, outData.get());
							conditionsByName[conditionName] = condPtr;
							outData->Conditions.emplace_back(std::move(condition));
						}
					}

					// -------------------------------------------------------------
					// PARSING TILE ET SPRITE NEARBY (Avec support Joker = OnScreen)
					// -------------------------------------------------------------
									
					else if(conditionType == "tileNearby" || conditionType == "spriteNearby") {

						bool isSprite = (conditionType == "spriteNearby");

						if(isSprite) condition.reset(new SnesHdPackSpriteNearbyCondition());
						else condition.reset(new SnesHdPackTileNearbyCondition());

						bool anyX = false, anyY = false;
						int offX = 0, offY = 0;

						// 1. Parsing du Décalage X avec gestion des Jokers
						std::string strX = StringUtilities::Trim(tokens[2]);
						if(strX == "*" || strX == "all" || strX == "any") {
							anyX = true;
						} else {
							offX = std::stoi(strX);
						}

						// 2. Parsing du Décalage Y avec gestion des Jokers
						std::string strY = StringUtilities::Trim(tokens[3]);
						if(strY == "*" || strY == "all" || strY == "any") {
							anyY = true;
						} else {
							offY = std::stoi(strY);
						}

						// 3. Extraction Hash & Palette (LE TRI INTELLIGENT EST ICI)
						uint64_t memHash = 0;
						std::string hashStr = StringUtilities::Trim(tokens[4]);

						if(hashStr.length() > 6) {
							// C'est un long Hash 64-bits
							memHash = std::stoull(hashStr, nullptr, 16);
						} else {
							// C'est un petit Index
							memHash = HexUtilities::FromHex(hashStr);
						}

						uint32_t palHash = 0;
						bool ignorePal = (tokens[5] == "ignore");

						if(!ignorePal) {
							palHash = HexUtilities::FromHex(tokens[5]);
						}

						// 4. Initialisation selon le type
						if(isSprite) {
							((SnesHdPackSpriteNearbyCondition*)condition.get())->Initialize(offX, offY, anyX, anyY, memHash, palHash, ignorePal);
						} else {
							((SnesHdPackTileNearbyCondition*)condition.get())->Initialize(offX, offY, anyX, anyY, memHash, palHash, ignorePal);
						}

						// 5. Finalisation de la condition
						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							condPtr->Initialize(&outData->ScreenInfo, outData.get());
							conditionsByName[conditionName] = condPtr;
							outData->Conditions.emplace_back(std::move(condition));
						}
						}


					// -------------------------------------------------------------
					// PARSING TILE ET SPRITE ATPOSITION (Adapté à l'architecture unique_ptr)
					// -------------------------------------------------------------

					else if(conditionType == "tileAtPosition") {
						condition.reset(new SnesHdPackTileAtPositionCondition());
						SnesHdPackTileAtPositionCondition* tileCond = (SnesHdPackTileAtPositionCondition*)condition.get();

						// 1. Parsing de la Colonne (X) avec gestion du Joker '*'
						std::string strX = StringUtilities::Trim(tokens[2]);
						int tCol = 0;
						if(strX == "*" || strX == "all") {
							tileCond->CheckAnyX = true;
						} else {
							tCol = std::stoi(strX);
						}

						// 2. Parsing de la Ligne (Y) avec gestion du Joker '*'
						std::string strY = StringUtilities::Trim(tokens[3]);
						int tRow = 0;
						if(strY == "*" || strY == "all") {
							tileCond->CheckAnyY = true;
						} else {
							tRow = std::stoi(strY);
						}

						// 3. Extraction Hash & Palette

								// --- POUR GÉRER LES HASHS GÉANTS ---
						uint64_t memHash = 0;
						std::string hashStr = StringUtilities::Trim(tokens[4]);

						if(hashStr.length() > 6) {
							// C'est un long Hash, on force la lecture en 64-bits avec stoull !
							memHash = std::stoull(hashStr, nullptr, 16);
						} else {
							// C'est un petit Index (ex: 101), on utilise FromHex
							memHash = HexUtilities::FromHex(hashStr);
						}
						// ------------------------------------------------

						uint32_t palHash = HexUtilities::FromHex(tokens[5]);
						bool ignorePal = (tokens[5] == "ignore");

						// Et on passe les valeurs à ta fonction Initialize (qui est prête !)
						tileCond->Initialize(tCol, tRow, memHash, palHash, ignorePal);

						// 4. Paramètre optionnel "Layers" (tokens[6])
						if(tokens.size() > 6) {
							tileCond->InitializeLayers(tokens[6]);
						} else {
							tileCond->InitializeLayers("all");
						}

						// 5.Paramètre optionnel "Scope" (tokens[7])
						if(tokens.size() > 7) {
							std::string scopeStr = StringUtilities::Trim(tokens[7]);
							if(scopeStr == "layer") {
								tileCond->Scope = SnesHdPackConditionScope::Layer;
							} else {
								tileCond->Scope = SnesHdPackConditionScope::Screen;
							}
						}

						// 6. Finalisation de la condition
						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							// Initialisation vitale (pour attacher le ScreenInfo et le PPU)
							condPtr->Initialize(&outData->ScreenInfo, outData.get());

							// Enregistrement dans le dictionnaire
							conditionsByName[conditionName] = condPtr;

							// Déplacement final
							outData->Conditions.emplace_back(std::move(condition));
						}
					}

					// PARSING spriteAtPosition  --------------
					else if(conditionType == "spriteAtPosition") {
						condition.reset(new SnesHdPackSpriteAtPositionCondition());

						// Syntaxe: spriteAtPosition, X, Y, MemoryHash, PaletteHash
						int tX = std::stoi(tokens[2]) * 8;
						int tY = std::stoi(tokens[3]) * 8;

						// --- LE TRI INTELLIGENT DU PARSEUR (Index ou Hash ?) ---
						uint64_t memHash = 0;
						std::string hashStr = StringUtilities::Trim(tokens[4]);

						if(hashStr.length() > 6) {
							// C'est un long Hash 64-bits
							memHash = std::stoull(hashStr, nullptr, 16);
						} else {
							// C'est un petit Index
							memHash = HexUtilities::FromHex(hashStr);
						}
						// -------------------------------------------------------

						uint32_t palHash = HexUtilities::FromHex(tokens[5]);
						bool ignorePal = (tokens[5] == "ignore");

						SnesHdPackSpriteAtPositionCondition* spriteCond = (SnesHdPackSpriteAtPositionCondition*)condition.get();
						spriteCond->Initialize(tX, tY, memHash, palHash, ignorePal);

						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							condPtr->Initialize(&outData->ScreenInfo, outData.get());
							conditionsByName[conditionName] = condPtr;
							outData->Conditions.emplace_back(std::move(condition));
						}
					}
					


				}  
			}
		} // end parsing spriteAtPosition TAG
		
		// =============================================================
		/// PARSING <addition> TAG
		else if(lineContent.substr(0, 10) == "<addition>") {
			std::string data = lineContent.substr(10);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 6) {
				SnesHdPackAddition add;
				try {
					// hash for pivot tile
					add.BaseMemoryHash = std::stoull(StringUtilities::Trim(tokens[0]), nullptr, 16);
					add.BasePaletteHash = std::stoul(StringUtilities::Trim(tokens[1]));

					// Hash for additional tile (must be  Hexa in hires file txt !)
					add.AddedMemoryHash = std::stoull(StringUtilities::Trim(tokens[2]), nullptr, 16);
					add.AddedPaletteHash = std::stoul(StringUtilities::Trim(tokens[3]));

					// Offsets
					add.OffsetX = std::stoi(StringUtilities::Trim(tokens[4]));
					add.OffsetY = std::stoi(StringUtilities::Trim(tokens[5]));

					// Ignorer la palette (Optionnel, 7ème paramètre)
					if(tokens.size() >= 7) {
						std::string opt = StringUtilities::ToLower(StringUtilities::Trim(tokens[6]));
						if(opt == "1" || opt == "true" || opt == "ignore") {
							add.IgnoreBasePalette = true;
						}
					}

					// On attache les conditions éventuelles (ex: [memoryCheck...])
					add.Conditions = currentConditions;

					// On l'enregistre dans le dictionnaire de SnesHdPackData 
					outData->Additions[add.BaseMemoryHash].push_back(add);

					MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - <addition> registered.");
				} catch(const std::exception& e) {
					MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - <addition> Parsing Error : Check your Hash format.");
				}
			}
		}// end parsing <addition> TAG

		// =============================================================
		// PARSING BACKGROUND TAG ==============
		else if(lineContent.substr(0, 12) == "<background>") {
			bool tileX = false;
			bool tileY = false;

			std::string data = lineContent.substr(12);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 1) {
				auto bg = std::make_unique<SnesHdBackgroundInfo>();

				std::string filename = StringUtilities::Trim(tokens[0]);
				std::string fullPath = FolderUtilities::CombinePath(rootFolder, filename);

				// === DÉTECTION DOSSIER OU FICHIER (ANIMATION) ===
				std::error_code ec;
				bool isDirectory = std::filesystem::is_directory(fullPath, ec);

				// ---PARAMÈTRES D'ANIMATION par parsing de PNG List dans le dossier---
				if(tokens.size() > 10) {
					bg->FramesBetweenSteps = std::stoi(StringUtilities::Trim(tokens[10]));
				}
				if(tokens.size() > 11) {
					std::string revStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[11]));
					bg->ReverseAnim = (revStr == "true" || revStr == "1" || revStr == "yes");
				}

				if(isDirectory) {
					bg->IsAnimated = true;
					std::vector<std::string> pngFiles;

					for(const auto& entry : std::filesystem::directory_iterator(fullPath)) {
						if(entry.path().extension() == ".png") {
							pngFiles.push_back(entry.path().filename().string());
						}
					}

					std::sort(pngFiles.begin(), pngFiles.end());
					if(bg->ReverseAnim) {
						std::reverse(pngFiles.begin(), pngFiles.end());
					}

					for(const std::string& frameFile : pngFiles) {
						std::string frameRelPath = FolderUtilities::CombinePath(filename, frameFile);
						std::string frameFullPath = FolderUtilities::CombinePath(rootFolder, frameRelPath);

						SnesHdPackBitmapInfo* targetBitmap = nullptr;
						for(auto& bmp : outData->ImageFileData) {
							if(bmp->PngName == frameRelPath) {
								targetBitmap = bmp.get();
								break;
							}
						}

						if(targetBitmap == nullptr) {
							outData->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
							targetBitmap = outData->ImageFileData.back().get();
							targetBitmap->PngName = frameRelPath;

							VirtualFile pngFile(frameFullPath);
							if(pngFile.IsValid()) pngFile.ReadFile(targetBitmap->FileData);
						}
						bg->AnimFrames.push_back(targetBitmap);
					}

					if(!bg->AnimFrames.empty()) bg->Bitmap = bg->AnimFrames[0];

				} else {
					// === LOGIQUE FICHIER UNIQUE (STATIQUE) ===
					SnesHdPackBitmapInfo* targetBitmap = nullptr;
					for(auto& bmp : outData->ImageFileData) {
						if(bmp->PngName == filename) {
							targetBitmap = bmp.get();
							break;
						}
					}

					if(targetBitmap == nullptr) {
						outData->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
						targetBitmap = outData->ImageFileData.back().get();
						targetBitmap->PngName = filename;

						VirtualFile pngFile(fullPath);
						if(pngFile.IsValid()) pngFile.ReadFile(targetBitmap->FileData);
					}
					bg->Bitmap = targetBitmap;
				}

				// --- PARAMÈTRES OPTIONNELS EXISTANTS ---
				if(tokens.size() >= 2) bg->Brightness = std::stof(tokens[1]);
				if(tokens.size() >= 3) bg->ScrollXRatio = std::stof(tokens[2]);
				if(tokens.size() >= 4) bg->ScrollYRatio = std::stof(tokens[3]);
				if(tokens.size() >= 5) bg->ZIndex = std::stoi(tokens[4]);
				if(tokens.size() >= 6) bg->OffsetX = std::stoi(tokens[5]);
				if(tokens.size() >= 7) bg->OffsetY = std::stoi(tokens[6]);
				if(tokens.size() >= 8) bg->MasterScrollLayer = std::stoi(tokens[7]);
				if(tokens.size() > 8) {
					std::string txStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[8]));
					tileX = (txStr == "true" || txStr == "1" || txStr == "yes");
				}
				if(tokens.size() > 9) {
					std::string tyStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[9]));
					tileY = (tyStr == "true" || tyStr == "1" || tyStr == "yes");
				}

				bg->TileX = tileX;
				bg->TileY = tileY;
				bg->Conditions = currentConditions;
				outData->Backgrounds.push_back(std::move(bg));
			}
		}// END BACKGROUND TAG PARSING 

		//---------------------------------------------------------------------------
		/// PARSING <m7img> TAGS
	
		else if(lineContent.substr(0, 7) == "<m7img>") {
			std::string data = lineContent.substr(7);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 3) {
				std::string filename = StringUtilities::Trim(tokens[0]);
				uint32_t mapHash = (uint32_t)HexUtilities::FromHex(tokens[1]);
				uint32_t m7Scale = std::stoi(tokens[2]);

				// LECTURE DES PARAMÈTRES OPTIONNELS (ColorMath et Bilinear)
				bool ignoreColorMath = false;
				bool useBilinear = false;

				if(tokens.size() >= 4) {
					std::string opt = StringUtilities::Trim(tokens[3]);
					if(opt == "1" || opt == "true" || opt == "ignore") {
						ignoreColorMath = true;
					}
				}
				if(tokens.size() >= 5) {
					std::string optB = StringUtilities::Trim(tokens[4]);
					if(optB == "1" || optB == "true" || optB == "bilinear") useBilinear = true;
				}


				// --- RÉCUPÉRATION OU CRÉATION DU BITMAP ---
				SnesHdPackBitmapInfo* targetBitmap = nullptr;
				for(auto& bmp : outData->ImageFileData) {
					if(bmp->PngName == filename) {
						targetBitmap = bmp.get();
						break;
					}
				}

				if(targetBitmap == nullptr) {
					outData->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
					targetBitmap = outData->ImageFileData.back().get();
					targetBitmap->PngName = filename;

					std::string fullPath = FolderUtilities::CombinePath(rootFolder, filename);
					VirtualFile pngFile(fullPath);
					if(pngFile.IsValid()) {
						pngFile.ReadFile(targetBitmap->FileData);
					} else {
						MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - Mode 7 Image not found : " + fullPath);
					}
				}

				// --- CRÉATION DE L'OBJET MODE 7 ---
				auto m7Map = std::make_unique<SnesHdMode7Image>();
				m7Map->Bitmap = targetBitmap;
				m7Map->Scale = m7Scale;
				m7Map->IgnoreColorMath = ignoreColorMath;
				m7Map->UseBilinearFilter = useBilinear;

				MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - [DEBUG - HD M7 PARSER] Map : " + filename + " | Bilinear activated : " + (useBilinear ? std::string("YES") : std::string("NO")));

				m7Map->Conditions = currentConditions;

				// On stocke dans le dictionnaire de outData
				outData->Mode7Maps[mapHash].push_back(std::move(m7Map));

				MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - [DEBUG - HD M7 PARSER] Map Mode 7 loaded : " + filename + " (Hash: " + tokens[1] + ")");
			}
		}



		//------------------------------------------------------------------------
		/// PARSING <m7tiles> TAGS
		else if(lineContent.substr(0, 9) == "<m7tiles>") {
			std::string data = lineContent.substr(9);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 3) {
				std::string filename = StringUtilities::Trim(tokens[0]);
				// C'est de l'Hexa, on utilise FromHex !
				uint32_t tileHash = (uint32_t)HexUtilities::FromHex(tokens[1]);
				uint32_t m7Scale = std::stoi(tokens[2]);

				// =======================================================
				// NOUVEAUTÉ : LECTURE DES COORDONNÉES X ET Y (S'ils existent)
				// =======================================================
				int srcX = 0;
				int srcY = 0;
				if(tokens.size() >= 5) {
					try {
						srcX = std::stoi(StringUtilities::Trim(tokens[3]));
						srcY = std::stoi(StringUtilities::Trim(tokens[4]));
					} catch(...) {
						MessageManager::Log("[SnesHDPack] M7Tiles Coord Parse Error line " + std::to_string(currentLine));
					}
				}
				// =======================================================

				// --- RÉCUPÉRATION OU CRÉATION DU BITMAP ---
				SnesHdPackBitmapInfo* targetBitmap = nullptr;
				for(auto& bmp : outData->ImageFileData) {
					if(bmp->PngName == filename) {
						targetBitmap = bmp.get();
						break;
					}
				}

				if(targetBitmap == nullptr) {
					outData->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
					targetBitmap = outData->ImageFileData.back().get();
					targetBitmap->PngName = filename;

					std::string fullPath = FolderUtilities::CombinePath(rootFolder, filename);
					VirtualFile pngFile(fullPath);
					if(pngFile.IsValid()) {
						pngFile.ReadFile(targetBitmap->FileData);
					} else {
						MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - [SnesHDPack] Tile Mode 7 not found : " + fullPath);
					}
				}

				// --- CRÉATION DE L'OBJET MODE 7 POUR LA TUILE ---
				auto m7Map = std::make_unique<SnesHdMode7Image>();
				m7Map->Bitmap = targetBitmap;
				m7Map->Scale = m7Scale;

				// =======================================================
				// NOUVEAUTÉ : AFFECTATION DES COORDONNÉES À L'OBJET
				// =======================================================
				m7Map->X = srcX;
				m7Map->Y = srcY;

				// LA LIGNE MAGIQUE POUR LES TUILES MODE 7 EST ICI !
				m7Map->Conditions = currentConditions;

				// On stocke la tuile dans le dictionnaire Mode7 existant !
				outData->Mode7Maps[tileHash].push_back(std::move(m7Map));

				// --- LE MOUCHARD DU PARSEUR ---
				MessageManager::Log("[MOUCHARD PARSEUR TILEM7] Tuile chargee:  " + filename + " | Hash: " + HexUtilities::ToHex(tileHash) +
									" | X: " + std::to_string(srcX) + " | Y: " + std::to_string(srcY));
			}
		}


		//-------------------------------
		

		/// PARSING <disableOriginalTiles> TAG
		else if(lineContent == "<disableOriginalTiles>") {
			outData->DisableOriginalTiles = true;
			MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - [SnesHDPack] Option DisableOriginalTiles activated !");
		}
		//------------------------------------------------------------------------
		/// PARSING <disableTile> TAGS
		else if(lineContent.substr(0, 13) == "<disableTile>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(13), ',');
			ProcessDisableTileTag(tokens, currentConditions, outData, currentLine);
}
		/// PARSING <disableOriginalSprites> TAG
		else if(lineContent == "<disableOriginalSprites>") {
			outData->DisableOriginalSprites = true;
			MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - [SnesHDPack] Option DisableOriginalSprites activated !");
		}

		/// PARSING <OVERSCAN> TAG --------------------------------
		else if(lineContent.substr(0, 10) == "<overscan>") {
			std::string data = lineContent.substr(10);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				outData->HasOverscanConfig = true;

				// L'ordre SNES/NES classique : Top, Right, Bottom, Left
				outData->Overscan.Top = std::stoi(StringUtilities::Trim(tokens[0]));
				outData->Overscan.Right = std::stoi(StringUtilities::Trim(tokens[1]));
				outData->Overscan.Bottom = std::stoi(StringUtilities::Trim(tokens[2]));
				outData->Overscan.Left = std::stoi(StringUtilities::Trim(tokens[3]));
			}
		}

		/// PARSING <setLayersOrder> TAG --------------------------------
		else if(lineContent.substr(0, 16) == "<setLayersOrder>") {
			std::string data = lineContent.substr(16);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				SnesHdLayerOrderRule rule;
				rule.Conditions = currentConditions; // On attache les [conditions]

				// On lit les 4 valeurs pour BG1, BG2, BG3, BG4
				for(int i = 0; i < 4; i++) {
					try {
						rule.ZIndexBg[i] = static_cast<uint8_t>(std::stoi(StringUtilities::Trim(tokens[i])));
					} catch(...) {
						rule.ZIndexBg[i] = 20; // Valeur par défaut si erreur de frappe
					}
				}

				outData->LayerOrders.push_back(rule);

				MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) +
					" - <setLayersOrder> added : " + tokens[0] + "," + tokens[1] + "," + tokens[2] + "," + tokens[3]);
			}

		}


		/// PARSING <disableBg*> TAG --------------------------------
		else if(lineContent == "<disableBg1>") {
			outData->HasDisableBg1Tag = true;
			outData->DisableBg1Conditions = currentConditions;
		} else if(lineContent == "<disableBg2>") {
			outData->HasDisableBg2Tag = true;
			outData->DisableBg2Conditions = currentConditions;
		} else if(lineContent == "<disableBg3>") {
			outData->HasDisableBg3Tag = true;
			outData->DisableBg3Conditions = currentConditions;
		} else if(lineContent == "<disableBg4>") {
			outData->HasDisableBg4Tag = true;
			outData->DisableBg4Conditions = currentConditions;
		}
		/// PARSING <disablesublayer> TAG --------------------------------
		else if(lineContent == "<disablesublayer>" || lineContent == "<disableSubLayer>" ) {
			outData->HasDisableSubLayerTag = true;
			outData->DisableSubLayerConditions = currentConditions;
			MessageManager::Log("[SnesHDPack] Tag found : <disableSubLayer>");
		}

		/// PARSING <disableColorMath> TAG --------------------------------
		else if(lineContent == "<disableColorMath>") {
			// On assigne les conditions lues juste au-dessus (ex: [memoryCheck...])
			outData->HasDisableColorMathTag = true; // On prévient le moteur !
			outData->DisableColorMathConditions = currentConditions;
		}


		// PARSING <msuToOgg> avec gestion des [conditions] -----------------------
		size_t msuTagPos = lineContent.find("<msuToOgg>");
		if(msuTagPos != std::string::npos) {

			// 1. Extraction des conditions (ex: [cond1&cond2])
			std::vector<SnesHdPackCondition*> trackConditions;
			if(msuTagPos > 0 && lineContent[0] == '[') {
				size_t endBracket = lineContent.find(']');
				if(endBracket != std::string::npos && endBracket < msuTagPos) {
					std::string condStr = lineContent.substr(1, endBracket - 1);
					std::vector<std::string> condNames = StringUtilities::Split(condStr, '&');
					for(auto& cName : condNames) {
						std::string cleanName = StringUtilities::Trim(cName);
						if(conditionsByName.find(cleanName) != conditionsByName.end()) {
							trackConditions.push_back(conditionsByName[cleanName]);
						}
					}
				}
			}

			// 2. Parsing des données audio (ID, Fichier, Loop)
			std::string data = lineContent.substr(msuTagPos + 10); // 10 = longueur de <msuToOgg>
			std::vector<std::string> msuTokens = StringUtilities::Split(data, ',');

			if(msuTokens.size() >= 2) {
				try {
					std::string trackIdStr = StringUtilities::Trim(msuTokens[0]);
					if(!trackIdStr.empty()) {
						int trackId = std::stoi(trackIdStr);

						SnesHdMsuTrack track;
						track.Filename = StringUtilities::Trim(msuTokens[1]);
						track.Conditions = trackConditions; // On accroche les conditions à la piste !

						if(msuTokens.size() >= 3) {
							std::string loopStr = StringUtilities::Trim(msuTokens[2]);
							if(!loopStr.empty()) {
								track.LoopPoint = std::stoul(loopStr);
							}
						}

						// On AJOUTE cette piste à la liste des possibilités pour cet ID
						outData->MsuTracks[trackId].push_back(track);
					}
				} catch(...) {} // Bouclier anti-crash
			}
		}// END parsing MSU2OGG





/// PARSING <VIDEO> TAGS --------------------------------
		else if(lineContent.substr(0, 7) == "<video>") {
			std::string data = lineContent.substr(7);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				auto video = std::make_unique<SnesHdVideoInfo>();

				// 1. Nom du fichier
				video->Filename = StringUtilities::Trim(tokens[0]);

				// 2. Mode
				std::string modeStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[1]));
				video->Mode = (modeStr == "pause") ? SnesHdVideoMode::Pause : SnesHdVideoMode::Overlay;

				// 3. Boutons de Skip (Masques Mesen classiques)
				std::string skipStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[2]));
				video->SkipButtonsMask = 0;
				if(skipStr.find("start") != std::string::npos) video->SkipButtonsMask |= 0x1000;
				if(skipStr.find("a") != std::string::npos)     video->SkipButtonsMask |= 0x0080;
				if(skipStr.find("b") != std::string::npos)     video->SkipButtonsMask |= 0x8000;

				// 4. Play Only Once
				std::string onceStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[3]));
				video->PlayOnlyOnce = (onceStr == "true" || onceStr == "1" || onceStr == "yes");

				// 5. Mute Audio (Optionnel)
				if(tokens.size() >= 5) {
					std::string muteStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[4]));
					video->MuteAudio = (muteStr == "true" || muteStr == "1" || muteStr == "yes");
				}

				// 6. Loop (Optionnel)
				if(tokens.size() >= 6) {
					std::string loopStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[5]));
					video->Loop = (loopStr == "true" || loopStr == "1" || loopStr == "yes");
				}

				// 7 & 8. Coordonnées X et Y (Optionnel)
				if(tokens.size() >= 8) {
					try {
						video->X = std::stoi(StringUtilities::Trim(tokens[6]));
						video->Y = std::stoi(StringUtilities::Trim(tokens[7]));
					} catch(...) {
						video->X = 0;
						video->Y = 0;
					}
				}

				// 9. Z-Index (Optionnel, 99 par défaut)
				if(tokens.size() >= 9) {
					try {
						video->ZIndex = static_cast<uint8_t>(std::stoi(StringUtilities::Trim(tokens[8])));
					} catch(...) {
						video->ZIndex = 99; // Fallback par défaut si la valeur est invalide
					}
				}

				// MOUCHARD PARSEUR
				MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) +
					" - [HD VIDEO] Tag found : " + video->Filename +
					" | PlayOnlyOnce: " + (video->PlayOnlyOnce ? "YES" : "NO") +
					" | Z-Index: " + std::to_string(video->ZIndex));

				video->Conditions = currentConditions;
				outData->Videos.push_back(std::move(video));
			}
		}
		// End Parsing VIDEOS ----------



	}
		


	//MessageManager::Log("[HD TRACE 1] Fichier texte lu ! Tuiles chargees : " + std::to_string(outData->TileByKey.size()));

	// Tri décroissant des backgrounds (Z-Index 100 en premier, 0 en dernier)
	std::sort(outData->Backgrounds.begin(), outData->Backgrounds.end(),
		[](const std::unique_ptr<SnesHdBackgroundInfo>& a, const std::unique_ptr<SnesHdBackgroundInfo>& b) {
		return a->ZIndex > b->ZIndex;
	});

	return true;
}

//-----------------------------------------------------------------------------------------------------




void SnesHdPackLoader::Clear() {
    //_tileMap.clear();
    //_sheetImages.clear();
	ActivePack = nullptr; // Cela va automatiquement vider les tuiles, images et conditions de la RAM !
	_isLoaded = false;
}
//-----------------------------------------------------------------------------------------------------
SnesHdPackTileInfo* SnesHdPackLoader::GetHdTile(uint64_t memoryHash, uint32_t paletteHash, SnesBpp bpp, bool isLargeTile, bool isSprite, bool isHFlip, bool isVFlip, int x, int y)
{
	if(ActivePack == nullptr) return nullptr;

	SnesHdTileKey key;
	key.MemoryHash = memoryHash;
	key.PaletteHash = paletteHash;
	key.Bpp = bpp;
	key.IsLargeTile = isLargeTile;
	key.IsSprite = isSprite;
	key.IsHFlip = isHFlip;
	key.IsVFlip = isVFlip;

	auto it = ActivePack->TileByKey.find(key);
	if(it != ActivePack->TileByKey.end()) {
		for(SnesHdPackTileInfo* tile : it->second) {
			if(tile->MatchesCondition(x, y, nullptr)) {
				return tile;
			}
		}
	}

	if(isHFlip || isVFlip) {
		key.IsHFlip = false;
		key.IsVFlip = false;
		it = ActivePack->TileByKey.find(key);
		if(it != ActivePack->TileByKey.end()) {
			for(SnesHdPackTileInfo* tile : it->second) {
				if(tile->MatchesCondition(x, y, nullptr)) {
					return tile;
				}
			}
		}
	}

	return nullptr;
}
//----------------------------------------------------------------------------
SnesHdPackBitmapInfo* SnesHdPackLoader::GetSheet(uint32_t imageIndex)
{
	if(ActivePack != nullptr && imageIndex < ActivePack->ImageFileData.size()) {
		return ActivePack->ImageFileData[imageIndex].get();
	}
	return nullptr;
}


//-------------------------------------------------------------------------------------------
void SnesHdPackLoader::ProcessScaleTag(const std::string& content) {
    int scale = std::stoi(content);
    if(scale < 1 || scale > 10) {
        logError("Scale must be between 1 and 10. Defaulting to 1.");
        scale = 1;
    }
    _data->Scale = static_cast<uint32_t>(scale);
}
//--------------------------------------------------------------------------------------------
bool SnesHdPackLoader::ProcessImgTag(const std::string& content)
{
	// On crée notre nouvel objet conteneur
	_data->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
	SnesHdPackBitmapInfo* bitmapInfo = _data->ImageFileData.back().get();

	bitmapInfo->PngName = content;

	// Sur SNES, on utilise FolderUtilities et VirtualFile pour lire les octets
	// (Adapte _hdPackFolder si ta variable s'appelle rootFolder)
	std::string fullPath = FolderUtilities::CombinePath(_hdPackFolder, content);
	VirtualFile pngFile(fullPath);

	if(pngFile.IsValid()) {
		pngFile.ReadFile(bitmapInfo->FileData);
		return true;
	} else {
		// L'image n'existe pas, on annule et on prévient
		_data->ImageFileData.pop_back();
		MessageManager::Log("[SnesHDPack] Image introuvable sur le disque : " + fullPath);
		return false;
	}
}
//-----------------------------------------------------------------------------------------------

void SnesHdPackLoader::ProcessTileTag(const std::vector<std::string>& tokens, const std::vector<SnesHdPackCondition*>& conditions)
{
	checkConstraint(tokens.size() >= 5, "Tile tag must have at least 5 parameters");

	// 1. Extraction des paramètres
	uint64_t memoryHash = std::stoull(tokens[0], nullptr, 16);
	uint32_t paletteHash = static_cast<uint32_t>(std::stoul(tokens[1]));
	int imageIndex = std::stoi(tokens[2]);
	int x = std::stoi(tokens[3]);
	int y = std::stoi(tokens[4]);

	// Sécurité sur l'index de l'image (On utilise _data ici !)
	if(imageIndex < 0 || imageIndex >= _data->ImageFileData.size()) return;

	// 2. Création de la VRAIE tuile HD
	SnesHdPackTileInfo* tile = new SnesHdPackTileInfo();
	tile->MemoryHash = memoryHash;
	tile->PaletteHash = paletteHash;
	tile->Width = 8;
	tile->Height = 8;
	tile->X = x;
	tile->Y = y;

	if(tokens.size() >= 6) {
		tile->BeyondPriority = (std::stoi(tokens[5]) != 0);
	}

	// 3. On attache l'image et les conditions
	tile->Bitmap = _data->ImageFileData[imageIndex].get();
	tile->Conditions = conditions;

	// 4. On crée la clé pour le dictionnaire
	SnesHdTileKey key;
	key.MemoryHash = memoryHash;
	key.PaletteHash = paletteHash;
	key.Bpp = SnesBpp::Bpp4; // Par défaut
	key.IsLargeTile = false;

	// 5. ENREGISTREMENT DANS L'UNIQUE DICTIONNAIRE
	key.IsSprite = false;
	_data->TileByKey[key].push_back(tile);

	key.IsSprite = true; 
	_data->TileByKey[key].push_back(tile);

	// Garde une trace pour pouvoir nettoyer la RAM quand le jeu se ferme
	_data->Tiles.push_back(std::unique_ptr<SnesHdPackTileInfo>(tile));
}
//----------------------------------------------------------------------------------------
void SnesHdPackLoader::LogError(const std::string& msg) {
    MessageManager::Log("[SnesHDPack - Line " + std::to_string(_currentLine) + "] " + msg);
    _errorCount++;
} 



//------------------------------------------------------------------------------------
void SnesHdPackLoader::ProcessDisableTileTag(const std::vector<std::string>& tokens, const std::vector<SnesHdPackCondition*>& conditions, std::shared_ptr<SnesHdPackData>& outData, int currentLine)
{
	if(tokens.size() < 2) {
		MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - <disableTile> nécessite au moins 2 paramètres (Hash, Palette).");
		return;
	}

	uint64_t memoryHash = 0;
	uint32_t paletteHash = 0;

	try {
		memoryHash = std::stoull(StringUtilities::Trim(tokens[0]), nullptr, 16);
		paletteHash = std::stoul(StringUtilities::Trim(tokens[1]));
	} catch(...) {
		MessageManager::Log("[SnesHDPack] Line " + std::to_string(currentLine) + " - <disableTile> : Erreur de conversion des paramètres.");
		return;
	}

	// 1. Création de la tuile fantôme
	SnesHdPackTileInfo* tile = new SnesHdPackTileInfo();
	tile->MemoryHash = memoryHash;
	tile->PaletteHash = paletteHash;
	tile->IsDisabled = true;
	tile->Conditions = conditions;

	// 2. Création de la clé de recherche
	SnesHdTileKey key;
	key.MemoryHash = memoryHash;
	key.PaletteHash = paletteHash;
	key.Bpp = SnesBpp::Bpp4; // BPP par défaut pour s'assurer que ça matche
	key.IsLargeTile = false;

	// 3. Injection dans le dictionnaire (Pour les Backgrounds et les Sprites)
	key.IsSprite = false;
	outData->TileByKey[key].push_back(tile);

	key.IsSprite = true;
	outData->TileByKey[key].push_back(tile);

	// 4. Sauvegarde dans la liste principale pour la libération mémoire
	outData->Tiles.push_back(std::unique_ptr<SnesHdPackTileInfo>(tile));
}
//------------------------------------------------------------------------------------
HdPackConditionOperator SnesHdPackLoader::ParseConditionOperator(std::string& opString)
{
	if(opString == "==") return HdPackConditionOperator::Equal;
	if(opString == "!=") return HdPackConditionOperator::NotEqual;
	if(opString == ">") return HdPackConditionOperator::GreaterThan;
	if(opString == "<") return HdPackConditionOperator::LowerThan;
	if(opString == "<=") return HdPackConditionOperator::LowerThanOrEqual;
	if(opString == ">=") return HdPackConditionOperator::GreaterThanOrEqual;
	if(opString == "In" || opString == "IN" || opString == "in") return HdPackConditionOperator::In;

	return HdPackConditionOperator::Invalid;
}

// ==========================================================
// IMPLÉMENTATION DES VIDÉOS HD
// ==========================================================
bool SnesHdVideoInfo::MatchesCondition()
{
	if(PlayOnlyOnce && HasPlayed) return false;

	for(auto* cond : Conditions) {
		if(!cond->CheckCondition(0, 0, nullptr)) return false;
	}
	return true;
}
```

---

## ./SnesHdPackLoader.h

```h
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

	 static void ProcessDisableTileTag(const std::vector<std::string>& tokens, const std::vector<SnesHdPackCondition*>& conditions, std::shared_ptr<SnesHdPackData>& outData, int currentLine);



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
```

---

## ./SnesHdVideoFilter.cpp

```cpp
#include "pch.h"
#include <vector>
#include <algorithm>
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/HdPacks/SnesHdPackLoader.h"
#include "SNES/SnesConsole.h"
#include "SNES/SnesPpu.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"

#include "Shared/pl_mpeg.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/Audio/SoundMixer.h"


//-----------------------------------------------------------------------------------------
SnesHdVideoFilter::SnesHdVideoFilter(Emulator* emu) : SnesDefaultVideoFilter(emu) {}


//----------------------------------------------------------------------------------------

FrameInfo SnesHdVideoFilter::GetFrameInfo()
{
	static bool frameLog = false;
	if(!frameLog) {
		MessageManager::Log("[HD TRACE 2.5] GetFrameInfo a pris le relais pour l'affichage !");
		frameLog = true;
	}

	_sdFrameInfo = SnesDefaultVideoFilter::GetFrameInfo();
	FrameInfo info = _sdFrameInfo;

	// On agrandit la fenêtre en fonction de l'échelle du pack
	if(SnesHdPackLoader::ActivePack) {
		uint32_t scale = SnesHdPackLoader::ActivePack->Scale;
		info.Width *= scale;
		info.Height *= scale;

	}
	return info;
}
//-------------------------------------------------------------------------

// ============================================================================
// IMPLEMENTATION DE LA REGLE DE Z-INDEX
// ============================================================================
bool SnesHdLayerOrderRule::MatchesCondition(int x, int y, SnesHdPixelData* tile)
{
	for(auto cond : Conditions) {
		if(!cond->CheckCondition(x, y, tile)) {
			return false;
		}
	}
	return true;
}

//---------------------------------------------------------------------------------------------------------------
void SnesHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	static bool filterLog = false;

	// La structure pour mémoriser TOUTES les tuiles HD tronquées à "déplier"
	struct ExpandedTile
	{
		SnesHdPackTileInfo* Tile;
		int startX;
		int startY;
		uint8_t zIndex;
		bool hFlip;
		bool vFlip;
		uint16_t baseColor;
		uint8_t oamIndex; // to manage Sprite order in OAM
	};
	std::vector<ExpandedTile> tilesToExpand;


	// Si pas de pack HD, on utilise le filtre classique
	if(!SnesHdPackLoader::ActivePack) {
		SnesDefaultVideoFilter::ApplyFilter(ppuOutputBuffer);
		return;
	}

	uint32_t scale = SnesHdPackLoader::ActivePack->Scale;
	uint32_t* out = GetOutputBuffer(); // Le tableau de la grande image finale

	uint32_t sdWidth = _sdFrameInfo.Width;
	uint32_t sdHeight = _sdFrameInfo.Height;
	OverscanDimensions overscan = GetOverscan();

	uint32_t xOffset = overscan.Left;
	uint32_t yOffset = overscan.Top * _baseFrameInfo.Width;


	// On récupère notre grand écran HD caché généré par le PPU !
	SnesConsole* console = (SnesConsole*)_emu->GetConsole().get();
	SnesHdPixelData* hdBuffer = console->GetPpu()->GetHdScreenBuffer();
	SnesHdPixelData* hdSubBuffer = console->GetPpu()->GetHdSubScreenOutputBuffer();

	// ====================================================================
	// DÉCLARATIONS HD ET INITIALISATION DU Z-BUFFER (CORRIGÉ)
	// ====================================================================
	uint32_t outWidth = sdWidth * scale;  // Largeur totale de l'écran HD final
	uint32_t hdHeight = sdHeight * scale; // <-- On a remonté hdHeight ici !

	if(_zBuffer.size() != outWidth * hdHeight) {
		_zBuffer.resize(outWidth * hdHeight);
	}
	memset(_zBuffer.data(), 0, _zBuffer.size()); // On efface le Z-Buffer pour cette nouvelle frame
	// ====================================================================
	for(uint32_t y = 0; y < sdHeight; y++) {

		// ====================================================================
		// ÉTAPE 1 : PRE-PASS MODE 7 (On déroule le sol SOUS les sprites)
		// ====================================================================
		bool hasMode7OnLine = false;
		SnesHdPixelData refM7Data;
		uint32_t refX = 0;
		uint16_t lineSnesPixel = 0;

		// On scanne la ligne pour attraper le PREMIER pixel Mode 7 visible
		for(uint32_t tx = 0; tx < sdWidth; tx++) {
			uint32_t offset = y * _baseFrameInfo.Width + tx + yOffset + xOffset;
			if(hdBuffer[offset].IsMode7) {
				refM7Data = hdBuffer[offset];
				lineSnesPixel = ppuOutputBuffer[offset];
				refX = tx; // On mémorise OÙ on a trouvé ce pixel
				hasMode7OnLine = true;
				break;
			}
		}

		if(hasMode7OnLine) {
			// On dessine le Mode 7 sur TOUTE la largeur de l'écran, sans trou !
			for(uint32_t x = 0; x < sdWidth; x++) {

				// ============================================================
				// LA CORRECTION EST ICI : L'EXTRAPOLATION DE LA MATRICE !
				// ============================================================
				SnesHdPixelData lineM7Data = refM7Data;

				// On calcule la distance entre le pixel actuel (x) et notre pixel de référence (refX)
				int32_t xDiff = (int32_t)x - (int32_t)refX;

				// On avance la texture proportionnellement à cette distance !
				// (M7A = Vitesse de défilement horizontal, M7C = Vitesse de défilement vertical)
				lineM7Data.M7XValue = refM7Data.M7XValue + (refM7Data.M7A * xDiff);
				lineM7Data.M7YValue = refM7Data.M7YValue + (refM7Data.M7C * xDiff);
				// ============================================================

				// --- Calcul de la profondeur (Z-Index) pour le fond Mode 7 ---
				uint8_t pixelZ = 20 + (lineM7Data.SnesPriorityLevel * 4);
				if(lineM7Data.IsHighPriority) pixelZ += 2;

				if(lineM7Data.SourceLayer >= 0 && lineM7Data.SourceLayer < 4) {
					for(auto& rule : SnesHdPackLoader::ActivePack->LayerOrders) {
						if(rule.MatchesCondition(x, y, &lineM7Data)) {
							pixelZ = rule.ZIndexBg[lineM7Data.SourceLayer];
							if(lineM7Data.IsHighPriority) pixelZ += 2;
							break;
						}
					}
				}

				// ============================================================
				// CORRECTION DU CODE-BARRES : On lit le VRAI pixel natif pour chaque X !
				// ============================================================
				uint32_t currentOffset = y * _baseFrameInfo.Width + x + yOffset + xOffset;
				uint16_t currentSnesPixel = ppuOutputBuffer[currentOffset];

				uint32_t sdColor = SnesHdPackLoader::ActivePack->IsColorMathDisabled
					? _calculatedPalette[lineM7Data.BaseColor]
					: _calculatedPalette[currentSnesPixel];

				// --- LA VOIE RAPIDE (FAST PATH) MODE 7 GÉANT ---
				SnesHdMode7Image* m7Img = SnesHdPackLoader::ActivePack->GetMode7Image(lineM7Data.M7MapHash);

				if(m7Img && m7Img->Bitmap && !m7Img->Bitmap->PixelData.empty()) {
					uint32_t outScale = scale;
					uint32_t imgScale = m7Img->Scale;
					uint32_t mapDim = 1024 * imgScale;

					if(m7Img->Mipmaps.empty()) {
						m7Img->Mipmaps.push_back(m7Img->Bitmap->PixelData);
						uint32_t currentDim = mapDim;
						for(int level = 1; level <= 3; level++) {
							uint32_t prevDim = currentDim;
							currentDim /= 2;
							std::vector<uint32_t> nextMip(currentDim * currentDim, 0);
							const std::vector<uint32_t>& prevMip = m7Img->Mipmaps[level - 1];

							for(uint32_t my = 0; my < currentDim; my++) {
								for(uint32_t mx = 0; mx < currentDim; mx++) {
									uint32_t p00 = prevMip[(my * 2) * prevDim + (mx * 2)];
									uint32_t p10 = prevMip[(my * 2) * prevDim + (mx * 2 + 1)];
									uint32_t p01 = prevMip[(my * 2 + 1) * prevDim + (mx * 2)];
									uint32_t p11 = prevMip[(my * 2 + 1) * prevDim + (mx * 2 + 1)];

									uint32_t a1 = p00 >> 24, a2 = p10 >> 24, a3 = p01 >> 24, a4 = p11 >> 24;
									uint32_t sumA = a1 + a2 + a3 + a4;

									if(sumA == 0) {
										nextMip[my * currentDim + mx] = 0x00000000;
									} else {
										uint32_t r = (((p00 >> 16) & 0xFF) * a1 + ((p10 >> 16) & 0xFF) * a2 + ((p01 >> 16) & 0xFF) * a3 + ((p11 >> 16) & 0xFF) * a4) / sumA;
										uint32_t g = (((p00 >> 8) & 0xFF) * a1 + ((p10 >> 8) & 0xFF) * a2 + ((p01 >> 8) & 0xFF) * a3 + ((p11 >> 8) & 0xFF) * a4) / sumA;
										uint32_t b = ((p00 & 0xFF) * a1 + (p10 & 0xFF) * a2 + (p01 & 0xFF) * a3 + (p11 & 0xFF) * a4) / sumA;
										nextMip[my * currentDim + mx] = ((sumA / 4) << 24) | (r << 16) | (g << 8) | b;
									}
								}
							}
							m7Img->Mipmaps.push_back(nextMip);
						}
					}

					bool applyColorMath = !m7Img->IgnoreColorMath;
					int diffR = 0, diffG = 0, diffB = 0;

					if(applyColorMath) {
						uint32_t baseSdColor = _calculatedPalette[lineM7Data.BaseColor];
						diffR = ((sdColor >> 16) & 0xFF) - ((baseSdColor >> 16) & 0xFF);
						diffG = ((sdColor >> 8) & 0xFF) - ((baseSdColor >> 8) & 0xFF);
						diffB = (sdColor & 0xFF) - (baseSdColor & 0xFF);
					}

					int32_t baseHD_X = lineM7Data.M7XValue * imgScale;
					int32_t baseHD_Y = lineM7Data.M7YValue * imgScale;
					int32_t stepX = (lineM7Data.M7A * (int32_t)imgScale) / (int32_t)outScale;
					int32_t stepY = (lineM7Data.M7C * (int32_t)imgScale) / (int32_t)outScale;

					int64_t stepDistSq = ((int64_t)stepX * stepX + (int64_t)stepY * stepY) >> 16;
					int lod = 0;
					if(stepDistSq > 16) lod = 3;
					else if(stepDistSq > 4) lod = 2;
					else if(stepDistSq > 1) lod = 1;
					if(lod >= m7Img->Mipmaps.size()) lod = m7Img->Mipmaps.size() - 1;

					uint32_t currentMapDim = mapDim >> lod;
					uint32_t currentBitMask = currentMapDim - 1;
					const uint32_t* currentPixelData = m7Img->Mipmaps[lod].data();

					for(uint32_t sy = 0; sy < outScale; sy++) {
						int32_t cur_BaseX = baseHD_X + (lineM7Data.M7B * (int32_t)imgScale * (int32_t)sy) / (int32_t)outScale;
						int32_t cur_BaseY = baseHD_Y + (lineM7Data.M7D * (int32_t)imgScale * (int32_t)sy) / (int32_t)outScale;
						uint32_t outBaseIdx = (y * outScale + sy) * outWidth + (x * outScale);

						for(uint32_t sx = 0; sx < outScale; sx++) {
							int32_t exactX = cur_BaseX + stepX * (int32_t)sx;
							int32_t exactY = cur_BaseY + stepY * (int32_t)sx;
							int32_t lodExactX = exactX >> lod;
							int32_t lodExactY = exactY >> lod;
							int32_t pngX = lodExactX >> 8;
							int32_t pngY = lodExactY >> 8;

							uint32_t finalColor = sdColor;

							if(!(lineM7Data.M7LargeMap && (pngX < 0 || pngX >= (int32_t)currentMapDim || pngY < 0 || pngY >= (int32_t)currentMapDim))) {
								uint32_t hdColorFromPng = 0;

								if(m7Img->UseBilinearFilter) {
									uint32_t fracX = lodExactX & 0xFF;
									uint32_t fracY = lodExactY & 0xFF;
									uint32_t x0 = pngX & currentBitMask;
									uint32_t x1 = (pngX + 1) & currentBitMask;
									uint32_t y0 = pngY & currentBitMask;
									uint32_t y1 = (pngY + 1) & currentBitMask;

									uint32_t p00 = currentPixelData[y0 * currentMapDim + x0];
									uint32_t p10 = currentPixelData[y0 * currentMapDim + x1];
									uint32_t p01 = currentPixelData[y1 * currentMapDim + x0];
									uint32_t p11 = currentPixelData[y1 * currentMapDim + x1];

									uint32_t w00 = ((256 - fracX) * (256 - fracY)) >> 8;
									uint32_t w10 = (fracX * (256 - fracY)) >> 8;
									uint32_t w01 = ((256 - fracX) * fracY) >> 8;
									uint32_t w11 = (fracX * fracY) >> 8;
									uint32_t a = (((p00 >> 24) & 0xFF) * w00 + ((p10 >> 24) & 0xFF) * w10 + ((p01 >> 24) & 0xFF) * w01 + ((p11 >> 24) & 0xFF) * w11) >> 8;

									if(a > 0) {
										uint32_t r = (((p00 >> 16) & 0xFF) * w00 + ((p10 >> 16) & 0xFF) * w10 + ((p01 >> 16) & 0xFF) * w01 + ((p11 >> 16) & 0xFF) * w11) >> 8;
										uint32_t g = (((p00 >> 8) & 0xFF) * w00 + ((p10 >> 8) & 0xFF) * w10 + ((p01 >> 8) & 0xFF) * w01 + ((p11 >> 8) & 0xFF) * w11) >> 8;
										uint32_t b = ((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10 + (p01 & 0xFF) * w01 + (p11 & 0xFF) * w11) >> 8;
										hdColorFromPng = (a << 24) | (r << 16) | (g << 8) | b;
									} else {
										hdColorFromPng = 0x00000000;
									}
								} else {
									pngX &= currentBitMask;
									pngY &= currentBitMask;
									hdColorFromPng = currentPixelData[pngY * currentMapDim + pngX];
								}

								if((hdColorFromPng >> 24) > 0) {
									if(applyColorMath) {
										int outR = ((hdColorFromPng >> 16) & 0xFF) + diffR;
										int outG = ((hdColorFromPng >> 8) & 0xFF) + diffG;
										int outB = (hdColorFromPng & 0xFF) + diffB;

										outR = outR < 0 ? 0 : (outR > 255 ? 255 : outR);
										outG = outG < 0 ? 0 : (outG > 255 ? 255 : outG);
										outB = outB < 0 ? 0 : (outB > 255 ? 255 : outB);

										finalColor = (hdColorFromPng & 0xFF000000) | (outR << 16) | (outG << 8) | outB;
									} else {
										finalColor = hdColorFromPng | 0xFF000000;
									}

								} else {
									// CORRECTION METROID : Si la texture HD a un trou transparent, 
									// on efface la texture SNES originale pour laisser le vide !
									finalColor = 0x00000000;

									/*
									if(SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
										finalColor = 0x00000000;
									} else {
										finalColor = sdColor;
									}
									*/


								}
							}

							out[outBaseIdx + sx] = finalColor;
							_zBuffer[outBaseIdx + sx] = ((finalColor >> 24) == 0) ? 0 : pixelZ;
						}
					}
				}
			}
		}

		// ====================================================================
		// ÉTAPE 2 : LA BOUCLE CLASSIQUE (Sprites et Tuiles)
		// ====================================================================
		for(uint32_t x = 0; x < sdWidth; x++) {

			uint32_t bufferOffset = y * _baseFrameInfo.Width + x + yOffset + xOffset;
			uint16_t snesPixel = ppuOutputBuffer[bufferOffset];

			SnesHdPixelData hdDataMain = hdBuffer[bufferOffset];
			SnesHdPixelData hdDataSub = hdSubBuffer[bufferOffset];
			SnesHdPixelData hdData = hdDataMain;

			bool hasSubScreenHdElement = (hdDataSub.Tile != nullptr && !hdDataSub.Tile->HdTileData.empty());
			if(hasSubScreenHdElement) {
				hdData = hdDataSub;
			}

			// ====================================================================
			// NOUVEAU : COURT-CIRCUIT DU MODE 7 !
			// Si le pixel d'origine est du Mode 7 géant, on le saute : le Pre-Pass 
			// l'a déjà dessiné juste au-dessus.
			// ====================================================================
			if(hdData.IsMode7) {
				continue;
			}

			uint32_t sdColor = SnesHdPackLoader::ActivePack->IsColorMathDisabled
				? _calculatedPalette[hdData.BaseColor]
				: _calculatedPalette[snesPixel];

			uint8_t pixelZ = 20 + (hdData.SnesPriorityLevel * 4);
			if(hdData.IsHighPriority) {
				pixelZ += 2;
			}

			if(!hdData.IsSprite && hdData.SourceLayer >= 0 && hdData.SourceLayer < 4) {
				for(auto& rule : SnesHdPackLoader::ActivePack->LayerOrders) {
					if(!rule.MatchesCondition(x, y, &hdData)) {
						continue;
					}
					pixelZ = rule.ZIndexBg[hdData.SourceLayer];
					if(hdData.IsHighPriority) {
						pixelZ += 2;
					}
					break;
				}
			}

			// ======
			// LA VOIE RAPIDE N°2 : TUILES MODE 7 INDIVIDUELLES (CLIPPÉES)
			// ===
			if(hdData.IsMode7Tile) {
				SnesHdMode7Image* m7Img = SnesHdPackLoader::ActivePack->GetMode7Image(hdData.M7TileHash);

				if(m7Img && m7Img->Bitmap && !m7Img->Bitmap->PixelData.empty()) {
					const uint32_t* rawPixelData = m7Img->Bitmap->PixelData.data();

					uint32_t imgScale = m7Img->Scale;
					int32_t tileDim = 8 * imgScale;

					uint32_t pngWidth = m7Img->Bitmap->Width;
					uint32_t pngHeight = m7Img->Bitmap->Height;
					int32_t srcX = m7Img->X;
					int32_t srcY = m7Img->Y;

					uint32_t mapDim = 1024 * imgScale;
					uint32_t bitMask = mapDim - 1;

					uint32_t baseSdColor = _calculatedPalette[hdData.BaseColor];
					int diffR = ((sdColor >> 16) & 0xFF) - ((baseSdColor >> 16) & 0xFF);
					int diffG = ((sdColor >> 8) & 0xFF) - ((baseSdColor >> 8) & 0xFF);
					int diffB = (sdColor & 0xFF) - (baseSdColor & 0xFF);

					int32_t baseHD_X = hdData.M7XValue * imgScale;
					int32_t baseHD_Y = hdData.M7YValue * imgScale;

					int32_t snesX = (hdData.M7XValue >> 8) & 1023;
					int32_t snesY = (hdData.M7YValue >> 8) & 1023;
					int32_t tileMinX = (snesX & ~7) * imgScale;
					int32_t tileMinY = (snesY & ~7) * imgScale;

					int32_t stepX = (hdData.M7A * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepY = (hdData.M7C * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepYTermX = (hdData.M7B * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepYTermY = (hdData.M7D * (int32_t)imgScale) / (int32_t)scale;

					for(uint32_t sy = 0; sy < scale; sy++) {

						int32_t yTermX = stepYTermX * (int32_t)sy;
						int32_t yTermY = stepYTermY * (int32_t)sy;

						uint32_t outBaseIdx = (y * scale + sy) * outWidth + (x * scale);

						for(uint32_t sx = 0; sx < scale; sx++) {

							int32_t pngX = (baseHD_X + stepX * (int32_t)sx + yTermX) >> 8;
							int32_t pngY = (baseHD_Y + stepY * (int32_t)sx + yTermY) >> 8;

							pngX &= bitMask;
							pngY &= bitMask;

							int32_t localX = (pngX - tileMinX) & bitMask;
							int32_t localY = (pngY - tileMinY) & bitMask;

							// Clamp de sécurité absolu dans la limite de la tuile HD
							if(localX > (int32_t)(bitMask >> 1)) localX = 0;
							else if(localX >= tileDim) localX = tileDim - 1;

							if(localY > (int32_t)(bitMask >> 1)) localY = 0;
							else if(localY >= tileDim) localY = tileDim - 1;

							// Coordonnées de lecture finales dans l'atlas PNG
							uint32_t finalReadX = srcX + localX;
							uint32_t finalReadY = srcY + localY;

							// Si le PNG est trop petit (ou mal configuré dans hires.txt), on évite le crash et on ignore.
							if(finalReadX < pngWidth && finalReadY < pngHeight) {

								uint32_t hdColorFromPng = rawPixelData[finalReadY * pngWidth + finalReadX];

								// L'Alpha test : on ne dessine que les pixels opaques de la tuile
								if((hdColorFromPng >> 24) > 0) {
									int outR = ((hdColorFromPng >> 16) & 0xFF) + diffR;
									int outG = ((hdColorFromPng >> 8) & 0xFF) + diffG;
									int outB = (hdColorFromPng & 0xFF) + diffB;

									outR = outR < 0 ? 0 : (outR > 255 ? 255 : outR);
									outG = outG < 0 ? 0 : (outG > 255 ? 255 : outG);
									outB = outB < 0 ? 0 : (outB > 255 ? 255 : outB);

									uint32_t finalColor = (hdColorFromPng & 0xFF000000) | (outR << 16) | (outG << 8) | outB;

									out[outBaseIdx + sx] = finalColor;
									_zBuffer[outBaseIdx + sx] = pixelZ;
								}
							}
						}
					}
				}
				continue; // La tuile Mode 7 a été interceptée et rendue, on skip la voie classique.
			}



			// ====================================================================
			// LA CAPTURE UNIVERSELLE (SPRITES ET BACKGROUNDS)
			// On mémorise la position d'origine de TOUTE tuile ayant un PNG HD !
			// ====================================================================
			if(hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty()) {

				// CORRECTION DU BÉGAIEMENT : Calcul de la position VISUELLE (0 à 7) à l'écran
				int visualX = 0;
				if(hdData.IsSprite) {
					// Pour les Sprites : TileX va de 0 à 7 normalement
					visualX = hdData.HFlip ? (7 - hdData.TileX) : hdData.TileX;
				} else {
					// Pour les Backgrounds : TileX va de 7 à 0 normalement
					visualX = hdData.HFlip ? hdData.TileX : (7 - hdData.TileX);
				}

				// VFlip se comporte de la même manière pour les BGs et les Sprites
				int visualY = hdData.VFlip ? (7 - hdData.TileY) : hdData.TileY;

				// Le point de départ absolu de l'image PNG
				int startX = (int)x - visualX;
				int startY = (int)y - visualY;

				bool alreadyCaptured = false;
				// OPTIMISATION : On lit la liste à l'envers pour trouver immédiatement le doublon
				for(int i = (int)tilesToExpand.size() - 1; i >= 0; i--) {
					if(tilesToExpand[i].Tile == hdData.Tile && tilesToExpand[i].startX == startX && tilesToExpand[i].startY == startY) {
						alreadyCaptured = true;
						break;
					}
				}

				if(!alreadyCaptured) {
					tilesToExpand.push_back({ hdData.Tile, startX, startY, pixelZ, 
													  hdData.HFlip, hdData.VFlip, hdData.BaseColor, hdData.OamIndex });
				}
			}

			// ===============================================
			// LA VOIE CLASSIQUE : ON APPELLE LE STANDARD POUR TOUT LE MONDE !
			// ===============================================
			//ApplyFilterSpritesStandard(x, y, scale, outWidth, sdColor, hdData, pixelZ, out);

			// --- DÉBUT DE LA CORRECTION ---
			bool hasHdReplacement = (hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty());
			bool hideNative = hasHdReplacement;

			// On vérifie si l'utilisateur a globalement désactivé les tuiles ou sprites originaux via le pack
			if(hdData.IsSprite && SnesHdPackLoader::ActivePack->DisableOriginalSprites) {
				hideNative = true;
			} else if(!hdData.IsSprite && SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
				hideNative = true;
			}

			// Si l'élément natif doit être caché et qu'on est sur une ligne contenant du Mode 7,
			// on passe au pixel suivant pour ne pas écraser le fond Mode 7 généré à l'étape 1 avec du noir.
			if(hasMode7OnLine && hideNative) {
				continue;
			}
			// --- FIN DE LA CORRECTION ---

			ApplyFilterSpritesStandard(x, y, scale, outWidth, sdColor, hdData, pixelZ, out);

		}
	}
	// ====================================================================
	// LE DÉPLIAGE UNIVERSEL (SCREEN-SPACE EXPANSION)
	// On dessine l'intégralité du PNG pour vaincre le clipping (Sprites et BGs)
	// ====================================================================


	//             ==================================== 
	// INJECTION DES ADDITIONS HD DANS LE PIPELINE (Z-BUFFER)
	//             ==================================== 
	if(SnesHdPackLoader::ActivePack) {

		// On récupère les paramètres de décalage internes du PPU
		SnesConsole* console = (SnesConsole*)_emu->GetConsole().get();
		bool overscanFrame = console->GetPpu()->GetState().OverscanMode;
		OverscanDimensions overscan = GetOverscan();

		for(const auto& add : SnesHdPackLoader::ActivePack->ActiveAdditionsQueue) {

			SnesHdPackTileInfo* hdTile = SnesHdPackLoader::GetHdTile(
				add.MemoryHash,
				add.PaletteHash,
				SnesBpp::Bpp4,
				false,
				true, // isSprite = true
				add.HFlip,
				add.VFlip
			);

			if(hdTile && hdTile->Bitmap != nullptr && !hdTile->HdTileData.empty()) {

				// On dérive le Z-Index depuis la priorité matérielle capturée par le sprite parent
				uint8_t pixelZ = 20 + (add.Priority * 4);

				int finalScreenX = add.ScreenX - overscan.Left;
				int finalScreenY = (overscanFrame ? (add.ScreenY - 1) : (add.ScreenY +7 /* + 6*/)) - overscan.Top;

				// Ajout direct dans la file de rendu universelle !
				tilesToExpand.push_back({
					hdTile,
					finalScreenX,
					finalScreenY,
					pixelZ,
					add.HFlip, // Le PNG HD sera lui aussi inversé
					add.VFlip, // Le PNG HD sera lui aussi inversé
					0, // baseColor
					128 // On lui donne un OAM index de 128 (se dessinera juste derrière le sprite parent 0-127)
				});
			}
		}
	}


	//             ==================================== 
	// Sorting Sprites for a correct Display:
	//             ==================================== 
	// 
	 // On trie les tuiles par OAM Index décroissant !
	 // Les sprites du fond (ex: OAM 120) seront dessinés en premier.
	 // Les sprites devant (ex: OAM 0) seront dessinés en dernier et écraseront le reste !
	 // L'utilisation de stable_sort préserve l'ordre natif pour les tuiles qui ne sont pas des sprites (OAM 255).
	std::stable_sort(tilesToExpand.begin(), tilesToExpand.end(), [](const ExpandedTile& a, const ExpandedTile& b) {
		return a.oamIndex > b.oamIndex;
	});

	for(auto& item : tilesToExpand) {
		uint32_t pngWidth = item.Tile->Width * scale;
		uint32_t pngHeight = item.Tile->Height * scale;

		bool doHFlip = item.hFlip && !item.Tile->IsHFlip;
		bool doVFlip = item.vFlip && !item.Tile->IsVFlip;

		for(uint32_t py = 0; py < pngHeight; py++) {
			for(uint32_t px = 0; px < pngWidth; px++) {

				int32_t screenX = (item.startX * scale) + px;
				int32_t screenY = (item.startY * scale) + py;

				// Sécurité pour ne pas dessiner hors de l'écran
				if(screenX < 0 || screenX >= (int32_t)outWidth || screenY < 0 || screenY >= (int32_t)hdHeight) continue;

				uint32_t readX = doHFlip ? (pngWidth - 1 - px) : px;
				uint32_t readY = doVFlip ? (pngHeight - 1 - py) : py;

				uint32_t hdColorFromPng = item.Tile->HdTileData[readY * pngWidth + readX];
				uint32_t alpha = (hdColorFromPng >> 24) & 0xFF;

				if(alpha > 0) {
					uint32_t pixelIndex = screenY * outWidth + screenX;

					// === LE Z-TEST MAGIQUE ===
					/*
					if(item.zIndex >= _zBuffer[pixelIndex]) {

						uint32_t finalColor = hdColorFromPng;

						if(!(SnesHdPackLoader::ActivePack->IsColorMathDisabled || item.Tile->IgnoreColorMath)) {
							// Tu peux remettre le calcul de ColorMath ici si nécessaire
						}

						out[pixelIndex] = finalColor | 0xFF000000;
						_zBuffer[pixelIndex] = item.zIndex;
					}
					*/
					// === LE Z-TEST MAGIQUE ET L'ALPHA BLENDING ===
					// === LE Z-TEST MAGIQUE ET L'ALPHA BLENDING ===
					if(item.zIndex >= _zBuffer[pixelIndex]) {

						uint32_t finalColor = hdColorFromPng;

						if(!(SnesHdPackLoader::ActivePack->IsColorMathDisabled || item.Tile->IgnoreColorMath)) {
							// Logique ColorMath si nécessaire
						}

						if(alpha == 255) {
							// Opacité totale : On écrase
							out[pixelIndex] = finalColor | 0xFF000000;
							_zBuffer[pixelIndex] = item.zIndex;
						} else {
							// Transparence : Alpha Blending 32-bits STANDARD (Non pré-multiplié)
							uint32_t dstColor = out[pixelIndex];
							uint32_t invAlpha = 255 - alpha;

							// Décomposition des canaux
							uint32_t s_R = (finalColor >> 16) & 0xFF;
							uint32_t s_G = (finalColor >> 8) & 0xFF;
							uint32_t s_B = finalColor & 0xFF;

							uint32_t d_R = (dstColor >> 16) & 0xFF;
							uint32_t d_G = (dstColor >> 8) & 0xFF;
							uint32_t d_B = dstColor & 0xFF;

							uint32_t out_R = ((s_R * alpha) + (d_R * invAlpha)) >> 8;
							uint32_t out_G = ((s_G * alpha) + (d_G * invAlpha)) >> 8;
							uint32_t out_B = ((s_B * alpha) + (d_B * invAlpha)) >> 8;

							out[pixelIndex] = (out_R << 16) | (out_G << 8) | out_B | 0xFF000000;

							// Mise à jour du Z-Buffer pour les pixels translucides
							_zBuffer[pixelIndex] = item.zIndex;
						}
					}


				}
			}
		}
	}


	// =======================================================
	// RENDU DES BACKGROUNDS HD (TAG <background>) AVEC PARALLAXE
	// =======================================================
	uint32_t hdWidth = outWidth; // On crée l'alias pour que le code du dessous fonctionne !

	for(auto& bg : SnesHdPackLoader::ActivePack->Backgrounds) {

		// Vérification de la condition
		if(!bg->MatchesCondition()) continue;
		// ==========================================================
		// MISE À JOUR DE L'ANIMATION DU BACKGROUND
		// ==========================================================
		if(bg->IsAnimated && !bg->AnimFrames.empty() && bg->FramesBetweenSteps > 0) {
			uint32_t currentFrame = SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber;
			int totalFrames = (int)bg->AnimFrames.size();
			int currentAnimIndex = (currentFrame / bg->FramesBetweenSteps) % totalFrames;

			bg->Bitmap = bg->AnimFrames[currentAnimIndex];
		}
		// ==========================================================
		if(bg->Bitmap == nullptr) continue; // Sécurité


		// Lazy Loading sécurisé
		bg->Bitmap->Init();
		if(bg->Bitmap->PixelData.empty() || bg->Bitmap->Width == 0 || bg->Bitmap->Height == 0) continue;

		int bgWidth = bg->Bitmap->Width;
		int bgHeight = bg->Bitmap->Height;
		int bgIndex = bg->MasterScrollLayer - 1;

		// ON DESSINE L'IMAGE SUR TOUTE LA TAILLE DE L'ÉCRAN HD
		for(uint32_t hdY = 0; hdY < hdHeight; hdY++) {

			// 1. On trouve la ligne de base de la SNES (0 à 239)
			uint32_t sdY = hdY / scale;

			// 2. VOYAGE DANS LE TEMPS : On lit l'historique du scroll pour CETTE ligne !
			int masterScrollX = 0;
			int masterScrollY = 0;

			if(bgIndex >= 0 && bgIndex < 4 && sdY < 240) {
				auto& screenInfo = SnesHdPackLoader::ActivePack->ScreenInfo;
				masterScrollX = screenInfo.ScanlineHScroll[sdY][bgIndex];
				masterScrollY = screenInfo.ScanlineVScroll[sdY][bgIndex];
			}

			// 3. PRÉCALCULS DE LIGNE (Exécutés 1 fois par ligne Y)
			// On calcule les offsets finaux pour cette ligne précise
			int lineScrollY = (int)(masterScrollY * bg->ScrollYRatio * scale) + (bg->OffsetY * scale);
			int lineScrollX = (int)(masterScrollX * bg->ScrollXRatio * scale) + (bg->OffsetX * scale);

			int rawY = hdY + lineScrollY;
			int pngY = rawY;

			// --- GESTION DU TILE Y ---
			if(!bg->TileY) {
				if(pngY < 0 || pngY >= bgHeight) continue;
			} else {
				pngY = rawY % bgHeight;
				if(pngY < 0) pngY += bgHeight;
			}

			// On pré-calcule les adresses de base pour éviter les multiplications dans la boucle X
			uint32_t pngBaseRowIndex = pngY * bgWidth;
			uint32_t outBaseRowIndex = hdY * hdWidth;



			// ==============================================================
			// LA FONCTION LAMBDA INLINÉE AVEC Z-BUFFER
			// ==============================================================
			auto blendPixel = [&](int localPngX, uint32_t localHdX) {
				uint32_t pixelColor = bg->Bitmap->PixelData[pngBaseRowIndex + localPngX];
				uint32_t alpha = (pixelColor >> 24);

				if(alpha > 0) {
					uint32_t outIdx = outBaseRowIndex + localHdX;

					// ===================================================
					// LE TEST DE PROFONDEUR (Z-TEST)
					// On dessine le background UNIQUEMENT s'il est devant (ou égal)
					// à l'élément qui se trouve actuellement à cet endroit de l'écran !
					// ===================================================
					if(bg->ZIndex >= _zBuffer[outIdx]) {

						if(alpha == 255) {
							// Opacité totale : On écrase
							out[outIdx] = pixelColor | 0xFF000000;
						} else {
							// Transparence : Alpha Blending 32-bits
							uint32_t dstColor = out[outIdx];
							uint32_t invAlpha = 255 - alpha;

							uint32_t s_RB = pixelColor & 0x00FF00FF;
							uint32_t s_AG = (pixelColor >> 8) & 0x00FF00FF;
							uint32_t d_RB = dstColor & 0x00FF00FF;
							uint32_t d_AG = (dstColor >> 8) & 0x00FF00FF;

							uint32_t out_RB = ((s_RB * alpha + d_RB * invAlpha) >> 8) & 0x00FF00FF;
							uint32_t out_AG = ((s_AG * alpha + d_AG * invAlpha) >> 8) & 0x00FF00FF;

							out[outIdx] = out_RB | (out_AG << 8) | 0xFF000000;
						}

						// OPTIONNEL MAIS RECOMMANDÉ : On met à jour le Z-Buffer !
						// Ainsi, si un 2ème <background> a un ZIndex de 60, il pourra se 
						// dessiner au-dessus de ce background (qui a par ex 50).
						_zBuffer[outIdx] = bg->ZIndex;
					}
				}
			};
			// ==============================================================
			// LES BOUCLES PRINCIPALES (Loop Unswitching & Modulo Removal)
			// ==============================================================

			// 2. OPTIMISATION : On sépare les deux logiques (Loop Unswitching)
			if(!bg->TileX) {

				// SCÉNARIO A : Pas de répétition (Image unique)
				for(uint32_t hdX = 0; hdX < hdWidth; hdX++) {
					int pngX = hdX + lineScrollX;
					if(pngX >= 0 && pngX < bgWidth) {
						blendPixel(pngX, hdX);
					}
				}

			} else {

				// SCÉNARIO B : Répétition infinie (TileX = true)
				// 1. OPTIMISATION : Calcul du point de départ SANS modulo dans la boucle
				int currentPngX = lineScrollX % bgWidth;
				if(currentPngX < 0) currentPngX += bgWidth;

				for(uint32_t hdX = 0; hdX < hdWidth; hdX++) {

					blendPixel(currentPngX, hdX);

					// Incrémentation basique : infiniment plus rapide qu'un modulo !
					currentPngX++;
					if(currentPngX >= bgWidth) {
						currentPngX = 0;
					}
				}
			}

		}
	}//backgrounds

	// ==========================================================
	// GESTION DES VIDÉOS (pl_mpeg) - LE RENDU FINAL
	// ==========================================================
	if(SnesHdPackLoader::ActivePack != nullptr && out != nullptr) {

		for(auto& video : SnesHdPackLoader::ActivePack->Videos) {
			if(!video->MatchesCondition()) continue;

			// 1. INITIALISATION (Lazy Loading)
			if(!video->IsPlaying && !video->HasPlayed) {
				std::string fullPath = FolderUtilities::CombinePath(SnesHdPackLoader::ActivePack->RootFolder, video->Filename);

				video->VideoPlayer = plm_create_with_filename(fullPath.c_str());
				if(video->VideoPlayer != nullptr) {
					video->VideoWidth = plm_get_width(video->VideoPlayer);
					video->VideoHeight = plm_get_height(video->VideoPlayer);

					if(video->VideoWidth <= 0 || video->VideoHeight <= 0) {
						plm_destroy(video->VideoPlayer);
						video->VideoPlayer = nullptr;
						video->HasPlayed = true;
						continue;
					}

					video->IsPlaying = true;
					video->TimeAccumulator = 0.0;
					plm_set_audio_enabled(video->VideoPlayer, !video->MuteAudio);

					// --- LE PONT AUDIO ---
					// On récupère le mixer directement depuis l'émulateur
					SoundMixer* mixer = _emu->GetSoundMixer();

					plm_set_audio_decode_callback(video->VideoPlayer, [](plm_t* mpeg, plm_samples_t* samples, void* user) {
						SoundMixer* audioMixer = static_cast<SoundMixer*>(user);
						if(audioMixer == nullptr) return;

						// La vidéo est stéréo (Gauche/Droite), donc 2 valeurs par "échantillon"
						int totalValues = samples->count * 2;
						std::vector<int16_t> intSamples(totalValues);

						// On convertit les 'float' de pl_mpeg en 'int16_t' pour Mesen
						for(int i = 0; i < totalValues; i++) {
							float f = samples->interleaved[i];

							// Sécurité : on limite la valeur pour éviter les distorsions/grésillements
							if(f > 1.0f) f = 1.0f;
							if(f < -1.0f) f = -1.0f;

							intSamples[i] = static_cast<int16_t>(f * 32767.0f);
						}

						// On récupère la fréquence de la vidéo (souvent 44100Hz ou 48000Hz)
						uint32_t sampleRate = plm_get_samplerate(mpeg);

						// On envoie le son converti à la carte son !
						audioMixer->PlayAudioBuffer(intSamples.data(), samples->count, sampleRate);

					}, mixer);
					// ----------------------


					video->CurrentFramePixels.resize(video->VideoWidth * video->VideoHeight * 4, 0);
				} else {
					video->HasPlayed = true;
					continue;
				}
			}

			// 2. LECTURE & SYNCHRONISATION
			if(video->IsPlaying && video->VideoPlayer != nullptr) {

				double fps = plm_get_framerate(video->VideoPlayer);
				if(fps <= 0.0) fps = 30.0;

				double frameTime = 1.0 / fps;
				video->TimeAccumulator += (1.0 / 60.0);

				bool newFrameDecoded = false;
				plm_frame_t* frame = nullptr;

				while(video->TimeAccumulator >= frameTime) {
					frame = plm_decode_video(video->VideoPlayer);
					video->TimeAccumulator -= frameTime;
					newFrameDecoded = true;
				}

				if(newFrameDecoded && frame != nullptr) {
					plm_frame_to_bgra(frame, video->CurrentFramePixels.data(), video->VideoWidth * 4);
				}

				if(plm_has_ended(video->VideoPlayer) && !video->Loop) {
					video->IsPlaying = false;
					video->HasPlayed = true;
					plm_destroy(video->VideoPlayer);
					video->VideoPlayer = nullptr;
					video->CurrentFramePixels.clear();
					continue;
				}

				// 3. DESSIN DES PIXELS SUR L'ÉCRAN 'out' !
				if(!video->CurrentFramePixels.empty()) {
					uint32_t* videoPixels = reinterpret_cast<uint32_t*>(video->CurrentFramePixels.data());

					// Les variables hdWidth, hdHeight et scale existent déjà dans ton code au-dessus !
					if(hdWidth > 0 && hdHeight > 0) {
						int startX = video->X;
						int startY = video->Y;
						int maxW = std::min(video->VideoWidth, (int)hdWidth - startX);
						int maxH = std::min(video->VideoHeight, (int)hdHeight - startY);

						if(maxW > 0 && maxH > 0) {
							for(int y = 0; y < maxH; y++) {
								for(int x = 0; x < maxW; x++) {
									int screenX = startX + x;
									int screenY = startY + y;



									if(screenX >= 0 && screenX < (int)hdWidth && screenY >= 0 && screenY < (int)hdHeight) {
										uint32_t pixel = videoPixels[y * video->VideoWidth + x];

										// On zappe le noir parfait de la vidéo
										if((pixel & 0x00FFFFFF) != 0) {

											uint32_t outIdx = screenY * outWidth + screenX;

											// ===================================================
											// LE TEST DE PROFONDEUR (Z-TEST) POUR LA VIDÉO
											// ===================================================
											if(video->ZIndex >= _zBuffer[outIdx]) {

												// La vidéo a le droit de dessiner !
												out[outIdx] = pixel | 0xFF000000;

												// On met à jour le Z-Buffer au cas où un élément
												// ayant un ZIndex encore plus élevé (ex: 99) voudrait
												// se dessiner par-dessus la vidéo plus tard !
												_zBuffer[outIdx] = video->ZIndex;
											}
										}
									}


								}
							}
						}
					}
				}
			}
		}
	}

	// ==========================================================
	// VERROUILLAGE FINAL DE L'ALPHA (Sécurité pour l'affichage)
	// ==========================================================
	// On s'assure que tous les pixels de l'écran final sont 100% opaques 
	// pour que la fenêtre de l'émulateur ne soit pas transparente.
	// ==========================================================
	if(out != nullptr && hdWidth > 0 && hdHeight > 0) {
		for(uint32_t y = 0; y < hdHeight; y++) {
			for(uint32_t x = 0; x < hdWidth; x++) {
				out[y * outWidth + x] |= 0xFF000000;
			}
		}
	}

}// end fonction ApplyFilter


//------------------------------------------------------------------------------------------------------------/


void SnesHdVideoFilter::ApplyFilterSpritesOAMScan(uint32_t scale, uint32_t outWidth, uint32_t outHeight, uint32_t* out)
{
	// 1. Sécurité
	if(!SnesHdPackLoader::ActivePack || !_emu) return;

	SnesConsole* console = (SnesConsole*)_emu->GetConsole().get();
	SnesPpu* ppu = console->GetPpu();
	if(!ppu) return;

	// 2. Récupération de la RAM des sprites brutes (OAM)
	uint8_t* oamRam = ppu->GetSpriteRam(); // 544 octets

	// --- LES PARAMÈTRES GLOBAUX DU PPU ---
	// La SNES permet de choisir deux tailles de sprites parmi une liste.
	// Ces valeurs sont dans le registre $2101 (OBSEL), on les simulera par des valeurs standards 
	// (8x8 et 16x16) pour le prototype, à affiner selon ton PPU.
	int smallSpriteSize = 8;
	int largeSpriteSize = 16;
	// -------------------------------------

	// 3. Boucle sur les 128 Sprites matériels
	for(int i = 0; i < 128; i++) {

		// --- LECTURE DU FORMAT OAM (544 Octets) ---
		// Les 512 premiers octets = 4 octets par sprite (X, Y, Char, Attr)
		uint32_t baseAddr = i * 4;

		// Les 32 derniers octets = La "Table Haute" (High Table) qui contient
		// le 9ème bit de X (pour les X négatifs ou > 255) et la taille du sprite.
		uint32_t highTableAddr = 512 + (i / 4);
		uint8_t highBits = (oamRam[highTableAddr] >> ((i % 4) * 2)) & 0x03;

		bool isLarge = (highBits & 0x02) != 0;
		int xBit9 = (highBits & 0x01) << 8;

		// --- EXTRACTION DES VALEURS ---
		int x = oamRam[baseAddr + 0] | xBit9;
		if(x > 255) x -= 512; // Gestion des nombres signés (hors-écran à gauche)

		int y = oamRam[baseAddr + 1];
		if(y == 240) continue; // Y=240 cache le sprite sur SNES
		if(y > 255) y -= 256;

		uint8_t tileIndex = oamRam[baseAddr + 2];
		uint8_t attributes = oamRam[baseAddr + 3];

		bool vFlip = (attributes & 0x80) != 0;
		bool hFlip = (attributes & 0x40) != 0;
		uint8_t priority = (attributes & 0x30) >> 4;
		uint8_t palette = (attributes & 0x0E) >> 1;
		bool nameTable = (attributes & 0x01) != 0; // Utilise la 2ème page de CHR RAM

		int size = isLarge ? largeSpriteSize : smallSpriteSize;

		// ====================================================================
		// LE TEST FATIDIQUE : CE SPRITE A-T-IL UN PNG HD ?
		// ====================================================================
		// On construit la clé de recherche. Le calcul du Hash dépend fortement 
		// de la façon dont 'MemoryHash' est généré dans ton loader pour les sprites.
		// En général, le Hash est (TileIndex | (NameTable << 8)).

		SnesHdTileKey key;
		key.MemoryHash = tileIndex | (nameTable ? 0x100 : 0x00);
		key.PaletteHash = 0; // Souvent 0 pour les sprites selon les packs
		key.Bpp = SnesBpp::Bpp4; // Les sprites sont toujours en 4BPP sur SNES
		key.IsLargeTile = isLarge;
		key.IsSprite = true;

		auto it = SnesHdPackLoader::ActivePack->TileByKey.find(key);
		if(it != SnesHdPackLoader::ActivePack->TileByKey.end() && !it->second.empty()) {

			// ON A TROUVÉ UN SPRITE HD !
			SnesHdPackTileInfo* hdTile = nullptr;

			// On cherche le bon PNG parmi les conditions
			for(auto* tile : it->second) {
				if(tile->MatchesCondition(x, y, nullptr)) {
					hdTile = tile;
					break;
				}
			}

			if(hdTile == nullptr || hdTile->HdTileData.empty()) continue;

			// ====================================================================
			// DESSIN DU SPRITE HD (SANS CLIPPING !)
			// ====================================================================
			// On calcule le Z-Index du sprite à partir de sa priorité matérielle
			uint8_t pixelZ = 20 + (priority * 4);

			uint32_t pngWidth = hdTile->Width * scale;
			uint32_t pngHeight = hdTile->Height * scale;

			// On dessine toute la surface du PNG !
			for(uint32_t py = 0; py < pngHeight; py++) {
				for(uint32_t px = 0; px < pngWidth; px++) {

					// Position finale sur l'écran HD
					int32_t screenX = (x * scale) + px;
					int32_t screenY = (y * scale) + py;

					// Culling : On ne dessine pas en dehors de l'écran final
					if(screenX < 0 || screenX >= (int32_t)outWidth || screenY < 0 || screenY >= (int32_t)outHeight) {
						continue;
					}

					// Lecture du pixel PNG avec gestion du Flip !
					uint32_t readX = hFlip ? (pngWidth - 1 - px) : px;
					uint32_t readY = vFlip ? (pngHeight - 1 - py) : py;

					uint32_t hdColorFromPng = hdTile->HdTileData[readY * pngWidth + readX];
					uint32_t alpha = (hdColorFromPng >> 24) & 0xFF;

					if(alpha > 0) {
						uint32_t pixelIndex = screenY * outWidth + screenX;

						// === LE Z-TEST ===
						// On ne dessine le sprite OAM que s'il est DEVANT ce qui est déjà dessiné.
						if(pixelZ >= _zBuffer[pixelIndex]) {

							// Opacité maximale (ou mixage si tu veux l'Alpha Blending complet ici)
							out[pixelIndex] = hdColorFromPng | 0xFF000000;

							// On met à jour le Z-Buffer
							_zBuffer[pixelIndex] = pixelZ;
						}
					}
				}
			}
		}
	}
}


//=====================================================================================================
//=====================================================================================================
void SnesHdVideoFilter::ApplyFilterSpritesStandard(uint32_t x, uint32_t y, uint32_t scale, uint32_t outWidth,
																								uint32_t sdColor, const SnesHdPixelData& hdData,
																								uint8_t pixelZ, uint32_t* out)
{
	// On détermine si ce pixel appartient à un élément remplacé par un PNG HD (Sprite ou Décor)
	bool hasHdReplacement = (hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty());
	bool eraseThisTile = hasHdReplacement;

	// 1. Règles globales d'effacement (désactivation via le pack)
	if(!hdData.IsSprite) {
		if(SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
			eraseThisTile = true;
		} else {
			if(hdData.SourceLayer == 0 && SnesHdPackLoader::ActivePack->DisableBg1) eraseThisTile = true;
			else if(hdData.SourceLayer == 1 && SnesHdPackLoader::ActivePack->DisableBg2) eraseThisTile = true;
			else if(hdData.SourceLayer == 2 && SnesHdPackLoader::ActivePack->DisableBg3) eraseThisTile = true;
			else if(hdData.SourceLayer == 3 && SnesHdPackLoader::ActivePack->DisableBg4) eraseThisTile = true;
		}
	} else {
		if(SnesHdPackLoader::ActivePack->DisableOriginalSprites) {
			eraseThisTile = true;
		}
	}

	uint32_t targetColor = sdColor | 0xFF000000;
	uint8_t targetZ = pixelZ;

	// 2. Si la tuile a un PNG HD, on ne la dessine SURTOUT PAS ici (elle sera dessinée par le Dépliage Universel)
	// À la place, on prépare la toile en révélant le décor qui était caché sous le sprite SNES natif !
	if(eraseThisTile) {
		if(!hdData.IsSprite || SnesHdPackLoader::ActivePack->DisableOriginalSprites) {
			targetColor = 0x00000000; // Fond transparent
			targetZ = 0;
		} else {
			// On restaure le pixel de décor original (UnderlyingColor) pour que l'étincelle HD puisse se mélanger avec !
			targetColor = _calculatedPalette[hdData.UnderlyingColor] | 0xFF000000;
			// TRÈS IMPORTANT : Le décor révélé n'a pas la priorité du sprite original ! 
			// On met son Z-Index à 0 pour laisser l'étincelle s'afficher par-dessus.
			targetZ = 0;
		}
	}

	// 3. Écriture sur l'écran HD
	for(uint32_t sy = 0; sy < scale; sy++) {
		for(uint32_t sx = 0; sx < scale; sx++) {
			uint32_t pixelIndex = (y * scale + sy) * outWidth + (x * scale + sx);

			// Z-Test : On n'écrase pas un décor HD déjà dessiné par le Pre-Pass Mode 7 géant
			if(targetZ >= _zBuffer[pixelIndex]) {

				// Si la couleur cible n'est pas transparente
				if((targetColor >> 24) > 0) {
					out[pixelIndex] = targetColor;
					_zBuffer[pixelIndex] = targetZ;
				}
			}
		}
	}
}
/*
//=====================================================================================================
void SnesHdVideoFilter::ApplyFilterSpritesStandard(uint32_t x, uint32_t y, uint32_t scale, uint32_t outWidth,
																	uint32_t sdColor, const SnesHdPixelData& hdData,
																	uint8_t pixelZ, uint32_t* out)
{
	// ===============================================
	// LA VOIE CLASSIQUE : SPRITES, TUILES TRADITIONNELLES
	// ===============================================
	for(uint32_t sy = 0; sy < scale; sy++) {
		for(uint32_t sx = 0; sx < scale; sx++) {

			uint32_t finalColor = sdColor;
			bool eraseThisTile = false;

			// 1. Règles globales d'effacement
			if(!hdData.IsSprite) {
				if(SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
					eraseThisTile = true;
				} else {
					if(hdData.SourceLayer == 0 && SnesHdPackLoader::ActivePack->DisableBg1) eraseThisTile = true;
					else if(hdData.SourceLayer == 1 && SnesHdPackLoader::ActivePack->DisableBg2) eraseThisTile = true;
					else if(hdData.SourceLayer == 2 && SnesHdPackLoader::ActivePack->DisableBg3) eraseThisTile = true;
					else if(hdData.SourceLayer == 3 && SnesHdPackLoader::ActivePack->DisableBg4) eraseThisTile = true;
				}
			}

			// 2. Correction   Background
			if(hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty()) {
				eraseThisTile = true;
			}
			if(hdData.Tile != nullptr && hdData.Tile->IsDisabled) {
				eraseThisTile = true;
			}
			if(eraseThisTile) {
				finalColor = 0x00000000;
			}

			// 3. Rendu HD
			if(hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty()) {

				uint32_t baseTileX = hdData.IsSprite ? hdData.TileX : (7 - hdData.TileX);
				uint32_t baseTileY = hdData.TileY;

				uint32_t trueTileX = hdData.Tile->IsHFlip ? (7 - baseTileX) : baseTileX;
				uint32_t trueTileY = hdData.Tile->IsVFlip ? (7 - baseTileY) : baseTileY;

				bool doHFlip = hdData.HFlip && !hdData.Tile->IsHFlip;
				bool doVFlip = hdData.VFlip && !hdData.Tile->IsVFlip;

				uint32_t readSx = doHFlip ? (scale - 1 - sx) : sx;
				uint32_t readSy = doVFlip ? (scale - 1 - sy) : sy;

				uint32_t hdPixelX = (trueTileX * scale) + readSx;
				uint32_t hdPixelY = (trueTileY * scale) + readSy;
				uint32_t tileWidthHD = hdData.Tile->Width * scale;

				if((hdPixelY * tileWidthHD + hdPixelX) < hdData.Tile->HdTileData.size()) {
					uint32_t hdColorFromPng = hdData.Tile->HdTileData[hdPixelY * tileWidthHD + hdPixelX];

					if((hdColorFromPng >> 24) > 0) {
						if(SnesHdPackLoader::ActivePack->IsColorMathDisabled || hdData.Tile->IgnoreColorMath) {
							finalColor = hdColorFromPng;
						} else {
							uint32_t baseSdColor = _calculatedPalette[hdData.BaseColor];
							int diffR = ((sdColor >> 16) & 0xFF) - ((baseSdColor >> 16) & 0xFF);
							int diffG = ((sdColor >> 8) & 0xFF) - ((baseSdColor >> 8) & 0xFF);
							int diffB = (sdColor & 0xFF) - (baseSdColor & 0xFF);

							int outR = ((hdColorFromPng >> 16) & 0xFF) + diffR;
							int outG = ((hdColorFromPng >> 8) & 0xFF) + diffG;
							int outB = (hdColorFromPng & 0xFF) + diffB;

							outR = outR < 0 ? 0 : (outR > 255 ? 255 : outR);
							outG = outG < 0 ? 0 : (outG > 255 ? 255 : outG);
							outB = outB < 0 ? 0 : (outB > 255 ? 255 : outB);

							finalColor = (hdColorFromPng & 0xFF000000) | (outR << 16) | (outG << 8) | outB;
						}
					} else {
						if(!hdData.IsSprite) {
							finalColor = 0x00000000;
						} else {
							if(SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
								finalColor = 0x00000000;
							} else {
								finalColor = _calculatedPalette[hdData.UnderlyingColor];
							}
						}
					}
				}
			}

			// 4. Écriture Pixel et Z-Buffer
			uint32_t pixelIndex = (y * scale + sy) * outWidth + (x * scale + sx);
			out[pixelIndex] = finalColor;
			_zBuffer[pixelIndex] = ((finalColor >> 24) == 0) ? 0 : pixelZ;
		}
	}
}

*/
//--------------------------------------------------------------------------------------------


bool SnesHdPackTileAtPositionCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!vram) return false;

	std::string logPath = "C:\\Users\\Gwen\\Documents\\Mesen2\\HdPacks\\TileDebug.Log";
	bool shouldLogThisFrame = (_screenInfo->FrameNumber == 60);
	std::ofstream* logfile = nullptr;

	if(shouldLogThisFrame) {
		logfile = new std::ofstream(logPath, std::ios_base::app);
		*logfile << "=== EVALUATION | Condition [" << Name << "] ===" << std::endl;
		*logfile << "Scope : " << (Scope == SnesHdPackConditionScope::Layer ? "LAYER" : "SCREEN") << std::endl;
		*logfile << "Cible Attendue : Tile=$" << HexUtilities::ToHex(this->MemoryHash) << " | Palette=" << this->PaletteHash << std::endl;
	}

	for(int layer = 0; layer < 4; layer++) {
		if((TargetLayers & (1 << layer)) == 0) continue;

		LayerConfig& config = state.Layers[layer];

		// Définition des limites de la boucle selon les Wildcards
		int startX = CheckAnyX ? 0 : this->TileX;
		int endX = CheckAnyX ? (Scope == SnesHdPackConditionScope::Layer ? (config.DoubleWidth ? 63 : 31) : 31) : this->TileX;

		int startY = CheckAnyY ? 0 : this->TileY;
		int endY = CheckAnyY ? (Scope == SnesHdPackConditionScope::Layer ? (config.DoubleHeight ? 63 : 31) : 31) : this->TileY;

		for(int ty = startY; ty <= endY; ty++) {
			for(int tx = startX; tx <= endX; tx++) {

				int tileCol, tileRow;

				// --- 1. CALCUL DES COORDONNÉES SELON LE SCOPE ---
				if(Scope == SnesHdPackConditionScope::Screen) {
					// Mode Écran : On convertit la tuile en pixels (tx * 8), on applique la caméra, et on repasse en tuile (/ 8)
					int mapX = ((tx * 8) + config.HScroll) & 0x3FF;
					int mapY = ((ty * 8) + config.VScroll) & 0x3FF;
					tileCol = mapX / 8;
					tileRow = mapY / 8;
				} else {
					// Mode Layer : On lit directement sur la carte brute (sans la caméra)
					tileCol = tx & (config.DoubleWidth ? 63 : 31);
					tileRow = ty & (config.DoubleHeight ? 63 : 31);
				}

				// --- 2. LOGIQUE VRAM SNES (Gestion des blocs 32x32) ---
				int finalCol = tileCol;
				int finalRow = tileRow;
				int mapOffset = 0;
				if(config.DoubleWidth && finalCol >= 32) { mapOffset += 32 * 32; finalCol -= 32; }
				if(config.DoubleHeight && finalRow >= 32) { mapOffset += (config.DoubleWidth ? 2 : 1) * 32 * 32; finalRow -= 32; }

				int vramIndex = config.TilemapAddress + mapOffset + (finalRow * 32) + finalCol;

				if(vramIndex >= 0x8000) continue; // Sécurité mémoire

				// --- 3. COMPARAISON ---
				uint16_t tileData = vram[vramIndex];
				uint16_t snesTileIndex = tileData & 0x03FF;
				uint32_t snesPalette = (tileData >> 10) & 0x07;

				bool isTileMatch = false;

				// --- LE TRI INTELLIGENT (Index ou Hash ?) ---
				if(this->MemoryHash <= 0xFFFF) {
					// CAS 1 : L'utilisateur a entré un Index VRAM (ex: 101)
					isTileMatch = (this->MemoryHash == snesTileIndex);
				} else {
					// CAS 2 : L'utilisateur a entré un Hash 64-bits (ex: 2FEE8E4AF5240326)

					// 1. On déduit le BPP exact en fonction du Mode BG de la SNES et du Calque
					uint32_t bpp = 2; // Valeur par défaut
					switch(state.BgMode) {
						case 0: bpp = 2; break;
						case 1: bpp = (layer < 2) ? 4 : 2; break;
						case 2: bpp = 4; break;
						case 3: bpp = (layer == 0) ? 8 : 4; break;
						case 4: bpp = (layer == 0) ? 8 : 2; break;
						case 5: bpp = (layer == 0) ? 4 : 2; break;
						case 6: bpp = 4; break;
						case 7: bpp = 8; break;
					}

					// 2. On calcule l'adresse en VRAM (ChrAddress dans Mesen)
					uint32_t tileStart = config.ChrAddress + (snesTileIndex * 4 * bpp);
					size_t wordCount = 4 * bpp;
					uint64_t actualTileHash = 0;

					// 3. On génère la signature 64-bits à la volée !
					for(size_t i = 0; i < wordCount; i++) {
						uint16_t word = vram[(tileStart + i) & 0x7FFF];
						actualTileHash = (actualTileHash * 31) + (word & 0xFF);
						actualTileHash = (actualTileHash * 31) + (word >> 8);
					}


					// MOUCHARD : Si l'émulateur croise la tuile 101, il écrit son vrai hash
					if(logfile && snesTileIndex == 101) {
						*logfile << "=> INFO : Hash reel calcule pour la tuile 101 = $" << HexUtilities::ToHex(actualTileHash) << std::endl;
					}
					isTileMatch = (this->MemoryHash == actualTileHash);
				}

				// --- VÉRIFICATION FINALE ---
				if(isTileMatch) {
					if(this->IgnorePalette || this->PaletteHash == snesPalette) {
						// VICTOIRE !
						if(logfile) {
							*logfile << "======> MATCH PARFAIT SUR LAYER " << (layer + 1)
								<< " | Trouvé à X:" << tx << " Y:" << ty << " <======" << std::endl;
							logfile->close(); delete logfile;
						}
						return true;
					}
				}

				/*
				// --- 3. COMPARAISON ---
				uint16_t tileData = vram[vramIndex];
				uint16_t snesTileIndex = tileData & 0x03FF;
				uint32_t snesPalette = (tileData >> 10) & 0x07;

				if(this->MemoryHash == snesTileIndex) {
					if(this->IgnorePalette || this->PaletteHash == snesPalette) {
						// VICTOIRE !
						if(logfile) {
							*logfile << "======> MATCH PARFAIT SUR LAYER " << (layer + 1)
								<< " | Trouvé à X:" << tx << " Y:" << ty << " <======" << std::endl;
							logfile->close(); delete logfile;
						}
						return true;
					}
				}//3
				*/


			}
		}
	}

	if(logfile) {
		*logfile << "=> ECHEC : Aucune tuile correspondante trouvée." << std::endl;
		logfile->close(); delete logfile;
	}

	return false;
}



//--------------------------------------------------------------------------------------------------
bool SnesHdPackSpriteAtPositionCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	uint8_t* oamRam = _screenInfo->Ppu->GetSpriteRam();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!oamRam || !vram) return false;

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();

	for(int i = 0; i < 128; i++) {
		int offset = i * 4;

		// 1. Lecture des coordonnées
		uint8_t spriteX = oamRam[offset + 0];
		uint8_t spriteY = oamRam[offset + 1];

		// On vérifie d'abord si on est à la bonne position
		if(spriteX == this->TileX && spriteY == this->TileY) {

			// 2. Extraction de l'Index et de la Palette depuis l'OAM de la SNES
			// Byte 2 = les 8 premiers bits de l'index
			// Byte 3 = le 9ème bit de l'index (bit 0) et la palette (bits 1 à 3)
			uint16_t snesTileIndex = oamRam[offset + 2] | ((oamRam[offset + 3] & 0x01) << 8);
			uint32_t snesPalette = (oamRam[offset + 3] >> 1) & 0x07;

			bool isTileMatch = false;

			// --- LE TRI INTELLIGENT (Index ou Hash ?) ---
			if(this->MemoryHash <= 0xFFFF) {
				// CAS 1 : Index VRAM (ex: 101)
				isTileMatch = (this->MemoryHash == snesTileIndex);
			} else {
				// CAS 2 : Hash 64-bits
								// Sur SNES, les sprites (OBJ) sont TOUJOURS en 4 BPP.
				uint32_t bpp = 4;

				// --- CALCUL DE L'ADRESSE VRAM DU SPRITE ---
				uint32_t tileAddress = state.OamBaseAddress;
				if(snesTileIndex >= 256) {
					// Si le sprite utilise la 2ème page de tuiles, on ajoute l'offset
					tileAddress += state.OamAddressOffset;
				}

				// On calcule le point de départ en VRAM (modulo 256 car on a géré la page au-dessus)
				uint32_t tileStart = tileAddress + ((snesTileIndex % 256) * 4 * bpp);
				size_t wordCount = 4 * bpp;
				uint64_t actualTileHash = 0;

				for(size_t j = 0; j < wordCount; j++) {
					uint16_t word = vram[(tileStart + j) & 0x7FFF];
					actualTileHash = (actualTileHash * 31) + (word & 0xFF);
					actualTileHash = (actualTileHash * 31) + (word >> 8);
				}

				isTileMatch = (this->MemoryHash == actualTileHash);
			}

			// --- VÉRIFICATION FINALE ---
			if(isTileMatch) {
				if(this->IgnorePalette || this->PaletteHash == snesPalette) {
					return true; // Match parfait du sprite !
				}
			}
		}
	}
	return false;
}
//--------------------------------------------------------------------------------------------------
bool SnesHdPackTileNearbyCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!vram) return false;

	// 1. Prise en compte du miroir de la tuile évaluée
	int xSign = (tile && tile->HFlip) ? -1 : 1;
	int ySign = (tile && tile->VFlip) ? -1 : 1;

	int startX = CheckAnyX ? 0 : (x + (OffsetX * xSign));
	int endX = CheckAnyX ? 255 : startX;
	int stepX = CheckAnyX ? 8 : 1;

	int startY = CheckAnyY ? 0 : (y + (OffsetY * ySign));
	int endY = CheckAnyY ? 239 : startY;
	int stepY = CheckAnyY ? 8 : 1;

	for(int layer = 0; layer < 4; layer++) {
		LayerConfig& config = state.Layers[layer];

		// Détermination de la profondeur de couleur (BPP)
		uint32_t bpp = 2;
		switch(state.BgMode) {
			case 0: bpp = 2; break;
			case 1: bpp = (layer < 2) ? 4 : 2; break;
			case 2: bpp = 4; break;
			case 3: bpp = (layer == 0) ? 8 : 4; break;
			case 4: bpp = (layer == 0) ? 8 : 2; break;
			case 5: bpp = (layer == 0) ? 4 : 2; break;
			case 6: bpp = 4; break;
			case 7: bpp = 8; break;
		}

		for(int py = startY; py <= endY; py += stepY) {
			for(int px = startX; px <= endX; px += stepX) {

				uint16_t snesTileIndex = 0;
				uint32_t snesPalette = 0;

				// --- CAS 1 : MODE 7 ---
				if(state.BgMode == 7) {
					if(layer > 0) continue; // Le Mode 7 n'a qu'un seul calque (BG1)

					int mapX = (px + state.Mode7.HScroll) & 0x3FF;
					int mapY = (py + state.Mode7.VScroll) & 0x3FF;

					uint16_t tilemapWord = vram[((mapY / 8) * 128) + (mapX / 8)];
					snesTileIndex = tilemapWord & 0xFF; // Octet de poids faible = Index
					snesPalette = 0;
				}
				// --- CAS 2 : MODES CLASSIQUES (BG0 à BG6) ---
				else {
					int mapX = (px + config.HScroll) & 0x3FF;
					int mapY = (py + config.VScroll) & 0x3FF;

					int tileSizeShift = config.LargeTiles ? 4 : 3; // 16px ou 8px
					int tileCol = mapX >> tileSizeShift;
					int tileRow = mapY >> tileSizeShift;

					int finalCol = tileCol;
					int finalRow = tileRow;
					int mapOffset = 0;

					if(config.DoubleWidth && finalCol >= 32) { mapOffset += 32 * 32; finalCol -= 32; }
					if(config.DoubleHeight && finalRow >= 32) { mapOffset += (config.DoubleWidth ? 2 : 1) * 32 * 32; finalRow -= 32; }

					int vramIndex = config.TilemapAddress + mapOffset + (finalRow * 32) + finalCol;
					if(vramIndex >= 0x8000) continue;

					uint16_t tileData = vram[vramIndex];
					uint16_t baseTileIndex = tileData & 0x03FF;
					snesPalette = (tileData >> 10) & 0x07;

					// Résolution de la sous-tuile 8x8 en mode 16x16
					if(config.LargeTiles) {
						bool hFlip = (tileData & 0x4000) != 0;
						bool vFlip = (tileData & 0x8000) != 0;

						int subX = (mapX / 8) & 1;
						int subY = (mapY / 8) & 1;

						if(hFlip) subX = 1 - subX;
						if(vFlip) subY = 1 - subY;

						snesTileIndex = baseTileIndex + subX + (subY * 16);
					} else {
						snesTileIndex = baseTileIndex;
					}
				}

				// --- VERIFICATION ET HASHING ---
				bool isTileMatch = false;

				if(this->MemoryHash <= 0xFFFF) {
					isTileMatch = (this->MemoryHash == snesTileIndex);
				} else {
					uint32_t tileStart = (state.BgMode == 7)
						? (snesTileIndex * 32) // Mode 7 : 64 octets par tuile = 32 mots VRAM
						: (config.ChrAddress + (snesTileIndex * 4 * bpp));

					size_t wordCount = 4 * bpp;
					uint64_t actualTileHash = 0;

					for(size_t j = 0; j < wordCount; j++) {
						uint16_t word = vram[(tileStart + j) & 0x7FFF];
						actualTileHash = (actualTileHash * 31) + (word & 0xFF);
						actualTileHash = (actualTileHash * 31) + (word >> 8);
					}

					isTileMatch = (this->MemoryHash == actualTileHash);
				}

				if(isTileMatch) {
					if(this->IgnorePalette || this->PaletteHash == snesPalette) {
						return true;
					}
				}
			}
		}
	}

	return false;
}
//--------------------------------------------------------------------------------------------------

bool SnesHdPackSpriteNearbyCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	uint8_t* oamRam = _screenInfo->Ppu->GetSpriteRam();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!oamRam || !vram) return false;

	static constexpr uint8_t oamWidth[16]  = { 8, 8, 8, 16, 16, 32, 16, 16, 16, 32, 64, 32, 64, 64, 32, 32 };
	static constexpr uint8_t oamHeight[16] = { 8, 8, 8, 16, 16, 32, 32, 32, 16, 32, 64, 32, 64, 64, 64, 32 };
	static constexpr uint16_t sign[2] = { 0x0000, 0xFF00 };

	int targetX = CheckAnyX ? -1 : (x + OffsetX);
	int targetY = CheckAnyY ? -1 : (y + OffsetY);

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();

	// --- LOG DE DÉBOGAGE SUR FICHIER HARDCODÉ ---
	/*
	bool isFrame700 = (_screenInfo->FrameNumber == 700);
	std::ofstream debugLog;
	if(isFrame700) {
		debugLog.open("D:\\Documents\\Mesen2\\HdPacks\\Super Mario Kart (USA)\\sprite_nearby_debug.log", std::ios::app);
		debugLog << "=== Frame 700 | Condition [" << Name << "] ===" << std::endl;
		debugLog << "  Roue en (" << x << ", " << y << ") -> Cible en (" << targetX << ", " << targetY << ")" << std::endl;
		debugLog << "  Hash cherché : $" << HexUtilities::ToHex(MemoryHash) << std::endl;
	}
	*/
	bool foundHit = false;

	for(uint8_t i = 0; i < 128; i++) {
		int offset = i * 4;

		uint8_t highTableValue = oamRam[0x200 | (i >> 2)] >> ((i << 1) & 0x06);
		int16_t spriteX = (int16_t)(sign[highTableValue & 0x01] | oamRam[offset + 0]);
		uint8_t spriteY = oamRam[offset + 1];

		if(spriteY >= 240 && spriteY != 255) continue;

		uint8_t mode = state.OamMode | ((highTableValue & 0x02) << 2);
		int sprWidth = oamWidth[mode];
		int sprHeight = oamHeight[mode];

		// 1. Hitbox du sprite global
		bool matchX = CheckAnyX || (targetX >= spriteX && targetX < (spriteX + sprWidth));
		bool matchY = CheckAnyY || (targetY >= spriteY && targetY < (spriteY + sprHeight));

		if(matchX && matchY) {
			foundHit = true;

			uint16_t baseTileIndex = oamRam[offset + 2] | ((oamRam[offset + 3] & 0x01) << 8);
			uint8_t flags = oamRam[offset + 3];
			uint32_t snesPalette = (flags >> 1) & 0x07;
			bool hFlip = (flags & 0x40) != 0;
			bool vFlip = (flags & 0x80) != 0;

			// 2. Calcul du décalage de sous-tuile dans le sprite
			int relX = CheckAnyX ? 0 : (targetX - spriteX);
			int relY = CheckAnyY ? 0 : (targetY - spriteY);

			int subX = relX / 8;
			int subY = relY / 8;

			if(hFlip) subX = (sprWidth / 8 - 1) - subX;
			if(vFlip) subY = (sprHeight / 8 - 1) - subY;

			// 3. Calcul de l'index réel dans la grille VRAM 16x16 de la SNES
			uint16_t col = ((baseTileIndex & 0x0F) + subX) & 0x0F;
			uint16_t row = (((baseTileIndex & 0x1F0) >> 4) + subY) & 0x1F;
			uint16_t subTileIndex = (row << 4) | col;

			bool isTileMatch = false;
			uint64_t actualTileHash = 0;

			if(this->MemoryHash <= 0xFFFF) {
				isTileMatch = (this->MemoryHash == subTileIndex);
			} else {
				// 4. Calcul du Hash 64-bits de la sous-tuile 8x8 (32 octets en 4 BPP)
				uint32_t bpp = 4;
				uint32_t tileAddress = state.OamBaseAddress;
				if(subTileIndex >= 256) {
					tileAddress += state.OamAddressOffset;
				}

				uint32_t tileStart = tileAddress + ((subTileIndex % 256) * 4 * bpp);

				uint8_t tileBuffer[32];
				for(size_t j = 0; j < 16; j++) {
					uint16_t word = vram[(tileStart + j) & 0x7FFF];
					tileBuffer[j * 2] = word & 0xFF;
					tileBuffer[j * 2 + 1] = word >> 8;
				}

				for(size_t j = 0; j < 32; j++) {
					actualTileHash = (actualTileHash * 31) + tileBuffer[j];
				}

				isTileMatch = (this->MemoryHash == actualTileHash);
			}

			/*if(isFrame700) {
				debugLog << "  [SPRITE TOUCHÉ] #" << (int)i << " à (" << spriteX << ", " << spriteY << ")"
					<< " | SubTile: $" << HexUtilities::ToHex(subTileIndex)
					<< " | Hash calculé: $" << HexUtilities::ToHex(actualTileHash)
					<< " | Palette: " << snesPalette
					<< " | Match: " << (isTileMatch ? "OUI" : "NON") << std::endl;
			}
			*/
			if(isTileMatch && (this->IgnorePalette || this->PaletteHash == snesPalette)) {
				/*	if(isFrame700) { 					debugLog << "  =====> MATCH PARFAIT !" << std::endl; 					debugLog.close(); 				}		*/
				return true;
			}
			
		}
	}

	/* if(isFrame700) {		if(!foundHit) {	debugLog << "  => ÉCHEC : Aucun sprite sous le point cible." << std::endl;		}	debugLog.close();	}	*/
	return false;
}
//--------------------------------------------------------------------------------------------------
OverscanDimensions SnesHdVideoFilter::GetOverscan()
{
	// Si un pack HD est actif et qu'il possède le tag <overscan>
	if(SnesHdPackLoader::ActivePack != nullptr && SnesHdPackLoader::ActivePack->HasOverscanConfig) {
		return SnesHdPackLoader::ActivePack->Overscan;
	} else {
		// Sinon, on appelle gentiment l'overscan standard de la console parent
		return SnesDefaultVideoFilter::GetOverscan();
	}
}
//--------------------------------------------------------------------------------------------------
```

---

## ./SnesHdVideoFilter.h

```h
#pragma once

#include "pch.h"
#include "SNES/SnesDefaultVideoFilter.h"

struct SnesHdPixelData;

// On hérite du filtre par défaut pour récupérer sa gestion des couleurs et de la luminosité
class SnesHdVideoFilter : public SnesDefaultVideoFilter
{
private:
	FrameInfo _sdFrameInfo = {}; // Pour garder en mémoire la taille basse résolution
	std::vector<uint8_t> _zBuffer; // La carte de profondeur HD


// Sous-fonction pour le rendu standard (via le PPU)
	void ApplyFilterSpritesStandard(uint32_t x, uint32_t y, uint32_t scale, uint32_t outWidth,
									  uint32_t sdColor, const SnesHdPixelData& hdData, // Plus de "struct" ici !
									  uint8_t pixelZ, uint32_t* out);

//La méthode OAM Scan (Bypass du clipping)
	void ApplyFilterSpritesOAMScan(uint32_t scale, uint32_t outWidth, uint32_t outHeight, uint32_t* out);

protected:
	FrameInfo GetFrameInfo() override;

public:
	SnesHdVideoFilter(Emulator* emu);
	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
	OverscanDimensions GetOverscan() override;
};
```

---

