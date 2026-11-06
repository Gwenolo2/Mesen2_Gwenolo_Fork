#include "pch.h"
#include <vector>
#include <algorithm>
#include <thread>
#include <cmath>
#include <cstdio>
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/HdPacks/SnesHdPackLoader.h"
#include "SNES/HdPacks/SnesHdMode7Camera.h"
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
void BuildMode7MipChain(SnesHdPackBitmapInfo* bmp, uint32_t mapDim)
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
void BuildMode7TileMips(SnesHdMode7Image* img)
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

	// ------------------------------------------------------------------
	// COLOR WINDOW : zones dont l'effet SD est supprime (<disableAllColorMathWindow>,
	// <disableColorMathWindow>HASH). Dans ces zones, on utilise la couleur de l'ecran
	// principal AVANT color math (memorisee par le PPU) a la place du pixel SD final.
	// ------------------------------------------------------------------
	const uint16_t* cwPreMath = console->GetPpu()->GetColorWindowPreMathBuffer();
	const auto& cwRegions = SnesHdPackLoader::ActivePack->ColorWindowRegions;
	const auto& cwLabels = SnesHdPackLoader::ActivePack->ColorWindowLabels;
	std::vector<char> cwOff(cwRegions.size(), 0);
	bool anyCwOff = false;
	if(!cwRegions.empty() && cwLabels.size() == 256 * 240 && _baseFrameInfo.Width == 256 && cwPreMath) {
		auto condsOk = [](const std::vector<SnesHdPackCondition*>& conds) {
			for(auto* c : conds) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
			return true;
		};
		bool all = false;
		for(const auto& conds : SnesHdPackLoader::ActivePack->DisableAllColorMathWindowRules) {
			if(condsOk(conds)) { all = true; break; }
		}
		for(size_t i = 0; i < cwRegions.size(); i++) {
			if(all) { cwOff[i] = 1; }
			else {
				for(const auto& rule : SnesHdPackLoader::ActivePack->DisableColorMathWindowRules) {
					if(rule.Hash == cwRegions[i].Hash && condsOk(rule.Conditions)) { cwOff[i] = 1; break; }
				}
			}
			if(cwOff[i]) anyCwOff = true;
		}
	}
	auto cwPixel = [&](uint32_t row, uint32_t col, uint16_t px) -> uint16_t {
		if(!anyCwOff || row >= 240 || col >= 256) return px;
		int16_t l = cwLabels[row * 256 + col];
		return (l >= 0 && l < (int16_t)cwOff.size() && cwOff[l]) ? cwPreMath[row * 256 + col] : px;
	};

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
	std::vector<uint32_t> m7CellHash; // hash de la tile de chaque case (pour <m7relief>)
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
					if(!p.M7LargeMap) { mx &= 1023; my &= 1023; } // carte repetee : la case est repliee
					if(m7CellTile.empty()) { m7CellTile.assign(128 * 128, nullptr); m7CellHash.assign(128 * 128, 0); }
					if(mx < 0 || my < 0 || mx > 1023 || my > 1023) {
						m7OutsideTile = img;
					} else {
						m7CellTile[(size_t)(my >> 3) * 128 + (mx >> 3)] = img;
						m7CellHash[(size_t)(my >> 3) * 128 + (mx >> 3)] = p.M7TileHash;
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
		// (Journal du lissage retire : decommenter pour diagnostiquer)
		// MessageManager::Log("[HD-MODE7] Lissage perspective : " + fitReport);
		(void)fitPattern; (void)fitReport;
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
				: _calculatedPalette[cwPixel(snesY, x + srcX, ppuOutputBuffer[currentOffset])];
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
			uint16_t snesPixel = cwPixel(snesY, snesX, ppuOutputBuffer[bufferOffset]);

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
		int type;     // 0 = Background, 1 = Tile/Sprite, 2 = <2DOverlay>, 3 = <3DParticles>, 4 = <m7relief>, 5 = <m7billboard>, 6 = <m7model>
		int oamIndex; // Utilisé pour ordonner les sprites de même ZIndex
		size_t index; // Index dans le vecteur source
	};

	// ====================================================================
	// <2DOverlay> : SIMULATION DES PARTICULES (sequentielle, une fois par frame, avant la composition)
	// ====================================================================
	auto& hdOverlays = SnesHdPackLoader::ActivePack->Overlays;
	std::vector<char> ovActive(hdOverlays.size(), 0);
	{
		const float ovDt = 1.0f / 60.0f;
		const float kPi = 3.14159265f;
		auto ovSpawn = [&](SnesHdOverlayInfo& ov) {
			SnesHdParticle p;
			p.X = (ov.SpawnX + ov.Rand() * ov.SpawnW) * ov.FieldWidthPx;
			p.Y = (ov.SpawnY + ov.Rand() * ov.SpawnH) * ov.FieldHeightPx;
			float sp = ov.RandRange(ov.SpeedMin, ov.SpeedMax);
			float ang = ov.RandRange(ov.AngleMin, ov.AngleMax) * kPi / 180.0f;
			p.VX = std::cos(ang) * sp;
			p.VY = std::sin(ang) * sp;
			p.Life = std::max(0.05f, ov.RandRange(ov.LifeMin, ov.LifeMax));
			p.Size = 1.0f + ov.SizeRandom * (ov.Rand() * 2.0f - 1.0f);
			p.Rot = ov.RandRange(ov.RotationMin, ov.RotationMax) * kPi / 180.0f;
			p.Spin = ov.RandRange(ov.SpinMin, ov.SpinMax) * kPi / 180.0f;
			p.Seed1 = ov.Rand() * 2.0f * kPi;
			p.Seed2 = ov.Rand() * 2.0f * kPi;
			ov.Particles.push_back(p);
		};
		auto ovStep = [&](SnesHdOverlayInfo& ov) {
			ov.Time += ovDt;
			ov.SpawnAcc += ov.Rate * ovDt;
			while(ov.SpawnAcc >= 1.0f) {
				ov.SpawnAcc -= 1.0f;
				if((int)ov.Particles.size() < ov.MaxParticles) ovSpawn(ov);
			}
			const float tw = 2.0f * kPi * ov.TurbulenceFreq;
			for(size_t k = 0; k < ov.Particles.size();) {
				SnesHdParticle& p = ov.Particles[k];
				p.Age += ovDt;
				if(p.Age >= p.Life) { p = ov.Particles.back(); ov.Particles.pop_back(); continue; }
				p.VX += ov.GravityX * ovDt;
				p.VY += ov.GravityY * ovDt;
				float tx = 0, ty = 0;
				if(ov.Turbulence != 0.0f) {
					tx = ov.Turbulence * std::sin(tw * ov.Time + p.Seed1);
					ty = ov.Turbulence * std::cos(tw * 0.73f * ov.Time + p.Seed2);
				}
				p.X += (p.VX + ov.WindX + tx) * ovDt;
				p.Y += (p.VY + ov.WindY + ty) * ovDt;
				p.Rot += p.Spin * ovDt;
				p.X = std::fmod(p.X, ov.FieldWidthPx); if(p.X < 0) p.X += ov.FieldWidthPx;
				p.Y = std::fmod(p.Y, ov.FieldHeightPx); if(p.Y < 0) p.Y += ov.FieldHeightPx;
				k++;
			}
		};

		for(size_t i = 0; i < hdOverlays.size(); i++) {
			SnesHdOverlayInfo& ov = *hdOverlays[i];
			ov.Active = ov.MatchesCondition();
			if(!ov.Active) continue; // effet fige tant que sa condition est fausse
			ovActive[i] = 1;
			ov.FieldWidthPx = (float)cropWidth * ov.FieldW;
			ov.FieldHeightPx = (float)cropHeight * ov.FieldH;

			if(!ov.Initialized) {
				ov.Initialized = true;
				ov.Rng ^= (uint32_t)(i + 1) * 2654435761u;
				if(ov.Prewarm) {
					int steps = (int)std::min(ov.LifeMax * 60.0f, 900.0f);
					for(int s = 0; s < steps; s++) ovStep(ov);
				}
			}

			// Ancrage : le defilement de la couche (x SpeedX/SpeedY) S'AJOUTE au mouvement propre
			if(ov.Layer >= 1 && ov.Layer <= 4) {
				int li = ov.Layer - 1;
				uint32_t line = std::min<uint32_t>(srcY + cropHeight / 2, 239);
				int scx = SnesHdPackLoader::ActivePack->ScreenInfo.ScanlineHScroll[line][li];
				int scy = SnesHdPackLoader::ActivePack->ScreenInfo.ScanlineVScroll[line][li];
				if(ov.HaveScroll) {
					int dx = (((scx - ov.LastScrollX) % 1024) + 1536) % 1024 - 512; // registres de scroll sur 10 bits
					int dy = (((scy - ov.LastScrollY) % 1024) + 1536) % 1024 - 512;
					// Saut brutal (changement de salle/zone) : non repercute
					if(std::abs(dx) <= 64) ov.OffsetX += (float)dx * ov.SpeedX;
					if(std::abs(dy) <= 64) ov.OffsetY += (float)dy * ov.SpeedY;
					ov.OffsetX = std::fmod(ov.OffsetX, ov.FieldWidthPx);
					ov.OffsetY = std::fmod(ov.OffsetY, ov.FieldHeightPx);
				}
				ov.LastScrollX = scx;
				ov.LastScrollY = scy;
				ov.HaveScroll = true;
			}

			ovStep(ov);
		}
	}

	// Rendu d'un overlay sur une bande de lignes HD (appele par composeBand, en parallele)
	auto renderOverlayBand = [&](size_t ovIndex, uint32_t bandY0, uint32_t bandY1) {
		const SnesHdOverlayInfo& ov = *hdOverlays[ovIndex];
		const SnesHdPackBitmapInfo* tex = (ov.Texture && !ov.Texture->PixelData.empty() && ov.Texture->Width > 0) ? ov.Texture : nullptr;
		const float fW = ov.FieldWidthPx, fH = ov.FieldHeightPx;
		const float sc = (float)scale;
		const uint8_t z = (uint8_t)std::clamp(ov.ZIndex, 0, 255);
		const int texW = tex ? (int)tex->Width : 0, texH = tex ? (int)tex->Height : 0;

		for(const SnesHdParticle& p : ov.Particles) {
			const float t = p.Age / p.Life;
			const float sizeMul = p.Size * (ov.SizeStart + (ov.SizeEnd - ov.SizeStart) * t);
			if(sizeMul <= 0.001f) continue;
			float a = ov.AlphaStart + (ov.AlphaEnd - ov.AlphaStart) * t;
			if(ov.FadeIn > 0.0f && t < ov.FadeIn) a *= t / ov.FadeIn;
			if(ov.FadeOut > 0.0f && t > 1.0f - ov.FadeOut) a *= (1.0f - t) / ov.FadeOut;
			a *= ov.Alpha;
			if(a <= 0.002f) continue;

			auto lerpC = [&](int sh) {
				float c0 = (float)((ov.ColorStart >> sh) & 0xFF), c1 = (float)((ov.ColorEnd >> sh) & 0xFF);
				return (c0 + (c1 - c0) * t) / 255.0f;
			};
			const float tr = lerpC(16), tg = lerpC(8), tb = lerpC(0);

			float halfW, halfH;
			if(tex) { halfW = texW * 0.5f * sizeMul; halfH = texH * 0.5f * sizeMul; }
			else { halfW = halfH = ov.DotSize * sc * 0.5f * sizeMul; }
			const bool rotated = (p.Rot != 0.0f || p.Spin != 0.0f);
			const float radius = rotated ? std::sqrt(halfW * halfW + halfH * halfH) : std::max(halfW, halfH);
			const float cosR = std::cos(p.Rot), sinR = std::sin(p.Rot);

			// Position affichee (pixels SNES) : position propre - decalage d'ancrage, dans le champ torique
			float sx = std::fmod(p.X - ov.OffsetX, fW); if(sx < 0) sx += fW;
			float sy = std::fmod(p.Y - ov.OffsetY, fH); if(sy < 0) sy += fH;

			// Dessin dans le champ et ses voisins immediats -> aucune couture aux bords
			for(int cyc = -1; cyc <= 1; cyc++) {
				for(int cxc = -1; cxc <= 1; cxc++) {
					const float pcx = (sx + cxc * fW + (float)dstX) * sc;
					const float pcy = (sy + cyc * fH + (float)dstY) * sc;
					int x0 = std::max(0, (int)std::floor(pcx - radius));
					int x1 = std::min((int)outWidth - 1, (int)std::ceil(pcx + radius));
					int y0 = std::max((int)bandY0, (int)std::floor(pcy - radius));
					int y1 = std::min((int)bandY1 - 1, (int)std::ceil(pcy + radius));
					if(x0 > x1 || y0 > y1) continue;

					for(int yy = y0; yy <= y1; yy++) {
						const float dy = (float)yy + 0.5f - pcy;
						for(int xx = x0; xx <= x1; xx++) {
							const float dx = (float)xx + 0.5f - pcx;
							float lx = dx, ly = dy;
							if(rotated) { lx = dx * cosR + dy * sinR; ly = -dx * sinR + dy * cosR; }

							float sa, sr, sg, sb;
							if(tex) {
								float u = lx / sizeMul + texW * 0.5f - 0.5f;
								float v = ly / sizeMul + texH * 0.5f - 0.5f;
								if(u < -0.5f || v < -0.5f || u > texW - 0.5f || v > texH - 0.5f) continue;
								int iu = (int)std::floor(u), iv = (int)std::floor(v);
								float fu = u - iu, fv = v - iv;
								float acc[4] = { 0, 0, 0, 0 };
								const int qx[4] = { iu, iu + 1, iu, iu + 1 }, qy[4] = { iv, iv, iv + 1, iv + 1 };
								const float wq[4] = { (1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv };
								for(int q = 0; q < 4; q++) {
									int cxq = std::clamp(qx[q], 0, texW - 1), cyq = std::clamp(qy[q], 0, texH - 1);
									uint32_t c = tex->PixelData[(size_t)cyq * texW + cxq];
									float ca = (float)(c >> 24) * wq[q];
									acc[0] += ca; acc[1] += ((c >> 16) & 0xFF) * ca; acc[2] += ((c >> 8) & 0xFF) * ca; acc[3] += (c & 0xFF) * ca;
								}
								if(acc[0] <= 0.5f) continue;
								sa = acc[0] / 255.0f; sr = acc[1] / acc[0]; sg = acc[2] / acc[0]; sb = acc[3] / acc[0];
							} else {
								float d = std::sqrt(lx * lx + ly * ly) / halfW;
								if(d >= 1.0f) continue;
								float s = 1.0f - d * d;
								sa = s * s; sr = sg = sb = 255.0f;
							}

							const float aa = sa * a;
							if(aa < 0.004f) continue;
							const uint32_t idx = (uint32_t)yy * outWidth + (uint32_t)xx;
							if(z < _zBuffer[idx]) continue;

							const uint32_t dstC = out[idx];
							const float dr = (float)((dstC >> 16) & 0xFF), dg = (float)((dstC >> 8) & 0xFF), db = (float)(dstC & 0xFF);
							const float cr = sr * tr, cg = sg * tg, cb = sb * tb;
							float nr, ng, nb;
							if(ov.Additive) {
								nr = std::min(255.0f, dr + cr * aa); ng = std::min(255.0f, dg + cg * aa); nb = std::min(255.0f, db + cb * aa);
							} else {
								nr = dr + (cr - dr) * aa; ng = dg + (cg - dg) * aa; nb = db + (cb - db) * aa;
							}
							out[idx] = 0xFF000000 | ((uint32_t)(nr + 0.5f) << 16) | ((uint32_t)(ng + 0.5f) << 8) | (uint32_t)(nb + 0.5f);
						}
					}
				}
			}
		}
	};

	// ====================================================================
	// CAMERA MODE 7 RECONSTRUITE (socle 3D) + <3DParticles>
	// Voir SnesHdMode7Camera.h et docs/claude/10_Mode7_Camera_3D.md
	// ====================================================================
	auto& hdParts3D = SnesHdPackLoader::ActivePack->Particles3D;
	bool m7DebugOn = SnesHdPackLoader::ActivePack->M7CameraDebug;
	for(auto* c : SnesHdPackLoader::ActivePack->M7CameraDebugConditions) {
		if(!c->CheckCondition(0, 0, nullptr)) { m7DebugOn = false; break; }
	}
	std::vector<SnesHdM7Zone> m7Zones; // toutes les zones Mode 7 de l'ecran
	std::vector<int> m7Views;          // zones en perspective avec camera valide (vue 0 = haut, 1 = bas)
	if(!hdParts3D.empty() || m7DebugOn) {
		const SnesHdM7LineCapture* cap = console->GetPpu()->GetMode7LineCapture();
		std::vector<SnesHdM7CamLine> camLines(cropHeight);
		for(uint32_t y = 0; y < cropHeight && cap; y++) {
			uint32_t row = y + srcY;
			if(row >= 240 || !cap[row].Valid) continue;
			SnesHdM7CamLine& cl = camLines[y];
			cl.Valid = true;
			cl.LargeMap = cap[row].LargeMap != 0;
			const M7Line& L = m7Lines[y];
			if(L.UseFit && L.Inv > 1e-12) {
				// memes parametres lisses que le rendu Mode 7 HD (origine a x = 0 de l'image recadree)
				double scl = 1.0 / L.Inv;
				cl.Sx = L.DirX * scl / 256.0; cl.Sy = L.DirY * scl / 256.0;
				cl.Px = (L.U[0] + L.V[0] * scl) / 256.0; cl.Py = (L.U[1] + L.V[1] * scl) / 256.0;
				cl.Smoothed = true;
			} else {
				cl.Sx = cap[row].A / 256.0; cl.Sy = cap[row].C / 256.0;
				cl.Px = cap[row].StartX / 256.0 + cl.Sx * (double)srcX;
				cl.Py = cap[row].StartY / 256.0 + cl.Sy * (double)srcX;
			}
		}
		SnesHdBuildMode7Zones(camLines, (double)cropWidth * 0.5, m7Zones);
		for(size_t z = 0; z < m7Zones.size() && m7Views.size() < 2; z++) {
			if(m7Zones[z].Perspective && m7Zones[z].HasPinhole) m7Views.push_back((int)z);
		}
	}

	// Particule prete a dessiner (pixels HD), triee du plus loin au plus proche
	struct P3Draw { float Cx, Cy, HalfW, HalfH, A, R, G, B, Depth, GroundRow; };
	std::vector<std::vector<P3Draw>> p3Draws(hdParts3D.size() * 2);
	{
		const float dt = 1.0f / 60.0f;
		const float kPi = 3.14159265f;
		for(size_t pIdx = 0; pIdx < hdParts3D.size(); pIdx++) {
			SnesHdParticles3DInfo& pi = *hdParts3D[pIdx];
			const bool on = pi.MatchesCondition();
			for(int v = 0; v < 2; v++) {
				auto& st = pi.Views[v];
				st.Active = false;
				if(!on || v >= (int)m7Views.size()) continue; // effet fige
				if(pi.Zone >= 0 && pi.Zone != v) continue;
				const SnesHdM7Zone& zn = m7Zones[m7Views[v]];

				// Camera "depliee" : on cumule ses deplacements (carte VRAM repliee sur 1024)
				if(!st.HaveCam) {
					st.HaveCam = true; st.CamWX = 0; st.CamWY = 0;
				} else {
					double dx = std::remainder(zn.Cx - st.LastCx, 1024.0), dy = std::remainder(zn.Cy - st.LastCy, 1024.0);
					if(std::abs(dx) < 128.0 && std::abs(dy) < 128.0) { st.CamWX += dx; st.CamWY += dy; } // sinon : teleportation
				}
				st.LastCx = zn.Cx; st.LastCy = zn.Cy;
				const float B = pi.BoxSize;
				st.CenterX = st.CamWX + zn.Fx * B * pi.BoxAhead;
				st.CenterY = st.CamWY + zn.Fy * B * pi.BoxAhead;
				st.Active = true;

				auto spawn = [&](bool anyHeight) {
					SnesHdParticle3D p;
					p.X = (float)(st.CenterX + (pi.Rand() - 0.5f) * B);
					p.Y = (float)(st.CenterY + (pi.Rand() - 0.5f) * B);
					float fall = pi.RandRange(pi.FallMin, pi.FallMax);
					p.Z = anyHeight ? pi.Rand() * pi.BoxHeight : (fall >= 0 ? pi.BoxHeight : 0.0f);
					float ang = pi.Rand() * 2.0f * kPi, dr = pi.RandRange(pi.DriftMin, pi.DriftMax);
					p.VX = std::cos(ang) * dr; p.VY = std::sin(ang) * dr; p.VZ = -fall;
					p.Life = pi.LifeMax > 0 ? std::max(0.05f, pi.RandRange(pi.LifeMin, pi.LifeMax)) : 0.0f;
					p.Age = (anyHeight && p.Life > 0) ? pi.Rand() * p.Life : 0.0f;
					p.Size = 1.0f + pi.SizeRandom * (pi.Rand() * 2.0f - 1.0f);
					p.Seed1 = pi.Rand() * 2.0f * kPi; p.Seed2 = pi.Rand() * 2.0f * kPi;
					st.Parts.push_back(p);
				};
				auto step = [&]() {
					st.Time += dt;
					if(pi.Rate > 0) {
						st.SpawnAcc += pi.Rate * dt;
						while(st.SpawnAcc >= 1.0f) { st.SpawnAcc -= 1.0f; if((int)st.Parts.size() < pi.MaxParticles) spawn(false); }
					} else {
						while((int)st.Parts.size() < pi.MaxParticles) spawn(false);
					}
					const float tw = 2.0f * kPi * pi.TurbulenceFreq;
					for(size_t k = 0; k < st.Parts.size();) {
						SnesHdParticle3D& p = st.Parts[k];
						bool dead = false;
						if(p.Life > 0) { p.Age += dt; dead = p.Age >= p.Life; }
						if(!dead) {
							p.VZ -= pi.Gravity * dt;
							float tx = 0, ty = 0;
							if(pi.Turbulence != 0.0f) {
								tx = pi.Turbulence * std::sin(tw * st.Time + p.Seed1);
								ty = pi.Turbulence * std::cos(tw * 0.73f * st.Time + p.Seed2);
							}
							p.X += (p.VX + pi.WindX + tx) * dt;
							p.Y += (p.VY + pi.WindY + ty) * dt;
							p.Z += p.VZ * dt;
							if(p.Z <= 0.0f) {
								if(pi.GroundRespawn) p.Z += pi.BoxHeight; else dead = true;
							} else if(p.Z > pi.BoxHeight) {
								p.Z -= pi.BoxHeight;
							}
							// boite torique qui suit la camera
							double rx = p.X - st.CenterX; rx -= B * std::floor(rx / B + 0.5); p.X = (float)(st.CenterX + rx);
							double ry = p.Y - st.CenterY; ry -= B * std::floor(ry / B + 0.5); p.Y = (float)(st.CenterY + ry);
						}
						if(dead) { p = st.Parts.back(); st.Parts.pop_back(); continue; }
						k++;
					}
				};

				if(!st.Initialized) {
					st.Initialized = true;
					st.Parts.clear();
					pi.Rng ^= (uint32_t)(pIdx * 2 + v + 1) * 2654435761u;
					if(pi.Rate <= 0) {
						while((int)st.Parts.size() < pi.MaxParticles) spawn(true);
					} else {
						int steps = (int)std::min(pi.LifeMax > 0 ? pi.LifeMax * 60.0f : 300.0f, 600.0f);
						for(int s = 0; s < steps; s++) step();
					}
				}
				step();

				// Projection (sequentielle) -> liste de dessin triee par profondeur
				std::vector<P3Draw>& dl = p3Draws[pIdx * 2 + v];
				const float sc = (float)scale;
				const float texAspect = (pi.Texture && pi.Texture->Width > 0) ? (float)pi.Texture->Height / (float)pi.Texture->Width : 1.0f;
				for(const SnesHdParticle3D& p : st.Parts) {
					double u = p.X - st.CamWX + zn.Cx, w = p.Y - st.CamWY + zn.Cy;
					double sx, sy, ppt;
					if(!zn.Project(u, w, p.Z, sx, sy, ppt)) continue;
					double D = zn.DepthOf(u, w);
					if(D < 2.0) continue;
					float t = p.Life > 0 ? p.Age / p.Life : 0.0f;
					float a = pi.AlphaStart + (pi.AlphaEnd - pi.AlphaStart) * t;
					if(p.Life > 0) {
						if(pi.FadeIn > 0 && t < pi.FadeIn) a *= t / pi.FadeIn;
						if(pi.FadeOut > 0 && t > 1.0f - pi.FadeOut) a *= (1.0f - t) / pi.FadeOut;
					}
					if(pi.NearFade > 0 && D < pi.NearFade) a *= (float)(D / pi.NearFade);
					float r = (float)std::max(std::abs(p.X - st.CenterX), std::abs(p.Y - st.CenterY)) / (B * 0.5f);
					if(pi.FarFade > 0 && r > 1.0f - pi.FarFade) a *= std::max(0.0f, (1.0f - r) / pi.FarFade);
					float sizePx = p.Size * (pi.SizeStart + (pi.SizeEnd - pi.SizeStart) * t) * (float)ppt * sc;
					if(sizePx <= 0.0f) continue;
					if(sizePx < pi.MinPixelSize) { a *= sizePx / pi.MinPixelSize; sizePx = pi.MinPixelSize; }
					a *= pi.Alpha;
					if(a <= 0.003f) continue;
					auto lerpC = [&](int sh) {
						float c0 = (float)((pi.ColorStart >> sh) & 0xFF), c1 = (float)((pi.ColorEnd >> sh) & 0xFF);
						return (c0 + (c1 - c0) * t) / 255.0f;
					};
					P3Draw d;
					d.Cx = (float)((sx + dstX) * sc); d.Cy = (float)((sy + dstY) * sc);
					d.HalfW = sizePx * 0.5f; d.HalfH = d.HalfW * texAspect;
					d.A = std::min(a, 1.0f); d.R = lerpC(16); d.G = lerpC(8); d.B = lerpC(0); d.Depth = (float)D;
					d.GroundRow = (float)(sy + p.Z * ppt); // ligne d'ecran du point de sol sous la particule
					dl.push_back(d);
				}
				std::sort(dl.begin(), dl.end(), [](const P3Draw& a, const P3Draw& b) { return a.Depth > b.Depth; });
			}
		}
	}

	// ====================================================================
	// <3DParticles> : OCCLUSION PAR LES SPRITES
	// Un sprite pose sur le sol Mode 7 : la ligne de ses pieds (bas des pixels opaques de son
	// groupe d'objets OAM qui se touchent) donne sa profondeur. Carte "ligne des pieds" par pixel HD :
	//  - sprites SD (sans tile HD) : remplie ici depuis le releve des pixels ;
	//  - sprites HD : ecrite par composeBand en dessinant leurs tiles (pixels alpha >= 128).
	// Au dessin, une particule dont le point de sol est plus haut a l'ecran (= plus loin) que les
	// pieds du sprite qui couvre ce pixel est cachee.
	// ====================================================================
	// ====================================================================
	// <m7relief> : PREPARATION (sequentielle) - relief de chaque case visible de la carte Mode 7
	// ====================================================================
	auto& hdReliefs = SnesHdPackLoader::ActivePack->M7Reliefs;
	std::vector<SnesHdM7ReliefInfo*> cellRelief;
	float reliefMaxH = 0.0f;
	if(!hdReliefs.empty() && !m7CellHash.empty() && !m7Views.empty()) {
		std::unordered_map<uint32_t, SnesHdM7ReliefInfo*> byHash;
		for(size_t c = 0; c < m7CellHash.size(); c++) {
			uint32_t h = m7CellHash[c];
			if(!h) continue;
			SnesHdM7ReliefInfo* rl = nullptr;
			auto itc = byHash.find(h);
			if(itc != byHash.end()) {
				rl = itc->second;
			} else {
				auto it = hdReliefs.find(h);
				if(it != hdReliefs.end()) {
					for(auto& cand : it->second) { if(cand->MatchesCondition()) { rl = cand.get(); break; } }
				}
				if(rl) { rl->Build(); reliefMaxH = std::max(reliefMaxH, rl->Height); }
				byHash[h] = rl;
			}
			if(!rl) continue;
			if(cellRelief.empty()) cellRelief.assign(128 * 128, nullptr);
			cellRelief[c] = rl;
		}
	}
	const bool reliefOn = !cellRelief.empty() && reliefMaxH > 0.0f;

	// ====================================================================
	// <m7billboard> : PREPARATION (sequentielle) - projection des panneaux de chaque vue
	// ====================================================================
	struct BBDraw { const SnesHdM7BillboardInfo* B; float Cx, Bottom, W, H, A, GroundRow, Depth; int Level; };
	auto& hdBillboards = SnesHdPackLoader::ActivePack->M7Billboards;
	std::vector<std::vector<BBDraw>> bbDraws(m7Views.size());
	bool bbOn = false;
	if(!hdBillboards.empty()) {
		for(size_t v = 0; v < m7Views.size(); v++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
			const uint32_t viewMap = m7Lines[zn.Y1].Img ? m7Lines[zn.Y1].Ref.M7MapHash : 0;
			std::vector<BBDraw>& dl = bbDraws[v];
			for(auto& ub : hdBillboards) {
				SnesHdM7BillboardInfo& bb = *ub;
				if(!bb.AnyMap && bb.MapHash != viewMap) continue;
				if(!bb.MatchesCondition()) continue;
				bb.Build();
				if(bb.Mips.empty()) continue;
				auto place = [&](double u0, double v0) {
					// instance de la carte (repliee sur 1024) la plus proche de la camera
					double u = zn.LargeMap ? u0 : u0 + 1024.0 * std::round((zn.Cx - u0) / 1024.0);
					double w = zn.LargeMap ? v0 : v0 + 1024.0 * std::round((zn.Cy - v0) / 1024.0);
					double sx, sy, ppt;
					if(!zn.Project(u, w, bb.Elevation, sx, sy, ppt)) return;
					double D = zn.DepthOf(u, w);
					if(D < 4.0) return;
					float hPx = (float)(bb.Height * ppt * scale);
					if(hPx < 1.0f) return;
					float wPx = hPx * (float)bb.MipW[0] / (float)bb.MipH[0];
					float cx = (float)((sx + dstX) * scale);
					if(cx + wPx * 0.5f < 0 || cx - wPx * 0.5f > (float)outWidth) return;
					BBDraw d;
					d.B = &bb; d.Cx = cx; d.Bottom = (float)((sy + dstY) * scale); d.W = wPx; d.H = hPx;
					d.GroundRow = (float)(sy + bb.Elevation * ppt);
					// fondu pres de la ligne la plus lointaine (sinon le panneau "pop" a l'horizon)
					float fr = std::clamp((d.GroundRow - (float)zn.Y0) / 6.0f, 0.0f, 1.0f);
					d.A = bb.Alpha * fr;
					if(d.A <= 0.003f) return;
					d.Depth = (float)D;
					float ratio = (float)bb.MipH[0] / hPx;
					int lvl = ratio > 1.0f ? (int)std::floor(std::log2(ratio)) : 0;
					d.Level = std::clamp(lvl, 0, (int)bb.Mips.size() - 1);
					dl.push_back(d);
				};
				if(bb.OnTile) {
					if(m7CellHash.empty()) continue;
					for(size_t c = 0; c < m7CellHash.size(); c++) {
						if(m7CellHash[c] == bb.TileHash) place((double)((c % 128) * 8 + 4), (double)((c / 128) * 8 + 4));
					}
				} else {
					place(bb.X, bb.Y);
				}
			}
			std::sort(dl.begin(), dl.end(), [](const BBDraw& a, const BBDraw& b) { return a.Depth > b.Depth; });
			if(!dl.empty()) bbOn = true;
		}
	}

	// ====================================================================
	// <m7model> : PREPARATION (sequentielle) - sommets des modeles 3D projetes par vue
	// Repere : OBJ (x, y vers le haut, z) -> carte (u = x, hauteur = y, v = z), meme orientation.
	// ====================================================================
	struct MVert { float X, Y, W, GR, U, V, L; }; // ecran HD, 1/profondeur, ligne du sol, texture, lumiere
	struct MTriDraw { MVert P[3]; const SnesHdModelMaterial* M; float A; };
	auto& hdModels = SnesHdPackLoader::ActivePack->M7Models;
	std::vector<std::vector<MTriDraw>> mdDraws(m7Views.size());
	bool mdOn = false;
	if(!hdModels.empty()) {
		// lumiere fixe dans le monde : du nord-ouest, en hauteur (u, v, h)
		const float lx = -0.45f, lv = -0.35f, lh = 0.82f;
		const float ln = std::sqrt(lx * lx + lv * lv + lh * lh);
		for(size_t vw = 0; vw < m7Views.size(); vw++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[vw]];
			const uint32_t viewMap = m7Lines[zn.Y1].Img ? m7Lines[zn.Y1].Ref.M7MapHash : 0;
			std::vector<MTriDraw>& dl = mdDraws[vw];
			for(auto& um : hdModels) {
				SnesHdM7ModelInfo& md = *um;
				if(!md.Mesh || (!md.AnyMap && md.MapHash != viewMap)) continue;
				if(!md.MatchesCondition()) continue;
				const SnesHdModelMesh& mesh = *md.Mesh;
				const float ox = md.KeepOrigin ? 0.0f : (mesh.MinX + mesh.MaxX) * 0.5f;
				const float oy = md.KeepOrigin ? 0.0f : mesh.MinY;
				const float oz = md.KeepOrigin ? 0.0f : (mesh.MinZ + mesh.MaxZ) * 0.5f;
				const float cr = std::cos(md.Rotation * 0.01745329f), sr = std::sin(md.Rotation * 0.01745329f);
				auto place = [&](double u0, double v0) {
					double U0 = zn.LargeMap ? u0 : u0 + 1024.0 * std::round((zn.Cx - u0) / 1024.0);
					double V0 = zn.LargeMap ? v0 : v0 + 1024.0 * std::round((zn.Cy - v0) / 1024.0);
					// fondu pres de la ligne la plus lointaine (comme les panneaux)
					double asx, asy, appt;
					float fade = 1.0f;
					if(zn.Project(U0, V0, 0.0, asx, asy, appt)) fade = std::clamp((float)(asy - zn.Y0) / 6.0f, 0.0f, 1.0f);
					for(const SnesHdModelTri& t : mesh.Tris) {
						const SnesHdModelMaterial& mat = mesh.Mats[std::clamp(t.Mat, 0, (int)mesh.Mats.size() - 1)];
						const float a = mat.Alpha * fade;
						if(a <= 0.003f) continue;
						MTriDraw d; d.M = &mat; d.A = a;
						bool ok = true;
						for(int k = 0; k < 3 && ok; k++) {
							float x = (t.P[k][0] - ox) * md.Scale, y = (t.P[k][1] - oy) * md.Scale, z = (t.P[k][2] - oz) * md.Scale;
							double u = U0 + x * cr - z * sr, w = V0 + x * sr + z * cr, h = md.Elevation + y;
							double sx, sy, ppt;
							if(!zn.Project(u, w, h, sx, sy, ppt)) { ok = false; break; }
							double D = zn.DepthOf(u, w);
							if(D < 2.0) { ok = false; break; }
							float nu = t.N[k][0] * cr - t.N[k][2] * sr, nv = t.N[k][0] * sr + t.N[k][2] * cr, nh = t.N[k][1];
							float nl = std::sqrt(nu * nu + nv * nv + nh * nh);
							float dotL = nl > 1e-6f ? std::abs(nu * lx + nv * lv + nh * lh) / (nl * ln) : 1.0f; // deux faces
							MVert& mv = d.P[k];
							mv.X = (float)((sx + dstX) * scale); mv.Y = (float)((sy + dstY) * scale);
							mv.W = (float)(1.0 / D);
							mv.GR = (float)(sy + h * ppt);
							mv.U = t.UV[k][0]; mv.V = t.UV[k][1];
							mv.L = md.Ambient + (1.0f - md.Ambient) * dotL;
						}
						if(!ok) continue;
						float minX = std::min({ d.P[0].X, d.P[1].X, d.P[2].X }), maxX = std::max({ d.P[0].X, d.P[1].X, d.P[2].X });
						if(maxX < 0 || minX > (float)outWidth) continue;
						dl.push_back(d);
					}
				};
				if(md.OnTile) {
					if(m7CellHash.empty()) continue;
					for(size_t c = 0; c < m7CellHash.size(); c++) {
						if(m7CellHash[c] == md.TileHash) place((double)((c % 128) * 8 + 4), (double)((c / 128) * 8 + 4));
					}
				} else {
					place(md.X, md.Y);
				}
			}
			if(!dl.empty()) mdOn = true;
		}
	}
	static std::vector<float> s_modelW; // 1/profondeur des pixels de modele (plus grand = plus proche)
	if(mdOn) s_modelW.assign((size_t)outWidth * hdHeight, 0.0f);

	static std::vector<float> s_sprRow;
	bool p3Occlusion = reliefOn || bbOn || mdOn; // carte des sprites utile aussi au relief, aux panneaux et aux modeles
	for(auto& dl : p3Draws) { if(!dl.empty()) { p3Occlusion = true; break; } }
	float sprGroupRow[128];
	for(float& r : sprGroupRow) r = -1.0f;
	if(p3Occlusion) {
		int parent[128];
		for(int i = 0; i < 128; i++) parent[i] = i;
		auto findRoot = [&](int a) { while(parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
		int bottom[128];
		for(int& b : bottom) b = -1;
		const uint32_t fw = _baseFrameInfo.Width;
		auto opaqueOam = [&](uint32_t x, uint32_t y) -> int {
			const SnesHdPixelData& px = hdBuffer[(y + srcY) * fw + x + srcX];
			return (px.IsSprite && px.OamIndex < 128 && px.BaseColor != 0) ? (int)px.OamIndex : -1;
		};
		for(uint32_t y = 0; y < cropHeight; y++) {
			for(uint32_t x = 0; x < cropWidth; x++) {
				int o = opaqueOam(x, y);
				if(o < 0) continue;
				bottom[o] = std::max(bottom[o], (int)y + 1);
				if(x + 1 < cropWidth) { int o2 = opaqueOam(x + 1, y); if(o2 >= 0 && o2 != o) parent[findRoot(o2)] = findRoot(o); }
				if(y + 1 < cropHeight) { int o2 = opaqueOam(x, y + 1); if(o2 >= 0 && o2 != o) parent[findRoot(o2)] = findRoot(o); }
			}
		}
		int groupBottom[128];
		for(int& b : groupBottom) b = -1;
		for(int i = 0; i < 128; i++) {
			if(bottom[i] >= 0) { int r = findRoot(i); groupBottom[r] = std::max(groupBottom[r], bottom[i]); }
		}
		for(int i = 0; i < 128; i++) {
			if(bottom[i] < 0) continue;
			int feet = groupBottom[findRoot(i)];
			// seuls les sprites dont les pieds sont sur le sol d'une vue cachent quelque chose
			for(int v : m7Views) {
				const SnesHdM7Zone& zn = m7Zones[v];
				if(feet > zn.Y0 + 1 && feet <= zn.Y1 + 1) { sprGroupRow[i] = (float)feet; break; }
			}
		}
		s_sprRow.assign((size_t)outWidth * hdHeight, -1.0f);
		for(uint32_t y = 0; y < cropHeight; y++) {
			for(uint32_t x = 0; x < cropWidth; x++) {
				const SnesHdPixelData& px = hdBuffer[(y + srcY) * fw + x + srcX];
				if(!px.IsSprite || px.OamIndex >= 128 || px.Tile != nullptr || px.BaseColor == 0) continue;
				float r = sprGroupRow[px.OamIndex];
				if(r < 0) continue;
				for(uint32_t sy2 = 0; sy2 < scale; sy2++) {
					uint32_t hy = (y + dstY) * scale + sy2;
					if(hy >= hdHeight) break;
					for(uint32_t sx2 = 0; sx2 < scale; sx2++) {
						uint32_t hx = (x + dstX) * scale + sx2;
						if(hx < outWidth) s_sprRow[(size_t)hy * outWidth + hx] = r;
					}
				}
			}
		}
	}

	// ====================================================================
	// <m7relief> : RENDU "VOXEL SPACE" d'une vue sur une bande de lignes HD (appele par composeBand)
	// Par colonne HD : on remonte le sol du bas de la vue vers l'horizon ; chaque point en relief
	// est projete (hauteur x pixels-par-texel) et remplit la colonne jusqu'au "plafond" deja dessine.
	// Ligne du dessus = couleur du dessus, en dessous = flanc (assombri). Les sprites plus proches
	// (ligne des pieds) restent devant ; le relief dessine devient a son tour un occulteur.
	// ====================================================================
	const uint8_t kReliefZ = 90;
	auto renderReliefBand = [&](size_t viewIndex, uint32_t bandY0, uint32_t bandY1) {
		const SnesHdM7Zone& zn = m7Zones[m7Views[viewIndex]];
		const int sc = (int)scale;
		const int zoneTopHD = (zn.Y0 + (int)dstY) * sc;
		const int zoneBotHD = (zn.Y1 + 1 + (int)dstY) * sc; // exclu
		const int areaTopHD = (zn.AreaY0 + (int)dstY) * sc;
		const int reach = (int)std::ceil(reliefMaxH * zn.Inv[zn.Count() - 1] * sc) + 2;
		const int rStart = std::min(zoneBotHD - 1, (int)bandY1 - 1 + reach);
		const int rEnd = std::max(zoneTopHD, (int)bandY0);
		if(rStart < rEnd) return;
		const int bandLo = std::max((int)bandY0, areaTopHD), bandHi = std::min((int)bandY1, zoneBotHD) - 1;
		if(bandLo > bandHi) return;

		for(int hx = 0; hx < (int)outWidth; hx++) {
			const double sx = ((double)hx + 0.5) / sc - (double)dstX;
			int ceiling = rStart + 1;
			for(int r = rStart; r >= rEnd; r--) {
				const double sy = ((double)r + 0.5) / sc - (double)dstY;
				double u, v;
				if(!zn.Unproject(sx, sy, u, v)) { ceiling = std::min(ceiling, r); continue; }
				const double fu = std::floor(u), fv = std::floor(v);
				if(zn.LargeMap && (fu < 0 || fv < 0 || fu > 1023 || fv > 1023)) { ceiling = std::min(ceiling, r); continue; } // hors carte (tile 0)
				const int mu = ((int)fu) & 1023, mv = ((int)fv) & 1023;
				const size_t cell = (size_t)(mv >> 3) * 128 + (size_t)(mu >> 3);
				SnesHdM7ReliefInfo* rl = cellRelief[cell];
				if(!rl) { ceiling = std::min(ceiling, r); continue; }
				// position dans la tile (0..1)
				const double tu = ((double)(mu & 7) + (u - fu)) / 8.0, tv = ((double)(mv & 7) + (v - fv)) / 8.0;
				const int rs = rl->Size;
				const int rx = std::clamp((int)(tu * rs), 0, rs - 1), ry = std::clamp((int)(tv * rs), 0, rs - 1);
				const uint8_t hv = rl->H[(size_t)ry * rs + rx];
				if(hv == 0) { ceiling = std::min(ceiling, r); continue; }
				const double hgt = rl->Height * (hv / 255.0);
				const int yt = std::max(areaTopHD, (int)std::floor((double)r + 0.5 - hgt * zn.InvAt(sy) * sc));
				if(yt >= ceiling) continue;

				// couleur du dessus : tile HD affichee dans cette case (animations comprises) ou PNG de relief
				uint32_t topC;
				SnesHdMode7Image* timg = m7CellTile.empty() ? nullptr : m7CellTile[cell];
				if(!rl->ColorFromPng && timg && !timg->TileMips.empty()) {
					const int dim = 8 * (int)timg->Scale;
					const int tx = std::clamp((int)(tu * dim), 0, dim - 1), ty = std::clamp((int)(tv * dim), 0, dim - 1);
					topC = timg->TileMips[0][(size_t)ty * dim + tx];
				} else {
					topC = rl->C[(size_t)ry * rs + rx];
				}
				const float side = rl->Sides;
				const uint32_t sideC = 0xFF000000 |
					((uint32_t)std::min(255.0f, ((topC >> 16) & 0xFF) * side) << 16) |
					((uint32_t)std::min(255.0f, ((topC >> 8) & 0xFF) * side) << 8) |
					(uint32_t)std::min(255.0f, (topC & 0xFF) * side);
				const float groundRow = (float)sy;
				const int y0 = std::max(yt, bandLo), y1 = std::min(ceiling - 1, bandHi);
				for(int yy = y0; yy <= y1; yy++) {
					const uint32_t idx = (uint32_t)yy * outWidth + (uint32_t)hx;
					if(kReliefZ < _zBuffer[idx]) continue;
					if(p3Occlusion) {
						const float sr2 = s_sprRow[idx];
						if(sr2 >= 0.0f && groundRow < sr2) continue; // sprite plus proche
						s_sprRow[idx] = groundRow;                    // le relief cache ce qui est plus loin
					}
					out[idx] = (yy == yt) ? (topC | 0xFF000000) : sideC;
					_zBuffer[idx] = kReliefZ;
				}
				ceiling = yt;
			}
		}
	};

	// ====================================================================
	// <m7billboard> : rendu des panneaux d'une vue sur une bande de lignes HD (appele par composeBand)
	// Du plus loin au plus proche ; un sprite ou un relief plus proche (ligne des pieds) reste devant ;
	// les pixels opaques du panneau deviennent a leur tour des occulteurs (particules, panneaux lointains).
	// ====================================================================
	auto renderBillboardBand = [&](size_t viewIndex, uint32_t bandY0, uint32_t bandY1) {
		const SnesHdM7Zone& zn = m7Zones[m7Views[viewIndex]];
		const int sc = (int)scale;
		const int clipY0 = std::max((int)bandY0, (zn.AreaY0 + (int)dstY) * sc);
		const int clipY1 = std::min((int)bandY1, (zn.AreaY1 + 1 + (int)dstY) * sc) - 1;
		if(clipY0 > clipY1) return;
		for(const BBDraw& d : bbDraws[viewIndex]) {
			const SnesHdM7BillboardInfo& bb = *d.B;
			const std::vector<uint32_t>& img = bb.Mips[d.Level];
			const int mw = (int)bb.MipW[d.Level], mh = (int)bb.MipH[d.Level];
			const float left = d.Cx - d.W * 0.5f, top = d.Bottom - d.H;
			const int x0 = std::max(0, (int)std::floor(left)), x1 = std::min((int)outWidth - 1, (int)std::ceil(left + d.W));
			const int y0 = std::max(clipY0, (int)std::floor(top)), y1 = std::min(clipY1, (int)std::ceil(d.Bottom) - 1);
			if(x0 > x1 || y0 > y1) continue;
			for(int yy = y0; yy <= y1; yy++) {
				const float tv = ((float)yy + 0.5f - top) / d.H * mh - 0.5f;
				if(tv < -0.5f || tv > mh - 0.5f) continue;
				const int iv = (int)std::floor(tv);
				const float fv = tv - iv;
				for(int xx = x0; xx <= x1; xx++) {
					const float tu = ((float)xx + 0.5f - left) / d.W * mw - 0.5f;
					if(tu < -0.5f || tu > mw - 0.5f) continue;
					const int iu = (int)std::floor(tu);
					const float fu = tu - iu;
					float acc[4] = { 0, 0, 0, 0 };
					const int qx[4] = { iu, iu + 1, iu, iu + 1 }, qy[4] = { iv, iv, iv + 1, iv + 1 };
					const float wq[4] = { (1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv };
					for(int q = 0; q < 4; q++) {
						int cxq = std::clamp(qx[q], 0, mw - 1), cyq = std::clamp(qy[q], 0, mh - 1);
						uint32_t c = img[(size_t)cyq * mw + cxq];
						float ca = (float)(c >> 24) * wq[q];
						acc[0] += ca; acc[1] += ((c >> 16) & 0xFF) * ca; acc[2] += ((c >> 8) & 0xFF) * ca; acc[3] += (c & 0xFF) * ca;
					}
					if(acc[0] <= 0.5f) continue;
					const float aa = (acc[0] / 255.0f) * d.A;
					if(aa < 0.004f) continue;
					const uint32_t idx = (uint32_t)yy * outWidth + (uint32_t)xx;
					if(kReliefZ < _zBuffer[idx]) continue;
					if(p3Occlusion) {
						const float sr2 = s_sprRow[idx];
						if(sr2 >= 0.0f && d.GroundRow < sr2) continue; // sprite / relief plus proche
						if(aa >= 0.5f) s_sprRow[idx] = d.GroundRow;
					}
					const float sr = acc[1] / acc[0], sg = acc[2] / acc[0], sb = acc[3] / acc[0];
					const uint32_t dstC = out[idx];
					const float dr = (float)((dstC >> 16) & 0xFF), dg = (float)((dstC >> 8) & 0xFF), db = (float)(dstC & 0xFF);
					const float nr = dr + (sr - dr) * aa, ng = dg + (sg - dg) * aa, nb = db + (sb - db) * aa;
					out[idx] = 0xFF000000 | ((uint32_t)(nr + 0.5f) << 16) | ((uint32_t)(ng + 0.5f) << 8) | (uint32_t)(nb + 0.5f);
					if(aa >= 0.5f) _zBuffer[idx] = kReliefZ;
				}
			}
		}
	};

	// ====================================================================
	// <m7model> : rasterisation des triangles d'une vue sur une bande de lignes HD (appele par composeBand)
	// Tampon de profondeur entre modeles (1/profondeur), texture corrigee en perspective, eclairage par
	// sommet, test alpha ; ordre avec sprites / relief / panneaux par la "ligne du sol" (s_sprRow).
	// ====================================================================
	auto renderModelBand = [&](size_t viewIndex, uint32_t bandY0, uint32_t bandY1) {
		const SnesHdM7Zone& zn = m7Zones[m7Views[viewIndex]];
		const int sc = (int)scale;
		const int clipY0 = std::max((int)bandY0, (zn.AreaY0 + (int)dstY) * sc);
		const int clipY1 = std::min((int)bandY1, (zn.AreaY1 + 1 + (int)dstY) * sc) - 1;
		if(clipY0 > clipY1) return;
		for(const MTriDraw& d : mdDraws[viewIndex]) {
			const MVert& a = d.P[0]; const MVert& b = d.P[1]; const MVert& c = d.P[2];
			const float area = (b.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (b.Y - a.Y);
			if(std::abs(area) < 1e-4f) continue;
			const float invArea = 1.0f / area;
			const int x0 = std::max(0, (int)std::floor(std::min({ a.X, b.X, c.X })));
			const int x1 = std::min((int)outWidth - 1, (int)std::ceil(std::max({ a.X, b.X, c.X })));
			const int y0 = std::max(clipY0, (int)std::floor(std::min({ a.Y, b.Y, c.Y })));
			const int y1 = std::min(clipY1, (int)std::ceil(std::max({ a.Y, b.Y, c.Y })));
			if(x0 > x1 || y0 > y1) continue;
			const SnesHdPackBitmapInfo* tex = (d.M->Texture && !d.M->Texture->PixelData.empty() && d.M->Texture->Width > 0) ? d.M->Texture : nullptr;
			const int tw = tex ? (int)tex->Width : 0, th = tex ? (int)tex->Height : 0;
			const float kr = (float)((d.M->Kd >> 16) & 0xFF) / 255.0f, kg = (float)((d.M->Kd >> 8) & 0xFF) / 255.0f, kb = (float)(d.M->Kd & 0xFF) / 255.0f;
			for(int yy = y0; yy <= y1; yy++) {
				const float py = (float)yy + 0.5f;
				for(int xx = x0; xx <= x1; xx++) {
					const float px = (float)xx + 0.5f;
					float w0 = ((b.X - px) * (c.Y - py) - (c.X - px) * (b.Y - py)) * invArea;
					float w1 = ((c.X - px) * (a.Y - py) - (a.X - px) * (c.Y - py)) * invArea;
					float w2 = 1.0f - w0 - w1;
					if(w0 < 0 || w1 < 0 || w2 < 0) continue;
					const float W = w0 * a.W + w1 * b.W + w2 * c.W;
					if(W <= 0) continue;
					const uint32_t idx = (uint32_t)yy * outWidth + (uint32_t)xx;
					if(W <= s_modelW[idx]) continue;              // un autre modele est plus proche
					if(kReliefZ < _zBuffer[idx]) continue;
					const float iW = 1.0f / W;
					const float gr = (w0 * a.GR * a.W + w1 * b.GR * b.W + w2 * c.GR * c.W) * iW;
					if(p3Occlusion) { const float sr2 = s_sprRow[idx]; if(sr2 >= 0.0f && gr < sr2) continue; }
					float r = kr * 255.0f, g = kg * 255.0f, bl = kb * 255.0f, al = 1.0f;
					if(tex) {
						float tu = (w0 * a.U * a.W + w1 * b.U * b.W + w2 * c.U * c.W) * iW;
						float tv = (w0 * a.V * a.W + w1 * b.V * b.W + w2 * c.V * c.W) * iW;
						tu = tu - std::floor(tu); tv = tv - std::floor(tv);
						float fx = tu * tw - 0.5f, fy = (1.0f - tv) * th - 0.5f; // OBJ : v vers le haut
						int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
						float ax = fx - ix, ay = fy - iy;
						float acc[4] = { 0, 0, 0, 0 };
						const int qx[4] = { ix, ix + 1, ix, ix + 1 }, qy[4] = { iy, iy, iy + 1, iy + 1 };
						const float wq[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
						for(int q = 0; q < 4; q++) {
							int cx = ((qx[q] % tw) + tw) % tw, cy = ((qy[q] % th) + th) % th;
							uint32_t col = tex->PixelData[(size_t)cy * tw + cx];
							float ca = (float)(col >> 24) * wq[q];
							acc[0] += ca; acc[1] += ((col >> 16) & 0xFF) * ca; acc[2] += ((col >> 8) & 0xFF) * ca; acc[3] += (col & 0xFF) * ca;
						}
						if(acc[0] < 127.5f) continue; // decoupe (feuillages, grillages)
						al = 1.0f;
						r *= acc[1] / acc[0] / 255.0f; g *= acc[2] / acc[0] / 255.0f; bl *= acc[3] / acc[0] / 255.0f;
					}
					const float lit = w0 * a.L + w1 * b.L + w2 * c.L;
					r = std::min(255.0f, r * lit); g = std::min(255.0f, g * lit); bl = std::min(255.0f, bl * lit);
					const float aa = al * d.A;
					const uint32_t dstC = out[idx];
					const float dr = (float)((dstC >> 16) & 0xFF), dg = (float)((dstC >> 8) & 0xFF), db = (float)(dstC & 0xFF);
					const float nr = dr + (r - dr) * aa, ng = dg + (g - dg) * aa, nb = db + (bl - db) * aa;
					out[idx] = 0xFF000000 | ((uint32_t)(nr + 0.5f) << 16) | ((uint32_t)(ng + 0.5f) << 8) | (uint32_t)(nb + 0.5f);
					if(aa >= 0.5f) {
						s_modelW[idx] = W;
						_zBuffer[idx] = kReliefZ;
						if(p3Occlusion) s_sprRow[idx] = gr;
					}
				}
			}
		}
	};

	auto render3DBand = [&](size_t drawIndex, uint32_t bandY0, uint32_t bandY1) {
		const SnesHdParticles3DInfo& pi = *hdParts3D[drawIndex / 2];
		const int v = (int)(drawIndex % 2);
		const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
		const std::vector<P3Draw>& dl = p3Draws[drawIndex];
		const int clipY0 = std::max((int)bandY0, (zn.AreaY0 + (int)dstY) * (int)scale);
		const int clipY1 = std::min((int)bandY1, (zn.AreaY1 + 1 + (int)dstY) * (int)scale) - 1;
		if(clipY0 > clipY1) return;
		const SnesHdPackBitmapInfo* tex = (pi.Texture && !pi.Texture->PixelData.empty() && pi.Texture->Width > 0) ? pi.Texture : nullptr;
		const int texW = tex ? (int)tex->Width : 0, texH = tex ? (int)tex->Height : 0;
		const uint8_t z = (uint8_t)std::clamp(pi.ZIndex, 0, 255);
		for(const P3Draw& d : dl) {
			int x0 = std::max(0, (int)std::floor(d.Cx - d.HalfW)), x1 = std::min((int)outWidth - 1, (int)std::ceil(d.Cx + d.HalfW));
			int y0 = std::max(clipY0, (int)std::floor(d.Cy - d.HalfH)), y1 = std::min(clipY1, (int)std::ceil(d.Cy + d.HalfH));
			if(x0 > x1 || y0 > y1) continue;
			for(int yy = y0; yy <= y1; yy++) {
				const float dy = (float)yy + 0.5f - d.Cy;
				for(int xx = x0; xx <= x1; xx++) {
					const float dx = (float)xx + 0.5f - d.Cx;
					float sa, sr, sg, sb;
					if(tex) {
						float u = (dx / d.HalfW * 0.5f + 0.5f) * texW - 0.5f;
						float vv = (dy / d.HalfH * 0.5f + 0.5f) * texH - 0.5f;
						if(u < -0.5f || vv < -0.5f || u > texW - 0.5f || vv > texH - 0.5f) continue;
						int iu = (int)std::floor(u), iv = (int)std::floor(vv);
						float fu = u - iu, fv = vv - iv;
						float acc[4] = { 0, 0, 0, 0 };
						const int qx[4] = { iu, iu + 1, iu, iu + 1 }, qy[4] = { iv, iv, iv + 1, iv + 1 };
						const float wq[4] = { (1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv };
						for(int q = 0; q < 4; q++) {
							int cxq = std::clamp(qx[q], 0, texW - 1), cyq = std::clamp(qy[q], 0, texH - 1);
							uint32_t c = tex->PixelData[(size_t)cyq * texW + cxq];
							float ca = (float)(c >> 24) * wq[q];
							acc[0] += ca; acc[1] += ((c >> 16) & 0xFF) * ca; acc[2] += ((c >> 8) & 0xFF) * ca; acc[3] += (c & 0xFF) * ca;
						}
						if(acc[0] <= 0.5f) continue;
						sa = acc[0] / 255.0f; sr = acc[1] / acc[0]; sg = acc[2] / acc[0]; sb = acc[3] / acc[0];
					} else {
						float dd = std::sqrt(dx * dx + dy * dy) / d.HalfW;
						if(dd >= 1.0f) continue;
						float s = 1.0f - dd * dd;
						sa = s * s; sr = sg = sb = 255.0f;
					}
					const float aa = sa * d.A;
					if(aa < 0.004f) continue;
					const uint32_t idx = (uint32_t)yy * outWidth + (uint32_t)xx;
					if(z < _zBuffer[idx]) continue;
					if(p3Occlusion) { const float sr2 = s_sprRow[idx]; if(sr2 >= 0.0f && d.GroundRow < sr2) continue; } // derriere un sprite
					const uint32_t dstC = out[idx];
					const float dr = (float)((dstC >> 16) & 0xFF), dg = (float)((dstC >> 8) & 0xFF), db = (float)(dstC & 0xFF);
					const float cr = sr * d.R, cg = sg * d.G, cb = sb * d.B;
					float nr, ng, nb;
					if(pi.Additive) {
						nr = std::min(255.0f, dr + cr * aa); ng = std::min(255.0f, dg + cg * aa); nb = std::min(255.0f, db + cb * aa);
					} else {
						nr = dr + (cr - dr) * aa; ng = dg + (cg - dg) * aa; nb = db + (cb - db) * aa;
					}
					out[idx] = 0xFF000000 | ((uint32_t)(nr + 0.5f) << 16) | ((uint32_t)(ng + 0.5f) << 8) | (uint32_t)(nb + 0.5f);
				}
			}
		}
	};

	std::vector<RenderTask> tasks;

	for(size_t i = 0; i < hdOverlays.size(); i++) {
		if(ovActive[i]) tasks.push_back({ hdOverlays[i]->ZIndex, 2, 0, i });
	}
	for(size_t i = 0; i < p3Draws.size(); i++) {
		if(!p3Draws[i].empty()) tasks.push_back({ hdParts3D[i / 2]->ZIndex, 3, 0, i });
	}
	if(reliefOn) {
		for(size_t v = 0; v < m7Views.size(); v++) tasks.push_back({ (int)kReliefZ, 4, 0, v });
	}
	for(size_t v = 0; v < bbDraws.size(); v++) {
		if(!bbDraws[v].empty()) tasks.push_back({ (int)kReliefZ, 5, 0, v }); // apres le relief (meme Z, type 5)
	}
	for(size_t v = 0; v < mdDraws.size(); v++) {
		if(!mdDraws[v].empty()) tasks.push_back({ (int)kReliefZ, 6, 0, v }); // modeles 3D (meme Z, type 6)
	}

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
		if(task.type == 2) {
			renderOverlayBand(task.index, bandY0, bandY1);
			continue;
		}
		if(task.type == 3) {
			render3DBand(task.index, bandY0, bandY1);
			continue;
		}
		if(task.type == 4) {
			renderReliefBand(task.index, bandY0, bandY1);
			continue;
		}
		if(task.type == 5) {
			renderBillboardBand(task.index, bandY0, bandY1);
			continue;
		}
		if(task.type == 6) {
			renderModelBand(task.index, bandY0, bandY1);
			continue;
		}
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
							// <3DParticles> : ligne des pieds du sprite (ou rien si une tile de decor le recouvre)
							if(p3Occlusion && alpha >= 128) s_sprRow[pixelIndex] = (item.oamIndex < 128) ? sprGroupRow[item.oamIndex] : -1.0f;
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
	// <m7CameraDebug> : reperes de controle de la camera Mode 7 reconstruite
	//  - poteaux plantes tous les 64 texels de la carte (pied rouge, sommet cyan, 24 texels de haut) :
	//    ils doivent rester colles au decor et verticaux, meme en virage ;
	//  - ligne magenta : horizon calcule ; croix blanche : axe de la camera.
	// ====================================================================
	if(m7DebugOn) {
		const int thick = std::max(1, (int)scale / 2);
		auto fillRect = [&](int x0, int y0, int x1, int y1, uint32_t c, int cy0, int cy1) {
			x0 = std::max(x0, 0); x1 = std::min(x1, (int)outWidth - 1);
			y0 = std::max(y0, cy0); y1 = std::min(y1, cy1);
			for(int yy = y0; yy <= y1; yy++) for(int xx = x0; xx <= x1; xx++) out[(uint32_t)yy * outWidth + (uint32_t)xx] = c;
		};
		for(const SnesHdM7Zone& zn : m7Zones) {
			const int cy0 = (zn.AreaY0 + (int)dstY) * (int)scale;
			const int cy1 = std::min((int)hdHeight - 1, (zn.AreaY1 + 1 + (int)dstY) * (int)scale - 1);
			// limites de la zone (vert)
			fillRect(0, (zn.Y0 + (int)dstY) * (int)scale, (int)outWidth - 1, (zn.Y0 + (int)dstY) * (int)scale + thick - 1, 0xFF00C000, cy0, cy1);
			if(!zn.Perspective || !zn.HasPinhole) continue;
			int hy = (int)std::lround((zn.Horizon + dstY) * scale);
			for(int xx = 0; xx < (int)outWidth; xx += 8 * thick) fillRect(xx, hy, xx + 4 * thick - 1, hy + thick - 1, 0xFFFF00FF, cy0, cy1);
			int ax = (int)std::lround((zn.AxisX + dstX) * scale), ay = (zn.Y1 + (int)dstY) * (int)scale;
			fillRect(ax - 6 * thick, ay - thick, ax + 6 * thick, ay, 0xFFFFFFFF, cy0, cy1);
			fillRect(ax - thick / 2, ay - 12 * thick, ax + thick / 2, ay, 0xFFFFFFFF, cy0, cy1);

			const int step = 64;
			int gx0 = (int)std::floor((zn.Cx - 640.0) / step), gx1 = (int)std::ceil((zn.Cx + 640.0) / step);
			int gy0 = (int)std::floor((zn.Cy - 640.0) / step), gy1 = (int)std::ceil((zn.Cy + 640.0) / step);
			for(int gy = gy0; gy <= gy1; gy++) {
				for(int gx = gx0; gx <= gx1; gx++) {
					double u = gx * step, w = gy * step, sx, syG, syT, ppt, ppt2;
					if(!zn.Project(u, w, 0.0, sx, syG, ppt)) continue;
					if(!zn.Project(u, w, 24.0, sx, syT, ppt2)) continue;
					int px = (int)std::lround((sx + dstX) * scale);
					if(px < -16 || px > (int)outWidth + 16) continue;
					int pyG = (int)std::lround((syG + dstY) * scale), pyT = (int)std::lround((syT + dstY) * scale);
					int r = std::max(thick, (int)std::lround(ppt * scale * 1.5));
					r = std::min(r, 6 * thick);
					fillRect(px - thick / 2, pyT, px + thick / 2, pyG, 0xFFFFE000, cy0, cy1);
					fillRect(px - r, pyG - r, px + r, pyG + r, 0xFFFF2020, cy0, cy1);
					fillRect(px - r / 2 - 1, pyT - r / 2 - 1, px + r / 2 + 1, pyT + r / 2 + 1, 0xFF20E0FF, cy0, cy1);
				}
			}
		}
		uint32_t fn = SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber;
		if(fn % 120 == 0) {
			std::string msg = "[HD-M7CAM] " + std::to_string(m7Zones.size()) + " zone(s) :";
			for(const SnesHdM7Zone& zn : m7Zones) {
				char buf[256];
				snprintf(buf, sizeof(buf), " [lignes %d-%d %s horizon %.1f focale %.1f hauteur %.1f camera (%.1f, %.1f) cap %.0f deg]",
					zn.Y0, zn.Y1, zn.Perspective ? (zn.HasPinhole ? "perspective" : "perspective SANS camera") : "plane",
					zn.Horizon, zn.Focal, zn.Height, zn.Cx, zn.Cy, std::atan2(zn.Fx, -zn.Fy) * 57.29578);
				msg += buf;
			}
			MessageManager::Log(msg);
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

				// Lecture depuis la memoire si le fichier a ete precharge par LoadAsync, sinon depuis le disque.
				// (free_when_done = 0 : les donnees restent disponibles pour une relecture)
				if(video->FileData && !video->FileData->empty()) {
					video->VideoPlayer = plm_create_with_memory(video->FileData->data(), video->FileData->size(), 0);
				} else {
					video->VideoPlayer = plm_create_with_filename(fullPath.c_str());
				}
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