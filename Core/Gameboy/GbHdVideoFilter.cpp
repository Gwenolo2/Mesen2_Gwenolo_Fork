#include "pch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <tuple>
#include "Gameboy/GbHdVideoFilter.h"
#include "Gameboy/GbConstants.h"
#include "Gameboy/GbPpu.h"
#include "Gameboy/HdPacks/GbHdDiorama.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/VirtualFile.h"

namespace {
	// Compteurs d'une bande (un par thread, additionnes a la fin : pas de course)
	struct BandCounts
	{
		uint64_t hd = 0;
		uint64_t fallback = 0;
		uint64_t bg = 0;
		uint64_t sprite = 0;
	};

	// Melange src sur dst avec l'alpha a (0-255). Resultat opaque.
	uint32_t BlendPixel(uint32_t src, uint32_t dst, uint32_t a)
	{
		uint32_t out = 0xFF000000;
		for(uint32_t shift = 0; shift <= 16; shift += 8) {
			uint32_t s = (src >> shift) & 0xFF;
			uint32_t d = (dst >> shift) & 0xFF;
			uint32_t c = (s * a + d * (255 - a) + 127) / 255;
			out |= c << shift;
		}
		return out;
	}

	// Luminosite : multiplie les canaux RGB, l'alpha est conserve
	uint32_t AdjustLuminosity(uint32_t px, double lum)
	{
		if(lum == 1.0) {
			return px;
		}
		uint32_t out = px & 0xFF000000;
		for(uint32_t shift = 0; shift <= 16; shift += 8) {
			double c = (double)((px >> shift) & 0xFF) * lum;
			uint32_t v = (uint32_t)std::min(255.0, std::max(0.0, std::round(c)));
			out |= v << shift;
		}
		return out;
	}

	// Scroll selon l'operateur (comme NES / SNES). scroll = SCX ou SCY (pixels SD), frame = compteur d'images
	double ScrollValue(GbHdBgOperator op, double scroll, double ratio, uint64_t frame)
	{
		double s = scroll;
		switch(op) {
			case GbHdBgOperator::Multiply: return s * ratio;
			case GbHdBgOperator::Divide: return ratio == 0.0 ? s : s / ratio;
			case GbHdBgOperator::Plus: return s + ratio * (double)frame;
			case GbHdBgOperator::Minus: return s - ratio * (double)frame;
		}
		return 0.0;
	}
}

