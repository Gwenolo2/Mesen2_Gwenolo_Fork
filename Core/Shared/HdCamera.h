#pragma once
// =====================================================================================
// INTERFACE CAMERA COMMUNE des packs HD (SNES + NES) et camera "couche 2D".
// Voir docs/claude/32_Scene_Diorama_Note_Conception.md (section 2) et 35_Diorama_Journal_Etapes_G.md (G1).
//
// C++ pur : aucun GPU, aucune dependance console. L'adaptateur Mode 7 est dans
// SNES/HdPacks/SnesHdM7CameraAdapter.h (il enveloppe SnesHdM7Zone sans changer ses formules).
//
// Conventions de l'interface (monde 3D de la camera) :
//  - Mode 7 (HasGround() = vrai) : (X, Y, Z) = (u carte, hauteur au-dessus du sol, v carte).
//  - Couche 2D (HasGround() = faux) : plan du jeu = (X, Y) a Z = 0, X vers la droite, Y vers le BAS (comme l'ecran),
//    Z = profondeur vers l'interieur de l'ecran (Z < 0 : devant le plan du jeu ; Z > 0 : derriere).
//  - Coordonnees d'ecran : pixels continus, ligne y = bord haut de la ligne y, x = bord gauche.
//    Pour la camera couche, c'est l'espace des lignes de defilement fournies (NES : lignes PPU absolues 0..239,
//    colonnes 0..255) ; l'appelant retranche l'overscan.
// Project() ne teste pas "hors de l'ecran" (l'appelant recadre) : il renvoie faux seulement si le point est
// derriere la camera (ou, en Mode 7, hors de la zone projetable).
// =====================================================================================
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
#include <string>

struct HdScreenPt
{
	double x = 0, y = 0;   // pixels d'ecran continus
	double scale = 1;      // pixels d'ecran par unite monde a cette profondeur
};

class HdCamera
{
public:
	virtual ~HdCamera() = default;
	// Projection d'un point monde. Faux si derriere la camera.
	virtual bool Project(double X, double Y, double Z, HdScreenPt& out) const = 0;
	// Point monde sous un pixel d'ecran. depth : profondeur Z voulue (NaN = plan du jeu pour la camera couche ;
	// ignoree en Mode 7, qui renvoie le point de sol de la ligne). Faux hors de la vue.
	virtual bool Unproject(double sx, double sy, double depth, double& X, double& Y, double& Z) const = 0;
	// Distance au plan de la camera, le long du regard (test de profondeur, "trop pres").
	virtual double DepthOf(double X, double Y, double Z) const = 0;
	// Bande d'ecran de la vue, lignes incluses.
	virtual void Band(int& y0, int& y1) const = 0;
	// Pixels d'ecran par unite monde a la ligne sy (plan du jeu pour la camera couche).
	virtual double ScaleAt(double sy) const = 0;
	// Vrai : le monde a un sol (Mode 7). Faux : couche 2D.
	virtual bool HasGround() const = 0;
};

// -------------------------------------------------------------------------------------
// Camera "couche 2D" : stenope frontal. La translation est le defilement de la couche, PAR LIGNE d'ecran.
//   s = F / (D + Z)                        (echelle a la profondeur Z ; F = D : le plan Z = 0 garde l'echelle 1)
//   x = Ox + (X - Cx(ligne)) * s           Cx(ligne) = defilementX(ligne) + Ox  (point du monde sous l'axe optique)
//   y = Oy + (Y - Cy(ligne)) * s           Cy(ligne) = defilementY(ligne) + Oy
// A Z = 0 : x = X - defilementX(ligne), y = Y - defilementY(ligne), donc les pixels du jeu restent a leur place.
// La ligne d'arrivee d'un point depend du defilement de cette ligne : 4 iterations d'un point fixe, qui convergent
// vite tant que le defilement varie peu d'une ligne a la suivante (NES, Mario : un seul changement par image).
// -------------------------------------------------------------------------------------
class HdLayerCamera : public HdCamera
{
public:
	// Prepare la camera pour les lignes y0..y1 (incluses). Les defilements sont a 0 jusqu'aux SetLine.
	void Begin(int y0, int y1, double ox, double oy, double focal, double dist)
	{
		_y0 = y0; _y1 = y1; _ox = ox; _oy = oy; _f = focal; _d = dist;
		const int n = std::max(0, y1 - y0 + 1);
		_cx.assign((size_t)n, ox);
		_cy.assign((size_t)n, oy);
	}
	// Defilement de la couche sur la ligne y (pixels, comme _lineScrollX / _lineScrollY du NES).
	void SetLine(int y, double scrollX, double scrollY)
	{
		if(y < _y0 || y > _y1) return;
		_cx[(size_t)(y - _y0)] = scrollX + _ox;
		_cy[(size_t)(y - _y0)] = scrollY + _oy;
	}
	bool Valid() const { return !_cx.empty() && _f > 0 && _d > 0; }
	double Focal() const { return _f; }
	double Distance() const { return _d; }

	// Parallaxe automatique : une couche qui defile a la fraction r (0 < r) du plan du jeu est a la profondeur
	// Z = F / r - D (= F * (1 / r - 1) quand F = D). r = 0 (immobile) : tres loin.
	double DepthForScrollRatio(double r) const { return r <= 1e-6 ? 1e9 : _f / r - _d; }

	bool Project(double X, double Y, double Z, HdScreenPt& out) const override
	{
		if(!Valid()) return false;
		const double den = _d + Z;
		if(den < 1e-3) return false; // derriere la camera
		const double s = _f / den;
		const int ly = LineFor(Y, s);
		const size_t i = (size_t)(ly - _y0);
		out.x = _ox + (X - _cx[i]) * s;
		out.y = _oy + (Y - _cy[i]) * s;
		out.scale = s;
		return true;
	}

