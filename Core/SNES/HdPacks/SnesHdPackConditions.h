#pragma once
#include "pch.h"
#include "SNES/HdPacks/SnesHdData.h" 
#include "Utilities/HexUtilities.h"
#include "Utilities/StringUtilities.h"
#include <fstream>
#include <sstream>


class SnesPpu;
struct SnesPpuState;

// ---------------------------------------------------------
// 1. CONDITIONS DE TUILES (Adaptées avec le Hash SNES)
// ---------------------------------------------------------
struct SnesHdPackBaseTileCondition : public SnesHdPackCondition
{
	int32_t TileX = 0;
	int32_t TileY = 0;

	// SNES : On utilise le Hash et le BPP plutôt que des octets bruts !
	uint64_t MemoryHash = 0;
	uint32_t PaletteHash = 0;
	bool IgnorePalette = false;

	int32_t PixelOffset = 0;

	void Initialize(int32_t x, int32_t y, uint64_t memoryHash, uint32_t paletteHash, bool ignorePalette)
	{
		TileX = x;
		TileY = y;
		PixelOffset = (y * 256) + x; // Attention : La SNES peut être en 512px, il faudra peut-être gérer ça plus tard
		MemoryHash = memoryHash;
		PaletteHash = paletteHash;
		IgnorePalette = ignorePalette;
	}

	string ToString() override
	{
		stringstream out;
		out << "<condition>" << Name << "," << GetConditionName() << ",";
		out << TileX << ",";
		out << TileY << ",";
		out << HexUtilities::ToHex(MemoryHash) << ",";
		out << HexUtilities::ToHex(PaletteHash, true);
		return out.str();
	}
};

enum class HdPackConditionOperator
{
	Invalid = -1,
	Equal = 0,
	NotEqual = 1,
	GreaterThan = 2,
	LowerThan = 3,
	LowerThanOrEqual = 4,
	GreaterThanOrEqual = 5,

	// In operator added by Gwenolo
	In = 6
};

// ---------------------------------------------------------
// 2. CONDITIONS MÉMOIRE (Mutualisées pour l'Architecture SNES)
// ---------------------------------------------------------

// Les marqueurs spatiaux à injecter dans les bits de poids fort (bits 24-31)
enum class SnesMemorySpace : uint32_t
{
	CPU = 0x00000000,
	VRAM = 0x10000000,
	OAM = 0x20000000,
	CGRAM = 0x30000000,
	ARAM = 0x40000000 // SPC700 Audio RAM
};

struct SnesHdPackBaseMemoryCondition : public SnesHdPackCondition
{
	uint32_t OperandA = 0; // Adresse cible (sans le marqueur)
	HdPackConditionOperator Operator = {};
	uint32_t OperandB = 0; // Valeur constante OU Adresse de comparaison
	uint8_t Mask = 0xFF;

	SnesMemorySpace Space = SnesMemorySpace::CPU;
	bool IsConstantCheck = true; // Permet de basculer entre CheckConstant et Check

	// added by Gwenolo 
	std::vector<uint8_t> OperandBList;

	void Initialize(uint32_t operandA, HdPackConditionOperator op, uint32_t operandB, uint8_t mask, SnesMemorySpace space, bool isConstant)
	{
		OperandA = operandA;
		Operator = op;
		OperandB = operandB;
		Mask = mask;
		Space = space;
		IsConstantCheck = isConstant;
	}

	void InitializeForInOperator(uint32_t operandA, HdPackConditionOperator op, const std::string& operandBStr, uint8_t mask, SnesMemorySpace space)
	{
		OperandA = operandA;
		Operator = op;
		Mask = mask;
		Space = space;
		IsConstantCheck = true; // L'opérateur IN n'a de sens qu'avec des constantes
		OperandBList.clear();

		std::vector<std::string> values = StringUtilities::Split(operandBStr, '|');
		for(const auto& val : values) {
			OperandBList.push_back(HexUtilities::FromHex(val));
		}
	}

