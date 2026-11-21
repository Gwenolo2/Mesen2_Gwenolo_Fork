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
	// Widescreen + diorama : copie l'image large du GPU (bandes comprises) dans out ; false = pas d'image (le centre et les bandes de fonds restent a faire)
	virtual bool TakeDioramaWide(uint32_t* out, size_t count) = 0;
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
		int32_t RawScrollX = 0; // scroll PPU brut de la ligne (pour <2DTexture>)
		int32_t RawScrollY = 0;
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
	uint32_t _dioramaBurst = 0; // images restantes du journal [HD-DIORAMA] (declenche par hd_debug_dump.flag)
	uint32_t _dioramaCheck = 0;
	uint32_t _dioramaFrame = 0;
	uint32_t _dioramaTail = 0;
	vector<int32_t> _dioramaPrevVals;
	int32_t _dioramaPrevMaxX = 0;
	vector<uint8_t> _sprNes;   // <2DReflect> : 1 par pixel NES si un sprite opaque y est affiche
	vector<float> _reflFloor;  // <2DReflect> : ordonnee ecran (NES) du sol par pixel, NaN = pas de sol
	bool _reflectEnabled = false;
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
	__forceinline void DrawCustomBackground(HdBackgroundInfo& bgInfo, uint32_t *outputBuffer, uint32_t x, uint32_t y, uint32_t screenWidth, uint32_t screenX, uint32_t screenY, int32_t rawScrollX, int32_t rawScrollY, const vector<NesHdTextureInfo>* tex2d);
	void ComputeReflectFloorLine(uint32_t nesY, const HdBgConfig* lineCfg, uint32_t xStart, uint32_t xEnd);
	void ProcessReflections(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan);
	uint32_t ApplyTexture2D(const vector<NesHdTextureInfo>& rules, uint32_t raw, int32_t hdX, int32_t hdY, int32_t rawScrollX, int32_t rawScrollY);

	void OnLineStart(HdPpuPixelInfo &lineFirstPixel);
	void LogDioramaDiag(HdScreenInfo* hdScreenInfo, uint32_t yStart, uint32_t yEnd);

	// G0 : test du contexte GPU commun (doc 34). Declenche par un fichier hd_gpu_test.flag dans le dossier du pack.
	uint32_t _gpuTestBurst = 0, _gpuTestCheck = 0, _gpuTestFrame = 0, _gpuTestFrames = 0;
	double _gpuTestMs = 0;
	size_t _gpuTestThread = 0;
	vector<uint32_t> _gpuTestImg;
	uint32_t _gpuTestCamBad = 0;

	// G2a : couches du diorama (doc 36). Actives seulement si le pack contient <Diorama>. L0 = couleur de fond PPU,
	// L1 = sprites a priorite fond, L2 = tiles de fond, L3 = sprites de premier plan ; ARGB premultiplie, taille = image de sortie.
	vector<uint32_t> _dioL[4];
	// G3c : couche fixe "off camera" (doc 38). _dioOffMask : 1 par pixel NES ((y << 8) | x) dans une zone <DioramaOffCamera>.
	// Dans ces zones, L1 a L3 sont dessinees dans _dioL4. _dioL4Y0 / _dioL4Rows : lignes HD concernees (0 ligne = aucune zone).
	vector<uint32_t> _dioL4;
	vector<uint8_t> _dioOffMask;
	bool _dioOffReady = false;
	// G3e : couches <DioramaDepth> (doc 38). _dioG[g] : tiles de fond du groupe g (0 a 3) ; _dioDepthMask : groupe (1 a 4) de chaque
	// pixel NES pour les zones rect=, 0 = aucun ; _dioGLines[g][y] = 1 si la ligne NES y a recu un pixel du groupe.
	static constexpr int DioGroups = 8; // groupes <DioramaDepth> (doit valoir HdDioMaxGroups de HdGpuCore.h)
	vector<uint32_t> _dioG[8];
	vector<uint8_t> _dioFoot; // topDown : pied des sprites (256 octets par ligne du plan, ligne du bas de la silhouette + 1)
	vector<uint32_t> _dioGMask[8]; // topDown : pixels opaques de tiles routees vers le groupe (sans fond ni couleur de fond), alpha 255
	vector<uint32_t> _dioWorldTmp; // doc 53 : tampon du fond L0 pour worldtex= (projection sur les tiles du groupe)
	vector<uint8_t> _dioDepthMask;
	vector<uint8_t> _dioGLines[8];
	// G3h : lignes NES ou L1 ([0]) et L3 ([1]) ont recu un sprite a cette image, lignes a renvoyer au GPU, image entiere a renvoyer
	vector<uint8_t> _dioLLines[2];
	vector<uint8_t> _dioLDirty[2];
	bool _dioLFull = true;
	uint64_t _dioLKey = 0;
	vector<uint8_t> _dioT2oHide;               // Tile2Obj (doc 53) : pixels dont la tile est remplacee par un modele (diorama seulement)
	vector<HdDioramaObjectRule> _dioT2oRules;  // Tile2Obj : instances de cette image
	void DetectTile2Obj();
	void DetectSprite2Obj();
	std::unordered_map<uint64_t, std::shared_ptr<vector<float>>> _s2oBuf;          // <Sprite2Obj> anime : sommets deformes (regle, silhouette, partie)
	std::unordered_map<std::string, std::pair<uint32_t, float>> _s2oTime;          // id= -> (derniere image, temps du clip en secondes)
	std::vector<float> _s2oPal;                                                   // palette d'os de la silhouette en cours
	uint32_t _s2oVersion = 0;
	vector<uint8_t> _s2oMask; // <Sprite2Obj> : par pixel NES, regle (+1) dont la silhouette a ete remplacee par un objet (0 = aucune)
	bool S2oHidden(uint32_t pixelIndex, const HdPpuTileInfo& t) const // sprite cache : sa silhouette est un objet et sa tile est dans la regle (hide=on)
	{
		if(pixelIndex >= _s2oMask.size() || _s2oMask[pixelIndex] == 0) return false;
		const HdDioramaTile2ObjRule& r = _hdData->Sprite2ObjRules[_s2oMask[pixelIndex] - 1];
		if(!r.Hide || (!r.AnyPalette && r.Palette != t.PaletteColors)) return false;
		for(uint32_t h : r.Group) { if(h == (uint32_t)t.TileIndex) return true; }
		return false;
	}
	vector<float> _dioObjSpin;       // H1 : angle de rotation continue (degres) de chaque regle <DioramaObject>
	vector<uint32_t> _dioNrm[10];       // G6d : normales (L2, L3, groupes 1 a 4), meme taille que les couches
	vector<uint8_t> _dioNrmLines[10];  // G6d : 1 = la ligne NES a recu une normale
	vector<uint32_t> _dioS;          // G6c : masque des silhouettes des sprites qui projettent une ombre (meme taille que les couches)
	vector<uint8_t> _dioSLines;     // G6c : 1 = la ligne NES a recu un pixel du masque
	vector<int> _dioSRule;          // G6c : plus petit numero de regle d'ombre vu sur la ligne NES (-1 = aucun)
	vector<uint32_t> _dioSkyCube; // G5b : cubemap en 6 fichiers assemblee en une bande 6 x 1
	const void* _dioSkyCubeKey = nullptr;
	uint32_t _dioSkyCubeF = 0;
	float _dioSkyX = 0.0f, _dioSkyY = 0.0f; // G5 : derive cumulee de la skybox image (pixels NES)
	uint64_t _dioGKey = 0; // G3g : geometrie des tampons de groupes (effacement partiel)
	bool _dioDepthReady = false;
	uint32_t _dioWallLog = 0; // G3b2 : couches (bit 0 a 7) dont le motif de mur a deja ete signale au journal
	uint32_t _dioL4Y0 = 0, _dioL4Rows = 0;
	uint32_t _dioFrame = 0;
	void DrawTileLayer(HdPpuTileInfo& tileInfo, HdPackTileInfo& hdPackTileInfo, uint32_t* outputBuffer, uint32_t screenWidth);
	void DrawNormalLayer(HdPpuTileInfo& tileInfo, HdPackTileInfo& hdPackTileInfo, uint32_t* outputBuffer, uint32_t screenWidth); // G6d
	void DrawPixelLayers(uint32_t x, uint32_t y, HdPpuPixelInfo& pixelInfo, size_t offset, uint32_t screenWidth, const HdBgConfig* lineCfg);
	void RunDioramaLayers(HdScreenInfo* hdScreenInfo, const uint32_t* outputBuffer, uint32_t screenWidth, uint32_t yStart, uint32_t yEnd, uint32_t xStart, uint32_t xEnd, bool compare);
	// G2b : envoi des couches au GPU, composition a plat, relecture ; la sortie est remplacee par l'image GPU.
	void DioramaGpuStep(HdScreenInfo* hdScreenInfo, uint32_t* outputBuffer, uint32_t screenWidth, uint32_t outHeight, uint32_t yStart, uint32_t yEnd, bool compare);
	vector<uint32_t> _dioGpuImg;
	bool _dioGpuTried = false, _dioGpuOk = false;
	double _dioMsLayers = 0, _dioMsGpu = 0, _dioMsCopy = 0;
	uint32_t _dioN = 0, _dioFail = 0;
	// Fonds de L0 dessines par le GPU (doc 52) : identifiants (priorite, index) et parametres du plan
	struct DioGpuBg { int Prio = 0, Index = 0; const uint32_t* Pix = nullptr; uint32_t W = 0, H = 0; float OffX = 0, OffY = 0, Bright = 1.0f; bool TileX = false, TileY = false; float Incl = 0.0f; int TexPrio = 0; int32_t RawX = 0, RawY = 0; };
	float _dioFloorMargin = -1.0f; // <DioramaView> floormargin= (topDown), -1 = defaut
	bool _dioDrawing = false; // vrai pendant RunDioramaLayers : <2DTexture> diorama=off ignore ; hors diorama, diorama=only ignore
	DioGpuBg _dioGpuBg[4];
	int _dioGpuBgN = 0;
	int DioGpuTexCount(int prio) const { int k = 0; for(const NesHdTextureInfo& r : _hdData->Textures2D[prio]) { if(r.DioMode != 2) k++; } return k; }
	bool IsDioGpuBg(int prio, int index) const { for(int i = 0; i < _dioGpuBgN; i++) { if(_dioGpuBg[i].Prio == prio && _dioGpuBg[i].Index == index) return true; } return false; }
	std::string _dioWideLog;
	std::vector<uint32_t> _dioL0Wide; // widescreen : L0 elargie
	bool _dioL0Clear = false; // skybox active : L0 commence transparente (la couleur de fond PPU n'est pas dessinee), le ciel apparait derriere les fonds
	uint32_t _dioOrbit = 0; // compteur d'images de l'orbite (G3a)
	// Cameras du diorama (doc 49) : camera par defaut, camera choisie au clavier, deplacement lisse entre deux reglages
	bool _camInit = false;
	bool _dioTopDown = false; // doc 53 : une regle <DioramaView>topDown est vraie (ce tour)
	int _camDefault = -1, _camManual = -1;
	float _camFrom[4] = { 0, 0, 0, 1 }, _camTo[4] = { 0, 0, 0, 1 }, _camCur[4] = { 0, 0, 0, 1 };
	float _camT = 0.0f, _camDur = 1.0f;
	vector<uint8_t> _camKeyDown;
	bool _camKeyWarned = false;
	void DioramaCameraStep(const HdDioramaConfig& cfg, float& yaw, float& pitch, float& dist, float& zoom);
	float _padYaw = 0.0f, _padPitch = 0.0f, _padZoom = 0.0f; // camera pilotee a la manette (lissee) : degres, degres, log2 du facteur de zoom
	void GpuTestG0(uint32_t* outputBuffer, uint32_t screenWidth, uint32_t outHeight, uint32_t yStart, uint32_t yEnd);
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
	bool TakeDioramaWide(uint32_t* out, size_t count) override
	{
		if(!_dioWsReady || _dioWsImg.size() != count) return false;
		memcpy(out, _dioWsImg.data(), count * sizeof(uint32_t));
		return true;
	}
	vector<uint32_t> _dioWsImg; // widescreen + diorama : image GPU large
	bool _dioWsReady = false;
	float _simFieldNes = 256.0f; // largeur du champ des particules (NES) : 256, ou largeur elargie en widescreen
	bool _wsMode = false; // vrai en widescreen : overlays et distorsions sont faits apres les bandes
	void ProcessWidescreenOverlays(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) override;
	void ProcessOverlaysLayout(uint32_t* outputBuffer, uint32_t screenWidth, uint32_t padHd, uint32_t centreHd, float originNes, float widthNes, float fieldWNes, uint32_t oLeft, uint32_t oTop, uint32_t oBottom);
	void ProcessWidescreenScreenLayers(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom) override;
	void ProcessScreenLayersLayout(uint32_t* outputBuffer, uint32_t screenWidth, float originNes, uint32_t oTop, uint32_t oBottom);
	std::vector<uint8_t> _wsZ; // z des bandes (priorite du dernier fond ecrit), indexe comme le buffer de sortie
	void DrawWidescreenBgPixel(const HdBgConfig& cfg, int64_t aCol, int64_t aRow, uint32_t* dst);
	void DrawWidescreenBgSpan(const HdBgConfig& cfg, int64_t aCol, int64_t aRow, size_t count, uint32_t* dst); // memes pixels, une suite de colonnes (ligne calculee une fois)
	double _dioMsWide = 0; uint64_t _dioWideBytes = 0;
	void DrawWidescreenLiquidPixel(uint32_t hb, uint32_t outWidth, float xN, float yN, size_t pixIdx, uint32_t* dst);
	void PrepareWidescreenBubbles(uint32_t outWidth, uint32_t padHd, uint32_t visRows);
	std::vector<std::vector<uint8_t>> _wsBubbleMap; // alpha des bulles par liquide, indexe comme le buffer de sortie (ligne visible x outWidth)
	std::vector<std::vector<float>> _wsLiqSurface; // surface par liquide et par colonne HD élargie, recalculée chaque image
	void DrawLiquidBand(int band, uint32_t nesX, uint32_t nesY, uint32_t* block, uint32_t screenWidth);
	uint32_t BlendLiquidPixel(const NesHdLiquidInfo& liq, float xN, float yN, float surf, uint32_t dst);
	uint32_t _liquidOffX = 0, _liquidOffY = 0;
	// <2DLiquid> mask=1 : couverture des <background> par priorite (alpha HD), remise a zero chaque image
	std::vector<uint8_t> _liqCover[50];
	int64_t _liqKey[50];                 // maskcolor par priorite (-1 = aucune)
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
