#pragma once
// Diorama SNES (doc 59) : interface legere pour le filtre HD (n'inclut pas les en-tetes NES).
// Le filtre compose l'image 2D comme d'habitude, puis appelle SnesHdDioRun juste avant le masquage des bandes (ETAPE 5) :
// l'image composee, le Z de chaque pixel HD et le releve PPU par source (SnesHdDioPixel) donnent les couches L0..L3 (+ fixe),
// envoyees au moteur GPU commun (Shared/HdGpuCore). Faux : diorama inactif ou en echec, l'image 2D reste intacte.
#include <cstdint>
#include <memory>

struct SnesHdDioPixel;
struct SnesHdScreenInfo;
class SnesHdPackData;
class SnesHdThreadPool;

struct SnesHdDioFrame
{
	uint32_t* Out = nullptr;         // image composee (ARGB), remplacee par la scene si le diorama reussit
	const uint8_t* Z = nullptr;      // Z de chaque pixel HD (meme taille que Out)
	uint32_t OutW = 0, OutH = 0;     // pixels HD
	uint32_t Scale = 1;
	int SrcX = 0, SrcY = 0, DstX = 0, DstY = 0; // pixels SNES : decalages de lecture / d'ecriture (overscan)
	uint32_t SdWidth = 256;          // largeur de l'image SNES (512 en haute resolution : diorama coupe)
	bool Wide = false;               // widescreen actif (diorama coupe pour l'instant)
	const SnesHdDioPixel* Rec = nullptr;  // releve PPU : 239 lignes x 5 sources x 256
	const uint16_t* Backdrop = nullptr;   // couleur 0 (CGRAM) par ligne
	const uint32_t* BgCol = nullptr;      // <background> du pack empiles (ARGB non premultiplie), meme sous les sprites ; 4 piles (BG d'ancrage 1..4)
	                                      // de OutW x OutH chacune, l'une apres l'autre ; nullptr = aucun
	const uint8_t* BgZ = nullptr;         // Z du fond opaque le plus haut de chaque pixel, meme disposition (4 piles)
	uint8_t BgPileMask = 0x0F;            // piles utilisees par cette image (bit m = BG d'ancrage m + 1) ; les autres ne sont pas lues
	const uint32_t* TrCol = nullptr;      // dernier pixel de tile semi-transparent pose (bord anticrenele), ARGB non premultiplie
	const uint8_t* TrZ = nullptr;         // Z de ce pixel (devant la source visible si TrZ > Z)
	const uint8_t* TrG = nullptr;         // groupe <DioramaDepth> de la tile de ce bord (0 = aucun)
	const uint8_t* OwnG = nullptr;        // groupe de la derniere tile opaque posee sur ce pixel HD (0 = aucun)
	const uint16_t* OwnSl = nullptr;      // regles <DioramaSprites>tile= de cette tile (indice dans SprSlots + 1, 0 = aucune)
	const uint16_t* TrSl = nullptr;       // idem pour le bord semi-transparent
	const uint32_t* Palette = nullptr;    // couleur SNES 15 bits -> ARGB
	int BgMode = 1;
	bool Bg3Priority = false;
	uint32_t FrameNumber = 0;
	const SnesHdScreenInfo* Screen = nullptr; // defilement par ligne (repere ref=bgN)
	SnesHdThreadPool* Pool = nullptr;
};

struct SnesHdDioRuntime;
struct SnesHdDioRuntimeDeleter { void operator()(SnesHdDioRuntime* p) const; };
using SnesHdDioRuntimePtr = std::unique_ptr<SnesHdDioRuntime, SnesHdDioRuntimeDeleter>;

SnesHdDioRuntimePtr SnesHdDioCreate();
bool SnesHdDioRun(SnesHdDioRuntime& rt, SnesHdPackData& pack, const SnesHdDioFrame& f);
// Apres SnesHdDioRun (etape 3a) : <DayNight> en rendu 2D normal quand la scene n'a pas ete dessinee (diorama absent, coupe ou en echec)
void SnesHdDioPost(SnesHdDioRuntime& rt, SnesHdPackData& pack, const SnesHdDioFrame& f, bool sceneDrawn);
