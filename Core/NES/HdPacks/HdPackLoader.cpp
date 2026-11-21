#include "pch.h"
#include <sstream>
#include <map>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include "NES/HdPacks/HdPackLoader.h"
#include "Utilities/HdFlatJson.h"
#include "NES/HdPacks/HdPackConditions.h"
#include "NES/HdPacks/HdNesPack.h"
#include "NES/HdPacks/HdGltfSkin.h"
#include "SNES/HdPacks/SnesHdGltf.h"
#include "NES/NesConsole.h"
#include "Shared/MessageManager.h"
#include "Utilities/ZipReader.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/HdPackRandomPath.h"
#include "Utilities/HdFlatJson.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/FastString.h"
#include "Utilities/magic_enum.hpp"

#include <iostream>
#include <string>

#define logError(y) MessageManager::Log("[HDPack - " + GetLineLocation() + "] " + (y)); _errorCount++;
#define checkConstraint(x, y) if(!(x)) { logError(y); return; }
#define checkConstraintEx(x, y) if(_data->Version >= 109) { checkConstraint(x, y); } else { if(!(x)) { logError(y); } }

HdPackLoader::HdPackLoader()
{
}

bool HdPackLoader::InitializeLoader(VirtualFile &romFile, HdPackData *data)
{
	_data = data;

	string romName = FolderUtilities::GetFilename(romFile.GetFileName(), false);
	string hdPackFolder = FolderUtilities::GetHdPackFolder();
	string definitionPath = FolderUtilities::CombinePath(romName, "hires.txt");

	string hdPackPath = FolderUtilities::CombinePath(hdPackFolder, definitionPath);
	if(ifstream(hdPackPath)) {
		_loadFromZip = false;
		_hdPackFolder = FolderUtilities::GetFolderName(hdPackPath);
		return true;
	}

#ifdef LIBRETRO
	// [fork HD] libretro : le dossier systeme de RetroArch ne contient pas le pack, on regarde a cote de la ROM (comme les packs SNES)
	string romFolder = romFile.GetFolderPath();
	if(!romFolder.empty() && ifstream(FolderUtilities::CombinePath(romFolder, "hires.txt"))) {
		_loadFromZip = false;
		_hdPackFolder = romFolder;
		return true;
	}
#endif

	return false;
}

bool HdPackLoader::LoadHdNesPack(string definitionFile, HdPackData &outData)
{
	HdPackLoader loader;
	if(ifstream(definitionFile)) {
		loader._data = &outData;
		loader._loadFromZip = false;
		loader._hdPackFolder = FolderUtilities::GetFolderName(definitionFile);
		return loader.LoadPack();
	}
	return false;
}

bool HdPackLoader::LoadHdNesPack(VirtualFile &romFile, HdPackData &outData)
{
	HdPackLoader loader;
	if(loader.InitializeLoader(romFile, &outData)) {
		return loader.LoadPack();
	}
	return false;
}

bool HdPackLoader::CheckFile(string filename)
{
	if(_loadFromZip) {
		return _reader.CheckFile(filename);
	} else {
		ifstream file(FolderUtilities::CombinePath(_hdPackFolder, filename), ios::in | ios::binary);
		if(file.good()) {
			return true;
		}
	}

	return false;
}

bool HdPackLoader::LoadFile(string filename, vector<uint8_t> &fileData)
{
	fileData.clear();

	

	if(_loadFromZip) {
		if(_reader.ExtractFile(filename, fileData)) {
			return true;
		}
	} else {
		ifstream file(FolderUtilities::CombinePath(_hdPackFolder, filename), ios::in | ios::binary);
		if(file.good()) {
			file.seekg(0, ios::end);
			uint32_t fileSize = (uint32_t)file.tellg();
			file.seekg(0, ios::beg);

			fileData.resize(fileSize);
			file.read((char*)fileData.data(), fileSize);
			
			return true;
		}
	}

	return false;
}

// Position de la ligne en cours pour les messages d'erreur.
// Sans <include> : "Line N" (comme avant). Avec <include> : "fichier - Line N" (ligne dans le fichier d'origine).
string HdPackLoader::GetLineLocation()
{
	if(_hasIncludes && _currentLine >= 1 && _currentLine <= (int)_lineOrigins.size()) {
		const std::pair<string, int>& origin = _lineOrigins[_currentLine - 1];
		return origin.first + " - Line " + std::to_string(origin.second);
	}
	return "Line " + std::to_string(_currentLine);
}

// Resolution des tags <include>fichier.txt : le contenu du fichier remplace la ligne, a l'endroit ou elle se trouve.
// Le chemin est relatif a la racine du pack (dossier ou zip), comme pour les PNG. Les inclusions imbriquees sont acceptees.
void HdPackLoader::ExpandIncludes(const vector<uint8_t>& input, const string& displayName, vector<uint8_t>& output, vector<string>& includeStack)
{
	size_t len = input.size();
	size_t pos = 0;
	int lineNumber = 0;

	//Ignore un eventuel BOM UTF-8 en debut de fichier inclus
	if(includeStack.size() > 1 && len >= 3 && input[0] == 0xEF && input[1] == 0xBB && input[2] == 0xBF) {
		pos = 3;
	}

	while(pos < len) {
		size_t start = pos;
		while(pos < len && input[pos] != '\n') {
			pos++;
		}
		string line((const char*)input.data() + start, pos - start);
		if(pos < len) {
			pos++; //saute le '\n'
		}
		lineNumber++;

		string trimmed = StringUtilities::Trim(line);
		while(trimmed.size() > 0 && trimmed[trimmed.size() - 1] == '\r') {
			trimmed = trimmed.substr(0, trimmed.size() - 1);
		}

		if(trimmed.size() >= 9 && StringUtilities::ToLower(trimmed.substr(0, 9)) == "<include>") {
			_hasIncludes = true;
			string includeFile = StringUtilities::Trim(trimmed.substr(9));

			//Meme position pour les erreurs ci-dessous que pour une ligne normale
			_lineOrigins.push_back({ displayName, lineNumber });
			_currentLine = (int)_lineOrigins.size();
			output.push_back('\n'); //ligne vide a la place du tag : garde la correspondance des numeros de ligne

			vector<uint8_t> includeData;
			if(includeFile.empty()) {
				logError("Include tag requires a file name");
			} else if(includeStack.size() > 20) {
				logError("Include depth limit reached: " + includeFile);
			} else if(std::find(includeStack.begin(), includeStack.end(), StringUtilities::ToLower(includeFile)) != includeStack.end()) {
				logError("Circular include ignored: " + includeFile);
			} else if(!LoadFile(includeFile, includeData)) {
				logError("Included file not found: " + includeFile);
			} else {
				includeStack.push_back(StringUtilities::ToLower(includeFile));
				ExpandIncludes(includeData, includeFile, output, includeStack);
				includeStack.pop_back();
			}
		} else {
			output.insert(output.end(), line.begin(), line.end());
			output.push_back('\n');
			_lineOrigins.push_back({ displayName, lineNumber });
		}
	}
}

// Remplacement textuel des @Alias dans une ligne : meme algorithme que le SNES (une substitution par passe, 10 passes au maximum)
static void ApplyAliases(string& line, const std::unordered_map<string, string>& aliases)
{
	bool replaced;
	int passCount = 0;
	do {
		replaced = false;
		size_t atPos = 0;
		while((atPos = line.find('@', atPos)) != string::npos) {
			size_t endPos = atPos + 1;
			while(endPos < line.length() && (std::isalnum(static_cast<unsigned char>(line[endPos])) || line[endPos] == '_')) {
				endPos++;
			}
			string aliasName = line.substr(atPos, endPos - atPos);
			auto it = aliases.find(aliasName);
			if(it != aliases.end()) {
				line.replace(atPos, aliasName.length(), it->second);
				replaced = true;
				break;
			}
			atPos = endPos;
		}
		passCount++;
	} while(replaced && passCount < 10);
}

bool HdPackLoader::LoadPack()
{
	string lineContent;
	_currentLine = 0;
	_data->PackFolder = _hdPackFolder; // dossier du pack, connu de tous les outils de diagnostic (flags)

	try {
		vector<uint8_t> hdDefinition;
		if(!LoadFile("hires.txt", hdDefinition)) {
			return false;
		}

		{
			//Resolution des <include> avant la lecture : la suite voit un seul fichier a plat
			vector<uint8_t> expanded;
			vector<string> includeStack = { "hires.txt" };
			_lineOrigins.clear();
			_hasIncludes = false;
			ExpandIncludes(hdDefinition, "hires.txt", expanded, includeStack);
			if(_hasIncludes) {
				hdDefinition = std::move(expanded);
			}
			_currentLine = 0;
		}

		InitializeGlobalConditions();

		size_t len = hdDefinition.size();
		size_t pos = 0;
		std::unordered_map<string, string> aliases; // <alias>Nom,Valeur -> cle "@Nom"
		while(pos < len) {
			lineContent.clear();

			size_t start = pos;
			for(; pos < len; pos++) {
				if(hdDefinition[pos] == '\n') {
					pos++;
					break;
				}
			}

			lineContent.insert(0, (char*)hdDefinition.data() + start, pos < len ? (pos - start - 1) : (len - start));
			_currentLine++;

			if(lineContent.empty()) {
				continue;
			}

			while(lineContent.size() > 0 && (lineContent[lineContent.size() - 1] == '\r' || lineContent[lineContent.size() - 1] == '\n')) {
				lineContent = lineContent.substr(0, lineContent.size() - 1);
			}

			// <alias>Nom,Valeur : remplacement textuel de @Nom dans les lignes suivantes (comme le SNES)
			if(lineContent.substr(0, 7) == "<alias>") {
				string aliasData = lineContent.substr(7);
				size_t commaPos = aliasData.find(',');
				if(commaPos != string::npos) {
					aliases["@" + StringUtilities::Trim(aliasData.substr(0, commaPos))] = StringUtilities::Trim(aliasData.substr(commaPos + 1));
				} else {
					logError("Syntax error <alias>. Expected : <alias>AliasName,Value");
				}
				continue;
			}

			// Remplacement des @Alias avant la lecture des conditions et des tags
			ApplyAliases(lineContent, aliases);

			vector<HdPackCondition*> conditions;
			if(lineContent.substr(0, 1) == "[") {
				size_t endOfCondition = lineContent.find_first_of(']', 1);
				if(endOfCondition == string::npos) {
					logError("Invalid condition tag: " + lineContent);
					continue;
				}
				conditions = ParseConditionString(lineContent.substr(1, endOfCondition - 1));
				lineContent = lineContent.substr(endOfCondition + 1);
			}

			// Blocs <Ignore2DOverlays>{ ... } : les tiles et fonds du bloc restent nets pendant la distorsion <2DOverlay>
			{
				string trimmed = StringUtilities::Trim(lineContent);
				if(trimmed == "}") {
					if(_blockKinds.empty()) {
						logError("'}' closing without any corresponding <Ignore2DOverlays>{ or <DioramaOffCamera>{ block.");
					} else {
						const char kind = _blockKinds.back();
						_blockKinds.pop_back();
						if(kind == 'i' && !_ign2dBlockStack.empty()) _ign2dBlockStack.pop_back();
						else if(kind == 'o' && _dioOffDepth > 0) _dioOffDepth--;
						else if(kind == 'g' && _dioIgnoreDepth > 0) _dioIgnoreDepth--;
						else if(kind == 'd' && !_dioDepthStack.empty()) _dioDepthStack.pop_back();
						else if(kind == 's' && !_dioShadowStack.empty()) _dioShadowStack.pop_back();
					}
					continue;
				}
				// Bloc <DioramaDepth>z=..,t=..{ ... } (doc 38) : les <tile> du bloc vont dans un groupe de profondeur
				if(trimmed.size() >= 15 && trimmed.back() == '{' && StringUtilities::ToLower(trimmed.substr(0, 14)) == "<dioramadepth>") {
					vector<string> depthTokens = StringUtilities::Split(trimmed.substr(14, trimmed.size() - 15), ',');
					HdDioramaDepthGroup depthGroup;
					bool depthOk = ParseDioramaDepthOptions(depthTokens, depthGroup);
					if(depthOk && depthGroup.HasRect) {
						logError("DioramaDepth : rect= n'est pas pris en charge avec un bloc { }, bloc ignore.");
						depthOk = false;
					}
					if(!conditions.empty()) {
						logError("DioramaDepth : les conditions sur le bloc sont ignorees (mettre les conditions sur les <tile>).");
					}
					// Le bloc est toujours empile pour que son '}' soit apparie ; groupe 0 = les tiles ne sont pas marquees
					_blockKinds.push_back('d');
					_dioDepthStack.push_back(depthOk ? AddDioramaDepthGroup(depthGroup) : 0);
					continue;
				}
				// Bloc <DioramaShadow>options{ ... } (doc 42, G6c) : les sprites des <tile> du bloc projettent une ombre
				if(trimmed.size() >= 16 && trimmed.back() == '{' && StringUtilities::ToLower(trimmed.substr(0, 15)) == "<dioramashadow>") {
					vector<string> shTokens = StringUtilities::Split(trimmed.substr(15, trimmed.size() - 16), ',');
					HdDioramaShadowRule shRule;
					bool shOk = ParseDioramaShadowOptions(shTokens, shRule, false);
					if(!conditions.empty()) {
						logError("DioramaShadow : les conditions sur le bloc sont ignorees (mettre les conditions sur les <tile>).");
					}
					int shId = 0;
					if(shOk) {
						if(_data->DioramaShadowRules.size() >= 200) {
							logError("DioramaShadow : 200 regles au plus, bloc ignore.");
						} else {
							_data->DioramaShadowRules.push_back(shRule);
							shId = (int)_data->DioramaShadowRules.size();
						}
					}
					_blockKinds.push_back('s'); // toujours empile pour que son '}' soit apparie
					_dioShadowStack.push_back(shId);
					continue;
				}
				// Bloc <DioramaIgnore>{ ... } (doc 38) : les <tile> du bloc ne sont dessinees dans aucune couche du diorama
				if(trimmed.size() >= 16 && StringUtilities::ToLower(trimmed.substr(0, 15)) == "<dioramaignore>" && StringUtilities::Trim(trimmed.substr(15)) == "{") {
					if(!conditions.empty()) {
						logError("DioramaIgnore : les conditions sur le bloc sont ignorees (mettre les conditions sur les <tile>).");
					}
					_blockKinds.push_back('g');
					_dioIgnoreDepth++;
					continue;
				}
				// Bloc <DioramaOffCamera>mode{ ... } (doc 38) : les <tile> du bloc restent a leur position 2D dans le diorama
				if(trimmed.size() >= 19 && trimmed.back() == '{' && StringUtilities::ToLower(trimmed.substr(0, 18)) == "<dioramaoffcamera>") {
					const string mode = StringUtilities::ToLower(StringUtilities::Trim(trimmed.substr(18, trimmed.size() - 19)));
					if(mode == "fixed3d") {
						_data->DioramaFixed3d = true;
					} else if(!mode.empty() && mode != "flat") {
						logError("DioramaOffCamera : mode inconnu (flat ou fixed3d) : " + mode + ", bloc traite en flat.");
					}
					if(!conditions.empty()) {
						logError("DioramaOffCamera : les conditions sur le bloc sont ignorees (mettre les conditions sur les <tile>).");
					}
					_blockKinds.push_back('o');
					_dioOffDepth++;
					continue;
				}
				if(trimmed.size() >= 18 && StringUtilities::ToLower(trimmed.substr(0, 18)) == "<ignore2doverlays>" && StringUtilities::Trim(trimmed.substr(18)).substr(0, 1) == "{") {
					vector<HdPackCondition*> merged = _ign2dBlockStack.empty() ? vector<HdPackCondition*>() : _ign2dBlockStack.back();
					merged.insert(merged.end(), conditions.begin(), conditions.end());
					_ign2dBlockStack.push_back(merged);
					_blockKinds.push_back('i');
					_data->HasIgnore2D = true;
					continue;
				}
			}

			vector<string> tokens;

			int conditional_bgm_counter = 0;

			if(lineContent.substr(0, 6) == "<tile>") {
				tokens = StringUtilities::Split(lineContent.substr(6), ',');
				ProcessTileTag(tokens, conditions);
			} else if(lineContent.substr(0, 12) == "<tile_hflip>") {
				tokens = StringUtilities::Split(lineContent.substr(12), ',');
				ProcessTileTag(tokens, conditions, true, false);
			} else if(lineContent.substr(0, 12) == "<tile_vflip>") {
				tokens = StringUtilities::Split(lineContent.substr(12), ',');
				ProcessTileTag(tokens, conditions, false, true);
			} else if(lineContent.substr(0, 13) == "<tile_hvflip>") {
				tokens = StringUtilities::Split(lineContent.substr(13), ',');
				ProcessTileTag(tokens, conditions, true, true);
			} else if(lineContent.substr(0, 15) == "<LoadingScreen>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				_data->LoadingScreen = HdLoadingScreen::ParseTag(tokens, [this](const string& name, vector<uint8_t>& data) { return LoadFile(name, data); });
			} else if(lineContent.substr(0, 12) == "<background>") {
				tokens = StringUtilities::Split(lineContent.substr(12), ',');
				ProcessBackgroundTag(tokens, conditions);
			} else if(lineContent.substr(0, 11) == "<condition>") {
				tokens = StringUtilities::Split(lineContent.substr(11), ',');
				ProcessConditionTag(tokens, false);
				ProcessConditionTag(tokens, true);
			} else if(lineContent.substr(0, 5) == "<img>") {
				lineContent = lineContent.substr(5);
				if(!ProcessImgTag(lineContent)) {
					return false;
				}
			} else if(lineContent.substr(0, 10) == "<addition>") {
				tokens = StringUtilities::Split(lineContent.substr(10), ',');
				ProcessAdditionTag(tokens);
			} else if(lineContent.substr(0, 10) == "<fallback>") {
				tokens = StringUtilities::Split(lineContent.substr(10), ',');
				ProcessFallbackTag(tokens);
			} else if(lineContent.substr(0, 5) == "<bgm>") {
				tokens = StringUtilities::Split(lineContent.substr(5), ',');
				ProcessBgmTag(tokens);
			} else if(lineContent.substr(0, 5) == "<sfx>") {
				tokens = StringUtilities::Split(lineContent.substr(5), ',');
				ProcessSfxTag(tokens);
			} else if(lineContent.substr(0, 7) == "<video>") {
				tokens = StringUtilities::Split(lineContent.substr(7), ',');
				ProcessVideoTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 18)) == "<ignore2doverlays>") {
				tokens = StringUtilities::Split(lineContent.substr(18), ',');
				ProcessIgnore2DTag(tokens, conditions);
			} else if(lineContent.substr(0, 11) == "<2DOverlay>") {
				tokens = StringUtilities::Split(lineContent.substr(11), ',');
				ProcessOverlayTag(tokens, conditions);
			} else if(lineContent.substr(0, 10) == "<2DLiquid>") {
				tokens = StringUtilities::Split(lineContent.substr(10), ',');
				ProcessLiquidTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 11)) == "<2dreflect>") {
				tokens = StringUtilities::Split(lineContent.substr(11), ',');
				ProcessReflectTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 11)) == "<2dtexture>") {
				tokens = StringUtilities::Split(lineContent.substr(11), ',');
				ProcessTexture2DTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 14)) == "<dioramadepth>") {
				tokens = StringUtilities::Split(lineContent.substr(14), ',');
				ProcessDioramaDepthTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 18)) == "<dioramaoffcamera>") {
				tokens = StringUtilities::Split(lineContent.substr(18), ',');
				ProcessDioramaOffCameraTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 10)) == "<tile2obj>") {
				tokens = StringUtilities::Split(lineContent.substr(10), ',');
				ProcessDioramaTile2ObjTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 12)) == "<sprite2obj>") {
				tokens = StringUtilities::Split(lineContent.substr(12), ',');
				ProcessDioramaTile2ObjTag(tokens, conditions, true);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 15)) == "<dioramaobject>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessDioramaObjectTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 15)) == "<dioramanormal>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessDioramaNormalTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 15)) == "<dioramashadow>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessDioramaShadowTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 14)) == "<dioramalight>") {
				tokens = StringUtilities::Split(lineContent.substr(14), ',');
				ProcessDioramaLightTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 15)) == "<dioramacamera>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessDioramaCameraTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 22)) == "<dioramacameradefault>") {
				tokens = StringUtilities::Split(lineContent.substr(22), ',');
				ProcessDioramaCameraDefaultTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 19)) == "<dioramacamerakeys>") {
				tokens = StringUtilities::Split(lineContent.substr(19), ',');
				ProcessDioramaCameraKeysTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 10)) == "<daynight>") {
				tokens = StringUtilities::Split(lineContent.substr(10), ',');
				ProcessDayNightTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 13)) == "<3dparticles>") {
				tokens = StringUtilities::Split(lineContent.substr(13), ',');
				ProcessDiorama3DParticlesTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 13)) == "<dioramamist>") {
				tokens = StringUtilities::Split(lineContent.substr(13), ',');
				ProcessDioramaMistTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 15)) == "<dioramaclouds>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessDioramaCloudsTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 12)) == "<dioramafog>") {
				tokens = StringUtilities::Split(lineContent.substr(12), ',');
				ProcessDioramaFogTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 13)) == "<dioramaview>") {
				tokens = StringUtilities::Split(lineContent.substr(13), ',');
				ProcessDioramaViewTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 12)) == "<dioramapad>") {
				tokens = StringUtilities::Split(lineContent.substr(12), ',');
				ProcessDioramaPadTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 15)) == "<dioramaskybox>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessDioramaSkyboxTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 15)) == "<dioramaextend>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessDioramaExtendTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 9)) == "<diorama>") {
				tokens = StringUtilities::Split(lineContent.substr(9), ',');
				ProcessDioramaTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 20)) == "<watchmemoryaddress>") {
				tokens = StringUtilities::Split(lineContent.substr(20), ',');
				ProcessWatchMemoryAddressTag(tokens, conditions);
			} else if(lineContent.substr(0, 15) == "<2DPerspective>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessPerspectiveTag(tokens, conditions);
			} else if(StringUtilities::ToLower(lineContent.substr(0, 15)) == "<2dspritelight>") {
				tokens = StringUtilities::Split(lineContent.substr(15), ',');
				ProcessSpriteLightTag(tokens, conditions);
			} else if(lineContent.substr(0, 5) == "<ver>") {
				_data->Version = stoi(lineContent.substr(5));
				if(_data->Version > BaseHdNesPack::CurrentVersion) {
					logError("This HD Pack was built with a more recent version of Mesen - update Mesen to the latest version and try again.");
					return false;
				}
			} else if(lineContent.substr(0, 7) == "<scale>") {
				lineContent = lineContent.substr(7);
				_data->Scale = std::stoi(lineContent);
				if(_data->Scale > 10) {
					logError("Scale ratios higher than 10 are not supported.");
					return false;
				}
			} else if(lineContent.substr(0, 12) == "<widescreen>") {
				tokens = StringUtilities::Split(lineContent.substr(12), ',');
				string wsOpt = tokens.size() > 0 ? StringUtilities::ToLower(StringUtilities::Trim(tokens[0])) : string();
				_data->Widescreen = (wsOpt == "on");
				if(wsOpt != "on" && wsOpt != "off") {
					logError("<widescreen> : valeur inconnue (on ou off) : " + wsOpt);
				}
			} else if(lineContent.substr(0, 10) == "<overscan>") {
				tokens = StringUtilities::Split(lineContent.substr(10), ',');
				ProcessOverscanTag(tokens);
			} else if(lineContent.substr(0, 7) == "<patch>") {
				tokens = StringUtilities::Split(lineContent.substr(7), ',');
				ProcessPatchTag(tokens);
			} else if(lineContent.substr(0, 9) == "<options>") {
				tokens = StringUtilities::Split(lineContent.substr(9), ',');
				ProcessOptionTag(tokens);
			}
			 
			  else if(lineContent.substr(0, 22) == "<play_conditional_bgm>") {
							
				//logfile << lineContent  << std::endl;
				//logfile.flush();				

				tokens = StringUtilities::Split(lineContent.substr(22), ',');
				ProcessPlayConditionalBgmTag(tokens, conditions);
			}
			  else if(lineContent.substr(0, 22) == "<play_conditional_sfx>") {
				tokens = StringUtilities::Split(lineContent.substr(22), ',');
				ProcessPlayConditionalSfxTag(tokens, conditions);
			}	
			  else if(lineContent.substr(0, 21) == "<disable_8bits_Sound>") {
				//tokens = StringUtilities::Split(lineContent.substr(21), ',');
				_data-> _disable_8bits_Sound = true;


			}

		}

		LoadCustomPalette();
		InitializeHdPack();

		if(_errorCount > 0) {
			if(_data->Version >= 109) {
				MessageManager::DisplayMessage("HDPack", "Loaded with " + std::to_string(_errorCount) + " errors");
			}
			MessageManager::Log("[HDPack] Loaded with " + std::to_string(_errorCount) + " errors");
		}

		
		std::ofstream logfile("D:\\Documents\\Mesen2\\HdPacks\\LegendofZelda_MESEN_Patch\\ProcessPlayConditionalSfxTagLog.txt", std::ios_base::trunc);
		
		logfile << " _data->SfxFilesById.size(): " << _data->SfxFilesById.size() << std::endl;
		int j =0 ;

		for(auto& pair : _data->SfxFilesById) {
			int id = pair.first;
			const std::string& path = pair.second;
			logfile << "          SFX ID: " << pair.first << " => " << pair.second << std::endl;
		}

		// log additionalSFX tags
		logfile <<  std::endl;
		logfile << "_data->PlayAdditionalSfxActions.size() : " << _data->PlayAdditionalSfxActions.size() << std::endl << std::endl;

		int i = 0;
		for(auto& action : _data->PlayAdditionalSfxActions) {
				logfile << "          -- SFXtrackID : " << _data->PlayAdditionalSfxActions[i].SfxId << " condition : " << _data->PlayAdditionalSfxActions[i].Conditions[0]->ToString() << std::endl;
				i++;
		   	logfile.flush();
			   
		}
		logfile.close();
		// end logs

		return true;
	} catch(std::exception &ex) {
		logError(string("Error: ") + ex.what() + " on line: " + lineContent);
		return false;
	}
}

