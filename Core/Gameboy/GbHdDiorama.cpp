// =====================================================================================
// Diorama Game Boy (doc 56) : scene 3D du pack HD Game Boy, avec le moteur GPU du diorama NES (Shared/HdGpuCore).
// Les regles (balises <Diorama...>, <DayNight>, <3DParticles>) sont lues par le chargeur NES (HdPackLoader::ParseForeignDioramaLine)
// dans GbHdDiorama::Data ; leurs conditions sont des termes Game Boy (GbHdDioCondition).
// Couches construites dans GbHdVideoFilter::ApplyFilter ; ici : camera, vues, lumieres, ciel, brouillard, nuages, brumes,
// particules 3D, objets 3D, <DayNight>, envoi au GPU. Les blocs repris du NES (HdNesPack::DioramaGpuStep) gardent leur code ;
// unites : pixels Game Boy (160 x 144) a la place des pixels NES.
// =====================================================================================
#include "pch.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include "Gameboy/GbHdVideoFilter.h"
#include "Gameboy/GbConstants.h"
#include "Gameboy/GbPpu.h"
#include "Gameboy/HdPacks/GbHdDiorama.h"
#include "NES/HdPacks/HdData.h"
#include "Shared/HdGpuCore.h"
#include "Shared/KeyManager.h"
#include "Shared/MessageManager.h"

GbHdVideoFilter::~GbHdVideoFilter()
{
	if(_pack.Dio && _pack.Dio->EvalOwner == this) {
		_pack.Dio->Eval = nullptr;
		_pack.Dio->EvalOwner = nullptr;
	}
}

bool GbHdVideoFilter::DioramaActive() const
{
	return _pack.Dio && _pack.Dio->Data.DioramaEnabled && !(_dioGpuTried && !_dioGpuOk) && _dioFail < 30;
}

// Une regle <DioramaSkybox> vraie : L0 commence transparente (le ciel se voit dans la couleur 0 du fond)
bool GbHdVideoFilter::DioramaSkyWanted()
{
	if(!_pack.Dio) return false;
	for(const HdDioramaSkyboxRule& r : _pack.Dio->Data.DioramaSkyboxRules) {
		bool ok = true;
		for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
		if(ok) return true;
	}
	return false;
}

// <DayNight> (doc 55) : avance des cycles une fois par image ; la derniere regle vraie donne la teinte
void GbHdVideoFilter::UpdateDayNight()
{
	_dn.Active = false;
	if(!_pack.Dio) return;
	const auto& rules = _pack.Dio->Data.DayNightRules;
	if(rules.empty()) return;
	if(_dnState.size() != rules.size()) _dnState.assign(rules.size(), HdDayNightState());
	for(size_t i = 0; i < rules.size(); i++) {
		const auto& r = rules[i];
		bool ok = true;
		for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
		HdDayNightAdvance(r.Def, _dnState[i], _frameNumber, r.Def.TimeAlways || ok, [&](int s) {
			if(s < 0 || (size_t)s >= r.Until.size()) return true;
			for(HdPackCondition* c : r.Until[s]) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
			return true;
		});
		if(ok) HdDayNightOutput(r.Def, _dnState[i], (int)i, _dn);
	}
}

