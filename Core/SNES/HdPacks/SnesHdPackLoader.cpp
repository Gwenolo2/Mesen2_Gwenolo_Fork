#include "pch.h"
#include <filesystem>
#include <random>
#include "SNES/HdPacks/SnesHdPackLoader.h"
#include "SNES/HdPacks/SnesHdPackConditions.h"
#include "SNES/HdPacks/SnesHdGltf.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/VirtualFile.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <array>

#include "SNES/SnesPpu.h"
#include "SNES/SnesPpuTypes.h"
#include "Utilities/Patches/IpsPatcher.h"
#include "Utilities/Patches/BpsPatcher.h"

// --- DÉCODEUR VIDÉO MPEG-1 ---
#define PL_MPEG_IMPLEMENTATION
#include "Shared/pl_mpeg.h" // Modifie le chemin si tu l'as mis dans un autre dossier
// -----------------------------



namespace
{

	std::string NormalizePath(std::string path)
	{
		std::replace(path.begin(), path.end(), '\\', '/');
		return StringUtilities::Trim(path);
	}

	struct SnesHdLineInfo
	{
		std::string Content;
		std::string Filename;
		int LineNumber;
	};

	void LoadFileLinesRecursive(const std::string& filepath, const std::string& displayFilename, const std::string& rootFolder, std::vector<SnesHdLineInfo>& lines, int depth = 0)
	{
		// Sécurité anti-récursion infinie
		if(depth > 20) {
			MessageManager::Log("[SnesHDPack] Max include depth reached at: " + filepath);
			return;
		}

		std::ifstream file(filepath);
		if(!file.is_open()) {
			MessageManager::Log("[SnesHDPack] Failed to open included file: " + filepath);
			return;
		}

		std::string lineContent;
		int lineNum = 0;

		while(std::getline(file, lineContent)) {
			lineNum++;

			std::string trimmed = lineContent;
			trimmed.erase(std::remove(trimmed.begin(), trimmed.end(), '\r'), trimmed.end());
			size_t commentPos = trimmed.find('#');
			if(commentPos != std::string::npos) {
				trimmed = trimmed.substr(0, commentPos);
			}
			trimmed.erase(0, trimmed.find_first_not_of(" \t"));

			if(trimmed.empty()) {
				continue;
			}

			// Résolution des tags <include>
			if(trimmed.substr(0, 9) == "<include>") {
				std::string includeFile = StringUtilities::Trim(trimmed.substr(9));
				std::string includePath = FolderUtilities::CombinePath(rootFolder, includeFile);
				LoadFileLinesRecursive(includePath, includeFile, rootFolder, lines, depth + 1);
			} else {
				lines.push_back({ lineContent, displayFilename, lineNum });
			}
		}
	}

}




// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------


bool SnesHdPackLoader::_isLoaded = false;
std::shared_ptr<SnesHdPackData> SnesHdPackLoader::ActivePack = nullptr; // C'est lui le vrai patron maintenant !

#define logError(y) LogError(y)
#define checkConstraint(x, y) if(!(x)) { logError(y); return; }
// ---------------------------------------------------------------------------
// Static API
// ---------------------------------------------------------------------------
//---------------------------------------------------------------------------------------------------------------
// Lecteur JSON minimal "a plat" pour les effets <2DOverlay> : { "cle": valeur, ... }
// Valeurs : nombres, chaines, true/false. Cles insensibles a la casse. Commentaires // acceptes.
// Dans une chaine, "\\" donne "\" et "\"" donne '"' ; un "\" isole est conserve (chemins Windows).
//---------------------------------------------------------------------------------------------------------------
// Hash(s) de carte Mode 7 : "*" / "any" = toutes ; "A|B|C" = plusieurs (cartes animees : un hash par etape)
static void SnesHdParseMaps(const std::string& s0, bool& any, uint32_t& first, std::vector<uint32_t>& all)
{
	std::string s = StringUtilities::ToLower(StringUtilities::Trim(s0));
	all.clear();
	any = (s.empty() || s == "*" || s == "any");
	if(any) return;
	for(std::string& part : StringUtilities::Split(s, '|')) {
		std::string p = StringUtilities::Trim(part);
		if(p.empty()) continue;
		try { all.push_back((uint32_t)HexUtilities::FromHex(p)); } catch(...) {}
	}
	if(all.empty()) { any = true; return; }
	first = all[0];
}

// Cles de mouvement communes (<m7model>, <m7Effect>) : renvoie vrai si la cle a ete reconnue
static bool SnesHdParseMotionKey(SnesHdM7ModelInfo& md, const std::string& k, const std::string& v)
{
	auto isTrue = [](std::string x) { x = StringUtilities::ToLower(x); return x == "1" || x == "true" || x == "yes" || x == "oui"; };
	try {
		if(k == "spin") md.Spin = std::stof(v);
		else if(k == "bobamp") md.BobAmp = std::stof(v);
		else if(k == "bobfreq") md.BobFreq = std::stof(v);
		else if(k == "swayamp") md.SwayAmp = std::stof(v);
		else if(k == "swayfreq") md.SwayFreq = std::stof(v);
		else if(k == "offset" || k == "timeoffset") md.TimeOffset = std::stof(v);
		else if(k == "speed" || k == "pathspeed") md.PathSpeed = std::stof(v);
		else if(k == "orient") md.PathOrient = isTrue(v);
		else if(k == "smooth") md.PathSmooth = isTrue(v);
		else if(k == "loop") { std::string m = StringUtilities::ToLower(v); md.PathLoop = (m == "pingpong" || m == "allerretour") ? 1 : ((m == "once" || m == "false" || m == "0") ? 2 : 0); }
		else if(k == "path" || k == "trajet") {
			md.Path.clear();
			for(std::string& pt : StringUtilities::Split(v, '|')) {
				std::vector<std::string> c = StringUtilities::Split(pt, ':');
				if(c.size() < 2) continue;
				std::array<float, 3> p = { std::stof(c[0]), std::stof(c[1]), c.size() >= 3 ? std::stof(c[2]) : 0.0f };
				md.Path.push_back(p);
			}
		}
		else return false;
	} catch(...) {}
	return true;
}

static std::unordered_map<std::string, std::string> ParseFlatJson(const std::string& s)
{
	std::unordered_map<std::string, std::string> out;
	size_t i = 0, n = s.size();
	auto readString = [&]() -> std::string {
		std::string r;
		i++; // guillemet ouvrant
		while(i < n && s[i] != '"') {
			if(s[i] == '\\' && i + 1 < n && (s[i + 1] == '\\' || s[i + 1] == '"')) { i++; }
			r += s[i++];
		}
		if(i < n) i++; // guillemet fermant
		return r;
	};
	while(i < n) {
		char c = s[i];
		if(c == '/' && i + 1 < n && s[i + 1] == '/') { while(i < n && s[i] != '\n') i++; continue; }
		if(c != '"') { i++; continue; }
		std::string key = readString();
		while(i < n && s[i] != ':' && s[i] != ',' && s[i] != '}') i++;
		if(i >= n || s[i] != ':') continue;
		i++;
		while(i < n && std::isspace((unsigned char)s[i])) i++;
		std::string val;
		if(i < n && s[i] == '"') {
			val = readString();
		} else {
			while(i < n && s[i] != ',' && s[i] != '}' && s[i] != '\n' && s[i] != '\r') val += s[i++];
			val = StringUtilities::Trim(val);
		}
		out[StringUtilities::ToLower(StringUtilities::Trim(key))] = val;
	}
	return out;
}

//---------------------------------------------------------------------------------------------------------------
// Lecteur OBJ / MTL minimal pour <m7model> : v, vt, vn, f (polygones -> eventails de triangles, indices
// negatifs acceptes), mtllib, usemtl ; MTL : newmtl, Kd, d, Tr, map_Kd. Chemins relatifs au dossier de l'OBJ.
//---------------------------------------------------------------------------------------------------------------
static std::shared_ptr<SnesHdModelMesh> LoadObjModel(const std::string& rootFolder, const std::string& objFile,
	std::shared_ptr<SnesHdPackData>& outData, std::unordered_map<std::string, SnesHdPackBitmapInfo*>& loadedBitmaps,
	const std::string& lineLocation)
{
	VirtualFile vf(FolderUtilities::CombinePath(rootFolder, objFile));
	std::vector<uint8_t> raw;
	if(!vf.IsValid() || !vf.ReadFile(raw)) {
		MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> : OBJ introuvable : " + objFile);
		return nullptr;
	}
	std::string objDir;
	{
		size_t sl = objFile.find_last_of("/\\");
		if(sl != std::string::npos) objDir = objFile.substr(0, sl + 1);
	}
	auto mesh = std::make_shared<SnesHdModelMesh>();
	mesh->Mats.push_back(SnesHdModelMaterial()); // materiau 0 : blanc par defaut
	std::unordered_map<std::string, int> matIndex;

	auto loadTexture = [&](const std::string& name) -> SnesHdPackBitmapInfo* {
		std::string filename = objDir + name;
		for(char& ch : filename) if(ch == '\\') ch = '/';
		auto itB = loadedBitmaps.find(filename);
		if(itB != loadedBitmaps.end()) return itB->second;
		auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
		bitmap->PngName = filename;
		VirtualFile pf(FolderUtilities::CombinePath(rootFolder, filename));
		if(pf.IsValid()) pf.ReadFile(bitmap->FileData);
		else MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> : texture introuvable : " + filename);
		SnesHdPackBitmapInfo* p = bitmap.get();
		loadedBitmaps[filename] = p;
		outData->ImageFileData.push_back(std::move(bitmap));
		return p;
	};
	auto restOf = [](const std::string& l, size_t from) { return StringUtilities::Trim(l.substr(std::min(from, l.size()))); };

	auto loadMtl = [&](const std::string& mtlName) {
		VirtualFile mf(FolderUtilities::CombinePath(rootFolder, objDir + mtlName));
		std::vector<uint8_t> mraw;
		if(!mf.IsValid() || !mf.ReadFile(mraw)) {
			MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> : MTL introuvable : " + objDir + mtlName);
			return;
		}
		std::stringstream ms(std::string(mraw.begin(), mraw.end()));
		std::string l;
		int cur = -1;
		while(std::getline(ms, l)) {
			l = StringUtilities::Trim(l);
			if(l.empty() || l[0] == '#') continue;
			std::stringstream ls(l);
			std::string k; ls >> k;
			if(k == "newmtl") {
				SnesHdModelMaterial m; m.Name = restOf(l, 6);
				mesh->Mats.push_back(m);
				cur = (int)mesh->Mats.size() - 1;
				matIndex[m.Name] = cur;
			} else if(cur >= 0 && k == "Kd") {
				float r = 1, g = 1, b = 1; ls >> r >> g >> b;
				auto c8 = [](float f) { return (uint32_t)std::clamp((int)(f * 255.0f + 0.5f), 0, 255); };
				mesh->Mats[cur].Kd = (c8(r) << 16) | (c8(g) << 8) | c8(b);
			} else if(cur >= 0 && k == "d") {
				float d = 1; ls >> d; mesh->Mats[cur].Alpha = std::clamp(d, 0.0f, 1.0f);
			} else if(cur >= 0 && k == "Tr") {
				float t = 0; ls >> t; mesh->Mats[cur].Alpha = std::clamp(1.0f - t, 0.0f, 1.0f);
			} else if(cur >= 0 && k == "map_Kd") {
				// le nom du fichier est le dernier element (des options -s, -o... peuvent preceder)
				std::string rest = restOf(l, 6), last;
				std::stringstream rs(rest); std::string tok;
				while(rs >> tok) last = tok;
				if(!last.empty()) mesh->Mats[cur].Texture = loadTexture(last);
			}
		}
	};

	std::vector<std::array<float, 3>> pos, nrm;
	std::vector<std::array<float, 2>> uvs;
	std::stringstream ss(std::string(raw.begin(), raw.end()));
	std::string line;
	int curMat = 0;
	bool first = true;
	while(std::getline(ss, line)) {
		if(line.empty() || line[0] == '#') continue;
		if(!line.empty() && line.back() == '\r') line.pop_back();
		std::stringstream ls(line);
		std::string k; ls >> k;
		if(k == "v") {
			std::array<float, 3> p = { 0, 0, 0 }; ls >> p[0] >> p[1] >> p[2]; pos.push_back(p);
		} else if(k == "vt") {
			std::array<float, 2> t = { 0, 0 }; ls >> t[0] >> t[1]; uvs.push_back(t);
		} else if(k == "vn") {
			std::array<float, 3> n = { 0, 1, 0 }; ls >> n[0] >> n[1] >> n[2]; nrm.push_back(n);
		} else if(k == "mtllib") {
			loadMtl(restOf(line, 6));
		} else if(k == "usemtl") {
			auto it = matIndex.find(restOf(line, 6));
			curMat = it != matIndex.end() ? it->second : 0;
		} else if(k == "f") {
			struct Corner { int p = -1, t = -1, n = -1; };
			std::vector<Corner> poly;
			std::string tok;
			while(ls >> tok) {
				Corner c;
				int idx[3] = { 0, 0, 0 }; int part = 0; std::string cur;
				for(size_t i = 0; i <= tok.size(); i++) {
					if(i == tok.size() || tok[i] == '/') {
						if(!cur.empty() && part < 3) { try { idx[part] = std::stoi(cur); } catch(...) { idx[part] = 0; } }
						cur.clear(); part++;
					} else cur += tok[i];
				}
				auto res = [](int i, size_t n) -> int { if(i > 0) return i - 1; if(i < 0) return (int)n + i; return -1; };
				c.p = res(idx[0], pos.size()); c.t = res(idx[1], uvs.size()); c.n = res(idx[2], nrm.size());
				if(c.p < 0 || c.p >= (int)pos.size()) continue;
				if(c.t >= (int)uvs.size()) c.t = -1;
				if(c.n >= (int)nrm.size()) c.n = -1;
				poly.push_back(c);
			}
			for(size_t i = 1; i + 1 < poly.size(); i++) {
				const Corner cs[3] = { poly[0], poly[i], poly[i + 1] };
				SnesHdModelTri t; t.Mat = curMat;
				for(int j = 0; j < 3; j++) {
					for(int a = 0; a < 3; a++) t.P[j][a] = pos[cs[j].p][a];
					t.UV[j][0] = cs[j].t >= 0 ? uvs[cs[j].t][0] : 0.0f;
					t.UV[j][1] = cs[j].t >= 0 ? uvs[cs[j].t][1] : 0.0f;
				}
				// normale de la face (si le fichier n'en donne pas)
				float e1[3], e2[3], fn[3];
				for(int a = 0; a < 3; a++) { e1[a] = t.P[1][a] - t.P[0][a]; e2[a] = t.P[2][a] - t.P[0][a]; }
				fn[0] = e1[1] * e2[2] - e1[2] * e2[1]; fn[1] = e1[2] * e2[0] - e1[0] * e2[2]; fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
				float fl = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
				if(fl < 1e-12f) continue; // triangle degenere
				for(int a = 0; a < 3; a++) fn[a] /= fl;
				for(int j = 0; j < 3; j++) {
					if(cs[j].n >= 0) { for(int a = 0; a < 3; a++) t.N[j][a] = nrm[cs[j].n][a]; }
					else { for(int a = 0; a < 3; a++) t.N[j][a] = fn[a]; }
					for(int a = 0; a < 3; a++) {
						float pv = t.P[j][a];
						float& mn = (a == 0 ? mesh->MinX : (a == 1 ? mesh->MinY : mesh->MinZ));
						float& mx = (a == 0 ? mesh->MaxX : (a == 1 ? mesh->MaxY : mesh->MaxZ));
						if(first) { mn = mx = pv; } else { mn = std::min(mn, pv); mx = std::max(mx, pv); }
					}
					first = false;
				}
				mesh->Tris.push_back(t);
				if(mesh->Tris.size() > 200000) break;
			}
		}
	}
	if(mesh->Tris.empty()) {
		MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> : aucun triangle dans " + objFile);
		return nullptr;
	}
	return mesh;
}

