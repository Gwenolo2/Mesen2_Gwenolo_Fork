#pragma once
// Diorama SNES (doc 59) : les balises diorama du snes_hires.txt sont lues par le chargeur NES (memes regles, meme syntaxe,
// HdPackLoader::ParseForeignDioramaLine) dans un HdPackData a part ; les conditions [A&!B] du SNES y sont branchees par un
// adaptateur (SnesHdDioCondition). Balises propres au SNES : <DioramaLayer> (BG -> couche) et <DioramaSprites> (sprites -> couche).
#include "pch.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "NES/HdPacks/HdData.h"
#include "SNES/HdPacks/SnesHdData.h"
#include "Shared/HdWatchExpr.h"

// Condition NES qui evalue une liste de conditions SNES a l'echelle de l'image (sans position ni tile)
struct SnesHdDioCondition : public HdPackCondition
{
	std::vector<SnesHdPackCondition*> Conds;

	SnesHdDioCondition()
	{
		Name = "snes"; // jamais '!' : la negation est portee par les conditions SNES ("!nom")
		// CheckCondition refuse un ecran nul : l'adaptateur n'en a pas besoin, on lui donne un pointeur non nul jamais lu
		Initialize(reinterpret_cast<HdScreenInfo*>(this), nullptr);
	}
	HdPackConditionType GetConditionType() override { return HdPackConditionType::MemoryCheck; }
	string GetConditionName() override { return "snes"; }
	string ToString() override { return "[snes]"; }

protected:
	bool InternalCheckCondition(int, int, HdPpuTileInfo*) override
	{
		for(SnesHdPackCondition* c : Conds) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	}
};

// Cible d'une source : 0..3 = L0..L3, 4 = couche fixe (a plat devant la scene, ex. HUD), 5 = retiree de l'image
constexpr int SnesHdDioFixed = 4;
constexpr int SnesHdDioOff = 5;

// <DioramaLayer>bgN,layer=K|fixed|off[,prio=0|1] : BG N (1..4) -> couche ; prio = bit de priorite des tiles (sans prio : les deux)
struct SnesHdDioLayerRule
{
	int Bg = 0;      // 0..3
	int Prio = -1;   // -1 toutes, 0 normale, 1 haute
	int Layer = 2;
	std::vector<SnesHdPackCondition*> Conditions;
};

// <DioramaSprites>layer=K|fixed|off[,prio=0..3] : sprites (priorite OAM, sans prio : toutes) -> couche
struct SnesHdDioSpriteRule
{
	int Prio = -1;   // -1 toutes, 0..3
	int Layer = 3;
	std::vector<SnesHdPackCondition*> Conditions;
};

struct SnesHdDiorama
{
	HdPackData Data;                                          // regles diorama (format NES)
	std::vector<std::unique_ptr<SnesHdDioCondition>> Conds;   // adaptateurs (proprietaires)
	std::unordered_map<std::string, HdPackCondition*> ByName; // conditions nommees, pour until= (etapes suivantes)
	std::vector<SnesHdDioLayerRule> LayerRules;
	std::vector<SnesHdDioSpriteRule> SpriteRules;
	int RefBg = 0;        // <Diorama>...,ref=bgN : BG de reference du repere du monde (0..3 = BG1..BG4)
	HdWatchExpr RefX, RefY; // <Diorama>...,refx=$expr,refy=$expr : repere depuis la RAM (remplace ref=bgN sur cet axe)

	HdPackCondition* MakeCond(const std::vector<SnesHdPackCondition*>& conds)
	{
		auto c = std::make_unique<SnesHdDioCondition>();
		c->Conds = conds;
		HdPackCondition* p = c.get();
		Conds.push_back(std::move(c));
		return p;
	}
};
