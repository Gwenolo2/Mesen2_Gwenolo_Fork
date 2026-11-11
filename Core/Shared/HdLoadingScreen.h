#pragma once
// Ecran de chargement des packs HD (balise <LoadingScreen>), commun NES et SNES.
// Header-only : etat partage entre le thread de chargement (LoadAsync) et le thread de rendu (VideoRenderer).
//
// Syntaxe : <LoadingScreen>fichier.png,centered,display_file_names,scrollBarX,scrollBarY,scrollBarColor
//   fichier.png        : PNG du pack (dossier ou zip). "black"/"blank"/absent/introuvable = ecran noir.
//                        "default" = image par defaut (pas encore integree : ecran noir).
//   centered           : true/1 (defaut) centre l'image ; false/0 la place en haut a gauche.
//   display_file_names : true/1 (defaut) affiche le nom du fichier en cours en bas de l'ecran.
//   scrollBarX/Y       : position de la barre en % (coin haut-gauche). Largeur fixe 40 %.
//   scrollBarColor     : couleur (black, white, red, green, blue, yellow ou #RRGGBB).

#include "pch.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include "Shared/MessageManager.h"
#include "Shared/Video/DebugHud.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/StringUtilities.h"

struct HdLoadingScreenConfig
{
	bool Enabled = false;
	std::string ImageName;
	std::vector<uint8_t> ImageData;   // octets du PNG (vide = ecran noir)
	bool Centered = true;
	bool ShowFileNames = true;
	double BarX = 10.0;               // % de la largeur
	double BarY = 95.0;               // % de la hauteur
	uint32_t BarColor = 0xFFFFFFFF;   // ARGB
};

namespace HdLoadingScreen
{
	namespace Detail
	{
		inline std::mutex& Mutex() { static std::mutex m; return m; }
		inline bool& Active() { static bool a = false; return a; }
		inline HdLoadingScreenConfig& Config() { static HdLoadingScreenConfig c; return c; }
		inline uint32_t& Total() { static uint32_t t = 0; return t; }
		inline uint32_t& Done() { static uint32_t d = 0; return d; }
		inline std::string& CurrentName() { static std::string n; return n; }
		inline std::vector<uint32_t>& Image() { static std::vector<uint32_t> i; return i; }
		inline uint32_t& ImageW() { static uint32_t w = 0; return w; }
		inline uint32_t& ImageH() { static uint32_t h = 0; return h; }
		inline std::vector<uint32_t>& TextBuf() { static std::vector<uint32_t> t; return t; }
		inline std::unique_ptr<DebugHud>& Hud() { static std::unique_ptr<DebugHud> h; return h; }

		inline bool ParseBool(const std::string& v, bool def)
		{
			std::string s = StringUtilities::ToLower(StringUtilities::Trim(v));
			if(s.empty()) return def;
			if(s == "true" || s == "1" || s == "yes" || s == "oui" || s == "y" || s == "o") return true;
			if(s == "false" || s == "0" || s == "no" || s == "non" || s == "n") return false;
			return def;
		}

		inline double ParseDouble(const std::string& v, double def)
		{
			std::string s = StringUtilities::Trim(v);
			if(s.empty()) return def;
			try {
				return std::stod(s);
			} catch(...) {
				return def;
			}
		}

		inline bool ParseColor(const std::string& v, uint32_t& out)
		{
			std::string s = StringUtilities::ToLower(StringUtilities::Trim(v));
			struct Named { const char* Name; uint32_t Color; };
			static const Named named[] = {
				{ "black", 0x000000 }, { "noir", 0x000000 },
				{ "white", 0xFFFFFF }, { "blanc", 0xFFFFFF },
				{ "red", 0xFF0000 }, { "rouge", 0xFF0000 },
				{ "green", 0x00FF00 }, { "vert", 0x00FF00 },
				{ "blue", 0x0000FF }, { "bleu", 0x0000FF },
				{ "yellow", 0xFFFF00 }, { "jaune", 0xFFFF00 }
			};
			for(const Named& n : named) {
				if(s == n.Name) {
					out = 0xFF000000 | n.Color;
					return true;
				}
			}
			if(!s.empty() && s[0] == '#') s = s.substr(1);
			if(s.size() != 6) return false;
			for(char c : s) {
				if(!std::isxdigit((unsigned char)c)) return false;
			}
			out = 0xFF000000 | (uint32_t)std::stoul(s, nullptr, 16);
			return true;
		}
	}