/*-------------Added by Gwenolo ----------------------*/
void HdPackLoader::ProcessPlayConditionalBgmTag(vector<string>& tokens, vector<HdPackCondition*>& conditions)
{
	checkConstraint(tokens.size() >= 2, "PlayAdditionalBgm tag should contain at least 2 parameters");

	//std::ofstream logfile("D:\\Documents\\Mesen2\\HdPacks\\LegendofZelda_MESEN_Patch\\ProcessPlayConditionalBgmTagLog.txt", std::ios_base::trunc);
	//logfile << "ProcessPlayConditionalBgmTag - " << tokens[0] << " - " << tokens[1] << std::endl;

	int album = std::stoi(tokens[0]);
	int track = std::stoi(tokens[1]);
	int trackId = album * 256 + track;

	PlayAdditionalBgmAction action = {};
	action.Conditions = conditions;
	action.TrackId = trackId;
	action.loop = true;

	if(tokens.size() > 2) {
		if ((uint32_t)std::stoul(tokens[2])==0  )
		action.loop = false;
	}

	_data->PlayAdditionalBgmActions.push_back(action);

	for(HdPackCondition* condition : conditions) {
		switch(condition->GetConditionType()) {
			case HdPackConditionType::TileAtPos:
			case HdPackConditionType::SpriteAtPos:
			case HdPackConditionType::MemoryCheck:
			case HdPackConditionType::MemoryCheckConstant:
			case HdPackConditionType::FrameRange:
				action.Conditions.push_back(condition);
				break;

			default:
				logError("Invalid condition type forPlayAdditionalBgm: " + tokens[0]);
				break;
		}

		/*logfile << "Vector Size  : _data->PlayAdditionalBgmActions" << _data->PlayAdditionalBgmActions.size() << std::endl << std::endl;

		int i = 0;
		if(_data->PlayAdditionalBgmActions.size() == 3) {
			for(auto& action : _data->PlayAdditionalBgmActions) {
				logfile << "          -- BGM trackID : " << _data->PlayAdditionalBgmActions[i].TrackId << " condition : " << _data->PlayAdditionalBgmActions[i].Conditions[0]->ToString() << std::endl;
				i++;
			}
			
			logfile.flush();
			logfile.close();
		}
		*/
	}

}
/*------------------------------------------------------------------------------------------------------------*/
void HdPackLoader::ProcessPlayConditionalSfxTag(vector<string>& tokens, vector<HdPackCondition*>& conditions)
{
	checkConstraint(tokens.size() >= 2, "PlayAdditionalSfx tag should contain at least 2 parameters");	

	int album = std::stoi(tokens[0]);
	int track = std::stoi(tokens[1]);
	int trackId = album * 256 + track;

	PlayAdditionalSfxAction action = {};
	action.Conditions = conditions;
	action.SfxId = trackId;

	_data->PlayAdditionalSfxActions.push_back(action);

	for(HdPackCondition* condition : conditions) {
		switch(condition->GetConditionType()) {
			case HdPackConditionType::TileAtPos:
			case HdPackConditionType::SpriteAtPos:
			case HdPackConditionType::MemoryCheck:
			case HdPackConditionType::MemoryCheckConstant:
			case HdPackConditionType::FrameRange:
				action.Conditions.push_back(condition);
				break;

			default:
				logError("Invalid condition type for PlayAdditionalSfx: " + tokens[0]);
				break;
		}
	}
}
/*-----------------------------------------------------------------------------------------------------------*/

bool HdPackLoader::ProcessImgTag(string src)
{
	_data->ImageFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
	HdPackBitmapInfo& bitmapInfo = *_data->ImageFileData.back().get();

	if(!LoadFile(src, bitmapInfo.FileData)) {
		_data->ImageFileData.pop_back();
		logError("Error loading HDPack: PNG file " + src + " could not be read.");
		return false;
	}
	bitmapInfo.PngName = src;
	return true;
}

template<typename T>
void HdPackLoader::AddGlobalCondition(string name) {
	T* cond = new T();
	cond->Name = name;
	_data->Conditions.push_back(unique_ptr<HdPackCondition>(cond));
	_conditionsByName[name] = cond;

	name = "!" + name;
	cond = new T();
	cond->Name = name;
	_data->Conditions.push_back(unique_ptr<HdPackCondition>(cond));
	_conditionsByName[name] = cond;
}

void HdPackLoader::InitializeGlobalConditions()
{
	AddGlobalCondition<HdPackHorizontalMirroringCondition>("hmirror");
	AddGlobalCondition<HdPackVerticalMirroringCondition>("vmirror");
	AddGlobalCondition<HdPackBgPriorityCondition>("bgpriority");
	
	if(_data->Version >= 107) {
		AddGlobalCondition<HdPackSpritePaletteCondition<0>>("sppalette0");
		AddGlobalCondition<HdPackSpritePaletteCondition<1>>("sppalette1");
		AddGlobalCondition<HdPackSpritePaletteCondition<2>>("sppalette2");
		AddGlobalCondition<HdPackSpritePaletteCondition<3>>("sppalette3");
	}
}

void HdPackLoader::ReadTileData(HdTileKey& key, string& tileData, string& palData)
{
	if(tileData.size() >= 32) {
		//CHR RAM tile, read the tile data
		for(int i = 0; i < 16; i++) {
			key.TileData[i] = HexUtilities::FromHex(tileData.substr(i * 2, 2));
		}
		key.TileIndex = -1;
		key.IsChrRamTile = true;
	} else {
		if(_data->Version <= 102) {
			key.TileIndex = std::stoi(tileData);
		} else {
			key.TileIndex = HexUtilities::FromHex(tileData);
		}
		key.IsChrRamTile = false;
	}

	key.PaletteColors = HexUtilities::FromHex(palData);
}

void HdPackLoader::ProcessOverscanTag(vector<string> &tokens)
{
	OverscanDimensions overscan;
	overscan.Top = std::stoi(tokens[0]);
	overscan.Right = std::stoi(tokens[1]);
	overscan.Bottom = std::stoi(tokens[2]);
	overscan.Left = std::stoi(tokens[3]);
	_data->HasOverscanConfig = true;
	_data->Overscan = overscan;
}

void HdPackLoader::ProcessPatchTag(vector<string> &tokens)
{
	checkConstraint(tokens.size() >= 2, "Patch tag requires more parameters");
	checkConstraintEx(tokens.size() < 3, "Patch tag contains too many parameters");
	checkConstraint(tokens[1].size() == 40, "Invalid SHA1 hash for patch (" + tokens[0] + "): " + tokens[1]);

	vector<uint8_t> fileData;
	if(!LoadFile(tokens[0], fileData)) {
		checkConstraint(false, string("Patch file not found: " + tokens[1]));
	}

	std::transform(tokens[1].begin(), tokens[1].end(), tokens[1].begin(), ::toupper);
	if(_loadFromZip) {
		_data->PatchesByHash[tokens[1]] = VirtualFile(_hdPackFolder, tokens[0]);
	} else {
		_data->PatchesByHash[tokens[1]] = FolderUtilities::CombinePath(_hdPackFolder, tokens[0]);
	}
}

// Resout un chemin de PNG contenant "Randomize" / "random" (insensible a la casse).
// Zip : la liste des entrees n'est lue que si le mot-cle est present. Retour false si erreur (journalisee).
bool HdPackLoader::ResolveRandomPath(const string& path, string& resolved)
{
	if(HdPackToLowerAscii(path).find("random") == string::npos) {
		resolved = path;
		return true;
	}

	const vector<string>* entries = nullptr;
	if(_loadFromZip) {
		if(_zipEntries.empty()) {
			_zipEntries = _reader.GetFileList();
		}
		entries = &_zipEntries;
	}

	string randomError;
	if(HdPackResolveRandomPath(path, _hdPackFolder, resolved, randomError, _randomChoices, entries) < 0) {
		logError(randomError);
		return false;
	}
	return true;
}

