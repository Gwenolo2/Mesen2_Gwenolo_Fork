#include "pch.h"
#include <chrono>
#include "NES/HdPacks/HdNesPack.h"
#include "NES/HdPacks/HdVideoFilter.h"
#include "NES/NesConsole.h"
#include "NES/NesConstants.h"
#include "Shared/Emulator.h"
#include "Shared/Video/BaseVideoFilter.h"

HdVideoFilter::HdVideoFilter(NesConsole* console, Emulator* emu, HdPackData* hdData) : BaseVideoFilter(emu)
{
	_hdData = hdData;
	switch(hdData->Scale) {
		case 1: _hdNesPack.reset(new HdNesPack<1>(console, emu->GetSettings(), hdData)); break;
		case 2: _hdNesPack.reset(new HdNesPack<2>(console, emu->GetSettings(), hdData)); break;
		case 3: _hdNesPack.reset(new HdNesPack<3>(console, emu->GetSettings(), hdData)); break;
		case 4: _hdNesPack.reset(new HdNesPack<4>(console, emu->GetSettings(), hdData)); break;
		case 5: _hdNesPack.reset(new HdNesPack<5>(console, emu->GetSettings(), hdData)); break;
		case 6: _hdNesPack.reset(new HdNesPack<6>(console, emu->GetSettings(), hdData)); break;
		case 7: _hdNesPack.reset(new HdNesPack<7>(console, emu->GetSettings(), hdData)); break;
		case 8: _hdNesPack.reset(new HdNesPack<8>(console, emu->GetSettings(), hdData)); break;
		case 9: _hdNesPack.reset(new HdNesPack<9>(console, emu->GetSettings(), hdData)); break;
		case 10: _hdNesPack.reset(new HdNesPack<10>(console, emu->GetSettings(), hdData)); break;
	}
	
}

// Taille de l'image visible et colonnes ajoutees de chaque cote (ws) pour le widescreen 16/9 (voie 1)
// ws : meme regle que le SNES (doc 14) : 16/9 avec correction 8:7, arrondi au multiple de 8
static void GetNesWidescreenLayout(const OverscanDimensions& overscan, bool enabled, uint32_t& visW, uint32_t& visH, uint32_t& ws)
{
	visW = NesConstants::ScreenWidth - overscan.Left - overscan.Right;
	visH = NesConstants::ScreenHeight - overscan.Top - overscan.Bottom;
	ws = 0;
	if(enabled && visH > 0) {
		const double target = (double)visH * 16.0 / 9.0 * 7.0 / 8.0;
		const double half = (target - (double)visW) / 2.0;
		if(half > 0) {
			ws = (uint32_t)(std::round(half / 8.0) * 8.0);
		}
	}
}

// Colonnes ajoutees de chaque cote (en pixels NES) : le decodeur les ajoute a la largeur de base,
// pour que le ratio d'affichage suive la largeur elargie (meme mecanisme que le SNES)
uint32_t HdVideoFilter::GetWidescreenExtraWidth()
{
	uint32_t visW = 0, visH = 0, ws = 0;
	GetNesWidescreenLayout(GetOverscan(), _hdData->Widescreen, visW, visH, ws);
	return ws;
}

FrameInfo HdVideoFilter::GetFrameInfo()
{
	OverscanDimensions overscan = GetOverscan();
	uint32_t hdScale = _hdNesPack->GetScale();
	uint32_t visW = 0, visH = 0, ws = 0;
	GetNesWidescreenLayout(overscan, _hdData->Widescreen, visW, visH, ws);

	return {
		(visW + 2 * ws) * hdScale,
		visH * hdScale
	};
}

OverscanDimensions HdVideoFilter::GetOverscan()
{
	if(_hdData->HasOverscanConfig) {
		return _hdData->Overscan;
	} else {
		return BaseVideoFilter::GetOverscan();
	}
}

