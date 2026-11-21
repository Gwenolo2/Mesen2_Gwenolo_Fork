#pragma once
// Diorama Game Boy (doc 56) : les balises diorama du gb_hires.txt sont lues par le chargeur NES (memes regles, meme syntaxe) dans un
// HdPackData a part ; les conditions [termes] du Game Boy y sont branchees par un adaptateur (GbHdDioCondition).
#include "pch.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "NES/HdPacks/HdData.h"
#include "Gameboy/HdPacks/GbHdPackLoader.h"

// Condition NES qui evalue des termes Game Boy ([A&!B]) a l'echelle de l'image (sans position ni tile)
struct GbHdDioCondition : public HdPackCondition
{
	std::vector<GbHdTerm> Terms;
	const std::function<bool(const std::vector<GbHdTerm>&)>* Eval = nullptr;

	GbHdDioCondition()
	{
		Name = "gb"; // jamais '!' : la negation est dans les termes
		// CheckCondition refuse un ecran nul : l'adaptateur n'en a pas besoin, on lui donne un pointeur non nul jamais lu
		Initialize(reinterpret_cast<HdScreenInfo*>(this), nullptr);
	}
	HdPackConditionType GetConditionType() override { return HdPackConditionType::MemoryCheck; }
	string GetConditionName() override { return "gb"; }
	string ToString() override { return "[gb]"; }

protected:
	bool InternalCheckCondition(int, int, HdPpuTileInfo*) override
	{
		return Eval && *Eval ? (*Eval)(Terms) : false;
	}
};

struct GbHdDiorama
{
	HdPackData Data;                                        // regles diorama (format NES)
	std::vector<std::unique_ptr<GbHdDioCondition>> Conds;   // adaptateurs (proprietaires)
	std::unordered_map<std::string, HdPackCondition*> ByName; // conditions nommees (et "!nom") pour until= de <DayNight>
	std::function<bool(const std::vector<GbHdTerm>&)> Eval;  // evaluation des termes, fournie par le filtre HD
	const void* EvalOwner = nullptr;                        // filtre qui a fourni Eval (efface a sa destruction)
	bool WindowFixed = true;                                // <DioramaWindow> : la fenetre (HUD) reste a plat devant la scene

	HdPackCondition* MakeCond(const std::vector<GbHdTerm>& terms)
	{
		auto c = std::make_unique<GbHdDioCondition>();
		c->Terms = terms;
		c->Eval = &Eval;
		HdPackCondition* p = c.get();
		Conds.push_back(std::move(c));
		return p;
	}
	HdPackCondition* Named(const std::string& name, bool negated)
	{
		const std::string key = (negated ? "!" : "") + name;
		auto it = ByName.find(key);
		if(it != ByName.end()) return it->second;
		GbHdTerm t; t.Name = name; t.Negated = negated;
		HdPackCondition* p = MakeCond({ t });
		ByName[key] = p;
		return p;
	}
};
