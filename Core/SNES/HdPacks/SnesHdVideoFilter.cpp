#include "pch.h"
#include <vector>
#include <algorithm>
#include <thread>
#include <cmath>
#include <cstdio>
#include "SNES/HdPacks/SnesHdVideoFilter.h"
#include "SNES/HdPacks/SnesHdPackLoader.h"
#include "SNES/HdPacks/SnesHdMode7Camera.h"
#include "SNES/HdPacks/SnesHdGpu.h"
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
	// SOCLE 3D EN AVANCE : camera Mode 7 + envoi des modeles a la carte graphique des la fin de la phase 1,
	// pour que la carte graphique travaille pendant que le processeur dessine la piste et les sprites.
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
	// la camera sert a TOUS les effets 3D (relief, panneaux, modeles, lumieres, effets, liquide, lumieres de sprites)
	const SnesHdPackData& pk3 = *SnesHdPackLoader::ActivePack;
	const bool m7CamNeeded = m7DebugOn || !hdParts3D.empty() || !pk3.M7Reliefs.empty() || !pk3.M7Billboards.empty() || !pk3.M7Models.empty() ||
		!pk3.M7Lights.empty() || !pk3.M7Ambients.empty() || !pk3.M7Liquids.empty() || !pk3.M7Effects.empty() || !pk3.SpriteLights.empty();
	if(m7CamNeeded) {
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

	// ====================================================================
	// <m7model> : PREPARATION (sequentielle) - placement, mouvement et animation des modeles 3D par vue
	// Repere : OBJ / glTF (x, y vers le haut, z) -> carte (u = x, hauteur = y, v = z), meme orientation.
	// Placement = matrice 3x4 (echelle, origine, balancement, cap, position, hauteur) ; animation glTF =
	// palette d'os calculee une fois par modele et par frame. Memes donnees pour le GPU et le processeur.
	// ====================================================================
	struct MVert { float X, Y, W, GR, U, V, L; }; // ecran HD, 1/profondeur, ligne du sol, texture, lumiere
	struct MTriDraw { MVert P[3]; const SnesHdModelMaterial* M; float A; };
	auto& hdModels = SnesHdPackLoader::ActivePack->M7Models;
	std::vector<std::vector<MTriDraw>> mdDraws(m7Views.size());
	bool mdOn = false;
	// <m7model> sur la carte graphique (SnesHdGpu) si disponible et non desactive par <gpu3d>off
	const bool mdUseGpu = !hdModels.empty() && !m7Views.empty() && SnesHdPackLoader::ActivePack->Gpu3D && SnesHdGpu::Get().Available();
	std::vector<SnesHdGpuView> gpuViews;
	static std::vector<uint32_t> s_gpuColor;
	static std::vector<float> s_gpuGround;
	bool gpuModelsOn = false;
	int gpuRx0 = 1 << 30, gpuRy0 = 1 << 30, gpuRx1 = -1, gpuRy1 = -1; // rectangle utile (pixels HD)
	if(mdUseGpu) gpuViews.resize(m7Views.size());
	std::vector<std::vector<float>> mdPalettes(hdModels.size()); // os animes de chaque modele (cette frame)
	if(!hdModels.empty() && !m7Views.empty()) {
		const float mdTime = (float)SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber / 60.0f;
		const float kDeg = 0.01745329f;
		// lumiere fixe dans le monde : du nord-ouest, en hauteur (u, v, h)
		const float lx = -0.45f, lv = -0.35f, lh = 0.82f;
		const float ln = std::sqrt(lx * lx + lv * lv + lh * lh);
		std::vector<char> mdActive(hdModels.size(), 0);
		for(size_t mi = 0; mi < hdModels.size(); mi++) {
			SnesHdM7ModelInfo& md = *hdModels[mi];
			if(!md.Mesh || !md.MatchesCondition()) continue;
			mdActive[mi] = 1;
			if(md.Mesh->Rig) md.Mesh->Rig->Evaluate(md.Anim, (mdTime + md.TimeOffset) * md.AnimSpeed, mdPalettes[mi]);
		}
		for(size_t vw = 0; vw < m7Views.size(); vw++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[vw]];
			const uint32_t viewMap = m7Lines[zn.Y1].Img ? m7Lines[zn.Y1].Ref.M7MapHash : 0;
			std::vector<MTriDraw>& dl = mdDraws[vw];
			for(size_t mi = 0; mi < hdModels.size(); mi++) {
				if(!mdActive[mi]) continue;
				SnesHdM7ModelInfo& md = *hdModels[mi];
				if(!SnesHdMapMatch(md.AnyMap, md.MapHashes, md.MapHash, viewMap)) continue;
				const SnesHdModelMesh& mesh = *md.Mesh;
				const std::vector<float>* pal = (mesh.Rig && !mdPalettes[mi].empty()) ? &mdPalettes[mi] : nullptr;
				const float ox = md.KeepOrigin ? 0.0f : (mesh.MinX + mesh.MaxX) * 0.5f;
				const float oy = md.KeepOrigin ? 0.0f : mesh.MinY;
				const float oz = md.KeepOrigin ? 0.0f : (mesh.MinZ + mesh.MaxZ) * 0.5f;
				const float t = mdTime + md.TimeOffset;
				auto place = [&](double u0, double v0) {
					// --- mouvement : trajet, cap, rotation continue, flottement, balancement ---
					float pu = 0, pv = 0, ph = 0, du = 0, dv = 1;
					const bool onPath = md.Path.size() >= 2 && md.PathLen > 0;
					if(onPath) md.EvalPath(t, pu, pv, ph, du, dv);
					double bu = onPath ? (md.OnTile ? u0 + pu : pu) : u0;
					double bv = onPath ? (md.OnTile ? v0 + pv : pv) : v0;
					float yawDeg = md.Rotation + md.Spin * t;
					if(onPath && md.PathOrient && (du != 0 || dv != 0)) yawDeg += std::atan2(-du, dv) / kDeg; // avant du modele (+z) dans le sens de la marche
					const float roll = md.SwayAmp != 0 ? md.SwayAmp * kDeg * std::sin(6.2831853f * md.SwayFreq * t) : 0.0f;
					const float bob = md.BobAmp != 0 ? md.BobAmp * std::sin(6.2831853f * md.BobFreq * t) : 0.0f;
					const double U0 = zn.LargeMap ? bu : bu + 1024.0 * std::round((zn.Cx - bu) / 1024.0);
					const double V0 = zn.LargeMap ? bv : bv + 1024.0 * std::round((zn.Cy - bv) / 1024.0);
					const float hBase = md.Elevation + ph + bob;
					// matrice : (u, v, h) = M x (x, y, z, 1)
					const float s = md.Scale, cy = std::cos(yawDeg * kDeg), sy = std::sin(yawDeg * kDeg), cro = std::cos(roll), sro = std::sin(roll);
					float M[12] = {
						s * cy * cro, -s * cy * sro, -s * sy, 0,
						s * sy * cro, -s * sy * sro,  s * cy, 0,
						s * sro,       s * cro,       0,      0 };
					M[3] = (float)U0 - (M[0] * ox + M[1] * oy + M[2] * oz);
					M[7] = (float)V0 - (M[4] * ox + M[5] * oy + M[6] * oz);
					M[11] = hBase - (M[8] * ox + M[9] * oy + M[10] * oz);
					auto xf = [&](const float* p, double& u, double& w, double& h) {
						u = M[0] * p[0] + M[1] * p[1] + M[2] * p[2] + M[3];
						w = M[4] * p[0] + M[5] * p[1] + M[6] * p[2] + M[7];
						h = M[8] * p[0] + M[9] * p[1] + M[10] * p[2] + M[11];
					};
					// fondu pres de la ligne la plus lointaine (comme les panneaux)
					double asx, asy, appt;
					float fade = 1.0f;
					if(zn.Project(U0, V0, 0.0, asx, asy, appt)) fade = std::clamp((float)(asy - zn.Y0) / 6.0f, 0.0f, 1.0f);
					if(fade <= 0.003f) return;
					if(mdUseGpu) {
						// la carte graphique projette elle-meme les sommets (meme table que ce code)
						SnesHdGpuInstance gi;
						gi.Mesh = &mesh; gi.Ambient = md.Ambient; gi.Fade = fade; gi.Palette = pal;
						memcpy(gi.Model, M, sizeof(M));
						gi.Model[3] -= (float)zn.Cx; gi.Model[7] -= (float)zn.Cy; // relatif a la camera (precision)
						// rectangle d'ecran occupe : coins de la boite englobante (elargie si animee) projetes
						const float grow = mesh.Rig ? 0.25f : 0.0f;
						const float ex = (mesh.MaxX - mesh.MinX) * grow, ey = (mesh.MaxY - mesh.MinY) * grow, ez = (mesh.MaxZ - mesh.MinZ) * grow;
						bool anyCorner = false, partial = false;
						float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
						for(int c = 0; c < 8; c++) {
							float pc[3] = { (c & 1) ? mesh.MaxX + ex : mesh.MinX - ex, (c & 2) ? mesh.MaxY + ey : mesh.MinY - ey, (c & 4) ? mesh.MaxZ + ez : mesh.MinZ - ez };
							double u, w, hh;
							xf(pc, u, w, hh);
							double sx, sy, ppt;
							if(!zn.Project(u, w, hh, sx, sy, ppt) || zn.DepthOf(u, w) < 2.0) { partial = true; continue; }
							float hx = (float)((sx + dstX) * scale), hy = (float)((sy + dstY) * scale);
							bx0 = std::min(bx0, hx); bx1 = std::max(bx1, hx); by0 = std::min(by0, hy); by1 = std::max(by1, hy);
							anyCorner = true;
						}
						const int areaTop = (zn.AreaY0 + (int)dstY) * (int)scale, areaBot = (zn.AreaY1 + 1 + (int)dstY) * (int)scale - 1;
						if(partial) { bx0 = 0; bx1 = (float)outWidth - 1; by0 = (float)areaTop; if(!anyCorner) by1 = (float)areaBot; }
						const float margin = 4.0f * (float)scale;
						if(bx1 + margin < 0 || bx0 - margin > (float)outWidth || by1 + margin < (float)areaTop || by0 - margin > (float)areaBot) return; // hors ecran
						gpuRx0 = std::min(gpuRx0, (int)std::floor(bx0 - margin)); gpuRx1 = std::max(gpuRx1, (int)std::ceil(bx1 + margin));
						gpuRy0 = std::min(gpuRy0, std::max(areaTop, (int)std::floor(by0 - margin)));
						gpuRy1 = std::max(gpuRy1, std::min(areaBot, (int)std::ceil(by1 + margin)));
						gpuViews[vw].Instances.push_back(gi);
						return;
					}
					// --- moteur processeur ---
					for(const SnesHdModelTri& tr : mesh.Tris) {
						const SnesHdModelMaterial& mat = mesh.Mats[std::clamp(tr.Mat, 0, (int)mesh.Mats.size() - 1)];
						const float a = mat.Alpha * fade;
						if(a <= 0.003f) continue;
						MTriDraw d; d.M = &mat; d.A = a;
						bool ok = true;
						for(int k = 0; k < 3 && ok; k++) {
							float p[3] = { tr.P[k][0], tr.P[k][1], tr.P[k][2] }, n[3] = { tr.N[k][0], tr.N[k][1], tr.N[k][2] };
							if(pal) {
								// os animes : somme ponderee des matrices
								float q[3] = { 0, 0, 0 }, m3[3] = { 0, 0, 0 };
								for(int j = 0; j < 4; j++) {
									const float wgt = tr.Wt[k][j];
									if(wgt <= 0) continue;
									const float* B = &(*pal)[(size_t)tr.J[k][j] * 12];
									for(int r = 0; r < 3; r++) {
										q[r] += wgt * (B[r * 4] * p[0] + B[r * 4 + 1] * p[1] + B[r * 4 + 2] * p[2] + B[r * 4 + 3]);
										m3[r] += wgt * (B[r * 4] * n[0] + B[r * 4 + 1] * n[1] + B[r * 4 + 2] * n[2]);
									}
								}
								memcpy(p, q, sizeof(q)); memcpy(n, m3, sizeof(m3));
							}
							double u, w, h;
							xf(p, u, w, h);
							double sx, sy, ppt;
							if(!zn.Project(u, w, h, sx, sy, ppt)) { ok = false; break; }
							double D = zn.DepthOf(u, w);
							if(D < 2.0) { ok = false; break; }
							float nu = M[0] * n[0] + M[1] * n[1] + M[2] * n[2], nv = M[4] * n[0] + M[5] * n[1] + M[6] * n[2], nh = M[8] * n[0] + M[9] * n[1] + M[10] * n[2];
							float nl = std::sqrt(nu * nu + nv * nv + nh * nh);
							float dotL = nl > 1e-6f ? std::abs(nu * lx + nv * lv + nh * lh) / (nl * ln) : 1.0f; // deux faces
							MVert& mv = d.P[k];
							mv.X = (float)((sx + dstX) * scale); mv.Y = (float)((sy + dstY) * scale);
							mv.W = (float)(1.0 / D);
							mv.GR = (float)(sy + h * ppt);
							mv.U = tr.UV[k][0]; mv.V = tr.UV[k][1];
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
	if(mdUseGpu) {
		bool any = false;
		for(size_t vw = 0; vw < gpuViews.size(); vw++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[vw]];
			gpuViews[vw].Zone = &zn;
			gpuViews[vw].ClipY0 = std::max(0, (zn.AreaY0 + (int)dstY) * (int)scale);
			gpuViews[vw].ClipY1 = std::min((int)hdHeight - 1, (zn.AreaY1 + 1 + (int)dstY) * (int)scale - 1);
			if(!gpuViews[vw].Instances.empty()) any = true;
		}
		gpuRx0 = std::max(gpuRx0, 0); gpuRy0 = std::max(gpuRy0, 0);
		gpuRx1 = std::min(gpuRx1, (int)outWidth - 1); gpuRy1 = std::min(gpuRy1, (int)hdHeight - 1);
		if(any && gpuRx0 <= gpuRx1 && gpuRy0 <= gpuRy1 &&
			SnesHdGpu::Get().SubmitModels(gpuViews, outWidth, hdHeight, (float)scale, (float)dstX, (float)dstY,
				gpuRx0, gpuRy0, gpuRx1, gpuRy1)) {
			gpuModelsOn = true;
			mdOn = true;
		}
	}
	static std::vector<float> s_modelW; // 1/profondeur des pixels de modele (plus grand = plus proche)
	if(mdOn) s_modelW.assign((size_t)outWidth * hdHeight, 0.0f);

	// ====================================================================
	// <m7LiquidBackground> : PREPARATION (sequentielle) - liquide de chaque ligne Mode 7 + sa vue
	// ====================================================================
	std::vector<const SnesHdM7LiquidInfo*> lineLiquid(cropHeight, nullptr);
	std::vector<const SnesHdM7Zone*> lineZone(cropHeight, nullptr);
	const float liqTime = (float)SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber / 60.0f;
	{
		auto& hdLiquids = SnesHdPackLoader::ActivePack->M7Liquids;
		if(!hdLiquids.empty()) {
			std::vector<char> liqOk(hdLiquids.size(), 0);
			for(size_t i = 0; i < hdLiquids.size(); i++) {
				liqOk[i] = hdLiquids[i]->MatchesCondition() ? 1 : 0;
				if(liqOk[i]) hdLiquids[i]->Build();
			}
			for(const SnesHdM7Zone& zn : m7Zones) {
				for(int yy = zn.Y0; yy <= zn.Y1; yy++) if(yy >= 0 && yy < (int)cropHeight) lineZone[yy] = &zn;
			}
			for(uint32_t yy = 0; yy < cropHeight; yy++) {
				if(!m7Lines[yy].Img) continue;
				const uint32_t mh = m7Lines[yy].Ref.M7MapHash;
				for(size_t i = 0; i < hdLiquids.size(); i++) {
					if(!liqOk[i]) continue;
					if(!SnesHdMapMatch(hdLiquids[i]->AnyMap, hdLiquids[i]->MapHashes, hdLiquids[i]->MapHash, mh)) continue;
					lineLiquid[yy] = hdLiquids[i].get();
					break;
				}
			}
		}
	}
	// Table de sinus (1024 points, interpolation lineaire) : ~10x plus rapide que std::sin/cos ici
	static float s_sinTab[1025];
	static bool s_sinTabInit = false;
	if(!s_sinTabInit) { for(int i = 0; i <= 1024; i++) s_sinTab[i] = std::sin(6.2831853f * i / 1024.0f); s_sinTabInit = true; }
	auto fastSinCos = [](float ph, float& s, float& c) {
		float x = ph * (1024.0f / 6.2831853f);
		x -= 1024.0f * std::floor(x / 1024.0f);
		int i = (int)x; float f = x - (float)i; i &= 1023;
		s = s_sinTab[i] + (s_sinTab[i + 1] - s_sinTab[i]) * f;
		int j = (i + 256) & 1023; // cos = sin(x + pi/2)
		c = s_sinTab[j] + (s_sinTab[j + 1] - s_sinTab[j]) * f;
	};
	// Couleur du liquide en un point de la carte (u, v texels), k = texels de carte par pixel HD (finesse)
	auto liquidShade = [&](const SnesHdM7LiquidInfo& L, double u, double v, float k, const SnesHdM7Zone* zn, float texAlpha) -> uint32_t {
		const float t = liqTime;
		const float kTwoPi = 6.2831853f;
		// vagues : 3 trains d'ondes dans le monde, attenues quand ils deviennent plus fins que ~3 pixels HD
		static const float dirs[3][2] = { { 0.957f, 0.287f }, { -0.371f, 0.928f }, { 0.707f, -0.707f } };
		const float lens[3] = { 1.0f, 0.63f, 0.41f }, amps[3] = { 1.0f, 0.6f, 0.4f }, spd[3] = { 1.0f, 0.8f, 1.3f };
		float h = 0, du = 0, dv = 0, ampSum = 0;
		for(int i = 0; i < 3; i++) {
			const float lambda = L.Scale * lens[i];
			const float fade = std::clamp((lambda / std::max(k, 1e-4f) - 3.0f) / 6.0f, 0.0f, 1.0f);
			const float a = amps[i] * fade;
			ampSum += amps[i];
			if(a <= 0.0f) continue;
			const float f = kTwoPi / lambda;
			const float ph = (float)((u * dirs[i][0] + v * dirs[i][1]) * f) - t * L.Speed * spd[i] * f;
			float s, c;
			fastSinCos(ph, s, c);
			h += a * s;
			du += a * c * f * dirs[i][0]; dv += a * c * f * dirs[i][1];
		}
		h /= ampSum;
		const float slope = L.Waves * L.Scale / kTwoPi / ampSum;
		float nx = -du * slope, ny = -dv * slope, nz = 1.0f;
		const float nl = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz); nx *= nl; ny *= nl; nz *= nl;

		auto ch = [](uint32_t c, int sh) { return (float)((c >> sh) & 0xFF); };
		const float m = 0.5f + 0.5f * h;
		float r = ch(L.Deep, 16) + (ch(L.Color, 16) - ch(L.Deep, 16)) * m;
		float g = ch(L.Deep, 8) + (ch(L.Color, 8) - ch(L.Deep, 8)) * m;
		float b = ch(L.Deep, 0) + (ch(L.Color, 0) - ch(L.Deep, 0)) * m;

		// detail optionnel : texture repetee, deux couches qui glissent en sens oppose (mip selon la finesse)
		if(!L.Mips.empty()) {
			auto sampleTex = [&](double su, double sv) {
				const float texelsPerPx = k / L.TexScale * (float)L.MipW[0];
				int lvl = texelsPerPx > 1.0f ? (int)std::floor(std::log2(texelsPerPx)) : 0;
				lvl = std::clamp(lvl, 0, (int)L.Mips.size() - 1);
				const int w = (int)L.MipW[lvl], hh = (int)L.MipH[lvl];
				double fu = su / L.TexScale, fv = sv / L.TexScale;
				fu -= std::floor(fu); fv -= std::floor(fv);
				int ix = std::clamp((int)(fu * w), 0, w - 1), iy = std::clamp((int)(fv * hh), 0, hh - 1);
				return L.Mips[lvl][(size_t)iy * w + ix];
			};
			const double off = (double)t * L.Speed * 0.5;
			uint32_t c1 = sampleTex(u + off, v + off * 0.3), c2 = sampleTex(u - off * 0.7, v + off);
			const float tr = (ch(c1, 16) + ch(c2, 16)) / 510.0f, tg = (ch(c1, 8) + ch(c2, 8)) / 510.0f, tb = (ch(c1, 0) + ch(c2, 0)) / 510.0f;
			r *= 0.5f + tr; g *= 0.5f + tg; b *= 0.5f + tb;
		}

		if(L.Lava) {
			// points chauds qui pulsent
			float ps, pc;
			fastSinCos(t * 2.1f + (float)(u * 0.05 + v * 0.031), ps, pc);
			const float hot = std::clamp((h - 0.35f) * 2.5f, 0.0f, 1.0f) * (0.75f + 0.25f * ps);
			r += (ch(L.Sky, 16) - r) * hot; g += (ch(L.Sky, 8) - g) * hot; b += (ch(L.Sky, 0) - b) * hot;
		} else {
			// direction de la camera (stenope de la vue) : reflet du ciel (angle rasant) et du soleil
			float vx = 0, vy = 0, vz = 1;
			if(zn && zn->HasPinhole) {
				vx = (float)(zn->Cx - u); vy = (float)(zn->Cy - v); vz = (float)zn->Height;
				const float vl = 1.0f / std::sqrt(vx * vx + vy * vy + vz * vz); vx *= vl; vy *= vl; vz *= vl;
			}
			const float ndv = std::max(0.0f, nx * vx + ny * vy + nz * vz);
			const float om = 1.0f - ndv;
			const float fres = L.Reflect * (0.04f + 0.96f * om * om * om);
			r += (ch(L.Sky, 16) - r) * fres; g += (ch(L.Sky, 8) - g) * fres; b += (ch(L.Sky, 0) - b) * fres;
			// soleil (devant, haut) : reflet = vue reflechie par la normale
			const float rx = 2.0f * ndv * nx - vx, ry = 2.0f * ndv * ny - vy, rz = 2.0f * ndv * nz - vz;
			const float sx = zn ? (float)zn->Fx * 0.55f : 0.0f, sy = zn ? (float)zn->Fy * 0.55f : -0.55f, sz = 0.83f;
			const float sp = std::max(0.0f, rx * sx + ry * sy + rz * sz);
			if(sp > 0.85f && L.Specular > 0.0f) { // sp^48 negligeable en dessous
				float p = sp * sp * sp;               // ^3
				p *= p; p *= p; p *= p; p *= p;       // ^48
				const float spec = L.Specular * p * 255.0f;
				r += spec; g += spec; b += spec;
			}
			// ecume : zones semi-transparentes de la texture (rivage)
			if(L.Foam > 0.0f && texAlpha > 0.01f) {
				const float band = std::clamp(texAlpha * 5.0f, 0.0f, 1.0f) * std::clamp((0.75f - texAlpha) * 3.0f, 0.0f, 1.0f);
				const float fo = L.Foam * band * (0.55f + 0.45f * h);
				r += (250.0f - r) * fo; g += (252.0f - g) * fo; b += (255.0f - b) * fo;
			}
		}
		return 0xFF000000 | ((uint32_t)std::clamp(r, 0.0f, 255.0f) << 16) | ((uint32_t)std::clamp(g, 0.0f, 255.0f) << 8) | (uint32_t)std::clamp(b, 0.0f, 255.0f);
	};

	// ====================================================================
	// MODE 7 HD - PHASE 2 (parallele) : rendu d'une ligne SNES (= 'scale' lignes HD)
	// Filtrage : trilineaire ('bilinear') ou anisotrope ('anisotropic', jusqu'a 8 echantillons).
	// Unites : positions et pas en 8.8, en texels du niveau 0.
	// ====================================================================
	auto renderMode7Line = [&](uint32_t y) {
		const M7Line& L = m7Lines[y];
		const SnesHdM7LiquidInfo* lineLiq = lineLiquid[y];
		const SnesHdM7Zone* lineZn = lineZone[y];
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

					// <m7LiquidBackground> : liquide visible a travers les pixels (semi-)transparents de la texture
					if(lineLiq && (hdColorFromPng >> 24) < 255) {
						const double lu = (double)posFX / ((double)imgScale * 65536.0), lv = (double)posFY / ((double)imgScale * 65536.0);
						const float ta = (float)(hdColorFromPng >> 24) / 255.0f;
						const uint32_t wc = liquidShade(*lineLiq, lu, lv, pMax / (float)imgScale, lineZn, ta);
						const float ia = 1.0f - ta;
						const uint32_t lr = (uint32_t)(((wc >> 16) & 0xFF) * ia + ((hdColorFromPng >> 16) & 0xFF) * ta + 0.5f);
						const uint32_t lg = (uint32_t)(((wc >> 8) & 0xFF) * ia + ((hdColorFromPng >> 8) & 0xFF) * ta + 0.5f);
						const uint32_t lb = (uint32_t)((wc & 0xFF) * ia + (hdColorFromPng & 0xFF) * ta + 0.5f);
						hdColorFromPng = 0xFF000000 | (std::min(lr, 255u) << 16) | (std::min(lg, 255u) << 8) | std::min(lb, 255u);
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
		int type;     // 0 = Background, 1 = Tile/Sprite, 2 = <2DOverlay>, 3 = <3DParticles>, 4 = <m7relief>, 5 = <m7billboard>, 6 = <m7model>, 7 = <m7model> GPU, 8 = <m7Effect>
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
				if(!SnesHdMapMatch(bb.AnyMap, bb.MapHashes, bb.MapHash, viewMap)) continue;
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


	// Position courante d'un <m7Effect> (trajet, flottement) ; base = (x, y) ou centre de la case (tile=)
	auto fxPos = [&](const SnesHdM7EffectInfo& fx, double baseU, double baseV, double& u, double& v, double& h) {
		u = baseU; v = baseV; h = fx.Elevation;
		if(!fx.HasMotion) return;
		const SnesHdM7ModelInfo& m = fx.Motion;
		const float t = (float)SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber / 60.0f + m.TimeOffset;
		if(m.Path.size() >= 2 && m.PathLen > 0) {
			float pu, pv, ph, du, dv;
			m.EvalPath(t, pu, pv, ph, du, dv);
			if(fx.OnTile) { u = baseU + pu; v = baseV + pv; } else { u = pu; v = pv; }
			h += ph;
		}
		if(m.BobAmp != 0) h += m.BobAmp * std::sin(6.2831853f * m.BobFreq * t);
	};

	// ====================================================================
	// <m7light> / <m7ambient> : PREPARATION (sequentielle) - lumieres visibles de chaque vue
	// ====================================================================
	struct LightDraw { float U, V, H, R2, InvR2, Cr, Cg, Cb; bool Add; int X0, X1, Y0, Y1; int Group = -1; float Self = 1.0f; };
	struct ViewLighting { float Ar = 1, Ag = 1, Ab = 1; bool Sky = true; bool AmbientOn = false; std::vector<LightDraw> Lights; };
	std::vector<ViewLighting> viewLights(m7Views.size());
	bool lightsOn = false;
	auto& hdLights = SnesHdPackLoader::ActivePack->M7Lights;
	auto& hdAmbients = SnesHdPackLoader::ActivePack->M7Ambients;
	if((!hdLights.empty() || !hdAmbients.empty()) && !m7Views.empty()) {
		const float tSec = (float)SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber / 60.0f;
		for(size_t v = 0; v < m7Views.size(); v++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
			const uint32_t viewMap = m7Lines[zn.Y1].Img ? m7Lines[zn.Y1].Ref.M7MapHash : 0;
			ViewLighting& VL = viewLights[v];
			for(auto& ua : hdAmbients) {
				SnesHdM7AmbientInfo& am = *ua;
				if(!SnesHdMapMatch(am.AnyMap, am.MapHashes, am.MapHash, viewMap)) continue;
				if(!am.MatchesCondition()) continue;
				VL.Ar = ((am.Tint >> 16) & 0xFF) / 255.0f * am.Brightness;
				VL.Ag = ((am.Tint >> 8) & 0xFF) / 255.0f * am.Brightness;
				VL.Ab = (am.Tint & 0xFF) / 255.0f * am.Brightness;
				VL.Sky = am.Sky;
				VL.AmbientOn = std::abs(VL.Ar - 1.0f) > 0.002f || std::abs(VL.Ag - 1.0f) > 0.002f || std::abs(VL.Ab - 1.0f) > 0.002f;
				break;
			}
			const int sc = (int)scale;
			const int zoneTopHD = (zn.Y0 + (int)dstY) * sc;
			const int areaTopHD = (zn.AreaY0 + (int)dstY) * sc, areaBotHD = (zn.AreaY1 + 1 + (int)dstY) * sc - 1;
			for(auto& ul : hdLights) {
				SnesHdM7LightInfo& lt = *ul;
				if(!SnesHdMapMatch(lt.AnyMap, lt.MapHashes, lt.MapHash, viewMap)) continue;
				if(!lt.MatchesCondition()) continue;
				auto addLight = [&](double u0, double v0, float seed) {
					float hL = lt.Height;
					if(lt.Follow && lt.Follow->HasMotion) {
						// lumiere automatique d'une flamme en mouvement : meme trajet, meme flottement
						double fu, fv, fh;
						fxPos(*lt.Follow, u0, v0, fu, fv, fh);
						u0 = fu; v0 = fv; hL = (float)fh + lt.Follow->SizeStart * 1.5f;
					}
					double u = zn.LargeMap ? u0 : u0 + 1024.0 * std::round((zn.Cx - u0) / 1024.0);
					double w = zn.LargeMap ? v0 : v0 + 1024.0 * std::round((zn.Cy - v0) / 1024.0);
					const double r = lt.Radius;
					// portee au sol vue par la camera : centre, point le plus proche / le plus loin, cotes
					const double px[5] = { u, u - zn.Fx * r, u + zn.Fx * r, u - zn.Rx * r, u + zn.Rx * r };
					const double py[5] = { w, w - zn.Fy * r, w + zn.Fy * r, w - zn.Ry * r, w + zn.Ry * r };
					bool okP[5]; int nOk = 0;
					float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, pptMax = 0;
					for(int k = 0; k < 5; k++) {
						double sx, sy, ppt;
						okP[k] = zn.Project(px[k], py[k], 0.0, sx, sy, ppt) && zn.DepthOf(px[k], py[k]) > 1.0;
						if(!okP[k]) continue;
						nOk++;
						float hx = (float)((sx + dstX) * scale), hy = (float)((sy + dstY) * scale);
						x0 = std::min(x0, hx); x1 = std::max(x1, hx); y0 = std::min(y0, hy); y1 = std::max(y1, hy);
						pptMax = std::max(pptMax, (float)ppt);
					}
					if(nOk == 0) return; // entierement au-dela de l'horizon ou derriere la camera
					if(!okP[1]) { x0 = 0; x1 = (float)outWidth - 1; y1 = (float)areaBotHD; } // le bord proche sort de la vue
					if(!okP[2]) y0 = (float)zoneTopHD;                                         // le bord lointain passe l'horizon
					const float m = (float)(r * pptMax * scale);
					LightDraw d;
					d.X0 = std::max(0, (int)std::floor(x0 - m)); d.X1 = std::min((int)outWidth - 1, (int)std::ceil(x1 + m));
					d.Y0 = std::max(zoneTopHD, (int)std::floor(y0 - 2)); d.Y1 = std::min(areaBotHD, (int)std::ceil(y1 + 2));
					if(d.X0 > d.X1 || d.Y0 > d.Y1) return;
					float flick = 1.0f;
					if(lt.Flicker > 0) {
						float n = 0.6f * std::sin(tSec * lt.FlickerSpeed + seed) + 0.4f * std::sin(tSec * lt.FlickerSpeed * 2.31f + seed * 1.7f);
						flick = 1.0f - lt.Flicker * (0.5f + 0.5f * n);
					}
					const float k = lt.Intensity * flick / 255.0f;
					d.U = (float)u; d.V = (float)w; d.H = hL;
					d.R2 = (float)(r * r); d.InvR2 = 1.0f / d.R2;
					d.Cr = ((lt.Color >> 16) & 0xFF) * k; d.Cg = ((lt.Color >> 8) & 0xFF) * k; d.Cb = (lt.Color & 0xFF) * k;
					if(lt.Shadow) { // ombre : retire (1 - couleur) ; noir = ombre neutre, bleu = ombre teintee de bleu
						d.Cr = -(255 - ((lt.Color >> 16) & 0xFF)) * k; d.Cg = -(255 - ((lt.Color >> 8) & 0xFF)) * k; d.Cb = -(255 - (lt.Color & 0xFF)) * k;
					}
					d.Add = lt.Additive;
					VL.Lights.push_back(d);
				};
				if(lt.OnTile) {
					if(m7CellHash.empty()) continue;
					for(size_t c = 0; c < m7CellHash.size(); c++) {
						if(m7CellHash[c] == lt.TileHash) addLight((double)((c % 128) * 8 + 4) + lt.Dx, (double)((c / 128) * 8 + 4) + lt.Dy, (float)c * 0.37f);
					}
				} else {
					addLight(lt.X, lt.Y, lt.X * 0.13f + lt.Y * 0.07f);
				}
			}
			(void)areaTopHD;
			if(VL.AmbientOn || !VL.Lights.empty()) lightsOn = true;
		}
	}
	// ====================================================================
	// <SpriteLight> : lumiere 3D attachee a une tile de sprite (explosion, impact, tir laser...)
	// Ici : y a-t-il une tile concernee a l'ecran ? (la position 3D est calculee apres la carte des sprites)
	// ====================================================================
	std::vector<const SnesHdSpriteLightInfo*> sprLightRules;
	bool sprLightCand = false;
	if(!SnesHdPackLoader::ActivePack->SpriteLights.empty() && !m7Views.empty()) {
		for(auto& ur : SnesHdPackLoader::ActivePack->SpriteLights) { if(ur->MatchesCondition()) sprLightRules.push_back(ur.get()); }
		for(const auto& item : tilesToExpand) {
			if(sprLightCand) break;
			if(item.oamIndex >= 128 || !item.Tile) continue;
			for(const SnesHdSpriteLightInfo* R : sprLightRules) { if(R->Matches(item.Tile)) { sprLightCand = true; break; } }
		}
		if(sprLightCand) lightsOn = true;
	}
	// self < 1 : il faut savoir quel groupe de sprites a dessine chaque pixel (masque HD, groupe + 1)
	bool slSelfOn = false;
	if(sprLightCand) for(const SnesHdSpriteLightInfo* R : sprLightRules) if(R->SelfLight < 0.999f || R->Glow > 0 || R->Halo > 0) slSelfOn = true;
	static std::vector<uint16_t> s_slGroup; // groupe de sprites + 1 (0 = aucun)
	if(slSelfOn) s_slGroup.assign((size_t)outWidth * hdHeight, 0);
	// ====================================================================
	// <m7Effect> fog / magicfog / clouds / flare : PREPARATION par vue (passes sur l'image finale)
	// ====================================================================
	struct AtmoFx { const SnesHdM7EffectInfo* Fx; double Cu, Cv; };
	struct FlareFx { SnesHdM7EffectInfo* Fx; int Key; float Sx, Sy, GroundRow; bool Visible; };
	auto& hdEffectsA = SnesHdPackLoader::ActivePack->M7Effects;
	std::vector<std::vector<AtmoFx>> atmoLists(m7Views.size());
	std::vector<std::vector<FlareFx>> flareLists(m7Views.size());
	bool atmoOn = false, flareOn = false;
	if(!hdEffectsA.empty() && !m7Views.empty()) {
		for(size_t v = 0; v < m7Views.size(); v++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
			const uint32_t viewMap = m7Lines[zn.Y1].Img ? m7Lines[zn.Y1].Ref.M7MapHash : 0;
			for(auto& ufx : hdEffectsA) {
				SnesHdM7EffectInfo& fx = *ufx;
				if(fx.Type < 2) continue;
				if(!SnesHdMapMatch(fx.AnyMap, fx.MapHashes, fx.MapHash, viewMap)) continue;
				if(!fx.MatchesCondition()) continue;
				auto addAt = [&](int key, double bu, double bv) {
					double u, w, h;
					fxPos(fx, bu, bv, u, w, h);
					u = zn.LargeMap ? u : u + 1024.0 * std::round((zn.Cx - u) / 1024.0);
					w = zn.LargeMap ? w : w + 1024.0 * std::round((zn.Cy - w) / 1024.0);
					if(fx.Type == 4) {
						FlareFx fl; fl.Fx = &fx; fl.Key = key; fl.Visible = false; fl.GroundRow = -1;
						if(fx.Sun) {
							// soleil a l'infini : projection directe avec le stenope de la vue
							const double az = fx.SunAz * 0.01745329, el = fx.SunEl * 0.01745329;
							const double dx = std::cos(az), dy = std::sin(az);
							const double dF = dx * zn.Fx + dy * zn.Fy;
							if(dF > 0.05) {
								const double sx = zn.AxisX + (dx * zn.Rx + dy * zn.Ry) / dF * zn.Focal;
								const double sy = zn.Horizon - std::tan(el) / dF * zn.Focal;
								fl.Sx = (float)((sx + dstX) * scale); fl.Sy = (float)((sy + dstY) * scale);
								fl.Visible = true;
							}
						} else {
							double sx, sy, ppt;
							if(zn.Project(u, w, h, sx, sy, ppt) && zn.DepthOf(u, w) > 2.0) {
								fl.Sx = (float)((sx + dstX) * scale); fl.Sy = (float)((sy + dstY) * scale);
								fl.GroundRow = (float)(sy + h * ppt);
								fl.Visible = true;
							}
						}
						flareLists[v].push_back(fl); // meme invisible : sa visibilite lissee redescend
						flareOn = true;
					} else {
						atmoLists[v].push_back({ &fx, u, w });
						atmoOn = true;
					}
				};
				if(fx.OnTile) {
					if(m7CellHash.empty()) continue;
					for(size_t c = 0; c < m7CellHash.size(); c++) {
						if(m7CellHash[c] == fx.TileHash) addAt((int)c, (double)((c % 128) * 8 + 4) + fx.Dx, (double)((c / 128) * 8 + 4) + fx.Dy);
					}
				} else {
					addAt(-1, fx.X, fx.Y);
				}
			}
		}
	}
	const bool fxPostOn = atmoOn || flareOn;

	// masque HD "pas d'eclairage / pas d'effet d'ecran" (HUD, sprites hors du sol, <NoLightFXOnSprite>)
	const bool maskOn = lightsOn || fxPostOn;
	// <NoLightFXOnSprite> : regles actives cette frame (utilisees pendant la composition)
	bool noLightAll = false;
	std::vector<const SnesHdNoLightRule*> noLight;
	if(maskOn) {
		for(auto& ur : SnesHdPackLoader::ActivePack->NoLightRules) {
			if(!ur->MatchesCondition()) continue;
			if(ur->AllSprites) noLightAll = true; else noLight.push_back(ur.get());
		}
	}
	auto noLightTile = [&](const SnesHdPackTileInfo* t) -> bool {
		if(noLightAll) return true;
		if(noLight.empty() || !t) return false;
		for(const SnesHdNoLightRule* r : noLight) {
			if(t->MemoryHash != r->Hash) continue;
			if(r->AnyPalette || t->PaletteHash == 0xFFFFFFFF || t->PaletteHash == r->Palette) return true;
		}
		return false;
	};
	// <FrontOf3D> : tiles / sprites gardes devant le relief, les panneaux, les modeles et les effets.
	// On leur donne une "ligne des pieds" tres basse (kFront) : tout objet 3D les considere plus proches.
	const float kFront = 1.0e7f;
	bool frontAll = false, frontHud = false;
	std::vector<const SnesHdNoLightRule*> frontRules;
	for(auto& ur : SnesHdPackLoader::ActivePack->FrontOf3DRules) {
		if(!ur->MatchesCondition()) continue;
		if(ur->AllSprites) frontAll = true; else if(ur->OffGround) frontHud = true; else frontRules.push_back(ur.get());
	}
	auto frontTile = [&](const SnesHdPackTileInfo* t) -> bool {
		if(frontRules.empty() || !t) return false;
		for(const SnesHdNoLightRule* r : frontRules) {
			if(t->MemoryHash != r->Hash) continue;
			if(r->AnyPalette || t->PaletteHash == 0xFFFFFFFF || t->PaletteHash == r->Palette) return true;
		}
		return false;
	};
	// Masque HD "pas d'eclairage" : pixels REELLEMENT dessines par un sprite hors du sol (HUD...) ou exclu
	static std::vector<uint8_t> s_noLight;
	if(maskOn) s_noLight.assign((size_t)outWidth * hdHeight, 0);

	// ====================================================================
	// <m7Effect> (flame, smoke) : SIMULATION + PROJECTION (sequentielle)
	// Emetteurs ponctuels ; chaque emetteur (ou chaque case pour tile=) garde ses particules,
	// en coordonnees relatives a lui. Un emetteur visible dans 2 vues n'avance qu'une fois par frame.
	// ====================================================================
	struct FxDraw { float Cx, Cy, HalfW, HalfH, A, R, G, B, Depth, GroundRow; bool Add; const SnesHdPackBitmapInfo* Tex; };
	auto& hdEffects = SnesHdPackLoader::ActivePack->M7Effects;
	std::vector<std::vector<FxDraw>> fxDraws(m7Views.size());
	bool fxOn = false;
	if(!hdEffects.empty() && !m7Views.empty()) {
		const uint32_t frameNo = SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber;
		const float dt = 1.0f / 60.0f;
		for(size_t v = 0; v < m7Views.size(); v++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
			const uint32_t viewMap = m7Lines[zn.Y1].Img ? m7Lines[zn.Y1].Ref.M7MapHash : 0;
			std::vector<FxDraw>& dl = fxDraws[v];
			for(auto& ufx : hdEffects) {
				SnesHdM7EffectInfo& fx = *ufx;
				if(fx.Type >= 2) continue; // fog, magicfog, flare, clouds : passes d'ecran plus bas
				if(!SnesHdMapMatch(fx.AnyMap, fx.MapHashes, fx.MapHash, viewMap)) continue;
				if(!fx.MatchesCondition()) continue;
				const SnesHdPackBitmapInfo* tex = (fx.Texture && !fx.Texture->PixelData.empty() && fx.Texture->Width > 0) ? fx.Texture : nullptr;
				const float texAspect = tex ? (float)tex->Height / (float)tex->Width : 1.0f;
				auto emit = [&](int key, double u0, double v0) {
					// reference R (case, ou debut du trajet si x/y non utilises) ; les particules restent
					// dans le monde, relatives a R : l'emetteur se deplace, la trainee reste derriere lui
					if(!fx.OnTile && fx.HasMotion && fx.Motion.Path.size() >= 2) { u0 = fx.Motion.Path[0][0]; v0 = fx.Motion.Path[0][1]; }
					double pu, pv, ph;
					fxPos(fx, fx.OnTile ? u0 : fx.X, fx.OnTile ? v0 : fx.Y, pu, pv, ph);
					const float offU = (float)(pu - u0), offV = (float)(pv - v0), offH = (float)(ph - fx.Elevation);
					const double U0 = zn.LargeMap ? u0 : u0 + 1024.0 * std::round((zn.Cx - u0) / 1024.0);
					const double V0 = zn.LargeMap ? v0 : v0 + 1024.0 * std::round((zn.Cy - v0) / 1024.0);
					// emetteur hors de vue (au-dela de l'horizon / derriere la camera) : ni simule ni dessine
					double sx0, sy0, ppt0;
					if(!zn.Project(U0 + offU, V0 + offV, ph, sx0, sy0, ppt0) && !zn.Project(U0 + offU, V0 + offV, ph + 32.0, sx0, sy0, ppt0)) return;
					SnesHdM7EffectInfo::Emitter& e = fx.Emitters[key];
					if(!e.Initialized) {
						e.Initialized = true;
						e.Rng ^= (uint32_t)(key + 7) * 2654435761u;
						int steps = (int)std::min(fx.LifeMax * 60.0f, 600.0f); // deja allume
						for(int s = 0; s < steps; s++) fx.Step(e, dt, offU, offV, offH);
						e.LastFrame = frameNo;
					}
					if(e.LastFrame != frameNo) { fx.Step(e, dt, offU, offV, offH); e.LastFrame = frameNo; }
					for(const SnesHdFxParticle& p : e.Parts) {
						const double u = U0 + p.Du, w = V0 + p.Dv, h = fx.Elevation + p.H;
						double sx, sy, ppt;
						if(!zn.Project(u, w, h, sx, sy, ppt)) continue;
						const double D = zn.DepthOf(u, w);
						if(D < 2.0) continue;
						const float t = p.Age / p.Life;
						float a = fx.AlphaStart + (fx.AlphaEnd - fx.AlphaStart) * t;
						if(fx.FadeIn > 0 && t < fx.FadeIn) a *= t / fx.FadeIn;
						if(fx.NearFade > 0 && D < fx.NearFade) a *= (float)(D / fx.NearFade);
						float sizePx = p.Size * (fx.SizeStart + (fx.SizeEnd - fx.SizeStart) * t) * (float)ppt * (float)scale;
						if(sizePx <= 0) continue;
						if(sizePx < fx.MinPixelSize) { a *= sizePx / fx.MinPixelSize; sizePx = fx.MinPixelSize; }
						if(a <= 0.003f) continue;
						// couleur sur 3 etapes : debut -> milieu -> fin de vie
						const uint32_t ca = t < 0.5f ? fx.Color0 : fx.Color1, cb = t < 0.5f ? fx.Color1 : fx.Color2;
						const float f = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
						auto lc = [&](int sh) { float x0 = (float)((ca >> sh) & 0xFF), x1 = (float)((cb >> sh) & 0xFF); return (x0 + (x1 - x0) * f) / 255.0f; };
						FxDraw d;
						d.Cx = (float)((sx + dstX) * scale); d.Cy = (float)((sy + dstY) * scale);
						d.HalfW = sizePx * 0.5f; d.HalfH = d.HalfW * texAspect;
						d.A = std::min(a, 1.0f); d.R = lc(16); d.G = lc(8); d.B = lc(0);
						d.Depth = (float)D; d.GroundRow = (float)(sy + h * ppt);
						d.Add = fx.Additive; d.Tex = tex;
						dl.push_back(d);
					}
				};
				if(fx.OnTile) {
					if(m7CellHash.empty()) continue;
					for(size_t c = 0; c < m7CellHash.size(); c++) {
						if(m7CellHash[c] == fx.TileHash) emit((int)c, (double)((c % 128) * 8 + 4) + fx.Dx, (double)((c / 128) * 8 + 4) + fx.Dy);
					}
				} else {
					emit(-1, fx.X, fx.Y);
				}
			}
			std::sort(dl.begin(), dl.end(), [](const FxDraw& a, const FxDraw& b) { return a.Depth > b.Depth; });
			if(!dl.empty()) fxOn = true;
		}
	}

	static std::vector<float> s_sprRow;
	bool p3Occlusion = reliefOn || bbOn || mdOn || lightsOn || fxOn || fxPostOn; // carte des sprites utile aussi au relief, aux panneaux, aux modeles et aux lumieres
	for(auto& dl : p3Draws) { if(!dl.empty()) { p3Occlusion = true; break; } }
	// Identifiant de sprite = numero OAM + 128 x bande d'ecran (vue Mode 7 qui contient la ligne).
	// Super Mario Kart recharge l'OAM en milieu d'image : la meme entree sert a un sprite de la vue du haut
	// ET a un sprite de la carte du bas. Sans la bande, les deux etaient fusionnes (pieds pris dans l'autre vue).
	const int sprBands = (int)m7Views.size() + 1;
	const int sprIds = 128 * sprBands;
	std::vector<int> sprRowBand(cropHeight, sprBands - 1);
	for(size_t v = 0; v < m7Views.size(); v++) {
		const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
		for(int y = std::max(0, zn.AreaY0); y <= zn.AreaY1 && y < (int)cropHeight; y++) sprRowBand[y] = (int)v;
	}
	auto sprId = [&](int oam, int y) -> int { return sprRowBand[std::clamp(y, 0, (int)cropHeight - 1)] * 128 + oam; };
	// tile HD de sprite : bande de sa ligne centrale (startY est en coordonnees de sortie)
	auto itemSprId = [&](const ExpandedTile& it) -> int { return sprId(it.oamIndex, it.startY - (int)dstY + (it.Tile ? (int)it.Tile->Height / 2 : 4)); };
	std::vector<float> sprGroupRow(sprIds, -1.0f);
	std::vector<int> sprGroupRoot(sprIds); // groupe (sprites qui se touchent) de chaque sprite
	for(int i = 0; i < sprIds; i++) sprGroupRoot[i] = i;
	if(p3Occlusion) {
		std::vector<int> parent(sprIds);
		for(int i = 0; i < sprIds; i++) parent[i] = i;
		auto findRoot = [&](int a) { while(parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
		std::vector<int> bottom(sprIds, -1);
		const uint32_t fw = _baseFrameInfo.Width;
		auto opaqueOam = [&](uint32_t x, uint32_t y) -> int {
			const SnesHdPixelData& px = hdBuffer[(y + srcY) * fw + x + srcX];
			return (px.IsSprite && px.OamIndex < 128 && px.BaseColor != 0) ? sprId((int)px.OamIndex, (int)y) : -1;
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
		std::vector<int> groupBottom(sprIds, -1);
		for(int i = 0; i < sprIds; i++) {
			if(bottom[i] >= 0) { int r = findRoot(i); groupBottom[r] = std::max(groupBottom[r], bottom[i]); }
		}
		for(int i = 0; i < sprIds; i++) {
			if(bottom[i] < 0) continue;
			int feet = groupBottom[findRoot(i)];
			sprGroupRoot[i] = findRoot(i);
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
				const int sid = sprId((int)px.OamIndex, (int)y);
				float r = sprGroupRow[sid];
				if(maskOn && (r < 0 || noLightAll)) {
					for(uint32_t sy2 = 0; sy2 < scale; sy2++) {
						uint32_t hy = (y + dstY) * scale + sy2;
						if(hy >= hdHeight) break;
						for(uint32_t sx2 = 0; sx2 < scale; sx2++) {
							uint32_t hx = (x + dstX) * scale + sx2;
							if(hx < outWidth) s_noLight[(size_t)hy * outWidth + hx] = 1;
						}
					}
				}
				if(slSelfOn) {
					for(uint32_t sy2 = 0; sy2 < scale; sy2++) {
						uint32_t hy = (y + dstY) * scale + sy2;
						if(hy >= hdHeight) break;
						for(uint32_t sx2 = 0; sx2 < scale; sx2++) {
							uint32_t hx = (x + dstX) * scale + sx2;
							if(hx < outWidth) s_slGroup[(size_t)hy * outWidth + hx] = (uint16_t)(sid + 1);
						}
					}
				}
				if(frontAll || (frontHud && r < 0)) r = kFront; // <FrontOf3D>
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
	// <SpriteLight> : POSITION 3D des lumieres de sprites (un point par groupe de sprites et par regle)
	// x = centre des tiles concernees, sol = ligne des pieds du groupe (point de la carte sous lui),
	// hauteur = hauteur du centre des tiles au-dessus du sol (auto) ou height=.
	// ====================================================================
	// <m7CameraDebug> : cadres autour des tiles reconnues + repere de chaque lumiere de sprite
	struct SlDebugBox { int X0, Y0, X1, Y1, Key; };
	struct SlDebugLight { int View; float U, V, H; uint32_t Color; };
	std::vector<SlDebugBox> slDbgBoxes;
	std::vector<SlDebugLight> slDbgLights;
	std::unordered_set<int> slLitKeys;
	std::vector<int> slRep(sprIds, -1); // self < 1 / glow : groupe de lumiere de chaque sprite (pixels du porteur)
	struct SlGlow { int Group; int X0, Y0, X1, Y1; float R, G, B, Glow, HaloPx; }; // rectangle en pixels HD
	std::vector<SlGlow> slGlows;
	if(sprLightCand) {
		const float tSec = (float)SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber / 60.0f;
		// Groupes PROPRES aux tiles reconnues : seules les tiles de la regle comptent (un champignon qui touche
		// Lakitu ou un kart n'est plus fusionne avec eux). Tiles du meme sprite, ou qui se touchent = une lumiere.
		// Pieds = bas des tiles reconnues (et non du groupe de sprites, qui peut contenir d'autres objets).
		struct SlTile { int Ri, Id, X0, Y0, X1, Y1; }; // rectangle en pixels SNES de la vue (inclus)
		std::vector<SlTile> slTiles;
		for(const auto& item : tilesToExpand) {
			if(item.oamIndex >= 128 || !item.Tile) continue;
			for(size_t ri = 0; ri < sprLightRules.size(); ri++) {
				if(!sprLightRules[ri]->Matches(item.Tile)) continue;
				const int x0 = item.startX - (int)dstX, y0 = item.startY - (int)dstY;
				slTiles.push_back({ (int)ri, itemSprId(item), x0, y0, x0 + (int)item.Tile->Width - 1, y0 + (int)item.Tile->Height - 1 });
			}
		}
		std::vector<int> slPar(slTiles.size());
		for(size_t i = 0; i < slTiles.size(); i++) slPar[i] = (int)i;
		auto slFind = [&](int a) { while(slPar[a] != a) { slPar[a] = slPar[slPar[a]]; a = slPar[a]; } return a; };
		for(size_t i = 0; i < slTiles.size(); i++) {
			for(size_t j = i + 1; j < slTiles.size(); j++) {
				const SlTile& A = slTiles[i]; const SlTile& Bt = slTiles[j];
				// meme regle, ou meme bloc <SpriteLight>{ } (chaque <tile> du bloc est une regle : les moities
				// d'un meme champignon ont des hashs differents)
				const int bA = sprLightRules[A.Ri]->BlockId, bB = sprLightRules[Bt.Ri]->BlockId;
				if(!(A.Ri == Bt.Ri || (bA >= 0 && bA == bB)) || A.Id / 128 != Bt.Id / 128) continue;
				// meme sprite, ou a moins d'une tile (8 px) : SMK dessine parfois un objet en morceaux un peu ecartes
				const int G = 8;
				const bool touch = A.Id == Bt.Id || (A.X0 <= Bt.X1 + G && Bt.X0 <= A.X1 + G && A.Y0 <= Bt.Y1 + G && Bt.Y0 <= A.Y1 + G);
				if(touch) slPar[slFind((int)j)] = slFind((int)i);
			}
		}
		struct SlAcc { double Sx = 0, Sy = 0; int N = 0; int Band = 0; int Feet = -1; int X0 = 1 << 30, Y0 = 1 << 30, X1 = -(1 << 30), Y1 = -(1 << 30); };
		std::unordered_map<int, SlAcc> acc; // (tile representative + 1) x 1024 + regle
		for(size_t i = 0; i < slTiles.size(); i++) {
			const SlTile& t = slTiles[i];
			const int root = slFind((int)i);
			const int key = (root + 1) * 1024 + slTiles[root].Ri; // regle du representant (memes reglages dans un bloc)
			SlAcc& a = acc[key];
			a.Sx += (t.X0 + t.X1 + 1) * 0.5; a.Sy += (t.Y0 + t.Y1 + 1) * 0.5;
			a.N++; a.Band = t.Id / 128; a.Feet = std::max(a.Feet, t.Y1 + 1);
			a.X0 = std::min(a.X0, t.X0); a.Y0 = std::min(a.Y0, t.Y0); a.X1 = std::max(a.X1, t.X1); a.Y1 = std::max(a.Y1, t.Y1);
			if(slSelfOn) slRep[t.Id] = key / 1024;
			if(m7DebugOn) slDbgBoxes.push_back({ t.X0 + (int)dstX, t.Y0 + (int)dstY, t.X1 + (int)dstX, t.Y1 + (int)dstY, key });
		}
		for(auto& kv : acc) {
			const SnesHdSpriteLightInfo& R = *sprLightRules[kv.first % 1024];
			const SlAcc& a = kv.second;
			const double cx = a.Sx / a.N, cy = a.Sy / a.N;
			const double feet = a.Feet;
			const int band = a.Band; // vue qui contient le sprite
			if(R.Glow > 0 || R.Halo > 0) {
				// lueur propre : independante de la position 3D (marche aussi pour Lakitu en l'air)
				float flick = 1.0f;
				if(R.Flicker > 0) {
					const float seed = (float)(kv.first / 1024) * 1.37f + (float)(kv.first % 1024) * 0.71f;
					float n = 0.6f * std::sin(tSec * R.FlickerSpeed + seed) + 0.4f * std::sin(tSec * R.FlickerSpeed * 2.31f + seed * 1.7f);
					flick = 1.0f - R.Flicker * (0.5f + 0.5f * n);
				}
				const int sc = (int)scale;
				slGlows.push_back({ kv.first / 1024, (a.X0 + (int)dstX) * sc, (a.Y0 + (int)dstY) * sc, (a.X1 + 1 + (int)dstX) * sc - 1, (a.Y1 + 1 + (int)dstY) * sc - 1,
					(float)((R.Color >> 16) & 0xFF) * flick, (float)((R.Color >> 8) & 0xFF) * flick, (float)(R.Color & 0xFF) * flick, R.Glow, R.Halo * (float)sc });
			}
			for(size_t v = 0; v < m7Views.size(); v++) {
				if((int)v != band) continue; // la lumiere nait dans la vue du sprite (elle se voit ensuite partout)
				const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
				if(!zn.Perspective && !R.PlaneViews) continue; // plane=0 : pas sur la carte vue de dessus
				double u, w; float h;
				if(feet > zn.Y0 + 1 && feet <= zn.Y1 + 1) {
					// pose au sol : point de la carte sous ses pieds, hauteur = centre des tiles au-dessus du sol
					if(feet <= zn.Y0 + 1 || feet > zn.Y1 + 1) continue;
					if(!zn.Unproject(cx, std::clamp(feet - 0.5, (double)zn.Y0, (double)zn.Y1), u, w)) continue;
					double gsx, gsy, gppt;
					if(!zn.Project(u, w, 0.0, gsx, gsy, gppt) || gppt <= 0) continue;
					// auto : centre des tiles au-dessus du sol, limite a la moitie du rayon (les sprites lointains
					// de SMK sont plus grands que la perspective du sol : sans limite, la lumiere passerait au-dessus)
					h = R.Height >= 0 ? R.Height : (float)std::min(0.5 * R.Radius, std::max(0.0, (feet - cy) / gppt));
				} else {
					// hors du sol (Lakitu, objet en l'air) : point "en l'air" a distance=N devant la camera,
					// dans la direction du centre du sprite (meme stenope que la vue)
					if(R.AirDistance <= 0 || !zn.HasPinhole) continue;
					if(cy < zn.AreaY0 || cy > zn.AreaY1 + 1) continue;
					const double D = R.AirDistance, ppt = zn.Focal / D;
					const double L = (cx - zn.AxisX) / ppt;
					u = zn.Cx + zn.Fx * D + zn.Rx * L; w = zn.Cy + zn.Fy * D + zn.Ry * L;
					h = R.Height >= 0 ? R.Height : (float)std::max(0.0, zn.Height - (cy - zn.Horizon) / ppt);
				}
				const double r = R.Radius;
				const int sc = (int)scale;
				const int zoneTopHD = (zn.Y0 + (int)dstY) * sc, areaBotHD = (zn.AreaY1 + 1 + (int)dstY) * sc - 1;
				const double px[5] = { u, u - zn.Fx * r, u + zn.Fx * r, u - zn.Rx * r, u + zn.Rx * r };
				const double py[5] = { w, w - zn.Fy * r, w + zn.Fy * r, w - zn.Ry * r, w + zn.Ry * r };
				bool okP[5]; int nOk = 0;
				float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, pptMax = 0;
				for(int k = 0; k < 5; k++) {
					double sx, sy, ppt;
					okP[k] = zn.Project(px[k], py[k], 0.0, sx, sy, ppt) && zn.DepthOf(px[k], py[k]) > 1.0;
					if(!okP[k]) continue;
					nOk++;
					float hx = (float)((sx + dstX) * scale), hy = (float)((sy + dstY) * scale);
					x0 = std::min(x0, hx); x1 = std::max(x1, hx); y0 = std::min(y0, hy); y1 = std::max(y1, hy);
					pptMax = std::max(pptMax, (float)ppt);
				}
				if(nOk == 0) continue;
				if(!okP[1]) { x0 = 0; x1 = (float)outWidth - 1; y1 = (float)areaBotHD; }
				if(!okP[2]) y0 = (float)zoneTopHD;
				const float m = (float)(r * pptMax * scale);
				LightDraw d;
				d.X0 = std::max(0, (int)std::floor(x0 - m)); d.X1 = std::min((int)outWidth - 1, (int)std::ceil(x1 + m));
				d.Y0 = std::max(zoneTopHD, (int)std::floor(y0 - 2)); d.Y1 = std::min(areaBotHD, (int)std::ceil(y1 + 2));
				if(d.X0 > d.X1 || d.Y0 > d.Y1) continue;
				float flick = 1.0f;
				if(R.Flicker > 0) {
					const float seed = (float)(kv.first / 1024) * 1.37f + (float)(kv.first % 1024) * 0.71f;
					float n = 0.6f * std::sin(tSec * R.FlickerSpeed + seed) + 0.4f * std::sin(tSec * R.FlickerSpeed * 2.31f + seed * 1.7f);
					flick = 1.0f - R.Flicker * (0.5f + 0.5f * n);
				}
				const float k = R.Intensity * flick / 255.0f;
				d.U = (float)u; d.V = (float)w; d.H = h;
				d.R2 = (float)(r * r); d.InvR2 = 1.0f / d.R2;
				d.Cr = ((R.Color >> 16) & 0xFF) * k; d.Cg = ((R.Color >> 8) & 0xFF) * k; d.Cb = (R.Color & 0xFF) * k;
				if(R.Shadow) { d.Cr = -(255 - ((R.Color >> 16) & 0xFF)) * k; d.Cg = -(255 - ((R.Color >> 8) & 0xFF)) * k; d.Cb = -(255 - (R.Color & 0xFF)) * k; }
				d.Add = R.Additive;
				d.Group = kv.first / 1024; d.Self = R.SelfLight;
				viewLights[v].Lights.push_back(d);
				slLitKeys.insert(kv.first);
				if(m7DebugOn) slDbgLights.push_back({ (int)v, (float)u, (float)w, h, R.Color });
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

	// ====================================================================
	// <m7model> GPU : fusion de l'image de la carte graphique sur une bande (appele par composeBand)
	// couleur premultipliee + ligne du sol par pixel ; memes regles de profondeur que le moteur processeur
	// ====================================================================
	auto renderGpuModelBand = [&](uint32_t bandY0, uint32_t bandY1) {
		// seulement le rectangle relu depuis la carte graphique
		const int y0 = std::max((int)bandY0, gpuRy0), y1 = std::min((int)std::min(bandY1, hdHeight) - 1, gpuRy1);
		const int rw = gpuRx1 - gpuRx0 + 1;
		for(int yy = y0; yy <= y1; yy++) {
			const uint32_t* srcC = &s_gpuColor[(size_t)(yy - gpuRy0) * rw];
			const float* srcG = &s_gpuGround[(size_t)(yy - gpuRy0) * rw];
			for(int xx = gpuRx0; xx <= gpuRx1; xx++) {
				const uint32_t idx = (uint32_t)yy * outWidth + (uint32_t)xx;
				const uint32_t c = srcC[xx - gpuRx0];
				const uint32_t a8 = c >> 24;
				if(a8 == 0) continue;
				if(kReliefZ < _zBuffer[idx]) continue;
				const float gr = srcG[xx - gpuRx0];
				if(p3Occlusion) {
					const float sr2 = s_sprRow[idx];
					if(sr2 >= 0.0f && (gr < 0.0f || gr < sr2)) continue; // sprite / relief / panneau plus proche
				}
				const float a = a8 / 255.0f, ia = 1.0f - a;
				const uint32_t d = out[idx];
				const uint32_t r = std::min(255u, (uint32_t)(((d >> 16) & 0xFF) * ia + ((c >> 16) & 0xFF) + 0.5f));
				const uint32_t g = std::min(255u, (uint32_t)(((d >> 8) & 0xFF) * ia + ((c >> 8) & 0xFF) + 0.5f));
				const uint32_t b = std::min(255u, (uint32_t)((d & 0xFF) * ia + (c & 0xFF) + 0.5f));
				out[idx] = 0xFF000000 | (r << 16) | (g << 8) | b;
				if(a8 >= 128) {
					_zBuffer[idx] = kReliefZ;
					if(p3Occlusion && gr >= 0.0f) s_sprRow[idx] = gr;
				}
			}
		}
	};

	// ====================================================================
	// <m7Effect> : dessin des particules d'une vue sur une bande (appele par composeBand)
	// ====================================================================
	const uint8_t kFxZ = 95;
	auto renderFxBand = [&](size_t viewIndex, uint32_t bandY0, uint32_t bandY1) {
		const SnesHdM7Zone& zn = m7Zones[m7Views[viewIndex]];
		const int clipY0 = std::max((int)bandY0, (zn.AreaY0 + (int)dstY) * (int)scale);
		const int clipY1 = std::min((int)bandY1, (zn.AreaY1 + 1 + (int)dstY) * (int)scale) - 1;
		if(clipY0 > clipY1) return;
		for(const FxDraw& d : fxDraws[viewIndex]) {
			int x0 = std::max(0, (int)std::floor(d.Cx - d.HalfW)), x1 = std::min((int)outWidth - 1, (int)std::ceil(d.Cx + d.HalfW));
			int y0 = std::max(clipY0, (int)std::floor(d.Cy - d.HalfH)), y1 = std::min(clipY1, (int)std::ceil(d.Cy + d.HalfH));
			if(x0 > x1 || y0 > y1) continue;
			const SnesHdPackBitmapInfo* tex = d.Tex;
			const int texW = tex ? (int)tex->Width : 0, texH = tex ? (int)tex->Height : 0;
			for(int yy = y0; yy <= y1; yy++) {
				const float dy = (float)yy + 0.5f - d.Cy;
				for(int xx = x0; xx <= x1; xx++) {
					const float dx = (float)xx + 0.5f - d.Cx;
					float sa, sr, sg, sb;
					if(tex) {
						int iu = (int)((dx / d.HalfW * 0.5f + 0.5f) * texW), iv = (int)((dy / d.HalfH * 0.5f + 0.5f) * texH);
						if(iu < 0 || iv < 0 || iu >= texW || iv >= texH) continue;
						uint32_t c = tex->PixelData[(size_t)iv * texW + iu];
						sa = (float)(c >> 24) / 255.0f; if(sa <= 0.002f) continue;
						sr = (float)((c >> 16) & 0xFF); sg = (float)((c >> 8) & 0xFF); sb = (float)(c & 0xFF);
					} else {
						float dd = std::sqrt(dx * dx + dy * dy) / d.HalfW;
						if(dd >= 1.0f) continue;
						float s = 1.0f - dd * dd;
						sa = s * s; sr = sg = sb = 255.0f;
					}
					const float aa = sa * d.A;
					if(aa < 0.004f) continue;
					const uint32_t idx = (uint32_t)yy * outWidth + (uint32_t)xx;
					if(kFxZ < _zBuffer[idx]) continue;
					if(p3Occlusion) { const float sr2 = s_sprRow[idx]; if(sr2 >= 0.0f && d.GroundRow < sr2) continue; } // derriere un objet
					const uint32_t dstC = out[idx];
					const float dr = (float)((dstC >> 16) & 0xFF), dg = (float)((dstC >> 8) & 0xFF), db = (float)(dstC & 0xFF);
					const float cr = sr * d.R, cg = sg * d.G, cb = sb * d.B;
					float nr, ng, nb;
					if(d.Add) { nr = std::min(255.0f, dr + cr * aa); ng = std::min(255.0f, dg + cg * aa); nb = std::min(255.0f, db + cb * aa); }
					else { nr = dr + (cr - dr) * aa; ng = dg + (cg - dg) * aa; nb = db + (cb - db) * aa; }
					out[idx] = 0xFF000000 | ((uint32_t)(nr + 0.5f) << 16) | ((uint32_t)(ng + 0.5f) << 8) | (uint32_t)(nb + 0.5f);
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

	// <m7model> GPU : recuperation de l'image dessinee pendant la phase Mode 7 / sprites
	if(gpuModelsOn) gpuModelsOn = SnesHdGpu::Get().FetchModels(s_gpuColor, s_gpuGround);

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
	if(gpuModelsOn) tasks.push_back({ (int)kReliefZ, 7, 0, 0 }); // modeles 3D dessines par la carte graphique
	for(size_t v = 0; v < fxDraws.size(); v++) {
		if(!fxDraws[v].empty()) tasks.push_back({ (int)kFxZ, 8, 0, v }); // <m7Effect> (flammes, fumee)
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
		if(task.type == 7) {
			renderGpuModelBand(bandY0, bandY1);
			continue;
		}
		if(task.type == 8) {
			renderFxBand(task.index, bandY0, bandY1);
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
							if(p3Occlusion && alpha >= 128) {
								const bool isSpr = item.oamIndex < 128;
								const int sid = isSpr ? itemSprId(item) : 0;
								const bool front = frontTile(item.Tile) || (isSpr && (frontAll || (frontHud && sprGroupRow[sid] < 0.0f))); // <FrontOf3D>
								s_sprRow[pixelIndex] = front ? kFront : (isSpr ? sprGroupRow[sid] : -1.0f);
								if(slSelfOn) s_slGroup[pixelIndex] = isSpr ? (uint16_t)(sid + 1) : 0;
							}
							if(maskOn && alpha >= 128) s_noLight[pixelIndex] = (item.oamIndex < 128 && (sprGroupRow[itemSprId(item)] < 0.0f || noLightTile(item.Tile))) ? 1 : 0;
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
	// <m7light> / <m7ambient> : ECLAIRAGE de l'image finale (parallele, par bandes)
	// Pixel du sol -> point de la carte (Unproject) ; pixel d'objet pose au sol (sprite, relief,
	// panneau, modele) -> point de sol sous ses pieds (s_sprRow). Couleur x (ambiance + lumieres)
	// + lumieres additives. Le ciel au-dessus de la piste ne recoit que l'ambiance (si sky=1).
	// ====================================================================
	if(lightsOn) {
		auto lightBand = [&](uint32_t b0, uint32_t b1) {
			const int sc = (int)scale;
			std::vector<float> mr(outWidth), mg(outWidth), mb(outWidth), ar(outWidth), ag(outWidth), ab(outWidth), wu(outWidth), wv(outWidth);
			std::vector<char> wok(outWidth);
			std::vector<int> feetHD(outWidth); // -1 = sol ; sinon ligne HD des pieds de l'objet dessine a ce pixel
			for(size_t v = 0; v < m7Views.size(); v++) {
				const ViewLighting& VL = viewLights[v];
				if(!VL.AmbientOn && VL.Lights.empty()) continue;
				const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
				const int areaTop = (zn.AreaY0 + (int)dstY) * sc, zoneTop = (zn.Y0 + (int)dstY) * sc;
				const int areaBot = std::min((int)hdHeight - 1, (zn.AreaY1 + 1 + (int)dstY) * sc - 1);
				const int y0 = std::max((int)b0, areaTop), y1 = std::min((int)b1 - 1, areaBot);
				for(int yy = y0; yy <= y1; yy++) {
					const bool sky = yy < zoneTop;
					// un objet pose au sol (modele, panneau, sprite...) est eclaire depuis le point sous ses pieds :
					// il peut depasser AU-DESSUS de la zone d'une lumiere, et meme de l'horizon
					const float* sprY = p3Occlusion ? &s_sprRow[(size_t)yy * outWidth] : nullptr;
					bool anyLight = false, anyGround = false;
					int lx0 = (int)outWidth, lx1 = -1;
					for(const LightDraw& d : VL.Lights) {
						if(!sky && yy >= d.Y0 && yy <= d.Y1) anyGround = true;
						if((!sky && yy >= d.Y0 && yy <= d.Y1) || (sprY && yy <= d.Y1)) { anyLight = true; lx0 = std::min(lx0, d.X0); lx1 = std::max(lx1, d.X1); }
					}
					if(sky && !VL.Sky && !sprY) continue;
					if(!VL.AmbientOn && !anyLight) continue;
					std::fill(mr.begin(), mr.end(), VL.Ar); std::fill(mg.begin(), mg.end(), VL.Ag); std::fill(mb.begin(), mb.end(), VL.Ab);
					std::fill(ar.begin(), ar.end(), 0.0f); std::fill(ag.begin(), ag.end(), 0.0f); std::fill(ab.begin(), ab.end(), 0.0f);
					if(anyLight) {
						// point de la carte sous chaque pixel de la ligne (calcule une fois)
						const double sy = ((double)yy + 0.5) / sc - (double)dstY;
						for(int xx = lx0; xx <= lx1; xx++) {
							const double sx = ((double)xx + 0.5) / sc - (double)dstX;
							double gRow = sy;
							feetHD[xx] = -1;
							if(sprY && sprY[xx] >= 0.0f && sprY[xx] < kFront) { gRow = sprY[xx] - 0.5 / sc; feetHD[xx] = (int)((sprY[xx] + (float)dstY) * sc) - 1; }
							else if(!anyGround) { wok[xx] = 0; continue; } // ligne hors de toute lumiere : seuls les objets comptent
							gRow = std::clamp(gRow, (double)zn.Y0, (double)zn.Y1);
							double u, w;
							wok[xx] = zn.Unproject(sx, gRow, u, w) ? 1 : 0;
							wu[xx] = (float)u; wv[xx] = (float)w;
						}
						for(const LightDraw& d : VL.Lights) {
							if(yy > d.Y1) continue;
							const bool groundRow = !sky && yy >= d.Y0;
							if(!groundRow && !sprY) continue;
							const float h2 = d.H * d.H;
							for(int xx = d.X0; xx <= d.X1; xx++) {
								if(!wok[xx]) continue;
								const int fr = feetHD[xx];
								if(fr < 0 ? !groundRow : (fr < d.Y0 || fr > d.Y1 + 2)) continue;
								const float du = wu[xx] - d.U, dv = wv[xx] - d.V;
								const float d2 = du * du + dv * dv + h2;
								if(d2 >= d.R2) continue;
								float a = 1.0f - d2 * d.InvR2; a *= a;
								if(d.Group >= 0 && slSelfOn) { const uint16_t g = s_slGroup[(size_t)yy * outWidth + xx]; if(g && slRep[g - 1] == d.Group) a *= d.Self; } // le sprite qui porte la lumiere
								if(d.Add) { ar[xx] += d.Cr * a * 255.0f; ag[xx] += d.Cg * a * 255.0f; ab[xx] += d.Cb * a * 255.0f; }
								else { mr[xx] += d.Cr * a; mg[xx] += d.Cg * a; mb[xx] += d.Cb * a; }
							}
						}
					}
					const int ax0 = VL.AmbientOn ? 0 : lx0, ax1 = VL.AmbientOn ? (int)outWidth - 1 : lx1;
					uint32_t* row = out + (size_t)yy * outWidth;
					const uint8_t* noL = &s_noLight[(size_t)yy * outWidth];
					for(int xx = ax0; xx <= ax1; xx++) {
						// sprites hors du sol (HUD, objets dans le ciel) et <NoLightFXOnSprite> : ni ambiance ni lumiere
						// (masque HD : seulement les pixels reellement dessines, pas tout le rectangle de la tile SNES)
						if(noL[xx]) continue;
						if(sky && !(sprY && sprY[xx] >= 0.0f && sprY[xx] < kFront)) {
							if(!VL.Sky) continue;                       // ciel sans ambiance
							if(!VL.AmbientOn) continue;                 // ciel : pas de lumiere ponctuelle
						}
						const uint32_t c = row[xx];
						const float r = std::clamp(((c >> 16) & 0xFF) * std::max(0.0f, mr[xx]) + ar[xx], 0.0f, 255.0f);
						const float g = std::clamp(((c >> 8) & 0xFF) * std::max(0.0f, mg[xx]) + ag[xx], 0.0f, 255.0f);
						const float b = std::clamp((c & 0xFF) * std::max(0.0f, mb[xx]) + ab[xx], 0.0f, 255.0f);
						row[xx] = (c & 0xFF000000) | ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
					}
				}
			}
		};
		unsigned hc = std::thread::hardware_concurrency();
		int nThreads = std::clamp((int)hc - 2, 1, 8);
		if(nThreads <= 1 || hdHeight < 64) {
			lightBand(0, hdHeight);
		} else {
			uint32_t bandH = (hdHeight + nThreads - 1) / nThreads;
			std::vector<std::thread> workers;
			for(int t = 0; t < nThreads; t++) {
				uint32_t b0 = (uint32_t)t * bandH, b1 = std::min(hdHeight, b0 + bandH);
				if(b0 >= b1) break;
				workers.emplace_back([&lightBand, b0, b1]() { lightBand(b0, b1); });
			}
			for(auto& w : workers) w.join();
		}
	}

	// ====================================================================
	// <SpriteLight> glow / halo : LUEUR PROPRE des sprites (apres l'eclairage, avant le brouillard)
	// Pixels du porteur (s_slGroup -> slRep) : + couleur x glow, meme s'ils sont exclus de l'eclairage
	// (HUD / en l'air : Lakitu). Halo : degrade additif autour du rectangle des tiles, sauf sur le HUD (<FrontOf3D>).
	// ====================================================================
	if(!slGlows.empty() && slSelfOn) {
		for(const SlGlow& gw : slGlows) {
			const int hp = (int)std::ceil(gw.HaloPx);
			const int x0 = std::max(0, gw.X0 - hp), x1 = std::min((int)outWidth - 1, gw.X1 + hp);
			const int y0 = std::max(0, gw.Y0 - hp), y1 = std::min((int)hdHeight - 1, gw.Y1 + hp);
			const float kOwn = gw.Glow * 0.8f, kHalo = (gw.Glow > 0 ? gw.Glow : 1.0f) * 0.55f;
			const float hcx = (gw.X0 + gw.X1 + 1) * 0.5f, hcy = (gw.Y0 + gw.Y1 + 1) * 0.5f;
			const float hrx = (gw.X1 - gw.X0 + 1) * 0.5f + gw.HaloPx, hry = (gw.Y1 - gw.Y0 + 1) * 0.5f + gw.HaloPx;
			for(int yy = y0; yy <= y1; yy++) {
				uint32_t* row = out + (size_t)yy * outWidth;
				const uint16_t* grp = &s_slGroup[(size_t)yy * outWidth];
				const float* spr = p3Occlusion ? &s_sprRow[(size_t)yy * outWidth] : nullptr;
				for(int xx = x0; xx <= x1; xx++) {
					float k;
					const uint16_t g = grp[xx];
					if(g && slRep[g - 1] == gw.Group) {
						if(kOwn <= 0) continue;
						k = kOwn;
					} else {
						if(hp <= 0) continue;
						if(spr && spr[xx] >= kFront) continue; // HUD garde devant
						// degrade radial (ellipse : demi-taille du sprite + halo), pas de bloc rectangulaire
						const float dx = ((float)xx + 0.5f - hcx) / hrx, dy = ((float)yy + 0.5f - hcy) / hry;
						const float d = std::sqrt(dx * dx + dy * dy);
						if(d >= 1.0f) continue;
						k = (1.0f - d) * (1.0f - d) * kHalo;
					}
					const uint32_t c = row[xx];
					const float r = std::min(255.0f, ((c >> 16) & 0xFF) + gw.R * k), gg = std::min(255.0f, ((c >> 8) & 0xFF) + gw.G * k), b = std::min(255.0f, (c & 0xFF) + gw.B * k);
					row[xx] = (c & 0xFF000000) | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
				}
			}
		}
	}

	// ====================================================================
	// <m7Effect> fog / magicfog / clouds : passe ATMOSPHERE (parallele, par bandes)
	// Pixel du sol -> point de la carte + distance ; objets poses au sol -> point sous leurs pieds.
	// Bruit de valeur (table 256 x 256) : nappes de brouillard, lueur magique, nuages et leurs ombres.
	// ====================================================================
	static float s_noiseTab[256 * 256];
	static bool s_noiseInit = false;
	if(!s_noiseInit) {
		uint32_t r = 0x6A09E667;
		for(float& n : s_noiseTab) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; n = (float)(r & 0xFFFF) / 65535.0f; }
		s_noiseInit = true;
	}
	// bruit periodique en x (periode px cases) : le ciel fait le tour complet sans couture
	auto vnoise = [](float x, float y, int px) -> float {
		const float fxl = std::floor(x), fyl = std::floor(y);
		const float tx = x - fxl, ty = y - fyl;
		const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
		int ix = (int)fxl, iy = (int)fyl;
		int x0 = px > 0 ? ((ix % px) + px) % px : (ix & 255), x1 = px > 0 ? (x0 + 1) % px : ((ix + 1) & 255);
		x0 &= 255; x1 &= 255;
		const int y0 = iy & 255, y1 = (iy + 1) & 255;
		const float a = s_noiseTab[y0 * 256 + x0], b = s_noiseTab[y0 * 256 + x1], c = s_noiseTab[y1 * 256 + x0], d = s_noiseTab[y1 * 256 + x1];
		return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
	};
	auto fbm = [&](float x, float y, int px) -> float {
		return 0.62f * vnoise(x, y, px) + 0.38f * vnoise(x * 2.0f + 17.3f, y * 2.0f + 5.1f, px > 0 ? px * 2 : 0);
	};
	auto smooth01 = [](float e0, float e1, float x) { float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); };
	if(atmoOn) {
		const float tA = (float)SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber / 60.0f;
		// Chaque effet = transformation affine de la couleur (c' = c x M + A) : on combine tous les effets
		// d'un point en un seul (M, A) par canal. Calcul sur une GRILLE de kS x kS pixels HD puis
		// interpolation (le brouillard varie lentement) ; les objets poses au sol sont calcules au pixel.
		struct AtmoMA { float M[3], A[3]; };
		auto evalPoint = [&](const std::vector<AtmoFx>& list, const SnesHdM7Zone& zn, bool sky, float skyRel, double colAng, double skyRows,
			float u, float v, float D, AtmoMA& o) {
			o.M[0] = o.M[1] = o.M[2] = 1.0f; o.A[0] = o.A[1] = o.A[2] = 0.0f;
			auto lerpTo = [&](float k, float cr, float cg, float cb) { // c' = c (1-k) + col k
				const float col[3] = { cr, cg, cb };
				for(int i = 0; i < 3; i++) { o.M[i] *= 1.0f - k; o.A[i] = o.A[i] * (1.0f - k) + col[i] * k; }
			};
			auto ch = [](uint32_t cc, int sh) { return (float)((cc >> sh) & 0xFF); };
			for(const AtmoFx& Af : list) {
				const SnesHdM7EffectInfo& fx = *Af.Fx;
				float regionW = 1.0f;
				if(!sky && fx.Region > 0) {
					const float du = u - (float)Af.Cu, dv = v - (float)Af.Cv;
					const float d2 = du * du + dv * dv;
					if(d2 >= fx.Region * fx.Region) continue;
					regionW = std::clamp((fx.Region - std::sqrt(d2)) / (fx.Region * 0.3f), 0.0f, 1.0f);
				}
				if(fx.Type == 2) { // brouillard
					float f;
					if(sky) {
						if(fx.Region > 0) continue;
						f = fx.Density * fx.SkyAmount * (1.0f - 0.5f * skyRel);
					} else {
						const float fd = std::clamp((D - fx.FogStart) / std::max(1.0f, fx.FogEnd - fx.FogStart), 0.0f, 1.0f) * fx.Density;
						float fn = 0;
						if(fx.Patches > 0) {
							const float n = fbm((u + tA * fx.WindX) / fx.NoiseScale, (v + tA * fx.WindY) / fx.NoiseScale, 0);
							fn = fx.Patches * smooth01(0.35f, 0.85f, n);
						}
						f = std::clamp(fd + (1.0f - fd) * fn * 0.75f, 0.0f, 1.0f) * regionW;
					}
					if(f > 0.001f) lerpTo(f, ch(fx.Color0, 16), ch(fx.Color0, 8), ch(fx.Color0, 0));
				} else if(fx.Type == 3) { // brume magique lumineuse (additive)
					if(sky) continue;
					const float n = fbm((u + tA * fx.WindX) / fx.NoiseScale, (v + tA * fx.WindY) / fx.NoiseScale, 0);
					const float pulse = 0.75f + 0.25f * std::sin(6.2831853f * fx.Pulse * tA + n * 4.0f);
					const float gl = fx.Intensity * fx.Patches * smooth01(0.3f, 0.85f, n) * regionW * pulse;
					if(gl <= 0.001f) continue;
					const float m = 0.5f + 0.5f * std::sin(tA * 0.7f + n * 6.0f);
					o.A[0] += (ch(fx.Color0, 16) + (ch(fx.Color1, 16) - ch(fx.Color0, 16)) * m) * gl;
					o.A[1] += (ch(fx.Color0, 8) + (ch(fx.Color1, 8) - ch(fx.Color0, 8)) * m) * gl;
					o.A[2] += (ch(fx.Color0, 0) + (ch(fx.Color1, 0) - ch(fx.Color0, 0)) * m) * gl;
				} else if(fx.Type == 5) { // nuages
					if(sky) {
						if(fx.SkyAmount <= 0 || skyRel > fx.SkyHeight) continue;
						// direction du regard de la colonne (angle sur la carte) : les nuages sont "a l'infini",
						// ils ne dependent que de cet angle -> stables quand la camera avance, tournent avec elle
						const int nc = std::clamp((int)std::lround(5000.0 / fx.NoiseScale), 4, 128); // cellules de bruit sur un tour
						const float su = (float)(colAng * (1.0 / 6.2831853) * nc) + tA * fx.WindX * 0.002f * nc;
						const float sv = (float)skyRows / (float)(fx.NoiseScale * 0.12f);
						const float n = fbm(su, sv, nc);
						float cov = smooth01(1.0f - fx.Coverage - 0.12f, 1.0f - fx.Coverage + 0.12f, n);
						cov *= std::clamp(skyRel * 6.0f, 0.0f, 1.0f) * std::clamp((fx.SkyHeight - skyRel) * 8.0f, 0.0f, 1.0f) * fx.SkyAmount;
						const float shade = 0.82f + 0.18f * n;
						if(cov > 0.001f) lerpTo(cov * 0.9f, ch(fx.Color0, 16) * shade, ch(fx.Color0, 8) * shade, ch(fx.Color0, 0) * shade);
					} else if(fx.Shadow > 0) {
						const float n = fbm((u + tA * fx.WindX) / fx.NoiseScale, (v + tA * fx.WindY) / fx.NoiseScale, 0);
						const float k = 1.0f - fx.Shadow * smooth01(1.0f - fx.Coverage - 0.12f, 1.0f - fx.Coverage + 0.12f, n) * regionW;
						for(int i = 0; i < 3; i++) { o.M[i] *= k; o.A[i] *= k; }
					}
				}
			}
		};
		// cap de chaque vue LISSE d'une frame a l'autre (la camera reconstruite tremble un peu) : ciel stable
		static double s_cloudHead[8]; static bool s_cloudHeadOk[8] = {};
		std::vector<double> viewHead(m7Views.size()), viewSide(m7Views.size());
		for(size_t v = 0; v < m7Views.size(); v++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
			const double h = std::atan2(zn.Fy, zn.Fx);
			viewSide[v] = (zn.Fx * zn.Ry - zn.Fy * zn.Rx) >= 0 ? 1.0 : -1.0;
			if(v >= 8) { viewHead[v] = h; continue; }
			if(!s_cloudHeadOk[v]) { s_cloudHead[v] = h; s_cloudHeadOk[v] = true; }
			else {
				const double d = std::remainder(h - s_cloudHead[v], 6.283185307179586);
				s_cloudHead[v] = std::abs(d) > 0.35 ? h : s_cloudHead[v] + d * 0.35; // virage brusque / changement de vue : suit tout de suite
			}
			viewHead[v] = s_cloudHead[v];
		}
		const int kS = 4; // pas de la grille (pixels HD)
		auto atmoBand = [&](uint32_t b0, uint32_t b1) {
			const int sc = (int)scale;
			const int nx = ((int)outWidth - 1) / kS + 2;
			std::vector<AtmoMA> rowA(nx), rowB(nx), rowC(nx), cacheMA(outWidth);
			std::vector<double> colAngle(nx);
			std::vector<float> cacheFeet(outWidth); // objets : meme ligne des pieds dans une colonne -> meme resultat
			for(size_t v = 0; v < m7Views.size(); v++) {
				const std::vector<AtmoFx>& list = atmoLists[v];
				if(list.empty()) continue;
				const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
				const int areaTop = (zn.AreaY0 + (int)dstY) * sc, zoneTop = (zn.Y0 + (int)dstY) * sc;
				const int areaBot = std::min((int)hdHeight - 1, (zn.AreaY1 + 1 + (int)dstY) * sc - 1);
				const int y0 = std::max((int)b0, areaTop), y1 = std::min((int)b1 - 1, areaBot);
				if(y0 > y1) continue;
				// angle du regard de chaque colonne de la grille (pour les nuages du ciel)
				for(int j = 0; j < nx; j++) {
					const int xx = std::min((int)outWidth - 1, j * kS);
					const double k = (((double)xx + 0.5) / sc - (double)dstX - zn.AxisX) / std::max(1.0, zn.Focal);
					colAngle[j] = viewHead[v] + viewSide[v] * std::atan(k);
				}
				std::fill(cacheFeet.begin(), cacheFeet.end(), -1e9f);
				auto skyRelOf = [&](int yy) { return (float)(zoneTop - yy) / (float)std::max(1, zoneTop - areaTop); };
				// une ligne de la grille : points tous les kS pixels ; au sol, point de carte interpole lineairement
				// le long de la ligne (exact : une ligne d'ecran = une droite sur la carte)
				auto evalRow = [&](int yy, std::vector<AtmoMA>& dst) {
					const bool sky = yy < zoneTop;
					const float sr = sky ? skyRelOf(yy) : 0.0f;
					const double sy = ((double)yy + 0.5) / sc - (double)dstY;
					double ua = 0, va = 0, ub = 0, vb = 0; bool okA = false, okB = false;
					const double xa = 0.5 / sc - (double)dstX, xb = ((double)outWidth - 0.5) / sc - (double)dstX;
					if(!sky) {
						const double g = std::clamp(sy, (double)zn.Y0, (double)zn.Y1);
						okA = zn.Unproject(xa, g, ua, va); okB = zn.Unproject(xb, g, ub, vb);
					}
					for(int j = 0; j < nx; j++) {
						const int xx = std::min((int)outWidth - 1, j * kS);
						const double sx = ((double)xx + 0.5) / sc - (double)dstX;
						float u = 0, w = 0, D = 0;
						if(!sky) {
							if(okA && okB) {
								const double f = (sx - xa) / (xb - xa);
								u = (float)(ua + (ub - ua) * f); w = (float)(va + (vb - va) * f);
							} else {
								double uu, ww;
								if(!zn.Unproject(sx, std::clamp(sy, (double)zn.Y0, (double)zn.Y1), uu, ww)) { dst[j] = { { 1, 1, 1 }, { 0, 0, 0 } }; continue; }
								u = (float)uu; w = (float)ww;
							}
							D = (float)zn.DepthOf(u, w);
						}
						evalPoint(list, zn, sky, sr, sky ? colAngle[j] : 0.0, (double)(zoneTop - yy) / sc, u, w, D, dst[j]);
					}
				};
				int curK0 = -1000000, curK1 = -1000000;
				std::vector<AtmoMA>* pK0 = &rowA; std::vector<AtmoMA>* pK1 = &rowB;
				for(int yy = y0; yy <= y1; yy++) {
					const bool sky = yy < zoneTop;
					// lignes de grille encadrantes (meme cote de l'horizon que la ligne courante, sinon calcul direct)
					const int k0 = (yy - areaTop) / kS;
					int g0 = areaTop + k0 * kS, g1 = std::min(areaBot, g0 + kS);
					const std::vector<AtmoMA>* A0; const std::vector<AtmoMA>* A1; float fy;
					if(((g0 < zoneTop) != sky) || ((g1 < zoneTop) != sky) || g1 == g0) {
						evalRow(yy, rowC); A0 = A1 = &rowC; fy = 0;
					} else {
						if(curK0 != g0) {
							if(curK1 == g0) { std::swap(pK0, pK1); curK0 = g0; curK1 = -1000000; }
							else { evalRow(g0, *pK0); curK0 = g0; }
						}
						if(curK1 != g1) { evalRow(g1, *pK1); curK1 = g1; }
						A0 = pK0; A1 = pK1; fy = (float)(yy - g0) / (float)(g1 - g0);
					}
					uint32_t* row = out + (size_t)yy * outWidth;
					const uint8_t* noL = maskOn ? &s_noLight[(size_t)yy * outWidth] : nullptr;
					const float* sprRowY = p3Occlusion ? &s_sprRow[(size_t)yy * outWidth] : nullptr; // objets au sol, meme au-dessus de l'horizon
					for(int xx = 0; xx < (int)outWidth; xx++) {
						if(noL && noL[xx]) continue;
						AtmoMA ma;
						if(sprRowY && sprRowY[xx] >= 0.0f && sprRowY[xx] < kFront) {
							// objet pose au sol : point de la carte sous ses pieds (calcul exact, garde par colonne)
							if(cacheFeet[xx] != sprRowY[xx]) {
								const double sx = ((double)xx + 0.5) / sc - (double)dstX;
								const double g = std::clamp((double)sprRowY[xx] - 0.5 / sc, (double)zn.Y0, (double)zn.Y1);
								double uu, ww;
								if(!zn.Unproject(sx, g, uu, ww)) { cacheMA[xx] = { { 1, 1, 1 }, { 0, 0, 0 } }; }
								else evalPoint(list, zn, false, 0.0f, 0.0, 0.0, (float)uu, (float)ww, (float)zn.DepthOf(uu, ww), cacheMA[xx]);
								cacheFeet[xx] = sprRowY[xx];
							}
							ma = cacheMA[xx];
						} else {
							const int j = xx / kS;
							const float fx = (float)(xx - j * kS) / (float)kS;
							const AtmoMA& a = (*A0)[j]; const AtmoMA& b = (*A0)[j + 1];
							const AtmoMA& c = (*A1)[j]; const AtmoMA& d = (*A1)[j + 1];
							for(int i = 0; i < 3; i++) {
								const float mt = a.M[i] + (b.M[i] - a.M[i]) * fx, mb = c.M[i] + (d.M[i] - c.M[i]) * fx;
								const float at = a.A[i] + (b.A[i] - a.A[i]) * fx, ab = c.A[i] + (d.A[i] - c.A[i]) * fx;
								ma.M[i] = mt + (mb - mt) * fy; ma.A[i] = at + (ab - at) * fy;
							}
						}
						const uint32_t c = row[xx];
						const float r = ((c >> 16) & 0xFF) * ma.M[0] + ma.A[0];
						const float g = ((c >> 8) & 0xFF) * ma.M[1] + ma.A[1];
						const float b = (c & 0xFF) * ma.M[2] + ma.A[2];
						row[xx] = (c & 0xFF000000) | ((uint32_t)std::clamp(r + 0.5f, 0.0f, 255.0f) << 16) | ((uint32_t)std::clamp(g + 0.5f, 0.0f, 255.0f) << 8) | (uint32_t)std::clamp(b + 0.5f, 0.0f, 255.0f);
					}
				}
			}
		};
		unsigned hc = std::thread::hardware_concurrency();
		int nThreads = std::clamp((int)hc - 2, 1, 8);
		if(nThreads <= 1 || hdHeight < 64) {
			atmoBand(0, hdHeight);
		} else {
			uint32_t bandH = (hdHeight + nThreads - 1) / nThreads;
			std::vector<std::thread> workers;
			for(int t = 0; t < nThreads; t++) {
				uint32_t b0 = (uint32_t)t * bandH, b1 = std::min(hdHeight, b0 + bandH);
				if(b0 >= b1) break;
				workers.emplace_back([&atmoBand, b0, b1]() { atmoBand(b0, b1); });
			}
			for(auto& w : workers) w.join();
		}
	}

	// ====================================================================
	// <m7Effect> flare : REFLETS DE LENTILLE (sequentiel, additif)
	// Visibilite = part de 5 x 5 echantillons autour de la source non caches (objet plus proche,
	// HUD...), lissee d'une frame a l'autre ; halo + trainee + reflets le long de la ligne source -> centre.
	// ====================================================================
	if(flareOn) {
		auto addGlow = [&](float cx, float cy, float rad, float cr, float cg, float cb, float inten, bool ring, int clipY0, int clipY1) {
			if(rad < 1.0f || inten <= 0.002f) return;
			const int x0 = std::max(0, (int)std::floor(cx - rad)), x1 = std::min((int)outWidth - 1, (int)std::ceil(cx + rad));
			const int y0 = std::max(clipY0, (int)std::floor(cy - rad)), y1 = std::min(clipY1, (int)std::ceil(cy + rad));
			for(int yy = y0; yy <= y1; yy++) {
				uint32_t* row = out + (size_t)yy * outWidth;
				const uint8_t* noL = maskOn ? &s_noLight[(size_t)yy * outWidth] : nullptr;
				for(int xx = x0; xx <= x1; xx++) {
					if(noL && noL[xx]) continue;
					const float dx = ((float)xx + 0.5f - cx) / rad, dy = ((float)yy + 0.5f - cy) / rad;
					const float d = std::sqrt(dx * dx + dy * dy);
					if(d >= 1.0f) continue;
					float k;
					if(ring) { const float e = (d - 0.8f) / 0.12f; k = std::exp(-e * e); }
					else { k = (1.0f - d); k *= k; }
					k *= inten * 255.0f;
					const uint32_t c = row[xx];
					const float r = std::min(255.0f, ((c >> 16) & 0xFF) + cr * k), g = std::min(255.0f, ((c >> 8) & 0xFF) + cg * k), b = std::min(255.0f, (c & 0xFF) + cb * k);
					row[xx] = (c & 0xFF000000) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
				}
			}
		};
		for(size_t v = 0; v < m7Views.size(); v++) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[v]];
			const int sc = (int)scale;
			const int areaTop = (zn.AreaY0 + (int)dstY) * sc, zoneTop = (zn.Y0 + (int)dstY) * sc;
			const int areaBot = std::min((int)hdHeight - 1, (zn.AreaY1 + 1 + (int)dstY) * sc - 1);
			const float ccx = (float)outWidth * 0.5f, ccy = (float)(areaTop + areaBot) * 0.5f;
			for(FlareFx& F : flareLists[v]) {
				SnesHdM7EffectInfo& fx = *F.Fx;
				float target = 0;
				if(F.Visible && F.Sx >= 0 && F.Sx < (float)outWidth && F.Sy >= (float)areaTop && F.Sy <= (float)areaBot) {
					int clear = 0, total = 0;
					for(int j = -2; j <= 2; j++) for(int i = -2; i <= 2; i++) {
						const int xx = (int)F.Sx + i * sc, yy = (int)F.Sy + j * sc;
						total++;
						if(xx < 0 || xx >= (int)outWidth || yy < areaTop || yy > areaBot) continue;
						const size_t idx = (size_t)yy * outWidth + xx;
						if(maskOn && s_noLight[idx]) continue;                       // HUD devant
						if(fx.Sun && yy >= zoneTop) continue;                         // soleil sous l'horizon
						if(p3Occlusion) {
							const float s2 = s_sprRow[idx];
							if(s2 >= 0.0f && (fx.Sun || s2 > F.GroundRow + 0.25f)) continue; // objet plus proche
						}
						clear++;
					}
					target = total ? (float)clear / (float)total : 0.0f;
				}
				float& vis = fx.FlareVis[F.Key];
				vis += (target - vis) * 0.35f;
				if(vis < 0.01f || !F.Visible) continue;
				const float cr = ((fx.Color0 >> 16) & 0xFF) / 255.0f, cg = ((fx.Color0 >> 8) & 0xFF) / 255.0f, cb = (fx.Color0 & 0xFF) / 255.0f;
				const float R = fx.FlareSize * (float)sc;
				const float inten = fx.Intensity * vis;
				addGlow(F.Sx, F.Sy, R, cr, cg, cb, inten, false, areaTop, areaBot);
				addGlow(F.Sx, F.Sy, R * 0.35f, 1.0f, 1.0f, 1.0f, inten * 0.8f, false, areaTop, areaBot);
				if(fx.Streak > 0) {
					// trainee horizontale fine
					const float half = R * 3.0f, th = std::max(1.0f, R * 0.05f);
					const int x0 = std::max(0, (int)(F.Sx - half)), x1 = std::min((int)outWidth - 1, (int)(F.Sx + half));
					const int y0 = std::max(areaTop, (int)(F.Sy - th * 2)), y1 = std::min(areaBot, (int)(F.Sy + th * 2));
					for(int yy = y0; yy <= y1; yy++) {
						uint32_t* row = out + (size_t)yy * outWidth;
						const float fy = 1.0f - std::min(1.0f, std::abs((float)yy + 0.5f - F.Sy) / (th * 2));
						for(int xx = x0; xx <= x1; xx++) {
							if(maskOn && s_noLight[(size_t)yy * outWidth + xx]) continue;
							const float fxv = 1.0f - std::abs((float)xx + 0.5f - F.Sx) / half;
							const float k = fx.Streak * inten * fxv * fxv * fy * 160.0f;
							const uint32_t c = row[xx];
							const float r = std::min(255.0f, ((c >> 16) & 0xFF) + cr * k), g = std::min(255.0f, ((c >> 8) & 0xFF) + cg * k), b = std::min(255.0f, (c & 0xFF) + cb * k);
							row[xx] = (c & 0xFF000000) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
						}
					}
				}
				if(fx.Ghosts > 0) {
					static const float kG[5][6] = { // position sur la ligne, taille, r, g, b, anneau
						{ 0.55f, 0.20f, 1.0f, 0.6f, 0.3f, 0 }, { 0.90f, 0.10f, 0.4f, 1.0f, 0.5f, 0 }, { 1.25f, 0.32f, 0.5f, 0.6f, 1.0f, 1 },
						{ 1.60f, 0.14f, 1.0f, 0.4f, 0.8f, 0 }, { 2.10f, 0.45f, 0.4f, 0.9f, 1.0f, 1 } };
					for(const auto& gh : kG) {
						const float gx = F.Sx + (ccx - F.Sx) * gh[0], gy = F.Sy + (ccy - F.Sy) * gh[0];
						addGlow(gx, gy, R * gh[1] * 2.0f, gh[2] * cr, gh[3] * cg, gh[4] * cb, 0.18f * fx.Ghosts * inten, gh[5] > 0.5f, areaTop, areaBot);
					}
				}
			}
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
		// <SpriteLight> : cadre vert = tile reconnue qui porte une lumiere, orange = reconnue mais sans lumiere
		// (hors du sol avec distance=0, plane=0, hors vue...) ; anneau de la couleur de la lumiere au sol + trait jusqu'a sa hauteur
		for(const SlDebugBox& b : slDbgBoxes) {
			const uint32_t c = slLitKeys.count(b.Key) ? 0xFF20FF40 : 0xFFFF9020;
			const int x0 = b.X0 * (int)scale, y0 = b.Y0 * (int)scale, x1 = (b.X1 + 1) * (int)scale - 1, y1 = (b.Y1 + 1) * (int)scale - 1;
			fillRect(x0, y0, x1, y0 + thick - 1, c, 0, (int)hdHeight - 1); fillRect(x0, y1 - thick + 1, x1, y1, c, 0, (int)hdHeight - 1);
			fillRect(x0, y0, x0 + thick - 1, y1, c, 0, (int)hdHeight - 1); fillRect(x1 - thick + 1, y0, x1, y1, c, 0, (int)hdHeight - 1);
		}
		for(const SlDebugLight& L : slDbgLights) {
			const SnesHdM7Zone& zn = m7Zones[m7Views[L.View]];
			const int cy0 = (zn.AreaY0 + (int)dstY) * (int)scale, cy1 = std::min((int)hdHeight - 1, (zn.AreaY1 + 1 + (int)dstY) * (int)scale - 1);
			double sx, syG, syT, ppt, ppt2;
			if(!zn.Project(L.U, L.V, 0.0, sx, syG, ppt) || !zn.Project(L.U, L.V, L.H, sx, syT, ppt2)) continue;
			const int px = (int)std::lround((sx + dstX) * scale), pyG = (int)std::lround((syG + dstY) * scale), pyT = (int)std::lround((syT + dstY) * scale);
			const uint32_t c = 0xFF000000 | L.Color;
			fillRect(px - thick / 2, std::min(pyT, pyG), px + thick / 2, std::max(pyT, pyG), c, cy0, cy1);
			fillRect(px - 3 * thick, pyG - thick, px + 3 * thick, pyG + thick, c, cy0, cy1);
			fillRect(px - 2 * thick, pyT - 2 * thick, px + 2 * thick, pyT + 2 * thick, 0xFFFFFFFF, cy0, cy1);
		}
		uint32_t fn = SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber;
		if(fn % 120 == 0 && sprLightCand) {
			MessageManager::Log("[HD-SPRITELIGHT] " + std::to_string(slDbgBoxes.size()) + " tile(s) reconnue(s), " + std::to_string(slDbgLights.size()) + " lumiere(s)");
		}
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