	// Parse les champs du tag (tokens[0] = fichier PNG). loadFile lit un fichier du pack (dossier ou zip).
	inline HdLoadingScreenConfig ParseTag(const std::vector<std::string>& tokens, const std::function<bool(const std::string&, std::vector<uint8_t>&)>& loadFile)
	{
		HdLoadingScreenConfig cfg;
		cfg.Enabled = true;
		if(!tokens.empty()) {
			cfg.ImageName = StringUtilities::Trim(tokens[0]);
			std::string lower = StringUtilities::ToLower(cfg.ImageName);
			if(lower.empty() || lower == "black" || lower == "blank") {
				// ecran noir
			} else if(lower == "default") {
				MessageManager::Log("[HD LoadingScreen] 'default' : image par defaut non encore integree, ecran noir.");
			} else if(!loadFile(cfg.ImageName, cfg.ImageData)) {
				MessageManager::Log("[HD LoadingScreen] Image introuvable : " + cfg.ImageName + " (ecran noir).");
				cfg.ImageData.clear();
			}
		}
		if(tokens.size() > 1) cfg.Centered = Detail::ParseBool(tokens[1], true);
		if(tokens.size() > 2) cfg.ShowFileNames = Detail::ParseBool(tokens[2], true);
		if(tokens.size() > 3) cfg.BarX = std::clamp(Detail::ParseDouble(tokens[3], 10.0), 0.0, 100.0);
		if(tokens.size() > 4) cfg.BarY = std::clamp(Detail::ParseDouble(tokens[4], 95.0), 0.0, 100.0);
		if(tokens.size() > 5) {
			uint32_t color = 0;
			if(Detail::ParseColor(tokens[5], color)) cfg.BarColor = color;
		}
		return cfg;
	}

	// Garde RAII : active l'ecran pendant la duree de LoadAsync (couvre tous les return).
	class Guard
	{
		bool _active;
	public:
		Guard(const HdLoadingScreenConfig& cfg, uint32_t totalFiles) : _active(cfg.Enabled)
		{
			if(!_active) return;
			std::lock_guard<std::mutex> lock(Detail::Mutex());
			Detail::Config() = cfg;
			Detail::Total() = std::max<uint32_t>(1, totalFiles);
			Detail::Done() = 0;
			Detail::CurrentName().clear();
			Detail::Image().clear();
			Detail::ImageW() = Detail::ImageH() = 0;
			if(!Detail::Config().ImageData.empty()) {
				std::vector<uint32_t> pixels;
				uint32_t w = 0, h = 0;
				if(PNGHelper::ReadPNG(Detail::Config().ImageData, pixels, w, h)) {
					Detail::Image() = std::move(pixels);
					Detail::ImageW() = w;
					Detail::ImageH() = h;
				}
			}
			Detail::Active() = true;
		}

		~Guard()
		{
			if(!_active) return;
			std::lock_guard<std::mutex> lock(Detail::Mutex());
			Detail::Active() = false;
			Detail::Image().clear();
			Detail::Config().ImageData.clear();
		}
	};

	// Appele apres chaque fichier charge.
	inline void Progress(const std::string& fileName)
	{
		std::lock_guard<std::mutex> lock(Detail::Mutex());
		if(!Detail::Active()) return;
		Detail::Done()++;
		Detail::CurrentName() = fileName;
	}

	inline bool IsActive()
	{
		std::lock_guard<std::mutex> lock(Detail::Mutex());
		return Detail::Active();
	}

