#pragma once
// =====================================================================================
// MOTEUR 3D GPU des packs HD (OpenGL 3.3 core, contexte invisible ouvert par SDL2).
// Voir docs/claude/11_GPU_3D.md
//
// Le filtre HD compose toujours l'image sur le processeur ; la carte graphique dessine les
// modeles <m7model> dans une image cachee (anticrenelage 4x), avec la MEME projection que le
// code processeur : la table ligne par ligne de la camera Mode 7 est envoyee comme texture et
// le shader de sommets refait la recherche par dichotomie. L'image (couleur premultipliee) et
// la "ligne du sol" de chaque pixel sont relues puis fusionnees par composeBand.
// Indisponible (pas de SDL2.dll, pas d'OpenGL 3.3...) -> le moteur processeur prend le relais.
// =====================================================================================
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include "SNES/HdPacks/SnesHdMode7Camera.h"

struct SnesHdModelMesh;
struct SnesHdPackBitmapInfo;

struct SnesHdGpuInstance
{
	const SnesHdModelMesh* Mesh = nullptr;
	// placement : (u, v, hauteur) = Model x (x, y, z, 1) ; 3 lignes de 4, u et v RELATIFS a la camera de la vue
	float Model[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
	const std::vector<float>* Palette = nullptr; // os animes (12 flottants par emplacement) ; nullptr = statique
	float Ambient = 0.45f, Fade = 1;
};

struct SnesHdGpuView
{
	const SnesHdM7Zone* Zone = nullptr;
	int ClipY0 = 0, ClipY1 = 0;    // lignes HD de la vue (incluses)
	std::vector<SnesHdGpuInstance> Instances;
};

class SnesHdGpu
{
public:
	static SnesHdGpu& Get();

	// Ouvre le contexte au premier appel (une seule tentative). Faux = moteur processeur.
	bool Available();
	const std::string& Info() const { return _info; }

	// Dessine les modeles de toutes les vues dans une image w x h (pixels HD), mais seulement dans le
	// rectangle [rx0..rx1] x [ry0..ry1] (inclus) ou ils se trouvent : effacement, dessin et relecture limites.
	// outColor / outGround : rectangle seulement (largeur rx1-rx0+1), lignes de haut en bas.
	// outColor : ARGB premultiplie (0 = rien) ; outGround : ligne du sol (pixels SNES), -1 = rien.
	// En deux temps pour ne pas attendre la carte graphique :
	//  SubmitModels : dessin + relecture lancee en arriere-plan (PBO), retour immediat ;
	//  FetchModels  : recuperation (normalement deja prete) au moment de la fusion, meme frame.
	bool SubmitModels(const std::vector<SnesHdGpuView>& views, uint32_t w, uint32_t h, float scale, float dstX, float dstY,
		int rx0, int ry0, int rx1, int ry1);
	bool FetchModels(std::vector<uint32_t>& outColor, std::vector<float>& outGround);

private:
	SnesHdGpu() = default;
	bool Init();
	bool MakeCurrent();
	void ReleaseCurrent();
	bool EnsureTargets(uint32_t w, uint32_t h);
	bool BuildPrograms();
	unsigned int GetTexture(SnesHdPackBitmapInfo* bmp);
	struct GpuMesh { unsigned int Vao = 0, Vbo = 0; struct Range { int Mat, First, Count; }; std::vector<Range> Ranges; };
	GpuMesh* GetMesh(const SnesHdModelMesh* mesh);

	std::mutex _lock;
	bool _tried = false, _ok = false;
	std::string _info;
	void* _window = nullptr;
	void* _context = nullptr;
	std::thread::id _currentThread; // thread ou le contexte est actif (evite un changement de contexte par frame)

	unsigned int _progColor = 0, _progGround = 0;
	unsigned int _msFbo = 0, _msColor = 0, _msDepth = 0;   // anticrenelage
	unsigned int _rsFbo = 0, _rsColor = 0;                 // couleur resolue
	unsigned int _grFbo = 0, _grTex = 0, _grDepth = 0;     // ligne du sol (1 echantillon)
	unsigned int _tableTex = 0;
	unsigned int _boneTex = 0;
	int _samples = 0;
	uint32_t _w = 0, _h = 0;
	std::unordered_map<const SnesHdModelMesh*, GpuMesh> _meshes;
	std::unordered_map<SnesHdPackBitmapInfo*, unsigned int> _textures;
	std::vector<uint32_t> _tmpColor;
	std::vector<float> _tmpGround;
	double _timeAcc = 0; uint32_t _timeFrames = 0; uint64_t _pixAcc = 0; // mesure du cout
	unsigned int _pboColor = 0, _pboGround = 0;
	size_t _pboCapacity = 0;
	bool _pending = false;
	int _pendW = 0, _pendH = 0;
};
