#include "pch.h"
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <algorithm>

#include "SNES/HdPacks/SnesHdPackBuilder.h"
#include "Utilities/CRC32.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/PNGHelper.h"

#include "Utilities/StringUtilities.h"

#include <string>


#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

std::string GetCurrentDateTimeString()
{
	auto now = std::chrono::system_clock::now();
	std::time_t time = std::chrono::system_clock::to_time_t(now);
	std::tm tm_local;

#if defined(_WIN32)
	// Version sécurisée pour Windows / Visual Studio
	localtime_s(&tm_local, &time);
#else
	// Version sécurisée pour Mac / Linux (POSIX)
	localtime_r(&time, &tm_local);
#endif

	std::stringstream ss;
	ss << std::put_time(&tm_local, "%Y-%m-%d %H:%M");
	return ss.str();
}


/* ============================================================================
 * SNES MODE 7 HD PACK BUILDER - IMPLEMENTATION SUMMARY
 * ============================================================================
 * * This module implements the extraction (dumping) of Mode 7 graphics for
 * HD Pack creation. Unlike standard SNES backgrounds (which use bitplanes),
 * Mode 7 treats the first 32KB of VRAM as a 1024x1024 chunky pixel map
 * (1 byte = 1 pixel color index).
 * 
 * * KEY ARCHITECTURE & LOGIC:
 * 
 * * 1. PPU Hook & Detection:
 * - The extraction is triggered at the end of the frame (VBlank/NMI) in
 * SnesPpu::ProcessEndOfScanline.
 * - We only dump if Mode 7 was actually rendered during the frame
 * (using a tracking boolean `_mode7UsedThisFrame`), bypassing aggressive
 * mid-screen `ForcedBlank` optimizations used by games like Super Mario Kart.
 * 
 * * 2. Anti-Combinatorial Hashing (The "Coin" Problem):
 * - Games like Super Mario Kart draw dynamic elements (coins, ? blocks)
 * directly into the Mode 7 tilemap. Hashing the entire 32KB VRAM would
 * generate a new PNG every time a coin is collected or animated (millions of files).
 * - SOLUTION: We generate a "Track Signature Hash" using only the first 512
 * bytes of VRAM (the top-left corner of the map, usually static background
 * like water/grass). This uniquely identifies the track without being
 * affected by gameplay elements.
 * 
 * * 3. Dual-Output Generation:
 *  * To allow artists to properly redraw dynamic elements, two files are dumped per track:
 * - [m7_HASH.png]: The full 1024x1024 map (scaled). Artists use this to paint
 * a clean, static background track (erasing baked-in coins/items).
 * - [m7tiles_HASH.png]: A 16x16 grid containing all 256 unique 8x8 Mode 7 tiles.
 * Artists use this to provide HD versions of dynamic elements (coins).
 * The rendering engine will superimpose these HD tiles over the static map.
 * 
 * * 4. Static Class Design:
 * - The builder operates using static variables (e.g., `_m7TexturesDone`, `_m7Rows`).
 * - Tags (`<m7img>`, `<m7tiles>`) are buffered in a std::stringstream and written
 * to `snes_hires.txt` during StopRecording().
 * - Crucially, static buffers are cleared during StopRecording() to prevent
 * memory leaks and cross-game contamination.
 * ============================================================================
 */



// Définition des variables statiques
std::unordered_set<uint32_t> SnesHdPackBuilder::_m7TexturesDone;
std::stringstream SnesHdPackBuilder::_m7Rows;
std::unordered_set<uint32_t> SnesHdPackBuilder::_m7TilesetsDone;

bool SnesHdPackBuilder::_isActive = false;
HdPackBuilderOptions SnesHdPackBuilder::_options;
std::string SnesHdPackBuilder::_saveFolder = "";
std::unordered_set<SnesHdTileKey> SnesHdPackBuilder::_processedTiles;
std::unordered_map<uint32_t, std::vector<SnesHdTileImage>>
    SnesHdPackBuilder::_paletteTiles;


// ==========================================================
// Variables statiques pour le Smart Rebuild
// ==========================================================
std::unordered_map<uint64_t, SnesHdPackBuilder::PngReference> SnesHdPackBuilder::_smartRebuildMap;
//std::vector<std::string> SnesHdPackBuilder::_smartRebuildImages;
std::vector<SnesHdPackBuilder::PreMappedTile> SnesHdPackBuilder::_preMappedTiles;

std::vector<SnesHdPackBuilder::SmartRebuildImage> SnesHdPackBuilder::_smartRebuildImages;
std::map<int, std::set<std::pair<int, int>>> SnesHdPackBuilder::_usedSmartBlocks;

std::unordered_set<uint32_t> SnesHdPackBuilder::_m7SingleTilesDone;

bool SnesHdPackBuilder::IsActive() { return _isActive; }
//--------------------------------------------------------------------------------------------------------

