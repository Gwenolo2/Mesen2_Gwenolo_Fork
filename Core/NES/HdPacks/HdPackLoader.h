#pragma once
#include <map>
#include "pch.h"
#include "NES/HdPacks/HdData.h"
#include "Utilities/ZipReader.h"
#include "Utilities/VirtualFile.h"

enum class HdPackConditionOperator;

class HdPackLoader
{
public:
	static bool LoadHdNesPack(string definitionFile, HdPackData &outData);
	static bool LoadHdNesPack(VirtualFile &romFile, HdPackData &outData);




private:
	HdPackData* _data = nullptr;
	bool _loadFromZip = false;
	int _currentLine = 0;
	int _errorCount = 0;
	// <include> : origine (fichier, ligne) de chaque ligne du hires.txt une fois les inclusions resolues
	vector<std::pair<string, int>> _lineOrigins;
	bool _hasIncludes = false;
	ZipReader _reader;
	std::map<string, string> _randomChoices; // tirage Randomize unique par pack
	vector<string> _zipEntries;             // entrees du zip, lues une fois si besoin
	string _hdPackDefinitionFile;
	string _hdPackFolder;
	unordered_map<string, HdPackCondition*> _conditionsByName;
	unordered_map<string, HdPackBitmapInfo*> _backgroundsByName;
	// Pile des blocs <Ignore2DOverlays>{ ... } ouverts : chaque entree = conditions du bloc (heritees)
	vector<vector<HdPackCondition*>> _ign2dBlockStack;

	HdPackLoader();

	bool InitializeLoader(VirtualFile &romPath, HdPackData *data);
	bool LoadFile(string filename, vector<uint8_t> &fileData);
	bool CheckFile(string filename);

	bool LoadPack();
	void ExpandIncludes(const vector<uint8_t>& input, const string& displayName, vector<uint8_t>& output, vector<string>& includeStack);
	string GetLineLocation();
	void InitializeHdPack();
	void LoadCustomPalette();
	
	void ReadTileData(HdTileKey& key, string& tileData, string& palData);

	template<typename T> void AddGlobalCondition(string name);
	void InitializeGlobalConditions();

	//Video
	bool ProcessImgTag(string src);
	int ResolveTileBitmapIndex(string imageParam);
	bool ResolveRandomPath(const string& path, string& resolved);
	void ProcessPatchTag(vector<string> &tokens);
	void ProcessOverscanTag(vector<string> &tokens);
	void ProcessConditionTag(vector<string> &tokens, bool createInvertedCondition);
	HdPackConditionOperator ParseConditionOperator(string& opString);
	void ProcessTileTag(vector<string> &tokens, vector<HdPackCondition*> conditions);
	void ProcessBackgroundTag(vector<string> &tokens, vector<HdPackCondition*> conditions);
	void ProcessIgnore2DTag(vector<string> &tokens, vector<HdPackCondition*> conditions);
	void ProcessAdditionTag(vector<string>& tokens);
	void ProcessFallbackTag(vector<string>& tokens);
	void ProcessOptionTag(vector<string>& tokens);

	//Audio
	int ProcessSoundTrack(string albumString, string trackString, string filename);
	

	/*--------------------------------- Added by Gwenolo  ----------------------------------*/
	/* added by Gwenolo */
	void ProcessPlayConditionalBgmTag(vector<string>& tokens, vector<HdPackCondition*>& conditions);
	void ProcessPlayConditionalSfxTag(vector<string>& tokens, vector<HdPackCondition*>& conditions);
	/*-------------------------------------------------------------------*/

	void ProcessBgmTag(vector<string> &tokens);
	void ProcessVideoTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessOverlayTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessSfxTag(vector<string> &tokens);

	vector<HdPackCondition*> ParseConditionString(string conditionString);
	bool ParseBooleanValue(string value);
};