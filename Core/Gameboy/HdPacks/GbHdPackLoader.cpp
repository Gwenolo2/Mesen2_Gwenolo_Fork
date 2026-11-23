#include "pch.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "Gameboy/HdPacks/GbHdPackLoader.h"
#include "Gameboy/HdPacks/GbHdDiorama.h"
#include "NES/HdPacks/HdPackLoader.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/MessageManager.h"
#include "Utilities/HdFlatJson.h"

namespace {

	std::string Trim(const std::string& s)
	{
		size_t a = s.find_first_not_of(" \t");
		if(a == std::string::npos) {
			return "";
		}
		size_t b = s.find_last_not_of(" \t");
		return s.substr(a, b - a + 1);
	}

	std::vector<std::string> SplitTrim(const std::string& s, char sep)
	{
		std::vector<std::string> out;
		std::stringstream ss(s);
		std::string field;
		while(std::getline(ss, field, sep)) {
			out.push_back(Trim(field));
		}
		return out;
	}

	// Expose les <watchMemoryAddress> aux regles diorama (scrollx= / scrolly= de <DioramaObject>) : meme ordre que WatchedVars
	void SyncDioWatch(GbHdPackData& d)
	{
		if(!d.Dio) return;
		d.Dio->Data.WatchedValues.clear();
		for(const GbHdWatchVar& v : d.WatchedVars) {
			HdWatchedValue w;
			w.Name = v.Name; w.Size = v.Size; w.Signed = v.Signed;
			w.Address = v.Slot[0] < d.WatchedAddresses.size() ? d.WatchedAddresses[v.Slot[0]] : 0;
			d.Dio->Data.WatchedValues.push_back(w);
		}
	}

	// Index de l'image dans la liste (ajoutee si absente) : un chemin PNG suffit, sans <img>
	uint32_t FindOrAddImage(std::vector<std::string>& images, const std::string& path)
	{
		for(size_t i = 0; i < images.size(); i++) {
			if(images[i] == path) {
				return (uint32_t)i;
			}
		}
		images.push_back(path);
		return (uint32_t)(images.size() - 1);
	}

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}
}

static double ToDouble(const std::string& s, double def)
{
	if(s.empty()) {
		return def;
	}
	return strtod(s.c_str(), nullptr);
}

static int ToInt(const std::string& s, int def)
{
	if(s.empty()) {
		return def;
	}
	return (int)strtol(s.c_str(), nullptr, 10);
}

static bool ToBool(const std::string& s, bool def)
{
	if(s.empty()) {
		return def;
	}
	std::string v = Lower(s);
	return v == "y" || v == "yes" || v == "true" || v == "1" || v == "on";
}

static GbHdBgOperator ParseOperator(const std::string& s)
{
	std::string v = Lower(s);
	if(v == "/" || v == "divide") {
		return GbHdBgOperator::Divide;
	}
	if(v == "+" || v == "plus") {
		return GbHdBgOperator::Plus;
	}
	if(v == "-" || v == "minus") {
		return GbHdBgOperator::Minus;
	}
	return GbHdBgOperator::Multiply;
}

// Liste des PNG d'un <background> : un fichier, ou les PNG d'un dossier (ordre alphabetique)
static std::vector<std::string> ListBackgroundFiles(const std::string& packFolder, std::string rel)
{
	while(!rel.empty() && (rel.back() == '/' || rel.back() == '\\')) {
		rel.pop_back();
	}
	std::vector<std::string> out;
	std::string low = Lower(rel);
	if(low.size() >= 4 && low.compare(low.size() - 4, 4, ".png") == 0) {
		out.push_back(rel);
		return out;
	}
	std::vector<std::string> names;
	for(const std::string& full : FolderUtilities::GetFilesInFolder(FolderUtilities::CombinePath(packFolder, rel), {".png", ".PNG"}, false)) {
		names.push_back(FolderUtilities::GetFilename(full, true));
	}
	std::sort(names.begin(), names.end());
	for(const std::string& n : names) {
		out.push_back(rel + "/" + n);
	}
	return out;
}

namespace {
	// Lecture de gb_hires.txt avec <include> (comme NES / SNES) : le fichier inclus remplace la ligne, a son endroit.
	// Chemin relatif a la racine du pack. Imbrication max 20. Inclusion circulaire ignoree. Balise sans casse.
	void ReadPackLines(const std::string& packFolder, const std::string& relPath, std::vector<std::string>& out, int depth, std::vector<std::string>& stack, int& errors)
	{
		std::string key = Lower(relPath);
		if(depth > 20) {
			MessageManager::Log("[HD-GB-LOAD] <include> : trop d'imbrications, ignore : " + relPath);
			errors++;
			return;
		}
		if(std::find(stack.begin(), stack.end(), key) != stack.end()) {
			MessageManager::Log("[HD-GB-LOAD] <include> circulaire, ignore : " + relPath);
			errors++;
			return;
		}
		std::ifstream f(FolderUtilities::CombinePath(packFolder, relPath));
		if(!f.good()) {
			MessageManager::Log("[HD-GB-LOAD] <include> : fichier introuvable : " + relPath);
			errors++;
			return;
		}
		stack.push_back(key);
		std::string l;
		bool first = true;
		while(std::getline(f, l)) {
			if(first && l.size() >= 3 && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) {
				l = l.substr(3); // BOM UTF-8 en tete de fichier
			}
			first = false;
			std::string t = l;
			size_t hashPos = t.find('#');
			if(hashPos != std::string::npos) {
				t = t.substr(0, hashPos);
			}
			t = Trim(t);
			if(t.size() >= 9 && Lower(t.substr(0, 9)) == "<include>") {
				std::string inc = Trim(t.substr(9));
				for(char& ch : inc) {
					if(ch == '\\') ch = '/';
				}
				ReadPackLines(packFolder, inc, out, depth + 1, stack, errors);
			} else {
				out.push_back(l);
			}
		}
		stack.pop_back();
	}