bool SnesHdPackLoader::LoadHdPack(const std::string& definitionFile, std::shared_ptr<SnesHdPackData>& outData)
{
	std::ifstream fileTest(definitionFile);
	if(!fileTest.is_open()) {
		MessageManager::Log("[SnesHDPack] Failed to open : " + definitionFile);
		return false;
	}
	fileTest.close();

	outData = std::make_shared<SnesHdPackData>();
	std::string rootFolder = FolderUtilities::GetFolderName(definitionFile);

	outData->RootFolder = rootFolder;

	// Extraction de toutes les lignes des fichiers (inclus ou non)
	std::vector<SnesHdLineInfo> parsedLines;
	LoadFileLinesRecursive(definitionFile, FolderUtilities::GetFilename(definitionFile, true), rootFolder, parsedLines);

	// Dictionnaire pour ne créer qu'un seul objet BitmapInfo par PNG
	std::unordered_map<std::string, SnesHdPackBitmapInfo*> loadedBitmaps;
	std::unordered_map<std::string, std::shared_ptr<SnesHdModelMesh>> loadedModels; // <m7model> : un meme OBJ charge une fois

	std::string lineContent;
	std::vector<std::string> imageCatalog;
	std::unordered_map<std::string, SnesHdPackCondition*> conditionsByName;

	//Dictionnaire pour stocker les alias (Clé = "@Nom", Valeur = "remplacement")
	std::unordered_map<std::string, std::string> aliases;

	outData->PackFolderPath = rootFolder;

	// ====================================================================
	// 1. DÉCLARATION DU SYSTÈME DE FALLBACK ET DE LA LAMBDA 
	// (HORS DE LA BOUCLE POUR OPTIMISATION)
	// ====================================================================
	struct FallbackBlockState
	{
		std::string Palette;
		std::string ImageParam;
	};
	std::vector<FallbackBlockState> fallbackStack;
	// Blocs entre accolades : 'F' = <fallback ...>{ , 'R' = <FrontOf3D>{ (pile commune pour la fermeture '}')
	std::vector<char> blockKinds;
	std::vector<std::vector<SnesHdPackCondition*>> frontBlockStack; // conditions de chaque bloc <FrontOf3D>{ ouvert
	std::vector<std::vector<SnesHdPackCondition*>> noReflBlockStack; // conditions de chaque bloc <NoLiquidReflect>{ ouvert ('N')
	std::vector<std::vector<SnesHdPackCondition*>> ign2dBlockStack; // conditions de chaque bloc <Ignore2DOverlays>{ ouvert ('I')
	std::vector<int> ign2dBlockId, ign2dBlockCount;                  // numero du bloc, tiles ajoutees
	int ign2dBlockNext = 0;
	std::unordered_set<std::string> ign2dBlockSeen;                  // bloc|hash|palette deja ajoutes
	std::vector<int> noReflBlockCount;
	std::unordered_set<std::string> noReflSeen;
	auto isNoReflTag = [](const std::string& line) -> size_t { // longueur de la balise, 0 si absente (accepte la faute "refelct")
		const std::string l = StringUtilities::ToLower(line.substr(0, 17));
		return (l == "<noliquidreflect>" || l == "<noliquidrefelct>") ? 17 : 0;
	};
	std::vector<int> frontBlockId, frontBlockCount;                 // numero du bloc, tiles ajoutees
	int frontBlockNext = 0;
	std::vector<SnesHdSpriteLightInfo> spriteLightBlockStack;     // modele de chaque bloc <SpriteLight ...>{ ouvert ('L')
	std::vector<int> spriteLightBlockCount;
	// Cles d'une <SpriteLight> : radius, color, intensity, height (ou auto), flicker, speed, mode=light|add
	auto parseSpriteLightKey = [](SnesHdSpriteLightInfo& L, std::string k, std::string v) -> bool {
		k = StringUtilities::ToLower(StringUtilities::Trim(k)); v = StringUtilities::Trim(v);
		auto col = [&](uint32_t def) -> uint32_t { std::string c = v; if(!c.empty() && c[0] == '#') c = c.substr(1); try { return (uint32_t)std::stoul(c, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; } };
		try {
			if(k == "radius" || k == "rayon") L.Radius = std::max(1.0f, std::stof(v));
			else if(k == "color" || k == "couleur") L.Color = col(L.Color);
			else if(k == "intensity" || k == "intensite") L.Intensity = std::max(0.0f, std::stof(v));
			else if(k == "height" || k == "hauteur") L.Height = (StringUtilities::ToLower(v) == "auto") ? -1.0f : std::stof(v);
			else if(k == "flicker" || k == "scintillement") L.Flicker = std::clamp(std::stof(v), 0.0f, 1.0f);
			else if(k == "speed" || k == "flickerspeed" || k == "vitesse") L.FlickerSpeed = std::stof(v);
			else if(k == "mode") { const std::string m = StringUtilities::ToLower(v); L.Additive = (m == "add" || m == "additive"); L.Shadow = (m == "shadow" || m == "sub" || m == "ombre"); }
			else if(k == "self" || k == "selflight") L.SelfLight = std::clamp(std::stof(v), 0.0f, 1.0f);
			else if(k == "glow" || k == "lueur") L.Glow = std::clamp(std::stof(v), 0.0f, 4.0f);
			else if(k == "halo") L.Halo = std::clamp(std::stof(v), 0.0f, 64.0f);
			else if(k == "distance" || k == "airdistance") L.AirDistance = std::max(0.0f, std::stof(v));
			else if(k == "plane" || k == "map" || k == "carte") L.PlaneViews = !(v == "0" || StringUtilities::ToLower(v) == "false" || StringUtilities::ToLower(v) == "non");
			else return false;
		} catch(...) {}
		return true;
	};
	std::unordered_set<std::string> frontBlockSeen;                 // bloc|hash|palette deja ajoutes (pas de doublons)

	// La Lambda prend désormais les conditions et la ligne en paramètre
	auto processTileTokens = [&](const std::vector<std::string>& tokens, bool reqHFlip, bool reqVFlip, const std::vector<SnesHdPackCondition*>& activeConditions, const std::string& lineLoc) {

		// 1. TRAITEMENT DE "disable" ET "blank"
		if(tokens.size() >= 3) {
			std::string imageParam = StringUtilities::ToLower(StringUtilities::Trim(tokens[2]));

			if(imageParam == "disable" || imageParam == "blank") {
				try {
					uint64_t memoryHash = std::stoull(tokens[0], nullptr, 16);
					uint32_t paletteHash = 0;

					std::string palStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[1]));
					if(palStr == "*" || palStr == "all") {
						paletteHash = 0xFFFFFFFF;
					} else {
						paletteHash = std::stoul(palStr);
					}

					SnesHdPackTileInfo* tile = new SnesHdPackTileInfo();
					tile->MemoryHash = memoryHash;
					tile->PaletteHash = paletteHash;
					tile->IsDisabled = true;
					tile->IsHFlip = reqHFlip;
					tile->IsVFlip = reqVFlip;
					tile->Conditions = activeConditions;
					tile->Bitmap = nullptr; // Explicite : pas d'image associée

					SnesHdTileKey key;
					key.MemoryHash = memoryHash;
					key.PaletteHash = paletteHash;
					key.IsLargeTile = false;
					key.IsHFlip = reqHFlip;
					key.IsVFlip = reqVFlip;

					std::vector<SnesBpp> bppsToTry = { SnesBpp::Bpp2, SnesBpp::Bpp4 };
					for(auto currentBpp : bppsToTry) {
						key.Bpp = currentBpp;

						key.IsSprite = false;
						outData->TileByKey[key].push_back(tile);

						key.IsSprite = true;
						outData->TileByKey[key].push_back(tile);
					}
					// Gestion de la mémoire
					outData->Tiles.push_back(std::unique_ptr<SnesHdPackTileInfo>(tile));
					return; // On sort de la lambda
				} catch(const std::exception& e) {
					MessageManager::Log("[SnesHDPack] Line " + lineLoc + " - <tile> : Erreur de parsing disable/blank.");
					return; // On sort de la lambda
				}
			}
		}

		// 2. TRAITEMENT DES TUILES STANDARD (Hash, Palette, Image/Index, X, Y)
		if(tokens.size() >= 5) {
			uint64_t memoryHash = 0;
			uint32_t paletteHash = 0;
			int x = 0;
			int y = 0;
			std::string pngPath = "";

			try {
				memoryHash = std::stoull(tokens[0], nullptr, 16);

				// SUPPORT DU WILDCARD SUR LA PALETTE ('*' ou 'all')
				std::string palStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[1]));
				if(palStr == "*" || palStr == "all") {
					paletteHash = 0xFFFFFFFF; // Marqueur universel
				} else {
					paletteHash = std::stoul(palStr);
				}

				x = std::stoi(tokens[3]);
				y = std::stoi(tokens[4]);

				std::string imageParam = tokens[2];

				if(imageParam.length() >= 4 && imageParam.substr(imageParam.length() - 4) == ".png") {
					pngPath = imageParam;
				} else {
					try {
						int imageIndex = std::stoi(imageParam);

						if(imageIndex >= 0 && (size_t)imageIndex < imageCatalog.size()) {
							pngPath = imageCatalog[imageIndex];
						} else {
							MessageManager::Log("[SnesHDPack] Line " + lineLoc + " - <tile> : Image Index out of bounds : " + imageParam);
							return; // On sort de la lambda
						}
					} catch(...) {
						pngPath = imageParam;
					}
				}

			} catch(const std::exception& e) {
				MessageManager::Log("[SnesHDPack] Line " + lineLoc + " - <tile> : Parameter Conversion error.");
				return; // On sort de la lambda
			}

			if(pngPath.empty()) {
				return; // On sort de la lambda
			}

			// --- GESTION DU MOT-CLÉ "Randomize" ---
			std::string randomToken = "Randomize";
			size_t randPos;

			while((randPos = pngPath.find(randomToken)) != std::string::npos) {
				std::string parentRelative = pngPath.substr(0, randPos);
				std::string afterRandomize = pngPath.substr(randPos + randomToken.length());
				std::string parentFullPath = FolderUtilities::CombinePath(rootFolder, parentRelative);

				std::vector<std::string> subFolders;
				try {
					for(const auto& entry : std::filesystem::directory_iterator(parentFullPath)) {
						if(entry.is_directory()) {
							subFolders.push_back(entry.path().filename().string());
						}
					}
				} catch(const std::exception& e) {
					MessageManager::Log("[SnesHDPack] Line " + lineLoc + " - Error reading 'Randomize' Folder : " + parentFullPath);
					pngPath = "";
					break;
				}

				if(!subFolders.empty()) {
					static std::random_device rd;
					static std::mt19937 gen(rd());
					std::uniform_int_distribution<> distrib(0, subFolders.size() - 1);

					std::string chosenFolder = subFolders[distrib(gen)];
					pngPath = parentRelative + chosenFolder + afterRandomize;
				} else {
					MessageManager::Log("[SnesHDPack] Line " + lineLoc + " - No SubFolder in : " + parentFullPath);
					pngPath = "";
					break;
				}
			}

			if(pngPath.empty()) {
				return; // On sort de la lambda
			}


			pngPath = NormalizePath(pngPath);

			// 3. RECUPERATION DU BITMAP ET INSCRIPTION
			if(loadedBitmaps.find(pngPath) == loadedBitmaps.end()) {
				auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
				bitmap->PngName = pngPath;

				std::string fullPath = FolderUtilities::CombinePath(rootFolder, pngPath);
				VirtualFile pngFile(fullPath);
				if(pngFile.IsValid()) {
					pngFile.ReadFile(bitmap->FileData);
				} else {
					MessageManager::Log("[SnesHDPack] Line " + lineLoc + " - Png file not found : " + fullPath);
				}

				loadedBitmaps[pngPath] = bitmap.get();
				outData->ImageFileData.push_back(std::move(bitmap));
			}

			SnesHdTileKey key;
			key.MemoryHash = memoryHash;
			key.PaletteHash = paletteHash;
			key.Bpp = SnesBpp::Bpp4;
			key.IsLargeTile = false;
			key.IsSprite = false;
			key.IsHFlip = reqHFlip;
			key.IsVFlip = reqVFlip;

			SnesHdPackTileInfo* tile = new SnesHdPackTileInfo();
			tile->MemoryHash = key.MemoryHash;
			tile->PaletteHash = key.PaletteHash;
			tile->Bpp = key.Bpp;
			tile->IsLargeTile = key.IsLargeTile;
			tile->IsSprite = false;
			tile->IsHFlip = reqHFlip;
			tile->IsVFlip = reqVFlip;
			tile->X = x;
			tile->Y = y;
			tile->Width = 8;
			tile->Height = 8;

			if(tokens.size() >= 6) {
				std::string opt = StringUtilities::ToLower(StringUtilities::Trim(tokens[5]));
				if(opt == "1" || opt == "true" || opt == "ignore") {
					tile->IgnoreColorMath = true;
				}
			}

			tile->Bitmap = loadedBitmaps[pngPath];
			tile->Conditions = activeConditions;

			std::vector<SnesBpp> bppsToTry = { SnesBpp::Bpp2, SnesBpp::Bpp4 };
			for(auto currentBpp : bppsToTry) {
				key.Bpp = currentBpp;

				key.IsSprite = false;
				outData->TileByKey[key].push_back(tile);

				key.IsSprite = true;
				outData->TileByKey[key].push_back(tile);
			}

			outData->Tiles.push_back(std::unique_ptr<SnesHdPackTileInfo>(tile));
		}
	};


	// ====================================================================
	// 2. PARCOURS DE TES LIGNES (BOUCLE PRINCIPALE)
	// ====================================================================
	for(const auto& lineInfo : parsedLines) {
		lineContent = lineInfo.Content;

		// Variable qui remplace currentLine par 'monfichier.txt:123'
		std::string lineLocation = lineInfo.Filename + ":" + std::to_string(lineInfo.LineNumber);

		// Nettoyage de la ligne (Retours chariot, commentaires, espaces)
		lineContent.erase(std::remove(lineContent.begin(), lineContent.end(), '\r'), lineContent.end());
		size_t commentPos = lineContent.find('#');
		if(commentPos != std::string::npos) {
			lineContent = lineContent.substr(0, commentPos);
		}
		lineContent.erase(0, lineContent.find_first_not_of(" \t"));

		if(lineContent.empty()) continue;

		// ====================================================================
		// --- ALIAS Management --- (text replacement with @AliasName)
		// ====================================================================
		if(lineContent.substr(0, 7) == "<alias>") {
			std::string aliasData = lineContent.substr(7);
			size_t commaPos = aliasData.find(',');
			if(commaPos != std::string::npos) {
				std::string aliasName = "@" + StringUtilities::Trim(aliasData.substr(0, commaPos));
				std::string aliasValue = StringUtilities::Trim(aliasData.substr(commaPos + 1));
				aliases[aliasName] = aliasValue;
			} else {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - Syntax error <alias>. Expected : <alias>AliasName,Value");
			}
			continue;
		}

		bool aliasReplaced;
		int passCount = 0;
		do {
			aliasReplaced = false;
			size_t atPos = 0;
			while((atPos = lineContent.find('@', atPos)) != std::string::npos) {
				size_t endPos = atPos + 1;
				while(endPos < lineContent.length() && (std::isalnum(static_cast<unsigned char>(lineContent[endPos])) || lineContent[endPos] == '_')) {
					endPos++;
				}
				std::string aliasName = lineContent.substr(atPos, endPos - atPos);
				auto it = aliases.find(aliasName);
				if(it != aliases.end()) {
					lineContent.replace(atPos, aliasName.length(), it->second);
					aliasReplaced = true;
					break;
				} else {
					atPos = endPos;
				}
			}
			passCount++;
		} while(aliasReplaced && passCount < 10);

		// ====================================================================
		// DÉTECTION DU DÉBUT DE BLOC <fallback,palette,image<{
		// ====================================================================
		if(lineContent.substr(0, 9) == "<fallback") {
			size_t bracePos = lineContent.find(">{");
			std::string data = lineContent;
			if(bracePos != std::string::npos) {
				data = lineContent.substr(0, bracePos);
			}

			std::vector<std::string> tokens = StringUtilities::Split(data, ',');
			if(tokens.size() >= 3) {
				FallbackBlockState fbState;
				fbState.Palette = StringUtilities::Trim(tokens[1]);
				fbState.ImageParam = StringUtilities::Trim(tokens[2]);
				fallbackStack.push_back(fbState);
				blockKinds.push_back('F');
			} else {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <fallback> invalid (3 parameters are required).");
			}
			continue;
		}

		// ====================================================================
		// DÉTECTION DE LA FERMETURE DU BLOC '}'
		// ====================================================================
		if(StringUtilities::Trim(lineContent) == "}" || StringUtilities::Trim(lineContent) == "}>") { //-----------------------------------
			if(!blockKinds.empty() && blockKinds.back() == 'L') {
				blockKinds.pop_back();
				if(!spriteLightBlockStack.empty()) spriteLightBlockStack.pop_back();
				if(!spriteLightBlockCount.empty()) {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <SpriteLight>{ } : " + std::to_string(spriteLightBlockCount.back()) + " tile(s) avec une lumiere 3D");
					spriteLightBlockCount.pop_back();
				}
			} else if(!blockKinds.empty() && blockKinds.back() == 'N') {
				blockKinds.pop_back();
				if(!noReflBlockStack.empty()) noReflBlockStack.pop_back();
				if(!noReflBlockCount.empty()) {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <NoLiquidReflect>{ } : " + std::to_string(noReflBlockCount.back()) + " tile(s) sans reflet");
					noReflBlockCount.pop_back();
				}
			} else if(!blockKinds.empty() && blockKinds.back() == 'I') {
				blockKinds.pop_back();
				if(!ign2dBlockStack.empty()) ign2dBlockStack.pop_back();
				if(!ign2dBlockCount.empty()) {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <Ignore2DOverlays>{ } : " + std::to_string(ign2dBlockCount.back()) + " tile(s) nettes pendant la distorsion");
					ign2dBlockCount.pop_back(); ign2dBlockId.pop_back();
				}
			} else if(!blockKinds.empty() && blockKinds.back() == 'R') {
				blockKinds.pop_back();
				if(!frontBlockStack.empty()) frontBlockStack.pop_back();
				if(!frontBlockCount.empty()) {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <FrontOf3D>{ } : " + std::to_string(frontBlockCount.back()) + " tile(s) gardee(s) devant la 3D");
					frontBlockCount.pop_back(); frontBlockId.pop_back();
				}
			} else if(!fallbackStack.empty()) {
				fallbackStack.pop_back();
				if(!blockKinds.empty()) blockKinds.pop_back();
			} else {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - '}' closing without any corresponding  <fallback> block.");
			}
			continue;
		}

		// ====================================================================
		// EXTRACTION DES CONDITIONS (Pour TOUS les tags !)
		// ====================================================================
		std::vector<SnesHdPackCondition*> currentConditions;

		if(lineContent.substr(0, 1) == "[") {
			size_t endOfCondition = lineContent.find_first_of(']', 1);
			if(endOfCondition != std::string::npos) {
				std::string conditionString = lineContent.substr(1, endOfCondition - 1);

				std::vector<std::string> condNames = StringUtilities::Split(conditionString, '&');
				for(const std::string& name : condNames) {
					auto result = conditionsByName.find(name);
					if(result != conditionsByName.end()) {
						currentConditions.push_back(result->second);
					}
				}
				lineContent = lineContent.substr(endOfCondition + 1);
			}
		}

		// ====================================================================
		// DÉBUT DE BLOC [conditions]<FrontOf3D>{ ... } : chaque <tile...> du bloc reste devant la 3D
		// ====================================================================
		if(StringUtilities::ToLower(lineContent.substr(0, 11)) == "<frontof3d>" && StringUtilities::Trim(lineContent.substr(11)).substr(0, 1) == "{") {
			std::vector<SnesHdPackCondition*> blockConds = currentConditions;
			if(!frontBlockStack.empty()) blockConds.insert(blockConds.begin(), frontBlockStack.back().begin(), frontBlockStack.back().end());
			frontBlockStack.push_back(blockConds);
			frontBlockId.push_back(frontBlockNext++); frontBlockCount.push_back(0);
			blockKinds.push_back('R');
			continue;
		}

		// DÉBUT DE BLOC [conditions]<Ignore2DOverlays>{ ... } : chaque <tile...> du bloc reste net pendant la distorsion <2DOverlay>
		if(StringUtilities::ToLower(lineContent.substr(0, 18)) == "<ignore2doverlays>" && StringUtilities::Trim(lineContent.substr(18)).substr(0, 1) == "{") {
			std::vector<SnesHdPackCondition*> blockConds = currentConditions;
			// imbrication : herite des conditions du <FrontOf3D> et du <Ignore2DOverlays> englobants
			if(!frontBlockStack.empty()) blockConds.insert(blockConds.begin(), frontBlockStack.back().begin(), frontBlockStack.back().end());
			if(!ign2dBlockStack.empty()) blockConds.insert(blockConds.begin(), ign2dBlockStack.back().begin(), ign2dBlockStack.back().end());
			ign2dBlockStack.push_back(blockConds);
			ign2dBlockId.push_back(ign2dBlockNext++); ign2dBlockCount.push_back(0);
			blockKinds.push_back('I');
			continue;
		}

// DÉBUT DE BLOC [conditions]<NoLiquidReflect>{ ... } : les <tile...> du bloc ne se refletent pas dans le liquide
		if(size_t nl = isNoReflTag(lineContent); nl && StringUtilities::Trim(lineContent.substr(nl)).substr(0, 1) == "{") {
			std::vector<SnesHdPackCondition*> blockConds = currentConditions;
			if(!noReflBlockStack.empty()) blockConds.insert(blockConds.begin(), noReflBlockStack.back().begin(), noReflBlockStack.back().end());
			noReflBlockStack.push_back(blockConds);
			noReflBlockCount.push_back(0);
			blockKinds.push_back('N');
			continue;
		}

		// DÉBUT DE BLOC [conditions]<SpriteLight>cle=valeur,...{ ... } : chaque <tile...> du bloc porte cette lumiere
		if(StringUtilities::ToLower(lineContent.substr(0, 13)) == "<spritelight>" && !StringUtilities::Trim(lineContent).empty() && StringUtilities::Trim(lineContent).back() == '{') {
			std::string params = StringUtilities::Trim(lineContent.substr(13));
			params = params.substr(0, params.size() - 1);
			SnesHdSpriteLightInfo L;
			if(!spriteLightBlockStack.empty()) L = spriteLightBlockStack.back(); // bloc imbrique : herite du bloc parent
			L.Conditions.insert(L.Conditions.end(), currentConditions.begin(), currentConditions.end());
			for(std::string& tok : StringUtilities::Split(params, ',')) {
				size_t eq = tok.find('=');
				if(eq != std::string::npos) parseSpriteLightKey(L, tok.substr(0, eq), tok.substr(eq + 1));
			}
			static int s_spriteLightBlockNext = 0;
			L.BlockId = s_spriteLightBlockNext++;
			spriteLightBlockStack.push_back(L);
			spriteLightBlockCount.push_back(0);
			blockKinds.push_back('L');
			continue;
		}

		/// <IMG> TAGS
		if(lineContent.substr(0, 5) == "<img>") {
			//imageCatalog.push_back(lineContent.substr(5));
			imageCatalog.push_back(NormalizePath(lineContent.substr(5)));
		} else if(lineContent.substr(0, 7) == "<scale>") {
			try { outData->Scale = std::stoi(lineContent.substr(7)); } catch(...) {}
		}

		// ====================================================================
		// PARSING DES TAGS <tile... > AVEC DUPLICATION FALLBACK
		// ====================================================================
		if(lineContent.substr(0, 6) == "<tile>" ||
			lineContent.substr(0, 12) == "<tile_hflip>" ||
			lineContent.substr(0, 12) == "<tile_vflip>" ||
			lineContent.substr(0, 13) == "<tile_hvflip>") {

			bool reqHFlip = false;
			bool reqVFlip = false;
			std::string data = "";

			if(lineContent.substr(0, 13) == "<tile_hvflip>") {
				reqHFlip = true; reqVFlip = true;
				data = lineContent.substr(13);
			} else if(lineContent.substr(0, 12) == "<tile_hflip>") {
				reqHFlip = true;
				data = lineContent.substr(12);
			} else if(lineContent.substr(0, 12) == "<tile_vflip>") {
				reqVFlip = true;
				data = lineContent.substr(12);
			} else {
				data = lineContent.substr(6);
			}

			std::stringstream ss(data);
			std::string item;
			std::vector<std::string> tokens;

			while(std::getline(ss, item, ',')) {
				tokens.push_back(StringUtilities::Trim(item));
			}

			// <NoLiquidReflect>{ ... } : la tile (hash, palette) ne se reflete pas (Lakitu en l'air, piece qui s'envole...)
			// <Ignore2DOverlays>{ ... } : la tile (hash, palette) reste nette pendant la distorsion (HUD)
			if(!ign2dBlockStack.empty() && !tokens.empty()) {
				auto addIgn2d = [&](const std::string& palTok) {
					auto rule = std::make_unique<SnesHdNoLightRule>();
					try { rule->Hash = std::stoull(tokens[0], nullptr, 16); } catch(...) { return; }
					std::string p = StringUtilities::ToLower(StringUtilities::Trim(palTok));
					if(!(p.empty() || p == "*" || p == "all")) {
						try { rule->Palette = (uint32_t)std::stoul(p); rule->AnyPalette = false; } catch(...) {}
					}
					const std::string key = std::to_string(ign2dBlockId.back()) + "|" + StringUtilities::ToLower(tokens[0]) + "|" + (rule->AnyPalette ? std::string("*") : std::to_string(rule->Palette));
					if(!ign2dBlockSeen.insert(key).second) return;
					ign2dBlockCount.back()++;
					rule->Conditions = ign2dBlockStack.back();
					outData->Ignore2DOverlaysRules.push_back(std::move(rule));
				};
				addIgn2d(tokens.size() >= 2 ? tokens[1] : std::string("*"));
				for(const auto& fbState : fallbackStack) addIgn2d(fbState.Palette);
				// reference simple (hash seul) : consommee ici si aucun autre bloc ne la prend
				if(tokens.size() < 3 && frontBlockStack.empty() && noReflBlockStack.empty() && spriteLightBlockStack.empty()) continue;
			}

			if(!noReflBlockStack.empty() && !tokens.empty()) {
				auto addNoRefl = [&](const std::string& palTok) {
					auto rule = std::make_unique<SnesHdNoLightRule>();
					try { rule->Hash = std::stoull(tokens[0], nullptr, 16); } catch(...) { return; }
					std::string p = StringUtilities::ToLower(StringUtilities::Trim(palTok));
					if(!(p.empty() || p == "*" || p == "all")) {
						try { rule->Palette = (uint32_t)std::stoul(p); rule->AnyPalette = false; } catch(...) {}
					}
					const std::string key = std::to_string(noReflBlockStack.size()) + "|" + std::to_string((uintptr_t)noReflBlockCount.size()) + "|" + StringUtilities::ToLower(tokens[0]) + "|" + (rule->AnyPalette ? std::string("*") : std::to_string(rule->Palette));
					if(!noReflSeen.insert(key).second) return;
					noReflBlockCount.back()++;
					rule->Conditions = noReflBlockStack.back();
					outData->NoReflectRules.push_back(std::move(rule));
				};
				addNoRefl(tokens.size() >= 2 ? tokens[1] : std::string("*"));
				for(const auto& fbState : fallbackStack) addNoRefl(fbState.Palette);
				if(tokens.size() < 3 && frontBlockStack.empty() && spriteLightBlockStack.empty()) continue; // simple reference
			}

			// <SpriteLight ...>{ ... } : la tile (hash, palette) porte la lumiere du bloc
			if(!spriteLightBlockStack.empty() && !tokens.empty()) {
				auto addLight = [&](const std::string& palTok) {
					auto L = std::make_unique<SnesHdSpriteLightInfo>(spriteLightBlockStack.back());
					try { L->Hash = std::stoull(tokens[0], nullptr, 16); } catch(...) { return; }
					std::string p = StringUtilities::ToLower(StringUtilities::Trim(palTok));
					L->AnyPalette = true;
					if(!(p.empty() || p == "*" || p == "all")) {
						try { L->Palette = (uint32_t)std::stoul(p); L->AnyPalette = false; } catch(...) {}
					}
					for(auto& e : outData->SpriteLights) {
						if(e->Hash == L->Hash && e->AnyPalette == L->AnyPalette && e->Palette == L->Palette && e->Conditions == L->Conditions) return; // deja (flip)
					}
					spriteLightBlockCount.back()++;
					outData->SpriteLights.push_back(std::move(L));
				};
				addLight(tokens.size() >= 2 ? tokens[1] : std::string("*"));
				for(const auto& fbState : fallbackStack) addLight(fbState.Palette);
				if(tokens.size() < 3 && frontBlockStack.empty()) continue;
			}

			// <FrontOf3D>{ ... } : la tile (hash, palette) reste devant le relief / panneaux / modeles / effets.
			// Dans le bloc, "<tile>HASH" ou "<tile>HASH,palette" seul suffit (pas d'image : rien d'autre n'est defini).
			if(!frontBlockStack.empty() && !tokens.empty()) {
				auto addFront = [&](const std::string& palTok) {
					auto rule = std::make_unique<SnesHdNoLightRule>();
					try { rule->Hash = std::stoull(tokens[0], nullptr, 16); } catch(...) { return; }
					std::string p = StringUtilities::ToLower(StringUtilities::Trim(palTok));
					if(!(p.empty() || p == "*" || p == "all")) {
						try { rule->Palette = (uint32_t)std::stoul(p); rule->AnyPalette = false; } catch(...) {}
					}
					const std::string key = std::to_string(frontBlockId.back()) + "|" + StringUtilities::ToLower(tokens[0]) + "|" + (rule->AnyPalette ? std::string("*") : std::to_string(rule->Palette));
					if(!frontBlockSeen.insert(key).second) return;
					frontBlockCount.back()++;
					rule->Conditions = frontBlockStack.back();
					outData->FrontOf3DRules.push_back(std::move(rule));
				};
				addFront(tokens.size() >= 2 ? tokens[1] : std::string("*"));
				for(const auto& fbState : fallbackStack) addFront(fbState.Palette);
				if(tokens.size() < 3) continue; // simple reference : pas de tile a inscrire
			}

			// 1. Inscription de la tuile originale (ex: Palette 14 / Green_turtle.png)
			processTileTokens(tokens, reqHFlip, reqVFlip, currentConditions, lineLocation);

			// 2. Inscription récursive pour chaque bloc <fallback> actif dans la pile
			for(const auto& fbState : fallbackStack) {
				std::vector<std::string> dupTokens = tokens;
				if(dupTokens.size() >= 3) {
					// On écrase la palette et l'index de l'image de la tuile originelle 
					// avec ceux paramétrés dans le tag de bloc <fallback>
					dupTokens[1] = fbState.Palette;
					dupTokens[2] = fbState.ImageParam;

					// Et on rappelle la fonction (avec les mêmes conditions de ligne)
					processTileTokens(dupTokens, reqHFlip, reqVFlip, currentConditions, lineLocation);
				}
			}

			continue; // On passe à la ligne suivante puisque les tuiles sont gérées
		}
		//-----------------------------------------------------------------------
		/// PARSING  <CONDITION> TAGS
		else if(lineContent.substr(0, 11) == "<condition>") {
			std::string data = lineContent.substr(11);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				std::string baseConditionName = tokens[0]; // On garde le nom original
				std::string conditionType = tokens[1];

				// Managing NOT Condition ( ! ) : On fait 2 tours ! Un normal (i=0), et un inversé (i=1)
				for(int inverted = 0; inverted < 2; inverted++) {

					// Au deuxième tour, on ajoute un "!" devant le nom
					std::string conditionName = (inverted == 1) ? "!" + baseConditionName : baseConditionName;

					std::unique_ptr<SnesHdPackCondition> condition;

					// ==========================================================
					// 1. CONDITIONS : MEMORY CHECKS (VRAM, OAM, CGRAM, ARAM, CPU)
					// ==========================================================
					if(conditionType.find("memoryCheck") != std::string::npos || conditionType.find("MemoryCheck") != std::string::npos) {

						// A. Détection du type de comparaison (Constante vs Registre)
						bool isConstant = (conditionType.find("Constant") != std::string::npos);

						// B. Détection de l'Espace Mémoire ciblé
						SnesMemorySpace space = SnesMemorySpace::CPU; // Par défaut
						if(conditionType.find("vram") == 0 || conditionType.find("ppu") == 0) space = SnesMemorySpace::VRAM;
						else if(conditionType.find("oam") == 0)   space = SnesMemorySpace::OAM;
						else if(conditionType.find("cgram") == 0) space = SnesMemorySpace::CGRAM;
						else if(conditionType.find("aram") == 0 || conditionType.find("spc") == 0) space = SnesMemorySpace::ARAM;

						// C. Extraction des paramètres communs
						uint32_t operandA = HexUtilities::FromHex(tokens[2]);
						HdPackConditionOperator op = ParseConditionOperator(tokens[3]);

						uint32_t mask = 0xFF;
						if(tokens.size() > 5) {
							mask = HexUtilities::FromHex(tokens[5]);
						}

						bool In_operator = (op == HdPackConditionOperator::In);

						// D. Instanciation de la bonne classe
						if(isConstant) {
							condition.reset(new SnesHdPackMemoryCheckConstantCondition());
							SnesHdPackMemoryCheckConstantCondition* condPtr = (SnesHdPackMemoryCheckConstantCondition*)condition.get();

							if(In_operator) {
								condPtr->InitializeForInOperator(operandA, op, tokens[4], (uint8_t)mask, space);
							} else {
								uint32_t operandB = HexUtilities::FromHex(tokens[4]);
								condPtr->Initialize(operandA, op, operandB, (uint8_t)mask, space, true);
							}
						} else {
							condition.reset(new SnesHdPackMemoryCheckCondition());
							SnesHdPackMemoryCheckCondition* condPtr = (SnesHdPackMemoryCheckCondition*)condition.get();

							uint32_t operandB = HexUtilities::FromHex(tokens[4]); // Ici Operand B est une adresse !
							condPtr->Initialize(operandA, op, operandB, (uint8_t)mask, space, false);
						}

						// E. Finalisation et Enregistrement
						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							// 1. Initialisation (attache ScreenInfo)
							condPtr->Initialize(&outData->ScreenInfo, outData.get());

							// 2. Ajout de l'adresse A à surveiller (AVEC le marqueur spatial !)
							uint32_t addrToWatchA = operandA | (uint32_t)space;
							outData->WatchedMemoryAddresses.emplace(addrToWatchA);

							// 3. Si c'est un check entre 2 registres, il faut aussi surveiller l'adresse B !
							if(!isConstant && !In_operator) {
								uint32_t addrToWatchB = HexUtilities::FromHex(tokens[4]) | (uint32_t)space;
								outData->WatchedMemoryAddresses.emplace(addrToWatchB);
							}

							// 4. Enregistrement dans le dictionnaire
							conditionsByName[conditionName] = condPtr;

							// 5. Déplacement final
							outData->Conditions.emplace_back(std::move(condition));

						} else {
							MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - Unknown or invalid condition type : " + conditionType);
						}
					}
					// ==========================================================
					// 2. CONDITION : FRAME RANGE 
					// ==========================================================
					else if(conditionType == "frameRange") {
						condition.reset(new SnesHdPackFrameRangeCondition());
						SnesHdPackFrameRangeCondition* frameCond = (SnesHdPackFrameRangeCondition*)condition.get();

						uint32_t opA = 1;
						uint32_t opB = 0;

						if(tokens.size() > 3) {
							opA = std::stoul(StringUtilities::Trim(tokens[2]));
							opB = std::stoul(StringUtilities::Trim(tokens[3]));
						}

						frameCond->Initialize(opA, opB);

						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							condPtr->Initialize(&outData->ScreenInfo, outData.get());
							conditionsByName[conditionName] = condPtr;
							outData->Conditions.emplace_back(std::move(condition));
						}
					}

					// -------------------------------------------------------------
					// PARSING TILE ET SPRITE NEARBY (Avec support Joker = OnScreen)
					// -------------------------------------------------------------

					else if(conditionType == "tileNearby" || conditionType == "spriteNearby") {

						bool isSprite = (conditionType == "spriteNearby");

						if(isSprite) condition.reset(new SnesHdPackSpriteNearbyCondition());
						else condition.reset(new SnesHdPackTileNearbyCondition());

						bool anyX = false, anyY = false;
						int offX = 0, offY = 0;

						// 1. Parsing du Décalage X avec gestion des Jokers
						std::string strX = StringUtilities::Trim(tokens[2]);
						if(strX == "*" || strX == "all" || strX == "any") {
							anyX = true;
						} else {
							offX = std::stoi(strX);
						}

						// 2. Parsing du Décalage Y avec gestion des Jokers
						std::string strY = StringUtilities::Trim(tokens[3]);
						if(strY == "*" || strY == "all" || strY == "any") {
							anyY = true;
						} else {
							offY = std::stoi(strY);
						}

						// 3. Extraction Hash & Palette (LE TRI INTELLIGENT EST ICI)
						uint64_t memHash = 0;
						std::string hashStr = StringUtilities::Trim(tokens[4]);

						if(hashStr.length() > 6) {
							// C'est un long Hash 64-bits
							memHash = std::stoull(hashStr, nullptr, 16);
						} else {
							// C'est un petit Index
							memHash = HexUtilities::FromHex(hashStr);
						}
						/*
						uint32_t palHash = 0;
						bool ignorePal = (tokens[5] == "ignore");

						if(!ignorePal) {
							palHash = HexUtilities::FromHex(tokens[5]);
						}
						*/
						//fix palette 21/10/2026
						uint32_t palHash = 0;
						bool ignorePal = (StringUtilities::ToLower(StringUtilities::Trim(tokens[5])) == "ignore");

						if(!ignorePal) {
							try {
								// Lecture en décimal standard (0 à 15)
								palHash = std::stoul(StringUtilities::Trim(tokens[5]));
							} catch(...) {
								// Fallback en hexadécimal si l'utilisateur a écrit A, B, C, D, E, F
								palHash = HexUtilities::FromHex(tokens[5]);
							}
						}
						

						// 4. Initialisation selon le type
						if(isSprite) {
							((SnesHdPackSpriteNearbyCondition*)condition.get())->Initialize(offX, offY, anyX, anyY, memHash, palHash, ignorePal);
						} else {
							((SnesHdPackTileNearbyCondition*)condition.get())->Initialize(offX, offY, anyX, anyY, memHash, palHash, ignorePal);
						}

						// 5. Finalisation de la condition
						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							condPtr->Initialize(&outData->ScreenInfo, outData.get());
							conditionsByName[conditionName] = condPtr;
							outData->Conditions.emplace_back(std::move(condition));
						}
					}


					// -------------------------------------------------------------
					// PARSING TILE AT POSITION (Adapté à l'architecture unique_ptr)
					// -------------------------------------------------------------

					else if(conditionType == "tileAtPosition") {
						condition.reset(new SnesHdPackTileAtPositionCondition());
						SnesHdPackTileAtPositionCondition* tileCond = (SnesHdPackTileAtPositionCondition*)condition.get();

						// 1. Parsing de la Colonne (X) avec gestion du Joker '*'
						std::string strX = StringUtilities::Trim(tokens[2]);
						int tCol = 0;
						if(strX == "*" || strX == "all") {
							tileCond->CheckAnyX = true;
						} else {
							tCol = std::stoi(strX);
						}

						// 2. Parsing de la Ligne (Y) avec gestion du Joker '*'
						std::string strY = StringUtilities::Trim(tokens[3]);
						int tRow = 0;
						if(strY == "*" || strY == "all") {
							tileCond->CheckAnyY = true;
						} else {
							tRow = std::stoi(strY);
						}

						// 3. Extraction Hash & Palette

						// --- POUR GÉRER LES HASHS GÉANTS ---
						uint64_t memHash = 0;
						std::string hashStr = StringUtilities::Trim(tokens[4]);

						if(hashStr.length() > 6) {
							// C'est un long Hash, on force la lecture en 64-bits avec stoull !
							memHash = std::stoull(hashStr, nullptr, 16);
						} else {
							// C'est un petit Index (ex: 101), on utilise FromHex
							memHash = HexUtilities::FromHex(hashStr);
						}
						// ------------------------------------------------

						uint32_t palHash = HexUtilities::FromHex(tokens[5]);
						bool ignorePal = (tokens[5] == "ignore");

						// Et on passe les valeurs à ta fonction Initialize (qui est prête !)
						tileCond->Initialize(tCol, tRow, memHash, palHash, ignorePal);

						// 4. Paramètre optionnel "Layers" (tokens[6])
						if(tokens.size() > 6) {
							tileCond->InitializeLayers(tokens[6]);
						} else {
							tileCond->InitializeLayers("all");
						}

						// 5.Paramètre optionnel "Scope" (tokens[7])
						if(tokens.size() > 7) {
							std::string scopeStr = StringUtilities::Trim(tokens[7]);
							if(scopeStr == "layer") {
								tileCond->Scope = SnesHdPackConditionScope::Layer;
							} else {
								tileCond->Scope = SnesHdPackConditionScope::Screen;
							}
						}

						// 6. Finalisation de la condition
						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							// Initialisation vitale (pour attacher le ScreenInfo et le PPU)
							condPtr->Initialize(&outData->ScreenInfo, outData.get());

							// Enregistrement dans le dictionnaire
							conditionsByName[conditionName] = condPtr;

							// Déplacement final
							outData->Conditions.emplace_back(std::move(condition));
						}
					}

					// -------------------------------------------------------------
					// PARSING COLOR WINDOW : nom,colorWindow,HASH|any|full,X|*[,Y|*[,tolerance]]
					// -------------------------------------------------------------
					else if(conditionType == "colorWindow") {
						condition.reset(new SnesHdPackColorWindowCondition());
						SnesHdPackColorWindowCondition* cw = (SnesHdPackColorWindowCondition*)condition.get();
						std::string hashStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[2]));
						if(hashStr == "any") cw->Mode = SnesHdPackColorWindowCondition::MatchMode::Any;
						else if(hashStr == "full") cw->Mode = SnesHdPackColorWindowCondition::MatchMode::Full;
						else cw->Hash = (uint32_t)HexUtilities::FromHex(hashStr);
						auto parsePos = [](const std::string& t, bool& any, int& v) {
							std::string s = StringUtilities::Trim(t);
							if(s == "*" || s == "all" || s == "any" || s.empty()) { any = true; } else { any = false; v = std::stoi(s); }
						};
						parsePos(tokens[3], cw->AnyX, cw->X);
						if(tokens.size() > 4) parsePos(tokens[4], cw->AnyY, cw->Y);
						if(tokens.size() > 5) {
							std::string tol = StringUtilities::Trim(tokens[5]);
							if(!tol.empty() && tol != "*") cw->Tolerance = std::stoi(tol);
						}
						if(tokens.size() > 6) {
							std::string col = StringUtilities::Trim(tokens[6]);
							if(!col.empty() && col != "*" && col != "any") {
								cw->AnyColor = false;
								size_t amp = col.find('&');
								if(amp != std::string::npos) {
									cw->Color = (uint16_t)HexUtilities::FromHex(StringUtilities::Trim(col.substr(0, amp)));
									cw->ColorMask = (uint16_t)(HexUtilities::FromHex(StringUtilities::Trim(col.substr(amp + 1))) & 0x7FFF);
								} else {
									cw->Color = (uint16_t)HexUtilities::FromHex(col);
								}
							}
						}

						condition->Name = conditionName;
						SnesHdPackCondition* condPtr = condition.get();
						condPtr->Initialize(&outData->ScreenInfo, outData.get());
						conditionsByName[conditionName] = condPtr;
						outData->Conditions.emplace_back(std::move(condition));
					}

					// -------------------------------------------------------------
					// PARSING SPRITE ATPOSITION 
					// -------------------------------------------------------------
					else if(conditionType == "spriteAtPosition") {
						condition.reset(new SnesHdPackSpriteAtPositionCondition());
						SnesHdPackSpriteAtPositionCondition* spriteCond = (SnesHdPackSpriteAtPositionCondition*)condition.get();

						// 1. Parsing du X avec gestion du Joker '*'
						std::string strX = StringUtilities::Trim(tokens[2]);
						int tX = 0;
						if(strX == "*" || strX == "all") {
							spriteCond->CheckAnyX = true;
						} else {
							tX = std::stoi(strX) * 8;
						}

						// 2. Parsing du Y avec gestion du Joker '*'
						std::string strY = StringUtilities::Trim(tokens[3]);
						int tY = 0;
						if(strY == "*" || strY == "all") {
							spriteCond->CheckAnyY = true;
						} else {
							tY = std::stoi(strY) * 8;
						}

						// --- LE TRI INTELLIGENT DU PARSEUR (Index ou Hash ?) ---
						uint64_t memHash = 0;
						std::string hashStr = StringUtilities::Trim(tokens[4]);

						if(hashStr.length() > 6) {
							// C'est un long Hash 64-bits
							memHash = std::stoull(hashStr, nullptr, 16);
						} else {
							// C'est un petit Index
							memHash = HexUtilities::FromHex(hashStr);
						}
						// -------------------------------------------------------

						uint32_t palHash = HexUtilities::FromHex(tokens[5]);
						bool ignorePal = (tokens[5] == "ignore");

						// 3. Nouveau paramètre optionnel 'consider_Visible' (défaut = false)
						bool considerVisible = false;
						if(tokens.size() > 6) {
							std::string visStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[6]));
							if(visStr == "true" || visStr == "1" || visStr == "yes") {
								considerVisible = true;
							}
						}

						// On remplit notre structure
						spriteCond->InitializeSpriteCond(tX, tY, spriteCond->CheckAnyX, spriteCond->CheckAnyY, memHash, palHash, ignorePal, considerVisible);

						if(condition) {
							condition->Name = conditionName;
							SnesHdPackCondition* condPtr = condition.get();

							condPtr->Initialize(&outData->ScreenInfo, outData.get());
							conditionsByName[conditionName] = condPtr;
							outData->Conditions.emplace_back(std::move(condition));
						}
						}
				


				}
			}
			// end parsing CONDITIONS TAGS
		}


		// =============================================================
		/// PARSING <addition> TAG
		else if(lineContent.substr(0, 10) == "<addition>") {
			std::string data = lineContent.substr(10);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 6) {
				SnesHdPackAddition add;
				try {
					// hash for pivot tile
					add.BaseMemoryHash = std::stoull(StringUtilities::Trim(tokens[0]), nullptr, 16);
					add.BasePaletteHash = std::stoul(StringUtilities::Trim(tokens[1]));

					// Hash for additional tile (must be  Hexa in hires file txt !)
					add.AddedMemoryHash = std::stoull(StringUtilities::Trim(tokens[2]), nullptr, 16);
					add.AddedPaletteHash = std::stoul(StringUtilities::Trim(tokens[3]));

					// Offsets
					add.OffsetX = std::stoi(StringUtilities::Trim(tokens[4]));
					add.OffsetY = std::stoi(StringUtilities::Trim(tokens[5]));

					// Ignorer la palette (Optionnel, 7ème paramètre)
					if(tokens.size() >= 7) {
						std::string opt = StringUtilities::ToLower(StringUtilities::Trim(tokens[6]));
						if(opt == "1" || opt == "true" || opt == "ignore") {
							add.IgnoreBasePalette = true;
						}
					}

					// On attache les conditions éventuelles (ex: [memoryCheck...])
					add.Conditions = currentConditions;

					// On l'enregistre dans le dictionnaire de SnesHdPackData 
					outData->Additions[add.BaseMemoryHash].push_back(add);

					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <addition> registered.");
				} catch(const std::exception& e) {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <addition> Parsing Error : Check your Hash format.");
				}
			}
		}// end parsing <addition> TAG

		// =============================================================
		// PARSING BACKGROUND TAG ==============
		/// PARSING <m7relief> : volume d'une tile Mode 7
		// [conditions]<m7relief>relief.png|full,HASH,hauteur[,x,y][,size=32][,sides=0.7][,mode=extrude|height][,color=tile|png]
		else if(lineContent.substr(0, 10) == "<m7relief>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(10), ',');
			if(tokens.size() < 3) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7relief> : il faut au moins png|full,HASH,hauteur.");
			} else {
				auto rl = std::make_unique<SnesHdM7ReliefInfo>();
				rl->Conditions = currentConditions;
				rl->Size = 8 * std::max<uint32_t>(1, outData->Scale);
				try { rl->TileHash = (uint32_t)HexUtilities::FromHex(StringUtilities::Trim(tokens[1])); } catch(...) {}
				try { rl->Height = std::stof(StringUtilities::Trim(tokens[2])); } catch(...) {}
				std::vector<std::string> positional;
				for(size_t t = 3; t < tokens.size(); t++) {
					std::string tok = StringUtilities::Trim(tokens[t]);
					size_t eq = tok.find('=');
					if(eq == std::string::npos) { positional.push_back(tok); continue; }
					std::string k = StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)));
					std::string v = StringUtilities::ToLower(StringUtilities::Trim(tok.substr(eq + 1)));
					try {
						if(k == "size") rl->Size = std::stoi(v);
						else if(k == "sides") rl->Sides = std::clamp(std::stof(v), 0.0f, 2.0f);
						else if(k == "mode") rl->HeightMap = (v == "height");
						else if(k == "color") rl->ColorFromPng = (v == "png");
						else if(k == "x") rl->X = std::stoi(v);
						else if(k == "y") rl->Y = std::stoi(v);
					} catch(...) {}
				}
				if(positional.size() >= 2) {
					try { rl->X = std::stoi(positional[0]); rl->Y = std::stoi(positional[1]); } catch(...) {}
				}
				std::string png = StringUtilities::Trim(tokens[0]);
				if(StringUtilities::ToLower(png) != "full") {
					std::string filename = NormalizePath(png);
					auto itBmp = loadedBitmaps.find(filename);
					if(itBmp != loadedBitmaps.end()) {
						rl->Bitmap = itBmp->second;
					} else {
						SnesHdPackBitmapInfo* found = nullptr;
						for(auto& bmp : outData->ImageFileData) { if(bmp->PngName == filename) { found = bmp.get(); break; } }
						if(!found) {
							auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
							bitmap->PngName = filename;
							VirtualFile pngFile(FolderUtilities::CombinePath(rootFolder, filename));
							if(pngFile.IsValid()) {
								pngFile.ReadFile(bitmap->FileData);
							} else {
								MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7relief> : PNG introuvable : " + filename);
							}
							found = bitmap.get();
							outData->ImageFileData.push_back(std::move(bitmap));
						}
						loadedBitmaps[filename] = found;
						rl->Bitmap = found;
					}
				}
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7relief> tile " + HexUtilities::ToHex32(rl->TileHash) +
					" hauteur " + std::to_string(rl->Height) + (rl->Bitmap ? std::string(" (") + png + ")" : std::string(" (tile entiere)")));
				outData->M7Reliefs[rl->TileHash].push_back(std::move(rl));
			}
		}

		/// PARSING <m7billboard> : panneau plante sur la carte Mode 7
		// [conditions]<m7billboard>image.png,HASH_CARTE|*,x,y[,height=48][,elevation=0][,alpha=1]
		// [conditions]<m7billboard>image.png,tile=HASH_TILE[,height=48][,elevation=0][,alpha=1]
		else if(lineContent.substr(0, 13) == "<m7billboard>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(13), ',');
			if(tokens.size() < 2) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7billboard> : il faut image.png,HASH_CARTE|*,x,y ou image.png,tile=HASH.");
			} else {
				auto bb = std::make_unique<SnesHdM7BillboardInfo>();
				bb->Conditions = currentConditions;
				std::vector<std::string> positional;
				for(size_t t = 1; t < tokens.size(); t++) {
					std::string tok = StringUtilities::Trim(tokens[t]);
					size_t eq = tok.find('=');
					if(eq == std::string::npos) { positional.push_back(tok); continue; }
					std::string k = StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)));
					std::string v = StringUtilities::Trim(tok.substr(eq + 1));
					try {
						if(k == "tile") { bb->OnTile = true; bb->TileHash = (uint32_t)HexUtilities::FromHex(v); }
						else if(k == "map") SnesHdParseMaps(v, bb->AnyMap, bb->MapHash, bb->MapHashes);
						else if(k == "height" || k == "hauteur") bb->Height = std::stof(v);
						else if(k == "elevation") bb->Elevation = std::stof(v);
						else if(k == "alpha") bb->Alpha = std::clamp(std::stof(v), 0.0f, 1.0f);
						else if(k == "x") bb->X = std::stof(v);
						else if(k == "y") bb->Y = std::stof(v);
					} catch(...) {}
				}
				size_t pi = 0;
				if(!bb->OnTile && pi < positional.size()) {
					SnesHdParseMaps(positional[pi++], bb->AnyMap, bb->MapHash, bb->MapHashes);
				}
				if(pi + 1 < positional.size()) {
					try { bb->X = std::stof(positional[pi]); bb->Y = std::stof(positional[pi + 1]); } catch(...) {}
				}
				std::string filename = NormalizePath(StringUtilities::Trim(tokens[0]));
				auto itBmp = loadedBitmaps.find(filename);
				if(itBmp != loadedBitmaps.end()) {
					bb->Bitmap = itBmp->second;
				} else {
					SnesHdPackBitmapInfo* found = nullptr;
					for(auto& bmp : outData->ImageFileData) { if(bmp->PngName == filename) { found = bmp.get(); break; } }
					if(!found) {
						auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
						bitmap->PngName = filename;
						VirtualFile pngFile(FolderUtilities::CombinePath(rootFolder, filename));
						if(pngFile.IsValid()) {
							pngFile.ReadFile(bitmap->FileData);
						} else {
							MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7billboard> : PNG introuvable : " + filename);
						}
						found = bitmap.get();
						outData->ImageFileData.push_back(std::move(bitmap));
					}
					loadedBitmaps[filename] = found;
					bb->Bitmap = found;
				}
				std::string where = bb->OnTile ? ("sur la tile " + HexUtilities::ToHex32(bb->TileHash)) :
					("en (" + std::to_string((int)bb->X) + ", " + std::to_string((int)bb->Y) + ") carte " + (bb->AnyMap ? std::string("*") : HexUtilities::ToHex32(bb->MapHash)));
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7billboard> " + filename + " " + where);
				outData->M7Billboards.push_back(std::move(bb));
			}
		}

		/// PARSING <m7Effect> : effets ancres sur la carte Mode 7 (etape 1 : flame, smoke)
		// [conditions]<m7Effect>HASH_CARTE|*,type=flame|smoke,x,y[,elevation=0][,params=effet.json][,cle=valeur...]
		// [conditions]<m7Effect>HASH_CARTE|*,type=flame,tile=HASH_TILE[,...]
		// flame : une <m7light> qui vacille est ajoutee au meme endroit (light=0 pour la retirer)
		else if(StringUtilities::ToLower(lineContent.substr(0, 10)) == "<m7effect>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(10), ',');
			std::vector<std::string> positional;
			std::vector<std::pair<std::string, std::string>> kvs;
			for(auto& t0 : tokens) {
				std::string tok = StringUtilities::Trim(t0);
				if(tok.empty()) continue;
				size_t eq = tok.find('=');
				if(eq == std::string::npos) positional.push_back(tok);
				else kvs.push_back({ StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq))), StringUtilities::Trim(tok.substr(eq + 1)) });
			}
			for(auto& kv : kvs) {
				if(kv.first != "params") continue;
				VirtualFile jf(FolderUtilities::CombinePath(rootFolder, NormalizePath(kv.second)));
				std::vector<uint8_t> rawJ;
				if(jf.IsValid() && jf.ReadFile(rawJ)) {
					auto cfg = ParseFlatJson(std::string(rawJ.begin(), rawJ.end()));
					std::vector<std::pair<std::string, std::string>> fromJson(cfg.begin(), cfg.end());
					kvs.insert(kvs.begin(), fromJson.begin(), fromJson.end());
				} else {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7Effect> : fichier de parametres introuvable : " + kv.second);
				}
				break;
			}
			auto fx = std::make_unique<SnesHdM7EffectInfo>();
			fx->Conditions = currentConditions;
			// type d'abord (il fixe les valeurs par defaut)
			std::string type = "flame";
			for(auto& kv : kvs) if(kv.first == "type") type = StringUtilities::ToLower(kv.second);
			if(type == "smoke" || type == "fumee") {
				fx->Type = 1; fx->MaxParticles = 60; fx->Rate = 10; fx->LifeMin = 2.5f; fx->LifeMax = 4.0f; fx->Radius = 2;
				fx->RiseMin = 8; fx->RiseMax = 14; fx->Drift = 3; fx->Turbulence = 4; fx->TurbulenceFreq = 0.6f; fx->WindX = 6;
				fx->SizeStart = 3; fx->SizeEnd = 10; fx->SizeRandom = 0.3f; fx->AlphaStart = 0.55f; fx->AlphaEnd = 0; fx->FadeIn = 0.15f;
				fx->Color0 = 0x9A9A9A; fx->Color1 = 0x7A7A7A; fx->Color2 = 0x5A5A5A; fx->Additive = false; fx->NearFade = 16;
			} else if(type == "fog" || type == "brouillard") {
				fx->Type = 2; fx->Color0 = fx->Color1 = fx->Color2 = 0xC8D0D8; fx->WindX = 6; fx->WindY = 2;
			} else if(type == "magicfog" || type == "brumemagique") {
				fx->Type = 3; fx->Color0 = 0x60FFB0; fx->Color1 = 0xB060FF; fx->Patches = 1.0f; fx->NoiseScale = 64; fx->WindX = 4; fx->WindY = -3;
			} else if(type == "flare") {
				fx->Type = 4; fx->Color0 = 0xFFF0C0; fx->Intensity = 1.0f;
			} else if(type == "clouds" || type == "nuages") {
				fx->Type = 5; fx->Color0 = 0xFFFFFF; fx->NoiseScale = 160; fx->WindX = 10; fx->WindY = 4; fx->SkyAmount = 1.0f;
			} else if(type != "flame" && type != "flamme") {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7Effect> : type '" + type + "' inconnu (flame, smoke, fog, magicfog, flare, clouds).");
			}
			bool light = (fx->Type == 0);
			float lightRadius = 48, lightIntensity = 1.4f, lightFlicker = 0.45f;
			uint32_t lightColor = 0xFFA040;
			auto col = [](std::string v, uint32_t def) -> uint32_t {
				v = StringUtilities::Trim(v);
				if(!v.empty() && v[0] == '#') v = v.substr(1);
				try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; }
			};
			auto isTrue = [](std::string v) { v = StringUtilities::ToLower(v); return v == "1" || v == "true" || v == "yes" || v == "oui"; };
			std::string texName;
			for(auto& kv : kvs) {
				const std::string& k = kv.first; const std::string& v = kv.second;
				try {
					if(k == "map") SnesHdParseMaps(v, fx->AnyMap, fx->MapHash, fx->MapHashes);
					else if(k == "tile") { fx->OnTile = true; fx->TileHash = (uint32_t)HexUtilities::FromHex(v); }
					else if(k == "x") fx->X = std::stof(v);
					else if(k == "dx") fx->Dx = std::stof(v);
					else if(k == "dy") fx->Dy = std::stof(v);
					else if(k == "y") fx->Y = std::stof(v);
					else if(k == "elevation") fx->Elevation = std::stof(v);
					else if(k == "z" || k == "zindex") fx->ZIndex = std::stoi(v);
					else if(k == "maxparticles") fx->MaxParticles = std::clamp(std::stoi(v), 1, 4000);
					else if(k == "rate") fx->Rate = std::max(0.0f, std::stof(v));
					else if(k == "lifemin") fx->LifeMin = std::stof(v);
					else if(k == "lifemax") fx->LifeMax = std::stof(v);
					else if(k == "radius" || k == "rayon") fx->Radius = std::stof(v);
					else if(k == "risemin") fx->RiseMin = std::stof(v);
					else if(k == "risemax") fx->RiseMax = std::stof(v);
					else if(k == "drift") fx->Drift = std::stof(v);
					else if(k == "turbulence") fx->Turbulence = std::stof(v);
					else if(k == "turbulencefreq") fx->TurbulenceFreq = std::stof(v);
					else if(k == "windx") fx->WindX = std::stof(v);
					else if(k == "windy") fx->WindY = std::stof(v);
					else if(k == "size") { float sz = std::stof(v); float r = fx->SizeEnd / std::max(0.001f, fx->SizeStart); fx->SizeStart = sz; fx->SizeEnd = sz * r; }
					else if(k == "sizestart") fx->SizeStart = std::stof(v);
					else if(k == "sizeend") fx->SizeEnd = std::stof(v);
					else if(k == "sizerandom") fx->SizeRandom = std::clamp(std::stof(v), 0.0f, 0.95f);
					else if(k == "alphastart") fx->AlphaStart = std::stof(v);
					else if(k == "alphaend") fx->AlphaEnd = std::stof(v);
					else if(k == "fadein") fx->FadeIn = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "color0" || k == "colorstart") fx->Color0 = col(v, fx->Color0);
					else if(k == "color1" || k == "colormid") fx->Color1 = col(v, fx->Color1);
					else if(k == "color2" || k == "colorend") fx->Color2 = col(v, fx->Color2);
					else if(k == "color") { uint32_t c = col(v, fx->Color1); fx->Color0 = fx->Color1 = fx->Color2 = c; }
					else if(k == "blend") fx->Additive = StringUtilities::ToLower(v) == "additive" || StringUtilities::ToLower(v) == "add";
					else if(k == "minpixelsize") fx->MinPixelSize = std::max(0.5f, std::stof(v));
					else if(k == "nearfade") fx->NearFade = std::max(0.0f, std::stof(v));
					else if(k == "texture") texName = v;
					else if(k == "light" || k == "lumiere") light = isTrue(v);
					else if(k == "lightradius") lightRadius = std::stof(v);
					else if(k == "lightintensity") lightIntensity = std::stof(v);
					else if(k == "lightflicker") lightFlicker = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "lightcolor") lightColor = col(v, lightColor);
					else if(k == "density" || k == "densite") fx->Density = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "start" || k == "debut") fx->FogStart = std::stof(v);
					else if(k == "end" || k == "fin") fx->FogEnd = std::stof(v);
					else if(k == "patches" || k == "nappes") fx->Patches = std::clamp(std::stof(v), 0.0f, 2.0f);
					else if(k == "scale" || k == "noisescale") fx->NoiseScale = std::max(4.0f, std::stof(v));
					else if(k == "region" || k == "zone") fx->Region = std::max(0.0f, std::stof(v));
					else if(k == "sky" || k == "ciel") fx->SkyAmount = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "intensity" || k == "intensite") fx->Intensity = std::max(0.0f, std::stof(v));
					else if(k == "pulse") fx->Pulse = std::stof(v);
					else if(k == "coverage" || k == "couverture") fx->Coverage = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "shadow" || k == "ombre") fx->Shadow = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "skyheight") fx->SkyHeight = std::clamp(std::stof(v), 0.05f, 1.0f);
					else if(k == "sun" || k == "soleil") fx->Sun = isTrue(v);
					else if(k == "sunaz" || k == "azimut") fx->SunAz = std::stof(v);
					else if(k == "sunel" || k == "hauteursoleil") fx->SunEl = std::stof(v);
					else if(k == "flaresize" || k == "halo") fx->FlareSize = std::max(1.0f, std::stof(v));
					else if(k == "ghosts" || k == "reflets") fx->Ghosts = std::max(0.0f, std::stof(v));
					else if(k == "streak" || k == "trainee") fx->Streak = std::max(0.0f, std::stof(v));
					else if(k == "color2nd") fx->Color1 = col(v, fx->Color1);
					else if(SnesHdParseMotionKey(fx->Motion, k, v)) fx->HasMotion = true;
				} catch(...) {}
			}
			fx->LifeMax = std::max(fx->LifeMin, fx->LifeMax);
			if(fx->HasMotion) fx->Motion.BuildPath();
			if(fx->Type >= 2) light = false; // seule la flamme a une lumiere automatique
			size_t pi = 0;
			if(pi < positional.size()) SnesHdParseMaps(positional[pi++], fx->AnyMap, fx->MapHash, fx->MapHashes);
			if(!fx->OnTile && pi + 1 < positional.size()) {
				try { fx->X = std::stof(positional[pi]); fx->Y = std::stof(positional[pi + 1]); } catch(...) {}
			}
			if(!texName.empty()) {
				std::string filename = NormalizePath(texName);
				auto itBmp = loadedBitmaps.find(filename);
				if(itBmp != loadedBitmaps.end()) fx->Texture = itBmp->second;
				else {
					auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
					bitmap->PngName = filename;
					VirtualFile pngFile(FolderUtilities::CombinePath(rootFolder, filename));
					if(pngFile.IsValid()) pngFile.ReadFile(bitmap->FileData);
					else MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7Effect> : texture introuvable : " + filename);
					fx->Texture = bitmap.get();
					loadedBitmaps[filename] = bitmap.get();
					outData->ImageFileData.push_back(std::move(bitmap));
				}
			}
			if(light) {
				// lumiere qui vacille au meme endroit (memes cartes, meme ancrage, memes conditions)
				auto lt = std::make_unique<SnesHdM7LightInfo>();
				lt->AnyMap = fx->AnyMap; lt->MapHash = fx->MapHash; lt->MapHashes = fx->MapHashes;
				lt->OnTile = fx->OnTile; lt->TileHash = fx->TileHash; lt->X = fx->X; lt->Y = fx->Y; lt->Dx = fx->Dx; lt->Dy = fx->Dy;
				lt->Height = fx->Elevation + fx->SizeStart * 1.5f; lt->Radius = lightRadius; lt->Intensity = lightIntensity;
				lt->Color = lightColor; lt->Flicker = lightFlicker; lt->FlickerSpeed = 9.0f;
				lt->Conditions = currentConditions;
				lt->Follow = fx.get();
				outData->M7Lights.push_back(std::move(lt));
			}
			static const char* kFxNames[6] = { "flame", "smoke", "fog", "magicfog", "flare", "clouds" };
			MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7Effect> " + std::string(kFxNames[std::clamp(fx->Type, 0, 5)]) + " " +
				(fx->OnTile ? ("sur la tile " + HexUtilities::ToHex32(fx->TileHash)) : ("en (" + std::to_string((int)fx->X) + ", " + std::to_string((int)fx->Y) + ")")) +
				(light ? std::string(" + lumiere") : std::string()));
			outData->M7Effects.push_back(std::move(fx));
		}

		/// PARSING <m7LiquidBackground> : liquide sous les pixels transparents de la texture Mode 7
		// [conditions]<m7LiquidBackground>HASH_CARTE|*[,style=water|lava][,color=..][,deep=..][,sky=..][,speed=10][,scale=48]
		//                                 [,waves=0.5][,specular=0.8][,reflect=0.65][,foam=0.6][,texture=png][,texscale=32]
		// (couleurs en hexadecimal SANS '#', qui commence un commentaire)
		else if(StringUtilities::ToLower(lineContent.substr(0, 20)) == "<m7liquidbackground>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(20), ',');
			auto lq = std::make_unique<SnesHdM7LiquidInfo>();
			lq->Conditions = currentConditions;
			auto col = [](std::string v, uint32_t def) -> uint32_t {
				v = StringUtilities::Trim(v);
				if(!v.empty() && v[0] == '#') v = v.substr(1);
				try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; }
			};
			bool styleSet = false, colorSet = false, deepSet = false, skySet = false, speedSet = false, objSet = false, rippleSet = false;
			std::string texName;
			for(size_t t = 0; t < tokens.size(); t++) {
				std::string tok = StringUtilities::Trim(tokens[t]);
				if(tok.empty()) continue;
				size_t eq = tok.find('=');
				if(eq == std::string::npos) {
					if(t == 0) {
						SnesHdParseMaps(tok, lq->AnyMap, lq->MapHash, lq->MapHashes);
					}
					continue;
				}
				std::string k = StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)));
				std::string v = StringUtilities::Trim(tok.substr(eq + 1));
				try {
					if(k == "style") { lq->Lava = StringUtilities::ToLower(v) == "lava" || StringUtilities::ToLower(v) == "lave"; styleSet = true; }
					else if(k == "color" || k == "couleur") { lq->Color = col(v, lq->Color); colorSet = true; }
					else if(k == "deep" || k == "profond") { lq->Deep = col(v, lq->Deep); deepSet = true; }
					else if(k == "sky" || k == "ciel" || k == "hot" || k == "chaud") { lq->Sky = col(v, lq->Sky); skySet = true; }
					else if(k == "speed" || k == "vitesse") { lq->Speed = std::stof(v); speedSet = true; }
					else if(k == "scale" || k == "echelle") lq->Scale = std::max(4.0f, std::stof(v));
					else if(k == "waves" || k == "vagues") lq->Waves = std::clamp(std::stof(v), 0.0f, 2.0f);
					else if(k == "specular") lq->Specular = std::clamp(std::stof(v), 0.0f, 4.0f);
					else if(k == "reflect" || k == "reflet") lq->Reflect = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "foam" || k == "ecume") lq->Foam = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "objreflect" || k == "reflets") { lq->ObjReflect = std::clamp(std::stof(v), 0.0f, 1.0f); objSet = true; }
					else if(k == "reflectfade" || k == "longueurreflet") lq->ReflectFade = std::max(1.0f, std::stof(v));
					else if(k == "reflectviews" || k == "vuesreflet") {
						// all | top | bottom | 1 | 2 | 1+2 (vue 1 = haut de l'ecran, vue 2 = bas)
						const std::string m = StringUtilities::ToLower(v);
						lq->ReflectViews = 0;
						if(m == "all" || m == "toutes" || m.empty()) lq->ReflectViews = 0;
						else {
							if(m.find("top") != std::string::npos || m.find("haut") != std::string::npos || m.find('1') != std::string::npos) lq->ReflectViews |= 1;
							if(m.find("bottom") != std::string::npos || m.find("bas") != std::string::npos || m.find('2') != std::string::npos) lq->ReflectViews |= 2;
							if(lq->ReflectViews == 0) lq->ReflectViews = 4; // valeur inconnue : aucune vue (voir le journal)
						}
					}
					else if(k == "ripple" || k == "ondulation") { lq->Ripple = std::clamp(std::stof(v), 0.0f, 8.0f); rippleSet = true; }
					else if(k == "texture") texName = v;
					else if(k == "texscale") lq->TexScale = std::max(1.0f, std::stof(v));
					else if(k == "map") SnesHdParseMaps(v, lq->AnyMap, lq->MapHash, lq->MapHashes);
				} catch(...) {}
			}
			(void)styleSet;
			if(lq->Lava) { // valeurs par defaut de la lave
				if(!colorSet) lq->Color = 0xE8460C;
				if(!deepSet) lq->Deep = 0x7A1004;
				if(!skySet) lq->Sky = 0xFFD050;
				if(!speedSet) lq->Speed = 3.0f;
				if(!objSet) lq->ObjReflect = 0.15f;  // lave : reflet faible
				if(!rippleSet) lq->Ripple = 0.5f;
			}
			if(!texName.empty()) {
				std::string filename = NormalizePath(texName);
				auto itBmp = loadedBitmaps.find(filename);
				if(itBmp != loadedBitmaps.end()) {
					lq->Texture = itBmp->second;
				} else {
					auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
					bitmap->PngName = filename;
					VirtualFile pngFile(FolderUtilities::CombinePath(rootFolder, filename));
					if(pngFile.IsValid()) pngFile.ReadFile(bitmap->FileData);
					else MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7LiquidBackground> : texture introuvable : " + filename);
					lq->Texture = bitmap.get();
					loadedBitmaps[filename] = bitmap.get();
					outData->ImageFileData.push_back(std::move(bitmap));
				}
			}
			MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7LiquidBackground> " + (lq->Lava ? std::string("lave") : std::string("eau")) +
				" carte " + (lq->AnyMap ? std::string("*") : HexUtilities::ToHex32(lq->MapHash)));
			outData->M7Liquids.push_back(std::move(lq));
		}

		/// PARSING <SpriteLight> : lumiere 3D attachee a une tile de sprite
		// [conditions]<SpriteLight>HASH_TILE[,palette|*][,radius=48][,color=FFA040][,intensity=1.4][,height=auto][,flicker=0][,speed=9][,mode=light|add]
		else if(StringUtilities::ToLower(lineContent.substr(0, 13)) == "<spritelight>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(13), ',');
			auto L = std::make_unique<SnesHdSpriteLightInfo>();
			L->Conditions = currentConditions;
			int pos = 0; bool ok = false;
			for(std::string tok : tokens) {
				tok = StringUtilities::Trim(tok);
				if(tok.empty()) continue;
				size_t eq = tok.find('=');
				if(eq != std::string::npos) { parseSpriteLightKey(*L, tok.substr(0, eq), tok.substr(eq + 1)); continue; }
				if(pos == 0) { try { L->Hash = std::stoull(tok, nullptr, 16); ok = true; } catch(...) {} }
				else if(pos == 1) {
					std::string p = StringUtilities::ToLower(tok);
					if(!(p == "*" || p == "all")) { try { L->Palette = (uint32_t)std::stoul(p); L->AnyPalette = false; } catch(...) {} }
				}
				pos++;
			}
			if(ok) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <SpriteLight> tile " + HexUtilities::ToHex32((uint32_t)L->Hash) +
					", rayon " + std::to_string((int)L->Radius));
				outData->SpriteLights.push_back(std::move(L));
			} else {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <SpriteLight> : hash invalide.");
			}
		}

		/// PARSING <NoLiquidReflect> (ligne seule) : [conditions]<NoLiquidReflect>HASH_TILE[,palette|*]
		else if(size_t nl = isNoReflTag(lineContent); nl) {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(nl), ',');
			auto rule = std::make_unique<SnesHdNoLightRule>();
			rule->Conditions = currentConditions;
			bool ok = !tokens.empty();
			if(ok) { try { rule->Hash = std::stoull(StringUtilities::Trim(tokens[0]), nullptr, 16); } catch(...) { ok = false; } }
			if(ok && tokens.size() >= 2) {
				std::string p = StringUtilities::ToLower(StringUtilities::Trim(tokens[1]));
				if(!(p.empty() || p == "*" || p == "all")) { try { rule->Palette = (uint32_t)std::stoul(p); rule->AnyPalette = false; } catch(...) {} }
			}
			if(ok) { MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <NoLiquidReflect> tile " + StringUtilities::Trim(tokens[0])); outData->NoReflectRules.push_back(std::move(rule)); }
			else MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <NoLiquidReflect> : hash invalide.");
		}

		/// PARSING <Ignore2DOverlays> : tile HD qui reste NETTE pendant la distorsion <2DOverlay> (HUD qui tremble sinon)
		// [conditions]<Ignore2DOverlays>HASH_TILE[,palette|*]
		else if(StringUtilities::ToLower(lineContent.substr(0, 18)) == "<ignore2doverlays>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(18), ',');
			auto rule = std::make_unique<SnesHdNoLightRule>();
			rule->Conditions = currentConditions;
			std::string h = tokens.empty() ? std::string("") : StringUtilities::ToLower(StringUtilities::Trim(tokens[0]));
			bool ok = true;
			try { rule->Hash = std::stoull(h, nullptr, 16); } catch(...) { ok = false; }
			if(tokens.size() >= 2) {
				std::string p = StringUtilities::ToLower(StringUtilities::Trim(tokens[1]));
				if(!(p.empty() || p == "*" || p == "all")) {
					try { rule->Palette = (uint32_t)std::stoul(p); rule->AnyPalette = false; } catch(...) {}
				}
			}
			if(ok) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <Ignore2DOverlays> tile " + h + (rule->AnyPalette ? std::string(", toutes palettes") : ", palette " + std::to_string(rule->Palette)));
				outData->Ignore2DOverlaysRules.push_back(std::move(rule));
			} else {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <Ignore2DOverlays> : hash invalide.");
			}
		}

		/// PARSING <FrontOf3D> : tiles / sprites qui restent DEVANT le relief, les panneaux, les modeles 3D et les effets
		// [conditions]<FrontOf3D>hud                     -> tous les sprites hors du sol (HUD, objets dans le ciel)
		// [conditions]<FrontOf3D>*                       -> tous les sprites (karts compris)
		// [conditions]<FrontOf3D>HASH_TILE[,palette|*]   -> une tile HD (sprite ou decor, hash comme <tile>)
		else if(StringUtilities::ToLower(lineContent.substr(0, 11)) == "<frontof3d>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(11), ',');
			auto rule = std::make_unique<SnesHdNoLightRule>();
			rule->Conditions = currentConditions;
			std::string h = tokens.empty() ? std::string("hud") : StringUtilities::ToLower(StringUtilities::Trim(tokens[0]));
			bool ok = true;
			if(h.empty() || h == "hud" || h == "ui") {
				rule->OffGround = true;
			} else if(h == "*" || h == "all") {
				rule->AllSprites = true;
			} else {
				try { rule->Hash = std::stoull(h, nullptr, 16); } catch(...) { ok = false; }
				if(tokens.size() >= 2) {
					std::string p = StringUtilities::ToLower(StringUtilities::Trim(tokens[1]));
					if(!(p.empty() || p == "*" || p == "all")) {
						try { rule->Palette = (uint32_t)std::stoul(p); rule->AnyPalette = false; } catch(...) {}
					}
				}
			}
			if(ok) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <FrontOf3D> " +
					(rule->OffGround ? std::string("sprites hors du sol (HUD)") : rule->AllSprites ? std::string("tous les sprites") :
					("tile " + h + (rule->AnyPalette ? std::string(", toutes palettes") : ", palette " + std::to_string(rule->Palette)))));
				outData->FrontOf3DRules.push_back(std::move(rule));
			} else {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <FrontOf3D> : hash invalide.");
			}
		}

		/// PARSING <NoLightFXOnSprite> : sprites exclus des lumieres et de l'ambiance
		// [conditions]<NoLightFXOnSprite>*                     -> tous les sprites
		// [conditions]<NoLightFXOnSprite>HASH_TILE[,palette|*]  -> une tile de sprite (hash comme <tile>), une palette ou toutes
		else if(StringUtilities::ToLower(lineContent.substr(0, 19)) == "<nolightfxonsprite>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(19), ',');
			auto rule = std::make_unique<SnesHdNoLightRule>();
			rule->Conditions = currentConditions;
			std::string h = tokens.empty() ? std::string("*") : StringUtilities::ToLower(StringUtilities::Trim(tokens[0]));
			bool ok = true;
			if(h.empty() || h == "*" || h == "all") {
				rule->AllSprites = true;
			} else {
				try { rule->Hash = std::stoull(h, nullptr, 16); } catch(...) { ok = false; }
				if(tokens.size() >= 2) {
					std::string p = StringUtilities::ToLower(StringUtilities::Trim(tokens[1]));
					if(!(p.empty() || p == "*" || p == "all")) {
						try { rule->Palette = (uint32_t)std::stoul(p); rule->AnyPalette = false; } catch(...) {}
					}
				}
			}
			if(ok) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <NoLightFXOnSprite> " +
					(rule->AllSprites ? std::string("tous les sprites") : ("tile " + h + (rule->AnyPalette ? std::string(", toutes palettes") : ", palette " + std::to_string(rule->Palette)))));
				outData->NoLightRules.push_back(std::move(rule));
			} else {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <NoLightFXOnSprite> : hash invalide.");
			}
		}

		/// PARSING <m7material> : materiaux 3D des pistes (cartes PBR standard, textures du web)
		// [conditions]<m7material>HASH_TILE,cle=valeur...            -> une tile <m7tiles>
		// [conditions]<m7material>img,map=HASH_CARTE|*,cle=valeur... -> toute la grosse texture <m7img>
		// set=dossier/ (reconnu par suffixes) ou albedo= normal= roughness= gloss= metallic= ao= height= emissive= orm=
		// scale= normalconv=gl|dx color=tile|texture|mix mix= strength= aostrength= specular= emissivestrength= rough= metal=
		else if(StringUtilities::ToLower(lineContent.substr(0, 12)) == "<m7material>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(12), ',');
			auto mt = std::make_unique<SnesHdM7MaterialInfo>();
			mt->Conditions = currentConditions;
			std::string fAlbedo, fNormal, fRough, fGloss, fMetal, fAo, fHeight, fEmis, fOrm, setDir;
			int normalConv = -1; // -1 = selon le nom du fichier
			float heightStrength = 2.0f;
			bool targetOk = false;
			for(size_t ti = 0; ti < tokens.size(); ti++) {
				std::string tok = StringUtilities::Trim(tokens[ti]);
				if(tok.empty()) continue;
				size_t eq = tok.find('=');
				if(eq == std::string::npos) {
					std::string low = StringUtilities::ToLower(tok);
					if(low == "img" || low == "image") { mt->OnImg = true; targetOk = true; }
					else if(!targetOk) { try { mt->TileHash = (uint32_t)HexUtilities::FromHex(tok); targetOk = true; } catch(...) {} }
					continue;
				}
				const std::string k = StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)));
				const std::string v = StringUtilities::Trim(tok.substr(eq + 1));
				const std::string lv = StringUtilities::ToLower(v);
				try {
					if(k == "map") { SnesHdParseMaps(v, mt->AnyMap, mt->MapHash, mt->MapHashes); if(!targetOk) { mt->OnImg = true; targetOk = true; } }
					else if(k == "tile") { mt->TileHash = (uint32_t)HexUtilities::FromHex(v); targetOk = true; }
					else if(k == "set" || k == "dossier") setDir = v;
					else if(k == "albedo" || k == "basecolor" || k == "color_map" || k == "diffuse") fAlbedo = v;
					else if(k == "normal") fNormal = v;
					else if(k == "roughness") fRough = v;
					else if(k == "gloss" || k == "glossiness") fGloss = v;
					else if(k == "metallic" || k == "metalness") fMetal = v;
					else if(k == "ao" || k == "occlusion") fAo = v;
					else if(k == "height" || k == "displacement" || k == "bump") fHeight = v;
					else if(k == "emissive" || k == "emission") fEmis = v;
					else if(k == "orm" || k == "arm") fOrm = v;
					else if(k == "scale" || k == "echelle") mt->Scale = std::max(0.0f, std::stof(v));
					else if(k == "normalconv") normalConv = (lv == "dx" || lv == "directx") ? 1 : 0;
					else if(k == "color" || k == "couleur") mt->ColorMode = (lv == "texture" || lv == "materiau") ? 1 : (lv == "mix" || lv == "melange") ? 2 : 0;
					else if(k == "mix") { mt->Mix = std::clamp(std::stof(v), 0.0f, 1.0f); if(mt->ColorMode == 0) mt->ColorMode = 2; }
					else if(k == "strength" || k == "force") mt->Strength = std::clamp(std::stof(v), 0.0f, 5.0f);
					else if(k == "heightstrength") heightStrength = std::max(0.0f, std::stof(v));
					else if(k == "aostrength") mt->AoStrength = std::clamp(std::stof(v), 0.0f, 2.0f);
					else if(k == "specular" || k == "brillance") mt->Specular = std::clamp(std::stof(v), 0.0f, 4.0f);
					else if(k == "emissivestrength") mt->EmissiveStrength = std::clamp(std::stof(v), 0.0f, 8.0f);
					else if(k == "rough" || k == "rugosite") mt->RoughValue = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "metal") mt->MetalValue = std::clamp(std::stof(v), 0.0f, 1.0f);
					else if(k == "debug") mt->Debug = (lv == "normal" || lv == "normals") ? 1 : (lv == "light" || lv == "lumiere" || lv == "sun") ? 2 : 0;
				} catch(...) {}
			}
			// dossier de textures du web : chaque image reconnue par les mots de son nom
			// (ambientCG : _Color _NormalGL _NormalDX _Roughness _Metalness _AmbientOcclusion _Displacement _Emission ;
			//  Poly Haven : _diff _nor_gl _nor_dx _rough _metal _ao _disp _arm ; autres : albedo, basecolor, gloss, height, orm...)
			if(!setDir.empty()) {
				std::vector<std::string> names;
				try {
					for(const auto& e : std::filesystem::directory_iterator(FolderUtilities::CombinePath(rootFolder, NormalizePath(setDir)))) {
						if(!e.is_regular_file()) continue;
						std::string ext = StringUtilities::ToLower(e.path().extension().string());
						if(ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") names.push_back(e.path().filename().string());
					}
				} catch(...) {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7material> : dossier introuvable : " + setDir);
				}
				std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
					auto png = [](const std::string& s) { return StringUtilities::ToLower(s).find(".png") != std::string::npos; };
					if(png(a) != png(b)) return png(a); // PNG en priorite
					return a < b;
				});
				auto words = [](std::string s) {
					s = StringUtilities::ToLower(s);
					std::vector<std::string> w; std::string cur;
					for(char c : s) { if(std::isalnum((unsigned char)c)) cur += c; else { if(!cur.empty()) w.push_back(cur); cur.clear(); } }
					if(!cur.empty()) w.push_back(cur);
					return w;
				};
				std::string setNormGL, setNormDX;
				for(const std::string& n : names) {
					std::vector<std::string> w = words(n);
					auto has = [&](std::initializer_list<const char*> l) { for(auto& x : w) for(const char* y : l) if(x == y) return true; return false; };
					const std::string rel = setDir + (setDir.back() == '/' || setDir.back() == '\\' ? "" : "/") + n;
					if(has({ "preview", "thumb", "sphere", "cube" })) continue;
					if(has({ "normal", "normalgl", "normaldx", "nor", "nrm", "normalmap", "norm" })) {
						const bool dx = has({ "normaldx", "dx", "directx" });
						std::string& slot = dx ? setNormDX : setNormGL;
						if(slot.empty()) slot = rel;
					}
					else if(has({ "roughness", "rough", "rgh" })) { if(fRough.empty()) fRough = rel; }
					else if(has({ "glossiness", "gloss" })) { if(fGloss.empty()) fGloss = rel; }
					else if(has({ "metalness", "metallic", "metal", "mtl" })) { if(fMetal.empty()) fMetal = rel; }
					else if(has({ "arm", "orm" })) { if(fOrm.empty()) fOrm = rel; }
					else if(has({ "ambientocclusion", "occlusion", "ao" })) { if(fAo.empty()) fAo = rel; }
					else if(has({ "displacement", "disp", "height", "bump", "depth" })) { if(fHeight.empty()) fHeight = rel; }
					else if(has({ "emission", "emissive", "emit", "glow" })) { if(fEmis.empty()) fEmis = rel; }
					else if(has({ "basecolor", "albedo", "diffuse", "diff", "color", "col", "colour" })) { if(fAlbedo.empty()) fAlbedo = rel; }
				}
				// normal map du dossier : OpenGL de preference (sauf si normalconv= ou normal= est donne)
				if(fNormal.empty()) {
					if(!setNormGL.empty() && normalConv != 1) { fNormal = setNormGL; if(normalConv < 0) normalConv = 0; }
					else if(!setNormDX.empty()) { fNormal = setNormDX; if(normalConv < 0) normalConv = 1; }
					else if(!setNormGL.empty()) fNormal = setNormGL;
				}
			}
			mt->NormalDX = normalConv == 1;
			// lecture d'une image (PNG, sinon JPG / TGA / BMP par stb_image)
			auto readImg = [&](const std::string& f, std::vector<uint32_t>& px, uint32_t& w, uint32_t& h) -> bool {
				if(f.empty()) return false;
				VirtualFile vf(FolderUtilities::CombinePath(rootFolder, NormalizePath(f)));
				std::vector<uint8_t> raw;
				if(!vf.IsValid() || !vf.ReadFile(raw)) { MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7material> : image introuvable : " + f); return false; }
				if(PNGHelper::ReadPNG(raw, px, w, h)) return true;
				if(SnesHdDecodeImageFallback(raw, px, w, h)) return true;
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7material> : format non lu : " + f);
				return false;
			};
			auto loadMap = [&](const std::string& f, SnesHdMaterialMap& m) {
				std::vector<uint32_t> px; uint32_t w = 0, h = 0;
				if(readImg(f, px, w, h)) m.Set(std::move(px), w, h);
			};
			loadMap(fAlbedo, mt->Albedo); loadMap(fNormal, mt->Normal); loadMap(fMetal, mt->Metal);
			loadMap(fAo, mt->Ao); loadMap(fEmis, mt->Emissive); loadMap(fOrm, mt->Orm);
			if(!fRough.empty()) loadMap(fRough, mt->Rough);
			else if(!fGloss.empty()) { loadMap(fGloss, mt->Rough); mt->GlossInverted = true; }
			// pas de normal map mais une carte de hauteur : normales calculees (Sobel), convention GL
			if(!mt->Normal.Valid() && !fHeight.empty()) {
				std::vector<uint32_t> hp; uint32_t w = 0, h = 0;
				if(readImg(fHeight, hp, w, h)) {
					std::vector<uint32_t> np((size_t)w * h);
					auto H = [&](int x, int y) { x = (x % (int)w + (int)w) % (int)w; y = (y % (int)h + (int)h) % (int)h; return (float)((hp[(size_t)y * w + x] >> 16) & 0xFF) / 255.0f; };
					for(uint32_t y = 0; y < h; y++) for(uint32_t x = 0; x < w; x++) {
						const float dx = (H(x + 1, y - 1) + 2 * H(x + 1, y) + H(x + 1, y + 1)) - (H(x - 1, y - 1) + 2 * H(x - 1, y) + H(x - 1, y + 1));
						const float dy = (H(x - 1, y + 1) + 2 * H(x, y + 1) + H(x + 1, y + 1)) - (H(x - 1, y - 1) + 2 * H(x, y - 1) + H(x + 1, y - 1));
						float nx = -dx * heightStrength, ny = dy * heightStrength, nz = 1.0f; // GL : vert = vers le haut de l'image
						const float l = std::sqrt(nx * nx + ny * ny + nz * nz); nx /= l; ny /= l; nz /= l;
						np[(size_t)y * w + x] = 0xFF000000 | ((uint32_t)((nx * 0.5f + 0.5f) * 255) << 16) | ((uint32_t)((ny * 0.5f + 0.5f) * 255) << 8) | (uint32_t)((nz * 0.5f + 0.5f) * 255);
					}
					mt->Normal.Set(std::move(np), w, h);
					mt->NormalDX = false;
				}
			}
			// regroupement normale + rugosite + AO dans une seule image (1 lecture par pixel au lieu de 3 ou 4)
			{
				const SnesHdMaterialMap* ref = mt->Normal.Valid() ? &mt->Normal : mt->Orm.Valid() ? &mt->Orm : mt->Rough.Valid() ? &mt->Rough : mt->Ao.Valid() ? &mt->Ao : nullptr;
				if(ref) {
					const uint32_t w = ref->W[0], h = ref->H[0];
					std::vector<uint32_t> pk((size_t)w * h);
					auto at = [&](const SnesHdMaterialMap& m, uint32_t x, uint32_t y) -> uint32_t {
						return m.Mips[0][(size_t)std::min(m.H[0] - 1, y * m.H[0] / h) * m.W[0] + std::min(m.W[0] - 1, x * m.W[0] / w)];
					};
					const uint32_t rough0 = (uint32_t)(mt->RoughValue * 255.0f + 0.5f);
					for(uint32_t y = 0; y < h; y++) for(uint32_t x = 0; x < w; x++) {
						uint32_t nr = 128, ng = 128, ro = rough0, ao = 255;
						if(mt->Normal.Valid()) { const uint32_t c = at(mt->Normal, x, y); nr = (c >> 16) & 0xFF; ng = (c >> 8) & 0xFF; }
						if(mt->Orm.Valid()) { const uint32_t c = at(mt->Orm, x, y); ao = (c >> 16) & 0xFF; ro = (c >> 8) & 0xFF; }
						if(mt->Ao.Valid()) ao = (at(mt->Ao, x, y) >> 16) & 0xFF;
						if(mt->Rough.Valid()) { ro = (at(mt->Rough, x, y) >> 16) & 0xFF; if(mt->GlossInverted) ro = 255 - ro; }
						pk[(size_t)y * w + x] = (ao << 24) | (nr << 16) | (ng << 8) | ro;
					}
					// metal d'une carte ORM : garde a part (rare)
					if(mt->Orm.Valid() && !mt->Metal.Valid()) {
						std::vector<uint32_t> mp((size_t)w * h);
						for(uint32_t y = 0; y < h; y++) for(uint32_t x = 0; x < w; x++) { const uint32_t b = at(mt->Orm, x, y) & 0xFF; mp[(size_t)y * w + x] = 0xFF000000 | (b << 16) | (b << 8) | b; }
						mt->Metal.Set(std::move(mp), w, h);
					}
					mt->HasNormal = mt->Normal.Valid();
					mt->Packed.Set(std::move(pk), w, h);
				}
			}
			if(!targetOk) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7material> : cible manquante (hash de tile <m7tiles>, ou img,map=HASH_CARTE).");
			} else {
				std::string got;
				auto add = [&](const SnesHdMaterialMap& m, const char* n) { if(m.Valid()) { got += got.empty() ? "" : ", "; got += n; got += " " + std::to_string(m.W[0]) + "x" + std::to_string(m.H[0]); } };
				add(mt->Albedo, "couleur"); add(mt->Normal, mt->NormalDX ? "normal(DX)" : "normal"); add(mt->Rough, mt->GlossInverted ? "gloss" : "rugosite");
				add(mt->Metal, "metal"); add(mt->Ao, "AO"); add(mt->Orm, "ORM"); add(mt->Emissive, "emissive");
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7material> " + (mt->OnImg ? std::string("grosse texture") : "tile " + HexUtilities::ToHex32(mt->TileHash)) +
					" : " + (got.empty() ? std::string("aucune carte (valeurs rough / metal seulement)") : got));
				outData->M7Materials.push_back(std::move(mt));
			}
		}

		/// PARSING <m7light> : lumiere sur la carte Mode 7
		// [conditions]<m7light>HASH_CARTE|*,x,y[,height=8][,radius=64][,color=#FFC080][,intensity=1][,flicker=0][,speed=6][,mode=light|add]
		// [conditions]<m7light>tile=HASH_TILE[,...]
		// PARSING <m7ambient> : [conditions]<m7ambient>HASH_CARTE|*,luminosite[,#teinte][,sky=1]
		else if(lineContent.substr(0, 9) == "<m7light>" || lineContent.substr(0, 11) == "<m7ambient>") {
			const bool isAmbient = lineContent.substr(0, 11) == "<m7ambient>";
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(isAmbient ? 11 : 9), ',');
			auto parseColor = [](std::string v, uint32_t def) -> uint32_t {
				v = StringUtilities::Trim(v);
				if(!v.empty() && v[0] == '#') v = v.substr(1);
				try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; }
			};
			std::vector<std::string> positional;
			std::vector<std::pair<std::string, std::string>> keys;
			for(auto& t0 : tokens) {
				std::string tok = StringUtilities::Trim(t0);
				if(tok.empty()) continue;
				size_t eq = tok.find('=');
				if(eq == std::string::npos) positional.push_back(tok);
				else keys.push_back({ StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq))), StringUtilities::Trim(tok.substr(eq + 1)) });
			}
			auto parseMap = [&](const std::string& m0, bool& anyMap, uint32_t& mapHash, std::vector<uint32_t>& all) {
				SnesHdParseMaps(m0, anyMap, mapHash, all);
			};
			if(isAmbient) {
				auto am = std::make_unique<SnesHdM7AmbientInfo>();
				am->Conditions = currentConditions;
				size_t pi = 0;
				if(pi < positional.size()) parseMap(positional[pi++], am->AnyMap, am->MapHash, am->MapHashes);
				if(pi < positional.size()) { try { am->Brightness = std::stof(positional[pi++]); } catch(...) {} }
				if(pi < positional.size()) am->Tint = parseColor(positional[pi++], 0xFFFFFF);
				for(auto& kv : keys) {
					try {
						if(kv.first == "sky" || kv.first == "ciel") am->Sky = !(kv.second == "0" || StringUtilities::ToLower(kv.second) == "false");
						else if(kv.first == "brightness" || kv.first == "luminosite") am->Brightness = std::stof(kv.second);
						else if(kv.first == "sunaz" || kv.first == "azimut") { am->SunAz = std::stof(kv.second); am->SunSet = true; }
						else if(kv.first == "sunel" || kv.first == "hauteursoleil") { am->SunEl = std::clamp(std::stof(kv.second), 1.0f, 89.0f); am->SunSet = true; }
						else if(kv.first == "sun" || kv.first == "soleil") { am->SunIntensity = std::clamp(std::stof(kv.second), 0.0f, 3.0f); am->SunSet = true; }
						else if(kv.first == "suncolor" || kv.first == "couleursoleil") { am->SunColor = parseColor(kv.second, am->SunColor); am->SunSet = true; }
						else if(kv.first == "tint" || kv.first == "color") am->Tint = parseColor(kv.second, 0xFFFFFF);
						else if(kv.first == "map") parseMap(kv.second, am->AnyMap, am->MapHash, am->MapHashes);
					} catch(...) {}
				}
				am->Brightness = std::clamp(am->Brightness, 0.0f, 2.0f);
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7ambient> luminosite " + std::to_string(am->Brightness));
				outData->M7Ambients.push_back(std::move(am));
			} else {
				auto lt = std::make_unique<SnesHdM7LightInfo>();
				lt->Conditions = currentConditions;
				for(auto& kv : keys) {
					try {
						const std::string& k = kv.first; const std::string& v = kv.second;
						if(k == "tile") { lt->OnTile = true; lt->TileHash = (uint32_t)HexUtilities::FromHex(v); }
						else if(k == "map") parseMap(v, lt->AnyMap, lt->MapHash, lt->MapHashes);
						else if(k == "height" || k == "hauteur") lt->Height = std::stof(v);
						else if(k == "radius" || k == "rayon") lt->Radius = std::max(1.0f, std::stof(v));
						else if(k == "intensity" || k == "intensite") lt->Intensity = std::max(0.0f, std::stof(v));
						else if(k == "color" || k == "couleur") lt->Color = parseColor(v, lt->Color);
						else if(k == "flicker" || k == "scintillement") lt->Flicker = std::clamp(std::stof(v), 0.0f, 1.0f);
						else if(k == "speed" || k == "flickerspeed" || k == "vitesse") lt->FlickerSpeed = std::stof(v);
						else if(k == "mode") { const std::string m = StringUtilities::ToLower(v); lt->Additive = (m == "add" || m == "additive"); lt->Shadow = (m == "shadow" || m == "sub" || m == "ombre"); }
						else if(k == "x") lt->X = std::stof(v);
						else if(k == "dx") lt->Dx = std::stof(v);
						else if(k == "dy") lt->Dy = std::stof(v);
						else if(k == "y") lt->Y = std::stof(v);
					} catch(...) {}
				}
				size_t pi = 0;
				if(!lt->OnTile && pi < positional.size()) parseMap(positional[pi++], lt->AnyMap, lt->MapHash, lt->MapHashes);
				if(!lt->OnTile && pi + 1 < positional.size()) {
					try { lt->X = std::stof(positional[pi]); lt->Y = std::stof(positional[pi + 1]); } catch(...) {}
				}
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7light> " +
					(lt->OnTile ? ("sur la tile " + HexUtilities::ToHex32(lt->TileHash)) : ("en (" + std::to_string((int)lt->X) + ", " + std::to_string((int)lt->Y) + ")")) +
					", rayon " + std::to_string((int)lt->Radius));
				outData->M7Lights.push_back(std::move(lt));
			}
		}

		/// PARSING <gpu3d>on|off : moteur 3D sur la carte graphique
		else if(lineContent.substr(0, 7) == "<gpu3d>") {
			std::string v = StringUtilities::ToLower(StringUtilities::Trim(lineContent.substr(7)));
			outData->Gpu3D = !(v == "off" || v == "0" || v == "false" || v == "cpu");
			MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <gpu3d> " + (outData->Gpu3D ? std::string("on") : std::string("off (moteur processeur)")));
		}

		/// PARSING <m7model> : modele 3D (.obj, .gltf, .glb) pose sur la carte Mode 7
		// [conditions]<m7model>modele.obj|.glb,HASH_CARTE|*,x,y[,scale=8|height=..][,rotation=0][,elevation=0][,ambient=0.45][,origin=base|obj]
		//                     [,anim=0|nom][,animspeed=1][,motion=trajet.json][,cles de mouvement...]
		// [conditions]<m7model>modele.obj,tile=HASH_TILE[,...]
		// Mouvement (balise ou JSON "motion") : path=x:y:h|x:y:h|..., speed, loop=loop|pingpong|once, orient, smooth,
		//   spin (deg/s), bobamp / bobfreq, swayamp / swayfreq, offset (s)
		else if(lineContent.substr(0, 9) == "<m7model>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(9), ',');
			if(tokens.size() < 2) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> : il faut modele.obj,HASH_CARTE|*,x,y ou modele.obj,tile=HASH.");
			} else {
				auto md = std::make_unique<SnesHdM7ModelInfo>();
				md->Conditions = currentConditions;
				md->File = NormalizePath(StringUtilities::Trim(tokens[0]));
				float wantHeight = 0;
				std::string animName;
				std::vector<std::string> positional;
				std::vector<std::pair<std::string, std::string>> kvs;
				for(size_t t = 1; t < tokens.size(); t++) {
					std::string tok = StringUtilities::Trim(tokens[t]);
					size_t eq = tok.find('=');
					if(eq == std::string::npos) { positional.push_back(tok); continue; }
					kvs.push_back({ StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq))), StringUtilities::Trim(tok.substr(eq + 1)) });
				}
				// fichier de mouvement JSON : ses cles passent avant celles de la balise (qui l'emportent)
				for(auto& kv : kvs) {
					if(kv.first != "motion") continue;
					VirtualFile jf(FolderUtilities::CombinePath(rootFolder, NormalizePath(kv.second)));
					std::vector<uint8_t> rawJ;
					if(jf.IsValid() && jf.ReadFile(rawJ)) {
						auto cfg = ParseFlatJson(std::string(rawJ.begin(), rawJ.end()));
						std::vector<std::pair<std::string, std::string>> fromJson(cfg.begin(), cfg.end());
						kvs.insert(kvs.begin(), fromJson.begin(), fromJson.end());
					} else {
						MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> : fichier de mouvement introuvable : " + kv.second);
					}
					break;
				}
				auto isTrue = [](std::string v) { v = StringUtilities::ToLower(v); return v == "1" || v == "true" || v == "yes" || v == "oui"; };
				for(auto& kv : kvs) {
					const std::string& k = kv.first;
					const std::string& v = kv.second;
					try {
						if(k == "anim" || k == "animation") animName = v;
						else if(k == "animspeed") md->AnimSpeed = std::stof(v);
						else if(k == "spin") md->Spin = std::stof(v);
						else if(k == "bobamp") md->BobAmp = std::stof(v);
						else if(k == "bobfreq") md->BobFreq = std::stof(v);
						else if(k == "swayamp") md->SwayAmp = std::stof(v);
						else if(k == "swayfreq") md->SwayFreq = std::stof(v);
						else if(k == "offset" || k == "timeoffset") md->TimeOffset = std::stof(v);
						else if(k == "speed" || k == "pathspeed") md->PathSpeed = std::stof(v);
						else if(k == "orient") md->PathOrient = isTrue(v);
						else if(k == "smooth") md->PathSmooth = isTrue(v);
						else if(k == "loop") { std::string m = StringUtilities::ToLower(v); md->PathLoop = (m == "pingpong" || m == "allerretour") ? 1 : ((m == "once" || m == "false" || m == "0") ? 2 : 0); }
						else if(k == "path" || k == "trajet") {
							md->Path.clear();
							for(std::string& pt : StringUtilities::Split(v, '|')) {
								std::vector<std::string> c = StringUtilities::Split(pt, ':');
								if(c.size() < 2) continue;
								std::array<float, 3> p = { std::stof(c[0]), std::stof(c[1]), c.size() >= 3 ? std::stof(c[2]) : 0.0f };
								md->Path.push_back(p);
							}
						}
						else if(k == "tile") { md->OnTile = true; md->TileHash = (uint32_t)HexUtilities::FromHex(v); }
						else if(k == "map") SnesHdParseMaps(v, md->AnyMap, md->MapHash, md->MapHashes);
						else if(k == "scale" || k == "echelle") md->Scale = std::stof(v);
						else if(k == "height" || k == "hauteur") wantHeight = std::stof(v);
						else if(k == "rotation") md->Rotation = std::stof(v);
						else if(k == "elevation") md->Elevation = std::stof(v);
						else if(k == "ambient" || k == "light") md->Ambient = std::clamp(std::stof(v), 0.0f, 1.0f);
						else if(k == "origin") md->KeepOrigin = (StringUtilities::ToLower(v) == "obj");
						else if(k == "x") md->X = std::stof(v);
						else if(k == "y") md->Y = std::stof(v);
					} catch(...) {}
				}
				size_t pi = 0;
				if(!md->OnTile && pi < positional.size()) {
					SnesHdParseMaps(positional[pi++], md->AnyMap, md->MapHash, md->MapHashes);
				}
				if(pi + 1 < positional.size()) {
					try { md->X = std::stof(positional[pi]); md->Y = std::stof(positional[pi + 1]); } catch(...) {}
				}
				auto itMesh = loadedModels.find(md->File);
				if(itMesh != loadedModels.end()) md->Mesh = itMesh->second;
				else {
					std::string ext = StringUtilities::ToLower(md->File.size() >= 4 ? md->File.substr(md->File.size() - 4) : std::string());
					if(ext == ".glb" || ext == "gltf") md->Mesh = SnesHdLoadGltfModel(rootFolder, md->File, outData->ImageFileData, loadedBitmaps, lineLocation);
					else md->Mesh = LoadObjModel(rootFolder, md->File, outData, loadedBitmaps, lineLocation);
					loadedModels[md->File] = md->Mesh;
				}
				if(md->Mesh) {
					// animation a jouer : par nom ou par numero (defaut : la premiere)
					if(md->Mesh->Rig) {
						md->Anim = md->Mesh->Rig->Anims.empty() ? -1 : 0;
						if(!animName.empty()) {
							std::string an = StringUtilities::ToLower(animName);
							if(an == "none" || an == "aucune" || an == "-1") md->Anim = -1;
							else {
								int found = -1;
								for(size_t a = 0; a < md->Mesh->Rig->Anims.size(); a++) if(StringUtilities::ToLower(md->Mesh->Rig->Anims[a].Name) == an) found = (int)a;
								if(found < 0) { try { found = std::stoi(animName); } catch(...) {} }
								if(found >= 0 && found < (int)md->Mesh->Rig->Anims.size()) md->Anim = found;
								else MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> : animation '" + animName + "' introuvable.");
							}
						}
					}
					md->BuildPath();
					float h = md->Mesh->MaxY - md->Mesh->MinY;
					if(wantHeight > 0 && h > 1e-6f) md->Scale = wantHeight / h;
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> " + md->File + " : " + std::to_string(md->Mesh->Tris.size()) +
						" triangles, " + std::to_string(md->Mesh->Mats.size() - 1) + " materiau(x), hauteur " + std::to_string((int)(h * md->Scale)) + " texels.");
					outData->M7Models.push_back(std::move(md));
				}
			}
		}

		/// PARSING <m7CameraDebug> : reperes de controle de la camera Mode 7 reconstruite
		else if(lineContent.substr(0, 15) == "<m7CameraDebug>") {
			outData->M7CameraDebug = true;
			outData->M7CameraDebugConditions = currentConditions;
			MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7CameraDebug> actif : reperes de la camera Mode 7 affiches.");
		}

		/// PARSING <3DParticles> : particules dans le monde Mode 7
		// [conditions]<3DParticles>fichier.json[,Zone=all|top|bottom][,Z=95][,Alpha=1][,cleJSON=valeur...]
		else if(lineContent.substr(0, 13) == "<3DParticles>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(13), ',');
			if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty()) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <3DParticles> : fichier JSON manquant.");
			} else {
				auto pi = std::make_unique<SnesHdParticles3DInfo>();
				pi->JsonFile = NormalizePath(StringUtilities::Trim(tokens[0]));
				pi->Conditions = currentConditions;

				std::unordered_map<std::string, std::string> cfg;
				VirtualFile jf(FolderUtilities::CombinePath(rootFolder, pi->JsonFile));
				std::vector<uint8_t> raw;
				if(jf.IsValid() && jf.ReadFile(raw)) {
					cfg = ParseFlatJson(std::string(raw.begin(), raw.end()));
				} else {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <3DParticles> : fichier introuvable : " + pi->JsonFile);
				}
				for(size_t t = 1; t < tokens.size(); t++) {
					std::string tok = StringUtilities::Trim(tokens[t]);
					size_t eq = tok.find('=');
					if(eq == std::string::npos) continue;
					cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
				}
				auto str = [&](const char* k) -> std::string { auto it = cfg.find(k); return it == cfg.end() ? std::string() : it->second; };
				auto num = [&](const char* k, float def) -> float {
					std::string v = str(k);
					if(v.empty()) return def;
					try { return std::stof(v); } catch(...) { return def; }
				};
				auto boolean = [&](const char* k, bool def) -> bool {
					std::string v = StringUtilities::ToLower(str(k));
					if(v.empty()) return def;
					return v == "true" || v == "1" || v == "yes";
				};
				auto color = [&](const char* k, uint32_t def) -> uint32_t {
					std::string v = StringUtilities::Trim(str(k));
					if(v.empty()) return def;
					if(v[0] == '#') v = v.substr(1);
					try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; }
				};

				std::string zone = StringUtilities::ToLower(str("zone"));
				if(zone == "top" || zone == "haut" || zone == "0") pi->Zone = 0;
				else if(zone == "bottom" || zone == "bas" || zone == "1") pi->Zone = 1;
				else pi->Zone = -1;

				pi->ZIndex = (int)num("z", num("zindex", 95.0f));
				pi->Alpha = std::clamp(num("alpha", 1.0f), 0.0f, 1.0f);
				pi->Additive = StringUtilities::ToLower(str("blend")) == "additive";
				pi->MaxParticles = std::clamp((int)num("maxparticles", 400.0f), 1, 20000);
				pi->Rate = std::max(0.0f, num("rate", 0.0f));
				pi->LifeMin = std::max(0.0f, num("lifemin", 0.0f));
				pi->LifeMax = std::max(pi->LifeMin, num("lifemax", pi->LifeMin));
				pi->BoxSize = std::clamp(num("boxsize", 512.0f), 32.0f, 2048.0f);
				pi->BoxHeight = std::clamp(num("boxheight", 128.0f), 4.0f, 2048.0f);
				pi->BoxAhead = std::clamp(num("boxahead", 0.35f), 0.0f, 0.5f);
				pi->FallMin = num("fallmin", 20.0f); pi->FallMax = num("fallmax", 40.0f);
				pi->DriftMin = num("driftmin", 0.0f); pi->DriftMax = num("driftmax", 6.0f);
				pi->Gravity = num("gravity", 0.0f);
				pi->WindX = num("windx", 0.0f); pi->WindY = num("windy", 0.0f);
				pi->Turbulence = num("turbulence", 0.0f); pi->TurbulenceFreq = num("turbulencefreq", 0.5f);
				pi->SizeStart = num("sizestart", 2.0f); pi->SizeEnd = num("sizeend", pi->SizeStart);
				pi->SizeRandom = std::clamp(num("sizerandom", 0.0f), 0.0f, 0.95f);
				pi->MinPixelSize = std::max(0.5f, num("minpixelsize", 1.5f));
				pi->AlphaStart = num("alphastart", 1.0f); pi->AlphaEnd = num("alphaend", pi->AlphaStart);
				pi->FadeIn = std::clamp(num("fadein", 0.1f), 0.0f, 1.0f);
				pi->FadeOut = std::clamp(num("fadeout", 0.2f), 0.0f, 1.0f);
				pi->ColorStart = color("colorstart", 0xFFFFFF); pi->ColorEnd = color("colorend", pi->ColorStart);
				pi->FarFade = std::clamp(num("farfade", 0.3f), 0.0f, 1.0f);
				pi->NearFade = std::max(0.0f, num("nearfade", 24.0f));
				pi->GroundRespawn = StringUtilities::ToLower(str("groundhit")) != "die";
				(void)boolean;

				std::string tex = StringUtilities::Trim(str("texture"));
				if(!tex.empty()) {
					std::string filename = NormalizePath(tex);
					auto itBmp = loadedBitmaps.find(filename);
					if(itBmp != loadedBitmaps.end()) {
						pi->Texture = itBmp->second;
					} else {
						auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
						bitmap->PngName = filename;
						VirtualFile pngFile(FolderUtilities::CombinePath(rootFolder, filename));
						if(pngFile.IsValid()) {
							pngFile.ReadFile(bitmap->FileData);
						} else {
							MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <3DParticles> : texture introuvable : " + filename);
						}
						pi->Texture = bitmap.get();
						loadedBitmaps[filename] = bitmap.get();
						outData->ImageFileData.push_back(std::move(bitmap));
					}
				}

				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <3DParticles> " + pi->JsonFile + " enregistre (Z=" +
					std::to_string(pi->ZIndex) + ", vue " + (pi->Zone < 0 ? std::string("toutes") : std::to_string(pi->Zone)) + ").");
				outData->Particles3D.push_back(std::move(pi));
			}
		}

		/// PARSING <2DOverlay> TAG (etape 1 : particules 2D) --------------------------------
		// [conditions]<2DOverlay>fichier.json[,Z=95][,Layer=2][,SpeedX=1][,SpeedY=1][,Alpha=0.5][,FieldW=1][,FieldH=1]
		else if(lineContent.substr(0, 11) == "<2DOverlay>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(11), ',');
			if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty()) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <2DOverlay> : fichier JSON manquant.");
			} else {
				auto ov = std::make_unique<SnesHdOverlayInfo>();
				ov->JsonFile = NormalizePath(StringUtilities::Trim(tokens[0]));
				ov->Conditions = currentConditions;

				std::unordered_map<std::string, std::string> cfg;
				VirtualFile jf(FolderUtilities::CombinePath(rootFolder, ov->JsonFile));
				std::vector<uint8_t> raw;
				if(jf.IsValid() && jf.ReadFile(raw)) {
					cfg = ParseFlatJson(std::string(raw.begin(), raw.end()));
				} else {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <2DOverlay> : fichier introuvable : " + ov->JsonFile);
				}
				// Les parametres de la balise remplacent ceux du JSON
				for(size_t t = 1; t < tokens.size(); t++) {
					std::string tok = StringUtilities::Trim(tokens[t]);
					size_t eq = tok.find('=');
					if(eq == std::string::npos) continue;
					cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
				}
				auto str = [&](const char* k) -> std::string { auto it = cfg.find(k); return it == cfg.end() ? std::string() : it->second; };
				auto num = [&](const char* k, float def) -> float {
					std::string v = str(k);
					if(v.empty()) return def;
					try { return std::stof(v); } catch(...) { return def; }
				};
				auto boolean = [&](const char* k, bool def) -> bool {
					std::string v = StringUtilities::ToLower(str(k));
					if(v.empty()) return def;
					return v == "true" || v == "1" || v == "yes";
				};
				auto color = [&](const char* k, uint32_t def) -> uint32_t {
					std::string v = StringUtilities::Trim(str(k));
					if(v.empty()) return def;
					if(v[0] == '#') v = v.substr(1);
					try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; }
				};

				std::string type = StringUtilities::ToLower(str("type"));
				ov->IsDistortion = (type == "distortion");
				ov->IsRays = (type == "rays");
				ov->IsPlainScreen = (type == "plainscreen");
				ov->IsEllipse = (type == "ellipse");
				ov->IsNoise = (type == "shadows" || type == "clouds" || type == "fog");
				ov->NoiseKind = (type == "clouds") ? 1 : ((type == "fog") ? 2 : 0);
				if(!type.empty() && type != "particles" && !ov->IsDistortion && !ov->IsRays && !ov->IsPlainScreen && !ov->IsEllipse && !ov->IsNoise) {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <2DOverlay> : type '" + type + "' pas encore gere (particles, distortion, rays, plainscreen, ellipse, shadows, clouds, fog).");
				}
				if(ov->IsDistortion) {
					// distorsion : parametres sinusoidaux (amplitude en px SNES, longueur d'onde en lignes SNES, vitesse en cycles/s, phase en degres)
					ov->DistAmp1 = num("amp1", 0.0f); ov->DistWave1 = std::max(1.0f, num("wave1", 32.0f));
					ov->DistSpeed1 = num("speed1", 0.5f); ov->DistPhase1 = num("phase1", 0.0f);
					ov->DistAmp2 = num("amp2", 0.0f); ov->DistWave2 = std::max(1.0f, num("wave2", 17.0f));
					ov->DistSpeed2 = num("speed2", 0.3f); ov->DistPhase2 = num("phase2", 0.0f);
					ov->ZoneX0 = std::clamp(num("xmin", 0.0f), 0.0f, 1.0f); ov->ZoneX1 = std::clamp(num("xmax", 1.0f), 0.0f, 1.0f);
					ov->ZoneY0 = std::clamp(num("ymin", 0.0f), 0.0f, 1.0f); ov->ZoneY1 = std::clamp(num("ymax", 1.0f), 0.0f, 1.0f);
				}

				if(ov->IsRays) {
					ov->ZoneX0 = std::clamp(num("xmin", 0.0f), 0.0f, 1.0f); ov->ZoneX1 = std::clamp(num("xmax", 1.0f), 0.0f, 1.0f);
					ov->ZoneY0 = std::clamp(num("ymin", 0.0f), 0.0f, 1.0f); ov->ZoneY1 = std::clamp(num("ymax", 1.0f), 0.0f, 1.0f);
					ov->RayX = std::clamp(num("rayx", 0.5f), 0.0f, 1.0f); ov->RayY = std::clamp(num("rayy", 0.0f), 0.0f, 1.0f);
					ov->RayCount = std::clamp((int)num("raycount", 8.0f), 1, 64);
					ov->RayAngle = num("rayangle", 90.0f); ov->RaySpread = std::clamp(num("rayspread", 60.0f), 0.0f, 360.0f);
					ov->RayWidth = std::max(0.1f, num("raywidth", 4.0f)); ov->RayLength = std::max(0.01f, num("raylength", 0.8f));
					ov->RaySpeed = num("rayspeed", 0.2f); ov->RayWobble = num("raywobble", 3.0f);
					ov->RayColor = color("raycolor", 0xFFE8A0);
				}

				auto fxColor = [&](const char* k, uint32_t def) -> uint32_t {
					std::string v = StringUtilities::ToLower(StringUtilities::Trim(str(k)));
					if(v.empty()) return def;
					if(v == "black" || v == "noir") return 0x000000;
					if(v == "white" || v == "blanc") return 0xFFFFFF;
					if(v == "yellow" || v == "jaune") return 0xFFFF00;
					if(v == "green" || v == "vert") return 0x00FF00;
					if(v == "bleue" || v == "bleu" || v == "blue") return 0x0000FF;
					if(v == "red" || v == "rouge") return 0xFF0000;
					return color(k, def);
				};
				if(ov->IsPlainScreen || ov->IsEllipse || ov->IsNoise) {
					// calques : zone (fractions de l'image) et couleur (hexa ou alias black/white/yellow/green/bleue/red)
					ov->ZoneX0 = std::clamp(num("xmin", 0.0f), 0.0f, 1.0f); ov->ZoneX1 = std::clamp(num("xmax", 1.0f), 0.0f, 1.0f);
					ov->ZoneY0 = std::clamp(num("ymin", 0.0f), 0.0f, 1.0f); ov->ZoneY1 = std::clamp(num("ymax", 1.0f), 0.0f, 1.0f);
					const uint32_t defCol = ov->IsEllipse ? (boolean("negative", false) ? 0x000000 : 0xFFFFFF) : (type == "fog" ? 0xA8B8C8 : (type == "clouds" ? 0xFFFFFF : 0x000000));
					ov->FxColor = fxColor("color", defCol);
				}
				if(ov->IsEllipse) {
					ov->FxEllipseX = num("x", 128.0f); ov->FxEllipseY = num("y", 112.0f);
					ov->FxNegative = boolean("negative", false);
					ov->FxRadiusX = std::max(0.5f, num("radiusx", 32.0f)); ov->FxRadiusY = std::max(0.5f, num("radiusy", 16.0f));
				}
				if(ov->IsNoise) {
					ov->FxSize = std::max(4.0f, num("size", 48.0f));
					ov->FxSpeed = num("speed", 8.0f); ov->FxDirection = num("direction", 0.0f);
					ov->FxCoverage = std::clamp(num("coverage", 0.5f), 0.0f, 1.0f);
					ov->FxSeed = (int)num("seed", 1.0f);
				}

				ov->ZIndex = (int)num("z", num("zindex", 95.0f));
				ov->Layer = std::clamp((int)num("layer", 0.0f), 0, 4);
				ov->SpeedX = num("speedx", 1.0f);
				ov->SpeedY = num("speedy", 1.0f);
				ov->Alpha = std::clamp(num("alpha", 1.0f), 0.0f, 1.0f);
				ov->FieldW = std::max(0.25f, num("fieldw", 1.0f));
				ov->FieldH = std::max(0.25f, num("fieldh", 1.0f));

				ov->Additive = StringUtilities::ToLower(str("blend")) == "additive";
				ov->MaxParticles = std::clamp((int)num("maxparticles", 300.0f), 1, 20000);
				ov->Rate = std::max(0.0f, num("rate", 60.0f));
				ov->LifeMin = std::max(0.05f, num("lifemin", 2.0f));
				ov->LifeMax = std::max(ov->LifeMin, num("lifemax", 4.0f));
				ov->SpawnX = num("spawnx", 0.0f); ov->SpawnY = num("spawny", 0.0f);
				ov->SpawnW = num("spawnw", 1.0f); ov->SpawnH = num("spawnh", 1.0f);
				ov->SpeedMin = num("speedmin", 20.0f); ov->SpeedMax = num("speedmax", 40.0f);
				ov->AngleMin = num("anglemin", 80.0f); ov->AngleMax = num("anglemax", 100.0f);
				ov->GravityX = num("gravityx", 0.0f); ov->GravityY = num("gravityy", 0.0f);
				ov->WindX = num("windx", 0.0f); ov->WindY = num("windy", 0.0f);
				ov->Turbulence = num("turbulence", 0.0f); ov->TurbulenceFreq = num("turbulencefreq", 0.5f);
				ov->RotationMin = num("rotationmin", 0.0f); ov->RotationMax = num("rotationmax", 0.0f);
				ov->SpinMin = num("spinmin", 0.0f); ov->SpinMax = num("spinmax", 0.0f);
				ov->SizeStart = num("sizestart", 1.0f); ov->SizeEnd = num("sizeend", ov->SizeStart);
				ov->SizeRandom = std::clamp(num("sizerandom", 0.0f), 0.0f, 0.95f);
				ov->AlphaStart = num("alphastart", 1.0f); ov->AlphaEnd = num("alphaend", ov->AlphaStart);
				ov->FadeIn = std::clamp(num("fadein", 0.1f), 0.0f, 1.0f);
				ov->FadeOut = std::clamp(num("fadeout", 0.2f), 0.0f, 1.0f);
				ov->ColorStart = color("colorstart", 0xFFFFFF); ov->ColorEnd = color("colorend", ov->ColorStart);
				ov->DotSize = std::max(0.25f, num("dotsize", 2.0f));
				ov->Prewarm = boolean("prewarm", true);

				std::string tex = StringUtilities::Trim(str("texture"));
				if(!tex.empty()) {
					std::string filename = NormalizePath(tex);
					auto itBmp = loadedBitmaps.find(filename);
					if(itBmp != loadedBitmaps.end()) {
						ov->Texture = itBmp->second;
					} else {
						auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
						bitmap->PngName = filename;
						VirtualFile pngFile(FolderUtilities::CombinePath(rootFolder, filename));
						if(pngFile.IsValid()) {
							pngFile.ReadFile(bitmap->FileData);
						} else {
							MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <2DOverlay> : texture introuvable : " + filename);
						}
						ov->Texture = bitmap.get();
						loadedBitmaps[filename] = bitmap.get();
						outData->ImageFileData.push_back(std::move(bitmap));
					}
				}

				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <2DOverlay> " + ov->JsonFile + " enregistre (Z=" +
					std::to_string(ov->ZIndex) + ", couche " + std::to_string(ov->Layer) + ").");
				outData->Overlays.push_back(std::move(ov));
			}
		}

		else if(lineContent.substr(0, 12) == "<background>") {
			bool tileX = false;
			bool tileY = false;

			std::string data = lineContent.substr(12);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 1) {
				auto bg = std::make_unique<SnesHdBackgroundInfo>();

				//std::string filename = StringUtilities::Trim(tokens[0]);
				std::string filename = NormalizePath(tokens[0]);
				std::string fullPath = FolderUtilities::CombinePath(rootFolder, filename);

				// === DÉTECTION DOSSIER OU FICHIER (ANIMATION) ===
				std::error_code ec;
				bool isDirectory = std::filesystem::is_directory(fullPath, ec);

				// ---PARAMÈTRES D'ANIMATION par parsing de PNG List dans le dossier---
				if(tokens.size() > 10) {
					bg->FramesBetweenSteps = std::stoi(StringUtilities::Trim(tokens[10]));
				}
				if(tokens.size() > 11) {
					std::string revStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[11]));
					bg->ReverseAnim = (revStr == "true" || revStr == "1" || revStr == "yes");
				}

				if(isDirectory) {
					bg->IsAnimated = true;
					std::vector<std::string> pngFiles;

					for(const auto& entry : std::filesystem::directory_iterator(fullPath)) {
						if(entry.path().extension() == ".png") {
							pngFiles.push_back(entry.path().filename().string());
						}
					}

					std::sort(pngFiles.begin(), pngFiles.end());
					if(bg->ReverseAnim) {
						std::reverse(pngFiles.begin(), pngFiles.end());
					}

					for(const std::string& frameFile : pngFiles) {
						std::string frameRelPath = FolderUtilities::CombinePath(filename, frameFile);
						std::string frameFullPath = FolderUtilities::CombinePath(rootFolder, frameRelPath);

						SnesHdPackBitmapInfo* targetBitmap = nullptr;
						for(auto& bmp : outData->ImageFileData) {
							if(bmp->PngName == frameRelPath) {
								targetBitmap = bmp.get();
								break;
							}
						}

						if(targetBitmap == nullptr) {
							outData->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
							targetBitmap = outData->ImageFileData.back().get();
							targetBitmap->PngName = frameRelPath;

							VirtualFile pngFile(frameFullPath);
							if(pngFile.IsValid()) pngFile.ReadFile(targetBitmap->FileData);
						}
						bg->AnimFrames.push_back(targetBitmap);
					}

					if(!bg->AnimFrames.empty()) bg->Bitmap = bg->AnimFrames[0];

				} else {
					// === LOGIQUE FICHIER UNIQUE (STATIQUE) ===
					SnesHdPackBitmapInfo* targetBitmap = nullptr;
					for(auto& bmp : outData->ImageFileData) {
						if(bmp->PngName == filename) {
							targetBitmap = bmp.get();
							break;
						}
					}

					if(targetBitmap == nullptr) {
						outData->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
						targetBitmap = outData->ImageFileData.back().get();
						targetBitmap->PngName = filename;

						VirtualFile pngFile(fullPath);
						if(pngFile.IsValid()) pngFile.ReadFile(targetBitmap->FileData);
					}
					bg->Bitmap = targetBitmap;
				}

				// --- PARAMÈTRES OPTIONNELS EXISTANTS ---
				if(tokens.size() >= 2) bg->Brightness = std::stof(tokens[1]);
				if(tokens.size() >= 3) bg->ScrollXRatio = std::stof(tokens[2]);
				if(tokens.size() >= 4) bg->ScrollYRatio = std::stof(tokens[3]);
				if(tokens.size() >= 5) bg->ZIndex = std::stoi(tokens[4]);
				if(tokens.size() >= 6) bg->OffsetX = std::stoi(tokens[5]);
				if(tokens.size() >= 7) bg->OffsetY = std::stoi(tokens[6]);
				if(tokens.size() >= 8) bg->MasterScrollLayer = std::stoi(tokens[7]);
				if(tokens.size() > 8) {
					std::string txStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[8]));
					tileX = (txStr == "true" || txStr == "1" || txStr == "yes");
				}
				if(tokens.size() > 9) {
					std::string tyStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[9]));
					tileY = (tyStr == "true" || tyStr == "1" || tyStr == "yes");
				}

				// Token 12 : alpha_factor (0.00 à 1.00)
				if(tokens.size() > 12) {
					try {
						float alphaFact = std::stof(StringUtilities::Trim(tokens[12]));
						bg->AlphaFactor = std::clamp(alphaFact, 0.0f, 1.0f);
					} catch(...) {
						bg->AlphaFactor = 1.0f;
					}
				}

				// Token 13 : scroll_speed_operator ('*', '/', '+', '-', 'multiply', 'divide', 'plus', 'minus')
				if(tokens.size() > 13) {
					std::string opStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[13]));
					if(opStr == "/" || opStr == "divide") {
						bg->ScrollSpeedOperator = SnesHdBackgroundInfo::ScrollOperator::Divide;
					} else if(opStr == "+" || opStr == "plus") {
						bg->ScrollSpeedOperator = SnesHdBackgroundInfo::ScrollOperator::Plus;
					} else if(opStr == "-" || opStr == "minus") {
						bg->ScrollSpeedOperator = SnesHdBackgroundInfo::ScrollOperator::Minus;
					} else {
						bg->ScrollSpeedOperator = SnesHdBackgroundInfo::ScrollOperator::Multiply;
					}
				}



				bg->TileX = tileX;
				bg->TileY = tileY;
				bg->Conditions = currentConditions;
				if(!ign2dBlockStack.empty()) { bg->Ignore2D = true; bg->Ignore2DConditions = ign2dBlockStack.back(); outData->HasIgnore2DResources = true; }
				outData->Backgrounds.push_back(std::move(bg));
			}
		}// END BACKGROUND TAG PARSING 

		//---------------------------------------------------------------------------
		/// PARSING <m7img> TAGS
	
	else if(lineContent.substr(0, 7) == "<m7img>") {
		std::string data = lineContent.substr(7);
		std::vector<std::string> tokens = StringUtilities::Split(data, ',');

		if(tokens.size() >= 3) {
			// Remplacement de : std::string filename = StringUtilities::Trim(tokens[0]);
			std::string filename = NormalizePath(tokens[0]);

			uint32_t mapHash = (uint32_t)HexUtilities::FromHex(tokens[1]);
			uint32_t m7Scale = std::stoi(tokens[2]);

			bool ignoreColorMath = false;
			Mode7FilterMode filterMode = Mode7FilterMode::Nearest;

			if(tokens.size() >= 4) {
				std::string opt = StringUtilities::Trim(tokens[3]);
				if(opt == "1" || opt == "true" || opt == "ignore") {
					ignoreColorMath = true;
				}
			}
			if(tokens.size() >= 5) {
				std::string optB = StringUtilities::Trim(tokens[4]);
				if(optB == "anisotropic" || optB == "2") filterMode = Mode7FilterMode::Anisotropic;
				else if(optB == "1" || optB == "true" || optB == "bilinear") filterMode = Mode7FilterMode::Bilinear;
				else if(optB == "bilinearLight" || optB == "bilinearlight") filterMode = Mode7FilterMode::BilinearLight;
			}

			// RÉCUPÉRATION OU CRÉATION INSTANTANÉE (O(1)) VIA LE DICTIONNAIRE LOCAL
			SnesHdPackBitmapInfo* targetBitmap = nullptr;
			auto itBmp = loadedBitmaps.find(filename);
			if(itBmp != loadedBitmaps.end()) {
				targetBitmap = itBmp->second;
			} else {
				auto bitmap = std::make_unique<SnesHdPackBitmapInfo>();
				bitmap->PngName = filename;

				std::string fullPath = FolderUtilities::CombinePath(rootFolder, filename);
				VirtualFile pngFile(fullPath);
				if(pngFile.IsValid()) {
					pngFile.ReadFile(bitmap->FileData);
				} else {
					MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - Mode 7 Image not found : " + fullPath);
				}

				targetBitmap = bitmap.get();
				loadedBitmaps[filename] = targetBitmap;
				outData->ImageFileData.push_back(std::move(bitmap));
			}

			// CRÉATION DE L'OBJET MODE 7
			auto m7Map = std::make_unique<SnesHdMode7Image>();
			if(!ign2dBlockStack.empty()) { m7Map->Ignore2D = true; m7Map->Ignore2DConditions = ign2dBlockStack.back(); outData->HasIgnore2DResources = true; }
			m7Map->Bitmap = targetBitmap;
			m7Map->Scale = m7Scale;
			m7Map->IgnoreColorMath = ignoreColorMath;
			m7Map->FilterMode = filterMode;
			m7Map->Conditions = currentConditions;

			outData->Mode7Maps[mapHash].push_back(std::move(m7Map));
		}
		}

		//------------------------------------------------------------------------
		/// PARSING <m7tiles> TAGS
		else if(lineContent.substr(0, 9) == "<m7tiles>") {
			std::string data = lineContent.substr(9);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 3) {
				// Remplacement de : std::string filename = StringUtilities::Trim(tokens[0]);
				std::string filename = NormalizePath(tokens[0]);

				// C'est de l'Hexa, on utilise FromHex !
				uint32_t tileHash = (uint32_t)HexUtilities::FromHex(tokens[1]);
				uint32_t m7Scale = std::stoi(tokens[2]);

				// =======================================================
				// NOUVEAUTÉ : LECTURE DES COORDONNÉES X ET Y (S'ils existent)
				// =======================================================
				int srcX = 0;
				int srcY = 0;
				if(tokens.size() >= 5) {
					try {
						srcX = std::stoi(StringUtilities::Trim(tokens[3]));
						srcY = std::stoi(StringUtilities::Trim(tokens[4]));
					} catch(...) {
						MessageManager::Log("[SnesHDPack] M7Tiles Coord Parse Error line " + lineLocation);
					}
				}
				// =======================================================

				// --- RÉCUPÉRATION OU CRÉATION DU BITMAP ---
				SnesHdPackBitmapInfo* targetBitmap = nullptr;
				for(auto& bmp : outData->ImageFileData) {
					if(bmp->PngName == filename) {
						targetBitmap = bmp.get();
						break;
					}
				}

				if(targetBitmap == nullptr) {
					outData->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
					targetBitmap = outData->ImageFileData.back().get();
					targetBitmap->PngName = filename;

					std::string fullPath = FolderUtilities::CombinePath(rootFolder, filename);
					VirtualFile pngFile(fullPath);
					if(pngFile.IsValid()) {
						pngFile.ReadFile(targetBitmap->FileData);
					} else {
						MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - [SnesHDPack] Tile Mode 7 not found : " + fullPath);
					}
				}

				// --- CRÉATION DE L'OBJET MODE 7 POUR LA TUILE ---
				auto m7Map = std::make_unique<SnesHdMode7Image>();
			if(!ign2dBlockStack.empty()) { m7Map->Ignore2D = true; m7Map->Ignore2DConditions = ign2dBlockStack.back(); outData->HasIgnore2DResources = true; }
				m7Map->Bitmap = targetBitmap;
				m7Map->Scale = m7Scale;

				// =======================================================
				// NOUVEAUTÉ : AFFECTATION DES COORDONNÉES À L'OBJET
				// =======================================================
				m7Map->X = srcX;
				m7Map->Y = srcY;

				// LA LIGNE MAGIQUE POUR LES TUILES MODE 7 EST ICI !
				m7Map->Conditions = currentConditions;

				// On stocke la tuile dans le dictionnaire Mode7 existant !
				outData->Mode7Maps[tileHash].push_back(std::move(m7Map));

				// --- LE MOUCHARD DU PARSEUR ---
				/*MessageManager::Log("[MOUCHARD TILEM7 Parser] M7 Tile Loaded:  " + filename + " | Hash: " + HexUtilities::ToHex(tileHash) +
									" | X: " + std::to_string(srcX) + " | Y: " + std::to_string(srcY));
				*/
			}
		}


		//-------------------------------
		

		/// PARSING <disableOriginalTiles> TAG
		else if(lineContent == "<disableOriginalTiles>") {
			outData->DisableOriginalTiles = true;
			MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - [SnesHDPack] Option DisableOriginalTiles activated !");
		}
		//------------------------------------------------------------------------
		/// PARSING <disableTile> TAGS
		else if(lineContent.substr(0, 13) == "<disableTile>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(13), ',');
			ProcessDisableTileTag(tokens, currentConditions, outData, lineLocation);
}
		/// PARSING <disableOriginalSprites> TAG
		else if(lineContent == "<disableOriginalSprites>") {
			outData->DisableOriginalSprites = true;
			MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - [SnesHDPack] Option DisableOriginalSprites activated !");
		}

		/// PARSING <OVERSCAN> TAG --------------------------------
		else if(lineContent.substr(0, 10) == "<overscan>") {
			std::string data = lineContent.substr(10);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				outData->HasOverscanConfig = true;

				// L'ordre SNES/NES classique : Top, Right, Bottom, Left
				outData->Overscan.Top = std::stoi(StringUtilities::Trim(tokens[0]));
				outData->Overscan.Right = std::stoi(StringUtilities::Trim(tokens[1]));
				outData->Overscan.Bottom = std::stoi(StringUtilities::Trim(tokens[2]));
				outData->Overscan.Left = std::stoi(StringUtilities::Trim(tokens[3]));

				// 5th optional parameter: Stretch (true is default value)
				outData->OverscanStretch = true;
				if(tokens.size() >= 5) {
					std::string opt = StringUtilities::ToLower(StringUtilities::Trim(tokens[4]));
					if(opt == "false" || opt == "0" || opt == "nostretch" || opt == "no") {
						outData->OverscanStretch = false;
					}
				}

			}

		}// End Parsing Overscan Tag


		/// PARSING <setLayersOrder> TAG --------------------------------
		else if(lineContent.substr(0, 16) == "<setLayersOrder>") {
			std::string data = lineContent.substr(16);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				SnesHdLayerOrderRule rule;
				rule.Conditions = currentConditions; // On attache les [conditions]

				// On lit les 4 valeurs pour BG1, BG2, BG3, BG4
				for(int i = 0; i < 4; i++) {
					try {
						rule.ZIndexBg[i] = static_cast<uint8_t>(std::stoi(StringUtilities::Trim(tokens[i])));
					} catch(...) {
						rule.ZIndexBg[i] = 20; // Valeur par défaut si erreur de frappe
					}
				}

				outData->LayerOrders.push_back(rule);

				MessageManager::Log("[SnesHDPack] Line " + lineLocation +
					" - <setLayersOrder> added : " + tokens[0] + "," + tokens[1] + "," + tokens[2] + "," + tokens[3]);
			}

		}

		/// PARSING <disableBg*> TAG --------------------------------
		else if(lineContent == "<disableBg1>") {
			outData->HasDisableBg1Tag = true;
			outData->DisableBg1Conditions = currentConditions;
		} else if(lineContent == "<disableBg2>") {
			outData->HasDisableBg2Tag = true;
			outData->DisableBg2Conditions = currentConditions;
		} else if(lineContent == "<disableBg3>") {
			outData->HasDisableBg3Tag = true;
			outData->DisableBg3Conditions = currentConditions;
		} else if(lineContent == "<disableBg4>") {
			outData->HasDisableBg4Tag = true;
			outData->DisableBg4Conditions = currentConditions;
		}
		/// PARSING <disablesublayer> TAG --------------------------------
		else if(lineContent == "<disablesublayer>" || lineContent == "<disableSubLayer>" ) {
			outData->HasDisableSubLayerTag = true;
			outData->DisableSubLayerConditions = currentConditions;
			MessageManager::Log("[SnesHDPack] Tag found : <disableSubLayer>");
		}

		/// PARSING <disableAllColorMathWindow> / <disableColorMathWindow>HASH -----------
		// Supprime l'effet SD des zones de color window (toutes, ou celles de cette forme)
		else if(StringUtilities::Trim(lineContent) == "<disableAllColorMathWindow>") {
			outData->DisableAllColorMathWindowRules.push_back(currentConditions);
		}
		else if(lineContent.rfind("<disableColorMathWindow>", 0) == 0) {
			std::string hashStr = StringUtilities::Trim(lineContent.substr(24));
			if(hashStr.empty()) {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <disableColorMathWindow> : hash manquant.");
			} else {
				SnesHdDisableColorWindowRule rule;
				rule.Hash = (uint32_t)HexUtilities::FromHex(hashStr);
				rule.Conditions = currentConditions;
				outData->DisableColorMathWindowRules.push_back(rule);
			}
		}

		/// PARSING <disableColorMath> TAG --------------------------------
		else if(lineContent == "<disableColorMath>") {
			// On assigne les conditions lues juste au-dessus (ex: [memoryCheck...])
			outData->HasDisableColorMathTag = true; // On prévient le moteur !
			outData->DisableColorMathConditions = currentConditions;
		}


		// PARSING <msuToOgg> avec gestion des [conditions] -----------------------
		size_t msuTagPos = lineContent.find("<msuToOgg>");
		if(msuTagPos != std::string::npos) {

			// 1. Extraction des conditions (ex: [cond1&cond2])
			std::vector<SnesHdPackCondition*> trackConditions;
			if(msuTagPos > 0 && lineContent[0] == '[') {
				size_t endBracket = lineContent.find(']');
				if(endBracket != std::string::npos && endBracket < msuTagPos) {
					std::string condStr = lineContent.substr(1, endBracket - 1);
					std::vector<std::string> condNames = StringUtilities::Split(condStr, '&');
					for(auto& cName : condNames) {
						std::string cleanName = StringUtilities::Trim(cName);
						if(conditionsByName.find(cleanName) != conditionsByName.end()) {
							trackConditions.push_back(conditionsByName[cleanName]);
						}
					}
				}
			}

			// 2. Parsing des données audio (ID, Fichier, Loop)
			std::string data = lineContent.substr(msuTagPos + 10); // 10 = longueur de <msuToOgg>
			std::vector<std::string> msuTokens = StringUtilities::Split(data, ',');

			if(msuTokens.size() >= 2) {
				try {
					std::string trackIdStr = StringUtilities::Trim(msuTokens[0]);
					if(!trackIdStr.empty()) {
						int trackId = std::stoi(trackIdStr);

						SnesHdMsuTrack track;
						// Remplacement de : track.Filename = StringUtilities::Trim(msuTokens[1]);
						track.Filename = NormalizePath(msuTokens[1]);

						track.Conditions = trackConditions; // On accroche les conditions à la piste !

						if(msuTokens.size() >= 3) {
							std::string loopStr = StringUtilities::Trim(msuTokens[2]);
							if(!loopStr.empty()) {
								track.LoopPoint = std::stoul(loopStr);
							}
						}

						// On AJOUTE cette piste à la liste des possibilités pour cet ID
						outData->MsuTracks[trackId].push_back(track);
					}
				} catch(...) {} // Bouclier anti-crash
			}
		}// END parsing MSU2OGG

		else if(lineContent.substr(0, 7) == "<patch>") {
			std::vector<std::string> tokens = StringUtilities::Split(lineContent.substr(7), ',');
			ProcessPatchTag(tokens, rootFolder, outData, lineLocation);
		}

