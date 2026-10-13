#pragma once

#include "pch.h"
#include "SNES/SnesDefaultVideoFilter.h"

struct SnesHdPixelData;

// On hérite du filtre par défaut pour récupérer sa gestion des couleurs et de la luminosité
class SnesHdVideoFilter : public SnesDefaultVideoFilter
{
private:
	FrameInfo _sdFrameInfo = {}; // Pour garder en mémoire la taille basse résolution
	std::vector<uint8_t> _zBuffer; // La carte de profondeur HD


// Sous-fonction pour le rendu standard (via le PPU)
	void ApplyFilterSpritesStandard(uint32_t x, uint32_t y, uint32_t scale, uint32_t outWidth,
									  uint32_t sdColor, const SnesHdPixelData& hdData, // Plus de "struct" ici !
									  uint8_t pixelZ, uint32_t* out);

//La méthode OAM Scan (Bypass du clipping)
	void ApplyFilterSpritesOAMScan(uint32_t scale, uint32_t outWidth, uint32_t outHeight, uint32_t* out);

protected:
	FrameInfo GetFrameInfo() override;

public:
	SnesHdVideoFilter(Emulator* emu);
	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
	OverscanDimensions GetOverscan() override;
}; 