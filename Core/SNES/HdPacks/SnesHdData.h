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
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"

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

	// ON NE MET QUE LA DÉCLARATION ICI (Pas de moustaches { } !)
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
	int16_t ScanlineHScroll[240][4] = { { 0 } };
	int16_t ScanlineVScroll[240][4] = { { 0 } };
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

	// Centralisation des Mipmaps partagés pour cette image PNG
	std::vector<std::vector<uint32_t>> Mipmaps;

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

		// =========================================================================
		// SÉCURITÉ ANTI-CRASH : Si la tuile est masquée ou sans image, on sort !
		// =========================================================================
		if(IsDisabled || Bitmap == nullptr) return;

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

	enum class ScrollOperator
	{
		Multiply,
		Divide,
		Plus,
		Minus
	};

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


	float AlphaFactor = 1.0f; // Défaut 1.00 (100% de l'alpha d'origine)
	ScrollOperator ScrollSpeedOperator = ScrollOperator::Multiply; // Défaut 'multiply' / '*'

	std::vector<SnesHdPackCondition*> Conditions;

	bool MatchesCondition()
	{
		for(auto* cond : Conditions) {
			if(!cond->CheckCondition(0, 0, nullptr)) return false;
		}
		return true;
	}
};


enum class Mode7FilterMode
{
	Nearest,
	Bilinear,
	Anisotropic
};


//----------------------------------------------------------------

struct SnesHdMode7Image
{
	SnesHdPackBitmapInfo* Bitmap = nullptr;
	uint32_t Scale = 1;

	bool IgnoreColorMath = false;
	Mode7FilterMode FilterMode = Mode7FilterMode::Nearest;

	// Rétrocompatibilité si d'anciennes fonctions lisent encore ce booléen
	bool UseBilinearFilter() const { return FilterMode == Mode7FilterMode::Bilinear; }

	std::vector<std::vector<uint32_t>> Mipmaps;
	int X = 0;
	int Y = 0;

	std::vector<SnesHdPackCondition*> Conditions;
	
	
	//bool MatchesCondition();

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


	//SnesHdPixelData GetPixel(int x, int y);


	SnesHdPixelData GetPixel(int x, int y)
	{
		if(!Bitmap || Bitmap->PixelData.empty()) return {};

		// x et y sont les coordonnées de la sous-tuile SNES (0-7)
		// On les multiplie par l'échelle pour taper dans l'image HD,
		// puis on y ajoute X et Y de l'atlas (qui sont déjà à l'échelle HD)
		uint32_t sx = ((uint32_t)x * Scale) + X;
		uint32_t sy = ((uint32_t)y * Scale) + Y;

		if(sx < Bitmap->Width && sy < Bitmap->Height) {
			SnesHdPixelData data;
			data.Color = Bitmap->PixelData[sy * Bitmap->Width + sx];
			data.HasData = true;
			return data;
		}
		return {};
	}




};
/*
struct SnesHdMode7Image
{
	SnesHdPackBitmapInfo* Bitmap = nullptr;
	uint32_t Scale = 1;
	
	bool IgnoreColorMath = false;
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



	//2026-09-01
	SnesHdPixelData GetPixel(int x, int y)
	{
		if(!Bitmap || Bitmap->PixelData.empty()) return {};

		// x et y sont les coordonnées de la sous-tuile SNES (0-7)
		// On les multiplie par l'échelle pour taper dans l'image HD,
		// puis on y ajoute X et Y de l'atlas (qui sont déjà à l'échelle HD)
		uint32_t sx = ((uint32_t)x * Scale) + X;
		uint32_t sy = ((uint32_t)y * Scale) + Y;

		if(sx < Bitmap->Width && sy < Bitmap->Height) {
			SnesHdPixelData data;
			data.Color = Bitmap->PixelData[sy * Bitmap->Width + sx];
			data.HasData = true;
			return data;
		}
		return {};
	}

};
*/
//----------------------------------------------------------
struct SnesHdMsuTrack
{
	std::string Filename;
	uint32_t LoopPoint = 0;

	// Données binaires du fichier OGG préchargées en mémoire
	std::shared_ptr<std::vector<uint8_t>> AudioData = nullptr;

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
	bool OverscanStretch = true; // true is défault (streched image)

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

	// Cache des fichiers audio OGG préchargés (évite les doublons en RAM si plusieurs pistes/conditions partagent le même fichier)
	std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> OggCache;

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

		// Préchargement en mémoire des fichiers audio OGG (<msu2ogg>)
		size_t loadedTracksCount = 0;
		size_t totalBytesLoaded = 0;

		for(auto& pair : MsuTracks) {
			for(auto& track : pair.second) {
				if(_cancelLoad) return;
				if(track.Filename.empty()) continue;

				// Si déjà en cache
				auto it = OggCache.find(track.Filename);
				if(it != OggCache.end()) {
					track.AudioData = it->second;
					continue;
				}

				// Chargement depuis le disque vers la mémoire
				string fullPath = FolderUtilities::CombinePath(PackFolderPath, track.Filename);
				auto fileData = std::make_shared<std::vector<uint8_t>>();
				VirtualFile vFile(fullPath);

				if(vFile.ReadFile(*fileData) && !fileData->empty()) {
					totalBytesLoaded += fileData->size();
					loadedTracksCount++;
					OggCache[track.Filename] = fileData;
					track.AudioData = fileData;
				} else {
					MessageManager::Log("[MSU1-HD] Erreur de préchargement audio : " + track.Filename);
				}
			}
		}

		if(loadedTracksCount > 0) {
			MessageManager::Log("[MSU1-HD] Préchargement audio terminé : " + 
				std::to_string(loadedTracksCount) + " fichier(s) OGG en RAM (" + 
				std::to_string(totalBytesLoaded / (1024 * 1024)) + " Mo)");
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


	//----------------------------------------------------------------------*/
	// 2. Chercher un pixel dans une tuile HD
	SnesHdPixelData GetHdTilePixel(uint64_t tileHash, int x, int y)
	{
		SnesHdTileKey key;
		key.MemoryHash = tileHash;
		key.PaletteHash = 0;
		key.Bpp = SnesBpp::Mode7;
		key.IsLargeTile = false;
		key.IsSprite = false;

		// Recherche avec palette 0 puis fallback wildcard 0xFFFFFFFF
		auto it = TileByKey.find(key);
		if(it == TileByKey.end()) {
			key.PaletteHash = 0xFFFFFFFF;
			it = TileByKey.find(key);
		}

		if(it != TileByKey.end() && !it->second.empty()) {
			for(SnesHdPackTileInfo* tile : it->second) {
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