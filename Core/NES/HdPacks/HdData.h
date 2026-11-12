#pragma once
#include "pch.h"
#include <atomic>
#include <thread>
#include "NES/NesConstants.h"
#include "Shared/MessageManager.h"
#include "Shared/HdLoadingScreen.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/SimpleLock.h"
#include "Utilities/Timer.h"
#include "Utilities/VirtualFile.h"
#include "Shared/pl_mpeg.h"

class BaseHdNesPack;



struct HdTileKey
{
	static constexpr int32_t NoTile = -1;

	//Code depends on these 2 fields being one after the other
	uint32_t PaletteColors = 0;
	uint8_t TileData[16] = {};

	int32_t TileIndex = 0;
	bool IsChrRamTile = false;

	HdTileKey GetKey(bool defaultKey)
	{
		if(defaultKey) {
			HdTileKey copy = *this;
			copy.PaletteColors = 0xFFFFFFFF;
			return copy;
		} else {
			return *this;
		}
	}

	uint32_t GetHashCode() const
	{
		if(IsChrRamTile) {
			return CalculateHash((uint8_t*)&PaletteColors, sizeof(PaletteColors) + sizeof(TileData));
		} else {
			return (uint32_t)TileIndex ^ PaletteColors;
		}
	}

	size_t operator() (const HdTileKey &tile) const {
		return tile.GetHashCode();
	}

	bool operator==(const HdTileKey &other) const
	{
		if(IsChrRamTile) {
			return memcmp((uint8_t*)&PaletteColors, (uint8_t*)&other.PaletteColors, sizeof(PaletteColors) + sizeof(TileData)) == 0;
		} else {
			return TileIndex == other.TileIndex && PaletteColors == other.PaletteColors;
		}
	}

	uint32_t CalculateHash(const uint8_t* key, size_t len) const
	{
		uint32_t result = 0;
		for(size_t i = 0; i < len; i += 4) {
			uint32_t chunk;
			memcpy(&chunk, key, sizeof(uint32_t));

			result += chunk;
			result = (result << 2) | (result >> 30);
			key += 4;
		}
		return result;
	}

	bool IsSpriteTile()
	{
		return (PaletteColors & 0xFF000000) == 0xFF000000;
	}
};

namespace std {
	template <> struct hash<HdTileKey>
	{
		size_t operator()(const HdTileKey& x) const
		{
			return x.GetHashCode();
		}
	};
}

struct HdPpuTileInfo : public HdTileKey
{
	uint8_t OffsetX = 0;
	uint8_t OffsetY = 0;
	bool HorizontalMirroring = false;
	bool VerticalMirroring = false;
	bool BackgroundPriority = false;
	
	uint8_t BgColorIndex = 0;
	uint8_t SpriteColorIndex = 0;
	uint8_t BgColor = 0;
	uint8_t SpriteColor = 0;
	uint8_t PpuBackgroundColor = 0;
	uint8_t PaletteOffset = 0;
};

struct HdPpuPixelInfo
{
	HdPpuTileInfo Tile = {};
	HdPpuTileInfo Sprite[4] = {};
	
	uint16_t TmpVideoRamAddr = 0;
	uint8_t XScroll = 0;
	uint8_t EmphasisBits = 0;
	bool Grayscale = false;
	uint8_t SpriteCount = 0;
};

struct HdScreenInfo
{
	HdPpuPixelInfo* ScreenTiles;
	unordered_map<uint32_t, uint8_t> WatchedAddressValues;
	uint32_t FrameNumber = 0;

	HdScreenInfo(const HdScreenInfo& that) = delete;

	HdScreenInfo(bool isChrRamGame)
	{
		ScreenTiles = new HdPpuPixelInfo[NesConstants::ScreenPixelCount];

		for(int i = 0; i < NesConstants::ScreenPixelCount; i++) {
			ScreenTiles[i].Tile.BackgroundPriority = false;
			ScreenTiles[i].Tile.IsChrRamTile = isChrRamGame;
			ScreenTiles[i].Tile.HorizontalMirroring = false;
			ScreenTiles[i].Tile.VerticalMirroring = false;

			for(int j = 0; j < 4; j++) {
				ScreenTiles[i].Sprite[j].IsChrRamTile = isChrRamGame;
			}
		}
	}

	~HdScreenInfo()
	{
		delete[] ScreenTiles;
	}
};

enum class HdPackConditionType
{
	HMirror,
	VMirror,
	BgPriority,
	FrameRange,
	MemoryCheck,
	MemoryCheckConstant,
	TileNearby,
	TileAtPos,
	SpriteAtPos,
	SpriteNearby,
	SpritePalette,
	PositionCheckX,
	PositionCheckY,
	OriginPositionCheckX,
	OriginPositionCheckY,
};

struct HdPackCondition
{
protected:
	HdScreenInfo* _screenInfo = nullptr;
	BaseHdNesPack* _hdPack = nullptr;

public:
	string Name;