	// Génération dynamique du nom pour le snes_hires.txt
	string GetConditionName() override
	{
		string prefix = "memory";
		switch(Space) {
			case SnesMemorySpace::VRAM:  prefix = "vramMemory"; break;
			case SnesMemorySpace::OAM:   prefix = "oamMemory"; break;
			case SnesMemorySpace::CGRAM: prefix = "cgramMemory"; break;
			case SnesMemorySpace::ARAM:  prefix = "aramMemory"; break;
			default: break; // CPU
		}
		return prefix + (IsConstantCheck ? "CheckConstant" : "Check");
	}

	string ToString() override
	{
		stringstream out;
		out << "<condition>" << Name << "," << GetConditionName() << ",";
		out << HexUtilities::ToHex(OperandA) << ",";

		switch(Operator) {
			case HdPackConditionOperator::Equal: out << "=="; break;
			case HdPackConditionOperator::NotEqual: out << "!="; break;
			case HdPackConditionOperator::GreaterThan: out << ">"; break;
			case HdPackConditionOperator::LowerThan: out << "<"; break;
			case HdPackConditionOperator::LowerThanOrEqual: out << "<="; break;
			case HdPackConditionOperator::GreaterThanOrEqual: out << ">="; break;
			case HdPackConditionOperator::In: out << "In"; break; // Added by Gwenolo
		}
		out << ",";

		if(Operator == HdPackConditionOperator::In) {
			for(size_t i = 0; i < OperandBList.size(); i++) {
				out << HexUtilities::ToHex(OperandBList[i]);
				if(i < OperandBList.size() - 1) out << "|";
			}
		} else {
			out << HexUtilities::ToHex(OperandB);
		}

		if(Mask != 0xFF) {
			out << "," << HexUtilities::ToHex(Mask);
		}

		return out.str();
	}
};

// ---------------------------------------------------------
// 3. CONDITIONS DE RENDU (Miroirs, Positions)
// ---------------------------------------------------------
struct SnesHdPackHorizontalMirroringCondition : public SnesHdPackCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::HMirror; }
	string GetConditionName() override { return "hmirror"; }
	string ToString() override { return ""; }
	bool IsExcludedFromFile() override { return true; }

	// SNES : On utilise notre SnesHdPixelData
	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		return tile && tile->HFlip;
	}
};

struct SnesHdPackVerticalMirroringCondition : public SnesHdPackCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::VMirror; }
	string GetConditionName() override { return "vmirror"; }
	string ToString() override { return ""; }
	bool IsExcludedFromFile() override { return true; }

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		return tile && tile->VFlip;
	}
};
// ---------------------------------------------------------
// 4. MEMORY CHECKS (Constantes et Variables)
// ---------------------------------------------------------

// --- VARIANTE 1 : COMPARAISON AVEC UNE CONSTANTE (ex: == 3F) ---
struct SnesHdPackMemoryCheckConstantCondition : public SnesHdPackBaseMemoryCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::MemoryCheckConstant; }
	SnesHdPackMemoryCheckConstantCondition() { _useCache = true; }

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		if(!_screenInfo) return false;

		// On injecte le marqueur d'espace mémoire dans l'adresse pour lire la bonne cachette
		uint32_t finalAddress = OperandA | (uint32_t)Space;
		uint8_t a = (uint8_t)(_screenInfo->WatchedAddressValues[finalAddress] & Mask);
		uint8_t b = OperandB; // Constante

		switch(Operator) {
			case HdPackConditionOperator::Equal: return a == b;
			case HdPackConditionOperator::NotEqual: return a != b;
			case HdPackConditionOperator::GreaterThan: return a > b;
			case HdPackConditionOperator::LowerThan: return a < b;
			case HdPackConditionOperator::LowerThanOrEqual: return a <= b;
			case HdPackConditionOperator::GreaterThanOrEqual: return a >= b;
			case HdPackConditionOperator::In:
			{
				for(uint8_t valB : OperandBList) {
					if(a == valB) return true;
				}
				return false;
			}
			default: return false;
		}
	}
};

