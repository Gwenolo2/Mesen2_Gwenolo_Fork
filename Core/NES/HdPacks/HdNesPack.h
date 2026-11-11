#pragma once
#include "pch.h"
#include "NES/HdPacks/HdData.h"
#include "SNES/HdPacks/SnesHdThreadPool.h"

class NesConsole;
class EmuSettings;
class SoundMixer;



class BaseHdNesPack
{
protected:
	unordered_map<int32_t, int32_t> _fallbackTiles;
	HdScreenInfo* _hdScreenInfo = nullptr;
	SoundMixer* _videoAudio = nullptr; // sortie audio des <video>

public:
	static constexpr uint32_t CurrentVersion = 109;

	virtual uint32_t GetScale() = 0;

	// Widescreen (voie 1) : fonds <background> dans les bandes, apres la copie du centre
	virtual void DrawWidescreenBands(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) = 0;
	virtual void ProcessWidescreenDistortions(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) = 0;
	virtual void SetWidescreenMode(bool on, float widthNes) = 0;
	virtual void ProcessWidescreenOverlays(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) = 0;
	virtual void ProcessWidescreenScreenLayers(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) = 0;
	virtual void ProcessWidescreenPerspectives(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) = 0;

	HdScreenInfo* GetScreenInfo() { return _hdScreenInfo; }

	int32_t GetFallbackTile(int32_t tileIndex) 
	{
		auto result = _fallbackTiles.find(tileIndex);
		if(result != _fallbackTiles.end()) {
			return result->second;
		}
		return -1;
	}

	virtual void Process(HdScreenInfo* hdScreenInfo, uint32_t* outputBuffer, OverscanDimensions& overscan) = 0;
	virtual void SetVideoAudioSink(SoundMixer* mixer) { _videoAudio = mixer; }

	virtual ~BaseHdNesPack() {}
};

template<uint32_t scale>
class HdNesPack final : public BaseHdNesPack
{
private:
	struct HdBgConfig
	{
		int32_t BackgroundIndex = -1;
		int32_t BgPriority = -1;
		int32_t BgScrollX = 0;
		int32_t BgScrollY = 0;
		int16_t BgMinX = -1;
		int16_t BgMaxX = -1;
	};

	static constexpr uint8_t PriorityLevelsPerLayer = 10;
	static constexpr uint8_t BehindBgSpritesPriority = 0 * PriorityLevelsPerLayer;
	static constexpr uint8_t BehindBgPriority = 1 * PriorityLevelsPerLayer;
	static constexpr uint8_t BehindFgSpritesPriority = 2 * PriorityLevelsPerLayer;
	static constexpr uint8_t ForegroundPriority = 3 * PriorityLevelsPerLayer;
	// Cinquieme bande (40 a 49) : au-dessus du premier plan
	static constexpr uint8_t TopPriority = 4 * PriorityLevelsPerLayer;

	NesConsole* _console = nullptr;
	EmuSettings* _settings = nullptr;
	HdPackData* _hdData = nullptr;

	uint8_t _activeBgCount[5] = {};
	HdBgConfig _bgConfig[50] = {};

	uint32_t _palette[512] = {};
	HdPackTileInfo* _cachedTile = nullptr;
	bool _cacheEnabled = false;
	bool _useCachedTile = false;
	int32_t _scrollX = 0;
	int32_t _lineScrollX[240] = {}; // scroll PPU par ligne NES (<2DPerspective>)
	int32_t _lineScrollY[240] = {};

	// Rendu multi-coeur : passe 1 (sequentielle) = recherche des tiles HD, passe 2 (parallele) = dessin.
	SnesHdThreadPool _pool;
	vector<HdPackTileInfo*> _resolvedBg;      // 1 entree par pixel NES (tile de fond HD ou nullptr)
	vector<HdPackTileInfo*> _resolvedSprites; // 4 entrees par pixel NES (sprites HD ou nullptr)
	// Z-index par pixel NES (256x240), utilise seulement si le pack contient un <video>
	vector<uint8_t> _zNes;
	// Chronometrage des passes (journal [HD-PASS] NES toutes les 300 frames) :
	// preparation, resolution des tiles, dessin, particules, calques, distorsion, videos
	double _passMs[7] = {};
	int _passFrames = 0;
	vector<uint8_t> _ignNes;                  // 1 par pixel NES : pixel net pendant la distorsion (<Ignore2DOverlays>)
	bool _zEnabled = false;
	
	unordered_map<HdTileKey, vector<HdPackAdditionalSpriteInfo>> _additionalTilesByKey;

	template<HdPackBlendMode blendMode>
	__forceinline void BlendColors(uint8_t output[4], uint8_t input[4]);

	__forceinline uint32_t AdjustBrightness(uint8_t input[4], int brightness);
	__forceinline void DrawColor(uint32_t color, uint32_t* outputBuffer, uint32_t screenWidth);
	__forceinline void DrawTile(HdPpuTileInfo &tileInfo, HdPackTileInfo &hdPackTileInfo, uint32_t* outputBuffer, uint32_t screenWidth);
	
