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


	bool AlphaEnable = true; // true par défaut : le noir pur (#000000) est transparent
	bool Stretch = false;     // false par défaut : étire la vidéo sur tout l'écran SNES visible

	//  PARAMÈTRES PIP (Picture-in-Picture) ---
	int LoopTargetFrame = 0; // 0 = pas de boucle, -1 = boucle infinie, > 0 = frame de redémarrage
	//bool Loop = false;
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

	// Contenu du fichier video precharge en memoire par LoadAsync (vide = lecture depuis le disque)
	std::shared_ptr<std::vector<uint8_t>> FileData;

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
//-------------------------------------------------------------------
// Structure qui référence TOUS les sprites HD actifs sur la frame.
// Permet de gérer les chevauchements de 3+ sprites (mini-map MK, Zelda, etc.)
struct ActiveHdSprite
{
	SnesHdPackTileInfo* HdTile; // Pointeur vers la tuile HD (nullptr = pas de remplacement)
	int StartX;                 // Coordonnée X exacte (DrawX) du groupe de 8px
	int StartY;                 // Coordonnée Y exacte de la tuile 8x8 dans le buffer d'image final
	uint8_t SnesPriority;       // Priorité OAM brute (0-3), sera convertie par la table de priorité
	uint8_t OamIndex;           // Index OAM (0 = priorité la plus haute en SNES)
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
	SpriteNearby,
	ColorWindow
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
	// <m7tiles> : mipmaps de la tile extraite de l'atlas (construits par le filtre HD)
	std::vector<std::vector<uint32_t>> TileMips;
	int X = 0;
	int Y = 0;

	std::vector<SnesHdPackCondition*> Conditions;
	
	// Pré-génération des Mipmaps en asynchrone

	void Init(bool& cancelToken)
	{
		if(!Bitmap) return;
		Bitmap->Init();
		if(cancelToken || Bitmap->PixelData.empty()) return;

		// Si les mipmaps sont déjà générés dans le Bitmap, on ne recalcule rien
		if(!Bitmap->Mipmaps.empty()) return;

		uint32_t mapDim = 1024 * Scale;
		Bitmap->Mipmaps.push_back(Bitmap->PixelData);

		uint32_t currentDim = mapDim;
		for(int level = 1; level <= 3; level++) {
			if(cancelToken) return;

			uint32_t prevDim = currentDim;
			currentDim /= 2;
			std::vector<uint32_t> nextMip(currentDim * currentDim, 0);
			const std::vector<uint32_t>& prevMip = Bitmap->Mipmaps[level - 1];

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
			Bitmap->Mipmaps.push_back(std::move(nextMip));
		}
	}
	



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

//----------------------------------------------------------
// Mipmaps Mode 7 (definis dans SnesHdVideoFilter.cpp) : appeles au chargement du pack (LoadAsync)
// et, par securite, a la premiere utilisation dans le filtre.
void BuildMode7MipChain(SnesHdPackBitmapInfo* bmp, uint32_t mapDim);
void BuildMode7TileMips(SnesHdMode7Image* img);

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
// <2DOverlay> : effets 2D calcules (etape 1 : particules). Voir docs/claude/09_2DOverlay.md
// Unites : positions en pixels SNES dans un champ torique (taille ecran x FieldW/FieldH),
// vitesses en pixels SNES / seconde, angles en degres (0 = droite, 90 = bas).
// ---------------------------------------------------------
struct SnesHdParticle
{
	float X = 0, Y = 0;       // position dans le champ
	float VX = 0, VY = 0;     // vitesse propre
	float Age = 0, Life = 1;  // secondes
	float Size = 1;           // multiplicateur de taille propre (aleatoire)
	float Rot = 0, Spin = 0;  // radians, radians / s
	float Seed1 = 0, Seed2 = 0;
};

struct SnesHdOverlayInfo
{
	std::string JsonFile;
	std::vector<SnesHdPackCondition*> Conditions;

	// Parametres de la balise (valeurs par defaut, puis JSON, puis balise)
	int ZIndex = 95;
	int Layer = 0;                      // 0 = fixe a l'ecran, 1-4 = couche BG d'ancrage
	float SpeedX = 1.0f, SpeedY = 1.0f; // ratio de defilement relatif a la couche (s'ajoute au mouvement propre)
	float Alpha = 1.0f;
	float FieldW = 1.0f, FieldH = 1.0f;

