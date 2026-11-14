#include "pch.h"
#include <cstdio>
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
			MessageManager::Log("[HD-GB-FLT] image " + img + " : " + std::to_string(w) + "x" + std::to_string(h) + ", pixels " + std::to_string(pixels.size()));
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

// Pixel (dx,dy) du bloc scale x scale de la tile (col,row) dans l'atlas. False si hors image.
bool GbHdVideoFilter::SamplePixel(const GbHdTileRef& ref, uint32_t col, uint32_t row, uint32_t dx, uint32_t dy, uint32_t& pixel)
{
	if(ref.ImageIndex >= _images.size()) {
		return false;
	}
	const std::vector<uint32_t>& img = _images[ref.ImageIndex];
	uint32_t imgW = _imageWidths[ref.ImageIndex];
	uint32_t imgH = _imageHeights[ref.ImageIndex];
	uint32_t sx = ref.X + col * _scale + dx;
	uint32_t sy = ref.Y + row * _scale + dy;
	if(img.empty() || sx >= imgW || sy >= imgH) {
		return false;
	}
	pixel = img[sy * imgW + sx];
	return true;
}

void GbHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	uint32_t* out = GetOutputBuffer();
	const GbHdFrameData* frameData = static_cast<const GbHdFrameData*>(_frameData);
	const GbHdPixelInfo* info = frameData ? frameData->Pixels : nullptr;
	const std::vector<GbHdSpriteInfo>* sprites = frameData ? frameData->Sprites : nullptr;
	const uint32_t outWidth = GbConstants::ScreenWidth * _scale;

	// Passe 1 : fonds HD ; le reste (sprites compris) en rendu par defaut agrandi
	for(uint32_t y = 0; y < GbConstants::ScreenHeight; y++) {
		for(uint32_t x = 0; x < GbConstants::ScreenWidth; x++) {
			uint32_t offset = y * GbConstants::ScreenWidth + x;
			uint32_t fallback = GetPixel(ppuOutputBuffer, offset);
			const GbHdPixelInfo* p = info ? &info[offset] : nullptr;
			const GbHdTileRef* ref = nullptr;
			if(p && p->Layer == 1) {
				auto it = _pack.Tiles.find(std::make_tuple(p->Hash, p->Palette, p->Flags));
				if(it != _pack.Tiles.end()) {
					ref = &it->second;
				}
			}
			if(ref) {
				_hdPixels++;
			} else {
				_fallbackPixels++;
			}
			for(uint32_t dy = 0; dy < _scale; dy++) {
				uint32_t* dstRow = out + (y * _scale + dy) * outWidth + x * _scale;
				for(uint32_t dx = 0; dx < _scale; dx++) {
					uint32_t px = 0;
					dstRow[dx] = (ref && SamplePixel(*ref, p->Col, p->Row, dx, dy, px) && (px >> 24)) ? px : fallback;
				}
			}
		}
	}

	// Passe 2 : sprites HD par empreinte, sur la grille OAM de l'image
	if(info && sprites) {
		const int32_t screenW = (int32_t)GbConstants::ScreenWidth;
		const int32_t screenH = (int32_t)GbConstants::ScreenHeight;
		for(const GbHdSpriteInfo& s : *sprites) {
			for(int32_t r = 0; r < s.Height; r++) {
				int32_t sy = s.Y + r;
				if(sy < 0 || sy >= screenH) {
					continue;
				}
				// Ligne a l'ecran r ; tile lue selon le flip vertical, ligne d'atlas = r & 7 (affichage)
				int32_t vr = (s.Flags & 4) ? (s.Height - 1 - r) : r;
				uint64_t hash = s.Hash[vr >> 3];
				uint32_t row = (uint32_t)(r & 7);
				for(int32_t c = 0; c < 8; c++) {
					int32_t sx = s.X + c;
					if(sx < 0 || sx >= screenW) {
						continue;
					}
					const GbHdPixelInfo& p = info[(uint32_t)sy * GbConstants::ScreenWidth + (uint32_t)sx];
					bool allowed = true;
					if(p.Layer == 2) {
						// Le PPU attribue ce pixel a un objet : seulement si c'est ce sprite
						allowed = p.Hash == hash && p.Palette == s.Palette && p.Flags == s.Flags;
					} else if(p.Layer == 1) {
						// Fond visible : le sprite passe derriere si priorite fond et couleur de fond non nulle
						allowed = !s.BgPriority || p.BgColor == 0;
					}
					if(!allowed) {
						continue;
					}
					auto it = _pack.Tiles.find(std::make_tuple(hash, s.Palette, s.Flags));
					if(it == _pack.Tiles.end()) {
						continue;
					}
					bool any = false;
					for(uint32_t dy = 0; dy < _scale; dy++) {
						for(uint32_t dx = 0; dx < _scale; dx++) {
							uint32_t px = 0;
							if(SamplePixel(it->second, (uint32_t)c, row, dx, dy, px) && (px >> 24)) {
								out[((uint32_t)sy * _scale + dy) * outWidth + (uint32_t)sx * _scale + dx] = px;
								any = true;
							}
						}
					}
					if(any) {
						_hdSpritePixels++;
					}
				}
			}
		}
	}

	if(++_frameCounter >= 300) {
		uint64_t total = _hdPixels + _fallbackPixels;
		uint64_t percent = total ? (_hdPixels * 100) / total : 0;
		MessageManager::Log("[HD-GB-FLT] 300 images : fond HD " + std::to_string(_hdPixels) + " (" + std::to_string(percent) + "%), objets HD " + std::to_string(_hdSpritePixels) + ", replis " + std::to_string(_fallbackPixels));
		_hdPixels = 0;
		_hdSpritePixels = 0;
		_fallbackPixels = 0;
		_frameCounter = 0;
	}
}