	virtual HdPackConditionType GetConditionType() = 0;
	virtual string GetConditionName() = 0;
	virtual bool IsExcludedFromFile() { return Name.size() > 0 && Name[0] == '!'; }
	virtual string ToString() = 0;

	virtual ~HdPackCondition() { }

	void Initialize(HdScreenInfo* screenInfo, BaseHdNesPack* hdPack)
	{
		_screenInfo = screenInfo;
		_hdPack = hdPack;
		_resultCache = -1;
	}

	bool CheckCondition(int x, int y, HdPpuTileInfo* tile)
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

protected:
	int8_t _resultCache = -1;
	bool _useCache = false;

	virtual bool InternalCheckCondition(int x, int y, HdPpuTileInfo* tile) = 0;
};

struct HdPackBitmapInfo
{
private:
	bool _initDone = false;
	SimpleLock _lock;

public:
	string PngName;
	vector<uint8_t> FileData;
	vector<uint32_t> PixelData;
	uint32_t Width = 0;
	uint32_t Height = 0;

	void Init()
	{
		if(_initDone) {
			return;
		}

		auto lock = _lock.AcquireSafe();
		if(_initDone) {
			return;
		}

		//Timer tmr;
		if(PNGHelper::ReadPNG(FileData, PixelData, Width, Height)) {
			//MessageManager::Log("[HDPack] PNG file loaded: " + PngName + " (" + std::to_string(tmr.GetElapsedMS()) + ")");
			PremultiplyAlpha();
		} else {
			MessageManager::Log("[HDPack] PNG file " + PngName + " is invalid.");
		}
		FileData = {};
		_initDone = true;
	}

	void PremultiplyAlpha()
	{
		for(size_t i = 0; i < PixelData.size(); i++) {
			if(PixelData[i] < 0xFF000000) {
				uint8_t* output = (uint8_t*)(PixelData.data() + i);
				uint8_t alpha = output[3] + 1;
				output[0] = (uint8_t)((alpha * output[0]) >> 8);
				output[1] = (uint8_t)((alpha * output[1]) >> 8);
				output[2] = (uint8_t)((alpha * output[2]) >> 8);
			}
		}
	}
};

struct HdPackTileInfo : public HdTileKey
{
private:
	bool _needInit = true;

public:
	uint32_t X;
	uint32_t Y;
	uint32_t Width;
	uint32_t Height;
	uint32_t BitmapIndex;
	HdPackBitmapInfo* Bitmap;
	int Brightness;
	bool DefaultTile;
	bool Blank;
	bool IsDisabled = false; // <tile>blank / disable : tuile masquee (ni SD ni HD)
	bool HasTransparentPixels;
	bool TransparencyRequired;
	bool IsFullyTransparent;
	vector<uint32_t> HdTileData;
	uint32_t ChrBankId;

	vector<HdPackCondition*> Conditions;
	bool ForceDisableCache;

	// Variante miroir (<tile_hflip>, <tile_vflip>, <tile_hvflip>) : l'image du pack est deja retournee, pas de miroir a l'affichage
	bool IsHFlip = false;
	bool IsVFlip = false;

	// <Ignore2DOverlays> : ensembles de conditions (un par bloc / ligne) ; la tile reste nette pendant la distorsion si l'un est vrai
	vector<vector<HdPackCondition*>> Ignore2DSets;
	bool IsIgnored2D(int x, int y, HdPpuTileInfo* tile)
	{
		for(auto& set : Ignore2DSets) {
			bool ok = true;
			for(HdPackCondition* c : set) {
				if(!c->CheckCondition(x, y, tile)) { ok = false; break; }
			}
			if(ok) return true;
		}
		return false;
	}

	bool MatchesCondition(int x, int y, HdPpuTileInfo* tile)
	{
		for(HdPackCondition* condition : Conditions) {
			if(!condition->CheckCondition(x, y, tile)) {
				return false;
			}
		}
		return true;
	}

	vector<uint32_t> ToRgb(uint32_t* palette)
	{
		vector<uint32_t> rgbBuffer;
		for(uint8_t i = 0; i < 8; i++) {
			uint8_t lowByte = TileData[i];
			uint8_t highByte = TileData[i + 8];
			for(uint8_t j = 0; j < 8; j++) {
				uint8_t color = ((lowByte >> (7 - j)) & 0x01) | (((highByte >> (7 - j)) & 0x01) << 1);
				uint32_t rgbColor;
				if(IsSpriteTile() || TransparencyRequired) {
					rgbColor = color == 0 ? 0x00FFFFFF : palette[(PaletteColors >> ((3 - color) * 8)) & 0x3F];
				} else {
					rgbColor = palette[(PaletteColors >> ((3 - color) * 8)) & 0x3F];
				}
				rgbBuffer.push_back(rgbColor);
			}
		}

		return rgbBuffer;
	}

	// <tile>blank / disable : tuile sans image, qui ne dessine rien (ni SD ni HD)
	void MarkDisabled()
	{
		IsDisabled = true;
		Bitmap = nullptr;
		_needInit = false;
		Blank = true;
		HasTransparentPixels = false;
		TransparencyRequired = false;
		IsFullyTransparent = true;
	}

