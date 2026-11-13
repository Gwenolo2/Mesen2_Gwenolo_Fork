#include "pch.h"
#include <cstdio>
#include <cstring>
#include <tuple>
#include "Gameboy/GbHdVideoFilter.h"
#include "Gameboy/GbConstants.h"
#include "Gameboy/GbPpu.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/VirtualFile.h"

GbHdVideoFilter::GbHdVideoFilter(Emulator* emu, const GbHdPackData& pack) : GbDefaultVideoFilter(emu, false)
{
	_pack = pack;
	_scale = pack.Scale;
	if(_scale < 1 || _scale > 10) {
		_scale = 4;
	}
	LoadImages();
}

void GbHdVideoFilter::LoadImages()
{
	int loaded = 0;
	for(const std::string& img : _pack.Images) {
		std::vector<uint8_t> raw;
		std::vector<uint32_t> pixels;
		uint32_t w = 0;
		uint32_t h = 0;
		VirtualFile file(FolderUtilities::CombinePath(_pack.FolderPath, img));
		if(file.IsValid() && file.ReadFile(raw) && PNGHelper::ReadPNG(raw, pixels, w, h)) {
			loaded++;
		} else {
			MessageManager::Log("[HD-GB-FLT] Image illisible : " + img);
			pixels.clear();
			w = 0;
			h = 0;
		}
		_images.push_back(std::move(pixels));
		_imageWidths.push_back(w);
		_imageHeights.push_back(h);
	}
	MessageManager::Log("[HD-GB-FLT] " + std::to_string(loaded) + "/" + std::to_string(_pack.Images.size()) + " images chargees, scale " + std::to_string(_scale));
}

void GbHdVideoFilter::OnBeforeApplyFilter()
{
	GbDefaultVideoFilter::OnBeforeApplyFilter();
	// Pas de melange d'images avec le pack HD : les pixels HD ne seraient pas melanges
	_blendFrames = false;
}

FrameInfo GbHdVideoFilter::GetFrameInfo()
{
	FrameInfo frameInfo;
	frameInfo.Width = GbConstants::ScreenWidth * _scale;
	frameInfo.Height = GbConstants::ScreenHeight * _scale;
	return frameInfo;
}

void GbHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	uint32_t* out = GetOutputBuffer();
	const GbHdPixelInfo* info = static_cast<const GbHdPixelInfo*>(_frameData);
	const uint32_t outWidth = GbConstants::ScreenWidth * _scale;

	for(uint32_t y = 0; y < GbConstants::ScreenHeight; y++) {
		for(uint32_t x = 0; x < GbConstants::ScreenWidth; x++) {
			uint32_t offset = y * GbConstants::ScreenWidth + x;
			bool drawn = false;

			if(info && (info[offset].Layer == 1 || info[offset].Layer == 2)) {
				auto it = _pack.Tiles.find(std::make_tuple(info[offset].Hash, info[offset].Palette, info[offset].Flags));
				if(it != _pack.Tiles.end()) {
					const GbHdTileRef& ref = it->second;
					const std::vector<uint32_t>& img = _images[ref.ImageIndex];
					uint32_t imgW = _imageWidths[ref.ImageIndex];
					uint32_t imgH = _imageHeights[ref.ImageIndex];
					uint32_t srcX = ref.X + info[offset].Col * _scale;
					uint32_t srcY = ref.Y + info[offset].Row * _scale;
					if(!img.empty() && srcX + _scale <= imgW && srcY + _scale <= imgH) {
						for(uint32_t dy = 0; dy < _scale; dy++) {
							const uint32_t* srcRow = &img[(srcY + dy) * imgW + srcX];
							uint32_t* dstRow = out + (y * _scale + dy) * outWidth + x * _scale;
							memcpy(dstRow, srcRow, _scale * sizeof(uint32_t));
							for(uint32_t dx = 0; dx < _scale; dx++) {
								if((srcRow[dx] >> 24) == 0) {
									_transparentPixels++;
									if(_samplesLogged < 5) {
										_samplesLogged++;
										char hashStr[32];
										snprintf(hashStr, sizeof(hashStr), "%016llX", (unsigned long long)info[offset].Hash);
										MessageManager::Log("[HD-GB-FLT] pixel transparent : hash " + std::string(hashStr) + ", palette " + std::to_string(info[offset].Palette) + ", flags " + std::to_string(info[offset].Flags) + ", image " + std::to_string(ref.ImageIndex) + ", tuile (" + std::to_string(ref.X) + "," + std::to_string(ref.Y) + "), case (" + std::to_string(info[offset].Col) + "," + std::to_string(info[offset].Row) + "), valeur 0x" + std::to_string(srcRow[dx]));
									}
								}
							}
						}
						drawn = true;
						_hdPixels++;
						if(info[offset].Layer == 2) {
							_hdSpritePixels++;
						}
					}
				}
			}

			if(!drawn) {
				// Repli : rendu par defaut, pixel agrandi
				uint32_t color = GetPixel(ppuOutputBuffer, offset);
				for(uint32_t dy = 0; dy < _scale; dy++) {
					uint32_t* dstRow = out + (y * _scale + dy) * outWidth + x * _scale;
					for(uint32_t dx = 0; dx < _scale; dx++) {
						dstRow[dx] = color;
					}
				}
				_fallbackPixels++;
			}
		}
	}

	if(++_frameCounter >= 300) {
		uint64_t total = _hdPixels + _fallbackPixels;
		uint64_t percent = total ? (_hdPixels * 100) / total : 0;
		MessageManager::Log("[HD-GB-FLT] 300 images : pixels HD " + std::to_string(_hdPixels) + " (objets " + std::to_string(_hdSpritePixels) + "), replis " + std::to_string(_fallbackPixels) + " (" + std::to_string(percent) + "% HD), transparents " + std::to_string(_transparentPixels));
		_transparentPixels = 0;
		_samplesLogged = 0;
		_hdPixels = 0;
		_hdSpritePixels = 0;
		_fallbackPixels = 0;
		_frameCounter = 0;
	}
}
