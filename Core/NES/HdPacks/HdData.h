#pragma once
#include "pch.h"
#include <atomic>
#include <thread>
#include "NES/NesConstants.h"
#include "Shared/MessageManager.h"
#include "Shared/HdLoadingScreen.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/HexUtilities.h"
#include "Shared/HdWatchExpr.h"
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

		// [fork HD] conditions pas encore rattachees a l'ecran (avant la premiere image decodee) :
		// evite un plantage quand play_conditional_bgm les teste des la premiere frame
		if(_screenInfo == nullptr) {
			return false;
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
	vector<int32_t> TopEdgeHd; // <2DReflect> : rangee HD de la premiere pixel opaque par colonne (-1 = aucune)

	void ComputeTopEdge()
	{
		TopEdgeHd.assign(Width, -1);
		if(Width == 0 || Height == 0 || PixelData.size() < (size_t)Width * Height) return;
		uint32_t found = 0;
		for(uint32_t r = 0; r < Height && found < Width; r++) {
			const uint32_t* row = PixelData.data() + (size_t)r * Width;
			for(uint32_t c = 0; c < Width; c++) {
				if(TopEdgeHd[c] < 0 && (row[c] >> 24) >= 128) { TopEdgeHd[c] = (int32_t)r; found++; }
			}
		}
	}
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
	// G6d : carte de normales (<DioramaNormal>) : PNG de relief associe au PNG de la tile ; meme region (X, Y, Width, Height)
	HdPackBitmapInfo* NormalBitmap = nullptr;
	vector<uint32_t> NormalTileData;
	uint32_t ChrBankId;

	vector<HdPackCondition*> Conditions;
	bool ForceDisableCache;

	// Variante miroir (<tile_hflip>, <tile_vflip>, <tile_hvflip>) : l'image du pack est deja retournee, pas de miroir a l'affichage
	bool IsHFlip = false;
	bool IsVFlip = false;

	// <DioramaOffCamera>{ ... } (doc 38) : tile declaree dans un bloc off camera, dessinee dans la couche fixe du diorama
	bool DioOffCamera = false;
	// <DioramaIgnore>{ ... } (doc 38) : tile declaree dans un bloc ignore, jamais dessinee dans les couches du diorama
	bool DioIgnore = false;
	// <DioramaDepth>z=..,t=..{ ... } (doc 38) : groupe de profondeur de la tile (1 a 4, 0 = aucun)
	uint8_t DioDepthGroup = 0;
	// <DioramaShadow>...{ ... } (doc 42, G6c) : numero (regle + 1, 0 = aucun) du bloc dont la tile de sprite projette une ombre
	uint8_t DioShadow = 0;

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

		if(NormalBitmap) {
			NormalBitmap->Init();
			const uint32_t nOffset0 = Y * NormalBitmap->Width + X;
			if(NormalBitmap->PixelData.size() >= nOffset0 + ((Height - 1) * NormalBitmap->Width) + Width) {
				NormalTileData.resize(Width * Height);
				uint32_t nOffset = nOffset0;
				for(uint32_t y = 0; y < Height; y++) {
					memcpy(NormalTileData.data() + (y * Width), NormalBitmap->PixelData.data() + nOffset, Width * sizeof(uint32_t));
					nOffset += NormalBitmap->Width;
				}
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

	// ramx= / ramy= : defilement pilote par la RAM ($nom de <watchMemoryAddress>) a la place du defilement PPU
	HdWatchExpr ScrollXExpr;
	HdWatchExpr ScrollYExpr;
	bool ExprXOk = false;   // mis a jour a chaque image (OnBeforeApplyFilter)
	bool ExprYOk = false;
	int32_t ExprX = 0;
	int32_t ExprY = 0;
	// Diorama : cible du fond. -1 = selon la priorite (0 a 9 -> L0, 10 a 19 -> L1, 20 a 29 -> au-dessus de la tile, 30 a 49 -> L3), -2 = ignore, 0 a 3 = L0..L3, 4 a 7 = groupe <DioramaDepth> 1 a 4 (bloc { } ou option dio=)
	int DioTarget = -1;
	int DioWideX = 0, DioWideY = 0; // diorama, fond sur L0 : marge (pixels NES) dessinee au-dela de l'ecran a gauche / droite (wide=) et en haut / en bas (tall=)

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

// <2DReflect> : reflet miroir des sprites sous le bord superieur du PNG du calque sol (vue de profil)
struct NesHdReflectInfo
{
	int Priority = 10;       // calque du sol (priorite du <background>)
	float Intensity = 0.5f;  // opacite du reflet (0 a 1)
	float Fade = 0.0f;       // distance (pixels NES) au bout de laquelle le reflet disparait ; 0 = pas de fondu
};

// <2DTexture> : texture repetee, etiree par scalex/scaley (%), defilante (scrollx/scrolly x scroll PPU).
// Remplace la couleur (ou TRANS = transparent) d'un <background> de meme priorite.
struct NesHdTextureInfo
{
	HdPackBitmapInfo* Texture = nullptr;
	bool HasKey = true;
	uint32_t Key = 0;          // RRGGBB
	float TexelPerPxX = 1.0f;  // texels par pixel HD = 100 / scalex
	float TexelPerPxY = 1.0f;
	float RatioX = 0.0f;       // defilement : scroll PPU (pixels NES) x ratio
	float RatioY = 0.0f;
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
	bool MaskBg = false;                  // mask=1 : ne recouvre pas les pixels opaques du <background> de meme prio
	bool HasMaskKey = false;              // maskcolor=RRGGBB : cette couleur de fond compte comme transparente
	uint32_t MaskKey = 0;
	bool NormalMode = false;              // normal=1 (view=top) : vaguelettes a normales, reflet, refraction
	float Specular = 0.6f;                // normal=1 : intensite du reflet 0 a 1
	float Refract = 4.0f;                 // normal=1 : decalage de la texture selon la pente (pixels NES)
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

// <watchMemoryAddress> : valeur RAM CPU nommee, lue une fois par image (diorama, doc 32 s0.1). Lecture seule.
struct HdWatchedValue
{
	string Name;
	uint32_t Address = 0;
	uint8_t Size = 1;      // 1 ou 2 octets (petit-boutiste)
	bool Signed = false;
};

// Reglages de <Diorama>plainTiles,options (doc 36, 37). Z : profondeur de chaque couche (pixels NES, positif = derriere le
// plan du jeu) : L0 fond, L1 sprites derriere, L2 tiles, L3 sprites devant. Yaw / Pitch en degres ; Dist = distance de la camera.
// Textures des murs d'une couche epaisse (G3b2, doc 38) : Top = dessus et dessous, Side = flancs. Top retombe sur Side quand il
// est absent (avec les etirements de Side) ; Side absent = mur d'origine (extrusion). Etirements : 1 = taille normale (un pixel
// du PNG = un pixel HD), 2 = deux fois plus grand ; X le long du bord, Y en profondeur ; de 0.01 a 100. Valeur negative (-1) =
// "adapt" : la texture couvre exactement la zone (X : largeur de la zone pour le dessus, hauteur pour les flancs ; Y : epaisseur).
struct HdDioramaWallTex
{
	HdPackBitmapInfo* Top = nullptr;
	HdPackBitmapInfo* Side = nullptr;
	int SideVol = 0;   // G3b3 : side=volume.png:N, bande de N images (0 = motif simple)
	int SideVolMode = 0; // orientation : 0 = automatique (plus haut que large = verticale), 1 = cote a cote (h), 2 = de haut en bas (v)
	float TopStretchX = 1.0f, TopStretchY = 1.0f;
	float SideStretchX = 1.0f, SideStretchY = 1.0f;
};

struct HdDioramaConfig
{
	float Z[4] = { 200.0f, 24.0f, 0.0f, -24.0f };
	HdDioramaWallTex Wall[4];                 // G3b2 : murs des couches L0..L3 (options side0..3, top0..3, stretchx0..3, stretchy0..3...)
	float T[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // G3b1 : epaisseur de chaque couche en tiles (8 pixels NES), vers l'arriere. 0 = feuille
	float Yaw = 0.0f, Pitch = 0.0f;
	float Dist = 256.0f;
	bool Orbit = false;
	// G3c3 : mode fixed3d de <DioramaOffCamera> : la couche fixe devient une plaque epaisse (FixedT tiles), face avant a la
	// profondeur FixedZ (pixels NES, negatif = devant le jeu ; taille apparente inchangee), perspective reelle x FixedPersp
	float FixedT = 1.0f, FixedZ = 0.0f, FixedPersp = 1.0f;
	float Zoom = 1.0f;           // G4 : zoom de la vue (0.1 a 4), 1 = normal ; < 1 recule pour voir tout le plan et ses marges
	float NormalStrength = 1.0f; // G6d : force du relief des cartes de normales (0 a 4), 1 = tel quel
	bool NormalFlipY = false;    // G6d : nflipy=on : le vert des normales pointe vers le bas (convention DirectX)
	bool ExtDebug = false;       // G4 : extdebug=on teinte en magenta les pixels venus d'une extension (controle)
	float FixedDark = 0.5f; // assombrissement des tranches vers le fond (0 = aucun, 1 = noir au fond)
	int Aa = 1;                  // G3f : surechantillonnage (aa=1 a 3), 1 = aucun
	float OrbitAmp = 25.0f;     // degres
	float OrbitPeriod = 600.0f; // images
};

// <DioramaOffCamera>x,y,largeur,hauteur,mode (doc 38) : zone d'ecran (pixels NES) dont les tiles et les sprites restent
// a leur position 2D quand la camera du diorama bouge.
struct HdDioramaOffRect
{
	int X = 0, Y = 0, W = 0, H = 0;
};

// <DioramaDepth>z=..,t=..[,rect=x,y,l,h] ou bloc <DioramaDepth>z=..,t=..{ ... } (doc 38) : groupe de tiles de fond ayant sa
// propre profondeur (Z, pixels NES, positif = derriere) et son epaisseur (T, tiles). 4 groupes au plus.
struct HdDioramaDepthGroup
{
	float Z = 0.0f;
	float T = 0.0f;
	bool HasRect = false;
	int X = 0, Y = 0, W = 0, H = 0;
	HdDioramaWallTex Wall;  // G3b2 : options side=, top=, stretchx=, stretchy=...
};

// <DioramaExtend>side=..,layer=..,mode=..,size=..,band=.. (doc 40) : complete l'ecran au-dela de ses bords (marge de Size pixels NES)
// sur un ou plusieurs cotes d'une couche. Side : bits 1 haut, 2 bas, 4 gauche, 8 droite. Mode : 1 stretch, 2 repeat (Band tiles NES
// du bord, alignees sur la grille du monde), 3 mirror, 4 wrap, 5 color (aplat). Conditions evaluees une fois par image.
struct HdDioramaExtendRule
{
	int Side = 0;
	int Layer = 2;
	int Mode = 0;
	int Size = 128;
	int Band = 1;
	int RangeMin = -1, RangeMax = -1; // rows (cotes gauche / droit) ou cols (haut / bas) : plage de l'ecran, pixels NES ; -1 = tout
	bool RangeIsRows = true;
	float Fog = 0.0f;     // G4c : force de brume (0..1) atteinte a FogDist pixels du bord
	int FogDist = -1;     // G4c : distance de la brume (pixels NES) ; -1 = la marge
	uint32_t Color = 0;   // G4c : couleur RRGGBB
	bool HasColor = false;
	int Edge = -1;   // debut reel du contenu (pixels NES, y pour haut / bas, x pour gauche / droite) ; -1 = bord de l'ecran
	vector<HdPackCondition*> Conditions;
};

// <DioramaLight>type=ambient|point|sun,... (doc 42) : lumieres de la scene diorama, sur conditions [ ].
struct HdDioramaLightRule
{
	int Type = 0;            // 1 ambiante, 2 ponctuelle, 3 soleil
	float X = 128, Y = 60, Z = -60;   // ponctuelle : position (pixels NES de la scene)
	float Dx = 0.3f, Dy = 0.5f, Dz = 1.0f; // soleil : direction de propagation de la lumiere
	uint32_t Color = 0xFFFFFF;
	float Intensity = 1.0f;
	float Radius = 256.0f;
	float Glow = 0.0f, GlowRadius = 48.0f; // halo visible (ponctuelle)
	// G6b : lumiere attachee a une tile de sprite (tile=HASH[,palette=..]) : suit la tile tant qu'elle est a l'ecran
	bool HasTile = false;
	HdTileKey Key;
	bool AnyPalette = true;
	float Flicker = 0.0f, FlickerSpeed = 9.0f;
	float OffX = 0.0f, OffY = 0.0f;   // decalage du centre de la tile (pixels NES, inverse si la tile est retournee)
	vector<HdPackCondition*> Conditions;

	bool MatchesTile(const HdPpuTileInfo& t) const
	{
		if(Key.IsChrRamTile) {
			if(memcmp(Key.TileData, t.TileData, sizeof(Key.TileData)) != 0) return false;
		} else if(Key.TileIndex != t.TileIndex) {
			return false;
		}
		return AnyPalette || Key.PaletteColors == t.PaletteColors;
	}
};

// <DioramaNormal>albedo.png,normal.png (doc 44, G6d) : les tiles prises dans albedo.png recoivent leurs normales dans normal.png (meme region).
struct HdDioramaNormalPair
{
	string AlbedoName;                // nom resolu du PNG de couleurs (comme HdPackBitmapInfo::PngName)
	HdPackBitmapInfo* Normal = nullptr;
};

// H1 : expression de position (somme de produits de nombres et de valeurs $nom de <watchMemoryAddress>), ex. $page*256+$x
struct HdDioramaExpr
{
	struct Factor { int Var = -1; float Val = 1.0f; };   // Var >= 0 : indice dans HdPackData::WatchedValues
	struct Term { float Sign = 1.0f; vector<Factor> F; };
	vector<Term> T;
	bool Empty() const { return T.empty(); }
	// false si une valeur surveillee n'est pas encore lue
	bool Eval(const vector<int32_t>& vals, float& out) const
	{
		float sum = 0.0f;
		for(const Term& t : T) {
			float p = t.Sign;
			for(const Factor& f : t.F) {
				if(f.Var >= 0) {
					if((size_t)f.Var >= vals.size() || vals[f.Var] == INT32_MIN) return false;
					p *= (float)vals[f.Var];
				} else {
					p *= f.Val;
				}
			}
			sum += p;
		}
		out = sum;
		return true;
	}
};

// <DioramaObject>fichier.obj,tex=...,x=,y=,z=,scale=,rotx=,roty=,rotz=,spin= (doc 45, H1) : objet 3D de decor, une regle par objet.
// <DioramaPad> : la manette pilote la camera du diorama. Les axes analogiques s'ajoutent a la vue de <Diorama> (rotation yaw/pitch ou zoom).
struct HdDioramaPadRule
{
	vector<HdPackCondition*> Conditions;
	int Port = 1;                 // manette 1 a 4
	int Left = 1, Right = 2;      // fonction du stick : 0 = aucune, 1 = rotation (yaw/pitch), 2 = zoom
	float YawMax = 35.0f, PitchMax = 25.0f; // degres atteints a fond de course
	float ZoomRange = 2.0f;       // facteur de zoom a fond de course (>= 1) : zoom x ZoomRange vers le haut, / ZoomRange vers le bas
	float Dead = 0.2f;            // zone morte (0 a 0.9)
	float Smooth = 0.2f;          // lissage par image (0.01 a 1, 1 = instantane)
	bool InvX = false, InvY = false;
};

// Cameras du diorama (doc 49). <DioramaCamera>nom,yaw=,pitch=,dist=,zoom=,time=,key=,default=on : une ligne declare une camera (la premiere
// pour ce nom) ou, avec [cond], modifie ses reglages tant que la condition est vraie (les lignes vraies s'appliquent dans l'ordre du fichier).
struct HdDioramaCameraLine
{
	vector<HdPackCondition*> Conditions;
	int Index = 0;                 // numero de la camera (ordre de premiere declaration)
	bool Has[4] = { false, false, false, false }; // yaw, pitch, dist, zoom
	float V[4] = { 0, 0, 0, 1 };
	bool HasTime = false; float Time = 40.0f;     // duree du deplacement vers cette camera, en images
	string Key;                    // touche (nom Mesen : F5, Pad1 R3...) qui selectionne cette camera
	bool IsDefault = false;        // default=on : camera par defaut si aucune regle <DioramaCameraDefault> n'est vraie
};
struct HdDioramaCameraDefaultRule
{
	vector<HdPackCondition*> Conditions;
	int Index = 0;
};
struct HdDioramaCameraKeys { string Next, Prev, Default; };

struct HdDioramaObjectRule
{
	std::shared_ptr<vector<float>> Mesh; // 8 flottants par sommet (position, normale, uv), triangles deplies
	HdPackBitmapInfo* Tex = nullptr;     // texture PNG (optionnelle)
	uint32_t Color = 0xFFFFFF;           // couleur multipliee a la texture (sans texture : couleur de l'objet)
	float X = 128, Y = 120, Z = 0;       // position (pixels NES de la scene, comme <DioramaLight>)
	float RefZ = 0;                      // refz= : profondeur (derriere la face avant, pixels NES) ou le modele colle exactement a la vue 2D en vue neutre
	bool Occlude = false;                // occlude=on : objet dessine en premier, les sprites et tiles sont testes contre sa profondeur
	float Scale = 16.0f;                 // pixels NES par unite du .obj
	float RotX = 0, RotY = 0, RotZ = 0;  // rotation fixe (degres)
	float Spin = 0;                      // rotation continue autour de Y (degres par image)
	bool Shadow = true;                  // shadow=off : l'objet ne recoit pas les ombres des sprites (<DioramaShadow>)
	bool Lit = true;                     // lit=off : l'objet n'est pas eclaire par <DioramaLight> (couleur de la texture telle quelle)
	bool HasWindow = false; float Window = 0.0f; // window=N : seuls les triangles a moins de N pixels NES de l'ecran (dans le niveau, avec scrollx=) sont dessines
	float Radius = 0.0f;                 // distance maximale d'un sommet a l'origine du modele x scale (pixels NES), pour ne pas dessiner un objet hors ecran
	HdDioramaExpr ScrollX, ScrollY;      // scrollx= / scrolly= : defilement du niveau (pixels) ; position a l'ecran = x - scrollx
	vector<HdPackCondition*> Conditions;
};

// <DioramaShadow>tile=HASH,... ou <DioramaShadow>...{ <tile>... } (doc 42, G6c) : les sprites reconnus projettent une ombre (silhouette) sur les
// couches OntoMask, dans la direction de la lumiere numero Light. Un seul style par image (celui de la premiere regle utilisee).
struct HdDioramaShadowRule
{
	float Opacity = 0.5f, Soft = 0.0f;
	uint32_t Color = 0x000000;
	uint32_t OntoMask = 4;   // bits 0..3 = L0..L3, 4..7 = groupes g1..g4 ; defaut L2
	int Light = 1;           // 1 = premiere lumiere ponctuelle / solaire active
	bool HasTile = false;    // forme ligne : tile=HASH
	bool AnyTile = false;    // tile=* : tous les sprites de la couche L3
	HdTileKey Key;
	bool AnyPalette = true;
	vector<HdPackCondition*> Conditions;

	bool MatchesTile(const HdPpuTileInfo& t) const
	{
		if(AnyTile) return true;
		if(Key.IsChrRamTile) {
			if(memcmp(Key.TileData, t.TileData, sizeof(Key.TileData)) != 0) return false;
		} else if(Key.TileIndex != t.TileIndex) {
			return false;
		}
		return AnyPalette || Key.PaletteColors == t.PaletteColors;
	}
};

// <DioramaSkybox>color,skytop=RRGGBB,skybottom=RRGGBB  ou  <DioramaSkybox>fichier.png,vitesseX,vitesseY (doc 41) : fond du diorama
// derriere les plans, sur conditions [ ]. Vitesses : pixels NES par image (derive automatique de l'image) ; pour pano / cube : degres par image (yaw, pitch).
struct HdDioramaSkyboxRule
{
	int Mode = 0;  // 1 degrade, 2 image repetee, 3 panorama equirectangulaire, 4 cubemap (image unique ou 6 faces)
	HdPackBitmapInfo* Faces[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }; // cubemap en 6 fichiers : gauche, devant, droite, derriere, haut, bas
	uint32_t Top = 0, Bottom = 0;
	HdPackBitmapInfo* Image = nullptr;
	float SpeedX = 0.0f, SpeedY = 0.0f;
	vector<HdPackCondition*> Conditions;
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
	vector<unique_ptr<HdPackBitmapInfo>> AuxBitmaps; // G6d / H1 : PNG des normales et des objets 3D, hors de la liste des <img> (dont l'ordre numerote les images des tiles)
	vector<unique_ptr<HdPackTileInfo>> Tiles;
	vector<unique_ptr<HdPackCondition>> Conditions;
	vector<unique_ptr<NesHdVideoInfo>> Videos;
	vector<unique_ptr<NesHdOverlayInfo>> Overlays;
	vector<unique_ptr<NesHdLiquidInfo>> Liquids; // <2DLiquid>
	vector<NesHdTextureInfo> Textures2D[50];     // <2DTexture>, par priorite (0-49)
	vector<NesHdReflectInfo> Reflects;           // <2DReflect> (un seul par pack)
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
	vector<HdWatchedValue> WatchedValues; // <watchMemoryAddress>
	string PackFolder;                    // dossier du pack (declencheur du journal [HD-DIORAMA])
	bool DioramaEnabled = false;          // <Diorama> present (doc 32)
	string DioramaMode;                   // "plainTiles"
	HdDioramaConfig Diorama;              // options de <Diorama>
	vector<HdDioramaOffRect> DioramaOffRects; // <DioramaOffCamera> (forme rectangle, mode flat)
	vector<HdDioramaExtendRule> DioramaExtendRules; // G4a : <DioramaExtend>
	vector<HdDioramaSkyboxRule> DioramaSkyboxRules; // G5
	vector<HdDioramaLightRule> DioramaLightRules;   // G6a
	vector<HdDioramaShadowRule> DioramaShadowRules; // G6c
	vector<HdDioramaNormalPair> DioramaNormalPairs; // G6d
	vector<HdDioramaObjectRule> DioramaObjectRules; // H1
	vector<HdDioramaPadRule> DioramaPadRules;       // manette -> camera
	vector<string> DioramaCameraNames;              // cameras declarees (minuscules), 16 au plus
	vector<HdDioramaCameraLine> DioramaCameraLines;
	vector<HdDioramaCameraDefaultRule> DioramaCameraDefaults;
	HdDioramaCameraKeys DioramaCameraKeys;
	bool DioramaFixed3d = false;          // G3c3 : au moins une zone ou un bloc <DioramaOffCamera> en mode fixed3d
	bool DioramaOffTiles = false;         // au moins une <tile> declaree dans un bloc <DioramaOffCamera>{ }
	vector<HdDioramaDepthGroup> DioramaDepthGroups; // <DioramaDepth> (G3e), 4 au plus
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

		// <2DReflect> : bord superieur des PNG de fond du calque sol (une fois au chargement)
		for(const NesHdReflectInfo& rf : Reflects) {
			for(auto& bg : BackgroundsByPriority[rf.Priority]) {
				if(bg.Data && bg.Data->TopEdgeHd.empty()) bg.Data->ComputeTopEdge();
			}
		}

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

		// Estimation de la memoire decodee du pack (log [HD-MEM])
		{
			uint64_t pngBytes = 0, tileBytes = 0, videoBytes = 0;
			for(auto& b : BackgroundFileData) pngBytes += (uint64_t)b->PixelData.size() * 4;
			for(auto& b : ImageFileData) pngBytes += (uint64_t)b->PixelData.size() * 4;
			for(auto& t : Tiles) if(t) tileBytes += (uint64_t)t->HdTileData.size() * 4;
			for(auto& v : Videos) if(v && v->FileData) videoBytes += v->FileData->size();
			uint64_t total = pngBytes + tileBytes + videoBytes + (uint64_t)oggBytes;
			auto mo = [](uint64_t b) { return std::to_string(b / (1024 * 1024)); };
			MessageManager::Log("[HD-MEM] NES : PNG " + mo(pngBytes) + " Mo, tuiles HD " + mo(tileBytes) + " Mo, OGG precharges " + mo(oggBytes) + " Mo, videos " + mo(videoBytes) + " Mo, total estime " + mo(total) + " Mo");
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