	void UpdateFlags()
	{
		Blank = true;
		HasTransparentPixels = false;
		IsFullyTransparent = true;
		for(size_t i = 0; i < HdTileData.size(); i++) {
			if(HdTileData[i] != HdTileData[0]) {
				Blank = false;
			}
			if((HdTileData[i] & 0xFF000000) != 0xFF000000) {
				HasTransparentPixels = true;
			}
			if(HdTileData[i] & 0xFF000000) {
				IsFullyTransparent = false;
			}
		}
	}

	__forceinline bool NeedInit()
	{
		return _needInit;
	}

	__noinline void Init()
	{
		_needInit = false;
		Bitmap->Init();

		uint32_t bitmapOffset = Y * Bitmap->Width + X;
		uint32_t* pngData = Bitmap->PixelData.data();

		HdTileData.resize(Width * Height);
		if(Bitmap->PixelData.size() >= bitmapOffset + ((Height - 1) * Bitmap->Width) + Width) {
			for(uint32_t y = 0; y < Height; y++) {
				memcpy(HdTileData.data() + (y * Width), pngData + bitmapOffset, Width * sizeof(uint32_t));
				bitmapOffset += Bitmap->Width;
			}
		}

		UpdateFlags();
	}

	string ToString(int pngIndex)
	{
		stringstream out;

		if(Conditions.size() > 0) {
			out << "[";
			for(size_t i = 0; i < Conditions.size(); i++) {
				if(i > 0) {
					out << "&";
				}
				out << Conditions[i]->Name;
			}
			out << "]";
		}

		if(IsChrRamTile) {
			out << "<tile>" << pngIndex << ",";

			for(int i = 0; i < 16; i++) {
				out << HexUtilities::ToHex(TileData[i]);
			}
			out << "," <<
				HexUtilities::ToHex(PaletteColors, true) << "," <<
				X << "," <<
				Y << "," <<
				(double)Brightness / 255 << "," <<
				(DefaultTile ? "Y" : "N") << "," <<
				ChrBankId << "," <<
				TileIndex;
		} else {
			out << "<tile>" <<
				pngIndex << "," <<
				HexUtilities::ToHex(TileIndex) << "," <<
				HexUtilities::ToHex(PaletteColors, true) << "," <<
				X << "," <<
				Y << "," <<
				(double)Brightness / 255 << "," <<
				(DefaultTile ? "Y" : "N");
		}

		return out.str();
	}
};

enum class HdPackBlendMode
{
	Alpha,
	Add,
	Subtract
};

struct HdBackgroundInfo
{
	HdPackBitmapInfo* Data;
	int Brightness;
	vector<HdPackCondition*> Conditions;
	float HorizontalScrollRatio;
	// <Ignore2DOverlays> : voir HdPackTileInfo::Ignore2DSets
	vector<vector<HdPackCondition*>> Ignore2DSets;
	bool IsIgnored2D(int x, int y, HdPpuTileInfo* tile)
	{
		for(auto& set : Ignore2DSets) {
			bool ok = true;
			for(HdPackCondition* c : set) {
				if(!c->CheckCondition(x, y, tile)) { ok = false; break; }
			}
			if(ok) return true;
		}
		return false;
	}
	float VerticalScrollRatio;
	uint8_t Priority;

	uint32_t Left;
	uint32_t Top;

	HdPackBlendMode BlendMode;

	// <background> etendu (NES) : repetition, animation par dossier, operateur de defilement
	bool TileX = false;
	bool TileY = false;
	int FramesBetweenSteps = 0;
	bool ReverseAnim = false;
	vector<HdPackBitmapInfo*> AnimFrames;
	enum class ScrollOperator { Multiply, Divide, Plus, Minus };
	ScrollOperator Operator = ScrollOperator::Multiply;

	uint32_t* data()
	{
		return Data->PixelData.data();
	}

	string ToString()
	{
		stringstream out;

		if(Conditions.size() > 0) {
			out << "[";
			for(size_t i = 0; i < Conditions.size(); i++) {
				if(i > 0) {
					out << "&";
				}
				out << Conditions[i]->Name;
			}
			out << "]";
		}
		
		out << "<background>";
		out << Data->PngName << ",";
		out << (Brightness / 255.0);

		return out.str();
	}
};

struct HdPackAdditionalSpriteInfo
{
	HdTileKey OriginalTile;
	HdTileKey AdditionalTile;

	int32_t OffsetX;
	int32_t OffsetY;
	bool IgnorePalette;
};

struct FallbackTileInfo
{
	int32_t TileIndex;
	int32_t FallbackTileIndex;
};

struct BgmTrackInfo
{
	string Filename;
	uint32_t LoopPosition = 0;
};

/*-----------------------------------------------------------*/
/* added by Gwenolo to manage additional musics and SFX from OGG */ 
struct PlayAdditionalBgmAction
{
	vector<HdPackCondition*> Conditions;
	int TrackId;
	bool loop = true;
};

