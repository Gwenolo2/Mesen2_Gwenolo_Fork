#pragma once
// =====================================================================================
// CAMERA MODE 7 RECONSTRUITE (socle de <3DParticles> et du futur relief Mode 7)
// Voir docs/claude/10_Mode7_Camera_3D.md
//
// Entree : pour chaque ligne d'ecran, l'origine P (point de carte sous le pixel x = 0) et le
// pas S (texels de carte par pixel d'ecran vers la droite), captures par le PPU.
// Une "zone" = suite de lignes Mode 7 continues (Mario Kart 2 joueurs = 2 zones).
//
// Projection d'un point 3D (u, v = carte en texels, h = hauteur en texels) :
//  - dans la zone : on cherche la ligne dont le segment de sol passe par (u, v) -> exact,
//    meme si le jeu deforme sa perspective (Mario Kart) ;
//  - plus pres que la derniere ligne : modele stenope ajuste sur le bas de la zone ;
//  - hauteur : h texels = h x (pixels par texel a cette profondeur) vers le haut.
// Coordonnees d'ecran continues, en pixels SNES de l'image recadree :
//  ligne y = bord haut de la ligne y ; x = bord gauche (meme convention que le rendu Mode 7 HD).
// =====================================================================================
#include <vector>
#include <cmath>
#include <algorithm>

struct SnesHdM7CamLine
{
	bool Valid = false;
	bool Smoothed = false; // parametres deja lisses par le rendu Mode 7 HD
	bool LargeMap = false; // carte non repetee
	double Px = 0, Py = 0; // origine (texels)
	double Sx = 0, Sy = 0; // pas (texels / pixel)
};

struct SnesHdM7Zone
{
	int Y0 = 0, Y1 = 0;          // lignes Mode 7 (incluses)
	int AreaY0 = 0, AreaY1 = 0;  // bande d'ecran de cette vue (ciel compris), incluse
	bool Perspective = false;
	bool LargeMap = false;       // carte non repetee : hors de 0-1023 = hors carte (pas de repli)
	std::vector<double> Px, Py, Sx, Sy, Inv, Nx, Ny; // Inv = pixels par texel ; N = normale "vers le loin"

	// Stenope ajuste sur le bas de la zone
	bool HasPinhole = false;
	double Horizon = 0;   // ligne d'horizon (pixels SNES)
	double Focal = 0;     // focale (pixels)
	double Height = 0;    // hauteur de camera (texels)
	double AxisX = 128;   // colonne de l'axe optique
	double Cx = 0, Cy = 0;  // position de la camera sur la carte (texels, non repliee)
	double Fx = 0, Fy = -1; // direction du regard (unitaire)
	double Rx = 1, Ry = 0;  // droite de l'ecran (unitaire)

	int Count() const { return Y1 - Y0 + 1; }

	double DepthOf(double u, double v) const { return (u - Cx) * Fx + (v - Cy) * Fy; }

	bool Project(double u, double v, double h, double& sx, double& sy, double& ppt) const
	{
		const int n = Count();
		if(n < 2) return false;
		auto dist = [&](int i) { return (u - Px[i]) * Nx[i] + (v - Py[i]) * Ny[i]; };
		auto lat = [&](int i) {
			double m2 = Sx[i] * Sx[i] + Sy[i] * Sy[i];
			return ((u - Px[i]) * Sx[i] + (v - Py[i]) * Sy[i]) / m2;
		};
		const double dFar = dist(0), dNear = dist(n - 1);
		if(dFar >= 0) return false; // au-dela de la ligne la plus lointaine (pres de l'horizon)
		if(dNear < 0) {
			if(!HasPinhole) return false;
			double D = DepthOf(u, v);
			if(D < 1.0) return false;
			double L = (u - Cx) * Rx + (v - Cy) * Ry;
			ppt = Focal / D;
			sx = AxisX + L * ppt;
			sy = Horizon + (Height - h) * ppt;
			return true;
		}
		int lo = 0, hi = n - 1; // dist(lo) < 0 <= dist(hi)
		while(hi - lo > 1) {
			int mid = (lo + hi) >> 1;
			if(dist(mid) >= 0) hi = mid; else lo = mid;
		}
		const double da = dist(lo), db = dist(hi);
		const double t = (da - db) != 0 ? da / (da - db) : 0.0;
		const double xa = lat(lo), xb = lat(hi);
		sx = xa + (xb - xa) * t;
		ppt = Inv[lo] + (Inv[hi] - Inv[lo]) * t;
		sy = (double)(Y0 + lo) + t - h * ppt;
		return true;
	}