	// Dessine l'ecran dans buffer (ARGB, width x height). Appele par le thread de rendu.
	inline void Compose(uint32_t* buffer, uint32_t width, uint32_t height)
	{
		std::lock_guard<std::mutex> lock(Detail::Mutex());
		if(!Detail::Active() || buffer == nullptr || width == 0 || height == 0) return;

		const HdLoadingScreenConfig& cfg = Detail::Config();
		const size_t count = (size_t)width * height;
		std::fill(buffer, buffer + count, 0xFF000000u);

		// 1. Image : mise a l'echelle si plus grande que l'ecran, centree ou en haut a gauche
		const uint32_t iw = Detail::ImageW();
		const uint32_t ih = Detail::ImageH();
		if(!Detail::Image().empty() && iw > 0 && ih > 0) {
			double scale = std::min(1.0, std::min((double)width / iw, (double)height / ih));
			uint32_t dw = std::max<uint32_t>(1, (uint32_t)(iw * scale + 0.5));
			uint32_t dh = std::max<uint32_t>(1, (uint32_t)(ih * scale + 0.5));
			int32_t ox = cfg.Centered ? ((int32_t)width - (int32_t)dw) / 2 : 0;
			int32_t oy = cfg.Centered ? ((int32_t)height - (int32_t)dh) / 2 : 0;
			const std::vector<uint32_t>& img = Detail::Image();
			for(uint32_t y = 0; y < dh; y++) {
				int32_t dy = oy + (int32_t)y;
				if(dy < 0 || dy >= (int32_t)height) continue;
				uint32_t sy = std::min(ih - 1, (uint32_t)((y + 0.5) * ih / dh));
				for(uint32_t x = 0; x < dw; x++) {
					int32_t dx = ox + (int32_t)x;
					if(dx < 0 || dx >= (int32_t)width) continue;
					uint32_t sx = std::min(iw - 1, (uint32_t)((x + 0.5) * iw / dw));
					uint32_t px = img[(size_t)sy * iw + sx];
					uint32_t a = px >> 24;
					uint32_t r = ((px >> 16) & 0xFF) * a / 255;
					uint32_t g = ((px >> 8) & 0xFF) * a / 255;
					uint32_t b = (px & 0xFF) * a / 255;
					buffer[(size_t)dy * width + dx] = 0xFF000000u | (r << 16) | (g << 8) | b;
				}
			}
		}

		// 2. Barre de progression : largeur fixe 40 %, coin haut-gauche en %
		const uint32_t barW = (uint32_t)(width * 0.40);
		const uint32_t barH = std::max<uint32_t>(6, height / 50);
		const int32_t bx = (int32_t)(width * cfg.BarX / 100.0);
		const int32_t by = (int32_t)(height * cfg.BarY / 100.0);
		double frac = std::clamp((double)Detail::Done() / Detail::Total(), 0.0, 1.0);
		uint32_t filled = (uint32_t)(barW * frac);
		for(uint32_t y = 0; y < barH; y++) {
			int32_t dy = by + (int32_t)y;
			if(dy < 0 || dy >= (int32_t)height) continue;
			for(uint32_t x = 0; x < barW; x++) {
				int32_t dx = bx + (int32_t)x;
				if(dx < 0 || dx >= (int32_t)width) continue;
				buffer[(size_t)dy * width + dx] = (x < filled) ? (0xFF000000u | (cfg.BarColor & 0xFFFFFF)) : 0xFF303030u;
			}
		}

		// 3. Nom du fichier en cours, en bas de l'ecran (texte via DebugHud, copie sur le calque)
		if(cfg.ShowFileNames && !Detail::CurrentName().empty()) {
			if(!Detail::Hud()) {
				Detail::Hud().reset(new DebugHud());
			}
			std::vector<uint32_t>& text = Detail::TextBuf();
			if(text.size() != count) {
				text.assign(count, 0);
			}
			Detail::Hud()->ClearScreen();
			// DrawStringCommand inverse le canal alpha : 0x00 = opaque. Blanc opaque = 0x00FFFFFF, fond noir opaque = 0x00000000
			Detail::Hud()->DrawString(8, (int)height - 24, Detail::CurrentName(), (int)0x00FFFFFF, (int)0x00000000, 1, -1);
			FrameInfo info = {};
			info.Width = width;
			info.Height = height;
			Detail::Hud()->Draw(text.data(), info, OverscanDimensions{}, 0, HudScaleFactors{}, true);
			for(size_t i = 0; i < count; i++) {
				if(text[i] >> 24) {
					buffer[i] = text[i];
				}
			}
		}
	}
}
