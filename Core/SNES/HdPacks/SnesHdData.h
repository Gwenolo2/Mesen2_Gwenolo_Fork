#pragma once
#include <map>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set> // Ajouté pour WatchedMemoryAddresses
#include <vector>
#include <array>
#include <cstring>
#include "Shared/MessageManager.h"
#include "Shared/HdLoadingScreen.h"
#include <unordered_set>
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

// Bande 16:9 Mode 7 (tuiles <m7tiles>) : rangee dans le buffer HD, apres les lignes 512 px.
// Ligne y, colonnes SNES -128..-1 -> index +0..127, colonnes 256..383 -> index +128..255.
constexpr uint32_t SnesHdM7BandBase = 512u * 240u;


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
bool SnesHdDecodeImageFallback(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h);

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
		} else if(!FileData.empty()) {
			// pas un PNG : JPG / BMP / TGA (textures des modeles glTF)
			SnesHdDecodeImageFallback(FileData, PixelData, Width, Height);
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
	// <Ignore2DOverlays>{ } : toute la ressource reste nette pendant la distorsion <2DOverlay> (si les conditions du bloc sont vraies)
	bool Ignore2D = false;
	std::vector<SnesHdPackCondition*> Ignore2DConditions;
	bool IsIgnored2D()
	{
		if(!Ignore2D) return false;
		for(auto* c : Ignore2DConditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}

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
	// widescreen (token 14) : 0 auto (tiled = etendu, non tiled = zone SNES), 1 on (repete dans les bandes), 2 off (zone SNES)
	uint8_t WsRule = 0;

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
	Anisotropic,
	BilinearLight // bilineaire sur un seul niveau de mip (moins de lectures, pas de melange de niveaux)
};


//----------------------------------------------------------------

struct SnesHdMode7Image
{
	// <Ignore2DOverlays>{ } : toute la ressource reste nette pendant la distorsion <2DOverlay> (si les conditions du bloc sont vraies)
	bool Ignore2D = false;
	std::vector<SnesHdPackCondition*> Ignore2DConditions;
	bool IsIgnored2D()
	{
		if(!Ignore2D) return false;
		for(auto* c : Ignore2DConditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
	SnesHdPackBitmapInfo* Bitmap = nullptr;
	uint32_t Scale = 1;

	bool IgnoreColorMath = false;
	Mode7FilterMode FilterMode = Mode7FilterMode::Nearest;

	// Rétrocompatibilité si d'anciennes fonctions lisent encore ce booléen
	bool UseBilinearFilter() const { return FilterMode == Mode7FilterMode::Bilinear || FilterMode == Mode7FilterMode::BilinearLight; }

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

// <2DLiquid> (SNES) : nappe de liquide, meme format que NES. view=side (profil, surface + bulles) ou view=top (zone + ondulations)
struct SnesHdLiquidBubble
{
	float X = 0, Y = 0, R = 0, A = 0; // pixels HD
	bool Ring = false;                // phase d'eclatement (anneau)
};

struct SnesHdLiquidInfo
{
	std::vector<SnesHdPackCondition*> Conditions;
	bool Active = false;
	bool IsTop = false;                   // view=top
	float Level = 176.0f;                 // view=side : hauteur de la surface (lignes SNES)
	float Depth = 64.0f;                  // profondeur du degrade (lignes SNES)
	float XMin = 0.0f, XMax = 1.0f;       // zone horizontale (fraction de la largeur)
	float YMin = 0.0f, YMax = 224.0f;     // view=top : zone verticale (lignes SNES)
	float Amp = 3.0f, Wave = 48.0f, Speed = 0.8f, Phase = 0.0f; // houle / ondulations
	float ScrollX = 0.0f;                 // defilement de la texture (px SNES / s)
	float TexScale = 1.0f;                // taille d'un texel en pixels SNES
	float Foam = 0.6f;                    // eclaircissement (ecume, crete des ondes) 0 a 1
	float Alpha = 1.0f;                   // opacite du liquide
	uint32_t Color = 0x1E6FB0, Deep = 0x0B2F5C;
	SnesHdPackBitmapInfo* Texture = nullptr;
	float Glow = 0.0f;                    // halo lumineux au-dessus de la surface (0 a 1)
	float GlowRadius = 24.0f;             // rayon du halo (lignes SNES)
	int BubbleCount = 0;                  // view=side : bulles qui montent et eclatent a la surface
	float BubbleSize = 1.5f;              // rayon des bulles (lignes SNES)
	float BubbleRise = 0.4f;              // montee des bulles (cycles / s)
	int ZIndex = 95;                      // prio= : z sur l'echelle SNES (comme Z de <2DOverlay>)
	float Time = 0.0f;

	// Etat par image (calcule dans ApplyFilter)
	uint32_t Col0 = 0, Col1 = 0;          // colonnes HD
	uint32_t RowMin = 0, RowMax = 0;      // lignes HD
	std::vector<float> Surface;           // view=side : surface par colonne HD (lignes SNES)
	std::vector<SnesHdLiquidBubble> Bubbles;

	bool MatchesCondition()
	{
		for(SnesHdPackCondition* cond : Conditions) {
			if(!cond->CheckCondition(0, 0, nullptr)) {
				return false;
			}
		}
		return true;
	}
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

	// Distorsion d'image (type=distortion) : deformation horizontale sinusoidale de l'image finale (chaleur)
	bool IsDistortion = false;
	float DistAmp1 = 0, DistWave1 = 32, DistSpeed1 = 0.5f, DistPhase1 = 0;  // amplitude (px SNES), longueur d'onde (lignes SNES), vitesse (cycles / s), phase (degres)
	float DistAmp2 = 0, DistWave2 = 17, DistSpeed2 = 0.3f, DistPhase2 = 0;  // 2e sinusoide, combinee a la 1re
	float ZoneX0 = 0, ZoneX1 = 1, ZoneY0 = 0, ZoneY1 = 1;

	// Rayons de lumiere (type=rays) : faisceaux additifs partant d'un point, animes
	bool IsRays = false;
	float RayX = 0.5f, RayY = 0.0f;        // source (fractions de l'image)
	int RayCount = 8;                      // nombre de faisceaux
	float RayAngle = 90.0f, RaySpread = 60.0f; // direction centrale et etalement (degres ; 360 = radial)
	float RayWidth = 4.0f, RayLength = 0.8f;   // demi-largeur (degres) et longueur (fraction de la diagonale)
	float RaySpeed = 0.2f, RayWobble = 3.0f;   // scintillement (cycles/s) et oscillation angulaire (degres)
	uint32_t RayColor = 0xFFE8A0;

	// Aurore boreale (type=aurora) : rideaux additifs qui derivent, bord inferieur ondule, couleur entre color1 et color2
	bool IsAurora = false;
	float AuroraSize = 40.0f;       // largeur d'un rideau (pixels SNES)
	float AuroraSpeed = 6.0f;       // derive horizontale (pixels SNES / s)
	float AuroraCoverage = 0.5f;    // part de la zone couverte par les rideaux (0 a 1)
	float AuroraFall = 0.7f;        // fraction de la zone sur laquelle la lumiere s'estompe vers le haut
	float AuroraWave = 6.0f;        // ondulation du bord inferieur (pixels SNES)
	uint32_t AuroraColor1 = 0x3CFF8C, AuroraColor2 = 0x8C4BFF;
	// Calques plein ecran et nuages (types plainscreen, ellipse, shadows, clouds, fog)
	bool IsPlainScreen = false, IsEllipse = false, IsNoise = false;
	int NoiseKind = 0;                  // IsNoise : 0 = ombres, 1 = nuages, 2 = brouillard
	uint32_t FxColor = 0x000000;        // couleur RGB du calque
	float FxEllipseX = 128.0f, FxEllipseY = 112.0f;   // centre de l'ellipse (pixels SNES)
	float FxRadiusX = 32.0f, FxRadiusY = 16.0f;       // rayons de l'ellipse (pixels SNES)
	float FxSize = 48.0f;               // taille des motifs de bruit (pixels SNES)
	float FxSpeed = 8.0f, FxDirection = 0.0f; // vitesse (pixels SNES / s) et direction (degres, 0 = droite, 90 = bas)
	float FxCoverage = 0.5f;            // part de l'image couverte (0 = rien, 1 = tout)
	int FxSeed = 1;
	bool FxNegative = false;            // ellipse negative : tout l'ecran est couvert sauf l'ellipse (fenetre de jeu)                   // zone touchee (fractions de l'image)

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

// Carte acceptee ? (any = toutes ; sinon liste A|B|C, ou le hash unique)
inline bool SnesHdMapMatch(bool any, const std::vector<uint32_t>& hashes, uint32_t first, uint32_t mapHash)
{
	if(any) return true;
	if(hashes.empty()) return first == mapHash;
	for(uint32_t h : hashes) { if(h == mapHash) return true; }
	return false;
}

// ---------------------------------------------------------
// <m7billboard> : panneau (PNG debout, toujours face a la camera) plante sur la carte Mode 7
// ---------------------------------------------------------
struct SnesHdM7BillboardInfo
{
	SnesHdPackBitmapInfo* Bitmap = nullptr;
	bool AnyMap = true;      // sinon : seulement sur la carte <m7img> de hash MapHash
	uint32_t MapHash = 0;
	std::vector<uint32_t> MapHashes; // A|B|C : toutes les cartes acceptees (cartes animees : un hash par etape)
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
	uint8_t J[3][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };                 // glTF : emplacements (os)
	float Wt[3][4] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };                // glTF : poids
};

// ---------------------------------------------------------
// Squelette / hierarchie animee d'un modele glTF (<m7model> .gltf / .glb)
// Matrices 4x4 en colonnes (convention glTF : m[colonne * 4 + ligne]).
// "Emplacements" (slots) : une matrice par os de skin, ou par noeud porteur d'un maillage rigide.
// ---------------------------------------------------------
struct SnesHdRigNode
{
	int Parent = -1;
	float T[3] = { 0, 0, 0 }, R[4] = { 0, 0, 0, 1 }, S[3] = { 1, 1, 1 };
	bool HasMatrix = false;
	float M[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
};
struct SnesHdRigChannel
{
	int Node = -1;
	int Path = 0;         // 0 = translation, 1 = rotation (quaternion x y z w), 2 = echelle
	bool Step = false;
	std::vector<float> Times, Values;
};
struct SnesHdRigAnim
{
	std::string Name;
	float Duration = 0;
	std::vector<SnesHdRigChannel> Channels;
};
struct SnesHdModelRig
{
	std::vector<SnesHdRigNode> Nodes;
	std::vector<int> SlotNode;                     // noeud de chaque emplacement
	std::vector<std::array<float, 16>> SlotInvBind; // matrice inverse de liaison (identite pour un noeud rigide)
	std::vector<SnesHdRigAnim> Anims;

	static void Mul(const float* a, const float* b, float* o)
	{
		float r[16];
		for(int c = 0; c < 4; c++) for(int l = 0; l < 4; l++) {
			r[c * 4 + l] = a[0 * 4 + l] * b[c * 4 + 0] + a[1 * 4 + l] * b[c * 4 + 1] + a[2 * 4 + l] * b[c * 4 + 2] + a[3 * 4 + l] * b[c * 4 + 3];
		}
		memcpy(o, r, sizeof(r));
	}
	static void Compose(const float* t, const float* q, const float* s, float* m)
	{
		float x = q[0], y = q[1], z = q[2], w = q[3];
		m[0] = (1 - 2 * (y * y + z * z)) * s[0]; m[1] = (2 * (x * y + z * w)) * s[0]; m[2] = (2 * (x * z - y * w)) * s[0]; m[3] = 0;
		m[4] = (2 * (x * y - z * w)) * s[1]; m[5] = (1 - 2 * (x * x + z * z)) * s[1]; m[6] = (2 * (y * z + x * w)) * s[1]; m[7] = 0;
		m[8] = (2 * (x * z + y * w)) * s[2]; m[9] = (2 * (y * z - x * w)) * s[2]; m[10] = (1 - 2 * (x * x + y * y)) * s[2]; m[11] = 0;
		m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
	}

	// Palette d'animation au temps donne : 12 flottants par emplacement (3 lignes x 4 colonnes).
	// anim < 0 ou absente : pose de repos.
	void Evaluate(int anim, float time, std::vector<float>& pal) const
	{
		const size_t n = Nodes.size();
		std::vector<float> trs(n * 10);
		for(size_t i = 0; i < n; i++) {
			float* d = &trs[i * 10];
			memcpy(d, Nodes[i].T, 3 * sizeof(float)); memcpy(d + 3, Nodes[i].R, 4 * sizeof(float)); memcpy(d + 7, Nodes[i].S, 3 * sizeof(float));
		}
		if(anim >= 0 && anim < (int)Anims.size()) {
			const SnesHdRigAnim& A = Anims[anim];
			float t = A.Duration > 0 ? std::fmod(time, A.Duration) : 0.0f;
			if(t < 0) t += A.Duration;
			for(const SnesHdRigChannel& ch : A.Channels) {
				if(ch.Node < 0 || ch.Node >= (int)n || ch.Times.empty()) continue;
				const int comps = ch.Path == 1 ? 4 : 3;
				const size_t keys = ch.Times.size();
				size_t k0 = 0, k1 = 0; float f = 0;
				if(t <= ch.Times[0]) { k0 = k1 = 0; }
				else if(t >= ch.Times[keys - 1]) { k0 = k1 = keys - 1; }
				else {
					size_t lo = 0, hi = keys - 1;
					while(hi - lo > 1) { size_t mid = (lo + hi) / 2; if(ch.Times[mid] <= t) lo = mid; else hi = mid; }
					k0 = lo; k1 = hi;
					float span = ch.Times[k1] - ch.Times[k0];
					f = (span > 1e-9f && !ch.Step) ? (t - ch.Times[k0]) / span : 0.0f;
				}
				if((k1 + 1) * comps > ch.Values.size()) continue;
				const float* a = &ch.Values[k0 * comps];
				const float* b = &ch.Values[k1 * comps];
				float* d = &trs[(size_t)ch.Node * 10 + (ch.Path == 0 ? 0 : (ch.Path == 1 ? 3 : 7))];
				if(ch.Path == 1) {
					float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
					float sgn = dot < 0 ? -1.0f : 1.0f; // chemin le plus court
					float q[4], len = 0;
					for(int c = 0; c < 4; c++) { q[c] = a[c] + (b[c] * sgn - a[c]) * f; len += q[c] * q[c]; }
					len = len > 1e-12f ? 1.0f / std::sqrt(len) : 1.0f;
					for(int c = 0; c < 4; c++) d[c] = q[c] * len;
				} else {
					for(int c = 0; c < 3; c++) d[c] = a[c] + (b[c] - a[c]) * f;
				}
			}
		}
		// matrices locales puis du monde (parents d'abord, par memo)
		std::vector<float> world(n * 16);
		std::vector<char> done(n, 0);
		std::function<void(int)> solve = [&](int i) {
			if(done[i]) return;
			float local[16];
			if(Nodes[i].HasMatrix) memcpy(local, Nodes[i].M, sizeof(local));
			else Compose(&trs[(size_t)i * 10], &trs[(size_t)i * 10 + 3], &trs[(size_t)i * 10 + 7], local);
			int p = Nodes[i].Parent;
			if(p >= 0 && p < (int)n) { solve(p); Mul(&world[(size_t)p * 16], local, &world[(size_t)i * 16]); }
			else memcpy(&world[(size_t)i * 16], local, sizeof(local));
			done[i] = 1;
		};
		for(size_t i = 0; i < n; i++) solve((int)i);
		pal.resize(SlotNode.size() * 12);
		for(size_t s = 0; s < SlotNode.size(); s++) {
			float m[16];
			int nd = SlotNode[s];
			if(nd >= 0 && nd < (int)n) Mul(&world[(size_t)nd * 16], SlotInvBind[s].data(), m);
			else { memcpy(m, SlotInvBind[s].data(), sizeof(m)); }
			for(int l = 0; l < 3; l++) for(int c = 0; c < 4; c++) pal[s * 12 + l * 4 + c] = m[c * 4 + l];
		}
	}
};

// Decodage d'image hors PNG (JPG, BMP, TGA... via stb_image) : textures des modeles glTF. Defini dans SnesHdGltf.cpp.
bool SnesHdDecodeImageFallback(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h);

struct SnesHdModelMesh
{
	std::vector<SnesHdModelTri> Tris;
	std::vector<SnesHdModelMaterial> Mats;
	float MinX = 0, MinY = 0, MinZ = 0, MaxX = 0, MaxY = 0, MaxZ = 0; // boite de la pose de repos
	std::shared_ptr<SnesHdModelRig> Rig; // glTF anime / avec squelette (nullptr : statique, OBJ)
};

struct SnesHdM7ModelInfo
{
	std::shared_ptr<SnesHdModelMesh> Mesh;
	std::string File;
	bool AnyMap = true;
	uint32_t MapHash = 0;
	std::vector<uint32_t> MapHashes; // A|B|C : toutes les cartes acceptees (cartes animees : un hash par etape)
	bool OnTile = false;
	uint32_t TileHash = 0;
	float X = 0, Y = 0;          // position sur la carte (texels)
	float Scale = 8.0f;          // texels de carte par unite OBJ (8 = 1 unite fait une tile)
	float Rotation = 0.0f;       // degres, autour de la verticale
	float Elevation = 0.0f;      // texels au-dessus du sol
	float Ambient = 0.45f;       // eclairage : part ambiante (0 = tres contraste, 1 = plat)
	bool KeepOrigin = false;     // false : la base du modele (centre, Y mini) est posee au sol en (x, y)
	std::vector<SnesHdPackCondition*> Conditions;

	// --- Animation glTF ---
	int Anim = 0;                // animation jouee (index ; -1 = pose de repos)
	float AnimSpeed = 1.0f;
	// --- Mouvements programmes ---
	float Spin = 0;              // degres / s autour de la verticale
	float BobAmp = 0, BobFreq = 0.5f;     // flottement vertical (texels, Hz)
	float SwayAmp = 0, SwayFreq = 0.5f;   // balancement lateral (degres, Hz)
	float TimeOffset = 0;        // decalage du temps (s) : deux copies desynchronisees
	std::vector<std::array<float, 3>> Path; // trajet (x, y, hauteur) ; avec tile=, positions relatives a la case
	float PathSpeed = 32;        // texels / s
	int PathLoop = 0;            // 0 = boucle, 1 = aller-retour, 2 = une fois
	bool PathOrient = true;      // se tourne dans le sens de la marche
	bool PathSmooth = true;      // courbe lissee (Catmull-Rom)
	std::vector<std::array<float, 3>> PathPoly; // trajet echantillonne
	std::vector<float> PathCum;  // longueurs cumulees
	float PathLen = 0;

	void BuildPath()
	{
		PathPoly.clear(); PathCum.clear(); PathLen = 0;
		const size_t n = Path.size();
		if(n < 2) return;
		const bool closed = (PathLoop == 0);
		auto P = [&](int i) -> const std::array<float, 3>& {
			int m = (int)n;
			if(closed) i = ((i % m) + m) % m; else i = std::clamp(i, 0, m - 1);
			return Path[(size_t)i];
		};
		const int segs = closed ? (int)n : (int)n - 1;
		const int sub = PathSmooth ? 12 : 1;
		for(int s = 0; s < segs; s++) {
			for(int k = 0; k < sub; k++) {
				float t = (float)k / sub;
				std::array<float, 3> q;
				if(PathSmooth) {
					const auto& p0 = P(s - 1); const auto& p1 = P(s); const auto& p2 = P(s + 1); const auto& p3 = P(s + 2);
					float t2 = t * t, t3 = t2 * t;
					for(int c = 0; c < 3; c++)
						q[c] = 0.5f * (2 * p1[c] + (-p0[c] + p2[c]) * t + (2 * p0[c] - 5 * p1[c] + 4 * p2[c] - p3[c]) * t2 + (-p0[c] + 3 * p1[c] - 3 * p2[c] + p3[c]) * t3);
				} else {
					const auto& p1 = P(s); const auto& p2 = P(s + 1);
					for(int c = 0; c < 3; c++) q[c] = p1[c] + (p2[c] - p1[c]) * t;
				}
				PathPoly.push_back(q);
			}
		}
		const std::array<float, 3> last = closed ? PathPoly.front() : Path.back(); // copie (push_back peut deplacer le tableau)
		PathPoly.push_back(last);
		PathCum.push_back(0);
		for(size_t i = 1; i < PathPoly.size(); i++) {
			float du = PathPoly[i][0] - PathPoly[i - 1][0], dv = PathPoly[i][1] - PathPoly[i - 1][1];
			PathLen += std::sqrt(du * du + dv * dv);
			PathCum.push_back(PathLen);
		}
	}
	// Position sur le trajet au temps t (s) ; du, dv = direction de la marche
	void EvalPath(float t, float& u, float& v, float& h, float& du, float& dv) const
	{
		if(PathPoly.size() < 2 || PathLen <= 0) { u = Path.empty() ? 0 : Path[0][0]; v = Path.empty() ? 0 : Path[0][1]; h = Path.empty() ? 0 : Path[0][2]; du = 0; dv = 1; return; }
		float s = PathSpeed * t, dir = 1.0f;
		if(PathLoop == 0) { s = std::fmod(s, PathLen); if(s < 0) s += PathLen; }
		else if(PathLoop == 1) { float m = std::fmod(s, 2 * PathLen); if(m < 0) m += 2 * PathLen; if(m > PathLen) { s = 2 * PathLen - m; dir = -1; } else s = m; }
		else s = std::clamp(s, 0.0f, PathLen);
		size_t lo = 0, hi = PathCum.size() - 1;
		while(hi - lo > 1) { size_t mid = (lo + hi) / 2; if(PathCum[mid] <= s) lo = mid; else hi = mid; }
		float span = PathCum[hi] - PathCum[lo];
		float f = span > 1e-6f ? (s - PathCum[lo]) / span : 0.0f;
		const auto& a = PathPoly[lo]; const auto& b = PathPoly[hi];
		u = a[0] + (b[0] - a[0]) * f; v = a[1] + (b[1] - a[1]) * f; h = a[2] + (b[2] - a[2]) * f;
		du = (b[0] - a[0]) * dir; dv = (b[1] - a[1]) * dir;
	}

	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
};

// ---------------------------------------------------------
// <m7light> : lumiere ponctuelle posee sur la carte Mode 7 (eclaire le sol et les objets poses au sol)
// <m7ambient> : luminosite / teinte generale d'une carte (nuit, chateau...) pour faire ressortir les lumieres
// ---------------------------------------------------------
struct SnesHdM7EffectInfo;
struct SnesHdM7LightInfo
{
	const SnesHdM7EffectInfo* Follow = nullptr; // lumiere automatique d'une flamme : suit ses mouvements
	bool AnyMap = true;
	uint32_t MapHash = 0;
	std::vector<uint32_t> MapHashes; // A|B|C : toutes les cartes acceptees (cartes animees : un hash par etape)
	bool OnTile = false;          // une lumiere au centre de chaque case contenant la tile TileHash
	uint32_t TileHash = 0;
	float X = 0, Y = 0;           // position sur la carte (texels)
	float Dx = 0, Dy = 0;         // tile= : decalage depuis le centre de la case (texels ; 4,4 = coin commun de 4 cases)
	float Height = 8.0f;          // hauteur de la source (texels)
	float Radius = 64.0f;         // portee (texels)
	float Intensity = 1.0f;
	uint32_t Color = 0xFFC080;
	float Flicker = 0.0f;         // 0 = stable, 1 = scintille fortement (flamme)
	float FlickerSpeed = 6.0f;
	bool Additive = false;        // false : eclaire (multiplie) ; true : ajoute de la lumiere (lueur meme sur du blanc)
	bool Shadow = false;          // mode=shadow : assombrit (ombre teintee : noir = ombre neutre)
	std::vector<SnesHdPackCondition*> Conditions;
	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
};

// <NoLightFXOnSprite> : sprites exclus de <m7light> / <m7ambient> (HUD...)
struct SnesHdNoLightRule
{
	bool AllSprites = false;       // "*" : tous les sprites
	bool OffGround = false;        // "hud" (<FrontOf3D>) : sprites dont les pieds ne sont pas sur le sol (HUD, ciel)
	uint64_t Hash = 0;             // hash de la tile HD du sprite (comme <tile>)
	bool AnyPalette = true;        // "*" : toutes ses palettes
	uint32_t Palette = 0;
	std::vector<SnesHdPackCondition*> Conditions;
	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
};

// <SpriteLight> : lumiere 3D attachee a une tile de sprite (explosion, impact, projectile...)
// Mode 7 : posee au sol sous le sprite (ligne des pieds), a la hauteur de son centre. Plus tard : scenes diorama 2D.
struct SnesHdSpriteLightInfo
{
	uint64_t Hash = 0;            // hash de la tile HD (comme <tile>)
	bool AnyPalette = true;
	uint32_t Palette = 0;
	float Height = -1.0f;         // hauteur (texels) ; < 0 = auto (centre des tiles au-dessus du sol)
	float Radius = 48.0f;
	float Intensity = 1.4f;
	uint32_t Color = 0xFFA040;
	float Flicker = 0.0f;
	float FlickerSpeed = 9.0f;
	bool Additive = false;
	bool Shadow = false;          // mode=shadow : assombrit au lieu d'eclairer
	float SelfLight = 1.0f;       // part de la lumiere recue par le sprite lui-meme (0 = eclaire autour, pas le sprite)
	float Glow = 0.0f;            // lueur propre du sprite (ajoute la couleur sur ses pixels, meme en l'air / hors eclairage)
	float Halo = 0.0f;            // halo lumineux autour du sprite (rayon en pixels SNES)
	int BlockId = -1;             // bloc <SpriteLight ...>{ } d'origine (-1 = ligne seule) : ses tiles forment un seul objet
	float AirDistance = 64.0f;    // sprite hors du sol (Lakitu...) : distance a la camera (texels) ; 0 = pas de lumiere
	bool PlaneViews = true;       // aussi dans les vues "plates" (carte vue de dessus, ecran du bas de SMK en 1 joueur)
	std::vector<SnesHdPackCondition*> Conditions;
	bool Matches(const SnesHdPackTileInfo* t) const
	{
		if(!t || t->MemoryHash != Hash) return false;
		return AnyPalette || t->PaletteHash == 0xFFFFFFFF || t->PaletteHash == Palette;
	}
	bool MatchesCondition() const
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
};

// ---------------------------------------------------------
// <m7material> : MATERIAUX 3D des pistes (cartes PBR standard : normal, rugosite, metal, AO, emissive, couleur)
// Une carte de materiau = image + chaine de mipmaps ; echantillonnage bilineaire en repetition.
struct SnesHdMaterialMap
{
	std::vector<std::vector<uint32_t>> Mips; // niveau 0 = image d'origine (ARGB)
	std::vector<uint32_t> W, H;
	bool Valid() const { return !Mips.empty() && W[0] > 0 && H[0] > 0; }
	void Set(std::vector<uint32_t>&& px, uint32_t w, uint32_t h)
	{
		Mips.clear(); W.clear(); H.clear();
		if(w == 0 || h == 0 || px.size() < (size_t)w * h) return;
		Mips.push_back(std::move(px)); W.push_back(w); H.push_back(h);
		while((W.back() > 1 || H.back() > 1) && Mips.size() < 14) {
			const uint32_t pw = W.back(), ph = H.back(), nw = std::max(1u, pw / 2), nh = std::max(1u, ph / 2);
			std::vector<uint32_t> n((size_t)nw * nh);
			const std::vector<uint32_t>& p = Mips.back();
			for(uint32_t y = 0; y < nh; y++) for(uint32_t x = 0; x < nw; x++) {
				uint32_t acc[4] = { 0, 0, 0, 0 };
				for(int k = 0; k < 4; k++) {
					const uint32_t c = p[(size_t)std::min(ph - 1, y * 2 + (k >> 1)) * pw + std::min(pw - 1, x * 2 + (k & 1))];
					acc[0] += c >> 24; acc[1] += (c >> 16) & 0xFF; acc[2] += (c >> 8) & 0xFF; acc[3] += c & 0xFF;
				}
				n[(size_t)y * nw + x] = ((acc[0] / 4) << 24) | ((acc[1] / 4) << 16) | ((acc[2] / 4) << 8) | (acc[3] / 4);
			}
			Mips.push_back(std::move(n)); W.push_back(nw); H.push_back(nh);
		}
	}
	// fu, fv : coordonnees en repetitions (1.0 = une image) ; foot = pixels de l'image par pixel HD
	// Reduction (foot >= 1, cas courant sur la piste) : 1 lecture au bon niveau de mipmap ; agrandissement : bilineaire.
	void Sample(float fu, float fv, float foot, float out[4]) const
	{
		int lv = 0;
		if(foot > 1.0f) { int e; std::frexp(foot, &e); lv = e - 1; }
		lv = std::min(lv, (int)Mips.size() - 1);
		const uint32_t w = W[lv], h = H[lv];
		const std::vector<uint32_t>& p = Mips[lv];
		if(foot >= 1.0f) {
			int ix = (int)std::floor(fu * (float)w), iy = (int)std::floor(fv * (float)h);
			if((w & (w - 1)) == 0) ix &= (int)w - 1; else { ix %= (int)w; if(ix < 0) ix += w; }
			if((h & (h - 1)) == 0) iy &= (int)h - 1; else { iy %= (int)h; if(iy < 0) iy += h; }
			const uint32_t c = p[(size_t)iy * w + ix];
			out[0] = (float)(c >> 24) * (1.0f / 255.0f); out[1] = (float)((c >> 16) & 0xFF) * (1.0f / 255.0f);
			out[2] = (float)((c >> 8) & 0xFF) * (1.0f / 255.0f); out[3] = (float)(c & 0xFF) * (1.0f / 255.0f);
			return;
		}
		float x = fu * (float)w - 0.5f, y = fv * (float)h - 0.5f;
		const float fx0 = std::floor(x), fy0 = std::floor(y);
		const float tx = x - fx0, ty = y - fy0;
		int ix = (int)fx0 % (int)w, iy = (int)fy0 % (int)h;
		if(ix < 0) ix += w;
		if(iy < 0) iy += h;
		const int ix1 = (ix + 1) % (int)w, iy1 = (iy + 1) % (int)h;
		const uint32_t c00 = p[(size_t)iy * w + ix], c10 = p[(size_t)iy * w + ix1], c01 = p[(size_t)iy1 * w + ix], c11 = p[(size_t)iy1 * w + ix1];
		for(int k = 0; k < 4; k++) {
			const int sh = 24 - k * 8; // a, r, g, b
			const float a = (float)((c00 >> sh) & 0xFF), b = (float)((c10 >> sh) & 0xFF), c = (float)((c01 >> sh) & 0xFF), d = (float)((c11 >> sh) & 0xFF);
			out[k] = (a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty) * (1.0f / 255.0f);
		}
	}
};

struct SnesHdM7MaterialInfo
{
	bool OnImg = false;              // true : toute la grosse texture <m7img> de la carte ; false : une tile <m7tiles>
	uint32_t TileHash = 0;
	bool AnyMap = true;
	uint32_t MapHash = 0;
	std::vector<uint32_t> MapHashes;
	SnesHdMaterialMap Albedo, Normal, Rough, Metal, Ao, Emissive, Orm; // Orm : R = AO, G = rugosite, B = metal
	SnesHdMaterialMap Packed;        // regroupee au chargement : A = AO, R/G = normale (X, Y), B = rugosite -> 1 lecture par pixel
	bool HasNormal = false;
	bool GlossInverted = false;      // carte "glossiness" (inverse de la rugosite)
	bool NormalDX = false;           // convention DirectX (vert inverse)
	float Scale = 0;                 // texels de carte par repetition (0 = auto : la tile, ou 64 pour la grosse texture)
	int ColorMode = 0;               // 0 = couleur de la tile / texture du pack, 1 = couleur du materiau, 2 = melange
	float Mix = 0.5f;
	float Strength = 1.0f;           // force du relief (normal map)
	float AoStrength = 1.0f;
	float Specular = 1.0f;           // force des reflets brillants
	float EmissiveStrength = 1.0f;
	float RoughValue = 0.75f;        // rugosite si pas de carte
	float MetalValue = 0.0f;         // metal si pas de carte
	int Debug = 0;                   // debug=normal (1) : couleurs des normales ; debug=light (2) : eclairage seul
	std::vector<SnesHdPackCondition*> Conditions;
	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
};

struct SnesHdM7AmbientInfo
{
	bool AnyMap = true;
	uint32_t MapHash = 0;
	std::vector<uint32_t> MapHashes; // A|B|C : toutes les cartes acceptees (cartes animees : un hash par etape)
	float Brightness = 1.0f;      // 1 = normal, 0.4 = nuit
	uint32_t Tint = 0xFFFFFF;
	bool Sky = true;              // assombrir aussi le ciel au-dessus de la piste
	bool SunSet = false;          // soleil des materiaux 3D regle par cette ambiance
	float SunAz = 135, SunEl = 40, SunIntensity = 0.5f; // direction (degres, comme flare sunaz / sunel), force
	uint32_t SunColor = 0xFFF6E8;
	std::vector<SnesHdPackCondition*> Conditions;
	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
};

// ---------------------------------------------------------
// <m7LiquidBackground> : liquide (eau, lave...) calcule dans le monde Mode 7, visible a travers
// les pixels transparents / semi-transparents de la texture <m7img> (et des <m7tiles> posees dessus)
// ---------------------------------------------------------
struct SnesHdM7LiquidInfo
{
	bool AnyMap = true;
	uint32_t MapHash = 0;
	std::vector<uint32_t> MapHashes; // A|B|C : toutes les cartes acceptees (cartes animees : un hash par etape)
	bool Lava = false;                 // style=lava (sinon water)
	uint32_t Color = 0x2A7FC0;         // couleur principale (eau peu profonde / lave)
	uint32_t Deep = 0x0C3A70;          // couleur des creux
	uint32_t Sky = 0xA8D8FF;           // eau : reflet du ciel ; lave : couleur des points chauds
	float Speed = 10.0f;               // texels / s
	float Scale = 48.0f;               // longueur des vagues (texels)
	float Waves = 0.5f;                // force du relief des vagues (0..1)
	float Specular = 0.8f;             // reflets du soleil (eau)
	float Reflect = 0.65f;             // reflet du ciel maxi (eau, angle rasant)
	float Foam = 0.6f;                 // ecume dans les zones semi-transparentes du rivage
	float ObjReflect = 0.55f;          // reflet des objets (karts, panneaux, modeles, relief) ; lave : 0.15
	float ReflectFade = 40.0f;         // longueur du reflet sous les pieds avant de disparaitre (pixels SNES)
	float Ripple = 1.0f;               // ondulation horizontale du reflet (pixels SNES)
	int ReflectViews = 0;              // vues avec reflets : 0 = toutes ; bit 0 = vue du haut (1), bit 1 = vue du bas (2)
	SnesHdPackBitmapInfo* Texture = nullptr; // detail optionnel (PNG qui se repete)
	float TexScale = 32.0f;            // taille d'une repetition de la texture (texels)
	std::vector<SnesHdPackCondition*> Conditions;

	std::vector<std::vector<uint32_t>> Mips; std::vector<uint32_t> MipW, MipH; bool Built = false;
	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
	void Build()
	{
		if(Built) return;
		Built = true;
		if(!Texture) return;
		if(Texture->PixelData.empty()) Texture->Init();
		if(Texture->PixelData.empty() || !Texture->Width || !Texture->Height) return;
		Mips.push_back(Texture->PixelData); MipW.push_back(Texture->Width); MipH.push_back(Texture->Height);
		while(MipW.back() > 1 || MipH.back() > 1) {
			uint32_t pw = MipW.back(), ph = MipH.back(), nw = std::max<uint32_t>(1, pw / 2), nh = std::max<uint32_t>(1, ph / 2);
			const std::vector<uint32_t>& prev = Mips.back();
			std::vector<uint32_t> m((size_t)nw * nh, 0);
			for(uint32_t y = 0; y < nh; y++) for(uint32_t x = 0; x < nw; x++) {
				uint32_t r = 0, g = 0, b = 0, a = 0;
				for(uint32_t dy = 0; dy < 2; dy++) for(uint32_t dx = 0; dx < 2; dx++) {
					uint32_t q = prev[(size_t)std::min(ph - 1, y * 2 + dy) * pw + std::min(pw - 1, x * 2 + dx)];
					a += q >> 24; r += (q >> 16) & 0xFF; g += (q >> 8) & 0xFF; b += q & 0xFF;
				}
				m[(size_t)y * nw + x] = ((a / 4) << 24) | ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
			}
			Mips.push_back(std::move(m)); MipW.push_back(nw); MipH.push_back(nh);
		}
	}
};

// ---------------------------------------------------------
// <m7Effect> : effets ancres sur la carte Mode 7 (etape 1 : type=flame, type=smoke)
// Emetteur PONCTUEL de particules 3D (positions relatives a l'emetteur, en texels de carte).
// ---------------------------------------------------------
struct SnesHdFxParticle
{
	float Du = 0, Dv = 0, H = 0;      // position relative a l'emetteur
	float Vu = 0, Vv = 0, Vh = 0;     // vitesse (texels / s)
	float Age = 0, Life = 1, Size = 1, Seed = 0;
};

struct SnesHdM7EffectInfo
{
	int Type = 0;                       // 0 = flame, 1 = smoke, 2 = fog, 3 = magicfog, 4 = flare, 5 = clouds
	bool AnyMap = true;
	uint32_t MapHash = 0;
	std::vector<uint32_t> MapHashes;
	bool OnTile = false;                // un emetteur au centre de chaque case contenant la tile
	uint32_t TileHash = 0;
	float X = 0, Y = 0, Elevation = 0;
	float Dx = 0, Dy = 0;               // tile= : decalage depuis le centre de la case (texels)
	std::vector<SnesHdPackCondition*> Conditions;

	int ZIndex = 95;
	int MaxParticles = 80;
	float Rate = 40;                    // naissances / s
	float LifeMin = 0.5f, LifeMax = 0.9f;
	float Radius = 3;                   // rayon de la zone d'apparition (texels)
	float RiseMin = 18, RiseMax = 30;   // vitesse de montee (texels / s)
	float Drift = 4;                    // vitesse laterale aleatoire
	float Turbulence = 6, TurbulenceFreq = 2;
	float WindX = 0, WindY = 0;
	float SizeStart = 5, SizeEnd = 1.5f, SizeRandom = 0.3f; // taille dans le monde (texels)
	float AlphaStart = 1, AlphaEnd = 0, FadeIn = 0.1f;
	uint32_t Color0 = 0xFFF2B0, Color1 = 0xFF9020, Color2 = 0xC02000; // debut / milieu / fin de vie
	bool Additive = true;
	float MinPixelSize = 1.5f, NearFade = 12;
	SnesHdPackBitmapInfo* Texture = nullptr;

	// --- mouvement (memes cles que <m7model> : path, speed, loop, smooth, bobamp, bobfreq, offset) ---
	SnesHdM7ModelInfo Motion;
	bool HasMotion = false;

	// --- fog / magicfog / clouds (passe "atmosphere" sur l'image finale) ---
	float Density = 0.6f;               // fog : brouillard de distance maxi
	float FogStart = 200, FogEnd = 900; // fog : distances (texels) debut / plein
	float Patches = 0.5f;               // nappes de bruit qui derivent (0 = aucune)
	float NoiseScale = 96;              // taille des nappes / nuages (texels)
	float Region = 0;                   // rayon de la zone (texels) autour de x, y ; 0 = toute la carte
	float SkyAmount = 0.5f;             // fog : part du brouillard sur le ciel ; clouds : 1 = nuages dans le ciel
	float Intensity = 0.6f;             // magicfog : force de la lueur
	float Pulse = 0.4f;                 // magicfog : pulsation (Hz)
	float Coverage = 0.5f;              // clouds : part du ciel couverte
	float Shadow = 0.35f;               // clouds : assombrissement des ombres sur la piste
	float SkyHeight = 0.85f;            // clouds : hauteur de la bande de nuages (part du ciel)
	// --- flare (reflets de lentille) ---
	bool Sun = false;                   // source a l'infini (soleil) : SunAz / SunEl
	float SunAz = 0, SunEl = 25;        // degres : 0 = vers +x de la carte, 90 = vers +y ; hauteur au-dessus de l'horizon
	float FlareSize = 24;               // rayon du halo (pixels SNES)
	float Ghosts = 1.0f;                // force des reflets secondaires
	float Streak = 0.6f;                // trainee horizontale
	std::unordered_map<int, float> FlareVis; // visibilite lissee (occultation progressive)

	struct Emitter
	{
		std::vector<SnesHdFxParticle> Parts;
		float Acc = 0, Time = 0;
		bool Initialized = false;
		uint32_t LastFrame = 0xFFFFFFFF;
		uint32_t Rng = 0x2545F491;
		float Rand() { Rng ^= Rng << 13; Rng ^= Rng >> 17; Rng ^= Rng << 5; return (float)(Rng & 0xFFFFFF) / 16777216.0f; }
	};
	std::unordered_map<int, Emitter> Emitters; // cle : -1 = emetteur unique, sinon numero de case (tile=)

	bool MatchesCondition()
	{
		for(auto* c : Conditions) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}

	void Step(Emitter& e, float dt, float spawnU = 0, float spawnV = 0, float spawnH = 0)
	{
		e.Time += dt;
		e.Acc += Rate * dt;
		while(e.Acc >= 1.0f) {
			e.Acc -= 1.0f;
			if((int)e.Parts.size() >= MaxParticles) continue;
			SnesHdFxParticle p;
			float a = e.Rand() * 6.2831853f, r = Radius * std::sqrt(e.Rand());
			p.Du = spawnU + std::cos(a) * r; p.Dv = spawnV + std::sin(a) * r; p.H = spawnH;
			float da = e.Rand() * 6.2831853f, dr = Drift * e.Rand();
			p.Vu = std::cos(da) * dr; p.Vv = std::sin(da) * dr;
			p.Vh = RiseMin + (RiseMax - RiseMin) * e.Rand();
			p.Life = std::max(0.05f, LifeMin + (LifeMax - LifeMin) * e.Rand());
			p.Size = 1.0f + SizeRandom * (e.Rand() * 2.0f - 1.0f);
			p.Seed = e.Rand() * 6.2831853f;
			e.Parts.push_back(p);
		}
		const float tw = 6.2831853f * TurbulenceFreq;
		for(size_t k = 0; k < e.Parts.size();) {
			SnesHdFxParticle& p = e.Parts[k];
			p.Age += dt;
			if(p.Age >= p.Life) { p = e.Parts.back(); e.Parts.pop_back(); continue; }
			float tu = 0, tv = 0;
			if(Turbulence != 0) { tu = Turbulence * std::sin(tw * e.Time + p.Seed); tv = Turbulence * std::cos(tw * 0.77f * e.Time + p.Seed * 1.3f); }
			p.Du += (p.Vu + WindX + tu) * dt;
			p.Dv += (p.Vv + WindY + tv) * dt;
			p.H += p.Vh * dt;
			k++;
		}
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
	// <widescreen> : 0 off, 1 mode7, 2 all (bandes 16:9 ; 2 traite comme 1 en v1)
	uint8_t WidescreenMode = 0;
	// Extension 16:9 courante (colonnes SNES de chaque cote), ecrite par le filtre a chaque frame
	uint32_t WsExtPx = 0;
	// Sprites en format large : 0 = safe (visibles si partiellement dans la zone SNES), 1 = unsafe (aussi dans la bande seule)
	uint8_t WsObjUnsafe = 0;

	// Tirage Randomize/random unique par pack : dossier parent (minuscules) -> sous-dossier tire
	std::map<std::string, std::string> RandomFolderChoices;
	std::vector<std::unique_ptr<SnesHdPackBitmapInfo>> ImageFileData;
	std::unordered_map<SnesHdTileKey, std::vector<SnesHdPackTileInfo*>> TileByKey;
	// <tile> ... defaultPalette=Y : tuiles utilisees pour toute palette sans tuile explicite (PaletteHash = 0, sentinelle)
	std::unordered_map<SnesHdTileKey, std::vector<SnesHdPackTileInfo*>> DefaultPaletteTileByKey;

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

	// Nappes <2DLiquid> (phase 2 SNES)
	std::vector<std::unique_ptr<SnesHdLiquidInfo>> Liquids;

	// Effets <3DParticles> (particules dans le monde Mode 7)
	std::vector<std::unique_ptr<SnesHdParticles3DInfo>> Particles3D;

	// <m7relief> : volume des tiles Mode 7, par hash de tile
	std::unordered_map<uint32_t, std::vector<std::unique_ptr<SnesHdM7ReliefInfo>>> M7Reliefs;

	// <m7billboard> : panneaux plantes sur la carte Mode 7
	std::vector<std::unique_ptr<SnesHdM7BillboardInfo>> M7Billboards;

	// <m7model> : modeles 3D .obj poses sur la carte Mode 7
	std::vector<std::unique_ptr<SnesHdM7ModelInfo>> M7Models;
	// <m7light> / <m7ambient> : eclairage de la piste Mode 7
	std::vector<std::unique_ptr<SnesHdM7LightInfo>> M7Lights;
	std::vector<std::unique_ptr<SnesHdM7AmbientInfo>> M7Ambients;
	std::vector<std::unique_ptr<SnesHdNoLightRule>> NoLightRules; // <NoLightFXOnSprite>
	std::vector<std::unique_ptr<SnesHdSpriteLightInfo>> SpriteLights; // <SpriteLight>
	std::vector<std::unique_ptr<SnesHdNoLightRule>> NoReflectRules; // <NoLiquidReflect> : tiles sans reflet dans l'eau / la lave
	std::vector<std::unique_ptr<SnesHdNoLightRule>> FrontOf3DRules; // <FrontOf3D> : devant relief / panneaux / modeles / effets
	std::vector<std::unique_ptr<SnesHdNoLightRule>> Ignore2DOverlaysRules; // <Ignore2DOverlays> : tiles nettes pendant la distorsion <2DOverlay> (HUD)
	bool HasIgnore2DResources = false; // une ressource (background, m7img, m7tiles) est dans un bloc <Ignore2DOverlays>
	std::vector<std::unique_ptr<SnesHdM7MaterialInfo>> M7Materials; // <m7material>
	std::vector<std::unique_ptr<SnesHdM7LiquidInfo>> M7Liquids;   // <m7LiquidBackground>
	std::vector<std::unique_ptr<SnesHdM7EffectInfo>> M7Effects;   // <m7Effect>

	// <gpu3d>on|off : moteur 3D sur la carte graphique (defaut : actif si disponible)
	bool Gpu3D = true;

	// <m7CameraDebug> : reperes de controle de la camera Mode 7 reconstruite
	bool M7CameraDebug = false;
	std::vector<SnesHdPackCondition*> M7CameraDebugConditions;

	// MPEG1 videos
	std::vector<std::unique_ptr<SnesHdVideoInfo>> Videos;


	HdLoadingScreenConfig LoadingScreen; // balise <LoadingScreen>
	std::unordered_map<std::string, std::string> PatchesByHash;
	

	/*------------------------------------------------------------------------------------*/

	void LoadAsync()
	{
		// Ecran de chargement : nombre de fichiers = PNG + OGG distincts
		uint32_t loadingTotal = (uint32_t)ImageFileData.size();
		{
			std::unordered_set<std::string> audioNames;
			for(auto& pair : MsuTracks) for(auto& track : pair.second) audioNames.insert(track.Filename);
			loadingTotal += (uint32_t)audioNames.size();
		}
		HdLoadingScreen::Guard loadingScreen(LoadingScreen, loadingTotal);
		for(auto& bitmap : ImageFileData) {
			bitmap->Init();
			HdLoadingScreen::Progress(bitmap->PngName);
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
				HdLoadingScreen::Progress(track.Filename);
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