GbHdVideoFilter::GbHdVideoFilter(Emulator* emu, const GbHdPackData& pack) : GbDefaultVideoFilter(emu, false)
{
	_pack = pack;
	_scale = pack.Scale;
	if(_scale < 1 || _scale > 10) {
		_scale = 4;
	}
	// Threads crees une seule fois : coeurs - 2 (emulation + systeme), borne a [1, 8], appelant compris
	_pool.Start(std::clamp((int)std::thread::hardware_concurrency() - 2, 1, 8));
	LoadImages();
	// Diorama / <DayNight> (doc 56) : les conditions des regles evaluent les termes Game Boy avec la memoire de l'image
	if(_pack.Dio) {
		_pack.Dio->Eval = [this](const std::vector<GbHdTerm>& t) { return EvaluateTerms(t, 0, _dioMem, 0, 0, false); };
		_pack.Dio->EvalOwner = this;
	}
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
// Si mirrorH / mirrorV : le bloc entier de la tile est retourne (la tile est affichee dans l'autre sens).
bool GbHdVideoFilter::SamplePixel(const GbHdTileRef& ref, bool mirrorH, bool mirrorV, uint32_t col, uint32_t row, uint32_t dx, uint32_t dy, uint32_t& pixel)
{
	if(ref.ImageIndex >= _images.size()) {
		return false;
	}
	if(mirrorH && col < 8) {
		col = 7 - col;
		dx = _scale - 1 - dx;
	}
	if(mirrorV && row < 8) {
		row = 7 - row;
		dy = _scale - 1 - dy;
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

// Pixel (x,y) de l'image de fond, decalee de (offX, offY) pixels HD.
// Sans repetition (tileX / tileY), hors de l'image = pas de pixel (le fond SD reste visible).
bool GbHdVideoFilter::SampleBackgroundPixel(uint32_t imageIndex, const GbHdBackground& bg, int64_t offX, int64_t offY, uint32_t x, uint32_t y, uint32_t& pixel) const
{
	if(imageIndex >= _images.size()) {
		return false;
	}
	const std::vector<uint32_t>& img = _images[imageIndex];
	const int64_t w = (int64_t)_imageWidths[imageIndex];
	const int64_t h = (int64_t)_imageHeights[imageIndex];
	if(img.empty() || w <= 0 || h <= 0) {
		return false;
	}
	int64_t sx = (int64_t)x + offX;
	int64_t sy = (int64_t)y + offY;
	if(bg.TileX) {
		sx %= w;
		if(sx < 0) {
			sx += w;
		}
	} else if(sx < 0 || sx >= w) {
		return false;
	}
	if(bg.TileY) {
		sy %= h;
		if(sy < 0) {
			sy += h;
		}
	} else if(sy < 0 || sy >= h) {
		return false;
	}
	pixel = img[(size_t)(sy * w + sx)];
	return true;
}

// Premier fond dont l'image existe et dont les termes sont vrais (fond sans termes : toujours vrai).
// Tous les fonds actifs (termes vrais), dans l'ordre du fichier : ils sont empiles par Z comme sur NES
std::vector<const GbHdBackground*> GbHdVideoFilter::FindBackgrounds(const std::vector<uint8_t>* mem) const
{
	std::vector<const GbHdBackground*> result;
	for(const GbHdBackground& bg : _pack.Backgrounds) {
		if(bg.Frames.empty() || bg.Frames[0] >= _images.size() || _images[bg.Frames[0]].empty()) {
			continue;
		}
		if(EvaluateTerms(bg.Terms, 0, mem, 0, 0, false)) {
			result.push_back(&bg);
		}
	}
	return result;
}

// Condition memoryCheckConstant : (octet & masque) compare a la (aux) constante(s). Faux si l'octet n'est pas releve.
// tileNearby / spriteNearby : une tile BG (ou sprite) de ce hash a son origine en (px, py).
// Origine BG = pixel dont la tile commence la (Col = 0, Row = 0 dans la tile).
static bool BgTileAt(const GbHdCondition& c, const GbHdPixelInfo* info, int32_t px, int32_t py)
{
	if(!info || px < 0 || py < 0 || px >= (int32_t)GbConstants::ScreenWidth || py >= (int32_t)GbConstants::ScreenHeight) {
		return false;
	}
	const GbHdPixelInfo& p = info[py * GbConstants::ScreenWidth + px];
	return p.Layer == 1 && p.Col == 0 && p.Row == 0 && p.Hash == c.Hash && (c.IgnorePalette || p.Palette == c.Palette);
}

// Sous-tile d'un sprite a l'origine (px, py). Sprite 8x16 : le hash Hash[b] est affiche en haut si retourne verticalement.
static bool SpriteTileAt(const GbHdCondition& c, const std::vector<GbHdSpriteInfo>* sprites, int32_t px, int32_t py)
{
	if(!sprites) {
		return false;
	}
	for(const GbHdSpriteInfo& s : *sprites) {
		const bool h16 = s.Height == 16;
		const bool flipV = (s.Flags & 4) != 0;
		for(int b = 0; b < (h16 ? 2 : 1); b++) {
			const int32_t block = h16 ? (flipV ? 1 - b : b) : 0;
			if(s.X == px && s.Y + block * 8 == py && s.Hash[b] == c.Hash && (c.IgnorePalette || s.Palette == c.Palette)) {
				return true;
			}
		}
	}
	return false;
}

// Joker sur les deux axes : ne depend pas de la tile evaluee, donc calcule une fois par image.
static bool AnywhereTile(const GbHdCondition& c, const GbHdPixelInfo* info)
{
	for(int32_t py = 0; py < (int32_t)GbConstants::ScreenHeight; py++) {
		for(int32_t px = 0; px < (int32_t)GbConstants::ScreenWidth; px++) {
			if(BgTileAt(c, info, px, py)) {
				return true;
			}
		}
	}
	return false;
}

static bool AnywhereSprite(const GbHdCondition& c, const std::vector<GbHdSpriteInfo>* sprites)
{
	if(!sprites) {
		return false;
	}
	for(const GbHdSpriteInfo& s : *sprites) {
		for(int b = 0; b < (s.Height == 16 ? 2 : 1); b++) {
			if(s.Hash[b] == c.Hash && (c.IgnorePalette || s.Palette == c.Palette)) {
				return true;
			}
		}
	}
	return false;
}

// tileAtPosition : une case 8x8 (ou toute colonne / ligne avec un joker) contient une tile BG de ce hash.
// Lu dans le tampon de l'image courante (calque 1 = fond).
static bool TileAtPositionMatch(const GbHdCondition& c, const GbHdPixelInfo* info)
{
	if(!info) {
		return false;
	}
	const int cols = (int)GbConstants::ScreenWidth / 8;
	const int rows = (int)GbConstants::ScreenHeight / 8;
	const int c0 = c.AnyX ? 0 : c.X;
	const int c1 = c.AnyX ? cols - 1 : c.X;
	const int r0 = c.AnyY ? 0 : c.Y;
	const int r1 = c.AnyY ? rows - 1 : c.Y;
	for(int row = r0; row <= r1; row++) {
		for(int col = c0; col <= c1; col++) {
			if(col < 0 || col >= cols || row < 0 || row >= rows) {
				continue;
			}
			for(int py = 0; py < 8; py++) {
				for(int px = 0; px < 8; px++) {
					const GbHdPixelInfo& p = info[(row * 8 + py) * GbConstants::ScreenWidth + col * 8 + px];
					if(p.Layer == 1 && p.Hash == c.Hash && (c.IgnorePalette || p.Palette == c.Palette)) {
						return true;
					}
				}
			}
		}
	}
	return false;
}

// spriteAtPosition : un sprite dont le coin haut-gauche tombe dans la case 8x8 (x, y en unites de 8 px).
// Liste des sprites de l'image : elle ne contient que les sprites visibles (filtre de SendFrame).
static bool SpriteAtPositionMatch(const GbHdCondition& c, const std::vector<GbHdSpriteInfo>* sprites)
{
	if(!sprites) {
		return false;
	}
	for(const GbHdSpriteInfo& s : *sprites) {
		if(!c.AnyX && (s.X < c.X * 8 || s.X >= c.X * 8 + 8)) {
			continue;
		}
		if(!c.AnyY && (s.Y < c.Y * 8 || s.Y >= c.Y * 8 + 8)) {
			continue;
		}
		if(!c.IgnorePalette && s.Palette != c.Palette) {
			continue;
		}
		if(s.Hash[0] == c.Hash || (s.Height == 16 && s.Hash[1] == c.Hash)) {
			return true;
		}
	}
	return false;
}

// Decalage relatif a la tile evaluee (origine ox, oy). Le decalage s'inverse si la tile est retournee (comme SNES / NES).
bool GbHdVideoFilter::NearbyAt(const GbHdCondition& c, int32_t ox, int32_t oy, uint8_t flags) const
{
	const int32_t px0 = ox + c.X * ((flags & 2) ? -1 : 1);
	const int32_t py0 = oy + c.Y * ((flags & 4) ? -1 : 1);
	const bool tile = c.Type == GbHdCondition::Kind::TileNearby;
	if(c.AnyX && !c.AnyY) {
		for(int32_t px = 0; px < (int32_t)GbConstants::ScreenWidth; px++) {
			if(tile ? BgTileAt(c, _frameInfo, px, py0) : SpriteTileAt(c, _frameSprites, px, py0)) {
				return true;
			}
		}
		return false;
	}
	if(!c.AnyX && c.AnyY) {
		for(int32_t py = 0; py < (int32_t)GbConstants::ScreenHeight; py++) {
			if(tile ? BgTileAt(c, _frameInfo, px0, py) : SpriteTileAt(c, _frameSprites, px0, py)) {
				return true;
			}
		}
		return false;
	}
	return tile ? BgTileAt(c, _frameInfo, px0, py0) : SpriteTileAt(c, _frameSprites, px0, py0);
}

bool GbHdVideoFilter::EvaluateCondition(const GbHdCondition& cond, const std::vector<uint8_t>* mem, const GbHdPixelInfo* info, const std::vector<GbHdSpriteInfo>* sprites) const
{
	if(cond.Type == GbHdCondition::Kind::FrameRange) {
		return (_frameNumber % cond.Divisor) >= cond.Threshold;
	}
	if(cond.Type == GbHdCondition::Kind::TileAtPosition) {
		return TileAtPositionMatch(cond, info);
	}
	if(cond.Type == GbHdCondition::Kind::SpriteAtPosition) {
		return SpriteAtPositionMatch(cond, sprites);
	}
	if(cond.Type == GbHdCondition::Kind::TileNearby || cond.Type == GbHdCondition::Kind::SpriteNearby) {
		// Seul le joker sur les deux axes est precalcule ; les autres sont evalues par tile (NearbyAt)
		if(!(cond.AnyX && cond.AnyY)) {
			return false;
		}
		return cond.Type == GbHdCondition::Kind::TileNearby ? AnywhereTile(cond, info) : AnywhereSprite(cond, sprites);
	}
	if(!mem || cond.Slot >= mem->size() || cond.Values.empty()) {
		return false;
	}
	uint8_t value = (uint8_t)((*mem)[cond.Slot] & cond.Mask);
	switch(cond.Operator) {
		case GbHdCondition::Op::Eq: return value == cond.Values[0];
		case GbHdCondition::Op::Ne: return value != cond.Values[0];
		case GbHdCondition::Op::Gt: return value > cond.Values[0];
		case GbHdCondition::Op::Lt: return value < cond.Values[0];
		case GbHdCondition::Op::Ge: return value >= cond.Values[0];
		case GbHdCondition::Op::Le: return value <= cond.Values[0];
		case GbHdCondition::Op::In:
			for(uint8_t v : cond.Values) {
				if(value == v) {
					return true;
				}
			}
			return false;
	}
	return false;
}

// Tous les termes doivent etre vrais. hmirror / vmirror : sens reel de la tile affichee (flags de la cle).
bool GbHdVideoFilter::EvaluateTerms(const std::vector<GbHdTerm>& terms, uint8_t flags, const std::vector<uint8_t>* mem, int32_t ox, int32_t oy, bool hasCtx) const
{
	for(const GbHdTerm& term : terms) {
		bool value = false;
		if(term.Name == "hmirror") {
			value = (flags & 2) != 0;
		} else if(term.Name == "vmirror") {
			value = (flags & 4) != 0;
		} else {
			auto it = _pack.Conditions.find(term.Name);
			if(it == _pack.Conditions.end()) {
				return false;
			}
			auto cached = _condResult.find(term.Name);
			const GbHdCondition& cond = it->second;
			if(cond.Type == GbHdCondition::Kind::TileNearby || cond.Type == GbHdCondition::Kind::SpriteNearby) {
				if(cond.AnyX && cond.AnyY) {
					value = (cached != _condResult.end()) && cached->second;
				} else {
					value = hasCtx && NearbyAt(cond, ox, oy, flags);
				}
			} else {
				value = (cached != _condResult.end()) && cached->second;
			}
		}
		if(value == term.Negated) {
			return false;
		}
	}
	return true;
}

// Tile a utiliser pour (hash, palette, flags).
// 1) Tile conditionnelle exacte (premiere vraie), puis tile sans condition exacte.
// 2) Repli de miroir (comme NES / SNES) : tile du pack dans un autre sens (bit 2 = hflip, bit 4 = vflip),
//    retournee a l'affichage si son sens differe. Le bit 1 (sprite) reste celui de la cle.
GbHdTileMatch GbHdVideoFilter::FindTile(uint64_t hash, uint32_t palette, uint8_t flags, const std::vector<uint8_t>* mem, int32_t ox, int32_t oy, bool hasCtx) const
{
	GbHdTileMatch match;
	if(!_pack.ConditionalIndex.empty()) {
		auto ci = _pack.ConditionalIndex.find(std::make_pair(hash, palette));
		if(ci != _pack.ConditionalIndex.end()) {
			for(size_t idx : ci->second) {
				const GbHdConditionalTile& ct = _pack.ConditionalTiles[idx];
				if(ct.Flags == flags && EvaluateTerms(ct.Terms, flags, mem, ox, oy, hasCtx)) {
					match.Ref = &ct.Ref;
					return match;
				}
			}
		}
	}
	auto it = _pack.Tiles.find(std::make_tuple(hash, palette, flags));
	if(it != _pack.Tiles.end()) {
		match.Ref = &it->second;
		return match;
	}
	// Repli de miroir : essayer d'abord la tile sans retournement, puis les autres sens
	const uint8_t base = flags & 1;
	const uint8_t candidates[] = {(uint8_t)(base | 0), (uint8_t)(base | 2), (uint8_t)(base | 4), (uint8_t)(base | 6)};
	for(uint8_t g : candidates) {
		if(g == flags) {
			continue;
		}
		auto jt = _pack.Tiles.find(std::make_tuple(hash, palette, g));
		if(jt != _pack.Tiles.end()) {
			match.Ref = &jt->second;
			match.MirrorH = ((flags ^ g) & 2) != 0;
			match.MirrorV = ((flags ^ g) & 4) != 0;
			return match;
		}
	}
	return match;
}

void GbHdVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	uint32_t* out = GetOutputBuffer();
	const GbHdFrameData* frameData = static_cast<const GbHdFrameData*>(_frameData);
	const GbHdPixelInfo* info = frameData ? frameData->Pixels : nullptr;
	const std::vector<GbHdSpriteInfo>* sprites = frameData ? frameData->Sprites : nullptr;
	const std::vector<uint8_t>* mem = frameData ? frameData->Memory : nullptr;
	_frameNumber = frameData ? frameData->FrameNumber : 0;
	// Conditions evaluees une fois par image (position, memoire, numero d'image) : les threads ne font que lire
	_frameInfo = info;
	_frameSprites = sprites;
	_condResult.clear();
	for(const auto& kv : _pack.Conditions) {
		if(kv.second.Type == GbHdCondition::Kind::Variable) continue; // <setVariable> : evaluee apres les autres conditions
		_condResult[kv.first] = EvaluateCondition(kv.second, mem, info, sprites);
	}
	// [fork GB] <setVariable> (doc 57) : valeur courante de chaque variable, la derniere regle vraie gagne ; puis conditions $nom=k
	static int varLogs = 0;
	for(const GbHdVariable& var : _pack.Variables) {
		int32_t cur = -1;
		for(const GbHdVariable::Rule& r : var.Rules) {
			if(EvaluateTerms(r.Terms, 0, mem, 0, 0, false)) cur = (int32_t)r.ValueIndex;
		}
		_varCurrent[var.Name] = cur;
		auto logged = _varLogged.find(var.Name);
		if(logged == _varLogged.end() || logged->second != cur) {
			if(varLogs < 60) {
				varLogs++;
				MessageManager::Log("[HD-VAR] $" + var.Name + " = " + (cur >= 0 ? var.Values[(size_t)cur] : std::string("(aucune)")));
			}
			_varLogged[var.Name] = cur;
		}
	}
	for(const auto& kv : _pack.Conditions) {
		if(kv.second.Type != GbHdCondition::Kind::Variable) continue;
		auto it = _varCurrent.find(kv.second.VarName);
		_condResult[kv.first] = it != _varCurrent.end() && it->second == (int32_t)kv.second.VarIndex;
	}
	// <muteAudio> : canaux coupes = union des regles dont les termes sont vrais ; lu par l'APU (GbApu::UpdateOutput)
	if(_pack.AudioMuteMask) {
		uint8_t muteMask = 0;
		for(const GbHdAudioMute& rule : _pack.AudioMutes) {
			if(EvaluateTerms(rule.Terms, 0, mem, 0, 0, false)) {
				muteMask |= rule.Mask;
			}
		}
		_pack.AudioMuteMask->store(muteMask, std::memory_order_relaxed);
	}
	// <play_conditional_bgm> : premiere regle vraie du fichier -> piste demandee ; lue par GbHdAudio (thread de l'emulation)
	if(_pack.BgmRequest) {
		int32_t request = -1;
		for(const GbHdConditionalBgm& rule : _pack.ConditionalBgms) {
			if(EvaluateTerms(rule.Terms, 0, mem, 0, 0, false)) {
				request = (rule.TrackId << 1) | (rule.Loop ? 1 : 0);
				break;
			}
		}
		_pack.BgmRequest->store(request, std::memory_order_relaxed);
	}
	// Diorama / <DayNight> (doc 56) : memoire de l'image pour les conditions, avance des cycles jour / nuit, defilement cumule
	_dioMem = mem;
	_dnDioApplied = false;
	UpdateDayNight();
	if(frameData && _pack.Dio) {
		const int sx = frameData->ScrollX, sy = frameData->ScrollY;
		if(_dioLastSX >= 0) {
			int ddx = ((sx - _dioLastSX) % 256 + 256) % 256; if(ddx > 128) ddx -= 256;
			int ddy = ((sy - _dioLastSY) % 256 + 256) % 256; if(ddy > 128) ddy -= 256;
			if(std::abs(ddx) <= 64) _dioWX += (float)ddx;
			if(std::abs(ddy) <= 64) _dioWY += (float)ddy;
		} else { _dioWX = (float)sx; _dioWY = (float)sy; }
		_dioLastSX = sx; _dioLastSY = sy;
	}
	const uint32_t outWidth = GbConstants::ScreenWidth * _scale;
	const uint32_t screenH = GbConstants::ScreenHeight;

	// Fonds <background> : tous les fonds actifs, empiles par Z (comme sur NES). Echelle Game Boy :
	// Z < spriteZ - 10 : derriere les tiles ; spriteZ - 10 <= Z < spriteZ : devant les tiles, derriere les sprites ;
	// Z >= spriteZ : devant les sprites (et devant les tiles GBC prioritaires si Z >= spriteZ + 10).
	struct BgLayer
	{
		const GbHdBackground* Bg;
		uint32_t Image;
		int64_t OffX;
		int64_t OffY;
	};
	std::vector<BgLayer> layers;
	if(frameData) {
		for(const GbHdBackground* b : FindBackgrounds(mem)) {
			uint64_t count = b->Frames.size();
			uint64_t idx = 0;
			if(b->FramesBetweenSteps > 0) {
				idx = (_bgFrame / b->FramesBetweenSteps) % count;
			}
			if(b->Reverse) {
				idx = count - 1 - idx;
			}
			// ramx= / ramy= : defilement de base lu en RAM a la place de SCX / SCY
			double baseX = frameData->ScrollX, baseY = frameData->ScrollY;
			if(!b->RamX.Empty() || !b->RamY.Empty()) {
				std::vector<int32_t> wv;
				for(const GbHdWatchVar& w : _pack.WatchedVars) {
					uint32_t raw = 0;
					bool ok = mem != nullptr;
					for(uint32_t i = 0; ok && i < w.Size; i++) {
						if(w.Slot[i] >= mem->size()) { ok = false; break; }
						raw |= (uint32_t)(*mem)[w.Slot[i]] << (8 * i);
					}
					wv.push_back(ok ? HdWatchCompose(raw, w.Size, w.Signed) : INT32_MIN);
				}
				float rv = 0.0f;
				if(!b->RamX.Empty() && b->RamX.Eval(wv, rv)) baseX = rv;
				if(!b->RamY.Empty() && b->RamY.Eval(wv, rv)) baseY = rv;
			}
			double sx = ScrollValue(b->Operator, baseX, b->RatioX, _bgFrame) + b->OffsetX;
			double sy = ScrollValue(b->Operator, baseY, b->RatioY, _bgFrame) + b->OffsetY;
			layers.push_back({ b, b->Frames[idx], std::llround(sx * _scale), std::llround(sy * _scale) });
		}
		// Ordre du peintre : Z croissant ; a Z egal, ordre du fichier
		std::stable_sort(layers.begin(), layers.end(), [](const BgLayer& l, const BgLayer& r) { return l.Bg->Z < r.Bg->Z; });
	}
	_bgFrame++;

	const int tileLowZ = _pack.SpriteZ - 10;
	const int tileHighZ = _pack.SpriteZ + 10;
	std::vector<BgLayer> backLayers, midLayers, frontLayers;
	for(const BgLayer& l : layers) {
		if(l.Bg->Z < tileLowZ) {
			backLayers.push_back(l);
		} else if(l.Bg->Z < _pack.SpriteZ) {
			midLayers.push_back(l);
		} else {
			frontLayers.push_back(l);
		}
	}

	// Peint une liste de fonds sur la couleur c au pixel HD (hx, hy). skipBelow : fonds de Z inferieur ignores (tile prioritaire).
	auto paintLayers = [&](const std::vector<BgLayer>& ls, uint32_t hx, uint32_t hy, uint32_t c, bool& painted, int skipBelow) -> uint32_t {
		for(const BgLayer& l : ls) {
			if(l.Bg->Z < skipBelow) {
				continue;
			}
			uint32_t bpx = 0;
			if(!SampleBackgroundPixel(l.Image, *l.Bg, l.OffX, l.OffY, hx, hy, bpx) || (bpx >> 24) == 0) {
				continue;
			}
			uint32_t a = (uint32_t)std::lround((double)(bpx >> 24) * l.Bg->Alpha);
			if(a == 0) {
				continue;
			}
			uint32_t src = AdjustLuminosity(bpx, l.Bg->Luminosity);
			c = (a >= 255) ? (src | 0xFF000000) : BlendPixel(src, c, a);
			painted = true;
		}
		return c;
	};

	const bool drawSprites = info && sprites;
	// Couleur sous chaque pixel de sprite (fond Game Boy + fonds derriere les sprites) : lue par la passe 2
	// pour les pixels transparents ou semi-transparents des sprites HD
	_under.resize((size_t)outWidth * screenH * _scale);

	// Tiles des sprites : recherchees une fois par image (2 au plus par sprite), avant le parallelisme
	std::vector<std::array<GbHdTileMatch, 2>> spriteTiles;
	if(drawSprites) {
		spriteTiles.resize(sprites->size());
		for(size_t k = 0; k < sprites->size(); k++) {
			const GbHdSpriteInfo& s = (*sprites)[k];
			spriteTiles[k][0] = FindTile(s.Hash[0], s.Palette, s.Flags, mem, s.X, s.Y + ((s.Height == 16 && (s.Flags & 4)) ? 8 : 0), true);
			spriteTiles[k][1] = (s.Height == 16) ? FindTile(s.Hash[1], s.Palette, s.Flags, mem, s.X, s.Y + ((s.Flags & 4) ? 0 : 8), true) : GbHdTileMatch();
		}
	}

	// ---- Diorama (doc 56) : couches separees envoyees au GPU a la place du rendu 2D (meme regles de visibilite que le rendu 2D) ----
	bool dioDone = false;
	if(info && DioramaActive()) {
		const size_t N = (size_t)outWidth * screenH * _scale;
		for(auto& L : _dioL) L.assign(N, 0u);
		const bool winFixed = _pack.Dio->WindowFixed;
		if(winFixed) _dioFix.assign(N, 0u);
		const size_t dioGroupCount = std::min<size_t>(_pack.Dio->Data.DioramaDepthGroups.size(), 8);
		for(size_t g = 0; g < 8; g++) {
			if(g < dioGroupCount) { _dioG[g].assign(N, 0u); _dioGLines[g].assign(screenH, 0); }
			else { _dioG[g].clear(); _dioGLines[g].clear(); }
		}
		const bool dioShadows = !_pack.Dio->Data.DioramaShadowRules.empty();
		if(dioShadows) { _dioS.assign(N, 0u); _dioSLines.assign(screenH, 0); _dioSRule.assign(screenH, -1); }
		else { _dioS.clear(); _dioSLines.clear(); _dioSRule.clear(); }
		const bool l0Clear = DioramaSkyWanted(); // ciel : la couleur 0 du fond laisse voir la skybox
		_dioBackdrop = ColorFromRgb555(info[0].BgZero) | 0xFF000000u;
		auto prem = [](uint32_t c) -> uint32_t {
			const uint32_t a = c >> 24;
			if(a == 255) return c;
			if(a == 0) return 0;
			return (a << 24) | ((((c >> 16) & 0xFF) * a + 127) / 255 << 16) | ((((c >> 8) & 0xFF) * a + 127) / 255 << 8) | (((c & 0xFF) * a + 127) / 255);
		};
		auto over = [](uint32_t& d, uint32_t s) { // s premultiplie sur d premultiplie
			const uint32_t sa = s >> 24;
			if(sa == 255) { d = s; return; }
			if(sa == 0) return;
			const uint32_t k = 255 - sa;
			uint32_t r = 0;
			for(uint32_t sh = 0; sh <= 24; sh += 8) r |= std::min(255u, ((s >> sh) & 0xFF) + (((d >> sh) & 0xFF) * k + 127) / 255) << sh;
			d = r;
		};
		// fonds <background> (alpha et luminosite) poses en premultiplie sur d ; skipBelow : fonds de Z inferieur ignores
		// <background> dessines par le GPU : opaques (alpha 1), luminosite <= 1 ; arriere en side et topDown, intermediaires et avant en side
		_dioBgGpu.clear();
		std::vector<BgLayer> backCpu, midCpu, frontCpu;
		auto routeBg = [&](const std::vector<BgLayer>& src, std::vector<BgLayer>& cpu, int kind) {
			for(const BgLayer& l : src) {
				const bool ok = l.Bg->Alpha >= 0.999 && l.Bg->Luminosity <= 1.0 && (kind == 0 || !_dioTopDown) && _dioBgGpu.size() < 8 &&
					l.Image < _images.size() && !_images[l.Image].empty() && _imageWidths[l.Image] > 0 && _imageHeights[l.Image] > 0;
				if(!ok) { cpu.push_back(l); continue; }
				DioBgGpu g; g.Image = l.Image; g.OffX = l.OffX; g.OffY = l.OffY; g.Bright = (float)l.Bg->Luminosity; g.TileX = l.Bg->TileX; g.TileY = l.Bg->TileY; g.Kind = kind;
				_dioBgGpu.push_back(g);
			}
		};
		routeBg(backLayers, backCpu, 0);
		routeBg(midLayers, midCpu, 1);
		routeBg(frontLayers, frontCpu, 2);
		auto paintPrem = [&](const std::vector<BgLayer>& ls, uint32_t hx, uint32_t hy, uint32_t& d, int skipBelow) {
			for(const BgLayer& l : ls) {
				if(l.Bg->Z < skipBelow) continue;
				uint32_t bpx = 0;
				if(!SampleBackgroundPixel(l.Image, *l.Bg, l.OffX, l.OffY, hx, hy, bpx) || (bpx >> 24) == 0) continue;
				const uint32_t a = (uint32_t)std::lround((double)(bpx >> 24) * l.Bg->Alpha);
				if(a == 0) continue;
				over(d, prem((AdjustLuminosity(bpx, l.Bg->Luminosity) & 0x00FFFFFFu) | (std::min(a, 255u) << 24)));
			}
		};
		const int dioBands = (_pool.Size() > 1) ? _pool.Size() : 1;
		const uint32_t dioRows = (screenH + dioBands - 1) / dioBands;
		auto dioBand = [&](int b) {
			const uint32_t y0 = (uint32_t)b * dioRows, y1 = std::min(screenH, y0 + dioRows);
			for(uint32_t y = y0; y < y1; y++) {
				for(uint32_t x = 0; x < GbConstants::ScreenWidth; x++) {
					const uint32_t offset = y * GbConstants::ScreenWidth + x;
					const GbHdPixelInfo& p = info[offset];
					const uint32_t sdColor = GetPixel(ppuOutputBuffer, offset) | 0xFF000000u;
					// tile de fond du pixel : la sienne, ou celle sous le sprite
					const bool isSpr = p.Layer == 2;
					const bool hasTile = p.Layer == 1 || (isSpr && p.ULayer);
					const uint64_t th = isSpr ? p.UHash : p.Hash;
					const uint32_t tpal = isSpr ? p.UPalette : p.Palette;
					const uint8_t tfl = isSpr ? p.UFlags : p.Flags, trow = isSpr ? p.URow : p.Row, tcol = isSpr ? p.UCol : p.Col;
					const uint8_t tcolor = isSpr ? p.UColor : p.BgColor;
					const bool win = hasTile && (isSpr ? p.UWindow : p.Window);
					GbHdTileMatch m;
					if(hasTile) m = FindTile(th, tpal, tfl, mem, (int32_t)x - (int32_t)tcol, (int32_t)y - (int32_t)trow, true);
					const uint32_t backdrop = ColorFromRgb555(p.BgZero) | 0xFF000000u;
					// couleur SD de la tile : pixel du PPU, ou couleur du fond sous le sprite
					const uint32_t tileSd = isSpr ? (ColorFromRgb555(p.Fallback) | 0xFF000000u) : sdColor;
					for(uint32_t dy = 0; dy < _scale; dy++) {
						const uint32_t hy = y * _scale + dy;
						for(uint32_t dx = 0; dx < _scale; dx++) {
							const uint32_t hx = x * _scale + dx;
							const size_t idx = (size_t)hy * outWidth + hx;
							uint32_t l0 = l0Clear ? 0u : backdrop;
							paintPrem(backCpu, hx, hy, l0, INT32_MIN);
							uint32_t t = 0;
							if(hasTile) {
								uint32_t px = 0;
								if(m.Ref && SamplePixel(*m.Ref, m.MirrorH, m.MirrorV, tcol, trow, dx, dy, px)) t = prem(px);
								else if(!m.Ref && tcolor != 0 && !_pack.DisableOriginalTiles) t = tileSd;
							}
							paintPrem(midCpu, hx, hy, t, INT32_MIN);
							const size_t grp = (hasTile && m.Ref && m.Ref->DepthGroup > 0 && (size_t)m.Ref->DepthGroup <= dioGroupCount) ? (size_t)m.Ref->DepthGroup : 0;
							if(win && winFixed) {
								uint32_t f = backdrop;
								over(f, t);
								_dioFix[idx] = f;
								_dioL[0][idx] = l0;
							} else if(grp > 0) {
								// tile d'un groupe <DioramaDepth> : dessinee dans l'image du groupe, pas dans L2 (les fonds intermediaires restent en L2)
								_dioL[0][idx] = l0;
								_dioG[grp - 1][idx] = t;
								if(t >> 24) _dioGLines[grp - 1][y] = 1;
								uint32_t t2 = 0;
								paintPrem(midCpu, hx, hy, t2, INT32_MIN);
								_dioL[2][idx] = t2;
							} else {
								_dioL[0][idx] = l0;
								_dioL[2][idx] = t;
							}
						}
					}
				}
			}
		};
		_pool.Run(dioBands, dioBand);
		// sprites : L1 (priorite fond OAM) ou L3, memes regles de visibilite que la passe 2 ; sprite sans tile HD : pixels SD du PPU
		if(sprites) {
			const int32_t screenW = (int32_t)GbConstants::ScreenWidth;
			for(size_t k = 0; k < sprites->size(); k++) {
				const GbHdSpriteInfo& s = (*sprites)[k];
				// G6c : regle d'ombre du sprite (bloc <DioramaShadow> sur sa tile, ou ligne tile=HASH) ; -1 = pas d'ombre
				int shRule = -1;
				if(dioShadows) {
					if(spriteTiles[k][0].Ref && spriteTiles[k][0].Ref->ShadowRule > 0) shRule = (int)spriteTiles[k][0].Ref->ShadowRule - 1;
					else if(spriteTiles[k][1].Ref && spriteTiles[k][1].Ref->ShadowRule > 0) shRule = (int)spriteTiles[k][1].Ref->ShadowRule - 1;
					else {
						for(const GbHdShadowKey& sk : _pack.ShadowKeys) {
							if(!sk.AnyTile && sk.Hash != s.Hash[0] && !(s.Height == 16 && sk.Hash == s.Hash[1])) continue;
							if(!sk.AnyPalette && sk.Palette != s.Palette) continue;
							if(!sk.Terms.empty() && !EvaluateTerms(sk.Terms, s.Flags, mem, s.X, s.Y, true)) continue;
							shRule = sk.Rule - 1;
							break;
						}
					}
				}
				const size_t sprGrp = (spriteTiles[k][0].Ref && spriteTiles[k][0].Ref->DepthGroup > 0 && (size_t)spriteTiles[k][0].Ref->DepthGroup <= dioGroupCount) ? (size_t)spriteTiles[k][0].Ref->DepthGroup : 0; // G7 : sprite dans un groupe (decide par sa premiere tile)
				for(int32_t r = 0; r < s.Height; r++) {
					const int32_t sy = s.Y + r;
					if(sy < 0 || sy >= (int32_t)screenH) continue;
					const int32_t vr = (s.Flags & 4) ? (s.Height - 1 - r) : r;
					const uint64_t hash = s.Hash[vr >> 3];
					const uint32_t row = (uint32_t)(r & 7);
					const GbHdTileMatch& m = spriteTiles[k][vr >> 3];
					std::vector<uint32_t>& L = sprGrp > 0 ? _dioG[sprGrp - 1] : _dioL[s.BgPriority ? 1 : 3];
					if(sprGrp > 0) _dioGLines[sprGrp - 1][sy] = 1;
					if(shRule >= 0) { _dioSLines[sy] = 1; if(_dioSRule[sy] < 0 || shRule < _dioSRule[sy]) _dioSRule[sy] = shRule; }
					for(int32_t c = 0; c < 8; c++) {
						const int32_t sx = s.X + c;
						if(sx < 0 || sx >= screenW) continue;
						const uint32_t offset = (uint32_t)sy * GbConstants::ScreenWidth + (uint32_t)sx;
						const GbHdPixelInfo& p = info[offset];
						const bool owned = p.Layer == 2 && p.Hash == hash && p.Palette == s.Palette && p.Flags == s.Flags;
						bool allowed = true;
						if(p.Layer == 2) allowed = owned;
						else if(p.Layer == 1) allowed = p.BgPriority ? p.BgColor == 0 : (!s.BgPriority || p.BgColor == 0);
						if(!allowed) continue;
						if(m.Ref) {
							for(uint32_t dy = 0; dy < _scale; dy++) {
								for(uint32_t dx = 0; dx < _scale; dx++) {
									uint32_t px = 0;
									if(SamplePixel(*m.Ref, m.MirrorH, m.MirrorV, (uint32_t)c, row, dx, dy, px) && (px >> 24)) {
										over(L[((size_t)sy * _scale + dy) * outWidth + (size_t)sx * _scale + dx], prem(px));
										if(shRule >= 0) over(_dioS[((size_t)sy * _scale + dy) * outWidth + (size_t)sx * _scale + dx], prem(px));
									}
								}
							}
						} else if(owned) {
							const uint32_t col = GetPixel(ppuOutputBuffer, offset) | 0xFF000000u;
							for(uint32_t dy = 0; dy < _scale; dy++) {
								for(uint32_t dx = 0; dx < _scale; dx++) { L[((size_t)sy * _scale + dy) * outWidth + (size_t)sx * _scale + dx] = col; if(shRule >= 0) _dioS[((size_t)sy * _scale + dy) * outWidth + (size_t)sx * _scale + dx] = col; }
							}
						}
					}
				}
			}
		}
		// fonds devant les sprites : L3 (sous spriteZ + 10, une tile GBC prioritaire reste devant)
		if(!frontCpu.empty()) {
			for(uint32_t y = 0; y < screenH; y++) {
				for(uint32_t x = 0; x < GbConstants::ScreenWidth; x++) {
					const GbHdPixelInfo& p = info[y * GbConstants::ScreenWidth + x];
					const int skipBelow = (p.Layer == 1 && p.BgPriority && p.BgColor != 0) ? tileHighZ : INT32_MIN;
					for(uint32_t dy = 0; dy < _scale; dy++) {
						for(uint32_t dx = 0; dx < _scale; dx++) {
							const uint32_t hx = x * _scale + dx, hy = y * _scale + dy;
							paintPrem(frontCpu, hx, hy, _dioL[3][(size_t)hy * outWidth + hx], skipBelow);
						}
					}
				}
			}
		}
		// lignes HD de la fenetre (HUD)
		_dioFixY0 = 0; _dioFixRows = 0;
		if(winFixed) {
			int first = -1, last = -1;
			for(uint32_t y = 0; y < screenH; y++) {
				bool any = false;
				for(uint32_t x = 0; x < GbConstants::ScreenWidth && !any; x++) {
					const GbHdPixelInfo& p = info[y * GbConstants::ScreenWidth + x];
					any = (p.Layer == 1 && p.Window) || (p.Layer == 2 && p.ULayer && p.UWindow);
				}
				if(any) { if(first < 0) first = (int)y; last = (int)y; }
			}
			if(first >= 0) { _dioFixY0 = (uint32_t)first * _scale; _dioFixRows = (uint32_t)(last - first + 1) * _scale; }
		}
		dioDone = DioramaSubmit(out);
	}

	// Bandes de lignes SD, une par thread. Chaque bande dessine son fond, ses sprites puis ses premiers plans :
	// les bandes ne se recouvrent pas, donc aucune ecriture n'est partagee entre threads.
	const int bands = (_pool.Size() > 1 && screenH >= 16) ? _pool.Size() : 1;
	const uint32_t rowsPerBand = (screenH + bands - 1) / bands;
	std::vector<BandCounts> counts(bands);

	auto renderBand = [&](int b) {
		const uint32_t y0 = (uint32_t)b * rowsPerBand;
		const uint32_t y1 = std::min(screenH, y0 + rowsPerBand);
		BandCounts& cnt = counts[b];
		if(y0 >= y1) {
			return;
		}

		// Passe 1 : fonds arriere, tiles HD (ou rendu SD), fonds intermediaires
		for(uint32_t y = y0; y < y1; y++) {
			for(uint32_t x = 0; x < GbConstants::ScreenWidth; x++) {
				uint32_t offset = y * GbConstants::ScreenWidth + x;
				uint32_t fallback = GetPixel(ppuOutputBuffer, offset);
				const GbHdPixelInfo* p = info ? &info[offset] : nullptr;
				const bool isSprite = p && p->Layer == 2;
				const bool hideSd = _pack.DisableOriginalTiles;

				GbHdTileMatch m;
				if(p && p->Layer == 1) {
					m = FindTile(p->Hash, p->Palette, p->Flags, mem, (int32_t)x - (int32_t)p->Col, (int32_t)y - (int32_t)p->Row, true);
				}
				bool painted = false;
				for(uint32_t dy = 0; dy < _scale; dy++) {
					const uint32_t hy = y * _scale + dy;
					uint32_t* dstRow = out + hy * outWidth + x * _scale;
					for(uint32_t dx = 0; dx < _scale; dx++) {
						const uint32_t hx = x * _scale + dx;
						if(isSprite) {
							// Pixel SD de sprite : garde le sprite SD (remplace par le sprite HD en passe 2) ;
							// on memorise ce qu'il y a dessous pour ses pixels transparents
							uint32_t under = paintLayers(backLayers, hx, hy, ColorFromRgb555(hideSd ? p->BgZero : p->Fallback), painted, INT32_MIN);
							under = paintLayers(midLayers, hx, hy, under, painted, INT32_MIN);
							_under[(size_t)hy * outWidth + hx] = under;
							dstRow[dx] = fallback;
							continue;
						}
						uint32_t px = 0;
						const bool hasHd = m.Ref && SamplePixel(*m.Ref, m.MirrorH, m.MirrorV, p->Col, p->Row, dx, dy, px);
						// Base : couleur Game Boy de fond sous une tile HD, sinon le rendu SD
						// <disableOriginalTiles> : tile SD sans remplacement = couleur 0 du fond (les <background> passent dessus)
						uint32_t c = hasHd ? ColorFromRgb555(p->Fallback) : ((hideSd && p && p->Layer == 1) ? ColorFromRgb555(p->BgZero) : fallback);
						c = paintLayers(backLayers, hx, hy, c, painted, INT32_MIN);
						if(hasHd) {
							const uint32_t alpha = px >> 24;
							if(alpha == 0xFF) {
								c = px;
							} else if(alpha) {
								c = BlendPixel(px, c, alpha);
							}
						}
						dstRow[dx] = paintLayers(midLayers, hx, hy, c, painted, INT32_MIN);
					}
				}
				if(painted) {
					cnt.bg++;
				}
				if(m.Ref) {
					cnt.hd++;
				} else if(!painted) {
					cnt.fallback++;
				}
			}
		}

		// Passe 2 : sprites HD par empreinte, uniquement sur les lignes de la bande (ordre du peintre : apres le fond)
		if(drawSprites) {
			const int32_t screenW = (int32_t)GbConstants::ScreenWidth;
			for(size_t k = 0; k < sprites->size(); k++) {
				const GbHdSpriteInfo& s = (*sprites)[k];
				for(int32_t r = 0; r < s.Height; r++) {
					int32_t sy = s.Y + r;
					if(sy < 0 || (uint32_t)sy < y0 || (uint32_t)sy >= y1) {
						continue;
					}
					// Ligne a l'ecran r ; tile lue selon le flip vertical, ligne d'atlas = r & 7 (affichage)
					int32_t vr = (s.Flags & 4) ? (s.Height - 1 - r) : r;
					uint64_t hash = s.Hash[vr >> 3];
					uint32_t row = (uint32_t)(r & 7);
					const GbHdTileMatch& m = spriteTiles[k][vr >> 3];
					if(!m.Ref) {
						continue;
					}
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
							// Tile de fond : bit de priorite GBC = devant le sprite (sauf couleur 0) ; sinon derriere
							// seulement si le sprite a sa priorite et que la couleur de fond n'est pas nulle
							if(p.BgPriority) {
								allowed = p.BgColor == 0;
							} else {
								allowed = !s.BgPriority || p.BgColor == 0;
							}
						}
						if(!allowed) {
							continue;
						}
						bool any = false;
						for(uint32_t dy = 0; dy < _scale; dy++) {
							for(uint32_t dx = 0; dx < _scale; dx++) {
								uint32_t px = 0;
								if(SamplePixel(*m.Ref, m.MirrorH, m.MirrorV, (uint32_t)c, row, dx, dy, px)) {
									uint32_t& dst = out[((uint32_t)sy * _scale + dy) * outWidth + (uint32_t)sx * _scale + dx];
									if((px >> 24) == 0xFF) {
										dst = px;
										any = true;
									} else if(px >> 24) {
										// Pixel semi-transparent : melange avec ce qu'il y a sous le sprite
										uint32_t under = (p.Layer == 2) ? _under[&dst - out] : dst;
										dst = BlendPixel(px, under, px >> 24);
										any = true;
									} else if(p.Layer == 2) {
										// Pixel transparent du pack : ce qu'il y a sous le sprite (fond Game Boy et fonds arriere)
										dst = _under[&dst - out];
									}
								}
							}
						}
						if(any) {
							cnt.sprite++;
						}
					}
				}
			}
		}

		// Passe 3 : fonds devant les sprites (Z >= spriteZ). Sous spriteZ + 10, une tile GBC prioritaire reste devant.
		if(!frontLayers.empty()) {
			for(uint32_t y = y0; y < y1; y++) {
				for(uint32_t x = 0; x < GbConstants::ScreenWidth; x++) {
					const GbHdPixelInfo* p = info ? &info[y * GbConstants::ScreenWidth + x] : nullptr;
					const bool prioTile = p && p->Layer == 1 && p->BgPriority && p->BgColor != 0;
					const int skipBelow = prioTile ? tileHighZ : INT32_MIN;
					bool painted = false;
					for(uint32_t dy = 0; dy < _scale; dy++) {
						const uint32_t hy = y * _scale + dy;
						uint32_t* dstRow = out + hy * outWidth + x * _scale;
						for(uint32_t dx = 0; dx < _scale; dx++) {
							dstRow[dx] = paintLayers(frontLayers, x * _scale + dx, hy, dstRow[dx], painted, skipBelow);
						}
					}
				}
			}
		}
	};

	if(!dioDone) {
		_pool.Run(bands, renderBand);
		ApplyDayNight2D(out, info); // <DayNight>, mode normal (en diorama : teinte dans la scene)
	}

	// <2DSpriteLight> : halos des sprites, sur l'image composee (avant les overlays, comme SNES)
	if(!_pack.SpriteLights.empty() && sprites) {
		RenderSpriteLights(out, sprites, mem);
	}

	// <2DOverlay> : simulation, puis particules (Z par pixel SD), calques plein ecran, distorsion
	if(!_pack.Overlays.empty()) {
		SimulateOverlays(frameData ? (int32_t)frameData->ScrollX : 0, frameData ? (int32_t)frameData->ScrollY : 0, mem);
		bool anyParticles = false;
		for(const GbHdOverlay& ov : _pack.Overlays) {
			if(ov.Active && !ov.Particles.empty()) {
				anyParticles = true;
				break;
			}
		}
		if(anyParticles) {
			// Niveau de ce qui est affiche sur chaque pixel SD : fond 0, tile 10 ou 30, sprite spriteZ,
			// puis le Z de chaque <background> qui couvre ce pixel (echantillon au centre du pixel SD)
			std::vector<uint8_t> zmap(GbConstants::PixelCount, 0);
			const int sprZ = std::clamp(_pack.SpriteZ, 0, 255);
			const int tileLow = std::clamp(tileLowZ, 0, 255);
			const int tileHigh = std::clamp(tileHighZ, 0, 255);
			const uint32_t half = _scale / 2;
			for(uint32_t i = 0; i < GbConstants::PixelCount; i++) {
				int z = 0;
				if(info) {
					const GbHdPixelInfo& q = info[i];
					if(q.Layer == 2) {
						z = sprZ;
					} else if(q.Layer == 1 && q.BgColor != 0) {
						z = q.BgPriority ? tileHigh : tileLow;
					}
				}
				const uint32_t hx = (i % GbConstants::ScreenWidth) * _scale + half;
				const uint32_t hy = (i / GbConstants::ScreenWidth) * _scale + half;
				for(const BgLayer& l : layers) {
					uint32_t bpx = 0;
					if(l.Bg->Z > z && SampleBackgroundPixel(l.Image, *l.Bg, l.OffX, l.OffY, hx, hy, bpx) && (bpx >> 24) != 0) {
						z = l.Bg->Z;
					}
				}
				zmap[i] = (uint8_t)std::clamp(z, 0, 255);
			}
			RenderOverlayParticles(out, zmap);
		}
		RenderOverlayLayers(out);
		RenderOverlayDistortions(out);
	}

	for(const BandCounts& cnt : counts) {
		_hdPixels += cnt.hd;
		_fallbackPixels += cnt.fallback;
		_bgPixels += cnt.bg;
		_hdSpritePixels += cnt.sprite;
	}

	if(++_frameCounter >= 300) {
		uint64_t total = _hdPixels + _fallbackPixels;
		uint64_t percent = total ? (_hdPixels * 100) / total : 0;
		MessageManager::Log("[HD-GB-FLT] 300 images : fond HD " + std::to_string(_hdPixels) + " (" + std::to_string(percent) + "%), objets HD " + std::to_string(_hdSpritePixels) + ", replis " + std::to_string(_fallbackPixels) + ", fond image " + std::to_string(_bgPixels) + ", threads " + std::to_string(bands));
		_hdPixels = 0;
		_hdSpritePixels = 0;
		_fallbackPixels = 0;
		_bgPixels = 0;
		_frameCounter = 0;
	}
}

