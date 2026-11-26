// =====================================================================================
// Diorama SNES (doc 59) : scene 3D du pack HD SNES avec le moteur GPU du diorama NES (Shared/HdGpuCore).
// Etape 1 : vue side, couches L0..L3 + couche fixe, <DioramaLayer> / <DioramaSprites>, camera (cameras multiples, touches,
// manette, orbit), skybox, lumieres de scene, brouillard, nuages, brume ; repere du monde ref=bgN ou refx= / refy= (RAM).
//
// Construction des couches : l'image 2D composee par le filtre donne les pixels visibles ; le Z de chaque pixel HD dit sa source
// (Z = 20 + priorite PPU x 4, +2 : les priorites sont toutes distinctes dans un mode). Sous chaque pixel, les sources qui sont
// derriere (releve PPU par source, SnesHdDioPixel) remplissent les couches de rang inferieur : pas de trou quand la camera tourne,
// et a vue neutre l'image est celle du 2D (le remplissage ne va jamais dans une couche devant la source visible).
// Les blocs de scene (camera, manette, skybox, lumieres, brouillard, nuages, brume) sont des copies de GbHdDiorama.cpp
// (elles-memes copies de HdNesPack::DioramaGpuStep) : une correction faite cote NES est a reporter ici.
// =====================================================================================
#include "pch.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "SNES/HdPacks/SnesHdDioramaRun.h"
#include "SNES/HdPacks/SnesHdDiorama.h"
#include "SNES/HdPacks/SnesHdThreadPool.h"
#include "SNES/HdPacks/SnesHdGpu.h"
#include "Shared/HdGpuCore.h"
#include "Shared/KeyManager.h"
#include "Shared/MessageManager.h"

struct SnesHdDioRuntime
{
	std::vector<uint32_t> L[4];
	std::vector<uint32_t> Fix;
	std::vector<uint32_t> G[8]; // groupes <DioramaDepth>{ } (etape 2b)
	std::vector<uint32_t> Sh;   // G6c : masque des silhouettes qui projettent une ombre (etape 2c)
	std::vector<int16_t> ShSd;  // regle d'ombre (0..) par pixel SNES, -1 = aucune
	std::vector<uint8_t> ShSrc; // source (0..4) qui porte l'ombre
	bool GpuTried = false, GpuOk = false;
	int Fail = 0;
	// camera (copie de GbHdVideoFilter)
	int CamDefault = -1, CamManual = -1;
	std::vector<uint8_t> CamKeyDown;
	bool CamKeyWarned = false, CamInit = false;
	float CamFrom[4] = {}, CamTo[4] = {}, CamCur[4] = {}, CamT = 1.0f, CamDur = 1.0f;
	float PadYaw = 0.0f, PadPitch = 0.0f, PadZoom = 0.0f;
	uint32_t Orbit = 0;
	// skybox
	const void* SkyCubeKey = nullptr;
	std::vector<uint32_t> SkyCube;
	uint32_t SkyCubeF = 0;
	float SkyX = 0.0f, SkyY = 0.0f;
	// repere du monde (pixels SNES) : defilement cumule du BG de reference, ou valeur RAM
	bool WInit = false;
	int LastSX = 0, LastSY = 0;
	float WX = 0.0f, WY = 0.0f;
	// topDown (doc 53 / 59 etape 2a)
	bool TopDown = false;
	float FloorMargin = -1.0f;
	std::vector<uint8_t> Foot;   // pied des sprites : 256 octets par ligne SNES (ligne du bas + 1 de la silhouette)
	std::vector<uint8_t> SprSd;  // pixel SNES visible : 1 = sprite, 2 + g = tile d'un groupe debout (stand=on), 0 = autre (silhouettes)
	// journaux (une fois)
	bool Logged[16] = {};
};

void SnesHdDioRuntimeDeleter::operator()(SnesHdDioRuntime* p) const { delete p; }
SnesHdDioRuntimePtr SnesHdDioCreate() { return SnesHdDioRuntimePtr(new SnesHdDioRuntime()); }

namespace {
	void LogOnce(SnesHdDioRuntime& rt, int k, const std::string& msg)
	{
		if(rt.Logged[k]) return;
		rt.Logged[k] = true;
		MessageManager::Log("[HD-SNES-DIORAMA] " + msg);
	}

	bool CondsOk(const std::vector<HdPackCondition*>& cs)
	{
		for(HdPackCondition* c : cs) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}

	bool SnesCondsOk(const std::vector<SnesHdPackCondition*>& cs)
	{
		for(SnesHdPackCondition* c : cs) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}

	// rang d'une cible pour l'ordre avant / arriere : L0..L3 = 0..3, fixe = 4 ; retiree : jamais remplie
	inline int RankOf(int layer) { return layer == SnesHdDioFixed ? 4 : layer; }

	inline uint32_t Premul(uint32_t c)
	{
		const uint32_t a = c >> 24;
		if(a == 255) return c;
		if(a == 0) return 0;
		const uint32_t r = ((c >> 16) & 0xFF) * a / 255, g = ((c >> 8) & 0xFF) * a / 255, b = (c & 0xFF) * a / 255;
		return (a << 24) | (r << 16) | (g << 8) | b;
	}