// --- VARIANTE 2 : COMPARAISON ENTRE DEUX REGISTRES (ex: == 8000) ---
struct SnesHdPackMemoryCheckCondition : public SnesHdPackBaseMemoryCondition
{
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::MemoryCheck; }
	SnesHdPackMemoryCheckCondition() { _useCache = true; }

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		if(!_screenInfo) return false;

		// Adresse A (Espace Mémoire cible)
		uint32_t finalAddressA = OperandA | (uint32_t)Space;
		uint8_t a = (uint8_t)(_screenInfo->WatchedAddressValues[finalAddressA] & Mask);

		// Adresse B (Ici OperandB est une adresse ! On assume qu'elle est dans le même Espace Mémoire)
		uint32_t finalAddressB = OperandB | (uint32_t)Space;
		uint8_t b = (uint8_t)(_screenInfo->WatchedAddressValues[finalAddressB] & Mask);

		switch(Operator) {
			case HdPackConditionOperator::Equal: return a == b;
			case HdPackConditionOperator::NotEqual: return a != b;
			case HdPackConditionOperator::GreaterThan: return a > b;
			case HdPackConditionOperator::LowerThan: return a < b;
			case HdPackConditionOperator::LowerThanOrEqual: return a <= b;
			case HdPackConditionOperator::GreaterThanOrEqual: return a >= b;
			case HdPackConditionOperator::In: return false; // In n'a pas de sens entre 2 registres
			default: return false;
		}
	}
};

// ---------------------------------------------------------
// 5. CONDITIONS DE Positions (TILES, SPRITES)
// ---------------------------------------------------------

struct SnesHdPackSpriteAtPositionCondition : public SnesHdPackBaseTileCondition
{
	bool CheckAnyX = false;
	bool CheckAnyY = false;
	bool ConsiderVisible = false;

	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::SpriteAtPosition; }
	string GetConditionName() override { return "spriteAtPosition"; }

	void InitializeSpriteCond(int32_t x, int32_t y, bool anyX, bool anyY, uint64_t memoryHash, uint32_t paletteHash, bool ignorePalette, bool considerVisible)
	{
		SnesHdPackBaseTileCondition::Initialize(x, y, memoryHash, paletteHash, ignorePalette);
		CheckAnyX = anyX;
		CheckAnyY = anyY;
		ConsiderVisible = considerVisible;
		_useCache = false; // La position dépend de l'OAM, à évaluer en temps réel
	}

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override;
};

enum class SnesHdPackConditionScope { Screen, Layer };

//--------------------------------------------------------------------------------
struct SnesHdPackTileAtPositionCondition : public SnesHdPackBaseTileCondition
{
	uint8_t TargetLayers = 0xFF; // Bitmask for layers


	bool CheckAnyX = false;
	bool CheckAnyY = false;
	SnesHdPackConditionScope Scope = SnesHdPackConditionScope::Screen;

	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::TileAtPosition; }
	string GetConditionName() override { return "tileAtPosition"; }

	void InitializeLayers(const string& layerStr)
	{
		if(layerStr == "all" || layerStr.empty()) {
			TargetLayers = 0xFF;
		} else {
			TargetLayers = 0;
			auto layers = StringUtilities::Split(layerStr, '|');
			for(auto& l : layers) {
				int layerIndex = std::stoi(l) - 1;
				if(layerIndex >= 0 && layerIndex < 4) {
					TargetLayers |= (1 << layerIndex);
				}
			}
		}
	}

	//--------------------
	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override;
	//------------------------

};
//--------------------------------------------------------------------------------