// Resout l'image d'une <tile> : index d'un <img> (nombre) ou chemin vers un PNG.
// Un chemin peut contenir "Randomize" / "random" (insensible a la casse), resolu a l'ouverture du pack.
// Retour : index dans ImageFileData, ou -1 en cas d'erreur (journalisee).
int HdPackLoader::ResolveTileBitmapIndex(string imageParam)
{
	bool isIndex = !imageParam.empty() && std::all_of(imageParam.begin(), imageParam.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
	if(isIndex) {
		size_t index = std::stoul(imageParam);
		if(index < _data->ImageFileData.size()) {
			return (int)index;
		}
		logError("Invalid bitmap index: " + imageParam);
		return -1;
	}

	string resolved;
	if(!ResolveRandomPath(imageParam, resolved)) {
		return -1;
	}

	// Meme PNG deja charge (par <img> ou par une autre tile) : on reutilise l'entree
	for(size_t i = 0; i < _data->ImageFileData.size(); i++) {
		if(_data->ImageFileData[i]->PngName == resolved) {
			return (int)i;
		}
	}

	_data->ImageFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
	HdPackBitmapInfo& bitmapInfo = *_data->ImageFileData.back().get();
	if(!LoadFile(resolved, bitmapInfo.FileData)) {
		_data->ImageFileData.pop_back();
		logError("Error loading HDPack: PNG file " + resolved + " could not be read.");
		return -1;
	}
	bitmapInfo.PngName = resolved;
	return (int)_data->ImageFileData.size() - 1;
}

void HdPackLoader::ProcessTileTag(vector<string> &tokens, vector<HdPackCondition*> conditions, bool hFlip, bool vFlip)
{
	HdPackTileInfo *tileInfo = new HdPackTileInfo();
	size_t index = 0;
	// <tile>blank,tuile,palette (ou disable) : tuile masquee, ni SD ni HD (comme "disable" en SNES)
	if(_data->Version >= 100 && tokens.size() >= 3) {
		string imageParam = StringUtilities::ToLower(StringUtilities::Trim(tokens[0]));
		if(imageParam == "blank" || imageParam == "disable") {
			ReadTileData(*tileInfo, tokens[1], tokens[2]);
			tileInfo->MarkDisabled();
			tileInfo->X = 0;
			tileInfo->Y = 0;
			tileInfo->Width = 8 * _data->Scale;
			tileInfo->Height = 8 * _data->Scale;
			tileInfo->Conditions = conditions;
			tileInfo->IsHFlip = hFlip;
			tileInfo->IsVFlip = vFlip;
			if(!_ign2dBlockStack.empty()) {
				tileInfo->Ignore2DSets.push_back(_ign2dBlockStack.back());
			}
			if(_dioOffDepth > 0) { tileInfo->DioOffCamera = true; _data->DioramaOffTiles = true; }
			if(_dioIgnoreDepth > 0) tileInfo->DioIgnore = true;
			// Desactive le cache : la tuile depend des conditions, et une tuile vide ne doit pas etre mise en cache a tort
			tileInfo->ForceDisableCache = true;
			tileInfo->Brightness = 255;
			tileInfo->DefaultTile = false;
			tileInfo->ChrBankId = tileInfo->TileIndex / 256;
			_data->Tiles.push_back(unique_ptr<HdPackTileInfo>(tileInfo));
			return;
		}
	}
	if(_data->Version < 100) {
		tileInfo->TileIndex = std::stoi(tokens[index++]);
		tileInfo->BitmapIndex = std::stoi(tokens[index++]);
		tileInfo->PaletteColors = std::stoi(tokens[index + 2]) | (std::stoi(tokens[index + 1]) << 8) | (std::stoi(tokens[index]) << 16);
		index += 3;
	} else {
		// Image : index d'un <img>, ou chemin vers un PNG (avec "Randomize" / "random" possible)
		string imageParam = StringUtilities::Trim(tokens[index++]);
		int bitmapIndex = ResolveTileBitmapIndex(imageParam);
		if(bitmapIndex < 0) {
			delete tileInfo;
			return;
		}
		tileInfo->BitmapIndex = (uint32_t)bitmapIndex;

		string tileData = tokens[index++];
		string palData = tokens[index++];
		ReadTileData(*tileInfo, tileData, palData);
	}
	tileInfo->X = std::stoi(tokens[index++]);
	tileInfo->Y = std::stoi(tokens[index++]);
	tileInfo->Conditions = conditions;
	tileInfo->IsHFlip = hFlip;
	tileInfo->IsVFlip = vFlip;
	if(!_ign2dBlockStack.empty()) {
		tileInfo->Ignore2DSets.push_back(_ign2dBlockStack.back());
	}
	if(_dioOffDepth > 0) { tileInfo->DioOffCamera = true; _data->DioramaOffTiles = true; }
	if(_dioIgnoreDepth > 0) tileInfo->DioIgnore = true;
	if(!_dioDepthStack.empty() && _dioDepthStack.back() > 0) tileInfo->DioDepthGroup = (uint8_t)_dioDepthStack.back();
	if(!_dioShadowStack.empty() && _dioShadowStack.back() > 0) tileInfo->DioShadow = (uint8_t)_dioShadowStack.back(); // G6c
	tileInfo->ForceDisableCache = false;
	for(HdPackCondition* condition : conditions) {
		HdPackConditionType type = condition->GetConditionType();
		switch(type){
			case HdPackConditionType::SpriteNearby:
			case HdPackConditionType::PositionCheckX:
			case HdPackConditionType::PositionCheckY:
			case HdPackConditionType::OriginPositionCheckX:
			case HdPackConditionType::OriginPositionCheckY:
				tileInfo->ForceDisableCache = true;
				break;

			case HdPackConditionType::TileNearby:
				HdPackTileNearbyCondition* tileNearby = (HdPackTileNearbyCondition*)condition;
				if(tileNearby->TileX % 8 > 0 || tileNearby->TileY % 8 > 0) {
					tileInfo->ForceDisableCache = true;
				}
				break;
		}
	}

	if(_data->Version >= 105) {
		tileInfo->Brightness = (int)(std::stof(tokens[index++]) * 255);
	} else if(_data->Version > 0) {
		tileInfo->Brightness = (uint8_t)(std::stof(tokens[index++]) * 255);
	} else {
		tileInfo->Brightness = 255;
	}
	tileInfo->DefaultTile = ParseBooleanValue(tokens[index++]);

	//For CHR ROM tiles, the ID is just the bank number in chr rom (4k banks)
	tileInfo->ChrBankId = tileInfo->TileIndex / 256;

	if(_data->Version < 100) {
		if(tokens.size() >= 24) {
			//CHR RAM tile, read the tile data
			for(int i = 0; i < 16; i++) {
				tileInfo->TileData[i] = std::stoi(tokens[index++]);
			}
			tileInfo->IsChrRamTile = true;
		} else {
			tileInfo->IsChrRamTile = false;
		}
	} else {
		if(tileInfo->IsChrRamTile && tokens.size() > index) {
			tileInfo->ChrBankId = std::stoul(tokens[index++]);
		}
		if(tileInfo->IsChrRamTile && tokens.size() > index) {
			tileInfo->TileIndex = std::stoi(tokens[index++]);
		}
	}

	checkConstraintEx(tokens.size() == index, "Tile tag contains too many parameters");
	checkConstraint(tileInfo->BitmapIndex < _data->ImageFileData.size(), "Invalid bitmap index: " + std::to_string(tileInfo->BitmapIndex));

	tileInfo->Bitmap = _data->ImageFileData[tileInfo->BitmapIndex].get();
	tileInfo->Width = 8 * _data->Scale;
	tileInfo->Height = 8 * _data->Scale;
	for(const HdDioramaNormalPair& np : _data->DioramaNormalPairs) { // G6d : carte de normales du PNG de cette tile
		if(np.AlbedoName == tileInfo->Bitmap->PngName) tileInfo->NormalBitmap = np.Normal;
	}

	_data->Tiles.push_back(unique_ptr<HdPackTileInfo>(tileInfo));
}

void HdPackLoader::ProcessOptionTag(vector<string> &tokens)
{
	for(string token : tokens) {
		if(token == "disableSpriteLimit") {
			_data->OptionFlags |= (int)HdPackOptions::NoSpriteLimit;
		} else if(token == "alternateRegisterRange") {
			_data->OptionFlags |= (int)HdPackOptions::AlternateRegisterRange;
		} else if(token == "disableCache") {
			_data->OptionFlags |= (int)HdPackOptions::DisableCache;
		} else if(token == "disableOriginalTiles") {
			_data->OptionFlags |= (int)HdPackOptions::DontRenderOriginalTiles;
		} else if(token == "automaticFallbackTiles") {
			_data->OptionFlags |= (int)HdPackOptions::AutomaticFallbackTiles;
		} else if(token == "disableContours") {
			//no longer exists, prevent warning
		} else {
			logError("Invalid option: " + token);
		}
	}
}

// Coordonnee d'une condition : nombre, ou joker '*' / 'all' / 'any' (toutes les colonnes ou lignes).
// Le joker n'est accepte que par tileAtPosition et spriteAtPosition (verifie dans ProcessConditionTag).
static int ParseConditionCoordinate(const string& token, bool& isAny)
{
	string t = StringUtilities::Trim(token);
	if(t == "*" || t == "all" || t == "any") {
		isAny = true;
		return 0;
	}
	return std::stoi(t);
}

void HdPackLoader::ProcessConditionTag(vector<string> &tokens, bool createInvertedCondition)
{
	checkConstraint(tokens.size() >= 4, "Condition tag should contain at least 4 parameters");
	checkConstraint(tokens[0].size() > 0, "Condition name may not be empty");
	checkConstraint(tokens[0].find_last_of('!') == string::npos, "Condition name may not contain '!' characters");

	unique_ptr<HdPackCondition> condition;
	
	if(tokens[1] == "tileAtPosition") {
		condition.reset(new HdPackTileAtPositionCondition());
	} else if(tokens[1] == "tileNearby") {
		condition.reset(new HdPackTileNearbyCondition());
	} else if(tokens[1] == "spriteAtPosition") {
		condition.reset(new HdPackSpriteAtPositionCondition());
	} else if(tokens[1] == "spriteNearby") {
		condition.reset(new HdPackSpriteNearbyCondition());
	} else if(tokens[1] == "memoryCheck" || tokens[1] == "ppuMemoryCheck") {
		condition.reset(new HdPackMemoryCheckCondition());
	} else if(tokens[1] == "memoryCheckConstant" || tokens[1] == "ppuMemoryCheckConstant") {
		condition.reset(new HdPackMemoryCheckConstantCondition());
	} else if(tokens[1] == "frameRange") {
		condition.reset(new HdPackFrameRangeCondition());
	} else if(tokens[1] == "positionCheckX") {
		condition.reset(new HdPackPositionCheckXCondition());
	} else if(tokens[1] == "positionCheckY") {
		condition.reset(new HdPackPositionCheckYCondition());
	} else if(tokens[1] == "originPositionCheckX") {
		condition.reset(new HdPackOriginPositionCheckXCondition());
	} else if(tokens[1] == "originPositionCheckY") {
		condition.reset(new HdPackOriginPositionCheckYCondition());
	} else {
		logError("Invalid condition type: " + tokens[1]);
		return;
	}

	tokens[0].erase(tokens[0].find_last_not_of(" \n\r\t") + 1);
	condition->Name = tokens[0];

	if(createInvertedCondition) {
		condition->Name = "!" + condition->Name;
	}

	int index = 2;
	switch(condition->GetConditionType()) {
		case HdPackConditionType::TileNearby:
		case HdPackConditionType::TileAtPos:
		case HdPackConditionType::SpriteNearby:
		case HdPackConditionType::SpriteAtPos: {
			checkConstraint(tokens.size() >= 6, "Condition tag should contain at least 6 parameters");
			checkConstraintEx(tokens.size() < 9, "Condition tag contains too many parameters");

			bool anyX = false, anyY = false;
			int x = ParseConditionCoordinate(tokens[index++], anyX);
			int y = ParseConditionCoordinate(tokens[index++], anyY);
			bool isAtPosition = condition->GetConditionType() == HdPackConditionType::TileAtPos || condition->GetConditionType() == HdPackConditionType::SpriteAtPos;
			if((anyX || anyY) && !isAtPosition) {
				logError("Joker '*' is only supported by tileAtPosition and spriteAtPosition");
				return;
			}
			string token = tokens[index++];
			int32_t tileIndex = -1;
			string tileData;
			if(token.size() == 32) {
				tileData = token;
			} else {
				if(_data->Version < 104) {
					tileIndex = std::stoi(token);
				} else {
					//Tile indexes are stored as hex starting from version 104+
					tileIndex = HexUtilities::FromHex(token);
				}
			}
			uint32_t palette = HexUtilities::FromHex(tokens[index++]);

			bool ignorePalette = false;
			if(tokens.size() >= 7) {
				checkConstraintEx(_data->Version >= 108, "Condition tag ignore palette feature requires version 108+ of HD Packs");
				if(_data->Version >= 108) {
					ignorePalette = ParseBooleanValue(tokens[index++]);
				}
			}

			((HdPackBaseTileCondition*)condition.get())->Initialize(x, y, palette, tileIndex, tileData, ignorePalette);
			((HdPackBaseTileCondition*)condition.get())->CheckAnyX = anyX;
			((HdPackBaseTileCondition*)condition.get())->CheckAnyY = anyY;
			break;
		}

		case HdPackConditionType::MemoryCheck:
		case HdPackConditionType::MemoryCheckConstant:
		{
			checkConstraint(_data->Version >= 101, "This feature requires version 101+ of HD Packs");
			checkConstraint(tokens.size() >= 5, "Condition tag should contain at least 5 parameters");
			checkConstraintEx(tokens.size() < 7, "Condition tag contains too many parameters");

			bool usePpuMemory = tokens[1].substr(0, 3) == "ppu";
			uint32_t operandA = HexUtilities::FromHex(tokens[index++]);

			if(usePpuMemory) {
				checkConstraint(operandA <= 0x3FFF, "Out of range memoryCheck operand");
				operandA |= HdPackBaseMemoryCondition::PpuMemoryMarker;
			} else {
				checkConstraint(operandA <= 0xFFFF, "Out of range memoryCheck operand");
			}

			HdPackConditionOperator op = ParseConditionOperator(tokens[index++]);
			checkConstraint(op != HdPackConditionOperator::Invalid, "Invalid operator.");


			uint32_t mask = 0xFF;
			string operandBListStr = "";

			// modification by Gwenolo
			uint32_t operandB = -1;
			bool In_operator_and_multiple_values_for_operandB = false;

			if(op != HdPackConditionOperator::In) {
				operandB = HexUtilities::FromHex(tokens[index++]);
			} else { //Operator == IN
				operandBListStr = tokens[index++];
				if(operandBListStr.find('|') != std::string::npos)  //contains '|'
				{
					In_operator_and_multiple_values_for_operandB = true;

				} else {// does not contain '|' : In is equivalent to Equal operator as it will be compared to one unique value
					operandB = HexUtilities::FromHex(operandBListStr);
					op = HdPackConditionOperator::Equal;

				}
			}


			//End of modification by Gwenolo : back to std Code
			if(tokens.size() > 5 && _data->Version >= 103) {
				checkConstraint(operandB <= 0xFF, "Out of range memoryCheck mask");
				mask = HexUtilities::FromHex(tokens[index++]);
			}


			//Modified by Gwenolo
			/*if(op == HdPackConditionOperator::In) {
				//checkConstraint(_data->Version >= 109, "The 'In' operator requires HD Pack version 109+");
				checkConstraint(tokens.size() >= 5, "Missing operand list for 'In' operator");

				string operandBListStr = tokens[index++]; // e.g. "03|05|0A"

				if(tokens.size() > index && _data->Version >= 103) {
					mask = HexUtilities::FromHex(tokens[index++]);
				}

				if(condition->GetConditionType() == HdPackConditionType::MemoryCheckConstant) {
					((HdPackMemoryCheckConstantCondition*)condition.get())->Initialize(operandA, op, operandBListStr, (uint8_t)mask);
				} else {
					logError("Operator 'In' is not supported with memoryCheck (only memoryCheckConstant)");
					return;
				}
			} else {
				uint32_t operandB = HexUtilities::FromHex(tokens[index++]);
			}

			if(tokens.size() > index && _data->Version >= 103) {
					mask = HexUtilities::FromHex(tokens[index++]);
			}
			*/
			// end of Modification by Gwenolo



			switch(condition->GetConditionType()) {
				case HdPackConditionType::MemoryCheck:
					if(usePpuMemory) {
						checkConstraint(operandB <= 0x3FFF, "Out of range memoryCheck operand");
						operandB |= HdPackBaseMemoryCondition::PpuMemoryMarker;
					} else {
						checkConstraint(operandB <= 0xFFFF, "Out of range memoryCheck operand");
					}
					_data->WatchedMemoryAddresses.emplace(operandB);
					break;

				case HdPackConditionType::MemoryCheckConstant:
				{
					if(!In_operator_and_multiple_values_for_operandB)	checkConstraint(operandB <= 0xFF, "Out of range memoryCheckConstant operand");
					break;
				}
			}

			_data->WatchedMemoryAddresses.emplace(operandA);

			if(!In_operator_and_multiple_values_for_operandB) {
			     ((HdPackBaseMemoryCondition*)condition.get())->Initialize(operandA, op, operandB, (uint8_t)mask);
		   }
			else {
				((HdPackMemoryCheckConstantCondition*)condition.get())->InitializeForInOperator(operandA, op, operandBListStr, (uint8_t)mask);
			}
			break;
		}

		case HdPackConditionType::PositionCheckX:
		case HdPackConditionType::PositionCheckY:
		case HdPackConditionType::OriginPositionCheckX:
		case HdPackConditionType::OriginPositionCheckY: {
			checkConstraint(_data->Version >= 108, "This condition type requires version 108+ of HD Packs");
			checkConstraint(tokens.size() >= 4, "Condition tag should contain at least 4 parameters");
			checkConstraintEx(tokens.size() < 5, "Condition tag contains too many parameters");

			HdPackConditionOperator op = ParseConditionOperator(tokens[index++]);
			checkConstraint(op != HdPackConditionOperator::Invalid, "Invalid operator.");

			uint32_t operand = std::stoi(tokens[index++]);

			checkConstraint(operand <= 0xFFFF, "Out of range positionCheck operand");
			((HdPackBasePositionCheckCondition*)condition.get())->Initialize(op, operand);
			break;
		}

		case HdPackConditionType::FrameRange: {
			checkConstraint(_data->Version >= 101, "This condition type requires version 101+ of HD Packs");
			checkConstraint(tokens.size() >= 4, "Condition tag should contain at least 4 parameters");
			checkConstraintEx(tokens.size() < 5, "Condition tag contains too many parameters");

			int32_t operandA;
			int32_t operandB;
			if(_data->Version == 101) {
				operandA = HexUtilities::FromHex(tokens[index++]);
				operandB = HexUtilities::FromHex(tokens[index++]);
			} else {
				//Version 102+
				operandA = std::stoi(tokens[index++]);
				operandB = std::stoi(tokens[index++]);
			}

			checkConstraint(operandA >= 0 && operandA <= 0xFFFF, "Out of range frameRange operand");
			checkConstraint(operandB >= 0 && operandB <= 0xFFFF, "Out of range frameRange operand");

			((HdPackFrameRangeCondition*)condition.get())->Initialize(operandA, operandB);
			break;
		}
	}
	
	HdPackCondition *cond = condition.get();
	condition.release();
	_data->Conditions.emplace_back(unique_ptr<HdPackCondition>(cond));
	_conditionsByName[cond->Name] = cond;
}

HdPackConditionOperator HdPackLoader::ParseConditionOperator(string& opString)
{
	if(opString == "==") {
		return HdPackConditionOperator::Equal;
	} else if(opString == "!=") {
		return HdPackConditionOperator::NotEqual;
	} else if(opString == ">") {
		return HdPackConditionOperator::GreaterThan;
	} else if(opString == "<") {
		return HdPackConditionOperator::LowerThan;
	} else if(opString == "<=") {
		return HdPackConditionOperator::LowerThanOrEqual;
	} else if(opString == ">=") {
		return HdPackConditionOperator::GreaterThanOrEqual;
	} 
	//added by Gwenolo
	else if(opString == "In" or opString == "IN" or opString == "in") {
		return HdPackConditionOperator::In;
	}

	return HdPackConditionOperator::Invalid;
}

void HdPackLoader::ProcessBackgroundTag(vector<string> &tokens, vector<HdPackCondition*> conditions)
{
	// Options nommees ramx=expr / ramy=expr (valeurs $nom de <watchMemoryAddress>) : retirees avant la lecture positionnelle
	HdWatchExpr bgExprX, bgExprY;
	for(size_t ti = 1; ti < tokens.size(); ) {
		string tk = StringUtilities::Trim(tokens[ti]);
		string low = StringUtilities::ToLower(tk);
		bool isX = low.compare(0, 5, "ramx=") == 0;
		bool isY = low.compare(0, 5, "ramy=") == 0;
		if(isX || isY) {
			string err;
			HdWatchExpr& dst = isX ? bgExprX : bgExprY;
			if(!HdWatchExpr::Parse(tk.substr(5), _data->WatchedValues, dst, err)) {
				logError(string("Background : ") + (isX ? "ramx" : "ramy") + " : " + err + ", option ignoree.");
				dst.T.clear();
			}
			tokens.erase(tokens.begin() + ti);
		} else {
			ti++;
		}
	}
	// Option nommee dio=off|l0..l3|g1..g4 : cible du fond dans le diorama (doc 50), retiree avant la lecture positionnelle
	int bgDio = -1;
	float bgIncl = 0.0f;
	int bgWideX = 0, bgWideY = 0;
	for(size_t ti = 1; ti < tokens.size(); ) {
		string tk = StringUtilities::Trim(tokens[ti]);
		string low = StringUtilities::ToLower(tk);
		if(low.compare(0, 5, "wide=") == 0 || low.compare(0, 5, "tall=") == 0) {
			try {
				const int n = std::max(0, std::min(1024, std::stoi(low.substr(5))));
				if(low[0] == 'w') bgWideX = n; else bgWideY = n;
			} catch(...) { logError("Background : " + low.substr(0, 4) + "= attend un nombre de pixels NES, option ignoree."); }
			tokens.erase(tokens.begin() + ti);
		} else if(low.compare(0, 12, "inclinaison=") == 0 || low.compare(0, 12, "inclination=") == 0) { // doc 53 : graphie francaise ou anglaise
			try { bgIncl = std::max(-90.0f, std::min(90.0f, std::stof(low.substr(12)))); } catch(...) { logError("Background : inclinaison= attend un angle en degres, option ignoree."); }
			tokens.erase(tokens.begin() + ti);
		} else if(low.compare(0, 4, "dio=") == 0) {
			const string v = low.substr(4);
			if(v == "off") bgDio = -2;
			else if(v == "l0") bgDio = 0;
			else if(v == "l1") bgDio = 1;
			else if(v == "l2") bgDio = 2;
			else if(v == "l3") bgDio = 3;
			else if(v == "g1") bgDio = 4;
			else if(v == "g2") bgDio = 5;
			else if(v == "g3") bgDio = 6;
			else if(v == "g4") bgDio = 7;
			else if(v == "g5") bgDio = 8;
			else if(v == "g6") bgDio = 9;
			else if(v == "g7") bgDio = 10;
			else if(v == "g8") bgDio = 11;
			else logError("Background : dio= doit valoir off, l0 a l3 ou g1 a g8 : " + v + ", option ignoree.");
			tokens.erase(tokens.begin() + ti);
		} else {
			ti++;
		}
	}
	if(bgDio == -1 && _dioIgnoreDepth > 0) bgDio = -2;
	if(bgDio == -1 && !_dioDepthStack.empty() && _dioDepthStack.back() > 0) bgDio = 3 + _dioDepthStack.back();
	checkConstraint(tokens.size() >= 2, "Background tag should contain at least 2 parameters");
	checkConstraintEx(tokens.size() < 14, "Background tag contains too many parameters");
	HdPackBitmapInfo* bgFileData = nullptr;

	// Mot-cle "Randomize" / "random" dans le chemin du PNG : tirage unique par pack
	string randomPath;
	if(ResolveRandomPath(tokens[0], randomPath)) {
		tokens[0] = randomPath;
	}

	// Dossier = animation : une image par PNG du dossier, triees par nom (inversees si reverse)
	const bool reverseAnim = tokens.size() > 11 && ParseBooleanValue(tokens[11]);
	vector<HdPackBitmapInfo*> animFrames;
	const string folderPath = FolderUtilities::CombinePath(_hdPackFolder, tokens[0]);
	if(std::filesystem::is_directory(folderPath)) {
		vector<string> pngFiles;
		for(const auto& entry : std::filesystem::directory_iterator(folderPath)) {
			if(entry.path().extension() == ".png") {
				pngFiles.push_back(entry.path().filename().string());
			}
		}
		std::sort(pngFiles.begin(), pngFiles.end());
		if(reverseAnim) {
			std::reverse(pngFiles.begin(), pngFiles.end());
		}
		for(const string& frameFile : pngFiles) {
			string frameRelPath = FolderUtilities::CombinePath(tokens[0], frameFile);
			_data->BackgroundFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
			HdPackBitmapInfo* frame = _data->BackgroundFileData.back().get();
			frame->PngName = frameRelPath;
			if(LoadFile(frameRelPath, frame->FileData)) {
				animFrames.push_back(frame);
			} else {
				_data->BackgroundFileData.pop_back();
			}
		}
	}

	auto result = _backgroundsByName.find(tokens[0]);
	if(!animFrames.empty()) {
		bgFileData = animFrames[0];
	} else if(result == _backgroundsByName.end()) {
		_data->BackgroundFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
		bgFileData = _data->BackgroundFileData.back().get();
		bgFileData->PngName = tokens[0];

		if(!LoadFile(bgFileData->PngName, bgFileData->FileData)) {
			bgFileData = nullptr;
			_data->BackgroundFileData.pop_back();
		} else {
			_backgroundsByName[tokens[0]] = bgFileData;
		}
	} else {
		bgFileData = result->second;
	}

	HdBackgroundInfo backgroundInfo;
	if(bgFileData) {
		backgroundInfo.Data = bgFileData;
        backgroundInfo.AnimFrames = animFrames;
        backgroundInfo.ReverseAnim = reverseAnim;
		if (_data->Version >= 105) {
			backgroundInfo.Brightness = (int)(std::stof(tokens[1]) * 255);
		} else {
			backgroundInfo.Brightness = (uint8_t)(std::stof(tokens[1]) * 255);
		}
		backgroundInfo.HorizontalScrollRatio = 0;
		backgroundInfo.VerticalScrollRatio = 0;
		backgroundInfo.Priority = 10;
		backgroundInfo.Left = 0;
		backgroundInfo.Top = 0;
		backgroundInfo.Conditions.reserve(conditions.size());
		backgroundInfo.ScrollXExpr = bgExprX;
		backgroundInfo.ScrollYExpr = bgExprY;
		backgroundInfo.DioTarget = bgDio;
		backgroundInfo.DioIncl = bgIncl;
		backgroundInfo.DioWideX = bgWideX; backgroundInfo.DioWideY = bgWideY;
		backgroundInfo.BlendMode = HdPackBlendMode::Alpha;
		if(!_ign2dBlockStack.empty()) {
			backgroundInfo.Ignore2DSets.push_back(_ign2dBlockStack.back());
		}

		for(HdPackCondition* condition : conditions) {
			switch(condition->GetConditionType()) {
				case HdPackConditionType::TileAtPos:
				case HdPackConditionType::SpriteAtPos:
				case HdPackConditionType::MemoryCheck:
				case HdPackConditionType::MemoryCheckConstant:
				case HdPackConditionType::FrameRange:
					backgroundInfo.Conditions.push_back(condition);
					break;

				default:
					logError("Invalid condition type for background: " + tokens[0]);
					break;
			}
		}

		if(tokens.size() > 2) {
			checkConstraint(_data->Version >= 101, "This feature requires version 101+ of HD Packs");

			backgroundInfo.HorizontalScrollRatio = std::stof(tokens[2]);
			if(tokens.size() > 3) {
				backgroundInfo.VerticalScrollRatio = std::stof(tokens[3]);
			}
			if(tokens.size() > 4) {
				checkConstraint(_data->Version >= 102, "This feature requires version 102+ of HD Packs");
				if(_data->Version >= 106) {
					backgroundInfo.Priority = std::stoi(tokens[4]);
					checkConstraint(backgroundInfo.Priority >= 0 && backgroundInfo.Priority < 50, "Invalid background priority value (0 to 49)");
				} else {
					backgroundInfo.Priority = ParseBooleanValue(tokens[4]) ? 0 : 10;
				}
			}
			if(tokens.size() > 6) {
				checkConstraint(_data->Version >= 105, "This feature requires version 105+ of HD Packs");
				backgroundInfo.Left = std::max(0, std::stoi(tokens[5]));
				backgroundInfo.Top = std::max(0, std::stoi(tokens[6]));
			}

			if(tokens.size() > 7) {
				checkConstraintEx(_data->Version >= 107, "Background blend mode feature requires version 107+ of HD Packs");
				auto blendMode  = magic_enum::enum_cast<HdPackBlendMode>(tokens[7]);
				if(blendMode.has_value()) {
					backgroundInfo.BlendMode = blendMode.value();
				} else {
					logError("Invalid blend mode: " + tokens[7]);
				}
			}
		}

		// Champs etendus (NES) : tileX, tileY, framesBetweenSteps, reverse (deja lu), operator
		if(tokens.size() > 8) {
			checkConstraintEx(_data->Version >= 107, "Background tileX/tileY fields require version 107+ of HD Packs");
			backgroundInfo.TileX = ParseBooleanValue(tokens[8]);
		}
		if(tokens.size() > 9) {
			backgroundInfo.TileY = ParseBooleanValue(tokens[9]);
		}
		if(tokens.size() > 10) {
			backgroundInfo.FramesBetweenSteps = std::max(0, std::stoi(tokens[10]));
		}
		if(tokens.size() > 12) {
			string op = StringUtilities::ToLower(tokens[12]);
			if(op == "*" || op == "multiply") {
				backgroundInfo.Operator = HdBackgroundInfo::ScrollOperator::Multiply;
			} else if(op == "/" || op == "divide") {
				backgroundInfo.Operator = HdBackgroundInfo::ScrollOperator::Divide;
			} else if(op == "+" || op == "plus") {
				backgroundInfo.Operator = HdBackgroundInfo::ScrollOperator::Plus;
			} else if(op == "-" || op == "minus") {
				backgroundInfo.Operator = HdBackgroundInfo::ScrollOperator::Minus;
			} else {
				logError("Invalid background operator: " + tokens[12]);
			}
		}

		_data->BackgroundsByPriority[backgroundInfo.Priority].push_back(backgroundInfo);
	} else {
		checkConstraint(false, "Error while loading background: " + tokens[0]);
	}
}

void HdPackLoader::ProcessAdditionTag(vector<string>& tokens)
{
	checkConstraint(_data->Version >= 107, "Additiona tag requires version 107+ of HD Packs");
	checkConstraint(tokens.size() >= 6, "Addition tag should contain at least 6 parameters");
	checkConstraintEx(tokens.size() < 8, "Addition tag contains too many parameters");

	_data->AdditionalSprites.push_back({});

	HdPackAdditionalSpriteInfo& info = _data->AdditionalSprites.back();
	ReadTileData(info.OriginalTile, tokens[0], tokens[1]);
	info.OffsetX = std::stoi(tokens[2]);
	info.OffsetY = std::stoi(tokens[3]);
	ReadTileData(info.AdditionalTile, tokens[4], tokens[5]);

	info.IgnorePalette = false;
	if(tokens.size() >= 7) {
		checkConstraint(_data->Version >= 108, "Addition tag ignore palette feature requires version 108+ of HD Packs");
		info.IgnorePalette = ParseBooleanValue(tokens[6]);
	}
}

void HdPackLoader::ProcessFallbackTag(vector<string>& tokens)
{
	checkConstraint(_data->Version >= 107, "Fallback tag requires version 107+ of HD Packs");
	checkConstraint(tokens.size() >= 2, "Fallback tag should contain at least 2 parameters");
	checkConstraintEx(tokens.size() < 3, "Fallback tag contains too many parameters");
	_data->FallbackTiles.push_back({ HexUtilities::FromHex(tokens[0]), HexUtilities::FromHex(tokens[1]) });
}

int HdPackLoader::ProcessSoundTrack(string albumString, string trackString, string filename)
{
	int album = std::stoi(albumString);
	if(album < 0 || album > 255) {
		logError("Invalid album value: " + albumString);
		return -1;
	}

	int track = std::stoi(trackString);
	if(track < 0 || track > 255) {
		logError("Invalid track value: " + trackString);
		return -1;
	}

	if(!CheckFile(filename)) {
		logError("OGG file not found: " + filename);
		return -1;
	}

	return album * 256 + track;
}





// Booleens du tag <video> : Y/true/yes/1 = vrai (le reste des tags NES utilise Y/N)
static bool ParseVideoBool(const string& value)
{
	string v = StringUtilities::ToLower(StringUtilities::Trim(value));
	return v == "y" || v == "true" || v == "yes" || v == "1";
}

// <video>fichier,mode,skip,once,mute,loop,X,Y,Z,alpha,stretch : meme format que cote SNES.
// Z : meme echelle que la priorite des <background> (0 a 39), 99 = au-dessus de tout (defaut).
// mode (pause) et skip sont lus mais pas encore utilises, comme cote SNES.
// <2DOverlay> : particules 2D. Syntaxe : [cond]<2DOverlay>fichier.json,Cle=valeur,...
// Les parametres de la balise remplacent ceux du JSON (meme format que le SNES, voir claude/09_2DOverlay.md).
void HdPackLoader::ProcessOverlayTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(tokens.empty()) {
		logError("2DOverlay tag should contain a JSON file name");
		return;
	}

	string jsonName = StringUtilities::Trim(tokens[0]);
	if(!CheckFile(jsonName)) {
		logError("2DOverlay file not found: " + jsonName);
		return;
	}
	vector<uint8_t> raw;
	if(!LoadFile(jsonName, raw)) {
		logError("2DOverlay file could not be read: " + jsonName);
		return;
	}

	std::unordered_map<std::string, std::string> cfg = ParseFlatJson(std::string(raw.begin(), raw.end()));
	for(size_t t = 1; t < tokens.size(); t++) {
		string tok = StringUtilities::Trim(tokens[t]);
		size_t eq = tok.find('=');
		if(eq == string::npos) {
			continue;
		}
		cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
	}

	auto str = [&](const char* k) -> string {
		auto it = cfg.find(k);
		return it == cfg.end() ? string() : it->second;
	};
	auto num = [&](const char* k, float def) -> float {
		string v = str(k);
		if(v.empty()) return def;
		try { return std::stof(v); } catch(...) { return def; }
	};
	auto boolean = [&](const char* k, bool def) -> bool {
		string v = StringUtilities::ToLower(str(k));
		if(v.empty()) return def;
		return v == "true" || v == "1" || v == "yes";
	};
	auto color = [&](const char* k, uint32_t def) -> uint32_t {
		string v = StringUtilities::Trim(str(k));
		if(v.empty()) return def;
		if(v[0] == '#') v = v.substr(1);
		try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; }
	};

	string type = StringUtilities::ToLower(str("type"));
	if(!type.empty() && type != "particles" && type != "plainscreen" && type != "ellipse" && type != "shadows" && type != "clouds" && type != "fog" && type != "rays" && type != "distortion" && type != "aurora" && type != "torchlight") {
		logError("2DOverlay type '" + type + "' is not supported on NES yet (particles, plainscreen, ellipse, shadows, clouds, fog, rays, distortion, aurora)");
		return;
	}

	auto overlay = std::make_unique<NesHdOverlayInfo>();
	overlay->JsonFile = jsonName;
	overlay->Conditions = conditions;
	overlay->ZIndex = std::clamp((int)num("z", num("zindex", 39.0f)), 0, 255);
	overlay->Layer = std::clamp((int)num("layer", 0.0f), 0, 1);
	overlay->SpeedX = num("speedx", 1.0f);
	overlay->SpeedY = num("speedy", 1.0f);
	overlay->Alpha = std::clamp(num("alpha", 1.0f), 0.0f, 1.0f);
	overlay->FieldW = std::max(0.25f, num("fieldw", 1.0f));
	overlay->FieldH = std::max(0.25f, num("fieldh", 1.0f));

	string blend = StringUtilities::ToLower(str("blend"));
	overlay->Additive = (blend == "additive" || blend == "add");
	overlay->MaxParticles = std::clamp((int)num("maxparticles", 300.0f), 1, 20000);
	overlay->Rate = std::max(0.0f, num("rate", 60.0f));
	overlay->LifeMin = std::max(0.05f, num("lifemin", 2.0f));
	overlay->LifeMax = std::max(overlay->LifeMin, num("lifemax", 4.0f));
	overlay->SpawnX = num("spawnx", 0.0f);
	overlay->SpawnY = num("spawny", 0.0f);
	overlay->SpawnW = num("spawnw", 1.0f);
	overlay->SpawnH = num("spawnh", 1.0f);
	overlay->SpeedMin = num("speedmin", 20.0f);
	overlay->SpeedMax = num("speedmax", 40.0f);
	overlay->AngleMin = num("anglemin", 80.0f);
	overlay->AngleMax = num("anglemax", 100.0f);
	overlay->GravityX = num("gravityx", 0.0f);
	overlay->GravityY = num("gravityy", 0.0f);
	overlay->WindX = num("windx", 0.0f);
	overlay->WindY = num("windy", 0.0f);
	overlay->Turbulence = num("turbulence", 0.0f);
	overlay->TurbulenceFreq = num("turbulencefreq", 0.5f);
	overlay->RotationMin = num("rotationmin", 0.0f);
	overlay->RotationMax = num("rotationmax", 0.0f);
	overlay->SpinMin = num("spinmin", 0.0f);
	overlay->SpinMax = num("spinmax", 0.0f);
	overlay->SizeStart = num("sizestart", 1.0f);
	overlay->SizeEnd = num("sizeend", overlay->SizeStart);
	overlay->SizeRandom = std::clamp(num("sizerandom", 0.0f), 0.0f, 0.95f);
	overlay->AlphaStart = num("alphastart", 1.0f);
	overlay->AlphaEnd = num("alphaend", overlay->AlphaStart);
	overlay->FadeIn = std::clamp(num("fadein", 0.1f), 0.0f, 1.0f);
	overlay->FadeOut = std::clamp(num("fadeout", 0.2f), 0.0f, 1.0f);
	overlay->ColorStart = color("colorstart", 0xFFFFFF);
	overlay->ColorEnd = color("colorend", overlay->ColorStart);
	overlay->DotSize = std::max(0.25f, num("dotsize", 2.0f));
	overlay->Stretch = std::clamp(num("stretch", 0.0f), 0.0f, 2.0f);
	overlay->Prewarm = boolean("prewarm", true);

	overlay->IsPlainScreen = (type == "plainscreen");
	overlay->IsEllipse = (type == "ellipse");
	overlay->IsNoise = (type == "shadows" || type == "clouds" || type == "fog");
	overlay->NoiseKind = (type == "clouds") ? 1 : ((type == "fog") ? 2 : 0);
	overlay->IsRays = (type == "rays");
	overlay->IsAurora = (type == "aurora");
	overlay->IsTorch = (type == "torchlight");
	overlay->IsDistortion = (type == "distortion");
	if(overlay->IsDistortion) {
		// distorsion : amplitude en px NES, longueur d'onde en lignes NES, vitesse en cycles/s, phase en degres
		overlay->DistAmp1 = num("amp1", 0.0f); overlay->DistWave1 = std::max(1.0f, num("wave1", 32.0f));
		overlay->DistSpeed1 = num("speed1", 0.5f); overlay->DistPhase1 = num("phase1", 0.0f);
		overlay->DistAmp2 = num("amp2", 0.0f); overlay->DistWave2 = std::max(1.0f, num("wave2", 17.0f));
		overlay->DistSpeed2 = num("speed2", 0.3f); overlay->DistPhase2 = num("phase2", 0.0f);
		overlay->ZoneX0 = std::clamp(num("xmin", 0.0f), 0.0f, 1.0f); overlay->ZoneX1 = std::clamp(num("xmax", 1.0f), 0.0f, 1.0f);
		overlay->ZoneY0 = std::clamp(num("ymin", 0.0f), 0.0f, 1.0f); overlay->ZoneY1 = std::clamp(num("ymax", 1.0f), 0.0f, 1.0f);
	}
	if(overlay->IsPlainScreen || overlay->IsEllipse || overlay->IsTorch || overlay->IsNoise || overlay->IsRays || overlay->IsAurora) {
		overlay->ZoneX0 = std::clamp(num("xmin", 0.0f), 0.0f, 1.0f);
		overlay->ZoneX1 = std::clamp(num("xmax", 1.0f), 0.0f, 1.0f);
		overlay->ZoneY0 = std::clamp(num("ymin", 0.0f), 0.0f, 1.0f);
		overlay->ZoneY1 = std::clamp(num("ymax", 1.0f), 0.0f, 1.0f);
		overlay->FxNegative = boolean("negative", false);
		auto namedColor = [&](const char* k, uint32_t def) -> uint32_t {
			string v = StringUtilities::ToLower(StringUtilities::Trim(str(k)));
			if(v.empty()) return def;
			if(v == "black" || v == "noir") return 0x000000;
			if(v == "white" || v == "blanc") return 0xFFFFFF;
			if(v == "yellow" || v == "jaune") return 0xFFFF00;
			if(v == "green" || v == "vert") return 0x00FF00;
			if(v == "bleue" || v == "bleu" || v == "blue") return 0x0000FF;
			if(v == "red" || v == "rouge") return 0xFF0000;
			return color(k, def);
		};
		const uint32_t defCol = overlay->IsTorch ? 0xFFE0A0u : overlay->IsEllipse ? (overlay->FxNegative ? 0x000000 : 0xFFFFFF) : (type == "fog" ? 0xA8B8C8 : (type == "clouds" ? 0xFFFFFF : 0x000000));
		overlay->FxColor = namedColor("color", defCol);
		if(overlay->IsTorch) {
			overlay->TorchX = num("x", 128.0f);
			overlay->TorchY = num("y", 120.0f);
			overlay->TorchRadius = std::max(1.0f, num("radius", 48.0f));
			overlay->TorchSoft = std::clamp(num("softness", 0.5f), 0.01f, 1.0f);
			overlay->TorchDark = boolean("dark", true);
			overlay->TorchGlow = std::clamp(num("glow", 0.15f), 0.0f, 1.0f);
		}
		if(overlay->IsEllipse) {
			overlay->FxEllipseX = num("x", 128.0f);
			overlay->FxEllipseY = num("y", 120.0f);
			overlay->FxRadiusX = std::max(0.5f, num("radiusx", 32.0f));
			overlay->FxRadiusY = std::max(0.5f, num("radiusy", 16.0f));
		}
		if(overlay->IsRays) {
			overlay->RayX = std::clamp(num("rayx", 0.5f), 0.0f, 1.0f);
			overlay->RayY = std::clamp(num("rayy", 0.0f), 0.0f, 1.0f);
			overlay->RayCount = std::clamp((int)num("raycount", 8.0f), 1, 64);
			overlay->RayAngle = num("rayangle", 90.0f);
			overlay->RaySpread = std::clamp(num("rayspread", 60.0f), 0.0f, 360.0f);
			overlay->RayWidth = std::max(0.1f, num("raywidth", 4.0f));
			overlay->RayLength = std::max(0.01f, num("raylength", 0.8f));
			overlay->RaySpeed = num("rayspeed", 0.2f);
			overlay->RayWobble = num("raywobble", 3.0f);
			overlay->RayColor = namedColor("raycolor", 0xFFE8A0);
		}
		if(overlay->IsAurora) {
			overlay->AuroraSize = std::max(4.0f, num("size", 40.0f));
			overlay->AuroraSpeed = num("speed", 6.0f);
			overlay->AuroraCoverage = std::clamp(num("coverage", 0.5f), 0.0f, 1.0f);
			overlay->AuroraFall = std::clamp(num("fall", 0.7f), 0.05f, 1.0f);
			overlay->AuroraWave = std::max(0.0f, num("wave", 6.0f));
			overlay->AuroraColor1 = namedColor("color1", 0x3CFF8C);
			overlay->AuroraColor2 = namedColor("color2", 0x8C4BFF);
		}
		if(overlay->IsNoise) {
			overlay->FxSize = std::max(4.0f, num("size", 48.0f));
			overlay->FxSpeed = num("speed", 8.0f);
			overlay->FxDirection = num("direction", 0.0f);
			overlay->FxCoverage = std::clamp(num("coverage", 0.5f), 0.0f, 1.0f);
			overlay->FxSeed = (int)num("seed", 1.0f);
		}
	}

	string tex = StringUtilities::Trim(str("texture"));
	if(!tex.empty()) {
		HdPackBitmapInfo* found = nullptr;
		for(auto& bitmap : _data->ImageFileData) {
			if(bitmap->PngName == tex) {
				found = bitmap.get();
				break;
			}
		}
		if(found == nullptr) {
			_data->ImageFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
			HdPackBitmapInfo* bitmapInfo = _data->ImageFileData.back().get();
			bitmapInfo->PngName = tex;
			if(!LoadFile(tex, bitmapInfo->FileData)) {
				logError("2DOverlay texture not found: " + tex);
			}
			found = bitmapInfo;
		}
		overlay->Texture = found;
	}

	_data->Overlays.push_back(std::move(overlay));
}

void HdPackLoader::ProcessVideoTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	checkConstraint(tokens.size() >= 4, "Video tag should contain at least 4 parameters");

	string fileName = StringUtilities::Trim(tokens[0]);
	if(!CheckFile(fileName)) {
		logError("Video file not found: " + fileName);
		return;
	}

	auto video = std::make_unique<NesHdVideoInfo>();
	if(_loadFromZip) {
		video->Filename = std::string(VirtualFile(_hdPackFolder, fileName));
	} else {
		video->Filename = FolderUtilities::CombinePath(_hdPackFolder, fileName);
	}

	video->PauseMode = StringUtilities::ToLower(StringUtilities::Trim(tokens[1])) == "pause";

	std::string skipStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[2]));
	if(skipStr.find("start") != string::npos) video->SkipButtonsMask |= 0x1000;
	if(skipStr.find("a") != string::npos)     video->SkipButtonsMask |= 0x0080;
	if(skipStr.find("b") != string::npos)     video->SkipButtonsMask |= 0x8000;

	video->PlayOnlyOnce = ParseVideoBool(tokens[3]);

	if(tokens.size() >= 5) {
		video->MuteAudio = ParseVideoBool(tokens[4]);
	}

	if(tokens.size() >= 6) {
		std::string loopStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[5]));
		if(loopStr == "false" || loopStr == "0" || loopStr == "no" || loopStr == "n") {
			video->LoopTargetFrame = 0;
		} else if(loopStr == "true" || loopStr == "yes" || loopStr == "y" || loopStr == "1") {
			video->LoopTargetFrame = -1;
		} else {
			try {
				video->LoopTargetFrame = std::stoi(loopStr);
			} catch(...) {
				video->LoopTargetFrame = -1;
			}
		}
	}

	if(tokens.size() >= 8) {
		try {
			video->X = std::stoi(StringUtilities::Trim(tokens[6]));
			video->Y = std::stoi(StringUtilities::Trim(tokens[7]));
		} catch(...) {
			video->X = 0;
			video->Y = 0;
		}
	}

	if(tokens.size() >= 9) {
		try {
			int z = std::stoi(StringUtilities::Trim(tokens[8]));
			video->ZIndex = (uint8_t)std::clamp(z, 0, 255);
		} catch(...) {
			video->ZIndex = 99;
		}
	}

	if(tokens.size() >= 10) {
		std::string alphaStr = StringUtilities::ToLower(StringUtilities::Trim(tokens[9]));
		video->AlphaEnable = !(alphaStr == "false" || alphaStr == "0" || alphaStr == "no" || alphaStr == "n");
	}

	if(tokens.size() >= 11) {
		video->Stretch = ParseVideoBool(tokens[10]);
	}

	// Memes conditions que <background> (les autres ne sont pas evaluables sans pixel)
	for(HdPackCondition* condition : conditions) {
		switch(condition->GetConditionType()) {
			case HdPackConditionType::TileAtPos:
			case HdPackConditionType::SpriteAtPos:
			case HdPackConditionType::MemoryCheck:
			case HdPackConditionType::MemoryCheckConstant:
			case HdPackConditionType::FrameRange:
				video->Conditions.push_back(condition);
				break;

			default:
				logError("Invalid condition type for video: " + fileName);
				break;
		}
	}

	_data->Videos.push_back(std::move(video));
}