/// PARSING <VIDEO> TAGS --------------------------------
		else if(lineContent.substr(0, 7) == "<video>") {
			std::string data = lineContent.substr(7);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				auto video = std::make_unique<SnesHdVideoInfo>();

				// 1. Nom du fichier
				// Remplacement de : video->Filename = StringUtilities::Trim(tokens[0]);
				video->Filename = NormalizePath(tokens[0]);

				// 2. Mode
				std::string modeStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[1]));
				video->Mode = (modeStr == "pause") ? SnesHdVideoMode::Pause : SnesHdVideoMode::Overlay;

				// 3. Boutons de Skip (Masques Mesen classiques)
				std::string skipStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[2]));
				video->SkipButtonsMask = 0;
				if(skipStr.find("start") != std::string::npos) video->SkipButtonsMask |= 0x1000;
				if(skipStr.find("a") != std::string::npos)     video->SkipButtonsMask |= 0x0080;
				if(skipStr.find("b") != std::string::npos)     video->SkipButtonsMask |= 0x8000;

				// 4. Play Only Once
				std::string onceStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[3]));
				video->PlayOnlyOnce = (onceStr == "true" || onceStr == "1" || onceStr == "yes");

				// 5. Mute Audio (Optionnel)
				if(tokens.size() >= 5) {
					std::string muteStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[4]));
					video->MuteAudio = (muteStr == "true" || muteStr == "1" || muteStr == "yes");
				}

				// 6. Loop (Optionnel)
				if(tokens.size() >= 6) {
					std::string loopStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[5]));
					if(loopStr == "false" || loopStr == "0" || loopStr == "no") {
						video->LoopTargetFrame = 0; // No Loop
					} else if(loopStr == "true" || loopStr == "yes") {
						video->LoopTargetFrame = -1; // infinite Loop since beginning
					} else {
						// Si c'est un nombre, c'est la frame cible de rebouclage
						try {
							video->LoopTargetFrame = std::stoi(loopStr);
						} catch(...) {
							video->LoopTargetFrame = -1; // Fallback infini
						}
					}
				}

				// 7 & 8. Coordonnées X et Y (Optionnel)
				if(tokens.size() >= 8) {
					try {
						video->X = std::stoi(StringUtilities::Trim(tokens[6]));
						video->Y = std::stoi(StringUtilities::Trim(tokens[7]));
					} catch(...) {
						video->X = 0;
						video->Y = 0;
					}
				}

				// 9. Z-Index (Optionnel, 99 par défaut)
				if(tokens.size() >= 9) {
					try {
						video->ZIndex = static_cast<uint8_t>(std::stoi(StringUtilities::Trim(tokens[8])));
					} catch(...) {
						video->ZIndex = 99; // Fallback par défaut si la valeur est invalide
					}
				}
				// 10. Alpha Enable (Optionnel, true par défaut)
				if(tokens.size() >= 10) {
					std::string alphaStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[9]));
					video->AlphaEnable = !(alphaStr == "false" || alphaStr == "0" || alphaStr == "no");
				}

				// 11. Stretch (Optionnel, false par défaut)
				if(tokens.size() >= 11) {
					std::string stretchStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[10]));
					video->Stretch = (stretchStr == "true" || stretchStr == "1" || stretchStr == "yes");
				}


				// MOUCHARD PARSEUR
				MessageManager::Log("[SnesHDPack] Line " + lineLocation +
									" - [HD VIDEO] Tag found : " + video->Filename +
									" | PlayOnlyOnce: " + (video->PlayOnlyOnce ? "YES" : "NO") +
									" | LoopTarget: " + std::to_string(video->LoopTargetFrame) +
									" | Alpha: " + (video->AlphaEnable ? "YES" : "NO") +
									" | Stretch: " + (video->Stretch ? "YES" : "NO"));

				video->Conditions = currentConditions;
				outData->Videos.push_back(std::move(video));
			}
		}
		// End Parsing VIDEOS ----------



	}		

	//MessageManager::Log("[HD TRACE 1] Fichier texte lu ! Tuiles chargees : " + std::to_string(outData->TileByKey.size()));

	// Tri croissant des backgrounds (du Z-Index le plus bas/arrière au plus haut/avant : Algorithme du peintre)
	std::sort(outData->Backgrounds.begin(), outData->Backgrounds.end(),
		[](const std::unique_ptr<SnesHdBackgroundInfo>& a, const std::unique_ptr<SnesHdBackgroundInfo>& b) {
		return a->ZIndex < b->ZIndex;
	});


	return true;
}