struct SnesHdPackFrameRangeCondition : public SnesHdPackCondition
{
	uint32_t OperandA = 0; // Le Diviseur
	uint32_t OperandB = 0; // La Valeur de Comparaison

	SnesHdPackFrameRangeCondition() { _useCache = true; } // Si ta classe de base gère le cache

	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::FrameRange; } // Assure-toi que FrameRange existe dans l'enum !
	string GetConditionName() override { return "frameRange"; }

	void Initialize(uint32_t operandA, uint32_t operandB)
	{
		// Sécurité anti-crash : on interdit la division par zéro !
		OperandA = (operandA == 0) ? 1 : operandA;
		OperandB = operandB;
	}

	string ToString() override
	{
		stringstream out;
		out << "<condition>" << Name << "," << GetConditionName() << ",";
		out << OperandA << ",";
		out << OperandB;

		return out.str();
	}

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override
	{
		// Si _screenInfo n'est pas dispo, on renvoie false
		if(!_screenInfo) return false;

		// La fameuse formule d'animation temporelle
		return (_screenInfo->FrameNumber % OperandA) >= OperandB;
	}
};
//--------------------------------------------------------------------------------

class SnesHdPackTileNearbyCondition : public SnesHdPackCondition
{
public:
	int OffsetX = 0;
	int OffsetY = 0;
	bool CheckAnyX = false;
	bool CheckAnyY = false;

	uint64_t MemoryHash = 0;
	uint32_t PaletteHash = 0;
	bool IgnorePalette = false;

	// On implémente les fonctions virtuelles pures de la classe mère
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::TileNearby; }
	std::string GetConditionName() override { return "tileNearby"; }

	std::string ToString() override
	{
		// Sert souvent pour le debug dans Mesen
		return "tileNearby condition: " + Name;
	}

	void Initialize(int offsetX, int offsetY, bool anyX, bool anyY, uint64_t memHash, uint32_t palHash, bool ignorePal)
	{
		OffsetX = offsetX;
		OffsetY = offsetY;
		CheckAnyX = anyX;
		CheckAnyY = anyY;
		MemoryHash = memHash;
		PaletteHash = palHash;
		IgnorePalette = ignorePal;

		// Les conditions basées sur la position relative ne peuvent généralement pas utiliser le cache global 
		// de la même manière qu'un MemoryCheck, car le résultat dépend des coordonnées X/Y évaluées.
		_useCache = false;
	}

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override;
};

//---------------------------------------------------------------------------------------------------------------------------------
class SnesHdPackSpriteNearbyCondition : public SnesHdPackCondition
{
public:
	int OffsetX = 0;
	int OffsetY = 0;
	bool CheckAnyX = false;
	bool CheckAnyY = false;

	uint64_t MemoryHash = 0;
	uint32_t PaletteHash = 0;
	bool IgnorePalette = false;

	// On implémente les fonctions virtuelles pures de la classe mère
	SnesHdPackConditionType GetConditionType() override { return SnesHdPackConditionType::SpriteNearby; }
	std::string GetConditionName() override { return "spriteNearby"; }

	std::string ToString() override
	{
		// Sert souvent pour le debug dans Mesen
		return "tileNearby condition: " + Name;
	}

	void Initialize(int offsetX, int offsetY, bool anyX, bool anyY, uint64_t memHash, uint32_t palHash, bool ignorePal)
	{
		OffsetX = offsetX;
		OffsetY = offsetY;
		CheckAnyX = anyX;
		CheckAnyY = anyY;
		MemoryHash = memHash;
		PaletteHash = palHash;
		IgnorePalette = ignorePal;

		// Les conditions basées sur la position relative ne peuvent généralement pas utiliser le cache global 
		// de la même manière qu'un MemoryCheck, car le résultat dépend des coordonnées X/Y évaluées.
		_useCache = false;
	}

	bool InternalCheckCondition(int x, int y, SnesHdPixelData* tile) override;
};