void HdPackLoader::ProcessBgmTag(vector<string> &tokens)
{
	checkConstraint(tokens.size() >= 3, "BGM tag should contain at least 3 parameters");
	checkConstraintEx(tokens.size() < 5, "BGM tag contains too many parameters");

	int trackId = ProcessSoundTrack(tokens[0], tokens[1], tokens[2]);
	if(trackId >= 0) {
		BgmTrackInfo track = {};
		if(tokens.size() > 3) {
			track.LoopPosition = (uint32_t)std::stoul(tokens[3]);
		}

		if(_loadFromZip) {
			track.Filename = 	VirtualFile(_hdPackFolder, tokens[2]);
		} else {
			track.Filename = FolderUtilities::CombinePath(_hdPackFolder, tokens[2]);
		}
		_data->BgmFilesById[trackId] = track;
	}
}

void HdPackLoader::ProcessSfxTag(vector<string> &tokens)
{
	checkConstraint(tokens.size() >= 3, "SFX tag should contain at least 3 parameters");
	checkConstraintEx(tokens.size() < 4, "SFX tag contains too many parameters");

	int trackId = ProcessSoundTrack(tokens[0], tokens[1], tokens[2]);
	if(trackId >= 0) {
		if(_loadFromZip) {
			VirtualFile file(_hdPackFolder, tokens[2]);
			_data->SfxFilesById[trackId] = file;
		} else {
			_data->SfxFilesById[trackId] = FolderUtilities::CombinePath(_hdPackFolder, tokens[2]);
		}
	}
}

vector<HdPackCondition*> HdPackLoader::ParseConditionString(string conditionString)
{
	FastString conditionName;
	vector<HdPackCondition*> conditions;
	conditions.reserve(3);

	auto processCondition = [&] {
		auto result = _conditionsByName.find(conditionName.ToString());
		if(result != _conditionsByName.end()) {
			conditions.push_back(result->second);
		} else {
			logError("Condition not found: " + string(conditionName.ToString()));
		}
		conditionName.Reset();
	};

	for(size_t i = 0, len = conditionString.size(); i < len; i++) {
		char c = conditionString[i];
		if(c == ' ' || c == '\n' || c == '\r' || c == '\t') {
			continue;
		} else if(c == '&') {
			processCondition();
		} else {
			conditionName.WriteSafe(c);
		}
	}

	processCondition();

	return conditions;
}

bool HdPackLoader::ParseBooleanValue(string value)
{
	// Y / N (historique) ; true / false, on / off, yes / no, 1 / 0 acceptes aussi (sans tenir compte de la casse)
	string v = value;
	std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	if(v == "y" || v == "yes" || v == "true" || v == "on" || v == "1") return true;
	if(v == "n" || v == "no" || v == "false" || v == "off" || v == "0") return false;
	logError("Invalid boolean value: " + value);
	return false;
}

void HdPackLoader::LoadCustomPalette()
{
	vector<uint8_t> fileData;
	if(LoadFile("palette.dat", fileData)) {
		vector<uint32_t> paletteData;

		for(size_t i = 0; i < fileData.size(); i+= 3){
			paletteData.push_back(0xFF000000 | (fileData[i] << 16) | (fileData[i+1] << 8) | fileData[i+2]);
		}

		if(paletteData.size() == 0x40) {
			_data->Palette = paletteData;
		}
	}
}

// <Ignore2DOverlays>tuile[,palette] : rattache l'ensemble de conditions de la regle aux tiles de meme cle
void HdPackLoader::ProcessIgnore2DTag(vector<string> &tokens, vector<HdPackCondition*> conditions)
{
	checkConstraint(!tokens.empty(), "Ignore2DOverlays tag needs a tile");
	auto rule = std::make_unique<HdIgnore2DRule>();
	string tileData = StringUtilities::Trim(tokens[0]);
	string palData = tokens.size() >= 2 ? StringUtilities::Trim(tokens[1]) : string("");
	string palLower = StringUtilities::ToLower(palData);
	rule->AnyPalette = palData.empty() || palLower == "*" || palLower == "all";
	if(rule->AnyPalette) {
		palData = "00000000";
	}
	ReadTileData(rule->Key, tileData, palData);
	rule->Conditions = conditions;
	_data->Ignore2DRules.push_back(std::move(rule));
	_data->HasIgnore2D = true;
}

void HdPackLoader::InitializeHdPack()
{
	// Regles ligne seule : rattachement aux tiles de meme cle (tuile, palette)
	for(auto& rule : _data->Ignore2DRules) {
		for(unique_ptr<HdPackTileInfo> &tileInfo : _data->Tiles) {
			bool same = (tileInfo->IsChrRamTile == rule->Key.IsChrRamTile);
			if(same) {
				same = tileInfo->IsChrRamTile
					? std::equal(tileInfo->TileData, tileInfo->TileData + 16, rule->Key.TileData)
					: (tileInfo->TileIndex == rule->Key.TileIndex);
			}
			if(same && !rule->AnyPalette) {
				same = (tileInfo->PaletteColors == rule->Key.PaletteColors);
			}
			if(same) {
				tileInfo->Ignore2DSets.push_back(rule->Conditions);
			}
		}
	}

	for(unique_ptr<HdPackTileInfo> &tileInfo : _data->Tiles) {
		auto tiles = _data->TileByKey.find(tileInfo->GetKey(false));
		if(tiles == _data->TileByKey.end()) {
			_data->TileByKey[tileInfo->GetKey(false)] = vector<HdPackTileInfo*>();
		}
		_data->TileByKey[tileInfo->GetKey(false)].push_back(tileInfo.get());

		if(tileInfo->DefaultTile) {
			auto tilesForDefaultKey = _data->TileByKey.find(tileInfo->GetKey(true));
			if(tilesForDefaultKey == _data->TileByKey.end()) {
				_data->TileByKey[tileInfo->GetKey(true)] = vector<HdPackTileInfo*>();
			}
			_data->TileByKey[tileInfo->GetKey(true)].push_back(tileInfo.get());
		}
	}
}

// <2DSpriteLight> (NES) : halo 2D au centre d'une tile de sprite.
// Syntaxe : [cond]<2DSpriteLight>TILE[,palette|*][,radius=48][,color=FFA040][,intensity=1.4][,flicker=0][,speed=9][,mode=add|shadow][,dx=0][,dy=0]
void HdPackLoader::ProcessSpriteLightTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	auto L = std::make_unique<NesHdSpriteLightInfo>();
	L->Conditions = conditions;
	vector<string> pos;
	for(const string& raw : tokens) {
		string tok = StringUtilities::Trim(raw);
		if(tok.empty()) continue;
		size_t eq = tok.find('=');
		if(eq == string::npos) {
			pos.push_back(tok);
			continue;
		}
		string k = StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)));
		string v = StringUtilities::Trim(tok.substr(eq + 1));
		try {
			if(k == "radius" || k == "rayon") L->Radius = std::max(1.0f, std::stof(v));
			else if(k == "color" || k == "couleur") { if(!v.empty() && v[0] == '#') v = v.substr(1); L->Color = (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; }
			else if(k == "intensity" || k == "intensite") L->Intensity = std::max(0.0f, std::stof(v));
			else if(k == "flicker" || k == "scintillement") L->Flicker = std::clamp(std::stof(v), 0.0f, 1.0f);
			else if(k == "speed" || k == "flickerspeed" || k == "vitesse") L->FlickerSpeed = std::stof(v);
			else if(k == "mode") { string m = StringUtilities::ToLower(v); L->Shadow = (m == "shadow" || m == "sub" || m == "ombre"); }
			else if(k == "dx" || k == "offsetx") L->OffsetX = std::stof(v);
			else if(k == "dy" || k == "offsety") L->OffsetY = std::stof(v);
			else { logError("2DSpriteLight : cle inconnue : " + k); }
		} catch(...) {
			logError("2DSpriteLight : valeur invalide pour " + k + " : " + v);
		}
	}
	if(pos.empty()) {
		logError("2DSpriteLight : tile manquante.");
		return;
	}
	string tileData = pos[0];
	string palData = pos.size() >= 2 ? StringUtilities::ToLower(pos[1]) : string("*");
	L->AnyPalette = (palData == "*" || palData == "all");
	if(L->AnyPalette) palData = "0";
	try {
		ReadTileData(L->Key, tileData, palData);
	} catch(...) {
		logError("2DSpriteLight : tile invalide : " + tileData);
		return;
	}
	MessageManager::Log("[HDPack] <2DSpriteLight> tile " + tileData + ", rayon " + std::to_string((int)L->Radius));
	_data->Sprite2DLights.push_back(std::move(L));
}

// <watchMemoryAddress>nom,adresse[,taille][,signed] : valeur RAM CPU nommee, lue une fois par image (diorama, doc 32 s0.1).
// Adresse en hexadecimal (0x ou $ facultatifs), taille 1 ou 2 octets (petit-boutiste). Lecture seule, sans condition.
void HdPackLoader::ProcessWatchMemoryAddressTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(!conditions.empty()) {
		logError("watchMemoryAddress : les conditions ne sont pas prises en charge, ligne ignoree.");
		return;
	}
	if(tokens.size() < 2 || tokens.size() > 4) {
		logError("watchMemoryAddress : syntaxe nom,adresse[,taille][,signed], ligne ignoree.");
		return;
	}
	HdWatchedValue w;
	w.Name = StringUtilities::Trim(tokens[0]);
	if(w.Name.empty()) {
		logError("watchMemoryAddress : nom vide, ligne ignoree.");
		return;
	}
	for(char c : w.Name) {
		if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
			logError("watchMemoryAddress : nom invalide (lettres, chiffres et _ seulement) : " + w.Name);
			return;
		}
	}
	string addr = StringUtilities::Trim(tokens[1]);
	if(!addr.empty() && addr[0] == '$') {
		addr = addr.substr(1);
	} else if(addr.size() > 2 && addr[0] == '0' && (addr[1] == 'x' || addr[1] == 'X')) {
		addr = addr.substr(2);
	}
	if(addr.empty() || addr.size() > 4 || addr.find_first_not_of("0123456789abcdefABCDEF") != string::npos) {
		logError("watchMemoryAddress : adresse hexadecimale invalide (0000 a FFFF) : " + tokens[1]);
		return;
	}
	w.Address = HexUtilities::FromHex(addr);
	for(size_t i = 2; i < tokens.size(); i++) {
		string t = StringUtilities::ToLower(StringUtilities::Trim(tokens[i]));
		if(t == "signed") {
			w.Signed = true;
		} else if(t == "unsigned") {
			w.Signed = false;
		} else if(t == "1") {
			w.Size = 1;
		} else if(t == "2") {
			w.Size = 2;
		} else {
			logError("watchMemoryAddress : parametre inconnu (taille 1 ou 2, signed) : " + t);
			return;
		}
	}
	if(w.Address + w.Size - 1 > 0xFFFF) {
		logError("watchMemoryAddress : adresse + taille depasse FFFF : " + w.Name);
		return;
	}
	for(const HdWatchedValue& other : _data->WatchedValues) {
		if(other.Name == w.Name) {
			logError("watchMemoryAddress : nom deja declare, ligne ignoree : " + w.Name);
			return;
		}
	}
	for(uint32_t i = 0; i < w.Size; i++) {
		_data->WatchedMemoryAddresses.emplace(w.Address + i);
	}
	_data->WatchedValues.push_back(w);
	_data->PackFolder = _hdPackFolder;
}

// Charge (ou retrouve) un PNG de motif de mur du diorama (G3b2). Les pixels sont decodes plus tard (HdPackBitmapInfo::Init).
HdPackBitmapInfo* HdPackLoader::LoadDioramaTexture(const string& filename)
{
	const string name = StringUtilities::Trim(filename);
	if(name.empty()) {
		logError("Diorama : nom de fichier de texture vide.");
		return nullptr;
	}
	auto found = _backgroundsByName.find(name);
	if(found != _backgroundsByName.end()) {
		return found->second;
	}
	_data->BackgroundFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
	HdPackBitmapInfo* bitmap = _data->BackgroundFileData.back().get();
	bitmap->PngName = name;
	if(!LoadFile(name, bitmap->FileData)) {
		_data->BackgroundFileData.pop_back();
		logError("Diorama : texture introuvable : " + name);
		return nullptr;
	}
	_backgroundsByName[name] = bitmap;
	return bitmap;
}

// <DioramaDepth>z=..,t=..[,rect=x,y,l,h] (doc 38) : groupe de profondeur. Cles : z (pixels NES, positif = derriere, defaut 0),
// t (epaisseur en tiles, 0 a 8, defaut 0), rect=x,y,largeur,hauteur (zone d'ecran en pixels NES, 4 valeurs separees par des
// virgules). Forme bloc : <DioramaDepth>z=..,t=..{ <tile>... } (voir LoadPack). Les tiles de fond du groupe quittent la couche
// L2 et vont dans une couche a part ; les sprites ne sont pas concernes par rect=. 4 groupes au plus.
static bool DioParseFloat(const string& s, float& out)
{
	try {
		size_t used = 0;
		const float v = std::stof(s, &used);
		if(used != s.size()) return false;
		out = v;
		return true;
	} catch(...) {
		return false;
	}
}

static bool DioParseInt(const string& s, int& out)
{
	try {
		size_t used = 0;
		const int v = std::stoi(s, &used);
		if(used != s.size()) return false;
		out = v;
		return true;
	} catch(...) {
		return false;
	}
}

// Cle de mur sans indice de couche : side, top (PNG), stretchx / stretchy (dessus et flancs), topstretchx, topstretchy,
// sidestretchx, sidestretchy. Etirement de 0.01 a 100, 1 = un pixel du PNG pour un pixel HD. Renvoie faux si la cle n'est pas
// une cle de mur.
bool HdPackLoader::ParseDioramaWallOption(const string& base, const string& val, HdDioramaWallTex& wall)
{
	if(base == "side") {
		// G3b3 : side=volume.png:N = bande de N images empilees de haut en bas (image 0 en surface, N-1 au fond)
		string name = val;
		int n = 0;
		const size_t colon = val.rfind(':');
		int parsed = 0;
		int mode = 0;
		string numPart = colon != string::npos ? StringUtilities::Trim(val.substr(colon + 1)) : string();
		if(!numPart.empty() && (numPart.back() == 'h' || numPart.back() == 'H' || numPart.back() == 'v' || numPart.back() == 'V')) {
			mode = (numPart.back() == 'h' || numPart.back() == 'H') ? 1 : 2;
			numPart.pop_back();
		}
		if(colon != string::npos && !numPart.empty() && DioParseInt(numPart, parsed)) {
			if(parsed < 1 || parsed > 64) {
				logError("Diorama : side=fichier:N, N doit etre de 1 a 64 : " + val);
				return true;
			}
			n = parsed;
			name = val.substr(0, colon);
		}
		wall.Side = LoadDioramaTexture(name);
		wall.SideVol = (wall.Side && n > 1) ? n : 0;
		wall.SideVolMode = mode;
		return true;
	}
	if(base == "top") {
		wall.Top = LoadDioramaTexture(val);
		return true;
	}
	float* targets[2] = { nullptr, nullptr };
	if(base == "stretchx") { targets[0] = &wall.TopStretchX; targets[1] = &wall.SideStretchX; }
	else if(base == "stretchy") { targets[0] = &wall.TopStretchY; targets[1] = &wall.SideStretchY; }
	else if(base == "topstretchx") { targets[0] = &wall.TopStretchX; }
	else if(base == "topstretchy") { targets[0] = &wall.TopStretchY; }
	else if(base == "sidestretchx") { targets[0] = &wall.SideStretchX; }
	else if(base == "sidestretchy") { targets[0] = &wall.SideStretchY; }
	else {
		return false;
	}
	float s = 1.0f;
	if(StringUtilities::ToLower(val) == "adapt") {
		s = -1.0f; // la texture couvre exactement la zone (voir HdDioramaWallTex)
	} else if(!DioParseFloat(val, s) || s < 0.01f || s > 100.0f) {
		logError("Diorama : " + base + " doit etre adapt ou un nombre de 0.01 a 100 : " + val);
		return true;
	}
	for(float* t : targets) {
		if(t) *t = s;
	}
	return true;
}

bool HdPackLoader::ParseDioramaDepthOptions(vector<string>& tokens, HdDioramaDepthGroup& group)
{
	for(size_t i = 0; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) {
			continue;
		}
		const size_t eq = opt.find('=');
		if(eq == string::npos) {
			logError("DioramaDepth : option sans '=' : " + opt);
			return false;
		}
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string val = StringUtilities::Trim(opt.substr(eq + 1));
		if(key == "z") {
			if(!DioParseFloat(val, group.Z)) {
				logError("DioramaDepth : valeur invalide pour z : " + val);
				return false;
			}
		} else if(key == "t") {
			if(!DioParseFloat(val, group.T)) {
				logError("DioramaDepth : valeur invalide pour t : " + val);
				return false;
			}
		} else if(ParseDioramaWallOption(key, val, group.Wall)) {
			// cle de mur reconnue : side, top, stretchx, stretchy, topstretchx, topstretchy, sidestretchx, sidestretchy
		} else if(key == "worldtex") {
			group.WorldTex = (val == "on" || val == "true" || val == "1"); // doc 53 : fond L0 projete sur le groupe
		} else if(key == "rect") {
			int v[4] = {};
			bool ok = DioParseInt(val, v[0]) && i + 3 < tokens.size();
			for(size_t k = 1; ok && k < 4; k++) {
				ok = DioParseInt(StringUtilities::Trim(tokens[i + k]), v[k]);
			}
			if(!ok) {
				logError("DioramaDepth : syntaxe rect=x,y,largeur,hauteur (4 entiers).");
				return false;
			}
			i += 3;
			const int x0 = std::max(0, v[0]), y0 = std::max(0, v[1]);
			const int x1 = std::min(256, v[0] + v[2]), y1 = std::min(240, v[1] + v[3]);
			if(v[2] <= 0 || v[3] <= 0 || x1 <= x0 || y1 <= y0) {
				logError("DioramaDepth : rect vide ou hors de l'ecran (256 x 240).");
				return false;
			}
			group.HasRect = true;
			group.X = x0; group.Y = y0; group.W = x1 - x0; group.H = y1 - y0;
		} else {
			logError("DioramaDepth : option inconnue : " + key);
			return false;
		}
	}
	if(group.T < 0.0f || group.T > 16.0f) {
		logError("DioramaDepth : t limite a 0..16 tiles.");
		group.T = group.T < 0.0f ? 0.0f : 16.0f;
	}
	if(group.Z < -230.0f) {
		logError("DioramaDepth : z trop proche de la camera, limite a -230.");
		group.Z = -230.0f;
	}
	return true;
}

// Ajoute le groupe, renvoie son numero (1 a 4) ou 0 s'il y en a deja 4.
int HdPackLoader::AddDioramaDepthGroup(const HdDioramaDepthGroup& group)
{
	if(_data->DioramaDepthGroups.size() >= 8) {
		logError("DioramaDepth : 8 groupes au maximum, groupe ignore.");
		return 0;
	}
	_data->DioramaDepthGroups.push_back(group);
	return (int)_data->DioramaDepthGroups.size();
}

// Forme ligne seule : <DioramaDepth>z=..,t=..,rect=x,y,l,h (la forme bloc est lue dans LoadPack).
void HdPackLoader::ProcessDioramaDepthTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(!conditions.empty()) {
		logError("DioramaDepth : les conditions ne sont pas prises en charge sur cette ligne, ligne ignoree.");
		return;
	}
	HdDioramaDepthGroup group;
	if(!ParseDioramaDepthOptions(tokens, group)) {
		return;
	}
	if(!group.HasRect) {
		logError("DioramaDepth : sans bloc { }, rect=x,y,largeur,hauteur est obligatoire, ligne ignoree.");
		return;
	}
	AddDioramaDepthGroup(group);
}

// <DioramaExtend>side=..,layer=..,mode=..,size=..,band=.. (doc 40, G4a). side : top, bottom, left, right, liste avec +, ou all ;
// layer : 0 a 3 (L0..L3, defaut 2) ou g1 a g4 (groupes <DioramaDepth>) ; edge : debut reel du contenu (pixels NES), un seul cote ; mode : stretch (ou clamp), repeat, mirror, wrap ; size : marge en pixels NES (1 a 512, defaut
// 128) ; band : nombre de tiles NES repetees pour repeat (1 a 16, defaut 1). Les conditions [ ] sont evaluees a chaque image.
// rows=a-b (cotes left / right) ou cols=a-b (cotes top / bottom) : plage de l'ecran ou l'extension s'applique. fog=0..1 (force de la brume au bout de fogdist), fogdist=pixels NES (defaut : la marge), color=RRGGBB (couleur de la brume ; sans color la brume rend transparent) ; mode=color = aplat de la couleur color.
void HdPackLoader::ProcessDioramaExtendTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	HdDioramaExtendRule rule;
	rule.Conditions = conditions;
	bool hasMode = false;
	for(size_t i = 0; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) {
			continue;
		}
		const size_t eq = opt.find('=');
		if(eq == string::npos) {
			logError("DioramaExtend : option sans '=' : " + opt + ", ligne ignoree.");
			return;
		}
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string val = StringUtilities::Trim(opt.substr(eq + 1));
		const string lval = StringUtilities::ToLower(val);
		int n = 0;
		if(key == "side") {
			rule.Side = 0;
			size_t start = 0;
			while(start <= lval.size()) {
				size_t plus = lval.find('+', start);
				const string part = StringUtilities::Trim(lval.substr(start, plus == string::npos ? string::npos : plus - start));
				if(part == "top") rule.Side |= 1;
				else if(part == "bottom") rule.Side |= 2;
				else if(part == "left") rule.Side |= 4;
				else if(part == "right") rule.Side |= 8;
				else if(part == "all") rule.Side |= 15;
				else {
					logError("DioramaExtend : side inconnu (top, bottom, left, right, all) : " + part + ", ligne ignoree.");
					return;
				}
				if(plus == string::npos) break;
				start = plus + 1;
			}
		} else if(key == "layer") {
			if(lval.size() == 2 && lval[0] == 'g' && lval[1] >= '1' && lval[1] <= '8') {
				rule.Layer = 4 + (lval[1] - '1'); // groupe <DioramaDepth> numero 1 a 8 (cotes gauche et droit seulement)
			} else if(DioParseInt(lval, n) && n >= 0 && n <= 3) {
				rule.Layer = n;
			} else {
				logError("DioramaExtend : layer doit etre de 0 a 3 ou g1 a g8 : " + val + ", ligne ignoree.");
				return;
			}
		} else if(key == "mode") {
			if(lval == "stretch" || lval == "clamp") rule.Mode = 1;
			else if(lval == "repeat") rule.Mode = 2;
			else if(lval == "mirror") rule.Mode = 3;
			else if(lval == "wrap") rule.Mode = 4;
			else if(lval == "color") rule.Mode = 5;
			else {
				logError("DioramaExtend : mode inconnu (stretch, repeat, mirror, wrap, color) : " + val + ", ligne ignoree.");
				return;
			}
			hasMode = true;
		} else if(key == "size") {
			if(!DioParseInt(lval, n) || n < 1 || n > 512) {
				logError("DioramaExtend : size doit etre de 1 a 512 pixels NES : " + val + ", ligne ignoree.");
				return;
			}
			rule.Size = n;
		} else if(key == "band") {
			if(!DioParseInt(lval, n) || n < 1 || n > 16) {
				logError("DioramaExtend : band doit etre de 1 a 16 tiles : " + val + ", ligne ignoree.");
				return;
			}
			rule.Band = n;
		} else if(key == "edge") {
			if(!DioParseInt(lval, n) || n < 0 || n > 512) {
				logError("DioramaExtend : edge doit etre une position de 0 a 512 pixels NES : " + val + ", ligne ignoree.");
				return;
			}
			rule.Edge = n;
		} else if(key == "rows" || key == "cols") {
			// plage a-b (ou a,b : le lecteur de ligne coupe sur la virgule, le nombre suivant est repris)
			int a = 0, b = 0;
			bool okRange = false;
			const size_t dash = lval.find('-', 1);
			if(dash != string::npos) {
				okRange = DioParseInt(StringUtilities::Trim(lval.substr(0, dash)), a) && DioParseInt(StringUtilities::Trim(lval.substr(dash + 1)), b);
			} else if(DioParseInt(lval, a) && i + 1 < tokens.size() && DioParseInt(StringUtilities::Trim(tokens[i + 1]), b)) {
				okRange = true;
				i++;
			}
			if(!okRange || a < 0 || b > 512 || a >= b) {
				logError("DioramaExtend : " + key + " doit etre une plage a-b de pixels NES (0 a 512, a < b) : " + val + ", ligne ignoree.");
				return;
			}
			rule.RangeMin = a;
			rule.RangeMax = b;
			rule.RangeIsRows = (key == "rows");
		} else if(key == "fog") {
			char* endp = nullptr;
			const double v = strtod(lval.c_str(), &endp);
			if(lval.empty() || endp == lval.c_str() || *endp != 0 || v < 0.0 || v > 1.0) {
				logError("DioramaExtend : fog doit etre un nombre de 0 a 1 : " + val + ", ligne ignoree.");
				return;
			}
			rule.Fog = (float)v;
		} else if(key == "fogdist") {
			if(!DioParseInt(lval, n) || n < 1 || n > 512) {
				logError("DioramaExtend : fogdist doit etre de 1 a 512 pixels NES : " + val + ", ligne ignoree.");
				return;
			}
			rule.FogDist = n;
		} else if(key == "color") {
			string hex = lval;
			if(!hex.empty() && hex[0] == '#') hex = hex.substr(1);
			char* endp = nullptr;
			const unsigned long v = hex.size() == 6 ? strtoul(hex.c_str(), &endp, 16) : 0;
			if(hex.size() != 6 || endp == hex.c_str() || *endp != 0) {
				logError("DioramaExtend : color doit etre RRGGBB (6 chiffres hexadecimaux) : " + val + ", ligne ignoree.");
				return;
			}
			rule.Color = (uint32_t)v;
			rule.HasColor = true;
		} else {
			logError("DioramaExtend : option inconnue : " + key + ", ligne ignoree.");
			return;
		}
	}
	if(rule.Mode == 5 && !rule.HasColor) {
		logError("DioramaExtend : mode=color demande color=RRGGBB, ligne ignoree.");
		return;
	}
	if(rule.Side == 0 || !hasMode) {
		logError("DioramaExtend : side= et mode= sont obligatoires, ligne ignoree.");
		return;
	}
	if(rule.RangeMin >= 0) {
		if(rule.RangeIsRows && (rule.Side & 3)) {
			logError("DioramaExtend : rows= ne vaut que pour les cotes left et right (cols= pour top et bottom), ligne ignoree.");
			return;
		}
		if(!rule.RangeIsRows && (rule.Side & 12)) {
			logError("DioramaExtend : cols= ne vaut que pour les cotes top et bottom (rows= pour left et right), ligne ignoree.");
			return;
		}
	}
	if(rule.Edge >= 0 && rule.Side != 1 && rule.Side != 2 && rule.Side != 4 && rule.Side != 8) {
		logError("DioramaExtend : edge= demande un seul cote dans side= (top, bottom, left ou right), ligne ignoree.");
		return;
	}
	if(_data->DioramaExtendRules.size() >= 16) {
		logError("DioramaExtend : 16 regles au plus, ligne ignoree.");
		return;
	}
	_data->DioramaExtendRules.push_back(rule);
}

