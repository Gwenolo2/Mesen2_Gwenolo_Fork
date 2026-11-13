#pragma once
#include "pch.h"
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// Position d'une tile dans une image d'atlas (en pixels HD)
struct GbHdTileRef
{
	uint32_t ImageIndex = 0; // index dans la liste des <img>
	uint32_t X = 0;
	uint32_t Y = 0;
};

// Contenu d'un pack Game Boy (gb_hires.txt)
struct GbHdPackData
{
	uint32_t Scale = 4;
	std::string FolderPath;
	std::vector<std::string> Images; // chemins relatifs au dossier du pack, dans l'ordre des <img>
	// Clé : (hash de la tile, palette encodée D8, flags : 2 = hflip, 4 = vflip)
	std::map<std::tuple<uint64_t, uint32_t, uint8_t>, GbHdTileRef> Tiles;
	bool IsLoaded = false;
};

class GbHdPackLoader
{
public:
	// Lit <packFolder>/gb_hires.txt. Retourne false si le fichier est absent.
	// Étape 1 : lecture seule, aucun rendu. Journal [HD-GB-LOAD].
	static bool Load(const std::string& packFolder, GbHdPackData& outData);
};
