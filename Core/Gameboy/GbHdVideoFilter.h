#pragma once
#include "pch.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Gameboy/GbDefaultVideoFilter.h"
#include "Gameboy/HdPacks/GbHdPackLoader.h"
#include "Gameboy/HdPacks/GbHdThreadPool.h"
#include "Shared/HdDayNight.h"

struct HdDioramaConfig;

class Emulator;
struct GbHdPixelInfo;
struct GbHdSpriteInfo;

// Tile trouvee pour une cle (hash, palette, flags). MirrorH / MirrorV : l'image du pack est dans l'autre sens,
// on la retourne a l'affichage (comme NES / SNES : une seule image sert aux deux sens).
struct GbHdTileMatch
{
	const GbHdTileRef* Ref = nullptr;
	bool MirrorH = false;
	bool MirrorV = false;
};

// Filtre video HD Game Boy (etapes 3, 4a, 5, repli de miroir, <background> et niveaux Z).
// Fonds : chaque pixel de fond dont la tile est dans le pack est remplace par le bloc scale x scale de l'atlas.
// Sprites : dessines par empreinte (8x8 ou 8x16) a partir de la liste OAM de l'image.
// Conditions (etape 5) : une tile conditionnelle ([termes]<tile...>) est choisie si ses termes sont vrais ;
// la premiere ligne vraie du fichier gagne. Sinon la tile sans condition est utilisee.
// Repli de miroir : si la tile exacte n'existe pas, la tile du pack dans l'autre sens est retournee.
// <background> (signature SNES) : image ou dossier d'animation, remplace la couche de fond SD (tiles comprises),
// luminosite, alpha, offsets, repetition X/Y, operateur de scroll. Z < spriteZ : derriere les sprites.
// Tiles : bit de priorite GBC = spriteZ + 10 (devant les sprites), sinon spriteZ - 10 (derriere).
class GbHdVideoFilter : public GbDefaultVideoFilter
{
private:
	GbHdThreadPool _pool; // reserve de threads permanente (comme SNES)
	GbHdPackData _pack;
	uint32_t _scale = 4;
	std::vector<std::vector<uint32_t>> _images;
	std::vector<uint32_t> _imageWidths;
	std::vector<uint32_t> _imageHeights;
	uint64_t _hdPixels = 0;
	uint64_t _hdSpritePixels = 0;
	uint64_t _fallbackPixels = 0;
	uint64_t _bgPixels = 0;
	uint64_t _bgFrame = 0; // compteur d'images pour l'animation et le defilement automatique
	uint32_t _frameCounter = 0;
	const GbHdPixelInfo* _frameInfo = nullptr;                 // tampon de l'image courante (valide pendant ApplyFilter)
	const std::vector<GbHdSpriteInfo>* _frameSprites = nullptr; // sprites de l'image courante
	std::map<std::string, bool> _condResult; // resultat de chaque condition pour l'image, calcule avant Run (lu par les threads)
	std::map<std::string, int32_t> _varCurrent; // [fork GB] <setVariable> : valeur courante (index, -1 = aucune)
	std::map<std::string, int32_t> _varLogged; // [fork GB] derniere valeur journalisee
	std::vector<uint32_t> _under; // couleur sous chaque pixel HD de sprite (fond Game Boy + fonds derriere les sprites)
	uint32_t _frameNumber = 0; // image courante, pour frameRange (ecrit avant Run, lu par les threads)

	void LoadImages();
	bool SamplePixel(const GbHdTileRef& ref, bool mirrorH, bool mirrorV, uint32_t col, uint32_t row, uint32_t dx, uint32_t dy, uint32_t& pixel);
	bool SampleBackgroundPixel(uint32_t imageIndex, const GbHdBackground& bg, int64_t offX, int64_t offY, uint32_t x, uint32_t y, uint32_t& pixel) const;
	bool EvaluateCondition(const GbHdCondition& cond, const std::vector<uint8_t>* mem, const GbHdPixelInfo* info, const std::vector<GbHdSpriteInfo>* sprites) const;
	bool EvaluateTerms(const std::vector<GbHdTerm>& terms, uint8_t flags, const std::vector<uint8_t>* mem, int32_t ox, int32_t oy, bool hasCtx) const;
	bool NearbyAt(const GbHdCondition& c, int32_t ox, int32_t oy, uint8_t flags) const;
	GbHdTileMatch FindTile(uint64_t hash, uint32_t palette, uint8_t flags, const std::vector<uint8_t>* mem, int32_t ox, int32_t oy, bool hasCtx) const;
	std::vector<const GbHdBackground*> FindBackgrounds(const std::vector<uint8_t>* mem) const;
	// <2DOverlay> (docs/claude/09_2DOverlay.md) : simulation une fois par image, puis particules (test de Z par pixel SD),
	// calques plein ecran et distorsion sur l'image finale
	void SimulateOverlays(int32_t scrollX, int32_t scrollY, const std::vector<uint8_t>* mem);
	void RenderOverlayParticles(uint32_t* out, std::vector<uint8_t>& zmap);
	void RenderOverlayLayers(uint32_t* out);
	void RenderOverlayDistortions(uint32_t* out);
	// <2DSpriteLight> : halos au centre des tiles de sprite reconnues
	void RenderSpriteLights(uint32_t* out, const std::vector<GbHdSpriteInfo>* sprites, const std::vector<uint8_t>* mem);

