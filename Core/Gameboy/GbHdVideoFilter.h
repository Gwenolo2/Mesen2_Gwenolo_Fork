#pragma once
#include "pch.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Gameboy/GbDefaultVideoFilter.h"
#include "Gameboy/HdPacks/GbHdPackLoader.h"
#include "Gameboy/HdPacks/GbHdThreadPool.h"

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

protected:
	void OnBeforeApplyFilter() override;
	FrameInfo GetFrameInfo() override;

public:
	GbHdVideoFilter(Emulator* emu, const GbHdPackData& pack);
	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
};