	// Particules (JSON)
	SnesHdPackBitmapInfo* Texture = nullptr; // PNG de la particule (dessine en pixels HD) ; nullptr = point doux
	bool Additive = false;
	int MaxParticles = 300;
	float Rate = 60.0f;                      // particules creees par seconde
	float LifeMin = 2.0f, LifeMax = 4.0f;
	float SpawnX = 0, SpawnY = 0, SpawnW = 1, SpawnH = 1; // zone d'apparition (fractions du champ)
	float SpeedMin = 20, SpeedMax = 40;
	float AngleMin = 80, AngleMax = 100;
	float GravityX = 0, GravityY = 0;        // pixels SNES / s^2
	float WindX = 0, WindY = 0;              // pixels SNES / s
	float Turbulence = 0, TurbulenceFreq = 0.5f;
	float RotationMin = 0, RotationMax = 0;  // degres
	float SpinMin = 0, SpinMax = 0;          // degres / s
	float SizeStart = 1, SizeEnd = 1, SizeRandom = 0;
	float AlphaStart = 1, AlphaEnd = 1, FadeIn = 0.1f, FadeOut = 0.2f;
	uint32_t ColorStart = 0xFFFFFF, ColorEnd = 0xFFFFFF; // teinte RGB (multiplie la texture)
	float DotSize = 2.0f;                    // diametre du point doux (pixels SNES) si pas de texture
	bool Prewarm = true;                     // remplit l'ecran des la 1re apparition

	// Etat d'execution (filtre HD)
	std::vector<SnesHdParticle> Particles;
	bool Initialized = false;
	bool Active = false;
	float SpawnAcc = 0, Time = 0;
	float OffsetX = 0, OffsetY = 0;          // decalage d'ancrage accumule (pixels SNES)
	int LastScrollX = 0, LastScrollY = 0;
	bool HaveScroll = false;
	uint32_t Rng = 0x12345678;
	float FieldWidthPx = 256, FieldHeightPx = 224;

	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
	float Rand() { Rng ^= Rng << 13; Rng ^= Rng >> 17; Rng ^= Rng << 5; return (float)(Rng & 0xFFFFFF) / 16777216.0f; }
	float RandRange(float a, float b) { return a + (b - a) * Rand(); }
};

// ---------------------------------------------------------
// Capture Mode 7 par ligne (PPU -> filtre) : origine (x = 0) et pas horizontal, en 8.8
// Base de la camera Mode 7 reconstruite (SnesHdMode7Camera.h)
// ---------------------------------------------------------
struct SnesHdM7LineCapture
{
	int32_t StartX = 0, StartY = 0;
	int16_t A = 0, C = 0;
	uint8_t Valid = 0;
	uint8_t LargeMap = 0; // carte non repetee (hors carte = tile 0 / transparent)
};

// ---------------------------------------------------------
// <3DParticles> : particules dans le monde Mode 7 (carte en texels + hauteur), projetees
// avec la camera reconstruite de chaque vue. Voir docs/claude/10_Mode7_Camera_3D.md
// ---------------------------------------------------------
struct SnesHdParticle3D
{
	float X = 0, Y = 0, Z = 0;     // monde (texels), Z = hauteur au-dessus du sol
	float VX = 0, VY = 0, VZ = 0;  // texels / s
	float Age = 0, Life = 0;       // Life = 0 : immortelle
	float Size = 1;
	float Seed1 = 0, Seed2 = 0;
};

struct SnesHdParticles3DInfo
{
	std::string JsonFile;
	std::vector<SnesHdPackCondition*> Conditions;

	int ZIndex = 95;
	int Zone = -1;          // -1 = toutes les vues, 0 = vue du haut, 1 = vue du bas
	float Alpha = 1.0f;

	SnesHdPackBitmapInfo* Texture = nullptr;
	bool Additive = false;
	int MaxParticles = 400;
	float Rate = 0;          // 0 = boite toujours pleine
	float LifeMin = 0, LifeMax = 0; // 0 = immortelles
	float BoxSize = 512, BoxHeight = 128, BoxAhead = 0.35f; // boite autour de la camera (texels)
	float FallMin = 20, FallMax = 40;    // vitesse de chute (texels / s, positive = vers le bas)
	float DriftMin = 0, DriftMax = 6;    // derive horizontale aleatoire
	float Gravity = 0;                   // texels / s^2 vers le bas
	float WindX = 0, WindY = 0;          // vent dans les axes de la carte
	float Turbulence = 0, TurbulenceFreq = 0.5f;
	float SizeStart = 2, SizeEnd = 2, SizeRandom = 0; // taille dans le monde (texels)
	float MinPixelSize = 1.5f;           // taille mini a l'ecran (pixels HD) : au-dela, l'alpha baisse
	float AlphaStart = 1, AlphaEnd = 1, FadeIn = 0.1f, FadeOut = 0.2f;
	uint32_t ColorStart = 0xFFFFFF, ColorEnd = 0xFFFFFF;
	float FarFade = 0.3f;                // part du bord de la boite ou l'alpha s'estompe
	float NearFade = 24.0f;              // en dessous de cette distance (texels), l'alpha s'estompe
	bool GroundRespawn = true;           // au sol : reapparait en haut (sinon meurt)

