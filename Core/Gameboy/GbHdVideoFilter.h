#pragma once
#include "pch.h"
#include <cstdint>
#include <string>
#include <vector>
#include "Gameboy/GbDefaultVideoFilter.h"
#include "Gameboy/HdPacks/GbHdPackLoader.h"

class Emulator;

// Filtre video HD Game Boy (etapes 3 et 4).
// Fonds : chaque pixel de fond dont la tile est dans le pack est remplace par le bloc scale x scale de l'atlas.
// Sprites : dessines par empreinte (8x8 ou 8x16) a partir de la liste OAM de l'image. Un pixel n'est
// remplace que s'il appartient a ce sprite (ou au fond, si le PPU ne l'a pas attribue a un autre objet)
// et si le pixel de l'atlas est opaque.
class GbHdVideoFilter : public GbDefaultVideoFilter
{
private:
	GbHdPackData _pack;
	uint32_t _scale = 4;
	std::vector<std::vector<uint32_t>> _images;
	std::vector<uint32_t> _imageWidths;
	std::vector<uint32_t> _imageHeights;
	uint64_t _hdPixels = 0;
	uint64_t _hdSpritePixels = 0;
	uint64_t _fallbackPixels = 0;
	uint32_t _frameCounter = 0;

	void LoadImages();
	bool SamplePixel(const GbHdTileRef& ref, uint32_t col, uint32_t row, uint32_t dx, uint32_t dy, uint32_t& pixel);

protected:
	void OnBeforeApplyFilter() override;
	FrameInfo GetFrameInfo() override;

public:
	GbHdVideoFilter(Emulator* emu, const GbHdPackData& pack);
	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
};
