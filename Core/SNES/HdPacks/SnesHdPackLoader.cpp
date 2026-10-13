#include "pch.h"
#include <filesystem>
#include <random>
#include "SNES/HdPacks/SnesHdPackLoader.h"
#include "SNES/HdPacks/SnesHdPackConditions.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/VirtualFile.h"
#include <fstream>
#include <iostream>
#include <algorithm>

#include "SNES/SnesPpu.h"
#include "SNES/SnesPpuTypes.h"

// --- DÉCODEUR VIDÉO MPEG-1 ---
#define PL_MPEG_IMPLEMENTATION
#include "Shared/pl_mpeg.h" // Modifie le chemin si tu l'as mis dans un autre dossier
// -----------------------------

namespace
{
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
			} else {
				MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <fallback> invalid (3 parameters are required).");
			}
			continue;
		}

		// ====================================================================
		// DÉTECTION DE LA FERMETURE DU BLOC '}'
		// ====================================================================
		if(lineContent == "}" || lineContent == "}>") { //-----------------------------------
			if(!fallbackStack.empty()) {
				fallbackStack.pop_back();
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

		/// <IMG> TAGS
		if(lineContent.substr(0, 5) == "<img>") {
			imageCatalog.push_back(lineContent.substr(5));
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

						uint32_t palHash = 0;
						bool ignorePal = (tokens[5] == "ignore");

						if(!ignorePal) {
							palHash = HexUtilities::FromHex(tokens[5]);
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
		else if(lineContent.substr(0, 12) == "<background>") {
			bool tileX = false;
			bool tileY = false;

			std::string data = lineContent.substr(12);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 1) {
				auto bg = std::make_unique<SnesHdBackgroundInfo>();

				std::string filename = StringUtilities::Trim(tokens[0]);
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
				outData->Backgrounds.push_back(std::move(bg));
			}
		}// END BACKGROUND TAG PARSING 

		//---------------------------------------------------------------------------
		/// PARSING <m7img> TAGS
	
	else if(lineContent.substr(0, 7) == "<m7img>") {
		std::string data = lineContent.substr(7);
		std::vector<std::string> tokens = StringUtilities::Split(data, ',');

		if(tokens.size() >= 3) {
			std::string filename = StringUtilities::Trim(tokens[0]);
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
				std::string filename = StringUtilities::Trim(tokens[0]);
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
				MessageManager::Log("[MOUCHARD PARSEUR TILEM7] Tuile chargee:  " + filename + " | Hash: " + HexUtilities::ToHex(tileHash) +
									" | X: " + std::to_string(srcX) + " | Y: " + std::to_string(srcY));
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
			}
		}

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
						track.Filename = StringUtilities::Trim(msuTokens[1]);
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



/// PARSING <VIDEO> TAGS --------------------------------
		else if(lineContent.substr(0, 7) == "<video>") {
			std::string data = lineContent.substr(7);
			std::vector<std::string> tokens = StringUtilities::Split(data, ',');

			if(tokens.size() >= 4) {
				auto video = std::make_unique<SnesHdVideoInfo>();

				// 1. Nom du fichier
				video->Filename = StringUtilities::Trim(tokens[0]);

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
					video->Loop = (loopStr == "true" || loopStr == "1" || loopStr == "yes");
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

				// MOUCHARD PARSEUR
				MessageManager::Log("[SnesHDPack] Line " + lineLocation +
					" - [HD VIDEO] Tag found : " + video->Filename +
					" | PlayOnlyOnce: " + (video->PlayOnlyOnce ? "YES" : "NO") +
					" | Z-Index: " + std::to_string(video->ZIndex));

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




void SnesHdPackLoader::Clear() {
    //_tileMap.clear();
    //_sheetImages.clear();
	ActivePack = nullptr; // Cela va automatiquement vider les tuiles, images et conditions de la RAM !
	_isLoaded = false;
}
//-----------------------------------------------------------------------------------------------------
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

	// 1. Recherche prioritaire : Palette spécifique explicite
	auto it = ActivePack->TileByKey.find(key);
	if(it != ActivePack->TileByKey.end()) {
		for(SnesHdPackTileInfo* tile : it->second) {
			if(tile->MatchesCondition(x, y, nullptr)) {
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
				if(tile->MatchesCondition(x, y, nullptr)) {
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
				if(tile->MatchesCondition(x, y, nullptr)) {
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
					if(tile->MatchesCondition(x, y, nullptr)) {
						return tile;
					}
				}
			}
		}
	}

	return nullptr;
}
//----------------------------------------------------------------------------
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

	bitmapInfo->PngName = content;

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