void SnesHdPackBuilder::StartRecording(HdPackBuilderOptions options) {
  _options = options;
  _saveFolder = options.SaveFolder;
  _processedTiles.clear();
  _paletteTiles.clear();
  _isActive = true;

  // Log de confirmation
  std::string logPath =
	  FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
  std::ofstream log(logPath);
  if(log.is_open()) {
	  log << "Recording initialized.\n";
	  log << "Root: " << _saveFolder << "\n";
	  log.close();
  }


  _smartRebuildMap.clear();
  _smartRebuildImages.clear();
  _preMappedTiles.clear();

  // ==========================================================
	 // --- SMART REBUILD : PRÉ-CHARGEMENT DES PNG ---
	 // ==========================================================
	 // On utilise des noms de variables uniques pour ne pas entrer en conflit avec la suite de ta fonction
  std::string smartLogPath = FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
  std::ofstream smartLog(smartLogPath, std::ios::app);

  if(_options.BuildFromPngFolder && !_saveFolder.empty()) {
	  if(smartLog.is_open()) smartLog << "\n--- SMART REBUILD INITIALIZATION ---\n";
	  if(smartLog.is_open()) smartLog << "Scanning folder: " << _saveFolder << "\n";

	  if(std::filesystem::exists(_saveFolder)) {
		  int imageIndexOffset = 0;
		  // CORRECTION: uint32_t pour matcher avec width et height !
		  uint32_t tileSize = 8 * (_options.Scale > 0 ? _options.Scale : 1);

		  _usedSmartBlocks.clear(); // On remet le traqueur à zéro !

		  for(const auto& entry : std::filesystem::recursive_directory_iterator(_saveFolder)) {
			  if(entry.is_regular_file() && entry.path().extension() == ".png") {

				  std::string filePath = entry.path().string();

				  // ANTI-BOUCLE INFINE : On ignore les fichiers _REBUILD existants !
				  if(filePath.find("_REBUILD.png") != std::string::npos) {
					  continue;
				  }

				  std::string relativePath = std::filesystem::relative(entry.path(), _saveFolder).string();
				  std::replace(relativePath.begin(), relativePath.end(), '\\', '/');

				  // Préparation du futur nom "_REBUILD"
				  std::string rebuildPath = relativePath;
				  size_t dotPos = rebuildPath.find_last_of('.');
				  if(dotPos != std::string::npos) {
					  rebuildPath.insert(dotPos, "_REBUILD");
				  } else {
					  rebuildPath += "_REBUILD.png";
				  }

				  if(smartLog.is_open()) smartLog << "Found PNG: " << relativePath << "\n";

				  std::vector<uint32_t> pixels;
				  uint32_t width = 0, height = 0;

				  // 1. On charge le fichier binaire en mémoire (C++ Standard)
				  std::ifstream file(filePath, std::ios::binary | std::ios::ate);
				  if(file.is_open()) {
					  std::streamsize size = file.tellg();
					  file.seekg(0, std::ios::beg);
					  std::vector<uint8_t> fileBytes(size);

					  if(file.read((char*)fileBytes.data(), size)) {

						  // 2. On décode les octets du fichier en pixels uint32_t
						  if(PNGHelper::ReadPNG(fileBytes, pixels, width, height)) {

							  if(smartLog.is_open()) smartLog << "  -> Successfully read pixels. Dimensions: " << width << "x" << height << "\n";

							  // ON UTILISE LA NOUVELLE STRUCTURE
							  SmartRebuildImage imgInfo;
							  imgInfo.OriginalAbsPath = filePath;
							  imgInfo.OriginalRelativePath = relativePath;
							  imgInfo.RebuildRelativePath = rebuildPath;
							  _smartRebuildImages.push_back(imgInfo);

							  for(uint32_t y = 0; y + tileSize <= height; y += tileSize) {
								  for(uint32_t x = 0; x + tileSize <= width; x += tileSize) {

									  uint64_t pixelHash = 14695981039346656037ULL;
									  bool isEmpty = true;

									  for(uint32_t py = 0; py < tileSize; py++) {
										  for(uint32_t px = 0; px < tileSize; px++) {
											  uint32_t color = pixels[(y + py) * width + (x + px)];

											  if((color >> 24) == 0) {
												  color = 0x00000000;
											  } else {
												  isEmpty = false;
											  }

											  pixelHash ^= color;
											  pixelHash *= 1099511628211ULL;
										  }
									  }

									  if(!isEmpty && _smartRebuildMap.find(pixelHash) == _smartRebuildMap.end()) {
										  _smartRebuildMap[pixelHash] = { imageIndexOffset, (int)x, (int)y };
									  }
								  }
							  }
							  imageIndexOffset++;
						  } else {
							  if(smartLog.is_open()) smartLog << "  -> ERROR: Failed to decode PNG format.\n";
						  }
					  } else {
						  if(smartLog.is_open()) smartLog << "  -> ERROR: Failed to read file bytes.\n";
					  }
				  } else {
					  if(smartLog.is_open()) smartLog << "  -> ERROR: Failed to open file.\n";
				  }
			  }
		  }
		  if(smartLog.is_open()) smartLog << "Smart Rebuild Map populated with " << _smartRebuildMap.size() << " unique tiles.\n";
	  } else {
		  if(smartLog.is_open()) smartLog << "Folder does not exist!\n";
	  }
  } else {
	  if(smartLog.is_open()) smartLog << "Smart Rebuild skipped (Option disabled or SaveFolder empty).\n";
  }
  if(smartLog.is_open()) smartLog.close();
  // ==========================================================

  // ==========================================================
	// --- PRÉ-CHARGEMENT DU MODE 7 (UPDATE MODE) ---
	// ==========================================================
  if(_options.UpdateIfExists) {
	  std::string filePath = FolderUtilities::CombinePath(_saveFolder, "snes_hires.txt");
	  std::ifstream inFile(filePath);

	  if(inFile.is_open()) {
		  std::string line;
		  while(std::getline(inFile, line)) {

			  // 1. Détection des Textures M7
			  if(line.substr(0, 7) == "<m7img>") {
				  std::vector<std::string> tokens = StringUtilities::Split(line.substr(7), ',');
				  if(tokens.size() > 1) {
					  uint64_t hash = std::stoull(StringUtilities::Trim(tokens[1]), nullptr, 16);
					  _m7TexturesDone.insert(hash);
				  }
			  }
			  // 2. Détection des Tilesets M7
			  else if(line.substr(0, 9) == "<m7tiles>") {
				  std::vector<std::string> tokens = StringUtilities::Split(line.substr(9), ',');
				  if(tokens.size() > 1) {
					  uint64_t hash = std::stoull(StringUtilities::Trim(tokens[1]), nullptr, 16);
					  _m7TilesetsDone.insert(hash);
				  }
			  }
		  }
		  inFile.close();
	  }
  }



  if (!_saveFolder.empty()) {
    // 1. Créer le dossier racine du Pack (ex: Documents/Mesen/HdPacks/MonJeu)
    FolderUtilities::CreateFolder(_saveFolder);

    // 2. Créer le dossier Tiles
    std::string tilesPath = FolderUtilities::CombinePath(_saveFolder, "Tiles");
    FolderUtilities::CreateFolder(tilesPath);

    // 3. Créer les sous-dossiers spécifiques
    std::string bgPath = FolderUtilities::CombinePath(tilesPath, "Backgrounds");
    std::string sprPath = FolderUtilities::CombinePath(tilesPath, "Sprites");

    FolderUtilities::CreateFolder(bgPath);
    FolderUtilities::CreateFolder(sprPath);
  }


}
//--------------------------------------------------------------------------------------------------------

