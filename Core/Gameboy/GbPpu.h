#pragma once
#include "pch.h"
#include "Gameboy/GbTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class Gameboy;
class GbMemoryManager;
class GbDmaController;

// Sprite visible de l'ecran, pour le dessin HD par empreinte
struct GbHdSpriteInfo
{
	int32_t X = 0;
	int32_t Y = 0;
	uint8_t Height = 8;
	uint8_t Flags = 0; // 2 = hflip, 4 = vflip
	bool BgPriority = false;
	uint32_t Palette = 0;
	uint64_t Hash[2] = {0, 0}; // tile de base et tile suivante (8x16)
};

// Tile d origine de chaque pixel affiche (packs HD). Layer : 0 = aucun, 1 = fond, 2 = objet
struct GbHdPixelInfo
{
	uint64_t Hash = 0;
	uint32_t Palette = 0;
	uint8_t Flags = 0; // 2 = hflip, 4 = vflip
	uint8_t Row = 0;   // ligne dans la tile (0-7)
	uint8_t Col = 0;   // colonne dans la tile (0-7)
	uint8_t Layer = 0;
	uint8_t BgColor = 0; // couleur du fond (0-3) quand Layer = 1
	uint16_t BgZero = 0;   // RGB555 de la couleur 0 de la palette de fond (<disableOriginalTiles>)
	uint16_t Fallback = 0; // RGB555 affiche si la tile HD est transparente (fond : couleur 0 ; sprite : fond sous le pixel)
	uint8_t BgPriority = 0; // 1 = tile de fond avec le bit de priorite GBC (0x80) : au-dessus des sprites
};

// Donnees HD transmises avec chaque image : tampon des pixels et liste des sprites
struct GbHdFrameData
{
	const GbHdPixelInfo* Pixels = nullptr;
	const std::vector<GbHdSpriteInfo>* Sprites = nullptr;
	const std::vector<uint8_t>* Memory = nullptr; // octets CPU des adresses surveillees (fin de l'image)
	uint8_t ScrollX = 0; // SCX de fin d'image (pour <background>)
	uint8_t ScrollY = 0; // SCY de fin d'image
	uint32_t FrameNumber = 0; // compteur d'images de la PPU (condition frameRange)
};

class GbPpu : public ISerializable
{
private:
	Emulator* _emu = nullptr;
	Gameboy* _gameboy = nullptr;
	GbPpuState _state = {};
	GbMemoryManager* _memoryManager = nullptr;
	GbDmaController* _dmaController = nullptr;
	uint16_t* _outputBuffers[2] = {};
	uint16_t* _currentBuffer = nullptr;

	uint16_t* _eventViewerBuffers[2] = {};
	uint16_t* _currentEventViewerBuffer = nullptr;
	EvtColor _evtColor = EvtColor::HBlank;
	int16_t _prevDrawnPixels = 0;

	uint8_t* _vram = nullptr;
	uint8_t* _oam = nullptr;

	uint64_t _lastFrameTime = 0;

	GbPpuFifo _bgFifo;
	GbPpuFetcher _bgFetcher;

	GbPpuFifo _oamFifo;
	GbPpuFetcher _oamFetcher;

	int16_t _drawnPixels = 0;
	uint8_t _fetchColumn = 0;
	bool _fetchWindow = false;
	int16_t _windowCounter = -1;
	bool _wyEnableFlag = false;
	bool _wxEnableFlag = false;
	bool _insertGlitchBgPixel = false;

	int16_t _fetchSprite = -1;
	uint8_t _spriteCount = 0;
	uint8_t _spriteX[10] = {};
	uint8_t _spriteY[10] = {};
	uint8_t _spriteIndexes[10] = {};
	uint8_t _oamReadBuffer[2] = {};
	
	bool _lcdDisabled = true;
	bool _stopOamBlocked = false;
	bool _stopVramBlocked = false;
	bool _stopPaletteBlocked = false;

	bool _oamReadBlocked = false;
	bool _oamWriteBlocked = false;
	bool _vramReadBlocked = false;
	bool _vramWriteBlocked = false;