	bool Unproject(double sx, double sy, double depth, double& X, double& Y, double& Z) const override
	{
		if(!Valid()) return false;
		const int ly = (int)std::floor(sy);
		if(ly < _y0 || ly > _y1) return false;
		Z = std::isnan(depth) ? 0.0 : depth;
		const double den = _d + Z;
		if(den < 1e-3) return false;
		const double s = _f / den;
		const size_t i = (size_t)(ly - _y0);
		X = _cx[i] + (sx - _ox) / s;
		Y = _cy[i] + (sy - _oy) / s;
		return true;
	}

	double DepthOf(double, double, double Z) const override { return _d + Z; }
	void Band(int& y0, int& y1) const override { y0 = _y0; y1 = _y1; }
	double ScaleAt(double) const override { return _d > 0 ? _f / _d : 1.0; }
	bool HasGround() const override { return false; }

private:
	// Ligne d'arrivee d'un point de coordonnee monde Y a l'echelle s (point fixe, depart au milieu de la bande).
	int LineFor(double Y, double s) const
	{
		int ly = (_y0 + _y1) / 2;
		for(int it = 0; it < 4; it++) {
			const double y = _oy + (Y - _cy[(size_t)(ly - _y0)]) * s;
			int n = (int)std::floor(y);
			n = std::clamp(n, _y0, _y1);
			if(n == ly) break;
			ly = n;
		}
		return ly;
	}

	int _y0 = 0, _y1 = -1;
	double _ox = 128, _oy = 120, _f = 256, _d = 256;
	std::vector<double> _cx, _cy;
};

// Auto-test sans jeu ni GPU. Renvoie vrai si tout passe ; report = resume pour le journal.
inline bool HdLayerCameraSelfTest(std::string& report)
{
	int total = 0, bad = 0;
	double worst = 0;
	auto check = [&](bool ok, double err) { total++; if(!ok) bad++; worst = std::max(worst, err); };
	const double eps = 1e-6;

	// 1) Z = 0 : les pixels du jeu restent a leur place ; barre de statut a 0, jeu decale (un changement de defilement)
	HdLayerCamera split;
	split.Begin(0, 239, 128.0, 120.0, 256.0, 256.0);
	for(int y = 0; y < 240; y++) split.SetLine(y, y < 32 ? 0.0 : 300.0, y < 32 ? 0.0 : 5.0);
	const double pts[4][2] = { { 10, 10 }, { 400, 20 }, { 50, 100 }, { 700, 200 } }; // (X, Y) monde
	for(auto& p : pts) {
		HdScreenPt s;
		bool ok = split.Project(p[0], p[1], 0.0, s);
		const bool top = p[1] < 32;
		const double ex = p[0] - (top ? 0.0 : 300.0), ey = p[1] - (top ? 0.0 : 5.0);
		check(ok && std::abs(s.x - ex) < eps && std::abs(s.y - ey) < eps && std::abs(s.scale - 1.0) < eps, ok ? std::abs(s.x - ex) + std::abs(s.y - ey) : 1e9);
		double X, Y, Z;
		ok = ok && split.Unproject(s.x, s.y, 0.0, X, Y, Z);
		check(ok && std::abs(X - p[0]) < eps && std::abs(Y - p[1]) < eps, ok ? std::abs(X - p[0]) + std::abs(Y - p[1]) : 1e9);
	}

	// 2) Aller-retour Project / Unproject a plusieurs profondeurs (defilement uniforme)
	HdLayerCamera uni;
	uni.Begin(0, 239, 128.0, 120.0, 256.0, 256.0);
	for(int y = 0; y < 240; y++) uni.SetLine(y, 123.0, 45.0);
	const double zs[4] = { -100.0, 0.0, 100.0, 512.0 };
	for(double z : zs) {
		const double wx = 123.0 + 128.0 + 40.0, wy = 45.0 + 120.0 - 30.0; // pres du centre de l'ecran
		HdScreenPt s;
		bool ok = uni.Project(wx, wy, z, s);
		double X, Y, Z;
		ok = ok && uni.Unproject(s.x, s.y, z, X, Y, Z);
		check(ok && std::abs(X - wx) < eps && std::abs(Y - wy) < eps && std::abs(Z - z) < eps, ok ? std::abs(X - wx) + std::abs(Y - wy) : 1e9);
	}

	// 3) Parallaxe : une couche qui defile a 50 % est a Z = F ; 10 px de defilement du jeu = 5 px a l'ecran
	{
		HdLayerCamera a, b;
		a.Begin(0, 239, 128.0, 120.0, 256.0, 256.0);
		b.Begin(0, 239, 128.0, 120.0, 256.0, 256.0);
		for(int y = 0; y < 240; y++) { a.SetLine(y, 100.0, 0.0); b.SetLine(y, 110.0, 0.0); }
		const double z = a.DepthForScrollRatio(0.5);
		HdScreenPt pa, pb;
		const bool ok = a.Project(300.0, 120.0, z, pa) && b.Project(300.0, 120.0, z, pb);
		check(ok && std::abs(z - 256.0) < eps && std::abs((pa.x - pb.x) - 5.0) < eps, ok ? std::abs((pa.x - pb.x) - 5.0) : 1e9);
	}

	// 4) Derriere la camera : refuse
	{
		HdScreenPt s;
		check(!uni.Project(0, 0, -300.0, s), 0);
	}

	report = std::to_string(total) + " verifications, " + std::to_string(bad) + " en echec, ecart max " + std::to_string(worst);
	return bad == 0;
}