static bool LightParseFloat(const string& v, float& out);

// PNG charge a part : il ne doit pas entrer dans ImageFileData, dont l'ordre (les <img>) numerote les images des <tile>.
HdPackBitmapInfo* HdPackLoader::LoadAuxBitmap(string name)
{
	string resolved;
	if(!ResolveRandomPath(name, resolved)) {
		return nullptr;
	}
	for(auto& b : _data->ImageFileData) {
		if(b->PngName == resolved) return b.get();
	}
	for(auto& b : _data->AuxBitmaps) {
		if(b->PngName == resolved) return b.get();
	}
	unique_ptr<HdPackBitmapInfo> bi(new HdPackBitmapInfo());
	if(!LoadFile(resolved, bi->FileData)) {
		logError("Error loading HDPack: PNG file " + resolved + " could not be read.");
		return nullptr;
	}
	bi->PngName = resolved;
	HdPackBitmapInfo* ptr = bi.get();
	_data->AuxBitmaps.push_back(std::move(bi));
	return ptr;
}

// H1 : lecture d'un .obj (v, vt, vn, f ; polygones triangules en eventail, indices negatifs acceptes). Y vers le haut, v de texture
// retourne (v = 1 - v dans le fichier). Sans normales dans le fichier : normale de la face. Le .mtl est ignore.
static std::shared_ptr<vector<float>> HdLoadObjMesh(const vector<uint8_t>& raw, string& err, std::map<string, std::shared_ptr<vector<float>>>* byMat = nullptr)
{
	vector<float> vp, vn, vt;
	auto out = std::make_shared<vector<float>>();
	std::istringstream in(string(raw.begin(), raw.end()));
	string line;
	string curMat; // usemtl courant (cle des parties par materiau)
	auto resolve = [](long i, size_t count) -> long { return i > 0 ? i - 1 : (long)count + i; };
	while(std::getline(in, line)) {
		if(!line.empty() && line.back() == '\r') line.pop_back();
		std::istringstream ls(line);
		string k;
		ls >> k;
		if(k == "v") { float a = 0, b = 0, c = 0; ls >> a >> b >> c; vp.push_back(a); vp.push_back(b); vp.push_back(c); }
		else if(k == "vn") { float a = 0, b = 0, c = 0; ls >> a >> b >> c; vn.push_back(a); vn.push_back(b); vn.push_back(c); }
		else if(k == "vt") { float a = 0, b = 0; ls >> a >> b; vt.push_back(a); vt.push_back(1.0f - b); }
		else if(k == "usemtl") { string m; ls >> m; curMat = m; }
		else if(k == "f") {
			struct Idx { long v = -1, t = -1, n = -1; };
			vector<Idx> face;
			string tk;
			while(ls >> tk) {
				Idx ix;
				const size_t s1 = tk.find('/');
				ix.v = resolve(std::strtol(tk.c_str(), nullptr, 10), vp.size() / 3);
				if(s1 != string::npos) {
					const size_t s2 = tk.find('/', s1 + 1);
					const string ts = tk.substr(s1 + 1, s2 == string::npos ? string::npos : s2 - s1 - 1);
					if(!ts.empty()) ix.t = resolve(std::strtol(ts.c_str(), nullptr, 10), vt.size() / 2);
					if(s2 != string::npos && s2 + 1 < tk.size()) ix.n = resolve(std::strtol(tk.c_str() + s2 + 1, nullptr, 10), vn.size() / 3);
				}
				if(ix.v < 0 || (size_t)ix.v >= vp.size() / 3) { err = "indice de sommet hors limites dans une face"; return nullptr; }
				face.push_back(ix);
			}
			for(size_t i = 1; i + 1 < face.size(); i++) {
				const Idx tri[3] = { face[0], face[i], face[i + 1] };
				float fn[3] = { 0, 0, 1 };
				{ // normale de la face
					const float* a = &vp[tri[0].v * 3]; const float* b = &vp[tri[1].v * 3]; const float* c = &vp[tri[2].v * 3];
					const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], wx = c[0] - a[0], wy = c[1] - a[1], wz = c[2] - a[2];
					float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
					const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
					if(l > 1e-12f) { fn[0] = nx / l; fn[1] = ny / l; fn[2] = nz / l; }
				}
				const size_t before = out->size();
				for(int c = 0; c < 3; c++) {
					const Idx& ix = tri[c];
					out->push_back(vp[ix.v * 3]); out->push_back(vp[ix.v * 3 + 1]); out->push_back(vp[ix.v * 3 + 2]);
					if(ix.n >= 0 && (size_t)ix.n < vn.size() / 3) { out->push_back(vn[ix.n * 3]); out->push_back(vn[ix.n * 3 + 1]); out->push_back(vn[ix.n * 3 + 2]); }
					else { out->push_back(fn[0]); out->push_back(fn[1]); out->push_back(fn[2]); }
					if(ix.t >= 0 && (size_t)ix.t < vt.size() / 2) { out->push_back(vt[ix.t * 2]); out->push_back(vt[ix.t * 2 + 1]); }
					else { out->push_back(0.0f); out->push_back(0.0f); }
				}
				if(byMat) {
					auto& dst = (*byMat)[curMat];
					if(!dst) dst = std::make_shared<vector<float>>();
					dst->insert(dst->end(), out->begin() + before, out->end());
				}
				if(out->size() / 8 > 300000) { err = "modele trop gros (300000 sommets au plus)"; return nullptr; }
			}
		}
	}
	if(out->size() < 24) { err = "aucune face lue"; return nullptr; }
	return out;
}

// H1 : expression de position "$page*256+$x" -> HdDioramaExpr (les valeurs $nom viennent de <watchMemoryAddress>, declare avant)
static bool HdParseDioramaExpr(const string& text, const vector<HdWatchedValue>& watched, HdDioramaExpr& out, string& err)
{
	string e;
	for(char c : text) if(c != ' ' && c != '\t') e += c;
	if(e.empty()) { err = "expression vide"; return false; }
	size_t i = 0;
	while(i < e.size()) {
		HdDioramaExpr::Term term;
		if(e[i] == '+') i++;
		else if(e[i] == '-') { term.Sign = -1.0f; i++; }
		size_t j = i;
		while(j < e.size() && e[j] != '+' && e[j] != '-') j++;
		const string t = e.substr(i, j - i);
		if(t.empty()) { err = "terme vide"; return false; }
		size_t a = 0;
		while(a <= t.size()) {
			size_t b = t.find('*', a);
			if(b == string::npos) b = t.size();
			const string f = t.substr(a, b - a);
			if(f.empty()) { err = "facteur vide"; return false; }
			HdDioramaExpr::Factor fac;
			if(f[0] == '$') {
				const string nm = f.substr(1);
				int idx = -1;
				for(size_t k = 0; k < watched.size(); k++) if(watched[k].Name == nm) { idx = (int)k; break; }
				if(idx < 0) { err = "valeur $" + nm + " inconnue (declarer <watchMemoryAddress>" + nm + ",adresse,taille avant cette ligne)"; return false; }
				fac.Var = idx;
			} else {
				char* endp = nullptr;
				fac.Val = std::strtof(f.c_str(), &endp);
				if(endp == f.c_str() || *endp != 0) { err = "nombre ou $nom attendu : " + f; return false; }
			}
			term.F.push_back(fac);
			a = b + 1;
		}
		out.T.push_back(term);
		i = j;
	}
	return true;
}

// <DioramaObject>fichier.obj,tex=image.png,x=,y=,z=,scale=,rotx=,roty=,rotz=,spin=,color= (doc 45, H1) : objet 3D de decor, soumis a [cond].
void HdPackLoader::ProcessDioramaObjectTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty()) {
		logError("DioramaObject : fichier .obj attendu en premier, ligne ignoree.");
		return;
	}
	for(const string& t : tokens) { // anchor=bgN : objet du monde (repere du fond), meme moteur que <Tile2Obj> (glTF, animation, ombre)
		const string lt = StringUtilities::ToLower(StringUtilities::Trim(t));
		if(lt.rfind("anchor=", 0) == 0) { ProcessDioramaTile2ObjTag(tokens, conditions, false, true); return; }
	}
	HdDioramaObjectRule r;
	r.Conditions = conditions;
	const string file = StringUtilities::Trim(tokens[0]);
	string texName;
	for(size_t i = 1; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		if(eq == string::npos) { logError("DioramaObject : option sans '=' : " + opt + ", ligne ignoree."); return; }
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string raw = StringUtilities::Trim(opt.substr(eq + 1));
		const string val = StringUtilities::ToLower(raw);
		float f = 0.0f;
		if(key == "tex") { texName = raw; }
		else if(key == "lit") { r.Lit = !(val == "off" || val == "0" || val == "false"); }
		else if(key == "shadow") { r.Shadow = !(val == "off" || val == "0" || val == "false"); }
		else if(key == "occlude") { r.Occlude = (val == "on" || val == "1" || val == "true"); }
		else if(key == "scrollx" || key == "scrolly") {
			string err;
			if(!HdParseDioramaExpr(raw, _data->WatchedValues, key == "scrollx" ? r.ScrollX : r.ScrollY, err)) { logError("DioramaObject : " + key + " : " + err + ", ligne ignoree."); return; }
		}
		else if(key == "color") {
			string h = val;
			if(!h.empty() && h[0] == '#') h = h.substr(1);
			char* endp = nullptr;
			const unsigned long v = h.size() == 6 ? strtoul(h.c_str(), &endp, 16) : 0;
			if(h.size() != 6 || endp == h.c_str() || *endp != 0) { logError("DioramaObject : color doit etre RRGGBB : " + val + ", ligne ignoree."); return; }
			r.Color = (uint32_t)v;
		}
		else {
			if(!LightParseFloat(val, f)) { logError("DioramaObject : valeur numerique attendue pour " + key + " : " + val + ", ligne ignoree."); return; }
			if(key == "x") r.X = f;
			else if(key == "y") r.Y = f;
			else if(key == "z") r.Z = f;
			else if(key == "refz") { if(f < -4096.0f || f > 4096.0f) { logError("DioramaObject : refz doit etre de -4096 a 4096 pixels NES : " + val + ", ligne ignoree."); return; } r.RefZ = f; }
			else if(key == "scale") { if(f <= 0.0f || f > 4096.0f) { logError("DioramaObject : scale doit etre de 0 a 4096 : " + val + ", ligne ignoree."); return; } r.Scale = f; }
			else if(key == "rotx") r.RotX = f;
			else if(key == "roty") r.RotY = f;
			else if(key == "rotz") r.RotZ = f;
			else if(key == "spin") r.Spin = f;
			else if(key == "bright") { if(f < 0.0f || f > 8.0f) { logError("DioramaObject : bright doit etre de 0 a 8 : " + val + ", ligne ignoree."); return; } r.Bright = f; }
			else if(key == "window") { if(f < 0.0f || f > 4096.0f) { logError("DioramaObject : window doit etre de 0 a 4096 pixels NES : " + val + ", ligne ignoree."); return; } r.HasWindow = true; r.Window = f; }
			else { logError("DioramaObject : option inconnue : " + key + ", ligne ignoree."); return; }
		}
	}
	auto cached = _dioObjCache.find(file);
	if(cached != _dioObjCache.end()) {
		r.Mesh = cached->second;
	} else {
		vector<uint8_t> raw;
		if(!LoadFile(file, raw)) { logError("DioramaObject : fichier introuvable : " + file + ", ligne ignoree."); return; }
		string err;
		r.Mesh = HdLoadObjMesh(raw, err);
		if(!r.Mesh) { logError("DioramaObject : " + file + " : " + err + ", ligne ignoree."); return; }
		_dioObjCache[file] = r.Mesh;
		MessageManager::Log("[HDPack] <DioramaObject> " + file + " : " + std::to_string(r.Mesh->size() / 24) + " triangle(s)");
	}
	{
		float m2 = 0.0f; // rayon du modele autour de son origine
		for(size_t i = 0; i + 7 < r.Mesh->size(); i += 8) {
			const float* v = r.Mesh->data() + i;
			m2 = std::max(m2, v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		}
		r.Radius = std::sqrt(m2) * r.Scale;
	}
	if(!texName.empty()) {
		r.Tex = LoadAuxBitmap(texName);
		if(!r.Tex) return; // erreur deja journalisee
	}
	_data->DioramaObjectRules.push_back(r);
}

// <Tile2Obj> (doc 53) : <Tile2Obj>modele.obj,tile=HASH[,palette=*|RRGGBBAA][,tex=..][,color=RRGGBB][,scale=..|height=..][,elevation=..][,rotation=..]
// Etape 1 : lecture seule (la regle est stockee). Le rendu des instances vient ensuite.
void HdPackLoader::ProcessDioramaTile2ObjTag(vector<string>& tokens, vector<HdPackCondition*> conditions, bool sprite, bool world)
{
	if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty()) {
		logError("Tile2Obj : fichier .obj attendu en premier, ligne ignoree.");
		return;
	}
	HdDioramaTile2ObjRule r;
	r.Conditions = conditions;
	const string file = StringUtilities::Trim(tokens[0]);
	string texName, animName;
	bool hasTile = false;
	for(size_t i = 1; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		if(eq == string::npos) { logError("Tile2Obj : option sans '=' : " + opt + ", ligne ignoree."); return; }
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string raw = StringUtilities::Trim(opt.substr(eq + 1));
		const string val = StringUtilities::ToLower(raw);
		float f = 0.0f;
		if(sprite && (key == "tile" || key == "group")) { // <Sprite2Obj> : tile=H1|H2|.. (toutes les tiles du personnage, toutes etapes d'animation)
			r.Group.clear();
			string part;
			for(size_t k = 0; k <= raw.size(); k++) {
				if(k == raw.size() || raw[k] == '|') {
					const string hs = StringUtilities::ToLower(StringUtilities::Trim(part));
					char* endp = nullptr;
					const unsigned long gv = strtoul(hs.c_str(), &endp, 16);
					if(hs.empty() || endp == hs.c_str() || *endp != 0 || gv > 0xFFFFul) { logError("Sprite2Obj : tile doit etre une liste de hashs hexa separes par | : " + raw + ", ligne ignoree."); return; }
					r.Group.push_back((uint32_t)gv);
					part.clear();
				} else {
					part += raw[k];
				}
			}
			hasTile = true;
		} else if(sprite && key == "id") {
			r.Id = raw;
		} else if(key == "anim") {
			animName = raw;
		} else if(world && key == "anchor") { // anchor=bgN : repere du fond de priorite N
			char* endp = nullptr;
			const string num = (val.size() > 2 && val.substr(0, 2) == "bg") ? val.substr(2) : val;
			const long p = strtol(num.c_str(), &endp, 10);
			if(num.empty() || *endp != 0 || p < 0 || p > 49) { logError("DioramaObject : anchor doit etre bgN (N = priorite du <background>, 0 a 49) : " + val + ", ligne ignoree."); return; }
			r.AnchorPrio = (int)p;
		} else if(world && key == "debug") {
			r.Debug = (val == "on" || val == "1" || val == "yes");
		} else if(world && key == "base") { // base=auto|floor|gK|nombre
			if(val == "auto") r.BaseMode = -1;
			else if(val == "floor" || val == "sol") r.BaseMode = 0;
			else if(val.size() >= 2 && val[0] == 'g' && val[1] >= '1' && val[1] <= '8') { r.BaseMode = 1; r.BaseGroup = val[1] - '1'; }
			else { float bz = 0.0f; if(!LightParseFloat(val, bz)) { logError("DioramaObject : base doit etre auto, floor, g1 a g8 ou une hauteur : " + val + ", ligne ignoree."); return; } r.BaseMode = 2; r.BaseZ = bz; }
		} else if(sprite && (key == "facing" || key == "hide")) {
			bool b = false;
			if(val == "on" || val == "1" || val == "yes") b = true;
			else if(val == "off" || val == "0" || val == "no") b = false;
			else { logError("Sprite2Obj : " + key + " doit etre on ou off : " + val + ", ligne ignoree."); return; }
			if(key == "facing") r.Facing = b; else r.Hide = b;
		} else if(key == "tile") {
			char* endp = nullptr;
			const unsigned long v = strtoul(val.c_str(), &endp, 16);
			if(val.empty() || endp == val.c_str() || *endp != 0 || v > 0xFFFFul) { logError("Tile2Obj : tile doit etre un hash hexa (4 chiffres) : " + val + ", ligne ignoree."); return; }
			r.TileHash = (uint32_t)v; r.Group.assign(1, (uint32_t)v); r.GridW = r.GridH = 1; hasTile = true;
		} else if(key == "group") {
			r.Group.clear();
			string part;
			for(size_t k = 0; k <= raw.size(); k++) {
				if(k == raw.size() || raw[k] == '|') {
					const string hs = StringUtilities::ToLower(StringUtilities::Trim(part));
					char* endp = nullptr;
					const unsigned long gv = strtoul(hs.c_str(), &endp, 16);
					if(hs.empty() || endp == hs.c_str() || *endp != 0 || gv > 0xFFFFul) { logError("Tile2Obj : group doit etre une liste de hashs hexa separes par | : " + raw + ", ligne ignoree."); return; }
					r.Group.push_back((uint32_t)gv);
					part.clear();
				} else {
					part += raw[k];
				}
			}
			hasTile = true;
		} else if(key == "grid") {
			const size_t xp = val.find('x');
			char* e1 = nullptr; char* e2 = nullptr;
			const long gc = xp == string::npos ? 0 : strtol(val.substr(0, xp).c_str(), &e1, 10);
			const long gr = xp == string::npos ? 0 : strtol(val.substr(xp + 1).c_str(), &e2, 10);
			if(xp == string::npos || gc < 1 || gr < 1 || gc > 8 || gr > 8 || *e1 != 0 || *e2 != 0) { logError("Tile2Obj : grid doit etre CxL, par exemple grid=2x2 (1 a 8) : " + val + ", ligne ignoree."); return; }
			r.GridW = (int)gc; r.GridH = (int)gr;
		} else if(key == "palette") {
			if(val == "*") { r.AnyPalette = true; }
			else {
				char* endp = nullptr;
				const unsigned long v = strtoul(val.c_str(), &endp, 16);
				if(val.empty() || endp == val.c_str() || *endp != 0) { logError("Tile2Obj : palette doit etre * ou RRGGBBAA hexa : " + val + ", ligne ignoree."); return; }
				r.AnyPalette = false; r.Palette = (uint32_t)v;
			}
		} else if(key == "lit") {
			if(val == "on" || val == "1" || val == "yes") r.Lit = true;
			else if(val == "off" || val == "0" || val == "no") r.Lit = false;
			else { logError("Tile2Obj : lit doit etre on ou off : " + val + ", ligne ignoree."); return; }
		} else if(key == "castshadow") {
			if(val == "on" || val == "1" || val == "yes") r.CastShadow = true;
			else if(val == "off" || val == "0" || val == "no") r.CastShadow = false;
			else { logError("Tile2Obj : castshadow doit etre on ou off : " + val + ", ligne ignoree."); return; }
		} else if(key == "tex") {
			texName = raw;
		} else if(key == "color") {
			string h = val;
			if(!h.empty() && h[0] == '#') h = h.substr(1);
			char* endp = nullptr;
			const unsigned long v = h.size() == 6 ? strtoul(h.c_str(), &endp, 16) : 0;
			if(h.size() != 6 || endp == h.c_str() || *endp != 0) { logError("Tile2Obj : color doit etre RRGGBB : " + val + ", ligne ignoree."); return; }
			r.Color = (uint32_t)v;
		} else {
			if(!LightParseFloat(val, f)) { logError("Tile2Obj : valeur numerique attendue pour " + key + " : " + val + ", ligne ignoree."); return; }
			if(key == "scale") { if(f <= 0.0f || f > 4096.0f) { logError("Tile2Obj : scale doit etre de 0 a 4096 : " + val + ", ligne ignoree."); return; } r.Scale = f; }
			else if(world && key == "x") r.WX = f;
			else if(world && key == "y") r.WY = f;
			else if(world && key == "roty") r.RotY = f;
			else if(key == "bright") { if(f < 0.0f || f > 8.0f) { logError("Tile2Obj : bright doit etre de 0 a 8 : " + val + ", ligne ignoree."); return; } r.Bright = f; }
			else if(key == "animtime") { if(f < 0.0f || f > 3600.0f) { logError("Tile2Obj : animtime doit etre de 0 a 3600 secondes : " + val + ", ligne ignoree."); return; } r.AnimTime = f; }
			else if(key == "blend") { if(f < 0.0f || f > 10.0f) { logError("Tile2Obj : blend doit etre de 0 a 10 secondes : " + val + ", ligne ignoree."); return; } r.Blend = f; }
			else if(key == "animspeed") { if(f < 0.0f || f > 16.0f) { logError("Tile2Obj : animspeed doit etre de 0 a 16 : " + val + ", ligne ignoree."); return; } r.AnimSpeed = f; }
			else if(key == "height") { if(f <= 0.0f || f > 4096.0f) { logError("Tile2Obj : height doit etre de 0 a 4096 pixels NES : " + val + ", ligne ignoree."); return; } r.Height = f; }
			else if(key == "elevation") r.Elevation = f;
			else if(key == "rotation") r.RotY = f;
			else if(key == "rotx") r.RotX = f;
			else if(key == "rotz") r.RotZ = f;
			else { logError("Tile2Obj : option inconnue : " + key + ", ligne ignoree."); return; }
		}
	}
	if(world && r.AnchorPrio < 0) { logError("DioramaObject : anchor=bgN obligatoire, ligne ignoree."); return; }
	if(!hasTile && !world) { logError("Tile2Obj : tile=HASH ou group=H1|H2|.. obligatoire, ligne ignoree."); return; }
	if(!sprite && !world && r.Group.size() != (size_t)(r.GridW * r.GridH)) { logError("Tile2Obj : group a " + std::to_string(r.Group.size()) + " hash(s), grid demande " + std::to_string(r.GridW * r.GridH) + ", ligne ignoree."); return; }
	const string lowFile = StringUtilities::ToLower(file);
	const bool isGltf = (lowFile.size() > 4 && lowFile.substr(lowFile.size() - 4) == ".glb") || (lowFile.size() > 5 && lowFile.substr(lowFile.size() - 5) == ".gltf");
	if(isGltf) {
		// glTF : lecteur du cote SNES (maillages, materiaux, textures embarquees, squelette, animations)
		std::vector<std::unique_ptr<SnesHdPackBitmapInfo>> gImgs;
		std::unordered_map<std::string, SnesHdPackBitmapInfo*> gLoaded;
		std::shared_ptr<SnesHdModelMesh> gm = SnesHdLoadGltfModel(_hdPackFolder, file, gImgs, gLoaded, "NES " + file);
		if(!gm) { logError("Tile2Obj : " + file + " : glTF non lu (voir la ligne precedente), ligne ignoree."); return; }
		std::vector<float> pal;
		if(gm->Rig) gm->Rig->Evaluate(-1, 0.0f, pal);
		HdPackBitmapInfo* explicitTex = texName.empty() ? nullptr : LoadAuxBitmap(texName);
		std::map<int, size_t> partOf;
		std::map<SnesHdPackBitmapInfo*, HdPackBitmapInfo*> texOf;
		for(uint32_t ti = 0; ti < (uint32_t)gm->Tris.size(); ti++) {
			const int mi = gm->Tris[ti].Mat;
			auto it = partOf.find(mi);
			if(it == partOf.end()) {
				HdDioramaTile2ObjPart part;
				part.Mesh = std::make_shared<vector<float>>();
				SnesHdPackBitmapInfo* sb = (mi >= 0 && (size_t)mi < gm->Mats.size()) ? gm->Mats[(size_t)mi].Texture : nullptr;
				if(explicitTex) part.Tex = explicitTex;
				else if(sb && !sb->FileData.empty()) {
					auto tt = texOf.find(sb);
					if(tt == texOf.end()) {
						_data->ImageFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
						HdPackBitmapInfo* b = _data->ImageFileData.back().get();
						b->PngName = sb->PngName; b->FileData = sb->FileData;
						tt = texOf.emplace(sb, b).first;
					}
					part.Tex = tt->second;
				}
				it = partOf.emplace(mi, r.Parts.size()).first;
				r.Parts.push_back(part);
				r.PartTris.emplace_back();
			}
			r.PartTris[it->second].push_back(ti);
		}
		float minY = 1e30f, maxY = -1e30f, m2 = 0.0f;
		for(size_t pi = 0; pi < r.Parts.size(); pi++) {
			HdGltfSkinPart(*gm, r.PartTris[pi], &pal, *r.Parts[pi].Mesh); // pose de repos
			const vector<float>& v = *r.Parts[pi].Mesh;
			for(size_t i = 0; i + 7 < v.size(); i += 8) { minY = std::min(minY, v[i + 1]); maxY = std::max(maxY, v[i + 1]); m2 = std::max(m2, v[i] * v[i] + v[i + 1] * v[i + 1] + v[i + 2] * v[i + 2]); }
		}
		if(r.Parts.empty()) { logError("Tile2Obj : " + file + " : aucun triangle, ligne ignoree."); return; }
		const float extY = maxY - minY;
		if(r.Height > 0.0f && extY > 0.0f) r.Scale = r.Height / extY;
		r.Radius = std::sqrt(m2) * r.Scale;
		r.Gltf = gm;
		string list;
		if(gm->Rig) {
			for(size_t a = 0; a < gm->Rig->Anims.size(); a++) {
				list += (a ? ", " : "") + std::to_string(a) + "=" + gm->Rig->Anims[a].Name;
				if(!animName.empty() && StringUtilities::ToLower(gm->Rig->Anims[a].Name) == StringUtilities::ToLower(animName)) r.AnimIndex = (int)a;
			}
			if(r.AnimIndex < 0 && !animName.empty()) {
				char* endp = nullptr;
				const long ai = strtol(animName.c_str(), &endp, 10);
				if(endp != animName.c_str() && *endp == 0 && ai >= 0 && (size_t)ai < gm->Rig->Anims.size()) r.AnimIndex = (int)ai;
				else logError("Tile2Obj : " + file + " : anim=" + animName + " introuvable (clips : " + (list.empty() ? string("aucun") : list) + "), pose de repos.");
			}
			if(animName.empty() && !gm->Rig->Anims.empty()) r.AnimIndex = 0;
		}
		MessageManager::Log("[HDPack] <" + string(world ? "DioramaObject" : (sprite ? "Sprite2Obj" : "Tile2Obj")) + "> " + file + " : glTF, " + std::to_string(gm->Tris.size()) + " triangle(s), " + std::to_string(r.Parts.size()) + " partie(s), clips : " + (list.empty() ? string("aucun") : list) + ", joue : " + (r.AnimIndex >= 0 ? std::to_string(r.AnimIndex) : string("pose de repos")));
	}
	if(!isGltf) {
		vector<uint8_t> raw;
		if(!LoadFile(file, raw)) { logError("Tile2Obj : fichier introuvable : " + file + ", ligne ignoree."); return; }
		string err;
		std::map<string, std::shared_ptr<vector<float>>> byMat;
		auto all = HdLoadObjMesh(raw, err, &byMat);
		if(!all) { logError("Tile2Obj : " + file + " : " + err + ", ligne ignoree."); return; }
		MessageManager::Log("[HDPack] <Tile2Obj> " + file + " : " + std::to_string(all->size() / 24) + " triangle(s), " + std::to_string(byMat.size()) + " materiau(x)");
		{
			float minY = 1e30f, maxY = -1e30f, m2 = 0.0f; // boite en y et rayon autour de l'origine (tout le modele)
			for(size_t i = 0; i + 7 < all->size(); i += 8) {
				const float* v = all->data() + i;
				minY = std::min(minY, v[1]); maxY = std::max(maxY, v[1]);
				m2 = std::max(m2, v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			}
			const float extY = maxY - minY;
			if(r.Height > 0.0f && extY > 0.0f) r.Scale = r.Height / extY; // height= : la hauteur du modele vaut height pixels NES
			r.Radius = std::sqrt(m2) * r.Scale;
		}
		// textures : tex= pour tous les materiaux ; sinon map_Kd du .mtl pour chaque materiau ; sinon <nom du .obj>.png du meme dossier
		const size_t slash = file.find_last_of("\\/");
		const string objDir = (slash == string::npos) ? string() : file.substr(0, slash + 1);
		const string objTxt(raw.begin(), raw.end());
		std::map<string, string> mapOf; // materiau -> chemin de la texture (relatif au pack)
		const size_t ml = objTxt.find("mtllib ");
		if(ml != string::npos) {
			const size_t me = objTxt.find_first_of("\r\n", ml);
			const string mtlName = StringUtilities::Trim(objTxt.substr(ml + 7, me == string::npos ? string::npos : me - ml - 7));
			vector<uint8_t> mtlRaw;
			if(!mtlName.empty() && LoadFile(objDir + mtlName, mtlRaw)) {
				std::istringstream mi(string(mtlRaw.begin(), mtlRaw.end()));
				string mline, curM;
				while(std::getline(mi, mline)) {
					if(!mline.empty() && mline.back() == '\r') mline.pop_back();
					std::istringstream ls(mline);
					string k;
					ls >> k;
					if(k == "newmtl") { string n; ls >> n; curM = n; }
					else if(k == "map_Kd") {
						const size_t p = mline.find("map_Kd");
						const string n = StringUtilities::Trim(mline.substr(p + 6));
						vector<uint8_t> probe;
						if(!n.empty() && LoadFile(objDir + n, probe)) mapOf[curM] = objDir + n;
					}
				}
			}
		}
		HdPackBitmapInfo* explicitTex = nullptr;
		HdPackBitmapInfo* defaultTex = nullptr;
		if(!texName.empty()) {
			explicitTex = LoadAuxBitmap(texName);
			if(!explicitTex) return; // erreur deja journalisee
		} else {
			string base = file.substr(objDir.size());
			if(base.size() > 4) base = base.substr(0, base.size() - 4);
			vector<uint8_t> probe;
			if(LoadFile(objDir + base + ".png", probe)) defaultTex = LoadAuxBitmap(objDir + base + ".png");
		}
		for(auto& kv : byMat) {
			HdDioramaTile2ObjPart part;
			part.Mesh = kv.second;
			if(explicitTex) part.Tex = explicitTex;
			else {
				auto it = mapOf.find(kv.first);
				if(it != mapOf.end()) part.Tex = LoadAuxBitmap(it->second);
				if(!part.Tex) part.Tex = defaultTex;
			}
			r.Parts.push_back(part);
		}
		if(r.Parts.empty()) { logError("Tile2Obj : " + file + " : aucune face lue, ligne ignoree."); return; }
	}
	if(world) _data->WorldObjRules.push_back(r); else if(sprite) _data->Sprite2ObjRules.push_back(r); else _data->Tile2ObjRules.push_back(r);
}