//-----------------------------------------------------------------------------------------------------
bool ApplyRomPatch(const std::string& patchPath, std::vector<uint8_t>& romData)
{
	std::string ext = StringUtilities::ToLower(FolderUtilities::GetExtension(patchPath));
	std::vector<uint8_t> patchedOutput;
	bool success = false;

	if(ext == ".ips") {
		success = IpsPatcher::PatchBuffer(patchPath, romData, patchedOutput);
	} else if(ext == ".bps") {
		success = BpsPatcher::PatchBuffer(patchPath, romData, patchedOutput);
	} else {
		MessageManager::Log("[SnesHDPack] Format de patch non supporté : " + ext);
		return false;
	}

	if(success && !patchedOutput.empty()) {
		romData = std::move(patchedOutput);
		return true;
	}

	return false;
}




//-----------------------------------------------------------------------------------------------------
/// <summary>
/// Vide toutes les données du pack HD actuellement chargé, y compris les tuiles, images et conditions. 
/// Cela permet de libérer la mémoire et de réinitialiser l'état du loader.
/// </summary>
void SnesHdPackLoader::Clear() {
    //_tileMap.clear();
    //_sheetImages.clear();
	ActivePack = nullptr; // Cela va automatiquement vider les tuiles, images et conditions de la RAM !
	_isLoaded = false;
}
//-----------------------------------------------------------------------------------------------------
/// <summary>	
/// Recherche une tuile HD dans le pack actif en fonction des paramètres fournis.
/// </summary>
SnesHdPackTileInfo* SnesHdPackLoader::GetHdTile(uint64_t memoryHash, uint32_t paletteHash, SnesBpp bpp, bool isLargeTile, bool isSprite, bool isHFlip, bool isVFlip, int x, int y)
{
	if(ActivePack == nullptr) return nullptr;

	SnesHdTileKey key;
	key.MemoryHash = memoryHash;
	key.PaletteHash = paletteHash;
	key.Bpp = bpp;
	key.IsLargeTile = isLargeTile;
	key.IsSprite = isSprite;
	key.IsHFlip = isHFlip;
	key.IsVFlip = isVFlip;

	// Contexte transmis aux conditions : orientation REELLE de la tile a l'ecran.
	// Permet a hmirror / vmirror / tileNearby / spriteNearby de connaitre le flip
	// (convention Mesen NES : les decalages des conditions "nearby" sont relatifs a
	// l'orientation de la tile et s'inversent quand elle est retournee).
	// Reste valable dans les fallbacks 3a/3b (PNG non flippe) : la tile est toujours
	// retournee a l'ecran.
	SnesHdPixelData condCtx;
	condCtx.HFlip = isHFlip;
	condCtx.VFlip = isVFlip;
	condCtx.IsSprite = isSprite;

	// 1. Recherche prioritaire : Palette spécifique explicite
	auto it = ActivePack->TileByKey.find(key);
	if(it != ActivePack->TileByKey.end()) {
		for(SnesHdPackTileInfo* tile : it->second) {
			if(tile->MatchesCondition(x, y, &condCtx)) {
				return tile; // Trouvé et condition validée !
			}
		}
	}

	// 2. Fallback 1 : Recherche avec la palette Wildcard (0xFFFFFFFF)
	if(paletteHash != 0xFFFFFFFF) {
		key.PaletteHash = 0xFFFFFFFF;
		it = ActivePack->TileByKey.find(key);
		if(it != ActivePack->TileByKey.end()) {
			for(SnesHdPackTileInfo* tile : it->second) {
				if(tile->MatchesCondition(x, y, &condCtx)) {
					return tile;
				}
			}
		}
	}

	// 3. Fallback 2 : Si la tuile est flippée mais qu'aucun PNG flippé explicite ne matche
	if(isHFlip || isVFlip) {
		key.IsHFlip = false;
		key.IsVFlip = false;

		// 3a. Version non-flippée avec Palette spécifique
		key.PaletteHash = paletteHash;
		it = ActivePack->TileByKey.find(key);
		if(it != ActivePack->TileByKey.end()) {
			for(SnesHdPackTileInfo* tile : it->second) {
				if(tile->MatchesCondition(x, y, &condCtx)) {
					return tile;
				}
			}
		}

		// 3b. Version non-flippée avec Palette Wildcard
		if(paletteHash != 0xFFFFFFFF) {
			key.PaletteHash = 0xFFFFFFFF;
			it = ActivePack->TileByKey.find(key);
			if(it != ActivePack->TileByKey.end()) {
				for(SnesHdPackTileInfo* tile : it->second) {
					if(tile->MatchesCondition(x, y, &condCtx)) {
						return tile;
					}
				}
			}
		}
	}

	return nullptr;
}
//-----------------------------------------------------------------------------------------------------
/// <summary>
/// Traite la balise <patch> d'un pack : valide les paramètres, résout le chemin du fichier de patch, vérifie le hash SHA1 et enregistre le patch dans outData si tout est correct.
/// </summary>
/// <param name="tokens">Les paramètres de la balise <patch> (nom du fichier, SHA1 optionnel).</param>
///
void SnesHdPackLoader::ProcessPatchTag(const std::vector<std::string>& tokens, const std::string& rootFolder, std::shared_ptr<SnesHdPackData>& outData, const std::string& lineLocation)
{
	if(tokens.empty()) {
		MessageManager::Log("[SnesHDPack " + lineLocation + "] - Error : <patch> tag empty.");
		return;
	}

	std::string patchFilename = StringUtilities::Trim(tokens[0]);
	if(patchFilename.empty()) return;

	std::replace(patchFilename.begin(), patchFilename.end(), '\\', '/');

	std::string fullPatchPath = FolderUtilities::CombinePath(rootFolder, patchFilename);
	VirtualFile patchFile(fullPatchPath);
	if(!patchFile.IsValid()) {
		MessageManager::Log("[SnesHDPack " + lineLocation + "] - File patch not found : " + fullPatchPath);
		return;
	}

	// Cas 1 : SHA-1 absent ou joker -> Patch inconditionnel
	if(tokens.size() == 1 || StringUtilities::Trim(tokens[1]).empty() || StringUtilities::Trim(tokens[1]) == "*") {
		outData->PatchesByHash["FORCE"] = fullPatchPath;
		MessageManager::Log("[SnesHDPack " + lineLocation + "] - Patch inconditionnel enregistre : " + patchFilename);
		return;
	}

	// Cas 2 : SHA-1 spécifié -> Vérification du format (40 caractères hexadécimaux)
	std::string targetSha1 = StringUtilities::ToUpper(StringUtilities::Trim(tokens[1]));
	if(targetSha1.length() != 40) {
		MessageManager::Log("[SnesHDPack " + lineLocation + "] - invalid SHA1 for patch (" + patchFilename + ") : " + targetSha1);
		return;
	}

	outData->PatchesByHash[targetSha1] = fullPatchPath;
	MessageManager::Log("[SnesHDPack " + lineLocation + "] - Patch enregistre pour le SHA1 " + targetSha1 + " -> " + patchFilename);
}