	// Camera : cameras multiples, touches et transitions (copie de GbHdVideoFilter::DioramaCamera, sans <DioramaView> : vue side)
	void DioramaCamera(SnesHdDioRuntime& rt, HdPackData& D, const HdDioramaConfig& cfg, float& yaw, float& pitch, float& dist, float& zoom)
	{
		const vector<string>& names = D.DioramaCameraNames;
		const int n = (int)names.size();
		int def = 0;
		bool found = false;
		for(const HdDioramaCameraDefaultRule& r : D.DioramaCameraDefaults) { if(CondsOk(r.Conditions)) { def = r.Index; found = true; } }
		if(!found) { for(const HdDioramaCameraLine& L : D.DioramaCameraLines) { if(L.IsDefault) { def = L.Index; break; } } }
		if(def != rt.CamDefault) { rt.CamDefault = def; rt.CamManual = -1; }
		const HdDioramaCameraKeys& ks = D.DioramaCameraKeys;
		vector<string> keyNames((size_t)n + 3);
		for(const HdDioramaCameraLine& L : D.DioramaCameraLines) { if(!L.Key.empty() && keyNames[(size_t)L.Index].empty()) keyNames[(size_t)L.Index] = L.Key; }
		keyNames[(size_t)n] = ks.Next; keyNames[(size_t)n + 1] = ks.Prev; keyNames[(size_t)n + 2] = ks.Default;
		if(n == 0) keyNames.clear();
		if(rt.CamKeyDown.size() != keyNames.size()) rt.CamKeyDown.assign(keyNames.size(), 0);
		int active = rt.CamManual >= 0 ? rt.CamManual : rt.CamDefault;
		for(size_t k = 0; k < keyNames.size(); k++) {
			if(keyNames[k].empty()) continue;
			const uint16_t code = KeyManager::GetKeyCode(keyNames[k]);
			if(code == 0) {
				if(!rt.CamKeyWarned) { rt.CamKeyWarned = true; MessageManager::Log("[HD-SNES-DIORAMA] camera : nom de touche inconnu : " + keyNames[k]); }
				continue;
			}
			const bool down = KeyManager::IsKeyPressed(code);
			if(down && !rt.CamKeyDown[k]) {
				if((int)k < n) rt.CamManual = (int)k;
				else if((int)k == n) rt.CamManual = (active + 1) % n;
				else if((int)k == n + 1) rt.CamManual = (active + n - 1) % n;
				else rt.CamManual = -1;
				active = rt.CamManual >= 0 ? rt.CamManual : rt.CamDefault;
			}
			rt.CamKeyDown[k] = down ? 1 : 0;
		}
		float v[4] = { cfg.Yaw, cfg.Pitch, cfg.Dist, cfg.Zoom };
		float time = 40.0f;
		for(const HdDioramaCameraLine& L : D.DioramaCameraLines) {
			if(L.Index != active || !CondsOk(L.Conditions)) continue;
			for(int i = 0; i < 4; i++) { if(L.Has[i]) v[i] = L.V[i]; }
			if(L.HasTime) time = L.Time;
		}
		// <DioramaView> (doc 53) : derniere regle vraie -> vue topDown (pitch, zoom, time) ; aucune : vue side
		rt.TopDown = false;
		rt.FloorMargin = -1.0f;
		for(const HdDioramaViewRule& r : D.DioramaViews) {
			if(!CondsOk(r.Conditions)) continue;
			rt.TopDown = true;
			if(r.FloorMargin >= 0.0f) rt.FloorMargin = r.FloorMargin;
			for(int i = 0; i < 4; i++) { if(r.Has[i]) v[i] = r.V[i]; }
			if(r.HasTime) time = r.Time;
		}
		float minZ = 0.0f;
		for(int k = 0; k < 4; k++) minZ = std::min(minZ, cfg.Z[k]);
		for(const HdDioramaDepthGroup& g : D.DioramaDepthGroups) minZ = std::min(minZ, g.Z);
		v[2] = std::max(v[2], std::max(16.0f, -minZ / 0.9f + 1.0f));
		if(!rt.CamInit) {
			for(int i = 0; i < 4; i++) { rt.CamFrom[i] = rt.CamTo[i] = rt.CamCur[i] = v[i]; }
			rt.CamT = rt.CamDur = 1.0f; rt.CamInit = true;
		}
		bool changed = false;
		for(int i = 0; i < 4; i++) { if(std::fabs(v[i] - rt.CamTo[i]) > 1e-4f) changed = true; }
		if(changed) {
			for(int i = 0; i < 4; i++) { rt.CamFrom[i] = rt.CamCur[i]; rt.CamTo[i] = v[i]; }
			rt.CamT = 0.0f; rt.CamDur = std::max(1.0f, time);
			if(time <= 0.0f) { for(int i = 0; i < 4; i++) rt.CamCur[i] = v[i]; rt.CamT = rt.CamDur; }
		}
		if(rt.CamT < rt.CamDur) {
			rt.CamT += 1.0f;
			const float t = std::min(1.0f, rt.CamT / rt.CamDur);
			const float s = t * t * (3.0f - 2.0f * t);
			for(int i = 0; i < 4; i++) rt.CamCur[i] = rt.CamFrom[i] + (rt.CamTo[i] - rt.CamFrom[i]) * s;
		}
		yaw = rt.CamCur[0]; pitch = rt.CamCur[1]; dist = rt.CamCur[2]; zoom = rt.CamCur[3];
	}

	// Manette : <DioramaPad> (copie de GbHdVideoFilter::DioramaSubmit)
	void DioramaPad(SnesHdDioRuntime& rt, HdPackData& D, float& yaw, float& pitch, float& zoom)
	{
		float tYaw = 0.0f, tPitch = 0.0f, tZoom = 0.0f, smooth = 0.2f;
		const HdDioramaPadRule* pr = nullptr;
		for(const HdDioramaPadRule& r : D.DioramaPadRules) { if(CondsOk(r.Conditions)) { pr = &r; break; } }
		if(pr) {
			smooth = pr->Smooth;
			const string pad = "Pad" + std::to_string(pr->Port) + " ", joy = "Joy" + std::to_string(pr->Port) + " ";
			auto axis = [&](const char* padName, const char* joyName, float& out) {
				optional<int16_t> a;
				const uint16_t cp = KeyManager::GetKeyCode(pad + padName);
				if(cp != 0) a = KeyManager::GetAxisPosition(cp);
				if(!a.has_value()) { const uint16_t cj = KeyManager::GetKeyCode(joy + joyName); if(cj != 0) a = KeyManager::GetAxisPosition(cj); }
				if(!a.has_value()) return false;
				float v = (float)a.value() / 32767.0f;
				const float av = std::fabs(v);
				if(av <= pr->Dead) v = 0.0f; else v = (v < 0 ? -1.0f : 1.0f) * std::min(1.0f, (av - pr->Dead) / (1.0f - pr->Dead));
				out = v; return true;
			};
			auto stick = [&](int fn, const char* nx, const char* ny, const char* jx, const char* jy) {
				if(fn == 0) return;
				float x = 0.0f, y = 0.0f;
				axis(nx, jx, x); axis(ny, jy, y);
				if(pr->InvX) x = -x;
				if(pr->InvY) y = -y;
				if(fn == 1) { tYaw += x * pr->YawMax; tPitch += y * pr->PitchMax; }
				else tZoom += y * std::log2(pr->ZoomRange);
			};
			stick(pr->Left, "LT X", "LT Y", "X", "Y");
			stick(pr->Right, "RT X", "RT Y", "X2", "Y2");
		}
		rt.PadYaw += (tYaw - rt.PadYaw) * smooth; rt.PadPitch += (tPitch - rt.PadPitch) * smooth; rt.PadZoom += (tZoom - rt.PadZoom) * smooth;
		if(std::fabs(rt.PadYaw) < 0.01f && tYaw == 0.0f) rt.PadYaw = 0.0f;
		if(std::fabs(rt.PadPitch) < 0.01f && tPitch == 0.0f) rt.PadPitch = 0.0f;
		if(std::fabs(rt.PadZoom) < 0.001f && tZoom == 0.0f) rt.PadZoom = 0.0f;
		yaw = std::max(-70.0f, std::min(70.0f, yaw + rt.PadYaw));
		pitch = std::max(-70.0f, std::min(70.0f, pitch + rt.PadPitch));
		zoom = std::max(0.1f, std::min(4.0f, zoom * std::pow(2.0f, rt.PadZoom)));
	}

	// Sources de chaque Z (doc 04 : Z = 20 + priorite PPU x 4, +2) selon le mode ; un Z inconnu (fond <background>, overlay,
	// <setLayersOrder>) prend la source du Z connu juste en dessous. Source : 0..3 BG1..BG4, 4 sprites, 5 fond (backdrop).
	void BuildZTable(int bgMode, bool bg3Prio, int8_t* zSrc, uint8_t* zSub, bool* ppuZ)
	{
		int8_t known[256];
		uint8_t sub[256];
		memset(known, -1, sizeof(known));
		memset(sub, 0, sizeof(sub));
		auto put = [&](int src, int s, int p) {
			const int z = 20 + p * 4;
			if(z + 2 < 256) { known[z] = (int8_t)src; known[z + 2] = (int8_t)src; sub[z] = (uint8_t)s; sub[z + 2] = (uint8_t)s; }
		};
		const uint8_t* spr = nullptr;
		static const uint8_t spr0[4] = { 3, 6, 9, 12 }, spr1[4] = { 2, 4, 7, 10 }, spr2[4] = { 2, 4, 6, 8 };
		switch(bgMode) {
			case 0:
				spr = spr0;
				put(0, 0, 8); put(0, 1, 11); put(1, 0, 7); put(1, 1, 10); put(2, 0, 2); put(2, 1, 5); put(3, 0, 1); put(3, 1, 4);
				break;
			case 1:
				spr = spr1;
				put(0, 0, 6); put(0, 1, 9); put(1, 0, 5); put(1, 1, 8); put(2, 0, 1); put(2, 1, bg3Prio ? 11 : 3);
				break;
			default: // 2, 3, 4
				spr = spr2;
				put(0, 0, 3); put(0, 1, 7); put(1, 0, 1); put(1, 1, 5);
				break;
		}
		for(int k = 0; k < 4; k++) put(4, k, spr[k]);
		for(int z = 0; z < 256; z++) ppuZ[z] = known[z] >= 0; // Z d'une tile BG ou d'un sprite (pas le fond)
		int8_t last = 5;
		uint8_t lastSub = 0;
		for(int z = 0; z < 256; z++) {
			if(z == 0 || z == 20) { known[z] = 5; sub[z] = 0; } // fond : Z 0 (rien de dessine) et 20 (priorite 0)
			if(known[z] >= 0) { last = known[z]; lastSub = sub[z]; }
			zSrc[z] = last; zSub[z] = lastSub;
		}
	}
}

