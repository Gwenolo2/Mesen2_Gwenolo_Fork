#pragma once
// Valeurs RAM nommees (<watchMemoryAddress>) et expressions de position ($page*256+$x), communes a NES / SNES / Game Boy.
// Utilise par <background> (options scrollx= / scrolly=). Lecture seule de la RAM du jeu.
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

struct HdWatchVar
{
	std::string Name;
	uint32_t Address = 0;
	uint8_t Size = 1;       // 1 ou 2 octets (petit-boutiste)
	bool Signed = false;
	uint32_t Space = 0;     // SNES : marqueur d'espace memoire (bits 28-31), 0 = CPU
};

// Valeur lue sur 1 ou 2 octets (raw = octets assembles)
inline int32_t HdWatchCompose(uint32_t raw, uint8_t size, bool isSigned)
{
	if(isSigned) return size == 1 ? (int32_t)(int8_t)raw : (int32_t)(int16_t)raw;
	return (int32_t)raw;
}

// Somme de produits de nombres et de valeurs $nom. Ex. : $page*256+$x   ou   -$x*0.5
struct HdWatchExpr
{
	struct Factor { int Var = -1; float Val = 1.0f; };   // Var >= 0 : indice dans le tableau de valeurs
	struct Term { float Sign = 1.0f; std::vector<Factor> F; };
	std::vector<Term> T;

	bool Empty() const { return T.empty(); }

	// false si une valeur surveillee n'est pas encore lue (INT32_MIN)
	bool Eval(const std::vector<int32_t>& vals, float& out) const
	{
		float sum = 0.0f;
		for(const Term& t : T) {
			float p = t.Sign;
			for(const Factor& f : t.F) {
				if(f.Var >= 0) {
					if((size_t)f.Var >= vals.size() || vals[f.Var] == INT32_MIN) return false;
					p *= (float)vals[f.Var];
				} else {
					p *= f.Val;
				}
			}
			sum += p;
		}
		out = sum;
		return true;
	}

	// V doit avoir un membre Name (HdWatchVar, HdWatchedValue...)
	template<class V>
	static bool Parse(const std::string& text, const std::vector<V>& vars, HdWatchExpr& out, std::string& err)
	{
		std::string e;
		for(char c : text) if(c != ' ' && c != '\t') e += c;
		if(e.empty()) { err = "expression vide"; return false; }
		out.T.clear();
		size_t i = 0;
		while(i < e.size()) {
			Term term;
			if(e[i] == '+') i++;
			else if(e[i] == '-') { term.Sign = -1.0f; i++; }
			size_t j = i;
			while(j < e.size() && e[j] != '+' && e[j] != '-') j++;
			const std::string t = e.substr(i, j - i);
			if(t.empty()) { err = "terme vide"; return false; }
			size_t a = 0;
			while(a <= t.size()) {
				size_t b = t.find('*', a);
				if(b == std::string::npos) b = t.size();
				const std::string f = t.substr(a, b - a);
				if(f.empty()) { err = "facteur vide"; return false; }
				Factor fac;
				if(f[0] == '$') {
					const std::string nm = f.substr(1);
					int idx = -1;
					for(size_t k = 0; k < vars.size(); k++) if(vars[k].Name == nm) { idx = (int)k; break; }
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
};

// Ligne <watchMemoryAddress>nom,adresse[,taille][,signed][,espace] -> HdWatchVar.
// allowSpace (SNES) : espace memoire vram | oam | cgram | aram (defaut : bus CPU). maxAddr : plus grande adresse valide.
inline bool HdWatchParseDecl(const std::vector<std::string>& tokens, bool allowSpace, uint32_t maxAddr, const std::vector<HdWatchVar>& existing, HdWatchVar& w, std::string& err)
{
	auto trim = [](const std::string& s) {
		size_t a = s.find_first_not_of(" \t");
		if(a == std::string::npos) return std::string();
		size_t b = s.find_last_not_of(" \t");
		return s.substr(a, b - a + 1);
	};
	auto lower = [](std::string s) { for(char& c : s) if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; };
	if(tokens.size() < 2 || tokens.size() > 5) { err = "syntaxe nom,adresse[,taille][,signed]" + std::string(allowSpace ? "[,vram|oam|cgram|aram]" : ""); return false; }
	w = HdWatchVar();
	w.Name = trim(tokens[0]);
	if(w.Name.empty()) { err = "nom vide"; return false; }
	for(char c : w.Name) {
		if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) { err = "nom invalide (lettres, chiffres et _ seulement) : " + w.Name; return false; }
	}
	std::string addr = trim(tokens[1]);
	if(!addr.empty() && addr[0] == '$') addr = addr.substr(1);
	else if(addr.size() > 2 && addr[0] == '0' && (addr[1] == 'x' || addr[1] == 'X')) addr = addr.substr(2);
	if(addr.empty() || addr.size() > 6 || addr.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) { err = "adresse hexadecimale invalide : " + tokens[1]; return false; }
	w.Address = (uint32_t)std::strtoul(addr.c_str(), nullptr, 16);
	for(size_t i = 2; i < tokens.size(); i++) {
		std::string t = lower(trim(tokens[i]));
		if(t == "signed") w.Signed = true;
		else if(t == "unsigned") w.Signed = false;
		else if(t == "1") w.Size = 1;
		else if(t == "2") w.Size = 2;
		else if(allowSpace && (t == "cpu" || t == "ram")) w.Space = 0;
		else if(allowSpace && (t == "vram" || t == "ppu")) w.Space = 0x10000000;
		else if(allowSpace && t == "oam") w.Space = 0x20000000;
		else if(allowSpace && t == "cgram") w.Space = 0x30000000;
		else if(allowSpace && (t == "aram" || t == "spc")) w.Space = 0x40000000;
		else { err = "parametre inconnu : " + t; return false; }
	}
	if(w.Address + w.Size - 1 > maxAddr) { err = "adresse + taille hors limite : " + w.Name; return false; }
	for(const HdWatchVar& o : existing) {
		if(o.Name == w.Name) { err = "nom deja declare : " + w.Name; return false; }
	}
	return true;
}