//----------------------------------------------------------------------------
/// <summary>
/// Récupère les informations d'une image (bitmap) dans le pack actif en fonction de l'index fourni.	
/// </summary>
SnesHdPackBitmapInfo* SnesHdPackLoader::GetSheet(uint32_t imageIndex)
{
	if(ActivePack != nullptr && imageIndex < ActivePack->ImageFileData.size()) {
		return ActivePack->ImageFileData[imageIndex].get();
	}
	return nullptr;
}


//-------------------------------------------------------------------------------------------
void SnesHdPackLoader::ProcessScaleTag(const std::string& content) {
    int scale = std::stoi(content);
    if(scale < 1 || scale > 10) {
        logError("Scale must be between 1 and 10. Defaulting to 1.");
        scale = 1;
    }
    _data->Scale = static_cast<uint32_t>(scale);
}
//--------------------------------------------------------------------------------------------
bool SnesHdPackLoader::ProcessImgTag(const std::string& content)
{
	// On crée notre nouvel objet conteneur
	_data->ImageFileData.push_back(std::make_unique<SnesHdPackBitmapInfo>());
	SnesHdPackBitmapInfo* bitmapInfo = _data->ImageFileData.back().get();

	//bitmapInfo->PngName = content;
	bitmapInfo->PngName = NormalizePath(content);

	// Sur SNES, on utilise FolderUtilities et VirtualFile pour lire les octets
	// (Adapte _hdPackFolder si ta variable s'appelle rootFolder)
	std::string fullPath = FolderUtilities::CombinePath(_hdPackFolder, content);
	VirtualFile pngFile(fullPath);

	if(pngFile.IsValid()) {
		pngFile.ReadFile(bitmapInfo->FileData);
		return true;
	} else {
		// L'image n'existe pas, on annule et on prévient
		_data->ImageFileData.pop_back();
		MessageManager::Log("[SnesHDPack] Image introuvable sur le disque : " + fullPath);
		return false;
	}
}
//-----------------------------------------------------------------------------------------------