uint32_t SnesHdPackBuilder::CalculateMode7Hash(uint8_t *vram, uint16_t *cgram) {
  // Le Mode 7 utilise toujours la première moitié de la VRAM (0x0000 - 0x7FFF en mots, donc 64 Ko en octets)
  uint32_t vramHash = CRC32::GetCRC(vram, 0x10000);

  // Le Mode 7 utilise toute la CGRAM (256 couleurs = 512 octets)
  uint32_t cgramHash = CRC32::GetCRC((uint8_t *)cgram, 512);

  // Combiner les deux pour avoir un identifiant unique
  return vramHash ^ (cgramHash << 1);
}
//--------------------------------------------------------------------------------------------------------------
void SnesHdPackBuilder::ProcessMode7(uint16_t* vram, uint16_t* cgram)
{
	if(!_isActive || vram == nullptr || cgram == nullptr) {
		return;
	}

	// =====================================================================
	// 1. DUMP DE LA MAP GEANTE (m7_*.png)
	// =====================================================================
	if(!_options.DisableM7TextureDump) {
		// Hash basé sur la disposition de la carte (512 premiers octets)
		uint32_t mapHash = CRC32::GetCRC((uint8_t*)vram, 512);

		if(_m7TexturesDone.find(mapHash) == _m7TexturesDone.end()) {
			_m7TexturesDone.insert(mapHash);
			DumpMode7Texture(vram, cgram, mapHash, _options.Scale);
		}
	}

	// =====================================================================
	// 2. DUMP DU CATALOGUE DE TUILES (m7tiles_*.png)
	// =====================================================================
	// On isole UNIQUEMENT les pixels des 256 tuiles (les octets de poids fort)
	// Il y a 256 tuiles * 64 pixels = 16 384 pixels.
	uint8_t tilePixels[16384];
	for(int i = 0; i < 16384; i++) {
		// On décale de 8 bits pour ne récupérer que le dessin, ignorant la carte !
		tilePixels[i] = (uint8_t)(vram[i] >> 8);
	}

	uint32_t pixelHash = CRC32::GetCRC(tilePixels, 16384);

	// On inclut la CGRAM car F-Zero fait clignoter les couleurs des bordures
	uint32_t cgramHash = CRC32::GetCRC((uint8_t*)cgram, 512);

	uint32_t tilesetHash = pixelHash ^ (cgramHash << 1);

	if(_m7TilesetsDone.find(tilesetHash) == _m7TilesetsDone.end()) {
		_m7TilesetsDone.insert(tilesetHash);
		// On passe le tilesetHash pour que le fichier ait un nom unique à son graphisme
		DumpMode7Tileset(vram, cgram, tilesetHash, _options.Scale);





	}
}
//--------------------------------------------------------------------------------------

void SnesHdPackBuilder::DumpMode7Tileset(uint16_t* vram, uint16_t* cgram, uint32_t globalHash, uint32_t scale)
{
	std::string mode7Folder = FolderUtilities::CombinePath(_saveFolder, "Tiles/Mode7");
	FolderUtilities::CreateFolder(mode7Folder);

	int tileSize = 8 * scale;
	int sheetWidth = 16 * tileSize;
	int sheetHeight = 16 * tileSize;

	// Toile transparente 128x128 (multipliée par le scale)
	std::vector<uint32_t> sheetPixels(sheetWidth * sheetHeight, 0x00000000);
	bool isCanvasEmpty = true;
	std::vector<std::string> tempTags;

	// Un seul fichier PNG pour toute la scène !
	std::string hexMapHash = HexUtilities::ToHex(globalHash);
	std::string filename = "m7tiles_" + hexMapHash + ".png";
	std::string relativePath = "Tiles/Mode7/" + filename;

	for(int tileIndex = 0; tileIndex < 256; tileIndex++) {

		uint8_t rawPixels[64];
		bool isTotallyEmpty = true;

		for(int py = 0; py < 8; py++) {
			for(int px = 0; px < 8; px++) {
				uint16_t pixelWordIndex = tileIndex * 64 + py * 8 + px;
				uint8_t colorIndex = (uint8_t)(vram[pixelWordIndex] >> 8);
				rawPixels[py * 8 + px] = colorIndex;

				if(colorIndex != 0) isTotallyEmpty = false;
			}
		}

		if(isTotallyEmpty) continue;

		uint32_t tileHash = CRC32::GetCRC(rawPixels, 64);

		// Anti-Spam global : on ne dumpe la tuile qu'une seule fois dans tout le jeu
		if(_m7SingleTilesDone.find(tileHash) != _m7SingleTilesDone.end()) {
			continue;
		}
		_m7SingleTilesDone.insert(tileHash);

		// Calcul des coordonnées X et Y dans la grille de notre Atlas
		int tx = tileIndex % 16;
		int ty = tileIndex / 16;
		int pixelX = tx * tileSize;
		int pixelY = ty * tileSize;

		// 1. DESSIN DANS UN BUFFER TEMPORAIRE (Pour le Smart Rebuild)
		std::vector<uint32_t> localTile(tileSize * tileSize, 0x00000000);

		for(int py = 0; py < 8; py++) {
			for(int px = 0; px < 8; px++) {
				uint8_t colorIndex = rawPixels[py * 8 + px];

				if(colorIndex != 0) {
					uint16_t c = cgram[colorIndex];
					uint8_t r = (c & 0x1F); r = (r << 3) | (r >> 2);
					uint8_t g = ((c >> 5) & 0x1F); g = (g << 3) | (g >> 2);
					uint8_t b = ((c >> 10) & 0x1F); b = (b << 3) | (b >> 2);

					uint32_t finalColor = ((uint32_t)0xFF << 24) | (r << 16) | (g << 8) | b;

					for(uint32_t sy = 0; sy < scale; sy++) {
						for(uint32_t sx = 0; sx < scale; sx++) {
							localTile[(py * scale + sy) * tileSize + (px * scale + sx)] = finalColor;
						}
					}
				}
			}
		}

		// =========================================================================
		// --- 2. SMART REBUILD : VÉRIFICATION DU HASH DE PIXELS ---
		// =========================================================================
		bool isMatched = false;
		if(_options.BuildFromPngFolder) {
			uint64_t pixelHash = 14695981039346656037ULL;
			for(uint32_t color : localTile) {
				if((color >> 24) == 0) {
					color = 0x00000000; // Normalisation de la transparence
				}
				pixelHash ^= color;
				pixelHash *= 1099511628211ULL;
			}

			auto it = _smartRebuildMap.find(pixelHash);
			if(it != _smartRebuildMap.end()) {
				isMatched = true;

				_usedSmartBlocks[it->second.ImageIndex].insert({ it->second.X, it->second.Y });

				// CORRECTION POUR LE MODE 7 : On utilise directement le chemin Rebuild !
				std::string matchedPath = _smartRebuildImages[it->second.ImageIndex].RebuildRelativePath;
				int matchedX = it->second.X;
				int matchedY = it->second.Y;

				// On écrit le tag M7 directement pointant vers le dossier d'input
				std::string tag = "<m7tiles>" + matchedPath + "," + HexUtilities::ToHex(tileHash) + "," +
					std::to_string(scale) + "," + std::to_string(matchedX) + "," + std::to_string(matchedY);
				tempTags.push_back(tag);

				if(_options.GenerateHdPackMetadata) {
					HdMetadataItem metaItem;
					metaItem.Type = "Mode7Tile";
					metaItem.Filename = matchedPath;
					metaItem.Hash = HexUtilities::ToHex(tileHash);
					metaItem.GrabX = matchedX;
					metaItem.GrabY = matchedY;
					metaItem.GrabFrame = 0;
					metaItem.SnesHiresLine = tag;
					_metadataCollection.push_back(metaItem);
				}
			}
		}
		// =========================================================================

		// 3. SI AUCUN MATCH : COMPORTEMENT CLASSIQUE
		if(!isMatched) {
			// On copie le buffer temporaire dans l'atlas final
			for(int py = 0; py < tileSize; py++) {
				for(int px = 0; px < tileSize; px++) {
					uint32_t color = localTile[py * tileSize + px];
					if((color >> 24) != 0) {
						sheetPixels[(pixelY + py) * sheetWidth + (pixelX + px)] = color;
					}
				}
			}
			isCanvasEmpty = false;

			std::string tag = "<m7tiles>" + relativePath + "," + HexUtilities::ToHex(tileHash) + "," +
				std::to_string(scale) + "," + std::to_string(pixelX) + "," + std::to_string(pixelY);
			tempTags.push_back(tag);

			if(_options.GenerateHdPackMetadata) {
				HdMetadataItem metaItem;
				metaItem.Type = "Mode7Tile";
				metaItem.Filename = relativePath;
				metaItem.Hash = HexUtilities::ToHex(tileHash);
				metaItem.GrabX = pixelX;
				metaItem.GrabY = pixelY;
				metaItem.GrabFrame = 0;
				metaItem.SnesHiresLine = tag;
				_metadataCollection.push_back(metaItem);
			}
		}
	}

	// Sauvegarde de l'Atlas Mode 7 (si on a dessiné au moins une tuile classique dedans)
	if(!isCanvasEmpty) {
		std::string fullPath = FolderUtilities::CombinePath(_saveFolder, relativePath);
		PNGHelper::WritePNG(fullPath, sheetPixels.data(), sheetWidth, sheetHeight, 32);
	}

	// Quoi qu'il arrive, on sauvegarde la liste des tags générés pour cette map !
	if(!tempTags.empty()) {
		_m7Rows << "\n# Mode 7 Atlas for Map: " << hexMapHash << "\n";
		for(const auto& tag : tempTags) {
			_m7Rows << tag << "\n";
		}
	}
}