struct PlayAdditionalSfxAction
{
	vector<HdPackCondition*> Conditions;
	int SfxId;
	//string filename;
};
/*-----------------------------------------------------------*/


// Video <video> : lecture MPEG-1 (pl_mpeg) superposee a l'image HD NES.
// Z-index : meme echelle que la priorite des <background> (0 a 39). 99 par defaut = au-dessus de tout.
// Le rendu NES n'a pas de z-buffer : HdNesPack garde, par pixel NES, le z du dernier element dessine.
struct NesHdVideoInfo
{
	string Filename;
	bool PauseMode = false;        // lu, pas encore utilise (comme cote SNES)
	uint16_t SkipButtonsMask = 0;  // lu, pas encore utilise (comme cote SNES)
	bool PlayOnlyOnce = false;
	bool MuteAudio = false;
	bool AlphaEnable = true;       // true : le noir pur (#000000) est transparent
	bool Stretch = false;          // true : etire sur toute la zone visible
	int LoopTargetFrame = 0;       // 0 = pas de boucle, -1 = boucle depuis le debut, > 0 = frame de redemarrage
	int X = 0;
	int Y = 0;
	uint8_t ZIndex = 99;

	bool HasPlayed = false;
	bool IsPlaying = false;
	double TimeAccumulator = 0.0;
	vector<uint8_t> CurrentFramePixels;
	int VideoWidth = 0;
	int VideoHeight = 0;
	plm_t* VideoPlayer = nullptr;
	shared_ptr<vector<uint8_t>> FileData; // precharge par LoadAsync (vide = lecture depuis le disque)
	vector<HdPackCondition*> Conditions;

	NesHdVideoInfo() = default;
	NesHdVideoInfo(const NesHdVideoInfo&) = delete;
	NesHdVideoInfo& operator=(const NesHdVideoInfo&) = delete;
	~NesHdVideoInfo()
	{
		if(VideoPlayer != nullptr) {
			plm_destroy(VideoPlayer);
			VideoPlayer = nullptr;
		}
	}

	bool MatchesCondition()
	{
		if(PlayOnlyOnce && HasPlayed) return false;
		for(HdPackCondition* cond : Conditions) {
			if(!cond->CheckCondition(0, 0, nullptr)) return false;
		}
		return true;
	}
};

// <2DOverlay> NES : particules 2D. Unites : pixels NES (256 x 240), vitesses en pixels NES / s, angles en degres (0 = droite, 90 = bas).
// Voir claude/09_2DOverlay.md (portage NES, claude/18_Architecture_PPU_NES.md pour le scroll).
struct NesHdParticle
{
	float X = 0, Y = 0;       // position dans le champ (pixels NES)
	float VX = 0, VY = 0;     // vitesse propre
	float Age = 0, Life = 1;  // secondes
	float Size = 1;           // multiplicateur de taille propre (aleatoire)
	float Rot = 0, Spin = 0;  // radians, radians / s
	float Seed1 = 0, Seed2 = 0;
};

struct NesHdOverlayInfo
{
	string JsonFile;
	vector<HdPackCondition*> Conditions;

	// Parametres de la balise (valeurs par defaut, puis JSON, puis balise)
	int ZIndex = 39;                    // echelle des <background> NES (0 a 39), 39 = au-dessus de tout
	int Layer = 0;                      // 0 = fixe a l'ecran, 1 = suit le defilement du PPU
	float SpeedX = 1.0f, SpeedY = 1.0f; // ratio de defilement (s'ajoute au mouvement propre)
	float Alpha = 1.0f;
	float FieldW = 1.0f, FieldH = 1.0f;

	// Particules (JSON)
	HdPackBitmapInfo* Texture = nullptr; // PNG de la particule (dessine en pixels HD) ; nullptr = point doux
	bool Additive = false;
	int MaxParticles = 300;
	float Rate = 60.0f;
	float LifeMin = 2.0f, LifeMax = 4.0f;
	float SpawnX = 0, SpawnY = 0, SpawnW = 1, SpawnH = 1;
	float SpeedMin = 20, SpeedMax = 40;
	float AngleMin = 80, AngleMax = 100;
	float GravityX = 0, GravityY = 0;
	float WindX = 0, WindY = 0;
	float Turbulence = 0, TurbulenceFreq = 0.5f;
	float RotationMin = 0, RotationMax = 0; // degres
	float SpinMin = 0, SpinMax = 0;         // degres / s
	float SizeStart = 1, SizeEnd = 1, SizeRandom = 0;
	float AlphaStart = 1, AlphaEnd = 1, FadeIn = 0.1f, FadeOut = 0.2f;
	uint32_t ColorStart = 0xFFFFFF, ColorEnd = 0xFFFFFF;
	float DotSize = 2.0f;                   // diametre du point doux (pixels NES) si pas de texture
	bool Prewarm = true;

