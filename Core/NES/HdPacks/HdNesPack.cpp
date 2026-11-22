#include "pch.h"
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include "NES/HdPacks/HdNesPack.h"
#include "NES/HdPacks/HdPackLoader.h"
#include "NES/HdPacks/HdGltfSkin.h"
#include "NES/NesConsole.h"
#include "NES/BaseMapper.h"
#include "NES/NesDefaultVideoFilter.h"
#include "Shared/MessageManager.h"
#include "Shared/HdGpuCore.h"
static_assert(HdDioMaxGroups == 8, "HdNesPack::DioGroups doit valoir HdDioMaxGroups");
#include "Shared/HdCamera.h"
#include "Shared/EmuSettings.h"
#include "Shared/KeyManager.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"

#include "NES/NesSoundMixer.h" //added by Gwenolo

template<uint32_t scale>
HdNesPack<scale>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData)
{
	_console = console;
	_settings = settings;
	_hdData = hdData;

	// Threads permanents du rendu HD : on laisse 2 coeurs a l'emulation et au reste du systeme.
	_pool.Start(std::clamp((int)std::thread::hardware_concurrency() - 2, 1, 8));
	_resolvedBg.resize(NesConstants::ScreenPixelCount, nullptr);
	_resolvedSprites.resize(NesConstants::ScreenPixelCount * 4, nullptr);
	_zNes.assign(NesConstants::ScreenPixelCount, 0);
	_ignNes.assign(NesConstants::ScreenPixelCount, 0);
	_sprNes.assign(NesConstants::ScreenPixelCount, 0);
	_reflFloor.assign(NesConstants::ScreenPixelCount, std::nanf(""));

	InitializeFallbackTiles();
	CleanupInvalidRules();

	/* ---------added by Gwenolo ---------*/
	if(hdData->_disable_8bits_Sound) {
		disable_8bitsSound();
	}
	/*--------------------------*/
}



/* ---------added by Gwenolo ---------*/
template<uint32_t scale>
void HdNesPack<scale>::disable_8bitsSound()
{   
	 
	for(int i = (int)AudioChannel::Square1; i <= (int)AudioChannel::DMC; i++) {
		_console->GetNesConfig().ChannelVolumes[i] = 0;
	}
	_console->GetSoundMixer()->UpdateRates(true);
		
}
/*-----------------------------------------*/

template<uint32_t scale>
HdNesPack<scale>::~HdNesPack()
{
}

template<uint32_t scale>
void HdNesPack<scale>::CleanupInvalidRules()
{
	//Cleanup invalid <addition> tags
	bool isChrRam = _console->GetMapper()->HasChrRam();
	if(_hdData->AdditionalSprites.size()) {
		vector<HdPackAdditionalSpriteInfo>& elems = _hdData->AdditionalSprites;
		elems.erase(std::remove_if(elems.begin(), elems.end(), [isChrRam](const HdPackAdditionalSpriteInfo& o) { return o.AdditionalTile.IsChrRamTile != isChrRam; }), elems.end());
	}
}

template<uint32_t scale>
void HdNesPack<scale>::InitializeFallbackTiles()
{
	BaseMapper* mapper = _console->GetMapper();
	uint8_t blankTile[16] = {};
	if(mapper->HasChrRom()) {
		if(_hdData->OptionFlags & (int)HdPackOptions::AutomaticFallbackTiles) {
			int32_t chrRomSize = (int32_t)mapper->GetChrRomSize();
			int32_t tileCount = chrRomSize / 16;

			HdTileKey tileKey = {};
			tileKey.IsChrRamTile = true;
			unordered_set<HdTileKey> usedTiles;
			for(auto& tile : _hdData->Tiles) {
				tileKey.TileIndex = tile->TileIndex;
				mapper->CopyChrTile(tile->TileIndex * 16, tileKey.TileData);

				usedTiles.emplace(tileKey);
			}

			for(int32_t i = 0; i < tileCount; i++) {
				mapper->CopyChrTile(i * 16, tileKey.TileData);
				if(memcmp(tileKey.TileData, blankTile, sizeof(blankTile)) == 0) {
					//Skip processing for blank/transparent tiles
					continue;
				}

				auto result = usedTiles.find(tileKey);
				if(result != usedTiles.end() && result->TileIndex != i) {
					_fallbackTiles[i] = result->TileIndex;
				}
			}
		}

		for(auto def : _hdData->FallbackTiles) {
			_fallbackTiles[def.TileIndex] = def.FallbackTileIndex;
		}
	}
}

template<uint32_t scale>
template<HdPackBlendMode blendMode>
void HdNesPack<scale>::BlendColors(uint8_t output[4], uint8_t input[4])
{
	if constexpr(blendMode == HdPackBlendMode::Alpha) {
		uint8_t invertedAlpha = 256 - input[3];
		// Destinataire transparent ou translucide (couche diorama) : l'alpha doit etre cumule, sinon le pixel devient
		// opaque avec une couleur premultipliee = halo noir. Destinataire opaque : comportement inchange.
		const uint8_t outA = output[3] == 0xFF ? 0xFF : (uint8_t)std::min(255, input[3] + ((invertedAlpha * output[3]) >> 8));
		output[0] = input[0] + (uint8_t)((invertedAlpha * output[0]) >> 8);
		output[1] = input[1] + (uint8_t)((invertedAlpha * output[1]) >> 8);
		output[2] = input[2] + (uint8_t)((invertedAlpha * output[2]) >> 8);
		output[3] = outA;
	} else if constexpr(blendMode == HdPackBlendMode::Add) {
		output[0] = (uint8_t)std::min(255, (int)input[0] + (int)output[0]);
		output[1] = (uint8_t)std::min(255, (int)input[1] + (int)output[1]);
		output[2] = (uint8_t)std::min(255, (int)input[2] + (int)output[2]);
		output[3] = 0xFF;
	} else if constexpr(blendMode == HdPackBlendMode::Subtract) {
		output[0] = (uint8_t)std::max(0, (int)output[0] - (int)input[0]);
		output[1] = (uint8_t)std::max(0, (int)output[1] - (int)input[1]);
		output[2] = (uint8_t)std::max(0, (int)output[2] - (int)input[2]);
		output[3] = 0xFF;
	}
}

template<uint32_t scale>
uint32_t HdNesPack<scale>::AdjustBrightness(uint8_t input[4], int brightness)
{
	return (
		std::min(255, (brightness * ((int)input[0] + 1)) >> 8) |
		(std::min(255, (brightness * ((int)input[1] + 1)) >> 8) << 8) |
		(std::min(255, (brightness * ((int)input[2] + 1)) >> 8) << 16) |
		(input[3] << 24)
	);
}

template<uint32_t scale>
void HdNesPack<scale>::DrawColor(uint32_t color, uint32_t *outputBuffer, uint32_t screenWidth)
{
	if constexpr(scale == 1) {
		*outputBuffer = color;
	} else if constexpr(scale == 2) {
		outputBuffer[0] = color;
		outputBuffer[1] = color;
		outputBuffer[screenWidth] = color;
		outputBuffer[screenWidth+1] = color;
	} else {
		for(uint32_t i = 0; i < scale; i++) {
			std::fill(outputBuffer, outputBuffer + scale, color);
			outputBuffer += screenWidth;
		}
	}
}

template<uint32_t scale>
template<HdPackBlendMode blendMode>
void HdNesPack<scale>::DrawCustomBackground(HdBackgroundInfo& bgInfo, uint32_t *outputBuffer, uint32_t x, uint32_t y, uint32_t screenWidth, uint32_t screenX, uint32_t screenY, int32_t rawScrollX, int32_t rawScrollY, const vector<NesHdTextureInfo>* tex2d)
{
	uint32_t width = bgInfo.Data->Width;
	// <background> : tileX / tileY repetent l'image (modulo de sa taille en pixels NES)
	int32_t px = (int32_t)bgInfo.Left + (int32_t)x;
	int32_t py = (int32_t)bgInfo.Top + (int32_t)y;
	if(bgInfo.TileX || bgInfo.TileY) {
		auto posMod = [](int32_t a, int32_t m) { int32_t r = a % m; return r < 0 ? r + m : r; };
		int32_t pngW = (int32_t)(width / scale);
		int32_t pngH = (int32_t)(bgInfo.Data->Height / scale);
		if(bgInfo.TileX && pngW > 0) px = posMod(px, pngW);
		if(bgInfo.TileY && pngH > 0) py = posMod(py, pngH);
	}
	uint32_t *pngData = bgInfo.data() + ((uint32_t)py * scale * width) + ((uint32_t)px * scale);
	// <2DLiquid> mask=1 : couverture du fond de meme priorite (alpha par pixel HD) pour le liquide
	std::vector<uint8_t>* covBuf = nullptr;
	// position a l'ecran (screenX, screenY), pas dans l'image du fond (x, y contiennent le defilement, ramx= / ramy= compris)
	if(bgInfo.Priority >= 0 && bgInfo.Priority < 50 && !_liqCover[bgInfo.Priority].empty() && screenX >= _liquidOffX && screenY >= _liquidOffY) {
		covBuf = &_liqCover[bgInfo.Priority];
	}
	const size_t covBase = covBuf ? ((size_t)(screenY - _liquidOffY) * scale * screenWidth + (size_t)(screenX - _liquidOffX) * scale) : 0;
	const int64_t covKey = covBuf ? _liqKey[bgInfo.Priority] : -1; // maskcolor : couleur cle (RGB)
	auto markCover = [&](uint32_t i, uint32_t j, uint32_t px) {
		const size_t idx = covBase + (size_t)i * screenWidth + j;
		const bool keyed = covKey >= 0 && (int64_t)(px & 0xFFFFFFu) == covKey;
		const uint32_t al = keyed ? 0u : (px >> 24);
		if(idx < covBuf->size()) { uint8_t& c = (*covBuf)[idx]; if(al > c) c = (uint8_t)al; }
	};

	if(bgInfo.Brightness == 255) {
		for(uint32_t i = 0; i < scale; i++) {
			for(uint32_t j = 0; j < scale; j++) {
				uint32_t pixelColor = pngData[j];
				if(tex2d) pixelColor = ApplyTexture2D(*tex2d, pixelColor, (int32_t)(screenX * scale + j), (int32_t)(screenY * scale + i), rawScrollX, rawScrollY);
				if(covBuf) markCover(i, j, pixelColor);
				if constexpr(blendMode == HdPackBlendMode::Alpha) {
					if(pixelColor >= 0xFF000000) {
						outputBuffer[j] = pixelColor;
					} else if(pixelColor >= 0x01000000) {
						BlendColors<blendMode>((uint8_t*)(outputBuffer + j), (uint8_t*)(&pixelColor));
					}
				} else {
					BlendColors<blendMode>((uint8_t*)(outputBuffer + j), (uint8_t*)(&pixelColor));
				}
			}
			outputBuffer += screenWidth;
			pngData += width;
		}
	} else {
		for(uint32_t i = 0; i < scale; i++) {
			for(uint32_t j = 0; j < scale; j++) {
				uint32_t raw = pngData[j];
				if(tex2d) raw = ApplyTexture2D(*tex2d, raw, (int32_t)(screenX * scale + j), (int32_t)(screenY * scale + i), rawScrollX, rawScrollY);
				uint32_t pixelColor = AdjustBrightness((uint8_t*)(&raw), bgInfo.Brightness);
				if(covBuf) markCover(i, j, pixelColor);
				if constexpr(blendMode == HdPackBlendMode::Alpha) {
					if(pixelColor >= 0xFF000000) {
						outputBuffer[j] = pixelColor;
					} else if(pixelColor >= 0x01000000) {
						BlendColors<blendMode>((uint8_t*)(outputBuffer + j), (uint8_t*)(&pixelColor));
					}
				} else {
					BlendColors<blendMode>((uint8_t*)(outputBuffer + j), (uint8_t*)(&pixelColor));
				}
			}
			outputBuffer += screenWidth;
			pngData += width;
		}
	}
}

// <2DTexture> : si la couleur du pixel de fond correspond a une regle, le pixel est remplace par la texture
// (repetee, etiree par scalex/scaley, defilante avec le scroll PPU x ratio). hdX / hdY : position ecran en pixels HD.
template<uint32_t scale>
uint32_t HdNesPack<scale>::ApplyTexture2D(const vector<NesHdTextureInfo>& rules, uint32_t raw, int32_t hdX, int32_t hdY, int32_t rawScrollX, int32_t rawScrollY)
{
	const uint32_t alpha = raw >> 24;
	for(const NesHdTextureInfo& r : rules) {
		if(r.DioMode == (_dioDrawing ? 2 : 1)) continue; // diorama= off / only
		bool match = r.HasKey ? (alpha != 0 && (raw & 0xFFFFFFu) == r.Key) : (alpha == 0);
		if(!match && r.HasKey && alpha != 0 && r.Tolerance > 0) { // tolerance= : ecart maximal par canal
			const int dr = std::abs((int)((raw >> 16) & 0xFF) - (int)((r.Key >> 16) & 0xFF)), dg = std::abs((int)((raw >> 8) & 0xFF) - (int)((r.Key >> 8) & 0xFF)), db = std::abs((int)(raw & 0xFF) - (int)(r.Key & 0xFF));
			match = std::max(dr, std::max(dg, db)) <= r.Tolerance;
		}
		if(!match || r.Texture == nullptr || r.Texture->PixelData.empty() || r.Texture->Width == 0 || r.Texture->Height == 0) {
			continue;
		}
		const int64_t tw = r.Texture->Width;
		const int64_t th = r.Texture->Height;
		const int64_t offX = (int64_t)std::floor((float)rawScrollX * r.RatioX * scale);
		const int64_t offY = (int64_t)std::floor((float)rawScrollY * r.RatioY * scale);
		int64_t tu = (int64_t)std::floor((float)(hdX + offX) * r.TexelPerPxX) % tw;
		int64_t tv = (int64_t)std::floor((float)(hdY + offY) * r.TexelPerPxY) % th;
		if(tu < 0) tu += tw;
		if(tv < 0) tv += th;
		return r.Texture->PixelData[(size_t)(tv * tw + tu)];
	}
	return raw;
}

// <2DReflect> : pour chaque colonne de la ligne, ordonnee ecran (NES) du bord superieur du PNG du calque sol.
// NaN si le calque n'est pas actif ou si la colonne n'a pas de pixel opaque.
template<uint32_t scale>
void HdNesPack<scale>::ComputeReflectFloorLine(uint32_t nesY, const HdBgConfig* lineCfg, uint32_t xStart, uint32_t xEnd)
{
	const float nan = std::nanf("");
	const size_t rowBase = (size_t)nesY << 8;
	const int prio = _hdData->Reflects[0].Priority;
	const HdBgConfig* cfg = nullptr;
	for(int n = 0; n < 50; n++) {
		if(lineCfg[n].BackgroundIndex >= 0 && lineCfg[n].BgPriority == prio) {
			cfg = &lineCfg[n];
			break;
		}
	}
	HdBackgroundInfo* bgInfo = cfg ? &_hdData->BackgroundsByPriority[prio][cfg->BackgroundIndex] : nullptr;
	const HdPackBitmapInfo* bmp = (bgInfo && bgInfo->Data) ? bgInfo->Data : nullptr;
	if(!bmp || bmp->TopEdgeHd.empty()) {
		for(uint32_t x = xStart; x < xEnd; x++) _reflFloor[rowBase + x] = nan;
		return;
	}
	const int32_t pngW = (int32_t)(bmp->Width / scale);
	for(uint32_t x = xStart; x < xEnd; x++) {
		float floorY = nan;
		int32_t px = (int32_t)x + cfg->BgScrollX + (int32_t)bgInfo->Left;
		bool ok = true;
		if(bgInfo->TileX && pngW > 0) {
			px %= pngW;
			if(px < 0) px += pngW;
		} else if(px < 0 || px >= pngW) {
			ok = false;
		}
		if(ok) {
			const int32_t edge = bmp->TopEdgeHd[(size_t)px * scale];
			if(edge >= 0) {
				floorY = (float)edge / (float)scale - (float)(cfg->BgScrollY + (int32_t)bgInfo->Top);
			}
		}
		_reflFloor[rowBase + x] = floorY;
	}
}

// <2DReflect> : chaque pixel de sprite situe au-dessus du sol est recopie en miroir sous le sol.
// Distance d = sol - pixel ; reflet a sol + d ; opacite = intensite x (1 - d / fade) si fade > 0.
// Source et cible sont dans la meme colonne, la cible est toujours sous le sol : pas de recouvrement.
template<uint32_t scale>
void HdNesPack<scale>::ProcessReflections(uint32_t* out, uint32_t screenWidth, const OverscanDimensions& overscan)
{
	const NesHdReflectInfo& refl = _hdData->Reflects[0];
	const uint32_t yStart = overscan.Top, yEnd = 240 - overscan.Bottom;
	const uint32_t xStart = overscan.Left, xEnd = 256 - overscan.Right;
	for(uint32_t y = yStart; y < yEnd; y++) {
		for(uint32_t x = xStart; x < xEnd; x++) {
			const size_t idx = ((size_t)y << 8) | x;
			if(!_sprNes[idx]) continue;
			const float fl = _reflFloor[idx];
			if(std::isnan(fl)) continue;
			const float cy = (float)y + 0.5f;
			if(cy >= fl) continue;
			const float d = fl - cy;
			float a = refl.Intensity;
			if(refl.Fade > 0.0f) a *= std::max(0.0f, 1.0f - d / refl.Fade);
			if(a <= 0.0f) continue;
			const uint32_t ty = (uint32_t)std::floor(fl + d);
			if(ty < yStart || ty >= yEnd) continue;
			const uint32_t* src = out + (size_t)(y - yStart) * scale * screenWidth + (size_t)(x - xStart) * scale;
			uint32_t* dst = out + (size_t)(ty - yStart) * scale * screenWidth + (size_t)(x - xStart) * scale;
			const uint32_t ia = (uint32_t)std::min(256.0f, a * 256.0f);
			const uint32_t ib = 256 - ia;
			for(uint32_t i = 0; i < scale; i++) {
				for(uint32_t j = 0; j < scale; j++) {
					const uint32_t s = src[i * screenWidth + j];
					uint32_t& dd = dst[i * screenWidth + j];
					const uint32_t r = (((s >> 0) & 0xFF) * ia + ((dd >> 0) & 0xFF) * ib) >> 8;
					const uint32_t g = (((s >> 8) & 0xFF) * ia + ((dd >> 8) & 0xFF) * ib) >> 8;
					const uint32_t b = (((s >> 16) & 0xFF) * ia + ((dd >> 16) & 0xFF) * ib) >> 8;
					dd = r | (g << 8) | (b << 16) | 0xFF000000u;
				}
			}
		}
	}
}

template<uint32_t scale>
void HdNesPack<scale>::DrawTile(HdPpuTileInfo &tileInfo, HdPackTileInfo &hdPackTileInfo, uint32_t *outputBuffer, uint32_t screenWidth)
{
	if(hdPackTileInfo.IsFullyTransparent) {
		return;
	}

	// Miroir effectif : celui de la tuile PPU, annule si l'image du pack est deja retournee (<tile_hflip> / <tile_vflip>)
	const bool hMirror = tileInfo.HorizontalMirroring != hdPackTileInfo.IsHFlip;
	const bool vMirror = tileInfo.VerticalMirroring != hdPackTileInfo.IsVFlip;
	uint32_t *bitmapData = hdPackTileInfo.HdTileData.data();
	uint32_t tileWidth = 8 * scale;
	uint8_t tileOffsetX = hMirror ? 7 - tileInfo.OffsetX : tileInfo.OffsetX;
	uint32_t bitmapOffset = (tileInfo.OffsetY * scale) * tileWidth + tileOffsetX * scale;
	int32_t bitmapSmallInc = 1;
	int32_t bitmapLargeInc = tileWidth - scale;
	if(hMirror) {
		bitmapOffset += scale - 1;
		bitmapSmallInc = -1;
		bitmapLargeInc = tileWidth + scale;
	}
	if(vMirror) {
		bitmapOffset += tileWidth * (scale - 1);
		bitmapLargeInc = (hMirror ? (int32_t)scale : -(int32_t)scale) - (int32_t)tileWidth;
	}

	uint32_t rgbValue;
	if(hdPackTileInfo.HasTransparentPixels || hdPackTileInfo.Brightness != 255) {
		for(uint32_t y = 0; y < scale; y++) {
			for(uint32_t x = 0; x < scale; x++) {
				if(hdPackTileInfo.Brightness == 255) {
					rgbValue = *(bitmapData + bitmapOffset);
				} else {
					rgbValue = AdjustBrightness((uint8_t*)(bitmapData + bitmapOffset), hdPackTileInfo.Brightness);
				}

				if(!hdPackTileInfo.HasTransparentPixels || (bitmapData[bitmapOffset] & 0xFF000000) == 0xFF000000) {
					*outputBuffer = rgbValue;
				} else {
					if(bitmapData[bitmapOffset] & 0xFF000000) {
						BlendColors<HdPackBlendMode::Alpha>((uint8_t*)outputBuffer, (uint8_t*)&rgbValue);
					}
				}
				outputBuffer++;
				bitmapOffset += bitmapSmallInc;
			}
			bitmapOffset += bitmapLargeInc;
			outputBuffer += screenWidth - scale;
		}
	} else {
		for(uint32_t y = 0; y < scale; y++) {
			for(uint32_t x = 0; x < scale; x++) {
				*outputBuffer = *(bitmapData + bitmapOffset);
				outputBuffer++;
				bitmapOffset += bitmapSmallInc;
			}
			bitmapOffset += bitmapLargeInc;
			outputBuffer += screenWidth - scale;
		}
	}
}

// ---------------------------------------------------------------------------
// <2DOverlay> NES : particules 2D (voir claude/09_2DOverlay.md et claude/18_Architecture_PPU_NES.md)
// ---------------------------------------------------------------------------
namespace {
	constexpr float OverlayPi = 3.14159265358979f;

	inline float LerpF(float a, float b, float t) { return a + (b - a) * t; }

	// Bruit de valeur lisse (0..1) : memes fonctions que le SNES
	inline float FxHash(int x, int y, int seed)
	{
		uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= h >> 16;
		return (float)(h & 0xFFFF) / 65535.0f;
	}
	inline float FxNoise(float x, float y, int seed)
	{
		const int xi = (int)std::floor(x), yi = (int)std::floor(y);
		const float fx = x - (float)xi, fy = y - (float)yi;
		const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
		const float a = FxHash(xi, yi, seed), b = FxHash(xi + 1, yi, seed);
		const float c = FxHash(xi, yi + 1, seed), d = FxHash(xi + 1, yi + 1, seed);
		const float top = a + (b - a) * ux, bot = c + (d - c) * ux;
		return top + (bot - top) * uy;
	}

	// Scroll PPU d'une ligne : memes formules que OnLineStart et ComputeLineBgConfig
	inline int32_t NesLineScrollX(const HdPpuPixelInfo& p)
	{
		return ((p.TmpVideoRamAddr & 0x1F) << 3) | p.XScroll | ((p.TmpVideoRamAddr & 0x400) ? 0x100 : 0);
	}
	inline int32_t NesLineScrollY(const HdPpuPixelInfo& p)
	{
		return (((p.TmpVideoRamAddr & 0x3E0) >> 2) | ((p.TmpVideoRamAddr & 0x7000) >> 12)) + ((p.TmpVideoRamAddr & 0x800) ? 240 : 0);
	}

	// Ecart de defilement modulo la periode (512 en X, 480 en Y). 0 si saut (changement de salle).
	inline int32_t NesScrollDelta(int32_t now, int32_t last, int32_t period)
	{
		int32_t d = ((now - last) % period + period) % period;
		if(d > period / 2) {
			d -= period;
		}
		return (std::abs(d) > 64) ? 0 : d;
	}

	inline float WrapCoord(float v, float period)
	{
		v = std::fmod(v, period);
		if(v < 0) {
			v += period;
		}
		return v;
	}

	void SpawnOverlayParticle(NesHdOverlayInfo& ov, float fieldW, float fieldH)
	{
		NesHdParticle p;
		p.X = WrapCoord((ov.SpawnX + ov.RandRange(0.0f, ov.SpawnW)) * fieldW, fieldW);
		p.Y = WrapCoord((ov.SpawnY + ov.RandRange(0.0f, ov.SpawnH)) * fieldH, fieldH);
		float angle = ov.RandRange(ov.AngleMin, ov.AngleMax) * OverlayPi / 180.0f;
		float speed = ov.RandRange(ov.SpeedMin, ov.SpeedMax);
		p.VX = std::cos(angle) * speed;
		p.VY = std::sin(angle) * speed;
		p.Age = 0;
		p.Life = ov.RandRange(ov.LifeMin, ov.LifeMax);
		p.Size = 1.0f + ov.RandRange(-ov.SizeRandom, ov.SizeRandom);
		p.Rot = ov.RandRange(ov.RotationMin, ov.RotationMax) * OverlayPi / 180.0f;
		p.Spin = ov.RandRange(ov.SpinMin, ov.SpinMax) * OverlayPi / 180.0f;
		p.Seed1 = ov.Rand() * 2.0f * OverlayPi;
		p.Seed2 = ov.Rand() * 2.0f * OverlayPi;
		ov.Particles.push_back(p);
	}

	void StepOverlayParticles(NesHdOverlayInfo& ov, float dt, float fieldW, float fieldH)
	{
		ov.SpawnAcc += ov.Rate * dt;
		while(ov.SpawnAcc >= 1.0f) {
			ov.SpawnAcc -= 1.0f;
			if((int)ov.Particles.size() < ov.MaxParticles) {
				SpawnOverlayParticle(ov, fieldW, fieldH);
			}
		}

		const float w = 2.0f * OverlayPi * ov.TurbulenceFreq;
		for(size_t k = 0; k < ov.Particles.size();) {
			NesHdParticle& p = ov.Particles[k];
			p.Age += dt;
			if(p.Age >= p.Life) {
				p = ov.Particles.back();
				ov.Particles.pop_back();
				continue;
			}
			p.VX += ov.GravityX * dt;
			p.VY += ov.GravityY * dt;
			p.EVX = p.VX + ov.WindX + ov.Turbulence * std::sin(w * p.Age + p.Seed1); // vitesse effective (stretch=)
			p.EVY = p.VY + ov.WindY + ov.Turbulence * std::sin(w * p.Age + p.Seed2);
			p.X = WrapCoord(p.X + p.EVX * dt, fieldW);
			p.Y = WrapCoord(p.Y + p.EVY * dt, fieldH);
			p.Rot += p.Spin * dt;
			k++;
		}
	}
}

template<uint32_t scale>
void HdNesPack<scale>::SimulateOverlays(HdScreenInfo* hdScreenInfo)
{
	const float dt = 1.0f / 60.0f;
	// <2DLiquid> : activation et temps, meme sans <2DOverlay> dans le pack (avant le retour anticipe)
	for(auto& liqPtr : _hdData->Liquids) {
		liqPtr->Active = liqPtr->MatchesCondition();
		if(liqPtr->Active) {
			liqPtr->Time += dt;
		}
	}

	if(_hdData->Overlays.empty()) {
		return;
	}

	// Defilement lu sur la ligne du milieu de l'ecran (comme cote SNES)
	const HdPpuPixelInfo& midPixel = hdScreenInfo->ScreenTiles[(NesConstants::ScreenHeight / 2) << 8];
	const int32_t scrollX = NesLineScrollX(midPixel);
	const int32_t scrollY = NesLineScrollY(midPixel);

	for(auto& ovPtr : _hdData->Overlays) {
		NesHdOverlayInfo& ov = *ovPtr;
		ov.Active = ov.MatchesCondition();

		// L'ancrage suit le PPU meme quand l'effet est masque : au retour, il est deja au bon endroit du decor
		if(!ov.HaveScroll) {
			ov.LastScrollX = scrollX;
			ov.LastScrollY = scrollY;
			ov.HaveScroll = true;
		}
		const int32_t dx = NesScrollDelta(scrollX, ov.LastScrollX, 512);
		const int32_t dy = NesScrollDelta(scrollY, ov.LastScrollY, 480);
		ov.LastScrollX = scrollX;
		ov.LastScrollY = scrollY;
		if(ov.Layer > 0) {
			ov.OffsetX += dx * ov.SpeedX;
			ov.OffsetY += dy * ov.SpeedY;
		}

		if(!ov.Active) {
			continue;
		}

		const float fieldW = _simFieldNes * ov.FieldW;
		const float fieldH = 240.0f * ov.FieldH;
		if(ov.IsNoise || ov.IsRays || ov.IsAurora || ov.IsDistortion) {
			ov.Time += dt;
			continue;
		}
		if(ov.IsPlainScreen || ov.IsEllipse || ov.IsTorch) {
			continue;
		}
		if(!ov.Initialized) {
			ov.Initialized = true;
			ov.Particles.clear();
			ov.SpawnAcc = 0;
			if(ov.Prewarm) {
				const float maxTime = std::min(ov.LifeMax, 20.0f);
				for(float t = 0; t < maxTime; t += dt) {
					StepOverlayParticles(ov, dt, fieldW, fieldH);
				}
			}
		}
		StepOverlayParticles(ov, dt, fieldW, fieldH);
	}
}

template<uint32_t scale>
void HdNesPack<scale>::ProcessScreenLayersLayout(uint32_t* outputBuffer, uint32_t screenWidth, float originNes, uint32_t oTop, uint32_t oBottom)
{
	// Calques plainscreen, ellipse, ombres / nuages / brouillard : passe finale sur l'image visible (comme cote SNES)
	const int hdHeight = (int)((240 - oTop - oBottom) * scale);
	if(screenWidth == 0 || hdHeight <= 0) {
		return;
	}
	const float fScale = (float)scale;
	const float visW = (float)screenWidth, visH = (float)hdHeight;
	// Boucle par lignes : chaque ligne ecrit ses propres pixels ; repartie sur le pool de threads
	auto forRows = [&](int r0, int r1, auto&& fn) {
		const int n = std::max(1, std::min(_pool.Size(), r1 - r0));
		if(n <= 1) {
			for(int y = r0; y < r1; y++) fn(y);
			return;
		}
		_pool.Run(n, [&](int t) {
			for(int y = r0 + t; y < r1; y += n) fn(y);
		});
	};

	// <2DSpriteLight> : halo 2D au centre de chaque tile de sprite reconnue. Additif, ou assombrissant en mode=shadow.
	// Les sprites viennent des infos PPU par pixel (zone 256 x 240) ; l'origine de la tile est deduite de l'offset du pixel.
	if(!_hdData->Sprite2DLights.empty() && _hdScreenInfo != nullptr) {
		struct Light2D { float cx, cy, rad, cr, cg, cb, inten; bool shadow; };
		std::vector<Light2D> lights2d;
		std::vector<uint32_t> seen; // (regle, origine x, origine y) deja ajoutes
		const float tSec = (float)_hdScreenInfo->FrameNumber / 60.0f;
		const size_t nRules = _hdData->Sprite2DLights.size();
		for(int y = 0; y < 240; y++) {
			for(int x = 0; x < 256; x++) {
				HdPpuPixelInfo& pi = _hdScreenInfo->ScreenTiles[(y << 8) | x];
				for(int s = 0; s < pi.SpriteCount; s++) {
					HdPpuTileInfo& spr = pi.Sprite[s];
					for(size_t r = 0; r < nRules; r++) {
						NesHdSpriteLightInfo& rule = *_hdData->Sprite2DLights[r];
						if(!rule.Matches(spr)) continue;
						const int ox = x - (spr.HorizontalMirroring ? 7 - (int)spr.OffsetX : (int)spr.OffsetX);
						const int oy = y - (spr.VerticalMirroring ? 7 - (int)spr.OffsetY : (int)spr.OffsetY);
						const uint32_t id = ((uint32_t)(r & 0xFFF) << 20) | ((uint32_t)((ox + 16) & 0x3FF) << 10) | (uint32_t)((oy + 16) & 0x3FF);
						if(std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
						seen.push_back(id);
						bool condOk = true;
						for(HdPackCondition* c : rule.Conditions) { if(!c->CheckCondition(x, y, &spr)) { condOk = false; break; } }
						if(!condOk) continue;
						// centre de la tile + dx / dy (inverses quand la tile est retournee)
						const float cxNes = (float)ox + 4.0f + (spr.HorizontalMirroring ? -rule.OffsetX : rule.OffsetX);
						const float cyNes = (float)oy + 4.0f + (spr.VerticalMirroring ? -rule.OffsetY : rule.OffsetY);
						const float fk = rule.Flicker > 0.0f ? 1.0f - rule.Flicker * 0.5f * (1.0f + std::sin(rule.FlickerSpeed * tSec)) : 1.0f;
						lights2d.push_back({ (cxNes - originNes) * fScale, (cyNes - (float)oTop) * fScale, std::max(1.0f, rule.Radius) * fScale,
							(float)((rule.Color >> 16) & 0xFF), (float)((rule.Color >> 8) & 0xFF), (float)(rule.Color & 0xFF),
							rule.Intensity * fk, rule.Shadow });
					}
				}
			}
		}
		for(const Light2D& L : lights2d) {
			const int x0 = std::max(0, (int)std::floor(L.cx - L.rad)), x1 = std::min((int)screenWidth - 1, (int)std::ceil(L.cx + L.rad));
			const int y0 = std::max(0, (int)std::floor(L.cy - L.rad)), y1 = std::min(hdHeight - 1, (int)std::ceil(L.cy + L.rad));
			for(int y = y0; y <= y1; y++) {
				uint32_t* row = outputBuffer + (size_t)y * screenWidth;
				for(int x = x0; x <= x1; x++) {
					const float ddx = (float)x + 0.5f - L.cx, ddy = (float)y + 0.5f - L.cy;
					const float f = std::clamp(1.0f - std::sqrt(ddx * ddx + ddy * ddy) / L.rad, 0.0f, 1.0f);
					const float k = f * f * L.inten;
					if(k <= 0.0f) continue;
					const uint32_t d = row[x];
					float r = (float)((d >> 16) & 0xFF), g = (float)((d >> 8) & 0xFF), b = (float)(d & 0xFF);
					if(L.shadow) { const float keep = std::clamp(1.0f - k, 0.0f, 1.0f); r *= keep; g *= keep; b *= keep; }
					else { r += L.cr * k; g += L.cg * k; b += L.cb * k; }
					row[x] = 0xFF000000u | ((uint32_t)std::clamp(r + 0.5f, 0.0f, 255.0f) << 16) | ((uint32_t)std::clamp(g + 0.5f, 0.0f, 255.0f) << 8) | (uint32_t)std::clamp(b + 0.5f, 0.0f, 255.0f);
				}
			}
		}
	}

	for(auto& ovPtr : _hdData->Overlays) {
		NesHdOverlayInfo& ov = *ovPtr;
		if(!ov.Active || !(ov.IsPlainScreen || ov.IsEllipse || ov.IsTorch || ov.IsNoise || ov.IsRays || ov.IsAurora)) {
			continue;
		}
		const int zx0 = (int)std::clamp(ov.ZoneX0 * visW, 0.0f, visW);
		const int zx1 = (int)std::clamp(ov.ZoneX1 * visW, 0.0f, visW);
		const int zy0 = (int)std::clamp(ov.ZoneY0 * visH, 0.0f, visH);
		const int zy1 = (int)std::clamp(ov.ZoneY1 * visH, 0.0f, visH);
		if(zx0 >= zx1 || zy0 >= zy1) {
			continue;
		}

		const float a = ov.Alpha;
		const float cr = (float)((ov.FxColor >> 16) & 0xFF);
		const float cg = (float)((ov.FxColor >> 8) & 0xFF);
		const float cb = (float)(ov.FxColor & 0xFF);
		// Melange d'un pixel avec la couleur de l'overlay, opacite = alpha de l'overlay x facteur local
		auto blend = [&](uint32_t& dst, float factor) {
			const float k = a * factor;
			const float keep = 1.0f - k;
			float r = (float)((dst >> 16) & 0xFF) * keep + cr * k;
			float g = (float)((dst >> 8) & 0xFF) * keep + cg * k;
			float b = (float)(dst & 0xFF) * keep + cb * k;
			dst = 0xFF000000 | ((uint32_t)std::clamp(r, 0.0f, 255.0f) << 16) | ((uint32_t)std::clamp(g, 0.0f, 255.0f) << 8) | (uint32_t)std::clamp(b, 0.0f, 255.0f);
		};

		if(ov.IsPlainScreen) {
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = outputBuffer + (size_t)y * screenWidth;
				for(int x = zx0; x < zx1; x++) {
					blend(row[x], 1.0f);
				}
			});
		} else if(ov.IsEllipse) {
			// Centre et rayons : pixels NES du cadre complet, convertis en pixels HD de la sortie
			const float cx = ((float)ov.FxEllipseX - originNes) * fScale;
			const float cy = ((float)ov.FxEllipseY - (float)oTop) * fScale;
			const float rx = std::max(0.5f, ov.FxRadiusX * fScale);
			const float ry = std::max(0.5f, ov.FxRadiusY * fScale);
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = outputBuffer + (size_t)y * screenWidth;
				const float dy = ((float)y + 0.5f - cy) / ry;
				for(int x = zx0; x < zx1; x++) {
					const float dx = ((float)x + 0.5f - cx) / rx;
					const bool inside = dx * dx + dy * dy < 1.0f;
					// normal : l'ellipse est teintee ; negatif : tout sauf l'ellipse est couvert (fenetre de jeu)
					if(inside != ov.FxNegative) {
						blend(row[x], 1.0f);
					}
				}
			});
		} else if(ov.IsTorch) {
			// torche : halo doux ; le centre suit le scroll PPU (decalage d'ancrage, periodes 512 x 480), copie a chaque periode
			const float rad = std::max(1.0f, ov.TorchRadius * fScale);
			const float soft = std::clamp(ov.TorchSoft, 0.01f, 1.0f);
			auto wrapN = [](float v, float per) { v = std::fmod(v, per); return v < 0.0f ? v + per : v; };
			const float bxNes = wrapN(ov.TorchX - ov.OffsetX, 512.0f), byNes = wrapN(ov.TorchY - ov.OffsetY, 480.0f);
			const float bx = (bxNes - originNes) * fScale, by = (byNes - (float)oTop) * fScale;
			const float tcx[2] = { bx, bx - 512.0f * fScale }, tcy[2] = { by, by - 480.0f * fScale };
			const bool dark = ov.TorchDark;
			const float glow = ov.TorchGlow;
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = outputBuffer + (size_t)y * screenWidth;
				const float py = (float)y + 0.5f;
				for(int x = zx0; x < zx1; x++) {
					const float px = (float)x + 0.5f;
					float L = 0.0f;
					for(int i = 0; i < 2; i++) {
						for(int j = 0; j < 2; j++) {
							const float ddx = px - tcx[i], ddy = py - tcy[j];
							const float rr = std::sqrt(ddx * ddx + ddy * ddy) / rad;
							L = std::max(L, std::clamp((1.0f - rr) / soft, 0.0f, 1.0f));
						}
					}
					const uint32_t d = row[x];
					const float dr = (float)((d >> 16) & 0xFF), dg = (float)((d >> 8) & 0xFF), db = (float)(d & 0xFF);
					float nr, ng, nb;
					if(dark) {
						const float keep = 1.0f - a * (1.0f - L);
						nr = dr * keep + cr * glow * L * a; ng = dg * keep + cg * glow * L * a; nb = db * keep + cb * glow * L * a;
					} else {
						nr = dr + cr * L * a; ng = dg + cg * L * a; nb = db + cb * L * a;
					}
					row[x] = 0xFF000000u | ((uint32_t)std::clamp(nr + 0.5f, 0.0f, 255.0f) << 16) | ((uint32_t)std::clamp(ng + 0.5f, 0.0f, 255.0f) << 8) | (uint32_t)std::clamp(nb + 0.5f, 0.0f, 255.0f);
				}
			});
		} else if(ov.IsRays) {
			// Rayons : faisceaux additifs partant d'un point ; intensite calculee sur une grille de noeuds (pas de 4 px HD), interpolee
			const int kStep = 4;
			const float radToDeg = 180.0f / OverlayPi;
			const float sx0 = ov.RayX * visW, sy0 = ov.RayY * visH;
			const float len = ov.RayLength * std::sqrt(visW * visW + visH * visH);
			const int N = ov.RayCount;
			// angle et amplitude de chaque faisceau pour cette image (oscillation + scintillement)
			std::vector<float> bAng(N), bAmp(N);
			for(int k = 0; k < N; k++) {
				const float f = (N > 1) ? (float)k / (float)(N - 1) : 0.5f;
				const float base = (ov.RaySpread >= 360.0f) ? ov.RayAngle + 360.0f * (float)k / (float)N : ov.RayAngle + (f - 0.5f) * ov.RaySpread;
				bAng[k] = base + ov.RayWobble * std::sin(2.0f * OverlayPi * (ov.RaySpeed * 0.5f * ov.Time) + (float)k * 1.7f);
				bAmp[k] = 0.6f + 0.4f * std::sin(2.0f * OverlayPi * (ov.RaySpeed * ov.Time + (float)k * 0.37f));
			}
			const float invW = 1.0f / std::max(0.01f, ov.RayWidth);
			const int gx = (zx1 - zx0) / kStep + 2, gy = (zy1 - zy0) / kStep + 2;
			std::vector<float> node((size_t)gx * gy, 0.0f);
			forRows(0, gy, [&](int j) {
				const float dy = (float)(zy0 + j * kStep) + 0.5f - sy0;
				for(int ii = 0; ii < gx; ii++) {
					const float dx = (float)(zx0 + ii * kStep) + 0.5f - sx0;
					const float dist = std::sqrt(dx * dx + dy * dy);
					float I = 0.0f;
					if(dist < len) {
						const float pa = std::atan2(dy, dx) * radToDeg;
						for(int k = 0; k < N; k++) {
							const float d = std::fmod(pa - bAng[k] + 540.0f, 360.0f) - 180.0f;
							const float q = d * invW;
							if(q > 3.0f || q < -3.0f) continue;
							I += bAmp[k] * std::exp(-q * q);
						}
						const float fade = 1.0f - dist / len;
						I *= fade * fade * ov.Alpha;
					}
					node[(size_t)j * gx + ii] = I;
				}
			});
			const float cR = (float)((ov.RayColor >> 16) & 0xFF), cG = (float)((ov.RayColor >> 8) & 0xFF), cB = (float)(ov.RayColor & 0xFF);
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = outputBuffer + (size_t)y * screenWidth;
				const float fy = (float)(y - zy0) / (float)kStep;
				const int jy = (int)fy;
				const float ty = fy - (float)jy;
				const float* n0 = &node[(size_t)jy * gx];
				const float* n1 = &node[(size_t)(jy + 1) * gx];
				for(int x = zx0; x < zx1; x++) {
					const float fx = (float)(x - zx0) / (float)kStep;
					const int ix = (int)fx;
					const float tx = fx - (float)ix;
					const float I = (n0[ix] * (1.0f - tx) + n0[ix + 1] * tx) * (1.0f - ty) + (n1[ix] * (1.0f - tx) + n1[ix + 1] * tx) * ty;
					if(I <= 0.0f) continue;
					const uint32_t c = row[x];
					const float rr = (float)((c >> 16) & 0xFF) + cR * I;
					const float gg = (float)((c >> 8) & 0xFF) + cG * I;
					const float bb = (float)(c & 0xFF) + cB * I;
					row[x] = 0xFF000000 | ((uint32_t)std::min(255.0f, rr) << 16) | ((uint32_t)std::min(255.0f, gg) << 8) | (uint32_t)std::min(255.0f, bb);
				}
			});
		} else if(ov.IsAurora) {
			// Aurore : rideaux additifs (bruit etire en hauteur, derive horizontal), plus lumineux vers le bas de la zone,
			// bord inferieur ondule ; couleur melangee entre color1 et color2. Grille de noeuds (pas de 4 px HD), interpolee.
			const int kStep = 4;
			const float zh = (float)(zy1 - zy0);
			const float size = std::max(4.0f, ov.AuroraSize);
			const float drift = ov.AuroraSpeed * ov.Time;                       // pixels NES
			const float thr = 1.0f - std::clamp(ov.AuroraCoverage, 0.0f, 1.0f);
			const float fall = std::max(0.05f, ov.AuroraFall);
			const float waveT = (zh > 0.0f) ? ov.AuroraWave / (zh / fScale) : 0.0f; // ondulation en fraction de la zone
			const int seed = ov.FxSeed;
			const int gx = (zx1 - zx0) / kStep + 2, gy = (zy1 - zy0) / kStep + 2;
			std::vector<float> nI((size_t)gx * gy, 0.0f), nM((size_t)gx * gy, 0.0f);
			forRows(0, gy, [&](int j) {
				const float y = (float)(zy0 + j * kStep) + 0.5f;
				const float py = y / fScale;
				const float t = (y - (float)zy0) / std::max(1.0f, zh);            // 0 en haut de la zone, 1 en bas
				for(int ii = 0; ii < gx; ii++) {
					const float x = (float)(zx0 + ii * kStep) + 0.5f;
					const float u = (x / fScale - drift) / size;
					const float v = py / (size * 3.0f) + ov.Time * 0.05f;
					const float n = 0.65f * FxNoise(u, v, seed) + 0.35f * FxNoise(u * 2.03f + 17.0f, v * 2.03f + 9.0f, seed + 1);
					const float edge = FxNoise(u * 0.5f + 3.0f, ov.Time * 0.3f, seed + 2) - 0.5f;
					const float tn = t + waveT * edge * 2.0f;
					float prof = std::clamp((tn - (1.0f - fall)) / fall, 0.0f, 1.0f);
					prof = prof * prof * (3.0f - 2.0f * prof);
					float m = std::clamp((n - thr) / 0.12f, 0.0f, 1.0f);
					m = m * m * (3.0f - 2.0f * m);
					nI[(size_t)j * gx + ii] = m * prof * ov.Alpha;
					nM[(size_t)j * gx + ii] = FxNoise(u * 0.5f + 5.0f, v * 0.5f + 1.0f, seed + 3);
				}
			});
			const float c1R = (float)((ov.AuroraColor1 >> 16) & 0xFF), c1G = (float)((ov.AuroraColor1 >> 8) & 0xFF), c1B = (float)(ov.AuroraColor1 & 0xFF);
			const float c2R = (float)((ov.AuroraColor2 >> 16) & 0xFF), c2G = (float)((ov.AuroraColor2 >> 8) & 0xFF), c2B = (float)(ov.AuroraColor2 & 0xFF);
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = outputBuffer + (size_t)y * screenWidth;
				const float fy = (float)(y - zy0) / (float)kStep;
				const int jy = (int)fy;
				const float ty = fy - (float)jy;
				const float* i0 = &nI[(size_t)jy * gx];
				const float* i1 = &nI[(size_t)(jy + 1) * gx];
				const float* m0 = &nM[(size_t)jy * gx];
				const float* m1 = &nM[(size_t)(jy + 1) * gx];
				for(int x = zx0; x < zx1; x++) {
					const float fx = (float)(x - zx0) / (float)kStep;
					const int ix = (int)fx;
					const float tx = fx - (float)ix;
					const float I = (i0[ix] * (1.0f - tx) + i0[ix + 1] * tx) * (1.0f - ty) + (i1[ix] * (1.0f - tx) + i1[ix + 1] * tx) * ty;
					if(I <= 0.0f) continue;
					const float M = (m0[ix] * (1.0f - tx) + m0[ix + 1] * tx) * (1.0f - ty) + (m1[ix] * (1.0f - tx) + m1[ix + 1] * tx) * ty;
					const float cR = c1R + (c2R - c1R) * M, cG = c1G + (c2G - c1G) * M, cB = c1B + (c2B - c1B) * M;
					const uint32_t c = row[x];
					const float rr = (float)((c >> 16) & 0xFF) + cR * I;
					const float gg = (float)((c >> 8) & 0xFF) + cG * I;
					const float bb = (float)(c & 0xFF) + cB * I;
					row[x] = 0xFF000000 | ((uint32_t)std::min(255.0f, rr) << 16) | ((uint32_t)std::min(255.0f, gg) << 8) | (uint32_t)std::min(255.0f, bb);
				}
			});
		} else {
			// Ombres / nuages / brouillard : bruit a deux octaves qui derive (direction, vitesse), seuille par la couverture
			const float size = std::max(4.0f, ov.FxSize);
			const float dirRad = ov.FxDirection * OverlayPi / 180.0f;
			const float ox = std::cos(dirRad) * ov.FxSpeed * ov.Time, oy = std::sin(dirRad) * ov.FxSpeed * ov.Time; // pixels NES
			const float thr = 1.0f - std::clamp(ov.FxCoverage, 0.0f, 1.0f);
			const int seed = ov.FxSeed, kind = ov.NoiseKind;
			const int iS = (int)scale;
			// Valeur du bruit pour un bloc NES (bx, by) : echantillonnee au centre du bloc, lissee, seuillee
			auto blockM = [&](int bx, int by) -> float {
				const float px = (float)bx + 0.5f - ox;
				const float py = (float)by + 0.5f - oy;
				const float u = px / size, v = py / size;
				const float n = 0.65f * FxNoise(u, v, seed) + 0.35f * FxNoise(u * 2.03f + 17.0f, v * 2.03f + 9.0f, seed + 1);
				float m;
				if(kind == 2) m = std::clamp((n - (thr - 0.1f)) / 0.4f, 0.0f, 1.0f) * 0.85f; // brouillard : bords doux
				else if(kind == 1) m = std::clamp((n - thr) / 0.08f, 0.0f, 1.0f);            // nuages : bords nets
				else m = std::clamp((n - thr) / 0.12f, 0.0f, 1.0f);                           // ombres
				return m * m * (3.0f - 2.0f * m);
			};
			// Bruit calcule une fois par bloc NES (scale x scale pixels HD), en parallele par lignes de blocs.
			// Chaque pixel HD lit la valeur de son bloc : cout divise par scale^2, aspect a la resolution NES.
			const int bx0 = zx0 / iS, bx1 = (zx1 - 1) / iS;
			const int by0 = zy0 / iS, by1 = (zy1 - 1) / iS;
			const int gwz = bx1 - bx0 + 1, ghz = by1 - by0 + 1;
			std::vector<float> nm((size_t)gwz * ghz, 0.0f);
			const int nT = std::max(1, std::min(_pool.Size(), ghz));
			_pool.Run(nT, [&](int t) {
				for(int j = t; j < ghz; j += nT) {
					for(int i = 0; i < gwz; i++) {
						nm[(size_t)j * gwz + i] = blockM(bx0 + i, by0 + j);
					}
				}
			});
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = outputBuffer + (size_t)y * screenWidth;
				const size_t rowOff = (size_t)(y / iS - by0) * gwz;
				for(int x = zx0; x < zx1; x++) {
					const float m = nm[rowOff + (size_t)(x / iS - bx0)];
					if(m <= 0.0f) {
						continue;
					}
					blend(row[x], m);
				}
			});
		}
	}
}

// Calques de plein ecran : chemin non widescreen (inchange).
template<uint32_t scale>
void HdNesPack<scale>::ProcessScreenLayers(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan)
{
	ProcessScreenLayersLayout(outputBuffer, screenWidth, (float)overscan.Left, overscan.Top, overscan.Bottom);
}

// Widescreen (voie 1) : calques de plein ecran sur toute la largeur elargie (bandes incluses).
// Zones et rayons en fraction de la largeur elargie ; ellipse en pixels NES du cadre, decales de l'origine elargie.
template<uint32_t scale>
void HdNesPack<scale>::ProcessWidescreenScreenLayers(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom)
{
	if(out == nullptr || outWidth == 0) {
		return;
	}
	const float ws = (float)padHd / (float)scale;
	ProcessScreenLayersLayout(out, outWidth, (float)overscanLeft - ws, overscanTop, overscanBottom);
}

template<uint32_t scale>
void HdNesPack<scale>::PrepareLiquids(uint32_t screenWidth)
{
	const float S = (float)scale;
	const float twoPi = 2.0f * OverlayPi;
	const float degToRad = OverlayPi / 180.0f;
	const int hdRows = 240 * (int)scale;
	// <2DLiquid> mask=1 : tampon de couverture par priorite de <background>, remis a zero chaque image
	for(auto& c : _liqCover) c.clear();
	for(auto& k : _liqKey) k = -1;
	for(auto& liqPtr : _hdData->Liquids) {
		if(liqPtr->Active && liqPtr->MaskBg) _liqCover[liqPtr->Priority].assign((size_t)screenWidth * hdRows, 0);
		if(liqPtr->Active && liqPtr->MaskBg && liqPtr->HasMaskKey) _liqKey[liqPtr->Priority] = (int64_t)liqPtr->MaskKey;
	}
	auto liqHash = [](uint32_t v) -> uint32_t { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; };
	for(auto& liqPtr : _hdData->Liquids) {
		NesHdLiquidInfo& liq = *liqPtr;
		// followx / followy : scroll PPU d'une ligne de reference (milieu de la zone), cumule d'image en image (sans saut au retour a 0)
		if((liq.FollowX != 0.0f || liq.FollowY != 0.0f) && _hdScreenInfo) {
			const int line = std::clamp((int)((liq.YMin + std::min(liq.YMax, 240.0f)) * 0.5f), 0, 239);
			const HdPpuPixelInfo& li = _hdScreenInfo->ScreenTiles[(size_t)line << 8];
			const int32_t sx = NesLineScrollX(li), sy = NesLineScrollY(li);
			if(!liq.FollowHasLast) { liq.FollowAccX = (float)sx; liq.FollowAccY = (float)sy; liq.FollowHasLast = true; }
			else {
				int32_t dx = (sx - liq.FollowLastX) % 512; if(dx >= 256) dx -= 512; if(dx < -256) dx += 512;
				int32_t dy = (sy - liq.FollowLastY) % 480; if(dy >= 240) dy -= 480; if(dy < -240) dy += 480;
				liq.FollowAccX += (float)dx; liq.FollowAccY += (float)dy;
			}
			liq.FollowLastX = sx; liq.FollowLastY = sy;
		}
		liq.Col0 = (int)std::clamp(liq.XMin * (float)screenWidth, 0.0f, (float)screenWidth);
		liq.Col1 = (int)std::clamp(liq.XMax * (float)screenWidth, 0.0f, (float)screenWidth);
		liq.Surface.assign(screenWidth, 0.0f);
		liq.BubbleMap.clear();
		if(!liq.Active || liq.IsTop) {
			continue;
		}
		for(int x = liq.Col0; x < liq.Col1; x++) {
			const float xN = (float)x / S;
			liq.Surface[x] = liq.Level + liq.Amp * std::sin(twoPi * (xN / liq.Wave + liq.Speed * liq.Time) + liq.Phase * degToRad);
		}
		if(liq.BubbleCount <= 0 || liq.Col1 <= liq.Col0) {
			continue;
		}
		// Bulles : rasterisees une fois par image dans une carte alpha HD (position deterministe, hash + temps)
		liq.BubbleMap.assign((size_t)screenWidth * hdRows, 0);
		const float bottom = liq.Level + liq.Depth * 0.7f;
		for(int b = 0; b < liq.BubbleCount; b++) {
			const uint32_t h1 = liqHash((uint32_t)b * 2654435761u + 1u);
			const uint32_t h2 = liqHash(h1 + 0x9E3779B9u);
			const uint32_t h3 = liqHash(h2 + 0x85EBCA6Bu);
			const float r1 = (float)(h1 & 0xFFFF) / 65535.0f;
			const float r2 = (float)(h2 & 0xFFFF) / 65535.0f;
			const float r3 = (float)(h3 & 0xFFFF) / 65535.0f;
			const float period = 1.0f / std::max(0.05f, liq.BubbleRise * (0.6f + 0.8f * r2));
			const float u = liq.Time / period + r3;
			const float ph = u - std::floor(u);
			int xh = liq.Col0 + (int)(r1 * (float)(liq.Col1 - liq.Col0 - 1));
			xh = std::clamp(xh, 0, (int)screenWidth - 1);
			const float sY = liq.Surface[xh];
			float cy, rad, alpha;
			bool ring;
			if(ph < 0.9f) {
				cy = (bottom + (sY - bottom) * (ph / 0.9f)) * S;
				rad = liq.BubbleSize * S * 0.8f;
				alpha = 1.0f;
				ring = false;
			} else {
				const float e = (ph - 0.9f) / 0.1f;
				cy = sY * S;
				rad = liq.BubbleSize * S * (1.0f + 1.5f * e);
				alpha = 1.0f - e;
				ring = true;
			}
			const float cx = (float)xh;
			const int x0 = std::max(0, (int)std::floor(cx - rad - 1.0f)), x1 = std::min((int)screenWidth - 1, (int)std::ceil(cx + rad + 1.0f));
			const int y0 = std::max(0, (int)std::floor(cy - rad - 1.0f)), y1 = std::min(hdRows - 1, (int)std::ceil(cy + rad + 1.0f));
			for(int y = y0; y <= y1; y++) {
				for(int x = x0; x <= x1; x++) {
					const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
					float v = 0.0f;
					if(!ring) {
						if(d <= rad) v = 0.55f * alpha;
					} else {
						const float e2 = std::fabs(d - rad);
						if(e2 <= 1.0f) v = 0.9f * alpha * (1.0f - e2);
					}
					if(v <= 0.0f) continue;
					const uint8_t q = (uint8_t)(v * 255.0f + 0.5f);
					uint8_t& m = liq.BubbleMap[(size_t)y * screenWidth + x];
					if(q > m) m = q;
				}
			}
		}
	}
}

// view=top : degrade par profondeur dans la zone + ondulations croisees (coordonnees NES)
static uint32_t LiquidTopPixel(const NesHdLiquidInfo& liq, float xN, float yN, uint32_t dst, float maskK = 1.0f, float offX = 0.0f, float offY = 0.0f)
{
	// followx / followy : vagues et texture en coordonnees de la carte (ecran + defilement suivi) ; le degrade reste en coordonnees d'ecran
	const float xW = xN + offX, yW = yN + offY;
	auto chan = [](uint32_t c, int sh) -> float { return (float)((c >> sh) & 0xFF); };
	const float zoneH = std::max(1.0f, liq.YMax - liq.YMin);
	const float depth = std::clamp((yN - liq.YMin) / zoneH, 0.0f, 1.0f);
	float rr = chan(liq.Color, 16) + (chan(liq.Deep, 16) - chan(liq.Color, 16)) * depth;
	float gg = chan(liq.Color, 8) + (chan(liq.Deep, 8) - chan(liq.Color, 8)) * depth;
	float bb = chan(liq.Color, 0) + (chan(liq.Deep, 0) - chan(liq.Color, 0)) * depth;
	const float twoPi = 2.0f * OverlayPi;
	if(liq.NormalMode) {
		// [normal=1] vaguelettes : hauteur a 3 trains d'ondes, normale par pixel, refraction de la texture, eclairage et reflet
		const float twoPiN = 2.0f * OverlayPi;
		const float p1 = twoPiN * (xW / liq.Wave + liq.Speed * liq.Time) + liq.Phase * (OverlayPi / 180.0f);
		const float p2 = twoPiN * (yW / (liq.Wave * 0.6f) - 0.7f * liq.Speed * liq.Time);
		const float p3 = twoPiN * ((xW + yW) / (liq.Wave * 1.7f) + 1.3f * liq.Speed * liq.Time);
		const float k1 = twoPiN / liq.Wave, k2 = twoPiN / (liq.Wave * 0.6f), k3 = twoPiN / (liq.Wave * 1.7f);
		const float slope = 1.25f * std::max(0.01f, liq.Amp);
		const float hn = 0.5f * (std::sin(p1) + 0.6f * std::sin(p2) + 0.4f * std::sin(p3));
		const float dhdx = slope * (k1 * std::cos(p1) + 0.4f * k3 * std::cos(p3));
		const float dhdy = slope * (0.6f * k2 * std::cos(p2) + 0.4f * k3 * std::cos(p3));
		float nx = -dhdx, ny = -dhdy, nz = 1.0f;
		const float nlen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
		nx *= nlen; ny *= nlen; nz *= nlen;
		const bool hasTexW = liq.Texture != nullptr && liq.Texture->Width > 0 && liq.Texture->Height > 0 && liq.Texture->PixelData.size() >= (size_t)liq.Texture->Width * liq.Texture->Height;
		if(hasTexW) {
			const int tw = (int)liq.Texture->Width, th = (int)liq.Texture->Height;
			const float u = (xW + dhdx * liq.Refract + liq.ScrollX * liq.Time) / liq.TexScale;
			const float v = (yW - liq.YMin + dhdy * liq.Refract) / liq.TexScale;
			int tx = (int)std::floor(u) % tw; if(tx < 0) tx += tw;
			int ty = (int)std::floor(v) % th; if(ty < 0) ty += th;
			const uint32_t c = liq.Texture->PixelData[(size_t)ty * tw + tx];
			const float a = chan(c, 24) / 255.0f;
			rr = chan(c, 16) + rr * (1.0f - a);
			gg = chan(c, 8) + gg * (1.0f - a);
			bb = chan(c, 0) + bb * (1.0f - a);
		}
		// eclairage : lumiere venant du haut-gauche
		const float ll = 1.0f / std::sqrt(0.45f * 0.45f + 0.55f * 0.55f + 0.70f * 0.70f);
		const float Lx = -0.45f * ll, Ly = -0.55f * ll, Lz = 0.70f * ll;
		const float diff = std::max(0.0f, nx * Lx + ny * Ly + nz * Lz);
		const float shade = 0.80f + 0.30f * diff;
		rr = std::min(255.0f, rr * shade); gg = std::min(255.0f, gg * shade); bb = std::min(255.0f, bb * shade);
		// reflet speculaire (Blinn)
		const float hx = Lx, hy = Ly, hz = Lz + 1.0f;
		const float hl = 1.0f / std::sqrt(hx * hx + hy * hy + hz * hz);
		const float ndh = std::max(0.0f, (nx * hx + ny * hy + nz * hz) * hl);
		const float spec = std::pow(ndh, 40.0f) * liq.Specular;
		rr += (255.0f - rr) * spec; gg += (255.0f - gg) * spec; bb += (255.0f - bb) * spec;
		// ecume sur les cretes
		const float crest = std::clamp((hn - 0.55f) / 0.35f, 0.0f, 1.0f) * liq.Foam * 0.6f;
		rr += (255.0f - rr) * crest; gg += (255.0f - gg) * crest; bb += (255.0f - bb) * crest;
	} else {
	const float w1 = std::sin(twoPi * (xW / liq.Wave + liq.Speed * liq.Time) + liq.Phase * (OverlayPi / 180.0f));
	const float w2 = std::sin(twoPi * (yW / (liq.Wave * 0.6f) - 0.7f * liq.Speed * liq.Time));
	const float rip = 0.5f * (w1 + w2) * std::min(1.0f, liq.Amp / 6.0f);
	if(rip >= 0.0f) {
		rr += (255.0f - rr) * rip * liq.Foam;
		gg += (255.0f - gg) * rip * liq.Foam;
		bb += (255.0f - bb) * rip * liq.Foam;
	} else {
		const float dk = 1.0f + rip * 0.3f;
		rr *= dk; gg *= dk; bb *= dk;
	}
	const bool hasTex = liq.Texture != nullptr && liq.Texture->Width > 0 && liq.Texture->Height > 0 && liq.Texture->PixelData.size() >= (size_t)liq.Texture->Width * liq.Texture->Height;
	if(hasTex) {
		const int tw = (int)liq.Texture->Width, th = (int)liq.Texture->Height;
		const float u = (xW + liq.ScrollX * liq.Time) / liq.TexScale;
		const float v = (yW - liq.YMin) / liq.TexScale;
		int tx = (int)std::floor(u) % tw; if(tx < 0) tx += tw;
		int ty = (int)std::floor(v) % th; if(ty < 0) ty += th;
		const uint32_t c = liq.Texture->PixelData[(size_t)ty * tw + tx];
		const float a = chan(c, 24) / 255.0f;
		rr = chan(c, 16) + rr * (1.0f - a);
		gg = chan(c, 8) + gg * (1.0f - a);
		bb = chan(c, 0) + bb * (1.0f - a);
	}
	}
	const float A = liq.Alpha * maskK;
	const float dr = chan(dst, 16), dg = chan(dst, 8), db = chan(dst, 0);
	const uint32_t R = (uint32_t)std::clamp(dr + (rr - dr) * A + 0.5f, 0.0f, 255.0f);
	const uint32_t G = (uint32_t)std::clamp(dg + (gg - dg) * A + 0.5f, 0.0f, 255.0f);
	const uint32_t B = (uint32_t)std::clamp(db + (bb - db) * A + 0.5f, 0.0f, 255.0f);
	return 0xFF000000u | (R << 16) | (G << 8) | B;
}

// Halo additif au-dessus de la surface ; d = distance normalisee (0 = surface, 1 = bord du halo)
static uint32_t LiquidGlowPixel(const NesHdLiquidInfo& liq, float d, uint32_t dst)
{
	auto chan = [](uint32_t c, int sh) -> float { return (float)((c >> sh) & 0xFF); };
	const float g = liq.Glow * (1.0f - d) * (1.0f - d);
	const float dr = chan(dst, 16), dg = chan(dst, 8), db = chan(dst, 0);
	const float gr = chan(liq.Color, 16) * 0.5f + 127.5f, gg = chan(liq.Color, 8) * 0.5f + 127.5f, gb = chan(liq.Color, 0) * 0.5f + 127.5f;
	const uint32_t R = (uint32_t)std::clamp(dr + gr * g + 0.5f, 0.0f, 255.0f);
	const uint32_t G = (uint32_t)std::clamp(dg + gg * g + 0.5f, 0.0f, 255.0f);
	const uint32_t B = (uint32_t)std::clamp(db + gb * g + 0.5f, 0.0f, 255.0f);
	return 0xFF000000u | (R << 16) | (G << 8) | B;
}

// Bulle : melange vers le blanc selon l'alpha de la carte de bulles (0 a 255)
static uint32_t LiquidBubblePixel(uint32_t dst, uint8_t a)
{
	const float f = (float)a / 255.0f;
	const float dr = (float)((dst >> 16) & 0xFF), dg = (float)((dst >> 8) & 0xFF), db = (float)(dst & 0xFF);
	const uint32_t R = (uint32_t)std::clamp(dr + (255.0f - dr) * f + 0.5f, 0.0f, 255.0f);
	const uint32_t G = (uint32_t)std::clamp(dg + (255.0f - dg) * f + 0.5f, 0.0f, 255.0f);
	const uint32_t B = (uint32_t)std::clamp(db + (255.0f - db) * f + 0.5f, 0.0f, 255.0f);
	return 0xFF000000u | (R << 16) | (G << 8) | B;
}

// Dessine le liquide dans le bloc HD d'un pixel NES, pour les liquides dont la bande est "band"
template<uint32_t scale>
void HdNesPack<scale>::DrawLiquidBand(int band, uint32_t nesX, uint32_t nesY, uint32_t* block, uint32_t screenWidth)
{
	if(_hdData->Liquids.empty()) {
		return;
	}
	// Pixel net (HUD) : pas de liquide
	if(_hdData->HasIgnore2D && _ignNes[((size_t)nesY << 8) | nesX] != 0) {
		return;
	}
	const int xRel = (int)nesX - (int)_liquidOffX;
	const int yRel = (int)nesY - (int)_liquidOffY;
	if(xRel < 0 || yRel < 0) {
		return;
	}
	for(auto& liqPtr : _hdData->Liquids) {
		const NesHdLiquidInfo& liq = *liqPtr;
		if(!liq.Active || liq.Priority / (int)HdNesPack<scale>::PriorityLevelsPerLayer != band) {
			continue;
		}
		for(int i = 0; i < (int)scale; i++) {
			const float yN = (float)yRel + (float)i / (float)scale;
			const int hdRow = yRel * (int)scale + i;
			uint32_t* row = block + (size_t)i * screenWidth;
			for(int j = 0; j < (int)scale; j++) {
				const int col = xRel * (int)scale + j;
				if(col < liq.Col0 || col >= liq.Col1 || col >= (int)screenWidth) {
					continue;
				}
				const float xN = (float)col / (float)scale;
				const uint32_t dst = row[j];
				uint32_t px = dst;
				if(liq.IsTop) {
					if(yN >= liq.YMin && yN < liq.YMax) {
						float maskK = 1.0f;
						bool covered = false;
						if(liq.MaskBg && !_liqCover[liq.Priority].empty()) {
							const size_t ci = (size_t)hdRow * screenWidth + (size_t)col;
							const uint8_t cv = ci < _liqCover[liq.Priority].size() ? _liqCover[liq.Priority][ci] : 0;
							if(cv == 255) covered = true; else maskK = (255.0f - (float)cv) / 255.0f;
						}
						if(!covered) px = LiquidTopPixel(liq, xN, yN, dst, maskK, liq.FollowX * liq.FollowAccX, liq.FollowY * liq.FollowAccY);
					}
				} else {
					const float s = liq.Surface[col];
					if(yN < s) {
						if(liq.Glow > 0.0f) {
							const float d = (s - yN) / std::max(1.0f, liq.GlowRadius);
							if(d < 1.0f) {
								px = LiquidGlowPixel(liq, d, dst);
							}
						}
					} else {
						px = BlendLiquidPixel(liq, xN, yN, s, dst);
					}
					if(!liq.BubbleMap.empty() && hdRow >= 0) {
						const size_t bi = (size_t)hdRow * screenWidth + (size_t)col;
						if(bi < liq.BubbleMap.size() && liq.BubbleMap[bi] != 0) {
							px = LiquidBubblePixel(px, liq.BubbleMap[bi]);
						}
					}
				}
				row[j] = px;
			}
		}
	}
}

template<uint32_t scale>
uint32_t HdNesPack<scale>::BlendLiquidPixel(const NesHdLiquidInfo& liq, float xN, float yN, float s, uint32_t dst)
{
	auto chan = [](uint32_t c, int sh) -> float { return (float)((c >> sh) & 0xFF); };

	// Couleur de base : de la couleur de surface vers la couleur profonde
	const float depth = std::clamp((yN - s) / liq.Depth, 0.0f, 1.0f);
	float rr = chan(liq.Color, 16) + (chan(liq.Deep, 16) - chan(liq.Color, 16)) * depth;
	float gg = chan(liq.Color, 8) + (chan(liq.Deep, 8) - chan(liq.Color, 8)) * depth;
	float bb = chan(liq.Color, 0) + (chan(liq.Deep, 0) - chan(liq.Color, 0)) * depth;

	// Texture : repetee horizontalement, defilante, ancree a la surface (PNG premultiplie)
	const bool hasTex = liq.Texture != nullptr && liq.Texture->Width > 0 && liq.Texture->Height > 0 && liq.Texture->PixelData.size() >= (size_t)liq.Texture->Width * liq.Texture->Height;
	if(hasTex) {
		const int tw = (int)liq.Texture->Width, th = (int)liq.Texture->Height;
		const float u = (xN + liq.ScrollX * liq.Time) / liq.TexScale;
		const float v = (yN - s) / liq.TexScale;
		int tx = (int)std::floor(u) % tw; if(tx < 0) tx += tw;
		int ty = (int)std::floor(v) % th; if(ty < 0) ty += th;
		const uint32_t c = liq.Texture->PixelData[(size_t)ty * tw + tx];
		const float a = chan(c, 24) / 255.0f;
		rr = chan(c, 16) + rr * (1.0f - a);
		gg = chan(c, 8) + gg * (1.0f - a);
		bb = chan(c, 0) + bb * (1.0f - a);
	}

	// Ecume : liseré clair sur la premiere ligne NES sous la surface
	if(yN - s < 1.0f) {
		rr += (255.0f - rr) * liq.Foam;
		gg += (255.0f - gg) * liq.Foam;
		bb += (255.0f - bb) * liq.Foam;
	}

	// Opacite globale : melange avec ce qui est deja dessine
	const float A = liq.Alpha;
	const float dr = chan(dst, 16), dg = chan(dst, 8), db = chan(dst, 0);
	const uint32_t R = (uint32_t)std::clamp(dr + (rr - dr) * A + 0.5f, 0.0f, 255.0f);
	const uint32_t G = (uint32_t)std::clamp(dg + (gg - dg) * A + 0.5f, 0.0f, 255.0f);
	const uint32_t B = (uint32_t)std::clamp(db + (bb - db) * A + 0.5f, 0.0f, 255.0f);
	return 0xFF000000u | (R << 16) | (G << 8) | B;
}

// <2DPerspective> (NES) : sol incline, ancre sur le scroll PPU par ligne. Passe finale (apres les distorsions, avant les videos).
// out : buffer de sortie (stride = largeur HD), padHd / centreHd : zone NES en widescreen (bandes hors zone).
// Centre : z NES (_zNes). Bandes widescreen : z des bandes (_wsZ). Meme test z que les overlays.
template<uint32_t scale>
void HdNesPack<scale>::ProcessPerspectivesLayout(uint32_t* out, uint32_t stride, uint32_t padHd, uint32_t centreHd, float originNes, uint32_t hdHeight, float topNes, float bottomNes, bool ws)
{
	if(out == nullptr || stride == 0 || hdHeight == 0) {
		return;
	}
	const float S = (float)scale;
	const float cx = 128.0f; // centre horizontal (pixels NES absolus, identique en 4:3 et en widescreen)
	const int hdRows = (int)hdHeight;

	for(auto& ppPtr : _hdData->Perspectives) {
		NesHdPerspectiveInfo& pp = *ppPtr;

		// Ancrage par ligne (meme regle que <2DOverlay> : saut > 64 ignore). Continue meme quand l'effet est inactif.
		if(pp.OffX.size() != 240) {
			pp.OffX.assign(240, 0.0f); pp.OffY.assign(240, 0.0f);
			pp.LastX.assign(240, 0); pp.LastY.assign(240, 0);
			pp.HaveScroll = false;
		}
		for(int L = 0; L < 240; L++) {
			const int sx = _lineScrollX[L], sy = _lineScrollY[L];
			if(pp.HaveScroll) {
				const int dx = ((sx - pp.LastX[L]) % 512 + 512 + 256) % 512 - 256; // scroll X sur 9 bits
				const int dy = ((sy - pp.LastY[L]) % 480 + 480 + 240) % 480 - 240; // scroll Y sur 480 lignes
				if(std::abs(dx) <= 64) pp.OffX[L] += (float)dx * pp.SpeedX;
				if(std::abs(dy) <= 64) pp.OffY[L] += (float)dy * pp.SpeedY;
				pp.OffX[L] = std::fmod(pp.OffX[L], 512.0f);
				pp.OffY[L] = std::fmod(pp.OffY[L], 480.0f);
			}
			pp.LastX[L] = sx;
			pp.LastY[L] = sy;
		}
		pp.HaveScroll = true;

		pp.Active = pp.MatchesCondition();
		if(!pp.Active) {
			continue;
		}
		const HdPackBitmapInfo* tex = pp.Texture;
		if(!tex || tex->Width == 0 || tex->Height == 0 || tex->PixelData.size() < (size_t)tex->Width * tex->Height) {
			continue;
		}
		const int texW = (int)tex->Width, texH = (int)tex->Height;
		if(pp.Wall) {
			// paroi verticale (side=left|right) : profondeur horizontale vers le bord, hauteur centree sur cy
			const bool right = pp.WallRight;
			const float xv = pp.HorizonSet ? pp.Horizon : cx;                  // colonne de fuite (NES absolue)
			const float cyN = pp.CySet ? pp.Cy : (topNes + bottomNes) * 0.5f;   // centre vertical (NES absolue)
			const float limitX = right ? xv + pp.LimitOffset : xv - pp.LimitOffset;
			const float farX = right ? originNes + (float)stride / S : originNes; // bord lointain de l'ecran
			float sScale = 1.0f;
			if(pp.Repeat > 0.0f) {
				const float kFar = pp.Depth / std::max(1.0f, std::fabs(farX - xv));
				const float kLimit = pp.Depth / std::max(1.0f, std::fabs(limitX - xv));
				if(kLimit > kFar) sScale = pp.Repeat * (float)texW / (kLimit - kFar);
			}
			const int cyLine = std::clamp((int)cyN, 0, 239);
			const float offD = pp.OffX[cyLine]; // defilement le long du mur (texels)
			const float offL = pp.OffY[cyLine]; // defilement vertical (texels)
			const float fogR = (float)((pp.Fog >> 16) & 0xFF);
			const float fogG = (float)((pp.Fog >> 8) & 0xFF);
			const float fogB = (float)(pp.Fog & 0xFF);
			auto packW = [](float r, float g, float b) -> uint32_t {
				const uint32_t R = (uint32_t)std::clamp(r + 0.5f, 0.0f, 255.0f);
				const uint32_t G = (uint32_t)std::clamp(g + 0.5f, 0.0f, 255.0f);
				const uint32_t B = (uint32_t)std::clamp(b + 0.5f, 0.0f, 255.0f);
				return 0xFF000000u | (R << 16) | (G << 8) | B;
			};
			const int nT = std::max(1, std::min(_pool.Size(), hdRows));
			const int bandRows = (hdRows + nT - 1) / nT;
			_pool.Run(nT, [&](int t) {
				const int r0 = t * bandRows;
				const int r1 = std::min(hdRows, r0 + bandRows);
				for(int hy = r0; hy < r1; hy++) {
					const float yN = topNes + ((float)hy + 0.5f) / S; // ligne NES absolue
					const float lat = yN - cyN;
					const int rowLine = std::clamp((int)yN, 0, 239);
					uint32_t* row = out + (size_t)hy * stride;
					for(uint32_t hx = 0; hx < stride; hx++) {
						const float xN = originNes + ((float)hx + 0.5f) / S;
						const int nx = std::clamp((int)xN, 0, 255);
						const bool inCentre = !ws || (hx >= padHd && hx < padHd + centreHd);
						uint8_t& zRef = inCentre ? _zNes[((size_t)rowLine << 8) | (size_t)nx] : _wsZ[(size_t)hy * stride + hx];
						if(pp.Priority < zRef) continue; // z-test : cache par un fond de priorite superieure
						if(right ? xN < limitX : xN > limitX) continue;
						const float dx = right ? xN - xv : xv - xN; // distance a la colonne de fuite
						if(dx <= 0.0f) continue;
						const float kL = pp.Depth / dx;  // facteur de profondeur : hauteur
						const float kD = kL * sScale;    // profondeur : x echelle repeat
						int tx = (int)std::floor(kD + offD);
						if(pp.TileX) { tx %= texW; if(tx < 0) tx += texW; }
						else if(tx < 0 || tx >= texW) continue;
						int ty = (int)std::floor(lat * kL + offL);
						if(pp.TileY) { ty %= texH; if(ty < 0) ty += texH; }
						else if(ty < 0 || ty >= texH) continue;
						const uint32_t c = tex->PixelData[(size_t)ty * texW + tx];
						const float a = ((float)((c >> 24) & 0xFF) / 255.0f) * pp.Alpha;
						if(a <= 0.0f) continue;
						float fogF = 0.0f;
						if(pp.FogRange > 0.0f) fogF = std::clamp(1.0f - std::fabs(xN - limitX) / pp.FogRange, 0.0f, 1.0f);
						float sr = (float)((c >> 16) & 0xFF), sg = (float)((c >> 8) & 0xFF), sb = (float)(c & 0xFF);
						if(fogF > 0.0f) {
							sr += (fogR - sr) * fogF;
							sg += (fogG - sg) * fogF;
							sb += (fogB - sb) * fogF;
						}
						const uint32_t dst = row[hx];
						const float dr = (float)((dst >> 16) & 0xFF), dg = (float)((dst >> 8) & 0xFF), db = (float)(dst & 0xFF);
						row[hx] = packW(dr + (sr - dr) * a, dg + (sg - dg) * a, db + (sb - db) * a);
						if(a >= 0.5f) zRef = (uint8_t)pp.Priority;
					}
				}
			});
			continue;
		}

		const bool ceil = pp.Ceiling;
		// sol : limite = Horizon + N, la profondeur croit vers le bas. plafond : limite = Horizon - N, la profondeur croit vers le haut
		const float limitY = ceil ? pp.Horizon - pp.LimitOffset : pp.Horizon + pp.LimitOffset;
		const float farNes = ceil ? topNes : bottomNes; // bord lointain de l'ecran, en lignes NES

		// repeat=N : echelle en profondeur seulement (la largeur garde la echelle de depth)
		float sScale = 1.0f;
		if(pp.Repeat > 0.0f) {
			const float kFar = pp.Depth / std::max(1.0f, ceil ? pp.Horizon - farNes : farNes - pp.Horizon);
			const float kLimit = pp.Depth / std::max(1.0f, std::fabs(limitY - pp.Horizon));
			if(kLimit > kFar) sScale = pp.Repeat * (float)texH / (kLimit - kFar);
		}

		const float fogR = (float)((pp.Fog >> 16) & 0xFF);
		const float fogG = (float)((pp.Fog >> 8) & 0xFF);
		const float fogB = (float)(pp.Fog & 0xFF);
		auto pack = [](float r, float g, float b) -> uint32_t {
			const uint32_t R = (uint32_t)std::clamp(r + 0.5f, 0.0f, 255.0f);
			const uint32_t G = (uint32_t)std::clamp(g + 0.5f, 0.0f, 255.0f);
			const uint32_t B = (uint32_t)std::clamp(b + 0.5f, 0.0f, 255.0f);
			return 0xFF000000u | (R << 16) | (G << 8) | B;
		};

		const int nT = std::max(1, std::min(_pool.Size(), hdRows));
		const int bandRows = (hdRows + nT - 1) / nT;
		_pool.Run(nT, [&](int t) {
			const int r0 = t * bandRows;
			const int r1 = std::min(hdRows, r0 + bandRows);
			for(int hy = r0; hy < r1; hy++) {
				const float yN = topNes + ((float)hy + 0.5f) / S; // ligne NES absolue
				if(ceil ? yN > limitY : yN < limitY) continue;
				const float dy = ceil ? pp.Horizon - yN : yN - pp.Horizon;
				if(dy <= 0.0f) continue;
				const int line = std::clamp((int)yN, 0, 239);
				const float kX = pp.Depth / dy;
				const float k = kX * sScale;
				const float offU = pp.OffX[line];
				const float offV = pp.OffY[line];
				float fogF = 0.0f;
				if(pp.FogRange > 0.0f) fogF = std::clamp(1.0f - std::fabs(yN - limitY) / pp.FogRange, 0.0f, 1.0f);
				int ty = (int)std::floor(k + offV);
				if(pp.TileY) { ty %= texH; if(ty < 0) ty += texH; }
				else if(ty < 0 || ty >= texH) continue;
				uint32_t* row = out + (size_t)hy * stride;
				for(uint32_t hx = 0; hx < stride; hx++) {
					const float xN = originNes + ((float)hx + 0.5f) / S;
					const int nx = std::clamp((int)xN, 0, 255);
					const bool inCentre = !ws || (hx >= padHd && hx < padHd + centreHd);
					uint8_t& zRef = inCentre ? _zNes[((size_t)line << 8) | (size_t)nx] : _wsZ[(size_t)hy * stride + hx];
					if(pp.Priority < zRef) continue; // z-test : cache par un fond de priorite superieure
					int tx = (int)std::floor((xN - cx) * kX + offU);
					if(pp.TileX) { tx %= texW; if(tx < 0) tx += texW; }
					else if(tx < 0 || tx >= texW) continue;
					const uint32_t c = tex->PixelData[(size_t)ty * texW + tx];
					const float a = ((float)((c >> 24) & 0xFF) / 255.0f) * pp.Alpha;
					if(a <= 0.0f) continue;
					float sr = (float)((c >> 16) & 0xFF), sg = (float)((c >> 8) & 0xFF), sb = (float)(c & 0xFF);
					if(fogF > 0.0f) {
						sr += (fogR - sr) * fogF;
						sg += (fogG - sg) * fogF;
						sb += (fogB - sb) * fogF;
					}
					const uint32_t dst = row[hx];
					const float dr = (float)((dst >> 16) & 0xFF), dg = (float)((dst >> 8) & 0xFF), db = (float)(dst & 0xFF);
					row[hx] = pack(dr + (sr - dr) * a, dg + (sg - dg) * a, db + (sb - db) * a);
					if(a >= 0.5f) zRef = (uint8_t)pp.Priority;
				}
			}
		});
	}
}

// 4:3 : le cadre visible est le buffer de sortie
template<uint32_t scale>
void HdNesPack<scale>::ProcessPerspectives(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan)
{
	const uint32_t hdHeight = (240 - overscan.Top - overscan.Bottom) * scale;
	ProcessPerspectivesLayout(outputBuffer, screenWidth, 0, screenWidth, (float)overscan.Left, hdHeight, (float)overscan.Top, (float)(240 - overscan.Bottom), false);
}

// Widescreen : meme mise en page que les overlays (bandes gauche/droite avec padHd)
template<uint32_t scale>
void HdNesPack<scale>::ProcessWidescreenPerspectives(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom)
{
	if(!_zEnabled || out == nullptr || outWidth == 0 || _hdData->Perspectives.empty()) {
		return;
	}
	const float ws = (float)padHd / (float)scale;
	const uint32_t hdHeight = (240 - overscanTop - overscanBottom) * scale;
	ProcessPerspectivesLayout(out, outWidth, padHd, outWidth - 2 * padHd, (float)overscanLeft - ws, hdHeight, (float)overscanTop, (float)(240 - overscanBottom), true);
}

template<uint32_t scale>
void HdNesPack<scale>::ProcessDistortions(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan)
{
	const int hdHeight = (int)((240 - overscan.Top - overscan.Bottom) * scale);
	if(screenWidth == 0 || hdHeight <= 0) {
		return;
	}
	const float S = (float)scale; // pixels HD par pixel NES
	const float twoPi = 2.0f * OverlayPi;
	const float degToRad = OverlayPi / 180.0f;
	const int xs = (int)overscan.Left, ys = (int)overscan.Top;
	// Pixel net (HUD) : lecture de l'indicateur au pixel NES correspondant au pixel HD
	auto ignAt = [&](int hx, int hy) -> bool {
		return _ignNes[((size_t)(ys + hy / (int)scale) << 8) | (size_t)(xs + hx / (int)scale)] != 0;
	};
	const bool ignOn = _hdData->HasIgnore2D;
	auto lerpArgb = [](uint32_t c0, uint32_t c1, float f) -> uint32_t {
		uint32_t res = 0xFF000000;
		for(int sh = 0; sh <= 16; sh += 8) {
			const float v0 = (float)((c0 >> sh) & 0xFF), v1 = (float)((c1 >> sh) & 0xFF);
			res |= ((uint32_t)std::min(255.0f, v0 + (v1 - v0) * f + 0.5f)) << sh;
		}
		return res;
	};

	for(auto& ovPtr : _hdData->Overlays) {
		const NesHdOverlayInfo& ov = *ovPtr;
		if(!ov.IsDistortion || !ov.Active) {
			continue;
		}
		const int zy0 = (int)std::clamp(ov.ZoneY0 * (float)hdHeight, 0.0f, (float)hdHeight);
		const int zy1 = (int)std::clamp(ov.ZoneY1 * (float)hdHeight, 0.0f, (float)hdHeight);
		const int zx0 = (int)std::clamp(ov.ZoneX0 * (float)screenWidth, 0.0f, (float)screenWidth);
		const int zx1 = (int)std::clamp(ov.ZoneX1 * (float)screenWidth, 0.0f, (float)screenWidth);
		if(zx0 >= zx1 || zy0 >= zy1) {
			continue;
		}

		// Parallele par lignes : chaque thread traite ses lignes avec son propre tampon
		const int nT = std::max(1, std::min(_pool.Size(), zy1 - zy0));
		_pool.Run(nT, [&](int t) {
			std::vector<uint32_t> rowCopy(screenWidth);
			for(int r = zy0 + t; r < zy1; r += nT) {
				const float yN = (float)r / S; // position en lignes NES
				float dx = ov.DistAmp1 * std::sin(twoPi * (yN / ov.DistWave1 + ov.DistSpeed1 * ov.Time) + ov.DistPhase1 * degToRad)
				         + ov.DistAmp2 * std::sin(twoPi * (yN / ov.DistWave2 + ov.DistSpeed2 * ov.Time) + ov.DistPhase2 * degToRad);
				dx *= S * ov.Alpha; // Alpha = force globale de la distorsion
				uint32_t* row = outputBuffer + (size_t)r * screenWidth;
				for(int x = zx0; x < zx1; x++) {
					rowCopy[x] = row[x];
				}
				for(int x = zx0; x < zx1; x++) {
					if(ignOn && ignAt(x, r)) continue; // HUD : reste net
					const float sx = std::clamp((float)x - dx, (float)zx0, (float)(zx1 - 1));
					const int ix = (int)sx;
					const int ix1 = std::min(ix + 1, zx1 - 1);
					if(ignOn && (ignAt(ix, r) || ignAt(ix1, r))) continue; // source HUD : pas de fantome
					row[x] = lerpArgb(rowCopy[ix], rowCopy[ix1], sx - (float)ix);
				}
			}
		});
	}
}

// Widescreen (voie 1) : distorsions sur tout le buffer de sortie (bandes incluses), apres DrawWidescreenBands.
// Zones ZoneX0/ZoneX1 en fraction de la largeur elargie. Le HUD (_ignNes) n'existe que dans le centre : hors centre, pas de masque.
template<uint32_t scale>
void HdNesPack<scale>::ProcessWidescreenDistortions(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom)
{
	const int hdHeight = (int)((240 - overscanTop - overscanBottom) * scale);
	if(out == nullptr || outWidth == 0 || hdHeight <= 0 || _hdData->Overlays.empty()) {
		return;
	}
	const float S = (float)scale;
	const float twoPi = 2.0f * OverlayPi;
	const float degToRad = OverlayPi / 180.0f;
	const int xs = (int)overscanLeft, ys = (int)overscanTop;
	const int visHdW = (int)outWidth - 2 * (int)padHd;
	const bool ignOn = _hdData->HasIgnore2D;
	// Pixel net (HUD) : seulement dans le centre ; hors centre, pas de masque
	auto ignAt = [&](int x, int r) -> bool {
		const int rel = x - (int)padHd;
		if(rel < 0 || rel >= visHdW) return false;
		return _ignNes[((size_t)(ys + r / (int)scale) << 8) | (size_t)(xs + rel / (int)scale)] != 0;
	};
	auto lerpArgb = [](uint32_t c0, uint32_t c1, float f) -> uint32_t {
		uint32_t res = 0xFF000000;
		for(int sh = 0; sh <= 16; sh += 8) {
			const float v0 = (float)((c0 >> sh) & 0xFF), v1 = (float)((c1 >> sh) & 0xFF);
			res |= ((uint32_t)std::min(255.0f, v0 + (v1 - v0) * f + 0.5f)) << sh;
		}
		return res;
	};

	for(auto& ovPtr : _hdData->Overlays) {
		const NesHdOverlayInfo& ov = *ovPtr;
		if(!ov.IsDistortion || !ov.Active) {
			continue;
		}
		const int zy0 = (int)std::clamp(ov.ZoneY0 * (float)hdHeight, 0.0f, (float)hdHeight);
		const int zy1 = (int)std::clamp(ov.ZoneY1 * (float)hdHeight, 0.0f, (float)hdHeight);
		const int zx0 = (int)std::clamp(ov.ZoneX0 * (float)outWidth, 0.0f, (float)outWidth);
		const int zx1 = (int)std::clamp(ov.ZoneX1 * (float)outWidth, 0.0f, (float)outWidth);
		if(zx0 >= zx1 || zy0 >= zy1) {
			continue;
		}
		const int nT = std::max(1, std::min(_pool.Size(), zy1 - zy0));
		_pool.Run(nT, [&](int t) {
			std::vector<uint32_t> rowCopy(outWidth);
			for(int r = zy0 + t; r < zy1; r += nT) {
				const float yN = (float)r / S;
				float dx = ov.DistAmp1 * std::sin(twoPi * (yN / ov.DistWave1 + ov.DistSpeed1 * ov.Time) + ov.DistPhase1 * degToRad)
				         + ov.DistAmp2 * std::sin(twoPi * (yN / ov.DistWave2 + ov.DistSpeed2 * ov.Time) + ov.DistPhase2 * degToRad);
				dx *= S * ov.Alpha;
				uint32_t* row = out + (size_t)r * outWidth;
				for(int x = zx0; x < zx1; x++) {
					rowCopy[x] = row[x];
				}
				for(int x = zx0; x < zx1; x++) {
					if(ignOn && ignAt(x, r)) continue;
					const float sx = std::clamp((float)x - dx, (float)zx0, (float)(zx1 - 1));
					const int ix = (int)sx;
					const int ix1 = std::min(ix + 1, zx1 - 1);
					if(ignOn && (ignAt(ix, r) || ignAt(ix1, r))) continue;
					row[x] = lerpArgb(rowCopy[ix], rowCopy[ix1], sx - (float)ix);
				}
			}
		});
	}
}

template<uint32_t scale>
void HdNesPack<scale>::ProcessOverlaysLayout(uint32_t* outputBuffer, uint32_t screenWidth, uint32_t padHd, uint32_t centreHd, float originNes, float widthNes, float fieldWNes, uint32_t oLeft, uint32_t oTop, uint32_t oBottom)
{
	const int hdHeight = (int)((240 - oTop - oBottom) * scale);
	if(screenWidth == 0 || hdHeight <= 0) {
		return;
	}
	const int iScale = (int)scale;
	const int xStart = (int)oLeft;
	const int yStart = (int)oTop;
	const float fScale = (float)scale;

	// Ecriture d'un pixel HD. src : couleur premultipliee (0-255). Test de z par pixel NES, comme les <video>.
	auto put = [&](const NesHdOverlayInfo& ov, int sx, int sy, float srcA, float sr, float sg, float sb) {
		// Centre : z NES (_zNes). Bandes widescreen : z des bandes (_wsZ). Meme test z dans les deux cas.
		uint8_t* zPtr;
		if(sx >= (int)padHd && sx < (int)(padHd + centreHd)) {
			zPtr = &_zNes[((size_t)(yStart + sy / iScale) << 8) | (size_t)(xStart + (sx - (int)padHd) / iScale)];
		} else {
			zPtr = &_wsZ[(size_t)sy * screenWidth + (size_t)sx];
		}
		if(ov.ZIndex < *zPtr) {
			return;
		}
		*zPtr = (uint8_t)ov.ZIndex;

		uint32_t& dst = outputBuffer[(size_t)sy * screenWidth + (size_t)sx];
		float dr = (float)((dst >> 16) & 0xFF), dg = (float)((dst >> 8) & 0xFF), db = (float)(dst & 0xFF);
		float r, g, b;
		if(ov.Additive) {
			r = dr + sr;
			g = dg + sg;
			b = db + sb;
		} else {
			float keep = 1.0f - srcA;
			r = sr + dr * keep;
			g = sg + dg * keep;
			b = sb + db * keep;
		}
		dst = 0xFF000000 | ((uint32_t)std::clamp(r, 0.0f, 255.0f) << 16) | ((uint32_t)std::clamp(g, 0.0f, 255.0f) << 8) | (uint32_t)std::clamp(b, 0.0f, 255.0f);
	};

	for(auto& ovPtr : _hdData->Overlays) {
		NesHdOverlayInfo& ov = *ovPtr;
		if(!ov.Active || ov.Particles.empty()) {
			continue;
		}

		const float fieldW = fieldWNes * ov.FieldW;
		const float fieldH = 240.0f * ov.FieldH;
		const bool hasTex = ov.Texture != nullptr && ov.Texture->Width > 0 && ov.Texture->Height > 0 && !ov.Texture->PixelData.empty();
		const float texW = hasTex ? (float)ov.Texture->Width : 0.0f;
		const float texH = hasTex ? (float)ov.Texture->Height : 0.0f;

		for(const NesHdParticle& p : ov.Particles) {
			const float t = std::clamp(p.Age / p.Life, 0.0f, 1.0f);
			float fade = 1.0f;
			if(ov.FadeIn > 0 && t < ov.FadeIn) {
				fade = t / ov.FadeIn;
			}
			if(ov.FadeOut > 0 && t > 1.0f - ov.FadeOut) {
				fade = std::min(fade, (1.0f - t) / ov.FadeOut);
			}
			const float opacity = std::clamp(LerpF(ov.AlphaStart, ov.AlphaEnd, t) * fade * ov.Alpha, 0.0f, 1.0f);
			if(opacity <= 0.0f) {
				continue;
			}
			const float sizeMul = std::max(0.01f, LerpF(ov.SizeStart, ov.SizeEnd, t) * p.Size);
			auto channel = [&](int shift) {
				return LerpF((float)((ov.ColorStart >> shift) & 0xFF), (float)((ov.ColorEnd >> shift) & 0xFF), t) / 255.0f;
			};
			const float tintR = channel(16), tintG = channel(8), tintB = channel(0);

			// Rayon en pixels NES, pour le repliement sur le champ (copies visibles)
			// stretch= : trainee dans le sens du mouvement (longueur = vitesse x stretch, en pixels HD), la tete reste sur la particule
			float sUx = 0.0f, sUy = 0.0f, sL = 0.0f;
			if(ov.Stretch > 0.0f) {
				const float vl = std::sqrt(p.EVX * p.EVX + p.EVY * p.EVY);
				sL = vl * ov.Stretch * fScale;
				if(sL > 0.5f) { sUx = p.EVX / vl; sUy = p.EVY / vl; } else sL = 0.0f;
			}
			const bool strk = sL > 0.0f;
			const float radiusNes = (hasTex ? std::max(texW, texH) * sizeMul * 0.5f / fScale : ov.DotSize * sizeMul * 0.5f) + sL / fScale;
			const float margin = radiusNes + 1.0f;
			const float px0 = WrapCoord(p.X - ov.OffsetX, fieldW);
			const float py0 = WrapCoord(p.Y - ov.OffsetY, fieldH);

			for(int mx = (int)std::ceil((originNes - margin - px0) / fieldW); ; mx++) {
				const float cx = px0 + mx * fieldW;
				if(cx > originNes + widthNes + margin) {
					break;
				}
				for(int my = (int)std::ceil((-margin - py0) / fieldH); ; my++) {
					const float cy = py0 + my * fieldH;
					if(cy > 240.0f + margin) {
						break;
					}

					// Centre en pixels HD de la sortie
					const float cxH = (cx - originNes) * fScale - sUx * sL * 0.5f;
					const float cyH = (cy - (float)yStart) * fScale - sUy * sL * 0.5f;

					if(hasTex) {
						const float halfExt = std::max(texW, texH) * sizeMul * 0.5f;
						const float reach = (halfExt + sL * 0.5f) * 1.5f; // marge pour la rotation / la trainee
						const int xa = std::max(0, (int)std::floor(cxH - reach));
						const int xb = std::min((int)screenWidth - 1, (int)std::ceil(cxH + reach));
						const int ya = std::max(0, (int)std::floor(cyH - reach));
						const int yb = std::min(hdHeight - 1, (int)std::ceil(cyH + reach));
						const float cosR = strk ? sUy : std::cos(p.Rot), sinR = strk ? -sUx : std::sin(p.Rot); // trainee : axe v de la texture = mouvement
						for(int sy = ya; sy <= yb; sy++) {
							for(int sx = xa; sx <= xb; sx++) {
								// Transformation inverse : pixel de sortie -> pixel de la texture
								const float dx = (float)sx + 0.5f - cxH, dy = (float)sy + 0.5f - cyH;
								const float lx = dx * cosR + dy * sinR;
								const float ly = -dx * sinR + dy * cosR;
								const float u = lx / sizeMul + texW * 0.5f;
								const float v = (strk ? ly * texH * sizeMul / (texH * sizeMul + sL) : ly) / sizeMul + texH * 0.5f;
								if(u < 0.0f || v < 0.0f || u >= texW || v >= texH) {
									continue;
								}
								const uint32_t texel = ov.Texture->PixelData[(size_t)(int)v * (size_t)texW + (size_t)(int)u];
								const float ta = (float)(texel >> 24) / 255.0f;
								if(ta <= 0.0f) {
									continue;
								}
								put(ov, sx, sy, ta * opacity,
									(float)((texel >> 16) & 0xFF) * tintR * opacity,
									(float)((texel >> 8) & 0xFF) * tintG * opacity,
									(float)(texel & 0xFF) * tintB * opacity);
							}
						}
					} else {
						// Point doux : alpha lineaire du centre vers le bord
						const float rHD = std::max(0.01f, ov.DotSize * sizeMul * 0.5f * fScale);
						const int xa = std::max(0, (int)std::floor(cxH - (rHD + sL * 0.5f)));
						const int xb = std::min((int)screenWidth - 1, (int)std::ceil(cxH + (rHD + sL * 0.5f)));
						const int ya = std::max(0, (int)std::floor(cyH - (rHD + sL * 0.5f)));
						const int yb = std::min(hdHeight - 1, (int)std::ceil(cyH + (rHD + sL * 0.5f)));
						for(int sy = ya; sy <= yb; sy++) {
							for(int sx = xa; sx <= xb; sx++) {
								const float dx = (float)sx + 0.5f - cxH, dy = (float)sy + 0.5f - cyH;
								float qx = dx, qy = dy;
								if(strk) { qx = dx * sUy - dy * sUx; qy = std::max(0.0f, std::fabs(dx * sUx + dy * sUy) - sL * 0.5f); } // capsule
								const float d = std::sqrt(qx * qx + qy * qy) / rHD;
								if(d >= 1.0f) {
									continue;
								}
								const float a = (1.0f - d) * opacity;
								put(ov, sx, sy, a, 255.0f * tintR * a, 255.0f * tintG * a, 255.0f * tintB * a);
							}
						}
					}
				}
			}
		}
	}
	// <DayNight> (doc 55), mode normal : teinte (multiplication) de l'image, apres les particules ; zindex= : seulement ce qui est a ce z ou en dessous.
	// En diorama, la teinte est appliquee dans la scene 3D (lumieres, ciel, brouillard) : rien ici.
	if(_dn.Active && !_dnDioApplied && (_dn.Col[0] < 0.999f || _dn.Col[1] < 0.999f || _dn.Col[2] < 0.999f)) {
		const int zMax = _hdData->DayNightRules[_dn.Def].Def.ZIndex;
		const uint32_t kr = (uint32_t)(_dn.Col[0] * 256.0f + 0.5f), kg = (uint32_t)(_dn.Col[1] * 256.0f + 0.5f), kb = (uint32_t)(_dn.Col[2] * 256.0f + 0.5f);
		for(int sy = 0; sy < hdHeight; sy++) {
			uint32_t* row = outputBuffer + (size_t)sy * screenWidth;
			for(int sx = 0; sx < (int)screenWidth; sx++) {
				if(zMax >= 0) {
					const uint8_t z = (sx >= (int)padHd && sx < (int)(padHd + centreHd))
						? _zNes[((size_t)(yStart + sy / iScale) << 8) | (size_t)(xStart + (sx - (int)padHd) / iScale)]
						: _wsZ[(size_t)sy * screenWidth + (size_t)sx];
					if((int)z > zMax) continue;
				}
				const uint32_t d = row[sx];
				row[sx] = (d & 0xFF000000u) | ((((d >> 16) & 0xFF) * kr >> 8) << 16) | ((((d >> 8) & 0xFF) * kg >> 8) << 8) | ((d & 0xFF) * kb >> 8);
			}
		}
	}
}

// <DayNight> (doc 55) : avance des cycles (une fois par image) ; la derniere regle dont la condition est vraie donne la teinte.
// time=always : le temps d'un cycle avance meme quand sa condition est fausse ; time=cond : seulement quand elle est vraie.
template<uint32_t scale>
void HdNesPack<scale>::UpdateDayNight(HdScreenInfo* hdScreenInfo)
{
	_dn.Active = false;
	const auto& rules = _hdData->DayNightRules;
	if(rules.empty() || !hdScreenInfo) return;
	if(_dnState.size() != rules.size()) _dnState.assign(rules.size(), HdDayNightState());
	const uint32_t frame = hdScreenInfo->FrameNumber;
	for(size_t i = 0; i < rules.size(); i++) {
		const auto& r = rules[i];
		bool ok = true;
		for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
		HdDayNightAdvance(r.Def, _dnState[i], frame, r.Def.TimeAlways || ok, [&](int s) {
			if(s < 0 || (size_t)s >= r.Until.size()) return true;
			for(HdPackCondition* c : r.Until[s]) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
			return true;
		});
		if(ok) HdDayNightOutput(r.Def, _dnState[i], (int)i, _dn);
	}
}

// Overlays : chemin non widescreen (inchange : meme champ 256 px, meme z NES).
template<uint32_t scale>
void HdNesPack<scale>::ProcessOverlays(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan)
{
	ProcessOverlaysLayout(outputBuffer, screenWidth, 0, screenWidth, (float)overscan.Left, (float)(256 - (int)overscan.Left - (int)overscan.Right), 256.0f, overscan.Left, overscan.Top, overscan.Bottom);
}

// Widescreen (voie 1) : overlays sur toute la largeur elargie (bandes incluses). Le champ suit la largeur elargie.
template<uint32_t scale>
void HdNesPack<scale>::ProcessWidescreenOverlays(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom)
{
	if(!_zEnabled || out == nullptr || outWidth == 0) {
		return;
	}
	const float ws = (float)padHd / (float)scale;
	const float widthNes = (float)outWidth / (float)scale;
	ProcessOverlaysLayout(out, outWidth, padHd, outWidth - 2 * padHd, (float)overscanLeft - ws, widthNes, widthNes, overscanLeft, overscanTop, overscanBottom);
}

template<uint32_t scale>
void HdNesPack<scale>::OnLineStart(HdPpuPixelInfo &lineFirstPixel)
{
	//Passe 1 (sequentielle) : etat du cache de tile pour la ligne
	_scrollX = ((lineFirstPixel.TmpVideoRamAddr & 0x1F) << 3) | lineFirstPixel.XScroll | ((lineFirstPixel.TmpVideoRamAddr & 0x400) ? 0x100 : 0);
	_useCachedTile = false;
}

template<uint32_t scale>
void HdNesPack<scale>::ComputeLineBgConfig(HdPpuPixelInfo &lineFirstPixel, uint8_t y, HdBgConfig* lineCfg)
{
	//Passe 2 (parallele) : position des <background> pour la ligne, dans un tableau local au thread.
	//Ne modifie aucun membre de la classe.
	int32_t lineScrollX = ((lineFirstPixel.TmpVideoRamAddr & 0x1F) << 3) | lineFirstPixel.XScroll | ((lineFirstPixel.TmpVideoRamAddr & 0x400) ? 0x100 : 0);
	int32_t lineScrollY = (((lineFirstPixel.TmpVideoRamAddr & 0x3E0) >> 2) | ((lineFirstPixel.TmpVideoRamAddr & 0x7000) >> 12)) + ((lineFirstPixel.TmpVideoRamAddr & 0x800) ? 240 : 0);
	
	for(int layer = 0; layer < 5; layer++) {
		for(int i = 0; i < _activeBgCount[layer]; i++) {
			int cfgIndex = layer * HdNesPack::PriorityLevelsPerLayer + i;
			HdBgConfig cfg = _bgConfig[cfgIndex];
			cfg.RawScrollX = lineScrollX;
			cfg.RawScrollY = lineScrollY;
			cfg.BgMinX = -1;
			cfg.BgMaxX = -1;
			if(cfg.BackgroundIndex >= 0) {
				HdBackgroundInfo& bgInfo = _hdData->BackgroundsByPriority[cfg.BgPriority][cfg.BackgroundIndex];

				// Operateur de defilement (comme SNES) : multiplication par defaut
				const float rx = bgInfo.HorizontalScrollRatio;
				const float ry = bgInfo.VerticalScrollRatio;
				const float frame = (float)_hdScreenInfo->FrameNumber;
				// ramx= / ramy= : defilement lu en RAM a la place du defilement PPU de la ligne
				const int32_t scrollX = bgInfo.ExprXOk ? bgInfo.ExprX : lineScrollX;
				const int32_t scrollY = bgInfo.ExprYOk ? bgInfo.ExprY : lineScrollY;
				switch(bgInfo.Operator) {
					case HdBackgroundInfo::ScrollOperator::Divide:
						cfg.BgScrollX = rx != 0.0f ? (int32_t)(scrollX / rx) : scrollX;
						cfg.BgScrollY = ry != 0.0f ? (int32_t)(scrollY / ry) : scrollY;
						break;
					case HdBackgroundInfo::ScrollOperator::Plus:
						cfg.BgScrollX = scrollX + (int32_t)(rx * frame);
						cfg.BgScrollY = scrollY + (int32_t)(ry * frame);
						break;
					case HdBackgroundInfo::ScrollOperator::Minus:
						cfg.BgScrollX = scrollX - (int32_t)(rx * frame);
						cfg.BgScrollY = scrollY - (int32_t)(ry * frame);
						break;
					case HdBackgroundInfo::ScrollOperator::Multiply:
					default:
						cfg.BgScrollX = (int32_t)(scrollX * rx);
						cfg.BgScrollY = (int32_t)(scrollY * ry);
						break;
				}
				// tileY : pas de bornes verticales (le PNG se repete) ; tileX : pas de bornes horizontales
				const bool yOk = bgInfo.TileY || (y >= -cfg.BgScrollY && (y + bgInfo.Top + cfg.BgScrollY + 1) * scale <= bgInfo.Data->Height);
				if(yOk) {
					if(bgInfo.TileX) {
						cfg.BgMinX = -30000;
						cfg.BgMaxX = 30000;
					} else {
						cfg.BgMinX = -cfg.BgScrollX;
						cfg.BgMaxX = bgInfo.Data->Width / scale - bgInfo.Left - cfg.BgScrollX - 1;
					}
				}
			}
			lineCfg[cfgIndex] = cfg;
		}
	}
}

template<uint32_t scale>
int32_t HdNesPack<scale>::GetLayerIndex(uint8_t priority)
{
	for(size_t i = 0; i < _hdData->BackgroundsByPriority[priority].size(); i++) {
		bool isMatch = true;
		for(HdPackCondition* condition : _hdData->BackgroundsByPriority[priority][i].Conditions) {
			if(!condition->CheckCondition(0, 0, nullptr)) {
				isMatch = false;
				break;
			}
		}

		if(isMatch) {
			return (int32_t)i;
		}
	}
	return -1;
}

template<uint32_t scale>
void HdNesPack<scale>::OnBeforeApplyFilter()
{
	for(unique_ptr<HdPackCondition>& condition : _hdData->Conditions) {
		condition->Initialize(_hdScreenInfo, this);
	}

	// [fork NES] <setVariable> (doc 57) : valeur courante de chaque variable, la derniere regle vraie gagne
	static int varLogs = 0;
	for(unique_ptr<HdPackVariable>& var : _hdData->Variables) {
		var->Current = -1; // recalcule a chaque image : aucune regle vraie = aucune valeur
		for(const HdPackVariable::Rule& r : var->Rules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) {
				if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; }
			}
			if(ok) var->Current = r.ValueIndex;
		}
		if(var->Current != var->LoggedValue) {
			if(varLogs < 60) {
				varLogs++;
				MessageManager::Log("[HD-VAR] $" + var->Name + " = " + (var->Current >= 0 ? var->Values[var->Current] : string("(aucune)")));
			}
			var->LoggedValue = var->Current;
		}
	}

	if(_hdData->Palette.size() == 0x40) {
		memcpy(_palette, _hdData->Palette.data(), 0x40 * sizeof(uint32_t));
	} else {
		NesDefaultVideoFilter::GetFullPalette(_palette, _settings->GetNesConfig(), PpuModel::Ppu2C02);
	}
	_cacheEnabled = (_hdData->OptionFlags & (int)HdPackOptions::DisableCache) == 0;

	for(int layer = 0; layer < 5; layer++) {
		uint32_t activeCount = 0;
		for(int i = 0; i < HdNesPack::PriorityLevelsPerLayer; i++) {
			int32_t index = GetLayerIndex(layer * HdNesPack::PriorityLevelsPerLayer + i);
			if(index >= 0) {
				_bgConfig[layer*10+activeCount].BgPriority = layer * HdNesPack::PriorityLevelsPerLayer + i;
				_bgConfig[layer*10+activeCount].BackgroundIndex = index;
				activeCount++;
			}
		}
		_activeBgCount[layer] = activeCount;

		// [fork NES] diagnostic (setVariable) : journal quand le fond actif d'un calque change
		{
			static int bgLogs = 0;
			static int32_t lastKey[5] = { -2, -2, -2, -2, -2 };
			const int32_t key = activeCount > 0 ? (int32_t)(_bgConfig[layer * 10].BgPriority * 1000 + _bgConfig[layer * 10].BackgroundIndex) : -1;
			if(key != lastKey[layer]) {
				lastKey[layer] = key;
				if(bgLogs < 80) {
					bgLogs++;
					string what = "aucun fond";
					if(activeCount > 0) {
						const HdBackgroundInfo& ab = _hdData->BackgroundsByPriority[_bgConfig[layer * 10].BgPriority][_bgConfig[layer * 10].BackgroundIndex];
						what = ab.Data ? ab.Data->PngName : string("?");
					}
					MessageManager::Log("[HD-BG] calque " + std::to_string(layer) + " : " + what);
				}
			}
		}

		//Decodage des PNG des <background> actifs ici (sequentiel), plus dans la boucle des lignes
		for(uint32_t i = 0; i < activeCount; i++) {
			HdBgConfig& cfg = _bgConfig[layer * HdNesPack::PriorityLevelsPerLayer + i];
			_hdData->BackgroundsByPriority[cfg.BgPriority][cfg.BackgroundIndex].Data->Init();
		}
	}

	// <background> avec ramx= / ramy= : valeurs RAM de cette image
	if(_hdScreenInfo) {
		vector<int32_t> wv;
		bool wvBuilt = false;
		for(auto& bgs : _hdData->BackgroundsByPriority) {
			for(HdBackgroundInfo& bgi : bgs) {
				if(bgi.ScrollXExpr.Empty() && bgi.ScrollYExpr.Empty()) continue;
				if(!wvBuilt) {
					wvBuilt = true;
					for(const HdWatchedValue& w : _hdData->WatchedValues) {
						uint32_t raw = 0;
						bool okv = true;
						for(uint32_t bi = 0; bi < w.Size; bi++) {
							auto it = _hdScreenInfo->WatchedAddressValues.find(w.Address + bi);
							if(it == _hdScreenInfo->WatchedAddressValues.end()) { okv = false; break; }
							raw |= (uint32_t)it->second << (8 * bi);
						}
						wv.push_back(okv ? HdWatchCompose(raw, w.Size, w.Signed) : INT32_MIN);
					}
				}
				float v = 0.0f;
				bgi.ExprXOk = !bgi.ScrollXExpr.Empty() && bgi.ScrollXExpr.Eval(wv, v);
				if(bgi.ExprXOk) bgi.ExprX = (int32_t)v;
				bgi.ExprYOk = !bgi.ScrollYExpr.Empty() && bgi.ScrollYExpr.Eval(wv, v);
				if(bgi.ExprYOk) bgi.ExprY = (int32_t)v;
			}
		}
	}

	ProcessAdditionalSprites();
}

template<uint32_t scale>
void HdNesPack<scale>::ProcessAdditionalSprites()
{
	if(_hdData->AdditionalSprites.empty()) {
		return;
	}

	bool checkFallbackTiles = _console->GetMapper()->HasChrRom() && _fallbackTiles.size() > 0;
	HdPpuPixelInfo& lineFirstPixel = _hdScreenInfo->ScreenTiles[0];
	uint32_t yScroll = (((lineFirstPixel.TmpVideoRamAddr & 0x3E0) >> 2) | ((lineFirstPixel.TmpVideoRamAddr & 0x7000) >> 12)) + ((lineFirstPixel.TmpVideoRamAddr & 0x800) ? 240 : 0);
	uint16_t tmpVramAddr = lineFirstPixel.TmpVideoRamAddr;
	bool processBgNextRow = true;

	for(int32_t y = 0; y < NesConstants::ScreenHeight; y++) {
		lineFirstPixel = _hdScreenInfo->ScreenTiles[y << 8];

		//Only process the first scanline for each row of tiles (if no additions are found)
		if(((yScroll + y) & 0x07) == 0 || tmpVramAddr != lineFirstPixel.TmpVideoRamAddr) {
			yScroll = (((lineFirstPixel.TmpVideoRamAddr & 0x3E0) >> 2) | ((lineFirstPixel.TmpVideoRamAddr & 0x7000) >> 12)) + ((lineFirstPixel.TmpVideoRamAddr & 0x800) ? 240 : 0);
			tmpVramAddr = lineFirstPixel.TmpVideoRamAddr;
			processBgNextRow = true;
		}

		if(processBgNextRow) {
			processBgNextRow = false;
			for(int32_t x = 0; x < NesConstants::ScreenWidth; x++) {
				HdPpuPixelInfo& pixelInfo = _hdScreenInfo->ScreenTiles[(y << 8) | x];
				for(uint8_t i = 0; i < pixelInfo.SpriteCount; i++) {
					DrawAdditionalTiles(x, y, pixelInfo.Sprite[i], checkFallbackTiles);
				}

				processBgNextRow |= DrawAdditionalTiles(x, y, pixelInfo.Tile, checkFallbackTiles);
			}
		} else {
			for(int32_t x = 0; x < NesConstants::ScreenWidth; x++) {
				HdPpuPixelInfo& pixelInfo = _hdScreenInfo->ScreenTiles[(y << 8) | x];
				for(uint8_t i = 0; i < pixelInfo.SpriteCount; i++) {
					DrawAdditionalTiles(x, y, pixelInfo.Sprite[i], checkFallbackTiles);
				}
			}
		}
	}
}

template<uint32_t scale>
bool HdNesPack<scale>::DrawAdditionalTiles(int32_t x, int32_t y, HdPpuTileInfo& tile, bool checkFallbackTiles)
{
	auto result = _additionalTilesByKey.find(tile);
	if(result == _additionalTilesByKey.end()) {
		BuildAdditionalTileCache(x, y, tile, checkFallbackTiles);
		result = _additionalTilesByKey.find(tile);
	}

	for(auto& additionalSprite : result->second) {
		InsertAdditionalSprite(x, y, tile, additionalSprite);
	}
	return result->second.size() > 0;
}

template<uint32_t scale>
void HdNesPack<scale>::BuildAdditionalTileCache(int32_t x, int32_t y, HdPpuTileInfo& tile, bool checkFallbackTiles)
{
	vector<HdPackAdditionalSpriteInfo> additions;
	for(HdPackAdditionalSpriteInfo& additionalSprite : _hdData->AdditionalSprites) {
		if(!additionalSprite.IgnorePalette && tile == additionalSprite.OriginalTile) {
			additions.push_back(additionalSprite);
		} else if(additionalSprite.IgnorePalette && tile.GetKey(true) == additionalSprite.OriginalTile.GetKey(true)) {
			additions.push_back(additionalSprite);
		} else if(checkFallbackTiles) {
			if(additionalSprite.IgnorePalette || tile.PaletteColors == additionalSprite.OriginalTile.PaletteColors) {
				if(GetFallbackTile(tile.TileIndex) == additionalSprite.OriginalTile.TileIndex) {
					additions.push_back(additionalSprite);
				}
			}
		}
	}

	//Cache list of additional tiles linked to this sprite
	_additionalTilesByKey.emplace(tile, additions);
}

template<uint32_t scale>
void HdNesPack<scale>::InsertAdditionalSprite(int32_t sourceX, int32_t sourceY, HdPpuTileInfo& sprite, HdPackAdditionalSpriteInfo& additionalSprite)
{
	int32_t targetX = sourceX + (sprite.HorizontalMirroring ? -additionalSprite.OffsetX : additionalSprite.OffsetX);
	int32_t targetY = sourceY + (sprite.VerticalMirroring ? -additionalSprite.OffsetY : additionalSprite.OffsetY);

	if(targetX >= 0 && targetX < NesConstants::ScreenWidth && targetY >= 0 && targetY < NesConstants::ScreenHeight) {
		HdPpuPixelInfo& pixelInfo = _hdScreenInfo->ScreenTiles[(targetY << 8) | targetX];
		if(pixelInfo.SpriteCount < 4) {
			HdPpuTileInfo& newSprite = pixelInfo.Sprite[pixelInfo.SpriteCount];
			memcpy(&newSprite, &sprite, sizeof(HdPpuTileInfo));
			newSprite.SpriteColorIndex = 0;
			newSprite.TileIndex = additionalSprite.AdditionalTile.TileIndex;
			newSprite.PaletteColors = additionalSprite.AdditionalTile.PaletteColors;
			memcpy(newSprite.TileData, additionalSprite.AdditionalTile.TileData, sizeof(newSprite.TileData));
			pixelInfo.SpriteCount++;
		}
	}
}

template<uint32_t scale>
HdPackTileInfo* HdNesPack<scale>::GetCachedMatchingTile(uint32_t x, uint32_t y, HdPpuTileInfo* tile)
{
	if(((_scrollX + x) & 0x07) == 0) {
		_useCachedTile = false;
	}

	bool disableCache = false;
	HdPackTileInfo* hdPackTileInfo;
	if(_useCachedTile) {
		hdPackTileInfo = _cachedTile;
	} else {
		hdPackTileInfo = GetMatchingTile(x, y, tile, &disableCache);

		if(!disableCache && _cacheEnabled) {
			//Use this tile for the next 8 horizontal pixels
			//Disable cache if a sprite condition is used, because sprites are not on a 8x8 grid
			_cachedTile = hdPackTileInfo;
			_useCachedTile = true;
		}
	}
	return hdPackTileInfo;
}

template<uint32_t scale>
HdPackTileInfo* HdNesPack<scale>::GetMatchingTile(uint32_t x, uint32_t y, HdPpuTileInfo* tile, bool* disableCache)
{
	auto hdTile = _hdData->TileByKey.find(*tile);
	if(hdTile == _hdData->TileByKey.end()) {
		int32_t fallbackTileIndex = GetFallbackTile(tile->TileIndex);
		if(fallbackTileIndex >= 0) {
			int32_t orgIndex = tile->TileIndex;
			tile->TileIndex = fallbackTileIndex;
			hdTile = _hdData->TileByKey.find(*tile);
			if(hdTile == _hdData->TileByKey.end()) {
				hdTile = _hdData->TileByKey.find(tile->GetKey(true));
				if(hdTile == _hdData->TileByKey.end()) {
					tile->TileIndex = orgIndex;
				}
			}
		}
	
		if(hdTile == _hdData->TileByKey.end()) {
			hdTile = _hdData->TileByKey.find(tile->GetKey(true));
		}
	}

	if(hdTile != _hdData->TileByKey.end()) {
		// Passe 0 : variantes miroir (<tile_hflip> etc.) dont les miroirs correspondent a la tuile PPU
		// Passe 1 : entrees normales (le miroir est alors fait a l'affichage)
		for(int pass = 0; pass < 2; pass++) {
			for(HdPackTileInfo* hdPackTile : hdTile->second) {
				const bool isVariant = hdPackTile->IsHFlip || hdPackTile->IsVFlip;
				if(pass == 0) {
					if(!isVariant || hdPackTile->IsHFlip != tile->HorizontalMirroring || hdPackTile->IsVFlip != tile->VerticalMirroring) {
						continue;
					}
				} else if(isVariant) {
					continue;
				}

				if(disableCache != nullptr && hdPackTile->ForceDisableCache) {
					*disableCache = true;
				}

				if(hdPackTile->MatchesCondition(x, y, tile)) {
					if(hdPackTile->NeedInit()) {
						hdPackTile->Init();
					}
					return hdPackTile;
				}
			}
		}
	}

	return nullptr;
}

template<uint32_t scale>
void HdNesPack<scale>::DrawBackgroundLayer(const HdBgConfig& bgConfig, uint32_t x, uint32_t y, uint32_t* outputBuffer, uint32_t screenWidth)
{
	if((int32_t)x >= bgConfig.BgMinX && (int32_t)x <= bgConfig.BgMaxX) {
		HdBackgroundInfo& bgInfo = _hdData->BackgroundsByPriority[bgConfig.BgPriority][bgConfig.BackgroundIndex];
		// <Ignore2DOverlays> : fond ignore => pixel net pendant la distorsion
		if(_hdData->HasIgnore2D && bgInfo.IsIgnored2D((int)x, (int)y, nullptr)) _ignNes[(y << 8) | x] = 1;
		// Z-index : ce fond est le dernier element dessine sur ce pixel (si un <video> existe)
		if(_zEnabled) _zNes[(y << 8) | x] = (uint8_t)bgConfig.BgPriority;
		const vector<NesHdTextureInfo>* tex2d = _hdData->Textures2D[bgInfo.Priority].empty() ? nullptr : &_hdData->Textures2D[bgInfo.Priority];
		switch(bgInfo.BlendMode) {
			case HdPackBlendMode::Alpha: DrawCustomBackground<HdPackBlendMode::Alpha>(bgInfo, outputBuffer, x + bgConfig.BgScrollX, y + bgConfig.BgScrollY, screenWidth, x, y, bgConfig.RawScrollX, bgConfig.RawScrollY, tex2d); break;
			case HdPackBlendMode::Add: DrawCustomBackground<HdPackBlendMode::Add>(bgInfo, outputBuffer, x + bgConfig.BgScrollX, y + bgConfig.BgScrollY, screenWidth, x, y, bgConfig.RawScrollX, bgConfig.RawScrollY, tex2d); break;
			case HdPackBlendMode::Subtract: DrawCustomBackground<HdPackBlendMode::Subtract>(bgInfo, outputBuffer, x + bgConfig.BgScrollX, y + bgConfig.BgScrollY, screenWidth, x, y, bgConfig.RawScrollX, bgConfig.RawScrollY, tex2d); break;
		}
	}
}

template<uint32_t scale>
void HdNesPack<scale>::ResolvePixel(uint32_t x, uint32_t y, HdPpuPixelInfo &pixelInfo)
{
	//Passe 1 (sequentielle) : recherche des tiles HD du pixel, dans le meme ordre que l'ancien GetPixels.
	//Les conditions (cache de resultat, tileNearby/spriteNearby) et l'init des tiles ne sont pas thread-safe.
	uint32_t pixelIndex = (y << 8) | x;

	HdPackTileInfo *hdPackTileInfo = nullptr;
	if(pixelInfo.Tile.TileIndex != HdPpuTileInfo::NoTile) {
		hdPackTileInfo = GetCachedMatchingTile(x, y, &pixelInfo.Tile);
	}
	_resolvedBg[pixelIndex] = hdPackTileInfo;

	if(pixelInfo.SpriteCount > 0) {
		HdPackTileInfo** sprites = _resolvedSprites.data() + (pixelIndex << 2);
		for(int k = 0; k < 4; k++) {
			sprites[k] = nullptr; // emplacements non remplis ce pixel : pas de tuile (evite les valeurs de l'image precedente)
		}
		int lowestBgSprite = 999;
		for(int k = pixelInfo.SpriteCount - 1; k >= 0; k--) {
			if(pixelInfo.Sprite[k].BackgroundPriority) {
				if(pixelInfo.Sprite[k].SpriteColorIndex != 0) {
					lowestBgSprite = k;
				}
				sprites[k] = GetMatchingTile(x, y, &pixelInfo.Sprite[k]);
			}
		}

		for(int k = pixelInfo.SpriteCount - 1; k >= 0; k--) {
			if(!pixelInfo.Sprite[k].BackgroundPriority && lowestBgSprite > k) {
				sprites[k] = GetMatchingTile(x, y, &pixelInfo.Sprite[k]);
			}
		}
	}

	// <Ignore2DOverlays> : pixel net si la tuile de fond ou une tuile sprite du pixel est dans un bloc / une regle ignore
	if(_hdData->HasIgnore2D) {
		bool ign = hdPackTileInfo && hdPackTileInfo->IsIgnored2D(x, y, &pixelInfo.Tile);
		if(!ign && pixelInfo.SpriteCount > 0) {
			HdPackTileInfo** sprites = _resolvedSprites.data() + (pixelIndex << 2);
			for(int k = 0; k < pixelInfo.SpriteCount && !ign; k++) {
				if(sprites[k] && sprites[k]->IsIgnored2D(x, y, &pixelInfo.Sprite[k])) ign = true;
			}
		}
		_ignNes[pixelIndex] = ign ? 1 : 0;
	}
}

template<uint32_t scale>
void HdNesPack<scale>::DrawPixel(uint32_t x, uint32_t y, HdPpuPixelInfo &pixelInfo, uint32_t *outputBuffer, uint32_t screenWidth, const HdBgConfig* lineCfg)
{
	//Passe 2 (parallele) : dessin seul. Ne lit que des donnees figees par la passe 1.
	uint32_t pixelIndex = (y << 8) | x;
	HdPackTileInfo *hdPackTileInfo = _resolvedBg[pixelIndex];
	HdPackTileInfo *hdPackSpriteInfo = nullptr;
	HdPackTileInfo* const* sprites = _resolvedSprites.data() + (pixelIndex << 2);

	bool hasSprite = pixelInfo.SpriteCount > 0;
	// <2DReflect> : marque les pixels ou un sprite opaque est affiche
	if(_reflectEnabled) {
		uint8_t opaqueSprite = 0;
		for(int k = 0; k < pixelInfo.SpriteCount; k++) {
			if(pixelInfo.Sprite[k].SpriteColorIndex != 0) { opaqueSprite = 1; break; }
		}
		_sprNes[pixelIndex] = opaqueSprite;
	}
	bool renderOriginalTiles = ((_hdData->OptionFlags & (int)HdPackOptions::DontRenderOriginalTiles) == 0);

	//La tile de fond couvre-t-elle tout le bloc scale x scale de facon opaque ?
	//Si oui, tout ce qui est dessine avant elle (couleur de fond, <background> derriere, sprites derriere) serait ecrase : on ne le dessine pas.
	bool opaqueBgTile;
	if(hdPackTileInfo) {
		opaqueBgTile = !hdPackTileInfo->IsFullyTransparent && !hdPackTileInfo->HasTransparentPixels;
	} else {
		opaqueBgTile = renderOriginalTiles && pixelInfo.Tile.BgColorIndex != 0;
	}

	//Z-index : le z est celui de l'element dessine en dernier. Les groupes de fond vont par priorite croissante,
	//donc chaque tuile prend le z le plus haut de la couche qui la precede (voir HdNesPack::PriorityLevelsPerLayer).
	auto markZ = [&](uint8_t z) {
		if(_zEnabled) _zNes[pixelIndex] = z;
	};

	int lowestBgSprite = 999;
	
	if(!opaqueBgTile) {
		DrawColor(_palette[pixelInfo.Tile.PpuBackgroundColor], outputBuffer, screenWidth);

		for(int i = 0; i < _activeBgCount[0]; i++) {
			DrawBackgroundLayer(lineCfg[HdNesPack::BehindBgSpritesPriority+i], x, y, outputBuffer, screenWidth);
		}
		DrawLiquidBand(0, x, y, outputBuffer, screenWidth);
	}

	if(hasSprite) {
		for(int k = pixelInfo.SpriteCount - 1; k >= 0; k--) {
			if(pixelInfo.Sprite[k].BackgroundPriority) {
				if(pixelInfo.Sprite[k].SpriteColorIndex != 0) {
					lowestBgSprite = k;
				}

				if(!opaqueBgTile) {
					hdPackSpriteInfo = sprites[k];
					if(hdPackSpriteInfo) {
						DrawTile(pixelInfo.Sprite[k], *hdPackSpriteInfo, outputBuffer, screenWidth);
						markZ(HdNesPack::BehindBgPriority - 1);
					} else if(pixelInfo.Sprite[k].SpriteColorIndex != 0) {
						DrawColor(_palette[pixelInfo.Sprite[k].SpriteColor], outputBuffer, screenWidth);
						markZ(HdNesPack::BehindBgPriority - 1);
					}
				}
			}
		}
	}
	
	if(!opaqueBgTile) {
		for(int i = 0; i < _activeBgCount[1]; i++) {
			DrawBackgroundLayer(lineCfg[HdNesPack::BehindBgPriority+i], x, y, outputBuffer, screenWidth);
		}
		DrawLiquidBand(1, x, y, outputBuffer, screenWidth);
	}
	
	if(hdPackTileInfo) {
		DrawTile(pixelInfo.Tile, *hdPackTileInfo, outputBuffer, screenWidth);
		markZ(HdNesPack::BehindFgSpritesPriority - 1);
	} else if(renderOriginalTiles) {
		//Draw regular SD background tile
		if(pixelInfo.Tile.BgColorIndex != 0) {
			DrawColor(_palette[pixelInfo.Tile.BgColor], outputBuffer, screenWidth);
			markZ(HdNesPack::BehindFgSpritesPriority - 1);
		}
	}

	for(int i = 0; i < _activeBgCount[2]; i++) {
		DrawBackgroundLayer(lineCfg[HdNesPack::BehindFgSpritesPriority+i], x, y, outputBuffer, screenWidth);
	}
	DrawLiquidBand(2, x, y, outputBuffer, screenWidth);

	if(hasSprite) {
		for(int k = pixelInfo.SpriteCount - 1; k >= 0; k--) {
			if(!pixelInfo.Sprite[k].BackgroundPriority && lowestBgSprite > k) {
				hdPackSpriteInfo = sprites[k];
				if(hdPackSpriteInfo) {
					DrawTile(pixelInfo.Sprite[k], *hdPackSpriteInfo, outputBuffer, screenWidth);
					markZ(HdNesPack::ForegroundPriority - 1);
				} else if(pixelInfo.Sprite[k].SpriteColorIndex != 0) {
					DrawColor(_palette[pixelInfo.Sprite[k].SpriteColor], outputBuffer, screenWidth);
					markZ(HdNesPack::ForegroundPriority - 1);
				}
			}
		}
	}

	for(int i = 0; i < _activeBgCount[3]; i++) {
		DrawBackgroundLayer(lineCfg[HdNesPack::ForegroundPriority+i], x, y, outputBuffer, screenWidth);
	}
	DrawLiquidBand(3, x, y, outputBuffer, screenWidth);

	//Cinquieme bande (40 a 49) : au-dessus du premier plan
	for(int i = 0; i < _activeBgCount[4]; i++) {
		DrawBackgroundLayer(lineCfg[HdNesPack::TopPriority+i], x, y, outputBuffer, screenWidth);
	}
	DrawLiquidBand(4, x, y, outputBuffer, screenWidth);
}

template<uint32_t scale>
void HdNesPack<scale>::UpdateBackgroundFrames(HdScreenInfo* hdScreenInfo)
{
	// Fond anime (dossier) : image = (frame / framesBetweenSteps) % nombre d'images, comme SNES.
	// Appele avant les passes paralleles : seul ce thread modifie bg.Data.
	for(int p = 0; p < HdPackData::BgLayerCount; p++) {
		for(auto& bg : _hdData->BackgroundsByPriority[p]) {
			if(bg.AnimFrames.empty() || bg.FramesBetweenSteps <= 0) continue;
			size_t idx = (size_t)((hdScreenInfo->FrameNumber / (uint32_t)bg.FramesBetweenSteps) % bg.AnimFrames.size());
			bg.Data = bg.AnimFrames[idx];
			bg.Data->Init();
		}
	}
}

template<uint32_t scale>
void HdNesPack<scale>::Process(HdScreenInfo *hdScreenInfo, uint32_t* outputBuffer, OverscanDimensions &overscan)
{
	_hdScreenInfo = hdScreenInfo;
	uint32_t hdScale = GetScale();
	uint32_t screenWidth = (NesConstants::ScreenWidth - overscan.Left - overscan.Right) * hdScale;
	const uint32_t yStart = overscan.Top, yEnd = 240 - overscan.Bottom;
	const uint32_t xStart = overscan.Left, xEnd = 256 - overscan.Right;

	// Chronometrage par passe (journal [HD-PASS] NES toutes les 300 frames)
	auto tLap = std::chrono::steady_clock::now();
	auto lap = [&](int pass) {
		const auto tNow = std::chrono::steady_clock::now();
		_passMs[pass] += std::chrono::duration<double, std::milli>(tNow - tLap).count();
		tLap = tNow;
	};

	OnBeforeApplyFilter();
	UpdateBackgroundFrames(hdScreenInfo);

	//Z-index par pixel : calcule seulement si le pack contient un <video>
	SimulateOverlays(hdScreenInfo);
	_liquidOffX = overscan.Left;
	_liquidOffY = overscan.Top;
	_reflectEnabled = !_hdData->Reflects.empty();
	PrepareLiquids(screenWidth);
	_zEnabled = !_hdData->Videos.empty() || !_hdData->Overlays.empty() || !_hdData->Perspectives.empty() || !_hdData->DayNightRules.empty();
	UpdateDayNight(hdScreenInfo); // <DayNight> : avance des cycles, couleur de l'image
	_dnDioApplied = false;
	if(_zEnabled) {
		std::fill(_zNes.begin(), _zNes.end(), (uint8_t)0);
	}

	lap(0);
	//Passe 1 (sequentielle) : quelle tile HD pour chaque pixel (fond + sprites)
	for(uint32_t i = yStart; i < yEnd; i++) {
		OnLineStart(hdScreenInfo->ScreenTiles[i << 8]);
		{
			const HdPpuPixelInfo& lp = hdScreenInfo->ScreenTiles[i << 8];
			_lineScrollX[i] = ((lp.TmpVideoRamAddr & 0x1F) << 3) | lp.XScroll | ((lp.TmpVideoRamAddr & 0x400) ? 0x100 : 0);
			_lineScrollY[i] = (((lp.TmpVideoRamAddr & 0x3E0) >> 2) | ((lp.TmpVideoRamAddr & 0x7000) >> 12)) + ((lp.TmpVideoRamAddr & 0x800) ? 240 : 0);
		}
		for(uint32_t j = xStart; j < xEnd; j++) {
			ResolvePixel(j, i, hdScreenInfo->ScreenTiles[i * 256 + j]);
		}
	}

	if(!_hdData->Tile2ObjRules.empty()) DetectTile2Obj(); else if(!_dioT2oRules.empty() || !_dioT2oHide.empty()) { _dioT2oRules.clear(); _dioT2oHide.clear(); }
	if(!_hdData->Sprite2ObjRules.empty()) DetectSprite2Obj(); else _s2oMask.clear(); // apres Tile2Obj : ajoute ses instances
	if(!_hdData->WorldObjRules.empty()) DetectWorldObj(); // <DioramaObject> anchor=bgN : ajoute ses instances
	lap(1);
	//Passe 2 (parallele) : dessin. Chaque ligne NES ecrit ses propres lignes HD, sans recouvrement.
	auto drawLine = [&](uint32_t i) {
		HdBgConfig lineCfg[50];
		ComputeLineBgConfig(hdScreenInfo->ScreenTiles[i << 8], (uint8_t)i, lineCfg);
		if(_reflectEnabled) ComputeReflectFloorLine(i, lineCfg, xStart, xEnd);
		uint32_t bufferIndex = (i - yStart) * screenWidth * hdScale;
		uint32_t lineStartIndex = bufferIndex;
		for(uint32_t j = xStart; j < xEnd; j++) {
			DrawPixel(j, i, hdScreenInfo->ScreenTiles[i * 256 + j], outputBuffer + bufferIndex, screenWidth, lineCfg);
			bufferIndex += hdScale;
		}

		ProcessGrayscaleAndEmphasis(hdScreenInfo->ScreenTiles[i * 256], outputBuffer + lineStartIndex, screenWidth);
	};

	int nThreads = _pool.Size();
	if(nThreads <= 1 || yEnd <= yStart || yEnd - yStart < 16) {
		for(uint32_t i = yStart; i < yEnd; i++) {
			drawLine(i);
		}
	} else {
		//Lignes entrelacees entre les threads pour equilibrer la charge
		_pool.Run(nThreads, [&](int t) {
			for(uint32_t i = yStart + (uint32_t)t; i < yEnd; i += (uint32_t)nThreads) {
				drawLine(i);
			}
		});
	}

	lap(2);
	//Diorama (G2a, G2b) : couches separees, composees par le GPU ; la sortie est remplacee par l'image GPU, identique a celle du
	//processeur (controle toutes les 300 images). Les lignes grayscale / emphase gardent l'image du processeur.
	//Le temps n'est pas compte dans les passes du journal [HD-PASS].
	_dioWsReady = false;
	if(_hdData->DioramaEnabled) {
		const bool dioCheck = (_dioFrame++ % 300) == 0;
		_dioDrawing = true;
		RunDioramaLayers(hdScreenInfo, outputBuffer, screenWidth, yStart, yEnd, xStart, xEnd, dioCheck);
		_dioDrawing = false;
		DioramaGpuStep(hdScreenInfo, outputBuffer, screenWidth, (yEnd - yStart) * hdScale, yStart, yEnd, dioCheck && _dioGpuBgN == 0);
		tLap = std::chrono::steady_clock::now();
	}
	//<2DReflect> : reflet des sprites sous le sol, sur l'image composee (avant les overlays)
	if(_reflectEnabled && !_wsMode) {
		ProcessReflections(outputBuffer, screenWidth, overscan);
	}
	//Overlays puis videos : appliques apres grayscale/emphasis, sequentiellement
	if(_zEnabled) {
		if(!_wsMode) {
			ProcessOverlays(outputBuffer, screenWidth, overscan);
		}
	}
	lap(3);
	//Ecrans plainscreen / ellipse : apres les overlays, avant les videos
	if(!_wsMode) {
		ProcessScreenLayers(outputBuffer, screenWidth, overscan);
	}
	lap(4);
	//Distorsions : deplacement horizontal des lignes de l'image composee (apres les calques, avant les videos)
	if(!_wsMode) {
		ProcessDistortions(outputBuffer, screenWidth, overscan);
	}
	//Plans <2DPerspective> : apres les distorsions, avant les videos (voie 1 widescreen : a venir)
	if(!_wsMode && !_hdData->Perspectives.empty()) {
		ProcessPerspectives(outputBuffer, screenWidth, overscan);
	}
	lap(5);
	//Videos : appliquees en dernier (apres grayscale/emphasis), sequentiellement
	if(_zEnabled) {
		ProcessVideos(outputBuffer, screenWidth, overscan);
	}
	lap(6);
	LogDioramaDiag(hdScreenInfo, yStart, yEnd);
	if(!_wsMode) GpuTestG0(outputBuffer, screenWidth, (yEnd - yStart) * hdScale, yStart, yEnd);

	// Moyenne sur 300 frames, meme principe que le filtre SNES
	if(++_passFrames >= 300) {
		double total = 0;
		for(double ms : _passMs) total += ms;
		char buf[320];
		snprintf(buf, sizeof(buf), "[HD-PASS] NES ms/frame : preparation %.2f, resolution des tiles (1 coeur) %.2f, dessin %.2f, particules %.2f, calques %.2f, distorsion %.2f, videos %.2f (total %.2f, %d threads)",
			_passMs[0] / _passFrames, _passMs[1] / _passFrames, _passMs[2] / _passFrames, _passMs[3] / _passFrames,
			_passMs[4] / _passFrames, _passMs[5] / _passFrames, _passMs[6] / _passFrames, total / _passFrames, _pool.Size());
		MessageManager::Log(buf);
		for(double& ms : _passMs) ms = 0;
		_passFrames = 0;
	}
}

// Journal de diagnostic du diorama (doc 32, E0). Lecture seule, aucun effet sur le rendu.
// Declenche par un fichier hd_debug_dump.flag dans le dossier du pack (supprime apres detection), puis 3600 images (60 s).
// Une ligne n'est ecrite que pour les images utiles : la premiere, une sur 60, et les 4 images (dont celle du saut) qui
// suivent un saut brusque : une valeur surveillee qui change de plus de 3 (ex. page, wrap 255 -> 0) ou le scroll X
// maximal qui change de plus de 3 (franchissement de page, changement de salle).
// Ne fait rien si le pack ne declare aucun <watchMemoryAddress>.
template<uint32_t scale>
void HdNesPack<scale>::LogDioramaDiag(HdScreenInfo* hdScreenInfo, uint32_t yStart, uint32_t yEnd)
{
	if(_hdData->WatchedValues.empty() || _hdData->PackFolder.empty()) return;
	if(_dioramaBurst == 0) {
		if(++_dioramaCheck < 30) return;
		_dioramaCheck = 0;
		std::error_code ec;
		std::filesystem::path flag = std::filesystem::path(_hdData->PackFolder) / "hd_debug_dump.flag";
		if(!std::filesystem::exists(flag, ec)) return;
		std::filesystem::remove(flag, ec);
		_dioramaBurst = 3600;
		_dioramaFrame = 0;
		_dioramaTail = 0;
		_dioramaPrevVals.clear();
		MessageManager::Log("[HD-DIORAMA] mesure demarree : 3600 images (flag detecte et supprime). Lignes ecrites : premiere image, une sur 60, et 4 images a chaque saut de valeur ou de scroll (>3)");
	}
	_dioramaBurst--;

	int32_t minX = INT32_MAX, maxX = INT32_MIN, minY = INT32_MAX, maxY = INT32_MIN;
	int changesX = 0, changesY = 0;
	for(uint32_t y = yStart; y < yEnd && y < 240; y++) {
		minX = std::min(minX, _lineScrollX[y]); maxX = std::max(maxX, _lineScrollX[y]);
		minY = std::min(minY, _lineScrollY[y]); maxY = std::max(maxY, _lineScrollY[y]);
		if(y > yStart) {
			if(_lineScrollX[y] != _lineScrollX[y - 1]) changesX++;
			if(_lineScrollY[y] != _lineScrollY[y - 1]) changesY++;
		}
	}

	// Valeurs surveillees de cette image (INT32_MIN = adresse non lue)
	vector<int32_t> vals;
	for(const HdWatchedValue& w : _hdData->WatchedValues) {
		uint32_t raw = 0;
		bool ok = true;
		for(uint32_t i = 0; i < w.Size; i++) {
			auto it = hdScreenInfo->WatchedAddressValues.find(w.Address + i);
			if(it == hdScreenInfo->WatchedAddressValues.end()) { ok = false; break; }
			raw |= (uint32_t)it->second << (8 * i);
		}
		int32_t v = INT32_MIN;
		if(ok) v = w.Signed ? (w.Size == 1 ? (int32_t)(int8_t)raw : (int32_t)(int16_t)raw) : (int32_t)raw;
		vals.push_back(v);
	}

	bool jump = false;
	if(_dioramaPrevVals.size() == vals.size()) {
		for(size_t i = 0; i < vals.size(); i++) {
			if(vals[i] != INT32_MIN && _dioramaPrevVals[i] != INT32_MIN && std::abs(vals[i] - _dioramaPrevVals[i]) > 3) jump = true;
		}
		if(std::abs(maxX - _dioramaPrevMaxX) > 3) jump = true;
	}
	_dioramaPrevVals = vals;
	_dioramaPrevMaxX = maxX;
	if(jump) _dioramaTail = 4;
	const bool emit = (_dioramaFrame++ % 60) == 0 || _dioramaTail > 0;
	const bool tagJump = jump;
	if(_dioramaTail > 0) _dioramaTail--;

	if(emit) {
		const uint32_t yMid = std::min<uint32_t>((yStart + yEnd) / 2, 239);
		std::string line = "[HD-DIORAMA] f=" + std::to_string(hdScreenInfo->FrameNumber) + (tagJump ? " SAUT" : "")
			+ " scrollX min/max/chg=" + std::to_string(minX) + "/" + std::to_string(maxX) + "/" + std::to_string(changesX)
			+ " scrollY min/max/chg=" + std::to_string(minY) + "/" + std::to_string(maxY) + "/" + std::to_string(changesY)
			+ " ligneMilieu=(" + std::to_string(_lineScrollX[yMid]) + "," + std::to_string(_lineScrollY[yMid]) + ")";
		for(size_t i = 0; i < vals.size(); i++) {
			line += " " + _hdData->WatchedValues[i].Name + "=";
			line += vals[i] == INT32_MIN ? std::string("?") : std::to_string(vals[i]);
		}
		MessageManager::Log(line);
	}
	if(_dioramaBurst == 0) {
		MessageManager::Log("[HD-DIORAMA] mesure terminee");
	}
}

// G0 (doc 34) : test du contexte GPU commun HdGpuCore. Declenche par un fichier hd_gpu_test.flag dans le dossier du
// pack (supprime apres detection), puis 600 images (10 s) : un triangle colore tournant et un carre jaune translucide,
// dessines par le GPU, sont composes PAR-DESSUS l'image du jeu. Aucun lien avec le rendu du pack ; sans flag, ne fait rien.
// Mesure : cout moyen (envoi + relecture + composition) et thread utilise, journal toutes les 120 images.
template<uint32_t scale>
void HdNesPack<scale>::GpuTestG0(uint32_t* outputBuffer, uint32_t screenWidth, uint32_t outHeight, uint32_t yStart, uint32_t yEnd)
{
	if(_hdData->PackFolder.empty()) return;
	HdGpuCore& gpu = HdGpuCore::Get();
	if(_gpuTestBurst == 0) {
		if(++_gpuTestCheck < 30) return;
		_gpuTestCheck = 0;
		std::error_code ec;
		std::filesystem::path flag = std::filesystem::path(_hdData->PackFolder) / "hd_gpu_test.flag";
		if(!std::filesystem::exists(flag, ec)) return;
		std::filesystem::remove(flag, ec);
		MessageManager::Log("[HD-GPU-G0] test demarre : 600 images (flag detecte et supprime)");
		{
			std::string camReport;
			const bool camOk = HdLayerCameraSelfTest(camReport);
			MessageManager::Log(std::string("[HD-CAM] auto-test camera couche : ") + (camOk ? "OK" : "ECHEC") + " (" + camReport + ")");
		}
		if(!gpu.Available()) {
			MessageManager::Log("[HD-GPU-G0] GPU indisponible : " + gpu.Info());
			return;
		}
		_gpuTestBurst = 600;
		_gpuTestFrame = 0;
		_gpuTestFrames = 0;
		_gpuTestMs = 0;
		_gpuTestThread = 0;
		_gpuTestCamBad = 0;
	}
	_gpuTestBurst--;

	// Camera couche construite avec le scroll PPU de CETTE image : aller-retour Unproject / Project du centre de
	// l'ecran a trois profondeurs (Z = -40, 0, 40). Tout ecart au-dela de 1e-6 px est compte comme echec.
	{
		HdLayerCamera cam;
		cam.Begin((int)yStart, (int)yEnd - 1, 128.0, 120.0, 256.0, 256.0);
		for(uint32_t y = yStart; y < yEnd && y < 240; y++) cam.SetLine((int)y, (double)_lineScrollX[y], (double)_lineScrollY[y]);
		const double testZ[3] = { -40.0, 0.0, 40.0 };
		for(double z : testZ) {
			double wx = 0, wy = 0, wz = 0;
			HdScreenPt p;
			const bool ok = cam.Unproject(128.0, 120.0, z, wx, wy, wz) && cam.Project(wx, wy, z, p) && std::abs(p.x - 128.0) < 1e-6 && std::abs(p.y - 120.0) < 1e-6;
			if(!ok) _gpuTestCamBad++;
		}
	}

	const auto t0 = std::chrono::steady_clock::now();
	const bool ok = gpu.SubmitTest(screenWidth, outHeight, _gpuTestFrame++) && gpu.Fetch(_gpuTestImg);
	if(ok && _gpuTestImg.size() >= (size_t)screenWidth * outHeight) {
		// composition premultipliee : sortie = GPU + jeu x (1 - alpha GPU)
		const size_t n = (size_t)screenWidth * outHeight;
		for(size_t i = 0; i < n; i++) {
			const uint32_t s = _gpuTestImg[i];
			const uint32_t a = s >> 24;
			if(a == 0) continue;
			if(a == 255) { outputBuffer[i] = s; continue; }
			const uint32_t d = outputBuffer[i], inv = 255 - a;
			uint32_t r = (s & 0xFF) + (((d & 0xFF) * inv) / 255);
			uint32_t g = ((s >> 8) & 0xFF) + ((((d >> 8) & 0xFF) * inv) / 255);
			uint32_t b = ((s >> 16) & 0xFF) + ((((d >> 16) & 0xFF) * inv) / 255);
			outputBuffer[i] = 0xFF000000u | (std::min(b, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(r, 255u);
		}
	}
	_gpuTestMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	_gpuTestFrames++;

	const size_t thr = std::hash<std::thread::id>{}(std::this_thread::get_id());
	if(thr != _gpuTestThread) {
		MessageManager::Log("[HD-GPU-G0] thread du filtre : " + std::to_string(thr) + (_gpuTestThread ? " (CHANGEMENT)" : ""));
		_gpuTestThread = thr;
	}
	if(!ok && _gpuTestFrames == 1) {
		MessageManager::Log("[HD-GPU-G0] envoi ou relecture en echec");
	}
	if(_gpuTestFrames >= 120 || _gpuTestBurst == 0) {
		char buf[260];
		snprintf(buf, sizeof(buf), "[HD-GPU-G0] image %ux%u : %.2f ms / image en moyenne (%u images), camera couche : %u echecs cumules%s",
			screenWidth, outHeight, _gpuTestMs / std::max(1u, _gpuTestFrames), _gpuTestFrames, _gpuTestCamBad, _gpuTestBurst == 0 ? " - test termine" : "");
		MessageManager::Log(buf);
		_gpuTestMs = 0;
		_gpuTestFrames = 0;
	}
}

// ---------------------------------------------------------------------------
// Diorama G2a (doc 36) : les memes tiles que DrawPixel, mais dans 4 images separees, alpha conserve.
// Ne modifie ni DrawPixel ni la sortie. Groupes <background>, liquides, overlays et grayscale/emphase : non geres.
// ---------------------------------------------------------------------------
// Fusion "par-dessus" d'un pixel premultiplie, memes arrondis que BlendColors<Alpha> (canal alpha compris).
static inline uint32_t DioOver(uint32_t dst, uint32_t src)
{
	const uint32_t a = src >> 24;
	if(a == 0) return dst;
	if(a == 255) return src;
	const uint32_t inv = 256 - a;
	uint32_t out = 0;
	for(int sh = 0; sh < 32; sh += 8) {
		const uint32_t s = (src >> sh) & 0xFF, d = (dst >> sh) & 0xFF;
		out |= ((s + ((inv * d) >> 8)) & 0xFF) << sh;
	}
	return out;
}

// Copie de DrawTile pour une image de couche : les pixels translucides sont fusionnes par DioOver (le destinataire peut
// etre transparent), les tuiles opaques ecrivent un pixel d'alpha 255.
template<uint32_t scale>
void HdNesPack<scale>::DrawTileLayer(HdPpuTileInfo &tileInfo, HdPackTileInfo &hdPackTileInfo, uint32_t *outputBuffer, uint32_t screenWidth)
{
	if(hdPackTileInfo.IsFullyTransparent) {
		return;
	}
	const bool hMirror = tileInfo.HorizontalMirroring != hdPackTileInfo.IsHFlip;
	const bool vMirror = tileInfo.VerticalMirroring != hdPackTileInfo.IsVFlip;
	uint32_t *bitmapData = hdPackTileInfo.HdTileData.data();
	uint32_t tileWidth = 8 * scale;
	uint8_t tileOffsetX = hMirror ? 7 - tileInfo.OffsetX : tileInfo.OffsetX;
	uint32_t bitmapOffset = (tileInfo.OffsetY * scale) * tileWidth + tileOffsetX * scale;
	int32_t bitmapSmallInc = 1;
	int32_t bitmapLargeInc = tileWidth - scale;
	if(hMirror) {
		bitmapOffset += scale - 1;
		bitmapSmallInc = -1;
		bitmapLargeInc = tileWidth + scale;
	}
	if(vMirror) {
		bitmapOffset += tileWidth * (scale - 1);
		bitmapLargeInc = (hMirror ? (int32_t)scale : -(int32_t)scale) - (int32_t)tileWidth;
	}

	const bool transparent = hdPackTileInfo.HasTransparentPixels;
	for(uint32_t y = 0; y < scale; y++) {
		for(uint32_t x = 0; x < scale; x++) {
			uint32_t rgbValue = hdPackTileInfo.Brightness == 255 ? bitmapData[bitmapOffset] : AdjustBrightness((uint8_t*)(bitmapData + bitmapOffset), hdPackTileInfo.Brightness);
			if(!transparent) {
				*outputBuffer = rgbValue | 0xFF000000u;
			} else if((bitmapData[bitmapOffset] & 0xFF000000) == 0xFF000000) {
				*outputBuffer = rgbValue;
			} else if(bitmapData[bitmapOffset] & 0xFF000000) {
				*outputBuffer = DioOver(*outputBuffer, rgbValue);
			}
			outputBuffer++;
			bitmapOffset += bitmapSmallInc;
		}
		bitmapOffset += bitmapLargeInc;
		outputBuffer += screenWidth - scale;
	}
}

// G6d : meme parcours que DrawTileLayer, mais ecrit la normale de la tile (RGB, alpha 255) ; un retournement de la tile inverse x (R) ou y (G).
template<uint32_t scale>
void HdNesPack<scale>::DrawNormalLayer(HdPpuTileInfo &tileInfo, HdPackTileInfo &hdPackTileInfo, uint32_t *outputBuffer, uint32_t screenWidth)
{
	if(hdPackTileInfo.NormalTileData.size() < (size_t)(8 * scale) * (8 * scale)) {
		return;
	}
	const bool hMirror = tileInfo.HorizontalMirroring != hdPackTileInfo.IsHFlip;
	const bool vMirror = tileInfo.VerticalMirroring != hdPackTileInfo.IsVFlip;
	const uint32_t *bitmapData = hdPackTileInfo.NormalTileData.data();
	uint32_t tileWidth = 8 * scale;
	uint8_t tileOffsetX = hMirror ? 7 - tileInfo.OffsetX : tileInfo.OffsetX;
	uint32_t bitmapOffset = (tileInfo.OffsetY * scale) * tileWidth + tileOffsetX * scale;
	int32_t bitmapSmallInc = 1;
	int32_t bitmapLargeInc = tileWidth - scale;
	if(hMirror) {
		bitmapOffset += scale - 1;
		bitmapSmallInc = -1;
		bitmapLargeInc = tileWidth + scale;
	}
	if(vMirror) {
		bitmapOffset += tileWidth * (scale - 1);
		bitmapLargeInc = (hMirror ? (int32_t)scale : -(int32_t)scale) - (int32_t)tileWidth;
	}
	for(uint32_t y = 0; y < scale; y++) {
		for(uint32_t x = 0; x < scale; x++) {
			const uint32_t v = bitmapData[bitmapOffset];
			if(v & 0xFF000000u) {
				uint32_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF;
				const uint32_t b = v & 0xFF;
				if(hMirror) r = 255 - r;
				if(vMirror) g = 255 - g;
				*outputBuffer = 0xFF000000u | (r << 16) | (g << 8) | b;
			}
			outputBuffer++;
			bitmapOffset += bitmapSmallInc;
		}
		bitmapOffset += bitmapLargeInc;
		outputBuffer += screenWidth - scale;
	}
}

// Meme logique de choix que DrawPixel (bloc scale x scale d'un pixel NES), repartie sur les couches.
// L0 et L1 sont dessinees meme quand la tile de fond est opaque (elles sont alors cachees : l'identite est inchangee,
// et le diorama pourra les montrer si la tile est decalee en profondeur).
// Tile2Obj (doc 53) : cherche chaque motif (group=, grid=) sur l'ecran. Les coins de tile seuls peuvent ancrer un motif (OffsetX/OffsetY = 0).
// Pour chaque motif trouve : ses pixels sont masques dans le diorama (la tile n'est plus dessinee dans les couches 3D) et une instance
// de modele est creee au centre du motif, base sur son bord bas (moins elevation=). Les conditions [cond] sont evaluees ici.
template<uint32_t scale>
void HdNesPack<scale>::DetectTile2Obj()
{
	_dioT2oHide.assign(256 * 240, 0);
	_dioT2oRules.clear();
	if(!_hdScreenInfo) return;
	for(const HdDioramaTile2ObjRule& r : _hdData->Tile2ObjRules) {
		bool ok = true;
		for(HdPackCondition* c : r.Conditions) {
			if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; }
		}
		if(!ok || r.Parts.empty() || r.Group.empty()) continue;
		const int gw = r.GridW, gh = r.GridH;
		for(int y = 0; y + 8 * gh <= 240; y++) {
			for(int x = 0; x + 8 * gw <= 256; x++) {
				const HdPpuPixelInfo& p0 = _hdScreenInfo->ScreenTiles[((uint32_t)y << 8) | (uint32_t)x];
				if(p0.Tile.OffsetX != 0 || p0.Tile.OffsetY != 0) continue;
				bool match = true;
				for(int j = 0; j < gh && match; j++) {
					for(int i = 0; i < gw && match; i++) {
						const HdPpuPixelInfo& q = _hdScreenInfo->ScreenTiles[((uint32_t)(y + 8 * j) << 8) | (uint32_t)(x + 8 * i)];
						if(q.Tile.OffsetX != 0 || q.Tile.OffsetY != 0 || (uint32_t)q.Tile.TileIndex != r.Group[j * gw + i]) match = false;
						else if(!r.AnyPalette && q.Tile.PaletteColors != r.Palette) match = false;
					}
				}
				if(!match) continue;
				for(int yy = 0; yy < 8 * gh; yy++) for(int xx = 0; xx < 8 * gw; xx++) _dioT2oHide[((y + yy) << 8) | (x + xx)] = 1;
				for(const HdDioramaTile2ObjPart& part : r.Parts) { // une instance par materiau (texture propre)
				HdDioramaObjectRule o;
				o.Mesh = part.Mesh; o.Tex = part.Tex; o.Color = r.Color;
				o.X = (float)x + 4.0f * (float)gw;
				o.Y = (float)(y + 8 * gh) - r.Elevation;
				o.Z = 0.0f;
				o.Scale = r.Scale; o.RotY = r.RotY; o.RotX = r.RotX; o.RotZ = r.RotZ; o.Radius = r.Radius;
				o.FromTile2Obj = true; o.BottomY = (float)(y + 8 * gh); o.Elevation = r.Elevation; o.CastShadow = r.CastShadow; o.Lit = r.Lit; o.Bright = r.Bright;
				_dioT2oRules.push_back(o);
				}
			}
		}
	}
}

// <Sprite2Obj> : chaque silhouette (sprites qui se touchent) qui contient au moins une tile de la liste devient un objet 3D,
// centre en x sur la silhouette, pieds en bas ; retourne en miroir si la plupart de ses tiles reconnues le sont (facing=on).
template<uint32_t scale>
void HdNesPack<scale>::DetectSprite2Obj()
{
	_s2oMask.assign((size_t)256 * 240, 0);
	if(!_hdScreenInfo) return;
	std::vector<uint8_t> seen((size_t)256 * 240, 0);
	std::vector<int> stack, comp;
	const uint32_t frame = _hdScreenInfo->FrameNumber;
	for(size_t ri = 0; ri < _hdData->Sprite2ObjRules.size() && ri < 255; ri++) {
		const HdDioramaTile2ObjRule& r = _hdData->Sprite2ObjRules[ri];
		int inst = 0; // numero de la silhouette de cette regle dans l'image
		if(r.Parts.empty() || r.Group.empty()) continue;
		std::fill(seen.begin(), seen.end(), (uint8_t)0); // silhouettes deja vues par cette regle ; _s2oMask : deja prises par une regle precedente
		auto matches = [&](const HdPpuTileInfo& t) {
			if(!r.AnyPalette && r.Palette != t.PaletteColors) return false;
			for(uint32_t h : r.Group) { if(h == (uint32_t)t.TileIndex) return true; }
			return false;
		};
		for(int y = 0; y < 240; y++) {
			for(int x = 0; x < 256; x++) {
				const int i0 = (y << 8) | x;
				if(seen[i0] || _s2oMask[i0]) continue;
				const HdPpuPixelInfo& p = _hdScreenInfo->ScreenTiles[i0];
				int mk = -1;
				for(int k = 0; k < p.SpriteCount && mk < 0; k++) if(matches(p.Sprite[k])) mk = k;
				if(mk < 0) continue;
				HdPpuTileInfo condTile = p.Sprite[mk]; // tile de reference pour les conditions ([hmirror], [vmirror], ...)
				// silhouette : pixels couverts par des sprites qui se touchent (4-voisins)
				int minX = x, maxX = x, minY = y, maxY = y, nMatch = 0, nH = 0, nV = 0;
				stack.clear(); comp.clear(); stack.push_back(i0); seen[i0] = 1;
				while(!stack.empty()) {
					const int c = stack.back(); stack.pop_back(); comp.push_back(c);
					const int cy = c >> 8, cx = c & 255;
					minX = std::min(minX, cx); maxX = std::max(maxX, cx); minY = std::min(minY, cy); maxY = std::max(maxY, cy);
					const HdPpuPixelInfo& q = _hdScreenInfo->ScreenTiles[c];
					for(int k = 0; k < q.SpriteCount; k++) {
						if(!matches(q.Sprite[k])) continue;
						nMatch++;
						if(q.Sprite[k].HorizontalMirroring) nH++;
						if(q.Sprite[k].VerticalMirroring) nV++;
					}
					const int nr[4] = { cy - 1, cy + 1, cy, cy }, nx[4] = { cx, cx, cx - 1, cx + 1 };
					for(int k = 0; k < 4; k++) {
						if(nr[k] < 0 || nr[k] >= 240 || nx[k] < 0 || nx[k] > 255) continue;
						const int j = (nr[k] << 8) | nx[k];
						if(seen[j] || _s2oMask[j] || _hdScreenInfo->ScreenTiles[j].SpriteCount == 0) continue;
						seen[j] = 1; stack.push_back(j);
					}
				}
				// conditions [ ] de la regle, evaluees pour cette silhouette (avec sa tile : hmirror, vmirror...)
				bool condOk = true;
				for(HdPackCondition* c : r.Conditions) {
					if(!c->CheckCondition(x, y, &condTile)) { condOk = false; break; }
				}
				if(!condOk) continue; // la silhouette reste libre pour une regle suivante
				for(int c : comp) _s2oMask[c] = (uint8_t)(ri + 1);
				const float foot = (float)(maxY + 1);
				// glTF anime : palette d'os au temps du personnage (id=), puis sommets deformes dans un tampon propre a la silhouette
				const bool animated = r.Gltf && r.AnimIndex >= 0 && r.PartTris.size() == r.Parts.size();
				if(animated) {
					// temps du clip par personnage (id=) et fondu entre clips (blend=)
					S2oPose(r.Id.empty() ? ("#" + std::to_string(ri)) : r.Id, r, frame);
					_s2oVersion++;
				}
				for(size_t pi = 0; pi < r.Parts.size(); pi++) { // une instance par materiau
					const HdDioramaTile2ObjPart& part = r.Parts[pi];
					HdDioramaObjectRule o;
					o.Mesh = part.Mesh; o.Tex = part.Tex; o.Color = r.Color;
					if(animated) {
						std::shared_ptr<vector<float>>& buf = _s2oBuf[((uint64_t)ri << 32) | ((uint64_t)inst << 8) | (uint64_t)pi];
						if(!buf) buf = std::make_shared<vector<float>>();
						HdGltfSkinPart(*r.Gltf, r.PartTris[pi], &_s2oPal, *buf);
						o.Mesh = buf; o.MeshVersion = _s2oVersion;
					}
					o.X = (float)(minX + maxX + 1) * 0.5f;
					o.Y = foot - r.Elevation;
					o.Z = _hdData->Diorama.Z[3]; // vue side : plan des sprites (L3)
					o.Scale = r.Scale; o.RotY = r.RotY; o.RotX = r.RotX; o.RotZ = r.RotZ; o.Radius = r.Radius;
					o.FromTile2Obj = true; o.BottomY = foot; o.Elevation = r.Elevation; o.CastShadow = r.CastShadow; o.Lit = r.Lit; o.Bright = r.Bright;
					o.MirrorX = r.Facing && nH * 2 > nMatch;
					o.MirrorY = r.Facing && nV * 2 > nMatch;
					if(o.MirrorY) { // retourne en haut / bas : le modele pend sous son origine, qui passe en haut de la silhouette
						o.Y = (float)minY - r.Elevation;
						o.Elevation = r.Elevation + (foot - (float)minY);
					}
					_dioT2oRules.push_back(o);
				}
				inst++;
			}
		}
	}
}

// <DioramaObject>...,anchor=bgN : objet du monde. Position = pixel NES du PNG du fond de priorite N moins le defilement de ce fond
// a cette image (ligne 120) ; hauteur de base = surface ou le fond est dessine (sol, ou dessus du groupe dio=gK), ou base=.
template<uint32_t scale>
void HdNesPack<scale>::DetectWorldObj()
{
	if(!_hdScreenInfo) return;
	HdBgConfig cfg[5 * HdNesPack::PriorityLevelsPerLayer];
	ComputeLineBgConfig(_hdScreenInfo->ScreenTiles[120 << 8], 120, cfg);
	const uint32_t frame = _hdScreenInfo->FrameNumber;
	for(size_t ri = 0; ri < _hdData->WorldObjRules.size(); ri++) {
		const HdDioramaTile2ObjRule& r = _hdData->WorldObjRules[ri];
		if(r.Parts.empty()) continue;
		bool ok = true;
		for(HdPackCondition* c : r.Conditions) {
			if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; }
		}
		if(!ok) continue;
		const HdBgConfig* bc = nullptr; const HdBackgroundInfo* bi = nullptr;
		for(int layer = 0; layer < 5 && !bc; layer++) {
			for(int i = 0; i < _activeBgCount[layer]; i++) {
				const HdBgConfig& c = cfg[layer * HdNesPack::PriorityLevelsPerLayer + i];
				if(c.BackgroundIndex < 0) continue;
				const HdBackgroundInfo& b = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex];
				if((int)b.Priority == r.AnchorPrio) { bc = &c; bi = &b; break; }
			}
		}
		const bool diag = r.Debug && (frame % 120) == 0; // debug=on : [HD-WORLDOBJ] toutes les 2 secondes (outil de placement)
		if(!bc) {
			if(diag) MessageManager::Log("[HD-WORLDOBJ] objet " + std::to_string(ri + 1) + " : fond bg" + std::to_string(r.AnchorPrio) + " inactif a cette image");
			continue; // fond d'ancrage inactif a cette image : objet absent
		}
		const float sx = r.WX - ((float)bi->Left + (float)bc->BgScrollX);
		const float sy = r.WY - ((float)bi->Top + (float)bc->BgScrollY);
		if(diag) {
			const float cx = (float)bi->Left + (float)bc->BgScrollX + 128.0f, cy = (float)bi->Top + (float)bc->BgScrollY + 120.0f;
			char b[256];
			snprintf(b, sizeof(b), "[HD-WORLDOBJ] objet %d : centre de l'ecran = x=%.0f,y=%.0f (PNG NES ; HD %.0f,%.0f) ; objet x=%.0f,y=%.0f -> ecran %.0f,%.0f%s",
				(int)ri + 1, cx, cy, cx * (float)scale, cy * (float)scale, r.WX, r.WY, sx, sy, (sx >= 0 && sx < 256 && sy >= 0 && sy < 240) ? " (a l'ecran)" : " (hors ecran)");
			MessageManager::Log(b);
		}
		const float m = 256.0f + r.Radius + (_dioFloorMargin > 0.0f ? _dioFloorMargin : 0.0f);
		if(sx < -m || sx > 256.0f + m || sy < -m || sy > 240.0f + m) continue; // loin de l'ecran
		float base = 0.0f;
		auto groupTop = [&](int g) { return (g >= 0 && (size_t)g < _hdData->DioramaDepthGroups.size()) ? _hdData->DioramaDepthGroups[g].Z + _hdData->DioramaDepthGroups[g].T * 8.0f : 0.0f; };
		if(r.BaseMode == -1) base = bi->DioTarget >= 4 ? groupTop(bi->DioTarget - 4) : 0.0f;
		else if(r.BaseMode == 1) base = groupTop(r.BaseGroup);
		else if(r.BaseMode == 2) base = r.BaseZ;
		const bool animated = r.Gltf && r.AnimIndex >= 0 && r.PartTris.size() == r.Parts.size();
		if(animated) {
			// temps du clip par personnage (id=) et fondu entre clips (blend=)
			S2oPose(r.Id.empty() ? ("#w" + std::to_string(ri)) : r.Id, r, frame);
			_s2oVersion++;
		}
		for(size_t pi = 0; pi < r.Parts.size(); pi++) {
			const HdDioramaTile2ObjPart& part = r.Parts[pi];
			HdDioramaObjectRule o;
			o.Mesh = part.Mesh; o.Tex = part.Tex; o.Color = r.Color;
			if(animated) {
				std::shared_ptr<vector<float>>& buf = _s2oBuf[((uint64_t)(0x8000u + ri) << 32) | (uint64_t)pi];
				if(!buf) buf = std::make_shared<vector<float>>();
				HdGltfSkinPart(*r.Gltf, r.PartTris[pi], &_s2oPal, *buf);
				o.Mesh = buf; o.MeshVersion = _s2oVersion;
			}
			const float elev = base + r.Elevation;
			o.X = sx; o.Y = sy - elev; o.Z = 0.0f;
			o.Scale = r.Scale; o.RotY = r.RotY; o.RotX = r.RotX; o.RotZ = r.RotZ; o.Radius = r.Radius;
			o.FromTile2Obj = true; o.BottomY = sy; o.Elevation = elev; o.CastShadow = r.CastShadow; o.Lit = r.Lit; o.Bright = r.Bright;
			_dioT2oRules.push_back(o);
		}
	}
}

// <Sprite2Obj> / objet du monde glTF anime : pose du personnage (id=) dans _s2oPal.
// Le temps du clip avance a animspeed= (fige avec animtime=). Quand le personnage change de clip (autre anim= ou animtime=),
// l'ancienne pose se fond dans la nouvelle pendant blend= secondes (defaut 0.2), l'ancien clip continuant de jouer pendant le fondu.
template<uint32_t scale>
void HdNesPack<scale>::S2oPose(const std::string& key, const HdDioramaTile2ObjRule& r, uint32_t frame)
{
	const SnesHdModelRig* rig = r.Gltf ? r.Gltf->Rig.get() : nullptr;
	if(!rig) { _s2oPal.clear(); return; }
	S2oAnimState& st = _s2oTime[key];
	if(!st.Has || st.Frame != frame) {
		const float dt = st.Has ? (float)std::min<uint32_t>(frame - st.Frame, 4u) / 60.0f : 0.0f;
		const void* model = r.Gltf.get();
		const bool changed = st.Has && (st.Model != model || st.Anim != r.AnimIndex || st.Frozen != r.AnimTime);
		if(changed) {
			const float dur = r.Blend >= 0.0f ? r.Blend : 0.2f;
			if(st.Model == model && dur > 0.0f) {
				// le clip quitte devient le clip "precedent" (au milieu d'un fondu : on repart de celui en cours)
				st.PrevAnim = st.Anim; st.PrevTime = st.Time; st.PrevFrozen = st.Frozen; st.PrevSpeed = st.Speed;
				st.BlendDur = dur; st.BlendLeft = dur;
			} else {
				st.BlendLeft = 0.0f; // autre modele : coupure (squelettes differents)
			}
		}
		if(st.BlendLeft > 0.0f) {
			st.BlendLeft -= dt;
			if(st.PrevFrozen < 0.0f) st.PrevTime += st.PrevSpeed * dt;
		}
		if(r.AnimTime < 0.0f) st.Time += r.AnimSpeed * dt; // animtime= : le temps du personnage n'avance pas (reprise sans saut)
		st.Has = true; st.Frame = frame; st.Model = model; st.Anim = r.AnimIndex; st.Frozen = r.AnimTime; st.Speed = r.AnimSpeed;
	}
	const float curT = r.AnimTime >= 0.0f ? r.AnimTime : st.Time;
	if(st.BlendLeft > 0.0f && st.BlendDur > 0.0f) {
		const float w = 1.0f - st.BlendLeft / st.BlendDur;
		const float prevT = st.PrevFrozen >= 0.0f ? st.PrevFrozen : st.PrevTime;
		rig->EvaluateBlend(st.PrevAnim, prevT, r.AnimIndex, curT, w, _s2oPal);
	} else {
		rig->Evaluate(r.AnimIndex, curT, _s2oPal);
	}
}

template<uint32_t scale>
void HdNesPack<scale>::DrawPixelLayers(uint32_t x, uint32_t y, HdPpuPixelInfo &pixelInfo, size_t offset, uint32_t screenWidth, const HdBgConfig* lineCfg)
{
	const uint32_t pixelIndex = (y << 8) | x;
	HdPackTileInfo* hdPackTileInfo = _resolvedBg[pixelIndex];
	HdPackTileInfo* const* sprites = _resolvedSprites.data() + (pixelIndex << 2);
	uint32_t* l0 = _dioL[0].data() + offset;
	uint32_t* l1 = _dioL[1].data() + offset;
	uint32_t* l2 = _dioL[2].data() + offset;
	uint32_t* l3 = _dioL[3].data() + offset;
	// G3c : dans une zone off camera, sprites et tiles vont dans la couche fixe (meme ordre de dessin, donc meme resultat a plat)
	// Tiles marquees par un bloc <DioramaOffCamera>{ } : meme couche fixe, ou qu'elles soient a l'ecran (l4 nul = pas de couche fixe).
	uint32_t* const l4 = _dioL4Rows ? _dioL4.data() + offset : nullptr;
	if(l4 && _dioOffMask[pixelIndex]) {
		l1 = l2 = l3 = l4;
	}
	// G3e : groupe de profondeur de la tile de fond (bloc : marque de la tile ; rect= : position). Sprites : voir spriteTgt (G7, bloc uniquement).
	uint32_t* tileTarget = l2;
	uint32_t* maskTarget = nullptr; // masque du groupe de la tile (topDown)
	int worldGroup = -1; // doc 53 : groupe avec worldtex=on, pour la tile courante
	int tileNrm = 0; // G6d : tampon de normales de la couche de la tile (0 = L2, 2 a 5 = groupes)
	if(!_hdData->DioramaDepthGroups.empty() && l2 != l4) {
		int g = hdPackTileInfo ? (int)hdPackTileInfo->DioDepthGroup : 0;
		if(g == 0) {
			g = _dioDepthMask[pixelIndex];
		}
		if(g > 0 && g <= DioGroups && !(hdPackTileInfo && hdPackTileInfo->DioOffCamera)) {
			tileTarget = _dioG[g - 1].data() + offset;
			if(_dioGMask[g - 1].size() == _dioG[g - 1].size()) maskTarget = _dioGMask[g - 1].data() + offset;
			tileNrm = 1 + g;
			if((size_t)(g - 1) < _hdData->DioramaDepthGroups.size() && _hdData->DioramaDepthGroups[g - 1].WorldTex) worldGroup = g - 1;
			_dioGLines[g - 1][y] = 1;
		}
	}
	const bool hasSprite = pixelInfo.SpriteCount > 0;
	if(hasSprite && l1 != l4) { _dioLLines[0][y] = 1; _dioLLines[1][y] = 1; } // G3h : la ligne NES porte un sprite (L1 / L3)
	const bool renderOriginalTiles = ((_hdData->OptionFlags & (int)HdPackOptions::DontRenderOriginalTiles) == 0);
	// <background> dans le diorama (doc 50) : memes dessins que DrawPixel (defilement, tileX, fusion, luminosite, animation), mais chaque fond
	// est ecrit dans la couche voulue : cible par defaut selon la bande de priorite, ou groupe <DioramaDepth> / option dio=.
	const size_t bgGroups = std::min<size_t>(_hdData->DioramaDepthGroups.size(), (size_t)DioGroups);
	auto drawBgs = [&](int cls, uint32_t* defTarget) {
		if(!lineCfg) return;
		for(int i = 0; i < _activeBgCount[cls]; i++) {
			const HdBgConfig& c = lineCfg[cls * HdNesPack::PriorityLevelsPerLayer + i];
			if(c.BackgroundIndex < 0) continue;
			const int t = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex].DioTarget;
			if(t == -2) continue;
			if(_dioGpuBgN > 0 && IsDioGpuBg(c.BgPriority, c.BackgroundIndex)) continue; // dessine par le GPU (L0 ou groupe sans epaisseur)
			uint32_t* tgt = defTarget;
			if(t >= 0 && t <= 3) {
				tgt = t == 0 ? l0 : (t == 1 ? l1 : (t == 2 ? l2 : l3));
			} else if(t >= 4 && t <= 3 + DioGroups && (size_t)(t - 4) < bgGroups && !(l4 && _dioOffMask[pixelIndex]) && _dioG[t - 4].size() > offset) {
				tgt = _dioG[t - 4].data() + offset;
				_dioGLines[t - 4][y] = 1;
			}
			if((tgt == l1 || tgt == l3) && tgt != l4) { _dioLLines[0][y] = 1; _dioLLines[1][y] = 1; } // L1 / L3 : lignes a renvoyer au GPU
			DrawBackgroundLayer(c, x, y, tgt, screenWidth);
		}
	};

	if(_dioL0Clear) {
		for(uint32_t i = 0; i < scale; i++) {
			uint32_t* p = l0 + (size_t)i * screenWidth;
			for(uint32_t j = 0; j < scale; j++) p[j] = 0u;
		}
	} else {
		DrawColor(_palette[pixelInfo.Tile.PpuBackgroundColor] | 0xFF000000u, l0, screenWidth);
	}
	drawBgs(0, l0);

	// G7 : sprite HD dont le <tile> est dans un bloc <DioramaDepth>{ } : dessine dans le groupe (plan de profondeur du groupe) au lieu de L1 / L3
	auto spriteTgt = [&](const HdPackTileInfo* st, uint32_t* def) -> uint32_t* {
		if(!st || st->DioDepthGroup == 0 || st->DioOffCamera || _hdData->DioramaDepthGroups.empty() || l2 == l4) return def;
		const int g = (int)st->DioDepthGroup;
		if(g > DioGroups || (size_t)(g - 1) >= bgGroups || _dioG[g - 1].size() <= offset) return def;
		_dioGLines[g - 1][y] = 1;
		static bool sprLogged = false;
		if(!sprLogged) {
			sprLogged = true;
			MessageManager::Log("[HD-DIORAMA] G7 sprite dans un groupe <DioramaDepth> : le sprite est dessine dans le groupe " + std::to_string(g) + " (et non dans L1 / L3)");
		}
		return _dioG[g - 1].data() + offset;
	};

	int lowestBgSprite = 999;
	if(hasSprite) {
		for(int k = pixelInfo.SpriteCount - 1; k >= 0; k--) {
			if(pixelInfo.Sprite[k].BackgroundPriority) {
				if(pixelInfo.Sprite[k].SpriteColorIndex != 0) {
					lowestBgSprite = k;
				}
				if(!_s2oMask.empty() && S2oHidden(pixelIndex, pixelInfo.Sprite[k])) continue; // <Sprite2Obj> : remplace par un objet 3D
				if(sprites[k]) {
					if(!sprites[k]->DioIgnore) DrawTileLayer(pixelInfo.Sprite[k], *sprites[k], (l4 && sprites[k]->DioOffCamera) ? l4 : spriteTgt(sprites[k], l1), screenWidth);
				} else if(pixelInfo.Sprite[k].SpriteColorIndex != 0) {
					DrawColor(_palette[pixelInfo.Sprite[k].SpriteColor] | 0xFF000000u, l1, screenWidth);
				}
			}
		}
	}

	drawBgs(1, l1);

	if(hdPackTileInfo) {
		if(!hdPackTileInfo->DioIgnore && !(pixelIndex < _dioT2oHide.size() && _dioT2oHide[pixelIndex])) DrawTileLayer(pixelInfo.Tile, *hdPackTileInfo, (l4 && hdPackTileInfo->DioOffCamera) ? l4 : tileTarget, screenWidth);
		// masque de profondeur (topDown) : seuls les pixels de tile opaques, pas le fond ni la couleur de fond
		if(maskTarget && hdPackTileInfo && !hdPackTileInfo->DioIgnore) {
			for(uint32_t yy = 0; yy < scale; yy++) for(uint32_t xx = 0; xx < scale; xx++) if(tileTarget[yy * screenWidth + xx] >> 24) maskTarget[yy * screenWidth + xx] = 0xFF000000u;
		}
		// doc 53 : worldtex=on : le fond L0 est recalcule ici aux memes coordonnees (meme DrawBackgroundLayer, donc memes ramx / ramy),
		// puis copie sur les pixels opaques du groupe. La forme de la tile sert de masque.
		if(worldGroup >= 0 && lineCfg && !hdPackTileInfo->DioIgnore && !(l4 && hdPackTileInfo->DioOffCamera)) {
			const HdBgConfig* wc = nullptr;
			for(int i = 0; i < _activeBgCount[0] && !wc; i++) {
				const HdBgConfig& c = lineCfg[i];
				if(c.BackgroundIndex < 0) continue;
				const int t = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex].DioTarget;
				if(t == -1 || t == 0) wc = &c;
			}
			if(wc) {
				if(_dioWorldTmp.size() != _dioG[worldGroup].size()) _dioWorldTmp.assign(_dioG[worldGroup].size(), 0u);
				uint32_t* tmp = _dioWorldTmp.data() + offset;
				for(uint32_t yy = 0; yy < scale; yy++) for(uint32_t xx = 0; xx < scale; xx++) tmp[yy * screenWidth + xx] = 0u;
				DrawBackgroundLayer(*wc, x, y, tmp, screenWidth);
				for(uint32_t yy = 0; yy < scale; yy++) for(uint32_t xx = 0; xx < scale; xx++) {
					uint32_t& d = tileTarget[yy * screenWidth + xx];
					const uint32_t s = tmp[yy * screenWidth + xx];
					if(d != 0 && (s >> 24) != 0) d = s | 0xFF000000u;
				}
			}
		}
		// G6d : normales de la tile (couches normales seulement)
		if(!_dioNrm[0].empty() && hdPackTileInfo->NormalBitmap && !hdPackTileInfo->NormalTileData.empty() && !hdPackTileInfo->DioIgnore && tileTarget != l4 && !(l4 && hdPackTileInfo->DioOffCamera)) {
			DrawNormalLayer(pixelInfo.Tile, *hdPackTileInfo, _dioNrm[tileNrm].data() + offset, screenWidth);
			_dioNrmLines[tileNrm][y] = 1;
		}
	} else if(renderOriginalTiles && pixelInfo.Tile.BgColorIndex != 0) {
		DrawColor(_palette[pixelInfo.Tile.BgColor] | 0xFF000000u, tileTarget, screenWidth);
	}
	drawBgs(2, tileTarget);

	if(hasSprite) {
		for(int k = pixelInfo.SpriteCount - 1; k >= 0; k--) {
			if(!pixelInfo.Sprite[k].BackgroundPriority && lowestBgSprite > k) {
				if(!_s2oMask.empty() && S2oHidden(pixelIndex, pixelInfo.Sprite[k])) continue; // <Sprite2Obj> : remplace par un objet 3D
				uint32_t* sprT = l3; // G7 : cible du sprite (L3, ou son groupe <DioramaDepth>)
				if(sprites[k]) {
					sprT = spriteTgt(sprites[k], l3);
					if(!sprites[k]->DioIgnore) DrawTileLayer(pixelInfo.Sprite[k], *sprites[k], (l4 && sprites[k]->DioOffCamera) ? l4 : sprT, screenWidth);
				} else if(pixelInfo.Sprite[k].SpriteColorIndex != 0) {
					DrawColor(_palette[pixelInfo.Sprite[k].SpriteColor] | 0xFF000000u, l3, screenWidth);
				}
				// G6d : normales du sprite (L3)
				if(!_dioNrm[1].empty() && sprT == l3 && sprites[k] && sprites[k]->NormalBitmap && !sprites[k]->NormalTileData.empty() && l3 != l4 && !sprites[k]->DioIgnore && !(l4 && sprites[k]->DioOffCamera)) {
					DrawNormalLayer(pixelInfo.Sprite[k], *sprites[k], _dioNrm[1].data() + offset, screenWidth);
					_dioNrmLines[1][y] = 1;
				}
				// G6c : silhouette du sprite dans le masque des ombres (sprite d'une couche normale seulement)
				if(!_dioS.empty() && l3 != l4 && !(sprites[k] && (sprites[k]->DioIgnore || (l4 && sprites[k]->DioOffCamera)))) {
					int sr = -1;
					if(sprites[k] && sprites[k]->DioShadow) {
						sr = (int)sprites[k]->DioShadow - 1;
					} else {
						const vector<HdDioramaShadowRule>& srules = _hdData->DioramaShadowRules;
						for(size_t r = 0; r < srules.size(); r++) {
							const HdDioramaShadowRule& rr = srules[r];
							if(!rr.HasTile || !rr.MatchesTile(pixelInfo.Sprite[k])) continue;
							bool ok = true;
							for(HdPackCondition* c : rr.Conditions) {
								if(!c->CheckCondition((int)x, (int)y, &pixelInfo.Sprite[k])) { ok = false; break; }
							}
							if(ok) { sr = (int)r; break; }
						}
					}
					if(sr >= 0) {
						uint32_t* ms = _dioS.data() + offset;
						if(sprites[k]) DrawTileLayer(pixelInfo.Sprite[k], *sprites[k], ms, screenWidth);
						else if(pixelInfo.Sprite[k].SpriteColorIndex != 0) DrawColor(_palette[pixelInfo.Sprite[k].SpriteColor] | 0xFF000000u, ms, screenWidth);
						_dioSLines[y] = 1;
						if(_dioSRule[y] < 0 || sr < _dioSRule[y]) _dioSRule[y] = sr;
					}
				}
			}
		}
	}
	drawBgs(3, l3);
	drawBgs(4, l3);
}

// Remplit les 4 couches (en parallele, une ligne NES par tache comme la passe 2) puis compare leur composition a plat
// avec l'image du processeur juste apres la passe 2 : couleurs RGB seulement, alpha ignore.
template<uint32_t scale>
void HdNesPack<scale>::RunDioramaLayers(HdScreenInfo* hdScreenInfo, const uint32_t* outputBuffer, uint32_t screenWidth, uint32_t yStart, uint32_t yEnd, uint32_t xStart, uint32_t xEnd, bool compare)
{
	const auto t0 = std::chrono::steady_clock::now();
	const uint32_t hdScale = GetScale();
	const size_t pixels = (size_t)screenWidth * (yEnd - yStart) * hdScale;
	// Skybox derriere les fonds : si une regle <DioramaSkybox> est vraie (et L0 sans epaisseur), L0 commence transparente au lieu de la
	// couleur de fond PPU ; les pixels transparents des <background> laissent alors voir le ciel.
	_dioL0Clear = false;
	if(!_hdData->DioramaSkyboxRules.empty() && _hdData->Diorama.T[0] <= 0.0f) {
		for(const HdDioramaSkyboxRule& r : _hdData->DioramaSkyboxRules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
			if(ok) { _dioL0Clear = true; break; }
		}
	}
	// Fonds de L0 dessines par le GPU (doc 52) : si tous les fonds de L0 ont le meme defilement sur toutes les lignes (ramx= / ramy=,
	// ou fond fixe), le GPU les dessine comme un plan repete a la profondeur de L0 (marges infinies, aucun envoi d'image) ; sinon,
	// dessin par le processeur comme avant. Pas pendant le controle d'identite GPU / processeur (compare).
	// Etendu (2026-11-21) aux fonds des groupes <DioramaDepth> sans epaisseur, toutes bandes de priorite : plan a la profondeur du groupe,
	// plus d'image de groupe a construire ni a envoyer. 8 fonds au plus. Les effets lies au z par pixel (<2DOverlay>, <video>,
	// <2DPerspective>) gardent le dessin par le processeur ; <DayNight> n'en a pas besoin (teinte dans la scene).
	_dioGpuBgN = 0;
	int nActAll = 0;
	for(int cls = 0; cls < 5; cls++) nActAll += _activeBgCount[cls];
	const bool zNeeded = !_hdData->Videos.empty() || !_hdData->Overlays.empty() || !_hdData->Perspectives.empty();
	if(_dioGpuOk && nActAll > 0 && !zNeeded && !_hdData->HasIgnore2D && yEnd > yStart && yEnd <= 240) {
		// ligne de reference dans la zone de jeu (aux 2/3 de l'image) : une barre d'etat a defilement separe (Zelda 2, SMB) ne la fausse pas
		HdBgConfig first[50], lc[50];
		const uint32_t yRef = yStart + (yEnd - yStart) * 2 / 3;
		ComputeLineBgConfig(hdScreenInfo->ScreenTiles[yRef << 8], (uint8_t)yRef, first);
		const size_t nGroups = std::min<size_t>(_hdData->DioramaDepthGroups.size(), (size_t)DioGroups);
		bool allOk = true;
		bool cand[50] = {};
		int nCand = 0;
		for(int cls = 0; cls < 5 && allOk; cls++) {
			for(int i = 0; i < _activeBgCount[cls] && allOk; i++) {
				const int k = cls * HdNesPack::PriorityLevelsPerLayer + i;
				const HdBgConfig& c = first[k];
				if(c.BackgroundIndex < 0) continue;
				const HdBackgroundInfo& bi = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex];
				const int t = bi.DioTarget;
				const bool toL0 = (t == 0 || (t == -1 && cls == 0)) && _hdData->Diorama.T[0] <= 0.0f;
				const bool toGroup = !_dioTopDown && t >= 4 && (size_t)(t - 4) < nGroups && _hdData->DioramaDepthGroups[t - 4].T <= 0.0f; // side seulement
				if(!toL0 && !toGroup) continue; // autre plan (L1..L3, groupe epais) : dessin par le processeur
				const uint32_t pw = bi.Data ? bi.Data->Width : 0, ph = bi.Data ? bi.Data->Height : 0;
				if(bi.BlendMode != HdPackBlendMode::Alpha || pw == 0 || ph == 0 || (bi.TileX && (pw % hdScale) != 0) || (bi.TileY && (ph % hdScale) != 0) || // taille multiple de l'echelle : seulement pour un fond repete (periode en pixels NES)
					DioGpuTexCount(bi.Priority) > 4 || (!bi.TileX && bi.Left != 0) || (!bi.TileY && bi.Top != 0)) {
					if(toL0) { allOk = false; break; } // L0 : tout ou rien (les fonds de L0 se superposent dans une seule image)
					continue; // groupe : ce fond reste dessine par le processeur
				}
				if(nCand >= 8) continue;
				cand[k] = true; nCand++;
			}
		}
		if(allOk && nCand > 0) {
			// meme fond et meme defilement que la ligne de reference sur au moins 80 % des lignes (une barre d'etat a defilement fixe,
			// en general opaque, est toleree : le fond y prend le defilement de la zone de jeu) ; sinon le fond (ou, pour L0, tous les
			// fonds de L0) reste au processeur
			int same[50] = {};
			const int nLines = (int)(yEnd - yStart);
			for(uint32_t y = yStart; y < yEnd; y++) {
				ComputeLineBgConfig(hdScreenInfo->ScreenTiles[y << 8], (uint8_t)y, lc);
				for(int k = 0; k < 50; k++) {
					if(cand[k] && lc[k].BackgroundIndex == first[k].BackgroundIndex && lc[k].BgPriority == first[k].BgPriority && lc[k].BgScrollX == first[k].BgScrollX && lc[k].BgScrollY == first[k].BgScrollY) same[k]++;
				}
			}
			for(int k = 0; k < 50; k++) {
				if(!cand[k] || same[k] * 5 >= nLines * 4) continue;
				const int t = _hdData->BackgroundsByPriority[first[k].BgPriority][first[k].BackgroundIndex].DioTarget;
				if(t >= 4) { cand[k] = false; nCand--; }
				else { for(int q = 0; q < 50; q++) { if(cand[q] && _hdData->BackgroundsByPriority[first[q].BgPriority][first[q].BackgroundIndex].DioTarget < 4) { cand[q] = false; nCand--; } } }
			}
		}
		if(allOk && nCand > 0) {
			for(int k = 0; k < 50; k++) {
				if(!cand[k]) continue;
				const HdBgConfig& c = first[k];
				HdBackgroundInfo& bi = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex];
				DioGpuBg& g = _dioGpuBg[_dioGpuBgN++];
				g.Group = bi.DioTarget >= 4 ? bi.DioTarget - 4 : -1;
				g.Prio = c.BgPriority; g.Index = c.BackgroundIndex;
				g.Incl = bi.DioIncl;
				g.Pix = bi.data(); g.W = bi.Data->Width; g.H = bi.Data->Height;
				g.OffX = (float)(((int64_t)bi.Left + c.BgScrollX) * (int64_t)hdScale);
				g.OffY = (float)(((int64_t)bi.Top + c.BgScrollY) * (int64_t)hdScale);
				g.Bright = bi.Brightness >= 255 ? 1.0f : (float)bi.Brightness / 255.0f;
				g.TileX = bi.TileX; g.TileY = bi.TileY;
				g.TexPrio = bi.Priority; g.RawX = c.RawScrollX; g.RawY = c.RawScrollY; // <2DTexture> sur le GPU
			}
		}
	}
	// G3h : effacement reduit. L0 est entierement reecrite (pas d'effacement) ; L1 et L3 (sprites) : seulement les lignes NES
	// utilisees a l'image precedente, qui sont aussi marquees a renvoyer au GPU ; L2 en entier.
	{
		const uint64_t layerKey = ((uint64_t)screenWidth << 40) ^ ((uint64_t)yStart << 24) ^ ((uint64_t)hdScale << 8) ^ (uint64_t)pixels;
		bool fullClear = _dioLKey != layerKey;
		for(int k = 0; k < 4; k++) if(_dioL[k].size() != pixels) fullClear = true;
		for(int i = 0; i < 2; i++) if(_dioLLines[i].size() != 240 || _dioLDirty[i].size() != 240) fullClear = true;
		if(fullClear) {
			for(int k = 0; k < 4; k++) _dioL[k].assign(pixels, 0u);
			for(int i = 0; i < 2; i++) { _dioLLines[i].assign(240, 0); _dioLDirty[i].assign(240, 0); }
			_dioLFull = true;
			_dioLKey = layerKey;
		} else {
			std::fill(_dioL[2].begin(), _dioL[2].end(), 0u);
			for(int i = 0; i < 2; i++) {
				vector<uint32_t>& buf = _dioL[i == 0 ? 1 : 3];
				for(uint32_t y = 0; y < 240; y++) {
					if(!_dioLLines[i][y]) continue;
					_dioLLines[i][y] = 0;
					_dioLDirty[i][y] = 1;
					if(y < yStart) continue;
					const size_t r0 = (size_t)(y - yStart) * hdScale * screenWidth;
					if(r0 >= pixels) continue;
					std::fill(buf.begin() + r0, buf.begin() + std::min(pixels, r0 + (size_t)hdScale * screenWidth), 0u);
				}
			}
		}
	}

	// G3c : masque des zones off camera (construit une fois) et lignes HD de la couche fixe
	if(!_dioOffReady) {
		_dioOffReady = true;
		_dioOffMask.assign(256 * 240, 0);
		for(const HdDioramaOffRect& r : _hdData->DioramaOffRects) {
			for(int y = r.Y; y < r.Y + r.H && y < 240; y++) {
				for(int x = r.X; x < r.X + r.W && x < 256; x++) _dioOffMask[(y << 8) | x] = 1;
			}
		}
	}
	// G3e : groupes <DioramaDepth> (masque des zones rect= construit une fois, un groupe plus tardif l'emporte)
	const size_t groupCount = std::min<size_t>(_hdData->DioramaDepthGroups.size(), (size_t)DioGroups);
	if(!_dioDepthReady) {
		_dioDepthReady = true;
		_dioDepthMask.assign(256 * 240, 0);
		for(size_t g = 0; g < groupCount; g++) {
			const HdDioramaDepthGroup& grp = _hdData->DioramaDepthGroups[g];
			if(!grp.HasRect) continue;
			for(int yy = grp.Y; yy < grp.Y + grp.H && yy < 240; yy++) {
				for(int xx = grp.X; xx < grp.X + grp.W && xx < 256; xx++) _dioDepthMask[(yy << 8) | xx] = (uint8_t)(g + 1);
			}
		}
	}
	// G3g : on n'efface que les lignes NES utilisees a l'image precedente (le reste est deja a zero) ; effacement complet si la geometrie change
	const uint64_t groupKey = ((uint64_t)screenWidth << 40) ^ ((uint64_t)yStart << 24) ^ ((uint64_t)hdScale << 8) ^ (uint64_t)pixels;
	for(size_t g = 0; g < groupCount; g++) {
		if(_dioGKey != groupKey || _dioG[g].size() != pixels || _dioGLines[g].size() != 240 || _dioGMask[g].size() != pixels) {
			_dioG[g].assign(pixels, 0u);
			_dioGMask[g].assign(pixels, 0u);
			_dioGLines[g].assign(240, 0);
		} else {
			for(uint32_t y = 0; y < 240; y++) {
				if(!_dioGLines[g][y]) continue;
				_dioGLines[g][y] = 0;
				if(y < yStart) continue;
				const size_t r0 = (size_t)(y - yStart) * hdScale * screenWidth;
				if(r0 >= pixels) continue;
				std::fill(_dioG[g].begin() + r0, _dioG[g].begin() + std::min(pixels, r0 + (size_t)hdScale * screenWidth), 0u);
				std::fill(_dioGMask[g].begin() + r0, _dioGMask[g].begin() + std::min(pixels, r0 + (size_t)hdScale * screenWidth), 0u);
			}
		}
	}
	// G6c : masque des silhouettes (ombres des sprites) : meme effacement partiel que les groupes
	if(!_hdData->DioramaShadowRules.empty()) {
		if(_dioGKey != groupKey || _dioS.size() != pixels || _dioSLines.size() != 240) {
			_dioS.assign(pixels, 0u);
			_dioSLines.assign(240, 0);
		} else {
			for(uint32_t y = 0; y < 240; y++) {
				if(!_dioSLines[y]) continue;
				_dioSLines[y] = 0;
				if(y < yStart) continue;
				const size_t r0 = (size_t)(y - yStart) * hdScale * screenWidth;
				if(r0 >= pixels) continue;
				std::fill(_dioS.begin() + r0, _dioS.begin() + std::min(pixels, r0 + (size_t)hdScale * screenWidth), 0u);
			}
		}
		_dioSRule.assign(240, -1);
	}
	// G6d : tampons de normales : meme effacement partiel que le masque des ombres
	if(!_hdData->DioramaNormalPairs.empty()) {
		for(int i = 0; i < 2 + DioGroups; i++) {
			if(_dioGKey != groupKey || _dioNrm[i].size() != pixels || _dioNrmLines[i].size() != 240) {
				_dioNrm[i].assign(pixels, 0u);
				_dioNrmLines[i].assign(240, 0);
			} else {
				for(uint32_t y = 0; y < 240; y++) {
					if(!_dioNrmLines[i][y]) continue;
					_dioNrmLines[i][y] = 0;
					if(y < yStart) continue;
					const size_t r0 = (size_t)(y - yStart) * hdScale * screenWidth;
					if(r0 >= pixels) continue;
					std::fill(_dioNrm[i].begin() + r0, _dioNrm[i].begin() + std::min(pixels, r0 + (size_t)hdScale * screenWidth), 0u);
				}
			}
		}
	}
	_dioGKey = groupKey;
	_dioL4Y0 = 0; _dioL4Rows = 0;
	if(!_hdData->DioramaOffRects.empty() || _hdData->DioramaOffTiles) {
		int ny0 = 240, ny1 = 0;
		for(const HdDioramaOffRect& r : _hdData->DioramaOffRects) { ny0 = std::min(ny0, r.Y); ny1 = std::max(ny1, r.Y + r.H); }
		if(_hdData->DioramaOffTiles) { ny0 = 0; ny1 = 240; } // tiles marquees : n'importe ou a l'ecran, couche fixe entiere
		ny0 = std::max(ny0, (int)yStart); ny1 = std::min(ny1, (int)yEnd);
		if(ny1 > ny0) {
			_dioL4Y0 = (uint32_t)(ny0 - (int)yStart) * hdScale;
			_dioL4Rows = (uint32_t)(ny1 - ny0) * hdScale;
			if(_dioL4.size() != pixels) _dioL4.assign(pixels, 0u);
			std::fill(_dioL4.begin() + (size_t)_dioL4Y0 * screenWidth, _dioL4.begin() + (size_t)(_dioL4Y0 + _dioL4Rows) * screenWidth, 0u);
		}
	}

	bool anyBg = false;
	for(int c = 0; c < 5; c++) { if(_activeBgCount[c] > 0) anyBg = true; }
	auto drawLine = [&](uint32_t i) {
		HdBgConfig lineCfg[50];
		if(anyBg) ComputeLineBgConfig(hdScreenInfo->ScreenTiles[i << 8], (uint8_t)i, lineCfg);
		size_t idx = (size_t)(i - yStart) * screenWidth * hdScale;
		for(uint32_t j = xStart; j < xEnd; j++) {
			DrawPixelLayers(j, i, hdScreenInfo->ScreenTiles[i * 256 + j], idx, screenWidth, anyBg ? lineCfg : nullptr);
			idx += hdScale;
		}
	};
	const int nThreads = _pool.Size();
	if(nThreads <= 1 || yEnd <= yStart || yEnd - yStart < 16) {
		for(uint32_t i = yStart; i < yEnd; i++) drawLine(i);
	} else {
		_pool.Run(nThreads, [&](int t) {
			for(uint32_t i = yStart + (uint32_t)t; i < yEnd; i += (uint32_t)nThreads) drawLine(i);
		});
	}
	const double layersMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	_dioMsLayers += layersMs;
	if(!compare || _dioGpuBgN > 0) return; // fond dessine par le GPU : L0 n'a pas le fond, controle d'identite sans objet (l'image de controle n'est plus differente : pas de flash)

	// Comparaison : L0, puis L1, L2, L3 par-dessus
	uint32_t diff = 0, big = 0, excused = 0, shown = 0;
	for(size_t p = 0; p < pixels; p++) {
		uint32_t c = _dioL[0][p];
		c = DioOver(c, _dioL[1][p]);
		c = DioOver(c, _dioL[2][p]);
		for(size_t g = 0; g < groupCount; g++) c = DioOver(c, _dioG[g][p]); // G3e : tiles routees vers un groupe (a la place de L2)
		c = DioOver(c, _dioL[3][p]);
		if(_dioL4Rows) c = DioOver(c, _dioL4[p]); // G3c : couche fixe (nulle hors de ses lignes)
		const uint32_t o = outputBuffer[p];
		if(((c ^ o) & 0x00FFFFFFu) == 0) continue;
		const uint32_t hx = (uint32_t)(p % screenWidth), hy = (uint32_t)(p / screenWidth);
		const uint32_t nx = xStart + hx / hdScale, ny = yStart + hy / hdScale;
		const HdPpuPixelInfo& lineInfo = hdScreenInfo->ScreenTiles[ny << 8];
		if(lineInfo.Grayscale || lineInfo.EmphasisBits) { excused++; continue; }
		diff++;
		int worst = 0;
		for(int sh = 0; sh < 24; sh += 8) {
			worst = std::max(worst, std::abs((int)((c >> sh) & 0xFF) - (int)((o >> sh) & 0xFF)));
		}
		if(worst > 2) big++;
		if(shown < 3) {
			shown++;
			char buf[220];
			snprintf(buf, sizeof(buf), "[HD-DIORAMA] G2a ecart nes=(%u,%u) sortie=%08X couches=%08X L0=%08X L1=%08X L2=%08X L3=%08X",
				nx, ny, o, c, _dioL[0][p], _dioL[1][p], _dioL[2][p], _dioL[3][p]);
			MessageManager::Log(buf);
		}
	}
	const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	char buf[260];
	snprintf(buf, sizeof(buf), "[HD-DIORAMA] G2a identite f=%u : %u pixels HD, differents %u (dont ecart > 2 : %u), lignes grayscale/emphase ignorees %u ; couches %.2f ms, avec controle %.2f ms",
		hdScreenInfo->FrameNumber, (uint32_t)pixels, diff, big, excused, layersMs, totalMs);
	MessageManager::Log(buf);
}

// G2b (doc 36) : les 4 couches de RunDioramaLayers partent au GPU, qui les compose a plat (un seul passage) ; l'image relue
// remplace la sortie du processeur. Si une ligne utilise grayscale / emphase (non geres par les couches), ou si le GPU est
// indisponible ou en echec, la sortie du processeur reste telle quelle. Toutes les 300 images : comparaison GPU / processeur.
// Cameras multiples (doc 49) : choisit la camera (par defaut selon les conditions, ou au clavier), calcule ses reglages
// (<Diorama> + lignes <DioramaCamera> vraies) et deplace la vue de facon lisse (smoothstep) quand la cible change.
template<uint32_t scale>
void HdNesPack<scale>::DioramaCameraStep(const HdDioramaConfig& cfg, float& yaw, float& pitch, float& dist, float& zoom)
{
	const vector<string>& names = _hdData->DioramaCameraNames;
	const int n = (int)names.size();
	auto condsOk = [](const vector<HdPackCondition*>& cs) {
		for(HdPackCondition* c : cs) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	};
	// camera par defaut : derniere regle <DioramaCameraDefault> vraie, sinon default=on, sinon la premiere declaree
	int def = 0;
	bool found = false;
	for(const HdDioramaCameraDefaultRule& r : _hdData->DioramaCameraDefaults) { if(condsOk(r.Conditions)) { def = r.Index; found = true; } }
	if(!found) { for(const HdDioramaCameraLine& L : _hdData->DioramaCameraLines) { if(L.IsDefault) { def = L.Index; break; } } }
	if(def != _camDefault) { _camDefault = def; _camManual = -1; } // la camera par defaut change : la vue la suit
	// touches : une par camera + suivante / precedente / retour a la camera par defaut (front montant)
	const HdDioramaCameraKeys& ks = _hdData->DioramaCameraKeys;
	vector<string> keyNames((size_t)n + 3);
	for(const HdDioramaCameraLine& L : _hdData->DioramaCameraLines) { if(!L.Key.empty() && keyNames[(size_t)L.Index].empty()) keyNames[(size_t)L.Index] = L.Key; }
	keyNames[(size_t)n] = ks.Next; keyNames[(size_t)n + 1] = ks.Prev; keyNames[(size_t)n + 2] = ks.Default;
	if(n == 0) keyNames.clear(); // sans camera declaree : pas de touches (evite le modulo par zero)
	if(_camKeyDown.size() != keyNames.size()) _camKeyDown.assign(keyNames.size(), 0);
	int active = _camManual >= 0 ? _camManual : _camDefault;
	for(size_t k = 0; k < keyNames.size(); k++) {
		if(keyNames[k].empty()) continue;
		const uint16_t code = KeyManager::GetKeyCode(keyNames[k]);
		if(code == 0) {
			if(!_camKeyWarned) { _camKeyWarned = true; MessageManager::Log("[HD-DIORAMA] camera : nom de touche inconnu : " + keyNames[k]); }
			continue;
		}
		const bool down = KeyManager::IsKeyPressed(code);
		if(down && !_camKeyDown[k]) {
			if((int)k < n) _camManual = (int)k;
			else if((int)k == n) _camManual = (active + 1) % n;
			else if((int)k == n + 1) _camManual = (active + n - 1) % n;
			else _camManual = -1;
			active = _camManual >= 0 ? _camManual : _camDefault;
		}
		_camKeyDown[k] = down ? 1 : 0;
	}
	// reglages de la camera active : <Diorama>, puis chaque ligne vraie de cette camera dans l'ordre du fichier
	float v[4] = { cfg.Yaw, cfg.Pitch, cfg.Dist, cfg.Zoom };
	float time = 40.0f;
	for(const HdDioramaCameraLine& L : _hdData->DioramaCameraLines) {
		if(L.Index != active || !condsOk(L.Conditions)) continue;
		for(int i = 0; i < 4; i++) { if(L.Has[i]) v[i] = L.V[i]; }
		if(L.HasTime) time = L.Time;
	}
	// <DioramaView> (doc 53) : reglages de vue selon [cond] ; la derniere regle vraie l'emporte (pitch, zoom, time)
	_dioTopDown = false;
	_dioFloorMargin = -1.0f;
	for(const HdDioramaViewRule& r : _hdData->DioramaViews) {
		if(!condsOk(r.Conditions)) continue;
		_dioTopDown = true; // doc 53 : vue topDown active (sol a plat)
		if(r.FloorMargin >= 0.0f) _dioFloorMargin = r.FloorMargin;
		for(int i = 0; i < 4; i++) { if(r.Has[i]) v[i] = r.V[i]; }
		if(r.HasTime) time = r.Time;
	}
	// la distance ne doit pas amener un plan derriere la camera (meme borne que le chargeur : z >= -0,9 x dist)
	float minZ = 0.0f;
	for(int k = 0; k < 4; k++) minZ = std::min(minZ, cfg.Z[k]);
	for(const HdDioramaDepthGroup& g : _hdData->DioramaDepthGroups) minZ = std::min(minZ, g.Z);
	for(const HdDioramaObjectRule& o : _hdData->DioramaObjectRules) minZ = std::min(minZ, o.Z);
	v[2] = std::max(v[2], std::max(16.0f, -minZ / 0.9f + 1.0f));
	if(!_camInit) {
		for(int i = 0; i < 4; i++) { _camFrom[i] = _camTo[i] = _camCur[i] = v[i]; }
		_camT = _camDur = 1.0f; _camInit = true;
	}
	bool changed = false;
	for(int i = 0; i < 4; i++) { if(std::fabs(v[i] - _camTo[i]) > 1e-4f) changed = true; }
	if(changed) {
		for(int i = 0; i < 4; i++) { _camFrom[i] = _camCur[i]; _camTo[i] = v[i]; }
		_camT = 0.0f; _camDur = std::max(1.0f, time);
		if(time <= 0.0f) { for(int i = 0; i < 4; i++) _camCur[i] = v[i]; _camT = _camDur; }
	}
	if(_camT < _camDur) {
		_camT += 1.0f;
		const float t = std::min(1.0f, _camT / _camDur);
		const float s = t * t * (3.0f - 2.0f * t);
		for(int i = 0; i < 4; i++) _camCur[i] = _camFrom[i] + (_camTo[i] - _camFrom[i]) * s;
	}
	yaw = _camCur[0]; pitch = _camCur[1]; dist = _camCur[2]; zoom = _camCur[3];
}

template<uint32_t scale>
void HdNesPack<scale>::DioramaGpuStep(HdScreenInfo* hdScreenInfo, uint32_t* outputBuffer, uint32_t screenWidth, uint32_t outHeight, uint32_t yStart, uint32_t yEnd, bool compare)
{
	HdGpuCore& gpu = HdGpuCore::Get();
	if(!_dioGpuTried) {
		_dioGpuTried = true;
		_dioGpuOk = gpu.Available();
		MessageManager::Log(_dioGpuOk ? ("[HD-DIORAMA] G2b : GPU actif (" + gpu.Info() + ")") : ("[HD-DIORAMA] G2b : GPU indisponible (" + gpu.Info() + "), image du processeur conservee"));
	}
	if(!_dioGpuOk) return;
	for(uint32_t y = yStart; y < yEnd && y < 240; y++) {
		const HdPpuPixelInfo& lineInfo = hdScreenInfo->ScreenTiles[y << 8];
		if(lineInfo.Grayscale || lineInfo.EmphasisBits) return;
	}

	const auto t0 = std::chrono::steady_clock::now();
	const size_t pixels = (size_t)screenWidth * outHeight;
	// G3a : vue du diorama. Options de <Diorama> (yaw, pitch, dist, z0..z3) + orbite automatique si orbit=on.
	const HdDioramaConfig& cfg = _hdData->Diorama;
	float yaw = cfg.Yaw, pitch = cfg.Pitch, dist = cfg.Dist, baseZoom = cfg.Zoom;
	if(!_hdData->DioramaCameraNames.empty() || !_hdData->DioramaViews.empty()) DioramaCameraStep(cfg, yaw, pitch, dist, baseZoom); // cameras multiples et vues (doc 53)
	if(cfg.Orbit) {
		const double ph = 6.283185307179586 * (double)(_dioOrbit++) / (double)cfg.OrbitPeriod;
		yaw += cfg.OrbitAmp * (float)std::sin(ph);
		pitch += 0.4f * cfg.OrbitAmp * (float)std::sin(ph / 1.5);
	}
	// Manette : <DioramaPad> ajoute au yaw/pitch/zoom de la vue ; la premiere regle dont les conditions sont vraies commande (lissage, retour a 0 sans regle)
	float zoom = baseZoom;
	{
		float tYaw = 0.0f, tPitch = 0.0f, tZoom = 0.0f, smooth = 0.2f;
		const HdDioramaPadRule* pr = nullptr;
		for(const HdDioramaPadRule& r : _hdData->DioramaPadRules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
			if(ok) { pr = &r; break; }
		}
		if(pr) {
			smooth = pr->Smooth;
			const string pad = "Pad" + std::to_string(pr->Port) + " ", joy = "Joy" + std::to_string(pr->Port) + " ";
			auto axis = [&](const char* padName, const char* joyName, float& out) {
				optional<int16_t> a;
				const uint16_t cp = KeyManager::GetKeyCode(pad + padName);
				if(cp != 0) a = KeyManager::GetAxisPosition(cp);
				if(!a.has_value()) { const uint16_t cj = KeyManager::GetKeyCode(joy + joyName); if(cj != 0) a = KeyManager::GetAxisPosition(cj); }
				if(!a.has_value()) return false;
				float v = (float)a.value() / 32767.0f;
				const float av = std::fabs(v);
				if(av <= pr->Dead) v = 0.0f; else v = (v < 0 ? -1.0f : 1.0f) * std::min(1.0f, (av - pr->Dead) / (1.0f - pr->Dead));
				out = v; return true;
			};
			auto stick = [&](int fn, const char* nx, const char* ny, const char* jx, const char* jy) {
				if(fn == 0) return;
				float x = 0.0f, y = 0.0f;
				axis(nx, jx, x); axis(ny, jy, y);
				if(pr->InvX) x = -x;
				if(pr->InvY) y = -y;
				if(fn == 1) { tYaw += x * pr->YawMax; tPitch += y * pr->PitchMax; }
				else tZoom += y * std::log2(pr->ZoomRange);
			};
			stick(pr->Left, "LT X", "LT Y", "X", "Y");
			stick(pr->Right, "RT X", "RT Y", "X2", "Y2");
		}
		_padYaw += (tYaw - _padYaw) * smooth; _padPitch += (tPitch - _padPitch) * smooth; _padZoom += (tZoom - _padZoom) * smooth;
		if(std::fabs(_padYaw) < 0.01f && tYaw == 0.0f) _padYaw = 0.0f;
		if(std::fabs(_padPitch) < 0.01f && tPitch == 0.0f) _padPitch = 0.0f;
		if(std::fabs(_padZoom) < 0.001f && tZoom == 0.0f) _padZoom = 0.0f;
		yaw = std::max(-70.0f, std::min(70.0f, yaw + _padYaw));
		pitch = std::max(-70.0f, std::min(70.0f, pitch + _padPitch));
		zoom = std::max(0.1f, std::min(4.0f, zoom * std::pow(2.0f, _padZoom)));
	}
	const bool neutral = (yaw == 0.0f && pitch == 0.0f);
	HdDioramaScene scene;
	scene.W = (float)screenWidth / (float)GetScale();
	scene.H = (float)outHeight / (float)GetScale();
	// Widescreen : la scene garde la taille de l'ecran NES visible ; seule l'image de sortie s'elargit (colonnes NES ajoutees de chaque cote)
	const float wsPadNes = (_wsMode && _simFieldNes > scene.W) ? std::floor((_simFieldNes - scene.W) * 0.5f + 0.5f) : 0.0f;
	scene.OutPad = wsPadNes;
	const size_t wsPadHd = (size_t)(wsPadNes * (float)GetScale());
	scene.D = dist;
	scene.Yaw = yaw; scene.Pitch = pitch;
	for(int k = 0; k < 4; k++) { scene.PlaneZ[k] = cfg.Z[k]; scene.Thick[k] = cfg.T[k] * 8.0f; } // G3b1 : tiles -> pixels NES
	if(_dioTopDown) scene.PlaneZ[0] = 0.0f; // doc 53 : sol topDown, L0 au niveau du sol
	scene.Scale = (float)GetScale();
	scene.Aa = cfg.Aa; // G3f
	scene.FlatMask = _dioTopDown ? ((1u << 0) | (1u << 2) | 0xFF0u) : 0u; // doc 53 : sol a plat = L0, decor L2 et groupes <DioramaDepth> (bits 4..11)
	// topDown : pied des sprites. Silhouette = pixels couverts par des sprites qui se touchent (4-voisins) ; pied = sa ligne la plus basse.
	// Le GPU place chaque pixel de sprite debout sur le sol a ce pied, pour le tester contre la profondeur des montagnes.
	if(_dioTopDown && hdScreenInfo && yEnd > yStart && yStart < 240) {
		const uint32_t rows = std::min(yEnd, 240u) - yStart;
		_dioFoot.assign((size_t)256 * rows, 0);
		std::vector<int> stack, comp;
		auto covered = [&](int r, int x) { return hdScreenInfo->ScreenTiles[((size_t)(yStart + (uint32_t)r) << 8) | (size_t)x].SpriteCount > 0; };
		for(int r = 0; r < (int)rows; r++) {
			for(int x = 0; x < 256; x++) {
				const int i0 = (r << 8) | x;
				if(_dioFoot[i0] || !covered(r, x)) continue;
				stack.clear(); comp.clear();
				_dioFoot[i0] = 255; stack.push_back(i0);
				int maxR = r, minX = x, maxX = x;
				while(!stack.empty()) {
					const int c = stack.back(); stack.pop_back(); comp.push_back(c);
					const int cr = c >> 8, cx = c & 255;
					if(cr > maxR) maxR = cr;
					if(cx < minX) minX = cx;
					if(cx > maxX) maxX = cx;
					const int nr[4] = { cr - 1, cr + 1, cr, cr }, nx[4] = { cx, cx, cx - 1, cx + 1 };
					for(int k = 0; k < 4; k++) {
						if(nr[k] < 0 || nr[k] >= (int)rows || nx[k] < 0 || nx[k] > 255) continue;
						const int j = (nr[k] << 8) | nx[k];
						if(_dioFoot[j] || !covered(nr[k], nx[k])) continue;
						_dioFoot[j] = 255; stack.push_back(j);
					}
				}
				const uint8_t v = (uint8_t)(maxR + 1);
				for(int c : comp) _dioFoot[c] = v;
				scene.SpriteBlobTotal++;
				if(scene.SpriteBlobN < 16) { // ombres : panneau de la silhouette (r = ligne du haut, balayage de haut en bas)
					float* b = scene.SpriteBlob[scene.SpriteBlobN++];
					b[0] = (float)minX; b[1] = (float)(maxX + 1); b[2] = (float)r; b[3] = (float)(maxR + 1);
				}
			}
		}
		scene.SpriteFoot = _dioFoot.data(); scene.SpriteFootH = rows;
	}
	scene.Clear = _dioL[0].empty() ? 0xFF000000u : (_dioL[0][0] | 0xFF000000u);
	if(_dioL4Rows && _dioL4.size() >= pixels) { scene.Fixed = _dioL4.data(); scene.FixedY0 = _dioL4Y0; scene.FixedRows = _dioL4Rows; } // G3c
	if(scene.Fixed && _hdData->DioramaFixed3d) { // G3c3 : plaque epaisse
		scene.FixedThick = cfg.FixedT * 8.0f; scene.FixedZ = cfg.FixedZ; scene.FixedPersp = cfg.FixedPersp; scene.FixedDark = cfg.FixedDark;
	}
	// G3b2 : motifs de murs (PNG decodes a la demande). Defilement du motif du dessus : celui de la ligne NES de reference.
	// zone : rectangle (pixels NES) que la texture couvre pour stretch=adapt ; thick : epaisseur de la couche (pixels NES)
	auto fillWall = [&](int idx, const HdDioramaWallTex& wt, int refLine, float zoneX, float zoneY, float zoneW, float zoneH, float thick) {
		if(!wt.Top && !wt.Side) return;
		if(wt.Top) wt.Top->Init();
		if(wt.Side) wt.Side->Init();
		HdPackBitmapInfo* topBmp = wt.Top ? wt.Top : (wt.SideVol > 1 ? nullptr : wt.Side); // le dessus retombe sur les flancs (sauf bande volume : elle sert a tous les bords)
		HdPackBitmapInfo* sideBmp = wt.Side;
		HdDioramaWall& w = scene.Wall[idx];
		if(topBmp && topBmp->Width > 0 && topBmp->Height > 0 && topBmp->PixelData.size() >= (size_t)topBmp->Width * topBmp->Height) {
			w.TopPix = topBmp->PixelData.data(); w.TopW = topBmp->Width; w.TopH = topBmp->Height;
		}
		if(sideBmp && sideBmp->Width > 0 && sideBmp->Height > 0 && sideBmp->PixelData.size() >= (size_t)sideBmp->Width * sideBmp->Height) {
			w.SidePix = sideBmp->PixelData.data(); w.SideW = sideBmp->Width; w.SideH = sideBmp->Height;
		}
		// pixels NES par repetition : taille du PNG en pixels HD / echelle, multipliee par l'etirement (1 = taille normale)
		const float hdScale = (float)GetScale();
		const float tsx = wt.Top ? wt.TopStretchX : wt.SideStretchX, tsy = wt.Top ? wt.TopStretchY : wt.SideStretchY;
		// stretch < 0 = adapt : dessus X = largeur de la zone, flancs X = hauteur de la zone, Y = epaisseur de la couche
		w.TopPerX = std::max(0.01f, tsx < 0.0f ? zoneW : (float)w.TopW / hdScale * tsx);
		w.TopPerY = std::max(0.01f, tsy < 0.0f ? thick : (float)w.TopH / hdScale * tsy);
		w.SidePerX = std::max(0.01f, wt.SideStretchX < 0.0f ? zoneH : (float)w.SideW / hdScale * wt.SideStretchX);
		w.SidePerY = std::max(0.01f, wt.SideStretchY < 0.0f ? thick : (float)w.SideH / hdScale * wt.SideStretchY);
		if(wt.SideVol > 1 && w.SidePix) {
			// G3b3 : bande volume, motif en coordonnees du plan ; une image = hauteur de la bande / N
			w.SideVol = (uint32_t)wt.SideVol;
			w.SideVolVert = (wt.SideVolMode == 2 || (wt.SideVolMode == 0 && w.SideH > w.SideW)) ? 1u : 0u;
			const float iw = (float)(w.SideVolVert ? w.SideW : w.SideW / w.SideVol);
			const float ih = (float)(w.SideVolVert ? w.SideH / w.SideVol : w.SideH);
			w.VolPerX = std::max(0.01f, wt.SideStretchX < 0.0f ? zoneW : iw / hdScale * wt.SideStretchX);
			w.VolPerY = std::max(0.01f, wt.SideStretchY < 0.0f ? zoneH : ih / hdScale * wt.SideStretchY);
			w.VolOriginX = wt.SideStretchX < 0.0f ? zoneX : 0.0f;
			w.VolOriginY = wt.SideStretchY < 0.0f ? zoneY : 0.0f;
			w.VolThick = thick;
		}
		w.TopOriginX = tsx < 0.0f ? zoneX : 0.0f;
		w.SideOriginY = wt.SideStretchX < 0.0f ? zoneY : 0.0f;
		if(!(_dioWallLog & (1u << idx))) {
			_dioWallLog |= (1u << idx);
			char buf[300];
			snprintf(buf, sizeof(buf), "[HD-DIORAMA] G3b2 mur couche %d : dessus %s (%ux%u, repetition %.1f x %.1f px NES), flancs %s (%ux%u, repetition %.1f x %.1f px NES)",
				idx, w.TopPix ? "ok" : "ABSENT ou illisible", w.TopW, w.TopH, w.TopPerX, w.TopPerY,
				w.SidePix ? "ok" : "absents (extrusion)", w.SideW, w.SideH, w.SidePerX, w.SidePerY);
			MessageManager::Log(buf);
			if(w.SideVol > 1) {
				snprintf(buf, sizeof(buf), "[HD-DIORAMA] G3b3 mur couche %d : volume de %u images (%s) de %ux%u (repetition %.1f x %.1f px NES, epaisseur %.0f)",
					idx, w.SideVol, w.SideVolVert ? "de haut en bas" : "cote a cote", w.SideVolVert ? w.SideW : w.SideW / w.SideVol, w.SideVolVert ? w.SideH / w.SideVol : w.SideH, w.VolPerX, w.VolPerY, w.VolThick);
				MessageManager::Log(buf);
			}
		}
		const int line = std::min(std::max(refLine, (int)yStart), std::min((int)yEnd, 240) - 1);
		w.Scroll = (float)NesLineScrollX(hdScreenInfo->ScreenTiles[(size_t)line << 8]);
	};
	for(int k = 0; k < 4; k++) fillWall(k, cfg.Wall[k], 120, 0.0f, 0.0f, scene.W, scene.H, cfg.T[k] * 8.0f);
	// G3e : groupes <DioramaDepth>, seulement leurs lignes non vides (une ligne NES de marge de chaque cote pour le filtrage)
	{
		const uint32_t hdScale = GetScale();
		const size_t groupCount = std::min<size_t>(_hdData->DioramaDepthGroups.size(), (size_t)DioGroups);
		for(size_t g = 0; g < groupCount; g++) {
			if(_dioG[g].size() < pixels || _dioGLines[g].size() < 240) continue;
			int a = -1, b = -1;
			for(int y = (int)yStart; y < (int)yEnd && y < 240; y++) {
				if(_dioGLines[g][y]) {
					if(a < 0) a = y;
					b = y;
				}
			}
			if(a < 0) continue;
			a = std::max(a - 1, (int)yStart);
			b = std::min(b + 1, (int)yEnd - 1);
			const HdDioramaDepthGroup& grp = _hdData->DioramaDepthGroups[g];
			scene.ExtraImg[g] = _dioG[g].data();
			scene.ExtraMask[g] = _dioGMask[g].size() == _dioG[g].size() ? _dioGMask[g].data() : nullptr;
			scene.ExtraY0[g] = (uint32_t)(a - (int)yStart) * hdScale;
			scene.ExtraRows[g] = (uint32_t)(b - a + 1) * hdScale;
			scene.ExtraZ[g] = std::max(grp.Z, -0.9f * dist);
			scene.ExtraThick[g] = grp.T * 8.0f;
			// zone du groupe : son rect=, sinon tout l'ecran
			fillWall(4 + (int)g, grp.Wall, (a + b) / 2, grp.HasRect ? (float)grp.X : 0.0f, grp.HasRect ? (float)grp.Y : 0.0f,
				grp.HasRect ? (float)grp.W : scene.W, grp.HasRect ? (float)grp.H : scene.H, grp.T * 8.0f);
		}
	}
	scene.ExtDebug = cfg.ExtDebug; // G4 : controle visuel des extensions
	scene.Zoom = zoom;
	// G4a : <DioramaExtend> (doc 40) : regles dont les conditions [ ] sont vraies a cette image ; la derniere regle vraie l'emporte
	if(!_hdData->DioramaExtendRules.empty()) {
		const int sideBit[4] = { 1, 2, 4, 8 };
		uint32_t active = 0;
		for(const HdDioramaExtendRule& r : _hdData->DioramaExtendRules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) {
				if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; }
			}
			if(!ok || r.Layer < 0 || r.Layer > 3 + DioGroups) continue;
			HdDioramaExtend& e = scene.Ext[r.Layer];
			for(int sd = 0; sd < 4; sd++) {
				if(!(r.Side & sideBit[sd])) continue;
				e.Active = true;
				e.Margin[sd] = (float)r.Size; e.Mode[sd] = r.Mode; e.Band[sd] = (float)r.Band * 8.0f;
				if(r.RangeMin >= 0) { e.Range[sd][0] = (float)r.RangeMin; e.Range[sd][1] = (float)r.RangeMax; } else { e.Range[sd][0] = -100000.0f; e.Range[sd][1] = 100000.0f; }
				e.Fog[sd] = r.Fog;
				e.FogDist[sd] = r.FogDist > 0 ? (float)r.FogDist : (float)r.Size;
				e.Color[sd][0] = r.HasColor ? (float)((r.Color >> 16) & 0xFF) / 255.0f : 0.0f;
				e.Color[sd][1] = r.HasColor ? (float)((r.Color >> 8) & 0xFF) / 255.0f : 0.0f;
				e.Color[sd][2] = r.HasColor ? (float)(r.Color & 0xFF) / 255.0f : 0.0f;
				e.Color[sd][3] = r.HasColor ? 1.0f : 0.0f;
				e.Edge[sd] = r.Edge >= 0 ? (float)r.Edge : (sd == 1 ? scene.H : (sd == 3 ? scene.W : 0.0f));
			}
			active++;
		}
		if(active) {
			const int line = std::min(std::max(120, (int)yStart), std::min((int)yEnd, 240) - 1);
			scene.ExtScrollX = (float)NesLineScrollX(hdScreenInfo->ScreenTiles[(size_t)line << 8]);
			scene.ExtScrollY = (float)NesLineScrollY(hdScreenInfo->ScreenTiles[(size_t)line << 8]);
			static bool logged = false;
			if(!logged) {
				logged = true;
				char buf[320];
				snprintf(buf, sizeof(buf), "[HD-DIORAMA] G4a extension de l'ecran : %u regle(s) active(s) sur %u, %u groupe(s) <DioramaDepth> defini(s)", active, (uint32_t)_hdData->DioramaExtendRules.size(), (uint32_t)_hdData->DioramaDepthGroups.size());
				MessageManager::Log(buf);
				uint32_t ri = 0;
				for(const HdDioramaExtendRule& r : _hdData->DioramaExtendRules) {
					ri++;
					char lay[24];
					if(r.Layer >= 4) snprintf(lay, sizeof(lay), "g%d", r.Layer - 3); else snprintf(lay, sizeof(lay), "L%d", r.Layer);
					const char* state = "";
					if(r.Layer >= 4) {
						const int g = r.Layer - 4;
						state = (g >= (int)_hdData->DioramaDepthGroups.size()) ? " : GROUPE INEXISTANT (numero trop grand)" : (scene.ExtraImg[g] && scene.ExtraRows[g] ? " : groupe present" : " : groupe VIDE a cette image (aucune tile dedans)");
					}
					snprintf(buf, sizeof(buf), "[HD-DIORAMA] G4a regle %u : couche %s, cotes %d (1 haut 2 bas 4 gauche 8 droite), mode %d, marge %d, bande %d, edge %d, plage %d-%d, brume %.2f sur %d%s", ri, lay, r.Side, r.Mode, r.Size, r.Band, r.Edge, r.RangeMin, r.RangeMax, r.Fog, r.FogDist > 0 ? r.FogDist : r.Size, state);
					MessageManager::Log(buf);
				}
			}
		}
	}
	// G3h : envoi reduit (L0 d'une seule couleur en 1 x 1 ; L1 et L3 : lignes modifiees seulement)
	const bool linesOk = _dioLLines[0].size() == 240 && _dioLLines[1].size() == 240 && _dioLDirty[0].size() == 240 && _dioLDirty[1].size() == 240;
	if(linesOk) {
		for(int i = 0; i < 2; i++) {
			for(int y = 0; y < 240; y++) if(_dioLLines[i][y]) _dioLDirty[i][y] = 1;
		}
		if(!_dioLFull) {
			scene.RowDirty[1] = _dioLDirty[0].data();
			scene.RowDirty[3] = _dioLDirty[1].data();
			scene.RowY0 = yStart;
			scene.RowScale = GetScale();
		}
	}
	scene.L0Uniform = _dioL[0].size() >= pixels && pixels > 1 && scene.Thick[0] <= 0.0f &&
		memcmp(_dioL[0].data(), _dioL[0].data() + 1, (pixels - 1) * sizeof(uint32_t)) == 0;
	{ // diagnostic : etat de la L0 elargie, journalise a chaque changement
		char b[256];
		snprintf(b, sizeof(b), "[HD-DIORAMA] L0 elargie : bande=%zu HD, L0 unie=%d, fonds actifs par classe=%d/%d/%d/%d/%d, L0 transparente=%d, fonds de L0 dessines par le GPU=%d", wsPadHd, scene.L0Uniform ? 1 : 0, _activeBgCount[0], _activeBgCount[1], _activeBgCount[2], _activeBgCount[3], _activeBgCount[4], _dioL0Clear ? 1 : 0, _dioGpuBgN);
		if(_dioWideLog != b) { _dioWideLog = b; MessageManager::Log(b); }
	}
	// Fonds de L0 dessines par le GPU (voir RunDioramaLayers)
	scene.BgCount = _dioGpuBgN;
	scene.BgYStart = (float)yStart;
	if(_dioTopDown && _dioFloorMargin >= 0.0f) scene.BgMargin = _dioFloorMargin; // <DioramaView> floormargin=
	for(int i = 0; i < _dioGpuBgN; i++) {
		const DioGpuBg& g = _dioGpuBg[i];
		HdDioramaBgPlane& bp = scene.Bg[i];
		bp.Pix = g.Pix; bp.W = g.W; bp.H = g.H; bp.OffX = g.OffX; bp.OffY = g.OffY; bp.Bright = g.Bright; bp.TileX = g.TileX; bp.TileY = g.TileY;
		bp.HasZ = g.Group >= 0 && (size_t)g.Group < _hdData->DioramaDepthGroups.size(); // fond de groupe : a la profondeur du groupe
		if(bp.HasZ) bp.Z = _hdData->DioramaDepthGroups[g.Group].Z;
		bp.Flat = _dioTopDown; const float incR = g.Incl * 3.14159265f / 180.0f; bp.InclC = std::cos(incR); bp.InclS = std::sin(incR); // doc 53
		// <2DTexture> : remplacement de couleur fait par le shader du fond (4 regles au plus, diorama=off exclues)
		bp.CtN = 0;
		for(const NesHdTextureInfo& r : _hdData->Textures2D[g.TexPrio]) {
			if(r.DioMode == 2 || !r.Texture || r.Texture->PixelData.empty() || r.Texture->Width == 0 || r.Texture->Height == 0) continue;
			if(r.Texture->PixelData.size() < (size_t)r.Texture->Width * r.Texture->Height || bp.CtN >= 4) continue;
			HdDioramaColorTex& ct = bp.Ct[bp.CtN++];
			ct.Pix = r.Texture->PixelData.data(); ct.W = r.Texture->Width; ct.H = r.Texture->Height;
			ct.HasKey = r.HasKey; ct.Key = r.Key; ct.Tol = (float)r.Tolerance;
			ct.TexelX = r.TexelPerPxX; ct.TexelY = r.TexelPerPxY;
			if(r.Mat && r.MatW > 0 && r.MatH > 0) { ct.MatPix = r.Mat->data(); ct.MatW = r.MatW; ct.MatH = r.MatH; ct.NStrength = r.NStrength; ct.AoStrength = r.AoStrength; ct.Specular = r.Specular; }
			{ // eau : la normale defile (pixels NES par seconde -> fraction de la texture), seconde couche a une autre echelle
				const float tSec = hdScreenInfo ? (float)hdScreenInfo->FrameNumber / 60.0f : 0.0f;
				const float kx = (float)GetScale() * r.TexelPerPxX / (float)r.Texture->Width, ky = (float)GetScale() * r.TexelPerPxY / (float)r.Texture->Height;
				ct.Flow[0] = std::fmod(r.FlowX * tSec * kx, 1.0f); ct.Flow[1] = std::fmod(r.FlowY * tSec * ky, 1.0f);
				ct.Flow[2] = std::fmod(r.Flow2X * tSec * kx * r.Flow2Scale, 1.0f); ct.Flow[3] = std::fmod(r.Flow2Y * tSec * ky * r.Flow2Scale, 1.0f);
				ct.HasFlow2 = r.HasFlow2; ct.Flow2Scale = r.Flow2Scale;
			}
			ct.OffX = std::floor((float)g.RawX * r.RatioX * (float)GetScale()); ct.OffY = std::floor((float)g.RawY * r.RatioY * (float)GetScale());
		}
	}
	// Fonds <background> de L0 prolonges au-dela de l'ecran (bandes widescreen et marges wide= / tall= pour la rotation de la camera) :
	// L0 elargie, centre = L0, marges = les memes fonds.
	{
		float marginX = (float)wsPadNes, marginY = 0.0f;
		bool anyL0Bg = false;
		if(_activeBgCount[0] > 0) {
			for(int pr = 0; pr < (int)HdPackData::BgLayerCount; pr++) {
				for(size_t bix = 0; bix < _hdData->BackgroundsByPriority[pr].size(); bix++) {
					const HdBackgroundInfo& bi = _hdData->BackgroundsByPriority[pr][bix];
					if((bi.DioTarget == -1 || bi.DioTarget == 0) && !IsDioGpuBg(pr, (int)bix)) {
						anyL0Bg = true;
						marginX = std::max(marginX, (float)bi.DioWideX);
						marginY = std::max(marginY, (float)bi.DioWideY);
					}
				}
			}
		}
		const size_t mxHd = (size_t)(marginX * (float)GetScale()), myHd = (size_t)(marginY * (float)GetScale());
		if(anyL0Bg && (mxHd > 0 || myHd > 0) && !scene.L0Uniform && _dioL[0].size() >= pixels) {
			const auto tWide0 = std::chrono::steady_clock::now();
			const size_t wideW = (size_t)screenWidth + 2 * mxHd, wideH = (size_t)outHeight + 2 * myHd;
			_dioL0Wide.assign(wideW * wideH, 0u);
			const uint32_t bandBase = _dioL0Clear ? 0u : (_dioL[0][0] | 0xFF000000u);
			const int sc_ = (int)GetScale();
			const int linesAround = (int)((myHd + sc_ - 1) / sc_);
			const int lnFirst = (int)yStart - linesAround, lnLast = (int)yEnd + linesAround;
			auto wideLine = [&](int ln) {
				HdBgConfig lc[50];
				const uint32_t cl = (uint32_t)std::max(0, std::min(239, ln));
				ComputeLineBgConfig(hdScreenInfo->ScreenTiles[cl << 8], (uint8_t)cl, lc);
				std::vector<const HdBgConfig*> bgs;
				for(int k = 0; k < _activeBgCount[0]; k++) {
					const HdBgConfig& c = lc[k];
					if(c.BackgroundIndex < 0) continue;
					const int t = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex].DioTarget;
					if((t == -1 || t == 0) && !IsDioGpuBg(c.BgPriority, c.BackgroundIndex)) bgs.push_back(&c);
				}
				for(int i = 0; i < sc_; i++) {
					const int64_t r = (int64_t)(ln - (int)yStart) * sc_ + i; // ligne de L0 (negative au-dessus)
					if(r < -(int64_t)myHd || r >= (int64_t)outHeight + (int64_t)myHd) continue;
					uint32_t* row = _dioL0Wide.data() + (size_t)(r + (int64_t)myHd) * wideW;
					const bool insideRows = r >= 0 && r < (int64_t)outHeight;
					if(insideRows) memcpy(row + mxHd, _dioL[0].data() + (size_t)r * screenWidth, (size_t)screenWidth * sizeof(uint32_t));
					const int64_t aRow = (int64_t)yStart * sc_ + r;
					auto fillSpan = [&](size_t x0, size_t n) {
						if(n == 0) return;
						std::fill(row + x0, row + x0 + n, bandBase);
						for(const HdBgConfig* c : bgs) DrawWidescreenBgSpan(*c, (int64_t)x0 - (int64_t)mxHd, aRow, n, row + x0);
					};
					if(insideRows) { fillSpan(0, mxHd); fillSpan(wideW - mxHd, mxHd); }
					else fillSpan(0, wideW);
				}
			};
			const int nT = _pool.Size();
			const int nLines = lnLast - lnFirst;
			if(nT <= 1 || nLines < 16) {
				for(int ln = lnFirst; ln < lnLast; ln++) wideLine(ln);
			} else {
				_pool.Run(nT, [&](int t) { for(int ln = lnFirst + t; ln < lnLast; ln += nT) wideLine(ln); });
			}
			scene.L0Wide = _dioL0Wide.data();
			scene.L0WideW = (uint32_t)wideW; scene.L0WideH = (uint32_t)wideH;
			scene.L0WideMX = (float)mxHd / (float)GetScale(); scene.L0WideMY = (float)myHd / (float)GetScale();
			_dioMsWide += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tWide0).count();
			_dioWideBytes = (uint64_t)wideW * wideH * 4;
		}
	}
	// G5 : skybox (doc 41). Seulement si L0 est unie : sa couleur est alors remplacee par le fond. La derniere regle vraie l'emporte.
	if(!_hdData->DioramaSkyboxRules.empty() && (scene.L0Uniform || _dioL0Clear)) {
		const HdDioramaSkyboxRule* chosen = nullptr;
		for(const HdDioramaSkyboxRule& r : _hdData->DioramaSkyboxRules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) {
				if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; }
			}
			if(ok) chosen = &r;
		}
		if(chosen) {
			auto rgb = [](uint32_t c, float* o) { o[0] = ((c >> 16) & 0xFF) / 255.0f; o[1] = ((c >> 8) & 0xFF) / 255.0f; o[2] = (c & 0xFF) / 255.0f; };
			if(chosen->Mode == 1) {
				scene.Sky.Mode = 1;
				rgb(chosen->Top, scene.Sky.Top); rgb(chosen->Bottom, scene.Sky.Bottom);
			} else if(chosen->Mode >= 3) {
				// G5b : panorama ou cubemap ; la rotation propre du fond (deg / image) s'ajoute a la camera de la scene
				const uint32_t* pix = nullptr; uint32_t sw = 0, sh = 0; int layout = 1;
				if(chosen->Mode == 4 && chosen->Faces[0]) {
					if(_dioSkyCubeKey != chosen) {
						_dioSkyCubeKey = chosen; _dioSkyCube.clear(); _dioSkyCubeF = 0;
						uint32_t f = 0; bool good = true;
						for(int k = 0; k < 6 && good; k++) {
							chosen->Faces[k]->Init();
							const HdPackBitmapInfo* b = chosen->Faces[k];
							if(b->Width == 0 || b->Width != b->Height || b->PixelData.size() < (size_t)b->Width * b->Height) good = false;
							else if(f == 0) f = b->Width;
							else if(f != b->Width) good = false;
						}
						if(good) {
							_dioSkyCube.assign((size_t)f * 6 * f, 0u);
							for(int k = 0; k < 6; k++) {
								for(uint32_t y = 0; y < f; y++) {
									memcpy(&_dioSkyCube[(size_t)y * f * 6 + (size_t)k * f], &chosen->Faces[k]->PixelData[(size_t)y * f], f * sizeof(uint32_t));
								}
							}
							_dioSkyCubeF = f;
						} else {
							MessageManager::Log("[HD-DIORAMA] G5b skybox cube : les 6 faces doivent etre carrees et de meme taille, skybox ignoree.");
						}
					}
					if(!_dioSkyCube.empty()) { pix = _dioSkyCube.data(); sw = _dioSkyCubeF * 6; sh = _dioSkyCubeF; layout = 3; }
				} else if(chosen->Image) {
					chosen->Image->Init();
					const HdPackBitmapInfo* b = chosen->Image;
					if(b->Width > 0 && b->Height > 0 && b->PixelData.size() >= (size_t)b->Width * b->Height) {
						pix = b->PixelData.data(); sw = b->Width; sh = b->Height;
						if(chosen->Mode == 4) {
							if(sw * 3 == sh * 4) layout = 1;
							else if(sw * 4 == sh * 3) layout = 2;
							else if(sw == sh * 6) layout = 3;
							else if(sh == sw * 6) layout = 4;
							else { pix = nullptr; MessageManager::Log("[HD-DIORAMA] G5b skybox cube : taille d'image inattendue (croix 4:3 ou 3:4, bande 6:1 ou 1:6), skybox ignoree."); }
						}
					}
				}
				if(pix) {
					_dioSkyX += chosen->SpeedX; _dioSkyY += chosen->SpeedY;
					scene.Sky.Mode = chosen->Mode;
					scene.Sky.Pix = pix; scene.Sky.W = sw; scene.Sky.H = sh;
					const float skyOff = chosen->HasOffset ? chosen->PitchOffset : (_dioTopDown ? 45.0f : 0.0f); // offset= du pack ; sinon 45 deg en topDown, 0 en side
					scene.Sky.RotYaw = _dioSkyX; scene.Sky.RotPitch = _dioSkyY; scene.Sky.CamPitch = skyOff; scene.Sky.CubeLayout = layout;
				}
			} else if(chosen->Image) {
				chosen->Image->Init();
				if(chosen->Image->Width > 0 && chosen->Image->Height > 0 && chosen->Image->PixelData.size() >= (size_t)chosen->Image->Width * chosen->Image->Height) {
					_dioSkyX += chosen->SpeedX; _dioSkyY += chosen->SpeedY;
					scene.Sky.Mode = 2;
					scene.Sky.Pix = chosen->Image->PixelData.data(); scene.Sky.W = chosen->Image->Width; scene.Sky.H = chosen->Image->Height;
					scene.Sky.OffX = _dioSkyX; scene.Sky.OffY = _dioSkyY;
				}
			}
			static bool skyLogged = false;
			if(!skyLogged) {
				skyLogged = true;
				MessageManager::Log(std::string("[HD-DIORAMA] G5 skybox : ") + (chosen->Mode == 1 ? "degrade" : chosen->Mode == 2 ? "image" : chosen->Mode == 3 ? "panorama" : "cubemap") + ", " + std::to_string(_hdData->DioramaSkyboxRules.size()) + " regle(s) definie(s)");
			}
		}
	} else if(!_hdData->DioramaSkyboxRules.empty()) {
		static bool skyWarned = false;
		if(!skyWarned) { skyWarned = true; MessageManager::Log("[HD-DIORAMA] G5 skybox : L0 n'est pas unie, skybox non affichee (elle remplace la couleur unie de L0)."); }
	}
	// G6a / G6b : lumieres de scene (doc 42) : regles dont les conditions sont vraies ; 8 lumieres au plus, derniere ambiante vraie.
	// Une regle avec tile= donne une lumiere par tile de sprite reconnue a l'ecran.
	if(!_hdData->DioramaLightRules.empty()) {
		auto rgb = [](uint32_t c, float k, float* o) { o[0] = ((c >> 16) & 0xFF) / 255.0f * k; o[1] = ((c >> 8) & 0xFF) / 255.0f * k; o[2] = (c & 0xFF) / 255.0f * k; };
		int n = 0;
		bool any = false;
		const float tSec = hdScreenInfo ? (float)hdScreenInfo->FrameNumber / 60.0f : 0.0f;
		auto addLight = [&](const HdDioramaLightRule& r, float px, float py, bool onFloor, int grp = 0) {
			if(n >= 8) return;
			HdDioramaLight& l = scene.Lights.L[n++];
			const float fk = r.Flicker > 0.0f ? 1.0f - r.Flicker * 0.5f * (1.0f + std::sin(r.FlickerSpeed * tSec)) : 1.0f;
			l.Type = r.Type;
			rgb(r.Color, r.Intensity * fk, l.Col);
			if(r.Type == 2) {
				if(_dioTopDown && onFloor) { // topDown, lumiere de tile : posee sur le sol (Y = -|y| au-dessus, profondeur = verticale de l'image)
					l.Pos[0] = px; l.Pos[1] = -std::fabs(r.Y); l.Pos[2] = scene.H * 0.5f - py;
				} else { l.Pos[0] = px; l.Pos[1] = py; l.Pos[2] = r.Z + ((grp > 0 && (size_t)(grp - 1) < _hdData->DioramaDepthGroups.size()) ? _hdData->DioramaDepthGroups[grp - 1].Z : 0.0f); } l.Radius = r.Radius; l.Glow = r.Glow * fk; l.GlowRadius = r.GlowRadius;
			} else {
				// topDown : soleil venant du dessus par defaut (0.3, 1.0, 0.3), sauf dx/dy/dz ecrits dans le pack
				const bool topSun = _dioTopDown && !r.DirGiven;
				const float sdx = topSun ? 0.3f : r.Dx, sdy = topSun ? 1.0f : r.Dy, sdz = topSun ? 0.3f : r.Dz;
				const float len = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
				l.Pos[0] = -sdx / len; l.Pos[1] = -sdy / len; l.Pos[2] = -sdz / len; // vers la lumiere
			}
		};
		bool anyTile = false;
		for(const HdDioramaLightRule& r : _hdData->DioramaLightRules) {
			if(r.HasTile) { anyTile = true; continue; }
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) {
				if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; }
			}
			if(!ok) continue;
			any = true;
			if(r.Type == 1) { rgb(r.Color, r.Intensity, scene.Lights.Ambient); continue; }
			// topDown : le sol est a Y = 0 et le haut est negatif : lumiere au-dessus du sol par defaut (y=-60), sauf y= ecrit
			addLight(r, r.X, (_dioTopDown && !r.PosYGiven) ? -std::fabs(r.Y) : r.Y, false);
		}
		if(anyTile && hdScreenInfo) {
			std::vector<uint32_t> seen; // (regle, origine x, origine y) deja ajoutes
			std::vector<float> nearLights; // groupes de 6 : regle, origine x, origine y, nombre de tiles, somme des x et des y (position moyenne)
			const size_t nRules = _hdData->DioramaLightRules.size();
			for(int y = 0; y < 240 && n < 8; y++) {
				for(int x = 0; x < 256 && n < 8; x++) {
					HdPpuPixelInfo& pi = hdScreenInfo->ScreenTiles[((size_t)y << 8) | (size_t)x];
					for(int sp = 0; sp < pi.SpriteCount; sp++) {
						HdPpuTileInfo& spr = pi.Sprite[sp];
						// G7 : sprite dans un groupe <DioramaDepth> : la lumiere prend la profondeur de ce groupe (z= reste relatif au plan)
						const HdPackTileInfo* sgInfo = _resolvedSprites[((size_t)y << 8 | (size_t)x) * 4 + (size_t)sp];
						const int sg = (sgInfo && !sgInfo->DioOffCamera && sgInfo->DioDepthGroup > 0 && sgInfo->DioDepthGroup <= DioGroups && (size_t)(sgInfo->DioDepthGroup - 1) < _hdData->DioramaDepthGroups.size()) ? (int)sgInfo->DioDepthGroup : 0;
						for(size_t ri = 0; ri < nRules; ri++) {
							const HdDioramaLightRule& r = _hdData->DioramaLightRules[ri];
							if(!r.HasTile || !r.MatchesTile(spr)) continue;
							const int ox = x - (spr.HorizontalMirroring ? 7 - (int)spr.OffsetX : (int)spr.OffsetX);
							const int oy = y - (spr.VerticalMirroring ? 7 - (int)spr.OffsetY : (int)spr.OffsetY);
							const uint32_t id = ((uint32_t)(ri & 0xFFF) << 20) | ((uint32_t)((ox + 16) & 0x3FF) << 10) | (uint32_t)((oy + 16) & 0x3FF);
							if(std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
							seen.push_back(id);
							bool ok = true;
							for(HdPackCondition* c : r.Conditions) {
								if(!c->CheckCondition(x, y, &spr)) { ok = false; break; }
							}
							if(!ok) continue;
							any = true;
							// tiles voisines d'un meme motif (miroir) : une lumiere par motif, placee au centre moyen des tiles
							const float lpx = (float)ox + 4.0f + (spr.HorizontalMirroring ? -r.OffX : r.OffX);
							const float lpy = (float)oy + 4.0f - (float)yStart + (spr.VerticalMirroring ? -r.OffY : r.OffY);
							bool near = false;
							for(size_t q = 0; q + 6 < nearLights.size() && !near; q += 7) {
								if(nearLights[q] == (float)ri && nearLights[q + 6] == (float)sg && std::abs(nearLights[q + 1] - (float)ox) <= 16.0f && std::abs(nearLights[q + 2] - (float)oy) <= 16.0f) {
									nearLights[q + 3] += 1.0f; nearLights[q + 4] += lpx; nearLights[q + 5] += lpy; near = true;
								}
							}
							if(!near) {
								nearLights.push_back((float)ri); nearLights.push_back((float)ox); nearLights.push_back((float)oy);
								nearLights.push_back(1.0f); nearLights.push_back(lpx); nearLights.push_back(lpy); nearLights.push_back((float)sg);
							}
						}
					}
				}
			}
			// une lumiere par groupe, a la position moyenne de ses tiles
			for(size_t q = 0; q + 6 < nearLights.size(); q += 7) {
				const HdDioramaLightRule& gr = _hdData->DioramaLightRules[(size_t)nearLights[q]];
				const float cnt = nearLights[q + 3];
				addLight(gr, nearLights[q + 4] / cnt, nearLights[q + 5] / cnt, true, (int)nearLights[q + 6]);
			}
		}
				scene.Lights.Active = any;
		static bool lightLogged = false;
		if(any && !lightLogged) {
			lightLogged = true;
			MessageManager::Log("[HD-DIORAMA] G6a lumieres de scene : " + std::to_string(n) + " lumiere(s) ponctuelle(s) ou solaire(s) active(s) (tiles de sprite comprises), " + std::to_string(_hdData->DioramaLightRules.size()) + " regle(s) definie(s)");
		}
	}
	// H1 : objets 3D de decor (doc 45) : regles <DioramaObject> dont la condition est vraie (8 au plus)
	scene.Objects.Count = 0;
	// <DioramaFog> (doc 54) : derniere regle vraie ; valeurs absentes = defaut de la vue (topDown : brouillard vers l'horizon ; side : voile leger sur les plans du fond)
	// <3DParticles> (diorama NES) : simulation SNES (boite torique autour de la camera), defauts selon la vue, puis positions scene pour le GPU.
	// topDown : X / Y = carte, Z = hauteur ; side : X = carte, Y = profondeur (depthmin..depthmax), Z = hauteur depuis le bas de l'ecran.
	if(!_hdData->Diorama3DParticles.empty()) {
		if(_p3State.size() != _hdData->Diorama3DParticles.size()) _p3State.assign(_hdData->Diorama3DParticles.size(), P3State());
		const float dt = 1.0f / 60.0f, kPi = 3.14159265f;
		for(size_t pi = 0; pi < _hdData->Diorama3DParticles.size() && scene.P3Count < 4; pi++) {
			const HdDiorama3DParticlesRule& R = _hdData->Diorama3DParticles[pi];
			bool ok = true;
			for(HdPackCondition* c : R.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
			if(!ok) continue; // effet fige
			P3State& st = _p3State[pi];
			const bool td = _dioTopDown;
			if(st.Init && st.TopDown != td) st.Init = false; // changement de vue : nouvelle boite
			st.TopDown = td;
			// defauts selon la vue (cle absente)
			const float B = R.HasBoxSize ? R.BoxSize : (td ? 384.0f : 320.0f);
			const float BH = td ? (R.HasBoxHeight ? R.BoxHeight : 96.0f) : scene.H + 32.0f;
			const float ahead = R.HasBoxAhead ? R.BoxAhead : (td ? 0.25f : 0.0f);
			const float fallMin = R.HasFall ? R.FallMin : (td ? 20.0f : 30.0f), fallMax = R.HasFall ? R.FallMax : (td ? 40.0f : 60.0f);
			const float dMin = R.DepthMin, dMax = R.DepthMax, DB = dMax - dMin;
			// repere : position monde du point (0, ligne 0) de la scene
			float ox = 0.0f, oy = 0.0f; bool found = false;
			if(R.Anchor >= 0 && hdScreenInfo) {
				HdBgConfig cfgL[5 * HdNesPack::PriorityLevelsPerLayer];
				ComputeLineBgConfig(hdScreenInfo->ScreenTiles[120 << 8], 120, cfgL);
				for(int layer = 0; layer < 5 && !found; layer++) {
					for(int i = 0; i < _activeBgCount[layer]; i++) {
						const HdBgConfig& c = cfgL[layer * HdNesPack::PriorityLevelsPerLayer + i];
						if(c.BackgroundIndex < 0) continue;
						const HdBackgroundInfo& b = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex];
						if((int)b.Priority == R.Anchor) { ox = (float)b.Left + (float)c.BgScrollX; oy = (float)b.Top + (float)c.BgScrollY; found = true; break; }
					}
				}
			}
			if(!found && _dioGpuBgN > 0) { ox = _dioGpuBg[0].OffX / (float)GetScale(); oy = _dioGpuBg[0].OffY / (float)GetScale(); found = true; }
			if(!found && hdScreenInfo) { const HdPpuPixelInfo& lp = hdScreenInfo->ScreenTiles[120 << 8]; ox = (float)NesLineScrollX(lp); oy = (float)NesLineScrollY(lp); }
			oy += (float)yStart;
			const float cX = ox + scene.W * 0.5f;
			const float cY = td ? oy + scene.H * 0.5f - B * ahead : dMin + DB * 0.5f;
			const float BY = td ? B : DB; // taille de la boite sur le second axe (carte en topDown, profondeur en side)
			auto rnd = [&]() { st.Rng ^= st.Rng << 13; st.Rng ^= st.Rng >> 17; st.Rng ^= st.Rng << 5; return (float)(st.Rng & 0xFFFFFF) / 16777216.0f; };
			auto rr = [&](float a, float b) { return a + (b - a) * rnd(); };
			auto spawn = [&](bool anyHeight) {
				P3Part p;
				p.X = cX + (rnd() - 0.5f) * B; p.Y = cY + (rnd() - 0.5f) * BY;
				const float fall = rr(fallMin, fallMax);
				p.Z = anyHeight ? rnd() * BH : (fall >= 0 ? BH : 0.0f);
				const float ang = rnd() * 2.0f * kPi, dr = rr(R.DriftMin, R.DriftMax);
				p.VX = std::cos(ang) * dr; p.VY = td ? std::sin(ang) * dr : 0.0f; p.VZ = -fall;
				p.Life = R.LifeMax > 0 ? std::max(0.05f, rr(R.LifeMin, R.LifeMax)) : 0.0f;
				p.Age = (anyHeight && p.Life > 0) ? rnd() * p.Life : 0.0f;
				p.Size = 1.0f + R.SizeRandom * (rnd() * 2.0f - 1.0f);
				p.Seed1 = rnd() * 2.0f * kPi; p.Seed2 = rnd() * 2.0f * kPi;
				st.Parts.push_back(p);
			};
			auto step = [&]() {
				st.Time += dt;
				if(R.Rate > 0) { st.SpawnAcc += R.Rate * dt; while(st.SpawnAcc >= 1.0f) { st.SpawnAcc -= 1.0f; if((int)st.Parts.size() < R.MaxParticles) spawn(false); } }
				else { while((int)st.Parts.size() < R.MaxParticles) spawn(false); }
				const float tw = 2.0f * kPi * R.TurbulenceFreq;
				for(size_t k = 0; k < st.Parts.size();) {
					P3Part& p = st.Parts[k];
					bool dead = false;
					if(p.Life > 0) { p.Age += dt; dead = p.Age >= p.Life; }
					if(!dead) {
						p.VZ -= R.Gravity * dt;
						float tx = 0, ty = 0;
						if(R.Turbulence != 0.0f) { tx = R.Turbulence * std::sin(tw * st.Time + p.Seed1); ty = R.Turbulence * std::cos(tw * 0.73f * st.Time + p.Seed2); }
						p.EVX = p.VX + R.WindX + tx; p.EVY = p.VY + (td ? R.WindY + ty : 0.0f);
						p.X += p.EVX * dt;
						p.Y += p.EVY * dt;
						p.Z += p.VZ * dt;
						if(p.Z <= 0.0f) { if(R.GroundRespawn) p.Z += BH; else dead = true; }
						else if(p.Z > BH) p.Z -= BH;
						float rx = p.X - cX; rx -= B * std::floor(rx / B + 0.5f); p.X = cX + rx;
						float ry = p.Y - cY; ry -= BY * std::floor(ry / BY + 0.5f); p.Y = cY + ry;
					}
					if(dead) { p = st.Parts.back(); st.Parts.pop_back(); continue; }
					k++;
				}
			};
			if(!st.Init) {
				st.Init = true; st.Parts.clear(); st.Time = 0; st.SpawnAcc = 0;
				st.Rng ^= (uint32_t)(pi + 1) * 2654435761u;
				if(R.Rate <= 0) { while((int)st.Parts.size() < R.MaxParticles) spawn(true); }
				else { const int steps = (int)std::min(R.LifeMax > 0 ? R.LifeMax * 60.0f : 300.0f, 600.0f); for(int s = 0; s < steps; s++) step(); }
			}
			step();
			// sortie : position scene centree (comme les objets) + taille + couleur / alpha
			const uint32_t cnt = (uint32_t)st.Parts.size();
			const uint32_t rows = (cnt + 255) / 256;
			vector<float>& out = _p3Out[scene.P3Count];
			out.assign((size_t)rows * 768 * 4, 0.0f); // 3 texels RGBA32F par particule, 256 particules par ligne
			for(uint32_t k = 0; k < cnt; k++) {
				const P3Part& p = st.Parts[k];
				const float t = p.Life > 0 ? p.Age / p.Life : 0.0f;
				float a = R.AlphaStart + (R.AlphaEnd - R.AlphaStart) * t;
				if(p.Life > 0) {
					if(R.FadeIn > 0 && t < R.FadeIn) a *= t / R.FadeIn;
					if(R.FadeOut > 0 && t > 1.0f - R.FadeOut) a *= (1.0f - t) / R.FadeOut;
				}
				const float rb = std::max(std::fabs(p.X - cX) / (B * 0.5f), std::fabs(p.Y - cY) / (BY * 0.5f));
				if(R.FarFade > 0 && rb > 1.0f - R.FarFade) a *= std::max(0.0f, (1.0f - rb) / R.FarFade);
				a *= R.Alpha;
				float x, y, z, pk = 1.0f, sx, sy, sz; // pk : facteur de perspective (side) ; s* : trainee (stretch=) dans le repere scene
				if(td) { x = p.X - ox - scene.W * 0.5f; y = -p.Z; z = scene.H * 0.5f - (p.Y - oy); sx = p.EVX; sy = -p.VZ; sz = -p.EVY; }
				else { z = p.Y; pk = (scene.D + z) / scene.D; x = (p.X - ox - scene.W * 0.5f) * pk; y = (scene.H * 0.5f + 16.0f - p.Z) * pk; sx = p.EVX * pk; sy = -p.VZ * pk; sz = 0.0f; }
				const float size = p.Size * (R.SizeStart + (R.SizeEnd - R.SizeStart) * t) * pk;
				auto lerpC = [&](int sh) { const float c0 = (float)((R.ColorStart >> sh) & 0xFF), c1 = (float)((R.ColorEnd >> sh) & 0xFF); return (c0 + (c1 - c0) * t) / 255.0f; };
				float* o = &out[(size_t)k * 12];
				o[0] = x; o[1] = y; o[2] = z; o[3] = size; o[4] = lerpC(16); o[5] = lerpC(8); o[6] = lerpC(0); o[7] = std::clamp(a, 0.0f, 1.0f);
				o[8] = sx * R.Stretch; o[9] = sy * R.Stretch; o[10] = sz * R.Stretch; o[11] = 0.0f;
			}
			HdDioramaP3Batch& b = scene.P3[scene.P3Count++];
			b.Data = out.data(); b.Count = cnt; b.Additive = R.Additive; b.MinPx = R.MinPixelSize;
			if(R.Texture) {
				R.Texture->Init();
				if(R.Texture->Width > 0 && R.Texture->Height > 0 && R.Texture->PixelData.size() >= (size_t)R.Texture->Width * R.Texture->Height) {
					b.TexPix = R.Texture->PixelData.data(); b.TexW = R.Texture->Width; b.TexH = R.Texture->Height;
				}
			}
		}
	}
	// <DioramaMist> (doc 54) : nappes de brume ; derniere regle vraie ; repere comme <DioramaClouds> ; chaque nappe derive un peu plus vite (parallaxe)
	if(!_hdData->DioramaMistRules.empty()) {
		const HdDioramaMistRule* mr = nullptr;
		for(const HdDioramaMistRule& r : _hdData->DioramaMistRules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
			if(ok) mr = &r;
		}
		if(mr && !mr->Off) {
			auto& M = scene.Mist;
			const bool td = _dioTopDown;
			M.Active = true;
			M.Count = mr->HasCount ? mr->Count : (td ? 3 : 2);
			M.From = mr->HasFrom ? mr->From : (td ? 4.0f : 120.0f);
			M.To = mr->HasTo ? mr->To : (td ? 28.0f : 30.0f);
			M.Opacity = mr->HasOpacity ? mr->Opacity : (td ? 0.3f : 0.25f);
			M.Size = mr->HasSize ? mr->Size : (td ? 72.0f : 96.0f);
			M.Coverage = mr->HasCoverage ? mr->Coverage : 0.55f;
			M.Soft = mr->HasSoft ? mr->Soft : 0.3f;
			M.Band = mr->HasBand ? mr->Band : 72.0f;
			M.Seed = mr->Seed;
			const uint32_t col = mr->HasColor ? mr->Color : 0xE0E8F0u;
			M.Col[0] = ((col >> 16) & 0xFF) / 255.0f; M.Col[1] = ((col >> 8) & 0xFF) / 255.0f; M.Col[2] = (col & 0xFF) / 255.0f;
			const float speed = mr->HasSpeed ? mr->Speed : (td ? 6.0f : 8.0f);
			const float dir = (mr->HasDir ? mr->Dir : (td ? 30.0f : 0.0f)) * 0.017453292f;
			float ox = 0.0f, oy = 0.0f; bool found = false;
			if(mr->Anchor >= 0 && hdScreenInfo) {
				HdBgConfig cfgL[5 * HdNesPack::PriorityLevelsPerLayer];
				ComputeLineBgConfig(hdScreenInfo->ScreenTiles[120 << 8], 120, cfgL);
				for(int layer = 0; layer < 5 && !found; layer++) {
					for(int i = 0; i < _activeBgCount[layer]; i++) {
						const HdBgConfig& c = cfgL[layer * HdNesPack::PriorityLevelsPerLayer + i];
						if(c.BackgroundIndex < 0) continue;
						const HdBackgroundInfo& b = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex];
						if((int)b.Priority == mr->Anchor) { ox = (float)b.Left + (float)c.BgScrollX; oy = (float)b.Top + (float)c.BgScrollY; found = true; break; }
					}
				}
			}
			if(!found && _dioGpuBgN > 0) { ox = _dioGpuBg[0].OffX / (float)GetScale(); oy = _dioGpuBg[0].OffY / (float)GetScale(); found = true; }
			if(!found && hdScreenInfo) { const HdPpuPixelInfo& lp = hdScreenInfo->ScreenTiles[120 << 8]; ox = (float)NesLineScrollX(lp); oy = (float)NesLineScrollY(lp); }
			const float tSec = hdScreenInfo ? (float)hdScreenInfo->FrameNumber / 60.0f : 0.0f;
			for(int i = 0; i < M.Count && i < 8; i++) {
				const float k = 1.0f + 0.25f * (float)i; // nappes du haut un peu plus rapides
				M.OffX[i] = ox - std::cos(dir) * speed * k * tSec + 37.0f * (float)i;
				M.OffY[i] = oy + (float)yStart - std::sin(dir) * speed * k * tSec + 53.0f * (float)i;
			}
		}
	}
	// <DioramaClouds> (doc 54) : derniere regle vraie par type ; repere : fond d'ancrage, sinon fond de sol GPU, sinon scroll PPU (ligne 120)
	if(!_hdData->DioramaCloudRules.empty()) {
		const HdDioramaCloudRule* cr[2] = { nullptr, nullptr };
		for(const HdDioramaCloudRule& r : _hdData->DioramaCloudRules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
			if(ok) cr[r.Type & 1] = &r;
		}
		const float tSec = hdScreenInfo ? (float)hdScreenInfo->FrameNumber / 60.0f : 0.0f;
		for(int k = 0; k < 2; k++) {
			const HdDioramaCloudRule* r = cr[k];
			if(!r || r->Off) continue;
			auto& C = scene.Cloud[k];
			C.Active = true;
			const bool td = _dioTopDown;
			C.Size = r->HasSize ? r->Size : (k == 0 ? (td ? 96.0f : 64.0f) : (td ? 64.0f : 48.0f));
			const float speed = r->HasSpeed ? r->Speed : (k == 0 ? 10.0f : 4.0f);
			const float dir = (r->HasDir ? r->Dir : (td ? 30.0f : 0.0f)) * 0.017453292f;
			C.Coverage = r->HasCoverage ? r->Coverage : (k == 0 ? 0.45f : 0.5f);
			C.Soft = r->HasSoft ? r->Soft : (k == 0 ? 0.12f : 0.25f);
			C.Opacity = r->HasOpacity ? r->Opacity : (k == 0 ? (td ? 0.35f : 0.25f) : (td ? 0.55f : 0.35f));
			C.GroundH = r->HasGroundH ? r->GroundH : 16.0f;
			C.Seed = r->Seed;
			const uint32_t col = r->HasColor ? r->Color : (k == 0 ? 0x101828u : 0xC8D4E0u);
			C.Col[0] = ((col >> 16) & 0xFF) / 255.0f; C.Col[1] = ((col >> 8) & 0xFF) / 255.0f; C.Col[2] = (col & 0xFF) / 255.0f;
			// repere du monde : position (pixels NES) du point de l'ecran (0, ligne 0 de la scene)
			float ox = 0.0f, oy = 0.0f;
			bool found = false;
			if(r->Anchor >= 0 && hdScreenInfo) {
				HdBgConfig cfgL[5 * HdNesPack::PriorityLevelsPerLayer];
				ComputeLineBgConfig(hdScreenInfo->ScreenTiles[120 << 8], 120, cfgL);
				for(int layer = 0; layer < 5 && !found; layer++) {
					for(int i = 0; i < _activeBgCount[layer]; i++) {
						const HdBgConfig& c = cfgL[layer * HdNesPack::PriorityLevelsPerLayer + i];
						if(c.BackgroundIndex < 0) continue;
						const HdBackgroundInfo& b = _hdData->BackgroundsByPriority[c.BgPriority][c.BackgroundIndex];
						if((int)b.Priority == r->Anchor) { ox = (float)b.Left + (float)c.BgScrollX; oy = (float)b.Top + (float)c.BgScrollY; found = true; break; }
					}
				}
			}
			if(!found && _dioGpuBgN > 0) { ox = _dioGpuBg[0].OffX / (float)GetScale(); oy = _dioGpuBg[0].OffY / (float)GetScale(); found = true; }
			if(!found && hdScreenInfo) { const HdPpuPixelInfo& lp = hdScreenInfo->ScreenTiles[120 << 8]; ox = (float)NesLineScrollX(lp); oy = (float)NesLineScrollY(lp); }
			C.OffX = ox - std::cos(dir) * speed * tSec;
			C.OffY = oy + (float)yStart - std::sin(dir) * speed * tSec;
		}
	}
	if(!_hdData->DioramaFogRules.empty()) {
		const HdDioramaFogRule* fr = nullptr;
		for(const HdDioramaFogRule& r : _hdData->DioramaFogRules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
			if(ok) fr = &r;
		}
		if(fr && !fr->Off) {
			auto& F = scene.Fog;
			F.Active = true;
			F.Start = fr->HasStart ? fr->Start : (_dioTopDown ? -20.0f : 150.0f); // topDown pitch 55 : bas de l'ecran ~ -70, haut ~ +70
			F.End = fr->HasEnd ? fr->End : (_dioTopDown ? 200.0f : 500.0f);
			if(F.End <= F.Start + 1.0f) F.End = F.Start + 1.0f;
			F.Opacity = fr->HasOpacity ? fr->Opacity : (_dioTopDown ? 0.7f : 0.45f);
			F.Ground = fr->HasGround ? fr->Ground : 0.0f;
			F.GroundH = fr->HasGroundH ? fr->GroundH : 16.0f;
			const bool skyCol = (fr->HasColor ? fr->ColorSky : true) && scene.Sky.Mode == 1; // defaut : bas du ciel en degrade
			if(skyCol) { F.Col[0] = scene.Sky.Bottom[0]; F.Col[1] = scene.Sky.Bottom[1]; F.Col[2] = scene.Sky.Bottom[2]; }
			else {
				const uint32_t c = (fr->HasColor && !fr->ColorSky) ? fr->Color : (_dioTopDown ? 0xC8D4E0u : 0xA8B8C8u);
				F.Col[0] = ((c >> 16) & 0xFF) / 255.0f; F.Col[1] = ((c >> 8) & 0xFF) / 255.0f; F.Col[2] = (c & 0xFF) / 255.0f;
			}
		}
	}
	// <DayNight> (doc 55), diorama : la couleur du cycle multiplie l'ambiante, le soleil et le ciel ; les lumieres ponctuelles restent intactes.
	// Les cles de brouillard des etapes pilotent <DioramaFog> (active avec ses valeurs par defaut s'il n'y a pas de regle).
	if(_dn.Active) {
		const HdDayNightDef& dd = _hdData->DayNightRules[_dn.Def].Def;
		const float* k = _dn.Col;
		if(dd.TAmbient) {
			if(!scene.Lights.Active) { scene.Lights.Active = true; scene.Lights.Ambient[0] = scene.Lights.Ambient[1] = scene.Lights.Ambient[2] = 1.0f; }
			for(int c = 0; c < 3; c++) scene.Lights.Ambient[c] *= k[c];
		}
		if(dd.TSun) {
			for(int i = 0; i < 8; i++) { if(scene.Lights.L[i].Type == 3) for(int c = 0; c < 3; c++) scene.Lights.L[i].Col[c] *= k[c]; }
		}
		if(dd.TSky) {
			for(int c = 0; c < 3; c++) scene.Sky.Tint[c] = k[c];
			const uint32_t cl = scene.Clear;
			scene.Clear = (cl & 0xFF000000u) | ((uint32_t)(((cl >> 16) & 0xFF) * k[0] + 0.5f) << 16) | ((uint32_t)(((cl >> 8) & 0xFF) * k[1] + 0.5f) << 8) | (uint32_t)((cl & 0xFF) * k[2] + 0.5f);
		}
		if(dd.DrivesFog) {
			auto& F = scene.Fog;
			if(!F.Active) {
				F.Active = true;
				F.Start = _dioTopDown ? -20.0f : 150.0f; F.End = _dioTopDown ? 200.0f : 500.0f; F.Opacity = 0.0f;
				F.Ground = 0.0f; F.GroundH = 16.0f;
				const uint32_t c = _dioTopDown ? 0xC8D4E0u : 0xA8B8C8u;
				if(scene.Sky.Mode == 1) { F.Col[0] = scene.Sky.Bottom[0]; F.Col[1] = scene.Sky.Bottom[1]; F.Col[2] = scene.Sky.Bottom[2]; }
				else { F.Col[0] = ((c >> 16) & 0xFF) / 255.0f; F.Col[1] = ((c >> 8) & 0xFF) / 255.0f; F.Col[2] = (c & 0xFF) / 255.0f; }
			}
			F.Opacity = std::clamp(HdDayNightFog(dd, _dn, 0, F.Opacity), 0.0f, 1.0f);
			F.Start = HdDayNightFog(dd, _dn, 1, F.Start);
			F.End = HdDayNightFog(dd, _dn, 2, F.End);
			if(F.End <= F.Start + 1.0f) F.End = F.Start + 1.0f;
			F.Ground = std::clamp(HdDayNightFog(dd, _dn, 3, F.Ground), 0.0f, 1.0f);
			HdDayNightFogColor(dd, _dn, F.Col);
		}
		if(dd.TAmbient) {
			// brouillard sans fogColor, brume, nappes de brouillard au sol et particules 3D (non additives) suivent la lumiere ambiante
			const HdDayNightStep& sa = dd.Steps[_dn.A]; const HdDayNightStep& sb = dd.Steps[_dn.B];
			const bool fogColDriven = sa.HasFogColor || sb.HasFogColor;
			for(int c = 0; c < 3; c++) {
				if(scene.Fog.Active && !fogColDriven) scene.Fog.Col[c] *= k[c];
				scene.Mist.Col[c] *= k[c];
				scene.Cloud[1].Col[c] *= k[c];
				scene.P3Tint[c] = k[c];
			}
		}
		_dnDioApplied = true;
	}
	if(!_hdData->DioramaObjectRules.empty() || !_dioT2oRules.empty()) {
		vector<const HdDioramaObjectRule*> orules; // <DioramaObject>, puis instances <Tile2Obj> de cette image
		for(const HdDioramaObjectRule& q : _hdData->DioramaObjectRules) orules.push_back(&q);
		for(const HdDioramaObjectRule& q : _dioT2oRules) orules.push_back(&q);
		if(_dioObjSpin.size() < orules.size()) _dioObjSpin.resize(orules.size(), 0.0f);
		int on = 0;
		vector<int32_t> wvals; // valeurs surveillees de cette image (INT32_MIN = adresse non lue), pour scrollx= / scrolly=
		if(hdScreenInfo) {
			for(const HdWatchedValue& w : _hdData->WatchedValues) {
				uint32_t raw = 0;
				bool okv = true;
				for(uint32_t bi = 0; bi < w.Size; bi++) {
					auto it = hdScreenInfo->WatchedAddressValues.find(w.Address + bi);
					if(it == hdScreenInfo->WatchedAddressValues.end()) { okv = false; break; }
					raw |= (uint32_t)it->second << (8 * bi);
				}
				int32_t v = INT32_MIN;
				if(okv) v = w.Signed ? (w.Size == 1 ? (int32_t)(int8_t)raw : (int32_t)(int16_t)raw) : (int32_t)raw;
				wvals.push_back(v);
			}
		}
		for(size_t i = 0; i < orules.size() && on < HdDioMaxObjects; i++) {
			const HdDioramaObjectRule& r = *orules[i];
			if(!r.Mesh || r.Mesh->size() < 24) continue;
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) {
				if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; }
			}
			if(!ok) continue;
			float scx = 0.0f, scy = 0.0f;
			if(!r.ScrollX.Empty() && !r.ScrollX.Eval(wvals, scx)) continue; // valeur RAM pas encore lue : objet masque
			if(!r.ScrollY.Empty() && !r.ScrollY.Eval(wvals, scy)) continue;
			if(!r.ScrollX.Empty() && (r.X - scx + r.Radius < -256.0f - wsPadNes || r.X - scx - r.Radius > (float)scene.W + 256.0f + wsPadNes)) continue; // hors du niveau visible
			_dioObjSpin[i] = std::fmod(_dioObjSpin[i] + r.Spin, 360.0f);
			HdDioramaObject& o = scene.Objects.O[on++];
			o.Verts = r.Mesh->data();
			o.Version = r.MeshVersion;
			o.VertCount = (uint32_t)(r.Mesh->size() / 8);
			o.TexPix = nullptr; o.TexW = 0; o.TexH = 0;
			if(r.Tex) {
				r.Tex->Init();
				if(r.Tex->Width > 0 && r.Tex->Height > 0 && r.Tex->PixelData.size() >= (size_t)r.Tex->Width * r.Tex->Height) {
					o.TexPix = r.Tex->PixelData.data(); o.TexW = r.Tex->Width; o.TexH = r.Tex->Height;
				}
			}
			o.Color[0] = ((r.Color >> 16) & 0xFF) / 255.0f; o.Color[1] = ((r.Color >> 8) & 0xFF) / 255.0f; o.Color[2] = (r.Color & 0xFF) / 255.0f;
			o.Pos[0] = r.X - scx; o.Pos[1] = r.Y - scy; o.Pos[2] = r.Z; o.RefZ = r.RefZ; o.Occlude = r.Occlude;
			if(r.FromTile2Obj && _dioTopDown) { // Tile2Obj en topDown : pose sur le sol, dans le meme repere que les couches a plat (sans perspective : kz = 1)
				o.Pos[0] = r.X; o.Pos[1] = scene.H * 0.5f - r.Elevation; o.Pos[2] = scene.H * 0.5f - r.BottomY; o.RefZ = -o.Pos[2];
			}
			o.Scale = r.Scale;
			o.Lit = r.Lit;
			o.Bright = r.Bright;
			o.Shadow = r.Shadow;
			o.CastShadow = r.CastShadow && _dioTopDown;
			o.WinOn = false;
			if(r.HasWindow && !r.ScrollX.Empty()) { // fenetre sur le niveau : [defilement - marge, defilement + largeur + marge], convertie en x du modele
				o.WinOn = true;
				o.WinX0 = (scx - r.Window - wsPadNes - r.X) / r.Scale;
				o.WinX1 = (scx + (float)scene.W + r.Window + wsPadNes - r.X) / r.Scale;
			}
			// R = Ry * Rx * Rz (degres) appliquee au modele avant le passage aux axes de la scene
			const float kd = 0.017453292f;
			const float ay = (r.RotY + _dioObjSpin[i]) * kd, ax = r.RotX * kd, az = r.RotZ * kd;
			const float cy = std::cos(ay), sy = std::sin(ay), cx = std::cos(ax), sx = std::sin(ax), cz = std::cos(az), sz = std::sin(az);
			const float Ry[9] = { cy, 0, sy, 0, 1, 0, -sy, 0, cy };
			const float Rx[9] = { 1, 0, 0, 0, cx, -sx, 0, sx, cx };
			const float Rz[9] = { cz, -sz, 0, sz, cz, 0, 0, 0, 1 };
			auto mul = [](const float* a, const float* b, float* c) { for(int i = 0; i < 3; i++) for(int j = 0; j < 3; j++) c[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j]; };
			float t1[9];
			mul(Ry, Rx, t1);
			mul(t1, Rz, o.Rot);
			if(r.MirrorX || r.MirrorY) { // <Sprite2Obj> : miroir horizontal / vertical de la scene, applique apres la rotation
				const float M[9] = { r.MirrorX ? -1.0f : 1.0f, 0, 0, 0, r.MirrorY ? -1.0f : 1.0f, 0, 0, 0, 1 };
				float t2[9]; mul(M, o.Rot, t2);
				for(int q = 0; q < 9; q++) o.Rot[q] = t2[q];
			}
		}
		scene.Objects.Count = on;
		static int objLast = -1; // journal a chaque changement du nombre d'objets actifs (ex. changement d'ecran)
		if(on != objLast) {
			objLast = on;
			MessageManager::Log("[HD-DIORAMA] H1 objets 3D : " + std::to_string(on) + " objet(s) actif(s) sur " + std::to_string(orules.size()) + " regle(s) <DioramaObject>, frame " + std::to_string(hdScreenInfo ? hdScreenInfo->FrameNumber : 0));
		}
	}
	// G6d : cartes de normales : bande de lignes de chaque tampon (L2, L3 : lignes touchees ; groupes : bande du groupe, choisie par le GPU)
	if(!_hdData->DioramaNormalPairs.empty() && !_dioNrm[0].empty()) {
		bool anyN = false;
		for(int i = 0; i < 2 + DioGroups; i++) {
			if(_dioNrmLines[i].size() != 240 || _dioNrm[i].empty()) continue;
			int y0 = -1, y1 = -1;
			for(int y = (int)yStart; y < (int)yEnd && y < 240; y++) {
				if(!_dioNrmLines[i][y]) continue;
				if(y0 < 0) y0 = y;
				y1 = y;
			}
			if(y0 < 0) continue;
			scene.Normals.Img[i] = _dioNrm[i].data();
			scene.Normals.Y0[i] = (uint32_t)(y0 - (int)yStart) * GetScale();
			scene.Normals.Rows[i] = (uint32_t)(y1 + 1 - (int)yStart) * GetScale() - scene.Normals.Y0[i];
			anyN = true;
		}
		scene.Normals.Active = anyN;
		scene.Normals.Strength = cfg.NormalStrength;
		scene.Normals.FlipY = cfg.NormalFlipY;
		static bool nrmLogged = false;
		if(anyN && !nrmLogged) {
			nrmLogged = true;
			MessageManager::Log("[HD-DIORAMA] G6d normales : " + std::to_string(_hdData->DioramaNormalPairs.size()) + " paire(s) <DioramaNormal>, force " + std::to_string(cfg.NormalStrength) + (scene.Lights.Active ? "" : " (aucune lumiere <DioramaLight> active : les normales ne servent qu'avec une lumiere)"));
		}
	}
	// G6c : ombres des sprites : bande de lignes du masque, style de la premiere regle utilisee
	if(!_hdData->DioramaShadowRules.empty() && !_dioS.empty() && _dioSLines.size() == 240 && _dioSRule.size() == 240) {
		int y0 = -1, y1 = -1, ruleId = -1;
		for(int y = (int)yStart; y < (int)yEnd && y < 240; y++) {
			if(!_dioSLines[y]) continue;
			if(y0 < 0) y0 = y;
			y1 = y;
			if(_dioSRule[y] >= 0 && (ruleId < 0 || _dioSRule[y] < ruleId)) ruleId = _dioSRule[y];
		}
		if(y0 >= 0 && ruleId >= 0 && scene.Lights.Active) {
			const HdDioramaShadowRule& sr = _hdData->DioramaShadowRules[ruleId];
			const uint32_t r0 = (uint32_t)(y0 - (int)yStart) * GetScale();
			const uint32_t r1 = (uint32_t)(y1 + 1 - (int)yStart) * GetScale();
			scene.Shadow.Active = true;
			scene.Shadow.Img = _dioS.data();
			scene.Shadow.Y0 = r0; scene.Shadow.Rows = r1 > r0 ? r1 - r0 : 0;
			scene.Shadow.Opacity = sr.Opacity; scene.Shadow.Soft = sr.Soft;
			scene.Shadow.Color[0] = ((sr.Color >> 16) & 0xFF) / 255.0f; scene.Shadow.Color[1] = ((sr.Color >> 8) & 0xFF) / 255.0f; scene.Shadow.Color[2] = (sr.Color & 0xFF) / 255.0f;
			scene.Shadow.OntoMask = sr.OntoMask;
			scene.Shadow.Light = sr.Light - 1;
			static bool shLogged = false;
			if(!shLogged) {
				shLogged = true;
				MessageManager::Log("[HD-DIORAMA] G6c ombres des sprites : regle " + std::to_string(ruleId + 1) + " sur " + std::to_string(_hdData->DioramaShadowRules.size()) + ", opacite " + std::to_string(sr.Opacity) + ", couches (masque) " + std::to_string(sr.OntoMask) + ", lumiere " + std::to_string(sr.Light));
			}
		} else if(y0 >= 0 && !scene.Lights.Active) {
			static bool shWarned = false;
			if(!shWarned) { shWarned = true; MessageManager::Log("[HD-DIORAMA] G6c ombres des sprites : aucune lumiere <DioramaLight> active, pas d'ombre (il faut une lumiere ponctuelle ou solaire)."); }
		}
	}
	// l'image relue n'est gardee a part que pour le controle d'identite ; sinon elle va droit dans la sortie (une seule passe)
	const bool needImg = compare && neutral && wsPadNes <= 0.0f && cfg.Aa == 1 && scene.FixedThick <= 0.0f && zoom == 1.0f && scene.Sky.Mode == 0 && !scene.Lights.Active && !scene.Shadow.Active && !scene.Normals.Active && scene.Objects.Count == 0; // fixed3d epais : la plaque differe volontairement de la 2D
	const bool ok = _dioL[0].size() >= pixels &&
		gpu.SubmitScene(_dioL[0].data(), _dioL[1].data(), _dioL[2].data(), _dioL[3].data(), screenWidth, outHeight, scene) &&
		(needImg ? (gpu.Fetch(_dioGpuImg) && _dioGpuImg.size() >= pixels)
			: (wsPadNes > 0.0f ? (_dioWsImg.resize((size_t)(screenWidth + 2 * wsPadHd) * outHeight), _dioWsReady = gpu.FetchOpaque(_dioWsImg.data(), _dioWsImg.size()))
				: gpu.FetchOpaque(outputBuffer, pixels)));
	if(ok && linesOk) {
		for(int i = 0; i < 2; i++) std::fill(_dioLDirty[i].begin(), _dioLDirty[i].end(), (uint8_t)0);
		_dioLFull = false;
	}
	if(!ok) {
		if(_dioFail++ < 3) MessageManager::Log("[HD-DIORAMA] G2b : envoi ou relecture GPU en echec, image du processeur conservee");
		return;
	}
	if(compare && !neutral) {
		char buf[160];
		snprintf(buf, sizeof(buf), "[HD-DIORAMA] G3a vue f=%u : yaw %.1f, pitch %.1f (controle d'identite sans objet hors vue neutre)", hdScreenInfo->FrameNumber, yaw, pitch);
		MessageManager::Log(buf);
	}
	if(needImg) { // avec aa > 1 le filtrage change les pixels : controle d'identite sans objet
		uint32_t diff = 0, big = 0, shown = 0;
		for(size_t p = 0; p < pixels; p++) {
			const uint32_t g = _dioGpuImg[p], o = outputBuffer[p];
			if(((g ^ o) & 0x00FFFFFFu) == 0) continue;
			diff++;
			int worst = 0;
			for(int sh = 0; sh < 24; sh += 8) worst = std::max(worst, std::abs((int)((g >> sh) & 0xFF) - (int)((o >> sh) & 0xFF)));
			if(worst > 2) big++;
			if(shown < 3) {
				shown++;
				char buf[160];
				snprintf(buf, sizeof(buf), "[HD-DIORAMA] G2b ecart hd=(%u,%u) processeur=%08X gpu=%08X", (uint32_t)(p % screenWidth), (uint32_t)(p / screenWidth), o, g);
				MessageManager::Log(buf);
			}
		}
		char buf[260];
		snprintf(buf, sizeof(buf), "[HD-DIORAMA] G3b identite GPU (vue neutre, epaisseurs %.2f/%.2f/%.2f/%.2f tiles) f=%u : %u pixels HD, differents %u (dont ecart > 2 : %u)",
			cfg.T[0], cfg.T[1], cfg.T[2], cfg.T[3], hdScreenInfo->FrameNumber, (uint32_t)pixels, diff, big);
		MessageManager::Log(buf);
	}
	const auto tCopy0 = std::chrono::steady_clock::now();
	if(needImg) {
		for(size_t p = 0; p < pixels; p++) outputBuffer[p] = _dioGpuImg[p] | 0xFF000000u;
	}
	_dioMsCopy += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tCopy0).count();

	_dioMsGpu += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if(++_dioN >= 300) {
		const HdGpuTimes gt = gpu.TakeLayerTimes();
		const double n = (double)_dioN;
		char buf[400];
		snprintf(buf, sizeof(buf), "[HD-DIORAMA] G2b cout moyen sur %u images (aa=%d, groupes=%u) : couches %.2f ms ; GPU total %.2f ms = envoi textures %.2f + dessin %.2f + attente GPU %.2f + retournement %.2f + copie sortie %.2f ; dont L0 elargie (CPU) %.2f ms, image L0 %.1f Mo",
			_dioN, cfg.Aa, (uint32_t)_hdData->DioramaDepthGroups.size(), _dioMsLayers / n, _dioMsGpu / n, gt.UploadMs / n, gt.DrawMs / n, gt.MapMs / n, gt.FlipMs / n, _dioMsCopy / n, _dioMsWide / n, (double)_dioWideBytes / 1048576.0);
		MessageManager::Log(buf);
		_dioN = 0; _dioMsLayers = 0; _dioMsGpu = 0; _dioMsCopy = 0; _dioMsWide = 0;
	}
}

// Widescreen (voie 1) : un pixel de bande HD. aCol / aRow : position HD absolue dans l'image NES (0..255 x 0..239, x scale).
// Meme calcul que DrawCustomBackground : pixel du PNG = (Left + scroll) * scale + position HD, modulo si tileX / tileY.
template<uint32_t scale>
void HdNesPack<scale>::DrawWidescreenBgPixel(const HdBgConfig& cfg, int64_t aCol, int64_t aRow, uint32_t* dst)
{
	HdBackgroundInfo& bgInfo = _hdData->BackgroundsByPriority[cfg.BgPriority][cfg.BackgroundIndex];
	if(!bgInfo.Data) {
		return;
	}
	const int64_t width = (int64_t)bgInfo.Data->Width;
	const int64_t height = (int64_t)bgInfo.Data->Height;
	if(width <= 0 || height <= 0) {
		return;
	}
	int64_t px = (int64_t)(bgInfo.Left + cfg.BgScrollX) * scale + aCol;
	int64_t py = (int64_t)(bgInfo.Top + cfg.BgScrollY) * scale + aRow;
	if(bgInfo.TileX) {
		px %= width; if(px < 0) px += width;
	}
	if(bgInfo.TileY) {
		py %= height; if(py < 0) py += height;
	}
	if(px < 0 || py < 0 || px >= width || py >= height) {
		return;
	}
	uint32_t c = bgInfo.data()[py * width + px];
	if(bgInfo.Brightness != 255) {
		c = AdjustBrightness((uint8_t*)&c, bgInfo.Brightness);
	}
	switch(bgInfo.BlendMode) {
		case HdPackBlendMode::Alpha:
			if(c >= 0xFF000000) {
				*dst = c;
			} else if(c >= 0x01000000) {
				BlendColors<HdPackBlendMode::Alpha>((uint8_t*)dst, (uint8_t*)&c);
			}
			break;
		case HdPackBlendMode::Add: BlendColors<HdPackBlendMode::Add>((uint8_t*)dst, (uint8_t*)&c); break;
		case HdPackBlendMode::Subtract: BlendColors<HdPackBlendMode::Subtract>((uint8_t*)dst, (uint8_t*)&c); break;
	}
}

// Comme DrawWidescreenBgPixel, pour "count" colonnes consecutives : la ligne du PNG, le mode de fusion et la luminosite sont resolus une fois.
template<uint32_t scale>
void HdNesPack<scale>::DrawWidescreenBgSpan(const HdBgConfig& cfg, int64_t aCol, int64_t aRow, size_t count, uint32_t* dst)
{
	HdBackgroundInfo& bgInfo = _hdData->BackgroundsByPriority[cfg.BgPriority][cfg.BackgroundIndex];
	if(!bgInfo.Data) return;
	const int64_t width = (int64_t)bgInfo.Data->Width, height = (int64_t)bgInfo.Data->Height;
	if(width <= 0 || height <= 0) return;
	int64_t py = (int64_t)(bgInfo.Top + cfg.BgScrollY) * scale + aRow;
	if(bgInfo.TileY) { py %= height; if(py < 0) py += height; }
	if(py < 0 || py >= height) return;
	int64_t px = (int64_t)(bgInfo.Left + cfg.BgScrollX) * scale + aCol;
	if(bgInfo.TileX) { px %= width; if(px < 0) px += width; }
	const uint32_t* src = bgInfo.data() + (size_t)py * (size_t)width;
	const bool tileX = bgInfo.TileX;
	const int bright = bgInfo.Brightness;
	const HdPackBlendMode mode = bgInfo.BlendMode;
	for(size_t k = 0; k < count; k++) {
		if(!tileX && (px < 0 || px >= width)) { px++; continue; }
		uint32_t c = src[px];
		if(++px >= width && tileX) px = 0;
		if(bright != 255) c = AdjustBrightness((uint8_t*)&c, bright);
		uint32_t* d = dst + k;
		switch(mode) {
			case HdPackBlendMode::Alpha:
				if(c >= 0xFF000000) *d = c;
				else if(c >= 0x01000000) BlendColors<HdPackBlendMode::Alpha>((uint8_t*)d, (uint8_t*)&c);
				break;
			case HdPackBlendMode::Add: BlendColors<HdPackBlendMode::Add>((uint8_t*)d, (uint8_t*)&c); break;
			case HdPackBlendMode::Subtract: BlendColors<HdPackBlendMode::Subtract>((uint8_t*)d, (uint8_t*)&c); break;
		}
	}
}

// Widescreen (voie 1) : un pixel de liquide dans une bande. xN : position NES relative au bord visible (negative a gauche),
// yN : position NES relative au haut visible. L'etendue XMin/XMax est une fraction de la largeur elargie (bandes incluses).
// La surface (side) vient de _wsLiqSurface ; les bulles viennent de _wsBubbleMap (calculees une fois par image).
// Pas de test z ni de masque HUD dans les bandes (aucune sprite n'y est dessinee).
template<uint32_t scale>
void HdNesPack<scale>::DrawWidescreenLiquidPixel(uint32_t hb, uint32_t outWidth, float xN, float yN, size_t pixIdx, uint32_t* dst)
{
	const float frac = (float)hb / (float)outWidth;
	uint32_t px = *dst;
	for(size_t k = 0; k < _hdData->Liquids.size(); k++) {
		const NesHdLiquidInfo& liq = *_hdData->Liquids[k];
		if(!liq.Active || frac < liq.XMin || frac >= liq.XMax) {
			continue;
		}
		if(liq.IsTop) {
			if(yN >= liq.YMin && yN < liq.YMax) {
				px = LiquidTopPixel(liq, xN, yN, px, 1.0f, liq.FollowX * liq.FollowAccX, liq.FollowY * liq.FollowAccY);
			}
		} else {
			const float s = _wsLiqSurface[k][hb];
			if(yN < s) {
				if(liq.Glow > 0.0f) {
					const float d = (s - yN) / std::max(1.0f, liq.GlowRadius);
					if(d < 1.0f) {
						px = LiquidGlowPixel(liq, d, px);
					}
				}
			} else {
				px = BlendLiquidPixel(liq, xN, yN, s, px);
			}
		}
		if(k < _wsBubbleMap.size() && !_wsBubbleMap[k].empty()) {
			const uint8_t a = _wsBubbleMap[k][pixIdx];
			if(a != 0) {
				px = LiquidBubblePixel(px, a);
			}
		}
	}
	*dst = px;
}

// Widescreen (voie 1) : bulles d'un liquide sur toute la largeur elargie (meme rendu que PrepareLiquids, sans sprite/HUD).
// Carte indexee comme le buffer de sortie : (ligne visible HD) * outWidth + colonne HD.
template<uint32_t scale>
void HdNesPack<scale>::PrepareWidescreenBubbles(uint32_t outWidth, uint32_t padHd, uint32_t visRows)
{
	const float S = (float)scale;
	auto liqHash = [](uint32_t v) -> uint32_t { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; };
	_wsBubbleMap.resize(_hdData->Liquids.size());
	for(size_t k = 0; k < _hdData->Liquids.size(); k++) {
		const NesHdLiquidInfo& liq = *_hdData->Liquids[k];
		std::vector<uint8_t>& map = _wsBubbleMap[k];
		map.clear();
		if(!liq.Active || liq.IsTop || liq.BubbleCount <= 0) {
			continue;
		}
		const int lo = std::clamp((int)(liq.XMin * (float)outWidth), 0, (int)outWidth - 1);
		const int hi = std::clamp((int)(liq.XMax * (float)outWidth), 0, (int)outWidth);
		if(hi <= lo) {
			continue;
		}
		map.assign((size_t)outWidth * visRows, 0);
		const std::vector<float>& surf = _wsLiqSurface[k];
		const float bottom = liq.Level + liq.Depth * 0.7f;
		const int visHdRows = (int)visRows;
		for(int b = 0; b < liq.BubbleCount; b++) {
			const uint32_t h1 = liqHash((uint32_t)b * 2654435761u + 1u);
			const uint32_t h2 = liqHash(h1 + 0x9E3779B9u);
			const uint32_t h3 = liqHash(h2 + 0x85EBCA6Bu);
			const float r1 = (float)(h1 & 0xFFFF) / 65535.0f;
			const float r2 = (float)(h2 & 0xFFFF) / 65535.0f;
			const float r3 = (float)(h3 & 0xFFFF) / 65535.0f;
			const float period = 1.0f / std::max(0.05f, liq.BubbleRise * (0.6f + 0.8f * r2));
			const float u = liq.Time / period + r3;
			const float ph = u - std::floor(u);
			int xh = lo + (int)(r1 * (float)(hi - lo - 1));
			xh = std::clamp(xh, 0, (int)outWidth - 1);
			const float sY = surf[xh];
			float cy, rad, alpha;
			bool ring;
			if(ph < 0.9f) {
				cy = (bottom + (sY - bottom) * (ph / 0.9f)) * S;
				rad = liq.BubbleSize * S * 0.8f;
				alpha = 1.0f;
				ring = false;
			} else {
				const float e = (ph - 0.9f) / 0.1f;
				cy = sY * S;
				rad = liq.BubbleSize * S * (1.0f + 1.5f * e);
				alpha = 1.0f - e;
				ring = true;
			}
			const float cx = (float)xh;
			const int x0 = std::max(0, (int)std::floor(cx - rad - 1.0f)), x1 = std::min((int)outWidth - 1, (int)std::ceil(cx + rad + 1.0f));
			const int y0 = std::max(0, (int)std::floor(cy - rad - 1.0f)), y1 = std::min(visHdRows - 1, (int)std::ceil(cy + rad + 1.0f));
			for(int y = y0; y <= y1; y++) {
				for(int x = x0; x <= x1; x++) {
					const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
					float v = 0.0f;
					if(!ring) {
						if(d <= rad) v = 0.55f * alpha;
					} else {
						const float e2 = std::fabs(d - rad);
						if(e2 <= 1.0f) v = 0.9f * alpha * (1.0f - e2);
					}
					if(v <= 0.0f) continue;
					const uint8_t q = (uint8_t)(v * 255.0f + 0.5f);
					uint8_t& m = map[(size_t)y * outWidth + x];
					if(q > m) m = q;
				}
			}
		}
	}
	(void)padHd;
}

// Widescreen (voie 1) : remplit les colonnes HD hors du centre (0..padHd et apres le centre) avec les <background>
// puis les liquides de chaque ligne. Le centre (256 px NES) doit deja etre copie dans "out" ; les effets du centre ne changent pas.
// Les lignes sont reparties sur le pool de threads (chaque ligne ecrit ses propres lignes HD).
template<uint32_t scale>
void HdNesPack<scale>::DrawWidescreenBands(uint32_t* out, uint32_t outWidth, uint32_t padHd, uint32_t overscanLeft, uint32_t overscanTop, uint32_t overscanBottom)
{
	if(_hdScreenInfo == nullptr || out == nullptr || outWidth == 0) {
		return;
	}
	const uint32_t yStart = overscanTop, yEnd = 240 - overscanBottom;

	// Surface des liquides : une seule evaluation par colonne et par image (pas de sin par pixel)
	const float twoPi = 2.0f * OverlayPi;
	const float degToRad = OverlayPi / 180.0f;
	_wsLiqSurface.resize(_hdData->Liquids.size());
	for(size_t k = 0; k < _hdData->Liquids.size(); k++) {
		const NesHdLiquidInfo& liq = *_hdData->Liquids[k];
		std::vector<float>& surf = _wsLiqSurface[k];
		surf.assign(outWidth, 0.0f);
		if(!liq.Active || liq.IsTop) {
			continue;
		}
		for(uint32_t hb = 0; hb < outWidth; hb++) {
			const float xN = ((float)hb - (float)padHd) / (float)scale;
			surf[hb] = liq.Level + liq.Amp * std::sin(twoPi * (xN / liq.Wave + liq.Speed * liq.Time) + liq.Phase * degToRad);
		}
	}

	_wsZ.assign(_zEnabled ? (size_t)outWidth * (yEnd - yStart) * scale : 0, 0);
	PrepareWidescreenBubbles(outWidth, padHd, (yEnd - yStart) * scale);

	auto drawRow = [&](uint32_t y) {
		HdBgConfig lineCfg[50];
		std::vector<const HdBgConfig*> layers;
		ComputeLineBgConfig(_hdScreenInfo->ScreenTiles[y << 8], (uint8_t)y, lineCfg);
		for(int k = 0; k < 50; k++) {
			if(lineCfg[k].BackgroundIndex >= 0) {
				layers.push_back(&lineCfg[k]);
			}
		}
		for(uint32_t i = 0; i < scale; i++) {
			const uint32_t outRow = (y - yStart) * scale + i;
			const int64_t aRow = (int64_t)y * scale + i;
			const float yN = (float)(y - yStart) + (float)i / (float)scale;
			uint32_t* row = out + (size_t)outRow * outWidth;
			for(uint32_t hb = 0; hb < outWidth; hb++) {
				if(hb >= padHd && hb < outWidth - padHd) {
					continue; // centre deja copie : on ne touche qu'aux bandes
				}
				const int64_t aCol = (int64_t)hb - (int64_t)padHd + (int64_t)overscanLeft * scale;
				for(const HdBgConfig* cfg : layers) {
					const uint32_t before = row[hb];
					DrawWidescreenBgPixel(*cfg, aCol, aRow, row + hb);
					if(_zEnabled && row[hb] != before) {
						_wsZ[(size_t)outRow * outWidth + hb] = (uint8_t)cfg->BgPriority;
					}
				}
				const float xN = ((float)hb - (float)padHd) / (float)scale;
				DrawWidescreenLiquidPixel(hb, outWidth, xN, yN, (size_t)outRow * outWidth + hb, row + hb);
			}
		}
	};

	int nThreads = _pool.Size();
	if(nThreads <= 1 || yEnd <= yStart || yEnd - yStart < 16) {
		for(uint32_t y = yStart; y < yEnd; y++) {
			drawRow(y);
		}
	} else {
		_pool.Run(nThreads, [&](int t) {
			for(uint32_t y = yStart + (uint32_t)t; y < yEnd; y += (uint32_t)nThreads) {
				drawRow(y);
			}
		});
	}
}

// Lecture des <video> et composition sur l'image HD. Appelee apres la passe de dessin, donc sans concurrence.
template<uint32_t scale>
void HdNesPack<scale>::ProcessVideos(uint32_t* outputBuffer, uint32_t screenWidth, const OverscanDimensions& overscan)
{
	const uint32_t hdHeight = (240 - overscan.Top - overscan.Bottom) * scale;
	if(screenWidth == 0 || hdHeight == 0) {
		return;
	}
	const uint32_t xStart = overscan.Left;
	const uint32_t yStart = overscan.Top;

	for(auto& video : _hdData->Videos) {
		if(!video || !video->MatchesCondition()) {
			continue;
		}

		if(!video->IsPlaying && !video->HasPlayed) {
			// Memoire si precharge par LoadAsync, sinon depuis le disque
			if(video->FileData && !video->FileData->empty()) {
				video->VideoPlayer = plm_create_with_memory(video->FileData->data(), video->FileData->size(), 0);
			} else {
				video->VideoPlayer = plm_create_with_filename(video->Filename.c_str());
			}
			if(video->VideoPlayer == nullptr) {
				video->HasPlayed = true;
				continue;
			}

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
			plm_set_audio_decode_callback(video->VideoPlayer, [](plm_t* mpeg, plm_samples_t* samples, void* user) {
				SoundMixer* audioMixer = static_cast<SoundMixer*>(user);
				if(audioMixer == nullptr) return;

				int totalValues = samples->count * 2;
				std::vector<int16_t> intSamples(totalValues);
				for(int i = 0; i < totalValues; i++) {
					float f = std::clamp(samples->interleaved[i], -1.0f, 1.0f);
					intSamples[i] = static_cast<int16_t>(f * 32767.0f);
				}
				audioMixer->PlayAudioBuffer(intSamples.data(), (uint32_t)samples->count, (uint32_t)plm_get_samplerate(mpeg));
			}, _videoAudio);

			video->CurrentFramePixels.resize((size_t)video->VideoWidth * video->VideoHeight * 4, 0);
		}

		// Lecture et synchronisation (une mise a jour par frame de l'emulateur, comme cote SNES)
		if(video->IsPlaying && video->VideoPlayer != nullptr) {
			double fps = plm_get_framerate(video->VideoPlayer);
			if(fps <= 0.0) fps = 30.0;
			double frameTime = 1.0 / fps;
			video->TimeAccumulator += 1.0 / 60.0;

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

			if(plm_has_ended(video->VideoPlayer)) {
				if(video->LoopTargetFrame != 0) {
					if(video->LoopTargetFrame > 0) {
						plm_seek(video->VideoPlayer, (double)video->LoopTargetFrame / fps, 0);
					} else {
						plm_rewind(video->VideoPlayer);
					}
					frame = plm_decode_video(video->VideoPlayer);
					if(frame != nullptr) {
						plm_frame_to_bgra(frame, video->CurrentFramePixels.data(), video->VideoWidth * 4);
					}
				} else {
					video->IsPlaying = false;
					video->HasPlayed = true;
					plm_destroy(video->VideoPlayer);
					video->VideoPlayer = nullptr;
					video->CurrentFramePixels.clear();
					continue;
				}
			}
		}

		if(video->CurrentFramePixels.empty() || video->VideoWidth <= 0 || video->VideoHeight <= 0) {
			continue;
		}

		const uint32_t* videoPixels = reinterpret_cast<const uint32_t*>(video->CurrentFramePixels.data());

		// Pixel HD (sx, sy) -> pixel NES (xStart + sx/scale, yStart + sy/scale) : test de z comme cote SNES
		auto plot = [&](uint32_t sx, uint32_t sy, uint32_t pixel) {
			if(video->AlphaEnable && (pixel & 0x00FFFFFF) == 0) return;
			uint32_t nesIdx = ((yStart + sy / scale) << 8) | (xStart + sx / scale);
			if(video->ZIndex >= _zNes[nesIdx]) {
				outputBuffer[sy * screenWidth + sx] = pixel | 0xFF000000;
				_zNes[nesIdx] = video->ZIndex;
			}
		};

		if(video->Stretch) {
			for(uint32_t sy = 0; sy < hdHeight; sy++) {
				uint32_t vy = (uint32_t)((uint64_t)sy * video->VideoHeight / hdHeight);
				const uint32_t* row = videoPixels + (size_t)vy * video->VideoWidth;
				for(uint32_t sx = 0; sx < screenWidth; sx++) {
					uint32_t vx = (uint32_t)((uint64_t)sx * video->VideoWidth / screenWidth);
					plot(sx, sy, row[vx]);
				}
			}
		} else {
			int maxW = std::min(video->VideoWidth, (int)screenWidth - video->X);
			int maxH = std::min(video->VideoHeight, (int)hdHeight - video->Y);
			for(int y = 0; y < maxH; y++) {
				for(int x = 0; x < maxW; x++) {
					int sx = video->X + x;
					int sy = video->Y + y;
					if(sx < 0 || sy < 0) continue;
					plot((uint32_t)sx, (uint32_t)sy, videoPixels[(size_t)y * video->VideoWidth + x]);
				}
			}
		}
	}
}

template<uint32_t scale>
void HdNesPack<scale>::ProcessGrayscaleAndEmphasis(HdPpuPixelInfo &pixelInfo, uint32_t* outputBuffer, uint32_t hdScreenWidth)
{
	//Apply grayscale/emphasis bits on a scanline level (less accurate, but shouldn't cause issues and simpler to implement)
	if(pixelInfo.Grayscale) {
		uint32_t* out = outputBuffer;
		for(uint32_t y = 0; y < scale; y++) {
			for(uint32_t x = 0; x < hdScreenWidth; x++) {
				uint32_t &rgbValue = out[x];
				uint8_t average = (((rgbValue >> 16) & 0xFF) + ((rgbValue >> 8) & 0xFF) + (rgbValue & 0xFF)) / 3;
				rgbValue = (rgbValue & 0xFF000000) | (average << 16) | (average << 8) | average;
			}
			out += hdScreenWidth;
		}
	}

	if(pixelInfo.EmphasisBits) {
		uint8_t emphasisBits = pixelInfo.EmphasisBits;
		double red = 1.0, green = 1.0, blue = 1.0;
		if(emphasisBits & 0x01) {
			//Intensify red
			red *= 1.1;
			green *= 0.9;
			blue *= 0.9;
		}
		if(emphasisBits & 0x02) {
			//Intensify green
			green *= 1.1;
			red *= 0.9;
			blue *= 0.9;
		}
		if(emphasisBits & 0x04) {
			//Intensify blue
			blue *= 1.1;
			red *= 0.9;
			green *= 0.9;
		}

		uint32_t* out = outputBuffer;
		for(uint32_t y = 0; y < scale; y++) {
			for(uint32_t x = 0; x < hdScreenWidth; x++) {
				uint32_t &rgbValue = out[x];

				rgbValue = 0xFF000000 |
					(std::min<uint16_t>((uint16_t)(((rgbValue >> 16) & 0xFF) * red), 255) << 16) |
					(std::min<uint16_t>((uint16_t)(((rgbValue >> 8) & 0xFF) * green), 255) << 8) |
					std::min<uint16_t>((uint16_t)((rgbValue & 0xFF) * blue), 255);
			}
			out += hdScreenWidth;
		}
	}
}

template HdNesPack<1>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<2>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<3>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<4>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<5>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<6>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<7>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<8>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<9>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
template HdNesPack<10>::HdNesPack(NesConsole* console, EmuSettings* settings, HdPackData* hdData);
