#pragma once
// <DayNight> : cycle jour / nuit partage (NES ; SNES et Game Boy a venir). Voir claude/55_DayNight.md (docs du projet).
// Un cycle = liste d'etapes (couleur, maintien, degrade vers l'etape suivante), en images, en boucle.
// Une etape peut attendre une condition (until=) au lieu de son maintien. Les cles de brouillard d'une etape sont interpolees.
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "Utilities/StringUtilities.h"

struct HdDayNightStep
{
	std::string Name;
	uint32_t Color = 0xFFFFFF;
	int Hold = 600, Fade = 120;      // images
	std::string Until;               // condition(s) "A&B" : maintien jusqu'a ce qu'elles soient vraies (remplace hold)
	// Brouillard (diorama) : 0 opacite, 1 debut, 2 fin, 3 brume au sol ; couleur a part
	float Fog[4] = { 0, 0, 0, 0 }; bool HasFog[4] = { false, false, false, false };
	uint32_t FogColor = 0; bool HasFogColor = false;
};

struct HdDayNightDef
{
	std::vector<HdDayNightStep> Steps;
	bool TimeAlways = true;          // time=always : le temps avance toujours ; time=cond : seulement quand la condition de la ligne est vraie
	int Start = 0;                   // image du cycle au demarrage
	int ZIndex = -1;                 // mode normal : teinte ce qui est a ce z ou en dessous (-1 = tout)
	bool TAmbient = true, TSun = true, TSky = true; // diorama : ce que la couleur teinte
	bool DrivesFog = false;          // une etape au moins a une cle de brouillard
};

// Etat d'execution d'un cycle
struct HdDayNightState
{
	bool Init = false;
	int Step = 0, Phase = 0;         // Phase 0 = maintien, 1 = degrade vers l'etape suivante
	float Elapsed = 0.0f;            // images ecoulees dans la phase
	uint32_t LastFrame = 0;
};

// Sortie : etapes A et B, poids du degrade W (0 = A, 1 = B)
struct HdDayNightOut
{
	bool Active = false;
	int Def = -1, A = 0, B = 0;
	float W = 0.0f;
	float Col[3] = { 1, 1, 1 };      // couleur de la teinte (0..1)
};