	// Ecrans (type=plainscreen, type=ellipse) : passe finale sur l'image, zone en fractions de l'image
	bool IsPlainScreen = false, IsEllipse = false;
	float ZoneX0 = 0, ZoneX1 = 1, ZoneY0 = 0, ZoneY1 = 1;
	uint32_t FxColor = 0x000000;
	float FxEllipseX = 128.0f, FxEllipseY = 120.0f;   // centre (pixels NES, cadre complet 256 x 240)
	float FxRadiusX = 32.0f, FxRadiusY = 16.0f;       // rayons (pixels NES)
	bool FxNegative = false;                          // true : tout l'ecran est couvert sauf l'ellipse

	// Torche (type=torchlight) : halo doux. dark=1 : hors du halo, obscurite opaque (alpha) ; dark=0 : lumiere additive
	bool IsTorch = false;
	float TorchX = 128.0f, TorchY = 120.0f;   // centre de depart (pixels NES, cadre complet), decale par l'ancrage Layer
	float TorchRadius = 48.0f;                // rayon du halo (pixels NES)
	float TorchSoft = 0.5f;                   // largeur du bord doux (fraction du rayon)
	bool TorchDark = true;                    // true : tout ce qui est hors du halo est assombri
	float TorchGlow = 0.15f;                  // intensite de la teinte ajoutee au centre (dark=1)

	// Ombres, nuages, brouillard (bruit qui derive) : IsNoise, NoiseKind 0 = ombres, 1 = nuages, 2 = brouillard
	bool IsNoise = false;
	int NoiseKind = 0;
	float FxSize = 48.0f;               // taille des motifs (pixels NES)
	float FxSpeed = 8.0f, FxDirection = 0.0f; // vitesse (pixels NES / s) et direction (degres)
	float FxCoverage = 0.5f;            // part de l'ecran couverte (0 a 1)
	int FxSeed = 1;

	// Rayons de lumiere (type=rays) : faisceaux additifs partant d'un point (RayX/RayY en fractions de l'image)
	bool IsRays = false;
	float RayX = 0.5f, RayY = 0.0f;
	int RayCount = 8;
	float RayAngle = 90.0f, RaySpread = 60.0f;   // degres (0 = droite, 90 = bas)
	float RayWidth = 4.0f, RayLength = 0.8f;     // largeur angulaire (degres), longueur (fraction de la diagonale)
	float RaySpeed = 0.2f, RayWobble = 3.0f;
	uint32_t RayColor = 0xFFE8A0;

	// Aurore boreale (type=aurora) : rideaux additifs qui derivent, bord inferieur ondule, couleur entre color1 et color2
	bool IsAurora = false;
	float AuroraSize = 40.0f;       // largeur d'un rideau (pixels NES)
	float AuroraSpeed = 6.0f;       // derive horizontale (pixels NES / s)
	float AuroraCoverage = 0.5f;    // part de la zone couverte par les rideaux (0 a 1)
	float AuroraFall = 0.7f;        // fraction de la zone sur laquelle la lumiere s'estompe vers le haut
	float AuroraWave = 6.0f;        // ondulation du bord inferieur (pixels NES)
	uint32_t AuroraColor1 = 0x3CFF8C, AuroraColor2 = 0x8C4BFF;

	// Distorsion (type=distortion) : deformation horizontale sinusoidale de l'image finale (zone = ZoneX0..ZoneY1)
	bool IsDistortion = false;
	float DistAmp1 = 0, DistWave1 = 32, DistSpeed1 = 0.5f, DistPhase1 = 0;  // amplitude (px NES), longueur d'onde (lignes NES), vitesse (cycles/s), phase (degres)
	float DistAmp2 = 0, DistWave2 = 17, DistSpeed2 = 0.3f, DistPhase2 = 0;  // 2e sinusoide, combinee a la 1re

	// Etat d'execution (filtre HD)
	vector<NesHdParticle> Particles;
	float Time = 0;                     // temps d'activite (bruit : derive)
	bool Initialized = false;
	bool Active = false;
	float SpawnAcc = 0;
	float OffsetX = 0, OffsetY = 0;         // decalage d'ancrage accumule (pixels NES)
	int LastScrollX = 0, LastScrollY = 0;
	bool HaveScroll = false;
	uint32_t Rng = 0x12345678;

	bool MatchesCondition()
	{
		for(HdPackCondition* cond : Conditions) {
			if(!cond->CheckCondition(0, 0, nullptr)) {
				return false;
			}
		}
		return true;
	}
	float Rand() { Rng ^= Rng << 13; Rng ^= Rng >> 17; Rng ^= Rng << 5; return (float)(Rng & 0xFFFFFF) / 16777216.0f; }
	float RandRange(float a, float b) { return a + (b - a) * Rand(); }
};

// <Ignore2DOverlays>tuile[,palette] : regle ligne seule (appliquee aux tiles de meme cle)
struct HdIgnore2DRule
{
	HdTileKey Key;
	bool AnyPalette = false;
	vector<HdPackCondition*> Conditions;
};