//-------------------------------------------------------------------------------------
void SnesHdPackBuilder::DumpMode7Texture(uint16_t *vram, uint16_t *cgram,
                                         uint32_t hash, uint32_t scale) {
  int baseWidth = 1024;
  int baseHeight = 1024;
  int scaledWidth = baseWidth * scale;
  int scaledHeight = baseHeight * scale;

  std::vector<uint32_t> pixels(scaledWidth * scaledHeight, 0);

  for (int ty = 0; ty < 128; ty++) {
    for (int tx = 0; tx < 128; tx++) {

      // On lit directement le mot de 16-bits
      uint16_t tilemapWord = vram[ty * 128 + tx];
      // L'index de la tuile est dans l'octet de poids faible
      uint8_t tileIndex = tilemapWord & 0xFF;

      for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {

          uint16_t pixelWord = vram[tileIndex * 64 + py * 8 + px];
          // La donnée du pixel est dans l'octet de poids fort
          uint8_t colorIndex = pixelWord >> 8;

          uint16_t c = cgram[colorIndex];

          uint8_t r = (c & 0x1F);
          r = (r << 3) | (r >> 2);
          uint8_t g = ((c >> 5) & 0x1F);
          g = (g << 3) | (g >> 2);
          uint8_t b = ((c >> 10) & 0x1F);
          b = (b << 3) | (b >> 2);

          uint8_t a = (colorIndex == 0) ? 0 : 255;
          uint32_t finalColor = (a << 24) | (r << 16) | (g << 8) | b;

          int baseX = (tx * 8 + px) * scale;
          int baseY = (ty * 8 + py) * scale;

          for (uint32_t sy = 0; sy < scale; sy++) {
            for (uint32_t sx = 0; sx < scale; sx++) {
              pixels[(baseY + sy) * scaledWidth + (baseX + sx)] = finalColor;
            }
          }
        }
      }
    }
  }

  // 1. On convertit le hash en Hexa une seule fois
  std::string hexHash = HexUtilities::ToHex(hash);

  // 2. On utilise hexHash pour le nom du fichier
  std::string filename = "m7_" + hexHash + ".png";
  std::string fullPath = FolderUtilities::CombinePath(_saveFolder, filename);
  PNGHelper::WritePNG(fullPath, pixels.data(), scaledWidth, scaledHeight, 32);

  // 3. Construction de la balise <m7img> dans une variable
  std::string m7imgTag = "<m7img>" + filename + "," + hexHash + "," + std::to_string(scale);

  // On l'écrit dans le flux du fichier texte
  _m7Rows << m7imgTag << "\n";

  // =========================================================================
  // --- COLLECTE DES MÉTADONNÉES MODE 7 TEXTURE GÉANTE ---
  // =========================================================================
  if(_options.GenerateHdPackMetadata) {
	  HdMetadataItem metaItem;
	  metaItem.Type = "Mode7Map";
	  metaItem.Filename = filename;
	  metaItem.Hash = hexHash;
	  metaItem.GrabX = 0;
	  metaItem.GrabY = 0;
	  metaItem.GrabFrame = 0;

	  // Maintenant la variable existe et contient la bonne ligne !
	  metaItem.SnesHiresLine = m7imgTag;

	  _metadataCollection.push_back(metaItem);
  }
}
//------------------------------------------------------------------------------------