// <DayNight>, mode normal : multiplication de l'image ; zindex= (echelle Game Boy) : seulement ce qui est a ce niveau ou en dessous
// (fond 0, tiles spriteZ - 10 ou + 10 si prioritaires, sprites spriteZ)
void GbHdVideoFilter::ApplyDayNight2D(uint32_t* out, const GbHdPixelInfo* info)
{
	if(!_dn.Active || _dnDioApplied) return;
	if(_dn.Col[0] >= 0.999f && _dn.Col[1] >= 0.999f && _dn.Col[2] >= 0.999f) return;
	const int zMax = _pack.Dio->Data.DayNightRules[_dn.Def].Def.ZIndex;
	const uint32_t kr = (uint32_t)(_dn.Col[0] * 256.0f + 0.5f), kg = (uint32_t)(_dn.Col[1] * 256.0f + 0.5f), kb = (uint32_t)(_dn.Col[2] * 256.0f + 0.5f);
	const uint32_t outW = GbConstants::ScreenWidth * _scale, outH = GbConstants::ScreenHeight * _scale;
	for(uint32_t hy = 0; hy < outH; hy++) {
		uint32_t* row = out + (size_t)hy * outW;
		for(uint32_t hx = 0; hx < outW; hx++) {
			if(zMax >= 0 && info) {
				const GbHdPixelInfo& q = info[(hy / _scale) * GbConstants::ScreenWidth + hx / _scale];
				int z = 0;
				if(q.Layer == 2) z = _pack.SpriteZ;
				else if(q.Layer == 1 && q.BgColor != 0) z = q.BgPriority ? _pack.SpriteZ + 10 : _pack.SpriteZ - 10;
				if(z > zMax) continue;
			}
			const uint32_t d = row[hx];
			row[hx] = (d & 0xFF000000u) | ((((d >> 16) & 0xFF) * kr >> 8) << 16) | ((((d >> 8) & 0xFF) * kg >> 8) << 8) | ((d & 0xFF) * kb >> 8);
		}
	}
}

// Camera : cameras multiples, touches, <DioramaView> et transitions (copie de HdNesPack::DioramaCameraStep)
void GbHdVideoFilter::DioramaCamera(const HdDioramaConfig& cfg, float& yaw, float& pitch, float& dist, float& zoom)
{
	HdPackData& D = _pack.Dio->Data;
	const vector<string>& names = D.DioramaCameraNames;
	const int n = (int)names.size();
	auto condsOk = [](const vector<HdPackCondition*>& cs) {
		for(HdPackCondition* c : cs) { if(!c->CheckCondition(0, 0, nullptr)) return false; }
		return true;
	};
	// camera par defaut : derniere regle <DioramaCameraDefault> vraie, sinon default=on, sinon la premiere declaree
	int def = 0;
	bool found = false;
	for(const HdDioramaCameraDefaultRule& r : D.DioramaCameraDefaults) { if(condsOk(r.Conditions)) { def = r.Index; found = true; } }
	if(!found) { for(const HdDioramaCameraLine& L : D.DioramaCameraLines) { if(L.IsDefault) { def = L.Index; break; } } }
	if(def != _camDefault) { _camDefault = def; _camManual = -1; } // la camera par defaut change : la vue la suit
	// touches : une par camera + suivante / precedente / retour a la camera par defaut (front montant)
	const HdDioramaCameraKeys& ks = D.DioramaCameraKeys;
	vector<string> keyNames((size_t)n + 3);
	for(const HdDioramaCameraLine& L : D.DioramaCameraLines) { if(!L.Key.empty() && keyNames[(size_t)L.Index].empty()) keyNames[(size_t)L.Index] = L.Key; }
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
	for(const HdDioramaCameraLine& L : D.DioramaCameraLines) {
		if(L.Index != active || !condsOk(L.Conditions)) continue;
		for(int i = 0; i < 4; i++) { if(L.Has[i]) v[i] = L.V[i]; }
		if(L.HasTime) time = L.Time;
	}
	// <DioramaView> (doc 53) : reglages de vue selon [cond] ; la derniere regle vraie l'emporte (pitch, zoom, time)
	_dioTopDown = false;
	_dioFloorMargin = -1.0f;
	for(const HdDioramaViewRule& r : D.DioramaViews) {
		if(!condsOk(r.Conditions)) continue;
		_dioTopDown = true; // doc 53 : vue topDown active (sol a plat)
		if(r.FloorMargin >= 0.0f) _dioFloorMargin = r.FloorMargin;
		for(int i = 0; i < 4; i++) { if(r.Has[i]) v[i] = r.V[i]; }
		if(r.HasTime) time = r.Time;
	}
	// la distance ne doit pas amener un plan derriere la camera (meme borne que le chargeur : z >= -0,9 x dist)
	float minZ = 0.0f;
	for(int k = 0; k < 4; k++) minZ = std::min(minZ, cfg.Z[k]);
	for(const HdDioramaDepthGroup& g : D.DioramaDepthGroups) minZ = std::min(minZ, g.Z);
	for(const HdDioramaObjectRule& o : D.DioramaObjectRules) minZ = std::min(minZ, o.Z);
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
	yaw = _camCur[0]; pitch = _camCur[1]; dist = _camCur[2]; zoom = _camCur[3];}