// <DioramaNormal>albedo.png,normal.png (doc 44, G6d) : carte de normales. Les tiles dont l'image est albedo.png (les <tile> de ce PNG) recoivent
// leur relief dans normal.png, a la meme position et a la meme taille (un PNG de normales parallele au PNG de couleurs). Le RGB code la normale
// ((R, G, B) = (0.5, 0.5, 1) : face a l'ecran ; G vers le haut, sinon <Diorama>nflipy=on). Pas de conditions. Force : <Diorama>nstrength=.
void HdPackLoader::ProcessDioramaNormalTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(!conditions.empty()) {
		logError("DioramaNormal : les conditions ne sont pas prises en charge, ligne ignoree.");
		return;
	}
	if(tokens.size() != 2 || StringUtilities::Trim(tokens[0]).empty() || StringUtilities::Trim(tokens[1]).empty()) {
		logError("DioramaNormal : albedo.png,normal.png attendus, ligne ignoree.");
		return;
	}
	string albedoResolved;
	if(!ResolveRandomPath(StringUtilities::Trim(tokens[0]), albedoResolved)) {
		return; // l'erreur est deja journalisee
	}
	HdPackBitmapInfo* nb = LoadAuxBitmap(StringUtilities::Trim(tokens[1]));
	if(!nb) {
		return;
	}
	HdDioramaNormalPair np;
	np.AlbedoName = albedoResolved;
	np.Normal = nb;
	_data->DioramaNormalPairs.push_back(np);
	size_t n = 0;
	for(auto& t : _data->Tiles) { // tiles deja chargees
		if(t->Bitmap && t->Bitmap->PngName == np.AlbedoName) { t->NormalBitmap = np.Normal; n++; }
	}
	MessageManager::Log("[HDPack] <DioramaNormal> " + StringUtilities::Trim(tokens[0]) + " -> " + StringUtilities::Trim(tokens[1]) + " (" + std::to_string(n) + " tile(s) deja chargee(s))");
}

// <DioramaShadow> (doc 42, G6c) : ombre des sprites. Forme ligne : [cond]<DioramaShadow>tile=HASH (ou tile=* : tous les sprites)[,palette=..|*][,opacity=0.5][,color=000000]
// [,soft=0][,onto=2][,light=1] ; forme bloc : <DioramaShadow>opacity=..,onto=..{ <tile>... } (les sprites des <tile> du bloc projettent une ombre).
// onto : couches qui recoivent l'ombre : 0 a 3 (L0..L3) ou g1 a g4, separees par + (defaut 2) ; light : numero de la lumiere ponctuelle / solaire
// (1 = la premiere active) ; soft : adoucissement en pixels NES (0 a 8) ; opacity 0..1.
bool HdPackLoader::ParseDioramaShadowOptions(vector<string>& tokens, HdDioramaShadowRule& rule, bool allowTile)
{
	string tileStr, palStr = "*";
	for(size_t i = 0; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		if(eq == string::npos) {
			logError("DioramaShadow : option sans '=' : " + opt);
			return false;
		}
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string val = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(eq + 1)));
		float f = 0.0f;
		if(key == "opacity") {
			if(!LightParseFloat(val, f) || f < 0.0f || f > 1.0f) { logError("DioramaShadow : opacity doit etre de 0 a 1 : " + val); return false; }
			rule.Opacity = f;
		} else if(key == "soft") {
			if(!LightParseFloat(val, f) || f < 0.0f || f > 8.0f) { logError("DioramaShadow : soft doit etre de 0 a 8 pixels NES : " + val); return false; }
			rule.Soft = f;
		} else if(key == "color") {
			string h = val;
			if(!h.empty() && h[0] == '#') h = h.substr(1);
			char* endp = nullptr;
			const unsigned long v = h.size() == 6 ? strtoul(h.c_str(), &endp, 16) : 0;
			if(h.size() != 6 || endp == h.c_str() || *endp != 0) { logError("DioramaShadow : color doit etre RRGGBB : " + val); return false; }
			rule.Color = (uint32_t)v;
		} else if(key == "light") {
			int n = 0;
			if(!DioParseInt(val, n) || n < 1 || n > 8) { logError("DioramaShadow : light doit etre de 1 a 8 : " + val); return false; }
			rule.Light = n;
		} else if(key == "onto") {
			uint32_t mask = 0;
			size_t start = 0;
			while(start <= val.size()) {
				size_t plus = val.find('+', start);
				const string part = StringUtilities::Trim(val.substr(start, plus == string::npos ? string::npos : plus - start));
				int n = 0;
				if(part.size() == 2 && part[0] == 'g' && part[1] >= '1' && part[1] <= '8') mask |= 1u << (4 + (part[1] - '1'));
				else if(DioParseInt(part, n) && n >= 0 && n <= 3) mask |= 1u << n;
				else { logError("DioramaShadow : onto = couches 0 a 3 ou g1 a g8 separees par + : " + val); return false; }
				if(plus == string::npos) break;
				start = plus + 1;
			}
			rule.OntoMask = mask;
		} else if(key == "tile") {
			if(!allowTile) { logError("DioramaShadow : tile= n'est pas permis dans la forme bloc (mettre des <tile> dans le bloc)."); return false; }
			tileStr = StringUtilities::Trim(opt.substr(eq + 1));
			rule.HasTile = !tileStr.empty();
		} else if(key == "palette") {
			palStr = val;
		} else {
			logError("DioramaShadow : option inconnue : " + key);
			return false;
		}
	}
	if(allowTile) {
		if(!rule.HasTile) { logError("DioramaShadow : tile= est obligatoire (ou utiliser la forme bloc)."); return false; }
		rule.AnyPalette = (palStr == "*" || palStr == "all");
		string pd = rule.AnyPalette ? string("0") : palStr;
		if(tileStr == "*") {
			rule.AnyTile = true; // tous les sprites
		} else {
			try {
				ReadTileData(rule.Key, tileStr, pd);
			} catch(...) {
				logError("DioramaShadow : tile invalide : " + tileStr);
				return false;
			}
		}
	}
	return true;
}

void HdPackLoader::ProcessDioramaShadowTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	HdDioramaShadowRule rule;
	rule.Conditions = conditions;
	if(!ParseDioramaShadowOptions(tokens, rule, true)) {
		logError("DioramaShadow : ligne ignoree.");
		return;
	}
	if(_data->DioramaShadowRules.size() >= 200) {
		logError("DioramaShadow : 200 regles au plus, ligne ignoree.");
		return;
	}
	_data->DioramaShadowRules.push_back(rule);
}

// <DioramaLight>type=ambient|point|sun,color=RRGGBB,intensity=..,x=..,y=..,z=..,radius=..,dx=..,dy=..,dz=.. (doc 42) : lumiere de la scene.
// ambient : color, intensity. point : position x, y, z (pixels NES de la scene, z negatif = devant), radius (portee, defaut 256).
// sun : direction de propagation dx, dy, dz (defaut 0.3, 0.5, 1). Conditions [ ] : jusqu'a 4 lumieres ponctuelles / solaires vraies, la derniere
// lumiere ambiante vraie l'emporte (defaut blanc : sans ambiante, les lumieres ne font qu'ajouter). point : glow=0..8, glowradius=pixels NES = halo visible. G6b : point + tile=HASH[,palette=..|*][,flicker=0..1][,speed=..][,offx=..][,offy=..] = lumiere attachee a une tile de sprite (8 lumieres au plus en tout).
static bool LightParseFloat(const string& v, float& out)
{
	char* endp = nullptr;
	const double d = strtod(v.c_str(), &endp);
	if(v.empty() || endp == v.c_str() || *endp != 0) return false;
	out = (float)d;
	return true;
}

// <DioramaCamera>nom[,yaw=,pitch=,dist=,zoom=,time=,key=,default=on] (doc 49) : declare une camera, ou (avec [cond]) change ses reglages.
// Les reglages non cites viennent de <Diorama> (ou de la declaration sans condition).
void HdPackLoader::ProcessDioramaCameraTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty() || tokens[0].find('=') != string::npos) {
		logError("DioramaCamera : syntaxe <DioramaCamera>nom[,options], ligne ignoree.");
		return;
	}
	HdDioramaCameraLine L;
	L.Conditions = conditions;
	const string name = StringUtilities::ToLower(StringUtilities::Trim(tokens[0]));
	for(size_t i = 1; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		if(eq == string::npos) { logError("DioramaCamera : option sans '=' : " + opt + ", ligne ignoree."); return; }
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string rawVal = StringUtilities::Trim(opt.substr(eq + 1));
		const string val = StringUtilities::ToLower(rawVal);
		float f = 0.0f;
		auto num = [&](float lo, float hi, float& out) {
			if(!LightParseFloat(val, f) || f < lo || f > hi) { logError("DioramaCamera : " + key + " doit etre de " + std::to_string(lo) + " a " + std::to_string(hi) + " : " + val + ", ligne ignoree."); return false; }
			out = f; return true;
		};
		if(key == "yaw") { if(!num(-70, 70, L.V[0])) return; L.Has[0] = true; }
		else if(key == "pitch") { if(!num(-70, 70, L.V[1])) return; L.Has[1] = true; }
		else if(key == "dist") { if(!num(16, 100000, L.V[2])) return; L.Has[2] = true; }
		else if(key == "zoom") { if(!num(0.1f, 4, L.V[3])) return; L.Has[3] = true; }
		else if(key == "time") { if(!num(0, 1000, L.Time)) return; L.HasTime = true; }
		else if(key == "key") L.Key = rawVal;
		else if(key == "default") L.IsDefault = (val == "on" || val == "1" || val == "true");
		else { logError("DioramaCamera : option inconnue : " + key + ", ligne ignoree."); return; }
	}
	int idx = -1;
	for(size_t k = 0; k < _data->DioramaCameraNames.size(); k++) if(_data->DioramaCameraNames[k] == name) idx = (int)k;
	if(idx < 0) {
		if(_data->DioramaCameraNames.size() >= 16) { logError("DioramaCamera : 16 cameras au plus, ligne ignoree."); return; }
		_data->DioramaCameraNames.push_back(name);
		idx = (int)_data->DioramaCameraNames.size() - 1;
	}
	L.Index = idx;
	_data->DioramaCameraLines.push_back(L);
}

// [cond]<DioramaCameraDefault>nom : camera par defaut tant que la condition est vraie (la derniere regle vraie l'emporte).
void HdPackLoader::ProcessDioramaCameraDefaultTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(tokens.size() != 1 || StringUtilities::Trim(tokens[0]).empty()) { logError("DioramaCameraDefault : syntaxe <DioramaCameraDefault>nom, ligne ignoree."); return; }
	const string name = StringUtilities::ToLower(StringUtilities::Trim(tokens[0]));
	for(size_t k = 0; k < _data->DioramaCameraNames.size(); k++) {
		if(_data->DioramaCameraNames[k] == name) {
			HdDioramaCameraDefaultRule r;
			r.Conditions = conditions; r.Index = (int)k;
			_data->DioramaCameraDefaults.push_back(r);
			return;
		}
	}
	logError("DioramaCameraDefault : camera inconnue (a declarer avant avec <DioramaCamera>) : " + name + ", ligne ignoree.");
}

// <DioramaCameraKeys>next=F9,prev=F8,default=F7 : touches (noms Mesen, clavier ou manette) pour changer de camera.
void HdPackLoader::ProcessDioramaCameraKeysTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(!conditions.empty()) { logError("DioramaCameraKeys : les conditions ne sont pas prises en charge, ligne ignoree."); return; }
	for(size_t i = 0; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		if(eq == string::npos) { logError("DioramaCameraKeys : option sans '=' : " + opt + ", ligne ignoree."); return; }
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string val = StringUtilities::Trim(opt.substr(eq + 1));
		if(key == "next") _data->DioramaCameraKeys.Next = val;
		else if(key == "prev") _data->DioramaCameraKeys.Prev = val;
		else if(key == "default") _data->DioramaCameraKeys.Default = val;
		else { logError("DioramaCameraKeys : option inconnue : " + key + ", ligne ignoree."); return; }
	}
}

// <DioramaView>topDown[,pitch=,zoom=,time=] (doc 53) : reglages de vue tant que la condition est vraie (la derniere regle vraie l'emporte).
// Seule la vue topDown est reconnue pour l'instant. Sans regle vraie, la vue reste celle de <Diorama> / <DioramaCamera>.
// <DioramaFog>[off][,color=RRGGBB|sky][,start=][,end=][,opacity=][,ground=][,groundheight=] (doc 54) : brouillard de distance du diorama.
// Les valeurs absentes prennent un defaut selon la vue (topDown ou side).
// <DioramaClouds>[off][,type=shadows|fog][,anchor=bgN][,size=][,speed=][,direction=][,coverage=][,softness=][,opacity=][,color=][,groundheight=][,seed=]
// <DioramaMist>[off][,count=][,from=][,to=][,opacity=][,size=][,speed=][,direction=][,coverage=][,softness=][,color=][,band=][,anchor=bgN][,seed=]
// <3DParticles>fichier.json[,cle=valeur...] (diorama NES) : memes cles que le SNES (particles3d) ; anchor=bgN, depthmin= / depthmax= (side) en plus.
void HdPackLoader::ProcessDiorama3DParticlesTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty()) { logError("3DParticles : fichier JSON manquant, ligne ignoree."); return; }
	HdDiorama3DParticlesRule r;
	r.Conditions = conditions;
	r.JsonFile = StringUtilities::Trim(tokens[0]);
	std::unordered_map<string, string> cfg;
	vector<uint8_t> raw;
	if(LoadFile(r.JsonFile, raw)) cfg = ParseFlatJson(string(raw.begin(), raw.end()));
	else logError("3DParticles : fichier introuvable : " + r.JsonFile);
	for(size_t t = 1; t < tokens.size(); t++) {
		const string tok = StringUtilities::Trim(tokens[t]);
		const size_t eq = tok.find('=');
		if(eq == string::npos) continue;
		cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
	}
	auto has = [&](const char* k) { return cfg.find(k) != cfg.end() && !cfg[k].empty(); };
	auto str = [&](const char* k) -> string { auto it = cfg.find(k); return it == cfg.end() ? string() : it->second; };
	auto num = [&](const char* k, float def) -> float { const string v = str(k); if(v.empty()) return def; try { return std::stof(v); } catch(...) { return def; } };
	auto color = [&](const char* k, uint32_t def) -> uint32_t { string v = StringUtilities::Trim(str(k)); if(v.empty()) return def; if(v[0] == '#') v = v.substr(1); try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { return def; } };
	r.Alpha = std::clamp(num("alpha", 1.0f), 0.0f, 1.0f);
	r.Additive = StringUtilities::ToLower(str("blend")) == "additive";
	r.MaxParticles = std::clamp((int)num("maxparticles", 400.0f), 1, 20000);
	r.Rate = std::max(0.0f, num("rate", 0.0f));
	r.LifeMin = std::max(0.0f, num("lifemin", 0.0f));
	r.LifeMax = std::max(r.LifeMin, num("lifemax", r.LifeMin));
	r.HasBoxSize = has("boxsize"); r.BoxSize = std::clamp(num("boxsize", 384.0f), 32.0f, 2048.0f);
	r.HasBoxHeight = has("boxheight"); r.BoxHeight = std::clamp(num("boxheight", 96.0f), 4.0f, 2048.0f);
	r.HasBoxAhead = has("boxahead"); r.BoxAhead = std::clamp(num("boxahead", 0.25f), 0.0f, 0.5f);
	r.HasDepth = has("depthmin") || has("depthmax"); r.DepthMin = num("depthmin", -24.0f); r.DepthMax = num("depthmax", 200.0f);
	if(r.DepthMax < r.DepthMin + 1.0f) r.DepthMax = r.DepthMin + 1.0f;
	r.HasFall = has("fallmin") || has("fallmax"); r.FallMin = num("fallmin", 20.0f); r.FallMax = num("fallmax", 40.0f);
	r.DriftMin = num("driftmin", 0.0f); r.DriftMax = num("driftmax", 6.0f);
	r.Gravity = num("gravity", 0.0f);
	r.WindX = num("windx", 0.0f); r.WindY = num("windy", 0.0f);
	r.Turbulence = num("turbulence", 0.0f); r.TurbulenceFreq = num("turbulencefreq", 0.5f);
	r.HasSize = has("sizestart"); r.SizeStart = num("sizestart", 2.0f); r.SizeEnd = num("sizeend", r.SizeStart);
	r.SizeRandom = std::clamp(num("sizerandom", 0.0f), 0.0f, 0.95f);
	r.MinPixelSize = std::max(0.5f, num("minpixelsize", 1.5f));
	r.AlphaStart = num("alphastart", 1.0f); r.AlphaEnd = num("alphaend", r.AlphaStart);
	r.FadeIn = std::clamp(num("fadein", 0.1f), 0.0f, 1.0f);
	r.FadeOut = std::clamp(num("fadeout", 0.2f), 0.0f, 1.0f);
	r.ColorStart = color("colorstart", 0xFFFFFF); r.ColorEnd = color("colorend", r.ColorStart);
	r.FarFade = std::clamp(num("farfade", 0.3f), 0.0f, 1.0f);
	r.GroundRespawn = StringUtilities::ToLower(str("groundhit")) != "die";
	r.Stretch = std::clamp(num("stretch", 0.0f), 0.0f, 2.0f);
	{
		string a = StringUtilities::ToLower(str("anchor"));
		if(!a.empty()) { if(a.size() > 2 && a.substr(0, 2) == "bg") a = a.substr(2); char* endp = nullptr; const long p = strtol(a.c_str(), &endp, 10); if(*endp == 0 && p >= 0 && p <= 49) r.Anchor = (int)p; else logError("3DParticles : anchor doit etre bgN : " + str("anchor")); }
	}
	const string tex = StringUtilities::Trim(str("texture"));
	if(!tex.empty()) r.Texture = LoadAuxBitmap(tex);
	if(_data->Diorama3DParticles.size() >= 4) { logError("3DParticles : 4 effets au plus en diorama, ligne ignoree."); return; }
	MessageManager::Log("[HDPack] <3DParticles> " + r.JsonFile + " enregistre (diorama, " + std::to_string(r.MaxParticles) + " particules max).");
	_data->Diorama3DParticles.push_back(r);
}

// <DayNight>fichier.json[,cle=valeur...] (doc 55) : cycle jour / nuit, diorama et mode normal. Les cles de la ligne remplacent celles du JSON.
void HdPackLoader::ProcessDayNightTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty()) { logError("DayNight : fichier JSON manquant, ligne ignoree."); return; }
	HdPackData::DayNightRule r;
	r.Conditions = conditions;
	r.JsonFile = StringUtilities::Trim(tokens[0]);
	std::unordered_map<string, string> cfg;
	vector<uint8_t> raw;
	if(LoadFile(r.JsonFile, raw)) cfg = ParseFlatJson(string(raw.begin(), raw.end()));
	else { logError("DayNight : fichier introuvable : " + r.JsonFile + ", ligne ignoree."); return; }
	for(size_t t = 1; t < tokens.size(); t++) {
		const string tok = StringUtilities::Trim(tokens[t]);
		const size_t eq = tok.find('=');
		if(eq == string::npos) continue;
		cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
	}
	const string err = HdDayNightParse(cfg, r.Def);
	if(r.Def.Steps.empty()) { logError("DayNight : " + r.JsonFile + " : " + err + ", ligne ignoree."); return; }
	if(!err.empty()) logError("DayNight : " + r.JsonFile + " : " + err);
	for(const HdDayNightStep& s : r.Def.Steps) r.Until.push_back(s.Until.empty() ? vector<HdPackCondition*>() : ParseConditionString(s.Until));
	int total = 0;
	for(const HdDayNightStep& s : r.Def.Steps) total += s.Hold + s.Fade;
	MessageManager::Log("[HDPack] <DayNight> " + r.JsonFile + " : " + std::to_string(r.Def.Steps.size()) + " etape(s), cycle " + std::to_string(total) + " images" + (r.Def.TimeAlways ? "" : " (temps : seulement sous condition)") + (r.Def.DrivesFog ? ", pilote le brouillard" : "") + ".");
	_data->DayNightRules.push_back(r);
}

void HdPackLoader::ProcessDioramaMistTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	HdDioramaMistRule r;
	r.Conditions = conditions;
	for(const string& t0 : tokens) {
		const string opt = StringUtilities::Trim(t0);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		const string key = StringUtilities::ToLower(StringUtilities::Trim(eq == string::npos ? opt : opt.substr(0, eq)));
		const string val = eq == string::npos ? string() : StringUtilities::ToLower(StringUtilities::Trim(opt.substr(eq + 1)));
		if(eq == string::npos) {
			if(key == "off") { r.Off = true; continue; }
			if(key == "on") continue;
			logError("DioramaMist : option inconnue : " + key + ", ligne ignoree."); return;
		}
		if(key == "anchor") {
			const string num = (val.size() > 2 && val.substr(0, 2) == "bg") ? val.substr(2) : val;
			char* endp = nullptr;
			const long p = strtol(num.c_str(), &endp, 10);
			if(num.empty() || *endp != 0 || p < 0 || p > 49) { logError("DioramaMist : anchor doit etre bgN : " + val + ", ligne ignoree."); return; }
			r.Anchor = (int)p; continue;
		}
		if(key == "color") {
			string h = val; if(!h.empty() && h[0] == '#') h = h.substr(1);
			char* endp = nullptr;
			const unsigned long v = h.size() == 6 ? strtoul(h.c_str(), &endp, 16) : 0;
			if(h.size() != 6 || endp == h.c_str() || *endp != 0) { logError("DioramaMist : color doit etre RRGGBB : " + val + ", ligne ignoree."); return; }
			r.Color = (uint32_t)v; r.HasColor = true; continue;
		}
		float f = 0.0f;
		if(!LightParseFloat(val, f)) { logError("DioramaMist : valeur numerique attendue pour " + key + " : " + val + ", ligne ignoree."); return; }
		if(key == "count") { if(f < 1.0f || f > 8.0f) { logError("DioramaMist : count doit etre de 1 a 8, ligne ignoree."); return; } r.Count = (int)f; r.HasCount = true; }
		else if(key == "from") { r.From = f; r.HasFrom = true; }
		else if(key == "to") { r.To = f; r.HasTo = true; }
		else if(key == "opacity") { r.Opacity = std::clamp(f, 0.0f, 1.0f); r.HasOpacity = true; }
		else if(key == "size") { if(f < 2.0f || f > 4096.0f) { logError("DioramaMist : size doit etre de 2 a 4096 pixels NES, ligne ignoree."); return; } r.Size = f; r.HasSize = true; }
		else if(key == "speed") { r.Speed = f; r.HasSpeed = true; }
		else if(key == "direction") { r.Dir = f; r.HasDir = true; }
		else if(key == "coverage") { r.Coverage = std::clamp(f, 0.0f, 1.0f); r.HasCoverage = true; }
		else if(key == "softness") { r.Soft = std::clamp(f, 0.0f, 1.0f); r.HasSoft = true; }
		else if(key == "band") { if(f <= 0.0f) { logError("DioramaMist : band doit etre positif, ligne ignoree."); return; } r.Band = f; r.HasBand = true; }
		else if(key == "seed") r.Seed = f;
		else { logError("DioramaMist : option inconnue : " + key + ", ligne ignoree."); return; }
	}
	if(_data->DioramaMistRules.size() >= 16) { logError("DioramaMist : 16 regles au plus, ligne ignoree."); return; }
	_data->DioramaMistRules.push_back(r);
}