// ---------------------------------------------------------------------------
// <2DOverlay> Game Boy : portage du NES (Core/NES/HdPacks/HdNesPack.cpp), unites en pixels SD (160 x 144).
// Voir docs/claude/09_2DOverlay.md.
// ---------------------------------------------------------------------------
namespace {
	constexpr float OvPi = 3.14159265358979f;

	inline float OvLerp(float a, float b, float t) { return a + (b - a) * t; }

	// Bruit de valeur lisse (0..1) : memes fonctions que NES / SNES
	inline float OvHash(int x, int y, int seed)
	{
		uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= h >> 16;
		return (float)(h & 0xFFFF) / 65535.0f;
	}
	inline float OvNoise(float x, float y, int seed)
	{
		const int xi = (int)std::floor(x), yi = (int)std::floor(y);
		const float fx = x - (float)xi, fy = y - (float)yi;
		const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
		const float a = OvHash(xi, yi, seed), b = OvHash(xi + 1, yi, seed);
		const float c = OvHash(xi, yi + 1, seed), d = OvHash(xi + 1, yi + 1, seed);
		const float top = a + (b - a) * ux, bot = c + (d - c) * ux;
		return top + (bot - top) * uy;
	}

	// Ecart de defilement modulo 256 (SCX / SCY). 0 si saut de plus de 64 px (changement de salle).
	inline int32_t OvScrollDelta(int32_t now, int32_t last)
	{
		int32_t d = ((now - last) % 256 + 256) % 256;
		if(d > 128) {
			d -= 256;
		}
		return (std::abs(d) > 64) ? 0 : d;
	}

