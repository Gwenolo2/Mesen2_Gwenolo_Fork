#include "pch.h"
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include "NES/HdPacks/HdNesPack.h"
#include "NES/HdPacks/HdPackLoader.h"
#include "NES/NesConsole.h"
#include "NES/BaseMapper.h"
#include "NES/NesDefaultVideoFilter.h"
#include "Shared/MessageManager.h"
#include "Shared/EmuSettings.h"
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
		output[0] = input[0] + (uint8_t)((invertedAlpha * output[0]) >> 8);
		output[1] = input[1] + (uint8_t)((invertedAlpha * output[1]) >> 8);
		output[2] = input[2] + (uint8_t)((invertedAlpha * output[2]) >> 8);
		output[3] = 0xFF;
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
void HdNesPack<scale>::DrawCustomBackground(HdBackgroundInfo& bgInfo, uint32_t *outputBuffer, uint32_t x, uint32_t y, uint32_t screenWidth)
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

	if(bgInfo.Brightness == 255) {
		for(uint32_t i = 0; i < scale; i++) {
			for(uint32_t j = 0; j < scale; j++) {
				uint32_t pixelColor = pngData[j];
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
				uint32_t pixelColor = AdjustBrightness((uint8_t*)(pngData + j), bgInfo.Brightness);
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
			p.X = WrapCoord(p.X + (p.VX + ov.WindX + ov.Turbulence * std::sin(w * p.Age + p.Seed1)) * dt, fieldW);
			p.Y = WrapCoord(p.Y + (p.VY + ov.WindY + ov.Turbulence * std::sin(w * p.Age + p.Seed2)) * dt, fieldH);
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
		if(ov.IsPlainScreen || ov.IsEllipse) {
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

	for(auto& ovPtr : _hdData->Overlays) {
		NesHdOverlayInfo& ov = *ovPtr;
		if(!ov.Active || !(ov.IsPlainScreen || ov.IsEllipse || ov.IsNoise || ov.IsRays || ov.IsAurora)) {
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
	auto liqHash = [](uint32_t v) -> uint32_t { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; };
	for(auto& liqPtr : _hdData->Liquids) {
		NesHdLiquidInfo& liq = *liqPtr;
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
static uint32_t LiquidTopPixel(const NesHdLiquidInfo& liq, float xN, float yN, uint32_t dst)
{
	auto chan = [](uint32_t c, int sh) -> float { return (float)((c >> sh) & 0xFF); };
	const float zoneH = std::max(1.0f, liq.YMax - liq.YMin);
	const float depth = std::clamp((yN - liq.YMin) / zoneH, 0.0f, 1.0f);
	float rr = chan(liq.Color, 16) + (chan(liq.Deep, 16) - chan(liq.Color, 16)) * depth;
	float gg = chan(liq.Color, 8) + (chan(liq.Deep, 8) - chan(liq.Color, 8)) * depth;
	float bb = chan(liq.Color, 0) + (chan(liq.Deep, 0) - chan(liq.Color, 0)) * depth;
	const float twoPi = 2.0f * OverlayPi;
	const float w1 = std::sin(twoPi * (xN / liq.Wave + liq.Speed * liq.Time) + liq.Phase * (OverlayPi / 180.0f));
	const float w2 = std::sin(twoPi * (yN / (liq.Wave * 0.6f) - 0.7f * liq.Speed * liq.Time));
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
		const float u = (xN + liq.ScrollX * liq.Time) / liq.TexScale;
		const float v = (yN - liq.YMin) / liq.TexScale;
		int tx = (int)std::floor(u) % tw; if(tx < 0) tx += tw;
		int ty = (int)std::floor(v) % th; if(ty < 0) ty += th;
		const uint32_t c = liq.Texture->PixelData[(size_t)ty * tw + tx];
		const float a = chan(c, 24) / 255.0f;
		rr = chan(c, 16) + rr * (1.0f - a);
		gg = chan(c, 8) + gg * (1.0f - a);
		bb = chan(c, 0) + bb * (1.0f - a);
	}
	const float A = liq.Alpha;
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
						px = LiquidTopPixel(liq, xN, yN, dst);
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
			const float radiusNes = hasTex ? std::max(texW, texH) * sizeMul * 0.5f / fScale : ov.DotSize * sizeMul * 0.5f;
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
					const float cxH = (cx - originNes) * fScale;
					const float cyH = (cy - (float)yStart) * fScale;

					if(hasTex) {
						const float halfExt = std::max(texW, texH) * sizeMul * 0.5f;
						const float reach = halfExt * 1.5f; // marge pour la rotation
						const int xa = std::max(0, (int)std::floor(cxH - reach));
						const int xb = std::min((int)screenWidth - 1, (int)std::ceil(cxH + reach));
						const int ya = std::max(0, (int)std::floor(cyH - reach));
						const int yb = std::min(hdHeight - 1, (int)std::ceil(cyH + reach));
						const float cosR = std::cos(p.Rot), sinR = std::sin(p.Rot);
						for(int sy = ya; sy <= yb; sy++) {
							for(int sx = xa; sx <= xb; sx++) {
								// Transformation inverse : pixel de sortie -> pixel de la texture
								const float dx = (float)sx + 0.5f - cxH, dy = (float)sy + 0.5f - cyH;
								const float lx = dx * cosR + dy * sinR;
								const float ly = -dx * sinR + dy * cosR;
								const float u = lx / sizeMul + texW * 0.5f;
								const float v = ly / sizeMul + texH * 0.5f;
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
						const int xa = std::max(0, (int)std::floor(cxH - rHD));
						const int xb = std::min((int)screenWidth - 1, (int)std::ceil(cxH + rHD));
						const int ya = std::max(0, (int)std::floor(cyH - rHD));
						const int yb = std::min(hdHeight - 1, (int)std::ceil(cyH + rHD));
						for(int sy = ya; sy <= yb; sy++) {
							for(int sx = xa; sx <= xb; sx++) {
								const float dx = (float)sx + 0.5f - cxH, dy = (float)sy + 0.5f - cyH;
								const float d = std::sqrt(dx * dx + dy * dy) / rHD;
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
	int32_t scrollX = ((lineFirstPixel.TmpVideoRamAddr & 0x1F) << 3) | lineFirstPixel.XScroll | ((lineFirstPixel.TmpVideoRamAddr & 0x400) ? 0x100 : 0);
	int32_t scrollY = (((lineFirstPixel.TmpVideoRamAddr & 0x3E0) >> 2) | ((lineFirstPixel.TmpVideoRamAddr & 0x7000) >> 12)) + ((lineFirstPixel.TmpVideoRamAddr & 0x800) ? 240 : 0);
	
	for(int layer = 0; layer < 5; layer++) {
		for(int i = 0; i < _activeBgCount[layer]; i++) {
			int cfgIndex = layer * HdNesPack::PriorityLevelsPerLayer + i;
			HdBgConfig cfg = _bgConfig[cfgIndex];
			cfg.BgMinX = -1;
			cfg.BgMaxX = -1;
			if(cfg.BackgroundIndex >= 0) {
				HdBackgroundInfo& bgInfo = _hdData->BackgroundsByPriority[cfg.BgPriority][cfg.BackgroundIndex];

				// Operateur de defilement (comme SNES) : multiplication par defaut
				const float rx = bgInfo.HorizontalScrollRatio;
				const float ry = bgInfo.VerticalScrollRatio;
				const float frame = (float)_hdScreenInfo->FrameNumber;
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

		//Decodage des PNG des <background> actifs ici (sequentiel), plus dans la boucle des lignes
		for(uint32_t i = 0; i < activeCount; i++) {
			HdBgConfig& cfg = _bgConfig[layer * HdNesPack::PriorityLevelsPerLayer + i];
			_hdData->BackgroundsByPriority[cfg.BgPriority][cfg.BackgroundIndex].Data->Init();
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
		switch(bgInfo.BlendMode) {
			case HdPackBlendMode::Alpha: DrawCustomBackground<HdPackBlendMode::Alpha>(bgInfo, outputBuffer, x + bgConfig.BgScrollX, y + bgConfig.BgScrollY, screenWidth); break;
			case HdPackBlendMode::Add: DrawCustomBackground<HdPackBlendMode::Add>(bgInfo, outputBuffer, x + bgConfig.BgScrollX, y + bgConfig.BgScrollY, screenWidth); break;
			case HdPackBlendMode::Subtract: DrawCustomBackground<HdPackBlendMode::Subtract>(bgInfo, outputBuffer, x + bgConfig.BgScrollX, y + bgConfig.BgScrollY, screenWidth); break;
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
	PrepareLiquids(screenWidth);
	_zEnabled = !_hdData->Videos.empty() || !_hdData->Overlays.empty();
	if(_zEnabled) {
		std::fill(_zNes.begin(), _zNes.end(), (uint8_t)0);
	}

	lap(0);
	//Passe 1 (sequentielle) : quelle tile HD pour chaque pixel (fond + sprites)
	for(uint32_t i = yStart; i < yEnd; i++) {
		OnLineStart(hdScreenInfo->ScreenTiles[i << 8]);
		for(uint32_t j = xStart; j < xEnd; j++) {
			ResolvePixel(j, i, hdScreenInfo->ScreenTiles[i * 256 + j]);
		}
	}

	lap(1);
	//Passe 2 (parallele) : dessin. Chaque ligne NES ecrit ses propres lignes HD, sans recouvrement.
	auto drawLine = [&](uint32_t i) {
		HdBgConfig lineCfg[50];
		ComputeLineBgConfig(hdScreenInfo->ScreenTiles[i << 8], (uint8_t)i, lineCfg);
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
	lap(5);
	//Videos : appliquees en dernier (apres grayscale/emphasis), sequentiellement
	if(_zEnabled) {
		ProcessVideos(outputBuffer, screenWidth, overscan);
	}
	lap(6);

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
				px = LiquidTopPixel(liq, xN, yN, px);
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
