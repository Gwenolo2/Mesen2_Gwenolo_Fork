#include "pch.h"
#include <vector>
#include <algorithm>
#include <thread>
#include <cmath>
#include <cstdio>
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/HdPacks/SnesHdPackLoader.h"
#include "SNES/SnesConsole.h"
#include "SNES/SnesPpu.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"

#include "Shared/pl_mpeg.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/Audio/SoundMixer.h"


//-----------------------------------------------------------------------------------------
SnesHdVideoFilter::SnesHdVideoFilter(Emulator* emu) : SnesDefaultVideoFilter(emu) {}


//----------------------------------------------------------------------------------------

FrameInfo SnesHdVideoFilter::GetFrameInfo()
{
	static bool frameLog = false;
	/*if(!frameLog) {
		MessageManager::Log("[HD TRACE 2.5] GetFrameInfo a pris le relais pour l'affichage !");
		frameLog = true;
	}
	*/

	_sdFrameInfo = SnesDefaultVideoFilter::GetFrameInfo();
	FrameInfo info = _sdFrameInfo;

	if(SnesHdPackLoader::ActivePack) {
		uint32_t scale = SnesHdPackLoader::ActivePack->Scale;
		bool stretch = SnesHdPackLoader::ActivePack->OverscanStretch;

		if(SnesHdPackLoader::ActivePack->HasOverscanConfig && !stretch) {
			// Mode No-Stretch : we keep native geometry 
			_sdFrameInfo = _baseFrameInfo;
			info.Width = _baseFrameInfo.Width * scale;
			info.Height = _baseFrameInfo.Height * scale;
		} else {
			info.Width *= scale;
			info.Height *= scale;
		}
	}
	return info;
}
//-------------------------------------------------------------------------

// ============================================================================
// IMPLEMENTATION DE LA REGLE DE Z-INDEX
// ============================================================================
bool SnesHdLayerOrderRule::MatchesCondition(int x, int y, SnesHdPixelData* tile)
{
	for(auto cond : Conditions) {
		if(!cond->CheckCondition(x, y, tile)) {
			return false;
		}
	}
	return true;
}


//---------------------------------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------------------------------
// MODE 7 HD : chaine de mipmaps COMPLETE (jusqu'a 1x1), construite une seule fois par image.
// Convention : le niveau 0 est Bitmap->PixelData (pas de copie, l'image peut faire 1 Go en scale 16) ;
// Mipmaps[0] reste vide, Mipmaps[i] = niveau i (dimension mapDim >> i).
// Reduction 2x2 ponderee par l'alpha (evite les franges sombres autour des zones transparentes).
//---------------------------------------------------------------------------------------------------------------
static void BuildMode7MipChain(SnesHdPackBitmapInfo* bmp, uint32_t mapDim)
{
	if(!bmp->Mipmaps.empty()) return;
	bmp->Mipmaps.emplace_back(); // niveau 0 = PixelData

	if(bmp->Width != mapDim || bmp->Height != mapDim || (mapDim & (mapDim - 1)) != 0 ||
	   bmp->PixelData.size() < (size_t)mapDim * mapDim) {
		MessageManager::Log("[HD-MODE7] Image " + bmp->PngName + " : dimensions inattendues, mipmaps desactives.");
		return;
	}

	uint32_t prevDim = mapDim;
	const uint32_t* prev = bmp->PixelData.data();
	while(prevDim > 1) {
		uint32_t dim = prevDim / 2;
		std::vector<uint32_t> mip((size_t)dim * dim, 0);
		for(uint32_t my = 0; my < dim; my++) {
			const uint32_t* row0 = prev + (size_t)(my * 2) * prevDim;
			const uint32_t* row1 = row0 + prevDim;
			for(uint32_t mx = 0; mx < dim; mx++) {
				uint32_t p[4] = { row0[mx * 2], row0[mx * 2 + 1], row1[mx * 2], row1[mx * 2 + 1] };
				uint32_t sumA = 0, r = 0, g = 0, b = 0;
				for(uint32_t q : p) {
					uint32_t a = q >> 24;
					sumA += a;
					r += ((q >> 16) & 0xFF) * a;
					g += ((q >> 8) & 0xFF) * a;
					b += (q & 0xFF) * a;
				}
				if(sumA > 0) {
					mip[(size_t)my * dim + mx] = ((sumA / 4) << 24) | ((r / sumA) << 16) | ((g / sumA) << 8) | (b / sumA);
				}
			}
		}
		bmp->Mipmaps.push_back(std::move(mip));
		prev = bmp->Mipmaps.back().data();
		prevDim = dim;
	}
	MessageManager::Log("[HD-MODE7] " + bmp->PngName + " : " + std::to_string(bmp->Mipmaps.size()) + " niveaux de mipmap.");
}

//---------------------------------------------------------------------------------------------------------------
// MODE 7 HD : mipmaps d'une tile individuelle (<m7tiles>). La tile (8*Scale pixels de cote, a la
// position X,Y de son atlas) est extraite puis reduite par 2 jusqu'a 1x1. Construit une seule fois.
//---------------------------------------------------------------------------------------------------------------
static void BuildMode7TileMips(SnesHdMode7Image* img)
{
	if(!img || !img->TileMips.empty() || !img->Bitmap || img->Bitmap->PixelData.empty()) return;
	const SnesHdPackBitmapInfo* bmp = img->Bitmap;
	uint32_t dim = 8 * img->Scale;
	std::vector<uint32_t> lvl((size_t)dim * dim, 0);
	for(uint32_t y = 0; y < dim; y++) {
		uint32_t sy = (uint32_t)img->Y + y;
		if(sy >= bmp->Height) continue;
		for(uint32_t x = 0; x < dim; x++) {
			uint32_t sx = (uint32_t)img->X + x;
			if(sx < bmp->Width) lvl[(size_t)y * dim + x] = bmp->PixelData[(size_t)sy * bmp->Width + sx];
		}
	}
	img->TileMips.push_back(std::move(lvl));
	while(dim > 1 && (dim % 2) == 0) {
		uint32_t nd = dim / 2;
		const std::vector<uint32_t>& prev = img->TileMips.back();
		std::vector<uint32_t> mip((size_t)nd * nd, 0);
		for(uint32_t my = 0; my < nd; my++) {
			for(uint32_t mx = 0; mx < nd; mx++) {
				uint32_t p[4] = { prev[(size_t)(my * 2) * dim + mx * 2], prev[(size_t)(my * 2) * dim + mx * 2 + 1],
									  prev[(size_t)(my * 2 + 1) * dim + mx * 2], prev[(size_t)(my * 2 + 1) * dim + mx * 2 + 1] };
				uint32_t sumA = 0, r = 0, g = 0, b = 0;
				for(uint32_t q : p) { uint32_t a = q >> 24; sumA += a; r += ((q >> 16) & 0xFF) * a; g += ((q >> 8) & 0xFF) * a; b += (q & 0xFF) * a; }
				if(sumA > 0) mip[(size_t)my * nd + mx] = ((sumA / 4) << 24) | ((r / sumA) << 16) | ((g / sumA) << 8) | (b / sumA);
			}
		}
		img->TileMips.push_back(std::move(mip));
		dim = nd;
	}
}

void SnesHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	static bool filterLog = false;

	// Structure pour mémoriser TOUTES les tuiles HD à "déplier"
	struct ExpandedTile
	{
		SnesHdPackTileInfo* Tile;
		int startX;
		int startY;
		uint8_t zIndex;
		bool hFlip;
		bool vFlip;
		uint16_t baseColor;
		uint8_t oamIndex;
	};
	std::vector<ExpandedTile> tilesToExpand;

	if(!SnesHdPackLoader::ActivePack) {
		SnesDefaultVideoFilter::ApplyFilter(ppuOutputBuffer);
		return;
	}

	uint32_t scale = SnesHdPackLoader::ActivePack->Scale;
	uint32_t* out = GetOutputBuffer();


	OverscanDimensions overscan = GetOverscan();
	bool stretch = SnesHdPackLoader::ActivePack->OverscanStretch;

	// Dimensions de la zone active utile (tronquée par l'overscan)
	uint32_t cropWidth = _baseFrameInfo.Width - overscan.Left - overscan.Right;
	uint32_t cropHeight = _baseFrameInfo.Height - overscan.Top - overscan.Bottom;

	// Dimensions du tampon de sortie selon le mode Stretch
	uint32_t outWidth = stretch ? (cropWidth * scale) : (_baseFrameInfo.Width * scale);
	uint32_t hdHeight = stretch ? (cropHeight * scale) : (_baseFrameInfo.Height * scale);

	// Décalages d'écriture (destination) et de lecture (source)
	uint32_t dstX = stretch ? 0 : overscan.Left;
	uint32_t dstY = stretch ? 0 : overscan.Top;
	uint32_t srcX = overscan.Left;
	uint32_t srcY = overscan.Top;

	// Décalage pour l'alignement des sprites et additions
	int sprOffsetX = stretch ? -(int)overscan.Left : 0;
	int sprOffsetY = stretch ? -(int)overscan.Top : 0;

	SnesConsole* console = (SnesConsole*)_emu->GetConsole().get();
	SnesHdPixelData* hdBuffer = console->GetPpu()->GetHdScreenBuffer();
	SnesHdPixelData* hdSubBuffer = console->GetPpu()->GetHdSubScreenOutputBuffer();
	SnesHdPixelData* hdBehindBuffer = console->GetPpu()->GetHdBehindScreenBuffer();
	// Index des queues (sprites + additions) fige par SnesPpu::SendFrame, lu UNE fois ici
	// en meme temps que les buffers HD pour que tout corresponde a la meme frame.
	int hdReadIndex = SnesHdPackLoader::ActivePack->ActiveSpriteQueueReadIndex;

	if(_zBuffer.size() != outWidth * hdHeight) {
		_zBuffer.resize(outWidth * hdHeight);
	}
	memset(_zBuffer.data(), 0, _zBuffer.size()); // Remise à zéro du Z-Buffer
	memset(out, 0, outWidth * hdHeight * sizeof(uint32_t)); // Remise à zéro du tampon de couleur HD

	// ====================================================================
	// STEP 1: PRE-PASS MODE 7 (OPTIMIZED BSNES-HD PERSPECTIVE & FAST PATH)
	// ====================================================================
	struct SdColorCache
	{
		uint32_t color;
		int diffR;
		int diffG;
		int diffB;
	};


	// ====================================================================
	// MODE 7 HD - TILES INDIVIDUELLES (<m7tiles>) VISIBLES CETTE FRAME
	// Carte Mode 7 = grille de 128 x 128 cases de 8x8 texels. On releve, a partir des pixels
	// SNES ou la console affiche une tile HD (buffers main et sub), quelle tile occupe quelle
	// case. Le rendu HD de la piste echantillonne ensuite ces tiles a la resolution HD
	// (contours nets, perspective lissee, mipmaps) au lieu de les dessiner bloc par bloc.
	// ====================================================================
	std::vector<SnesHdMode7Image*> m7CellTile;
	SnesHdMode7Image* m7OutsideTile = nullptr; // tile affichee hors de la carte (Mode 7 "fill with tile 0")
	bool anyM7Cell = false;
	{
		std::unordered_map<uint32_t, SnesHdMode7Image*> imgByHash;
		auto scanTiles = [&](const SnesHdPixelData* buf) {
			if(!buf) return;
			for(uint32_t yy = 0; yy < cropHeight; yy++) {
				const SnesHdPixelData* row = buf + (yy + srcY) * _baseFrameInfo.Width + srcX;
				for(uint32_t xx = 0; xx < cropWidth; xx++) {
					const SnesHdPixelData& p = row[xx];
					if(!p.IsMode7Tile) continue;
					SnesHdMode7Image* img;
					auto it = imgByHash.find(p.M7TileHash);
					if(it != imgByHash.end()) {
						img = it->second;
					} else {
						img = SnesHdPackLoader::ActivePack->GetMode7Image(p.M7TileHash);
						BuildMode7TileMips(img);
						imgByHash[p.M7TileHash] = img;
					}
					if(!img || img->TileMips.empty()) continue;
					int32_t mx = p.M7XValue >> 8, my = p.M7YValue >> 8;
					if(m7CellTile.empty()) m7CellTile.assign(128 * 128, nullptr);
					if(mx < 0 || my < 0 || mx > 1023 || my > 1023) {
						m7OutsideTile = img;
					} else {
						m7CellTile[(size_t)(my >> 3) * 128 + (mx >> 3)] = img;
					}
					anyM7Cell = true;
				}
			}
		};
		scanTiles(hdBuffer);
		scanTiles(hdSubBuffer);
	}

	// ====================================================================
	// MODE 7 HD - PHASE 1 (sequentielle) : preparation de chaque ligne
	// (ligne de reference, image HD selon les conditions, mipmaps, derivee verticale)
	// ====================================================================
	struct M7Line
	{
		SnesHdMode7Image* Img = nullptr;
		SnesHdPixelData Ref;
		SnesHdPixelData Next;
		uint32_t RefX = 0;
		uint32_t NextRefX = 0;
		bool HasNext = false;
		float Dy[4] = { 0, 0, 0, 0 }; // ecart ligne SNES suivante : d0x, d0y, gradX, gradY (8.8, texels SNES)
		bool UseFit = false;           // parametres lisses actifs sur cette ligne
		bool FitFirst = false, FitLast = false; // premiere / derniere ligne de sa zone
		double Inv = 0;                // 1/echelle lissee
		double DirX = 1, DirY = 0;     // direction lissee du pas
		double U[2] = { 0 }, V[2] = { 0 }, W[2] = { 0 }; // origine locale : P = U + V*s + W*(l - ligne)
	};
	std::vector<M7Line> m7Lines(cropHeight);
	bool anyMode7 = false;
	{
		bool haveLastDy = false;
		float lastDy[4] = { 0, 0, 0, 0 };
		for(uint32_t y = 0; y < cropHeight; y++) {
			uint32_t snesY = y + srcY;
			M7Line& L = m7Lines[y];

			bool found = false;
			for(uint32_t tx = 0; tx < cropWidth; tx++) {
				uint32_t offset = snesY * _baseFrameInfo.Width + tx + srcX;
				if(hdBuffer[offset].IsMode7) { L.Ref = hdBuffer[offset]; L.RefX = tx; found = true; break; }
			}
			if(!found) continue;

			if(snesY + 1 < _baseFrameInfo.Height) {
				for(uint32_t tx = 0; tx < cropWidth; tx++) {
					uint32_t offset = (snesY + 1) * _baseFrameInfo.Width + tx + srcX;
					if(hdBuffer[offset].IsMode7) { L.Next = hdBuffer[offset]; L.NextRefX = tx; L.HasNext = true; break; }
				}
			}
			if(!L.HasNext) { L.Next = L.Ref; L.NextRefX = L.RefX; }

			SnesHdMode7Image* img = SnesHdPackLoader::ActivePack->GetMode7Image(L.Ref.M7MapHash);
			if(!img || !img->Bitmap || img->Bitmap->PixelData.empty()) continue;
			BuildMode7MipChain(img->Bitmap, 1024 * img->Scale);
			L.Img = img;
			anyMode7 = true;

			int32_t sX = L.Ref.M7XValue - (L.Ref.M7A * (int32_t)L.RefX);
			int32_t sY = L.Ref.M7YValue - (L.Ref.M7C * (int32_t)L.RefX);
			int32_t nX = L.Next.M7XValue - (L.Next.M7A * (int32_t)L.NextRefX);
			int32_t nY = L.Next.M7YValue - (L.Next.M7C * (int32_t)L.NextRefX);
			float d0x = (float)(nX - sX), d0y = (float)(nY - sY);
			float gx = (float)(L.Next.M7A - L.Ref.M7A), gy = (float)(L.Next.M7C - L.Ref.M7C);
			float lenSd = std::sqrt(d0x * d0x + d0y * d0y) / 256.0f;
			bool discontinuity = !L.HasNext || L.Next.M7MapHash != L.Ref.M7MapHash || lenSd > 256.0f;
			if(discontinuity) {
				if(haveLastDy) { d0x = lastDy[0]; d0y = lastDy[1]; gx = lastDy[2]; gy = lastDy[3]; }
			} else {
				lastDy[0] = d0x; lastDy[1] = d0y; lastDy[2] = gx; lastDy[3] = gy; haveLastDy = true;
			}
			L.Dy[0] = d0x; L.Dy[1] = d0y; L.Dy[2] = gx; L.Dy[3] = gy;
		}

		// ------------------------------------------------------------------
		// LISSAGE DES PARAMETRES MODE 7 (anti-ondulation des droites)
		// Le jeu calcule A/C et l'origine de chaque ligne avec des arrondis : d'une ligne a
		// l'autre ces petites erreurs font onduler les droites en HD. Pour chaque ligne, on
		// ajuste localement (fenetre de 9 lignes) :
		//   - 1/echelle : polynome de degre 2 (Savitzky-Golay)  -> suit la perspective du jeu
		//   - direction du pas : moyenne des directions
		//   - origine : P = U + V*s + W*(l - lc)                 -> relation d'un plan en perspective
		// Aucune hypothese globale sur la camera (Mario Kart deforme volontairement sa perspective).
		// Zone conservee en valeurs brutes si une ligne s'ecarte trop des valeurs du jeu.
		// ------------------------------------------------------------------
		auto solve3 = [](double M[3][3], const double B[3], double X[3]) -> bool {
			double a[3][4];
			for(int r = 0; r < 3; r++) { for(int c = 0; c < 3; c++) a[r][c] = M[r][c]; a[r][3] = B[r]; }
			for(int c = 0; c < 3; c++) {
				int piv = c;
				for(int r = c + 1; r < 3; r++) if(std::abs(a[r][c]) > std::abs(a[piv][c])) piv = r;
				if(std::abs(a[piv][c]) < 1e-12) return false;
				if(piv != c) for(int k = 0; k < 4; k++) std::swap(a[c][k], a[piv][k]);
				for(int r = 0; r < 3; r++) {
					if(r == c) continue;
					double fct = a[r][c] / a[c][c];
					for(int k = c; k < 4; k++) a[r][k] -= fct * a[c][k];
				}
			}
			for(int r = 0; r < 3; r++) X[r] = a[r][3] / a[r][r];
			return true;
		};

		std::string fitReport;   // ex: "[lignes 35-113 : CORRIGEE (...)]"
		std::string fitPattern;  // idem sans les valeurs (journal uniquement aux changements)
		uint32_t y0 = 0;
		while(y0 < cropHeight) {
			if(!m7Lines[y0].Img) { y0++; continue; }
			uint32_t y1 = y0 + 1;
			while(y1 < cropHeight && m7Lines[y1].Img == m7Lines[y0].Img &&
					m7Lines[y1].Ref.M7MapHash == m7Lines[y0].Ref.M7MapHash && m7Lines[y1 - 1].HasNext) {
				y1++;
			}
			const int n = (int)(y1 - y0);
			std::string verdict = "trop courte";
			double maxErrP = 0, maxErrS = 0;
			if(n >= 5) {
				verdict = "rejetee";
				std::vector<double> l(n), sx(n), sy(n), px(n), py(n), inv(n), invS(n), s(n), dX(n), dY(n), U0(n), U1(n), V0(n), V1(n), W0(n), W1(n);
				std::vector<char> linePass(n, 0);
				bool ok = true;
				for(int i = 0; i < n && ok; i++) {
					const M7Line& L = m7Lines[y0 + i];
					l[i] = (double)(y0 + i);
					sx[i] = (double)L.Ref.M7A;
					sy[i] = (double)L.Ref.M7C;
					px[i] = (double)L.Ref.M7XValue - (double)L.Ref.M7A * (double)L.RefX;
					py[i] = (double)L.Ref.M7YValue - (double)L.Ref.M7C * (double)L.RefX;
					double mag = std::sqrt(sx[i] * sx[i] + sy[i] * sy[i]);
					if(mag < 1.0) { ok = false; break; }
					inv[i] = 1.0 / mag;
				}
				const int halfW = 4;
				for(int i = 0; i < n && ok; i++) {
					int a = std::max(0, i - halfW), b = std::min(n - 1, i + halfW);
					// 1/echelle : regression quadratique centree sur la ligne i
					double m[3][3] = { { 0 } }, rhs[3] = { 0 }, c[3];
					double ux = 0, uy = 0;
					for(int j = a; j <= b; j++) {
						double d = l[j] - l[i];
						double f[3] = { 1.0, d, d * d };
						for(int r = 0; r < 3; r++) { for(int q = 0; q < 3; q++) m[r][q] += f[r] * f[q]; rhs[r] += f[r] * inv[j]; }
						double mg = 1.0 / inv[j];
						ux += sx[j] / mg; uy += sy[j] / mg;
					}
					invS[i] = ((b - a) >= 2 && solve3(m, rhs, c) && c[0] > 0) ? c[0] : inv[i];
					double un = std::sqrt(ux * ux + uy * uy);
					if(un < 1e-9) { ok = false; break; }
					dX[i] = ux / un; dY[i] = uy / un;
					s[i] = 1.0 / invS[i];
				}
				for(int i = 0; i < n && ok; i++) {
					int a = std::max(0, i - halfW), b = std::min(n - 1, i + halfW);
					// Origine : P = a0 + a1*(s - s_i) + a2*(l - l_i)
					double m[3][3] = { { 0 } }, bxv[3] = { 0 }, byv[3] = { 0 }, cx[3], cy[3];
					for(int j = a; j <= b; j++) {
						double f[3] = { 1.0, s[j] - s[i], l[j] - l[i] };
						for(int r = 0; r < 3; r++) { for(int q = 0; q < 3; q++) m[r][q] += f[r] * f[q]; bxv[r] += f[r] * px[j]; byv[r] += f[r] * py[j]; }
					}
					if(solve3(m, bxv, cx) && solve3(m, byv, cy)) {
						U0[i] = cx[0] - cx[1] * s[i]; V0[i] = cx[1]; W0[i] = cx[2];
						U1[i] = cy[0] - cy[1] * s[i]; V1[i] = cy[1]; W1[i] = cy[2];
					} else {
						// Echelle constante sur la fenetre (carte plane) : P = a0 + a2*(l - l_i)
						double sw = 0, sl = 0, sll = 0, spx = 0, spy = 0, slpx = 0, slpy = 0;
						for(int j = a; j <= b; j++) {
							double d = l[j] - l[i];
							sw += 1; sl += d; sll += d * d; spx += px[j]; spy += py[j]; slpx += d * px[j]; slpy += d * py[j];
						}
						double den = sw * sll - sl * sl;
						if(std::abs(den) < 1e-9) { ok = false; break; }
						W0[i] = (sw * slpx - sl * spx) / den; U0[i] = (spx - W0[i] * sl) / sw; V0[i] = 0;
						W1[i] = (sw * slpy - sl * spy) / den; U1[i] = (spy - W1[i] * sl) / sw; V1[i] = 0;
					}
					// Verification par rapport aux valeurs du jeu
					double fx = U0[i] + V0[i] * s[i], fy = U1[i] + V1[i] * s[i];
					double errP = std::sqrt((fx - px[i]) * (fx - px[i]) + (fy - py[i]) * (fy - py[i])) / 256.0;
					double fsx = dX[i] * s[i], fsy = dY[i] * s[i];
					double magS = 1.0 / inv[i];
					double errS = std::sqrt((fsx - sx[i]) * (fsx - sx[i]) + (fsy - sy[i]) * (fsy - sy[i])) / magS;
					// Ecart de position exprime en PIXELS d'ecran (pres de l'horizon, 1 pixel = beaucoup de texels)
					double errPix = errP / std::max(magS / 256.0, 1e-6);
					linePass[i] = (errPix <= 1.0 && errS <= 0.04) ? 1 : 0;
					maxErrP = std::max(maxErrP, errPix); maxErrS = std::max(maxErrS, errS);
				}
				if(ok) {
					int passed = 0;
					for(int i = 0; i < n; i++) passed += linePass[i];
					verdict = (passed == n) ? std::string("CORRIGEE") :
						("CORRIGEE " + std::to_string(passed) + "/" + std::to_string(n) + " lignes");
					if(passed == 0) verdict = "rejetee";
					for(int i = 0; i < n; i++) {
						M7Line& L = m7Lines[y0 + i];
						if(!linePass[i]) continue; // cette ligne garde les valeurs brutes du jeu
						L.UseFit = true;
						L.Inv = invS[i]; L.DirX = dX[i]; L.DirY = dY[i];
						L.U[0] = U0[i]; L.V[0] = V0[i]; L.W[0] = W0[i];
						L.U[1] = U1[i]; L.V[1] = V1[i]; L.W[1] = W1[i];
						L.FitFirst = (i == 0);
						L.FitLast = (i == n - 1);
					}
				}
			}
			fitPattern += "[" + std::to_string(y0) + "-" + std::to_string(y1 - 1) + " " + verdict.substr(0, 8) + "] ";
			char buf[96];
			snprintf(buf, sizeof(buf), " (ecart max %.2f pixel, %.1f%%)", maxErrP, maxErrS * 100.0);
			fitReport += "[lignes " + std::to_string(y0) + "-" + std::to_string(y1 - 1) + " : " + verdict + (n >= 5 ? std::string(buf) : std::string()) + "] ";
			y0 = y1;
		}
		{
			static std::string s_lastFitPattern;
			if(fitPattern != s_lastFitPattern) {
				s_lastFitPattern = fitPattern;
				if(!fitPattern.empty()) MessageManager::Log("[HD-MODE7] Lissage perspective : " + fitReport);
			}
		}
	}

	// ====================================================================
	// MODE 7 HD - PHASE 2 (parallele) : rendu d'une ligne SNES (= 'scale' lignes HD)
	// Filtrage : trilineaire ('bilinear') ou anisotrope ('anisotropic', jusqu'a 8 echantillons).
	// Unites : positions et pas en 8.8, en texels du niveau 0.
	// ====================================================================
	auto renderMode7Line = [&](uint32_t y) {
		const M7Line& L = m7Lines[y];
		const SnesHdPixelData& refM7Data = L.Ref;
		const SnesHdPixelData& nextRefM7Data = L.Next;
		SnesHdMode7Image* m7Img = L.Img;
		uint32_t snesY = y + srcY;
		uint32_t outY = y + dstY;

		SdColorCache sdColorRow[256];
		uint32_t outScale = scale;
		uint32_t imgScale = m7Img->Scale;
		uint32_t mapDim = 1024 * imgScale;

		bool applyColorMath = !m7Img->IgnoreColorMath;
		uint8_t pixelZ = 20 + (refM7Data.SnesPriorityLevel * 4);
		if(refM7Data.IsHighPriority) pixelZ += 2;
		uint32_t baseSdColor = _calculatedPalette[refM7Data.BaseColor];

		for(uint32_t x = 0; x < cropWidth; x++) {
			uint32_t currentOffset = snesY * _baseFrameInfo.Width + x + srcX;
			uint32_t sdColor = SnesHdPackLoader::ActivePack->IsColorMathDisabled
				? baseSdColor
				: _calculatedPalette[ppuOutputBuffer[currentOffset]];
			sdColorRow[x].color = sdColor;
			// Color math PAR PIXEL (couleur SD finale - couleur Mode 7 brute de ce pixel),
			// seulement si le Mode 7 est au premier plan (pas sous un sprite).
			const SnesHdPixelData& m7Px = hdBuffer[currentOffset];
			if(applyColorMath && (m7Px.IsMode7 || m7Px.IsMode7Tile) && !SnesHdPackLoader::ActivePack->IsColorMathDisabled) {
				uint32_t pxBase = _calculatedPalette[m7Px.BaseColor];
				sdColorRow[x].diffR = ((sdColor >> 16) & 0xFF) - (int)((pxBase >> 16) & 0xFF);
				sdColorRow[x].diffG = ((sdColor >> 8) & 0xFF) - (int)((pxBase >> 8) & 0xFF);
				sdColorRow[x].diffB = (sdColor & 0xFF) - (int)(pxBase & 0xFF);
			} else {
				sdColorRow[x].diffR = sdColorRow[x].diffG = sdColorRow[x].diffB = 0;
			}
		}

		const int numLevels = std::min((int)m7Img->Bitmap->Mipmaps.size(), 32);
		const uint32_t* levelPx[32];
		for(int lv = 0; lv < numLevels; lv++) {
			levelPx[lv] = (lv == 0) ? m7Img->Bitmap->PixelData.data() : m7Img->Bitmap->Mipmaps[lv].data();
		}
		const bool largeMap = refM7Data.M7LargeMap;
		const Mode7FilterMode filterMode = m7Img->FilterMode;
		static constexpr int kMaxAniso = 8;
		// Biais de niveau de detail (< 0 = plus net). -0.5 : compromis nettete / scintillement.
		static constexpr float kLodBias = -0.5f;

		auto addBilinear = [&](int32_t sX, int32_t sY, int lv, float w, float& aR, float& aG, float& aB, float& aA) {
			const uint32_t dim = mapDim >> lv;
			const uint32_t mask = dim - 1;
			const uint32_t* px = levelPx[lv];
			int32_t lX = (sX >> lv) - 128; // -128 : centre des texels
			int32_t lY = (sY >> lv) - 128;
			int32_t ix = lX >> 8, iy = lY >> 8;
			float fx = (lX & 0xFF) * (1.0f / 256.0f);
			float fy = (lY & 0xFF) * (1.0f / 256.0f);
			const float wq[4] = { (1 - fx) * (1 - fy) * w, fx * (1 - fy) * w, (1 - fx) * fy * w, fx * fy * w };
			const int32_t qx[4] = { ix, ix + 1, ix, ix + 1 };
			const int32_t qy[4] = { iy, iy, iy + 1, iy + 1 };
			for(int q = 0; q < 4; q++) {
				if(wq[q] <= 0.0f) continue;
				uint32_t c;
				if(largeMap) {
					if(qx[q] < 0 || qy[q] < 0 || qx[q] >= (int32_t)dim || qy[q] >= (int32_t)dim) continue;
					c = px[(size_t)qy[q] * dim + qx[q]];
				} else {
					c = px[(size_t)(qy[q] & mask) * dim + (qx[q] & mask)];
				}
				float a = (float)(c >> 24) * wq[q];
				if(a <= 0.0f) continue;
				aA += a;
				aR += ((c >> 16) & 0xFF) * a;
				aG += ((c >> 8) & 0xFF) * a;
				aB += (c & 0xFF) * a;
			}
		};

		// Parametres lisses a la position continue (ligne y, fraction tt dans [0,1[) :
		// pas S (8.8 texels SNES / pixel SNES), origine P, et leurs derivees par ligne SNES.
		auto evalSmooth = [&](double tt, double& Sx, double& Sy, double& Px, double& Py,
									 double& dPx, double& dPy, double& dSx, double& dSy) {
			const M7Line& A = L;
			const bool hasB = !A.FitLast && (y + 1 < cropHeight) && m7Lines[y + 1].UseFit;
			const M7Line& B = hasB ? m7Lines[y + 1] : A;
			double invA = A.Inv, invB;
			if(hasB) invB = B.Inv;
			else if(!A.FitFirst && y > 0 && m7Lines[y - 1].UseFit) invB = 2.0 * invA - m7Lines[y - 1].Inv;
			else invB = invA;
			double iv = invA + tt * (invB - invA);
			if(iv <= 1e-9) iv = invA;
			double sc = 1.0 / iv;
			double ds = -(invB - invA) * sc * sc;
			double dx = A.DirX + tt * (B.DirX - A.DirX), dy = A.DirY + tt * (B.DirY - A.DirY);
			double dn = std::sqrt(dx * dx + dy * dy); if(dn > 1e-9) { dx /= dn; dy /= dn; }
			Sx = dx * sc; Sy = dy * sc; dSx = dx * ds; dSy = dy * ds;
			double pAx = A.U[0] + A.V[0] * sc + A.W[0] * tt, pAy = A.U[1] + A.V[1] * sc + A.W[1] * tt;
			double dAx = A.V[0] * ds + A.W[0], dAy = A.V[1] * ds + A.W[1];
			if(hasB) {
				double pBx = B.U[0] + B.V[0] * sc + B.W[0] * (tt - 1.0), pBy = B.U[1] + B.V[1] * sc + B.W[1] * (tt - 1.0);
				double dBx = B.V[0] * ds + B.W[0], dBy = B.V[1] * ds + B.W[1];
				Px = (1.0 - tt) * pAx + tt * pBx; Py = (1.0 - tt) * pAy + tt * pBy;
				dPx = (1.0 - tt) * dAx + tt * dBx + (pBx - pAx);
				dPy = (1.0 - tt) * dAy + tt * dBy + (pBy - pAy);
			} else {
				Px = pAx; Py = pAy; dPx = dAx; dPy = dAy;
			}
		};

		// Derivee verticale par ligne HD (texels niveau 0, 8.8)
		const float k = (float)imgScale / (float)outScale;
		const float rawDy0X = L.Dy[0] * k, rawDy0Y = L.Dy[1] * k, rawDyGradX = L.Dy[2] * k, rawDyGradY = L.Dy[3] * k;

		int32_t startX_y = refM7Data.M7XValue - (refM7Data.M7A * (int32_t)L.RefX);
		int32_t startY_y = refM7Data.M7YValue - (refM7Data.M7C * (int32_t)L.RefX);
		int32_t startX_next = nextRefM7Data.M7XValue - (nextRefM7Data.M7A * (int32_t)L.NextRefX);
		int32_t startY_next = nextRefM7Data.M7YValue - (nextRefM7Data.M7C * (int32_t)L.NextRefX);

		for(uint32_t sy = 0; sy < outScale; sy++) {
			double t = ((double)sy + 0.5) / (double)outScale; // centre de la ligne HD

			double curM7A_d = refM7Data.M7A;
			if(refM7Data.M7A != nextRefM7Data.M7A && refM7Data.M7A > 0 && nextRefM7Data.M7A > 0) {
				double invA1 = 1.0 / (double)refM7Data.M7A;
				double invA2 = 1.0 / (double)nextRefM7Data.M7A;
				curM7A_d = 1.0 / (invA1 + t * (invA2 - invA1));
			} else {
				curM7A_d = (double)refM7Data.M7A + t * ((double)nextRefM7Data.M7A - (double)refM7Data.M7A);
			}
			double curM7C_d = refM7Data.M7C;
			if(refM7Data.M7C != nextRefM7Data.M7C && refM7Data.M7C != 0 && nextRefM7Data.M7C != 0) {
				double invC1 = 1.0 / (double)refM7Data.M7C;
				double invC2 = 1.0 / (double)nextRefM7Data.M7C;
				curM7C_d = 1.0 / (invC1 + t * (invC2 - invC1));
			} else {
				curM7C_d = (double)refM7Data.M7C + t * ((double)nextRefM7Data.M7C - (double)refM7Data.M7C);
			}

			int32_t lineStartX = startX_y + (int32_t)std::round(t * (startX_next - startX_y));
			int32_t lineStartY = startY_y + (int32_t)std::round(t * (startY_next - startY_y));
			int32_t stepX = (int32_t)std::round((curM7A_d * (double)imgScale) / (double)outScale);
			int32_t stepY = (int32_t)std::round((curM7C_d * (double)imgScale) / (double)outScale);

			uint32_t hdRowIdx = (outY * outScale + sy) * outWidth;
			int32_t exactX = lineStartX * (int32_t)imgScale + (stepX >> 1); // centre du 1er pixel HD
			int32_t exactY = lineStartY * (int32_t)imgScale + (stepY >> 1);

			float lineDy0X = rawDy0X, lineDy0Y = rawDy0Y, lineDyGradX = rawDyGradX, lineDyGradY = rawDyGradY;
			if(L.UseFit) {
				double Sx, Sy, Px, Py, dPx, dPy, dSx, dSy;
				evalSmooth(t, Sx, Sy, Px, Py, dPx, dPy, dSx, dSy);
				double hk = (double)imgScale / (double)outScale;
				stepX = (int32_t)std::lround(Sx * hk);
				stepY = (int32_t)std::lround(Sy * hk);
				// Derivee verticale par ligne HD (texels niveau 0)
				lineDy0X = (float)(dPx * hk); lineDy0Y = (float)(dPy * hk);
				lineDyGradX = (float)(dSx * hk); lineDyGradY = (float)(dSy * hk);
			}

			const float lenX = std::sqrt((float)stepX * stepX + (float)stepY * stepY) / 256.0f;

			// Accumulation precise (16.16) : un pas arrondi a 1/256 de texel derivait de ~2 texels
			// en fin de ligne, differemment a chaque ligne -> bords obliques ondules.
			int64_t posFX, posFY, stepFX, stepFY;
			{
				double stXd, stYd, p0Xd, p0Yd;
				if(L.UseFit) {
					double Sx, Sy, Px, Py, dPx, dPy, dSx, dSy;
					evalSmooth(t, Sx, Sy, Px, Py, dPx, dPy, dSx, dSy);
					double hk = (double)imgScale / (double)outScale;
					stXd = Sx * hk; stYd = Sy * hk;
					p0Xd = Px * imgScale; p0Yd = Py * imgScale;
				} else {
					stXd = (curM7A_d * (double)imgScale) / (double)outScale;
					stYd = (curM7C_d * (double)imgScale) / (double)outScale;
					p0Xd = ((double)startX_y + t * (double)(startX_next - startX_y)) * imgScale;
					p0Yd = ((double)startY_y + t * (double)(startY_next - startY_y)) * imgScale;
				}
				stepFX = (int64_t)std::llround(stXd * 256.0);
				stepFY = (int64_t)std::llround(stYd * 256.0);
				posFX = (int64_t)std::llround((p0Xd + stXd * 0.5) * 256.0);
				posFY = (int64_t)std::llround((p0Yd + stYd * 0.5) * 256.0);
			}
			exactX = (int32_t)(posFX >> 8);
			exactY = (int32_t)(posFY >> 8);

			for(uint32_t x = 0; x < cropWidth; x++) {
				const SdColorCache& cache = sdColorRow[x];
				uint32_t outIdx = hdRowIdx + ((x + dstX) * outScale);

				// --- Niveau de detail : calcule UNE fois par pixel SNES (identique pour ses 'scale' pixels HD) ---
				float xSd = (float)x + 0.5f;
				float dyX = lineDy0X + xSd * lineDyGradX;
				float dyY = lineDy0Y + xSd * lineDyGradY;
				float lenY = std::sqrt(dyX * dyX + dyY * dyY) / 256.0f;
				float pMax = std::max(lenX, lenY);
				float pMin = std::max(std::min(lenX, lenY), 0.0001f);

				int taps = 1;
				float lambda;
				if(filterMode == Mode7FilterMode::Anisotropic) {
					taps = std::clamp((int)std::ceil(pMax / pMin), 1, kMaxAniso);
					lambda = std::log2(std::max(pMax / (float)taps, 1e-6f)) + kLodBias;
				} else {
					lambda = std::log2(std::max(pMax, 1e-6f)) + kLodBias;
				}
				if(lambda < 0.0f) lambda = 0.0f;
				int L0 = (int)lambda;
				float frac = lambda - (float)L0;
				const double tileLambdaBase = (double)lambda; // niveau de detail pour les <m7tiles> (avant ajustements)
				if(L0 >= numLevels - 1) { L0 = numLevels - 1; frac = 0.0f; }
				// Melange de 2 niveaux seulement dans la zone de transition (economise ~40% des lectures)
				if(frac < 0.25f) frac = 0.0f;
				else if(frac > 0.75f) { L0++; frac = 0.0f; }
				else frac = (frac - 0.25f) * 2.0f;

				float majX, majY;
				if(lenX >= lenY) { majX = (float)stepX; majY = (float)stepY; } else { majX = dyX; majY = dyY; }
				const float wTap = 1.0f / (float)taps;

				for(uint32_t sx = 0; sx < outScale; sx++) {
					uint32_t hdColorFromPng = 0;

					if(filterMode == Mode7FilterMode::Nearest) {
						int32_t lX = exactX >> L0, lY = exactY >> L0;
						int32_t ix = lX >> 8, iy = lY >> 8;
						uint32_t dim = mapDim >> L0;
						if(!(largeMap && (ix < 0 || iy < 0 || ix >= (int32_t)dim || iy >= (int32_t)dim))) {
							hdColorFromPng = levelPx[L0][(size_t)(iy & (dim - 1)) * dim + (ix & (dim - 1))];
						}
					} else {
						float aR = 0, aG = 0, aB = 0, aA = 0;
						for(int i = 0; i < taps; i++) {
							float o = (taps == 1) ? 0.0f : (((float)i + 0.5f) / (float)taps - 0.5f);
							int32_t sX = exactX + (int32_t)(majX * o);
							int32_t sY = exactY + (int32_t)(majY * o);
							if(frac == 0.0f) {
								addBilinear(sX, sY, L0, wTap, aR, aG, aB, aA);
							} else {
								addBilinear(sX, sY, L0, wTap * (1.0f - frac), aR, aG, aB, aA);
								addBilinear(sX, sY, L0 + 1, wTap * frac, aR, aG, aB, aA);
							}
						}
						if(aA > 0.5f) {
							float inv = 1.0f / aA;
							uint32_t a = (uint32_t)std::min(aA + 0.5f, 255.0f);
							uint32_t r = (uint32_t)std::min(aR * inv + 0.5f, 255.0f);
							uint32_t g = (uint32_t)std::min(aG * inv + 0.5f, 255.0f);
							uint32_t b = (uint32_t)std::min(aB * inv + 0.5f, 255.0f);
							hdColorFromPng = (a << 24) | (r << 16) | (g << 8) | b;
						}
					}

					// --- Tile Mode 7 individuelle presente dans cette case de la carte ? ---
					if(anyM7Cell) {
						const int64_t W16 = (int64_t)1024 << 16;
						int64_t mX = posFX / (int64_t)imgScale; // coordonnee carte, 16.16 texels SNES
						int64_t mY = posFY / (int64_t)imgScale;
						SnesHdMode7Image* timg = nullptr;
						int64_t lx16, ly16; // position dans la case (16.16 texels SNES, 0..8)
						bool outside = (mX < 0 || mY < 0 || mX >= W16 || mY >= W16);
						if(outside && largeMap) {
							timg = m7OutsideTile;
							const int64_t C16 = (int64_t)8 << 16;
							lx16 = ((mX % C16) + C16) % C16; ly16 = ((mY % C16) + C16) % C16;
						} else {
							int64_t wx = ((mX % W16) + W16) % W16, wy = ((mY % W16) + W16) % W16;
							int64_t cx = wx >> 19, cy = wy >> 19;
							timg = m7CellTile[(size_t)cy * 128 + (size_t)cx];
							lx16 = wx - (cx << 19); ly16 = wy - (cy << 19);
						}
						if(timg) {
							const uint32_t tScale = timg->Scale;
							const int tLevels = (int)timg->TileMips.size();
							int lt = (int)std::lround(tileLambdaBase + std::log2((double)tScale / (double)imgScale));
							lt = std::clamp(lt, 0, tLevels - 1);
							const uint32_t dim = (8 * tScale) >> lt;
							const uint32_t* tp = timg->TileMips[lt].data();
							// 8.8 texels de la tile au niveau lt, centre des texels
							int32_t tx = (int32_t)((lx16 * (int64_t)tScale) >> (8 + lt)) - 128;
							int32_t ty = (int32_t)((ly16 * (int64_t)tScale) >> (8 + lt)) - 128;
							int32_t ix = tx >> 8, iy = ty >> 8;
							float fx = (tx & 0xFF) * (1.0f / 256.0f), fy = (ty & 0xFF) * (1.0f / 256.0f);
							float tA = 0, tR = 0, tG = 0, tB = 0;
							const float wq[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
							const int32_t qx[4] = { ix, ix + 1, ix, ix + 1 };
							const int32_t qy[4] = { iy, iy, iy + 1, iy + 1 };
							for(int q = 0; q < 4; q++) {
								int32_t cxq = std::clamp(qx[q], 0, (int32_t)dim - 1), cyq = std::clamp(qy[q], 0, (int32_t)dim - 1);
								uint32_t c = tp[(size_t)cyq * dim + cxq];
								float a = (float)(c >> 24) * wq[q];
								tA += a; tR += ((c >> 16) & 0xFF) * a; tG += ((c >> 8) & 0xFF) * a; tB += (c & 0xFF) * a;
							}
							if(tA > 0.5f) {
								// Tile par-dessus la piste (alpha de la tile)
								float ta = std::min(tA, 255.0f) / 255.0f;
								float r = tR / tA, g = tG / tA, b = tB / tA;
								float ma = (float)(hdColorFromPng >> 24) / 255.0f;
								float mr = (float)((hdColorFromPng >> 16) & 0xFF), mg = (float)((hdColorFromPng >> 8) & 0xFF), mb = (float)(hdColorFromPng & 0xFF);
								float oa = ta + ma * (1.0f - ta);
								if(oa > 0.0f) {
									float k1 = ta / oa, k2 = ma * (1.0f - ta) / oa;
									uint32_t orr = (uint32_t)std::min(r * k1 + mr * k2 + 0.5f, 255.0f);
									uint32_t og = (uint32_t)std::min(g * k1 + mg * k2 + 0.5f, 255.0f);
									uint32_t ob = (uint32_t)std::min(b * k1 + mb * k2 + 0.5f, 255.0f);
									uint32_t oA = (uint32_t)std::min(oa * 255.0f + 0.5f, 255.0f);
									hdColorFromPng = (oA << 24) | (orr << 16) | (og << 8) | ob;
								}
							}
						}
					}

					uint32_t finalColor;
					if((hdColorFromPng >> 24) > 0) {
						if(applyColorMath) {
							int outR = std::clamp(((int)(hdColorFromPng >> 16) & 0xFF) + cache.diffR, 0, 255);
							int outG = std::clamp(((int)(hdColorFromPng >> 8) & 0xFF) + cache.diffG, 0, 255);
							int outB = std::clamp(((int)hdColorFromPng & 0xFF) + cache.diffB, 0, 255);
							finalColor = (hdColorFromPng & 0xFF000000) | (outR << 16) | (outG << 8) | outB;
						} else {
							finalColor = hdColorFromPng | 0xFF000000;
						}
					} else {
						finalColor = 0x00000000;
					}

					out[outIdx + sx] = finalColor;
					_zBuffer[outIdx + sx] = ((finalColor >> 24) == 0) ? 0 : pixelZ;

					posFX += stepFX;
					posFY += stepFY;
					exactX = (int32_t)(posFX >> 8);
					exactY = (int32_t)(posFY >> 8);
				}
			}
		}
	};

	if(anyMode7) {
		// Lignes reparties sur plusieurs coeurs (entrelacees pour equilibrer la charge).
		// On laisse 2 coeurs a l'emulation et au reste du systeme.
		unsigned hc = std::thread::hardware_concurrency();
		int nThreads = std::clamp((int)hc - 2, 1, 8);
		if(nThreads <= 1 || cropHeight < 16) {
			for(uint32_t y = 0; y < cropHeight; y++) {
				if(m7Lines[y].Img) renderMode7Line(y);
			}
		} else {
			std::vector<std::thread> workers;
			workers.reserve(nThreads);
			for(int t = 0; t < nThreads; t++) {
				workers.emplace_back([&, t]() {
					for(uint32_t y = (uint32_t)t; y < cropHeight; y += (uint32_t)nThreads) {
						if(m7Lines[y].Img) renderMode7Line(y);
					}
				});
			}
			for(auto& w : workers) w.join();
		}
	}

	for(uint32_t y = 0; y < cropHeight; y++) {
		uint32_t snesY = y + srcY;
		uint32_t outY = y + dstY;

		// --- BOUCLE X POUR LES SPRITES ET TUILES DE BASE ---
		for(uint32_t x = 0; x < cropWidth; x++) {
			uint32_t snesX = x + srcX;
			uint32_t outX = x + dstX;

			uint32_t bufferOffset = snesY * _baseFrameInfo.Width + snesX;
			uint16_t snesPixel = ppuOutputBuffer[bufferOffset];

			SnesHdPixelData hdDataMain = hdBuffer[bufferOffset];
			SnesHdPixelData hdDataSub = hdSubBuffer[bufferOffset];
			SnesHdPixelData hdData = hdDataMain;

			bool hasSubScreenHdElement = (hdDataSub.Tile != nullptr && !hdDataSub.Tile->HdTileData.empty());
			if(hasSubScreenHdElement) {
				hdData = hdDataSub;
			}

			if(hdData.IsMode7) continue;

			uint32_t sdColor = SnesHdPackLoader::ActivePack->IsColorMathDisabled
				? _calculatedPalette[hdData.BaseColor]
				: _calculatedPalette[snesPixel];

			uint8_t pixelZ = 20 + (hdData.SnesPriorityLevel * 4);
			if(hdData.IsHighPriority) pixelZ += 2;

			if(!hdData.IsSprite && hdData.SourceLayer >= 0 && hdData.SourceLayer < 4) {
				for(auto& rule : SnesHdPackLoader::ActivePack->LayerOrders) {
					if(!rule.MatchesCondition(snesX, snesY, &hdData)) continue;
					pixelZ = rule.ZIndexBg[hdData.SourceLayer];
					if(hdData.IsHighPriority) pixelZ += 2;
					break;
				}
			}

			// --- STEP 1.5: INDIVIDUAL MODE 7 TILES (<m7tiles>) WITH ANISOTROPIC FILTERING ---
			auto renderMode7Tile = [&](const SnesHdPixelData& m7Data, uint8_t targetZ, bool useColorMath) {
				SnesHdMode7Image* m7Img = SnesHdPackLoader::ActivePack->GetMode7Image(m7Data.M7TileHash);

				if(m7Img && m7Img->Bitmap && !m7Img->Bitmap->PixelData.empty()) {
					const uint32_t* rawPixelData = m7Img->Bitmap->PixelData.data();

					uint32_t imgScale = m7Img->Scale;
					int32_t tileDim = 8 * imgScale;

					uint32_t pngWidth = m7Img->Bitmap->Width;
					uint32_t pngHeight = m7Img->Bitmap->Height;
					int32_t atlasSrcX = m7Img->X;
					int32_t atlasSrcY = m7Img->Y;

					uint32_t mapDim = 1024 * imgScale;
					uint32_t bitMask = mapDim - 1;

					uint32_t baseSdColor = _calculatedPalette[m7Data.BaseColor];
					int diffR = 0, diffG = 0, diffB = 0;
					if(useColorMath) {
						diffR = ((sdColor >> 16) & 0xFF) - ((baseSdColor >> 16) & 0xFF);
						diffG = ((sdColor >> 8) & 0xFF) - ((baseSdColor >> 8) & 0xFF);
						diffB = (sdColor & 0xFF) - (baseSdColor & 0xFF);
					}

					int32_t baseHD_X = m7Data.M7XValue * imgScale;
					int32_t baseHD_Y = m7Data.M7YValue * imgScale;

					int32_t snesM7X = (m7Data.M7XValue >> 8) & 1023;
					int32_t snesM7Y = (m7Data.M7YValue >> 8) & 1023;
					int32_t tileMinX = (snesM7X & ~7) * imgScale;
					int32_t tileMinY = (snesM7Y & ~7) * imgScale;

					int32_t stepX = (m7Data.M7A * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepY = (m7Data.M7C * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepYTermX = (m7Data.M7B * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepYTermY = (m7Data.M7D * (int32_t)imgScale) / (int32_t)scale;

					double lenX = std::sqrt((double)stepX * stepX + (double)stepY * stepY) / 256.0;
					double lenY = std::sqrt((double)stepYTermX * stepYTermX + (double)stepYTermY * stepYTermY) / 256.0;

					double maxLen = std::max(lenX, lenY);
					double minLen = std::max(std::min(lenX, lenY), 0.001);

					int tileSamples = std::clamp((int)std::ceil(maxLen / minLen), 1, 16);

					double dirU = (lenX >= lenY && lenX > 0.0001) ? (stepX / (lenX * 256.0)) : (lenY > 0.0001 ? (stepYTermX / (lenY * 256.0)) : 1.0);
					double dirV = (lenX >= lenY && lenX > 0.0001) ? (stepY / (lenX * 256.0)) : (lenY > 0.0001 ? (stepYTermY / (lenY * 256.0)) : 0.0);

					auto getTileTexel = [&](int32_t sampleX, int32_t sampleY) -> uint32_t {
						int32_t px = sampleX >> 8;
						int32_t py = sampleY >> 8;
						int32_t localX = (px - tileMinX) & bitMask;
						int32_t localY = (py - tileMinY) & bitMask;

						if(localX > (int32_t)(bitMask >> 1)) localX = 0;
						else if(localX >= tileDim) localX = tileDim - 1;

						if(localY > (int32_t)(bitMask >> 1)) localY = 0;
						else if(localY >= tileDim) localY = tileDim - 1;

						uint32_t finalReadX = atlasSrcX + localX;
						uint32_t finalReadY = atlasSrcY + localY;

						if(finalReadX < pngWidth && finalReadY < pngHeight) {
							return rawPixelData[finalReadY * pngWidth + finalReadX];
						}
						return 0;
					};

					auto sampleTileBilinear = [&](int32_t sampleX, int32_t sampleY) -> uint32_t {
						uint32_t fracX = sampleX & 0xFF;
						uint32_t fracY = sampleY & 0xFF;

						uint32_t p00 = getTileTexel(sampleX, sampleY);
						uint32_t p10 = getTileTexel(sampleX + 256, sampleY);
						uint32_t p01 = getTileTexel(sampleX, sampleY + 256);
						uint32_t p11 = getTileTexel(sampleX + 256, sampleY + 256);

						uint32_t w00 = ((256 - fracX) * (256 - fracY)) >> 8;
						uint32_t w10 = (fracX * (256 - fracY)) >> 8;
						uint32_t w01 = ((256 - fracX) * fracY) >> 8;
						uint32_t w11 = (fracX * fracY) >> 8;

						uint32_t a = (((p00 >> 24) & 0xFF) * w00 + ((p10 >> 24) & 0xFF) * w10 + ((p01 >> 24) & 0xFF) * w01 + ((p11 >> 24) & 0xFF) * w11) >> 8;
						if(a == 0) return 0;

						uint32_t r = (((p00 >> 16) & 0xFF) * w00 + ((p10 >> 16) & 0xFF) * w10 + ((p01 >> 16) & 0xFF) * w01 + ((p11 >> 16) & 0xFF) * w11) >> 8;
						uint32_t g = (((p00 >> 8) & 0xFF) * w00 + ((p10 >> 8) & 0xFF) * w10 + ((p01 >> 8) & 0xFF) * w01 + ((p11 >> 8) & 0xFF) * w11) >> 8;
						uint32_t b = ((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10 + (p01 & 0xFF) * w01 + (p11 & 0xFF) * w11) >> 8;

						return (a << 24) | (r << 16) | (g << 8) | b;
					};

					for(uint32_t sy = 0; sy < scale; sy++) {
						int32_t yTermX = stepYTermX * (int32_t)sy;
						int32_t yTermY = stepYTermY * (int32_t)sy;
						uint32_t outBaseIdx = (outY * scale + sy) * outWidth + (outX * scale);

						for(uint32_t sx = 0; sx < scale; sx++) {
							int32_t exactX = baseHD_X + stepX * (int32_t)sx + yTermX;
							int32_t exactY = baseHD_Y + stepY * (int32_t)sx + yTermY;

							uint32_t hdColorFromPng = 0;

							if(m7Img->FilterMode == Mode7FilterMode::Anisotropic) {
								uint32_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
								double stepSize = maxLen / tileSamples;
								double startOffset = -0.5 * maxLen + 0.5 * stepSize;

								for(int i = 0; i < tileSamples; i++) {
									double offset = startOffset + i * stepSize;
									int32_t sampleX = exactX + (int32_t)(offset * dirU * 256.0);
									int32_t sampleY = exactY + (int32_t)(offset * dirV * 256.0);

									uint32_t c = sampleTileBilinear(sampleX, sampleY);
									uint32_t a = (c >> 24) & 0xFF;
									sumA += a;
									sumR += ((c >> 16) & 0xFF) * a;
									sumG += ((c >> 8) & 0xFF) * a;
									sumB += (c & 0xFF) * a;
								}

								if(sumA > 0) {
									uint32_t avgA = sumA / tileSamples;
									uint32_t avgR = sumR / sumA;
									uint32_t avgG = sumG / sumA;
									uint32_t avgB = sumB / sumA;
									hdColorFromPng = (avgA << 24) | (avgR << 16) | (avgG << 8) | avgB;
								}
							} else if(m7Img->FilterMode == Mode7FilterMode::Bilinear) {
								hdColorFromPng = sampleTileBilinear(exactX, exactY);
							} else {
								hdColorFromPng = getTileTexel(exactX, exactY);
							}

							if((hdColorFromPng >> 24) > 0) {
								int outR = ((hdColorFromPng >> 16) & 0xFF) + diffR;
								int outG = ((hdColorFromPng >> 8) & 0xFF) + diffG;
								int outB = (hdColorFromPng & 0xFF) + diffB;

								outR = std::clamp(outR, 0, 255);
								outG = std::clamp(outG, 0, 255);
								outB = std::clamp(outB, 0, 255);

								uint32_t finalColor = (hdColorFromPng & 0xFF000000) | (outR << 16) | (outG << 8) | outB;

								out[outBaseIdx + sx] = finalColor;
								_zBuffer[outBaseIdx + sx] = targetZ;
							}
						}
					}
				}
			};

			// Si une tuile Mode 7 individuelle (<m7tiles>) se trouve sous le sprite (sauvegardée dans hdDataSub),
			// on la dessine d'abord dans le buffer de sortie afin que le fond ne reste pas noir/transparent sous le sprite.
			// Lignes rendues par la passe Mode 7 HD : les <m7tiles> y sont deja echantillonnees
			// a la resolution HD. Le rendu bloc par bloc ne sert plus qu'aux autres lignes.
			const bool m7LineDone = (m7Lines[y].Img != nullptr);
			if(hdDataSub.IsMode7Tile && !m7LineDone) {
				uint8_t m7PixelZ = 20 + (hdDataSub.SnesPriorityLevel * 4);
				renderMode7Tile(hdDataSub, m7PixelZ, false);
			}

			if(hdData.IsMode7Tile) {
				if(!m7LineDone) renderMode7Tile(hdData, pixelZ, true);
				continue;
			}

			// CAPTURE DES TUILES DE BASE (Sprites et Backgrounds)
			if(hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty()) {
				int visualX = hdData.IsSprite
					? (hdData.HFlip ? (7 - hdData.TileX) : hdData.TileX)
					: (hdData.HFlip ? hdData.TileX : (7 - hdData.TileX));

				int visualY = hdData.VFlip ? (7 - hdData.TileY) : hdData.TileY;

				int startX = (int)outX - visualX;
				int startY = (int)outY - visualY;

				bool alreadyCaptured = false;
				for(int i = (int)tilesToExpand.size() - 1; i >= 0; i--) {
					if(tilesToExpand[i].Tile == hdData.Tile &&
					   tilesToExpand[i].startX == startX &&
					   tilesToExpand[i].startY == startY &&
					   tilesToExpand[i].oamIndex == hdData.OamIndex) {
						alreadyCaptured = true;
						break;
					}
				}

				if(!alreadyCaptured) {
					tilesToExpand.push_back({ hdData.Tile, startX, startY, pixelZ,
											  hdData.HFlip, hdData.VFlip, hdData.BaseColor, hdData.OamIndex });
				}
			}

			// CAPTURE DES SPRITES EN SECOND PLAN (CHEVAUCHEMENT / SPRITES PERDANTS)
			if(hdBehindBuffer != nullptr) {
				SnesHdPixelData hdBehindData = hdBehindBuffer[bufferOffset];
				if(hdBehindData.Tile != nullptr && !hdBehindData.Tile->HdTileData.empty()) {
					int visualX = hdBehindData.HFlip ? (7 - hdBehindData.TileX) : hdBehindData.TileX;
					int visualY = hdBehindData.VFlip ? (7 - hdBehindData.TileY) : hdBehindData.TileY;

					int startX = (int)outX - visualX;
					int startY = (int)outY - visualY;

					bool alreadyCaptured = false;
					for(int i = (int)tilesToExpand.size() - 1; i >= 0; i--) {
						if(tilesToExpand[i].Tile == hdBehindData.Tile &&
						   tilesToExpand[i].startX == startX &&
						   tilesToExpand[i].startY == startY &&
						   tilesToExpand[i].oamIndex == hdBehindData.OamIndex) {
							alreadyCaptured = true;
							break;
						}
					}

					if(!alreadyCaptured) {
						uint8_t behindPixelZ = 20 + (hdBehindData.SnesPriorityLevel * 4);
						if(hdBehindData.IsHighPriority) behindPixelZ += 2;
						tilesToExpand.push_back({ hdBehindData.Tile, startX, startY, behindPixelZ,
												  hdBehindData.HFlip, hdBehindData.VFlip, hdBehindData.BaseColor, hdBehindData.OamIndex });
					}
				}
			}

			// Rendu standard du pixel : ApplyFilterSpritesStandard gère l'effacement du sprite natif
			// et la restauration de UnderlyingColor (évite les empreintes noires sous les sprites HD).
			ApplyFilterSpritesStandard(outX, outY, scale, outWidth, sdColor, hdData, pixelZ, out);
		}
	} 

	// ====================================================================
	// ÉTAPE 2 : INJECTION DES ADDITIONS HD
	// ====================================================================
	if(SnesHdPackLoader::ActivePack) {
		bool overscanFrame = console->GetPpu()->GetState().OverscanMode;

		// Queues figees par SnesPpu::SendFrame (hdReadIndex lu au debut du filtre)
		for(const auto& add : SnesHdPackLoader::ActivePack->ActiveAdditionsQueues[hdReadIndex]) {
			SnesHdPackTileInfo* hdTile = SnesHdPackLoader::GetHdTile(
				add.MemoryHash,
				add.PaletteHash,
				SnesBpp::Bpp4,
				false,
				true,
				add.HFlip,
				add.VFlip
			);

			if(hdTile && hdTile->Bitmap != nullptr && !hdTile->HdTileData.empty()) {
				uint8_t pixelZ = 20 + (add.Priority * 4);

				int finalScreenX = add.ScreenX + sprOffsetX;
				int finalScreenY = (overscanFrame ? (add.ScreenY - 1) : (add.ScreenY + 7)) + sprOffsetY;

				tilesToExpand.push_back({
					hdTile,
					finalScreenX,
					finalScreenY,
					pixelZ,
					add.HFlip,
					add.VFlip,
					0,
					128
				});
			}
		}

		// ====================================================================
		// INJECTION DE TOUS LES SPRITES HD DE LA FRAME (ActiveSpritesQueues - double-buffered)
		// Résout la superposition de 3+ sprites (mini-map Mario Kart, Zelda, etc.)
		// ====================================================================
		const uint8_t* pSpritePri = console->GetPpu()->GetCurrentSpritePriorities();

		for(const auto& spr : SnesHdPackLoader::ActivePack->ActiveSpritesQueues[hdReadIndex]) {
			if(spr.HdTile == nullptr || spr.HdTile->HdTileData.empty()) continue;

			// Coordonnées exactes de la tuile 8x8 dans le repère de tilesToExpand
			int tileStartX = spr.StartX + sprOffsetX;
			int tileStartY = spr.StartY + sprOffsetY;

			// Déduplication : évite d'ajouter deux fois la même tuile pour le même sprite au même emplacement
			bool alreadyCaptured = false;
			for(int i = (int)tilesToExpand.size() - 1; i >= 0; i--) {
				if(tilesToExpand[i].Tile == spr.HdTile &&
				   tilesToExpand[i].startX == tileStartX &&
				   tilesToExpand[i].startY == tileStartY &&
				   tilesToExpand[i].oamIndex == spr.OamIndex) {
					alreadyCaptured = true;
					break;
				}
			}
			if(alreadyCaptured) continue;

			// Calcul du Z-index cohérent avec Step 1
			uint8_t mappedPrio = pSpritePri[spr.SnesPriority & 3];
			uint8_t sprPixelZ  = 20 + (mappedPrio * 4);
			if(spr.SnesPriority >= 2) sprPixelZ += 2;

			tilesToExpand.push_back({
				spr.HdTile,
				tileStartX,
				tileStartY,
				sprPixelZ,
				spr.HFlip,
				spr.VFlip,
				0,
				spr.OamIndex
			});
		}
	}

	// ====================================================================
	// ÉTAPE 3 : PASSE DE RENDU UNIFIÉE ET TRIÉE PAR Z-INDEX ASCENDANT
	// ====================================================================
	struct RenderTask
	{
		int zIndex;
		int type;     // 0 = Background, 1 = Tile/Sprite
		int oamIndex; // Utilisé pour ordonner les sprites de même ZIndex
		size_t index; // Index dans le vecteur source
	};

	std::vector<RenderTask> tasks;

	for(size_t i = 0; i < SnesHdPackLoader::ActivePack->Backgrounds.size(); i++) {
		auto& bg = SnesHdPackLoader::ActivePack->Backgrounds[i];
		if(!bg->MatchesCondition()) continue;
		tasks.push_back({ bg->ZIndex, 0, 0, i });
	}

	for(size_t i = 0; i < tilesToExpand.size(); i++) {
		tasks.push_back({ (int)tilesToExpand[i].zIndex, 1, (int)tilesToExpand[i].oamIndex, i });
	}

	std::stable_sort(tasks.begin(), tasks.end(), [](const RenderTask& a, const RenderTask& b) {
		if(a.zIndex != b.zIndex) {
			return a.zIndex < b.zIndex;
		}
		if(a.type != b.type) {
			return a.type < b.type;
		}
		return a.oamIndex > b.oamIndex;
	});


	uint32_t hdWidth = outWidth;

	// Preparation SEQUENTIELLE des backgrounds (image d'animation courante + PNG charge),
	// avant de repartir la composition sur plusieurs coeurs.
	std::vector<char> bgReady(SnesHdPackLoader::ActivePack->Backgrounds.size(), 0);
	for(const auto& task : tasks) {
		if(task.type != 0) continue;
		auto& bg = SnesHdPackLoader::ActivePack->Backgrounds[task.index];
		if(bg->IsAnimated && !bg->AnimFrames.empty() && bg->FramesBetweenSteps > 0) {
			uint32_t currentFrame = SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber;
			int totalFrames = (int)bg->AnimFrames.size();
			int currentAnimIndex = (currentFrame / bg->FramesBetweenSteps) % totalFrames;
			bg->Bitmap = bg->AnimFrames[currentAnimIndex];
		}
		if(bg->Bitmap == nullptr) continue;
		bg->Bitmap->Init();
		if(bg->Bitmap->PixelData.empty() || bg->Bitmap->Width == 0 || bg->Bitmap->Height == 0) continue;
		bgReady[task.index] = 1;
	}

	// Composition d'une BANDE de lignes HD [bandY0, bandY1[ : toutes les taches dans l'ordre
	// (backgrounds + tiles/sprites tries par Z). Les bandes sont disjointes -> resultat identique
	// a un rendu sequentiel, mais reparti sur plusieurs coeurs.
	auto composeBand = [&](uint32_t bandY0, uint32_t bandY1) {
	for(const auto& task : tasks) {
		if(task.type == 0) {
			auto& bg = SnesHdPackLoader::ActivePack->Backgrounds[task.index];
			if(!bgReady[task.index]) continue;

			int bgWidth = bg->Bitmap->Width;
			int bgHeight = bg->Bitmap->Height;
			int bgIndex = bg->MasterScrollLayer - 1;

			for(uint32_t hdY = bandY0; hdY < bandY1; hdY++) {
				uint32_t sdY = hdY / scale;
				int masterScrollX = 0, masterScrollY = 0;

				if(bgIndex >= 0 && bgIndex < 4 && sdY < 240) {
					auto& screenInfo = SnesHdPackLoader::ActivePack->ScreenInfo;
					masterScrollX = screenInfo.ScanlineHScroll[sdY][bgIndex];
					masterScrollY = screenInfo.ScanlineVScroll[sdY][bgIndex];
				}

				int calcScrollX = 0;
				int calcScrollY = 0;
				uint32_t currentFrame = SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber;

				switch(bg->ScrollSpeedOperator) {
					case SnesHdBackgroundInfo::ScrollOperator::Divide:
						calcScrollX = (bg->ScrollXRatio != 0.0f) ? (int)(masterScrollX / bg->ScrollXRatio) : masterScrollX;
						calcScrollY = (bg->ScrollYRatio != 0.0f) ? (int)(masterScrollY / bg->ScrollYRatio) : masterScrollY;
						break;
					case SnesHdBackgroundInfo::ScrollOperator::Plus:
						calcScrollX = masterScrollX + (int)(bg->ScrollXRatio * currentFrame);
						calcScrollY = masterScrollY + (int)(bg->ScrollYRatio * currentFrame);
						break;
					case SnesHdBackgroundInfo::ScrollOperator::Minus:
						calcScrollX = masterScrollX - (int)(bg->ScrollXRatio * currentFrame);
						calcScrollY = masterScrollY - (int)(bg->ScrollYRatio * currentFrame);
						break;
					case SnesHdBackgroundInfo::ScrollOperator::Multiply:
					default:
						calcScrollX = (int)(masterScrollX * bg->ScrollXRatio);
						calcScrollY = (int)(masterScrollY * bg->ScrollYRatio);
						break;
				}

				int lineScrollY = (calcScrollY * scale) + (bg->OffsetY * scale);
				int lineScrollX = (calcScrollX * scale) + (bg->OffsetX * scale);

				int pngY = bg->TileY ? ((hdY + lineScrollY) % bgHeight + bgHeight) % bgHeight : (hdY + lineScrollY);
				if(!bg->TileY && (pngY < 0 || pngY >= bgHeight)) continue;

				uint32_t pngBaseRowIndex = pngY * bgWidth;
				uint32_t outBaseRowIndex = hdY * hdWidth;

				auto blendPixel = [&](int localPngX, uint32_t localHdX) {
					uint32_t pixelColor = bg->Bitmap->PixelData[pngBaseRowIndex + localPngX];
					uint32_t alpha = (pixelColor >> 24);

					if(bg->AlphaFactor < 1.0f) {
						alpha = static_cast<uint32_t>(alpha * bg->AlphaFactor);
					}

					if(alpha > 0) {
						uint32_t outIdx = outBaseRowIndex + localHdX;

						if(bg->ZIndex >= _zBuffer[outIdx]) {
							if(alpha == 255) {
								out[outIdx] = pixelColor | 0xFF000000;
							} else {
								uint32_t dstColor = out[outIdx];
								uint32_t invAlpha = 255 - alpha;

								uint32_t s_RB = pixelColor & 0x00FF00FF;
								uint32_t s_AG = (pixelColor >> 8) & 0x00FF00FF;
								uint32_t d_RB = dstColor & 0x00FF00FF;
								uint32_t d_AG = (dstColor >> 8) & 0x00FF00FF;

								uint32_t out_RB = ((s_RB * alpha + d_RB * invAlpha) >> 8) & 0x00FF00FF;
								uint32_t out_AG = ((s_AG * alpha + d_AG * invAlpha) >> 8) & 0x00FF00FF;

								out[outIdx] = out_RB | (out_AG << 8) | 0xFF000000;
							}
							_zBuffer[outIdx] = bg->ZIndex;
						}
					}
				};

				if(!bg->TileX) {
					for(uint32_t hdX = 0; hdX < hdWidth; hdX++) {
						int pngX = hdX + lineScrollX;
						if(pngX >= 0 && pngX < bgWidth) blendPixel(pngX, hdX);
					}
				} else {
					int currentPngX = lineScrollX % bgWidth;
					if(currentPngX < 0) currentPngX += bgWidth;

					for(uint32_t hdX = 0; hdX < hdWidth; hdX++) {
						blendPixel(currentPngX, hdX);
						if(++currentPngX >= bgWidth) currentPngX = 0;
					}
				}
			}
		} else {
			const auto& item = tilesToExpand[task.index];
			uint32_t pngWidth = item.Tile->Width * scale;
			uint32_t pngHeight = item.Tile->Height * scale;

			bool doHFlip = item.hFlip && !item.Tile->IsHFlip;
			bool doVFlip = item.vFlip && !item.Tile->IsVFlip;

			// Seules les lignes de la tile situees dans la bande
			int32_t tileBaseY = item.startY * (int32_t)scale;
			int32_t pyStart = std::max<int32_t>(0, (int32_t)bandY0 - tileBaseY);
			int32_t pyEnd = std::min<int32_t>((int32_t)pngHeight, (int32_t)bandY1 - tileBaseY);
			for(int32_t pyi = pyStart; pyi < pyEnd; pyi++) {
				uint32_t py = (uint32_t)pyi;
				for(uint32_t px = 0; px < pngWidth; px++) {

					int32_t screenX = (item.startX * scale) + px;
					int32_t screenY = (item.startY * scale) + py;

					if(screenX < 0 || screenX >= (int32_t)outWidth || screenY < 0 || screenY >= (int32_t)hdHeight) continue;

					uint32_t readX = doHFlip ? (pngWidth - 1 - px) : px;
					uint32_t readY = doVFlip ? (pngHeight - 1 - py) : py;

					uint32_t hdColorFromPng = item.Tile->HdTileData[readY * pngWidth + readX];
					uint32_t alpha = (hdColorFromPng >> 24) & 0xFF;

					if(alpha > 0) {
						uint32_t pixelIndex = screenY * outWidth + screenX;

						if(item.zIndex >= _zBuffer[pixelIndex]) {
							uint32_t finalColor = hdColorFromPng;

							if(alpha == 255) {
								out[pixelIndex] = finalColor | 0xFF000000;
								_zBuffer[pixelIndex] = item.zIndex;
							} else {
								uint32_t dstColor = out[pixelIndex];
								uint32_t invAlpha = 255 - alpha;

								uint32_t s_R = (finalColor >> 16) & 0xFF;
								uint32_t s_G = (finalColor >> 8) & 0xFF;
								uint32_t s_B = finalColor & 0xFF;

								uint32_t d_R = (dstColor >> 16) & 0xFF;
								uint32_t d_G = (dstColor >> 8) & 0xFF;
								uint32_t d_B = dstColor & 0xFF;

								uint32_t out_R = ((s_R * alpha) + (d_R * invAlpha)) >> 8;
								uint32_t out_G = ((s_G * alpha) + (d_G * invAlpha)) >> 8;
								uint32_t out_B = ((s_B * alpha) + (d_B * invAlpha)) >> 8;

								out[pixelIndex] = (out_R << 16) | (out_G << 8) | out_B | 0xFF000000;
							}
						}
					}
				}
			}
		}
	}
	};

	{
		unsigned hc = std::thread::hardware_concurrency();
		int nThreads = std::clamp((int)hc - 2, 1, 8);
		if(nThreads <= 1 || hdHeight < 64 || tasks.empty()) {
			composeBand(0, hdHeight);
		} else {
			uint32_t bandH = (hdHeight + nThreads - 1) / nThreads;
			std::vector<std::thread> workers;
			workers.reserve(nThreads);
			for(int t = 0; t < nThreads; t++) {
				uint32_t b0 = (uint32_t)t * bandH;
				uint32_t b1 = std::min(hdHeight, b0 + bandH);
				if(b0 >= b1) break;
				workers.emplace_back([&composeBand, b0, b1]() { composeBand(b0, b1); });
			}
			for(auto& w : workers) w.join();
		}
	}

	// ====================================================================
	// ÉTAPE 4 : GESTION DES VIDÉOS (pl_mpeg)
	// ====================================================================
	if(SnesHdPackLoader::ActivePack != nullptr && out != nullptr) {
		for(auto& video : SnesHdPackLoader::ActivePack->Videos) {
			if(!video->MatchesCondition()) continue;

			if(!video->IsPlaying && !video->HasPlayed) {
				std::string fullPath = FolderUtilities::CombinePath(SnesHdPackLoader::ActivePack->RootFolder, video->Filename);

				video->VideoPlayer = plm_create_with_filename(fullPath.c_str());
				if(video->VideoPlayer != nullptr) {
					video->VideoWidth = plm_get_width(video->VideoPlayer);
					video->VideoHeight = plm_get_height(video->VideoPlayer);

					if(video->VideoWidth <= 0 || video->VideoHeight <= 0) {
						plm_destroy(video->VideoPlayer);
						video->VideoPlayer = nullptr;
						video->HasPlayed = true;
						continue;
					}

					video->IsPlaying = true;
					video->TimeAccumulator = 0.0;
					plm_set_audio_enabled(video->VideoPlayer, !video->MuteAudio);

					SoundMixer* mixer = _emu->GetSoundMixer();

					plm_set_audio_decode_callback(video->VideoPlayer, [](plm_t* mpeg, plm_samples_t* samples, void* user) {
						SoundMixer* audioMixer = static_cast<SoundMixer*>(user);
						if(audioMixer == nullptr) return;

						int totalValues = samples->count * 2;
						std::vector<int16_t> intSamples(totalValues);

						for(int i = 0; i < totalValues; i++) {
							float f = samples->interleaved[i];
							if(f > 1.0f) f = 1.0f;
							if(f < -1.0f) f = -1.0f;

							intSamples[i] = static_cast<int16_t>(f * 32767.0f);
						}

						uint32_t sampleRate = plm_get_samplerate(mpeg);
						audioMixer->PlayAudioBuffer(intSamples.data(), samples->count, sampleRate);

					}, mixer);

					video->CurrentFramePixels.resize(video->VideoWidth * video->VideoHeight * 4, 0);
				} else {
					video->HasPlayed = true;
					continue;
				}
			}

			// 2. LECTURE & SYNCHRONISATION
			/*if(video->IsPlaying && video->VideoPlayer != nullptr) {
				double fps = plm_get_framerate(video->VideoPlayer);
				if(fps <= 0.0) fps = 30.0;

				double frameTime = 1.0 / fps;
				video->TimeAccumulator += (1.0 / 60.0);

				bool newFrameDecoded = false;
				plm_frame_t* frame = nullptr;

				while(video->TimeAccumulator >= frameTime) {
					frame = plm_decode_video(video->VideoPlayer);
					video->TimeAccumulator -= frameTime;
					newFrameDecoded = true;
				}

				if(newFrameDecoded && frame != nullptr) {
					plm_frame_to_bgra(frame, video->CurrentFramePixels.data(), video->VideoWidth * 4);
				}

				if(plm_has_ended(video->VideoPlayer) && !video->Loop) {
					video->IsPlaying = false;
					video->HasPlayed = true;
					plm_destroy(video->VideoPlayer);
					video->VideoPlayer = nullptr;
					video->CurrentFramePixels.clear();
					continue;
				}



				// 3. DESSIN DES PIXELS SUR L'ÉCRAN 'out'
				if(!video->CurrentFramePixels.empty()) {
					uint32_t* videoPixels = reinterpret_cast<uint32_t*>(video->CurrentFramePixels.data());

					if(hdWidth > 0 && hdHeight > 0) {
						if(video->Stretch) {
							// ========================================================
							// MODE STRETCH : ÉTIREMENT PLEIN ÉCRAN VISIBLE
							// ========================================================
							for(uint32_t screenY = 0; screenY < hdHeight; screenY++) {
								int vy = (screenY * video->VideoHeight) / hdHeight;
								uint32_t* videoRow = &videoPixels[vy * video->VideoWidth];
								uint32_t outRow = screenY * outWidth;

								for(uint32_t screenX = 0; screenX < hdWidth; screenX++) {
									int vx = (screenX * video->VideoWidth) / hdWidth;
									uint32_t pixel = videoRow[vx];

									// Si AlphaEnable est actif, on saute le noir pur (#000000). Sinon on dessine tout.
									if(!video->AlphaEnable || (pixel & 0x00FFFFFF) != 0) {
										uint32_t outIdx = outRow + screenX;
										if(video->ZIndex >= _zBuffer[outIdx]) {
											out[outIdx] = pixel | 0xFF000000;
											_zBuffer[outIdx] = video->ZIndex;
										}
									}
								}
							}
						} else {
							// ========================================================
							// MODE NORMAL : POSITIONNEMENT 1:1 (X, Y)
							// ========================================================
							int startX = video->X;
							int startY = video->Y;
							int maxW = std::min(video->VideoWidth, (int)hdWidth - startX);
							int maxH = std::min(video->VideoHeight, (int)hdHeight - startY);

							if(maxW > 0 && maxH > 0) {
								for(int y = 0; y < maxH; y++) {
									for(int x = 0; x < maxW; x++) {
										int screenX = startX + x;
										int screenY = startY + y;

										if(screenX >= 0 && screenX < (int)hdWidth && screenY >= 0 && screenY < (int)hdHeight) {
											uint32_t pixel = videoPixels[y * video->VideoWidth + x];

											// Si AlphaEnable est actif, on saute le noir pur (#000000). Sinon on dessine tout.
											if(!video->AlphaEnable || (pixel & 0x00FFFFFF) != 0) {
												uint32_t outIdx = screenY * outWidth + screenX;

												if(video->ZIndex >= _zBuffer[outIdx]) {
													out[outIdx] = pixel | 0xFF000000;
													_zBuffer[outIdx] = video->ZIndex;
												}
											}
										}
									}
								}
							}
						}
					}
				}


			}
			*/

			// 2. LECTURE & SYNCHRONISATION
if(video->IsPlaying && video->VideoPlayer != nullptr) {

	double fps = plm_get_framerate(video->VideoPlayer);
	if(fps <= 0.0) fps = 30.0;

	double frameTime = 1.0 / fps;
	video->TimeAccumulator += (1.0 / 60.0);

	bool newFrameDecoded = false;
	plm_frame_t* frame = nullptr;

	while(video->TimeAccumulator >= frameTime) {
		frame = plm_decode_video(video->VideoPlayer);
		video->TimeAccumulator -= frameTime;
		newFrameDecoded = true;
	}

	if(newFrameDecoded && frame != nullptr) {
		plm_frame_to_bgra(frame, video->CurrentFramePixels.data(), video->VideoWidth * 4);
	}

	// === GESTION DE LA FIN ET DE LA BOUCLE ===
	if(plm_has_ended(video->VideoPlayer)) {
		if(video->LoopTargetFrame != 0) {
			// La vidéo boucle
			if(video->LoopTargetFrame > 0) {
				// On cherche le temps (en secondes) correspondant à la frame demandée
				double targetTime = (double)video->LoopTargetFrame / fps;
				plm_seek(video->VideoPlayer, targetTime, false);
			} else {
				// Boucle infinie classique depuis le début
				plm_rewind(video->VideoPlayer);
			}
			// On lit la nouvelle première frame
			frame = plm_decode_video(video->VideoPlayer);
			if(frame != nullptr) {
				plm_frame_to_bgra(frame, video->CurrentFramePixels.data(), video->VideoWidth * 4);
			}
		} else {
			// La vidéo s'arrête
			video->IsPlaying = false;
			video->HasPlayed = true;
			plm_destroy(video->VideoPlayer);
			video->VideoPlayer = nullptr;
			video->CurrentFramePixels.clear();
			continue;
		}
	}

	// 3. DESSIN DES PIXELS SUR L'ÉCRAN 'out' !
	if(!video->CurrentFramePixels.empty()) {
		uint32_t* videoPixels = reinterpret_cast<uint32_t*>(video->CurrentFramePixels.data());

		if(hdWidth > 0 && hdHeight > 0) {
			if(video->Stretch) {
				// ========================================================
				// MODE STRETCH : ÉTIREMENT PLEIN ÉCRAN VISIBLE
				// ========================================================
				for(uint32_t screenY = 0; screenY < hdHeight; screenY++) {
					int vy = (screenY * video->VideoHeight) / hdHeight;
					uint32_t* videoRow = &videoPixels[vy * video->VideoWidth];
					uint32_t outRow = screenY * outWidth;

					for(uint32_t screenX = 0; screenX < hdWidth; screenX++) {
						int vx = (screenX * video->VideoWidth) / hdWidth;
						uint32_t pixel = videoRow[vx];

						// Si AlphaEnable est actif, on saute le noir pur (#000000). Sinon on dessine tout.
						if(!video->AlphaEnable || (pixel & 0x00FFFFFF) != 0) {
							uint32_t outIdx = outRow + screenX;
							if(video->ZIndex >= _zBuffer[outIdx]) {
								out[outIdx] = pixel | 0xFF000000;
								_zBuffer[outIdx] = video->ZIndex;
							}
						}
					}
				}
			} else {
				// ========================================================
				// MODE NORMAL : POSITIONNEMENT 1:1 (X, Y)
				// ========================================================
				int startX = video->X;
				int startY = video->Y;
				int maxW = std::min(video->VideoWidth, (int)hdWidth - startX);
				int maxH = std::min(video->VideoHeight, (int)hdHeight - startY);

				if(maxW > 0 && maxH > 0) {
					for(int y = 0; y < maxH; y++) {
						for(int x = 0; x < maxW; x++) {
							int screenX = startX + x;
							int screenY = startY + y;

							if(screenX >= 0 && screenX < (int)hdWidth && screenY >= 0 && screenY < (int)hdHeight) {
								uint32_t pixel = videoPixels[y * video->VideoWidth + x];

								// Si AlphaEnable est actif, on saute le noir pur (#000000). Sinon on dessine tout.
								if(!video->AlphaEnable || (pixel & 0x00FFFFFF) != 0) {
									uint32_t outIdx = screenY * outWidth + screenX;

									if(video->ZIndex >= _zBuffer[outIdx]) {
										out[outIdx] = pixel | 0xFF000000;
										_zBuffer[outIdx] = video->ZIndex;
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

		}
	}

	// ====================================================================
	// ÉTAPE 5 : MASQUAGE DES BANDES NOIRES (Si Stretch == false)
	// ====================================================================
	if(!stretch && out != nullptr) {
		uint32_t topPx = overscan.Top * scale;
		uint32_t btmPx = (_baseFrameInfo.Height - overscan.Bottom) * scale;
		uint32_t leftPx = overscan.Left * scale;
		uint32_t rightPx = (_baseFrameInfo.Width - overscan.Right) * scale;

		// Bande supérieure
		for(uint32_t y = 0; y < topPx && y < hdHeight; y++) {
			for(uint32_t x = 0; x < outWidth; x++) {
				out[y * outWidth + x] = 0xFF000000;
				_zBuffer[y * outWidth + x] = 255;
			}
		}
		// Bande inférieure
		for(uint32_t y = btmPx; y < hdHeight; y++) {
			for(uint32_t x = 0; x < outWidth; x++) {
				out[y * outWidth + x] = 0xFF000000;
				_zBuffer[y * outWidth + x] = 255;
			}
		}
		// Bandes gauche et droite
		for(uint32_t y = topPx; y < btmPx && y < hdHeight; y++) {
			for(uint32_t x = 0; x < leftPx && x < outWidth; x++) {
				out[y * outWidth + x] = 0xFF000000;
				_zBuffer[y * outWidth + x] = 255;
			}
			for(uint32_t x = rightPx; x < outWidth; x++) {
				out[y * outWidth + x] = 0xFF000000;
				_zBuffer[y * outWidth + x] = 255;
			}
		}
	}

	// ====================================================================
	// ÉTAPE 6 : VERROUILLAGE FINAL DE L'ALPHA
	// ====================================================================
	if(out != nullptr && hdWidth > 0 && hdHeight > 0) {
		for(uint32_t y = 0; y < hdHeight; y++) {
			for(uint32_t x = 0; x < hdWidth; x++) {
				out[y * outWidth + x] |= 0xFF000000;
			}
		}
	}
}
//------------------------------------------------------------------------------------------------------------/


void SnesHdVideoFilter::ApplyFilterSpritesOAMScan(uint32_t scale, uint32_t outWidth, uint32_t outHeight, uint32_t* out)
{
	// 1. Sécurité
	if(!SnesHdPackLoader::ActivePack || !_emu) return;

	SnesConsole* console = (SnesConsole*)_emu->GetConsole().get();
	SnesPpu* ppu = console->GetPpu();
	if(!ppu) return;

	// 2. Récupération de la RAM des sprites brutes (OAM)
	uint8_t* oamRam = ppu->GetSpriteRam(); // 544 octets

	// --- LES PARAMÈTRES GLOBAUX DU PPU ---
	// La SNES permet de choisir deux tailles de sprites parmi une liste.
	// Ces valeurs sont dans le registre $2101 (OBSEL), on les simulera par des valeurs standards 
	// (8x8 et 16x16) pour le prototype, à affiner selon ton PPU.
	int smallSpriteSize = 8;
	int largeSpriteSize = 16;
	// -------------------------------------

	// 3. Boucle sur les 128 Sprites matériels
	for(int i = 0; i < 128; i++) {

		// --- LECTURE DU FORMAT OAM (544 Octets) ---
		// Les 512 premiers octets = 4 octets par sprite (X, Y, Char, Attr)
		uint32_t baseAddr = i * 4;

		// Les 32 derniers octets = La "Table Haute" (High Table) qui contient
		// le 9ème bit de X (pour les X négatifs ou > 255) et la taille du sprite.
		uint32_t highTableAddr = 512 + (i / 4);
		uint8_t highBits = (oamRam[highTableAddr] >> ((i % 4) * 2)) & 0x03;

		bool isLarge = (highBits & 0x02) != 0;
		int xBit9 = (highBits & 0x01) << 8;

		// --- EXTRACTION DES VALEURS ---
		int x = oamRam[baseAddr + 0] | xBit9;
		if(x > 255) x -= 512; // Gestion des nombres signés (hors-écran à gauche)

		int y = oamRam[baseAddr + 1];
		if(y == 240) continue; // Y=240 cache le sprite sur SNES
		if(y > 255) y -= 256;

		uint8_t tileIndex = oamRam[baseAddr + 2];
		uint8_t attributes = oamRam[baseAddr + 3];

		bool vFlip = (attributes & 0x80) != 0;
		bool hFlip = (attributes & 0x40) != 0;
		uint8_t priority = (attributes & 0x30) >> 4;
		uint8_t palette = (attributes & 0x0E) >> 1;
		bool nameTable = (attributes & 0x01) != 0; // Utilise la 2ème page de CHR RAM

		int size = isLarge ? largeSpriteSize : smallSpriteSize;

		// ====================================================================
		// LE TEST FATIDIQUE : CE SPRITE A-T-IL UN PNG HD ?
		// ====================================================================
		// On construit la clé de recherche. Le calcul du Hash dépend fortement 
		// de la façon dont 'MemoryHash' est généré dans ton loader pour les sprites.
		// En général, le Hash est (TileIndex | (NameTable << 8)).

		SnesHdTileKey key;
		key.MemoryHash = tileIndex | (nameTable ? 0x100 : 0x00);
		key.PaletteHash = 0; // Souvent 0 pour les sprites selon les packs
		key.Bpp = SnesBpp::Bpp4; // Les sprites sont toujours en 4BPP sur SNES
		key.IsLargeTile = isLarge;
		key.IsSprite = true;

		auto it = SnesHdPackLoader::ActivePack->TileByKey.find(key);
		if(it != SnesHdPackLoader::ActivePack->TileByKey.end() && !it->second.empty()) {

			// ON A TROUVÉ UN SPRITE HD !
			SnesHdPackTileInfo* hdTile = nullptr;

			// On cherche le bon PNG parmi les conditions
			for(auto* tile : it->second) {
				if(tile->MatchesCondition(x, y, nullptr)) {
					hdTile = tile;
					break;
				}
			}

			if(hdTile == nullptr || hdTile->HdTileData.empty()) continue;

			// ====================================================================
			// DESSIN DU SPRITE HD (SANS CLIPPING !)
			// ====================================================================
			// On calcule le Z-Index du sprite à partir de sa priorité matérielle
			uint8_t pixelZ = 20 + (priority * 4);

			uint32_t pngWidth = hdTile->Width * scale;
			uint32_t pngHeight = hdTile->Height * scale;

			// On dessine toute la surface du PNG !
			for(uint32_t py = 0; py < pngHeight; py++) {
				for(uint32_t px = 0; px < pngWidth; px++) {

					// Position finale sur l'écran HD
					int32_t screenX = (x * scale) + px;
					int32_t screenY = (y * scale) + py;

					// Culling : On ne dessine pas en dehors de l'écran final
					if(screenX < 0 || screenX >= (int32_t)outWidth || screenY < 0 || screenY >= (int32_t)outHeight) {
						continue;
					}

					// Lecture du pixel PNG avec gestion du Flip !
					uint32_t readX = hFlip ? (pngWidth - 1 - px) : px;
					uint32_t readY = vFlip ? (pngHeight - 1 - py) : py;

					uint32_t hdColorFromPng = hdTile->HdTileData[readY * pngWidth + readX];
					uint32_t alpha = (hdColorFromPng >> 24) & 0xFF;

					if(alpha > 0) {
						uint32_t pixelIndex = screenY * outWidth + screenX;

						// === LE Z-TEST ===
						// On ne dessine le sprite OAM que s'il est DEVANT ce qui est déjà dessiné.
						if(pixelZ >= _zBuffer[pixelIndex]) {

							// Opacité maximale (ou mixage si tu veux l'Alpha Blending complet ici)
							out[pixelIndex] = hdColorFromPng | 0xFF000000;

							// On met à jour le Z-Buffer
							_zBuffer[pixelIndex] = pixelZ;
						}
					}
				}
			}
		}
	}
}

/*-------------------------------------------------------------------------------------------------------------------*/
	/*
conservation des fonds sous les sprites masqués : Lorsqu'un sprite est marqué blank ou disable, 
FinalColor récupère la couleur déjà présente dans out (ou UnderlyingColor par défaut) au lieu de forcer 0x00000000. 
Cela élimine les trous noirs/fantômes autour des karts ou des personnages.

Réinitialisation de la profondeur (finalZ = 0) : Pour toute tuile masquée sans remplacement HD, le Z-buffer local est remis à zéro pour que les arrière-plans, le Mode 7 et les calques <background> puissent s'afficher librement à cet endroit.

Isolation stricte de hasHdReplacement : Si la tuile a le flag IsDisabled = true, elle est exclue du rendu HD (hasHdReplacement = false), évitant tout conflit d'affichage.
	
	*/

void SnesHdVideoFilter::ApplyFilterSpritesStandard(uint32_t x, uint32_t y, uint32_t scale, uint32_t outWidth,
																	 uint32_t sdColor, const SnesHdPixelData& hdData,
																	 uint8_t pixelZ, uint32_t* out)
{
	// ====================================================================
	// 1. DÉTERMINATION DES RÈGLES D'EFFACEMENT ET DE REMPLACEMENT
	// ====================================================================
	bool isTileDisabled = (hdData.Tile != nullptr && hdData.Tile->IsDisabled);
	bool hasHdReplacement = (hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty() && !isTileDisabled);
	bool eraseThisTile = hasHdReplacement || isTileDisabled;

	// Règles globales d'effacement du pack
	if(!hdData.IsSprite) {
		if(SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
			eraseThisTile = true;
		} else {
			if(hdData.SourceLayer == 0 && SnesHdPackLoader::ActivePack->DisableBg1) eraseThisTile = true;
			else if(hdData.SourceLayer == 1 && SnesHdPackLoader::ActivePack->DisableBg2) eraseThisTile = true;
			else if(hdData.SourceLayer == 2 && SnesHdPackLoader::ActivePack->DisableBg3) eraseThisTile = true;
			else if(hdData.SourceLayer == 3 && SnesHdPackLoader::ActivePack->DisableBg4) eraseThisTile = true;
		}
	} else {
		if(SnesHdPackLoader::ActivePack->DisableOriginalSprites) {
			eraseThisTile = true;
		}
	}

	// ====================================================================
	// 2. BOUCLE DE RENDU PIXEL PAR PIXEL
	// ====================================================================
	for(uint32_t sy = 0; sy < scale; sy++) {
		for(uint32_t sx = 0; sx < scale; sx++) {

			uint32_t pixelIndex = (y * scale + sy) * outWidth + (x * scale + sx);
			uint32_t finalColor = sdColor | 0xFF000000;
			uint8_t finalZ = pixelZ;

			// --- ÉTAPE A : NETTOYAGE DU PIXEL NATIF ---
			if(eraseThisTile) {
				uint32_t currentOutColor = out[pixelIndex];

				if((currentOutColor & 0x00FFFFFF) != 0) {
					// 1. Un décor HD (Mode 7 ou <background>) a déjà été dessiné : on le conserve,
					// COULEUR ET Z-BUFFER. Remettre le Z à 0 ici laissait les passes suivantes
					// (ex : <background> de Z bas) repeindre le décor là où le sprite SD avait des
					// pixels mais où le PNG HD du sprite est transparent ("empreintes" sur le Mode 7).
					continue;
				} else if(hdData.IsSprite) {
					// 2. Pour un sprite : restauration de la couleur du décor cachée sous le sprite
					finalColor = _calculatedPalette[hdData.UnderlyingColor] | 0xFF000000;
				} else if(SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
					// 3. Pour une tuile de décor désactivée : transparent
					finalColor = 0x00000000;
				} else {
					// 4. Restauration standard de l'arrière-plan
					finalColor = _calculatedPalette[hdData.UnderlyingColor] | 0xFF000000;
				}

				finalZ = 0; // Remise à zéro du Z-Buffer pour laisser passer les passes ultérieures
			}

			// --- ÉTAPE B : RENDU DU PNG HD ---
			// Le rendu des tuiles HD est désormais délégué à l'Étape 3 (tilesToExpand)
			// afin de respecter l'ordre des Z-Index et permettre le vrai Alpha Blending
			// avec les <background>. On conserve finalZ = 0 pour ne pas verrouiller le Z-buffer.

			// --- ÉTAPE C : ÉCRITURE PIXEL ET Z-BUFFER ---
			if(finalZ >= _zBuffer[pixelIndex] || finalZ == 0) {
				out[pixelIndex] = finalColor;
				_zBuffer[pixelIndex] = ((finalColor >> 24) == 0) ? 0 : finalZ;
			}
		}
	}
}
//-------------------------------------------------------------------------------------------------------------

bool SnesHdPackTileAtPositionCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!vram) return false;

	std::string logPath = "C:\\Users\\Gwen\\Documents\\Mesen2\\HdPacks\\TileDebug.Log";
	bool shouldLogThisFrame = (_screenInfo->FrameNumber == 60);
	std::ofstream* logfile = nullptr;

	if(shouldLogThisFrame) {
		logfile = new std::ofstream(logPath, std::ios_base::app);
		*logfile << "=== EVALUATION | Condition [" << Name << "] ===" << std::endl;
		*logfile << "Scope : " << (Scope == SnesHdPackConditionScope::Layer ? "LAYER" : "SCREEN") << std::endl;
		*logfile << "Cible Attendue : Tile=$" << HexUtilities::ToHex(this->MemoryHash) << " | Palette=" << this->PaletteHash << std::endl;
	}

	for(int layer = 0; layer < 4; layer++) {
		if((TargetLayers & (1 << layer)) == 0) continue;

		LayerConfig& config = state.Layers[layer];

		// Définition des limites de la boucle selon les Wildcards
		int startX = CheckAnyX ? 0 : this->TileX;
		int endX = CheckAnyX ? (Scope == SnesHdPackConditionScope::Layer ? (config.DoubleWidth ? 63 : 31) : 31) : this->TileX;

		int startY = CheckAnyY ? 0 : this->TileY;
		int endY = CheckAnyY ? (Scope == SnesHdPackConditionScope::Layer ? (config.DoubleHeight ? 63 : 31) : 31) : this->TileY;

		for(int ty = startY; ty <= endY; ty++) {
			for(int tx = startX; tx <= endX; tx++) {

				int tileCol, tileRow;

				// --- 1. CALCUL DES COORDONNÉES SELON LE SCOPE ---
				if(Scope == SnesHdPackConditionScope::Screen) {
					// Mode Écran : On convertit la tuile en pixels (tx * 8), on applique la caméra, et on repasse en tuile (/ 8)
					int mapX = ((tx * 8) + config.HScroll) & 0x3FF;
					int mapY = ((ty * 8) + config.VScroll) & 0x3FF;
					tileCol = mapX / 8;
					tileRow = mapY / 8;
				} else {
					// Mode Layer : On lit directement sur la carte brute (sans la caméra)
					tileCol = tx & (config.DoubleWidth ? 63 : 31);
					tileRow = ty & (config.DoubleHeight ? 63 : 31);
				}

				// --- 2. LOGIQUE VRAM SNES (Gestion des blocs 32x32) ---
				int finalCol = tileCol;
				int finalRow = tileRow;
				int mapOffset = 0;
				if(config.DoubleWidth && finalCol >= 32) { mapOffset += 32 * 32; finalCol -= 32; }
				if(config.DoubleHeight && finalRow >= 32) { mapOffset += (config.DoubleWidth ? 2 : 1) * 32 * 32; finalRow -= 32; }

				int vramIndex = config.TilemapAddress + mapOffset + (finalRow * 32) + finalCol;

				if(vramIndex >= 0x8000) continue; // Sécurité mémoire

				// --- 3. COMPARAISON ---
				uint16_t tileData = vram[vramIndex];
				uint16_t snesTileIndex = tileData & 0x03FF;
				uint32_t snesPalette = (tileData >> 10) & 0x07;

				bool isTileMatch = false;

				// --- LE TRI INTELLIGENT (Index ou Hash ?) ---
				if(this->MemoryHash <= 0xFFFF) {
					// CAS 1 : L'utilisateur a entré un Index VRAM (ex: 101)
					isTileMatch = (this->MemoryHash == snesTileIndex);
				} else {
					// CAS 2 : L'utilisateur a entré un Hash 64-bits (ex: 2FEE8E4AF5240326)

					// 1. On déduit le BPP exact en fonction du Mode BG de la SNES et du Calque
					uint32_t bpp = 2; // Valeur par défaut
					switch(state.BgMode) {
						case 0: bpp = 2; break;
						case 1: bpp = (layer < 2) ? 4 : 2; break;
						case 2: bpp = 4; break;
						case 3: bpp = (layer == 0) ? 8 : 4; break;
						case 4: bpp = (layer == 0) ? 8 : 2; break;
						case 5: bpp = (layer == 0) ? 4 : 2; break;
						case 6: bpp = 4; break;
						case 7: bpp = 8; break;
					}

					// 2. On calcule l'adresse en VRAM (ChrAddress dans Mesen)
					uint32_t tileStart = config.ChrAddress + (snesTileIndex * 4 * bpp);
					size_t wordCount = 4 * bpp;
					uint64_t actualTileHash = 0;

					// 3. On génère la signature 64-bits à la volée !
					for(size_t i = 0; i < wordCount; i++) {
						uint16_t word = vram[(tileStart + i) & 0x7FFF];
						actualTileHash = (actualTileHash * 31) + (word & 0xFF);
						actualTileHash = (actualTileHash * 31) + (word >> 8);
					}


					// MOUCHARD : Si l'émulateur croise la tuile 101, il écrit son vrai hash
					if(logfile && snesTileIndex == 101) {
						*logfile << "=> INFO : Hash reel calcule pour la tuile 101 = $" << HexUtilities::ToHex(actualTileHash) << std::endl;
					}
					isTileMatch = (this->MemoryHash == actualTileHash);
				}

				// --- VÉRIFICATION FINALE ---
				if(isTileMatch) {
					if(this->IgnorePalette || this->PaletteHash == snesPalette) {
						// VICTOIRE !
						if(logfile) {
							*logfile << "======> MATCH PARFAIT SUR LAYER " << (layer + 1)
								<< " | Trouvé à X:" << tx << " Y:" << ty << " <======" << std::endl;
							logfile->close(); delete logfile;
						}
						return true;
					}
				}

			}
		}
	}

	if(logfile) {
		*logfile << "=> ECHEC : Aucune tuile correspondante trouvée." << std::endl;
		logfile->close(); delete logfile;
	}

	return false;
}



//--------------------------------------------------------------------------------------------------
bool SnesHdPackSpriteAtPositionCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	uint8_t* oamRam = _screenInfo->Ppu->GetSpriteRam();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!oamRam || !vram) return false;

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();

	for(int i = 0; i < 128; i++) {
		int offset = i * 4;

		// 1. Lecture des coordonnées avec le VRAI calcul du 9ème bit pour X !
		uint8_t highTableValue = oamRam[0x200 | (i >> 2)] >> ((i << 1) & 0x06);
		static constexpr uint16_t sign[2] = { 0x0000, 0xFF00 };
		int16_t spriteX = (int16_t)(sign[highTableValue & 0x01] | oamRam[offset + 0]);
		uint8_t spriteY = oamRam[offset + 1];

		// Vérification de la visibilité (le comportement par défaut est d'exiger qu'il soit sur l'écran)
		if(!this->ConsiderVisible) {
			if(spriteY >= 240 && spriteY != 255) continue; // Caché en Y par les conventions de la SNES
			if(spriteX <= -64 || spriteX >= 256) continue; // Caché en X (hors écran complètement)
		}

		// On vérifie d'abord si on est à la bonne position.
		// On teste oamRam[offset + 0] ET le vrai spriteX pour assurer une rétrocompatibilité parfaite 
		// si tu avais déjà des lignes configurées avec la valeur tronquée.
		uint8_t oldSpriteX = oamRam[offset + 0];

		/*if((this->CheckAnyX || oldSpriteX == this->TileX || spriteX == this->TileX) &&
			(this->CheckAnyY || spriteY == this->TileY)) {
			*/


		// On vérifie si la position du sprite tombe dans la case 8x8 correspondante (ex: pixels 112 à 119)
		bool matchX = this->CheckAnyX ||
			(spriteX >= this->TileX && spriteX < this->TileX + 8) ||
			(oldSpriteX >= this->TileX && oldSpriteX < this->TileX + 8);

		bool matchY = this->CheckAnyY ||
			(spriteY >= this->TileY && spriteY < this->TileY + 8);

		if(matchX && matchY) {

			// 2. Extraction de l'Index et de la Palette depuis l'OAM de la SNES
			uint16_t snesTileIndex = oamRam[offset + 2] | ((oamRam[offset + 3] & 0x01) << 8);
			uint32_t snesPalette = (oamRam[offset + 3] >> 1) & 0x07;

			bool isTileMatch = false;

			// --- LE TRI INTELLIGENT (Index ou Hash ?) ---
			if(this->MemoryHash <= 0xFFFF) {
				isTileMatch = (this->MemoryHash == snesTileIndex);
			} else {
				uint32_t bpp = 4;
				uint32_t tileAddress = state.OamBaseAddress;
				if(snesTileIndex >= 256) {
					tileAddress += state.OamAddressOffset;
				}

				uint32_t tileStart = tileAddress + ((snesTileIndex % 256) * 4 * bpp);
				size_t wordCount = 4 * bpp;
				uint64_t actualTileHash = 0;

				for(size_t j = 0; j < wordCount; j++) {
					uint16_t word = vram[(tileStart + j) & 0x7FFF];
					actualTileHash = (actualTileHash * 31) + (word & 0xFF);
					actualTileHash = (actualTileHash * 31) + (word >> 8);
				}

				isTileMatch = (this->MemoryHash == actualTileHash);
			}

			// --- VÉRIFICATION FINALE ---
			if(isTileMatch) {
				if(this->IgnorePalette || this->PaletteHash == snesPalette) {
					return true; // Match parfait du sprite !
				}
			}
		}
	}
	return false;
}
//--------------------------------------------------------------------------------------------------
bool SnesHdPackTileNearbyCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!vram) return false;

	// 1. Prise en compte du miroir de la tuile évaluée
	int xSign = (tile && tile->HFlip) ? -1 : 1;
	int ySign = (tile && tile->VFlip) ? -1 : 1;

	int startX = CheckAnyX ? 0 : (x + (OffsetX * xSign));
	int endX = CheckAnyX ? 255 : startX;
	int stepX = CheckAnyX ? 8 : 1;

	int startY = CheckAnyY ? 0 : (y + (OffsetY * ySign));
	int endY = CheckAnyY ? 239 : startY;
	int stepY = CheckAnyY ? 8 : 1;

	for(int layer = 0; layer < 4; layer++) {
		LayerConfig& config = state.Layers[layer];

		// Détermination de la profondeur de couleur (BPP)
		uint32_t bpp = 2;
		switch(state.BgMode) {
			case 0: bpp = 2; break;
			case 1: bpp = (layer < 2) ? 4 : 2; break;
			case 2: bpp = 4; break;
			case 3: bpp = (layer == 0) ? 8 : 4; break;
			case 4: bpp = (layer == 0) ? 8 : 2; break;
			case 5: bpp = (layer == 0) ? 4 : 2; break;
			case 6: bpp = 4; break;
			case 7: bpp = 8; break;
		}

		for(int py = startY; py <= endY; py += stepY) {
			for(int px = startX; px <= endX; px += stepX) {

				uint16_t snesTileIndex = 0;
				uint32_t snesPalette = 0;

				// --- CAS 1 : MODE 7 ---
				if(state.BgMode == 7) {
					if(layer > 0) continue; // Le Mode 7 n'a qu'un seul calque (BG1)

					int mapX = (px + state.Mode7.HScroll) & 0x3FF;
					int mapY = (py + state.Mode7.VScroll) & 0x3FF;

					uint16_t tilemapWord = vram[((mapY / 8) * 128) + (mapX / 8)];
					snesTileIndex = tilemapWord & 0xFF; // Octet de poids faible = Index
					snesPalette = 0;
				}
				// --- CAS 2 : MODES CLASSIQUES (BG0 à BG6) ---
				else {
					int mapX = (px + config.HScroll) & 0x3FF;
					int mapY = (py + config.VScroll) & 0x3FF;

					int tileSizeShift = config.LargeTiles ? 4 : 3; // 16px ou 8px
					int tileCol = mapX >> tileSizeShift;
					int tileRow = mapY >> tileSizeShift;

					int finalCol = tileCol;
					int finalRow = tileRow;
					int mapOffset = 0;

					if(config.DoubleWidth && finalCol >= 32) { mapOffset += 32 * 32; finalCol -= 32; }
					if(config.DoubleHeight && finalRow >= 32) { mapOffset += (config.DoubleWidth ? 2 : 1) * 32 * 32; finalRow -= 32; }

					int vramIndex = config.TilemapAddress + mapOffset + (finalRow * 32) + finalCol;
					if(vramIndex >= 0x8000) continue;

					uint16_t tileData = vram[vramIndex];
					uint16_t baseTileIndex = tileData & 0x03FF;
					snesPalette = (tileData >> 10) & 0x07;

					// Résolution de la sous-tuile 8x8 en mode 16x16
					if(config.LargeTiles) {
						bool hFlip = (tileData & 0x4000) != 0;
						bool vFlip = (tileData & 0x8000) != 0;

						int subX = (mapX / 8) & 1;
						int subY = (mapY / 8) & 1;

						if(hFlip) subX = 1 - subX;
						if(vFlip) subY = 1 - subY;

						snesTileIndex = baseTileIndex + subX + (subY * 16);
					} else {
						snesTileIndex = baseTileIndex;
					}
				}

				// --- VERIFICATION ET HASHING ---
				bool isTileMatch = false;

				if(this->MemoryHash <= 0xFFFF) {
					isTileMatch = (this->MemoryHash == snesTileIndex);
				} else {
					uint32_t tileStart = (state.BgMode == 7)
						? (snesTileIndex * 32) // Mode 7 : 64 octets par tuile = 32 mots VRAM
						: (config.ChrAddress + (snesTileIndex * 4 * bpp));

					size_t wordCount = 4 * bpp;
					uint64_t actualTileHash = 0;

					for(size_t j = 0; j < wordCount; j++) {
						uint16_t word = vram[(tileStart + j) & 0x7FFF];
						actualTileHash = (actualTileHash * 31) + (word & 0xFF);
						actualTileHash = (actualTileHash * 31) + (word >> 8);
					}

					isTileMatch = (this->MemoryHash == actualTileHash);
				}

				if(isTileMatch) {
					if(this->IgnorePalette || this->PaletteHash == snesPalette) {
						return true;
					}
				}
			}
		}
	}

	return false;
}
//--------------------------------------------------------------------------------------------------
/* Vérifie si un sprite matériel est proche d'une position donnée (x, y) sur l'écran SNES.
* @param x La coordonnée X de la position à vérifier.
* @param y La coordonnée Y de la position à vérifier.
* @param tile Pointeur vers les données du sprite HD (peut être null si non utilisé).
*/
bool SnesHdPackSpriteNearbyCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	uint8_t* oamRam = _screenInfo->Ppu->GetSpriteRam();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!oamRam || !vram) return false;

	static constexpr uint8_t oamWidth[16]  = { 8, 8, 8, 16, 16, 32, 16, 16, 16, 32, 64, 32, 64, 64, 32, 32 };
	static constexpr uint8_t oamHeight[16] = { 8, 8, 8, 16, 16, 32, 32, 32, 16, 32, 64, 32, 64, 64, 64, 32 };
	static constexpr uint16_t sign[2] = { 0x0000, 0xFF00 };

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();

	// Flip de la tile porteuse de la condition (convention Mesen NES, comme TileNearby) :
	// les decalages s'inversent quand la tile evaluee est retournee.
	// 1) Cas normal : le PPU transmet l'orientation reelle via 'tile' (GetHdTile).
	// 2) Repli (appel sans contexte, ex. additions) : on cherche dans l'OAM le premier
	//    sprite qui recouvre (x, y) - moins fiable si des sprites se chevauchent.
	bool evaluatingHFlip = false;
	bool evaluatingVFlip = false;
	if(tile != nullptr) {
		evaluatingHFlip = tile->HFlip;
		evaluatingVFlip = tile->VFlip;
	} else for(uint8_t si = 0; si < 128; si++) {
		int soff = si * 4;
		uint8_t htv = oamRam[0x200 | (si >> 2)] >> ((si << 1) & 0x06);
		int16_t sx = (int16_t)(sign[htv & 0x01] | oamRam[soff]);
		uint8_t sy_raw = oamRam[soff + 1];
		if(sy_raw >= 240 && sy_raw != 255) continue;
		uint8_t smd = state.OamMode | ((htv & 0x02) << 2); // << 2 (et non << 4) : index 0-15 dans oamWidth/oamHeight
		int sw = oamWidth[smd];
		int sh = oamHeight[smd];
		if(x >= (int)sx && x < (int)(sx + sw) && y >= (int)sy_raw && y < (int)(sy_raw + sh)) {
			uint8_t fl = oamRam[soff + 3];
			evaluatingHFlip = (fl & 0x40) != 0;
			evaluatingVFlip = (fl & 0x80) != 0;
			break;
		}
	}

	int xSign = evaluatingHFlip ? -1 : 1;
	int ySign = evaluatingVFlip ? -1 : 1;
	int targetX = CheckAnyX ? -1 : (x + OffsetX * xSign);
	int targetY = CheckAnyY ? -1 : (y + OffsetY * ySign);

	// --- LOG DE DÉBOGAGE SUR FICHIER HARDCODÉ ---
	/*
	bool isFrame700 = (_screenInfo->FrameNumber == 700);
	std::ofstream debugLog;
	if(isFrame700) {
		debugLog.open("D:\\Documents\\Mesen2\\HdPacks\\Super Mario Kart (USA)\\sprite_nearby_debug.log", std::ios::app);
		debugLog << "=== Frame 700 | Condition [" << Name << "] ===" << std::endl;
		debugLog << "  Roue en (" << x << ", " << y << ") -> Cible en (" << targetX << ", " << targetY << ")" << std::endl;
		debugLog << "  Hash cherché : $" << HexUtilities::ToHex(MemoryHash) << std::endl;
	}
	*/
	
	bool foundHit = false;

	// --- BOUCLE SUR LES 128 SPRITES OAM ---
	for(uint8_t i = 0; i < 128; i++) {
		int offset = i * 4;

		uint8_t highTableValue = oamRam[0x200 | (i >> 2)] >> ((i << 1) & 0x06);
		int16_t spriteX = (int16_t)(sign[highTableValue & 0x01] | oamRam[offset + 0]);
		uint8_t spriteY = oamRam[offset + 1];

		if(spriteY >= 240 && spriteY != 255) continue;

		uint8_t mode = state.OamMode | ((highTableValue & 0x02) << 2);
		int sprWidth = oamWidth[mode];
		int sprHeight = oamHeight[mode];

		// 1. Hitbox du sprite global
		bool matchX = CheckAnyX || (targetX >= spriteX && targetX < (spriteX + sprWidth));
		bool matchY = CheckAnyY || (targetY >= spriteY && targetY < (spriteY + sprHeight));

		// 2. Si le point cible tombe dans la hitbox du sprite, on continue l'analyse
		if(matchX && matchY) {
			foundHit = true;

			uint16_t baseTileIndex = oamRam[offset + 2] | ((oamRam[offset + 3] & 0x01) << 8);

			uint8_t flags = oamRam[offset + 3];
			uint32_t snesPalette = (flags >> 1) & 0x07;

			bool hFlip = (flags & 0x40) != 0;
			bool vFlip = (flags & 0x80) != 0;

			// 2. Calcul du décalage de sous-tuile dans le sprite
			int relX = CheckAnyX ? 0 : (targetX - spriteX);
			int relY = CheckAnyY ? 0 : (targetY - spriteY);

			int subX = relX / 8;
			int subY = relY / 8;

			if(hFlip) subX = (sprWidth / 8 - 1) - subX;
			if(vFlip) subY = (sprHeight / 8 - 1) - subY;

			// 3. Calcul de l'index réel dans la grille VRAM 16x16 de la SNES
			uint16_t col = ((baseTileIndex & 0x0F) + subX) & 0x0F;
			uint16_t row = (((baseTileIndex & 0x1F0) >> 4) + subY) & 0x1F;
			uint16_t subTileIndex = (row << 4) | col;

			bool isTileMatch = false;
			uint64_t actualTileHash = 0;
			// MOUCHARD : Si l'émulateur croise la tuile 101, il écrit son vrai hash
			if(this->MemoryHash <= 0xFFFF) {
				isTileMatch = (this->MemoryHash == subTileIndex);
			} else {
				// 4. Calcul du Hash 64-bits de la sous-tuile 8x8 (32 octets en 4 BPP)
				uint32_t bpp = 4;
				uint32_t tileAddress = state.OamBaseAddress;
				if(subTileIndex >= 256) {
					tileAddress += state.OamAddressOffset;
				}
				// Calcul de l'adresse de départ de la sous-tuile dans la VRAM
				uint32_t tileStart = tileAddress + ((subTileIndex % 256) * 4 * bpp);

				uint8_t tileBuffer[32];
				for(size_t j = 0; j < 16; j++) {
					uint16_t word = vram[(tileStart + j) & 0x7FFF];
					tileBuffer[j * 2] = word & 0xFF;
					tileBuffer[j * 2 + 1] = word >> 8;
				}

				for(size_t j = 0; j < 32; j++) {
					actualTileHash = (actualTileHash * 31) + tileBuffer[j];
				}

				isTileMatch = (this->MemoryHash == actualTileHash);
			}

			// 5. Vérification finale du match avec la palette
			//if(isTileMatch && (this->IgnorePalette || this->PaletteHash == snesPalette )) {
			
			
			if(isTileMatch && (  this->IgnorePalette 
									|| this->PaletteHash == snesPalette 
									|| this->PaletteHash == (snesPalette + 8))) // Normalisation : si la palette renseignée est entre 8 et 15, on la ramène entre 0 et 7
			{
				return true ;
			}
			
		}
	}

	/* if(isFrame700) {		if(!foundHit) {	debugLog << "  => ÉCHEC : Aucun sprite sous le point cible." << std::endl;		}	debugLog.close();	}	*/
	return false;
}
//--------------------------------------------------------------------------------------------------
OverscanDimensions SnesHdVideoFilter::GetOverscan()
{
	// Si un pack HD est actif et qu'il possède le tag <overscan>
	if(SnesHdPackLoader::ActivePack != nullptr && SnesHdPackLoader::ActivePack->HasOverscanConfig) {
		return SnesHdPackLoader::ActivePack->Overscan;
	} else {
		// Sinon, on appelle gentiment l'overscan standard de la console parent
		return SnesDefaultVideoFilter::GetOverscan();
	}
}
//--------------------------------------------------------------------------------------------------