	struct ViewState
	{
		std::vector<SnesHdParticle3D> Parts;
		bool Initialized = false;
		bool HaveCam = false;
		double CamWX = 0, CamWY = 0;     // camera dans le monde "deplie"
		double LastCx = 0, LastCy = 0;   // camera dans les coordonnees de la carte VRAM
		double CenterX = 0, CenterY = 0; // centre de la boite (monde)
		float SpawnAcc = 0, Time = 0;
		bool Active = false;
	};
	ViewState Views[2];

	uint32_t Rng = 0x9E3779B9;
	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
	float Rand() { Rng ^= Rng << 13; Rng ^= Rng >> 17; Rng ^= Rng << 5; return (float)(Rng & 0xFFFFFF) / 16777216.0f; }
	float RandRange(float a, float b) { return a + (b - a) * Rand(); }
};

// ---------------------------------------------------------
// <m7relief> : volume d'une tile Mode 7 (rendu "Voxel Space" sur la camera reconstruite)
// PNG de relief : pixel transparent = sol plat ; opaque = en volume.
//  mode extrude (defaut) : tout pixel d'alpha >= 128 monte a Height ; mode height : alpha = hauteur.
// ---------------------------------------------------------
struct SnesHdM7ReliefInfo
{
	uint32_t TileHash = 0;
	SnesHdPackBitmapInfo* Bitmap = nullptr; // nullptr = "full" : toute la tile en volume
	int X = 0, Y = 0, Size = 32;            // zone de la PNG (atlas possible)
	float Height = 6.0f;                    // hauteur maxi (texels de carte)
	float Sides = 0.7f;                     // luminosite des flancs
	bool HeightMap = false;                 // alpha = hauteur (sinon extrusion)
	bool ColorFromPng = false;              // couleur : PNG de relief (sinon tile HD affichee)
	std::vector<SnesHdPackCondition*> Conditions;

	std::vector<uint8_t> H;                 // hauteur 0-255 par pixel (Size x Size)
	std::vector<uint32_t> C;                // couleur de la PNG
	bool Built = false;

	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
	void Build()
	{
		if(Built) return;
		Built = true;
		if(!Bitmap) { Size = 1; H.assign(1, 255); C.assign(1, 0xFFFFFFFF); return; }
		if(Bitmap->PixelData.empty()) Bitmap->Init();
		Size = std::max(1, Size);
		H.assign((size_t)Size * Size, 0);
		C.assign((size_t)Size * Size, 0);
		for(int y = 0; y < Size; y++) {
			for(int x = 0; x < Size; x++) {
				int sx = X + x, sy = Y + y;
				if(sx < 0 || sy < 0 || sx >= (int)Bitmap->Width || sy >= (int)Bitmap->Height) continue;
				uint32_t c = Bitmap->PixelData[(size_t)sy * Bitmap->Width + sx];
				uint8_t a = (uint8_t)(c >> 24);
				H[(size_t)y * Size + x] = HeightMap ? a : (a >= 128 ? 255 : 0);
				C[(size_t)y * Size + x] = c;
			}
		}
	}
};

// ---------------------------------------------------------
// <m7billboard> : panneau (PNG debout, toujours face a la camera) plante sur la carte Mode 7
// ---------------------------------------------------------
struct SnesHdM7BillboardInfo
{
	SnesHdPackBitmapInfo* Bitmap = nullptr;
	bool AnyMap = true;      // sinon : seulement sur la carte <m7img> de hash MapHash
	uint32_t MapHash = 0;
	bool OnTile = false;     // plante au centre de chaque case contenant la tile TileHash
	uint32_t TileHash = 0;
	float X = 0, Y = 0;      // position sur la carte (texels, 0-1023)
	float Height = 48.0f;    // hauteur dans le monde (texels de carte)
	float Elevation = 0.0f;  // hauteur du pied au-dessus du sol
	float Alpha = 1.0f;
	std::vector<SnesHdPackCondition*> Conditions;

