#pragma once
// =====================================================================================
// BASE GPU COMMUNE des packs HD (OpenGL 3.3 core, contexte invisible ouvert par SDL2).
// Voir docs/claude/34_Diorama_Chemin_GPU_NES.md (option C, etape G0).
//
// Independante de la console : ne connait ni le Mode 7 ni la SNES. Premier utilisateur : le diorama NES.
// SnesHdGpu n'est PAS modifie (il garde son propre contexte) ; une fois le diorama valide, il pourra etre
// bascule sur cette classe dans un chantier separe.
//
// G0 : seulement le contexte, une image cachee, un dessin de test et la relecture asynchrone (PBO).
// Indisponible (pas de SDL2.dll, pas d'OpenGL 3.3, coeur libretro...) -> Available() renvoie faux.
// =====================================================================================
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <cstdint>

class HdGpuCore
{
public:
	static HdGpuCore& Get();

	// Ouvre le contexte au premier appel (une seule tentative). Faux = pas de GPU.
	bool Available();
	const std::string& Info() const { return _info; }

	// G0 : dessine dans une image w x h un triangle colore anime (selon frame) et un carre jaune translucide,
	// sur fond transparent, puis lance la relecture asynchrone. Retour immediat apres l'envoi.
	bool SubmitTest(uint32_t w, uint32_t h, uint32_t frame);
	// Recupere l'image dessinee par SubmitTest : ARGB premultiplie (0 = rien), lignes de haut en bas.
	bool Fetch(std::vector<uint32_t>& outColor);

private:
	HdGpuCore() = default;
	bool Init();
	bool MakeCurrent();
	void ReleaseCurrent();
	bool EnsureTarget(uint32_t w, uint32_t h);
	bool BuildProgram();

	std::mutex _lock;
	bool _tried = false, _ok = false;
	std::string _info;
	void* _window = nullptr;
	void* _context = nullptr;
	std::thread::id _currentThread; // thread ou le contexte est actif (evite un changement de contexte par frame)

	unsigned int _prog = 0, _vao = 0;
	unsigned int _fbo = 0, _tex = 0;
	unsigned int _pbo = 0;
	size_t _pboCapacity = 0;
	uint32_t _w = 0, _h = 0;
	bool _pending = false;
	int _pendW = 0, _pendH = 0;
};