	// Pixels d'ecran par texel a la ligne (continue) sy
	double InvAt(double sy) const
	{
		double fy = std::clamp(sy - (double)Y0, 0.0, (double)(Count() - 1));
		int i = std::min((int)fy, Count() - 2);
		if(i < 0) return Inv[0];
		return Inv[i] + (Inv[i + 1] - Inv[i]) * (fy - i);
	}

	// Point de sol sous un point d'ecran (relief) ; faux hors zone
	bool Unproject(double sx, double sy, double& u, double& v) const
	{
		double fy = sy - (double)Y0;
		if(fy < 0 || fy > (double)(Count() - 1)) return false;
		int i = std::min((int)fy, Count() - 2);
		double t = fy - i;
		double px = Px[i] + (Px[i + 1] - Px[i]) * t, py = Py[i] + (Py[i + 1] - Py[i]) * t;
		double s0 = Sx[i] + (Sx[i + 1] - Sx[i]) * t, s1 = Sy[i] + (Sy[i + 1] - Sy[i]) * t;
		u = px + s0 * sx; v = py + s1 * sx;
		return true;
	}
};

// Regression lineaire y = a + b*x
inline bool SnesHdM7LinFit(const double* x, const double* y, int n, double& a, double& b)
{
	double sx = 0, sy = 0, sxx = 0, sxy = 0;
	for(int i = 0; i < n; i++) { sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; }
	double den = n * sxx - sx * sx;
	if(n < 2 || std::abs(den) < 1e-12) return false;
	b = (n * sxy - sx * sy) / den;
	a = (sy - b * sx) / n;
	return true;
}