	inline float OvWrap(float v, float period)
	{
		v = std::fmod(v, period);
		if(v < 0) {
			v += period;
		}
		return v;
	}

	inline uint32_t OvPack(float r, float g, float b)
	{
		return 0xFF000000u | ((uint32_t)std::clamp(r + 0.5f, 0.0f, 255.0f) << 16) | ((uint32_t)std::clamp(g + 0.5f, 0.0f, 255.0f) << 8) | (uint32_t)std::clamp(b + 0.5f, 0.0f, 255.0f);
	}

	void OvSpawn(GbHdOverlay& ov, float fieldW, float fieldH)
	{
		GbHdParticle p;
		p.X = OvWrap((ov.SpawnX + ov.RandRange(0.0f, ov.SpawnW)) * fieldW, fieldW);
		p.Y = OvWrap((ov.SpawnY + ov.RandRange(0.0f, ov.SpawnH)) * fieldH, fieldH);
		const float angle = ov.RandRange(ov.AngleMin, ov.AngleMax) * OvPi / 180.0f;
		const float speed = ov.RandRange(ov.SpeedMin, ov.SpeedMax);
		p.VX = std::cos(angle) * speed;
		p.VY = std::sin(angle) * speed;
		p.Life = ov.RandRange(ov.LifeMin, ov.LifeMax);
		p.Size = 1.0f + ov.RandRange(-ov.SizeRandom, ov.SizeRandom);
		p.Rot = ov.RandRange(ov.RotationMin, ov.RotationMax) * OvPi / 180.0f;
		p.Spin = ov.RandRange(ov.SpinMin, ov.SpinMax) * OvPi / 180.0f;
		p.Seed1 = ov.Rand() * 2.0f * OvPi;
		p.Seed2 = ov.Rand() * 2.0f * OvPi;
		ov.Particles.push_back(p);
	}