void HdVideoFilter::ApplyFilter(uint16_t *ppuOutputBuffer)
{
	if(_frameData == nullptr) {
		//_frameData can be null when loading a save state
		return;
	}

	OverscanDimensions overscan = GetOverscan();
	_hdNesPack->SetVideoAudioSink(_emu->GetSoundMixer());

	uint32_t visW = 0, visH = 0, ws = 0;
	GetNesWidescreenLayout(overscan, _hdData->Widescreen, visW, visH, ws);
	if(ws == 0) {
		_hdNesPack->SetWidescreenMode(false, 256.0f);
		_hdNesPack->Process((HdScreenInfo*)_frameData, GetOutputBuffer(), overscan);
		return;
	}

	// Widescreen (voie 1) : image NES calculee dans un tampon sans bandes, puis copiee au centre de la sortie elargie.
	// Les bandes restent noires tant que le pack ne fournit pas de <background> pour elles (etape 3).
	const uint32_t hdScale = _hdNesPack->GetScale();
	const size_t srcW = (size_t)visW * hdScale;
	const size_t srcH = (size_t)visH * hdScale;
	const size_t dstW = (size_t)(visW + 2 * ws) * hdScale;
	if(_wsBuffer.size() != srcW * srcH) {
		_wsBuffer.resize(srcW * srcH);
	}
	uint32_t* out = GetOutputBuffer();
	std::fill(out, out + dstW * srcH, 0xFF000000u);
	_hdNesPack->SetWidescreenMode(true, (float)dstW / (float)hdScale);
	_hdNesPack->Process((HdScreenInfo*)_frameData, _wsBuffer.data(), overscan);
	const size_t offX = (size_t)ws * hdScale;
	for(size_t y = 0; y < srcH; y++) {
		memcpy(out + y * dstW + offX, _wsBuffer.data() + y * srcW, srcW * sizeof(uint32_t));
	}
	// Fonds <background> dans les bandes (apres le centre, sans toucher aux effets)
	// Chronometrage des passes widescreen (journal [HD-WS] toutes les 300 images)
	using WsClock = std::chrono::steady_clock;
	static double wsMs[4] = {0, 0, 0, 0};
	static int wsFrames = 0;
	auto wsT0 = WsClock::now();
	auto wsLap = [&](int i) {
		auto tNow = WsClock::now();
		wsMs[i] += std::chrono::duration<double, std::milli>(tNow - wsT0).count();
		wsT0 = tNow;
	};
	_hdNesPack->DrawWidescreenBands(out, (uint32_t)dstW, (uint32_t)offX, overscan.Left, overscan.Top, overscan.Bottom);
	wsLap(0);
	_hdNesPack->ProcessWidescreenOverlays(out, (uint32_t)dstW, (uint32_t)offX, overscan.Left, overscan.Top, overscan.Bottom);
	wsLap(1);
	_hdNesPack->ProcessWidescreenScreenLayers(out, (uint32_t)dstW, (uint32_t)offX, overscan.Left, overscan.Top, overscan.Bottom);
	wsLap(2);
	_hdNesPack->ProcessWidescreenDistortions(out, (uint32_t)dstW, (uint32_t)offX, overscan.Left, overscan.Top, overscan.Bottom);
	_hdNesPack->ProcessWidescreenPerspectives(out, (uint32_t)dstW, (uint32_t)offX, overscan.Left, overscan.Top, overscan.Bottom);
	wsLap(3);
	if(++wsFrames >= 300) {
		double total = wsMs[0] + wsMs[1] + wsMs[2] + wsMs[3];
		char buf[256];
		snprintf(buf, sizeof(buf), "[HD-WS] NES ms/frame : bandes %.2f, overlays %.2f, calques %.2f, distorsion %.2f (total %.2f)",
			wsMs[0] / wsFrames, wsMs[1] / wsFrames, wsMs[2] / wsFrames, wsMs[3] / wsFrames, total / wsFrames);
		MessageManager::Log(buf);
		for(double& ms : wsMs) ms = 0;
		wsFrames = 0;
	}
}