	std::vector<std::vector<uint32_t>> Mips; // niveau 0 = PNG, puis moities
	std::vector<uint32_t> MipW, MipH;
	bool Built = false;

	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
	void Build()
	{
		if(Built) return;
		Built = true;
		if(!Bitmap) return;
		if(Bitmap->PixelData.empty()) Bitmap->Init();
		if(Bitmap->PixelData.empty() || Bitmap->Width == 0 || Bitmap->Height == 0) return;
		Mips.push_back(Bitmap->PixelData); MipW.push_back(Bitmap->Width); MipH.push_back(Bitmap->Height);
		while(MipW.back() > 1 || MipH.back() > 1) {
			uint32_t pw = MipW.back(), ph = MipH.back();
			uint32_t nw = std::max<uint32_t>(1, pw / 2), nh = std::max<uint32_t>(1, ph / 2);
			const std::vector<uint32_t>& prev = Mips.back();
			std::vector<uint32_t> m((size_t)nw * nh, 0);
			for(uint32_t y = 0; y < nh; y++) {
				for(uint32_t x = 0; x < nw; x++) {
					uint32_t sumA = 0, r = 0, g = 0, b = 0, n = 0;
					for(uint32_t dy = 0; dy < 2; dy++) for(uint32_t dx = 0; dx < 2; dx++) {
						uint32_t sx = std::min(pw - 1, x * 2 + dx), sy = std::min(ph - 1, y * 2 + dy);
						uint32_t q = prev[(size_t)sy * pw + sx], a = q >> 24;
						sumA += a; r += ((q >> 16) & 0xFF) * a; g += ((q >> 8) & 0xFF) * a; b += (q & 0xFF) * a; n++;
					}
					if(sumA > 0) m[(size_t)y * nw + x] = ((sumA / n) << 24) | ((r / sumA) << 16) | ((g / sumA) << 8) | (b / sumA);
				}
			}
			Mips.push_back(std::move(m)); MipW.push_back(nw); MipH.push_back(nh);
		}
	}
};

// ---------------------------------------------------------
// <m7model> : modele 3D (.obj + .mtl) pose sur la carte Mode 7, rendu par un petit moteur logiciel
// ---------------------------------------------------------
struct SnesHdModelMaterial
{
	std::string Name;
	uint32_t Kd = 0xFFFFFF;                 // couleur diffuse (multiplie la texture)
	float Alpha = 1.0f;                     // d
	SnesHdPackBitmapInfo* Texture = nullptr; // map_Kd
};

struct SnesHdModelTri
{
	float P[3][3];   // positions (unites OBJ, Y vers le haut)
	float UV[3][2];  // coordonnees de texture (v vers le haut, convention OBJ)
	float N[3][3];   // normales (normale de la face si le fichier n'en a pas)
	int Mat = 0;
};

struct SnesHdModelMesh
{
	std::vector<SnesHdModelTri> Tris;
	std::vector<SnesHdModelMaterial> Mats;
	float MinX = 0, MinY = 0, MinZ = 0, MaxX = 0, MaxY = 0, MaxZ = 0;
};

struct SnesHdM7ModelInfo
{
	std::shared_ptr<SnesHdModelMesh> Mesh;
	std::string File;
	bool AnyMap = true;
	uint32_t MapHash = 0;
	bool OnTile = false;
	uint32_t TileHash = 0;
	float X = 0, Y = 0;          // position sur la carte (texels)
	float Scale = 8.0f;          // texels de carte par unite OBJ (8 = 1 unite fait une tile)
	float Rotation = 0.0f;       // degres, autour de la verticale
	float Elevation = 0.0f;      // texels au-dessus du sol
	float Ambient = 0.45f;       // eclairage : part ambiante (0 = tres contraste, 1 = plat)
	bool KeepOrigin = false;     // false : la base du modele (centre, Y mini) est posee au sol en (x, y)
	std::vector<SnesHdPackCondition*> Conditions;

	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
};

// ---------------------------------------------------------
// Zone de color window (fenetre PPU appliquee au color math) visible sur la frame :
// transparences/masques dessines par fenetres, ex. numero de position de Mario Kart.
struct SnesHdColorWindowRegion
{
	int X = 0, Y = 0, W = 0, H = 0; // cadre (pixels SNES, lignes du buffer de sortie)
	uint32_t PixelCount = 0;
	uint32_t Hash = 0;               // hash de la forme, independant de la position
	bool FullScreen = false;         // > 80% de l'ecran (fondu, voile...)
	uint16_t FixedColor = 0;         // couleur fixe COLDATA (BGR555) sur la 1re ligne de la zone
	uint8_t Op = 0;                  // bit0 = soustraction, bit1 = resultat / 2, bit2 = source sub-screen
};

// Regle <disableColorMathWindow>HASH (avec ses conditions)
struct SnesHdDisableColorWindowRule
{
	uint32_t Hash = 0;
	std::vector<SnesHdPackCondition*> Conditions;
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
	// Double-bufferisee comme ActiveSpritesQueues (meme index d'ecriture / de lecture).
	std::vector<ActiveHdAddition> ActiveAdditionsQueues[2];