void SnesHdPackBuilder::StopRecording()
{
	_isActive = false;

	// FORCE BRUTE POUR TESTER LE JSON !
	_options.GenerateHdPackMetadata = true;

	if(_saveFolder.empty())
		return;

	std::string logPath = FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
	std::ofstream log(logPath, std::ios::app);
	std::string filePath = FolderUtilities::CombinePath(_saveFolder, "snes_hires.txt");

	// =========================================================
	// 1. LECTURE DE L'EXISTANT (Mode Append)
	// =========================================================
	std::string existingContent = "";
	std::unordered_set<uint64_t> existingHashes;
	int startingImageIndex = 0;

	if(_options.UpdateIfExists) {
		std::ifstream inFile(filePath);
		if(inFile.is_open()) {
			std::string line;
			while(std::getline(inFile, line)) {
				existingContent += line + "\n"; // Sauvegarde pour la réécriture

				if(line.substr(0, 5) == "<img>") {
					startingImageIndex++;
				} else if(line.substr(0, 6) == "<tile>") {
					std::vector<std::string> tokens = StringUtilities::Split(line.substr(6), ',');
					if(!tokens.empty()) {
						uint64_t hash = std::stoull(tokens[0], nullptr, 16);
						existingHashes.insert(hash);
					}
				}
			}
			inFile.close();
			if(log.is_open()) log << "[HD Builder] Fichier existant: " << startingImageIndex << " img, " << existingHashes.size() << " tiles.\n";
		}
	}

	// =========================================================
	// 2. PRÉPARATION DES NOUVELLES TUILES
	// =========================================================
	struct TileDef
	{
		uint64_t MemoryHash;
		uint32_t PaletteIndex;
		int ImageIndex;
		int X;
		int Y;
		uint16_t VramTileIndex;

		bool IsHFlip;
		bool IsVFlip;
	};

	std::vector<TileDef> tileDefinitions;
	std::vector<std::string> imageList;

	// /!\ DÉCALAGE DE L'INDEX
	int currentImageIndex = startingImageIndex;
	int scale = _options.Scale;
	if(scale < 1) scale = 4;

	int tileSize = 8 * scale;
	int tilesPerRow = 16;
	int maxTilesPerSheet = 256;

	for(auto& entry : _paletteTiles) {
		uint32_t paletteIndex = entry.first;
		auto& tiles = entry.second;

		// Filtre anti-tuiles unies
		if(_options.IgnorePlainTiles) {
			tiles.erase(std::remove_if(tiles.begin(), tiles.end(), [](const auto& tile) {
				if(tile.Pixels.empty()) return false;
				uint32_t firstColor = tile.Pixels[0];
				for(size_t p = 1; p < tile.Pixels.size(); p++) {
					if(tile.Pixels[p] != firstColor) return false;
				}
				return true;
			}), tiles.end());
		}

		// Filtre anti-doublons (Append Mode)
		if(_options.UpdateIfExists && !tiles.empty()) {
			tiles.erase(std::remove_if(tiles.begin(), tiles.end(), [&](const auto& tile) {
				return existingHashes.find(tile.MemoryHash) != existingHashes.end();
			}), tiles.end());
		}

		if(tiles.empty()) continue;

		bool isSprite = tiles[0].IsSprite;
		std::string subFolder = isSprite ? "Sprites" : "Backgrounds";

		// ==========================================================
		// --- LE PACKING DYNAMIQUE ---
		// ==========================================================
		if(_options.GenerateSpritesPerFrame && isSprite) {
			std::map<uint32_t, std::vector<const SnesHdTileImage*>> tilesByFrame;
			for(const auto& tile : tiles) tilesByFrame[tile.FrameSeen].push_back(&tile);

			int canvasWidth = 256 * scale;
			int canvasHeight = 256 * scale;

			std::string relativeFolder = "Tiles/" + subFolder + "/Palette_" + std::to_string(paletteIndex);
			std::string dirPath = FolderUtilities::CombinePath(_saveFolder, relativeFolder);
			FolderUtilities::CreateFolder(dirPath);

			std::vector<uint32_t> currentCanvas(canvasWidth * canvasHeight, 0);
			std::vector<TileDef> currentTileDefs;
			std::set<std::pair<int, int>> occupiedCoords;
			bool isCanvasEmpty = true;
			int packIndex = 0;

			auto flushCanvas = [&]() {
				if(isCanvasEmpty) return;

				std::string fileName = relativeFolder + "/packed_" + std::to_string(packIndex) + ".png";
				std::string fullPath = FolderUtilities::CombinePath(_saveFolder, fileName);

				if(PNGHelper::WritePNG(fullPath, currentCanvas.data(), canvasWidth, canvasHeight, 32)) {
					if(log.is_open()) log << "Successfully wrote packed canvas: " << fileName << "\n";
				}

				imageList.push_back(fileName);
				tileDefinitions.insert(tileDefinitions.end(), currentTileDefs.begin(), currentTileDefs.end());

				std::fill(currentCanvas.begin(), currentCanvas.end(), 0);
				currentTileDefs.clear();
				occupiedCoords.clear();
				isCanvasEmpty = true;

				packIndex++;
				currentImageIndex++;
			};

			for(auto& frameEntry : tilesByFrame) {
				for(const auto* tilePtr : frameEntry.second) {
					const auto& tile = *tilePtr;
					int sx = tile.ScreenX;
					int sy = tile.ScreenY;

					std::pair<int, int> currentCoord = { sx, sy };

					if(occupiedCoords.find(currentCoord) != occupiedCoords.end()) flushCanvas();

					bool drewSomething = false;
					for(int py = 0; py < tileSize; py++) {
						for(int px = 0; px < tileSize; px++) {
							int destX = (sx * scale) + px;
							int destY = (sy * scale) + py;

							if(destX >= 0 && destX < canvasWidth && destY >= 0 && destY < canvasHeight) {
								uint32_t pixel = tile.Pixels[py * tileSize + px];
								if((pixel & 0xFF000000) != 0) {
									currentCanvas[destY * canvasWidth + destX] = pixel;
									drewSomething = true;
								}
							}
						}
					}

					if(drewSomething) {
						isCanvasEmpty = false;
						occupiedCoords.insert(currentCoord);
						currentTileDefs.push_back({ tile.MemoryHash, paletteIndex, currentImageIndex, sx * scale, sy * scale, tile.VramTileIndex });

						if(_options.GenerateHdPackMetadata) {
							HdMetadataItem metaItem;
							metaItem.Type = "Sprite";
							metaItem.Filename = relativeFolder + "/packed_" + std::to_string(packIndex) + ".png";
							metaItem.Hash = HexUtilities::ToHex(tile.MemoryHash);
							metaItem.GrabX = sx;
							metaItem.GrabY = sy;
							metaItem.GrabFrame = tile.FrameSeen;

							// GESTION DYNAMIQUE DU TAG
							std::string tileTag = "<tile>";
							if(tile.IsHFlip && tile.IsVFlip) tileTag = "<tile_hvflip>";
							else if(tile.IsHFlip) tileTag = "<tile_hflip>";
							else if(tile.IsVFlip) tileTag = "<tile_vflip>";

							std::stringstream ssLine;
							ssLine << tileTag << std::hex << std::uppercase << tile.MemoryHash << ","
								<< std::dec << paletteIndex << "," << currentImageIndex << "," << (sx * scale) << "," << (sy * scale);
							metaItem.SnesHiresLine = ssLine.str();
							_metadataCollection.push_back(metaItem);
						}
					}
				}
			}
			flushCanvas();
		}
		// ==========================================================
		// --- LA GRILLE CLASSIQUE ---
		// ==========================================================
		else {
			std::sort(tiles.begin(), tiles.end(), [](const SnesHdTileImage& a, const SnesHdTileImage& b) {
				if(a.FrameSeen != b.FrameSeen) return a.FrameSeen < b.FrameSeen;
				if(a.ScreenY != b.ScreenY) return a.ScreenY < b.ScreenY;
				return a.ScreenX < b.ScreenX;
			});

			for(int i = 0; i < (int)tiles.size(); i += maxTilesPerSheet) {
				int count = std::min(maxTilesPerSheet, (int)tiles.size() - i);
				int rows = (count + tilesPerRow - 1) / tilesPerRow;
				int sheetWidth = tilesPerRow * tileSize;
				int sheetHeight = rows * tileSize;

				std::vector<uint32_t> sheetPixels(sheetWidth * sheetHeight, 0);

				std::string dirPath = FolderUtilities::CombinePath(_saveFolder, "Tiles/" + subFolder);
				FolderUtilities::CreateFolder(dirPath);

				std::string pageSuffix = "_page_" + std::to_string(i / maxTilesPerSheet);
				std::string fileName = "Tiles/" + subFolder + "/pal_" + std::to_string(paletteIndex) + pageSuffix + ".png";

				for(int j = 0; j < count; j++) {
					const auto& tile = tiles[i + j];
					int tx = j % tilesPerRow;
					int ty = j / tilesPerRow;

					for(int py = 0; py < tileSize; py++) {
						for(int px = 0; px < tileSize; px++) {
							sheetPixels[(ty * tileSize + py) * sheetWidth + (tx * tileSize + px)] = tile.Pixels[py * tileSize + px];
						}
					}

					tileDefinitions.push_back({ tile.MemoryHash, paletteIndex, currentImageIndex, tx * tileSize, ty * tileSize, tile.VramTileIndex });

					if(_options.GenerateHdPackMetadata) {

						// GESTION DYNAMIQUE DU TAG TILE
						std::string tileTag = "<tile>";
						if(tile.IsHFlip && tile.IsVFlip) tileTag = "<tile_hvflip>";
						else if(tile.IsHFlip) tileTag = "<tile_hflip>";
						else if(tile.IsVFlip) tileTag = "<tile_vflip>";

						HdMetadataItem metaItem;
						metaItem.Type = isSprite ? "Sprite" : "Background";
						metaItem.Filename = fileName;
						metaItem.Hash = HexUtilities::ToHex(tile.MemoryHash);
						metaItem.GrabX = tile.ScreenX;
						metaItem.GrabY = tile.ScreenY;
						metaItem.GrabFrame = tile.FrameSeen;

						std::stringstream ssLine;
						ssLine << tileTag << std::hex << std::uppercase << tile.MemoryHash << ","
							<< std::dec << paletteIndex << "," << currentImageIndex << "," << (tx * tileSize) << "," << (ty * tileSize);
						metaItem.SnesHiresLine = ssLine.str();
						_metadataCollection.push_back(metaItem);
					}
				}

				std::string fullPath = FolderUtilities::CombinePath(_saveFolder, fileName);
				if(PNGHelper::WritePNG(fullPath, sheetPixels.data(), sheetWidth, sheetHeight, 32)) {
					if(log.is_open()) log << "Successfully wrote: " << fileName << "\n";
				}

				imageList.push_back(fileName);
				currentImageIndex++;
			}
		}
	}
	// ==========================================================
	// --- SMART REBUILD : GÉNÉRATION DES PNG "_REBUILD" ---
	// ==========================================================
	if(_options.BuildFromPngFolder) {
		// On rouvre le mouchard
		std::string smartLogPath = FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
		std::ofstream smartLog(smartLogPath, std::ios::app);
		if(smartLog.is_open()) smartLog << "\n--- STARTING PNG REBUILD GENERATION ---\n";

		uint32_t tileSize = 8 * (_options.Scale > 0 ? _options.Scale : 1);

		// On ne parcourt que les images qui ont eu au moins 1 tuile utilisée !
		for(auto const& [imgIndex, blocks] : _usedSmartBlocks) {

			SmartRebuildImage& imgInfo = _smartRebuildImages[imgIndex];
			if(smartLog.is_open()) smartLog << "Processing: " << imgInfo.OriginalRelativePath << "\n";
			if(smartLog.is_open()) smartLog << " -> Blocks mapped: " << blocks.size() << "\n";

			std::vector<uint32_t> origPixels;
			uint32_t w = 0, h = 0;

			// 1. On recharge l'originale en mémoire
			std::ifstream origFile(imgInfo.OriginalAbsPath, std::ios::binary | std::ios::ate);
			if(origFile.is_open()) {
				std::streamsize size = origFile.tellg();
				origFile.seekg(0, std::ios::beg);
				std::vector<uint8_t> fileBytes(size);

				if(origFile.read((char*)fileBytes.data(), size)) {
					if(PNGHelper::ReadPNG(fileBytes, origPixels, w, h)) {
						if(smartLog.is_open()) smartLog << " -> Original file reloaded ok. Dimensions: " << w << "x" << h << "\n";

						// 2. On crée une toile vierge (100% transparente) de la MÊME taille
						std::vector<uint32_t> rebuildPixels(w * h, 0x00000000);

						// 3. On copie UNIQUEMENT les blocs de pixels utilisés !
						for(auto const& block : blocks) {
							int startX = block.first;
							int startY = block.second;

							for(uint32_t py = 0; py < tileSize; py++) {
								for(uint32_t px = 0; px < tileSize; px++) {
									if(startY + py < h && startX + px < w) {
										rebuildPixels[(startY + py) * w + (startX + px)] = origPixels[(startY + py) * w + (startX + px)];
									}
								}
							}
						}

						// 4. On sauvegarde le PNG "Épuré"
						std::string outPath = FolderUtilities::CombinePath(_saveFolder, imgInfo.RebuildRelativePath);
						if(smartLog.is_open()) smartLog << " -> Attempting to save to: " << outPath << "\n";

						if(PNGHelper::WritePNG(outPath, rebuildPixels.data(), w, h, 32)) {
							if(smartLog.is_open()) smartLog << " -> SUCCESS! File written.\n";
						} else {
							if(smartLog.is_open()) smartLog << " -> ERROR: PNGHelper::WritePNG failed to write the file!\n";
						}
					} else {
						if(smartLog.is_open()) smartLog << " -> ERROR: PNGHelper::ReadPNG failed to decode original bytes.\n";
					}
				} else {
					if(smartLog.is_open()) smartLog << " -> ERROR: origFile.read failed.\n";
				}
			} else {
				if(smartLog.is_open()) smartLog << " -> ERROR: Could not open original file: " << imgInfo.OriginalAbsPath << "\n";
			}
		}
		if(smartLog.is_open()) smartLog.close();
	}

	// =========================================================
	// 3. ÉCRITURE DU FICHIER TEXTE snes_hires.txt (MERGE)
	// =========================================================
	std::ofstream file(filePath);
	if(!file.is_open()) return;

	std::string currentTimestamp = GetCurrentDateTimeString();

	auto writeBlockHeader = [&](const std::string& blockType) {
		if(_options.UpdateIfExists) { // If we are in Append mode
			file << "\n##############################\n";
			file << "###    HD PACK BUILDER: ADDITION in Append Mode on " << currentTimestamp << " [" << blockType << "]\n";
			file << "##############################\n";
		} else {
			file << "\n# === " << blockType << " ===\n";
		}
	};

	// A. Réinjection de l'existant
	if(!_options.UpdateIfExists || existingContent.empty()) {
		if(!_romInfoTxt.empty()) file << _romInfoTxt;
		file << "<scale>" << _options.Scale << "\n\n";
	} else {
		file << existingContent;
		// We remove the old "# === NOUVELLES TUILES (UPDATE) ===" line here
		// because the new header system handles it better.
	}

	// B. Nouvelles images
	if(!imageList.empty() || !_smartRebuildImages.empty()) {
		writeBlockHeader("IMAGES");

		// 1. Les spritesheets trouvées par le Smart Rebuild
		/*	for(const auto& img : _smartRebuildImages) {
			file << "<img>" << img << "\n";
		}
		*/
		// --- SMART REBUILD IMAGES ---
		for(size_t i = 0; i < _smartRebuildImages.size(); i++) {
			// Si on s'en est servi, on pointe le fichier texte vers l'image optimisée (_REBUILD)
			if(_usedSmartBlocks.find(i) != _usedSmartBlocks.end()) {
				file << "<img>" << _smartRebuildImages[i].RebuildRelativePath << "\n";
			}
			// Si l'image n'a pas du tout été utilisée, on laisse le chemin d'origine.
			else {
				file << "<img>" << _smartRebuildImages[i].OriginalRelativePath << "\n";
			}
		}
		// 2. Les nouvelles images créées par le Builder
		for(const auto& img : imageList) {
			file << "<img>" << img << "\n";
		}
		file << "\n";
	}

	int lastImageIndex = -1;

	// C. Nouvelles tuiles
	if(!tileDefinitions.empty() || !_preMappedTiles.empty()) {
		writeBlockHeader("TILES");

		// --- SMART REBUILD TILES ---
		if(!_preMappedTiles.empty()) {
			file << "\n# --- SMART REBUILD MAPPED TILES ---\n";
			for(const auto& td : _preMappedTiles) {
				file << "<tile>" << std::hex << std::uppercase << td.MemoryHash << ","
					<< std::dec << td.PaletteIndex << "," << td.ImageIndex << ","
					<< td.X << "," << td.Y << "\n";
			}
			file << "\n";
		}
		for(const auto& td : tileDefinitions) {
			if(td.ImageIndex != lastImageIndex) {
				lastImageIndex = td.ImageIndex;
				file << "\n# =========================================================================\n";

				int localImgIndex = td.ImageIndex - startingImageIndex;
				if(localImgIndex >= 0 && localImgIndex < imageList.size()) {
					file << "# Tiles from PNG file <img> n°" << td.ImageIndex << " : " << imageList[localImgIndex] << "\n";
				}
				file << "# =========================================================================\n\n";
			}

			std::string commentLine = "# Tile Index: $" + HexUtilities::ToHex(td.VramTileIndex / 32) + ", Palette: " + std::to_string(td.PaletteIndex);
			file << commentLine << "\n";

			//  Sélection du bon Tag d'écriture selon la symétrie !
			std::string tileTag = "<tile>";
			if(td.IsHFlip && td.IsVFlip) tileTag = "<tile_hvflip>";
			else if(td.IsHFlip) tileTag = "<tile_hflip>";
			else if(td.IsVFlip) tileTag = "<tile_vflip>";

			file << tileTag << std::hex << std::uppercase << td.MemoryHash << ","
				<< std::dec << td.PaletteIndex << "," << td.ImageIndex << "," << td.X << "," << td.Y << "\n\n";
		}
	}

	// D. Mode 7
	std::string m7Data = _m7Rows.str();
	if(!m7Data.empty()) {
		writeBlockHeader("MODE 7 TILES");
		file << m7Data;
	}

	file.close();

	// =========================================================
	// 4. ÉCRITURE DES MÉTADONNÉES JSON
	// =========================================================
	if(_options.GenerateHdPackMetadata && !_metadataCollection.empty()) {
		std::string jsonPath = FolderUtilities::CombinePath(_saveFolder, "hdpack_metadata.json");
		std::ofstream jsonFile(jsonPath);

		if(jsonFile.is_open()) {
			std::stringstream ss;
			ss << "{\n";
			if(!_romInfoJson.empty()) ss << _romInfoJson;
			ss << "{\n  \"generatedTiles\": [\n";

			for(size_t i = 0; i < _metadataCollection.size(); i++) {
				const auto& item = _metadataCollection[i];
				ss << "    {\n"
					<< "      \"type\": \"" << item.Type << "\",\n"
					<< "      \"filename\": \"" << item.Filename << "\",\n"
					<< "      \"hash\": \"" << item.Hash << "\",\n"
					<< "      \"grabX\": " << item.GrabX << ",\n"
					<< "      \"grabY\": " << item.GrabY << ",\n"
					<< "      \"grabFrame\": " << item.GrabFrame << ",\n"
					<< "      \"snesHiresLine\": \"" << item.SnesHiresLine << "\"\n"
					<< "    }";
				if(i < _metadataCollection.size() - 1) ss << ",";
				ss << "\n";
			}
			ss << "  ]\n}";
			jsonFile << ss.str();
			jsonFile.close();
		}
	}

	if(log.is_open()) log.close();

	// =========================================================
	// 5. NETTOYAGE DES BUFFERS
	// =========================================================
	_metadataCollection.clear();
	_processedTiles.clear();
	_paletteTiles.clear();
	_m7Rows.str("");
	_m7Rows.clear();
	_m7TexturesDone.clear();
	_m7TilesetsDone.clear();
}


