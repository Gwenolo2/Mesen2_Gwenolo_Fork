#include "pch.h"
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include "NES/HdPacks/HdPackLoader.h"
#include "NES/HdPacks/HdPackConditions.h"
#include "NES/HdPacks/HdNesPack.h"
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
					if(_ign2dBlockStack.empty()) {
						logError("'}' closing without any corresponding <Ignore2DOverlays>{ block.");
					} else {
						_ign2dBlockStack.pop_back();
					}
					continue;
				}
				if(trimmed.size() >= 18 && StringUtilities::ToLower(trimmed.substr(0, 18)) == "<ignore2doverlays>" && StringUtilities::Trim(trimmed.substr(18)).substr(0, 1) == "{") {
					vector<HdPackCondition*> merged = _ign2dBlockStack.empty() ? vector<HdPackCondition*>() : _ign2dBlockStack.back();
					merged.insert(merged.end(), conditions.begin(), conditions.end());
					_ign2dBlockStack.push_back(merged);
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
	if(!type.empty() && type != "particles" && type != "plainscreen" && type != "ellipse" && type != "shadows" && type != "clouds" && type != "fog" && type != "rays" && type != "distortion" && type != "aurora") {
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
	overlay->Prewarm = boolean("prewarm", true);

	overlay->IsPlainScreen = (type == "plainscreen");
	overlay->IsEllipse = (type == "ellipse");
	overlay->IsNoise = (type == "shadows" || type == "clouds" || type == "fog");
	overlay->NoiseKind = (type == "clouds") ? 1 : ((type == "fog") ? 2 : 0);
	overlay->IsRays = (type == "rays");
	overlay->IsAurora = (type == "aurora");
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
	if(overlay->IsPlainScreen || overlay->IsEllipse || overlay->IsNoise || overlay->IsRays || overlay->IsAurora) {
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
		const uint32_t defCol = overlay->IsEllipse ? (overlay->FxNegative ? 0x000000 : 0xFFFFFF) : (type == "fog" ? 0xA8B8C8 : (type == "clouds" ? 0xFFFFFF : 0x000000));
		overlay->FxColor = namedColor("color", defCol);
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
	if(value != "Y" && value != "N") {
		logError("Invalid boolean value: " + value);
	}
	return value == "Y";
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
	liquid->TexScale = std::max(0.01f, num("texscale", 1.0f));
	liquid->Foam = std::clamp(num("foam", 0.6f), 0.0f, 1.0f);
	liquid->Alpha = std::clamp(num("alpha", 1.0f), 0.0f, 1.0f);
	liquid->Priority = std::clamp((int)num("prio", 40.0f), 0, 49);
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