// <2DLiquid> (vue de profil, phase 1) : liquide sous une surface ondulee. Dimensions en pixels NES.
// <2DSpriteLight> (NES) : halo 2D au centre d'une tile de sprite reconnue (meme principe que cote SNES).
// Syntaxe : [cond]<2DSpriteLight>TILE[,palette|*][,radius=48][,color=FFA040][,intensity=1.4][,flicker=0][,speed=9][,mode=add|shadow][,dx=0][,dy=0]
struct NesHdSpriteLightInfo
{
	HdTileKey Key;                // tile (index CHR ROM, ou 32 chiffres hexa en CHR RAM) et palette, comme <tile>
	bool AnyPalette = true;
	float Radius = 48.0f;         // pixels NES
	float Intensity = 1.4f;
	uint32_t Color = 0xFFA040;
	float Flicker = 0.0f;
	float FlickerSpeed = 9.0f;
	bool Shadow = false;          // mode=shadow : assombrit au lieu d'eclairer
	float OffsetX = 0.0f;         // dx= : decalage du halo (pixels NES), inverse si la tile est retournee
	float OffsetY = 0.0f;         // dy=
	vector<HdPackCondition*> Conditions;

	bool Matches(const HdPpuTileInfo& t) const
	{
		if(Key.IsChrRamTile) {
			if(memcmp(Key.TileData, t.TileData, sizeof(Key.TileData)) != 0) return false;
		} else if(Key.TileIndex != t.TileIndex) {
			return false;
		}
		return AnyPalette || Key.PaletteColors == t.PaletteColors;
	}
};

// <2DPerspective> (NES) : sol incline a partir d'un PNG, ancre sur le scroll PPU par ligne. Coordonnees NES absolues.
struct NesHdPerspectiveInfo
{
	vector<HdPackCondition*> Conditions;
	bool Active = false;
	bool Ceiling = false;                 // mode=ceiling : plafond (lignes au-dessus de l'horizon)
	bool Wall = false, WallRight = false; // mode=wall : paroi verticale, a gauche (side=left) ou a droite (side=right)
	bool HorizonSet = false, CySet = false; // horizon= et cy= explicites (sinon centre de l'ecran)
	float Cy = 120.0f;                    // cy= : centre vertical de la paroi (lignes NES)
	float Horizon = 120.0f;               // ligne NES de l'horizon
	float Depth = 64.0f;                  // distance (lignes NES) sous l'horizon ou la texture est a 1:1
	float Repeat = 0.0f;                  // repeat=N : repetitions de la texture entre la limite et le bas (profondeur seulement)
	float LimitOffset = 0.0f;             // limit=+N : premiere ligne dessinee = Horizon + N
	float FogRange = 0.0f;                // lignes du fondu vers le brouillard (0 = coupure)
	uint32_t Fog = 0x000000;
	float Alpha = 1.0f;
	bool TileX = true, TileY = true;
	float SpeedX = 0.0f, SpeedY = 0.0f;   // ratios de defilement par rapport au scroll PPU
	int Priority = 39;                    // Z : meme echelle que <background> NES (0 a 49)
	HdPackBitmapInfo* Texture = nullptr;
	vector<float> OffX, OffY;             // decalage par ligne NES (240 lignes), en texels
	vector<int> LastX, LastY;             // dernier scroll PPU lu par ligne
	bool HaveScroll = false;

	bool MatchesCondition()
	{
		for(HdPackCondition* cond : Conditions) {
			if(!cond->CheckCondition(0, 0, nullptr)) {
				return false;
			}
		}
		return true;
	}
};

struct NesHdLiquidInfo
{
	vector<HdPackCondition*> Conditions;
	bool Active = false;
	float Level = 176.0f;                 // hauteur de la surface (lignes NES, depuis le haut de l'image)
	float Depth = 64.0f;                  // profondeur du degrade vers la couleur profonde (lignes NES)
	float XMin = 0.0f, XMax = 1.0f;       // zone horizontale (fraction de la largeur)
	float Amp = 3.0f, Wave = 48.0f, Speed = 0.8f, Phase = 0.0f; // houle : amplitude (lignes NES), longueur d'onde (px NES), vitesse (cycles/s), phase (degres)
	float ScrollX = 0.0f;                 // defilement de la texture (px NES / s)
	float TexScale = 1.0f;                // taille d'un texel en pixels NES
	float Foam = 0.6f;                    // eclaircissement de la ligne de surface (0 a 1)
	float Alpha = 1.0f;                   // opacite du liquide (1 = remplace l'image)
	uint32_t Color = 0x1E6FB0, Deep = 0x0B2F5C;
	HdPackBitmapInfo* Texture = nullptr;  // PNG optionnel, repete et defilant
	float Time = 0.0f;                    // temps d'activite (houle, defilement)
	bool IsTop = false;                   // view=top : zone rectangulaire ondulee (ymin/ymax) au lieu d'une surface
	float YMin = 0.0f, YMax = 240.0f;     // view=top : zone verticale (lignes NES depuis le haut de l'image)
	float Glow = 0.0f;                    // halo au-dessus de la surface (0 a 1)
	float GlowRadius = 24.0f;             // rayon du halo (lignes NES)
	int BubbleCount = 0;                  // bulles qui montent et eclatent a la surface (view=side)
	float BubbleSize = 1.5f;              // rayon des bulles (lignes NES)
	float BubbleRise = 0.4f;              // montee des bulles (cycles / s)
	std::vector<uint8_t> BubbleMap;      // alpha des bulles par pixel HD de l'image visible (calcule par image)

