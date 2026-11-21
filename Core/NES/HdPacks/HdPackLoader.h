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
	// Blocs { } ouverts, dans l'ordre : 'i' = <Ignore2DOverlays>{, 'o' = <DioramaOffCamera>{ (doc 38). '}' ferme le dernier.
	vector<char> _blockKinds;
	int _dioOffDepth = 0;
	vector<int> _dioShadowStack; // G6c : regle (numero + 1) de chaque bloc <DioramaShadow>{ ouvert (type 's' dans _blockKinds)
	int _dioIgnoreDepth = 0; // blocs <DioramaIgnore>{ ouverts (type 'g' dans _blockKinds)

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
	void ProcessTileTag(vector<string> &tokens, vector<HdPackCondition*> conditions, bool hFlip = false, bool vFlip = false);
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
	void ProcessPerspectiveTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessWatchMemoryAddressTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaOffCameraTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaExtendTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaSkyboxTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaLightTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaPadTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaCameraTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaCameraDefaultTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaCameraKeysTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaViewTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaFogTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaCloudsTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaMistTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDiorama3DParticlesTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDayNightTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	bool ParseDioramaShadowOptions(vector<string>& tokens, HdDioramaShadowRule& rule, bool allowTile);
	void ProcessDioramaShadowTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaNormalTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	HdPackBitmapInfo* LoadAuxBitmap(string name); // PNG hors de la liste des <img> (reutilise celui d'un <img> de meme nom)
	void ProcessDioramaObjectTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessDioramaTile2ObjTag(vector<string>& tokens, vector<HdPackCondition*> conditions, bool sprite = false, bool world = false); // sprite = <Sprite2Obj>, world = <DioramaObject> anchor=
	std::map<string, std::shared_ptr<vector<float>>> _dioObjCache; // H1 : .obj deja lus
	void ProcessDioramaDepthTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	bool ParseDioramaDepthOptions(vector<string>& tokens, HdDioramaDepthGroup& group);
	HdPackBitmapInfo* LoadDioramaTexture(const string& filename);
	bool ParseDioramaWallOption(const string& base, const string& val, HdDioramaWallTex& wall);
	int AddDioramaDepthGroup(const HdDioramaDepthGroup& group);
	vector<int> _dioDepthStack; // groupe (1 a 4, 0 = aucun) de chaque bloc <DioramaDepth>{ ouvert (type 'd' dans _blockKinds)
	void ProcessSpriteLightTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessLiquidTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessTexture2DTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessReflectTag(vector<string>& tokens, vector<HdPackCondition*> conditions);
	void ProcessSfxTag(vector<string> &tokens);

	vector<HdPackCondition*> ParseConditionString(string conditionString);
	bool ParseBooleanValue(string value);
};