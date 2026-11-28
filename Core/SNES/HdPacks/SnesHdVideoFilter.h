#pragma once

#include "pch.h"
#include <chrono>
#include "SNES/SnesDefaultVideoFilter.h"
#include "SNES/HdPacks/SnesHdThreadPool.h"
#include "SNES/HdPacks/SnesHdDioramaRun.h"

struct SnesHdPixelData;

class SnesHdPackData;

// On hérite du filtre par défaut pour récupérer sa gestion des couleurs et de la luminosité
class SnesHdVideoFilter : public SnesDefaultVideoFilter
{
private:
	FrameInfo _sdFrameInfo = {}; // Pour garder en mémoire la taille basse résolution
	std::vector<uint8_t> _zBuffer; // La carte de profondeur HD
	SnesHdThreadPool _pool; // Reserve de threads permanente (passes du filtre)
	SnesHdDioRuntimePtr _dio; // diorama SNES (doc 59) : etat de la scene (cree au premier usage)
	std::vector<uint32_t> _dioBgCol; // diorama : <background> du pack empiles (ARGB), sans test de profondeur
	std::vector<uint8_t> _dioBgZ;    // diorama : Z du fond opaque le plus haut de chaque pixel
	std::vector<uint32_t> _dioTrCol; // diorama : dernier pixel de tile semi-transparent (ARGB non premultiplie)
	std::vector<uint8_t> _dioTrZ;    // diorama : Z de ce pixel
	std::vector<uint8_t> _dioTrG, _dioOwnG; // diorama : groupe <DioramaDepth> du bord / de la derniere tile opaque posee
	double _dioCapMs = 0.0, _dioRunMs = 0.0; int _dioFrames = 0; // diorama : chronometrage (journal toutes les 300 images)
	std::vector<uint16_t> _dioOwnSl, _dioTrSl; // diorama : regles <DioramaSprites>tile= (indice + 1) de la derniere tile opaque / du bord
	std::shared_ptr<SnesHdPackData> _framePack; // Snapshot du pack HD pour la frame en cours (evite la course avec LoadHdPack)
	// Chronometrage des passes (journal [HD-PASS] toutes les 300 frames) : m7, compose, reflets, eclairage, atmosphere
	double _passMs[5] = {};
	int _passFrames = 0;
	double _filterMs = 0; // duree totale de ApplyFilter (journal [HD-PASS] filtre)
	double _idleMs = 0;   // temps ou le filtre attend la frame suivante de l'emulation
	std::chrono::steady_clock::time_point _lastEnd;
	bool _hasLastEnd = false;
	double _gpuSubmitMs = 0; // envoi des modeles a la carte graphique
	double _gpuFetchMs = 0;  // recuperation de l'image des modeles
	double _tailMs = 0;      // fin de l'image : flares, videos, alpha
	double _prepBeforeMs = 0; // preparation avant la passe m7 (phase 1 Mode 7, materiaux, releve des tiles)
	double _prepAfterMs = 0;  // collecte et tri des tiles et sprites, entre m7 et la composition
	double _prepLoopMs = 0;   // boucle sur les pixels SNES (tiles de base, sprites, pixels)
	double _prepAddMs = 0;    // additions HD et sprites (ETAPE 2 et debut de l'ETAPE 3)
	double _prepSortMs = 0;   // taches, tri et debut de la composition
	double _prepEffectsMs = 0; // preparation des effets (lumieres, reliefs, flares...) avant la liste des taches


// Sous-fonction pour le rendu standard (via le PPU)
	void ApplyFilterSpritesStandard(uint32_t x, uint32_t y, uint32_t scale, uint32_t outWidth,
									  uint32_t sdColor, const SnesHdPixelData& hdData, // Plus de "struct" ici !
									  uint8_t pixelZ, uint32_t* out);

//La méthode OAM Scan (Bypass du clipping)
	void ApplyFilterSpritesOAMScan(uint32_t scale, uint32_t outWidth, uint32_t outHeight, uint32_t* out);

protected:
	FrameInfo GetFrameInfo() override;
	uint32_t GetWidescreenExtraWidth() override;

public:
	SnesHdVideoFilter(Emulator* emu);
	void ApplyFilter(uint16_t* ppuOutputBuffer) override;
	OverscanDimensions GetOverscan() override;
}; 