	// <2DOverlay>fichier.json,Cle=valeur,... : meme format que NES / SNES (docs/claude/09_2DOverlay.md).
	// Les parametres de la balise remplacent ceux du JSON. Unites Game Boy : pixels SD (160 x 144).
	bool ParseOverlay(const std::string& packFolder, const std::string& params, const std::vector<GbHdTerm>& terms, GbHdPackData& outData)
	{
		std::vector<std::string> tokens = SplitTrim(params, ',');
		if(tokens.empty() || tokens[0].empty()) {
			MessageManager::Log("[HD-GB-LOAD] <2DOverlay> : fichier JSON manquant");
			return false;
		}
		std::string jsonName = tokens[0];
		for(char& ch : jsonName) {
			if(ch == '\\') ch = '/';
		}
		std::ifstream jf(FolderUtilities::CombinePath(packFolder, jsonName), std::ios::binary);
		if(!jf.good()) {
			MessageManager::Log("[HD-GB-LOAD] <2DOverlay> : fichier introuvable : " + jsonName);
			return false;
		}
		std::stringstream buf;
		buf << jf.rdbuf();
		std::unordered_map<std::string, std::string> cfg = ParseFlatJson(buf.str());
		for(size_t t = 1; t < tokens.size(); t++) {
			size_t eq = tokens[t].find('=');
			if(eq == std::string::npos) {
				continue;
			}
			cfg[Lower(Trim(tokens[t].substr(0, eq)))] = Trim(tokens[t].substr(eq + 1));
		}

		auto str = [&](const char* k) -> std::string {
			auto it = cfg.find(k);
			return it == cfg.end() ? std::string() : it->second;
		};
		auto num = [&](const char* k, float def) -> float {
			std::string v = str(k);
			if(v.empty()) return def;
			try { return std::stof(v); } catch(...) { return def; }
		};
		auto boolean = [&](const char* k, bool def) -> bool {
			std::string v = Lower(str(k));
			if(v.empty()) return def;
			return v == "true" || v == "1" || v == "yes" || v == "y";
		};
		auto color = [&](const char* k, uint32_t def) -> uint32_t {
			std::string v = Trim(str(k));
			if(v.empty()) return def;
			if(v[0] == '#') v = v.substr(1);
			try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; }
		};
		auto namedColor = [&](const char* k, uint32_t def) -> uint32_t {
			std::string v = Lower(Trim(str(k)));
			if(v.empty()) return def;
			if(v == "black" || v == "noir") return 0x000000;
			if(v == "white" || v == "blanc") return 0xFFFFFF;
			if(v == "yellow" || v == "jaune") return 0xFFFF00;
			if(v == "green" || v == "vert") return 0x00FF00;
			if(v == "blue" || v == "bleu" || v == "bleue") return 0x0000FF;
			if(v == "red" || v == "rouge") return 0xFF0000;
			return color(k, def);
		};

		std::string type = Lower(str("type"));
		if(!type.empty() && type != "particles" && type != "plainscreen" && type != "ellipse" && type != "shadows" && type != "clouds" && type != "fog" && type != "rays" && type != "distortion" && type != "aurora" && type != "torchlight") {
			MessageManager::Log("[HD-GB-LOAD] <2DOverlay> " + jsonName + " : type '" + type + "' non gere (particles, plainscreen, ellipse, shadows, clouds, fog, rays, distortion, aurora, torchlight)");
			return false;
		}

		GbHdOverlay ov;
		ov.JsonFile = jsonName;
		ov.Terms = terms;
		ov.ZIndex = std::clamp((int)num("z", num("zindex", 99.0f)), 0, 255);
		ov.Layer = std::clamp((int)num("layer", 0.0f), 0, 1);
		ov.SpeedX = num("speedx", 1.0f);
		ov.SpeedY = num("speedy", 1.0f);
		ov.Alpha = std::clamp(num("alpha", 1.0f), 0.0f, 1.0f);
		ov.FieldW = std::max(0.25f, num("fieldw", 1.0f));
		ov.FieldH = std::max(0.25f, num("fieldh", 1.0f));

		std::string blend = Lower(str("blend"));
		ov.Additive = (blend == "additive" || blend == "add");
		ov.MaxParticles = std::clamp((int)num("maxparticles", 300.0f), 1, 20000);
		ov.Rate = std::max(0.0f, num("rate", 60.0f));
		ov.LifeMin = std::max(0.05f, num("lifemin", 2.0f));
		ov.LifeMax = std::max(ov.LifeMin, num("lifemax", 4.0f));
		ov.SpawnX = num("spawnx", 0.0f);
		ov.SpawnY = num("spawny", 0.0f);
		ov.SpawnW = num("spawnw", 1.0f);
		ov.SpawnH = num("spawnh", 1.0f);
		ov.SpeedMin = num("speedmin", 20.0f);
		ov.SpeedMax = num("speedmax", 40.0f);
		ov.AngleMin = num("anglemin", 80.0f);
		ov.AngleMax = num("anglemax", 100.0f);
		ov.GravityX = num("gravityx", 0.0f);
		ov.GravityY = num("gravityy", 0.0f);
		ov.WindX = num("windx", 0.0f);
		ov.WindY = num("windy", 0.0f);
		ov.Turbulence = num("turbulence", 0.0f);
		ov.TurbulenceFreq = num("turbulencefreq", 0.5f);
		ov.RotationMin = num("rotationmin", 0.0f);
		ov.RotationMax = num("rotationmax", 0.0f);
		ov.SpinMin = num("spinmin", 0.0f);
		ov.SpinMax = num("spinmax", 0.0f);
		ov.SizeStart = num("sizestart", 1.0f);
		ov.SizeEnd = num("sizeend", ov.SizeStart);
		ov.SizeRandom = std::clamp(num("sizerandom", 0.0f), 0.0f, 0.95f);
		ov.AlphaStart = num("alphastart", 1.0f);
		ov.AlphaEnd = num("alphaend", ov.AlphaStart);
		ov.FadeIn = std::clamp(num("fadein", 0.1f), 0.0f, 1.0f);
		ov.FadeOut = std::clamp(num("fadeout", 0.2f), 0.0f, 1.0f);
		ov.ColorStart = color("colorstart", 0xFFFFFF);
		ov.ColorEnd = color("colorend", ov.ColorStart);
		ov.DotSize = std::max(0.25f, num("dotsize", 2.0f));
		ov.Stretch = std::clamp(num("stretch", 0.0f), 0.0f, 2.0f);
		ov.Prewarm = boolean("prewarm", true);

		ov.IsPlainScreen = (type == "plainscreen");
		ov.IsEllipse = (type == "ellipse");
		ov.IsNoise = (type == "shadows" || type == "clouds" || type == "fog");
		ov.NoiseKind = (type == "clouds") ? 1 : ((type == "fog") ? 2 : 0);
		ov.IsRays = (type == "rays");
		ov.IsAurora = (type == "aurora");
		ov.IsTorch = (type == "torchlight");
		ov.IsDistortion = (type == "distortion");