// Scene et envoi au GPU ; l'image relue remplace la sortie. Faux : GPU absent ou en echec (le rendu 2D est alors utilise).
bool GbHdVideoFilter::DioramaSubmit(uint32_t* out)
{
	HdGpuCore& gpu = HdGpuCore::Get();
	if(!_dioGpuTried) {
		_dioGpuTried = true;
		_dioGpuOk = gpu.Available();
		MessageManager::Log(_dioGpuOk ? ("[HD-GB-DIORAMA] GPU actif (" + gpu.Info() + ")") : ("[HD-GB-DIORAMA] GPU indisponible (" + gpu.Info() + "), rendu 2D conserve"));
	}
	if(!_dioGpuOk) return false;
	HdPackData& D = _pack.Dio->Data;
	const HdDioramaConfig& cfg = D.Diorama;
	const uint32_t screenWidth = GbConstants::ScreenWidth * _scale, outHeight = GbConstants::ScreenHeight * _scale;
	const size_t pixels = (size_t)screenWidth * outHeight;
	if(_dioL[0].size() < pixels) return false;
	float yaw = cfg.Yaw, pitch = cfg.Pitch, dist = cfg.Dist, baseZoom = cfg.Zoom;
	DioramaCamera(cfg, yaw, pitch, dist, baseZoom);
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
		for(const HdDioramaPadRule& r : D.DioramaPadRules) {
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

	HdDioramaScene scene;
	scene.W = (float)GbConstants::ScreenWidth;
	scene.H = (float)GbConstants::ScreenHeight;
	scene.D = dist;
	scene.Yaw = yaw; scene.Pitch = pitch;
	for(int k = 0; k < 4; k++) { scene.PlaneZ[k] = cfg.Z[k]; scene.Thick[k] = cfg.T[k] * 8.0f; }
	if(_dioTopDown) scene.PlaneZ[0] = 0.0f; // sol topDown : L0 au niveau du sol
	scene.Scale = (float)_scale;
	scene.Aa = cfg.Aa;
	scene.Zoom = zoom;
	scene.FlatMask = _dioTopDown ? ((1u << 0) | (1u << 2)) : 0u; // topDown : sol (L0) et tiles (L2) a plat, sprites debout
	scene.Clear = _dioBackdrop | 0xFF000000u;
	// topDown : pied des sprites (silhouettes de pixels de sprite qui se touchent), meme calcul que le NES ; 256 octets par ligne
	if(_dioTopDown) {
		const int rows = (int)GbConstants::ScreenHeight, cols = (int)GbConstants::ScreenWidth;
		_dioFoot.assign((size_t)256 * rows, 0);
		const uint32_t half = _scale / 2;
		auto covered = [&](int r, int x) {
			const size_t hi = ((size_t)r * _scale + half) * screenWidth + (size_t)x * _scale + half;
			return (_dioL[1][hi] >> 24) != 0 || (_dioL[3][hi] >> 24) != 0;
		};
		std::vector<int> stack, comp;
		for(int r = 0; r < rows; r++) {
			for(int x = 0; x < cols; x++) {
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
						if(nr[k] < 0 || nr[k] >= rows || nx[k] < 0 || nx[k] >= cols) continue;
						const int j = (nr[k] << 8) | nx[k];
						if(_dioFoot[j] || !covered(nr[k], nx[k])) continue;
						_dioFoot[j] = 255; stack.push_back(j);
					}
				}
				const uint8_t v = (uint8_t)std::min(255, maxR + 1);
				for(int c : comp) _dioFoot[c] = v;
				scene.SpriteBlobTotal++;
				if(scene.SpriteBlobN < 16) {
					float* b = scene.SpriteBlob[scene.SpriteBlobN++];
					b[0] = (float)minX; b[1] = (float)(maxX + 1); b[2] = (float)r; b[3] = (float)(maxR + 1);
				}
			}
		}
		scene.SpriteFoot = _dioFoot.data(); scene.SpriteFootH = (uint32_t)rows;
	}
	if(_dioFixRows > 0 && _dioFix.size() >= pixels) { scene.Fixed = _dioFix.data(); scene.FixedY0 = _dioFixY0; scene.FixedRows = _dioFixRows; }
	// <background> dessines par le GPU (doc 52) : arriere devant L0, intermediaires devant L2, avant devant L3 (L0 uniforme : couleur d'effacement)
	scene.BgCount = 0;
	scene.BgYStart = 0.0f;
	if(_dioTopDown && _dioFloorMargin >= 0.0f) scene.BgMargin = _dioFloorMargin;
	for(const DioBgGpu& g : _dioBgGpu) {
		if(scene.BgCount >= 8) break;
		HdDioramaBgPlane& bp = scene.Bg[scene.BgCount++];
		bp.Pix = _images[g.Image].data(); bp.W = _imageWidths[g.Image]; bp.H = _imageHeights[g.Image];
		bp.OffX = (float)g.OffX; bp.OffY = (float)g.OffY; bp.Bright = g.Bright; bp.TileX = g.TileX; bp.TileY = g.TileY;
		bp.Flat = _dioTopDown && g.Kind == 0;
		bp.HasZ = g.Kind != 0;
		if(g.Kind == 1) bp.Z = scene.PlaneZ[2] - 0.5f;
		else if(g.Kind == 2) bp.Z = scene.PlaneZ[3] - 0.5f;
	}
	{ // L0 d'une seule couleur (couleur 0 du fond, fonds au GPU) : pas d'envoi, couleur d'effacement
		const uint32_t c0 = _dioL[0][0];
		bool uni = (c0 >> 24) == 0xFF;
		for(size_t p = 1; uni && p < pixels; p += 7) uni = _dioL[0][p] == c0;
		if(uni) uni = std::equal(_dioL[0].begin() + 1, _dioL[0].begin() + pixels, _dioL[0].begin());
		scene.L0Uniform = uni && scene.Thick[0] <= 0.0f;
		if(uni) scene.Clear = c0 | 0xFF000000u;
	}
	if(!D.DioramaSkyboxRules.empty() && true) {
		const HdDioramaSkyboxRule* chosen = nullptr;
		for(const HdDioramaSkyboxRule& r : D.DioramaSkyboxRules) {
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
				MessageManager::Log(std::string("[HD-DIORAMA] G5 skybox : ") + (chosen->Mode == 1 ? "degrade" : chosen->Mode == 2 ? "image" : chosen->Mode == 3 ? "panorama" : "cubemap") + ", " + std::to_string(D.DioramaSkyboxRules.size()) + " regle(s) definie(s)");
			}
		}
	} else if(!D.DioramaSkyboxRules.empty()) {
		static bool skyWarned = false;
		if(!skyWarned) { skyWarned = true; MessageManager::Log("[HD-DIORAMA] G5 skybox : L0 n'est pas unie, skybox non affichee (elle remplace la couleur unie de L0)."); }
	}
	// G6a / G6b : lumieres de scene (doc 42) : regles dont les conditions sont vraies ; 8 lumieres au plus, derniere ambiante vraie.
	// Une regle avec tile= donne une lumiere par tile de sprite reconnue a l'ecran.
	if(!D.DioramaLightRules.empty()) {
		auto rgb = [](uint32_t c, float k, float* o) { o[0] = ((c >> 16) & 0xFF) / 255.0f * k; o[1] = ((c >> 8) & 0xFF) / 255.0f * k; o[2] = (c & 0xFF) / 255.0f * k; };
		int n = 0;
		bool any = false;
		const float tSec = (float)_frameNumber / 60.0f;
		auto addLight = [&](const HdDioramaLightRule& r, float px, float py, bool onFloor) {
			if(n >= 8) return;
			HdDioramaLight& l = scene.Lights.L[n++];
			const float fk = r.Flicker > 0.0f ? 1.0f - r.Flicker * 0.5f * (1.0f + std::sin(r.FlickerSpeed * tSec)) : 1.0f;
			l.Type = r.Type;
			rgb(r.Color, r.Intensity * fk, l.Col);
			if(r.Type == 2) {
				if(_dioTopDown && onFloor) { // topDown, lumiere de tile : posee sur le sol (Y = -|y| au-dessus, profondeur = verticale de l'image)
					l.Pos[0] = px; l.Pos[1] = -std::fabs(r.Y); l.Pos[2] = scene.H * 0.5f - py;
				} else { l.Pos[0] = px; l.Pos[1] = py; l.Pos[2] = r.Z; } l.Radius = r.Radius; l.Glow = r.Glow * fk; l.GlowRadius = r.GlowRadius;
			} else {
				// topDown : soleil venant du dessus par defaut (0.3, 1.0, 0.3), sauf dx/dy/dz ecrits dans le pack
				const bool topSun = _dioTopDown && !r.DirGiven;
				const float sdx = topSun ? 0.3f : r.Dx, sdy = topSun ? 1.0f : r.Dy, sdz = topSun ? 0.3f : r.Dz;
				const float len = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
				l.Pos[0] = -sdx / len; l.Pos[1] = -sdy / len; l.Pos[2] = -sdz / len; // vers la lumiere
			}
		};
		bool anyTile = false; // lumieres de tile (hash NES) : non prises en charge sur Game Boy
		for(const HdDioramaLightRule& r : D.DioramaLightRules) {
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
		(void)anyTile;
		scene.Lights.Active = any;
		static bool lightLogged = false;
		if(any && !lightLogged) {
			lightLogged = true;
			MessageManager::Log("[HD-DIORAMA] G6a lumieres de scene : " + std::to_string(n) + " lumiere(s) ponctuelle(s) ou solaire(s) active(s) (tiles de sprite comprises), " + std::to_string(D.DioramaLightRules.size()) + " regle(s) definie(s)");
		}
	}
	// H1 : objets 3D de decor (doc 45) : regles <DioramaObject> dont la condition est vraie (8 au plus)
	scene.Objects.Count = 0;
	// <DioramaFog> (doc 54) : derniere regle vraie ; valeurs absentes = defaut de la vue (topDown : brouillard vers l'horizon ; side : voile leger sur les plans du fond)
	// <3DParticles> (diorama NES) : simulation SNES (boite torique autour de la camera), defauts selon la vue, puis positions scene pour le GPU.
	// topDown : X / Y = carte, Z = hauteur ; side : X = carte, Y = profondeur (depthmin..depthmax), Z = hauteur depuis le bas de l'ecran.
	if(!D.Diorama3DParticles.empty()) {
		if(_p3State.size() != D.Diorama3DParticles.size()) _p3State.assign(D.Diorama3DParticles.size(), P3State());
		const float dt = 1.0f / 60.0f, kPi = 3.14159265f;
		for(size_t pi = 0; pi < D.Diorama3DParticles.size() && scene.P3Count < 4; pi++) {
			const HdDiorama3DParticlesRule& R = D.Diorama3DParticles[pi];
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
			if(!found) { ox = _dioWX; oy = _dioWY; found = true; } // Game Boy : defilement cumule (SCX / SCY deplies)
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
	if(!D.DioramaMistRules.empty()) {
		const HdDioramaMistRule* mr = nullptr;
		for(const HdDioramaMistRule& r : D.DioramaMistRules) {
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
			if(!found) { ox = _dioWX; oy = _dioWY; found = true; } // Game Boy : defilement cumule (SCX / SCY deplies)
			const float tSec = (float)_frameNumber / 60.0f;
			for(int i = 0; i < M.Count && i < 8; i++) {
				const float k = 1.0f + 0.25f * (float)i; // nappes du haut un peu plus rapides
				M.OffX[i] = ox - std::cos(dir) * speed * k * tSec + 37.0f * (float)i;
				M.OffY[i] = oy + 0.0f - std::sin(dir) * speed * k * tSec + 53.0f * (float)i;
			}
		}
	}
	// <DioramaClouds> (doc 54) : derniere regle vraie par type ; repere : fond d'ancrage, sinon fond de sol GPU, sinon scroll PPU (ligne 120)
	if(!D.DioramaCloudRules.empty()) {
		const HdDioramaCloudRule* cr[2] = { nullptr, nullptr };
		for(const HdDioramaCloudRule& r : D.DioramaCloudRules) {
			bool ok = true;
			for(HdPackCondition* c : r.Conditions) { if(!c->CheckCondition(0, 0, nullptr)) { ok = false; break; } }
			if(ok) cr[r.Type & 1] = &r;
		}
		const float tSec = (float)_frameNumber / 60.0f;
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
			if(!found) { ox = _dioWX; oy = _dioWY; found = true; } // Game Boy : defilement cumule (SCX / SCY deplies)
			C.OffX = ox - std::cos(dir) * speed * tSec;
			C.OffY = oy + 0.0f - std::sin(dir) * speed * tSec;
		}
	}
	if(!D.DioramaFogRules.empty()) {
		const HdDioramaFogRule* fr = nullptr;
		for(const HdDioramaFogRule& r : D.DioramaFogRules) {
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
		const HdDayNightDef& dd = D.DayNightRules[_dn.Def].Def;
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
	if(!D.DioramaObjectRules.empty()) {
		vector<const HdDioramaObjectRule*> orules; // <DioramaObject>, puis instances <Tile2Obj> de cette image
		for(const HdDioramaObjectRule& q : D.DioramaObjectRules) orules.push_back(&q);
		if(_dioObjSpin.size() < orules.size()) _dioObjSpin.resize(orules.size(), 0.0f);
		int on = 0;
		vector<int32_t> wvals; // valeurs surveillees de cette image (INT32_MIN = adresse non lue), pour scrollx= / scrolly=
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
			if(!r.ScrollX.Empty() && (r.X - scx + r.Radius < -256.0f - 0.0f || r.X - scx - r.Radius > (float)scene.W + 256.0f + 0.0f)) continue; // hors du niveau visible
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
				o.WinX0 = (scx - r.Window - 0.0f - r.X) / r.Scale;
				o.WinX1 = (scx + (float)scene.W + r.Window + 0.0f - r.X) / r.Scale;
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
			MessageManager::Log("[HD-DIORAMA] H1 objets 3D : " + std::to_string(on) + " objet(s) actif(s) sur " + std::to_string(orules.size()) + " regle(s) <DioramaObject>, frame " + std::to_string(_frameNumber));
		}
	}
	const bool ok = gpu.SubmitScene(_dioL[0].data(), _dioL[1].data(), _dioL[2].data(), _dioL[3].data(), screenWidth, outHeight, scene) && gpu.FetchOpaque(out, pixels);
	if(!ok) {
		if(_dioFail++ < 3) MessageManager::Log("[HD-GB-DIORAMA] envoi ou relecture GPU en echec, rendu 2D utilise");
		return false;
	}
	static bool logged = false;
	if(!logged) {
		logged = true;
		char buf[200];
		snprintf(buf, sizeof(buf), "[HD-GB-DIORAMA] scene active : %ux%u, yaw %.1f, pitch %.1f, dist %.0f, vue %s, fenetre %s", screenWidth, outHeight, yaw, pitch, dist, _dioTopDown ? "topDown" : "side", _pack.Dio->WindowFixed ? "a plat" : "dans la scene");
		MessageManager::Log(buf);
	}
	return true;
}