	__forceinline HdPackTileInfo* GetCachedMatchingTile(uint32_t x, uint32_t y, HdPpuTileInfo* tile);
	__forceinline HdPackTileInfo* GetMatchingTile(uint32_t x, uint32_t y, HdPpuTileInfo* tile, bool* disableCache = nullptr);

	__forceinline void DrawBackgroundLayer(const HdBgConfig& bgConfig, uint32_t x, uint32_t y, uint32_t* outputBuffer, uint32_t screenWidth);

	template<HdPackBlendMode blendMode>
	__forceinline void DrawCustomBackground(HdBackgroundInfo& bgInfo, uint32_t *outputBuffer, uint32_t x, uint32_t y, uint32_t screenWidth);

	void OnLineStart(HdPpuPixelInfo &lineFirstPixel);
	void ComputeLineBgConfig(HdPpuPixelInfo &lineFirstPixel, uint8_t y, HdBgConfig* lineCfg);
	void ProcessVideos(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan);
	void SimulateOverlays(HdScreenInfo* hdScreenInfo);
	void UpdateBackgroundFrames(HdScreenInfo* hdScreenInfo);
	void ProcessOverlays(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan);
	void ProcessScreenLayers(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan);
	void ProcessDistortions(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan);
	void ProcessPerspectivesLayout(uint32_t* out, uint32_t stride, uint32_t padHd, uint32_t centreHd, float originNes, uint32_t hdHeight, float topNes, float bottomNes, bool ws);
	void ProcessPerspectives(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan);
	void ProcessWidescreenPerspectives(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) override;
	void PrepareLiquids(uint32_t screenWidth);
	void DrawWidescreenBands(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) override;
	void ProcessWidescreenDistortions(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) override;
	void SetWidescreenMode(bool on, float widthNes) override { _wsMode = on; _simFieldNes = widthNes; }
	float _simFieldNes = 256.0f; // largeur du champ des particules (NES) : 256, ou largeur elargie en widescreen
	bool _wsMode = false; // vrai en widescreen : overlays et distorsions sont faits apres les bandes
	void ProcessWidescreenOverlays(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) override;
	void ProcessOverlaysLayout(uint32_t* outputBuffer, uint32_t screenWidth, uint32_t padHd, uint32_t centreHd, float originNes, float widthNes, float fieldWNes, uint32_t oLeft, uint32_t oTop, uint32_t oBottom);
	void ProcessWidescreenScreenLayers(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) override;
	void ProcessScreenLayersLayout(uint32_t* outputBuffer, uint32_t screenWidth, float originNes, uint32_t oTop, uint32_t oBottom);
	std::vector<uint8_t> _wsZ; // z des bandes (priorite du dernier fond ecrit), indexe comme le buffer de sortie
	void DrawWidescreenBgPixel(const HdBgConfig& cfg, int64_t aCol, int64_t aRow, uint32_t* dst);
	void DrawWidescreenLiquidPixel(uint32_t hb, uint32_t outWidth, float xN, float yN, size_t pixIdx, uint32_t* dst);
	void PrepareWidescreenBubbles(uint32_t outWidth, uint32_t padHd, uint32_t visRows);
	std::vector<std::vector<uint8_t>> _wsBubbleMap; // alpha des bulles par liquide, indexe comme le buffer de sortie (ligne visible x outWidth)
	std::vector<std::vector<float>> _wsLiqSurface; // surface par liquide et par colonne HD élargie, recalculée chaque image
	void DrawLiquidBand(int band, uint32_t nesX, uint32_t nesY, uint32_t* block, uint32_t screenWidth);
	uint32_t BlendLiquidPixel(const NesHdLiquidInfo& liq, float xN, float yN, float surf, uint32_t dst);
	uint32_t _liquidOffX = 0, _liquidOffY = 0;
	int32_t GetLayerIndex(uint8_t priority);
	void OnBeforeApplyFilter();

	void ProcessAdditionalSprites();
	bool DrawAdditionalTiles(int32_t x, int32_t y, HdPpuTileInfo& tile, bool checkFallbackTiles);
	void BuildAdditionalTileCache(int32_t x, int32_t y, HdPpuTileInfo& tile, bool checkFallbackTiles);
	void InsertAdditionalSprite(int32_t x, int32_t y, HdPpuTileInfo& sprite, HdPackAdditionalSpriteInfo& additionalSprite);

	__forceinline void ResolvePixel(uint32_t x, uint32_t y, HdPpuPixelInfo &pixelInfo);
	__forceinline void DrawPixel(uint32_t x, uint32_t y, HdPpuPixelInfo &pixelInfo, uint32_t *outputBuffer, uint32_t screenWidth, const HdBgConfig* lineCfg);
	__forceinline void ProcessGrayscaleAndEmphasis(HdPpuPixelInfo &pixelInfo, uint32_t* outputBuffer, uint32_t hdScreenWidth);
	
	void CleanupInvalidRules();
	void InitializeFallbackTiles();

	/* Added by Gwenolo */
	void disable_8bitsSound();

public:
	HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
	virtual ~HdNesPack();

	uint32_t GetScale() override { return scale; }
	
	void Process(HdScreenInfo *hdScreenInfo, uint32_t *outputBuffer, OverscanDimensions &overscan) override;
};