		const bool isLayer = ov.IsPlainScreen || ov.IsEllipse || ov.IsTorch || ov.IsNoise || ov.IsRays || ov.IsAurora;
		if(isLayer || ov.IsDistortion) {
			ov.ZoneX0 = std::clamp(num("xmin", 0.0f), 0.0f, 1.0f);
			ov.ZoneX1 = std::clamp(num("xmax", 1.0f), 0.0f, 1.0f);
			ov.ZoneY0 = std::clamp(num("ymin", 0.0f), 0.0f, 1.0f);
			ov.ZoneY1 = std::clamp(num("ymax", 1.0f), 0.0f, 1.0f);
		}
		if(ov.IsDistortion) {
			ov.DistAmp1 = num("amp1", 0.0f); ov.DistWave1 = std::max(1.0f, num("wave1", 32.0f));
			ov.DistSpeed1 = num("speed1", 0.5f); ov.DistPhase1 = num("phase1", 0.0f);
			ov.DistAmp2 = num("amp2", 0.0f); ov.DistWave2 = std::max(1.0f, num("wave2", 17.0f));
			ov.DistSpeed2 = num("speed2", 0.3f); ov.DistPhase2 = num("phase2", 0.0f);
		}
		if(isLayer) {
			ov.FxNegative = boolean("negative", false);
			const uint32_t defCol = ov.IsTorch ? 0xFFE0A0u : ov.IsEllipse ? (ov.FxNegative ? 0x000000u : 0xFFFFFFu) : (type == "fog" ? 0xA8B8C8u : (type == "clouds" ? 0xFFFFFFu : 0x000000u));
			ov.FxColor = namedColor("color", defCol);
			if(ov.IsTorch) {
				ov.TorchX = num("x", 80.0f);
				ov.TorchY = num("y", 72.0f);
				ov.TorchRadius = std::max(1.0f, num("radius", 48.0f));
				ov.TorchSoft = std::clamp(num("softness", 0.5f), 0.01f, 1.0f);
				ov.TorchDark = boolean("dark", true);
				ov.TorchGlow = std::clamp(num("glow", 0.15f), 0.0f, 1.0f);
			}
			if(ov.IsEllipse) {
				ov.FxEllipseX = num("x", 80.0f);
				ov.FxEllipseY = num("y", 72.0f);
				ov.FxRadiusX = std::max(0.5f, num("radiusx", 32.0f));
				ov.FxRadiusY = std::max(0.5f, num("radiusy", 16.0f));
			}
			if(ov.IsRays) {
				ov.RayX = std::clamp(num("rayx", 0.5f), 0.0f, 1.0f);
				ov.RayY = std::clamp(num("rayy", 0.0f), 0.0f, 1.0f);
				ov.RayCount = std::clamp((int)num("raycount", 8.0f), 1, 64);
				ov.RayAngle = num("rayangle", 90.0f);
				ov.RaySpread = std::clamp(num("rayspread", 60.0f), 0.0f, 360.0f);
				ov.RayWidth = std::max(0.1f, num("raywidth", 4.0f));
				ov.RayLength = std::max(0.01f, num("raylength", 0.8f));
				ov.RaySpeed = num("rayspeed", 0.2f);
				ov.RayWobble = num("raywobble", 3.0f);
				ov.RayColor = namedColor("raycolor", 0xFFE8A0);
			}
			if(ov.IsAurora) {
				ov.AuroraSize = std::max(4.0f, num("size", 40.0f));
				ov.AuroraSpeed = num("speed", 6.0f);
				ov.AuroraCoverage = std::clamp(num("coverage", 0.5f), 0.0f, 1.0f);
				ov.AuroraFall = std::clamp(num("fall", 0.7f), 0.05f, 1.0f);
				ov.AuroraWave = std::max(0.0f, num("wave", 6.0f));
				ov.AuroraColor1 = namedColor("color1", 0x3CFF8C);
				ov.AuroraColor2 = namedColor("color2", 0x8C4BFF);
			}
			if(ov.IsNoise || ov.IsAurora) {
				ov.FxSeed = (int)num("seed", 1.0f);
			}
			if(ov.IsNoise) {
				ov.FxSize = std::max(4.0f, num("size", 48.0f));
				ov.FxSpeed = num("speed", 8.0f);
				ov.FxDirection = num("direction", 0.0f);
				ov.FxCoverage = std::clamp(num("coverage", 0.5f), 0.0f, 1.0f);
			}
		}

		std::string tex = Trim(str("texture"));
		if(!tex.empty()) {
			for(char& ch : tex) {
				if(ch == '\\') ch = '/';
			}
			std::ifstream tf(FolderUtilities::CombinePath(packFolder, tex), std::ios::binary);
			if(!tf.good()) {
				MessageManager::Log("[HD-GB-LOAD] <2DOverlay> " + jsonName + " : texture introuvable : " + tex + " (point doux utilise)");
			} else {
				ov.TextureIndex = (int)FindOrAddImage(outData.Images, tex);
			}
		}

		std::string kind = type.empty() ? "particles" : type;
		MessageManager::Log("[HD-GB-LOAD] <2DOverlay> " + jsonName + " : type " + kind + ", Z " + std::to_string(ov.ZIndex) + ", couche " + std::to_string(ov.Layer) + (terms.empty() ? "" : ", sous condition"));
		outData.Overlays.push_back(std::move(ov));
		return true;
	}
}

