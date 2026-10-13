#include "pch.h"
#include <vector>
#include <algorithm>
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
	if(!frameLog) {
		MessageManager::Log("[HD TRACE 2.5] GetFrameInfo a pris le relais pour l'affichage !");
		frameLog = true;
	}

	_sdFrameInfo = SnesDefaultVideoFilter::GetFrameInfo();
	FrameInfo info = _sdFrameInfo;

	// On agrandit la fenêtre en fonction de l'échelle du pack
	if(SnesHdPackLoader::ActivePack) {
		uint32_t scale = SnesHdPackLoader::ActivePack->Scale;
		info.Width *= scale;
		info.Height *= scale;

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

	uint32_t sdWidth = _sdFrameInfo.Width;
	uint32_t sdHeight = _sdFrameInfo.Height;
	OverscanDimensions overscan = GetOverscan();

	uint32_t xOffset = overscan.Left;
	uint32_t yOffset = overscan.Top * _baseFrameInfo.Width;

	SnesConsole* console = (SnesConsole*)_emu->GetConsole().get();
	SnesHdPixelData* hdBuffer = console->GetPpu()->GetHdScreenBuffer();
	SnesHdPixelData* hdSubBuffer = console->GetPpu()->GetHdSubScreenOutputBuffer();
	SnesHdPixelData* hdBehindBuffer = console->GetPpu()->GetHdBehindScreenBuffer();

	uint32_t outWidth = sdWidth * scale;
	uint32_t hdHeight = sdHeight * scale;

	if(_zBuffer.size() != outWidth * hdHeight) {
		_zBuffer.resize(outWidth * hdHeight);
	}
	memset(_zBuffer.data(), 0, _zBuffer.size()); // Remise à zéro du Z-Buffer
	memset(out, 0, outWidth * hdHeight * sizeof(uint32_t)); // CORRECTION TRAÎNÉE : Remise à zéro du tampon de couleur HD

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

	// Cache fixe sur la pile pour éviter les allocations dynamiques
	SdColorCache sdColorRow[256];

	for(uint32_t y = 0; y < sdHeight; y++) {
		bool hasMode7OnLine = false;
		SnesHdPixelData refM7Data;
		SnesHdPixelData nextRefM7Data;
		uint32_t refX = 0;

		// 1. Recherche du premier pixel Mode 7 sur la ligne actuelle (y)
		for(uint32_t tx = 0; tx < sdWidth; tx++) {
			uint32_t offset = y * _baseFrameInfo.Width + tx + yOffset + xOffset;
			if(hdBuffer[offset].IsMode7) {
				refM7Data = hdBuffer[offset];
				refX = tx;
				hasMode7OnLine = true;
				break;
			}
		}

		if(hasMode7OnLine) {
			// 2. Recherche du premier pixel Mode 7 sur la ligne suivante (y+1)
			bool hasNextLine = false;
			uint32_t nextRefX = 0;
			if(y + 1 < sdHeight) {
				for(uint32_t tx = 0; tx < sdWidth; tx++) {
					uint32_t offset = (y + 1) * _baseFrameInfo.Width + tx + yOffset + xOffset;
					if(hdBuffer[offset].IsMode7) {
						nextRefM7Data = hdBuffer[offset];
						nextRefX = tx;
						hasNextLine = true;
						break;
					}
				}
			}
			if(!hasNextLine) {
				nextRefM7Data = refM7Data;
				nextRefX = refX;
			}

			SnesHdMode7Image* m7Img = SnesHdPackLoader::ActivePack->GetMode7Image(refM7Data.M7MapHash);

			if(m7Img && m7Img->Bitmap && !m7Img->Bitmap->PixelData.empty()) {
				uint32_t outScale = scale;
				uint32_t imgScale = m7Img->Scale;
				uint32_t mapDim = 1024 * imgScale;

				// Génération des Mipmaps partagées au niveau du Bitmap (exécutée 1 fois par fichier PNG)
				if(m7Img->Bitmap->Mipmaps.empty()) {
					m7Img->Bitmap->Mipmaps.push_back(m7Img->Bitmap->PixelData);
					uint32_t currentDim = mapDim;
					for(int level = 1; level <= 3; level++) {
						uint32_t prevDim = currentDim;
						currentDim /= 2;
						std::vector<uint32_t> nextMip(currentDim * currentDim, 0);
						const std::vector<uint32_t>& prevMip = m7Img->Bitmap->Mipmaps[level - 1];

						for(uint32_t my = 0; my < currentDim; my++) {
							for(uint32_t mx = 0; mx < currentDim; mx++) {
								uint32_t p00 = prevMip[(my * 2) * prevDim + (mx * 2)];
								uint32_t p10 = prevMip[(my * 2) * prevDim + (mx * 2 + 1)];
								uint32_t p01 = prevMip[(my * 2 + 1) * prevDim + (mx * 2)];
								uint32_t p11 = prevMip[(my * 2 + 1) * prevDim + (mx * 2 + 1)];

								uint32_t a1 = p00 >> 24, a2 = p10 >> 24, a3 = p01 >> 24, a4 = p11 >> 24;
								uint32_t sumA = a1 + a2 + a3 + a4;

								if(sumA == 0) {
									nextMip[my * currentDim + mx] = 0x00000000;
								} else {
									uint32_t r = (((p00 >> 16) & 0xFF) * a1 + ((p10 >> 16) & 0xFF) * a2 + ((p01 >> 16) & 0xFF) * a3 + ((p11 >> 16) & 0xFF) * a4) / sumA;
									uint32_t g = (((p00 >> 8) & 0xFF) * a1 + ((p10 >> 8) & 0xFF) * a2 + ((p01 >> 8) & 0xFF) * a3 + ((p11 >> 8) & 0xFF) * a4) / sumA;
									uint32_t b = ((p00 & 0xFF) * a1 + (p10 & 0xFF) * a2 + (p01 & 0xFF) * a3 + (p11 & 0xFF) * a4) / sumA;
									nextMip[my * currentDim + mx] = ((sumA / 4) << 24) | (r << 16) | (g << 8) | b;
								}
							}
						}
						m7Img->Bitmap->Mipmaps.push_back(nextMip);
					}
				}

				bool applyColorMath = !m7Img->IgnoreColorMath;
				uint8_t pixelZ = 20 + (refM7Data.SnesPriorityLevel * 4);
				if(refM7Data.IsHighPriority) pixelZ += 2;

				// =================================================================
				// OPTIMISATION 1 : PRE-CACHE DU COLOR MATH (256 ITERATIONS/LINE)
				// =================================================================
				uint32_t baseSdColor = _calculatedPalette[refM7Data.BaseColor];
				int baseR = (baseSdColor >> 16) & 0xFF;
				int baseG = (baseSdColor >> 8) & 0xFF;
				int baseB = baseSdColor & 0xFF;

				for(uint32_t x = 0; x < sdWidth; x++) {
					uint32_t currentOffset = y * _baseFrameInfo.Width + x + yOffset + xOffset;
					uint16_t currentSnesPixel = ppuOutputBuffer[currentOffset];
					uint32_t sdColor = SnesHdPackLoader::ActivePack->IsColorMathDisabled
						? baseSdColor
						: _calculatedPalette[currentSnesPixel];

					sdColorRow[x].color = sdColor;
					if(applyColorMath) {
						sdColorRow[x].diffR = ((sdColor >> 16) & 0xFF) - baseR;
						sdColorRow[x].diffG = ((sdColor >> 8) & 0xFF) - baseG;
						sdColorRow[x].diffB = (sdColor & 0xFF) - baseB;
					} else {
						sdColorRow[x].diffR = 0;
						sdColorRow[x].diffG = 0;
						sdColorRow[x].diffB = 0;
					}
				}

				// Projection de l'origine à X = 0 (style bsnes)
				int32_t startX_y = refM7Data.M7XValue - (refM7Data.M7A * (int32_t)refX);
				int32_t startY_y = refM7Data.M7YValue - (refM7Data.M7C * (int32_t)refX);

				int32_t startX_next = nextRefM7Data.M7XValue - (nextRefM7Data.M7A * (int32_t)nextRefX);
				int32_t startY_next = nextRefM7Data.M7YValue - (nextRefM7Data.M7C * (int32_t)nextRefX);

				// =================================================================
				// BOUCLE SUR LES SOUS-LIGNES HD (sy)
				// =================================================================
				for(uint32_t sy = 0; sy < outScale; sy++) {
					double t = (double)sy / (double)outScale;

					// OPTIMISATION 2 : INTERPOLATION RÉCIPROQUE PERSPECTIVE (BSNES-HD)
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

					// Calcul du LOD basé sur le vecteur Mipmaps du Bitmap partagé
					int64_t distSq = (((int64_t)stepX * stepX) + ((int64_t)stepY * stepY)) >> 16;
					int lod = 0;
					if(distSq > 16) lod = 3;
					else if(distSq > 4) lod = 2;
					else if(distSq > 1) lod = 1;
					if(lod >= (int)m7Img->Bitmap->Mipmaps.size()) lod = (int)m7Img->Bitmap->Mipmaps.size() - 1;

					uint32_t curDim = mapDim >> lod;
					uint32_t curMask = curDim - 1;
					const uint32_t* curPixels = m7Img->Bitmap->Mipmaps[lod].data();

					uint32_t hdRowIdx = (y * outScale + sy) * outWidth;

					// OPTIMISATION 3 : INCRÉMENTATION LINÉAIRE PAR DÉCALAGE
					int32_t exactX = lineStartX * (int32_t)imgScale;
					int32_t exactY = lineStartY * (int32_t)imgScale;

					// Lambda d'échantillonnage bilinéaire
					auto sampleBilinearFast = [&](int32_t sX, int32_t sY) -> uint32_t {
						int32_t lX = sX >> lod;
						int32_t lY = sY >> lod;
						int32_t px = lX >> 8;
						int32_t py = lY >> 8;

						if(refM7Data.M7LargeMap && (px < 0 || px >= (int32_t)curDim || py < 0 || py >= (int32_t)curDim)) {
							return 0;
						}

						uint32_t fx = lX & 0xFF;
						uint32_t fy = lY & 0xFF;
						uint32_t x0 = px & curMask;
						uint32_t x1 = (px + 1) & curMask;
						uint32_t y0 = py & curMask;
						uint32_t y1 = (py + 1) & curMask;

						uint32_t p00 = curPixels[y0 * curDim + x0];
						uint32_t p10 = curPixels[y0 * curDim + x1];
						uint32_t p01 = curPixels[y1 * curDim + x0];
						uint32_t p11 = curPixels[y1 * curDim + x1];

						uint32_t w00 = (256 - fx) * (256 - fy);
						uint32_t w10 = fx * (256 - fy);
						uint32_t w01 = (256 - fx) * fy;
						uint32_t w11 = fx * fy;

						uint32_t a = (((p00 >> 24) & 0xFF) * w00 + ((p10 >> 24) & 0xFF) * w10 + ((p01 >> 24) & 0xFF) * w01 + ((p11 >> 24) & 0xFF) * w11) >> 16;
						if(a == 0) return 0;

						uint32_t r = (((p00 >> 16) & 0xFF) * w00 + ((p10 >> 16) & 0xFF) * w10 + ((p01 >> 16) & 0xFF) * w01 + ((p11 >> 16) & 0xFF) * w11) >> 16;
						uint32_t g = (((p00 >> 8) & 0xFF) * w00 + ((p10 >> 8) & 0xFF) * w10 + ((p01 >> 8) & 0xFF) * w01 + ((p11 >> 8) & 0xFF) * w11) >> 16;
						uint32_t b = ((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10 + (p01 & 0xFF) * w01 + (p11 & 0xFF) * w11) >> 16;

						return (a << 24) | (r << 16) | (g << 8) | b;
					};

					int32_t halfStepX = stepX >> 1;
					int32_t halfStepY = stepY >> 1;

					// =============================================================
					// BOUCLE INTERNE CRITIQUE : PARCOURS DES PIXELS HD DE LA LIGNE
					// =============================================================
					for(uint32_t x = 0; x < sdWidth; x++) {
						const SdColorCache& cache = sdColorRow[x];
						uint32_t outIdx = hdRowIdx + (x * outScale);

						for(uint32_t sx = 0; sx < outScale; sx++) {
							uint32_t hdColorFromPng = 0;

							if(m7Img->FilterMode == Mode7FilterMode::Anisotropic) {
								uint32_t c1 = sampleBilinearFast(exactX - halfStepX, exactY - halfStepY);
								uint32_t c2 = sampleBilinearFast(exactX + halfStepX, exactY + halfStepY);

								uint32_t a1 = (c1 >> 24) & 0xFF, a2 = (c2 >> 24) & 0xFF;
								uint32_t sumA = a1 + a2;
								if(sumA > 0) {
									uint32_t avgR = (((c1 >> 16) & 0xFF) * a1 + ((c2 >> 16) & 0xFF) * a2) / sumA;
									uint32_t avgG = (((c1 >> 8) & 0xFF) * a1 + ((c2 >> 8) & 0xFF) * a2) / sumA;
									uint32_t avgB = ((c1 & 0xFF) * a1 + (c2 & 0xFF) * a2) / sumA;
									hdColorFromPng = ((sumA >> 1) << 24) | (avgR << 16) | (avgG << 8) | avgB;
								}
							} else if(m7Img->FilterMode == Mode7FilterMode::Bilinear) {
								hdColorFromPng = sampleBilinearFast(exactX, exactY);
							} else {
								// Mode Nearest
								int32_t lodX = exactX >> lod;
								int32_t lodY = exactY >> lod;
								int32_t px = lodX >> 8;
								int32_t py = lodY >> 8;

								if(!(refM7Data.M7LargeMap && (px < 0 || px >= (int32_t)curDim || py < 0 || py >= (int32_t)curDim))) {
									hdColorFromPng = curPixels[(py & curMask) * curDim + (px & curMask)];
								}
							}

							uint32_t finalColor = cache.color;
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

							exactX += stepX;
							exactY += stepY;
						}
					}
				}
			}





	//
		}
	
	

		// --- BOUCLE X POUR LES SPRITES ET TUILES DE BASE ---
		for(uint32_t x = 0; x < sdWidth; x++) {

			uint32_t bufferOffset = y * _baseFrameInfo.Width + x + yOffset + xOffset;
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
					if(!rule.MatchesCondition(x, y, &hdData)) continue;
					pixelZ = rule.ZIndexBg[hdData.SourceLayer];
					if(hdData.IsHighPriority) pixelZ += 2;
					break;
				}
			}

			// --- STEP 1.5: INDIVIDUAL MODE 7 TILES (<m7tiles>) WITH ANISOTROPIC FILTERING ---
			if(hdData.IsMode7Tile) {
				SnesHdMode7Image* m7Img = SnesHdPackLoader::ActivePack->GetMode7Image(hdData.M7TileHash);

				if(m7Img && m7Img->Bitmap && !m7Img->Bitmap->PixelData.empty()) {
					const uint32_t* rawPixelData = m7Img->Bitmap->PixelData.data();

					uint32_t imgScale = m7Img->Scale;
					int32_t tileDim = 8 * imgScale;

					uint32_t pngWidth = m7Img->Bitmap->Width;
					uint32_t pngHeight = m7Img->Bitmap->Height;
					int32_t srcX = m7Img->X;
					int32_t srcY = m7Img->Y;

					uint32_t mapDim = 1024 * imgScale;
					uint32_t bitMask = mapDim - 1;

					uint32_t baseSdColor = _calculatedPalette[hdData.BaseColor];
					int diffR = ((sdColor >> 16) & 0xFF) - ((baseSdColor >> 16) & 0xFF);
					int diffG = ((sdColor >> 8) & 0xFF) - ((baseSdColor >> 8) & 0xFF);
					int diffB = (sdColor & 0xFF) - (baseSdColor & 0xFF);

					int32_t baseHD_X = hdData.M7XValue * imgScale;
					int32_t baseHD_Y = hdData.M7YValue * imgScale;

					int32_t snesX = (hdData.M7XValue >> 8) & 1023;
					int32_t snesY = (hdData.M7YValue >> 8) & 1023;
					int32_t tileMinX = (snesX & ~7) * imgScale;
					int32_t tileMinY = (snesY & ~7) * imgScale;

					int32_t stepX = (hdData.M7A * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepY = (hdData.M7C * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepYTermX = (hdData.M7B * (int32_t)imgScale) / (int32_t)scale;
					int32_t stepYTermY = (hdData.M7D * (int32_t)imgScale) / (int32_t)scale;

					// lenX & lenY: Vector lengths along screen X and Y axes (in texels)
					double lenX = std::sqrt((double)stepX * stepX + (double)stepY * stepY) / 256.0;
					double lenY = std::sqrt((double)stepYTermX * stepYTermX + (double)stepYTermY * stepYTermY) / 256.0;

					// maxLen & minLen: Major and minor axes of the tile footprint
					double maxLen = std::max(lenX, lenY);
					double minLen = std::max(std::min(lenX, lenY), 0.001);

					// tileSamples: Dynamic sample count along major axis for anisotropic filtering
					int tileSamples = std::clamp((int)std::ceil(maxLen / minLen), 1, 16);

					// dirU & dirV: Normalized direction unit vector along major distortion axis
					double dirU = (lenX >= lenY && lenX > 0.0001) ? (stepX / (lenX * 256.0)) : (lenY > 0.0001 ? (stepYTermX / (lenY * 256.0)) : 1.0);
					double dirV = (lenX >= lenY && lenX > 0.0001) ? (stepY / (lenX * 256.0)) : (lenY > 0.0001 ? (stepYTermY / (lenY * 256.0)) : 0.0);

					// Helper lambda to fetch a single pixel from tile atlas with clamping inside tile bounds
					auto getTileTexel = [&](int32_t sampleX, int32_t sampleY) -> uint32_t {
						int32_t px = sampleX >> 8;
						int32_t py = sampleY >> 8;
						int32_t localX = (px - tileMinX) & bitMask;
						int32_t localY = (py - tileMinY) & bitMask;

						if(localX > (int32_t)(bitMask >> 1)) localX = 0;
						else if(localX >= tileDim) localX = tileDim - 1;

						if(localY > (int32_t)(bitMask >> 1)) localY = 0;
						else if(localY >= tileDim) localY = tileDim - 1;

						uint32_t finalReadX = srcX + localX;
						uint32_t finalReadY = srcY + localY;

						if(finalReadX < pngWidth && finalReadY < pngHeight) {
							return rawPixelData[finalReadY * pngWidth + finalReadX];
						}
						return 0;
					};

					// Helper lambda for bilinear sampling inside the tile bounds
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
						uint32_t outBaseIdx = (y * scale + sy) * outWidth + (x * scale);

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
								_zBuffer[outBaseIdx + sx] = pixelZ;
							}
						}
					}
				}
				continue;
			}

			// CAPTURE DES TUILES DE BASE (Sprites et Backgrounds)
			if(hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty()) {
				int visualX = hdData.IsSprite
					? (hdData.HFlip ? (7 - hdData.TileX) : hdData.TileX)
					: (hdData.HFlip ? hdData.TileX : (7 - hdData.TileX));

				int visualY = hdData.VFlip ? (7 - hdData.TileY) : hdData.TileY;

				int startX = (int)x - visualX;
				int startY = (int)y - visualY;

				bool alreadyCaptured = false;
				for(int i = (int)tilesToExpand.size() - 1; i >= 0; i--) {
					if(tilesToExpand[i].Tile == hdData.Tile && tilesToExpand[i].startX == startX && tilesToExpand[i].startY == startY) {
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

					int startX = (int)x - visualX;
					int startY = (int)y - visualY;

					bool alreadyCaptured = false;
					for(int i = (int)tilesToExpand.size() - 1; i >= 0; i--) {
						if(tilesToExpand[i].Tile == hdBehindData.Tile && tilesToExpand[i].startX == startX && tilesToExpand[i].startY == startY) {
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

			bool hasHdReplacement = (hdData.Tile != nullptr && !hdData.Tile->HdTileData.empty());
			bool isTileDisabled = (hdData.Tile != nullptr && hdData.Tile->IsDisabled);
			bool hideNative = hasHdReplacement || isTileDisabled;

			if(hdData.IsSprite && SnesHdPackLoader::ActivePack->DisableOriginalSprites) {
				hideNative = true;
			} else if(!hdData.IsSprite && SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
				hideNative = true;
			}

			// Si la tuile d'origine doit être masquée (remplacée par du HD ou désactivée) :
			// Elle a déjà été capturée dans tilesToExpand et sera dessinée à l'Étape 3
			// (avec tri Z-Index et Alpha Blending au-dessus des <background> et du Mode 7).
			// On ne doit donc pas appeler ApplyFilterSpritesStandard ici afin de laisser
			// le Z-Buffer à zéro pour que les <background> puissent se dessiner en dessous.
			if(hideNative) {
				continue;
			}

			ApplyFilterSpritesStandard(x, y, scale, outWidth, sdColor, hdData, pixelZ, out);
		}
	}

	// ====================================================================
	// ÉTAPE 2 : INJECTION DES ADDITIONS HD
	// ====================================================================
	if(SnesHdPackLoader::ActivePack) {
		SnesConsole* console = (SnesConsole*)_emu->GetConsole().get();
		bool overscanFrame = console->GetPpu()->GetState().OverscanMode;

		for(const auto& add : SnesHdPackLoader::ActivePack->ActiveAdditionsQueue) {
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

				int finalScreenX = add.ScreenX - overscan.Left;
				int finalScreenY = (overscanFrame ? (add.ScreenY - 1) : (add.ScreenY + 7)) - overscan.Top;

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
	}

	// ====================================================================
	// ÉTAPE 3 : PASSE DE RENDU UNIFIÉE ET TRIÉE PAR Z-INDEX ASCENDANT
	// (Combine tous les <background> et toutes les tilesToExpand)
	// ====================================================================
	struct RenderTask
	{
		int zIndex;
		int type;     // 0 = Background, 1 = Tile/Sprite
		int oamIndex; // Utilisé pour ordonner les sprites de même ZIndex
		size_t index; // Index dans le vecteur source
	};

	std::vector<RenderTask> tasks;

	// 1. Ingestion de tous les Backgrounds HD
	for(size_t i = 0; i < SnesHdPackLoader::ActivePack->Backgrounds.size(); i++) {
		auto& bg = SnesHdPackLoader::ActivePack->Backgrounds[i];
		if(!bg->MatchesCondition()) continue;
		tasks.push_back({ bg->ZIndex, 0, 0, i });
	}

	// 2. Ingestion de tous les Sprites/Tiles HD (tilesToExpand)
	for(size_t i = 0; i < tilesToExpand.size(); i++) {
		tasks.push_back({ (int)tilesToExpand[i].zIndex, 1, (int)tilesToExpand[i].oamIndex, i });
	}

	// 3. Tri Unifié par Z-Index Croissant
	std::stable_sort(tasks.begin(), tasks.end(), [](const RenderTask& a, const RenderTask& b) {
		if(a.zIndex != b.zIndex) {
			return a.zIndex < b.zIndex; // ZIndex le plus bas (arrière-plan) dessiné en premier
		}
		if(a.type != b.type) {
			return a.type < b.type; // À ZIndex égal, les backgrounds (0) passent sous les sprites (1)
		}
		// Pour les sprites de même ZIndex, tri OAM DÉCROISSANT (OAM 127 derrière, OAM 0 devant)
		return a.oamIndex > b.oamIndex;
	});

	// 4. Exécution Séquentielle des Tâches
	uint32_t hdWidth = outWidth;

	for(const auto& task : tasks) {

		// --------------------------------------------------------------------
		// BRANCHE A : RENDU D'UN BACKGROUND HD (<background>)
		// --------------------------------------------------------------------
		if(task.type == 0) {
			auto& bg = SnesHdPackLoader::ActivePack->Backgrounds[task.index];

			// Animation
			if(bg->IsAnimated && !bg->AnimFrames.empty() && bg->FramesBetweenSteps > 0) {
				uint32_t currentFrame = SnesHdPackLoader::ActivePack->ScreenInfo.FrameNumber;
				int totalFrames = (int)bg->AnimFrames.size();
				int currentAnimIndex = (currentFrame / bg->FramesBetweenSteps) % totalFrames;
				bg->Bitmap = bg->AnimFrames[currentAnimIndex];
			}
			if(bg->Bitmap == nullptr) continue;

			bg->Bitmap->Init();
			if(bg->Bitmap->PixelData.empty() || bg->Bitmap->Width == 0 || bg->Bitmap->Height == 0) continue;

			int bgWidth = bg->Bitmap->Width;
			int bgHeight = bg->Bitmap->Height;
			int bgIndex = bg->MasterScrollLayer - 1;

			for(uint32_t hdY = 0; hdY < hdHeight; hdY++) {
				uint32_t sdY = hdY / scale;
				int masterScrollX = 0, masterScrollY = 0;

				if(bgIndex >= 0 && bgIndex < 4 && sdY < 240) {
					auto& screenInfo = SnesHdPackLoader::ActivePack->ScreenInfo;
					masterScrollX = screenInfo.ScanlineHScroll[sdY][bgIndex];
					masterScrollY = screenInfo.ScanlineVScroll[sdY][bgIndex];
				}

				// --- CALCUL DU SCROLL SELON L'OPÉRATEUR (*, /, +, -) ---
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

				// --- FONCTION DE FUSION AVEC GESTION DU ALPHA FACTOR ---
				auto blendPixel = [&](int localPngX, uint32_t localHdX) {
					uint32_t pixelColor = bg->Bitmap->PixelData[pngBaseRowIndex + localPngX];
					uint32_t alpha = (pixelColor >> 24);

					// Application du facteur d'opacité dynamique
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
		}
		// --------------------------------------------------------------------
		// BRANCHE B : RENDU D'UN SPRITE / TUILE HD (tilesToExpand)
		// --------------------------------------------------------------------
		else {
			const auto& item = tilesToExpand[task.index];
			uint32_t pngWidth = item.Tile->Width * scale;
			uint32_t pngHeight = item.Tile->Height * scale;

			bool doHFlip = item.hFlip && !item.Tile->IsHFlip;
			bool doVFlip = item.vFlip && !item.Tile->IsVFlip;

			for(uint32_t py = 0; py < pngHeight; py++) {
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
								// Pixel totalement opaque : écriture directe + verrouillage du z-buffer
								out[pixelIndex] = finalColor | 0xFF000000;
								_zBuffer[pixelIndex] = item.zIndex;
							} else {
								// Pixel semi-transparent : alpha blending avec ce qui est déjà dessiné.
								// IMPORTANT : on ne met PAS à jour le z-buffer pour laisser les sprites
								// en second plan visibles là où ce sprite est semi-transparent.
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
								// Ne pas mettre à jour _zBuffer : les sprites derrière restent accessibles
							}
						}
					}
				}
			}
		}
	}

	// ====================================================================
	// ÉTAPE 4 : GESTION DES VIDÉOS (pl_mpeg) & VERROUILLAGE FINAL
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

				if(plm_has_ended(video->VideoPlayer) && !video->Loop) {
					video->IsPlaying = false;
					video->HasPlayed = true;
					plm_destroy(video->VideoPlayer);
					video->VideoPlayer = nullptr;
					video->CurrentFramePixels.clear();
					continue;
				}

				if(!video->CurrentFramePixels.empty()) {
					uint32_t* videoPixels = reinterpret_cast<uint32_t*>(video->CurrentFramePixels.data());

					if(hdWidth > 0 && hdHeight > 0) {
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

										if((pixel & 0x00FFFFFF) != 0) {
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

	// VERROUILLAGE FINAL DE L'ALPHA
	if(out != nullptr && hdWidth > 0 && hdHeight > 0) {
		for(uint32_t y = 0; y < hdHeight; y++) {
			for(uint32_t x = 0; x < hdWidth; x++) {
				out[y * outWidth + x] |= 0xFF000000;
			}
		}
	}

} // end fonction ApplyFilter
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
					// 1. Un décor HD (Mode 7 ou <background>) a déjà été dessiné : on le conserve !
					finalColor = currentOutColor;
				} else if(!hdData.IsSprite || SnesHdPackLoader::ActivePack->DisableOriginalSprites || SnesHdPackLoader::ActivePack->DisableOriginalTiles) {
					// 2. Pas de décor HD et décors originaux/sprites désactivés : transparent (0x00000000)
					finalColor = 0x00000000;
				} else {
					// 3. Mode standard avec décors originaux activés : restauration de la couleur d'arrière-plan
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

bool SnesHdPackSpriteNearbyCondition::InternalCheckCondition(int x, int y, SnesHdPixelData* tile)
{
	if(!_screenInfo || !_screenInfo->Ppu) return false;

	uint8_t* oamRam = _screenInfo->Ppu->GetSpriteRam();
	uint16_t* vram = (uint16_t*)_screenInfo->Ppu->GetVideoRam();
	if(!oamRam || !vram) return false;

	static constexpr uint8_t oamWidth[16]  = { 8, 8, 8, 16, 16, 32, 16, 16, 16, 32, 64, 32, 64, 64, 32, 32 };
	static constexpr uint8_t oamHeight[16] = { 8, 8, 8, 16, 16, 32, 32, 32, 16, 32, 64, 32, 64, 64, 64, 32 };
	static constexpr uint16_t sign[2] = { 0x0000, 0xFF00 };

	int targetX = CheckAnyX ? -1 : (x + OffsetX);
	int targetY = CheckAnyY ? -1 : (y + OffsetY);

	SnesPpuState& state = _screenInfo->Ppu->GetStateRef();

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

			if(this->MemoryHash <= 0xFFFF) {
				isTileMatch = (this->MemoryHash == subTileIndex);
			} else {
				// 4. Calcul du Hash 64-bits de la sous-tuile 8x8 (32 octets en 4 BPP)
				uint32_t bpp = 4;
				uint32_t tileAddress = state.OamBaseAddress;
				if(subTileIndex >= 256) {
					tileAddress += state.OamAddressOffset;
				}

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


			if(isTileMatch && (this->IgnorePalette || this->PaletteHash == snesPalette )) {

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