inline bool HdDayNightParseColor(std::string v, uint32_t& out)
{
	v = StringUtilities::Trim(v);
	if(!v.empty() && v[0] == '#') v = v.substr(1);
	if(v.size() != 6) return false;
	try { out = (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; return true; } catch(...) { return false; }
}

// Lecture depuis le JSON a plat (cles en minuscules, objets imbriques "nom.cle"). Retourne un message d'erreur (vide si tout va bien).
inline std::string HdDayNightParse(const std::unordered_map<std::string, std::string>& cfg, HdDayNightDef& d)
{
	auto str = [&](const std::string& k) -> std::string { auto it = cfg.find(k); return it == cfg.end() ? std::string() : StringUtilities::Trim(it->second); };
	auto num = [&](const std::string& k, float def, bool* has = nullptr) -> float {
		const std::string v = str(k); if(has) *has = false; if(v.empty()) return def;
		try { const float f = std::stof(v); if(has) *has = true; return f; } catch(...) { return def; }
	};
	std::string err;
	const std::string steps = str("steps");
	if(steps.empty()) return "cle \"steps\" manquante (ex. \"nuit|aube|jour|crepuscule\")";
	for(std::string nm : StringUtilities::Split(steps, '|')) {
		nm = StringUtilities::ToLower(StringUtilities::Trim(nm));
		if(nm.empty()) continue;
		HdDayNightStep s; s.Name = nm;
		const std::string c = str(nm + ".color");
		if(c.empty() || !HdDayNightParseColor(c, s.Color)) err += "etape " + nm + " : couleur absente ou invalide (RRGGBB) ; ";
		s.Hold = std::max(0, (int)num(nm + ".hold", 600.0f));
		s.Fade = std::max(0, (int)num(nm + ".fade", 120.0f));
		s.Until = str(nm + ".until");
		static const char* fk[4] = { ".fogopacity", ".fogstart", ".fogend", ".fogground" };
		for(int i = 0; i < 4; i++) { s.Fog[i] = num(nm + fk[i], 0.0f, &s.HasFog[i]); if(s.HasFog[i]) d.DrivesFog = true; }
		const std::string fc = str(nm + ".fogcolor");
		if(!fc.empty()) { if(HdDayNightParseColor(fc, s.FogColor)) { s.HasFogColor = true; d.DrivesFog = true; } else err += "etape " + nm + " : fogColor invalide ; "; }
		d.Steps.push_back(s);
	}
	if(d.Steps.empty()) return "aucune etape";
	const std::string t = StringUtilities::ToLower(str("time"));
	if(!t.empty()) { if(t == "always") d.TimeAlways = true; else if(t == "cond") d.TimeAlways = false; else err += "time doit etre always ou cond ; "; }
	d.Start = std::max(0, (int)num("start", 0.0f));
	d.ZIndex = (int)num("zindex", -1.0f);
	const std::string tg = StringUtilities::ToLower(str("targets"));
	if(!tg.empty()) {
		d.TAmbient = tg.find("ambient") != std::string::npos;
		d.TSun = tg.find("sun") != std::string::npos;
		d.TSky = tg.find("sky") != std::string::npos;
	}
	return err;
}

// Avance d'une image (ou plus) ; untilTrue(i) : conditions until= de l'etape i vraies
inline void HdDayNightAdvance(const HdDayNightDef& d, HdDayNightState& st, uint32_t frame, bool advance, const std::function<bool(int)>& untilTrue)
{
	const int n = (int)d.Steps.size();
	if(n == 0) return;
	if(!st.Init) {
		st.Init = true; st.Step = 0; st.Phase = 0; st.Elapsed = 0.0f; st.LastFrame = frame;
		// start= : position de depart dans le cycle (les etapes until= comptent pour leur maintien)
		int total = 0;
		for(const HdDayNightStep& s : d.Steps) total += s.Hold + s.Fade;
		int rest = total > 0 ? d.Start % total : 0;
		for(int i = 0; i < n && rest > 0; i++) {
			const HdDayNightStep& s = d.Steps[i];
			if(rest < s.Hold) { st.Step = i; st.Phase = 0; st.Elapsed = (float)rest; rest = 0; break; }
			rest -= s.Hold;
			if(rest < s.Fade) { st.Step = i; st.Phase = 1; st.Elapsed = (float)rest; rest = 0; break; }
			rest -= s.Fade;
			st.Step = (i + 1) % n; st.Phase = 0; st.Elapsed = 0.0f;
		}
		return;
	}
	float df = (float)std::min<uint32_t>(frame - st.LastFrame, 8u);
	st.LastFrame = frame;
	if(!advance) return;
	st.Elapsed += df;
	for(int guard = 0; guard < 2 * n + 2; guard++) {
		const HdDayNightStep& s = d.Steps[st.Step];
		if(st.Phase == 0) {
			if(!s.Until.empty()) {
				if(!untilTrue(st.Step)) { st.Elapsed = 0.0f; break; }
				st.Phase = 1; st.Elapsed = 0.0f; // le degrade commence a l'image ou la condition devient vraie
				continue;
			}
			if(st.Elapsed < (float)s.Hold) break;
			st.Elapsed -= (float)s.Hold; st.Phase = 1;
		} else {
			if(st.Elapsed < (float)s.Fade) break;
			st.Elapsed -= (float)s.Fade; st.Phase = 0; st.Step = (st.Step + 1) % n;
		}
	}
}

// Melange de deux couleurs en lumiere lineaire (evite les teintes ternes a mi-chemin)
inline void HdDayNightMix(uint32_t a, uint32_t b, float w, float out[3])
{
	for(int c = 0; c < 3; c++) {
		const int sh = 16 - 8 * c;
		const float la = std::pow(((a >> sh) & 0xFF) / 255.0f, 2.2f), lb = std::pow(((b >> sh) & 0xFF) / 255.0f, 2.2f);
		out[c] = std::pow(la + (lb - la) * w, 1.0f / 2.2f);
	}
}

inline void HdDayNightOutput(const HdDayNightDef& d, const HdDayNightState& st, int defIndex, HdDayNightOut& o)
{
	const int n = (int)d.Steps.size();
	if(n == 0) { o.Active = false; return; }
	o.Active = true; o.Def = defIndex;
	o.A = st.Step; o.B = (st.Step + 1) % n;
	const HdDayNightStep& s = d.Steps[st.Step];
	o.W = (st.Phase == 1 && s.Fade > 0) ? std::clamp(st.Elapsed / (float)s.Fade, 0.0f, 1.0f) : 0.0f;
	HdDayNightMix(d.Steps[o.A].Color, d.Steps[o.B].Color, o.W, o.Col);
}

// Valeur de brouillard interpolee (cle absente d'une etape : valeur de base)
inline float HdDayNightFog(const HdDayNightDef& d, const HdDayNightOut& o, int k, float base)
{
	const HdDayNightStep& a = d.Steps[o.A]; const HdDayNightStep& b = d.Steps[o.B];
	const float va = a.HasFog[k] ? a.Fog[k] : base, vb = b.HasFog[k] ? b.Fog[k] : base;
	return va + (vb - va) * o.W;
}
inline void HdDayNightFogColor(const HdDayNightDef& d, const HdDayNightOut& o, float col[3])
{
	const HdDayNightStep& a = d.Steps[o.A]; const HdDayNightStep& b = d.Steps[o.B];
	if(!a.HasFogColor && !b.HasFogColor) return;
	auto pack = [](const float* c) { return ((uint32_t)std::clamp(c[0] * 255.0f + 0.5f, 0.0f, 255.0f) << 16) | ((uint32_t)std::clamp(c[1] * 255.0f + 0.5f, 0.0f, 255.0f) << 8) | (uint32_t)std::clamp(c[2] * 255.0f + 0.5f, 0.0f, 255.0f); };
	const uint32_t base = pack(col);
	HdDayNightMix(a.HasFogColor ? a.FogColor : base, b.HasFogColor ? b.FogColor : base, o.W, col);
}