	// Liste de TOUS les sprites HD actifs sur la frame en cours (double-bufferisée).
	// - Le PPU écrit dans ActiveSpritesQueues[ActiveSpriteQueueWriteIndex].
	// - SnesPpu::SendFrame() fige ActiveSpriteQueueReadIndex = WriteIndex : c'est la queue
	//   de la frame envoyée au thread vidéo.
	// - Le filtre lit ActiveSpritesQueues[ActiveSpriteQueueReadIndex].
	// - Au scanline 0, le PPU ne bascule sur l'autre queue que si la queue courante a été
	//   remise au filtre (WriteIndex == ReadIndex), puis la vide.
	// Ainsi la queue lue par le filtre correspond toujours à la frame décodée, et n'est
	// jamais vidée pendant sa lecture.
	std::vector<ActiveHdSprite> ActiveSpritesQueues[2];
	int ActiveSpriteQueueWriteIndex = 0;
	int ActiveSpriteQueueReadIndex = 1;

	// Zones de color window de la derniere frame envoyee (SnesPpu::UpdateColorWindowRegions)
	std::vector<SnesHdColorWindowRegion> ColorWindowRegions;
	// Numero de zone par pixel (256 x 240, -1 = hors zone) ; vide s'il n'y a aucune zone
	std::vector<int16_t> ColorWindowLabels;

	// <disableAllColorMathWindow> : une entree par ligne du pack (conditions de cette ligne)
	std::vector<std::vector<SnesHdPackCondition*>> DisableAllColorMathWindowRules;
	// <disableColorMathWindow>HASH
	std::vector<SnesHdDisableColorWindowRule> DisableColorMathWindowRules;

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

	// Effets <2DOverlay> (particules 2D...)
	std::vector<std::unique_ptr<SnesHdOverlayInfo>> Overlays;

	// Effets <3DParticles> (particules dans le monde Mode 7)
	std::vector<std::unique_ptr<SnesHdParticles3DInfo>> Particles3D;

	// <m7relief> : volume des tiles Mode 7, par hash de tile
	std::unordered_map<uint32_t, std::vector<std::unique_ptr<SnesHdM7ReliefInfo>>> M7Reliefs;

	// <m7billboard> : panneaux plantes sur la carte Mode 7
	std::vector<std::unique_ptr<SnesHdM7BillboardInfo>> M7Billboards;

	// <m7model> : modeles 3D .obj poses sur la carte Mode 7
	std::vector<std::unique_ptr<SnesHdM7ModelInfo>> M7Models;

	// <m7CameraDebug> : reperes de controle de la camera Mode 7 reconstruite
	bool M7CameraDebug = false;
	std::vector<SnesHdPackCondition*> M7CameraDebugConditions;

	// MPEG1 videos
	std::vector<std::unique_ptr<SnesHdVideoInfo>> Videos;


	std::unordered_map<std::string, std::string> PatchesByHash;
	

	/*------------------------------------------------------------------------------------*/

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