void HdPackLoader::ProcessDioramaCloudsTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	HdDioramaCloudRule r;
	r.Conditions = conditions;
	for(const string& t0 : tokens) {
		const string opt = StringUtilities::Trim(t0);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		const string key = StringUtilities::ToLower(StringUtilities::Trim(eq == string::npos ? opt : opt.substr(0, eq)));
		const string val = eq == string::npos ? string() : StringUtilities::ToLower(StringUtilities::Trim(opt.substr(eq + 1)));
		if(eq == string::npos) {
			if(key == "off") { r.Off = true; continue; }
			if(key == "shadows" || key == "fog") { r.Type = key == "fog" ? 1 : 0; continue; }
			logError("DioramaClouds : option inconnue : " + key + ", ligne ignoree."); return;
		}
		if(key == "type") {
			if(val == "shadows" || val == "shadow" || val == "ombres") r.Type = 0;
			else if(val == "fog" || val == "brouillard") r.Type = 1;
			else { logError("DioramaClouds : type doit etre shadows ou fog : " + val + ", ligne ignoree."); return; }
			continue;
		}
		if(key == "anchor") {
			const string num = (val.size() > 2 && val.substr(0, 2) == "bg") ? val.substr(2) : val;
			char* endp = nullptr;
			const long p = strtol(num.c_str(), &endp, 10);
			if(num.empty() || *endp != 0 || p < 0 || p > 49) { logError("DioramaClouds : anchor doit etre bgN : " + val + ", ligne ignoree."); return; }
			r.Anchor = (int)p;
			continue;
		}
		if(key == "color") {
			string h = val; if(!h.empty() && h[0] == '#') h = h.substr(1);
			char* endp = nullptr;
			const unsigned long v = h.size() == 6 ? strtoul(h.c_str(), &endp, 16) : 0;
			if(h.size() != 6 || endp == h.c_str() || *endp != 0) { logError("DioramaClouds : color doit etre RRGGBB : " + val + ", ligne ignoree."); return; }
			r.Color = (uint32_t)v; r.HasColor = true;
			continue;
		}
		float f = 0.0f;
		if(!LightParseFloat(val, f)) { logError("DioramaClouds : valeur numerique attendue pour " + key + " : " + val + ", ligne ignoree."); return; }
		if(key == "size") { if(f < 2.0f || f > 4096.0f) { logError("DioramaClouds : size doit etre de 2 a 4096 pixels NES, ligne ignoree."); return; } r.Size = f; r.HasSize = true; }
		else if(key == "speed") { r.Speed = f; r.HasSpeed = true; }
		else if(key == "direction") { r.Dir = f; r.HasDir = true; }
		else if(key == "coverage") { r.Coverage = std::clamp(f, 0.0f, 1.0f); r.HasCoverage = true; }
		else if(key == "softness") { r.Soft = std::clamp(f, 0.0f, 1.0f); r.HasSoft = true; }
		else if(key == "opacity") { r.Opacity = std::clamp(f, 0.0f, 1.0f); r.HasOpacity = true; }
		else if(key == "groundheight") { if(f <= 0.0f) { logError("DioramaClouds : groundheight doit etre positif, ligne ignoree."); return; } r.GroundH = f; r.HasGroundH = true; }
		else if(key == "seed") r.Seed = f;
		else { logError("DioramaClouds : option inconnue : " + key + ", ligne ignoree."); return; }
	}
	if(_data->DioramaCloudRules.size() >= 16) { logError("DioramaClouds : 16 regles au plus, ligne ignoree."); return; }
	_data->DioramaCloudRules.push_back(r);
}

void HdPackLoader::ProcessDioramaFogTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	HdDioramaFogRule r;
	r.Conditions = conditions;
	for(const string& t0 : tokens) {
		const string opt = StringUtilities::Trim(t0);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		const string key = StringUtilities::ToLower(StringUtilities::Trim(eq == string::npos ? opt : opt.substr(0, eq)));
		const string val = eq == string::npos ? string() : StringUtilities::ToLower(StringUtilities::Trim(opt.substr(eq + 1)));
		if(eq == string::npos) {
			if(key == "off") { r.Off = true; continue; }
			if(key == "on") continue;
			logError("DioramaFog : option inconnue : " + key + ", ligne ignoree."); return;
		}
		float f = 0.0f;
		if(key == "color") {
			if(val == "sky" || val == "ciel") { r.HasColor = true; r.ColorSky = true; continue; }
			string h = val; if(!h.empty() && h[0] == '#') h = h.substr(1);
			char* endp = nullptr;
			const unsigned long v = h.size() == 6 ? strtoul(h.c_str(), &endp, 16) : 0;
			if(h.size() != 6 || endp == h.c_str() || *endp != 0) { logError("DioramaFog : color doit etre RRGGBB ou sky : " + val + ", ligne ignoree."); return; }
			r.HasColor = true; r.Color = (uint32_t)v;
			continue;
		}
		if(!LightParseFloat(val, f)) { logError("DioramaFog : valeur numerique attendue pour " + key + " : " + val + ", ligne ignoree."); return; }
		if(key == "start") { r.Start = f; r.HasStart = true; }
		else if(key == "end") { r.End = f; r.HasEnd = true; }
		else if(key == "opacity") { if(f < 0.0f || f > 1.0f) { logError("DioramaFog : opacity doit etre de 0 a 1, ligne ignoree."); return; } r.Opacity = f; r.HasOpacity = true; }
		else if(key == "ground") { if(f < 0.0f || f > 1.0f) { logError("DioramaFog : ground doit etre de 0 a 1, ligne ignoree."); return; } r.Ground = f; r.HasGround = true; }
		else if(key == "groundheight") { if(f <= 0.0f || f > 1024.0f) { logError("DioramaFog : groundheight doit etre de 0 a 1024 pixels NES, ligne ignoree."); return; } r.GroundH = f; r.HasGroundH = true; }
		else { logError("DioramaFog : option inconnue : " + key + ", ligne ignoree."); return; }
	}
	if(_data->DioramaFogRules.size() >= 16) { logError("DioramaFog : 16 regles au plus, ligne ignoree."); return; }
	_data->DioramaFogRules.push_back(r);
}

void HdPackLoader::ProcessDioramaViewTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty() || tokens[0].find('=') != string::npos) {
		logError("DioramaView : syntaxe <DioramaView>topDown[,pitch=,zoom=,time=], ligne ignoree.");
		return;
	}
	const string name = StringUtilities::ToLower(StringUtilities::Trim(tokens[0]));
	if(name != "topdown") { logError("DioramaView : vue inconnue : " + name + " (seule topDown existe), ligne ignoree."); return; }
	HdDioramaViewRule r;
	r.Conditions = conditions;
	for(size_t i = 1; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		if(eq == string::npos) { logError("DioramaView : option sans '=' : " + opt + ", ligne ignoree."); return; }
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string val = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(eq + 1)));
		float f = 0.0f;
		auto num = [&](float lo, float hi, float& out) {
			if(!LightParseFloat(val, f) || f < lo || f > hi) { logError("DioramaView : " + key + " doit etre de " + std::to_string(lo) + " a " + std::to_string(hi) + " : " + val + ", ligne ignoree."); return false; }
			out = f; return true;
		};
		if(key == "pitch") { if(!num(-70, 70, r.V[1])) return; r.Has[1] = true; }
		else if(key == "zoom") { if(!num(0.1f, 4, r.V[3])) return; r.Has[3] = true; }
		else if(key == "time") { if(!num(0, 1000, r.Time)) return; r.HasTime = true; }
		else if(key == "floormargin") { if(!num(0, 4096, r.FloorMargin)) return; }
		else { logError("DioramaView : option inconnue : " + key + ", ligne ignoree."); return; }
	}
	if(_data->DioramaViews.size() >= 16) { logError("DioramaView : 16 regles au plus, ligne ignoree."); return; }
	_data->DioramaViews.push_back(r);
}

// <DioramaPad>left=rot,right=zoom,port=1,yawmax=35,pitchmax=25,zoomrange=2,dead=0.2,smooth=0.2,invx=off,invy=off
// La manette pilote la camera du diorama (stick analogique). left= / right= : rot (yaw+pitch), zoom ou off. Accepte [cond].
void HdPackLoader::ProcessDioramaPadTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	HdDioramaPadRule r;
	r.Conditions = conditions;
	for(size_t i = 0; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		if(eq == string::npos) { logError("DioramaPad : option sans '=' : " + opt + ", ligne ignoree."); return; }
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string val = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(eq + 1)));
		float f = 0.0f;
		auto fn = [&](float lo, float hi, float& out) {
			if(!LightParseFloat(val, f) || f < lo || f > hi) { logError("DioramaPad : " + key + " doit etre de " + std::to_string(lo) + " a " + std::to_string(hi) + " : " + val + ", ligne ignoree."); return false; }
			out = f; return true;
		};
		auto fnc = [&](int& out) {
			if(val == "rot" || val == "rotation") out = 1;
			else if(val == "zoom") out = 2;
			else if(val == "off" || val == "none") out = 0;
			else { logError("DioramaPad : " + key + " = rot, zoom ou off : " + val + ", ligne ignoree."); return false; }
			return true;
		};
		if(key == "left") { if(!fnc(r.Left)) return; }
		else if(key == "right") { if(!fnc(r.Right)) return; }
		else if(key == "port") { float p = 1; if(!fn(1, 4, p)) return; r.Port = (int)p; }
		else if(key == "yawmax") { if(!fn(0, 70, r.YawMax)) return; }
		else if(key == "pitchmax") { if(!fn(0, 70, r.PitchMax)) return; }
		else if(key == "zoomrange") { if(!fn(1, 4, r.ZoomRange)) return; }
		else if(key == "dead") { if(!fn(0, 0.9f, r.Dead)) return; }
		else if(key == "smooth") { if(!fn(0.01f, 1, r.Smooth)) return; }
		else if(key == "invx") r.InvX = (val == "on" || val == "1" || val == "true");
		else if(key == "invy") r.InvY = (val == "on" || val == "1" || val == "true");
		else { logError("DioramaPad : option inconnue : " + key + ", ligne ignoree."); return; }
	}
	if(_data->DioramaPadRules.size() >= 16) { logError("DioramaPad : 16 regles au plus, ligne ignoree."); return; }
	_data->DioramaPadRules.push_back(r);
}

void HdPackLoader::ProcessDioramaLightTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	HdDioramaLightRule r;
	r.Conditions = conditions;
	string tileStr, palStr = "*";
	for(size_t i = 0; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		if(opt.empty()) continue;
		const size_t eq = opt.find('=');
		if(eq == string::npos) {
			logError("DioramaLight : option sans '=' : " + opt + ", ligne ignoree.");
			return;
		}
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string val = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(eq + 1)));
		float f = 0.0f;
		if(key == "type") {
			if(val == "ambient") r.Type = 1;
			else if(val == "point") r.Type = 2;
			else if(val == "sun") r.Type = 3;
			else { logError("DioramaLight : type inconnu (ambient, point, sun) : " + val + ", ligne ignoree."); return; }
		} else if(key == "color") {
			string h = val;
			if(!h.empty() && h[0] == '#') h = h.substr(1);
			char* endp = nullptr;
			const unsigned long v = h.size() == 6 ? strtoul(h.c_str(), &endp, 16) : 0;
			if(h.size() != 6 || endp == h.c_str() || *endp != 0) { logError("DioramaLight : color doit etre RRGGBB : " + val + ", ligne ignoree."); return; }
			r.Color = (uint32_t)v;
		} else if(key == "intensity") {
			if(!LightParseFloat(val, f) || f < 0.0f || f > 16.0f) { logError("DioramaLight : intensity doit etre de 0 a 16 : " + val + ", ligne ignoree."); return; }
			r.Intensity = f;
		} else if(key == "radius") {
			if(!LightParseFloat(val, f) || f < 1.0f || f > 4096.0f) { logError("DioramaLight : radius doit etre de 1 a 4096 pixels NES : " + val + ", ligne ignoree."); return; }
			r.Radius = f;
		} else if(key == "tile") {
			tileStr = StringUtilities::Trim(opt.substr(eq + 1)); // casse conservee (hash hexadecimal)
			r.HasTile = !tileStr.empty();
		} else if(key == "palette") {
			palStr = val;
		} else if(key == "flicker") {
			if(!LightParseFloat(val, f) || f < 0.0f || f > 1.0f) { logError("DioramaLight : flicker doit etre de 0 a 1 : " + val + ", ligne ignoree."); return; }
			r.Flicker = f;
		} else if(key == "speed") {
			if(!LightParseFloat(val, f) || f < 0.0f || f > 100.0f) { logError("DioramaLight : speed doit etre de 0 a 100 : " + val + ", ligne ignoree."); return; }
			r.FlickerSpeed = f;
		} else if(key == "offx" || key == "offy") {
			if(!LightParseFloat(val, f) || f < -256.0f || f > 256.0f) { logError("DioramaLight : " + key + " doit etre de -256 a 256 : " + val + ", ligne ignoree."); return; }
			if(key == "offx") r.OffX = f; else r.OffY = f;
		} else if(key == "glow") {
			if(!LightParseFloat(val, f) || f < 0.0f || f > 8.0f) { logError("DioramaLight : glow doit etre de 0 a 8 : " + val + ", ligne ignoree."); return; }
			r.Glow = f;
		} else if(key == "glowradius") {
			if(!LightParseFloat(val, f) || f < 1.0f || f > 1024.0f) { logError("DioramaLight : glowradius doit etre de 1 a 1024 pixels NES : " + val + ", ligne ignoree."); return; }
			r.GlowRadius = f;
		} else if(key == "x" || key == "y" || key == "z" || key == "dx" || key == "dy" || key == "dz") {
			if(!LightParseFloat(val, f) || f < -4096.0f || f > 4096.0f) { logError("DioramaLight : " + key + " doit etre un nombre de -4096 a 4096 : " + val + ", ligne ignoree."); return; }
			if(key[0] == 'd') r.DirGiven = true; else if(key == "y") r.PosYGiven = true;
			if(key == "x") r.X = f; else if(key == "y") r.Y = f; else if(key == "z") r.Z = f;
			else if(key == "dx") r.Dx = f; else if(key == "dy") r.Dy = f; else r.Dz = f;
		} else {
			logError("DioramaLight : option inconnue : " + key + ", ligne ignoree.");
			return;
		}
	}
	if(r.Type == 0) {
		logError("DioramaLight : type= est obligatoire (ambient, point, sun), ligne ignoree.");
		return;
	}
	if(r.HasTile) {
		if(r.Type != 2) {
			logError("DioramaLight : tile= ne vaut que pour type=point, ligne ignoree.");
			return;
		}
		r.AnyPalette = (palStr == "*" || palStr == "all");
		string pd = r.AnyPalette ? string("0") : palStr;
		try {
			ReadTileData(r.Key, tileStr, pd);
		} catch(...) {
			logError("DioramaLight : tile invalide : " + tileStr + ", ligne ignoree.");
			return;
		}
	}
	if(r.Type == 3 && r.Dx == 0.0f && r.Dy == 0.0f && r.Dz == 0.0f) {
		logError("DioramaLight : la direction du soleil ne peut pas etre nulle, ligne ignoree.");
		return;
	}
	if(_data->DioramaLightRules.size() >= 32) {
		logError("DioramaLight : 32 regles au plus, ligne ignoree.");
		return;
	}
	_data->DioramaLightRules.push_back(r);
}

// <DioramaSkybox>color,skytop=RRGGBB,skybottom=RRGGBB : degrade vertical derriere les plans.
// <DioramaSkybox>pano,fichier.png[,yaw,pitch] : panorama equirectangulaire (2:1) qui tourne avec la camera ; cube,fichier.png : cubemap
// (croix 4 x 3 ou 3 x 4, bande 6 x 1 ou 1 x 6) ; cube,left=a.png,front=b.png,right=..,back=..,top=..,bottom=.. : 6 fichiers. Vitesses en degres par image.
// <DioramaSkybox>fichier.png,vitesseX,vitesseY : image (un pixel du PNG = un pixel HD, repetee) qui derive de vitesseX / vitesseY
// pixels NES par image (0 = fixe). Conditions [ ] : la derniere regle vraie l'emporte. Visible la ou L0 est unie (la couleur de L0
// est remplacee) ; pas de skybox si L0 n'est pas unie.
static bool SkyParseHex(const string& v, uint32_t& out)
{
	string h = v;
	if(!h.empty() && h[0] == '#') h = h.substr(1);
	if(h.size() != 6) return false;
	char* endp = nullptr;
	const unsigned long x = strtoul(h.c_str(), &endp, 16);
	if(endp == h.c_str() || *endp != 0) return false;
	out = (uint32_t)x;
	return true;
}

void HdPackLoader::ProcessDioramaSkyboxTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty()) {
		logError("DioramaSkybox : color,skytop=..,skybottom=.. ou fichier.png,vitesseX,vitesseY attendu, ligne ignoree.");
		return;
	}
	HdDioramaSkyboxRule rule;
	rule.Conditions = conditions;
	// offset=<deg> : decalage angulaire du ciel (pitch) en degres. Lu avant les vitesses. Absent : defaut selon la vue (topDown ou side), voir HdNesPack.cpp.
	for(size_t ti = 1; ti < tokens.size();) {
		const string opt = StringUtilities::Trim(tokens[ti]);
		if(StringUtilities::ToLower(opt).compare(0, 7, "offset=") == 0) {
			try { rule.PitchOffset = std::max(-180.0f, std::min(180.0f, std::stof(opt.substr(7)))); rule.HasOffset = true; }
			catch(...) { logError("DioramaSkybox : offset= doit etre un nombre de degres : " + opt + ", ligne ignoree."); return; }
			tokens.erase(tokens.begin() + ti);
		} else ti++;
	}
	const string first = StringUtilities::Trim(tokens[0]);
	if(StringUtilities::ToLower(first) == "color") {
		rule.Mode = 1;
		bool hasTop = false, hasBottom = false;
		for(size_t i = 1; i < tokens.size(); i++) {
			const string opt = StringUtilities::Trim(tokens[i]);
			const size_t eq = opt.find('=');
			if(eq == string::npos) {
				logError("DioramaSkybox : option sans '=' : " + opt + ", ligne ignoree.");
				return;
			}
			const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
			const string val = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(eq + 1)));
			uint32_t c = 0;
			if(key == "skytop") {
				if(!SkyParseHex(val, c)) { logError("DioramaSkybox : skytop doit etre RRGGBB : " + val + ", ligne ignoree."); return; }
				rule.Top = c; hasTop = true;
			} else if(key == "skybottom" || key == "skybotom") {
				if(!SkyParseHex(val, c)) { logError("DioramaSkybox : skybottom doit etre RRGGBB : " + val + ", ligne ignoree."); return; }
				rule.Bottom = c; hasBottom = true;
			} else {
				logError("DioramaSkybox : option inconnue (skytop, skybottom) : " + key + ", ligne ignoree.");
				return;
			}
		}
		if(!hasTop && !hasBottom) {
			logError("DioramaSkybox : color demande skytop= et / ou skybottom=, ligne ignoree.");
			return;
		}
		if(!hasTop) rule.Top = rule.Bottom;
		if(!hasBottom) rule.Bottom = rule.Top;
	} else {
		const string lfirst = StringUtilities::ToLower(first);
		size_t speedFrom = 1;
		if(lfirst == "pano" || lfirst == "cube") {
			// G5b : pano,fichier[,yaw,pitch] ; cube,fichier[,yaw,pitch] (croix ou bande) ; cube,left=..,front=..,right=..,back=..,top=..,bottom=..[,yaw,pitch]
			rule.Mode = (lfirst == "pano") ? 3 : 4;
			if(tokens.size() < 2 || StringUtilities::Trim(tokens[1]).empty()) {
				logError("DioramaSkybox : " + lfirst + " demande un fichier, ligne ignoree.");
				return;
			}
			const string second = StringUtilities::Trim(tokens[1]);
			if(rule.Mode == 4 && second.find('=') != string::npos) {
				const char* names[6] = { "left", "front", "right", "back", "top", "bottom" };
				size_t i = 1;
				for(; i < tokens.size(); i++) {
					const string opt = StringUtilities::Trim(tokens[i]);
					const size_t eq = opt.find('=');
					if(eq == string::npos) break;
					const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
					int f = -1;
					for(int k = 0; k < 6; k++) if(key == names[k]) f = k;
					if(key == "up") f = 4;
					if(key == "down") f = 5;
					if(f < 0) {
						logError("DioramaSkybox : face inconnue (left, front, right, back, top, bottom) : " + key + ", ligne ignoree.");
						return;
					}
					rule.Faces[f] = LoadDioramaTexture(StringUtilities::Trim(opt.substr(eq + 1)));
					if(!rule.Faces[f]) return;
				}
				for(int k = 0; k < 6; k++) {
					if(!rule.Faces[k]) {
						logError(string("DioramaSkybox : cube en 6 fichiers : la face ") + names[k] + " manque, ligne ignoree.");
						return;
					}
				}
				speedFrom = i;
			} else {
				rule.Image = LoadDioramaTexture(second);
				if(!rule.Image) return;
				speedFrom = 2;
			}
		} else {
			rule.Mode = 2;
			rule.Image = LoadDioramaTexture(first);
			if(!rule.Image) return; // l'erreur est deja journalisee
		}
		float* speeds[2] = { &rule.SpeedX, &rule.SpeedY };
		for(size_t i = speedFrom; i < tokens.size() && i < speedFrom + 2; i++) {
			string v = StringUtilities::Trim(tokens[i]);
			const size_t eq = v.find('=');
			if(eq != string::npos) v = StringUtilities::Trim(v.substr(eq + 1));
			char* endp = nullptr;
			const double d = strtod(v.c_str(), &endp);
			if(v.empty() || endp == v.c_str() || *endp != 0 || d < -64.0 || d > 64.0) {
				logError("DioramaSkybox : la vitesse doit etre un nombre de -64 a 64 (pixels NES par image) : " + v + ", ligne ignoree.");
				return;
			}
			*speeds[i - speedFrom] = (float)d;
		}
	}
	if(_data->DioramaSkyboxRules.size() >= 16) {
		logError("DioramaSkybox : 16 regles au plus, ligne ignoree.");
		return;
	}
	_data->DioramaSkyboxRules.push_back(rule);
}

// <DioramaOffCamera>x,y,largeur,hauteur,mode (doc 38) : zone d'ecran en pixels NES ; ses tiles et ses sprites restent a leur
// position 2D d'origine, a plat devant la scene (mode flat). Balise repetable. Pas encore : mode fixed3d.
// Forme bloc <DioramaOffCamera>mode{ <tile>... } : traitee dans LoadPack (marque DioOffCamera sur les tiles du bloc).
void HdPackLoader::ProcessDioramaOffCameraTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(!conditions.empty()) {
		logError("DioramaOffCamera : les conditions ne sont pas prises en charge, ligne ignoree.");
		return;
	}
	if(!tokens.empty() && tokens.back().find('{') != string::npos) {
		logError("DioramaOffCamera : la forme bloc { } n'est pas encore prise en charge, ligne ignoree.");
		return;
	}
	if(tokens.size() < 4 || tokens.size() > 5) {
		logError("DioramaOffCamera : syntaxe <DioramaOffCamera>x,y,largeur,hauteur,mode, ligne ignoree.");
		return;
	}
	int v[4] = {};
	try {
		for(int i = 0; i < 4; i++) {
			const string s = StringUtilities::Trim(tokens[i]);
			size_t used = 0;
			v[i] = std::stoi(s, &used);
			if(used != s.size()) throw std::invalid_argument("suite");
		}
	} catch(...) {
		logError("DioramaOffCamera : x, y, largeur et hauteur doivent etre des entiers, ligne ignoree.");
		return;
	}
	if(tokens.size() == 5) {
		const string mode = StringUtilities::ToLower(StringUtilities::Trim(tokens[4]));
		if(mode == "fixed3d") {
			_data->DioramaFixed3d = true;
		} else if(mode != "flat") {
			logError("DioramaOffCamera : mode inconnu (flat ou fixed3d) : " + mode + ", ligne ignoree.");
			return;
		}
	}
	// Decoupe a l'ecran NES (256 x 240)
	int x0 = std::max(0, v[0]), y0 = std::max(0, v[1]);
	int x1 = std::min(256, v[0] + v[2]), y1 = std::min(240, v[1] + v[3]);
	if(v[2] <= 0 || v[3] <= 0 || x1 <= x0 || y1 <= y0) {
		logError("DioramaOffCamera : zone vide ou hors de l'ecran (256 x 240), ligne ignoree.");
		return;
	}
	HdDioramaOffRect r;
	r.X = x0; r.Y = y0; r.W = x1 - x0; r.H = y1 - y0;
	_data->DioramaOffRects.push_back(r);
}