	void OvStep(GbHdOverlay& ov, float dt, float fieldW, float fieldH)
	{
		ov.SpawnAcc += ov.Rate * dt;
		while(ov.SpawnAcc >= 1.0f) {
			ov.SpawnAcc -= 1.0f;
			if((int)ov.Particles.size() < ov.MaxParticles) {
				OvSpawn(ov, fieldW, fieldH);
			}
		}
		const float w = 2.0f * OvPi * ov.TurbulenceFreq;
		for(size_t k = 0; k < ov.Particles.size();) {
			GbHdParticle& p = ov.Particles[k];
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
			p.X = OvWrap(p.X + p.EVX * dt, fieldW);
			p.Y = OvWrap(p.Y + p.EVY * dt, fieldH);
			p.Rot += p.Spin * dt;
			k++;
		}
	}
}

void GbHdVideoFilter::SimulateOverlays(int32_t scrollX, int32_t scrollY, const std::vector<uint8_t>* mem)
{
	const float dt = 1.0f / 60.0f;
	for(GbHdOverlay& ov : _pack.Overlays) {
		ov.Active = EvaluateTerms(ov.Terms, 0, mem, 0, 0, false);

		// L'ancrage suit le defilement meme quand l'effet est masque : au retour, il est deja au bon endroit du decor
		if(!ov.HaveScroll) {
			ov.LastScrollX = scrollX;
			ov.LastScrollY = scrollY;
			ov.HaveScroll = true;
		}
		const int32_t dx = OvScrollDelta(scrollX, ov.LastScrollX);
		const int32_t dy = OvScrollDelta(scrollY, ov.LastScrollY);
		ov.LastScrollX = scrollX;
		ov.LastScrollY = scrollY;
		if(ov.Layer > 0) {
			ov.OffsetX += dx * ov.SpeedX;
			ov.OffsetY += dy * ov.SpeedY;
		}

		if(!ov.Active) {
			continue; // effet fige et masque : il reprend ou il en etait
		}
		if(ov.IsNoise || ov.IsRays || ov.IsAurora || ov.IsDistortion) {
			ov.Time += dt;
			continue;
		}
		if(ov.IsPlainScreen || ov.IsEllipse || ov.IsTorch) {
			continue;
		}
		const float fieldW = (float)GbConstants::ScreenWidth * ov.FieldW;
		const float fieldH = (float)GbConstants::ScreenHeight * ov.FieldH;
		if(!ov.Initialized) {
			ov.Initialized = true;
			ov.Particles.clear();
			ov.SpawnAcc = 0;
			if(ov.Prewarm) {
				const float maxTime = std::min(ov.LifeMax, 20.0f);
				for(float t = 0; t < maxTime; t += dt) {
					OvStep(ov, dt, fieldW, fieldH);
				}
			}
		}
		OvStep(ov, dt, fieldW, fieldH);
	}
}