//--------------------------------------------------------------------------------------------------------


void SnesHdPackBuilder::ProcessTile(const uint8_t* tileData, size_t dataSize,
                                    uint32_t paletteIndex, SnesBpp bpp,
                                    bool isLargeTile, const uint16_t *cgram,
                                    bool isSprite, uint16_t vramTileIndex,
												int screenX, int screenY, uint32_t frameCount, bool hMirror, bool vMirror) {
  if (!_isActive || dataSize == 0 || tileData == nullptr || cgram == nullptr) {
    return;
  }

  // =========================================================================
	// --- LE BOUCLIER ANTI-MIROIR ---
	// Si c'est un sprite, qu'il est en miroir horizontal, et que l'option est active : on rejette !
	// =========================================================================
  if(_options.PreventHMirroredSpritesGrabbing && isSprite && hMirror) {
	  return;
  }


  // Hash MemoryHash basique
  uint64_t hash = 0;
  for (size_t i = 0; i < dataSize; i++) {
    hash = (hash * 31) + tileData[i];
  }

  SnesHdTileKey key;
  key.MemoryHash = hash;
  key.PaletteHash = paletteIndex;
  key.Bpp = bpp;
  key.IsLargeTile = isLargeTile;
  key.IsSprite = isSprite;

  // On enregistre les symétries pour différencier la clé !
  key.IsHFlip = hMirror;
  key.IsVFlip = vMirror;

  if (_processedTiles.find(key) == _processedTiles.end()) {
    _processedTiles.insert(key);

    // Log occasionnel pour ne pas saturer
    /*if(_processedTiles.size() % 100 == 0) {
      std::string logPath =
          FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
      std::ofstream log(logPath, std::ios::app);
      if (log.is_open()) {
        log << "Processed 100 more tiles. Total: " << _processedTiles.size()
            << "\n";
        log.close();
      }
    }
	 */

    SaveTilePng(tileData, dataSize, hash, paletteIndex, bpp, isLargeTile, cgram,
                isSprite, vramTileIndex, screenX, screenY, frameCount, hMirror, vMirror);

	 

	 // Hash MemoryHash basique
	 uint64_t hash = 0;
	 for(size_t i = 0; i < dataSize; i++) {
		 hash = (hash * 31) + tileData[i];
	 }

	 // ==========================================================
	 // MOUCHARD 1 : LE BUILDER (Maintenant avec le Hash !)
	 // ==========================================================
	 /*std::string mouchardPath = FolderUtilities::CombinePath(_saveFolder, "mouchard_hash.txt");
	 std::ofstream logB(mouchardPath, std::ios::app);
	 if(logB.is_open()) {
		 logB << "=== [BUILDER] ===\n";
		 logB << "Hash Généré : " << std::hex << hash << std::dec << "\n";
		 logB << "VramTileIndex: " << vramTileIndex << " | BPP: " << (int)bpp << " | DataSize: " << dataSize << "\n";
		 logB << "Les 16 premiers octets lus : ";
		 for(size_t i = 0; i < 16 && i < dataSize; i++) {
			 logB << std::hex << (int)tileData[i] << " ";
		 }
		 logB << std::dec << "\n\n";
		 logB.close();
	 }
	 */
	 // ==========================================================


  } 
}


