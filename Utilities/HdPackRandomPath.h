#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "Utilities/FolderUtilities.h"

// Mot-cle "Randomize" ou "random" (insensible a la casse) dans un chemin de PNG de HD pack.
// Le mot-cle est remplace par un sous-dossier du niveau qui le precede, tire au hasard.
// Exemple : "tiles/Randomize/sol.png" -> "tiles/brique/sol.png" (ou "tiles/pierre/sol.png", ...).
// Le mot-cle le plus tot dans le chemin est traite en premier ; a position egale, "randomize" l'emporte sur "random".
//
// Tirage unique par pack : "choices" memorise, pour chaque dossier parent (en minuscules), le sous-dossier tire.
// Toutes les lignes qui pointent sur le meme niveau recoivent donc le meme sous-dossier. "choices" doit etre
// conserve pendant tout le chargement du pack et vide a chaque nouveau pack.
//
// Sources de listing : disque (dossier de pack) ou entrees d'un zip (zipEntries non nul).
// Si le niveau ne contient aucun sous-dossier, la fonction renvoie -1 (aucun repli silencieux).
//
// Retour : 0 = pas de mot-cle (resolved = path), 1 = chemin tire (resolved), -1 = erreur (errorInfo renseigne).
inline std::string HdPackToLowerAscii(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

inline std::set<std::string> HdPackListFoldersOnDisk(const std::string& rootFolder, const std::string& parentRelative)
{
	std::set<std::string> folders;
	std::error_code ec;
	for(const auto& entry : std::filesystem::directory_iterator(FolderUtilities::CombinePath(rootFolder, parentRelative), ec)) {
		if(entry.is_directory(ec)) {
			folders.insert(entry.path().filename().string());
		}
	}
	return folders;
}

inline std::set<std::string> HdPackListFoldersInZip(const std::vector<std::string>& entries, const std::string& parentRelative)
{
	std::set<std::string> folders;
	std::string prefix = parentRelative;
	if(!prefix.empty() && prefix.back() != '/') {
		prefix += '/';
	}
	for(const std::string& e : entries) {
		if(e.size() <= prefix.size() || e.compare(0, prefix.size(), prefix) != 0) {
			continue;
		}
		std::string rest = e.substr(prefix.size());
		size_t slash = rest.find('/');
		if(slash != std::string::npos && slash > 0) {
			folders.insert(rest.substr(0, slash));
		}
	}
	return folders;
}

inline int HdPackResolveRandomPath(const std::string& path, const std::string& rootFolder, std::string& resolved, std::string& errorInfo,
                                   std::map<std::string, std::string>& choices, const std::vector<std::string>* zipEntries = nullptr)
{
	std::string current = path;
	std::replace(current.begin(), current.end(), '\\', '/');
	bool replaced = false;

	for(int depth = 0; depth < 16; depth++) {
		std::string lower = HdPackToLowerAscii(current);
		size_t posLong = lower.find("randomize");
		size_t posShort = lower.find("random");
		size_t pos, len;
		if(posLong != std::string::npos && posLong <= posShort) {
			pos = posLong;
			len = 9;
		} else if(posShort != std::string::npos) {
			pos = posShort;
			len = 6;
		} else {
			break;
		}

		std::string parentRelative = current.substr(0, pos);
		std::string afterKeyword = current.substr(pos + len);
		std::string key = HdPackToLowerAscii(parentRelative);

		std::string chosen;
		auto found = choices.find(key);
		if(found != choices.end()) {
			chosen = found->second;
		} else {
			std::set<std::string> folders = zipEntries ? HdPackListFoldersInZip(*zipEntries, parentRelative) : HdPackListFoldersOnDisk(rootFolder, parentRelative);
			if(folders.empty()) {
				errorInfo = "no readable subfolder for randomized path at: '" + parentRelative + "'";
				return -1;
			}
			std::vector<std::string> list(folders.begin(), folders.end());
			static std::mt19937 gen(std::random_device{}());
			std::uniform_int_distribution<size_t> distrib(0, list.size() - 1);
			chosen = list[distrib(gen)];
			choices[key] = chosen;
		}

		current = parentRelative + chosen + afterKeyword;
		replaced = true;
	}

	resolved = replaced ? current : path;
	return replaced ? 1 : 0;
}