void GbHdVideoFilter::RenderOverlayParticles(uint32_t* out, std::vector<uint8_t>& zmap)
{
	const int iScale = (int)_scale;
	const float fScale = (float)_scale;
	const int outW = (int)(GbConstants::ScreenWidth * _scale);
	const int outH = (int)(GbConstants::ScreenHeight * _scale);
	const float screenW = (float)GbConstants::ScreenWidth;
	const float screenH = (float)GbConstants::ScreenHeight;

	// Ecriture d'un pixel HD (couleur premultipliee). Test de Z par pixel SD : un element de niveau superieur reste devant.
	auto put = [&](const GbHdOverlay& ov, int sx, int sy, float srcA, float sr, float sg, float sb) {
		uint8_t& z = zmap[(size_t)(sy / iScale) * GbConstants::ScreenWidth + (size_t)(sx / iScale)];
		if(ov.ZIndex < z) {
			return;
		}
		uint32_t& dst = out[(size_t)sy * outW + (size_t)sx];
		const float dr = (float)((dst >> 16) & 0xFF), dg = (float)((dst >> 8) & 0xFF), db = (float)(dst & 0xFF);
		if(ov.Additive) {
			dst = OvPack(dr + sr, dg + sg, db + sb);
		} else {
			const float keep = 1.0f - srcA;
			dst = OvPack(sr + dr * keep, sg + dg * keep, sb + db * keep);
		}
	};

	for(GbHdOverlay& ov : _pack.Overlays) {
		if(!ov.Active || ov.Particles.empty()) {
			continue;
		}
		const float fieldW = screenW * ov.FieldW;
		const float fieldH = screenH * ov.FieldH;
		const bool hasTex = ov.TextureIndex >= 0 && (size_t)ov.TextureIndex < _images.size() && !_images[ov.TextureIndex].empty();
		const std::vector<uint32_t>* tex = hasTex ? &_images[ov.TextureIndex] : nullptr;
		const float texW = hasTex ? (float)_imageWidths[ov.TextureIndex] : 0.0f;
		const float texH = hasTex ? (float)_imageHeights[ov.TextureIndex] : 0.0f;

		for(const GbHdParticle& p : ov.Particles) {
			const float t = std::clamp(p.Age / p.Life, 0.0f, 1.0f);
			float fade = 1.0f;
			if(ov.FadeIn > 0 && t < ov.FadeIn) {
				fade = t / ov.FadeIn;
			}
			if(ov.FadeOut > 0 && t > 1.0f - ov.FadeOut) {
				fade = std::min(fade, (1.0f - t) / ov.FadeOut);
			}
			const float opacity = std::clamp(OvLerp(ov.AlphaStart, ov.AlphaEnd, t) * fade * ov.Alpha, 0.0f, 1.0f);
			if(opacity <= 0.0f) {
				continue;
			}
			const float sizeMul = std::max(0.01f, OvLerp(ov.SizeStart, ov.SizeEnd, t) * p.Size);
			auto channel = [&](int shift) {
				return OvLerp((float)((ov.ColorStart >> shift) & 0xFF), (float)((ov.ColorEnd >> shift) & 0xFF), t) / 255.0f;
			};
			const float tintR = channel(16), tintG = channel(8), tintB = channel(0);

			// Champ torique : la particule est dessinee a chaque copie visible (aucune couture)
			// stretch= : trainee dans le sens du mouvement (longueur = vitesse x stretch, en pixels HD), la tete reste sur la particule
			float sUx = 0.0f, sUy = 0.0f, sL = 0.0f;
			if(ov.Stretch > 0.0f) {
				const float vl = std::sqrt(p.EVX * p.EVX + p.EVY * p.EVY);
				sL = vl * ov.Stretch * fScale;
				if(sL > 0.5f) { sUx = p.EVX / vl; sUy = p.EVY / vl; } else sL = 0.0f;
			}
			const bool strk = sL > 0.0f;
			const float radiusSd = (hasTex ? std::max(texW, texH) * sizeMul * 0.5f / fScale : ov.DotSize * sizeMul * 0.5f) + sL / fScale;
			const float margin = radiusSd + 1.0f;
			const float px0 = OvWrap(p.X - ov.OffsetX, fieldW);
			const float py0 = OvWrap(p.Y - ov.OffsetY, fieldH);

			for(int mx = (int)std::ceil((-margin - px0) / fieldW); ; mx++) {
				const float cx = px0 + mx * fieldW;
				if(cx > screenW + margin) {
					break;
				}
				for(int my = (int)std::ceil((-margin - py0) / fieldH); ; my++) {
					const float cy = py0 + my * fieldH;
					if(cy > screenH + margin) {
						break;
					}
					const float cxH = cx * fScale - sUx * sL * 0.5f;
					const float cyH = cy * fScale - sUy * sL * 0.5f;
					if(hasTex) {
						const float halfExt = std::max(texW, texH) * sizeMul * 0.5f;
						const float reach = (halfExt + sL * 0.5f) * 1.5f; // marge pour la rotation / la trainee
						const int xa = std::max(0, (int)std::floor(cxH - reach));
						const int xb = std::min(outW - 1, (int)std::ceil(cxH + reach));
						const int ya = std::max(0, (int)std::floor(cyH - reach));
						const int yb = std::min(outH - 1, (int)std::ceil(cyH + reach));
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
								const uint32_t texel = (*tex)[(size_t)(int)v * (size_t)texW + (size_t)(int)u];
								const float ta = (float)(texel >> 24) / 255.0f;
								if(ta <= 0.0f) {
									continue;
								}
								put(ov, sx, sy, ta * opacity,
									(float)((texel >> 16) & 0xFF) * tintR * ta * opacity,
									(float)((texel >> 8) & 0xFF) * tintG * ta * opacity,
									(float)(texel & 0xFF) * tintB * ta * opacity);
							}
						}
					} else {
						// Point doux : alpha lineaire du centre vers le bord
						const float rHD = std::max(0.01f, ov.DotSize * sizeMul * 0.5f * fScale);
						const int xa = std::max(0, (int)std::floor(cxH - (rHD + sL * 0.5f)));
						const int xb = std::min(outW - 1, (int)std::ceil(cxH + (rHD + sL * 0.5f)));
						const int ya = std::max(0, (int)std::floor(cyH - (rHD + sL * 0.5f)));
						const int yb = std::min(outH - 1, (int)std::ceil(cyH + (rHD + sL * 0.5f)));
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
}

void GbHdVideoFilter::RenderOverlayLayers(uint32_t* out)
{
	const int outW = (int)(GbConstants::ScreenWidth * _scale);
	const int outH = (int)(GbConstants::ScreenHeight * _scale);
	const int iS = (int)_scale;
	const float fScale = (float)_scale;
	const float visW = (float)outW, visH = (float)outH;
	// Boucle par lignes, repartie sur le pool : chaque ligne n'ecrit que ses propres pixels
	auto forRows = [&](int r0, int r1, const std::function<void(int)>& fn) {
		const int n = std::max(1, std::min(_pool.Size(), r1 - r0));
		if(n <= 1) {
			for(int y = r0; y < r1; y++) fn(y);
			return;
		}
		_pool.Run(n, [&](int t) {
			for(int y = r0 + t; y < r1; y += n) fn(y);
		});
	};

	for(GbHdOverlay& ov : _pack.Overlays) {
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
		auto blend = [&](uint32_t& dst, float factor) {
			const float k = a * factor;
			const float keep = 1.0f - k;
			dst = OvPack((float)((dst >> 16) & 0xFF) * keep + cr * k, (float)((dst >> 8) & 0xFF) * keep + cg * k, (float)(dst & 0xFF) * keep + cb * k);
		};

		if(ov.IsPlainScreen) {
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = out + (size_t)y * outW;
				for(int x = zx0; x < zx1; x++) {
					blend(row[x], 1.0f);
				}
			});
		} else if(ov.IsEllipse) {
			const float cx = ov.FxEllipseX * fScale, cy = ov.FxEllipseY * fScale;
			const float rx = std::max(0.5f, ov.FxRadiusX * fScale), ry = std::max(0.5f, ov.FxRadiusY * fScale);
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = out + (size_t)y * outW;
				const float dy = ((float)y + 0.5f - cy) / ry;
				for(int x = zx0; x < zx1; x++) {
					const float dx = ((float)x + 0.5f - cx) / rx;
					const bool inside = dx * dx + dy * dy < 1.0f;
					if(inside != ov.FxNegative) {
						blend(row[x], 1.0f);
					}
				}
			});
		} else if(ov.IsTorch) {
			// Halo doux ; le centre suit le defilement (periode 256 x 256), copie a chaque periode
			const float rad = std::max(1.0f, ov.TorchRadius * fScale);
			const float soft = std::clamp(ov.TorchSoft, 0.01f, 1.0f);
			const float bx = OvWrap(ov.TorchX - ov.OffsetX, 256.0f) * fScale, by = OvWrap(ov.TorchY - ov.OffsetY, 256.0f) * fScale;
			const float tcx[2] = { bx, bx - 256.0f * fScale }, tcy[2] = { by, by - 256.0f * fScale };
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = out + (size_t)y * outW;
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
					if(ov.TorchDark) {
						const float keep = 1.0f - a * (1.0f - L);
						const float g = ov.TorchGlow * L * a;
						row[x] = OvPack(dr * keep + cr * g, dg * keep + cg * g, db * keep + cb * g);
					} else {
						row[x] = OvPack(dr + cr * L * a, dg + cg * L * a, db + cb * L * a);
					}
				}
			});
		} else if(ov.IsRays) {
			// Faisceaux additifs ; intensite calculee sur une grille de noeuds (pas de 4 px HD), interpolee
			const int kStep = 4;
			const float radToDeg = 180.0f / OvPi;
			const float sx0 = ov.RayX * visW, sy0 = ov.RayY * visH;
			const float len = ov.RayLength * std::sqrt(visW * visW + visH * visH);
			const int N = ov.RayCount;
			std::vector<float> bAng(N), bAmp(N);
			for(int k = 0; k < N; k++) {
				const float f = (N > 1) ? (float)k / (float)(N - 1) : 0.5f;
				const float base = (ov.RaySpread >= 360.0f) ? ov.RayAngle + 360.0f * (float)k / (float)N : ov.RayAngle + (f - 0.5f) * ov.RaySpread;
				bAng[k] = base + ov.RayWobble * std::sin(2.0f * OvPi * (ov.RaySpeed * 0.5f * ov.Time) + (float)k * 1.7f);
				bAmp[k] = 0.6f + 0.4f * std::sin(2.0f * OvPi * (ov.RaySpeed * ov.Time + (float)k * 0.37f));
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
						const float fadeR = 1.0f - dist / len;
						I *= fadeR * fadeR * ov.Alpha;
					}
					node[(size_t)j * gx + ii] = I;
				}
			});
			const float cR = (float)((ov.RayColor >> 16) & 0xFF), cG = (float)((ov.RayColor >> 8) & 0xFF), cB = (float)(ov.RayColor & 0xFF);
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = out + (size_t)y * outW;
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
					row[x] = OvPack((float)((c >> 16) & 0xFF) + cR * I, (float)((c >> 8) & 0xFF) + cG * I, (float)(c & 0xFF) + cB * I);
				}
			});
		} else if(ov.IsAurora) {
			// Rideaux additifs (bruit etire en hauteur, derive horizontale), plus lumineux vers le bas de la zone
			const int kStep = 4;
			const float zh = (float)(zy1 - zy0);
			const float size = std::max(4.0f, ov.AuroraSize);
			const float drift = ov.AuroraSpeed * ov.Time;
			const float thr = 1.0f - std::clamp(ov.AuroraCoverage, 0.0f, 1.0f);
			const float fall = std::max(0.05f, ov.AuroraFall);
			const float waveT = (zh > 0.0f) ? ov.AuroraWave / (zh / fScale) : 0.0f;
			const int seed = ov.FxSeed;
			const int gx = (zx1 - zx0) / kStep + 2, gy = (zy1 - zy0) / kStep + 2;
			std::vector<float> nI((size_t)gx * gy, 0.0f), nM((size_t)gx * gy, 0.0f);
			forRows(0, gy, [&](int j) {
				const float y = (float)(zy0 + j * kStep) + 0.5f;
				const float py = y / fScale;
				const float t = (y - (float)zy0) / std::max(1.0f, zh);
				for(int ii = 0; ii < gx; ii++) {
					const float x = (float)(zx0 + ii * kStep) + 0.5f;
					const float u = (x / fScale - drift) / size;
					const float v = py / (size * 3.0f) + ov.Time * 0.05f;
					const float n = 0.65f * OvNoise(u, v, seed) + 0.35f * OvNoise(u * 2.03f + 17.0f, v * 2.03f + 9.0f, seed + 1);
					const float edge = OvNoise(u * 0.5f + 3.0f, ov.Time * 0.3f, seed + 2) - 0.5f;
					const float tn = t + waveT * edge * 2.0f;
					float prof = std::clamp((tn - (1.0f - fall)) / fall, 0.0f, 1.0f);
					prof = prof * prof * (3.0f - 2.0f * prof);
					float m = std::clamp((n - thr) / 0.12f, 0.0f, 1.0f);
					m = m * m * (3.0f - 2.0f * m);
					nI[(size_t)j * gx + ii] = m * prof * ov.Alpha;
					nM[(size_t)j * gx + ii] = OvNoise(u * 0.5f + 5.0f, v * 0.5f + 1.0f, seed + 3);
				}
			});
			const float c1R = (float)((ov.AuroraColor1 >> 16) & 0xFF), c1G = (float)((ov.AuroraColor1 >> 8) & 0xFF), c1B = (float)(ov.AuroraColor1 & 0xFF);
			const float c2R = (float)((ov.AuroraColor2 >> 16) & 0xFF), c2G = (float)((ov.AuroraColor2 >> 8) & 0xFF), c2B = (float)(ov.AuroraColor2 & 0xFF);
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = out + (size_t)y * outW;
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
					row[x] = OvPack((float)((c >> 16) & 0xFF) + cR * I, (float)((c >> 8) & 0xFF) + cG * I, (float)(c & 0xFF) + cB * I);
				}
			});
		} else {
			// Ombres / nuages / brouillard : bruit a deux octaves qui derive, calcule une fois par pixel SD
			const float size = std::max(4.0f, ov.FxSize);
			const float dirRad = ov.FxDirection * OvPi / 180.0f;
			// derive (comme NES) moins l'ancrage : avec Layer=1, le motif reste pose sur le decor
			const float ox = std::cos(dirRad) * ov.FxSpeed * ov.Time - ov.OffsetX;
			const float oy = std::sin(dirRad) * ov.FxSpeed * ov.Time - ov.OffsetY;
			const float thr = 1.0f - std::clamp(ov.FxCoverage, 0.0f, 1.0f);
			const int seed = ov.FxSeed, kind = ov.NoiseKind;
			auto blockM = [&](int bx, int by) -> float {
				const float u = ((float)bx + 0.5f - ox) / size, v = ((float)by + 0.5f - oy) / size;
				const float n = 0.65f * OvNoise(u, v, seed) + 0.35f * OvNoise(u * 2.03f + 17.0f, v * 2.03f + 9.0f, seed + 1);
				float m;
				if(kind == 2) m = std::clamp((n - (thr - 0.1f)) / 0.4f, 0.0f, 1.0f) * 0.85f; // brouillard : bords doux
				else if(kind == 1) m = std::clamp((n - thr) / 0.08f, 0.0f, 1.0f);            // nuages : bords nets
				else m = std::clamp((n - thr) / 0.12f, 0.0f, 1.0f);                           // ombres
				return m * m * (3.0f - 2.0f * m);
			};
			const int bx0 = zx0 / iS, bx1 = (zx1 - 1) / iS;
			const int by0 = zy0 / iS, by1 = (zy1 - 1) / iS;
			const int gwz = bx1 - bx0 + 1, ghz = by1 - by0 + 1;
			std::vector<float> nm((size_t)gwz * ghz, 0.0f);
			forRows(0, ghz, [&](int j) {
				for(int i = 0; i < gwz; i++) {
					nm[(size_t)j * gwz + i] = blockM(bx0 + i, by0 + j);
				}
			});
			forRows(zy0, zy1, [&](int y) {
				uint32_t* row = out + (size_t)y * outW;
				const size_t rowOff = (size_t)(y / iS - by0) * gwz;
				for(int x = zx0; x < zx1; x++) {
					const float m = nm[rowOff + (size_t)(x / iS - bx0)];
					if(m > 0.0f) {
						blend(row[x], m);
					}
				}
			});
		}
	}
}