//---------------------------------------------------------------- 
void SnesHdPackBuilder::SaveTilePng(const uint8_t *tileData, size_t dataSize,
                                    uint64_t memoryHash, uint32_t paletteIndex,
                                    SnesBpp bpp, bool isLargeTile,
                                    const uint16_t *cgram, bool isSprite,uint16_t vramTileIndex,
												int screenX, int screenY, uint32_t frameCount, bool hMirror, bool vMirror) {

	 
	// =========================================================
	// FORCE BRUTE POUR TESTER LE JSON !
	// =========================================================
	_options.GenerateHdPackMetadata = true;
	// =========================================================

  int scale = _options.Scale;
  if (scale < 1)
    scale = 1;
  if (scale > 10)
    scale = 10;

  int tileSize = 8 * scale;
  std::vector<uint32_t> rgbaBuffer(tileSize * tileSize, 0);

  int actualBpp = 2;
  if (bpp == SnesBpp::Bpp4)
    actualBpp = 4;
  else if (bpp == SnesBpp::Bpp8)
    actualBpp = 8;

  int palOffset = 0;
  if (bpp == SnesBpp::Bpp2)
    palOffset = paletteIndex * 4;
  else if (bpp == SnesBpp::Bpp4)
    palOffset = paletteIndex * 16;
  else if (bpp == SnesBpp::Bpp8)
    palOffset = 0;

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      uint8_t colorIndex = 0;
      for (int bp = 0; bp < actualBpp; bp++) {
        int pairIdx = bp / 2;
        int bitIdx = bp % 2;
        int byteOffset = pairIdx * 16 + y * 2 + bitIdx;
        if (byteOffset < dataSize && (tileData[byteOffset] & (1 << (7 - x)))) {
          colorIndex |= (1 << bp);
        }
      }

      uint16_t c = cgram[palOffset + colorIndex];
      uint8_t r = (c & 0x1F);
      r = (r << 3) | (r >> 2);
      uint8_t g = ((c >> 5) & 0x1F);
      g = (g << 3) | (g >> 2);
      uint8_t b = ((c >> 10) & 0x1F);
      b = (b << 3) | (b >> 2);
      uint8_t a = (colorIndex == 0) ? 0 : 255;

      uint32_t finalColor = (a << 24) | (r << 16) | (g << 8) | b;

      for (int dy = 0; dy < scale; dy++) {
        for (int dx = 0; dx < scale; dx++) {
			  // Inversion des coordonnées X et Y si miroir actif
			  int finalHdX = hMirror ? (tileSize - 1 - (x * scale + dx)) : (x * scale + dx);
			  int finalHdY = vMirror ? (tileSize - 1 - (y * scale + dy)) : (y * scale + dy);
           rgbaBuffer[(y * scale + dy) * tileSize + (x * scale + dx)] =
              finalColor;
        }
      }
    }
  }
  // ==========================================================
	// --- SMART REBUILD : VÉRIFICATION DU HASH DE PIXELS ---
	// ==========================================================
  if(_options.BuildFromPngFolder) {
	  uint64_t pixelHash = 14695981039346656037ULL;
	  for(uint32_t color : rgbaBuffer) {

		  if((color >> 24) == 0) {
			  color = 0x00000000;
		  }

		  pixelHash ^= color;
		  pixelHash *= 1099511628211ULL;
	  }

	  auto it = _smartRebuildMap.find(pixelHash);
	  if(it != _smartRebuildMap.end()) {
		  _usedSmartBlocks[it->second.ImageIndex].insert({ it->second.X, it->second.Y });
		  
		  /*
		  // MOUCHARD DE MATCH (Limité à 20 pour la perf)
		  static int matchCount = 0;
		  if(matchCount < 20) {
			  matchCount++;
			  std::string logPath = FolderUtilities::CombinePath(_saveFolder, "debug_log.txt");
			  std::ofstream log(logPath, std::ios::app);
			  if(log.is_open()) {
				  log << "[MATCH " << matchCount << "] MemoryHash: " << std::hex << std::uppercase << memoryHash
					  << " -> Maps to ImgIndex " << std::dec << it->second.ImageIndex
					  << " at (" << it->second.X << "," << it->second.Y << ")\n";
			  }
		  }
		  */


		  PreMappedTile preMapped = {
				memoryHash,
				paletteIndex,
				it->second.ImageIndex,
				it->second.X,
				it->second.Y
		  };
		  _preMappedTiles.push_back(preMapped);
		  return;
	  }
  }
  // ==========================================================

  // Si on arrive ici, c'est que l'option est désactivée OU que la tuile n'existait pas dans le dossier.
  // On reprend le comportement classique !
  SnesHdTileImage img;
  img.MemoryHash = memoryHash;
  img.Pixels = std::move(rgbaBuffer);
  img.IsSprite = isSprite;
  img.VramTileIndex = vramTileIndex;

  // --- L'ENREGISTREMENT des infos de contexte à l'ecran et temporel ---
  img.FrameSeen = frameCount;
  img.ScreenX = screenX;
  img.ScreenY = screenY;

  //Sauvegarde des flags pour la génération du TXT !
  img.IsHFlip = hMirror;
  img.IsVFlip = vMirror;

  _paletteTiles[paletteIndex].push_back(std::move(img));


}