bool GbHdPackLoader::Load(const std::string& packFolder, GbHdPackData& outData)
{
	outData = GbHdPackData();
	outData.FolderPath = packFolder;

	{
		std::ifstream probe(FolderUtilities::CombinePath(packFolder, "gb_hires.txt"));
		if(!probe.good()) {
			return false;
		}
	}
	std::vector<std::string> lines;
	std::vector<std::string> includeStack;
	int includeErrors = 0;
	// Index des balises <img>, dans leur ordre : le numero d'image d'une <tile> designe la n-ieme <img>,
	// pas la n-ieme image de la liste (un <background> ou un chemin PNG direct ajoutent aussi des images)
	std::vector<uint32_t> imgTags;
	ReadPackLines(packFolder, "gb_hires.txt", lines, 0, includeStack, includeErrors);

	// Blocs <DioramaDepth>{ } / <DioramaShadow>{ } : pile (type, numero) ; 'x' = bloc non gere (apparie seulement)
	std::vector<std::pair<char, int>> blockStack;
	int ignoredLines = 0;
	int duplicates = 0;
	int conditionalTiles = 0;
	int unsupportedConditions = 0;
	int unknownConditionLines = 0;
	std::string line;
	for(const std::string& rawLine : lines) {
		line = rawLine;
		if(!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		// Commentaire : # jusqu'en fin de ligne
		size_t hashPos = line.find('#');
		if(hashPos != std::string::npos) {
			line = line.substr(0, hashPos);
		}
		line = Trim(line);
		if(line.empty()) {
			continue;
		}

		{
			// [fork GB] blocs de profondeur / d'ombre du diorama (comme NES) : les <tile> du bloc sont marquees
			std::string bl = line; // sans la condition [ ] eventuelle (ignoree sur un bloc, comme NES)
			if(bl[0] == '[') { const size_t cb = bl.find(']'); if(cb != std::string::npos) bl = Trim(bl.substr(cb + 1)); }
			const std::string lo = Lower(bl);
			if(line == "}") {
				if(!blockStack.empty()) blockStack.pop_back();
				continue;
			}
			if(!bl.empty() && bl.back() == '{' && bl[0] == '<' && lo.find('>') != std::string::npos && lo.compare(0, 5, "<tile") != 0) {
				int id = 0;
				char kind = 'x';
				const bool isDepth = lo.compare(0, 14, "<dioramadepth>") == 0, isShadow = lo.compare(0, 15, "<dioramashadow>") == 0;
				if(isDepth || isShadow) {
					if(!outData.Dio) { outData.Dio = std::make_shared<GbHdDiorama>(); SyncDioWatch(outData); }
					const std::string opts = bl.substr(isDepth ? 14 : 15, bl.size() - (isDepth ? 15 : 16));
					if(isDepth) { kind = 'd'; id = HdPackLoader::ParseForeignDepthBlock(packFolder, opts, outData.Dio->Data); }
					else { kind = 's'; id = HdPackLoader::ParseForeignShadowBlock(packFolder, opts, outData.Dio->Data); }
					if(id == 0) { MessageManager::Log("[HD-GB-LOAD] bloc diorama refuse : " + bl); ignoredLines++; }
				} else {
					MessageManager::Log("[HD-GB-LOAD] bloc non pris en charge sur Game Boy (ses lignes restent lues) : " + bl);
					ignoredLines++;
				}
				blockStack.push_back(std::make_pair(kind, id));
				continue;
			}
		}
		if(line.compare(0, 7, "<scale>") == 0) {
			long s = strtol(line.c_str() + 7, nullptr, 10);
			if(s >= 1 && s <= 10) {
				outData.Scale = (uint32_t)s;
			} else {
				ignoredLines++;
			}
		} else if(Lower(line) == "<disableoriginaltiles>" || (Lower(line).compare(0, 9, "<options>") == 0 && Lower(line).find("disableoriginaltiles") != std::string::npos)) {
			// Comme SNES (<disableOriginalTiles>) et NES (<options>disableOriginalTiles)
			outData.DisableOriginalTiles = true;
			MessageManager::Log("[HD-GB-LOAD] <disableOriginalTiles> : tiles de fond SD masquees");
		} else if(Lower(line.substr(0, 5)) == "<bgm>") {
			// <bgm>album,piste,fichier.ogg[,positionDeBoucle]
			std::vector<std::string> f = SplitTrim(line.substr(5), ',');
			int album = f.size() >= 1 ? (int)strtol(f[0].c_str(), nullptr, 10) : -1;
			int track = f.size() >= 2 ? (int)strtol(f[1].c_str(), nullptr, 10) : -1;
			if(f.size() < 3 || album < 0 || album > 255 || track < 0 || track > 255 || f[2].empty()) {
				MessageManager::Log("[HD-GB-LOAD] <bgm> invalide (album,piste,fichier.ogg[,boucle]) : " + line);
				ignoredLines++;
			} else {
				std::string rel = f[2];
				for(char& ch : rel) {
					if(ch == '\\') ch = '/';
				}
				std::string full = FolderUtilities::CombinePath(packFolder, rel);
				std::ifstream probeOgg(full, std::ios::binary);
				if(!probeOgg.good()) {
					MessageManager::Log("[HD-GB-LOAD] <bgm> : fichier OGG introuvable : " + rel);
					ignoredLines++;
				} else {
					GbHdBgmTrack t;
					t.Filename = full;
					if(f.size() >= 4) {
						t.LoopPosition = (uint32_t)strtoul(f[3].c_str(), nullptr, 10);
					}
					outData.BgmFiles[album * 256 + track] = t;
				}
			}
		} else if(line.compare(0, 5, "<img>") == 0) {
			imgTags.push_back(FindOrAddImage(outData.Images, Trim(line.substr(5))));
		} else if(line.compare(0, 9, "<spriteZ>") == 0) {
			outData.SpriteZ = (int)strtol(line.c_str() + 9, nullptr, 10);
		} else if(line.size() >= 20 && Lower(line.substr(0, 20)) == "<watchmemoryaddress>") {
			// <watchMemoryAddress>nom,adresse[,taille][,signed] : valeur RAM nommee, lue a chaque image
			std::vector<HdWatchVar> existing;
			for(const GbHdWatchVar& v : outData.WatchedVars) { HdWatchVar e; e.Name = v.Name; existing.push_back(e); }
			HdWatchVar wv;
			std::string werr;
			if(!HdWatchParseDecl(SplitTrim(line.substr(20), ','), false, 0xFFFF, existing, wv, werr)) {
				MessageManager::Log("[HD-GB-LOAD] <watchMemoryAddress> : " + werr + " : " + line);
				ignoredLines++;
			} else {
				GbHdWatchVar gv;
				gv.Name = wv.Name;
				gv.Size = wv.Size;
				gv.Signed = wv.Signed;
				for(uint32_t i = 0; i < wv.Size; i++) {
					uint16_t a = (uint16_t)(wv.Address + i);
					auto found = std::find(outData.WatchedAddresses.begin(), outData.WatchedAddresses.end(), a);
					gv.Slot[i] = (uint32_t)(found - outData.WatchedAddresses.begin());
					if(found == outData.WatchedAddresses.end()) {
						outData.WatchedAddresses.push_back(a);
					}
				}
				outData.WatchedVars.push_back(gv);
				SyncDioWatch(outData);
			}
		} else if(line.compare(0, 11, "<condition>") == 0) {
			// nom,memoryCheckConstant,adresse,operateur,valeur[,masque]
			std::vector<std::string> f = SplitTrim(line.substr(11), ',');
			if(f.size() >= 4 && !f[0].empty() && Lower(f[1]) == "framerange") {
				// nom,frameRange,diviseur,valeur : (image % diviseur) >= valeur, comme NES / SNES
				GbHdCondition c;
				c.Type = GbHdCondition::Kind::FrameRange;
				uint32_t divisor = (uint32_t)strtoul(f[2].c_str(), nullptr, 10);
				c.Divisor = (divisor == 0) ? 1 : divisor; // division par zero interdite (comme SNES)
				c.Threshold = (uint32_t)strtoul(f[3].c_str(), nullptr, 10);
				outData.Conditions[f[0]] = c;
				continue;
			}
			if(f.size() >= 2 && !f[0].empty() && (Lower(f[1]) == "tileatposition" || Lower(f[1]) == "spriteatposition" || Lower(f[1]) == "tilenearby" || Lower(f[1]) == "spritenearby")) {
				// tileAtPosition : nom,tileAtPosition,colonne,ligne,hash,palette[,ignore]  (colonne / ligne en tuiles 8 px)
				// spriteAtPosition : nom,spriteAtPosition,x,y,hash,palette[,ignore]        (x / y en unites de 8 px)
				// Joker * / all / any sur x ou y. Un index de tile (6 caracteres ou moins) n'est pas pris en charge sur la Game Boy.
				if(f.size() < 6 || f[4].size() <= 6) {
					unsupportedConditions++;
					continue;
				}
				GbHdCondition c;
				if(Lower(f[1]) == "tileatposition") c.Type = GbHdCondition::Kind::TileAtPosition;
				else if(Lower(f[1]) == "spriteatposition") c.Type = GbHdCondition::Kind::SpriteAtPosition;
				else if(Lower(f[1]) == "tilenearby") c.Type = GbHdCondition::Kind::TileNearby;
				else c.Type = GbHdCondition::Kind::SpriteNearby;
				std::string sx = Lower(f[2]);
				std::string sy = Lower(f[3]);
				c.AnyX = (sx == "*" || sx == "all" || sx == "any");
				c.AnyY = (sy == "*" || sy == "all" || sy == "any");
				c.X = c.AnyX ? 0 : (int32_t)strtol(f[2].c_str(), nullptr, 10);
				c.Y = c.AnyY ? 0 : (int32_t)strtol(f[3].c_str(), nullptr, 10);
				c.Hash = strtoull(f[4].c_str(), nullptr, 16);
				// Joker de palette : ignore, *, all, any (comme les jokers de position)
				std::string sp = Lower(f[5]);
				c.IgnorePalette = (sp == "ignore" || sp == "*" || sp == "all" || sp == "any") || (f.size() >= 7 && Lower(f[6]) == "ignore");
				c.Palette = c.IgnorePalette ? 0 : (uint32_t)strtoul(f[5].c_str(), nullptr, 10);
				outData.Conditions[f[0]] = c;
				continue;
			}
			if(f.size() < 5 || f[0].empty() || f[1] != "memoryCheckConstant") {
				// Autres types (memoryCheck, ...) : pas encore pris en charge
				unsupportedConditions++;
				continue;
			}
			GbHdCondition c;
			std::string op = Lower(f[3]);
			if(op == "==") c.Operator = GbHdCondition::Op::Eq;
			else if(op == "!=") c.Operator = GbHdCondition::Op::Ne;
			else if(op == ">") c.Operator = GbHdCondition::Op::Gt;
			else if(op == "<") c.Operator = GbHdCondition::Op::Lt;
			else if(op == ">=") c.Operator = GbHdCondition::Op::Ge;
			else if(op == "<=") c.Operator = GbHdCondition::Op::Le;
			else if(op == "in") c.Operator = GbHdCondition::Op::In;
			else {
				ignoredLines++;
				continue;
			}
			uint16_t address = (uint16_t)strtoul(f[2].c_str(), nullptr, 16);
			if(c.Operator == GbHdCondition::Op::In) {
				for(const std::string& v : SplitTrim(f[4], '|')) {
					c.Values.push_back((uint8_t)strtoul(v.c_str(), nullptr, 16));
				}
			} else {
				c.Values.push_back((uint8_t)strtoul(f[4].c_str(), nullptr, 16));
			}
			if(f.size() >= 6) {
				c.Mask = (uint8_t)strtoul(f[5].c_str(), nullptr, 16);
			}
			auto found = std::find(outData.WatchedAddresses.begin(), outData.WatchedAddresses.end(), address);
			c.Slot = (uint32_t)(found - outData.WatchedAddresses.begin());
			if(found == outData.WatchedAddresses.end()) {
				outData.WatchedAddresses.push_back(address);
			}
			outData.Conditions[f[0]] = c;
		} else {
			// Ligne conditionnelle optionnelle : [terme&terme]<tile...>
			std::vector<GbHdTerm> terms;
			if(line[0] == '[') {
				size_t close = line.find(']');
				if(close == std::string::npos) {
					ignoredLines++;
					continue;
				}
				bool known = true;
				for(const std::string& t : SplitTrim(line.substr(1, close - 1), '&')) {
					GbHdTerm term;
					term.Negated = !t.empty() && t[0] == '!';
					term.Name = Trim(term.Negated ? t.substr(1) : t);
					if(term.Name != "hmirror" && term.Name != "vmirror" && !outData.Conditions.count(term.Name)) {
						known = false;
					}
					terms.push_back(term);
				}
				line = Trim(line.substr(close + 1));
				if(terms.empty() || !known) {
					// Condition non declaree avant usage, ou prefixe vide
					unknownConditionLines++;
					continue;
				}
			}

			{
				// Diorama et <DayNight> (doc 56) : lus par le chargeur NES (memes balises, memes options), conditions Game Boy adaptees
				std::string low = Lower(line);
				if(low.compare(0, 15, "<dioramawindow>") == 0) {
					if(!outData.Dio) { outData.Dio = std::make_shared<GbHdDiorama>(); SyncDioWatch(outData); }
					const std::string v = Lower(Trim(line.substr(15)));
					outData.Dio->WindowFixed = !(v == "scene" || v == "off" || v == "3d");
					MessageManager::Log(std::string("[HD-GB-LOAD] <DioramaWindow> : fenetre ") + (outData.Dio->WindowFixed ? "a plat devant la scene" : "dans la scene (couche des tiles)"));
					continue;
				}
				if(low.compare(0, 8, "<diorama") == 0 || low.compare(0, 10, "<daynight>") == 0 || low.compare(0, 13, "<3dparticles>") == 0) {
					if(!outData.Dio) { outData.Dio = std::make_shared<GbHdDiorama>(); SyncDioWatch(outData); }
					GbHdDiorama& dio = *outData.Dio;
					for(const auto& kv : outData.Conditions) { dio.Named(kv.first, false); dio.Named(kv.first, true); }
					std::vector<HdPackCondition*> conds;
					for(const GbHdTerm& t : terms) {
						if(t.Name == "hmirror" || t.Name == "vmirror") continue; // sans tile : ignores
						conds.push_back(dio.Named(t.Name, t.Negated));
					}
					if(low.compare(0, 15, "<dioramashadow>") == 0) {
						// forme ligne : tile=HASH (hash Game Boy, hexadecimal) ou tile=*, palette=N ou *, puis les options communes de la forme bloc
						std::string tileStr, palStr = "*", rest;
						for(const std::string& tk : SplitTrim(line.substr(15), ',')) {
							const std::string lk = Lower(tk);
							if(lk.compare(0, 5, "tile=") == 0) tileStr = Trim(tk.substr(5));
							else if(lk.compare(0, 8, "palette=") == 0) palStr = Lower(Trim(tk.substr(8)));
							else rest += (rest.empty() ? "" : ",") + tk;
						}
						const int rid = tileStr.empty() ? 0 : HdPackLoader::ParseForeignShadowBlock(packFolder, rest, dio.Data);
						if(rid == 0) {
							MessageManager::Log("[HD-GB-LOAD] <DioramaShadow> : tile= obligatoire (ou bloc { }) / options invalides : " + line);
							ignoredLines++;
						} else {
							GbHdShadowKey k;
							k.Rule = rid;
							k.AnyTile = tileStr == "*";
							k.Hash = k.AnyTile ? 0 : strtoull(tileStr.c_str(), nullptr, 16);
							k.AnyPalette = (palStr == "*" || palStr == "all" || palStr == "any");
							k.Palette = k.AnyPalette ? 0 : (uint32_t)strtoul(palStr.c_str(), nullptr, 10);
							for(const GbHdTerm& t : terms) { if(t.Name != "hmirror" && t.Name != "vmirror") k.Terms.push_back(t); }
							outData.ShadowKeys.push_back(k);
						}
						continue;
					}
					if(!HdPackLoader::ParseForeignDioramaLine(packFolder, line, conds, dio.ByName, dio.Data)) {
						MessageManager::Log("[HD-GB-LOAD] balise diorama non prise en charge sur Game Boy : " + line);
						ignoredLines++;
					}
					continue;
				}
			}
			{
				std::string low = Lower(line);
				size_t tagLen = 0;
				if(low.compare(0, 22, "<play_conditional_bgm>") == 0) tagLen = 22;
				else if(low.compare(0, 20, "<playconditionalbgm>") == 0) tagLen = 20;
				if(tagLen > 0) {
					// [termes]<play_conditional_bgm>album,piste[,boucle] (boucle = 0 : une seule fois)
					std::vector<std::string> f = SplitTrim(line.substr(tagLen), ',');
					int album = f.size() >= 1 ? (int)strtol(f[0].c_str(), nullptr, 10) : -1;
					int track = f.size() >= 2 ? (int)strtol(f[1].c_str(), nullptr, 10) : -1;
					if(album < 0 || album > 255 || track < 0 || track > 255) {
						MessageManager::Log("[HD-GB-LOAD] <play_conditional_bgm> invalide (album,piste[,boucle]) : " + line);
						ignoredLines++;
					} else {
						GbHdConditionalBgm rule;
						rule.TrackId = album * 256 + track;
						rule.Loop = !(f.size() >= 3 && f[2] == "0");
						rule.Terms = terms;
						outData.ConditionalBgms.push_back(rule);
						if(!outData.BgmRequest) {
							outData.BgmRequest = std::make_shared<std::atomic<int32_t>>(-1);
						}
					}
					continue;
				}
			}

			if(line.size() >= 11 && Lower(line.substr(0, 11)) == "<muteaudio>") {
				// <muteAudio>* ou liste de canaux : Square1 / S1, Square2 / S2, Wave / W, Noise / N
				GbHdAudioMute rule;
				rule.Terms = terms;
				bool bad = false;
				for(const std::string& tok : SplitTrim(line.substr(11), ',')) {
					std::string c = Lower(tok);
					if(c.empty()) continue;
					if(c == "*" || c == "all") rule.Mask |= 0x0F;
					else if(c == "square1" || c == "s1" || c == "sq1") rule.Mask |= 0x01;
					else if(c == "square2" || c == "s2" || c == "sq2") rule.Mask |= 0x02;
					else if(c == "wave" || c == "w") rule.Mask |= 0x04;
					else if(c == "noise" || c == "noice" || c == "n") rule.Mask |= 0x08;
					else {
						MessageManager::Log("[HD-GB-LOAD] <muteAudio> : canal inconnu '" + tok + "' (Square1, Square2, Wave, Noise, S1, S2, *)");
						bad = true;
					}
				}
				if(rule.Mask == 0) {
					if(!bad) MessageManager::Log("[HD-GB-LOAD] <muteAudio> : aucun canal");
					ignoredLines++;
				} else {
					outData.AudioMutes.push_back(rule);
					if(!outData.AudioMuteMask) {
						outData.AudioMuteMask = std::make_shared<std::atomic<uint8_t>>(0);
					}
				}
				continue;
			}

			if(line.size() >= 15 && Lower(line.substr(0, 15)) == "<2dspritelight>") {
				// <2DSpriteLight>HASH[,palette|*][,cle=valeur...] (comme SNES)
				GbHdSpriteLight L;
				L.Terms = terms;
				int pos = 0;
				bool ok = false;
				for(const std::string& tok : SplitTrim(line.substr(15), ',')) {
					if(tok.empty()) continue;
					size_t eq = tok.find('=');
					if(eq != std::string::npos) {
						std::string k = Lower(Trim(tok.substr(0, eq)));
						std::string v = Trim(tok.substr(eq + 1));
						std::string vc = (!v.empty() && v[0] == '#') ? v.substr(1) : v;
						try {
							if(k == "radius" || k == "rayon") L.Radius = std::max(1.0f, std::stof(v));
							else if(k == "color" || k == "couleur") L.Color = (uint32_t)std::stoul(vc, nullptr, 16) & 0xFFFFFF;
							else if(k == "intensity" || k == "intensite") L.Intensity = std::max(0.0f, std::stof(v));
							else if(k == "flicker" || k == "scintillement") L.Flicker = std::clamp(std::stof(v), 0.0f, 1.0f);
							else if(k == "speed" || k == "flickerspeed" || k == "vitesse") L.FlickerSpeed = std::stof(v);
							else if(k == "mode") { std::string m = Lower(v); L.Shadow = (m == "shadow" || m == "sub" || m == "ombre"); }
							else if(k == "dx" || k == "offsetx") L.OffsetX = std::stof(v);
							else if(k == "dy" || k == "offsety") L.OffsetY = std::stof(v);
						} catch(...) {}
						continue;
					}
					if(pos == 0) {
						if(tok.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos && tok.size() > 6) {
							L.Hash = strtoull(tok.c_str(), nullptr, 16);
							ok = true;
						}
					} else if(pos == 1) {
						std::string pl = Lower(tok);
						if(pl != "*" && pl != "all" && pl != "any" && pl != "ignore") {
							L.Palette = (uint32_t)strtoul(pl.c_str(), nullptr, 10);
							L.AnyPalette = false;
						}
					}
					pos++;
				}
				if(ok) {
					outData.SpriteLights.push_back(L);
				} else {
					MessageManager::Log("[HD-GB-LOAD] <2DSpriteLight> : hash invalide : " + line);
					ignoredLines++;
				}
				continue;
			}

			if(line.size() >= 11 && Lower(line.substr(0, 11)) == "<2doverlay>") {
				if(!ParseOverlay(packFolder, line.substr(11), terms, outData)) {
					ignoredLines++;
				}
				continue;
			}

			if(line.compare(0, 13, "<setVariable>") == 0) {
				// [fork GB] <setVariable>nom,valeur (doc 57) : une valeur possible de $nom ; la derniere regle vraie gagne a chaque image
				std::vector<std::string> sv = SplitTrim(line.substr(13), ',');
				const std::string vname = sv.size() >= 1 ? sv[0] : std::string();
				const std::string vvalue = sv.size() >= 2 ? sv[1] : std::string();
				bool termsOk = true;
				for(const GbHdTerm& t : terms) {
					if(!t.Name.empty() && t.Name[0] == '$') termsOk = false;
				}
				if(vname.empty() || vname.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos || vvalue.empty() || vvalue[0] == '$' || !termsOk) {
					MessageManager::Log("[HD-GB-LOAD] <setVariable> : syntaxe invalide (nom alphanumerique, valeur = chemin de PNG, pas de variable dans les conditions) : " + line);
					ignoredLines++;
					continue;
				}
				GbHdVariable* var = nullptr;
				for(GbHdVariable& v : outData.Variables) { if(v.Name == vname) { var = &v; break; } }
				if(!var) {
					outData.Variables.push_back(GbHdVariable());
					var = &outData.Variables.back();
					var->Name = vname;
				}
				uint32_t vidx = 0;
				bool found = false;
				for(size_t i = 0; i < var->Values.size(); i++) {
					if(var->Values[i] == vvalue) { vidx = (uint32_t)i; found = true; break; }
				}
				if(!found) {
					var->Values.push_back(vvalue);
					vidx = (uint32_t)(var->Values.size() - 1);
				}
				GbHdVariable::Rule rule;
				rule.Terms = terms;
				rule.ValueIndex = vidx;
				var->Rules.push_back(rule);
				continue;
			}
			if(line.compare(0, 12, "<background>") == 0) {
				// <background>fichier|dossier,luminosite,scrollX,scrollY,Z,offsetX,offsetY,coucheMaitre,tileX,tileY,framesParImage,inverse,alpha,operateur
				std::vector<std::string> f = SplitTrim(line.substr(12), ',');
				// Options nommees ramx=expr / ramy=expr : retirees avant la lecture positionnelle
				HdWatchExpr bgRamX, bgRamY;
				for(size_t ti = 1; ti < f.size(); ) {
					std::string low = Lower(f[ti]);
					bool isX = low.compare(0, 5, "ramx=") == 0;
					bool isY = low.compare(0, 5, "ramy=") == 0;
					if(isX || isY) {
						std::string err;
						HdWatchExpr& dst = isX ? bgRamX : bgRamY;
						if(!HdWatchExpr::Parse(f[ti].substr(5), outData.WatchedVars, dst, err)) {
							MessageManager::Log(std::string("[HD-GB-LOAD] <background> : ") + (isX ? "ramx" : "ramy") + " : " + err + ", option ignoree.");
							dst.T.clear();
						}
						f.erase(f.begin() + ti);
					} else {
						ti++;
					}
				}
				// [fork GB] <background>$nom (doc 57) : une variante par valeur de <setVariable>$nom, conditionnee par $nom = valeur
				struct GbBgVariant { std::string Path; std::vector<GbHdTerm> Terms; };
				std::vector<GbBgVariant> variants;
				if(!f.empty() && f[0].size() > 1 && f[0][0] == '$') {
					const std::string vname = f[0].substr(1);
					const GbHdVariable* var = nullptr;
					for(const GbHdVariable& v : outData.Variables) { if(v.Name == vname) { var = &v; break; } }
					if(!var || var->Values.empty()) {
						MessageManager::Log("[HD-GB-LOAD] <background> : variable $" + vname + " inconnue (declarer <setVariable> avant cette ligne), fond ignore.");
						ignoredLines++;
						continue;
					}
					for(size_t k = 0; k < var->Values.size(); k++) {
						const std::string cname = "$" + vname + "=" + std::to_string(k);
						GbHdCondition vc;
						vc.Type = GbHdCondition::Kind::Variable;
						vc.VarName = vname;
						vc.VarIndex = (uint32_t)k;
						outData.Conditions[cname] = vc;
						GbBgVariant gv;
						gv.Path = var->Values[k];
						gv.Terms = terms;
						GbHdTerm vt;
						vt.Name = cname;
						vt.Negated = false;
						gv.Terms.push_back(vt);
						variants.push_back(gv);
					}
				} else {
					GbBgVariant gv;
					gv.Path = f.empty() ? std::string() : f[0];
					gv.Terms = terms;
					variants.push_back(gv);
				}
				for(const GbBgVariant& gv : variants) {
				std::vector<std::string> fv = f;
				if(!fv.empty()) fv[0] = gv.Path;
				std::vector<std::string> files = fv.empty() ? std::vector<std::string>() : ListBackgroundFiles(packFolder, fv[0]);
				if(files.empty()) {
					ignoredLines++;
					continue;
				}
				GbHdBackground bg;
				bg.Terms = gv.Terms;
				bg.RamX = bgRamX;
				bg.RamY = bgRamY;
				for(const std::string& rel : files) {
					bg.Frames.push_back(FindOrAddImage(outData.Images, rel));
				}
				auto field = [&fv](size_t i) { return i < fv.size() ? fv[i] : std::string(); };
				bg.Luminosity = ToDouble(field(1), 1.0);
				bg.RatioX = ToDouble(field(2), 0.0);
				bg.RatioY = ToDouble(field(3), 0.0);
				bg.Z = ToInt(field(4), 5);
				bg.OffsetX = ToInt(field(5), 0);
				bg.OffsetY = ToInt(field(6), 0);
				// field(7) : coucheMaitre (SNES) : sans objet sur Game Boy (une seule couche de fond), lu et ignore
				bg.TileX = ToBool(field(8), false);
				bg.TileY = ToBool(field(9), false);
				bg.FramesBetweenSteps = (uint32_t)ToInt(field(10), 1);
				bg.Reverse = ToBool(field(11), false);
				bg.Alpha = ToDouble(field(12), 1.0);
				bg.Operator = ParseOperator(field(13));
				outData.Backgrounds.push_back(bg);
				}
				continue;
			}
			if(line.compare(0, 5, "<tile") != 0) {
				continue;
			}
			size_t close = line.find('>');
			if(close == std::string::npos) {
				ignoredLines++;
				continue;
			}
			std::string tag = line.substr(0, close + 1);
			uint8_t flags = 0;
			if(tag == "<tile>") {
				flags = 0;
			} else if(tag == "<tile_hflip>") {
				flags = 2;
			} else if(tag == "<tile_vflip>") {
				flags = 4;
			} else if(tag == "<tile_hvflip>") {
				flags = 6;
			} else {
				ignoredLines++;
				continue;
			}

			// Corps : hash,palette,image,x,y
			std::vector<std::string> fields = SplitTrim(line.substr(close + 1), ',');
			if(fields.size() != 5) {
				ignoredLines++;
				continue;
			}

			uint64_t hash = strtoull(fields[0].c_str(), nullptr, 16);
			uint32_t palette = (uint32_t)strtoul(fields[1].c_str(), nullptr, 10);
			uint32_t imageIndex = 0;
			std::string imgField = Trim(fields[2]);
			if(!imgField.empty() && imgField.find_first_not_of("0123456789") == std::string::npos) {
				// Index d'une <img> deja declaree
				uint32_t tagIndex = (uint32_t)strtoul(imgField.c_str(), nullptr, 10);
				if(tagIndex >= imgTags.size()) {
					ignoredLines++;
					continue;
				}
				imageIndex = imgTags[tagIndex];
			} else if(!imgField.empty()) {
				// Chemin PNG direct, relatif au dossier du pack
				imageIndex = FindOrAddImage(outData.Images, imgField);
			} else {
				ignoredLines++;
				continue;
			}

			GbHdTileRef ref;
			ref.ImageIndex = imageIndex;
			ref.X = (uint32_t)strtoul(fields[3].c_str(), nullptr, 10);
			ref.Y = (uint32_t)strtoul(fields[4].c_str(), nullptr, 10);
			for(size_t bi = blockStack.size(); bi-- > 0; ) { // bloc le plus interne de chaque sorte
				if(blockStack[bi].first == 'd' && ref.DepthGroup == 0) ref.DepthGroup = (uint8_t)blockStack[bi].second;
				if(blockStack[bi].first == 's' && ref.ShadowRule == 0) ref.ShadowRule = (uint16_t)blockStack[bi].second;
			}

			if(!terms.empty()) {
				GbHdConditionalTile ct;
				ct.Flags = flags;
				ct.Terms = terms;
				ct.Ref = ref;
				outData.ConditionalTiles.push_back(ct);
				outData.ConditionalIndex[std::make_pair(hash, palette)].push_back(outData.ConditionalTiles.size() - 1);
				conditionalTiles++;
			} else {
				auto key = std::make_tuple(hash, palette, flags);
				if(outData.Tiles.count(key)) {
					duplicates++;
				}
				outData.Tiles[key] = ref;
			}
		}
	}

	int missingImages = 0;
	for(const std::string& img : outData.Images) {
		std::ifstream imgFile(FolderUtilities::CombinePath(packFolder, img), std::ios::binary);
		if(!imgFile.good()) {
			missingImages++;
		}
	}

	outData.IsLoaded = true;
	MessageManager::Log("[HD-GB-LOAD] Pack : " + packFolder);
	MessageManager::Log("[HD-GB-LOAD] scale " + std::to_string(outData.Scale) + ", " + std::to_string(outData.Images.size()) + " images, " + std::to_string(outData.Tiles.size()) + " tiles, " + std::to_string(duplicates) + " doublons, " + std::to_string(ignoredLines) + " lignes ignorees, " + std::to_string(missingImages) + " images absentes");
	MessageManager::Log("[HD-GB-LOAD] conditions " + std::to_string(outData.Conditions.size()) + ", tiles conditionnelles " + std::to_string(conditionalTiles) + ", adresses surveillees " + std::to_string(outData.WatchedAddresses.size()) + ", types non pris en charge " + std::to_string(unsupportedConditions) + ", lignes conditionnelles inconnues " + std::to_string(unknownConditionLines));
	MessageManager::Log("[HD-GB-LOAD] fonds " + std::to_string(outData.Backgrounds.size()) + ", spriteZ " + std::to_string(outData.SpriteZ) + ", effets 2DOverlay " + std::to_string(outData.Overlays.size()) + ", halos 2DSpriteLight " + std::to_string(outData.SpriteLights.size()) + ", regles muteAudio " + std::to_string(outData.AudioMutes.size()) + ", pistes bgm " + std::to_string(outData.BgmFiles.size()) + ", regles play_conditional_bgm " + std::to_string(outData.ConditionalBgms.size()));
	return true;
}