		// Mode 7 : mipmaps des cartes <m7img> et des tiles <m7tiles>.
		// (sinon construits a la premiere image Mode 7 affichee -> saccade de plusieurs secondes en scale 8/16)
		size_t m7MapCount = 0, m7TileCount = 0;
		for(auto& pair : Mode7Maps) {
			for(auto& img : pair.second) {
				if(_cancelLoad) return;
				if(!img || !img->Bitmap) continue;
				img->Bitmap->Init();
				if(img->Bitmap->PixelData.empty()) continue;
				uint32_t mapDim = 1024 * img->Scale;
				if(img->X == 0 && img->Y == 0 && img->Bitmap->Width == mapDim && img->Bitmap->Height == mapDim) {
					BuildMode7MipChain(img->Bitmap, mapDim);
					m7MapCount++;
				} else {
					BuildMode7TileMips(img.get());
					m7TileCount++;
				}
			}
		}
		if(m7MapCount + m7TileCount > 0) {
			MessageManager::Log("[HD-MODE7] Prechargement : " + std::to_string(m7MapCount) + " carte(s) <m7img>, " +
				std::to_string(m7TileCount) + " tile(s) <m7tiles> avec mipmaps.");
		}

		// Videos <video> : prechargement du fichier en memoire (evite l'acces disque au lancement de la video).
		// Au-dela de 512 Mo, la video reste lue depuis le disque.
		for(auto& video : Videos) {
			if(_cancelLoad) return;
			if(!video || video->Filename.empty()) continue;
			std::string fullPath = FolderUtilities::CombinePath(PackFolderPath, video->Filename);
			VirtualFile vFile(fullPath);
			if(!vFile.IsValid()) {
				MessageManager::Log("[HD-VIDEO] Fichier introuvable : " + fullPath);
				continue;
			}
			if(vFile.GetSize() > (size_t)512 * 1024 * 1024) {
				MessageManager::Log("[HD-VIDEO] " + video->Filename + " : plus de 512 Mo, lecture depuis le disque.");
				continue;
			}
			auto data = std::make_shared<std::vector<uint8_t>>();
			if(vFile.ReadFile(*data) && !data->empty()) {
				video->FileData = data;
				MessageManager::Log("[HD-VIDEO] Precharge en memoire : " + video->Filename + " (" +
					std::to_string(data->size() / 1024) + " Ko)");
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


	// version buguée pour les M4img
	/*
	void LoadAsync()
	{
		// 1. Décompression des PNG bruts (Backgrounds, Spritesheets, Atlas Mode 7)
		for(auto& bitmap : ImageFileData) {
			bitmap->Init();
			if(_cancelLoad) return;
		}

		// 2. Découpage et initialisation des tuiles HD classiques
		for(auto& entry : TileByKey) {
			for(auto* tile : entry.second) {
				tile->Init(Scale);
				if(_cancelLoad) return;
			}
		}

		// 3. Préchargement et génération des Mipmaps Mode 7 (<m7img> et <m7tiles>)
		size_t m7LoadedCount = 0;
		for(auto& pair : Mode7Maps) {
			for(auto& m7Image : pair.second) {
				if(_cancelLoad) return;
				if(m7Image && m7Image->Bitmap) {
					m7Image->Init(_cancelLoad);
					m7LoadedCount++;
				}
			}
		}

		if(m7LoadedCount > 0) {
			MessageManager::Log("[HD-MODE7] Prechargement termine : " +
				std::to_string(m7LoadedCount) + " texture(s) Mode 7 initialisee(s).");
		}

		// 4. Préchargement audio OGG (<msu2ogg>)
		size_t loadedTracksCount = 0;
		size_t totalBytesLoaded = 0;

		for(auto& pair : MsuTracks) {
			for(auto& track : pair.second) {
				if(_cancelLoad) return;
				if(track.Filename.empty()) continue;

				auto it = OggCache.find(track.Filename);
				if(it != OggCache.end()) {
					track.AudioData = it->second;
					continue;
				}

				std::string fullPath = FolderUtilities::CombinePath(PackFolderPath, track.Filename);
				auto fileData = std::make_shared<std::vector<uint8_t>>();
				VirtualFile vFile(fullPath);

				if(vFile.ReadFile(*fileData) && !fileData->empty()) {
					totalBytesLoaded += fileData->size();
					loadedTracksCount++;
					OggCache[track.Filename] = fileData;
					track.AudioData = fileData;
				} else {
					MessageManager::Log("[MSU1-HD] Erreur de prechargement audio : " + track.Filename);
				}
			}
		}

		if(loadedTracksCount > 0) {
			MessageManager::Log("[MSU1-HD] Prechargement audio termine : " +
				std::to_string(loadedTracksCount) + " fichier(s) OGG en RAM (" +
				std::to_string(totalBytesLoaded / (1024 * 1024)) + " Mo)");
		}

		MessageManager::Log("[HD TRACE SYNCHRON] SUCCESS : Hash of tile success !");
	}
	*/
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