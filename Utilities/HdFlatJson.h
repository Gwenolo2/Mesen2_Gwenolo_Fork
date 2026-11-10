#pragma once
// Lecteur JSON "a plat" partage par les <2DOverlay> SNES et NES.
// Format : { "cle": valeur, ... } ; cles insensibles a la casse ; commentaires // acceptes ; pas d'objets imbriques.
// Les valeurs entre guillemets sont lues sans guillemets ; les autres sont lues jusqu'a , } ou fin de ligne.
#include <string>
#include <unordered_map>
#include <cctype>
#include "Utilities/StringUtilities.h"

inline std::unordered_map<std::string, std::string> ParseFlatJson(const std::string& s)
{
	std::unordered_map<std::string, std::string> out;
	size_t i = 0, n = s.size();
	auto readString = [&]() -> std::string {
		std::string r;
		i++; // guillemet ouvrant
		while(i < n && s[i] != '"') {
			if(s[i] == '\\' && i + 1 < n && (s[i + 1] == '\\' || s[i + 1] == '"')) { i++; }
			r += s[i++];
		}
		if(i < n) i++; // guillemet fermant
		return r;
	};
	while(i < n) {
		char c = s[i];
		if(c == '/' && i + 1 < n && s[i + 1] == '/') { while(i < n && s[i] != '\n') i++; continue; }
		if(c != '"') { i++; continue; }
		std::string key = readString();
		while(i < n && s[i] != ':' && s[i] != ',' && s[i] != '}') i++;
		if(i >= n || s[i] != ':') continue;
		i++;
		while(i < n && std::isspace((unsigned char)s[i])) i++;
		std::string val;
		if(i < n && s[i] == '"') {
			val = readString();
		} else {
			while(i < n && s[i] != ',' && s[i] != '}' && s[i] != '\n' && s[i] != '\r') val += s[i++];
			val = StringUtilities::Trim(val);
		}
		out[StringUtilities::ToLower(StringUtilities::Trim(key))] = val;
	}
	return out;
}