void GbHdVideoFilter::RenderOverlayDistortions(uint32_t* out)
{
	const int outW = (int)(GbConstants::ScreenWidth * _scale);
	const int outH = (int)(GbConstants::ScreenHeight * _scale);
	const float S = (float)_scale;
	const float twoPi = 2.0f * OvPi;
	const float degToRad = OvPi / 180.0f;
	auto lerpArgb = [](uint32_t c0, uint32_t c1, float f) -> uint32_t {
		uint32_t res = 0xFF000000;
		for(int sh = 0; sh <= 16; sh += 8) {
			const float v0 = (float)((c0 >> sh) & 0xFF), v1 = (float)((c1 >> sh) & 0xFF);
			res |= ((uint32_t)std::min(255.0f, v0 + (v1 - v0) * f + 0.5f)) << sh;
		}
		return res;
	};

	for(const GbHdOverlay& ov : _pack.Overlays) {
		if(!ov.IsDistortion || !ov.Active) {
			continue;
		}
		const int zy0 = (int)std::clamp(ov.ZoneY0 * (float)outH, 0.0f, (float)outH);
		const int zy1 = (int)std::clamp(ov.ZoneY1 * (float)outH, 0.0f, (float)outH);
		const int zx0 = (int)std::clamp(ov.ZoneX0 * (float)outW, 0.0f, (float)outW);
		const int zx1 = (int)std::clamp(ov.ZoneX1 * (float)outW, 0.0f, (float)outW);
		if(zx0 >= zx1 || zy0 >= zy1) {
			continue;
		}
		// Parallele par lignes : chaque thread a son propre tampon de ligne
		const int nT = std::max(1, std::min(_pool.Size(), zy1 - zy0));
		_pool.Run(nT, [&](int t) {
			std::vector<uint32_t> rowCopy(outW);
			for(int r = zy0 + t; r < zy1; r += nT) {
				const float yS = (float)r / S + ov.OffsetY; // position en lignes SD (suit le decor si Layer=1)
				float dx = ov.DistAmp1 * std::sin(twoPi * (yS / ov.DistWave1 + ov.DistSpeed1 * ov.Time) + ov.DistPhase1 * degToRad)
				         + ov.DistAmp2 * std::sin(twoPi * (yS / ov.DistWave2 + ov.DistSpeed2 * ov.Time) + ov.DistPhase2 * degToRad);
				dx *= S * ov.Alpha; // Alpha = force globale
				uint32_t* row = out + (size_t)r * outW;
				std::copy(row + zx0, row + zx1, rowCopy.begin() + zx0);
				for(int x = zx0; x < zx1; x++) {
					const float sx = std::clamp((float)x - dx, (float)zx0, (float)(zx1 - 1));
					const int ix = (int)sx;
					const int ix1 = std::min(ix + 1, zx1 - 1);
					row[x] = lerpArgb(rowCopy[ix], rowCopy[ix1], sx - (float)ix);
				}
			}
		});
	}
}

