#pragma once
#include "pch.h"
#include "NES/NesConstants.h"
#include "Shared/MessageManager.h"
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
	bool HasTransparentPixels;
	bool TransparencyRequired;
	bool IsFullyTransparent;
	vector<uint32_t> HdTileData;
	uint32_t ChrBankId;

	vector<HdPackCondition*> Conditions;
	bool ForceDisableCache;

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
	float VerticalScrollRatio;
	uint8_t Priority;

	uint32_t Left;
	uint32_t Top;

	HdPackBlendMode BlendMode;

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

struct HdPackData
{
private:
	bool _cancelLoad = false;

public:
	static constexpr int BgLayerCount = 40;

	vector<HdBackgroundInfo> BackgroundsByPriority[HdPackData::BgLayerCount];
	vector<unique_ptr<HdPackBitmapInfo>> BackgroundFileData;
	vector<unique_ptr<HdPackBitmapInfo>> ImageFileData;
	vector<unique_ptr<HdPackTileInfo>> Tiles;
	vector<unique_ptr<HdPackCondition>> Conditions;
	vector<unique_ptr<NesHdVideoInfo>> Videos;

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

	bool HasOverscanConfig = false;
	OverscanDimensions Overscan;

	uint32_t Scale = 1;
	uint32_t Version = 0;
	uint32_t OptionFlags = 0;

	HdPackData() { }
	~HdPackData() { }

	HdPackData(const HdPackData&) = delete;
	HdPackData& operator=(const HdPackData&) = delete;

	// Prechargement complet du pack, appele AVANT le debut de l'emulation (appel bloquant dans NesConsole::LoadHdPack) :
	// 1. decodage des PNG (<background> et <img>), 2. decoupage de toutes les tiles HD, 3. lecture des OGG en memoire.
	// Plus aucun acces disque ni decodage PNG pendant le jeu.
	void LoadAsync(bool preloadAudio = true)
	{
		Timer timer;

		// 1. PNG
		for(auto& bitmap : BackgroundFileData) {
			bitmap->Init();
			if(_cancelLoad) {
				return;
			}
		}
		for(auto& bitmap : ImageFileData) {
			bitmap->Init();
			if(_cancelLoad) {
				return;
			}
		}

		// 2. Tiles HD (sinon decoupees a leur premiere apparition a l'ecran)
		size_t tileCount = 0;
		for(auto& tile : Tiles) {
			if(_cancelLoad) {
				return;
			}
			if(tile && tile->Bitmap && tile->NeedInit()) {
				tile->Init();
				tileCount++;
			}
		}

		// 3. Audio OGG (<bgm> et <sfx>)
		size_t oggCount = 0;
		size_t oggBytes = 0;
		if(preloadAudio) {
			auto preloadOgg = [&](const string& filename) {
				if(filename.empty() || OggCache.find(filename) != OggCache.end()) {
					return;
				}
				auto fileData = std::make_shared<vector<uint8_t>>();
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