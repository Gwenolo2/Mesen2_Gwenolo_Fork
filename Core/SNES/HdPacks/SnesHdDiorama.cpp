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
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include "SNES/HdPacks/SnesHdDioramaRun.h"
#include "SNES/HdPacks/SnesHdDiorama.h"
#include "SNES/HdPacks/SnesHdThreadPool.h"
#include "SNES/HdPacks/SnesHdGpu.h"
#include "Shared/HdGpuCore.h"
#include "NES/HdPacks/HdGltfSkin.h"
#include "Shared/HdDayNight.h"
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
	// <DayNight> (etape 3a) : etat des cycles, sortie de l'image, derniere image avancee
	std::vector<HdDayNightState> DnState;
	HdDayNightOut Dn;
	uint32_t DnFrame = 0xFFFFFFFFu;
	// <3DParticles> du diorama (etape 3b-1, copie de GbHdVideoFilter / GbHdDiorama.cpp)
	struct P3Part { float X = 0, Y = 0, Z = 0, VX = 0, VY = 0, VZ = 0, Age = 0, Life = 0, Size = 1, Seed1 = 0, Seed2 = 0, EVX = 0, EVY = 0; };
	struct P3State { std::vector<P3Part> Parts; bool Init = false; bool TopDown = false; float Time = 0, SpawnAcc = 0; uint32_t Rng = 0x9E3779B9; };
	std::vector<P3State> P3;
	std::vector<float> P3Out[4];
	// objets 3D (etape 3a)
	std::vector<float> ObjSpin;
	int ObjLast = -1;
	// <Tile2Obj> / <Sprite2Obj> (etape 3b-2) : instances de l'image ; pixels SNES retires des couches (bit s = source 0..4) ;
	// animation glTF par personnage (id=), copie de HdNesPack (S2oAnimState, _s2oTime, _s2oPal, _s2oBuf)
	std::vector<HdDioramaObjectRule> T2oInst;
	std::vector<uint8_t> Hide;
	struct S2oAnimState
	{
		bool Has = false; uint32_t Frame = 0; float Time = 0.0f;
		const void* Model = nullptr; int Anim = -1; float Frozen = -1.0f, Speed = 1.0f;
		int PrevAnim = -1; float PrevTime = 0.0f, PrevFrozen = -1.0f, PrevSpeed = 1.0f;
		float BlendLeft = 0.0f, BlendDur = 0.0f;
	};
	std::unordered_map<std::string, S2oAnimState> S2oTime;
	std::vector<float> S2oPal;
	uint32_t S2oVersion = 0;
	std::unordered_map<uint64_t, std::shared_ptr<std::vector<float>>> S2oBuf;
	int T2oLast = -1, S2oLast = -1;
	// envoi partiel (perf) : empreinte de chaque ligne SNES de L0..L3 a l'image precedente, lignes a renvoyer
	uint32_t RowHash[4][240] = {}, RowHashNew[4][240] = {};
	uint8_t Dirty[4][240] = {};
	bool HashValid = false;
	// perf : lignes HD qui avaient du contenu a l'image precedente (0..3 = L0..L3, 4 = fixe, 5..12 = groupes, 13 = ombres) ;
	// seules ces lignes sont videes, puis seuls les pixels non vides sont ecrits
	std::vector<uint8_t> RowUsed[14];
	uint32_t HashW = 0, HashH = 0;
	// chronometrage (journal toutes les 300 images)
	double MsBuild = 0.0, MsScene = 0.0, MsGpu = 0.0;
	int MsFrames = 0;
	// async=on : une scene envoyee a l'image AsyncFrame attend sa relecture (taille AsyncPixels)
	bool AsyncPending = false;
	uint32_t AsyncFrame = 0;
	uint32_t Calls = 0; // appels de SnesHdDioRun (un par image du filtre) : ScreenInfo.FrameNumber est incremente par l'emulation, sur un autre thread
	size_t AsyncPixels = 0;
	// journaux (une fois)
	bool Logged[24] = {};
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

	// <Sprite2Obj> glTF anime : pose du personnage (id=) dans rt.S2oPal (copie de HdNesPack::S2oPose ; frame = compteur d'appels)
	void S2oPose(SnesHdDioRuntime& rt, const std::string& key, const HdDioramaTile2ObjRule& r, uint32_t frame)
	{
		const SnesHdModelRig* rig = r.Gltf ? r.Gltf->Rig.get() : nullptr;
		if(!rig) { rt.S2oPal.clear(); return; }
		SnesHdDioRuntime::S2oAnimState& st = rt.S2oTime[key];
		if(!st.Has || st.Frame != frame) {
			const float dt = st.Has ? (float)std::min<uint32_t>(frame - st.Frame, 4u) / 60.0f : 0.0f;
			const void* model = r.Gltf.get();
			const bool changed = st.Has && (st.Model != model || st.Anim != r.AnimIndex || st.Frozen != r.AnimTime);
			if(changed) {
				const float dur = r.Blend >= 0.0f ? r.Blend : 0.2f;
				if(st.Model == model && dur > 0.0f) {
					st.PrevAnim = st.Anim; st.PrevTime = st.Time; st.PrevFrozen = st.Frozen; st.PrevSpeed = st.Speed;
					st.BlendDur = dur; st.BlendLeft = dur;
				} else {
					st.BlendLeft = 0.0f;
				}
			}
			if(st.BlendLeft > 0.0f) {
				st.BlendLeft -= dt;
				if(st.PrevFrozen < 0.0f) st.PrevTime += st.PrevSpeed * dt;
			}
			if(r.AnimTime < 0.0f) st.Time += r.AnimSpeed * dt;
			st.Has = true; st.Frame = frame; st.Model = model; st.Anim = r.AnimIndex; st.Frozen = r.AnimTime; st.Speed = r.AnimSpeed;
		}
		const float curT = r.AnimTime >= 0.0f ? r.AnimTime : st.Time;
		if(st.BlendLeft > 0.0f && st.BlendDur > 0.0f) {
			const float w = 1.0f - st.BlendLeft / st.BlendDur;
			const float prevT = st.PrevFrozen >= 0.0f ? st.PrevFrozen : st.PrevTime;
			rig->EvaluateBlend(st.PrevAnim, prevT, r.AnimIndex, curT, w, rt.S2oPal);
		} else {
			rig->Evaluate(r.AnimIndex, curT, rt.S2oPal);
		}
	}

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

	// <DayNight> (doc 55) : avance des cycles une fois par image ; la derniere regle vraie donne la teinte (copie de GbHdVideoFilter::UpdateDayNight)
	void UpdateDayNight(SnesHdDioRuntime& rt, HdPackData& D, uint32_t frame)
	{
		if(rt.DnFrame == frame) return;
		rt.DnFrame = frame;
		rt.Dn = HdDayNightOut();
		const auto& rules = D.DayNightRules;
		if(rules.empty()) return;
		if(rt.DnState.size() != rules.size()) rt.DnState.assign(rules.size(), HdDayNightState());
		for(size_t i = 0; i < rules.size(); i++) {
			const auto& r = rules[i];
			const bool ok = CondsOk(r.Conditions);
			HdDayNightAdvance(r.Def, rt.DnState[i], frame, r.Def.TimeAlways || ok, [&](int s) {
				if(s < 0 || (size_t)s >= r.Until.size()) return true;
				return CondsOk(r.Until[s]);
			});
			if(ok) HdDayNightOutput(r.Def, rt.DnState[i], (int)i, rt.Dn);
		}
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
	rt.Calls++;
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
	// <DioramaSprites>tile= : couche de chaque liste de regles (derniere regle vraie), -1 = regle prio= du sprite
	std::vector<int> slotLayer(dio.SprSlots.size(), -1);
	for(size_t si = 0; si < dio.SprSlots.size(); si++) {
		for(int ri : dio.SprSlots[si]) { if(SnesCondsOk(dio.SpriteTileRules[(size_t)ri].Conditions)) slotLayer[si] = dio.SpriteTileRules[(size_t)ri].Layer; }
	}
	auto slotLay = [&](int slot1) -> int { return (slot1 > 0 && (size_t)slot1 <= slotLayer.size()) ? slotLayer[(size_t)slot1 - 1] : -1; };
	auto sprCandLayer = [&](const SnesHdDioPixel& r) -> int {
		const int sl = r.Tile ? slotLay(r.Tile->DioSprSlot + 1) : -1;
		return sl >= 0 ? sl : sprLayer[r.OamPrio & 3];
	};
	// topDown (decide avant la construction) : un sprite envoye dans une couche sans sprite prio= est couche a plat, pas debout
	bool tdPre = false;
	for(const HdDioramaViewRule& r : D.DioramaViews) { if(CondsOk(r.Conditions)) tdPre = true; }
	uint32_t sprMaskPre = 0;
	for(int p = 0; p < 4; p++) { if(sprLayer[p] >= 0 && sprLayer[p] <= 3) sprMaskPre |= 1u << sprLayer[p]; }
	const uint32_t flatPre = tdPre ? (~sprMaskPre & 0xFu) : 0u;
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
	for(int k = 0; k < 4; k++) { if(rt.L[k].size() != pixels) { rt.L[k].assign(pixels, 0u); rt.RowUsed[k].assign(H, 0); } }
	if(rt.Fix.size() != pixels) { rt.Fix.assign(pixels, 0u); rt.RowUsed[4].assign(H, 0); }
	for(int b = 0; b < 14; b++) { if(rt.RowUsed[b].size() != H) rt.RowUsed[b].assign(H, 1); } // taille inconnue : tout vider une fois
	const uint32_t sdW = W / S, sdH = H / S;
	const int nG = (int)std::min<size_t>(D.DioramaDepthGroups.size(), 8);
	for(int g = 0; g < nG; g++) { if(rt.G[g].size() != pixels) { rt.G[g].assign(pixels, 0u); rt.RowUsed[5 + g].assign(H, 0); } }
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
	// --- <Tile2Obj> / <Sprite2Obj> (etape 3b-2 ; copie de HdNesPack::DetectTile2Obj / DetectSprite2Obj avec les hashs SNES) ---
	// Tiles du pack reconnues dans le releve PPU (MemoryHash, comme les lumieres de tile) : leurs pixels sont retires des couches
	// (rt.Hide : bit s = source 0..4, le remplissage montre ce qui est dessous) et une instance d'objet 3D est creee (rt.T2oInst).
	rt.T2oInst.clear();
	const bool objTiles = (!dio.T2oKeys.empty() && !D.Tile2ObjRules.empty()) || (!dio.S2oKeys.empty() && !D.Sprite2ObjRules.empty());
	if(objTiles) rt.Hide.assign((size_t)239 * 256, 0);
	const uint8_t* const hideM = objTiles ? rt.Hide.data() : nullptr;
	if(objTiles) {
		const float offX = (float)(f.DstX - f.SrcX), offY = (float)(f.DstY - f.SrcY); // pixel SNES du releve -> pixel de la scene
		auto palOk = [](const SnesHdDiorama::ObjTileKey& k, const SnesHdDioPixel& r) {
			return k.AnyPalette || r.Tile->PaletteHash == k.Palette || r.Tile->PaletteHash == 0xFFFFFFFFu;
		};
		auto mirOk = [](const SnesHdDiorama::ObjTileKey& k, const SnesHdDioPixel& r) {
			const int hf = (r.Flags & 2) ? 1 : 0, vf = (r.Flags & 4) ? 1 : 0;
			return !((k.HMir >= 0 && hf != k.HMir) || (k.VMir >= 0 && vf != k.VMir));
		};
		// Tile2Obj : hash de la tile de BG du releve (calcule par la PPU, meme sans <tile> dans le pack), sinon celui de la tile HD
		auto bgHash = [](const SnesHdDioPixel& r) -> uint64_t { return r.Hash ? r.Hash : (r.Tile ? r.Tile->MemoryHash : 0); };
		auto bgPalOk = [](const SnesHdDiorama::ObjTileKey& k, const SnesHdDioPixel& r) {
			if(k.AnyPalette) return true;
			if(r.Tile) return r.Tile->PaletteHash == k.Palette || r.Tile->PaletteHash == 0xFFFFFFFFu;
			return (uint32_t)r.Pal == k.Palette;
		};
		auto recAt = [&](int s, int x, int y) -> const SnesHdDioPixel& { return f.Rec[((size_t)y * SnesHdDioSources + (size_t)s) * 256 + (size_t)x]; };
		// Tile2Obj : tiles de BG (sources 0..3), avec ou sans <tile> dans le pack (sans : la tile est simplement retiree, comme blank). Origine d'une tile = coin haut gauche a l'ecran (pixel releve - position dans la tile ;
		// tile de BG : TileX = colonne lue, retournee si la tile ne l'est pas). Un motif grid=CxL : chaque case a sa tile, meme BG.
		int t2oCount = 0;
		for(size_t ri = 0; ri < D.Tile2ObjRules.size() && ri < dio.T2oKeys.size(); ri++) {
			const HdDioramaTile2ObjRule& r = D.Tile2ObjRules[ri];
			const SnesHdDiorama::ObjTileKey& k = dio.T2oKeys[ri];
			const int gw = r.GridW, gh = r.GridH;
			if(r.Parts.empty() || k.Hashes.size() != (size_t)(gw * gh) || !CondsOk(r.Conditions)) continue;
			for(int s = 0; s < 4; s++) {
				std::vector<uint8_t> done((size_t)(256 + 8) * (239 + 8), 0); // origines deja traitees (decalees de 8)
				for(int y = 0; y < 239; y++) {
					for(int x = 0; x < 256; x++) {
						const SnesHdDioPixel& p0 = recAt(s, x, y);
						if(!(p0.Flags & 1) || bgHash(p0) != k.Hashes[0] || !bgPalOk(k, p0) || !mirOk(k, p0)) continue;
						const int lx = (p0.Flags & 2) ? (int)(p0.TileX & 7) : 7 - (int)(p0.TileX & 7);
						const int ly = (p0.Flags & 4) ? 7 - (int)(p0.TileY & 7) : (int)(p0.TileY & 7);
						const int ox = x - lx, oy = y - ly;
						if(ox < -8 || oy < -8) continue;
						uint8_t& dn = done[(size_t)(oy + 8) * (256 + 8) + (size_t)(ox + 8)];
						if(dn) continue;
						dn = 1;
						// chaque case (i, j) du motif : au moins un pixel releve de la meme tile, a la meme origine
						bool match = true;
						for(int j = 0; j < gh && match; j++) for(int i = 0; i < gw && match; i++) {
							if(i == 0 && j == 0) continue;
							const uint64_t h = k.Hashes[(size_t)j * gw + i];
							const int cx0 = ox + 8 * i, cy0 = oy + 8 * j;
							bool found = false;
							for(int yy = std::max(0, cy0); yy < std::min(239, cy0 + 8) && !found; yy++) for(int xx = std::max(0, cx0); xx < std::min(256, cx0 + 8) && !found; xx++) {
								const SnesHdDioPixel& q = recAt(s, xx, yy);
								if(!(q.Flags & 1) || bgHash(q) != h || !bgPalOk(k, q)) continue;
								const int qx = (q.Flags & 2) ? (int)(q.TileX & 7) : 7 - (int)(q.TileX & 7);
								const int qy = (q.Flags & 4) ? 7 - (int)(q.TileY & 7) : (int)(q.TileY & 7);
								if(xx - qx == cx0 && yy - qy == cy0) found = true;
							}
							if(!found) match = false;
						}
						if(!match) continue;
						for(int yy = std::max(0, oy); yy < std::min(239, oy + 8 * gh); yy++) for(int xx = std::max(0, ox); xx < std::min(256, ox + 8 * gw); xx++) rt.Hide[(size_t)yy * 256 + xx] |= (uint8_t)(1u << s);
						const int lay = bgLayer[s][(p0.Flags & 8) ? 1 : 0];
						const float pz = (lay >= 0 && lay <= 3) ? cfg.Z[lay] : 0.0f;
						const float bottom = (float)(oy + 8 * gh) + offY;
						for(const HdDioramaTile2ObjPart& part : r.Parts) { // une instance par materiau (texture propre)
							HdDioramaObjectRule o;
							o.Mesh = part.Mesh; o.Tex = part.Tex; o.Color = r.Color;
							o.X = (float)ox + 4.0f * (float)gw + offX;
							o.Y = bottom - r.Elevation;
							o.Z = pz; // vue side : plan de la couche du BG
							o.Scale = r.Scale; o.RotY = r.RotY; o.RotX = r.RotX; o.RotZ = r.RotZ; o.Radius = r.Radius;
							o.FromTile2Obj = true; o.BottomY = bottom; o.Elevation = r.Elevation; o.CastShadow = r.CastShadow; o.Lit = r.Lit; o.Bright = r.Bright;
							o.Conditions.clear();
							rt.T2oInst.push_back(o);
						}
						t2oCount++;
					}
				}
			}
		}
		// Sprite2Obj : silhouette = pixels de sprite releves qui se touchent (4-voisins, sprite le plus prioritaire de chaque pixel) ;
		// retenue si elle contient une tile de la liste ; sprite cache (hide=on) sur les pixels de ces tiles ; miroir si la plupart le sont.
		int s2oCount = 0;
		if(!dio.S2oKeys.empty() && !D.Sprite2ObjRules.empty()) {
			std::vector<uint8_t> taken((size_t)239 * 256, 0), seen((size_t)239 * 256, 0);
			std::vector<int> stack, comp;
			for(size_t ri = 0; ri < D.Sprite2ObjRules.size() && ri < dio.S2oKeys.size(); ri++) {
				const HdDioramaTile2ObjRule& r = D.Sprite2ObjRules[ri];
				const SnesHdDiorama::ObjTileKey& k = dio.S2oKeys[ri];
				if(r.Parts.empty() || !CondsOk(r.Conditions)) continue;
				int inst = 0;
				std::fill(seen.begin(), seen.end(), (uint8_t)0);
				auto matches = [&](const SnesHdDioPixel& q) {
					if(!(q.Flags & 1) || !q.Tile || !palOk(k, q)) return false;
					for(uint64_t h : k.Hashes) { if(q.Tile->MemoryHash == h) return true; }
					return false;
				};
				for(int y = 0; y < 239; y++) {
					for(int x = 0; x < 256; x++) {
						const int i0 = y * 256 + x;
						if(seen[i0] || taken[i0]) continue;
						const SnesHdDioPixel& p = recAt(4, x, y);
						if(!matches(p)) continue;
						const SnesHdDioPixel condTile = p; // tile de reference pour [hmirror] / [vmirror]
						int minX = x, maxX = x, minY = y, maxY = y, nMatch = 0, nH = 0, nV = 0;
						stack.clear(); comp.clear(); stack.push_back(i0); seen[i0] = 1;
						while(!stack.empty()) {
							const int c = stack.back(); stack.pop_back(); comp.push_back(c);
							const int cy = c / 256, cx = c % 256;
							minX = std::min(minX, cx); maxX = std::max(maxX, cx); minY = std::min(minY, cy); maxY = std::max(maxY, cy);
							const SnesHdDioPixel& q = recAt(4, cx, cy);
							if(matches(q)) { nMatch++; if(q.Flags & 2) nH++; if(q.Flags & 4) nV++; }
							const int nr[4] = { cy - 1, cy + 1, cy, cy }, nx[4] = { cx, cx, cx - 1, cx + 1 };
							for(int d = 0; d < 4; d++) {
								if(nr[d] < 0 || nr[d] >= 239 || nx[d] < 0 || nx[d] > 255) continue;
								const int j = nr[d] * 256 + nx[d];
								if(seen[j] || taken[j] || !(recAt(4, nx[d], nr[d]).Flags & 1)) continue;
								seen[j] = 1; stack.push_back(j);
							}
						}
						if(!mirOk(k, condTile)) continue; // la silhouette reste libre pour une regle suivante
						for(int c : comp) {
							taken[c] = (uint8_t)(ri + 1);
							if(r.Hide && matches(recAt(4, c % 256, c / 256))) rt.Hide[(size_t)c] |= 0x10;
						}
						const int lay = sprCandLayer(condTile);
						const float pz = (lay >= 0 && lay <= 3) ? cfg.Z[lay] : cfg.Z[3];
						const float foot = (float)(maxY + 1) + offY;
						const bool animated = r.Gltf && r.AnimIndex >= 0 && r.PartTris.size() == r.Parts.size();
						if(animated) {
							S2oPose(rt, r.Id.empty() ? ("#" + std::to_string(ri)) : r.Id, r, rt.Calls);
							rt.S2oVersion++;
						}
						for(size_t pi = 0; pi < r.Parts.size(); pi++) { // une instance par materiau
							const HdDioramaTile2ObjPart& part = r.Parts[pi];
							HdDioramaObjectRule o;
							o.Mesh = part.Mesh; o.Tex = part.Tex; o.Color = r.Color;
							if(animated) {
								std::shared_ptr<std::vector<float>>& buf = rt.S2oBuf[((uint64_t)ri << 32) | ((uint64_t)(inst & 0xFFFFFF) << 8) | (uint64_t)(pi & 0xFF)];
								if(!buf) buf = std::make_shared<std::vector<float>>();
								HdGltfSkinPart(*r.Gltf, r.PartTris[pi], &rt.S2oPal, *buf);
								o.Mesh = buf; o.MeshVersion = rt.S2oVersion;
							}
							o.X = (float)(minX + maxX + 1) * 0.5f + offX;
							o.Y = foot - r.Elevation;
							o.Z = pz; // vue side : plan de la couche du sprite
							o.Scale = r.Scale; o.RotY = r.RotY; o.RotX = r.RotX; o.RotZ = r.RotZ; o.Radius = r.Radius;
							o.FromTile2Obj = true; o.BottomY = foot; o.Elevation = r.Elevation; o.CastShadow = r.CastShadow; o.Lit = r.Lit; o.Bright = r.Bright;
							o.MirrorX = r.Facing && nH * 2 > nMatch;
							o.MirrorY = r.Facing && nV * 2 > nMatch;
							if(o.MirrorY) { // retourne en haut / bas : le modele pend sous son origine, qui passe en haut de la silhouette
								o.Y = (float)minY + offY - r.Elevation;
								o.Elevation = r.Elevation + (foot - ((float)minY + offY));
							}
							rt.T2oInst.push_back(o);
						}
						inst++;
						s2oCount++;
					}
				}
			}
		}
		if(t2oCount != rt.T2oLast || s2oCount != rt.S2oLast) {
			rt.T2oLast = t2oCount; rt.S2oLast = s2oCount;
			MessageManager::Log("[HD-SNES-DIORAMA] <Tile2Obj> : " + std::to_string(t2oCount) + " motif(s) reconnu(s) ; <Sprite2Obj> : " + std::to_string(s2oCount) + " silhouette(s), frame " + std::to_string(f.FrameNumber));
		}
	}
	std::vector<int> fixMin, fixMax;
	const int nBands = (f.Pool && f.Pool->Size() > 1 && H >= 64) ? f.Pool->Size() : 1;
	fixMin.assign((size_t)nBands, INT32_MAX); fixMax.assign((size_t)nBands, -1);
	std::vector<int> gMin((size_t)nBands * 8, INT32_MAX), gMax((size_t)nBands * 8, -1);
	// G6c (etape 2c) : regle d'ombre de chaque pixel SNES, d'apres le releve PPU (le sprite le plus prioritaire, meme cache par un BG).
	// Tile avec un bloc <DioramaShadow>{ } (sprite ou BG), sinon forme ligne tile=HASH|* dont les conditions sont vraies (sprites).
	const bool shadowsOn = !D.DioramaShadowRules.empty();
	std::vector<int> sMin((size_t)nBands, INT32_MAX), sMax((size_t)nBands, -1), sRule((size_t)nBands, INT32_MAX);
	if(shadowsOn) {
		if(rt.Sh.size() != pixels) { rt.Sh.assign(pixels, 0u); rt.RowUsed[13].assign(H, 0); }
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
					if(hideM && ((hideM[(size_t)y * 256 + x] >> s) & 1)) continue; // remplace par un objet 3D (Tile2Obj / Sprite2Obj)
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
	// bandes alignees sur les lignes SNES (empreintes et cache par ligne SNES)
	const uint32_t bandH = ((H / S + (uint32_t)nBands - 1) / (uint32_t)nBands) * S;
	std::vector<uint8_t> l0Uni((size_t)nBands, 1);
	std::vector<uint32_t> l0First((size_t)nBands, 0u);
	std::vector<uint8_t> l0Has((size_t)nBands, 0);
	// cache par pixel SNES des sources relevees (calcule une fois par ligne SNES) : source, couche, Z, groupe, tries par Z decroissant
	struct RcSet { int8_t n; int8_t s[5]; int8_t lay[5]; uint8_t zc[5]; uint8_t grp[5]; };
	auto build = [&](int band) {
		const uint32_t y0 = (uint32_t)band * bandH, y1 = std::min(H, y0 + bandH);
		std::vector<RcSet> rcRow(sdW);
		uint32_t hsh[4] = {};
		for(uint32_t hy = y0; hy < y1; hy++) {
			const int snesY = (int)(hy / S) - f.DstY + f.SrcY;
			const bool rowOk = snesY >= 0 && snesY < 239;
			const SnesHdDioPixel* rec = rowOk ? f.Rec + (size_t)snesY * SnesHdDioSources * 256 : nullptr;
			const uint32_t bdc = rowOk ? (f.Palette[f.Backdrop[snesY] & 0x7FFF] | 0xFF000000u) : 0xFF000000u;
			const uint32_t v = hy % S;
			if(v == 0 || hy == y0) {
				for(int k = 0; k < 4; k++) hsh[k] = 2166136261u;
				for(uint32_t cx = 0; cx < sdW; cx++) {
					RcSet& rs = rcRow[cx];
					rs.n = 0;
					const int sx = (int)cx - f.DstX + f.SrcX;
					if(!rec || sx < 0 || sx >= 256) continue;
					for(int s = 0; s < (int)SnesHdDioSources; s++) {
						const SnesHdDioPixel& r = rec[(size_t)s * 256 + sx];
						if(!(r.Flags & 1)) continue;
						if(hideM && ((hideM[(size_t)snesY * 256 + sx] >> s) & 1)) continue; // remplace par un objet 3D
						const bool hi = s < 4 ? (r.Flags & 8) != 0 : r.OamPrio >= 2;
						const int zc = 20 + r.Prio * 4 + (hi ? 2 : 0);
						int j = rs.n++;
						while(j > 0 && rs.zc[j - 1] < zc) { rs.s[j] = rs.s[j - 1]; rs.lay[j] = rs.lay[j - 1]; rs.zc[j] = rs.zc[j - 1]; rs.grp[j] = rs.grp[j - 1]; j--; }
						rs.s[j] = (int8_t)s; rs.lay[j] = (int8_t)(s < 4 ? bgLayer[s][hi ? 1 : 0] : sprCandLayer(r)); rs.zc[j] = (uint8_t)std::min(zc, 255);
						rs.grp[j] = (uint8_t)(nG > 0 ? groupOf(r) : 0);
					}
				}
			}
			// lignes qui avaient du contenu : videes en bloc ; ensuite, seuls les pixels non vides sont ecrits
			uint32_t* const rowBuf[14] = {
				rt.L[0].data(), rt.L[1].data(), rt.L[2].data(), rt.L[3].data(), rt.Fix.data(),
				nG > 0 ? rt.G[0].data() : nullptr, nG > 1 ? rt.G[1].data() : nullptr, nG > 2 ? rt.G[2].data() : nullptr, nG > 3 ? rt.G[3].data() : nullptr,
				nG > 4 ? rt.G[4].data() : nullptr, nG > 5 ? rt.G[5].data() : nullptr, nG > 6 ? rt.G[6].data() : nullptr, nG > 7 ? rt.G[7].data() : nullptr,
				shadowsOn ? rt.Sh.data() : nullptr };
			uint8_t rowHas[14] = {};
			for(int b = 0; b < 14; b++) {
				if(rowBuf[b] && rt.RowUsed[b][hy]) memset(rowBuf[b] + (size_t)hy * W, 0, W * sizeof(uint32_t));
			}
			bool fixRow = false;
			for(uint32_t hx = 0; hx < W; hx++) {
				const RcSet& rs = rcRow[std::min(hx / S, sdW - 1)];
				uint32_t lv[4] = { 0u, 0u, 0u, 0u }, fx = 0u, shv = 0u;
				uint32_t gv[8] = {};
				const size_t i = (size_t)hy * W + hx;
				if(shadowsOn) {
					const int sx = (int)(hx / S) - f.DstX + f.SrcX;
					if(rowOk && sx >= 0 && sx < 256) {
						const size_t si = (size_t)snesY * 256 + sx;
						const int shr = rt.ShSd[si];
						if(shr >= 0) {
							const uint32_t col = recColor(rt.ShSrc[si], rec[(size_t)rt.ShSrc[si] * 256 + sx], hx % S, v);
							if(col >> 24) {
								shv = col;
								sMin[band] = std::min(sMin[band], (int)hy); sMax[band] = std::max(sMax[band], (int)hy); sRule[band] = std::min(sRule[band], shr);
							}
						}
					}
				}
				const uint8_t z = f.Z[i];
				const int src = zSrc[z];
				int own = layerOfZ(z);
				if(src == 4 && f.OwnSl && f.OwnSl[i]) { const int sl = slotLay(f.OwnSl[i]); if(sl >= 0) own = sl; } // <DioramaSprites>tile=
				// Tile2Obj / Sprite2Obj : source visible remplacee par un objet 3D -> retiree (comme off), le dessous remplit les couches
				const int snesXh = (int)(hx / S) - f.DstX + f.SrcX;
				const uint8_t hideB = (hideM && rowOk && snesXh >= 0 && snesXh < 256) ? hideM[(size_t)snesY * 256 + snesXh] : 0;
				if(src >= 0 && src < 5 && ((hideB >> src) & 1)) own = SnesHdDioOff;
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
				// Couche montee : une source vraiment SOUS le pixel visible (Z plus petit, d'apres le releve PPU ou les fonds du pack) placee
				// dans une couche plus en avant (ex. menu BG3 en fixed, oiseau dessine par-dessus) : le pixel visible monte dans cette couche,
				// sinon il resterait un trou dans la couche de devant (on verrait le fond, et le sprite decale derriere). Hors groupes.
				if(ownG == 0 && own != SnesHdDioOff) {
					int bestRank = RankOf(own), bestLay = own;
					for(int q = 0; q < rs.n; q++) {
						if(rs.s[q] == src || rs.zc[q] >= z) continue; // pas dessous
						const int lay = rs.lay[q];
						if(lay != SnesHdDioOff && RankOf(lay) > bestRank) { bestRank = RankOf(lay); bestLay = lay; }
					}
					for(int m = 0; f.BgCol && m < 4; m++) { if(!(f.BgPileMask & (1u << m))) continue; // piles de fonds (une par BG d'ancrage)
						const size_t bi = (size_t)m * pixels + i;
						if(f.BgZ[bi] >= z || !(f.BgCol[bi] >> 24)) continue;
						const int lay = layerOfZ(f.BgZ[bi]);
						if(lay != SnesHdDioOff && RankOf(lay) > bestRank) { bestRank = RankOf(lay); bestLay = lay; }
					}
					own = bestLay;
				}
				if(f.TrCol && f.TrZ[i] > z && (f.TrCol[i] >> 24)) {
					uint32_t clean = 0;
					int bgOwn = -1; // pile de fonds dont le fond opaque est la source visible
					for(int m = 0; f.BgCol && m < 4 && bgOwn < 0; m++) { if(!(f.BgPileMask & (1u << m))) continue; const size_t bi = (size_t)m * pixels + i; if(f.BgZ[bi] == z && (f.BgCol[bi] >> 24) == 255) bgOwn = m; }
					if(bgOwn >= 0) clean = f.BgCol[(size_t)bgOwn * pixels + i] | 0xFF000000u;
					else if(src < 5 && rec && snesXo >= 0 && snesXo < 256 && (rec[(size_t)src * 256 + snesXo].Flags & 1)) {
						const uint32_t rc = recColor(src, rec[(size_t)src * 256 + snesXo], hx % S, v);
						if((rc >> 24) == 255) clean = rc;
					} else if(src == 5) clean = bdc;
					int tl = layerOfZ(f.TrZ[i]);
					if(zSrc[f.TrZ[i]] == 4 && f.TrSl && f.TrSl[i]) { const int sl = slotLay(f.TrSl[i]); if(sl >= 0) tl = sl; }
					const int tg = (nG > 0 && f.TrG && f.TrG[i] <= nG) ? f.TrG[i] : 0;
					// le bord part s'il est dans une couche devant, ou dans un autre groupe que la source visible (meme couche d'origine)
					const bool moves = tl == SnesHdDioOff || own == SnesHdDioOff || RankOf(tl) > RankOf(own) ||
						(RankOf(tl) == RankOf(own) && tg != ownG && tl != SnesHdDioFixed);
					const int trSrc = zSrc[f.TrZ[i]];
					const bool trHidden = trSrc >= 0 && trSrc < 5 && ((hideB >> trSrc) & 1); // bord d'une tile remplacee par un objet : supprime
					if(clean && (moves || trHidden)) {
						c = clean;
						if(tl != SnesHdDioOff && !trHidden) { trCol = Premul(f.TrCol[i]); trLayer = tl; trGroup = (tl == SnesHdDioFixed) ? 0 : tg; }
					}
				}
				if(hx % S == S / 2 && v == S / 2 && hx / S < sdW && hy / S < sdH) rt.SprSd[(size_t)(hy / S) * sdW + hx / S] =
					(own == SnesHdDioOff) ? 0 : (ownG > 0 && dio.GroupStand[ownG - 1]) ? (uint8_t)(1 + ownG) : ((src == 4 && own < 4 && !((flatPre >> own) & 1u)) ? 1 : 0);
				auto markG = [&](int g) { const size_t k = (size_t)band * 8 + (size_t)g; gMin[k] = std::min(gMin[k], (int)hy); gMax[k] = std::max(gMax[k], (int)hy); };
				int ownRank;
				if(own == SnesHdDioOff) {
					ownRank = 6; // source retiree : rien n'est pose, tout ce qui est derriere peut se voir
				} else if(own == SnesHdDioFixed) {
					fx = c; fixRow = true; ownRank = 4;
				} else if(ownG > 0) {
					gv[ownG - 1] = c; markG(ownG - 1);
					ownRank = own;
				} else {
					// skybox : seul le vrai fond (backdrop) devient transparent. Un <background> du pack dont le Z n'est pas celui d'une
					// tile (ex. sol de Zelda 3, Z 20) est range avec le fond par la table des Z : on le reconnait a sa pile capturee.
					bool backdrop = src == 5 && sky;
					for(int m = 0; backdrop && f.BgCol && m < 4; m++) {
						if(!(f.BgPileMask & (1u << m))) continue;
						const size_t bi = (size_t)m * pixels + i;
						if(f.BgZ[bi] == z && (f.BgCol[bi] >> 24)) backdrop = false;
					}
					lv[own] = backdrop ? 0u : c;
					ownRank = own;
				}
				// remplissage sous le pixel visible : sources de rang inferieur, de la plus prioritaire a la moins prioritaire ; si la
				// tile visible est partie dans un groupe, sa couche d'origine est remplie aussi (sol sous un buisson du groupe)
				const int snesX = (int)(hx / S) - f.DstX + f.SrcX;
				const int fillRank = (ownG > 0 && own != SnesHdDioOff && own != SnesHdDioFixed) ? ownRank + 1 : ownRank;
				if(fillRank > 0) {
					// candidats : sources PPU (0..4) et fonds <background> du pack (5), tries par Z decroissant (Z = 20 + priorite x 4, +2)
					int cs[SnesHdDioSources + 4], cl[SnesHdDioSources + 4], cz[SnesHdDioSources + 4], cg[SnesHdDioSources + 4], n = 0;
					auto addCand = [&](int s, int lay, int zc, int grp) {
						if(lay == SnesHdDioOff || RankOf(lay) >= fillRank) return;
						int j = n++;
						while(j > 0 && cz[j - 1] < zc) { cs[j] = cs[j - 1]; cl[j] = cl[j - 1]; cz[j] = cz[j - 1]; cg[j] = cg[j - 1]; j--; }
						cs[j] = s; cl[j] = lay; cz[j] = zc; cg[j] = (lay == SnesHdDioFixed) ? 0 : grp;
					};
					for(int q = 0; q < rs.n; q++) { if(rs.s[q] != src) addCand(rs.s[q], rs.lay[q], rs.zc[q], rs.grp[q]); }
					for(int m = 0; f.BgCol && m < 4; m++) { if(!(f.BgPileMask & (1u << m))) continue; // fonds du pack : une pile par BG d'ancrage (candidat 5 + m)
						const size_t bi = (size_t)m * pixels + i;
						if(f.BgZ[bi] == z || !(f.BgCol[bi] >> 24)) continue;
						addCand(5 + m, layerOfZ(f.BgZ[bi]), f.BgZ[bi], 0);
					}
					const uint32_t u = hx % S;
					for(int k = 0; k < n; k++) {
						uint32_t& dst = cg[k] > 0 ? gv[cg[k] - 1] : (cl[k] == SnesHdDioFixed ? fx : lv[cl[k]]);
						if(dst >> 24) continue;
						const uint32_t col = cs[k] >= 5 ? Premul(f.BgCol[(size_t)(cs[k] - 5) * pixels + i]) : recColor(cs[k], rec[(size_t)cs[k] * 256 + snesX], u, v);
						if(!(col >> 24)) continue;
						dst = col;
						if(cg[k] > 0) markG(cg[k] - 1);
						else if(cl[k] == SnesHdDioFixed) fixRow = true;
					}
				}
				if(trLayer >= 0) { // bord semi-transparent dans sa couche (ou son groupe), par-dessus ce qui y est deja (remplissage compris)
					uint32_t& td = trGroup > 0 ? gv[trGroup - 1] : (trLayer == SnesHdDioFixed ? fx : lv[trLayer]);
					if(trGroup > 0) markG(trGroup - 1);
					const uint32_t d = td, a = trCol >> 24, ia = 255 - a;
					const uint32_t rb = ((trCol & 0xFF00FFu) + (((d & 0xFF00FFu) * ia / 255) & 0xFF00FFu)) & 0xFF00FFu;
					const uint32_t g = ((trCol & 0x00FF00u) + (((d & 0x00FF00u) * ia / 255) & 0x00FF00u)) & 0x00FF00u;
					const uint32_t na = a + (d >> 24) * ia / 255;
					td = (std::min<uint32_t>(na, 255) << 24) | rb | g;
					if(trLayer == SnesHdDioFixed) fixRow = true;
				}
				if(!sky && !(lv[0] >> 24)) lv[0] = bdc; // L0 sans trou : couleur du fond de la ligne
				// empreintes de la ligne SNES (envoi partiel) et L0 d'une seule couleur
				for(int k = 0; k < 4; k++) hsh[k] = (hsh[k] ^ lv[k]) * 16777619u;
				if(!l0Has[band]) { l0Has[band] = 1; l0First[band] = lv[0]; }
				else if(l0Uni[band] && lv[0] != l0First[band]) l0Uni[band] = 0;
				// ecriture des seuls pixels non vides (les lignes ont ete videes au debut si besoin)
				for(int k = 0; k < 4; k++) { if(lv[k]) { rowBuf[k][i] = lv[k]; rowHas[k] = 1; } }
				if(fx) { rowBuf[4][i] = fx; rowHas[4] = 1; }
				for(int g = 0; g < nG; g++) { if(gv[g]) { rowBuf[5 + g][i] = gv[g]; rowHas[5 + g] = 1; } }
				if(shv) { rowBuf[13][i] = shv; rowHas[13] = 1; }
			}
			for(int b = 0; b < 14; b++) { if(rowBuf[b]) rt.RowUsed[b][hy] = rowHas[b]; }
			if(v == S - 1 || hy + 1 == y1) {
				const uint32_t sdRow = hy / S;
				if(sdRow < 240) { for(int k = 0; k < 4; k++) rt.RowHashNew[k][sdRow] = hsh[k]; }
			}
			if(fixRow) { fixMin[band] = std::min(fixMin[band], (int)hy); fixMax[band] = std::max(fixMax[band], (int)hy); }
		}
	};
	const auto tBuild = std::chrono::steady_clock::now();
	if(nBands > 1) f.Pool->Run(nBands, [&](int t) { build(t); });
	else build(0);
	const auto tBuilt = std::chrono::steady_clock::now();
	// envoi partiel : une ligne SNES n'est renvoyee au GPU que si son empreinte a change (premiere image, taille changee : tout)
	{
		const bool valid = rt.HashValid && rt.HashW == W && rt.HashH == H;
		for(int k = 0; k < 4; k++) {
			for(uint32_t r = 0; r < 240; r++) {
				rt.Dirty[k][r] = (!valid || r >= sdH || rt.RowHashNew[k][r] != rt.RowHash[k][r]) ? 1 : 0;
				rt.RowHash[k][r] = rt.RowHashNew[k][r];
			}
		}
		rt.HashValid = true; rt.HashW = W; rt.HashH = H;
	}
	bool l0Uniform = true;
	{
		uint32_t first = 0; bool has = false;
		for(int b = 0; b < nBands && l0Uniform; b++) {
			if(!l0Has[b]) continue;
			if(!l0Uni[b]) l0Uniform = false;
			else if(!has) { has = true; first = l0First[b]; }
			else if(first != l0First[b]) l0Uniform = false;
		}
	}
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
					if(scene.SpriteBlobN < 16) { // ombres : 16 silhouettes (tableau du shader)
						float* b = scene.SpriteBlob[scene.SpriteBlobN++];
						b[0] = (float)minX; b[1] = (float)(maxX + 1); b[2] = (float)r; b[3] = (float)(maxR + 1);
					}
					if(scene.SpriteBlobXN < 64) { // sprites debout : 64 silhouettes (au-dela : couche entiere, decalage visible)
						float* b = scene.SpriteBlobX[scene.SpriteBlobXN++];
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
		if(scene.SpriteBlobTotal > 64) LogOnce(rt, 17, "topDown : plus de 64 silhouettes de sprites dans une image, couche des sprites dessinee d'un bloc pour cette image (decalage possible).");
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
	{ // L0 d'une seule couleur (calcule pendant la construction) : pas d'envoi, couleur d'effacement
		const uint32_t c0 = rt.L[0][0];
		const bool uni = l0Uniform && (c0 >> 24) == 0xFF;
		scene.L0Uniform = uni && scene.Thick[0] <= 0.0f;
		if(uni) scene.Clear = c0 | 0xFF000000u;
	}
	for(int k = 0; k < 4; k++) scene.RowDirty[k] = rt.Dirty[k]; // envoi partiel (G3h) : lignes SNES changees
	scene.RowY0 = 0; scene.RowScale = S;

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

	// G6a / G6b : lumieres de scene (doc 42) : regles vraies ; 8 lumieres au plus, derniere ambiante vraie. Une regle tile=HASH (hash SNES)
	// donne une lumiere par motif de sprite reconnu a l'ecran (tiles voisines d'une meme regle regroupees, position moyenne), comme le NES.
	if(!D.DioramaLightRules.empty()) {
		auto rgb = [](uint32_t c, float k, float* o) { o[0] = ((c >> 16) & 0xFF) / 255.0f * k; o[1] = ((c >> 8) & 0xFF) / 255.0f * k; o[2] = (c & 0xFF) / 255.0f * k; };
		int n = 0;
		bool any = false;
		const float tSec = (float)f.FrameNumber / 60.0f;
		auto addLight = [&](const HdDioramaLightRule& r, float px, float py, bool onFloor) {
			if(n >= 8) return;
			HdDioramaLight& l = scene.Lights.L[n++];
			const float fk = r.Flicker > 0.0f ? 1.0f - r.Flicker * 0.5f * (1.0f + std::sin(r.FlickerSpeed * tSec)) : 1.0f;
			l.Type = r.Type;
			rgb(r.Color, r.Intensity * fk, l.Col);
			if(r.Type == 2) {
				if(td && onFloor) { // topDown, lumiere de tile : posee sur le sol (Y = -|y| au-dessus, profondeur = verticale de l'image)
					l.Pos[0] = px; l.Pos[1] = -std::fabs(r.Y); l.Pos[2] = scene.H * 0.5f - py;
				} else { l.Pos[0] = px; l.Pos[1] = py; l.Pos[2] = r.Z; }
				l.Radius = r.Radius; l.Glow = r.Glow * fk; l.GlowRadius = r.GlowRadius;
			} else {
				const bool topSun = td && !r.DirGiven; // topDown : soleil venant du dessus par defaut
				const float sdx = topSun ? 0.3f : r.Dx, sdy = topSun ? 1.0f : r.Dy, sdz = topSun ? 0.3f : r.Dz;
				const float len = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
				l.Pos[0] = -sdx / len; l.Pos[1] = -sdy / len; l.Pos[2] = -sdz / len;
			}
		};
		std::vector<uint8_t> isTile(D.DioramaLightRules.size(), 0);
		for(const SnesHdDiorama::LightTileKey& k : dio.LightTiles) { if((size_t)k.Rule < isTile.size()) isTile[(size_t)k.Rule] = 1; }
		bool anyNesTile = false;
		for(size_t ri = 0; ri < D.DioramaLightRules.size(); ri++) {
			const HdDioramaLightRule& r = D.DioramaLightRules[ri];
			if(isTile[ri]) continue;
			if(r.HasTile) { anyNesTile = true; continue; }
			if(!CondsOk(r.Conditions)) continue;
			any = true;
			if(r.Type == 1) { rgb(r.Color, r.Intensity, scene.Lights.Ambient); continue; }
			addLight(r, r.X, (td && !r.PosYGiven) ? -std::fabs(r.Y) : r.Y, false);
		}
		if(anyNesTile) LogOnce(rt, 6, "<DioramaLight>tile= : hash non lu, ligne ignoree.");
		// lumieres de tile : sprites du releve PPU (le plus prioritaire par pixel) dont la tile HD a ce hash
		if(!dio.LightTiles.empty() && f.Rec) {
			std::vector<const SnesHdDiorama::LightTileKey*> keys;
			for(const SnesHdDiorama::LightTileKey& k : dio.LightTiles) {
				if((size_t)k.Rule < D.DioramaLightRules.size() && CondsOk(D.DioramaLightRules[(size_t)k.Rule].Conditions)) keys.push_back(&k);
			}
			std::vector<uint32_t> seen;
			struct Near { int Rule; float Ox, Oy, Cnt, Sx, Sy; };
			std::vector<Near> nearL;
			for(int y = 0; y < 239 && !keys.empty(); y++) {
				const SnesHdDioPixel* rr = f.Rec + (size_t)y * SnesHdDioSources * 256 + 4 * 256;
				for(int x = 0; x < 256; x++) {
					const SnesHdDioPixel& r = rr[x];
					if(!(r.Flags & 1) || !r.Tile) continue;
					for(const SnesHdDiorama::LightTileKey* k : keys) {
						if(r.Tile->MemoryHash != k->Hash) continue;
						if(!k->AnyPalette && r.Tile->PaletteHash != k->Palette && r.Tile->PaletteHash != 0xFFFFFFFFu) continue;
						const bool hf = (r.Flags & 2) != 0, vf = (r.Flags & 4) != 0;
						if((k->HMir >= 0 && (int)hf != k->HMir) || (k->VMir >= 0 && (int)vf != k->VMir)) continue; // [hmirror] / [!hmirror]...
						const int ox = x - (hf ? 7 - (int)r.TileX : (int)r.TileX);
						const int oy = y - (vf ? 7 - (int)r.TileY : (int)r.TileY);
						const uint32_t id = ((uint32_t)(k->Rule & 0xFFF) << 20) | ((uint32_t)((ox + 16) & 0x3FF) << 10) | (uint32_t)((oy + 16) & 0x3FF);
						if(std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
						seen.push_back(id);
						const HdDioramaLightRule& lr = D.DioramaLightRules[(size_t)k->Rule];
						// position dans la scene (pixels SNES de l'image), centre de la tile + decalage (inverse si la tile est retournee)
						const float lpx = (float)(ox - f.SrcX + f.DstX) + 4.0f + (hf ? -lr.OffX : lr.OffX);
						const float lpy = (float)(oy - f.SrcY + f.DstY) + 4.0f + (vf ? -lr.OffY : lr.OffY);
						bool merged = false;
						for(Near& q : nearL) {
							if(q.Rule == k->Rule && std::abs(q.Ox - (float)ox) <= 16.0f && std::abs(q.Oy - (float)oy) <= 16.0f) { q.Cnt += 1.0f; q.Sx += lpx; q.Sy += lpy; merged = true; break; }
						}
						if(!merged) nearL.push_back({ k->Rule, (float)ox, (float)oy, 1.0f, lpx, lpy });
					}
				}
			}
			for(const Near& q : nearL) {
				any = true;
				addLight(D.DioramaLightRules[(size_t)q.Rule], q.Sx / q.Cnt, q.Sy / q.Cnt, true);
			}
			if(!nearL.empty()) LogOnce(rt, 18, "lumieres de tile : " + std::to_string(nearL.size()) + " motif(s) de sprite reconnu(s) (regles tile= : " + std::to_string(dio.LightTiles.size()) + ")");
		}
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

	// <3DParticles> (diorama, copie de GbHdDiorama.cpp) : boite torique autour de la camera, defauts selon la vue ; repere = ref=bgN / refx=.
	// topDown : X / Y = carte, Z = hauteur ; side : X = carte, Y = profondeur (depthmin..depthmax), Z = hauteur depuis le bas de l'ecran.
	if(!D.Diorama3DParticles.empty()) {
		if(rt.P3.size() != D.Diorama3DParticles.size()) rt.P3.assign(D.Diorama3DParticles.size(), SnesHdDioRuntime::P3State());
		const float dt = 1.0f / 60.0f, kPi = 3.14159265f;
		for(size_t pi = 0; pi < D.Diorama3DParticles.size() && scene.P3Count < 4; pi++) {
			const HdDiorama3DParticlesRule& R = D.Diorama3DParticles[pi];
			if(!CondsOk(R.Conditions)) continue; // effet fige
			SnesHdDioRuntime::P3State& st = rt.P3[pi];
			if(st.Init && st.TopDown != td) st.Init = false; // changement de vue : nouvelle boite
			st.TopDown = td;
			const float B = R.HasBoxSize ? R.BoxSize : (td ? 384.0f : 320.0f);
			const float BH = td ? (R.HasBoxHeight ? R.BoxHeight : 96.0f) : scene.H + 32.0f;
			const float ahead = R.HasBoxAhead ? R.BoxAhead : (td ? 0.25f : 0.0f);
			const float fallMin = R.HasFall ? R.FallMin : (td ? 20.0f : 30.0f), fallMax = R.HasFall ? R.FallMax : (td ? 40.0f : 60.0f);
			const float dMin = R.DepthMin, dMax = R.DepthMax, DB = dMax - dMin;
			const float ox = wx, oy = wy; // repere du monde (pixels SNES)
			const float cX = ox + scene.W * 0.5f;
			const float cY = td ? oy + scene.H * 0.5f - B * ahead : dMin + DB * 0.5f;
			const float BY = td ? B : DB;
			auto rnd = [&]() { st.Rng ^= st.Rng << 13; st.Rng ^= st.Rng >> 17; st.Rng ^= st.Rng << 5; return (float)(st.Rng & 0xFFFFFF) / 16777216.0f; };
			auto rr = [&](float a0, float b0) { return a0 + (b0 - a0) * rnd(); };
			auto spawn = [&](bool anyHeight) {
				SnesHdDioRuntime::P3Part p;
				p.X = cX + (rnd() - 0.5f) * B; p.Y = cY + (rnd() - 0.5f) * BY;
				const float fall = rr(fallMin, fallMax);
				p.Z = anyHeight ? rnd() * BH : (fall >= 0 ? BH : 0.0f);
				const float ang = rnd() * 2.0f * kPi, dr = rr(R.DriftMin, R.DriftMax);
				p.VX = std::cos(ang) * dr; p.VY = td ? std::sin(ang) * dr : 0.0f; p.VZ = -fall;
				p.Life = R.LifeMax > 0 ? std::max(0.05f, rr(R.LifeMin, R.LifeMax)) : 0.0f;
				p.Age = (anyHeight && p.Life > 0) ? rnd() * p.Life : 0.0f;
				p.Size = 1.0f + R.SizeRandom * (rnd() * 2.0f - 1.0f);
				p.Seed1 = rnd() * 2.0f * kPi; p.Seed2 = rnd() * 2.0f * kPi;
				st.Parts.push_back(p);
			};
			auto step = [&]() {
				st.Time += dt;
				if(R.Rate > 0) { st.SpawnAcc += R.Rate * dt; while(st.SpawnAcc >= 1.0f) { st.SpawnAcc -= 1.0f; if((int)st.Parts.size() < R.MaxParticles) spawn(false); } }
				else { while((int)st.Parts.size() < R.MaxParticles) spawn(false); }
				const float tw = 2.0f * kPi * R.TurbulenceFreq;
				for(size_t k = 0; k < st.Parts.size();) {
					SnesHdDioRuntime::P3Part& p = st.Parts[k];
					bool dead = false;
					if(p.Life > 0) { p.Age += dt; dead = p.Age >= p.Life; }
					if(!dead) {
						p.VZ -= R.Gravity * dt;
						float tx = 0, ty = 0;
						if(R.Turbulence != 0.0f) { tx = R.Turbulence * std::sin(tw * st.Time + p.Seed1); ty = R.Turbulence * std::cos(tw * 0.73f * st.Time + p.Seed2); }
						p.EVX = p.VX + R.WindX + tx; p.EVY = p.VY + (td ? R.WindY + ty : 0.0f);
						p.X += p.EVX * dt;
						p.Y += p.EVY * dt;
						p.Z += p.VZ * dt;
						if(p.Z <= 0.0f) { if(R.GroundRespawn) p.Z += BH; else dead = true; }
						else if(p.Z > BH) p.Z -= BH;
						float rx = p.X - cX; rx -= B * std::floor(rx / B + 0.5f); p.X = cX + rx;
						float ry = p.Y - cY; ry -= BY * std::floor(ry / BY + 0.5f); p.Y = cY + ry;
					}
					if(dead) { p = st.Parts.back(); st.Parts.pop_back(); continue; }
					k++;
				}
			};
			if(!st.Init) {
				st.Init = true; st.Parts.clear(); st.Time = 0; st.SpawnAcc = 0;
				st.Rng ^= (uint32_t)(pi + 1) * 2654435761u;
				if(R.Rate <= 0) { while((int)st.Parts.size() < R.MaxParticles) spawn(true); }
				else { const int steps = (int)std::min(R.LifeMax > 0 ? R.LifeMax * 60.0f : 300.0f, 600.0f); for(int s = 0; s < steps; s++) step(); }
			}
			step();
			const uint32_t cnt = (uint32_t)st.Parts.size();
			const uint32_t prow = (cnt + 255) / 256;
			std::vector<float>& outP = rt.P3Out[scene.P3Count];
			outP.assign((size_t)prow * 768 * 4, 0.0f); // 3 texels RGBA32F par particule, 256 particules par ligne
			for(uint32_t k = 0; k < cnt; k++) {
				const SnesHdDioRuntime::P3Part& p = st.Parts[k];
				const float tt = p.Life > 0 ? p.Age / p.Life : 0.0f;
				float a = R.AlphaStart + (R.AlphaEnd - R.AlphaStart) * tt;
				if(p.Life > 0) {
					if(R.FadeIn > 0 && tt < R.FadeIn) a *= tt / R.FadeIn;
					if(R.FadeOut > 0 && tt > 1.0f - R.FadeOut) a *= (1.0f - tt) / R.FadeOut;
				}
				const float rb = std::max(std::fabs(p.X - cX) / (B * 0.5f), std::fabs(p.Y - cY) / (BY * 0.5f));
				if(R.FarFade > 0 && rb > 1.0f - R.FarFade) a *= std::max(0.0f, (1.0f - rb) / R.FarFade);
				a *= R.Alpha;
				float x, y, z, pk = 1.0f, sx, sy, sz;
				if(td) { x = p.X - ox - scene.W * 0.5f; y = -p.Z; z = scene.H * 0.5f - (p.Y - oy); sx = p.EVX; sy = -p.VZ; sz = -p.EVY; }
				else { z = p.Y; pk = (scene.D + z) / scene.D; x = (p.X - ox - scene.W * 0.5f) * pk; y = (scene.H * 0.5f + 16.0f - p.Z) * pk; sx = p.EVX * pk; sy = -p.VZ * pk; sz = 0.0f; }
				const float size = p.Size * (R.SizeStart + (R.SizeEnd - R.SizeStart) * tt) * pk;
				auto lerpC = [&](int sh) { const float c0 = (float)((R.ColorStart >> sh) & 0xFF), c1 = (float)((R.ColorEnd >> sh) & 0xFF); return (c0 + (c1 - c0) * tt) / 255.0f; };
				float* o = &outP[(size_t)k * 12];
				o[0] = x; o[1] = y; o[2] = z; o[3] = size; o[4] = lerpC(16); o[5] = lerpC(8); o[6] = lerpC(0); o[7] = std::clamp(a, 0.0f, 1.0f);
				o[8] = sx * R.Stretch; o[9] = sy * R.Stretch; o[10] = sz * R.Stretch; o[11] = 0.0f;
			}
			HdDioramaP3Batch& pb = scene.P3[scene.P3Count++];
			pb.Data = outP.data(); pb.Count = cnt; pb.Additive = R.Additive; pb.MinPx = R.MinPixelSize;
			if(R.Texture) {
				R.Texture->Init();
				if(R.Texture->Width > 0 && R.Texture->Height > 0 && R.Texture->PixelData.size() >= (size_t)R.Texture->Width * R.Texture->Height) {
					pb.TexPix = R.Texture->PixelData.data(); pb.TexW = R.Texture->Width; pb.TexH = R.Texture->Height;
				}
			}
			LogOnce(rt, 19, "<3DParticles> du diorama actives (" + std::to_string(cnt) + " particules pour la premiere regle)");
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

	// <DayNight> (doc 55), diorama : la couleur du cycle multiplie l'ambiante, le soleil et le ciel (copie de GbHdDiorama.cpp)
	UpdateDayNight(rt, D, f.FrameNumber);
	if(rt.Dn.Active && rt.Dn.Def >= 0) {
		const HdDayNightDef& dd = D.DayNightRules[(size_t)rt.Dn.Def].Def;
		const float* k = rt.Dn.Col;
		if(dd.TAmbient) {
			if(!scene.Lights.Active) { scene.Lights.Active = true; scene.Lights.Ambient[0] = scene.Lights.Ambient[1] = scene.Lights.Ambient[2] = 1.0f; }
			for(int c = 0; c < 3; c++) scene.Lights.Ambient[c] *= k[c];
		}
		if(dd.TSun) {
			for(int li = 0; li < 8; li++) { if(scene.Lights.L[li].Type == 3) for(int c = 0; c < 3; c++) scene.Lights.L[li].Col[c] *= k[c]; }
		}
		if(dd.TSky) {
			for(int c = 0; c < 3; c++) scene.Sky.Tint[c] = k[c];
			const uint32_t cl = scene.Clear;
			scene.Clear = (cl & 0xFF000000u) | ((uint32_t)(((cl >> 16) & 0xFF) * k[0] + 0.5f) << 16) | ((uint32_t)(((cl >> 8) & 0xFF) * k[1] + 0.5f) << 8) | (uint32_t)((cl & 0xFF) * k[2] + 0.5f);
		}
		if(dd.DrivesFog) {
			auto& F = scene.Fog;
			if(!F.Active) {
				F.Active = true;
				F.Start = td ? -20.0f : 150.0f; F.End = td ? 200.0f : 500.0f; F.Opacity = 0.0f;
				F.Ground = 0.0f; F.GroundH = 16.0f;
				const uint32_t c = td ? 0xC8D4E0u : 0xA8B8C8u;
				if(scene.Sky.Mode == 1) { F.Col[0] = scene.Sky.Bottom[0]; F.Col[1] = scene.Sky.Bottom[1]; F.Col[2] = scene.Sky.Bottom[2]; }
				else { F.Col[0] = ((c >> 16) & 0xFF) / 255.0f; F.Col[1] = ((c >> 8) & 0xFF) / 255.0f; F.Col[2] = (c & 0xFF) / 255.0f; }
			}
			F.Opacity = std::clamp(HdDayNightFog(dd, rt.Dn, 0, F.Opacity), 0.0f, 1.0f);
			F.Start = HdDayNightFog(dd, rt.Dn, 1, F.Start);
			F.End = HdDayNightFog(dd, rt.Dn, 2, F.End);
			if(F.End <= F.Start + 1.0f) F.End = F.Start + 1.0f;
			F.Ground = std::clamp(HdDayNightFog(dd, rt.Dn, 3, F.Ground), 0.0f, 1.0f);
			HdDayNightFogColor(dd, rt.Dn, F.Col);
		}
		if(dd.TAmbient) {
			const HdDayNightStep& sa = dd.Steps[(size_t)rt.Dn.A]; const HdDayNightStep& sb = dd.Steps[(size_t)rt.Dn.B];
			const bool fogColDriven = sa.HasFogColor || sb.HasFogColor;
			for(int c = 0; c < 3; c++) {
				if(scene.Fog.Active && !fogColDriven) scene.Fog.Col[c] *= k[c];
				scene.Mist.Col[c] *= k[c];
				scene.Cloud[1].Col[c] *= k[c];
				scene.P3Tint[c] = k[c];
			}
		}
		LogOnce(rt, 14, "<DayNight> actif dans le diorama");
	}

	// G4a : <DioramaExtend> (doc 40) : extension des couches au-dela des bords (copie de GbHdDiorama.cpp) ; bandes repeat alignees sur le repere du monde
	if(!D.DioramaExtendRules.empty()) {
		const int sideBit[4] = { 1, 2, 4, 8 };
		uint32_t extActive = 0;
		for(const HdDioramaExtendRule& r : D.DioramaExtendRules) {
			if(!CondsOk(r.Conditions) || r.Layer < 0 || r.Layer >= 4 + 8) continue;
			HdDioramaExtend& e = scene.Ext[r.Layer];
			for(int sd = 0; sd < 4; sd++) {
				if(!(r.Side & sideBit[sd])) continue;
				e.Active = true;
				e.Margin[sd] = (float)r.Size; e.Mode[sd] = r.Mode; e.Band[sd] = (float)r.Band * 8.0f;
				if(r.RangeMin >= 0) { e.Range[sd][0] = (float)r.RangeMin; e.Range[sd][1] = (float)r.RangeMax; } else { e.Range[sd][0] = -100000.0f; e.Range[sd][1] = 100000.0f; }
				e.Fog[sd] = r.Fog;
				e.FogDist[sd] = r.FogDist > 0 ? (float)r.FogDist : (float)r.Size;
				e.Color[sd][0] = r.HasColor ? (float)((r.Color >> 16) & 0xFF) / 255.0f : 0.0f;
				e.Color[sd][1] = r.HasColor ? (float)((r.Color >> 8) & 0xFF) / 255.0f : 0.0f;
				e.Color[sd][2] = r.HasColor ? (float)(r.Color & 0xFF) / 255.0f : 0.0f;
				e.Color[sd][3] = r.HasColor ? 1.0f : 0.0f;
				e.Edge[sd] = r.Edge >= 0 ? (float)r.Edge : (sd == 1 ? scene.H : (sd == 3 ? scene.W : 0.0f));
			}
			extActive++;
		}
		if(extActive) {
			scene.ExtScrollX = wx; scene.ExtScrollY = wy;
			LogOnce(rt, 15, "<DioramaExtend> : " + std::to_string(extActive) + " regle(s) active(s) sur " + std::to_string(D.DioramaExtendRules.size()));
		}
	}

	// H1 : objets 3D de decor (doc 45, copie de GbHdDiorama.cpp) : <DioramaObject> dont la condition est vraie ; repeat= et scrollx= / scrolly= ($var)
	scene.Objects.Count = 0;
	if(!D.DioramaObjectRules.empty() || !rt.T2oInst.empty()) {
		// <DioramaObject>, puis instances <Tile2Obj> / <Sprite2Obj> de cette image (etape 3b-2)
		std::vector<const HdDioramaObjectRule*> orules;
		for(const HdDioramaObjectRule& q : D.DioramaObjectRules) orules.push_back(&q);
		const size_t nBaseObj = orules.size();
		for(const HdDioramaObjectRule& q : rt.T2oInst) orules.push_back(&q);
		if(rt.ObjSpin.size() < nBaseObj) rt.ObjSpin.resize(nBaseObj, 0.0f);
		// valeurs surveillees de cette image (meme ordre que Data.WatchedValues), INT32_MIN = adresse pas encore lue
		std::vector<int32_t> wvals;
		for(const HdWatchVar& w : pack.WatchedVars) {
			uint32_t raw = 0;
			bool okv = f.Screen != nullptr;
			for(uint32_t bi = 0; okv && bi < w.Size; bi++) {
				auto it = f.Screen->WatchedAddressValues.find((w.Address + bi) | w.Space);
				if(it == f.Screen->WatchedAddressValues.end()) { okv = false; break; }
				raw |= (uint32_t)it->second << (8 * bi);
			}
			wvals.push_back(okv ? HdWatchCompose(raw, w.Size, w.Signed) : INT32_MIN);
		}
		int on = 0;
		for(size_t i = 0; i < orules.size() && on < HdDioMaxObjects; i++) {
			const HdDioramaObjectRule& r = *orules[i];
			if(!r.Mesh || r.Mesh->size() < 24) continue;
			if(!CondsOk(r.Conditions)) continue;
			float scx = 0.0f, scy = 0.0f;
			if(!r.ScrollX.Empty() && !r.ScrollX.Eval(wvals, scx)) continue; // valeur RAM pas encore lue : objet masque
			if(!r.ScrollY.Empty() && !r.ScrollY.Eval(wvals, scy)) continue;
			if(i < nBaseObj) rt.ObjSpin[i] = std::fmod(rt.ObjSpin[i] + r.Spin, 360.0f);
			const float spinDeg = i < nBaseObj ? rt.ObjSpin[i] : 0.0f;
			const float visMargin = 256.0f; // marge hors ecran (pixels SNES) ou un objet est encore emis
			int kr[3][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
			const float org[3] = { r.X, r.Y, r.Z };
			for(int ax = 0; ax < 3; ax++) {
				if(r.RepStep[ax] <= 0.0f) continue;
				float lo = r.RepHasFrom[ax] ? r.RepFrom[ax] : org[ax];
				float hi = r.RepHasTo[ax] ? r.RepTo[ax] : 1.0e9f;
				if(ax == 0 && !r.ScrollX.Empty()) {
					lo = std::max(lo, scx - r.Radius - visMargin);
					hi = std::min(hi, scx + (float)scene.W + r.Radius + visMargin);
				}
				const float k0 = std::ceil((lo - org[ax]) / r.RepStep[ax]), k1 = std::floor((hi - org[ax]) / r.RepStep[ax]);
				kr[ax][0] = (int)std::max(-100000.0f, k0); kr[ax][1] = (int)std::min(100000.0f, k1);
				if(kr[ax][1] - kr[ax][0] > 255) kr[ax][1] = kr[ax][0] + 255;
			}
			for(int kz = kr[2][0]; kz <= kr[2][1]; kz++) for(int ky = kr[1][0]; ky <= kr[1][1]; ky++) for(int kx = kr[0][0]; kx <= kr[0][1]; kx++) {
				if(on >= HdDioMaxObjects) break;
				const float rx = r.X + (float)kx * r.RepStep[0], ry = r.Y + (float)ky * r.RepStep[1], rz = r.Z + (float)kz * r.RepStep[2];
				if(!r.ScrollX.Empty() && (rx - scx + r.Radius < -visMargin || rx - scx - r.Radius > (float)scene.W + visMargin)) continue;
				HdDioramaObject& o = scene.Objects.O[on++];
				o.Verts = r.Mesh->data();
				o.Version = r.MeshVersion;
				o.VertCount = (uint32_t)(r.Mesh->size() / 8);
				o.TexPix = nullptr; o.TexW = 0; o.TexH = 0;
				if(r.Tex) {
					r.Tex->Init();
					if(r.Tex->Width > 0 && r.Tex->Height > 0 && r.Tex->PixelData.size() >= (size_t)r.Tex->Width * r.Tex->Height) {
						o.TexPix = r.Tex->PixelData.data(); o.TexW = r.Tex->Width; o.TexH = r.Tex->Height;
					}
				}
				o.Color[0] = ((r.Color >> 16) & 0xFF) / 255.0f; o.Color[1] = ((r.Color >> 8) & 0xFF) / 255.0f; o.Color[2] = (r.Color & 0xFF) / 255.0f;
				o.Pos[0] = rx - scx; o.Pos[1] = ry - scy; o.Pos[2] = rz; o.RefZ = r.RefZ; o.Occlude = r.Occlude;
				if(r.FromTile2Obj && td) { // Tile2Obj / Sprite2Obj en topDown : pose sur le sol, dans le repere des couches a plat (copie du GB)
					o.Pos[0] = r.X; o.Pos[1] = scene.H * 0.5f - r.Elevation; o.Pos[2] = scene.H * 0.5f - r.BottomY; o.RefZ = -o.Pos[2];
				}
				o.Scale = r.Scale;
				o.Lit = r.Lit;
				o.Bright = r.Bright;
				o.Shadow = r.Shadow;
				o.CastShadow = r.CastShadow && td;
				o.WinOn = false;
				if(r.HasWindow && !r.ScrollX.Empty()) {
					o.WinOn = true;
					o.WinX0 = (scx - r.Window - rx) / r.Scale;
					o.WinX1 = (scx + (float)scene.W + r.Window - rx) / r.Scale;
				}
				const float kd = 0.017453292f;
				const float ay = (r.RotY + spinDeg) * kd, axr = r.RotX * kd, az = r.RotZ * kd;
				const float cy = std::cos(ay), sy = std::sin(ay), cx = std::cos(axr), sx = std::sin(axr), cz = std::cos(az), sz = std::sin(az);
				const float Ry[9] = { cy, 0, sy, 0, 1, 0, -sy, 0, cy };
				const float Rx[9] = { 1, 0, 0, 0, cx, -sx, 0, sx, cx };
				const float Rz[9] = { cz, -sz, 0, sz, cz, 0, 0, 0, 1 };
				auto mul = [](const float* a, const float* b, float* c) { for(int ii = 0; ii < 3; ii++) for(int jj = 0; jj < 3; jj++) c[ii * 3 + jj] = a[ii * 3] * b[jj] + a[ii * 3 + 1] * b[3 + jj] + a[ii * 3 + 2] * b[6 + jj]; };
				float t1[9];
				mul(Ry, Rx, t1);
				mul(t1, Rz, o.Rot);
				if(r.MirrorX || r.MirrorY) {
					const float M[9] = { r.MirrorX ? -1.0f : 1.0f, 0, 0, 0, r.MirrorY ? -1.0f : 1.0f, 0, 0, 0, 1 };
					float t2[9]; mul(M, o.Rot, t2);
					for(int q = 0; q < 9; q++) o.Rot[q] = t2[q];
				}
			}
		}
		scene.Objects.Count = on;
		if(on != rt.ObjLast) {
			rt.ObjLast = on;
			MessageManager::Log("[HD-SNES-DIORAMA] objets 3D : " + std::to_string(on) + " objet(s) actif(s) sur " + std::to_string(nBaseObj) + " regle(s) <DioramaObject> + " + std::to_string(rt.T2oInst.size()) + " instance(s) Tile2Obj / Sprite2Obj, frame " + std::to_string(f.FrameNumber));
		}
	}

	const auto tGpu = std::chrono::steady_clock::now();
	// async=on : on recupere d'abord le resultat de l'image precedente (le GPU a eu une image entiere pour le dessiner), puis on envoie
	// cette image ; sinon (premiere image, image precedente sans diorama, taille changee) : envoi et relecture tout de suite.
	bool ok;
	const bool chain = dio.Async && rt.AsyncPending && rt.AsyncFrame + 1 == rt.Calls && rt.AsyncPixels == pixels;
	if(chain && gpu.FetchOpaque(f.Out, pixels)) {
		ok = gpu.SubmitScene(rt.L[0].data(), rt.L[1].data(), rt.L[2].data(), rt.L[3].data(), W, H, scene);
		rt.AsyncPending = ok;
		LogOnce(rt, 16, "async=on : relecture GPU decalee d'une image");
	} else {
		ok = gpu.SubmitScene(rt.L[0].data(), rt.L[1].data(), rt.L[2].data(), rt.L[3].data(), W, H, scene);
		if(ok && dio.Async) {
			// premiere image de la chaine : il faut une image pour cette fois (resultat de cette scene), la suivante sera decalee
			ok = gpu.FetchOpaque(f.Out, pixels);
			if(ok) ok = gpu.SubmitScene(rt.L[0].data(), rt.L[1].data(), rt.L[2].data(), rt.L[3].data(), W, H, scene);
			rt.AsyncPending = ok;
		} else {
			ok = ok && gpu.FetchOpaque(f.Out, pixels);
			rt.AsyncPending = false;
		}
	}
	rt.AsyncFrame = rt.Calls; rt.AsyncPixels = pixels;
	if(!ok) {
		rt.HashValid = false; rt.AsyncPending = false;
		if(rt.Fail++ < 3) MessageManager::Log("[HD-SNES-DIORAMA] envoi ou relecture GPU en echec, rendu 2D utilise");
		return false;
	}
	{
		const auto tEnd = std::chrono::steady_clock::now();
		auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
		rt.MsBuild += ms(tBuild, tBuilt); rt.MsScene += ms(tBuilt, tGpu); rt.MsGpu += ms(tGpu, tEnd);
		if(++rt.MsFrames >= 300) {
			char tb[220];
			snprintf(tb, sizeof(tb), "[HD-SNES-DIORAMA] ms/image : couches %.2f, scene (ombres, pieds, objets...) %.2f, GPU envoi + relecture %.2f%s",
				rt.MsBuild / rt.MsFrames, rt.MsScene / rt.MsFrames, rt.MsGpu / rt.MsFrames, dio.Async ? " (async=on)" : "");
			MessageManager::Log(tb);
			// detail du GPU commun : envoi des textures, dessin, attente du rendu (Map), copie retournee (Flip)
			const HdGpuTimes gt = gpu.TakeLayerTimes();
			if(gt.Count > 0) {
				char gb[220];
				snprintf(gb, sizeof(gb), "[HD-SNES-DIORAMA] GPU ms/image : envoi des textures %.2f, dessin %.2f, attente du rendu %.2f, copie %.2f (%u envois)",
					gt.UploadMs / rt.MsFrames, gt.DrawMs / rt.MsFrames, gt.MapMs / rt.MsFrames, gt.FlipMs / rt.MsFrames, gt.Count);
				MessageManager::Log(gb);
			}
			rt.MsBuild = rt.MsScene = rt.MsGpu = 0.0; rt.MsFrames = 0;
		}
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

// <DayNight> en rendu 2D normal (etape 3a) : multiplication de l'image ; zindex= (echelle SNES : Z des tiles, sprites et <background>) :
// seulement ce qui est a ce Z ou en dessous. Rien si la scene du diorama a ete dessinee (la teinte y est deja appliquee).
void SnesHdDioPost(SnesHdDioRuntime& rt, SnesHdPackData& pack, const SnesHdDioFrame& f, bool sceneDrawn)
{
	if(sceneDrawn || !pack.Dio || !f.Out || !f.Z) return;
	HdPackData& D = pack.Dio->Data;
	if(D.DayNightRules.empty()) return;
	UpdateDayNight(rt, D, f.FrameNumber);
	if(!rt.Dn.Active || rt.Dn.Def < 0) return;
	if(rt.Dn.Col[0] >= 0.999f && rt.Dn.Col[1] >= 0.999f && rt.Dn.Col[2] >= 0.999f) return;
	const int zMax = D.DayNightRules[(size_t)rt.Dn.Def].Def.ZIndex;
	const uint32_t kr = (uint32_t)(rt.Dn.Col[0] * 256.0f + 0.5f), kg = (uint32_t)(rt.Dn.Col[1] * 256.0f + 0.5f), kb = (uint32_t)(rt.Dn.Col[2] * 256.0f + 0.5f);
	const uint32_t W = f.OutW, H = f.OutH;
	const int nBands = (f.Pool && f.Pool->Size() > 1 && H >= 64) ? f.Pool->Size() : 1;
	const uint32_t bandH = (H + nBands - 1) / nBands;
	auto band = [&](int b) {
		const uint32_t y0 = (uint32_t)b * bandH, y1 = std::min(H, y0 + bandH);
		for(uint32_t y = y0; y < y1; y++) {
			for(uint32_t x = 0; x < W; x++) {
				const size_t i = (size_t)y * W + x;
				if(zMax >= 0 && (int)f.Z[i] > zMax) continue;
				const uint32_t d = f.Out[i];
				f.Out[i] = (d & 0xFF000000u) | ((((d >> 16) & 0xFF) * kr >> 8) << 16) | ((((d >> 8) & 0xFF) * kg >> 8) << 8) | ((d & 0xFF) * kb >> 8);
			}
		}
	};
	if(nBands > 1) f.Pool->Run(nBands, [&](int b) { band(b); });
	else band(0);
	if(!rt.Logged[13]) { rt.Logged[13] = true; MessageManager::Log("[HD-SNES-DIORAMA] <DayNight> actif en rendu 2D (zindex " + std::to_string(zMax) + ")"); }
}