// <2DSpriteLight> : halo 2D au centre de chaque tile de sprite reconnue (portage SNES / NES). Additif, ou assombrissant (mode=shadow).
// Les sprites viennent de la liste OAM de l'image (sprites HD ou non). Conditions evaluees avec le sens reel de la tile (hmirror / vmirror)
// et son origine (tileNearby / spriteNearby).
void GbHdVideoFilter::RenderSpriteLights(uint32_t* out, const std::vector<GbHdSpriteInfo>* sprites, const std::vector<uint8_t>* mem)
{
	struct Light2D { float cx, cy, rad, cr, cg, cb, inten; bool shadow; };
	std::vector<Light2D> lights;
	const float S = (float)_scale;
	const float tSec = (float)_frameNumber / 60.0f;
	const int outW = (int)(GbConstants::ScreenWidth * _scale);
	const int outH = (int)(GbConstants::ScreenHeight * _scale);

	for(const GbHdSpriteInfo& s : *sprites) {
		const bool flipH = (s.Flags & 2) != 0;
		const bool flipV = (s.Flags & 4) != 0;
		const int blocks = (s.Height == 16) ? 2 : 1;
		for(int b = 0; b < blocks; b++) {
			const uint64_t hash = s.Hash[b];
			// Origine a l'ecran de la tile b : en 8x16 retourne verticalement, la tile 0 est en bas
			const int32_t ox = s.X;
			const int32_t oy = s.Y + ((blocks == 2) ? ((flipV ? 1 - b : b) * 8) : 0);
			if(ox <= -8 || ox >= (int32_t)GbConstants::ScreenWidth || oy <= -8 || oy >= (int32_t)GbConstants::ScreenHeight) {
				continue;
			}
			for(const GbHdSpriteLight& rule : _pack.SpriteLights) {
				if(rule.Hash != hash || (!rule.AnyPalette && rule.Palette != s.Palette)) {
					continue;
				}
				if(!rule.Terms.empty() && !EvaluateTerms(rule.Terms, s.Flags, mem, ox, oy, true)) {
					continue;
				}
				// Centre de la tile + dx / dy (inverses quand la tile est retournee), en pixels HD
				const float cx = ((float)ox + 4.0f + (flipH ? -rule.OffsetX : rule.OffsetX)) * S;
				const float cy = ((float)oy + 4.0f + (flipV ? -rule.OffsetY : rule.OffsetY)) * S;
				const float fk = rule.Flicker > 0.0f ? 1.0f - rule.Flicker * 0.5f * (1.0f + std::sin(rule.FlickerSpeed * tSec)) : 1.0f;
				lights.push_back({ cx, cy, std::max(1.0f, rule.Radius) * S,
					(float)((rule.Color >> 16) & 0xFF), (float)((rule.Color >> 8) & 0xFF), (float)(rule.Color & 0xFF),
					rule.Intensity * fk, rule.Shadow });
			}
		}
	}
	if(lights.empty()) {
		return;
	}

	// Par lignes sur le pool : chaque ligne applique toutes les lumieres qui la touchent (pas d'ecriture partagee)
	const int nT = std::max(1, std::min(_pool.Size(), outH));
	_pool.Run(nT, [&](int t) {
		for(int y = t; y < outH; y += nT) {
			uint32_t* row = out + (size_t)y * outW;
			const float py = (float)y + 0.5f;
			for(const Light2D& L : lights) {
				if(py < L.cy - L.rad || py > L.cy + L.rad) {
					continue;
				}
				const int x0 = std::max(0, (int)std::floor(L.cx - L.rad));
				const int x1 = std::min(outW - 1, (int)std::ceil(L.cx + L.rad));
				const float ddy = py - L.cy;
				for(int x = x0; x <= x1; x++) {
					const float ddx = (float)x + 0.5f - L.cx;
					const float f = std::clamp(1.0f - std::sqrt(ddx * ddx + ddy * ddy) / L.rad, 0.0f, 1.0f);
					const float k = f * f * L.inten;
					if(k <= 0.0f) continue;
					const uint32_t d = row[x];
					float r = (float)((d >> 16) & 0xFF), g = (float)((d >> 8) & 0xFF), bl = (float)(d & 0xFF);
					if(L.shadow) {
						const float keep = std::clamp(1.0f - k, 0.0f, 1.0f);
						r *= keep; g *= keep; bl *= keep;
					} else {
						r += L.cr * k; g += L.cg * k; bl += L.cb * k;
					}
					row[x] = OvPack(r, g, bl);
				}
			}
		}
	});
}