void SnesHdPackLoader::ProcessTileTag(const std::vector<std::string>& tokens, const std::vector<SnesHdPackCondition*>& conditions)
{
	checkConstraint(tokens.size() >= 5, "Tile tag must have at least 5 parameters");

	// 1. Extraction des paramètres
	uint64_t memoryHash = std::stoull(tokens[0], nullptr, 16);
	uint32_t paletteHash = static_cast<uint32_t>(std::stoul(tokens[1]));
	int imageIndex = std::stoi(tokens[2]);
	int x = std::stoi(tokens[3]);
	int y = std::stoi(tokens[4]);

	// Sécurité sur l'index de l'image (On utilise _data ici !)
	if(imageIndex < 0 || imageIndex >= _data->ImageFileData.size()) return;

	// 2. Création de la VRAIE tuile HD
	SnesHdPackTileInfo* tile = new SnesHdPackTileInfo();
	tile->MemoryHash = memoryHash;
	tile->PaletteHash = paletteHash;
	tile->Width = 8;
	tile->Height = 8;
	tile->X = x;
	tile->Y = y;

	if(tokens.size() >= 6) {
		tile->BeyondPriority = (std::stoi(tokens[5]) != 0);
	}

	// 3. On attache l'image et les conditions
	tile->Bitmap = _data->ImageFileData[imageIndex].get();
	tile->Conditions = conditions;

	// 4. On crée la clé pour le dictionnaire
	SnesHdTileKey key;
	key.MemoryHash = memoryHash;
	key.PaletteHash = paletteHash;
	key.Bpp = SnesBpp::Bpp4; // Par défaut
	key.IsLargeTile = false;

	// 5. ENREGISTREMENT DANS L'UNIQUE DICTIONNAIRE
	key.IsSprite = false;
	_data->TileByKey[key].push_back(tile);

	key.IsSprite = true; 
	_data->TileByKey[key].push_back(tile);

	// Garde une trace pour pouvoir nettoyer la RAM quand le jeu se ferme
	_data->Tiles.push_back(std::unique_ptr<SnesHdPackTileInfo>(tile));
}
//----------------------------------------------------------------------------------------
void SnesHdPackLoader::LogError(const std::string& msg) {
    MessageManager::Log("[SnesHDPack - Line " + std::to_string(_currentLine) + "] " + msg);
    _errorCount++;
} 



