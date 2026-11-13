#pragma once
#include "pch.h"
#include <cstdint>
#include <string>
#include <vector>
#include "Gameboy/GbDefaultVideoFilter.h"
#include "Gameboy/HdPacks/GbHdPackLoader.h"

class Emulator;

// Filtre video HD Game Boy (etapes 3 et 4 : fonds et sprites tile par tile).
// Chaque pixel de fond ou d'objet dont la tile est dans le pack est remplace par le carre scale x scale
// correspondant dans l'atlas. Le PPU a deja tranche la priorite fond/objet et les pixels couleur 0 des objets
// (jamais enregistres comme gagnants). Les autres pixels (tiles absentes) gardent le rendu par defaut.
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
	uint64_t _transparentPixels = 0;
	uint32_t _samplesLogged = 0;
	uint32_t _frameCounter = 0;

	void LoadImages();

protected:
	void OnBeforeApplyFilter() override;
	FrameInfo GetFrameInfo() override;

public:
	GbHdVideoFilter(Emulator* emu, const GbHdPackData& pack);
	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
};