inline void SnesHdBuildMode7Zones(const std::vector<SnesHdM7CamLine>& lines, double centerX, std::vector<SnesHdM7Zone>& zones)
{
	zones.clear();
	const int H = (int)lines.size();
	int y = 0;
	while(y < H) {
		if(!lines[y].Valid) { y++; continue; }
		int y1 = y;
		while(y1 + 1 < H && lines[y1 + 1].Valid) {
			const SnesHdM7CamLine& a = lines[y1];
			const SnesHdM7CamLine& b = lines[y1 + 1];
			double ma = std::sqrt(a.Sx * a.Sx + a.Sy * a.Sy), mb = std::sqrt(b.Sx * b.Sx + b.Sy * b.Sy);
			if(ma < 1e-6 || mb < 1e-6) break;
			double ratio = mb / ma;
			if(ratio < 0.6 || ratio > 1.67) break;
			double cosA = (a.Sx * b.Sx + a.Sy * b.Sy) / (ma * mb);
			if(cosA < 0.94) break; // rotation > ~20 degres entre deux lignes : autre vue
			double cax = a.Px + a.Sx * centerX, cay = a.Py + a.Sy * centerX;
			double cbx = b.Px + b.Sx * centerX, cby = b.Py + b.Sy * centerX;
			if(std::hypot(cbx - cax, cby - cay) > 512.0) break;
			y1++;
		}
		const int n = y1 - y + 1;
		if(n >= 8) {
			SnesHdM7Zone z;
			z.Y0 = y; z.Y1 = y1;
			z.Px.resize(n); z.Py.resize(n); z.Sx.resize(n); z.Sy.resize(n); z.Inv.resize(n); z.Nx.resize(n); z.Ny.resize(n);
			std::vector<double> rawInv(n);
			bool allSmoothed = true;
			for(int i = 0; i < n; i++) {
				const SnesHdM7CamLine& L = lines[y + i];
				z.Px[i] = L.Px; z.Py[i] = L.Py; z.Sx[i] = L.Sx; z.Sy[i] = L.Sy;
				rawInv[i] = 1.0 / std::sqrt(L.Sx * L.Sx + L.Sy * L.Sy);
				if(L.LargeMap) z.LargeMap = true;
				allSmoothed = allSmoothed && L.Smoothed;
			}
			// Lissage de l'echelle (arrondis 8.8 des registres A/C) : quadratique locale sur 9 lignes
			for(int i = 0; i < n; i++) {
				if(allSmoothed) { z.Inv[i] = rawInv[i]; continue; }
				int a = std::max(0, i - 4), b = std::min(n - 1, i + 4);
				double m[3][3] = { { 0 } }, r[3] = { 0 };
				for(int j = a; j <= b; j++) {
					double d = j - i, f[3] = { 1.0, d, d * d };
					for(int p = 0; p < 3; p++) { for(int q = 0; q < 3; q++) m[p][q] += f[p] * f[q]; r[p] += f[p] * rawInv[j]; }
				}
				// Gauss
				double A[3][4];
				for(int p = 0; p < 3; p++) { for(int q = 0; q < 3; q++) A[p][q] = m[p][q]; A[p][3] = r[p]; }
				bool ok = (b - a) >= 2;
				for(int c = 0; c < 3 && ok; c++) {
					int piv = c;
					for(int p = c + 1; p < 3; p++) if(std::abs(A[p][c]) > std::abs(A[piv][c])) piv = p;
					if(std::abs(A[piv][c]) < 1e-12) { ok = false; break; }
					if(piv != c) for(int k = 0; k < 4; k++) std::swap(A[c][k], A[piv][k]);
					for(int p = 0; p < 3; p++) {
						if(p == c) continue;
						double fct = A[p][c] / A[c][c];
						for(int k = c; k < 4; k++) A[p][k] -= fct * A[c][k];
					}
				}
				double c0 = ok ? A[0][3] / A[0][0] : rawInv[i];
				z.Inv[i] = (c0 > 0) ? c0 : rawInv[i];
				// le pas garde sa direction, sa longueur suit l'echelle lissee
				double k = rawInv[i] / z.Inv[i];
				z.Sx[i] *= k; z.Sy[i] *= k;
			}
			// Perspective : l'echelle varie nettement et les lignes du bas sont plus proches
			double invFar = z.Inv[0], invNear = z.Inv[n - 1];
			z.Perspective = invNear > invFar * 1.3;

			// Direction "vers le loin" : du centre de la ligne du bas vers celui de la ligne du haut
			double fcx = (z.Px[0] + z.Sx[0] * centerX) - (z.Px[n - 1] + z.Sx[n - 1] * centerX);
			double fcy = (z.Py[0] + z.Sy[0] * centerX) - (z.Py[n - 1] + z.Sy[n - 1] * centerX);
			for(int i = 0; i < n; i++) {
				double m = std::sqrt(z.Sx[i] * z.Sx[i] + z.Sy[i] * z.Sy[i]);
				double nx = -z.Sy[i] / m, ny = z.Sx[i] / m;
				if(nx * fcx + ny * fcy < 0) { nx = -nx; ny = -ny; }
				z.Nx[i] = nx; z.Ny[i] = ny;
			}

			if(z.Perspective) {
				// Stenope sur les 16 lignes du bas : Inv = (y - horizon) / hauteur ; D = focale / Inv
				int k0 = std::max(0, n - 16), kn = n - k0;
				std::vector<double> ly(kn), li(kn), lm(kn), lf(kn), lr(kn);
				double rx = z.Sx[n - 1], ry = z.Sy[n - 1];
				double rm = std::sqrt(rx * rx + ry * ry); rx /= rm; ry /= rm;
				double fx = z.Nx[n - 1], fy = z.Ny[n - 1];
				for(int j = 0; j < kn; j++) {
					int i = k0 + j;
					double cx = z.Px[i] + z.Sx[i] * centerX, cy = z.Py[i] + z.Sy[i] * centerX;
					ly[j] = (double)(z.Y0 + i);
					li[j] = z.Inv[i];
					lm[j] = 1.0 / z.Inv[i];          // texels par pixel
					lf[j] = cx * fx + cy * fy;       // position du centre le long du regard
					lr[j] = cx * rx + cy * ry;       // position laterale du centre
				}
				double a, b, fa, fb, ra, rb;
				if(SnesHdM7LinFit(ly.data(), li.data(), kn, a, b) && b > 1e-6 &&
					SnesHdM7LinFit(lm.data(), lf.data(), kn, fa, fb) && fb > 1e-3 &&
					SnesHdM7LinFit(lm.data(), lr.data(), kn, ra, rb)) {
					z.Horizon = -a / b;
					z.Height = 1.0 / b;
					z.Focal = fb;
					z.AxisX = centerX - rb;
					z.Fx = fx; z.Fy = fy; z.Rx = rx; z.Ry = ry;
					z.Cx = fa * fx + ra * rx;
					z.Cy = fa * fy + ra * ry;
					z.HasPinhole = z.Focal > 1.0 && z.Height > 0.0;
				}
			}
			zones.push_back(std::move(z));
		}
		y = y1 + 1;
	}
	// Bande d'ecran de chaque vue : de la fin de la vue precedente a la fin de celle-ci
	int prevEnd = -1;
	for(auto& z : zones) {
		z.AreaY0 = prevEnd + 1;
		z.AreaY1 = z.Y1;
		prevEnd = z.Y1;
	}
}