	int Priority = 40;                    // bande de priorite (prio=0-49, defaut 40) : meme echelle que <background>
	int Col0 = 0, Col1 = 0;               // colonnes HD de la zone (calculees par image)
	vector<float> Surface;               // hauteur de la surface par colonne HD (lignes NES, calculee par image)

	bool MatchesCondition()
	{
		for(HdPackCondition* cond : Conditions) {
			if(!cond->CheckCondition(0, 0, nullptr)) {
				return false;
			}
		}
		return true;
	}
};

struct HdPackData
{
private:
	std::atomic<bool> _cancelLoad{false};

public:
	static constexpr int BgLayerCount = 50;

	vector<HdBackgroundInfo> BackgroundsByPriority[HdPackData::BgLayerCount];
	vector<unique_ptr<HdPackBitmapInfo>> BackgroundFileData;
	vector<unique_ptr<HdPackBitmapInfo>> ImageFileData;
	vector<unique_ptr<HdPackTileInfo>> Tiles;
	vector<unique_ptr<HdPackCondition>> Conditions;
	vector<unique_ptr<NesHdVideoInfo>> Videos;
	vector<unique_ptr<NesHdOverlayInfo>> Overlays;
	vector<unique_ptr<NesHdLiquidInfo>> Liquids; // <2DLiquid>
	vector<unique_ptr<NesHdPerspectiveInfo>> Perspectives; // <2DPerspective>
	vector<unique_ptr<NesHdSpriteLightInfo>> Sprite2DLights; // <2DSpriteLight>

	// <Ignore2DOverlays> : regles ligne seule, et drapeau si un bloc ou une regle existe dans le pack
	vector<unique_ptr<HdIgnore2DRule>> Ignore2DRules;
	bool HasIgnore2D = false;

	/*-------------Added by Gwenolo : v1 useless-------------------*/
	vector<PlayAdditionalBgmAction> PlayAdditionalBgmActions;
	vector<PlayAdditionalSfxAction> PlayAdditionalSfxActions;
	bool _disable_8bits_Sound = false;
   /*---------------------------------*/

	vector<HdPackAdditionalSpriteInfo> AdditionalSprites;
	vector<FallbackTileInfo> FallbackTiles;
	unordered_set<uint32_t> WatchedMemoryAddresses;
	unordered_map<HdTileKey, vector<HdPackTileInfo*>> TileByKey;
	unordered_map<string, string> PatchesByHash;
	unordered_map<int, BgmTrackInfo> BgmFilesById;
	unordered_map<int, string> SfxFilesById;
	// Fichiers OGG precharges en memoire par LoadAsync (cle = meme chemin que BgmFilesById / SfxFilesById)
	unordered_map<string, shared_ptr<vector<uint8_t>>> OggCache;
	vector<uint32_t> Palette;

	HdLoadingScreenConfig LoadingScreen; // balise <LoadingScreen>
	bool HasOverscanConfig = false;
	OverscanDimensions Overscan;
	bool Widescreen = false;   // <widescreen>on : sortie elargie a 16/9 (voie 1, fonds fournis par le pack)

	uint32_t Scale = 1;
	uint32_t Version = 0;
	uint32_t OptionFlags = 0;

	HdPackData() { }
	~HdPackData() { }

	HdPackData(const HdPackData&) = delete;
	HdPackData& operator=(const HdPackData&) = delete;

	// Execute fn(i) pour i dans [0, count) sur plusieurs threads (ou sequentiellement si peu de travail).
	// Chaque index est traite une seule fois ; l'annulation (_cancelLoad) arrete la distribution.
	template<typename TFunc>
	void ParallelForLoad(size_t count, TFunc fn)
	{
		if(count == 0) {
			return;
		}
		size_t nThreads = std::clamp<size_t>((size_t)std::thread::hardware_concurrency() - 2, 1, 8);
		if(nThreads <= 1 || count < 4) {
			for(size_t i = 0; i < count && !_cancelLoad; i++) {
				fn(i);
			}
			return;
		}
		std::atomic<size_t> next{0};
		std::vector<std::thread> workers;
		workers.reserve(nThreads);
		for(size_t t = 0; t < nThreads; t++) {
			workers.emplace_back([&]() {
				while(!_cancelLoad) {
					size_t i = next.fetch_add(1);
					if(i >= count) {
						break;
					}
					fn(i);
				}
			});
		}
		for(auto& w : workers) {
			w.join();
		}
	}