	// ---- Diorama Game Boy (doc 56, GbHdDiorama.cpp) : memes balises et meme moteur GPU (HdGpuCore) que le diorama NES ----
	// Couches (ARGB premultiplie, taille de la sortie) : L0 couleur 0 du fond + <background> arriere, L1 sprites derriere le fond (OAM
	// priorite), L2 tiles de fond + <background> intermediaires, L3 sprites + <background> avant. _dioFix : fenetre (HUD) a plat.
	std::vector<uint32_t> _dioL[4];
	std::vector<uint32_t> _dioFix;
	// Groupes <DioramaDepth> (G7) : image complete par groupe (ARGB premultiplie) + lignes SD non vides ; vides si le pack n'en declare pas
	std::vector<uint32_t> _dioG[8];
	std::vector<uint8_t> _dioGLines[8];
	// Ombres des sprites (G6c) : silhouettes (ARGB premultiplie), lignes SD utilisees, numero de regle le plus petit par ligne (-1 = aucune)
	std::vector<uint32_t> _dioS;
	std::vector<uint8_t> _dioSLines;
	std::vector<int> _dioSRule;
	uint32_t _dioFixY0 = 0, _dioFixRows = 0; // lignes HD de la fenetre
	uint32_t _dioBackdrop = 0xFF000000;      // couleur 0 du fond (hors des plans)
	bool _dioGpuTried = false, _dioGpuOk = false;
	int _dioFail = 0;
	bool _dioTopDown = false;
	float _dioFloorMargin = -1.0f;
	float _dioWX = 0.0f, _dioWY = 0.0f; // defilement cumule (SCX / SCY deplies) : repere du monde des nuages, brumes, particules
	int _dioLastSX = -1, _dioLastSY = -1;
	float _camFrom[4] = {}, _camTo[4] = {}, _camCur[4] = {};
	float _camT = 0.0f, _camDur = 1.0f;
	bool _camInit = false;
	int _camDefault = -1, _camManual = -1;
	std::vector<uint8_t> _camKeyDown;
	bool _camKeyWarned = false;
	float _padYaw = 0.0f, _padPitch = 0.0f, _padZoom = 0.0f;
	uint32_t _dioOrbit = 0;
	float _dioSkyX = 0.0f, _dioSkyY = 0.0f;
	const void* _dioSkyCubeKey = nullptr;
	std::vector<uint32_t> _dioSkyCube;
	uint32_t _dioSkyCubeF = 0;
	std::vector<uint8_t> _dioFoot;
	std::vector<float> _dioObjSpin;
	struct P3Part { float X = 0, Y = 0, Z = 0, VX = 0, VY = 0, VZ = 0, Age = 0, Life = 0, Size = 1, Seed1 = 0, Seed2 = 0, EVX = 0, EVY = 0; };
	struct P3State { std::vector<P3Part> Parts; bool Init = false; bool TopDown = false; float Time = 0, SpawnAcc = 0; uint32_t Rng = 0x9E3779B9; };
	std::vector<P3State> _p3State;
	std::vector<float> _p3Out[4];
	// <DayNight> (doc 55 / 56)
	std::vector<HdDayNightState> _dnState;
	HdDayNightOut _dn;
	bool _dnDioApplied = false;
	const std::vector<uint8_t>* _dioMem = nullptr; // memoire de l'image (conditions des regles diorama)
	// <background> dessines par le GPU (comme le doc 52 NES) : plus d'echantillonnage par pixel ni d'envoi d'image. Kind : 0 arriere
	// (devant L0), 1 intermediaire (devant L2), 2 avant (devant L3) ; OffX / OffY en pixels HD
	struct DioBgGpu { uint32_t Image = 0; int64_t OffX = 0, OffY = 0; float Bright = 1.0f; bool TileX = false, TileY = false; int Kind = 0; };
	std::vector<DioBgGpu> _dioBgGpu;

	bool DioramaActive() const;
	bool DioramaSkyWanted();
	void DioramaCamera(const HdDioramaConfig& cfg, float& yaw, float& pitch, float& dist, float& zoom);
	bool DioramaSubmit(uint32_t* out);
	void UpdateDayNight();
	void ApplyDayNight2D(uint32_t* out, const GbHdPixelInfo* info);

protected:
	void OnBeforeApplyFilter() override;
	FrameInfo GetFrameInfo() override;

public:
	GbHdVideoFilter(Emulator* emu, const GbHdPackData& pack);
	~GbHdVideoFilter();
	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
};