bool SnesHdDioRun(SnesHdDioRuntime& rt, SnesHdPackData& pack, const SnesHdDioFrame& f)
{
	if(!pack.Dio || !pack.Dio->Data.DioramaEnabled || !f.Out || !f.Z) return false;
	if(rt.Fail >= 30 || (rt.GpuTried && !rt.GpuOk)) return false;
	if(f.Wide) { LogOnce(rt, 0, "widescreen actif : diorama coupe (etape suivante), rendu 2D conserve."); return false; }
	if(f.BgMode == 7) { LogOnce(rt, 1, "Mode 7 : pas encore pris en charge par le diorama (doc 59, etape 4), rendu 2D conserve."); return false; }
	if(f.BgMode >= 5 || f.SdWidth != 256) { LogOnce(rt, 2, "mode 5 / 6 ou haute resolution 512 : diorama coupe (doc 59), rendu 2D conserve."); return false; }
	if(!f.Rec || !f.Backdrop || !f.Palette) { LogOnce(rt, 3, "releve PPU absent pour cette image, rendu 2D conserve."); return false; }

	SnesHdDiorama& dio = *pack.Dio;
	HdPackData& D = dio.Data;
	const HdDioramaConfig& cfg = D.Diorama;

	// Deux contextes OpenGL (celui du Mode 7, SnesHdGpu, et celui du diorama, HdGpuCore) gardent chacun "deja actif sur ce
	// thread" : on les relache tous les deux pour que HdGpuCore active vraiment le sien, et que SnesHdGpu fasse de meme ensuite.
	SnesHdGpu::Get().ReleaseForOtherContext();
	HdGpuCore& gpu = HdGpuCore::Get();
	gpu.ReleaseForOtherContext();
	if(!rt.GpuTried) {
		rt.GpuTried = true;
		rt.GpuOk = gpu.Available();
		MessageManager::Log(rt.GpuOk ? ("[HD-SNES-DIORAMA] GPU actif (" + gpu.Info() + ")") : ("[HD-SNES-DIORAMA] GPU indisponible (" + gpu.Info() + "), rendu 2D conserve"));
		if(!rt.GpuOk) return false;
		gpu.ReleaseForOtherContext();
	}

	// --- affectation des sources aux couches pour cette image (derniere regle vraie) ---
	int bgLayer[4][2] = { { 2, 2 }, { 1, 1 }, { 1, 1 }, { 1, 1 } }; // defaut : BG1 -> L2, BG2..BG4 -> L1
	int sprLayer[4] = { 3, 3, 3, 3 };                                 // defaut : sprites -> L3
	for(const SnesHdDioLayerRule& r : dio.LayerRules) {
		if(!SnesCondsOk(r.Conditions)) continue;
		for(int p = 0; p < 2; p++) { if(r.Prio < 0 || r.Prio == p) bgLayer[r.Bg][p] = r.Layer; }
	}
	for(const SnesHdDioSpriteRule& r : dio.SpriteRules) {
		if(!SnesCondsOk(r.Conditions)) continue;
		for(int p = 0; p < 4; p++) { if(r.Prio < 0 || r.Prio == p) sprLayer[p] = r.Layer; }
	}
	int8_t zSrc[256];
	uint8_t zSub[256];
	bool ppuZ[256];
	BuildZTable(f.BgMode, f.Bg3Priority, zSrc, zSub, ppuZ);
	// <background> du pack : un Z qui n'est pas celui d'une tile BG ou d'un sprite suit la couche du BG d'ancrage du fond (masterLayer)
	uint8_t bgMaster[256] = {};
	for(const auto& bgp : pack.Backgrounds) {
		if(bgp && bgp->ZIndex >= 0 && bgp->ZIndex < 256 && bgp->MasterScrollLayer >= 1 && bgp->MasterScrollLayer <= 4) bgMaster[bgp->ZIndex] = (uint8_t)bgp->MasterScrollLayer;
	}
	auto layerOf = [&](int src, int sub) -> int {
		if(src == 5) return 0;
		if(src == 4) return sprLayer[sub & 3];
		return bgLayer[src & 3][sub & 1];
	};
	auto layerOfZ = [&](uint8_t zz) -> int {
		if(!ppuZ[zz] && bgMaster[zz]) return bgLayer[bgMaster[zz] - 1][1];
		return layerOf(zSrc[zz], zSub[zz]);
	};

	// skybox vraie : la couleur du fond (backdrop) devient transparente dans L0, le ciel s'y voit
	bool sky = false;
	for(const HdDioramaSkyboxRule& r : D.DioramaSkyboxRules) { if(CondsOk(r.Conditions)) { sky = true; break; } }

	// --- couches ---
	const uint32_t W = f.OutW, H = f.OutH, S = std::max<uint32_t>(1, f.Scale);
	const size_t pixels = (size_t)W * H;
	for(int k = 0; k < 4; k++) { if(rt.L[k].size() != pixels) rt.L[k].assign(pixels, 0u); }
	if(rt.Fix.size() != pixels) rt.Fix.assign(pixels, 0u);
	const uint32_t sdW = W / S, sdH = H / S;
	const int nG = (int)std::min<size_t>(D.DioramaDepthGroups.size(), 8);
	for(int g = 0; g < nG; g++) { if(rt.G[g].size() != pixels) rt.G[g].assign(pixels, 0u); }
	// groupe d'une tile relevee par la PPU (1..nG), 0 = aucun
	auto groupOf = [&](const SnesHdDioPixel& r) -> int {
		return ((r.Flags & 1) && r.Tile && r.Tile->DioGroup >= 1 && r.Tile->DioGroup <= nG) ? r.Tile->DioGroup : 0;
	};
	if(rt.SprSd.size() != (size_t)sdW * sdH) rt.SprSd.assign((size_t)sdW * sdH, 0);
	const bool disT = pack.DisableOriginalTiles, disS = pack.DisableOriginalSprites;
	auto recColor = [&](int s, const SnesHdDioPixel& r, uint32_t u, uint32_t v) -> uint32_t {
		const bool spr = s == 4;
		if(r.Tile && r.Tile->IsDisabled) return 0;
		if(r.Tile && !r.Tile->HdTileData.empty()) {
			const bool hf = (r.Flags & 2) != 0, vf = (r.Flags & 4) != 0;
			const uint32_t vx = spr ? (hf ? 7u - r.TileX : r.TileX) : (hf ? r.TileX : 7u - r.TileX);
			const uint32_t vy = vf ? 7u - r.TileY : r.TileY;
			const uint32_t pw = r.Tile->Width * S, ph = r.Tile->Height * S;
			const uint32_t px = (vx & 7) * S + u, py = (vy & 7) * S + v;
			if(px >= pw || py >= ph || r.Tile->HdTileData.size() < (size_t)pw * ph) return 0;
			const uint32_t rx = (hf && !r.Tile->IsHFlip) ? pw - 1 - px : px;
			const uint32_t ry = (vf && !r.Tile->IsVFlip) ? ph - 1 - py : py;
			return Premul(r.Tile->HdTileData[(size_t)ry * pw + rx]);
		}
		if(spr ? disS : disT) return 0;
		return f.Palette[r.Color & 0x7FFF] | 0xFF000000u;
	};
	std::vector<int> fixMin, fixMax;
	const int nBands = (f.Pool && f.Pool->Size() > 1 && H >= 64) ? f.Pool->Size() : 1;
	fixMin.assign((size_t)nBands, INT32_MAX); fixMax.assign((size_t)nBands, -1);
	std::vector<int> gMin((size_t)nBands * 8, INT32_MAX), gMax((size_t)nBands * 8, -1);
	// G6c (etape 2c) : regle d'ombre de chaque pixel SNES, d'apres le releve PPU (le sprite le plus prioritaire, meme cache par un BG).
	// Tile avec un bloc <DioramaShadow>{ } (sprite ou BG), sinon forme ligne tile=HASH|* dont les conditions sont vraies (sprites).
	const bool shadowsOn = !D.DioramaShadowRules.empty();
	std::vector<int> sMin((size_t)nBands, INT32_MAX), sMax((size_t)nBands, -1), sRule((size_t)nBands, INT32_MAX);
	if(shadowsOn) {
		if(rt.Sh.size() != pixels) rt.Sh.assign(pixels, 0u);
		rt.ShSd.assign((size_t)239 * 256, -1);
		rt.ShSrc.assign((size_t)239 * 256, 0);
		std::vector<const SnesHdDiorama::ShadowKey*> keys;
		for(const SnesHdDiorama::ShadowKey& k : dio.ShadowKeys) { if(SnesCondsOk(k.Conditions)) keys.push_back(&k); }
		static const int order[5] = { 4, 0, 1, 2, 3 };
		for(int y = 0; y < 239; y++) {
			const SnesHdDioPixel* rr = f.Rec + (size_t)y * SnesHdDioSources * 256;
			for(int x = 0; x < 256; x++) {
				for(int oi = 0; oi < 5; oi++) {
					const int s = order[oi];
					const SnesHdDioPixel& r = rr[(size_t)s * 256 + x];
					if(!(r.Flags & 1)) continue;
					int rule = -1;
					if(r.Tile && r.Tile->DioShadow) rule = r.Tile->DioShadow - 1;
					else if(s == 4) {
						for(const SnesHdDiorama::ShadowKey* k : keys) {
							if(k->AnyTile || (r.Tile && r.Tile->MemoryHash == k->Hash && (k->AnyPalette || r.Tile->PaletteHash == k->Palette || r.Tile->PaletteHash == 0xFFFFFFFFu))) { rule = k->Rule - 1; break; }
						}
					}
					if(rule >= 0 && rule < (int)D.DioramaShadowRules.size()) { rt.ShSd[(size_t)y * 256 + x] = (int16_t)rule; rt.ShSrc[(size_t)y * 256 + x] = (uint8_t)s; break; }
				}
			}
		}
	}
	const uint32_t bandH = (H + nBands - 1) / nBands;
	auto build = [&](int band) {
		const uint32_t y0 = (uint32_t)band * bandH, y1 = std::min(H, y0 + bandH);
		for(uint32_t hy = y0; hy < y1; hy++) {
			const int snesY = (int)(hy / S) - f.DstY + f.SrcY;
			const bool rowOk = snesY >= 0 && snesY < 239;
			const SnesHdDioPixel* rec = rowOk ? f.Rec + (size_t)snesY * SnesHdDioSources * 256 : nullptr;
			const uint32_t bdc = rowOk ? (f.Palette[f.Backdrop[snesY] & 0x7FFF] | 0xFF000000u) : 0xFF000000u;
			const uint32_t v = hy % S;
			bool fixRow = false;
			for(uint32_t hx = 0; hx < W; hx++) {
				const size_t i = (size_t)hy * W + hx;
				rt.L[0][i] = rt.L[1][i] = rt.L[2][i] = rt.L[3][i] = 0u;
				rt.Fix[i] = 0u;
				for(int g = 0; g < nG; g++) rt.G[g][i] = 0u;
				if(shadowsOn) {
					rt.Sh[i] = 0u;
					const int sx = (int)(hx / S) - f.DstX + f.SrcX;
					if(rowOk && sx >= 0 && sx < 256) {
						const size_t si = (size_t)snesY * 256 + sx;
						const int shr = rt.ShSd[si];
						if(shr >= 0) {
							const uint32_t col = recColor(rt.ShSrc[si], rec[(size_t)rt.ShSrc[si] * 256 + sx], hx % S, v);
							if(col >> 24) {
								rt.Sh[i] = col;
								sMin[band] = std::min(sMin[band], (int)hy); sMax[band] = std::max(sMax[band], (int)hy); sRule[band] = std::min(sRule[band], shr);
							}
						}
					}
				}
				const uint8_t z = f.Z[i];
				const int src = zSrc[z];
				const int own = layerOfZ(z);
				uint32_t c = f.Out[i] | 0xFF000000u;
				// Bord semi-transparent d'une tile (sprite HD anticrenele) pose devant la source visible : le 2D l'a melange sans ecrire
				// son Z, il resterait dans la couche de dessous a la position native. On rend a la source visible sa couleur propre
				// (fond <background> capture, ou tile / couleur du releve PPU) et le bord va dans sa propre couche (vue neutre identique).
				uint32_t trCol = 0;
				int trLayer = -1, trGroup = 0;
				const int snesXo = (int)(hx / S) - f.DstX + f.SrcX;
				// groupe <DioramaDepth> de la tile visible : capture du filtre (tile HD opaque qui a pose ce pixel), sinon releve PPU
				int ownG = 0;
				if(nG > 0 && src < 5) {
					if(f.OwnG) ownG = f.OwnG[i] <= nG ? f.OwnG[i] : 0;
					else if(rec && snesXo >= 0 && snesXo < 256) ownG = groupOf(rec[(size_t)src * 256 + snesXo]);
				}
				if(f.TrCol && (f.TrCol[i] >> 24) && f.TrZ[i] > z) {
					uint32_t clean = 0;
					if(f.BgCol && f.BgZ[i] == z && (f.BgCol[i] >> 24) == 255) clean = f.BgCol[i] | 0xFF000000u;
					else if(src < 5 && rec && snesXo >= 0 && snesXo < 256 && (rec[(size_t)src * 256 + snesXo].Flags & 1)) {
						const uint32_t rc = recColor(src, rec[(size_t)src * 256 + snesXo], hx % S, v);
						if((rc >> 24) == 255) clean = rc;
					} else if(src == 5) clean = bdc;
					const int tl = layerOfZ(f.TrZ[i]);
					const int tg = (nG > 0 && f.TrG && f.TrG[i] <= nG) ? f.TrG[i] : 0;
					// le bord part s'il est dans une couche devant, ou dans un autre groupe que la source visible (meme couche d'origine)
					const bool moves = tl == SnesHdDioOff || own == SnesHdDioOff || RankOf(tl) > RankOf(own) ||
						(RankOf(tl) == RankOf(own) && tg != ownG && tl != SnesHdDioFixed);
					if(clean && moves) {
						c = clean;
						if(tl != SnesHdDioOff) { trCol = Premul(f.TrCol[i]); trLayer = tl; trGroup = (tl == SnesHdDioFixed) ? 0 : tg; }
					}
				}
				if(hx % S == S / 2 && v == S / 2 && hx / S < sdW && hy / S < sdH) rt.SprSd[(size_t)(hy / S) * sdW + hx / S] =
					(own == SnesHdDioOff) ? 0 : (ownG > 0 && dio.GroupStand[ownG - 1]) ? (uint8_t)(1 + ownG) : (src == 4 ? 1 : 0);
				auto markG = [&](int g) { const size_t k = (size_t)band * 8 + (size_t)g; gMin[k] = std::min(gMin[k], (int)hy); gMax[k] = std::max(gMax[k], (int)hy); };
				int ownRank;
				if(own == SnesHdDioOff) {
					ownRank = 6; // source retiree : rien n'est pose, tout ce qui est derriere peut se voir
				} else if(own == SnesHdDioFixed) {
					rt.Fix[i] = c; fixRow = true; ownRank = 4;
				} else if(ownG > 0) {
					rt.G[ownG - 1][i] = c; markG(ownG - 1);
					ownRank = own;
				} else {
					rt.L[own][i] = (src == 5 && sky) ? 0u : c;
					ownRank = own;
				}
				// remplissage sous le pixel visible : sources de rang inferieur, de la plus prioritaire a la moins prioritaire ; si la
				// tile visible est partie dans un groupe, sa couche d'origine est remplie aussi (sol sous un buisson du groupe)
				const int snesX = (int)(hx / S) - f.DstX + f.SrcX;
				const int fillRank = (ownG > 0 && own != SnesHdDioOff && own != SnesHdDioFixed) ? ownRank + 1 : ownRank;
				if(fillRank > 0) {
					// candidats : sources PPU (0..4) et fonds <background> du pack (5), tries par Z decroissant (Z = 20 + priorite x 4, +2)
					int cs[SnesHdDioSources + 1], cl[SnesHdDioSources + 1], cz[SnesHdDioSources + 1], cg[SnesHdDioSources + 1], n = 0;
					auto addCand = [&](int s, int lay, int zc, int grp) {
						if(lay == SnesHdDioOff || RankOf(lay) >= fillRank) return;
						int j = n++;
						while(j > 0 && cz[j - 1] < zc) { cs[j] = cs[j - 1]; cl[j] = cl[j - 1]; cz[j] = cz[j - 1]; cg[j] = cg[j - 1]; j--; }
						cs[j] = s; cl[j] = lay; cz[j] = zc; cg[j] = (lay == SnesHdDioFixed) ? 0 : grp;
					};
					if(rec && snesX >= 0 && snesX < 256) {
						for(int s = 0; s < (int)SnesHdDioSources; s++) {
							const SnesHdDioPixel& r = rec[(size_t)s * 256 + snesX];
							if(!(r.Flags & 1) || s == src) continue;
							const bool hi = s < 4 ? (r.Flags & 8) != 0 : r.OamPrio >= 2;
							addCand(s, s < 4 ? bgLayer[s][hi ? 1 : 0] : sprLayer[r.OamPrio & 3], 20 + r.Prio * 4 + (hi ? 2 : 0), nG > 0 ? groupOf(r) : 0);
						}
					}
					if(f.BgCol && (f.BgCol[i] >> 24) && f.BgZ[i] != z) {
						const uint8_t bz = f.BgZ[i];
						addCand(5, layerOfZ(bz), bz, 0);
					}
					const uint32_t u = hx % S;
					for(int k = 0; k < n; k++) {
						uint32_t* dst = cg[k] > 0 ? rt.G[cg[k] - 1].data() : (cl[k] == SnesHdDioFixed ? rt.Fix.data() : rt.L[cl[k]].data());
						if(dst[i] >> 24) continue;
						const uint32_t col = cs[k] == 5 ? Premul(f.BgCol[i]) : recColor(cs[k], rec[(size_t)cs[k] * 256 + snesX], u, v);
						if(!(col >> 24)) continue;
						dst[i] = col;
						if(cg[k] > 0) markG(cg[k] - 1);
						else if(cl[k] == SnesHdDioFixed) fixRow = true;
					}
				}
				if(trLayer >= 0) { // bord semi-transparent dans sa couche (ou son groupe), par-dessus ce qui y est deja (remplissage compris)
					uint32_t* td = trGroup > 0 ? rt.G[trGroup - 1].data() : (trLayer == SnesHdDioFixed ? rt.Fix.data() : rt.L[trLayer].data());
					if(trGroup > 0) markG(trGroup - 1);
					const uint32_t d = td[i], a = trCol >> 24, ia = 255 - a;
					const uint32_t rb = ((trCol & 0xFF00FFu) + (((d & 0xFF00FFu) * ia / 255) & 0xFF00FFu)) & 0xFF00FFu;
					const uint32_t g = ((trCol & 0x00FF00u) + (((d & 0x00FF00u) * ia / 255) & 0x00FF00u)) & 0x00FF00u;
					const uint32_t na = a + (d >> 24) * ia / 255;
					td[i] = (std::min<uint32_t>(na, 255) << 24) | rb | g;
					if(trLayer == SnesHdDioFixed) fixRow = true;
				}
				if(!sky && !(rt.L[0][i] >> 24)) rt.L[0][i] = bdc; // L0 sans trou : couleur du fond de la ligne
			}
			if(fixRow) { fixMin[band] = std::min(fixMin[band], (int)hy); fixMax[band] = std::max(fixMax[band], (int)hy); }
		}
	};
	if(nBands > 1) f.Pool->Run(nBands, [&](int t) { build(t); });
	else build(0);
	int fy0 = INT32_MAX, fy1 = -1;
	for(int b = 0; b < nBands; b++) { fy0 = std::min(fy0, fixMin[b]); fy1 = std::max(fy1, fixMax[b]); }

	// --- repere du monde : defilement cumule du BG de reference (registres 10 bits, saut > 64 ignore), ou valeur RAM ---
	if(f.Screen) {
		const int line = 112, b = dio.RefBg & 3;
		const int sx = f.Screen->ScanlineHScroll[line][b], sy = f.Screen->ScanlineVScroll[line][b];
		if(rt.WInit) {
			const int dx = (((sx - rt.LastSX) % 1024) + 1536) % 1024 - 512;
			const int dy = (((sy - rt.LastSY) % 1024) + 1536) % 1024 - 512;
			if(std::abs(dx) <= 64) rt.WX += (float)dx;
			if(std::abs(dy) <= 64) rt.WY += (float)dy;
		} else { rt.WX = (float)sx; rt.WY = (float)sy; rt.WInit = true; }
		rt.LastSX = sx; rt.LastSY = sy;
	}
	float wx = rt.WX, wy = rt.WY, rv = 0.0f;
	if(!dio.RefX.Empty() && pack.EvalWatchExpr(dio.RefX, rv)) wx = rv;
	if(!dio.RefY.Empty() && pack.EvalWatchExpr(dio.RefY, rv)) wy = rv;

	// --- camera ---
	float yaw = cfg.Yaw, pitch = cfg.Pitch, dist = cfg.Dist, zoom = cfg.Zoom;
	DioramaCamera(rt, D, cfg, yaw, pitch, dist, zoom);
	if(cfg.Orbit) {
		const double ph = 6.283185307179586 * (double)(rt.Orbit++) / (double)cfg.OrbitPeriod;
		yaw += cfg.OrbitAmp * (float)std::sin(ph);
		pitch += 0.4f * cfg.OrbitAmp * (float)std::sin(ph / 1.5);
	}
	DioramaPad(rt, D, yaw, pitch, zoom);
	const bool td = rt.TopDown;

	HdDioramaScene scene;
	scene.W = (float)(W / S);
	scene.H = (float)(H / S);
	scene.D = dist;
	scene.Yaw = yaw; scene.Pitch = pitch;
	for(int k = 0; k < 4; k++) { scene.PlaneZ[k] = cfg.Z[k]; scene.Thick[k] = cfg.T[k] * 8.0f; }
	if(td) scene.PlaneZ[0] = 0.0f; // sol topDown : L0 au niveau du sol
	scene.Scale = (float)S;
	scene.Aa = cfg.Aa;
	scene.Zoom = zoom;
	// topDown : couches sans sprite a plat (sol, toits...), couches des sprites debout a leurs pieds ; groupes a plat
	uint32_t sprMask = 0;
	for(int p = 0; p < 4; p++) { if(sprLayer[p] >= 0 && sprLayer[p] <= 3) sprMask |= 1u << sprLayer[p]; }
	uint32_t standMask = 0; // groupes debout (stand=on) : pas a plat en topDown
	for(int g = 0; g < nG; g++) { if(dio.GroupStand[g]) standMask |= 1u << (4 + g); }
	scene.FlatMask = td ? ((~sprMask & 0xFu) | (0xFF0u & ~standMask)) : 0u;
	// topDown : pour une couche a plat, le Z du GPU est une HAUTEUR (positif = au-dessus du sol) et les plans a plat sont dessines du plus
	// bas au plus haut. Les z0..z3 de <Diorama> (profondeurs de la vue side) souleveraient le sol (z1=24) au-dessus des toits : les
	// couches a plat sont empilees selon leur rang (L0 < L1 < L2 < L3) a des hauteurs quasi nulles.
	if(td) { for(int k = 0; k < 4; k++) { if((scene.FlatMask >> k) & 1u) scene.PlaneZ[k] = 0.01f * (float)k; } }
	if(td && rt.FloorMargin >= 0.0f) scene.BgMargin = rt.FloorMargin;
	if(td) { // pied des sprites : silhouettes de pixels de sprite qui se touchent (meme calcul que le NES / Game Boy)
		const int rows = (int)sdH, cols = (int)std::min<uint32_t>(sdW, 256);
		rt.Foot.assign((size_t)256 * rows, 0);
		int standN[8] = {};
		auto typeAt = [&](int r, int x) { return rt.SprSd[(size_t)r * sdW + x]; };
		std::vector<int> stack, comp;
		for(int r = 0; r < rows; r++) {
			for(int x = 0; x < cols; x++) {
				const int i0 = (r << 8) | x;
				const uint8_t ty = typeAt(r, x);
				if(rt.Foot[i0] || ty == 0) continue;
				stack.clear(); comp.clear();
				rt.Foot[i0] = 255; stack.push_back(i0);
				int maxR = r, minX = x, maxX = x;
				while(!stack.empty()) {
					const int c = stack.back(); stack.pop_back(); comp.push_back(c);
					const int cr = c >> 8, cx = c & 255;
					maxR = std::max(maxR, cr); minX = std::min(minX, cx); maxX = std::max(maxX, cx);
					const int nr[4] = { cr - 1, cr + 1, cr, cr }, nx[4] = { cx, cx, cx - 1, cx + 1 };
					for(int k = 0; k < 4; k++) {
						if(nr[k] < 0 || nr[k] >= rows || nx[k] < 0 || nx[k] >= cols) continue;
						const int j = (nr[k] << 8) | nx[k];
						if(rt.Foot[j] || typeAt(nr[k], nx[k]) != ty) continue; // un buisson et Link qui se touchent restent separes
						rt.Foot[j] = 255; stack.push_back(j);
					}
				}
				const uint8_t fv = (uint8_t)std::min(255, maxR + 1);
				for(int c : comp) rt.Foot[c] = fv;
				if(ty == 1) {
					scene.SpriteBlobTotal++;
					if(scene.SpriteBlobN < 16) {
						float* b = scene.SpriteBlob[scene.SpriteBlobN++];
						b[0] = (float)minX; b[1] = (float)(maxX + 1); b[2] = (float)r; b[3] = (float)(maxR + 1);
					}
				} else {
					const int g = ty - 2;
					if(standN[g] < 64) {
						float* b = scene.ExtraBlob[g][standN[g]];
						b[0] = (float)minX; b[1] = (float)(maxX + 1); b[2] = (float)r; b[3] = (float)(maxR + 1);
					}
					standN[g]++;
				}
			}
		}
		scene.SpriteFoot = rt.Foot.data(); scene.SpriteFootH = (uint32_t)rows;
		for(int g = 0; g < nG; g++) {
			if(!dio.GroupStand[g]) continue;
			if(standN[g] > 64) { // trop de silhouettes : le groupe reste a plat pour cette image
				scene.FlatMask |= 1u << (4 + g);
				LogOnce(rt, 10, "stand=on : plus de 64 silhouettes dans un groupe, groupe dessine a plat pour ces images.");
				continue;
			}
			scene.ExtraStand[g] = standN[g] > 0;
			scene.ExtraBlobN[g] = standN[g];
		}
		LogOnce(rt, 4, "vue topDown active : couches a plat 0x" + std::to_string(scene.FlatMask & 0xF) + " (bit n = Ln), sprites debout en L" + std::to_string(sprLayer[0]));
	}
	scene.Clear = f.Palette[f.Backdrop[120] & 0x7FFF] | 0xFF000000u; // hors des plans : couleur du fond (milieu de l'ecran)
	if(fy1 >= fy0) { scene.Fixed = rt.Fix.data(); scene.FixedY0 = (uint32_t)fy0; scene.FixedRows = (uint32_t)(fy1 - fy0 + 1); }
	// G7 : groupes <DioramaDepth>{ } : seulement leurs lignes non vides (une ligne SNES de marge de chaque cote pour le filtrage)
	int gActive = 0;
	for(int g = 0; g < nG; g++) {
		int a = INT32_MAX, b = -1;
		for(int bd = 0; bd < nBands; bd++) { a = std::min(a, gMin[(size_t)bd * 8 + g]); b = std::max(b, gMax[(size_t)bd * 8 + g]); }
		if(b < a) continue;
		a = std::max(a - (int)S, 0);
		b = std::min(b + (int)S, (int)H - 1);
		const HdDioramaDepthGroup& grp = D.DioramaDepthGroups[(size_t)g];
		scene.ExtraImg[g] = rt.G[g].data();
		scene.ExtraY0[g] = (uint32_t)a;
		scene.ExtraRows[g] = (uint32_t)(b - a + 1);
		// side : profondeur (negatif = devant) ; topDown : hauteur au-dessus des couches a plat = -z (z=-16 devant en side -> 16 au-dessus
		// du sol en topDown ; z=0 -> pose sur le sol, t= donne alors un volume qui monte)
		scene.ExtraZ[g] = td ? std::max(0.0f, -grp.Z) + 0.05f : std::max(grp.Z, -0.9f * dist);
		scene.ExtraThick[g] = grp.T * 8.0f;
		gActive++;
	}
	if(gActive > 0) LogOnce(rt, 9, "groupes <DioramaDepth> : " + std::to_string(gActive) + " groupe(s) visible(s) sur " + std::to_string(nG));
	{ // L0 d'une seule couleur : pas d'envoi, couleur d'effacement
		const uint32_t c0 = rt.L[0][0];
		bool uni = (c0 >> 24) == 0xFF;
		for(size_t p = 1; uni && p < pixels; p += 7) uni = rt.L[0][p] == c0;
		if(uni) uni = std::equal(rt.L[0].begin() + 1, rt.L[0].begin() + pixels, rt.L[0].begin());
		scene.L0Uniform = uni && scene.Thick[0] <= 0.0f;
		if(uni) scene.Clear = c0 | 0xFF000000u;
	}

	// G5 : skybox (copie de GbHdDiorama.cpp)
	if(!D.DioramaSkyboxRules.empty()) {
		const HdDioramaSkyboxRule* chosen = nullptr;
		for(const HdDioramaSkyboxRule& r : D.DioramaSkyboxRules) { if(CondsOk(r.Conditions)) chosen = &r; }
		if(chosen) {
			auto rgb = [](uint32_t c, float* o) { o[0] = ((c >> 16) & 0xFF) / 255.0f; o[1] = ((c >> 8) & 0xFF) / 255.0f; o[2] = (c & 0xFF) / 255.0f; };
			if(chosen->Mode == 1) {
				scene.Sky.Mode = 1;
				rgb(chosen->Top, scene.Sky.Top); rgb(chosen->Bottom, scene.Sky.Bottom);
			} else if(chosen->Mode >= 3) {
				const uint32_t* pix = nullptr; uint32_t sw = 0, sh = 0; int layout = 1;
				if(chosen->Mode == 4 && chosen->Faces[0]) {
					if(rt.SkyCubeKey != chosen) {
						rt.SkyCubeKey = chosen; rt.SkyCube.clear(); rt.SkyCubeF = 0;
						uint32_t fs = 0; bool good = true;
						for(int k = 0; k < 6 && good; k++) {
							chosen->Faces[k]->Init();
							const HdPackBitmapInfo* b = chosen->Faces[k];
							if(b->Width == 0 || b->Width != b->Height || b->PixelData.size() < (size_t)b->Width * b->Height) good = false;
							else if(fs == 0) fs = b->Width;
							else if(fs != b->Width) good = false;
						}
						if(good) {
							rt.SkyCube.assign((size_t)fs * 6 * fs, 0u);
							for(int k = 0; k < 6; k++) {
								for(uint32_t y = 0; y < fs; y++) memcpy(&rt.SkyCube[(size_t)y * fs * 6 + (size_t)k * fs], &chosen->Faces[k]->PixelData[(size_t)y * fs], fs * sizeof(uint32_t));
							}
							rt.SkyCubeF = fs;
						} else {
							MessageManager::Log("[HD-SNES-DIORAMA] skybox cube : les 6 faces doivent etre carrees et de meme taille, skybox ignoree.");
						}
					}
					if(!rt.SkyCube.empty()) { pix = rt.SkyCube.data(); sw = rt.SkyCubeF * 6; sh = rt.SkyCubeF; layout = 3; }
				} else if(chosen->Image) {
					chosen->Image->Init();
					const HdPackBitmapInfo* b = chosen->Image;
					if(b->Width > 0 && b->Height > 0 && b->PixelData.size() >= (size_t)b->Width * b->Height) {
						pix = b->PixelData.data(); sw = b->Width; sh = b->Height;
						if(chosen->Mode == 4) {
							if(sw * 3 == sh * 4) layout = 1;
							else if(sw * 4 == sh * 3) layout = 2;
							else if(sw == sh * 6) layout = 3;
							else if(sh == sw * 6) layout = 4;
							else { pix = nullptr; MessageManager::Log("[HD-SNES-DIORAMA] skybox cube : taille d'image inattendue (croix 4:3 ou 3:4, bande 6:1 ou 1:6), skybox ignoree."); }
						}
					}
				}
				if(pix) {
					rt.SkyX += chosen->SpeedX; rt.SkyY += chosen->SpeedY;
					scene.Sky.Mode = chosen->Mode;
					scene.Sky.Pix = pix; scene.Sky.W = sw; scene.Sky.H = sh;
					scene.Sky.RotYaw = rt.SkyX; scene.Sky.RotPitch = rt.SkyY; scene.Sky.CamPitch = chosen->HasOffset ? chosen->PitchOffset : (td ? 45.0f : 0.0f); scene.Sky.CubeLayout = layout;
				}
			} else if(chosen->Image) {
				chosen->Image->Init();
				if(chosen->Image->Width > 0 && chosen->Image->Height > 0 && chosen->Image->PixelData.size() >= (size_t)chosen->Image->Width * chosen->Image->Height) {
					rt.SkyX += chosen->SpeedX; rt.SkyY += chosen->SpeedY;
					scene.Sky.Mode = 2;
					scene.Sky.Pix = chosen->Image->PixelData.data(); scene.Sky.W = chosen->Image->Width; scene.Sky.H = chosen->Image->Height;
					scene.Sky.OffX = rt.SkyX; scene.Sky.OffY = rt.SkyY;
				}
			}
			LogOnce(rt, 5, std::string("skybox : ") + (chosen->Mode == 1 ? "degrade" : chosen->Mode == 2 ? "image" : chosen->Mode == 3 ? "panorama" : "cubemap") + ", " + std::to_string(D.DioramaSkyboxRules.size()) + " regle(s)");
		}
	}

	// G6a : lumieres de scene (copie de GbHdDiorama.cpp, vue side) ; lumieres attachees a une tile (tile=, hash NES) non prises en charge
	if(!D.DioramaLightRules.empty()) {
		auto rgb = [](uint32_t c, float k, float* o) { o[0] = ((c >> 16) & 0xFF) / 255.0f * k; o[1] = ((c >> 8) & 0xFF) / 255.0f * k; o[2] = (c & 0xFF) / 255.0f * k; };
		int n = 0;
		bool any = false, anyTile = false;
		const float tSec = (float)f.FrameNumber / 60.0f;
		for(const HdDioramaLightRule& r : D.DioramaLightRules) {
			if(r.HasTile) { anyTile = true; continue; }
			if(!CondsOk(r.Conditions)) continue;
			any = true;
			if(r.Type == 1) { rgb(r.Color, r.Intensity, scene.Lights.Ambient); continue; }
			if(n >= 8) continue;
			HdDioramaLight& l = scene.Lights.L[n++];
			const float fk = r.Flicker > 0.0f ? 1.0f - r.Flicker * 0.5f * (1.0f + std::sin(r.FlickerSpeed * tSec)) : 1.0f;
			l.Type = r.Type;
			rgb(r.Color, r.Intensity * fk, l.Col);
			if(r.Type == 2) {
				// topDown : le sol est a Y = 0 et le haut est negatif : lumiere au-dessus du sol par defaut, sauf y= ecrit
				l.Pos[0] = r.X; l.Pos[1] = (td && !r.PosYGiven) ? -std::fabs(r.Y) : r.Y; l.Pos[2] = r.Z;
				l.Radius = r.Radius; l.Glow = r.Glow * fk; l.GlowRadius = r.GlowRadius;
			} else {
				const bool topSun = td && !r.DirGiven; // topDown : soleil venant du dessus par defaut
				const float sdx = topSun ? 0.3f : r.Dx, sdy = topSun ? 1.0f : r.Dy, sdz = topSun ? 0.3f : r.Dz;
				const float len = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
				l.Pos[0] = -sdx / len; l.Pos[1] = -sdy / len; l.Pos[2] = -sdz / len;
			}
		}
		if(anyTile) LogOnce(rt, 6, "<DioramaLight>tile= : lumieres attachees a une tile pas encore prises en charge sur SNES, ignorees.");
		scene.Lights.Active = any;
		if(any) LogOnce(rt, 7, "lumieres de scene : " + std::to_string(n) + " ponctuelle(s) ou solaire(s), " + std::to_string(D.DioramaLightRules.size()) + " regle(s)");
	}

	// G6c (etape 2c) : ombres des silhouettes : bande de lignes du masque, style de la plus petite regle utilisee (copie de GbHdDiorama.cpp)
	if(shadowsOn) {
		int y0 = INT32_MAX, y1 = -1, ruleId = INT32_MAX;
		for(int b = 0; b < nBands; b++) { y0 = std::min(y0, sMin[b]); y1 = std::max(y1, sMax[b]); ruleId = std::min(ruleId, sRule[b]); }
		if(y1 >= y0 && ruleId < (int)D.DioramaShadowRules.size() && scene.Lights.Active) {
			const HdDioramaShadowRule& sr = D.DioramaShadowRules[(size_t)ruleId];
			scene.Shadow.Active = true;
			scene.Shadow.Img = rt.Sh.data();
			scene.Shadow.Y0 = (uint32_t)y0; scene.Shadow.Rows = (uint32_t)(y1 - y0 + 1);
			scene.Shadow.Opacity = sr.Opacity; scene.Shadow.Soft = sr.Soft;
			scene.Shadow.Color[0] = ((sr.Color >> 16) & 0xFF) / 255.0f; scene.Shadow.Color[1] = ((sr.Color >> 8) & 0xFF) / 255.0f; scene.Shadow.Color[2] = (sr.Color & 0xFF) / 255.0f;
			scene.Shadow.OntoMask = sr.OntoMask;
			scene.Shadow.Light = sr.Light - 1;
			LogOnce(rt, 11, "ombres des silhouettes : regle " + std::to_string(ruleId + 1) + " sur " + std::to_string(D.DioramaShadowRules.size()) + ", opacite " + std::to_string(sr.Opacity) + ", masque onto= " + std::to_string(sr.OntoMask) + " (bit n = Ln, bits 4+ = groupes)");
		} else if(y1 >= y0 && !scene.Lights.Active) {
			LogOnce(rt, 12, "ombres des silhouettes : aucune <DioramaLight> active, pas d'ombre (il faut une lumiere ponctuelle ou solaire).");
		}
	}

	// <DioramaMist> (copie de GbHdDiorama.cpp, vue side ; repere = ref=bgN ou refx= / refy=)
	if(!D.DioramaMistRules.empty()) {
		const HdDioramaMistRule* mr = nullptr;
		for(const HdDioramaMistRule& r : D.DioramaMistRules) { if(CondsOk(r.Conditions)) mr = &r; }
		if(mr && !mr->Off) {
			auto& M = scene.Mist;
			M.Active = true;
			M.Count = mr->HasCount ? mr->Count : (td ? 3 : 2);
			M.From = mr->HasFrom ? mr->From : (td ? 4.0f : 120.0f);
			M.To = mr->HasTo ? mr->To : (td ? 28.0f : 30.0f);
			M.Opacity = mr->HasOpacity ? mr->Opacity : (td ? 0.3f : 0.25f);
			M.Size = mr->HasSize ? mr->Size : (td ? 72.0f : 96.0f);
			M.Coverage = mr->HasCoverage ? mr->Coverage : 0.55f;
			M.Soft = mr->HasSoft ? mr->Soft : 0.3f;
			M.Band = mr->HasBand ? mr->Band : 72.0f;
			M.Seed = mr->Seed;
			const uint32_t col = mr->HasColor ? mr->Color : 0xE0E8F0u;
			M.Col[0] = ((col >> 16) & 0xFF) / 255.0f; M.Col[1] = ((col >> 8) & 0xFF) / 255.0f; M.Col[2] = (col & 0xFF) / 255.0f;
			const float speed = mr->HasSpeed ? mr->Speed : (td ? 6.0f : 8.0f);
			const float dir = (mr->HasDir ? mr->Dir : (td ? 30.0f : 0.0f)) * 0.017453292f;
			const float tSec = (float)f.FrameNumber / 60.0f;
			for(int i = 0; i < M.Count && i < 8; i++) {
				const float k = 1.0f + 0.25f * (float)i;
				M.OffX[i] = wx - std::cos(dir) * speed * k * tSec + 37.0f * (float)i;
				M.OffY[i] = wy - std::sin(dir) * speed * k * tSec + 53.0f * (float)i;
			}
		}
	}
	// <DioramaClouds> (copie de GbHdDiorama.cpp, vue side)
	if(!D.DioramaCloudRules.empty()) {
		const HdDioramaCloudRule* cr[2] = { nullptr, nullptr };
		for(const HdDioramaCloudRule& r : D.DioramaCloudRules) { if(CondsOk(r.Conditions)) cr[r.Type & 1] = &r; }
		const float tSec = (float)f.FrameNumber / 60.0f;
		for(int k = 0; k < 2; k++) {
			const HdDioramaCloudRule* r = cr[k];
			if(!r || r->Off) continue;
			auto& C = scene.Cloud[k];
			C.Active = true;
			C.Size = r->HasSize ? r->Size : (k == 0 ? (td ? 96.0f : 64.0f) : (td ? 64.0f : 48.0f));
			const float speed = r->HasSpeed ? r->Speed : (k == 0 ? 10.0f : 4.0f);
			const float dir = (r->HasDir ? r->Dir : (td ? 30.0f : 0.0f)) * 0.017453292f;
			C.Coverage = r->HasCoverage ? r->Coverage : (k == 0 ? 0.45f : 0.5f);
			C.Soft = r->HasSoft ? r->Soft : (k == 0 ? 0.12f : 0.25f);
			C.Opacity = r->HasOpacity ? r->Opacity : (k == 0 ? (td ? 0.35f : 0.25f) : (td ? 0.55f : 0.35f));
			C.GroundH = r->HasGroundH ? r->GroundH : 16.0f;
			C.Seed = r->Seed;
			const uint32_t col = r->HasColor ? r->Color : (k == 0 ? 0x101828u : 0xC8D4E0u);
			C.Col[0] = ((col >> 16) & 0xFF) / 255.0f; C.Col[1] = ((col >> 8) & 0xFF) / 255.0f; C.Col[2] = (col & 0xFF) / 255.0f;
			C.OffX = wx - std::cos(dir) * speed * tSec;
			C.OffY = wy - std::sin(dir) * speed * tSec;
		}
	}
	// <DioramaFog> (copie de GbHdDiorama.cpp, vue side)
	if(!D.DioramaFogRules.empty()) {
		const HdDioramaFogRule* fr = nullptr;
		for(const HdDioramaFogRule& r : D.DioramaFogRules) { if(CondsOk(r.Conditions)) fr = &r; }
		if(fr && !fr->Off) {
			auto& F = scene.Fog;
			F.Active = true;
			F.Start = fr->HasStart ? fr->Start : (td ? -20.0f : 150.0f);
			F.End = fr->HasEnd ? fr->End : (td ? 200.0f : 500.0f);
			if(F.End <= F.Start + 1.0f) F.End = F.Start + 1.0f;
			F.Opacity = fr->HasOpacity ? fr->Opacity : (td ? 0.7f : 0.45f);
			F.Ground = fr->HasGround ? fr->Ground : 0.0f;
			F.GroundH = fr->HasGroundH ? fr->GroundH : 16.0f;
			const bool skyCol = (fr->HasColor ? fr->ColorSky : true) && scene.Sky.Mode == 1;
			if(skyCol) { F.Col[0] = scene.Sky.Bottom[0]; F.Col[1] = scene.Sky.Bottom[1]; F.Col[2] = scene.Sky.Bottom[2]; }
			else {
				const uint32_t c = (fr->HasColor && !fr->ColorSky) ? fr->Color : (td ? 0xC8D4E0u : 0xA8B8C8u);
				F.Col[0] = ((c >> 16) & 0xFF) / 255.0f; F.Col[1] = ((c >> 8) & 0xFF) / 255.0f; F.Col[2] = (c & 0xFF) / 255.0f;
			}
		}
	}

	const bool ok = gpu.SubmitScene(rt.L[0].data(), rt.L[1].data(), rt.L[2].data(), rt.L[3].data(), W, H, scene) && gpu.FetchOpaque(f.Out, pixels);
	if(!ok) {
		if(rt.Fail++ < 3) MessageManager::Log("[HD-SNES-DIORAMA] envoi ou relecture GPU en echec, rendu 2D utilise");
		return false;
	}
	if(!rt.Logged[8]) {
		rt.Logged[8] = true;
		char buf[260];
		snprintf(buf, sizeof(buf), "[HD-SNES-DIORAMA] scene active : %ux%u, mode BG %d, yaw %.1f, pitch %.1f, dist %.0f, couches BG1 %d/%d BG2 %d/%d BG3 %d/%d BG4 %d/%d sprites %d, repere BG%d, vue %s",
			W, H, f.BgMode, yaw, pitch, dist, bgLayer[0][0], bgLayer[0][1], bgLayer[1][0], bgLayer[1][1], bgLayer[2][0], bgLayer[2][1], bgLayer[3][0], bgLayer[3][1], sprLayer[0], dio.RefBg + 1, td ? "topDown" : "side");
		MessageManager::Log(buf);
	}
	return true;
}