	bool _isFirstFrame = true;
	bool _forceBlankFrame = true;
	bool _rendererIdle = false;

	uint8_t _tileIndex = 0;
	uint8_t _gbcTileGlitch = 0;

	GbPixelType _lastPixelType = {};
	uint8_t _lastBgColor = 0;
	std::vector<GbHdPixelInfo> _hdInfo[2];
	std::vector<GbHdSpriteInfo> _hdSprites[2];
	GbHdFrameData _hdFrame[2];
	std::vector<uint8_t> _hdMem[2];
	GbHdPixelInfo* _hdCur = nullptr;
	int _hdIndex = 0;
	bool _hdEnabled = false;
	uint32_t _hdLogCounter = 0;

	__forceinline void WriteBgPixel(uint8_t colorIndex);
	__forceinline void WriteObjPixel(uint8_t colorIndex);
	uint16_t BgZeroColor(const GbFifoEntry& e);
	uint16_t BgRawColor(const GbFifoEntry& e);
	__forceinline void RecordHdPixel(const GbFifoEntry& e, uint8_t layer, uint16_t fallback, uint16_t bgZero);

	__forceinline void ProcessPpuCycle();

	__forceinline void ExecCycle();
	__forceinline void ProcessVblankScanline();
	void ProcessFirstScanlineAfterPowerOn();
	__forceinline void ProcessVisibleScanline();
	__forceinline void RunDrawCycle();
	__forceinline void RunSpriteEvaluation();
	void ResetRenderer();
	void ClockSpriteFetcher();
	void FindNextSprite();
	__forceinline void ClockTileFetcher();
	__forceinline void PushSpriteToPixelFifo();
	__forceinline void PushTileToPixelFifo();

	void UpdateStatIrq();

	__forceinline uint8_t LcdReadOam(uint8_t addr);
	__forceinline uint8_t LcdReadVram(uint16_t addr);
	__forceinline uint16_t LcdReadBgPalette(uint8_t addr);
	__forceinline uint16_t LcdReadObjPalette(uint8_t addr);

	void SendFrame();
	void UpdatePalette();

	uint8_t ReadCgbPalette(uint8_t& pos, uint16_t* pal);
	void WriteCgbPalette(uint8_t& pos, uint16_t* pal, bool autoInc, uint8_t value);

public:
	virtual ~GbPpu();

	void Init(Emulator* emu, Gameboy* gameboy, GbMemoryManager* memoryManager, GbDmaController* dmaController, uint8_t* vram, uint8_t* oam);

	GbPpuState GetState();
	GbPpuState& GetStateRef();
	uint16_t* GetOutputBuffer();
	uint16_t* GetEventViewerBuffer();
	uint16_t* GetPreviousEventViewerBuffer();

	void SetCpuStopState(bool stopped);

	uint32_t GetFrameCount();
	uint8_t GetScanline();
	uint16_t GetCycle();
	bool IsLcdEnabled();
	bool IsCgbEnabled();
	PpuMode GetMode();

	template<bool singleStep>
	void Exec();

	uint8_t Read(uint16_t addr);
	void Write(uint16_t addr, uint8_t value);

	void SetTileFetchGlitchState();

	bool IsVramReadAllowed();
	bool IsVramWriteAllowed();
	uint8_t ReadVram(uint16_t addr);
	uint8_t PeekVram(uint16_t addr);
	void WriteVram(uint16_t addr, uint8_t value);

	bool IsOamReadAllowed();
	bool IsOamWriteAllowed();
	uint8_t ReadOam(uint8_t addr);
	uint8_t PeekOam(uint8_t addr);
	void WriteOam(uint8_t addr, uint8_t value, bool forDma);

	template<GbOamCorruptionType oamCorruptionType>
	void ProcessOamCorruption(uint16_t addr);

	void ProcessOamIncDecCorruption(int row);

	uint8_t ReadCgbRegister(uint16_t addr);
	void WriteCgbRegister(uint16_t addr, uint8_t value);

	void DebugSendFrame();

	void Serialize(Serializer& s) override;
};