// <Diorama>mode[,options] (doc 32, 36, 37) : scene diorama. Mode plainTiles seulement. Options (cle=valeur) : z0..z3
// (profondeur des 4 couches), t0..t3 (epaisseur des 4 couches en tiles, 0 a 16, fractions permises ; doc 38),
// side0..side3 / top0..top3 (PNG des flancs / du dessus et du dessous des couches epaisses), stretchx0..3 / stretchy0..3 (et
// topstretchx0..3, topstretchy0..3, sidestretchx0..3, sidestretchy0..3 : etirement des motifs, 1 = taille normale, 0.01 a 100),
// yaw, pitch, dist, orbit=on|off, orbitamp, orbitperiod.
// Sans cette balise, le filtre reste strictement inchange.
void HdPackLoader::ProcessDioramaTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(!conditions.empty()) {
		logError("Diorama : les conditions ne sont pas prises en charge, ligne ignoree.");
		return;
	}
	if(tokens.empty() || StringUtilities::Trim(tokens[0]).empty()) {
		logError("Diorama : syntaxe <Diorama>mode[,options], ligne ignoree.");
		return;
	}
	const string mode = StringUtilities::ToLower(StringUtilities::Trim(tokens[0]));
	if(mode != "plaintiles") {
		logError("Diorama : mode inconnu (seul plainTiles existe pour l'instant) : " + StringUtilities::Trim(tokens[0]) + ", ligne ignoree.");
		return;
	}
	HdDioramaConfig cfg;
	for(size_t i = 1; i < tokens.size(); i++) {
		const string opt = StringUtilities::Trim(tokens[i]);
		const size_t eq = opt.find('=');
		if(eq == string::npos) {
			logError("Diorama : option sans '=' ignoree : " + opt);
			continue;
		}
		const string key = StringUtilities::ToLower(StringUtilities::Trim(opt.substr(0, eq)));
		const string val = StringUtilities::Trim(opt.substr(eq + 1));
		try {
			auto num = [&](float& out) {
				size_t used = 0;
				const float v = std::stof(val, &used);
				if(used != val.size()) throw std::invalid_argument("suite");
				out = v;
			};
			if(key == "z0") num(cfg.Z[0]);
			else if(key == "z1") num(cfg.Z[1]);
			else if(key == "z2") num(cfg.Z[2]);
			else if(key == "z3") num(cfg.Z[3]);
			else if(key.size() >= 2 && key.back() >= '0' && key.back() <= '3' && ParseDioramaWallOption(key.substr(0, key.size() - 1), val, cfg.Wall[key.back() - '0'])) {
				// cle de mur indexee par couche (side2, top2, stretchx2, topstretchy0, ...)
			}
			else if(key == "t0") num(cfg.T[0]);
			else if(key == "t1") num(cfg.T[1]);
			else if(key == "t2") num(cfg.T[2]);
			else if(key == "t3") num(cfg.T[3]);
			else if(key == "aa") {
				float aaValue = 1.0f;
				num(aaValue);
				if(aaValue < 1.0f || aaValue > 3.0f || aaValue != std::floor(aaValue)) {
					logError("Diorama : aa doit valoir 1, 2 ou 3 (surechantillonnage), remis a 1.");
					aaValue = 1.0f;
				}
				cfg.Aa = (int)aaValue;
			}
			else if(key == "yaw") num(cfg.Yaw);
			else if(key == "pitch") num(cfg.Pitch);
			else if(key == "dist") num(cfg.Dist);
			else if(key == "orbitamp") num(cfg.OrbitAmp);
			else if(key == "orbitperiod") num(cfg.OrbitPeriod);
			else if(key == "fixedt") {
				num(cfg.FixedT);
				if(cfg.FixedT < 0.0f || cfg.FixedT > 16.0f) { logError("Diorama : fixedt doit etre de 0 a 16 tiles, remis a 1."); cfg.FixedT = 1.0f; }
			}
			else if(key == "zoom") {
				num(cfg.Zoom);
				if(cfg.Zoom < 0.1f || cfg.Zoom > 4.0f) { logError("Diorama : zoom doit etre de 0.1 a 4, remis a 1."); cfg.Zoom = 1.0f; }
			}
			else if(key == "nstrength") { num(cfg.NormalStrength); if(cfg.NormalStrength < 0.0f || cfg.NormalStrength > 4.0f) { logError("Diorama : nstrength doit etre de 0 a 4, remis a 1."); cfg.NormalStrength = 1.0f; } }
			else if(key == "nflipy") { const string v = StringUtilities::ToLower(val); cfg.NormalFlipY = (v == "on" || v == "1" || v == "true"); }
			else if(key == "extdebug") {
				const string v = StringUtilities::ToLower(val);
				cfg.ExtDebug = (v == "on" || v == "1" || v == "true");
			}
			else if(key == "fixedz") num(cfg.FixedZ);
			else if(key == "fixeddark") {
				num(cfg.FixedDark);
				if(cfg.FixedDark < 0.0f || cfg.FixedDark > 1.0f) { logError("Diorama : fixeddark doit etre de 0 a 1, remis a 0.5."); cfg.FixedDark = 0.5f; }
			}
			else if(key == "fixedpersp") {
				num(cfg.FixedPersp);
				if(cfg.FixedPersp < 0.0f || cfg.FixedPersp > 4.0f) { logError("Diorama : fixedpersp doit etre de 0 a 4, remis a 1."); cfg.FixedPersp = 1.0f; }
			}
			else if(key == "orbit") {
				const string v = StringUtilities::ToLower(val);
				if(v == "on" || v == "1" || v == "true") cfg.Orbit = true;
				else if(v == "off" || v == "0" || v == "false") cfg.Orbit = false;
				else logError("Diorama : orbit=on ou orbit=off, valeur ignoree : " + val);
			} else {
				logError("Diorama : option inconnue, ignoree : " + key);
			}
		} catch(...) {
			logError("Diorama : valeur invalide pour " + key + " : " + val);
		}
	}
	// Bornes : la camera ne coupe pas les plans qui passent derriere elle (pas de decoupage proche), donc on reste loin de ce cas.
	if(cfg.Dist < 16.0f) { logError("Diorama : dist doit etre >= 16, remis a 256."); cfg.Dist = 256.0f; }
	if(cfg.Yaw < -70.0f || cfg.Yaw > 70.0f) { logError("Diorama : yaw limite a +-70 degres."); cfg.Yaw = cfg.Yaw < 0 ? -70.0f : 70.0f; }
	if(cfg.Pitch < -70.0f || cfg.Pitch > 70.0f) { logError("Diorama : pitch limite a +-70 degres."); cfg.Pitch = cfg.Pitch < 0 ? -70.0f : 70.0f; }
	if(cfg.OrbitAmp < 0.0f || cfg.OrbitAmp > 70.0f) { logError("Diorama : orbitamp limite a 0..70 degres."); cfg.OrbitAmp = cfg.OrbitAmp < 0 ? 0.0f : 70.0f; }
	if(cfg.OrbitPeriod < 30.0f) { logError("Diorama : orbitperiod doit etre >= 30 images, remis a 600."); cfg.OrbitPeriod = 600.0f; }
	for(int k = 0; k < 4; k++) {
		if(cfg.Z[k] < -0.9f * cfg.Dist) {
			logError("Diorama : z" + std::to_string(k) + " trop proche de la camera, limite a -0.9 x dist.");
			cfg.Z[k] = -0.9f * cfg.Dist;
		}
		if(cfg.T[k] < 0.0f || cfg.T[k] > 16.0f) {
			logError("Diorama : t" + std::to_string(k) + " limite a 0..16 tiles.");
			cfg.T[k] = cfg.T[k] < 0.0f ? 0.0f : 16.0f;
		}
	}
	_data->DioramaEnabled = true;
	_data->DioramaMode = "plainTiles";
	_data->Diorama = cfg;
}

// <2DPerspective> (NES) : sol incline. Syntaxe : [cond]<2DPerspective>fichier.png,Z,tileX,tileY,alpha,speedX,speedY,horizon=..,depth=..,repeat=..,limit=+N,fog=RRGGBB,fogrange=N
void HdPackLoader::ProcessPerspectiveTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	vector<string> pos;
	std::unordered_map<string, string> cfg;
	for(const string& raw : tokens) {
		string tok = StringUtilities::Trim(raw);
		if(tok.empty()) continue;
		size_t eq = tok.find('=');
		if(eq == string::npos) {
			pos.push_back(tok);
		} else {
			cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
		}
	}
	auto str = [&](const char* k) -> string { auto it = cfg.find(k); return it == cfg.end() ? string() : it->second; };
	auto numOf = [&](const string& v, const char* what, float def) -> float {
		if(v.empty()) return def;
		try { return std::stof(v); } catch(...) { logError("2DPerspective : valeur invalide pour " + string(what) + " : " + v); return def; }
	};
	auto posNum = [&](size_t i, float def) -> float { return numOf(i < pos.size() ? pos[i] : string(), "champ positionnel", def); };
	auto cfgNum = [&](const char* k, float def) -> float { return numOf(str(k), k, def); };

	if(pos.empty()) {
		logError("2DPerspective : fichier PNG manquant.");
		return;
	}
	string mode = StringUtilities::ToLower(str("mode"));
	string side = StringUtilities::ToLower(str("side"));
	if(mode == "wall" && side != "left" && side != "right") {
		logError("2DPerspective : mode=wall demande side=left ou side=right, ignore.");
		return;
	}
	if(!mode.empty() && mode != "floor" && mode != "ceiling" && mode != "wall") {
		logError("2DPerspective : mode=" + mode + " pas encore gere (floor, ceiling et wall).");
		return;
	}

	unique_ptr<NesHdPerspectiveInfo> pp(new NesHdPerspectiveInfo());
	pp->Ceiling = (mode == "ceiling");
	pp->Wall = (mode == "wall");
	pp->WallRight = (side == "right");
	pp->Conditions = conditions;
	pp->Priority = std::clamp((int)posNum(1, 39.0f), 0, 49);
	pp->TileX = posNum(2, 1.0f) != 0.0f;
	pp->TileY = posNum(3, 1.0f) != 0.0f;
	pp->Alpha = std::clamp(posNum(4, 1.0f), 0.0f, 1.0f);
	pp->SpeedX = cfgNum("speedx", posNum(5, 0.0f));
	pp->SpeedY = cfgNum("speedy", posNum(6, 0.0f));
	pp->HorizonSet = !str("horizon").empty();
	pp->Horizon = pp->Wall ? std::max(0.0f, cfgNum("horizon", 0.0f)) : std::clamp(cfgNum("horizon", 120.0f), 0.0f, 239.0f);
	pp->CySet = !str("cy").empty();
	pp->Cy = cfgNum("cy", 120.0f);
	pp->Depth = std::max(1.0f, cfgNum("depth", 64.0f));
	pp->Repeat = std::max(0.0f, cfgNum("repeat", 0.0f));
	string lim = str("limit");
	if(lim.empty() || lim[0] == '+') {
		pp->LimitOffset = std::max(0.0f, cfgNum("limit", 0.0f));
	} else {
		logError("2DPerspective : limit doit etre ecrit +N (relatif a l'horizon), ignore : " + lim);
	}
	pp->FogRange = std::max(0.0f, cfgNum("fogrange", 0.0f));
	string fog = StringUtilities::Trim(str("fog"));
	if(!fog.empty()) {
		if(fog[0] == '#') fog = fog.substr(1);
		try { pp->Fog = (uint32_t)std::stoul(fog, nullptr, 16) & 0xFFFFFF; } catch(...) { logError("2DPerspective : fog invalide : " + fog); }
	}

	string tex = StringUtilities::Trim(pos[0]);
	HdPackBitmapInfo* found = nullptr;
	for(auto& bitmap : _data->ImageFileData) {
		if(bitmap->PngName == tex) {
			found = bitmap.get();
			break;
		}
	}
	if(found == nullptr) {
		_data->ImageFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
		HdPackBitmapInfo* bitmapInfo = _data->ImageFileData.back().get();
		bitmapInfo->PngName = tex;
		if(!LoadFile(tex, bitmapInfo->FileData)) {
			logError("2DPerspective texture not found: " + tex);
		}
		found = bitmapInfo;
	}
	pp->Texture = found;

	_data->Perspectives.push_back(std::move(pp));
}

// <2DReflect> : reflet des sprites sous le bord superieur du PNG du calque sol (vue de profil).
// Syntaxe : <2DReflect>prio=10,intensity=0.5,fade=0   (un seul <2DReflect> par pack, pas de conditions)
void HdPackLoader::ProcessReflectTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(!conditions.empty()) {
		logError("2DReflect : les conditions ne sont pas prises en charge (ligne ignoree)");
		return;
	}
	if(!_data->Reflects.empty()) {
		logError("2DReflect : un seul <2DReflect> par pack (ligne ignoree)");
		return;
	}
	std::unordered_map<string, string> cfg;
	for(const string& raw : tokens) {
		string tok = StringUtilities::Trim(raw);
		size_t eq = tok.find('=');
		if(eq == string::npos) {
			if(!tok.empty()) {
				logError("2DReflect : parametre sans valeur : " + tok);
			}
			continue;
		}
		cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
	}
	auto str = [&](const char* k) -> string {
		auto it = cfg.find(k);
		return it == cfg.end() ? string() : it->second;
	};
	auto num = [&](const char* k, float def) -> float {
		string v = str(k);
		if(v.empty()) return def;
		try { return std::stof(v); } catch(...) { logError("2DReflect : valeur invalide pour " + string(k) + " : " + v); return def; }
	};

	NesHdReflectInfo info;
	info.Priority = std::clamp((int)num("prio", 10.0f), 0, 49);
	info.Intensity = std::clamp(num("intensity", 0.5f), 0.0f, 1.0f);
	info.Fade = std::max(0.0f, num("fade", 0.0f));
	_data->Reflects.push_back(info);
}

// <2DTexture> : texture repetee qui remplace les pixels d'une couleur (TRANS = transparents) d'un <background>
// de meme priorite. Syntaxe : <2DTexture>texture=fichier.png,color=000000|TRANS,prio=10,scalex=100,scaley=100,scrollx=0,scrolly=0
// Pas de conditions pour l'instant (refusees avec une erreur).
void HdPackLoader::ProcessTexture2DTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	if(!conditions.empty()) {
		logError("2DTexture : les conditions ne sont pas prises en charge (ligne ignoree)");
		return;
	}
	std::unordered_map<string, string> cfg;
	for(const string& raw : tokens) {
		string tok = StringUtilities::Trim(raw);
		size_t eq = tok.find('=');
		if(eq == string::npos) {
			if(!tok.empty()) {
				logError("2DTexture : parametre sans valeur : " + tok);
			}
			continue;
		}
		cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
	}
	auto str = [&](const char* k) -> string {
		auto it = cfg.find(k);
		return it == cfg.end() ? string() : it->second;
	};
	auto num = [&](const char* k, float def) -> float {
		string v = str(k);
		if(v.empty()) return def;
		try { return std::stof(v); } catch(...) { logError("2DTexture : valeur invalide pour " + string(k) + " : " + v); return def; }
	};

	string tex = StringUtilities::Trim(str("texture"));
	// Materiau PBR : set=dossier (cartes reconnues par les mots du nom, comme <m7material>) ou normal= roughness= ao=
	string fNormal = str("normal"), fRough = str("roughness"), fAo = str("ao");
	if(fAo.empty()) fAo = str("occlusion");
	int normalConv = -1; // -1 = selon le nom du fichier (dx), 0 = OpenGL, 1 = DirectX
	{ const string nc = StringUtilities::ToLower(str("normalconv")); if(nc == "dx" || nc == "directx") normalConv = 1; else if(nc == "gl" || nc == "opengl") normalConv = 0; }
	const string setDir = StringUtilities::Trim(str("set"));
	if(!setDir.empty()) {
		std::vector<string> names;
		try {
			for(const auto& e : std::filesystem::directory_iterator(FolderUtilities::CombinePath(_hdPackFolder, setDir))) {
				if(!e.is_regular_file()) continue;
				const string ext = StringUtilities::ToLower(e.path().extension().string());
				if(ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") names.push_back(e.path().filename().string());
			}
		} catch(...) { logError("2DTexture : dossier introuvable : " + setDir); }
		std::sort(names.begin(), names.end());
		auto words = [](string s) { s = StringUtilities::ToLower(s); std::vector<string> w; string cur; for(char ch : s) { if(std::isalnum((unsigned char)ch)) cur += ch; else { if(!cur.empty()) w.push_back(cur); cur.clear(); } } if(!cur.empty()) w.push_back(cur); return w; };
		string nGL, nDX, fAlb;
		for(const string& nm : names) {
			const std::vector<string> w = words(nm);
			auto has = [&](std::initializer_list<const char*> lst) { for(const string& x : w) for(const char* y : lst) if(x == y) return true; return false; };
			const string rel = setDir + ((setDir.back() == '/' || setDir.back() == '\\') ? "" : "\\") + nm;
			if(has({ "preview", "thumb", "sphere", "cube" })) continue;
			if(has({ "normal", "normalgl", "normaldx", "nor", "nrm", "normalmap", "norm" })) { string& slot = has({ "normaldx", "dx", "directx" }) ? nDX : nGL; if(slot.empty()) slot = rel; }
			else if(has({ "roughness", "rough", "rgh" })) { if(fRough.empty()) fRough = rel; }
			else if(has({ "ambientocclusion", "occlusion", "ao", "occ" })) { if(fAo.empty()) fAo = rel; }
			else if(has({ "displacement", "disp", "height", "bump", "depth", "metalness", "metallic", "metal", "gloss", "glossiness", "emission", "emissive", "arm", "orm" })) continue;
			else if(has({ "basecolor", "albedo", "diffuse", "diff", "color", "col", "colour" })) { if(fAlb.empty()) fAlb = rel; }
		}
		if(fNormal.empty()) {
			if(!nGL.empty() && normalConv != 1) { fNormal = nGL; if(normalConv < 0) normalConv = 0; }
			else if(!nDX.empty()) { fNormal = nDX; if(normalConv < 0) normalConv = 1; }
		}
		if(tex.empty()) tex = fAlb;
	}
	if(tex.empty()) {
		logError("2DTexture : parametre texture=fichier.png manquant");
		return;
	}
	string colorStr = StringUtilities::ToLower(StringUtilities::Trim(str("color")));
	if(colorStr.empty()) {
		logError("2DTexture : parametre color=RRGGBB ou TRANS manquant");
		return;
	}

	NesHdTextureInfo info;
	if(colorStr == "trans") {
		info.HasKey = false;
	} else {
		if(colorStr[0] == '#') colorStr = colorStr.substr(1);
		try {
			info.Key = (uint32_t)std::stoul(colorStr, nullptr, 16) & 0xFFFFFFu;
		} catch(...) {
			logError("2DTexture : couleur invalide : " + colorStr);
			return;
		}
		info.HasKey = true;
	}

	int prio = std::clamp((int)num("prio", 10.0f), 0, 49);
	float sx = num("scalex", 100.0f);
	float sy = num("scaley", 100.0f);
	if(sx <= 0.0f || sy <= 0.0f) {
		logError("2DTexture : scalex et scaley doivent etre superieurs a 0");
		return;
	}
	info.TexelPerPxX = 100.0f / sx;
	info.TexelPerPxY = 100.0f / sy;
	info.RatioX = num("scrollx", 0.0f);
	info.RatioY = num("scrolly", 0.0f);
	info.Tolerance = std::clamp((int)num("tolerance", 0.0f), 0, 255); // couleurs proches (bords anticrenelés)
	{
		const string dm = StringUtilities::ToLower(StringUtilities::Trim(str("diorama")));
		if(dm.empty() || dm == "on" || dm == "all") info.DioMode = 0;
		else if(dm == "only") info.DioMode = 1;
		else if(dm == "off") info.DioMode = 2;
		else { logError("2DTexture : diorama doit etre on, only ou off : " + dm); return; }
	}

	// Texture chargee une seule fois, partagee par nom (comme <2DLiquid>)
	HdPackBitmapInfo* found = nullptr;
	for(auto& bitmap : _data->ImageFileData) {
		if(bitmap->PngName == tex) {
			found = bitmap.get();
			break;
		}
	}
	if(found == nullptr) {
		_data->ImageFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
		HdPackBitmapInfo* bitmapInfo = _data->ImageFileData.back().get();
		bitmapInfo->PngName = tex;
		if(!LoadFile(tex, bitmapInfo->FileData)) {
			logError("2DTexture texture not found: " + tex);
		}
		found = bitmapInfo;
	}
	info.Texture = found;
	// Materiau PBR : normale, AO et rugosite reunies dans une image (taille de la normale, sinon de la premiere carte trouvee)
	if(!fNormal.empty() || !fRough.empty() || !fAo.empty()) {
		auto readImg = [&](const string& f, std::vector<uint32_t>& px, uint32_t& w, uint32_t& h) -> bool {
			if(f.empty()) return false;
			std::vector<uint8_t> raw;
			if(!LoadFile(f, raw)) { logError("2DTexture : carte introuvable : " + f); return false; }
			if(PNGHelper::ReadPNG(raw, px, w, h)) return true;
			if(SnesHdDecodeImageFallback(raw, px, w, h)) return true;
			logError("2DTexture : format d'image non lu : " + f);
			return false;
		};
		std::vector<uint32_t> pn, pr, pa; uint32_t wn = 0, hn = 0, wr = 0, hr = 0, wa = 0, ha = 0;
		const bool hasN = readImg(fNormal, pn, wn, hn), hasR = readImg(fRough, pr, wr, hr), hasA = readImg(fAo, pa, wa, ha);
		if(normalConv < 0) normalConv = 0;
		const float roughDef = std::clamp(num("rough", 0.8f), 0.0f, 1.0f);
		uint32_t W = hasN ? wn : (hasR ? wr : wa), H = hasN ? hn : (hasR ? hr : ha);
		if((hasN || hasR || hasA) && W > 0 && H > 0) {
			auto m = std::make_shared<std::vector<uint32_t>>((size_t)W * H);
			auto at = [](const std::vector<uint32_t>& p, uint32_t pw, uint32_t ph, uint32_t x, uint32_t y, uint32_t W, uint32_t H) {
				return p[(size_t)((uint64_t)y * ph / H) * pw + (size_t)((uint64_t)x * pw / W)];
			};
			for(uint32_t y = 0; y < H; y++) {
				for(uint32_t x = 0; x < W; x++) {
					uint32_t nx = 128, ny = 128, ao = 255, ro = (uint32_t)(roughDef * 255.0f + 0.5f);
					if(hasN) { const uint32_t c = at(pn, wn, hn, x, y, W, H); nx = (c >> 16) & 0xFF; ny = (c >> 8) & 0xFF; if(normalConv == 1) ny = 255 - ny; }
					if(hasR) ro = (at(pr, wr, hr, x, y, W, H) >> 16) & 0xFF;
					if(hasA) ao = (at(pa, wa, ha, x, y, W, H) >> 16) & 0xFF;
					(*m)[(size_t)y * W + x] = (ro << 24) | (nx << 16) | (ny << 8) | ao;
				}
			}
			info.Mat = m; info.MatW = W; info.MatH = H;
			info.NStrength = std::clamp(num("strength", 1.0f), 0.0f, 5.0f);
			info.AoStrength = std::clamp(num("aostrength", 1.0f), 0.0f, 2.0f);
			info.Specular = std::clamp(num("specular", 0.5f), 0.0f, 4.0f);
			info.FlowX = num("flowx", 0.0f); info.FlowY = num("flowy", 0.0f);
			info.HasFlow2 = !str("flow2x").empty() || !str("flow2y").empty();
			info.Flow2X = num("flow2x", 0.0f); info.Flow2Y = num("flow2y", 0.0f);
			info.Flow2Scale = std::clamp(num("flow2scale", 1.37f), 0.05f, 20.0f);
		}
	}
	_data->Textures2D[prio].push_back(info);
}

// <2DLiquid> : liquide 2D, vue de profil (phase 1). Syntaxe : [cond]<2DLiquid>cle=valeur,...
// Cles : view (side seulement pour l'instant), prio (0-49, defaut 40), level, depth, xmin, xmax (px NES),
// amplitude, wavelength, speed, phase, scrollx, texture, texscale, color, deep, foam, alpha.
void HdPackLoader::ProcessLiquidTag(vector<string>& tokens, vector<HdPackCondition*> conditions)
{
	std::unordered_map<string, string> cfg;
	for(const string& raw : tokens) {
		string tok = StringUtilities::Trim(raw);
		size_t eq = tok.find('=');
		if(eq == string::npos) {
			if(!tok.empty()) {
				logError("2DLiquid : parametre sans valeur : " + tok);
			}
			continue;
		}
		cfg[StringUtilities::ToLower(StringUtilities::Trim(tok.substr(0, eq)))] = StringUtilities::Trim(tok.substr(eq + 1));
	}

	auto str = [&](const char* k) -> string {
		auto it = cfg.find(k);
		return it == cfg.end() ? string() : it->second;
	};
	auto num = [&](const char* k, float def) -> float {
		string v = str(k);
		if(v.empty()) return def;
		try { return std::stof(v); } catch(...) { logError("2DLiquid : valeur invalide pour " + string(k) + " : " + v); return def; }
	};
	auto colorOf = [&](const char* k, uint32_t def) -> uint32_t {
		string v = StringUtilities::Trim(str(k));
		if(v.empty()) return def;
		if(v[0] == '#') v = v.substr(1);
		try { return (uint32_t)std::stoul(v, nullptr, 16) & 0xFFFFFF; } catch(...) { logError("2DLiquid : couleur invalide pour " + string(k) + " : " + v); return def; }
	};

	string view = StringUtilities::ToLower(str("view"));
	if(view.empty()) view = "side";
	if(view != "side" && view != "top") {
		logError("2DLiquid : view=" + view + " inconnu (side ou top)");
		return;
	}

	auto liquid = std::make_unique<NesHdLiquidInfo>();
	liquid->Conditions = conditions;
	liquid->Level = std::clamp(num("level", 176.0f), 0.0f, 240.0f);
	liquid->Depth = std::max(1.0f, num("depth", 64.0f));
	liquid->XMin = std::clamp(num("xmin", 0.0f) / 256.0f, 0.0f, 1.0f);
	liquid->XMax = std::clamp(num("xmax", 256.0f) / 256.0f, 0.0f, 1.0f);
	liquid->Amp = std::max(0.0f, num("amplitude", 3.0f));
	liquid->Wave = std::max(1.0f, num("wavelength", 48.0f));
	liquid->Speed = num("speed", 0.8f);
	liquid->Phase = num("phase", 0.0f);
	liquid->ScrollX = num("scrollx", 0.0f);
	liquid->FollowX = num("followx", 0.0f);
	liquid->FollowY = num("followy", 0.0f);
	liquid->TexScale = std::max(0.01f, num("texscale", 1.0f));
	liquid->Foam = std::clamp(num("foam", 0.6f), 0.0f, 1.0f);
	liquid->Alpha = std::clamp(num("alpha", 1.0f), 0.0f, 1.0f);
	liquid->Priority = std::clamp((int)num("prio", 40.0f), 0, 49);
	liquid->MaskBg = num("mask", 0.0f) >= 0.5f;
	uint32_t mk = colorOf("maskcolor", 0xFFFFFFFFu);
	liquid->HasMaskKey = mk <= 0xFFFFFFu;
	liquid->MaskKey = mk & 0xFFFFFFu;
	liquid->NormalMode = num("normal", 0.0f) >= 0.5f;
	liquid->Specular = std::clamp(num("specular", 0.6f), 0.0f, 1.0f);
	liquid->Refract = std::max(0.0f, num("refract", 4.0f));
	liquid->IsTop = (view == "top");
	liquid->YMin = std::clamp(num("ymin", 0.0f), 0.0f, 240.0f);
	liquid->YMax = std::max(liquid->YMin + 1.0f, std::clamp(num("ymax", 240.0f), 0.0f, 240.0f));
	liquid->Glow = std::clamp(num("glow", 0.0f), 0.0f, 1.0f);
	liquid->GlowRadius = std::max(1.0f, num("glowradius", 24.0f));
	liquid->BubbleCount = std::clamp((int)num("bubbles", 0.0f), 0, 64);
	liquid->BubbleSize = std::max(0.5f, num("bubblesize", 1.5f));
	liquid->BubbleRise = std::max(0.0f, num("bubblerise", 0.4f));
	liquid->Color = colorOf("color", 0x1E6FB0);
	liquid->Deep = colorOf("deep", 0x0B2F5C);

	string tex = StringUtilities::Trim(str("texture"));
	if(!tex.empty()) {
		HdPackBitmapInfo* found = nullptr;
		for(auto& bitmap : _data->ImageFileData) {
			if(bitmap->PngName == tex) {
				found = bitmap.get();
				break;
			}
		}
		if(found == nullptr) {
			_data->ImageFileData.push_back(unique_ptr<HdPackBitmapInfo>(new HdPackBitmapInfo()));
			HdPackBitmapInfo* bitmapInfo = _data->ImageFileData.back().get();
			bitmapInfo->PngName = tex;
			if(!LoadFile(tex, bitmapInfo->FileData)) {
				logError("2DLiquid texture not found: " + tex);
			}
			found = bitmapInfo;
		}
		liquid->Texture = found;
	}

	_data->Liquids.push_back(std::move(liquid));
}