//------------------------------------------------------------------------------------
void SnesHdPackLoader::ProcessDisableTileTag(const std::vector<std::string>& tokens, const std::vector<SnesHdPackCondition*>& conditions, std::shared_ptr<SnesHdPackData>& outData, const std::string& lineLocation)
{
	if(tokens.size() < 2) {
		MessageManager::Log("[SnesHDPack] " + lineLocation + " - <disableTile> nécessite au moins 2 paramètres (Hash, Palette).");
		return;
	}

	uint64_t memoryHash = 0;
	uint32_t paletteHash = 0;

	try {
		memoryHash = std::stoull(StringUtilities::Trim(tokens[0]), nullptr, 16);
		paletteHash = std::stoul(StringUtilities::Trim(tokens[1]));
	} catch(...) {
		MessageManager::Log("[SnesHDPack] " + lineLocation + " - <disableTile> : Erreur de conversion des paramètres.");
		return;
	}

	// 1. Création de la tuile fantôme
	SnesHdPackTileInfo* tile = new SnesHdPackTileInfo();
	tile->MemoryHash = memoryHash;
	tile->PaletteHash = paletteHash;
	tile->IsDisabled = true;
	tile->Conditions = conditions;

	// 2. Création de la clé de recherche
	SnesHdTileKey key;
	key.MemoryHash = memoryHash;
	key.PaletteHash = paletteHash;
	key.Bpp = SnesBpp::Bpp4; // BPP par défaut pour s'assurer que ça matche
	key.IsLargeTile = false;

	// 3. Injection dans le dictionnaire (Pour les Backgrounds et les Sprites)
	key.IsSprite = false;
	outData->TileByKey[key].push_back(tile);

	key.IsSprite = true;
	outData->TileByKey[key].push_back(tile);

	// 4. Sauvegarde dans la liste principale pour la libération mémoire
	outData->Tiles.push_back(std::unique_ptr<SnesHdPackTileInfo>(tile));
}
//------------------------------------------------------------------------------------
HdPackConditionOperator SnesHdPackLoader::ParseConditionOperator(std::string& opString)
{
	if(opString == "==") return HdPackConditionOperator::Equal;
	if(opString == "!=") return HdPackConditionOperator::NotEqual;
	if(opString == ">") return HdPackConditionOperator::GreaterThan;
	if(opString == "<") return HdPackConditionOperator::LowerThan;
	if(opString == "<=") return HdPackConditionOperator::LowerThanOrEqual;
	if(opString == ">=") return HdPackConditionOperator::GreaterThanOrEqual;
	if(opString == "In" || opString == "IN" || opString == "in") return HdPackConditionOperator::In;

	return HdPackConditionOperator::Invalid;
}

// ==========================================================
// IMPLÉMENTATION DES VIDÉOS HD
// ==========================================================
bool SnesHdVideoInfo::MatchesCondition()
{
	if(PlayOnlyOnce && HasPlayed) return false;

	for(auto* cond : Conditions) {
		if(!cond->CheckCondition(0, 0, nullptr)) return false;
	}
	return true;
}