	// Prechargement complet du pack, appele AVANT le debut de l'emulation (appel bloquant dans NesConsole::LoadHdPack) :
	// 1. decodage des PNG (<background> et <img>), 2. decoupage de toutes les tiles HD, 3. lecture des OGG en memoire.
	// Plus aucun acces disque ni decodage PNG pendant le jeu.
	void LoadAsync(bool preloadAudio = true)
	{
		Timer timer;

		// Ecran de chargement : nombre de fichiers = PNG + OGG distincts
		uint32_t loadingTotal = (uint32_t)(BackgroundFileData.size() + ImageFileData.size());
		if(preloadAudio) {
			unordered_set<string> audioNames;
			for(auto& bgm : BgmFilesById) audioNames.insert(bgm.second.Filename);
			for(auto& sfx : SfxFilesById) audioNames.insert(sfx.second);
			loadingTotal += (uint32_t)audioNames.size();
		}
		HdLoadingScreen::Guard loadingScreen(LoadingScreen, loadingTotal);

		// 1. PNG : decodage en parallele (verrou par bitmap ; Progress protege par mutex)
		Timer pngTimer;
		const size_t bgCount = BackgroundFileData.size();
		ParallelForLoad(bgCount + ImageFileData.size(), [&](size_t i) {
			HdPackBitmapInfo& bitmap = i < bgCount ? *BackgroundFileData[i] : *ImageFileData[i - bgCount];
			bitmap.Init();
			HdLoadingScreen::Progress(bitmap.PngName);
		});
		if(_cancelLoad) {
			return;
		}
		const double pngMs = pngTimer.GetElapsedMS();

		// 2. Tiles HD (sinon decoupees a leur premiere apparition a l'ecran) : apres la barriere des PNG
		Timer tileTimer;
		std::atomic<size_t> tileInits{0};
		ParallelForLoad(Tiles.size(), [&](size_t i) {
			HdPackTileInfo* tile = Tiles[i].get();
			if(tile && tile->Bitmap && tile->NeedInit()) {
				tile->Init();
				tileInits++;
			}
		});
		if(_cancelLoad) {
			return;
		}
		size_t tileCount = tileInits.load();
		MessageManager::Log("[HD-LOAD] NES : PNG " + std::to_string((int)pngMs) + " ms, tuiles " + std::to_string((int)tileTimer.GetElapsedMS()) +
			" ms (" + std::to_string(std::clamp((int)std::thread::hardware_concurrency() - 2, 1, 8)) + " threads)");

		// 3. Audio OGG (<bgm> et <sfx>)
		size_t oggCount = 0;
		size_t oggBytes = 0;
		if(preloadAudio) {
			auto preloadOgg = [&](const string& filename) {
				if(filename.empty() || OggCache.find(filename) != OggCache.end()) {
					return;
				}
				auto fileData = std::make_shared<vector<uint8_t>>();
				HdLoadingScreen::Progress(filename);
				VirtualFile file = filename;
				if(file.ReadFile(*fileData) && !fileData->empty()) {
					oggBytes += fileData->size();
					oggCount++;
					OggCache[filename] = fileData;
				} else {
					MessageManager::Log("[HDPack] Audio preload failed: " + filename);
				}
			};

			for(auto& bgm : BgmFilesById) {
				if(_cancelLoad) {
					return;
				}
				preloadOgg(bgm.second.Filename);
			}
			for(auto& sfx : SfxFilesById) {
				if(_cancelLoad) {
					return;
				}
				preloadOgg(sfx.second);
			}
		}

		// 4. Videos <video> : fichier en memoire (512 Mo max). Au-dela, le fichier reste lu depuis le disque.
		if(preloadAudio) {
			for(auto& video : Videos) {
				if(_cancelLoad) {
					return;
				}
				if(!video || video->Filename.empty()) {
					continue;
				}
				VirtualFile file = video->Filename;
				if(!file.IsValid() || file.GetSize() > (size_t)512 * 1024 * 1024) {
					continue;
				}
				auto data = std::make_shared<vector<uint8_t>>();
				if(file.ReadFile(*data) && !data->empty()) {
					video->FileData = data;
				}
			}
		}

		MessageManager::Log("[HDPack] Preload done: " +
			std::to_string(BackgroundFileData.size() + ImageFileData.size()) + " PNG, " +
			std::to_string(tileCount) + " tiles, " +
			std::to_string(oggCount) + " OGG (" + std::to_string(oggBytes / (1024 * 1024)) + " MB) in " +
			std::to_string((int)timer.GetElapsedMS()) + " ms");
	}

	// Donnees OGG prechargees pour ce fichier, ou nullptr (lecture depuis le disque)
	shared_ptr<vector<uint8_t>> GetPreloadedAudio(const string& filename)
	{
		auto result = OggCache.find(filename);
		return result != OggCache.end() ? result->second : nullptr;
	}

	void CancelLoad()
	{
		_cancelLoad = true;
	}
};

enum class HdPackOptions
{
	None = 0,
	NoSpriteLimit = 1,
	AlternateRegisterRange = 2,
	DisableCache = 8,
	DontRenderOriginalTiles = 16,
	AutomaticFallbackTiles = 32
};