#pragma once
// =====================================================================================
// ADAPTATEUR : camera Mode 7 reconstruite (SnesHdM7Zone) derriere l'interface commune HdCamera.
// Aucune formule changee : chaque methode appelle directement celle de la zone.
// Voir docs/claude/35_Diorama_Journal_Etapes_G.md (G1).
//
// Correspondance des axes (HasGround() = vrai) : (X, Y, Z) de l'interface = (u carte, hauteur, v carte) de la zone.
// La zone doit rester valide aussi longtemps que l'adaptateur (reference, pas de copie).
// =====================================================================================
#include "Shared/HdCamera.h"
#include "SNES/HdPacks/SnesHdMode7Camera.h"

class HdM7Camera : public HdCamera
{
public:
	explicit HdM7Camera(const SnesHdM7Zone& zone) : _z(zone) {}
	const SnesHdM7Zone& Zone() const { return _z; }

	bool Project(double X, double Y, double Z, HdScreenPt& out) const override
	{
		double sx = 0, sy = 0, ppt = 1;
		if(!_z.Project(X, Z, Y, sx, sy, ppt)) return false; // zone.Project(u, v, h)
		out.x = sx; out.y = sy; out.scale = ppt;
		return true;
	}

	// depth ignoree : le Mode 7 renvoie le point de sol de la ligne (hauteur 0)
	bool Unproject(double sx, double sy, double, double& X, double& Y, double& Z) const override
	{
		double u = 0, v = 0;
		if(!_z.Unproject(sx, sy, u, v)) return false;
		X = u; Y = 0.0; Z = v;
		return true;
	}

	double DepthOf(double X, double, double Z) const override { return _z.DepthOf(X, Z); }
	void Band(int& y0, int& y1) const override { y0 = _z.AreaY0; y1 = _z.AreaY1; }
	double ScaleAt(double sy) const override { return _z.InvAt(sy); }
	bool HasGround() const override { return true; }

private:
	const SnesHdM7Zone& _z;
};
