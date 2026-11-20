#include "pch.h"
#include <type_traits>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include "Shared/HdGpuCore.h"
#include "SNES/HdPacks/SnesHdGl.h"
#include "Shared/MessageManager.h"

#ifdef LIBRETRO
// Coeur libretro : pas de SDL2 ni de contexte OpenGL prive (meme choix que SnesHdGpu).
HdGpuCore& HdGpuCore::Get()
{
	static HdGpuCore instance;
	return instance;
}
bool HdGpuCore::Available() { return false; }
bool HdGpuCore::SubmitTest(uint32_t, uint32_t, uint32_t) { return false; }
bool HdGpuCore::SubmitLayers(const uint32_t*, const uint32_t*, const uint32_t*, const uint32_t*, uint32_t, uint32_t) { return false; }
bool HdGpuCore::Fetch(std::vector<uint32_t>&) { return false; }
bool HdGpuCore::FetchOpaque(uint32_t*, size_t) { return false; }
HdGpuTimes HdGpuCore::TakeLayerTimes() { return HdGpuTimes(); }
bool HdGpuCore::SubmitScene(const uint32_t*, const uint32_t*, const uint32_t*, const uint32_t*, uint32_t, uint32_t, const HdDioramaScene&) { return false; }
#else

#define SDL_MAIN_HANDLED
#include "SDL.h"
#include <cstdarg>
#include <fstream>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
// Le projet Core est compile en mode conforme : <windows.h> n'y compile pas. On declare donc seulement
// les 4 fonctions de kernel32 utiles ici (memes declarations que SnesHdGpu.cpp).
extern "C" {
	__declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t* name);
	__declspec(dllimport) int __stdcall GetModuleHandleExW(unsigned long flags, const wchar_t* name, void** module);
	__declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* module, wchar_t* path, unsigned long size);
	__declspec(dllimport) void* __stdcall LoadLibraryW(const wchar_t* name);
}
#endif

static SnesHdGlApi G = {};

HdGpuCore& HdGpuCore::Get()
{
	static HdGpuCore instance;
	return instance;
}

#ifdef _WIN32
// SDL2.dll est chargee "a la demande" (/DELAYLOAD) : on la cherche d'abord a cote de MesenCore.dll.
static int s_hdGpuCoreAnchor = 0; // adresse situee dans MesenCore.dll (pour retrouver son dossier)
static bool HdGpuCoreLoadSdlDll()
{
	if(GetModuleHandleW(L"SDL2.dll")) return true;
	void* self = nullptr;
	const unsigned long kFromAddress = 0x4, kUnchangedRefCount = 0x2; // GET_MODULE_HANDLE_EX_FLAG_*
	if(GetModuleHandleExW(kFromAddress | kUnchangedRefCount, (const wchar_t*)&s_hdGpuCoreAnchor, &self)) {
		wchar_t path[260] = {};
		unsigned long n = GetModuleFileNameW(self, path, 260);
		if(n > 0 && n < 260) {
			std::wstring p(path);
			size_t sl = p.find_last_of(L"\\/");
			if(sl != std::wstring::npos) {
				p = p.substr(0, sl + 1) + L"SDL2.dll";
				if(LoadLibraryW(p.c_str())) return true;
			}
		}
	}
	return LoadLibraryW(L"SDL2.dll") != nullptr;
}
#endif

bool HdGpuCore::Available()
{
	std::lock_guard<std::mutex> lk(_lock);
	if(!_tried) {
		_tried = true;
		_ok = Init();
		MessageManager::Log(_ok ? ("[HD-GPU-CORE] Contexte GPU commun actif : " + _info) : ("[HD-GPU-CORE] GPU indisponible (" + _info + ")."));
	}
	return _ok;
}

bool HdGpuCore::Init()
{
#ifdef _WIN32
	if(!HdGpuCoreLoadSdlDll()) { _info = "SDL2.dll introuvable"; return false; }
#endif
#ifdef __APPLE__
	_info = "macOS : pas encore gere";
	return false;
#endif
	SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
	if(!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		_info = std::string("SDL_InitSubSystem : ") + SDL_GetError();
		return false;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
	SDL_Window* win = SDL_CreateWindow("MesenHdGpuCore", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	if(!win) { _info = std::string("SDL_CreateWindow : ") + SDL_GetError(); SDL_GL_ResetAttributes(); return false; }
	SDL_GLContext ctx = SDL_GL_CreateContext(win);
	SDL_GL_ResetAttributes(); // ne pas modifier les reglages OpenGL du rendu de Mesen (Linux)
	if(!ctx) { _info = std::string("SDL_GL_CreateContext : ") + SDL_GetError(); SDL_DestroyWindow(win); return false; }
	_window = win; _context = ctx;

	bool missing = false;
	auto load = [&](auto& fn, const char* name) {
		fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(SDL_GL_GetProcAddress(name));
		if(!fn) { missing = true; _info = std::string("fonction OpenGL absente : ") + name; }
	};
	load(G.GetString, "glGetString"); load(G.GetError, "glGetError");
	load(G.Viewport, "glViewport"); load(G.Enable, "glEnable"); load(G.Disable, "glDisable");
	load(G.ClearBufferfv, "glClearBufferfv"); load(G.BlendFunc, "glBlendFunc"); load(G.DepthFunc, "glDepthFunc"); load(G.DepthMask, "glDepthMask");
	load(G.GenTextures, "glGenTextures"); load(G.DeleteTextures, "glDeleteTextures"); load(G.BindTexture, "glBindTexture");
	load(G.TexImage2D, "glTexImage2D"); load(G.TexSubImage2D, "glTexSubImage2D"); load(G.TexParameteri, "glTexParameteri"); load(G.PixelStorei, "glPixelStorei");
	load(G.ActiveTexture, "glActiveTexture"); load(G.GenerateMipmap, "glGenerateMipmap");
	load(G.GenFramebuffers, "glGenFramebuffers"); load(G.DeleteFramebuffers, "glDeleteFramebuffers"); load(G.BindFramebuffer, "glBindFramebuffer");
	load(G.FramebufferTexture2D, "glFramebufferTexture2D"); load(G.CheckFramebufferStatus, "glCheckFramebufferStatus");
	load(G.ReadPixels, "glReadPixels"); load(G.CreateShader, "glCreateShader"); load(G.ShaderSource, "glShaderSource");
	load(G.CompileShader, "glCompileShader"); load(G.GetShaderiv, "glGetShaderiv"); load(G.GetShaderInfoLog, "glGetShaderInfoLog");
	load(G.DeleteShader, "glDeleteShader"); load(G.CreateProgram, "glCreateProgram"); load(G.AttachShader, "glAttachShader");
	load(G.LinkProgram, "glLinkProgram"); load(G.GetProgramiv, "glGetProgramiv"); load(G.GetProgramInfoLog, "glGetProgramInfoLog");
	load(G.UseProgram, "glUseProgram"); load(G.GetUniformLocation, "glGetUniformLocation"); load(G.Uniform1i, "glUniform1i");
	load(G.Uniform1f, "glUniform1f"); load(G.Uniform2f, "glUniform2f"); load(G.Uniform3f, "glUniform3f"); load(G.Uniform4f, "glUniform4f");
	load(G.GenVertexArrays, "glGenVertexArrays"); load(G.BindVertexArray, "glBindVertexArray");
	load(G.GenBuffers, "glGenBuffers"); load(G.BindBuffer, "glBindBuffer"); load(G.BufferData, "glBufferData");
	load(G.DrawArrays, "glDrawArrays"); load(G.Flush, "glFlush");
	load(G.MapBufferRange, "glMapBufferRange"); load(G.UnmapBuffer, "glUnmapBuffer");
	if(missing) { ReleaseCurrent(); return false; }

	const char* ver = (const char*)G.GetString(GL_VERSION);
	const char* ren = (const char*)G.GetString(GL_RENDERER);
	_info = std::string("OpenGL ") + (ver ? ver : "?") + " - " + (ren ? ren : "?");

	if(!BuildProgram()) { ReleaseCurrent(); return false; }
	G.GenVertexArrays(1, &_vao); // un VAO (vide) est obligatoire en profil core, meme sans attribut
	ReleaseCurrent();
	return true;
}

bool HdGpuCore::MakeCurrent()
{
	// le contexte reste actif sur le thread video ; on ne le reactive que si le thread a change
	if(_currentThread == std::this_thread::get_id()) return true;
	if(SDL_GL_MakeCurrent((SDL_Window*)_window, (SDL_GLContext)_context) != 0) return false;
	_currentThread = std::this_thread::get_id();
	return true;
}

void HdGpuCore::ReleaseCurrent()
{
	if(_window) SDL_GL_MakeCurrent((SDL_Window*)_window, nullptr);
	_currentThread = std::thread::id();
}

// G0 : sommets calcules dans le shader (gl_VertexID), aucun tampon de sommets.
// uMode 0 : triangle colore tournant (3 sommets) ; uMode 1 : carre uni translucide (6 sommets).
static const char* kHdGpuCoreVertex = R"(#version 330 core
uniform int uMode;
uniform float uAngle;
uniform vec2 uCenter;   // centre en coordonnees ecran normalisees (-1..1)
uniform float uSize;
uniform float uAspect;  // largeur / hauteur de l'image
out vec3 vCol;
void main()
{
	vec2 tri[3] = vec2[3](vec2(0.0, 1.0), vec2(-0.866, -0.5), vec2(0.866, -0.5));
	vec3 col[3] = vec3[3](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));
	vec2 quad[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));
	vec2 q = (uMode == 0) ? tri[gl_VertexID] : quad[gl_VertexID];
	vCol = (uMode == 0) ? col[gl_VertexID] : vec3(1.0);
	float ca = cos(uAngle), sa = sin(uAngle);
	q = vec2(q.x * ca - q.y * sa, q.x * sa + q.y * ca);
	q.x /= uAspect;
	gl_Position = vec4(uCenter + q * uSize, 0.0, 1.0);
}
)";

static const char* kHdGpuCoreFragment = R"(#version 330 core
in vec3 vCol;
uniform int uMode;
uniform vec4 uTint;
layout(location = 0) out vec4 oColor;
void main()
{
	vec4 c = (uMode == 0) ? vec4(vCol, 1.0) : uTint;
	oColor = vec4(c.rgb * c.a, c.a);   // premultiplie
}
)";

static unsigned int HdGpuCoreCompile(GLenum type, const char* src, std::string& err)
{
	GLuint s = G.CreateShader(type);
	G.ShaderSource(s, 1, &src, nullptr);
	G.CompileShader(s);
	GLint ok = 0;
	G.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if(!ok) {
		char log[1024] = {};
		G.GetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
		err = std::string("shader : ") + log;
		G.DeleteShader(s);
		return 0;
	}
	return s;
}

bool HdGpuCore::BuildProgram()
{
	std::string err;
	GLuint v = HdGpuCoreCompile(GL_VERTEX_SHADER, kHdGpuCoreVertex, err);
	if(!v) { _info = err; return false; }
	GLuint f = HdGpuCoreCompile(GL_FRAGMENT_SHADER, kHdGpuCoreFragment, err);
	if(!f) { G.DeleteShader(v); _info = err; return false; }
	GLuint p = G.CreateProgram();
	G.AttachShader(p, v); G.AttachShader(p, f);
	G.LinkProgram(p);
	G.DeleteShader(v); G.DeleteShader(f);
	GLint ok = 0;
	G.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if(!ok) {
		char log[1024] = {};
		G.GetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
		_info = std::string("programme : ") + log;
		return false;
	}
	_prog = p;
	return true;
}

bool HdGpuCore::EnsureTarget(uint32_t w, uint32_t h)
{
	if(w == _w && h == _h && _fbo) return true;
	if(_fbo) { G.DeleteFramebuffers(1, &_fbo); G.DeleteTextures(1, &_tex); _fbo = 0; _tex = 0; }
	_w = w; _h = h;
	G.GenTextures(1, &_tex); G.BindTexture(GL_TEXTURE_2D, _tex);
	G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	G.GenFramebuffers(1, &_fbo); G.BindFramebuffer(GL_FRAMEBUFFER, _fbo);
	G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _tex, 0);
	const bool complete = G.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	return complete;
}

bool HdGpuCore::SubmitTest(uint32_t w, uint32_t h, uint32_t frame)
{
	std::lock_guard<std::mutex> lk(_lock);
	if(!_ok || w == 0 || h == 0) return false;
	if(!MakeCurrent()) return false;
	if(!EnsureTarget(w, h)) { ReleaseCurrent(); _ok = false; MessageManager::Log("[HD-GPU-CORE] Image cachee impossible : GPU desactive."); return false; }

	G.BindFramebuffer(GL_FRAMEBUFFER, _fbo);
	G.Viewport(0, 0, (GLsizei)w, (GLsizei)h);
	G.Disable(GL_SCISSOR_TEST); G.Disable(GL_DEPTH_TEST); G.Disable(GL_CULL_FACE);
	const GLfloat clear[4] = { 0, 0, 0, 0 };
	G.ClearBufferfv(GL_COLOR, 0, clear);
	G.Enable(GL_BLEND); G.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	G.UseProgram(_prog);
	G.BindVertexArray(_vao);
	auto U = [&](const char* n) { return G.GetUniformLocation(_prog, n); };
	G.Uniform1f(U("uAspect"), (float)w / (float)h);
	// carre jaune translucide, fixe, en bas a gauche
	G.Uniform1i(U("uMode"), 1); G.Uniform1f(U("uAngle"), 0.0f);
	G.Uniform2f(U("uCenter"), -0.55f, -0.55f); G.Uniform1f(U("uSize"), 0.25f);
	G.Uniform4f(U("uTint"), 1.0f, 1.0f, 0.0f, 0.5f);
	G.DrawArrays(GL_TRIANGLES, 0, 6);
	// triangle colore tournant, au centre
	G.Uniform1i(U("uMode"), 0); G.Uniform1f(U("uAngle"), (float)frame * 0.05f);
	G.Uniform2f(U("uCenter"), 0.0f, 0.0f); G.Uniform1f(U("uSize"), 0.4f);
	G.DrawArrays(GL_TRIANGLES, 0, 3);
	G.BindVertexArray(0);
	G.Disable(GL_BLEND);

	StartReadback(w, h);
	return true;
}

// Relecture ASYNCHRONE de l'image cachee (_fbo, liee en lecture) dans un tampon PBO : glReadPixels rend la main tout de suite.
void HdGpuCore::StartReadback(uint32_t w, uint32_t h)
{
	const size_t bytes = (size_t)w * h * 4;
	if(!_pbo) G.GenBuffers(1, &_pbo);
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pbo);
	if(bytes > _pboCapacity) {
		G.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_READ);
		_pboCapacity = bytes;
	}
	G.PixelStorei(GL_PACK_ALIGNMENT, 4);
	G.ReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_BYTE, (void*)0);
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	G.Flush();
	const GLenum err = G.GetError();
	if(err != GL_NO_ERROR) {
		static int logged = 0;
		if(logged++ < 3) MessageManager::Log("[HD-GPU-CORE] Erreur OpenGL " + std::to_string(err));
	}
	_pending = true; _pendW = (int)w; _pendH = (int)h;
}

// G2b : triangle plein ecran (3 sommets, aucun tampon), 4 textures de couches lues pixel pour pixel par texelFetch.
// Les textures sont envoyees ligne 0 = haut ; gl_FragCoord a son origine en bas : la ligne est retournee dans le shader,
// et Fetch retourne de nouveau les lignes de l'image relue, donc la sortie est dans le meme ordre que les entrees.
static const char* kHdGpuCoreLayerVertex = R"(#version 330 core
void main()
{
	vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
	gl_Position = vec4(p[gl_VertexID], 0.0, 1.0);
}
)";

static const char* kHdGpuCoreLayerFragment = R"(#version 330 core
uniform sampler2D uL0;
uniform sampler2D uL1;
uniform sampler2D uL2;
uniform sampler2D uL3;
uniform int uH;
layout(location = 0) out vec4 oColor;
void main()
{
	ivec2 p = ivec2(int(gl_FragCoord.x), uH - 1 - int(gl_FragCoord.y));
	vec4 c = texelFetch(uL0, p, 0);
	vec4 s = texelFetch(uL1, p, 0); c = s + c * (1.0 - s.a);
	s = texelFetch(uL2, p, 0); c = s + c * (1.0 - s.a);
	s = texelFetch(uL3, p, 0); c = s + c * (1.0 - s.a);
	oColor = c;
}
)";

bool HdGpuCore::BuildLayerProgram()
{
	std::string err;
	GLuint v = HdGpuCoreCompile(GL_VERTEX_SHADER, kHdGpuCoreLayerVertex, err);
	if(!v) { MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint f = HdGpuCoreCompile(GL_FRAGMENT_SHADER, kHdGpuCoreLayerFragment, err);
	if(!f) { G.DeleteShader(v); MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint p = G.CreateProgram();
	G.AttachShader(p, v); G.AttachShader(p, f);
	G.LinkProgram(p);
	G.DeleteShader(v); G.DeleteShader(f);
	GLint ok = 0;
	G.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if(!ok) {
		char log[1024] = {};
		G.GetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
		MessageManager::Log(std::string("[HD-GPU-CORE] programme des couches : ") + log);
		return false;
	}
	_progLayers = p;
	return true;
}

bool HdGpuCore::SubmitLayers(const uint32_t* l0, const uint32_t* l1, const uint32_t* l2, const uint32_t* l3, uint32_t w, uint32_t h)
{
	std::lock_guard<std::mutex> lk(_lock);
	if(!_ok || _layerProgFailed || w == 0 || h == 0) return false;
	if(!MakeCurrent()) return false;
	if(!_progLayers && !BuildLayerProgram()) { _layerProgFailed = true; return false; }
	if(!EnsureTarget(w, h)) { ReleaseCurrent(); _ok = false; MessageManager::Log("[HD-GPU-CORE] Image cachee impossible : GPU desactive."); return false; }

	if(!_layerTex[0] || w != _layerW || h != _layerH) {
		for(int k = 0; k < 4; k++) {
			if(_layerTex[k]) G.DeleteTextures(1, &_layerTex[k]);
			G.GenTextures(1, &_layerTex[k]);
			G.BindTexture(GL_TEXTURE_2D, _layerTex[k]);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		_layerW = w; _layerH = h;
		for(int k = 0; k < 4; k++) _layerStore[k] = false;
	}
	const auto tA = std::chrono::steady_clock::now();
	const uint32_t* src[4] = { l0, l1, l2, l3 };
	G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	for(int k = 0; k < 4; k++) {
		G.ActiveTexture(GL_TEXTURE0 + k);
		G.BindTexture(GL_TEXTURE_2D, _layerTex[k]);
		// 0xAARRGGBB en memoire = octets B, G, R, A
		G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA, GL_UNSIGNED_BYTE, src[k]);
		_layerStore[k] = false; // G3h : SubmitScene devra renvoyer l'image entiere
	}

	const auto tB = std::chrono::steady_clock::now();
	G.BindFramebuffer(GL_FRAMEBUFFER, _fbo);
	G.Viewport(0, 0, (GLsizei)w, (GLsizei)h);
	G.Disable(GL_SCISSOR_TEST); G.Disable(GL_DEPTH_TEST); G.Disable(GL_CULL_FACE); G.Disable(GL_BLEND);
	G.UseProgram(_progLayers);
	G.Uniform1i(G.GetUniformLocation(_progLayers, "uL0"), 0);
	G.Uniform1i(G.GetUniformLocation(_progLayers, "uL1"), 1);
	G.Uniform1i(G.GetUniformLocation(_progLayers, "uL2"), 2);
	G.Uniform1i(G.GetUniformLocation(_progLayers, "uL3"), 3);
	G.Uniform1i(G.GetUniformLocation(_progLayers, "uH"), (GLint)h);
	G.BindVertexArray(_vao);
	G.DrawArrays(GL_TRIANGLES, 0, 3);
	G.BindVertexArray(0);
	G.ActiveTexture(GL_TEXTURE0);

	StartReadback(w, h);
	const auto tC = std::chrono::steady_clock::now();
	_times.UploadMs += std::chrono::duration<double, std::milli>(tB - tA).count();
	_times.DrawMs += std::chrono::duration<double, std::milli>(tC - tB).count();
	_times.Count++;
	return true;
}

// G3a : un plan = 2 triangles (6 sommets calcules par gl_VertexID). Le plan de profondeur Z est agrandi de (D + Z) / D pour que,
// vu de face, il remplisse l'ecran. La scene tourne autour du centre du plan Z = 0 (matrice = Rx(pitch) * Ry(yaw), rangees
// envoyees en trois vecteurs). w = profondeur : l'interpolation des coordonnees de texture est corrigee de la perspective.
// Pas de decoupage proche : les angles sont bornes par le chargeur de pack.
static const char* kHdGpuCoreSceneVertex = R"(#version 330 core
uniform vec2 uSize;     // largeur, hauteur de la scene (pixels NES)
uniform float uZ;       // profondeur du plan
uniform float uZoom;    // G4 : zoom de la vue (1 = normal)
uniform int uExt;       // G4a : 1 = quad etendu, vUV = fraction du plan (hors de 0..1 dans les marges)
uniform float uKFix;    // G3c3 : echelle du plan imposee (> 0), sinon (D + Z) / D propre a chaque tranche
uniform float uD;       // distance de la camera (= focale)
uniform vec2 uO;        // axe optique (pixels NES)
uniform vec3 uRt0, uRt1, uRt2;
uniform float uScale;   // pixels HD par pixel NES
uniform vec2 uOut;      // taille de l'image (pixels HD)
uniform vec4 uR;        // G3c : partie du plan couverte (origine xy, taille zw, en fraction) ; (0,0,1,1) = tout le plan
uniform int uZb;        // occlude=on : ecrit la profondeur (meme formule que les objets 3D) pour le test contre eux
uniform int uFlat;     // doc 53 (topDown) : 1 = couche posee a plat (sol) : uZ = hauteur, l'axe vertical de l'image devient la profondeur
uniform float uIncC, uIncS; // doc 53 : cos et sin de l'inclinaison du plan a plat (0 = parallele au sol, 90 = parallele a l'ecran)
out vec2 vUV;
void main()
{
	vec2 q[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
	vec2 uv = q[gl_VertexID];
	float k = uFlat != 0 ? 1.0 : (uKFix > 0.0 ? uKFix : (uD + uZ) / uD);
	vec2 pv = uR.xy + uv * uR.zw;
	vec3 P = uFlat != 0 ? vec3((pv.x - 0.5) * uSize.x, -uZ * uIncC + (pv.y - 0.5) * uSize.y * uIncS, uZ * uIncS - (pv.y - 0.5) * uSize.y * uIncC) : vec3((pv.x - 0.5) * uSize.x * k, (pv.y - 0.5) * uSize.y * k, uZ);
	vec3 c = vec3(dot(uRt0, P), dot(uRt1, P), dot(uRt2, P)) + vec3(0.0, 0.0, uD);
	float dpt = max(c.z, 1.0);
	vec2 s = uO + c.xy * (uD / dpt) * uZoom;
	vec2 hd = s * uScale;
	vec2 ndc = vec2(hd.x / uOut.x * 2.0 - 1.0, 1.0 - hd.y / uOut.y * 2.0);
	float zb = uZb != 0 ? (16384.0 + 1.0) / (16384.0 - 1.0) * dpt - 2.0 * 16384.0 / (16384.0 - 1.0) : 0.0;
	gl_Position = vec4(ndc * dpt, zb, dpt);
	vUV = uExt != 0 ? pv : uv;
}
)";

static const char* kHdGpuCoreSceneFragment = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform sampler2D uTopTex;   // G3b2 : motif du dessus et du dessous
uniform sampler2D uSideTex;  // G3b2 : motif des flancs
uniform int uExt;       // G4a : 1 = quad etendu
uniform int uExtDebug;   // G4 : 1 = teinte magenta des pixels venus d'une extension
uniform vec4 uExtMode;  // G4a : mode des cotes haut, bas, gauche, droite (0 aucun, 1 stretch, 2 repeat, 3 mirror, 4 wrap)
uniform vec4 uExtBand;  // G4a : bande repetee (pixels NES) des cotes haut, bas, gauche, droite
uniform vec4 uExtRangeV; // G4a : plage des colonnes (x0, x1) du cote haut puis du cote bas
uniform vec4 uExtRangeH; // G4a : plage des lignes (y0, y1) du cote gauche puis du cote droit
uniform vec4 uExtEdge;  // G4a : position de depart de l'extension : haut y, bas y, gauche x, droite x (pixels NES)
uniform vec4 uExtFog;   // G4c : force de brume des cotes haut, bas, gauche, droite
uniform vec4 uExtFogDist; // G4c : distance (pixels NES) pour atteindre la force
uniform vec4 uExtColT, uExtColB, uExtColL, uExtColR; // G4c : couleur de brume (rgb, w = 1 si definie)
uniform vec2 uExtBandY;  // G4a : groupes : bande de lignes de la texture (debut, hauteur, fractions du plan)
uniform vec2 uExtScroll; // G4a : defilement x, y (pixels) pour aligner les bandes sur la grille de tiles du monde
float gPlaneY;           // fraction du plan en Y du fragment
float gFogF = 0.0;       // G4c : brume du pixel (0..1)
vec4 gFogCol = vec4(0.0);
bool gSolid = false;     // G4c : mode color (aplat)
void FogSide(float mode, float d, float fog, float dist, vec4 col)
{
	if(mode < 0.5 || d <= 0.0) return;
	if(int(mode + 0.5) == 5) { gSolid = true; gFogCol = col; }
	float f = fog * clamp(d / max(dist, 1.0), 0.0, 1.0);
	if(f > gFogF) { gFogF = f; gFogCol = col; }
}
uniform float uZ;       // G6a : profondeur de la tranche (meme variable que dans le sommet)
uniform int uFlat;     // doc 53 (topDown) : 1 = couche a plat (sol) (meme variable que dans le sommet)
uniform float uIncC, uIncS; // doc 53 : cos et sin de l'inclinaison du plan a plat (meme variable que dans le sommet)
uniform float uKFix;    // G6a : echelle imposee (meme variable que dans le sommet)
uniform float uD;       // G6a : focale (meme variable que dans le sommet)
uniform int uLightOn;   // G6a : 1 = lumieres de scene actives
uniform int uNrmOn;     // G6d : 1 = la couche a une carte de normales
uniform sampler2D uNrmTex;  // G6d : carte de normales (unite 4)
uniform vec2 uNrmBand;      // G6d : bande de lignes de la carte (debut, hauteur, fractions de l'image)
uniform vec2 uNrmP;         // G6d : x force du relief, y 1 = vert vers le bas
uniform int uShOn;      // G6c : 1 = ombres des sprites recues par cette couche
uniform sampler2D uShTex;   // G6c : masque des silhouettes (unite 3)
uniform vec2 uShBand;       // G6c : bande de lignes du masque (debut, hauteur, fractions de l'image)
uniform vec4 uShL;          // G6c : lumiere qui projette : xyz position (ponctuelle) ou vecteur vers la lumiere (soleil), w type
uniform vec4 uShP;          // G6c : x opacite, y adoucissement (pixels NES), z profondeur du plan des sprites, w echelle (D + z) / D
uniform vec3 uShCol;        // G6c : couleur de l'ombre
uniform vec3 uLAmb;     // G6a : lumiere ambiante
uniform vec4 uLP0, uLP1, uLP2, uLP3, uLP4, uLP5, uLP6, uLP7;  // xyz position (ponctuelle) ou vecteur vers la lumiere (soleil), w = type (0 aucune, 2 ponctuelle, 3 soleil)
uniform vec4 uLC0, uLC1, uLC2, uLC3, uLC4, uLC5, uLC6, uLC7;  // rgb = couleur x intensite, w = portee
uniform float uShade;   // G3c3 : facteur de couleur de la tranche (1 = inchange)
uniform float uInvN;    // G3b1 : 1 / nombre de tranches de la couche (1 = feuille)
uniform float uDepth;   // G3b2 : profondeur de la tranche depuis la face avant (pixels NES), 0 = face avant
uniform int uHasTop, uHasSide;
uniform vec2 uOrigins;  // pixels NES : debut du motif du dessus en X, debut du motif des flancs en Y
uniform vec4 uPeriods;  // pixels NES par repetition : x dessus (le long du bord), y dessus (profondeur), z flancs (bord), w flancs (profondeur)
uniform float uScroll;  // defilement horizontal (pixels NES)
uniform int uVolVert;   // G3b3 : 1 = images empilees de haut en bas, 0 = cote a cote
uniform int uSideVol;   // G3b3 : nombre d'images de la bande volume des flancs (0 = motif simple)
uniform vec4 uVol;      // G3b3 : repetition x, y (pixels NES), origine x, y
uniform float uVolThick; // G3b3 : epaisseur de la couche (pixels NES)
uniform vec2 uSize;     // taille de la scene (pixels NES), meme variable que dans le sommet
uniform vec4 uR;        // partie du plan couverte, meme variable que dans le sommet
uniform float uScale;   // pixels HD par pixel NES
uniform int uBg;        // 1 = fond <background> : la texture est le PNG, repete et decale (uBgP)
uniform vec4 uBgP;      // decalage x, y (pixels HD), premiere ligne NES de l'image, luminosite
uniform vec2 uBgTile;   // repetition en x, en y
layout(location = 0) out vec4 oColor;
float AlphaAt(ivec2 q, ivec2 sz)
{
	if(q.x < 0 || q.y < 0 || q.x >= sz.x || q.y >= sz.y) return 0.0;   // hors image = vide
	return texelFetch(uTex, q, 0).a;
}
vec4 SideVolume()
{
	ivec2 ts = textureSize(uSideTex, 0);
	int n = max(uSideVol, 1);
	ivec2 iz = uVolVert != 0 ? ivec2(ts.x, max(ts.y / n, 1)) : ivec2(max(ts.x / n, 1), ts.y);
	vec2 pl = vec2((vUV.x * uSize.x + uScroll - uVol.z) / uVol.x, (gPlaneY * uSize.y - uVol.w) / uVol.y);
	pl = fract(pl);
	int k = clamp(int(uDepth / max(uVolThick, 0.01) * float(n)), 0, n - 1);
	ivec2 off = uVolVert != 0 ? ivec2(0, k * iz.y) : ivec2(k * iz.x, 0);
	ivec2 q = off + ivec2(min(int(pl.x * float(iz.x)), iz.x - 1), min(int(pl.y * float(iz.y)), iz.y - 1));
	return texelFetch(uSideTex, q, 0);
}
vec4 TopFromVolume()
{
	// dessus / dessous d'une dalle en volume : la profondeur est partagee en N bandes, la bande k montre l'image k de la bande PNG
	// (x le long du bord avec la repetition de stretchx, tout l'ecart de la bande en profondeur)
	ivec2 ts = textureSize(uSideTex, 0);
	int n = max(uSideVol, 1);
	ivec2 iz = uVolVert != 0 ? ivec2(ts.x, max(ts.y / n, 1)) : ivec2(max(ts.x / n, 1), ts.y);
	float s = clamp(uDepth / max(uVolThick, 0.01), 0.0, 0.9999) * float(n);
	int k = clamp(int(s), 0, n - 1);
	vec2 pl = vec2(fract((vUV.x * uSize.x + uScroll - uVol.z) / uVol.x), fract(s));
	ivec2 off = uVolVert != 0 ? ivec2(0, k * iz.y) : ivec2(k * iz.x, 0);
	return texelFetch(uSideTex, off + ivec2(min(int(pl.x * float(iz.x)), iz.x - 1), min(int(pl.y * float(iz.y)), iz.y - 1)), 0);
}
float ExtendAxis(float q, float len, float modeLo, float modeHi, float bandLo, float bandHi, float edgeLo, float edgeHi, bool loOk, bool hiOk, float scroll, out bool hole)
{
	// G4a : coordonnee q (pixels NES) au-dela d'un bord (edgeLo ou edgeHi, 0 et len par defaut) ramenee dans l'image selon le mode
	hole = false;
	int mLo = int(modeLo + 0.5), mHi = int(modeHi + 0.5);
	bool lo = mLo != 0 && q < edgeLo && loOk;
	bool hi = mHi != 0 && q > edgeHi && hiOk;
	if(!lo && !hi) {
		if(q < 0.0 || q > len) { hole = true; return clamp(q, 0.0, len); }
		return q;
	}
	if(lo) {
		float band = max(bandLo, 1.0);
		if(mLo == 1 || mLo == 5) return edgeLo;
		if(mLo == 2) {
			float b0 = edgeLo + mod(8.0 - mod(edgeLo + scroll, 8.0), 8.0);   // premiere frontiere de tile du monde apres le bord
			return b0 + mod(q - b0, band);
		}
		if(mLo == 3) { float L = max(len - edgeLo, 1.0); return edgeLo + (L - abs(mod(edgeLo - q, 2.0 * L) - L)); }
		if(mLo == 4) return edgeLo + mod(q - edgeLo, max(len - edgeLo, 1.0));
	} else {
		float band = max(bandHi, 1.0);
		if(mHi == 1 || mHi == 5) return edgeHi;
		if(mHi == 2) {
			float b1 = edgeHi - mod(edgeHi + scroll, 8.0);                   // derniere frontiere de tile du monde avant le bord
			return b1 - band + mod(q - b1, band);
		}
		if(mHi == 3) { float L = max(edgeHi, 1.0); return edgeHi - (L - abs(mod(q - edgeHi, 2.0 * L) - L)); }
		if(mHi == 4) return mod(q, max(edgeHi, 1.0));
	}
	hole = true;
	return clamp(q, 0.0, len);
}
vec3 LightOne(vec4 lp, vec4 lc, vec3 P, vec3 N)
{
	if(lp.w < 1.5) return vec3(0.0);
	if(lp.w > 2.5) return lc.rgb * max(dot(N, lp.xyz), 0.0);   // soleil
	vec3 d = lp.xyz - P;
	float dist = length(d);
	float k = clamp(1.0 - dist / max(lc.w, 1.0), 0.0, 1.0);
	return lc.rgb * max(dot(N, d / max(dist, 0.001)), 0.0) * k * k;
}
void main()
{
	vec2 uv0 = vUV;   // coordonnee de lecture de la texture
	gPlaneY = uExt != 0 ? vUV.y : uR.y + vUV.y * uR.w;
	bool hole = false;
	bool remapped = false;
	if(uExt != 0) {
		bool hx, hy;
		vec2 q0 = vUV * uSize;
		vec2 q = q0;
		bool lOk = q0.y >= uExtRangeH.x && q0.y < uExtRangeH.y, rOk = q0.y >= uExtRangeH.z && q0.y < uExtRangeH.w;   // plages rows
		bool tOk = q0.x >= uExtRangeV.x && q0.x < uExtRangeV.y, bOk = q0.x >= uExtRangeV.z && q0.x < uExtRangeV.w;   // plages cols
		q.x = ExtendAxis(q0.x, uSize.x, uExtMode.z, uExtMode.w, uExtBand.z, uExtBand.w, uExtEdge.z, uExtEdge.w, lOk, rOk, uExtScroll.x, hx);
		q.y = ExtendAxis(q0.y, uSize.y, uExtMode.x, uExtMode.y, uExtBand.x, uExtBand.y, uExtEdge.x, uExtEdge.y, tOk, bOk, uExtScroll.y, hy);
		remapped = (q.x != q0.x) || (q.y != q0.y);
		FogSide(uExtMode.x, tOk ? uExtEdge.x - q0.y : 0.0, uExtFog.x, uExtFogDist.x, uExtColT);
		FogSide(uExtMode.y, bOk ? q0.y - uExtEdge.y : 0.0, uExtFog.y, uExtFogDist.y, uExtColB);
		FogSide(uExtMode.z, lOk ? uExtEdge.z - q0.x : 0.0, uExtFog.z, uExtFogDist.z, uExtColL);
		FogSide(uExtMode.w, rOk ? q0.x - uExtEdge.w : 0.0, uExtFog.w, uExtFogDist.w, uExtColR);
		uv0 = q / uSize;
		uv0.y = (uv0.y - uExtBandY.x) / uExtBandY.y;
		hole = hx || hy || uv0.y < -0.0001 || uv0.y > 1.0001; // groupes : ligne repetee hors de la bande de lignes envoyee
	}
	vec4 t;
	if(uBg != 0) {
		// fond : coordonnees du plan en pixels NES -> pixel du PNG (meme calcul que le filtre normal : (left + defilement + x) x echelle)
		vec2 pn = vec2((uR.x + vUV.x * uR.z) * uSize.x, gPlaneY * uSize.y);
		vec2 tp = vec2(uBgP.x + pn.x * uScale, uBgP.y + (uBgP.z + pn.y) * uScale);
		vec2 ts = vec2(textureSize(uTex, 0));
		bool inX = uBgTile.x > 0.5 || (tp.x >= 0.0 && tp.x < ts.x);
		bool inY = uBgTile.y > 0.5 || (tp.y >= 0.0 && tp.y < ts.y);
		t = (inX && inY) ? textureLod(uTex, tp / ts, 0.0) : vec4(0.0);
		t.rgb *= uBgP.w;
	} else {
		t = texture(uTex, uv0);   // texture premultipliee
	}
	// G3b2 : tranches arriere. Un pixel du bord de la forme (a moins d'un pixel NES d'un vide) prend le motif du mur : dessus ou
	// dessous = motif du dessus, ancre au monde (xN + defilement) ; flancs = motif des flancs. Le reste garde l'image d'origine.
	vec2 wUp = vec2((vUV.x * uSize.x + uScroll - uOrigins.x) / uPeriods.x, uDepth / uPeriods.y);
	vec2 wSide = vec2((gPlaneY * uSize.y - uOrigins.y) / uPeriods.z, uDepth / uPeriods.w);
	vec4 patTop = texture(uTopTex, wUp);     // echantillons hors des branches (derivees definies)
	vec4 patSide = texture(uSideTex, wSide);
	if(uDepth > 0.0 && t.a > 0.0 && (uHasTop != 0 || uHasSide != 0)) {
		ivec2 sz = textureSize(uTex, 0);
		ivec2 p = clamp(ivec2(uv0 * vec2(sz)), ivec2(0), sz - ivec2(1));
		int b = max(int(uScale + 0.5), 1);
		bool eUp = AlphaAt(p + ivec2(0, -b), sz) < 0.5;
		bool eDown = AlphaAt(p + ivec2(0, b), sz) < 0.5;
		bool eLeft = AlphaAt(p + ivec2(-b, 0), sz) < 0.5;
		bool eRight = AlphaAt(p + ivec2(b, 0), sz) < 0.5;
		if((eUp || eDown) && uHasTop != 0) t = patTop * t.a;
		else if(uSideVol > 0 && uHasSide != 0 && (eUp || eDown)) t = TopFromVolume() * t.a;
		else if(uSideVol > 0 && uHasSide != 0 && (eLeft || eRight)) t = SideVolume() * t.a;
		else if((eLeft || eRight) && uHasSide != 0) t = patSide * t.a;
	}
	// G3b1 : N tranches empilees doivent donner, vues de face, la meme image qu'une seule : alpha de tranche a' = 1 - (1 - a)^(1/N),
	// couleur (non premultipliee) inchangee. Opaque et transparent ne changent pas.
	if(uInvN < 1.0 && t.a > 0.0 && t.a < 1.0) {
		float a2 = 1.0 - pow(1.0 - t.a, uInvN);
		t = vec4(t.rgb * (a2 / t.a), a2);
	}
	// G6a : lumieres de scene. Position monde du pixel : plan a l'echelle k de sa tranche, profondeur uZ. Normale : face avant (0, 0, -1) ;
	// sur une tranche arriere, un pixel de bord prend la normale du flanc (haut, bas, gauche, droite).
	if(uLightOn != 0 && t.a > 0.0 && !hole) {
		float kk = uKFix > 0.0 ? uKFix : (uD + uZ) / uD;
		float px = uExt != 0 ? vUV.x : uR.x + vUV.x * uR.z;
		vec3 P = vec3((px - 0.5) * uSize.x * kk + uSize.x * 0.5, (gPlaneY - 0.5) * uSize.y * kk + uSize.y * 0.5, uZ);
		vec3 N = vec3(0.0, 0.0, -1.0);
		// doc 53 (topDown) : couche a plat = sol. Position comme au sommet (plan XZ, hauteur -uZ) et normale vers le haut, sinon la lumiere ne touche pas le sol.
		if(uFlat != 0) {
			P = vec3((px - 0.5) * uSize.x + uSize.x * 0.5, -uZ * uIncC + (gPlaneY - 0.5) * uSize.y * uIncS, uZ * uIncS - (gPlaneY - 0.5) * uSize.y * uIncC);
			N = vec3(0.0, -uIncC, -uIncS);
		}
		if(uDepth > 0.0 && uFlat == 0) {
			ivec2 sz = textureSize(uTex, 0);
			ivec2 p = clamp(ivec2(uv0 * vec2(sz)), ivec2(0), sz - ivec2(1));
			int b = max(int(uScale + 0.5), 1);
			N.x = (AlphaAt(p + ivec2(b, 0), sz) < 0.5 ? 1.0 : 0.0) - (AlphaAt(p + ivec2(-b, 0), sz) < 0.5 ? 1.0 : 0.0);
			N.y = (AlphaAt(p + ivec2(0, b), sz) < 0.5 ? 1.0 : 0.0) - (AlphaAt(p + ivec2(0, -b), sz) < 0.5 ? 1.0 : 0.0);
			N = normalize(N + vec3(0.0, 0.0, -0.35));
		}
		if(uNrmOn != 0) {
			float vb = (uv0.y - uNrmBand.x) / max(uNrmBand.y, 0.0001);
			if(uv0.x >= 0.0 && uv0.x <= 1.0 && vb >= 0.0 && vb <= 1.0) {
				vec4 nm = textureLod(uNrmTex, vec2(uv0.x, vb), 0.0);
				if(nm.a > 0.5) {
					vec3 nt = nm.rgb * 2.0 - 1.0;
					if(uNrmP.y > 0.5) nt.y = -nt.y;
					nt.xy *= uNrmP.x;
					N = normalize(vec3(nt.x, -nt.y, -nt.z));   // image : y vers le haut, z vers l'observateur ; monde : y vers le bas, z vers le fond
				}
			}
		}
		vec3 lit = uLAmb + LightOne(uLP0, uLC0, P, N) + LightOne(uLP1, uLC1, P, N) + LightOne(uLP2, uLC2, P, N) + LightOne(uLP3, uLC3, P, N)
			+ LightOne(uLP4, uLC4, P, N) + LightOne(uLP5, uLC5, P, N) + LightOne(uLP6, uLC6, P, N) + LightOne(uLP7, uLC7, P, N);
		t.rgb = min(t.rgb * lit, vec3(t.a));
	}
	// G6c : ombre des sprites. Le rayon du pixel vers la lumiere coupe le plan des sprites ; le masque y dit si un sprite s'y trouve.
	if(uShOn != 0 && t.a > 0.0 && !hole) {
		float kk = uKFix > 0.0 ? uKFix : (uD + uZ) / uD;
		float px = uExt != 0 ? vUV.x : uR.x + vUV.x * uR.z;
		vec3 P = vec3((px - 0.5) * uSize.x * kk + uSize.x * 0.5, (gPlaneY - 0.5) * uSize.y * kk + uSize.y * 0.5, uZ);
		vec3 dv = uShL.w > 2.5 ? uShL.xyz : (uShL.xyz - P);
		if(dv.z < -0.0001) {
			float tt = (uShP.z - P.z) / dv.z;
			if(tt > 0.0 && (uShL.w > 2.5 || tt < 1.0)) {
				vec3 S = P + dv * tt;
				vec2 c = vec2((S.x - uSize.x * 0.5) / (uSize.x * uShP.w) + 0.5, (S.y - uSize.y * 0.5) / (uSize.y * uShP.w) + 0.5);
				vec2 st = vec2(uShP.y / (uSize.x * uShP.w), uShP.y / (uSize.y * uShP.w));
				vec2 o[5] = vec2[5](vec2(0.0), vec2(st.x, 0.0), vec2(-st.x, 0.0), vec2(0.0, st.y), vec2(0.0, -st.y));
				float a = 0.0;
				for(int i = 0; i < 5; i++) {
					vec2 q = c + o[i];
					float vb = (q.y - uShBand.x) / max(uShBand.y, 0.0001);
					if(q.x >= 0.0 && q.x <= 1.0 && vb >= 0.0 && vb <= 1.0) a += textureLod(uShTex, vec2(q.x, vb), 0.0).a;
				}
				a = uShP.y > 0.0 ? a * 0.2 : textureLod(uShTex, vec2(clamp(c.x, 0.0, 1.0), clamp((c.y - uShBand.x) / max(uShBand.y, 0.0001), 0.0, 1.0)), 0.0).a * ((c.x >= 0.0 && c.x <= 1.0 && c.y >= uShBand.x && c.y <= uShBand.x + uShBand.y) ? 1.0 : 0.0);
				float sh = clamp(a * uShP.x, 0.0, 1.0);
				t.rgb = mix(t.rgb, uShCol * t.a, sh);
			}
		}
	}
	if(gSolid && remapped) t = vec4(gFogCol.rgb, 1.0);   // G4c : mode color
	if(gFogF > 0.0 && remapped) {
		if(gFogCol.w > 0.5) t.rgb = mix(t.rgb, gFogCol.rgb * t.a, gFogF);   // brume coloree : la couleur ne touche que les pixels visibles
		else t *= (1.0 - gFogF);                                               // sans couleur : fondu vers le transparent
	}
	if(uExtDebug != 0 && remapped && !hole) t = t * 0.7 + vec4(0.3, 0.0, 0.3, 0.3);   // controle : zone d'extension en magenta
	oColor = hole ? vec4(0.0) : vec4(t.rgb * uShade, t.a);   // premultiplie : la couleur seule s'assombrit
}
)";

// H1 : objets 3D. Les sommets sont lus dans une texture RGBA32F (3 texels par sommet : position, normale, uv), 256 sommets par ligne.
// Meme projection que les plans de la scene ; la profondeur sert a l'occlusion entre les triangles des objets.
static const char* kHdGpuCoreObjVertex = R"(#version 330 core
uniform sampler2D uMesh;
uniform vec3 uMr0, uMr1, uMr2;   // rotation du modele (lignes)
uniform vec3 uAnchor;            // position de l'ancre (axes de la scene centres sur la scene, echelle de profondeur deja appliquee)
uniform float uK;                // (D + z) / D de l'ancre
uniform float uMScale;           // pixels NES par unite du modele
uniform vec2 uO;
uniform vec3 uRt0, uRt1, uRt2;
uniform float uD, uZoom, uScale;
uniform vec2 uOut;
uniform vec3 uWin;               // fenetre en x du modele (min, max, 1 = active)
out vec2 vUV;
out vec3 vN;
out vec3 vPc;                    // position du point (axes de la scene centres sur la scene) pour l'eclairage
vec4 Fetch(int vi, int j)
{
	return texelFetch(uMesh, ivec2((vi % 256) * 3 + j, vi / 256), 0);
}
void main()
{
	int vi = gl_VertexID;
	if(uWin.z > 0.5) {   // le triangle entier est decide sur son premier sommet : hors fenetre, il sort du volume de decoupage
		float wx = Fetch(vi - vi % 3, 0).x;
		if(wx < uWin.x || wx > uWin.y) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); vUV = vec2(0.0); vN = vec3(0.0, 0.0, 1.0); vPc = vec3(0.0); return; }
	}
	vec3 p = Fetch(vi, 0).xyz;
	vec3 n = Fetch(vi, 1).xyz;
	vUV = Fetch(vi, 2).xy;
	vec3 m = vec3(dot(uMr0, p), dot(uMr1, p), dot(uMr2, p));
	vec3 mn = vec3(dot(uMr0, n), dot(uMr1, n), dot(uMr2, n));
	m *= uMScale;
	m = vec3(m.x, -m.y, -m.z);       // modele (Y haut, Z vers l'observateur) -> scene (Y bas, Z vers le fond)
	vN = vec3(mn.x, -mn.y, -mn.z);
	vec3 P = uAnchor + vec3(m.xy * uK, m.z);
	vPc = P;
	vec3 c = vec3(dot(uRt0, P), dot(uRt1, P), dot(uRt2, P)) + vec3(0.0, 0.0, uD);
	// projection homogene : le decoupage par OpenGL est correct meme quand une partie du modele passe derriere la camera (plan proche c.z = 1)
	vec2 k2 = 2.0 * uScale / uOut;
	float cx = k2.x * (uO.x * c.z + c.x * uD * uZoom) - c.z;
	float cy = c.z - k2.y * (uO.y * c.z + c.y * uD * uZoom);
	float cz = (16384.0 + 1.0) / (16384.0 - 1.0) * c.z - 2.0 * 16384.0 / (16384.0 - 1.0);
	gl_Position = vec4(cx, cy, cz, c.z);
}
)";

static const char* kHdGpuCoreObjFragment = R"(#version 330 core
in vec2 vUV;
in vec3 vN;
in vec3 vPc;
uniform sampler2D uTexM;
uniform int uHasTex;
uniform vec3 uColor;
uniform int uLightOn;   // H2 : 1 = eclaire par les lumieres de la scene
uniform vec3 uLAmb;
uniform vec4 uLP0, uLP1, uLP2, uLP3, uLP4, uLP5, uLP6, uLP7;   // xyz position (ponctuelle) ou vecteur vers la lumiere (soleil), w = type (0 aucune, 2 ponctuelle, 3 soleil)
uniform vec4 uLC0, uLC1, uLC2, uLC3, uLC4, uLC5, uLC6, uLC7;   // rgb = couleur x intensite, w = portee
uniform vec2 uSize;     // taille de la scene (pixels NES)
uniform vec3 uCam;      // position de la camera (axes centres sur la scene)
uniform int uShOn;      // H3 : ombres des sprites (meme masque et meme methode que les plans, G6c)
uniform sampler2D uShTex;
uniform vec2 uShBand;
uniform vec4 uShL;
uniform vec4 uShP;
uniform vec3 uShCol;
layout(location = 0) out vec4 oColor;
vec3 LightOne(vec4 lp, vec4 lc, vec3 P, vec3 N)
{
	if(lp.w < 1.5) return vec3(0.0);
	if(lp.w > 2.5) return lc.rgb * max(dot(N, lp.xyz), 0.0);   // soleil
	vec3 d = lp.xyz - P;
	float dist = length(d);
	float k = clamp(1.0 - dist / max(lc.w, 1.0), 0.0, 1.0);
	return lc.rgb * max(dot(N, d / max(dist, 0.001)), 0.0) * k * k;
}
void main()
{
	vec4 t = uHasTex != 0 ? texture(uTexM, vUV) : vec4(1.0);
	if(t.a < 0.5) discard;
	vec3 col = t.rgb / t.a * uColor;   // texture premultipliee -> couleur opaque
	if(uLightOn != 0) {
		vec3 N = normalize(vN);
		N = faceforward(N, vPc - uCam, N);                 // modele a deux faces : la normale regarde vers la camera
		vec3 P = vPc + vec3(uSize * 0.5, 0.0);               // meme repere que les lumieres des plans
		vec3 lit = uLAmb + LightOne(uLP0, uLC0, P, N) + LightOne(uLP1, uLC1, P, N) + LightOne(uLP2, uLC2, P, N) + LightOne(uLP3, uLC3, P, N)
			+ LightOne(uLP4, uLC4, P, N) + LightOne(uLP5, uLC5, P, N) + LightOne(uLP6, uLC6, P, N) + LightOne(uLP7, uLC7, P, N);
		col = min(col * lit, vec3(1.0));
	}
	if(uShOn != 0) {   // H3 : le rayon du point vers la lumiere coupe le plan des sprites ; le masque y dit si un sprite s'y trouve
		vec3 P = vPc + vec3(uSize * 0.5, 0.0);
		vec3 dv = uShL.w > 2.5 ? uShL.xyz : (uShL.xyz - P);
		if(dv.z < -0.0001) {
			float tt = (uShP.z - P.z) / dv.z;
			if(tt > 0.0 && (uShL.w > 2.5 || tt < 1.0)) {
				vec3 S = P + dv * tt;
				vec2 c = vec2((S.x - uSize.x * 0.5) / (uSize.x * uShP.w) + 0.5, (S.y - uSize.y * 0.5) / (uSize.y * uShP.w) + 0.5);
				vec2 st = vec2(uShP.y / (uSize.x * uShP.w), uShP.y / (uSize.y * uShP.w));
				vec2 o[5] = vec2[5](vec2(0.0), vec2(st.x, 0.0), vec2(-st.x, 0.0), vec2(0.0, st.y), vec2(0.0, -st.y));
				float a = 0.0;
				for(int i = 0; i < 5; i++) {
					vec2 q = c + o[i];
					float vb = (q.y - uShBand.x) / max(uShBand.y, 0.0001);
					if(q.x >= 0.0 && q.x <= 1.0 && vb >= 0.0 && vb <= 1.0) a += textureLod(uShTex, vec2(q.x, vb), 0.0).a;
				}
				a = uShP.y > 0.0 ? a * 0.2 : textureLod(uShTex, vec2(clamp(c.x, 0.0, 1.0), clamp((c.y - uShBand.x) / max(uShBand.y, 0.0001), 0.0, 1.0)), 0.0).a * ((c.x >= 0.0 && c.x <= 1.0 && c.y >= uShBand.x && c.y <= uShBand.x + uShBand.y) ? 1.0 : 0.0);
				col = mix(col, uShCol, clamp(a * uShP.x, 0.0, 1.0));
			}
		}
	}
	oColor = vec4(col, 1.0);
}
)";

bool HdGpuCore::BuildObjProgram()
{
	std::string err;
	GLuint v = HdGpuCoreCompile(GL_VERTEX_SHADER, kHdGpuCoreObjVertex, err);
	if(!v) { MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint f = HdGpuCoreCompile(GL_FRAGMENT_SHADER, kHdGpuCoreObjFragment, err);
	if(!f) { G.DeleteShader(v); MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint p = G.CreateProgram();
	G.AttachShader(p, v); G.AttachShader(p, f);
	G.LinkProgram(p);
	G.DeleteShader(v); G.DeleteShader(f);
	GLint ok = 0;
	G.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if(!ok) {
		char log[1024] = {};
		G.GetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
		MessageManager::Log(std::string("[HD-GPU-CORE] programme des objets 3D : ") + log);
		return false;
	}
	_progObj = p;
	return true;
}

// G3b2 : texture de mur envoyee une fois (REPEAT, mipmaps). Renvoie 0 si pas de donnees.
unsigned int HdGpuCore::WallTexture(const uint32_t* pix, uint32_t w, uint32_t h)
{
	if(!pix || w == 0 || h == 0) return 0;
	const size_t n = (size_t)w * h;
	const uint32_t sig = pix[0] ^ (pix[n / 2] * 31u) ^ (pix[n - 1] * 131u);
	for(WallTex& e : _wallTex) {
		if(e.Pix == pix && e.W == w && e.H == h && e.Sig == sig) return e.Tex;
	}
	// meme adresse mais contenu ou taille differents (pack recharge) : on remplace
	for(WallTex& e : _wallTex) {
		if(e.Pix == pix) {
			G.BindTexture(GL_TEXTURE_2D, e.Tex);
			G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
			G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pix);
			G.GenerateMipmap(GL_TEXTURE_2D);
			e.W = w; e.H = h; e.Sig = sig;
			return e.Tex;
		}
	}
	WallTex e = { pix, w, h, sig, 0 };
	G.GenTextures(1, &e.Tex);
	G.BindTexture(GL_TEXTURE_2D, e.Tex);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pix);
	G.GenerateMipmap(GL_TEXTURE_2D);
	_wallTex.push_back(e);
	return e.Tex;
}

// Fond <background> du diorama : texture du PNG (sans mipmaps), repetee selon tileX / tileY. Renvoie 0 si pas de donnees.
unsigned int HdGpuCore::BgTexture(const uint32_t* pix, uint32_t w, uint32_t h, bool tileX, bool tileY)
{
	if(!pix || w == 0 || h == 0) return 0;
	const size_t n = (size_t)w * h;
	const uint32_t sig = pix[0] ^ (pix[n / 2] * 31u) ^ (pix[n - 1] * 131u);
	BgTex* found = nullptr;
	for(BgTex& e : _bgTex) { if(e.Pix == pix) { found = &e; break; } }
	if(!found) {
		if(_bgTex.size() >= 96) { for(BgTex& e : _bgTex) G.DeleteTextures(1, &e.Tex); _bgTex.clear(); } // animations : on repart de zero
		BgTex e = { pix, 0, 0, 0, 0 };
		G.GenTextures(1, &e.Tex);
		_bgTex.push_back(e);
		found = &_bgTex.back();
	}
	G.BindTexture(GL_TEXTURE_2D, found->Tex);
	if(found->W != w || found->H != h || found->Sig != sig) {
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
		G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pix);
		found->W = w; found->H = h; found->Sig = sig;
	}
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, tileX ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, tileY ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	return found->Tex;
}

// G3f : passe de moyenne du surechantillonnage. Triangle plein ecran ; chaque pixel final = moyenne du bloc Aa x Aa de l'image
// intermediaire (donnees premultipliees : la moyenne de l'alpha et des couleurs est exacte).
static const char* kHdGpuCoreResolveVertex = R"(#version 330 core
void main()
{
	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kHdGpuCoreResolveFragment = R"(#version 330 core
uniform sampler2D uSrc;
uniform int uAa;
layout(location = 0) out vec4 oColor;
void main()
{
	ivec2 b = ivec2(gl_FragCoord.xy) * uAa;
	vec4 sum = vec4(0.0);
	for(int j = 0; j < uAa; j++) {
		for(int i = 0; i < uAa; i++) {
			sum += texelFetch(uSrc, b + ivec2(i, j), 0);
		}
	}
	oColor = sum / float(uAa * uAa);
}
)";

static const char* kHdGpuCoreSkyFragment = R"(#version 330 core
uniform int uMode;      // 1 degrade, 2 image repetee, 3 panorama, 4 cubemap
uniform vec3 uTop, uBottom;
uniform sampler2D uSky;
uniform vec2 uOut;      // taille de l'image dessinee (pixels, aa compris)
uniform vec2 uOff;      // decalage (pixels de l'image dessinee)
uniform vec2 uTexSize;
uniform vec3 uRt0, uRt1, uRt2; // rotation de la camera de la scene
uniform float uD, uZoom, uPxNes; // focale (pixels NES), zoom, pixels dessines par pixel NES
uniform vec2 uO;        // centre de la vue (pixels NES)
uniform vec2 uRot;      // decalage de yaw / pitch du fond (radians)
uniform float uCamPitch; // G5c : decalage de pitch dans le repere de la camera (radians), axe horizontal de l'ecran
uniform int uCubeLayout;
uniform vec4 uGlow;     // G6a : halo (mode 5) : x, y centre (pixels dessines, origine en haut a gauche), z rayon, w intensite
layout(location = 0) out vec4 oColor;
vec3 SkyDir()
{
	vec2 off = (vec2(gl_FragCoord.x, uOut.y - gl_FragCoord.y) / uPxNes - uO) / uZoom;
	vec3 cam = vec3(off, uD);
	float ct = cos(uCamPitch), st = sin(uCamPitch);
	cam = vec3(cam.x, cam.y * ct - cam.z * st, cam.y * st + cam.z * ct);
	vec3 d = cam.x * uRt0 + cam.y * uRt1 + cam.z * uRt2;   // monde = transposee de la rotation de la camera
	float cy = cos(uRot.x), sy = sin(uRot.x);
	d = vec3(d.x * cy + d.z * sy, d.y, -d.x * sy + d.z * cy);
	float cp = cos(uRot.y), sp = sin(uRot.y);
	return vec3(d.x, d.y * cp - d.z * sp, d.y * sp + d.z * cp);
}
vec4 CubeSample(vec3 d)
{
	// monde : x a droite, y vers le bas, z devant ; faces 0 gauche, 1 devant, 2 droite, 3 derriere, 4 haut (-y), 5 bas (+y)
	vec3 a = abs(d);
	int face; float s, t;
	if(a.z >= a.x && a.z >= a.y) {
		if(d.z > 0.0) { face = 1; s = d.x / a.z; t = d.y / a.z; }
		else { face = 3; s = -d.x / a.z; t = d.y / a.z; }
	} else if(a.x >= a.y) {
		if(d.x > 0.0) { face = 2; s = -d.z / a.x; t = d.y / a.x; }
		else { face = 0; s = d.z / a.x; t = d.y / a.x; }
	} else {
		if(d.y < 0.0) { face = 4; s = d.x / a.y; t = d.z / a.y; }
		else { face = 5; s = d.x / a.y; t = -d.z / a.y; }
	}
	vec2 uv = vec2(s, t) * 0.5 + 0.5;
	ivec2 cell, grid;
	if(uCubeLayout == 1 || uCubeLayout == 2) {
		grid = uCubeLayout == 1 ? ivec2(4, 3) : ivec2(3, 4);
		if(face == 0) cell = ivec2(0, 1);
		else if(face == 1) cell = ivec2(1, 1);
		else if(face == 2) cell = ivec2(2, 1);
		else if(face == 3) cell = uCubeLayout == 1 ? ivec2(3, 1) : ivec2(1, 3);
		else if(face == 4) cell = ivec2(1, 0);
		else cell = ivec2(1, 2);
	} else if(uCubeLayout == 3) { grid = ivec2(6, 1); cell = ivec2(face, 0); }
	else { grid = ivec2(1, 6); cell = ivec2(0, face); }
	vec2 ts = vec2(textureSize(uSky, 0));
	vec2 cs = ts / vec2(grid);
	vec2 px = clamp(uv * cs, vec2(0.5), cs - 0.5);
	return textureLod(uSky, (vec2(cell) * cs + px) / ts, 0.0);
}
void main()
{
	if(uMode == 5) {
		vec2 p = vec2(gl_FragCoord.x, uOut.y - gl_FragCoord.y);
		float k = clamp(1.0 - length(p - uGlow.xy) / max(uGlow.z, 1.0), 0.0, 1.0);
		oColor = vec4(uTop * uGlow.w * k * k, 0.0);   // ajout (blend ONE, ONE)
	} else if(uMode == 1) {
		float t = clamp(1.0 - gl_FragCoord.y / uOut.y, 0.0, 1.0);   // 0 en haut de l'image
		oColor = vec4(mix(uTop, uBottom, t), 1.0);
	} else if(uMode == 3) {
		vec3 d = normalize(SkyDir());
		float lon = atan(d.x, d.z);          // 0 devant, positif vers la droite
		float lat = asin(clamp(-d.y, -1.0, 1.0));   // positif vers le haut (y est vers le bas)
		oColor = vec4(textureLod(uSky, vec2(lon / 6.2831853 + 0.5, 0.5 - lat / 3.1415927), 0.0).rgb, 1.0);
	} else if(uMode == 4) {
		oColor = vec4(CubeSample(SkyDir()).rgb, 1.0);
	} else {
		vec2 p = vec2(gl_FragCoord.x, uOut.y - gl_FragCoord.y) + uOff;
		oColor = vec4(texture(uSky, p / uTexSize).rgb, 1.0);
	}
}
)";

bool HdGpuCore::BuildSkyProgram()
{
	std::string err;
	GLuint v = HdGpuCoreCompile(GL_VERTEX_SHADER, kHdGpuCoreResolveVertex, err);
	if(!v) { MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint f = HdGpuCoreCompile(GL_FRAGMENT_SHADER, kHdGpuCoreSkyFragment, err);
	if(!f) { G.DeleteShader(v); MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint p = G.CreateProgram();
	G.AttachShader(p, v); G.AttachShader(p, f);
	G.LinkProgram(p);
	G.DeleteShader(v); G.DeleteShader(f);
	GLint ok = 0;
	G.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if(!ok) {
		char log[1024] = {};
		G.GetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
		MessageManager::Log(std::string("[HD-GPU-CORE] programme de la skybox : ") + log);
		return false;
	}
	_progSky = p;
	return true;
}

bool HdGpuCore::BuildResolveProgram()
{
	std::string err;
	GLuint v = HdGpuCoreCompile(GL_VERTEX_SHADER, kHdGpuCoreResolveVertex, err);
	if(!v) { MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint f = HdGpuCoreCompile(GL_FRAGMENT_SHADER, kHdGpuCoreResolveFragment, err);
	if(!f) { G.DeleteShader(v); MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint p = G.CreateProgram();
	G.AttachShader(p, v); G.AttachShader(p, f);
	G.LinkProgram(p);
	G.DeleteShader(v); G.DeleteShader(f);
	GLint ok = 0;
	G.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if(!ok) {
		char log[1024] = {};
		G.GetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
		MessageManager::Log(std::string("[HD-GPU-CORE] programme de moyenne (aa) : ") + log);
		return false;
	}
	_progResolve = p;
	return true;
}

bool HdGpuCore::EnsureAaTarget(uint32_t w, uint32_t h)
{
	if(w == _aaW && h == _aaH && _aaFbo) return true;
	if(_aaFbo) { G.DeleteFramebuffers(1, &_aaFbo); G.DeleteTextures(1, &_aaTex); _aaFbo = 0; _aaTex = 0; }
	_aaW = w; _aaH = h;
	G.GenTextures(1, &_aaTex); G.BindTexture(GL_TEXTURE_2D, _aaTex);
	G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	G.GenFramebuffers(1, &_aaFbo); G.BindFramebuffer(GL_FRAMEBUFFER, _aaFbo);
	G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _aaTex, 0);
	const bool complete = G.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	return complete;
}

bool HdGpuCore::BuildSceneProgram()
{
	std::string err;
	GLuint v = HdGpuCoreCompile(GL_VERTEX_SHADER, kHdGpuCoreSceneVertex, err);
	if(!v) { MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint f = HdGpuCoreCompile(GL_FRAGMENT_SHADER, kHdGpuCoreSceneFragment, err);
	if(!f) { G.DeleteShader(v); MessageManager::Log("[HD-GPU-CORE] " + err); return false; }
	GLuint p = G.CreateProgram();
	G.AttachShader(p, v); G.AttachShader(p, f);
	G.LinkProgram(p);
	G.DeleteShader(v); G.DeleteShader(f);
	GLint ok = 0;
	G.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if(!ok) {
		char log[1024] = {};
		G.GetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
		MessageManager::Log(std::string("[HD-GPU-CORE] programme de la scene : ") + log);
		return false;
	}
	_progScene = p;
	return true;
}

bool HdGpuCore::SubmitScene(const uint32_t* l0, const uint32_t* l1, const uint32_t* l2, const uint32_t* l3, uint32_t w, uint32_t h, const HdDioramaScene& sc)
{
	std::lock_guard<std::mutex> lk(_lock);
	if(!_ok || _sceneProgFailed || w == 0 || h == 0) return false;
	if(!MakeCurrent()) return false;
	if(!_progScene && !BuildSceneProgram()) { _sceneProgFailed = true; return false; }
	// Widescreen : l'image de sortie est plus large que les couches (OutPad colonnes NES de chaque cote) ; la scene garde sa taille et son centre
	const uint32_t padHd = sc.OutPad > 0.0f ? (uint32_t)std::lround(sc.OutPad * (sc.Scale > 0.0f ? sc.Scale : 1.0f)) : 0u;
	const uint32_t ow = w + 2 * padHd;
	const float oPadNes = padHd > 0 ? (float)padHd / (sc.Scale > 0.0f ? sc.Scale : 1.0f) : 0.0f;
	if(!EnsureTarget(ow, h)) { ReleaseCurrent(); _ok = false; MessageManager::Log("[HD-GPU-CORE] Image cachee impossible : GPU desactive."); return false; }

	if(!_layerTex[0] || w != _layerW || h != _layerH) {
		for(int k = 0; k < 4; k++) {
			if(_layerTex[k]) G.DeleteTextures(1, &_layerTex[k]);
			G.GenTextures(1, &_layerTex[k]);
			G.BindTexture(GL_TEXTURE_2D, _layerTex[k]);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		_layerW = w; _layerH = h;
		for(int k = 0; k < 4; k++) _layerStore[k] = false;
	}
	const auto tA = std::chrono::steady_clock::now();
	const uint32_t* src[4] = { l0, l1, l2, l3 };
	G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	for(int k = 0; k < 4; k++) {
		G.ActiveTexture(GL_TEXTURE0 + k);
		G.BindTexture(GL_TEXTURE_2D, _layerTex[k]);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		if(k == 0 && sc.L0Wide && sc.L0WideW > 0 && sc.L0WideH > 0 && !sc.L0Uniform && !sc.Ext[0].Active) {
			_layerStore[k] = false; // L0 elargie envoyee a la place (plus bas) : la texture normale de L0 n'est pas utilisee
		} else if(k == 0 && sc.L0Uniform) {
			// G3h : fond d'une seule couleur, texture 1 x 1
			G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, src[k]);
			_layerStore[k] = false;
		} else if(sc.RowDirty[k] && _layerStore[k] && sc.RowScale > 0) {
			// G3h : seulement les lignes NES marquees, par suites de lignes
			const uint8_t* dirty = sc.RowDirty[k];
			uint32_t y = sc.RowY0;
			while(y < 240) {
				if(!dirty[y]) { y++; continue; }
				const uint32_t a = y;
				while(y < 240 && dirty[y]) y++;
				const uint32_t hdY0 = (a - sc.RowY0) * sc.RowScale;
				if(hdY0 >= h) break;
				uint32_t rows = (y - a) * sc.RowScale;
				if(hdY0 + rows > h) rows = h - hdY0;
				G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)hdY0, (GLsizei)w, (GLsizei)rows, GL_BGRA, GL_UNSIGNED_BYTE, src[k] + (size_t)hdY0 * w);
			}
		} else {
			G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA, GL_UNSIGNED_BYTE, src[k]);
			_layerStore[k] = true;
		}
	}
	G.ActiveTexture(GL_TEXTURE0);
	// Widescreen : L0 elargie (fonds <background> prolonges dans les bandes), envoyee entiere
	const bool l0WideOn = sc.L0Wide && sc.L0WideW > 0 && sc.L0WideH > 0 && !sc.L0Uniform;
	if(l0WideOn) {
		if(!_l0WideTex) {
			G.GenTextures(1, &_l0WideTex);
			G.BindTexture(GL_TEXTURE_2D, _l0WideTex);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		G.BindTexture(GL_TEXTURE_2D, _l0WideTex);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)sc.L0WideW, (GLsizei)sc.L0WideH, 0, GL_BGRA, GL_UNSIGNED_BYTE, sc.L0Wide);
	}
	// G3c : bande de la couche fixe (seulement ses lignes), sans filtrage
	const bool hasFixed = sc.Fixed && sc.FixedRows > 0 && sc.FixedY0 + sc.FixedRows <= h;
	if(hasFixed) {
		if(!_fixedTex) {
			G.GenTextures(1, &_fixedTex);
			G.BindTexture(GL_TEXTURE_2D, _fixedTex);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		G.BindTexture(GL_TEXTURE_2D, _fixedTex);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)sc.FixedRows, 0, GL_BGRA, GL_UNSIGNED_BYTE, sc.Fixed + (size_t)sc.FixedY0 * w);
	}
	// G3e : bandes des couches supplementaires (seulement leurs lignes), filtrage lineaire comme les couches de base
	bool hasExtra[HdDioMaxGroups] = {};
	for(int g = 0; g < HdDioMaxGroups; g++) {
		hasExtra[g] = sc.ExtraImg[g] && sc.ExtraRows[g] > 0 && sc.ExtraY0[g] + sc.ExtraRows[g] <= h;
		if(!hasExtra[g]) continue;
		if(!_extraTex[g]) {
			G.GenTextures(1, &_extraTex[g]);
			G.BindTexture(GL_TEXTURE_2D, _extraTex[g]);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		G.BindTexture(GL_TEXTURE_2D, _extraTex[g]);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)sc.ExtraRows[g], 0, GL_BGRA, GL_UNSIGNED_BYTE, sc.ExtraImg[g] + (size_t)sc.ExtraY0[g] * w);
	}
	// G3b2 : textures de murs (une fois), pour les 8 couches possibles
	unsigned int wallTop[4 + HdDioMaxGroups] = {}, wallSide[4 + HdDioMaxGroups] = {};
	for(int l = 0; l < 4 + HdDioMaxGroups; l++) {
		wallTop[l] = WallTexture(sc.Wall[l].TopPix, sc.Wall[l].TopW, sc.Wall[l].TopH);
		wallSide[l] = WallTexture(sc.Wall[l].SidePix, sc.Wall[l].SideW, sc.Wall[l].SideH);
	}
	G.ActiveTexture(GL_TEXTURE0);
	const auto tB = std::chrono::steady_clock::now();

	const float kDeg = 0.017453292f;
	const float ca = std::cos(sc.Yaw * kDeg), sa = std::sin(sc.Yaw * kDeg);
	const float cb = std::cos(sc.Pitch * kDeg), sb = std::sin(sc.Pitch * kDeg);

	// G3f : surechantillonnage. Les coordonnees du sommet sont normalisees : seul le cadre (viewport) change de taille.
	int aa = sc.Aa < 1 ? 1 : (sc.Aa > 4 ? 4 : sc.Aa);
	if(aa > 1) {
		if(!_progResolve && !_resolveProgFailed && !BuildResolveProgram()) _resolveProgFailed = true;
		if(_resolveProgFailed || !EnsureAaTarget(ow * (uint32_t)aa, h * (uint32_t)aa)) {
			static int aaLogged = 0;
			if(aaLogged++ < 2) MessageManager::Log("[HD-GPU-CORE] aa : surechantillonnage impossible, rendu normal.");
			_resolveProgFailed = true;
			aa = 1;
		}
	}
	G.BindFramebuffer(GL_FRAMEBUFFER, aa > 1 ? _aaFbo : _fbo);
	G.Viewport(0, 0, (GLsizei)(ow * (uint32_t)aa), (GLsizei)(h * (uint32_t)aa));
	G.Disable(GL_SCISSOR_TEST); G.Disable(GL_DEPTH_TEST); G.Disable(GL_CULL_FACE);
	const GLfloat clear[4] = { ((sc.Clear >> 16) & 0xFF) / 255.0f, ((sc.Clear >> 8) & 0xFF) / 255.0f, (sc.Clear & 0xFF) / 255.0f, 1.0f };
	G.ClearBufferfv(GL_COLOR, 0, clear);
	// G5 : skybox derriere les plans (remplace la couleur d'effacement)
	if(sc.Sky.Mode != 0 && !_skyProgFailed) {
		if(!_progSky && !BuildSkyProgram()) _skyProgFailed = true;
		const unsigned int skyTex = (sc.Sky.Mode >= 2) ? WallTexture(sc.Sky.Pix, sc.Sky.W, sc.Sky.H) : 0;
		if(!_skyProgFailed && (sc.Sky.Mode == 1 || skyTex)) {
			G.Disable(GL_BLEND);
			G.UseProgram(_progSky);
			auto US = [&](const char* n) { return G.GetUniformLocation(_progSky, n); };
			G.Uniform1i(US("uMode"), sc.Sky.Mode);
			G.Uniform3f(US("uTop"), sc.Sky.Top[0], sc.Sky.Top[1], sc.Sky.Top[2]);
			G.Uniform3f(US("uBottom"), sc.Sky.Bottom[0], sc.Sky.Bottom[1], sc.Sky.Bottom[2]);
			G.Uniform2f(US("uOut"), (float)(ow * (uint32_t)aa), (float)(h * (uint32_t)aa));
			G.Uniform2f(US("uOff"), sc.Sky.OffX * sc.Scale * (float)aa, sc.Sky.OffY * sc.Scale * (float)aa);
			G.Uniform2f(US("uTexSize"), (float)sc.Sky.W * (float)aa, (float)sc.Sky.H * (float)aa);
			G.Uniform3f(US("uRt0"), ca, 0.0f, sa);
			G.Uniform3f(US("uRt1"), sb * sa, cb, -sb * ca);
			G.Uniform3f(US("uRt2"), -cb * sa, sb, cb * ca);
			G.Uniform1f(US("uD"), sc.D);
			G.Uniform1f(US("uZoom"), sc.Zoom > 0.05f ? sc.Zoom : 1.0f);
			G.Uniform1f(US("uPxNes"), sc.Scale * (float)aa);
			G.Uniform2f(US("uO"), sc.W * 0.5f + oPadNes, sc.H * 0.5f);
			G.Uniform2f(US("uRot"), sc.Sky.RotYaw * kDeg, sc.Sky.RotPitch * kDeg);
			G.Uniform1f(US("uCamPitch"), sc.Sky.CamPitch * kDeg);
			G.Uniform1i(US("uCubeLayout"), sc.Sky.CubeLayout);
			if(skyTex) {
				G.ActiveTexture(GL_TEXTURE0);
				G.BindTexture(GL_TEXTURE_2D, skyTex);
				G.Uniform1i(US("uSky"), 0);
			}
			G.BindVertexArray(_vao);
			G.DrawArrays(GL_TRIANGLES, 0, 3);
		}
	}
	G.Enable(GL_BLEND); G.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	G.UseProgram(_progScene);
	auto U = [&](const char* n) { return G.GetUniformLocation(_progScene, n); };
	G.Uniform2f(U("uSize"), sc.W, sc.H);
	G.Uniform1f(U("uD"), sc.D);
	// G6a : lumieres de scene
	const GLint locLightOn = U("uLightOn");
	G.Uniform1i(locLightOn, sc.Lights.Active ? 1 : 0);
	if(sc.Lights.Active) {
		G.Uniform3f(U("uLAmb"), sc.Lights.Ambient[0], sc.Lights.Ambient[1], sc.Lights.Ambient[2]);
		static const char* lpn[8] = { "uLP0", "uLP1", "uLP2", "uLP3", "uLP4", "uLP5", "uLP6", "uLP7" };
		static const char* lcn[8] = { "uLC0", "uLC1", "uLC2", "uLC3", "uLC4", "uLC5", "uLC6", "uLC7" };
		for(int i = 0; i < 8; i++) {
			const HdDioramaLight& l = sc.Lights.L[i];
			G.Uniform4f(U(lpn[i]), l.Pos[0], l.Pos[1], l.Pos[2], (float)l.Type);
			G.Uniform4f(U(lcn[i]), l.Col[0], l.Col[1], l.Col[2], l.Radius);
		}
	}
	// G6d : cartes de normales (unite 4) ; une texture par tampon, filtre au plus proche
	const GLint locNrmOn = U("uNrmOn");
	bool nrmOk[2 + HdDioMaxGroups] = {};
	float nrmBand[2 + HdDioMaxGroups][2] = {};
	G.Uniform1i(locNrmOn, 0);
	if(sc.Normals.Active && sc.Lights.Active) {
		for(int i = 0; i < 2 + HdDioMaxGroups; i++) {
			if(!sc.Normals.Img[i]) continue;
			uint32_t y0 = sc.Normals.Y0[i], rows = sc.Normals.Rows[i];
			float b0 = (float)y0 / (float)h, b1 = (float)rows / (float)h;
			if(i >= 2) { // groupe : la bande du groupe, comme sa texture de couleurs
				if(!hasExtra[i - 2]) continue;
				y0 = sc.ExtraY0[i - 2]; rows = sc.ExtraRows[i - 2];
				b0 = 0.0f; b1 = 1.0f; // uv0.y d'un groupe est deja relatif a sa bande
			}
			if(rows == 0 || y0 + rows > h) continue;
			if(!_nrmTex[i]) {
				G.GenTextures(1, &_nrmTex[i]);
				G.BindTexture(GL_TEXTURE_2D, _nrmTex[i]);
				G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}
			G.ActiveTexture(GL_TEXTURE0 + 4);
			G.BindTexture(GL_TEXTURE_2D, _nrmTex[i]);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)rows, 0, GL_BGRA, GL_UNSIGNED_BYTE, sc.Normals.Img[i] + (size_t)y0 * w);
			G.ActiveTexture(GL_TEXTURE0);
			nrmOk[i] = true; nrmBand[i][0] = b0; nrmBand[i][1] = b1;
		}
		G.Uniform1i(U("uNrmTex"), 4);
		G.Uniform2f(U("uNrmP"), sc.Normals.Strength, sc.Normals.FlipY ? 1.0f : 0.0f);
	}
	// G6c : ombres des sprites : masque des silhouettes (unite 3) et lumiere qui projette
	const GLint locShOn = U("uShOn");
	bool shadowOn = false;
	float shObjL[4] = { 0, 0, 0, 0 }, shObjP[4] = { 0, 0, 0, 1 }, shObjBand[2] = { 0, 1 }, shObjCol[3] = { 0, 0, 0 }; // H3 : memes valeurs pour les objets 3D
	G.Uniform1i(locShOn, 0);
	if(sc.Shadow.Active && sc.Shadow.Img && sc.Shadow.Rows > 0 && sc.Shadow.Y0 + sc.Shadow.Rows <= h && sc.Lights.Active) {
		int found = -1, count = 0;
		for(int i = 0; i < 8; i++) {
			if(sc.Lights.L[i].Type != 2 && sc.Lights.L[i].Type != 3) continue;
			if(count == sc.Shadow.Light) { found = i; break; }
			count++;
		}
		if(found >= 0) {
			shadowOn = true;
			const HdDioramaLight& sl = sc.Lights.L[found];
			if(!_shadowTex) {
				G.GenTextures(1, &_shadowTex);
				G.BindTexture(GL_TEXTURE_2D, _shadowTex);
				G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			}
			G.ActiveTexture(GL_TEXTURE0 + 3);
			G.BindTexture(GL_TEXTURE_2D, _shadowTex);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)sc.Shadow.Rows, 0, GL_BGRA, GL_UNSIGNED_BYTE, sc.Shadow.Img + (size_t)sc.Shadow.Y0 * w);
			G.ActiveTexture(GL_TEXTURE0);
			G.Uniform1i(U("uShTex"), 3);
			G.Uniform2f(U("uShBand"), (float)sc.Shadow.Y0 / (float)h, (float)sc.Shadow.Rows / (float)h);
			G.Uniform4f(U("uShL"), sl.Pos[0], sl.Pos[1], sl.Pos[2], (float)sl.Type);
			const float zc = sc.PlaneZ[3];
			G.Uniform4f(U("uShP"), sc.Shadow.Opacity, sc.Shadow.Soft, zc, (sc.D + zc) / sc.D);
			G.Uniform3f(U("uShCol"), sc.Shadow.Color[0], sc.Shadow.Color[1], sc.Shadow.Color[2]);
			shObjL[0] = sl.Pos[0]; shObjL[1] = sl.Pos[1]; shObjL[2] = sl.Pos[2]; shObjL[3] = (float)sl.Type;
			shObjP[0] = sc.Shadow.Opacity; shObjP[1] = sc.Shadow.Soft; shObjP[2] = zc; shObjP[3] = (sc.D + zc) / sc.D;
			shObjBand[0] = (float)sc.Shadow.Y0 / (float)h; shObjBand[1] = (float)sc.Shadow.Rows / (float)h;
			shObjCol[0] = sc.Shadow.Color[0]; shObjCol[1] = sc.Shadow.Color[1]; shObjCol[2] = sc.Shadow.Color[2];
		}
	}
	G.Uniform2f(U("uO"), sc.W * 0.5f + oPadNes, sc.H * 0.5f);
	G.Uniform3f(U("uRt0"), ca, 0.0f, sa);
	G.Uniform3f(U("uRt1"), sb * sa, cb, -sb * ca);
	G.Uniform3f(U("uRt2"), -cb * sa, sb, cb * ca);
	G.Uniform1f(U("uScale"), sc.Scale);
	G.Uniform2f(U("uOut"), (float)ow, (float)h);
	G.Uniform1i(U("uTex"), 0);
	G.Uniform1i(U("uTopTex"), 1);
	G.Uniform1i(U("uSideTex"), 2);
	G.Uniform4f(U("uR"), 0.0f, 0.0f, 1.0f, 1.0f);
	G.BindVertexArray(_vao);
	// G3b1 : chaque couche = pile de tranches (la meme texture), de PlaneZ + Thick (fond) a PlaneZ (face avant), environ une
	// tranche par pixel NES de profondeur (128 intervalles au plus). Toutes les tranches de toutes les couches sont triees
	// du plus loin au plus pres ; a profondeur egale, la couche de plus petit numero d'abord (tri stable).
	struct Slice { float Z; int Layer; float InvN; float Depth; };
	Slice slices[(4 + HdDioMaxGroups) * 129 + 16 + HdDioMaxObjects]; // (+ objets 3D H1, couche 100 + numero) couches 0 a 3 = L0..L3, couches 4 a 7 = groupes <DioramaDepth> (G3e)
	int sliceCount = 0;
	for(int layer = 0; layer < 4 + HdDioMaxGroups; layer++) {
		if(layer >= 4 && !hasExtra[layer - 4]) continue;
		if(layer == 0 && sc.L0Uniform) continue; // G3h : fond d'une seule couleur = couleur d'effacement, plan inutile
		const float layerZ = layer < 4 ? sc.PlaneZ[layer] : sc.ExtraZ[layer - 4];
		const float layerThick = layer < 4 ? sc.Thick[layer] : sc.ExtraThick[layer - 4];
		const float th = layerThick > 0.0f ? layerThick : 0.0f;
		// G3h : pas plus de tranches que le decalage a l'ecran entre la face avant et le fond (en pixels dessines, marge 25 %) :
		// vue de face l'epaisseur est invisible, 1 intervalle suffit ; a fort angle on retrouve une tranche par pixel NES.
		const float tanSum = std::fabs(sa) / (std::max)(std::fabs(ca), 0.05f) + std::fabs(sb) / (std::max)(std::fabs(cb), 0.05f);
		const int need = (int)std::ceil(th * sc.Scale * (float)aa * tanSum * 1.25f) + 1;
		const int steps = th > 0.0f ? (std::min)((std::min)(128, (int)std::ceil(th)), (std::max)(1, need)) : 0;
		const float invN = 1.0f / (float)(steps + 1);
		for(int i = steps; i >= 0; i--) {
			const float depth = steps > 0 ? th * (float)i / (float)steps : 0.0f;
			slices[sliceCount++] = { layerZ + depth, layer, invN, depth };
		}
	}
	for(int bi = 0; bi < sc.BgCount && bi < 4; bi++) { // fonds de L0 dessines par le GPU : juste devant la face avant de L0
		slices[sliceCount++] = { sc.PlaneZ[0] - 0.001f * (float)(bi + 1), 300 + bi, 1.0f, 0.0f };
	}
	for(int oi = 0; oi < sc.Objects.Count && oi < HdDioMaxObjects; oi++) { // H1 : objets 3D, ranges avec les tranches selon leur profondeur
		slices[sliceCount++] = { sc.Objects.O[oi].Occlude ? 1.0e9f : sc.Objects.O[oi].Pos[2], 100 + oi, 1.0f, 0.0f };
	}
	// doc 53 : couches a plat (sol, murs) d'abord, du plus bas au plus haut ; puis les plans verticaux (sprites), du plus loin au plus pres
	auto flatSlice = [&](const Slice& s) {
		if(s.Layer >= 300) return s.Layer - 300 < sc.BgCount && sc.Bg[s.Layer - 300].Flat;
		return s.Layer >= 0 && s.Layer < 12 && ((sc.FlatMask >> s.Layer) & 1u) != 0;
	};
	std::stable_sort(slices, slices + sliceCount, [&](const Slice& a, const Slice& b) {
		const bool fa = flatSlice(a), fb = flatSlice(b);
		if(fa != fb) return fa;
		return fa ? a.Z < b.Z : a.Z > b.Z;
	});
	// H1 : dessin d'un objet 3D (programme de maillage, profondeur propre a l'image), puis retour a l'etat des tranches
	bool objDepthOn = false;
	bool occlOn = false;   // occlude=on : les tranches suivantes sont testees contre la profondeur des objets
	auto drawObject = [&](const HdDioramaObject& o) {
		if(!o.Verts || o.VertCount < 3 || _objProgFailed) return;
		if(!_progObj && !BuildObjProgram()) { _objProgFailed = true; return; }
		if(!objDepthOn) {
			const uint32_t tw = ow * (uint32_t)aa, th = h * (uint32_t)aa;
			if(!_objDepthTex) G.GenTextures(1, &_objDepthTex);
			G.ActiveTexture(GL_TEXTURE0);
			G.BindTexture(GL_TEXTURE_2D, _objDepthTex);
			if(_objDepthW != tw || _objDepthH != th) {
				G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				G.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, (GLsizei)tw, (GLsizei)th, 0, 0x1902 /* GL_DEPTH_COMPONENT */, 0x1405 /* GL_UNSIGNED_INT */, nullptr);
				_objDepthW = tw; _objDepthH = th;
			}
			G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _objDepthTex, 0);
			if(G.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
				_objProgFailed = true;
				MessageManager::Log("[HD-GPU-CORE] objets 3D : tampon de profondeur impossible, objets ignores.");
				return;
			}
			const GLfloat one = 1.0f;
			G.ClearBufferfv(GL_DEPTH, 0, &one);
			objDepthOn = true;
		}
		// donnees des sommets en texture (une fois par modele)
		ObjMesh* mesh = nullptr;
		for(ObjMesh& e : _objMeshes) { if(e.Key == o.Verts && e.N == o.VertCount) { mesh = &e; break; } }
		if(!mesh) {
			const uint32_t rows = (o.VertCount + 255) / 256;
			std::vector<float> buf((size_t)768 * rows * 4, 0.0f);
			for(uint32_t i = 0; i < o.VertCount; i++) {
				float* d = &buf[((size_t)(i / 256) * 768 + (size_t)(i % 256) * 3) * 4];
				const float* s8 = o.Verts + (size_t)i * 8;
				d[0] = s8[0]; d[1] = s8[1]; d[2] = s8[2];
				d[4] = s8[3]; d[5] = s8[4]; d[6] = s8[5];
				d[8] = s8[6]; d[9] = s8[7];
			}
			ObjMesh e = { o.Verts, o.VertCount, 0 };
			G.GenTextures(1, &e.Tex);
			G.ActiveTexture(GL_TEXTURE0);
			G.BindTexture(GL_TEXTURE_2D, e.Tex);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 768, (GLsizei)rows, 0, GL_RGBA, GL_FLOAT, buf.data());
			_objMeshes.push_back(e);
			mesh = &_objMeshes.back();
		}
		G.ActiveTexture(GL_TEXTURE0);
		const unsigned int ot = WallTexture(o.TexPix, o.TexW, o.TexH);
		G.UseProgram(_progObj);
		auto UO = [&](const char* n) { return G.GetUniformLocation(_progObj, n); };
		G.ActiveTexture(GL_TEXTURE0 + 5);
		G.BindTexture(GL_TEXTURE_2D, mesh->Tex);
		G.Uniform1i(UO("uMesh"), 5);
		G.ActiveTexture(GL_TEXTURE0 + 6);
		G.BindTexture(GL_TEXTURE_2D, ot);
		G.Uniform1i(UO("uTexM"), 6);
		G.Uniform1i(UO("uHasTex"), ot ? 1 : 0);
		G.Uniform3f(UO("uColor"), o.Color[0], o.Color[1], o.Color[2]);
		G.Uniform3f(UO("uMr0"), o.Rot[0], o.Rot[1], o.Rot[2]);
		G.Uniform3f(UO("uMr1"), o.Rot[3], o.Rot[4], o.Rot[5]);
		G.Uniform3f(UO("uMr2"), o.Rot[6], o.Rot[7], o.Rot[8]);
		const float kz = (sc.D + o.Pos[2] + o.RefZ) / sc.D;   // refz : le modele est a l'echelle de la vue 2D a cette profondeur derriere sa face avant
		G.Uniform1f(UO("uK"), kz);
		G.Uniform3f(UO("uAnchor"), (o.Pos[0] - sc.W * 0.5f) * kz, (o.Pos[1] - sc.H * 0.5f) * kz, o.Pos[2]);
		G.Uniform1f(UO("uMScale"), o.Scale);
		G.Uniform3f(UO("uWin"), o.WinX0, o.WinX1, o.WinOn ? 1.0f : 0.0f);
		{ // H2 : lumieres de la scene
			const bool lit = o.Lit && sc.Lights.Active;
			G.Uniform1i(UO("uLightOn"), lit ? 1 : 0);
			if(lit) {
				G.Uniform3f(UO("uLAmb"), sc.Lights.Ambient[0], sc.Lights.Ambient[1], sc.Lights.Ambient[2]);
				static const char* lpn[8] = { "uLP0", "uLP1", "uLP2", "uLP3", "uLP4", "uLP5", "uLP6", "uLP7" };
				static const char* lcn[8] = { "uLC0", "uLC1", "uLC2", "uLC3", "uLC4", "uLC5", "uLC6", "uLC7" };
				for(int li = 0; li < 8; li++) {
					const HdDioramaLight& l = sc.Lights.L[li];
					G.Uniform4f(UO(lpn[li]), l.Pos[0], l.Pos[1], l.Pos[2], (float)l.Type);
					G.Uniform4f(UO(lcn[li]), l.Col[0], l.Col[1], l.Col[2], l.Radius);
				}
				G.Uniform2f(UO("uSize"), sc.W, sc.H);
				G.Uniform3f(UO("uCam"), -sc.D * (-cb * sa), -sc.D * sb, -sc.D * (cb * ca));   // -D * ligne 3 de la rotation (camera a (0, 0, -D) dans le repere de la vue)
			}
		}
		{ // H3 : ombres des sprites sur l'objet (masque en unite 3, deja envoye pour les plans)
			const bool shOn = shadowOn && o.Shadow;
			G.Uniform1i(UO("uShOn"), shOn ? 1 : 0);
			if(shOn) {
				G.Uniform1i(UO("uShTex"), 3);
				G.Uniform2f(UO("uShBand"), shObjBand[0], shObjBand[1]);
				G.Uniform4f(UO("uShL"), shObjL[0], shObjL[1], shObjL[2], shObjL[3]);
				G.Uniform4f(UO("uShP"), shObjP[0], shObjP[1], shObjP[2], shObjP[3]);
				G.Uniform3f(UO("uShCol"), shObjCol[0], shObjCol[1], shObjCol[2]);
				G.Uniform2f(UO("uSize"), sc.W, sc.H);
			}
		}
		G.Uniform2f(UO("uO"), sc.W * 0.5f + oPadNes, sc.H * 0.5f);
		G.Uniform3f(UO("uRt0"), ca, 0.0f, sa);
		G.Uniform3f(UO("uRt1"), sb * sa, cb, -sb * ca);
		G.Uniform3f(UO("uRt2"), -cb * sa, sb, cb * ca);
		G.Uniform1f(UO("uD"), sc.D);
		G.Uniform1f(UO("uZoom"), sc.Zoom > 0.05f ? sc.Zoom : 1.0f);
		G.Uniform1f(UO("uScale"), sc.Scale);
		G.Uniform2f(UO("uOut"), (float)ow, (float)h);
		static int objDrawLog = 0;
		if(objDrawLog++ < 3) MessageManager::Log("[HD-GPU-CORE] objet 3D dessine : " + std::to_string(o.VertCount) + " sommets, position " + std::to_string(o.Pos[0]) + "," + std::to_string(o.Pos[1]) + "," + std::to_string(o.Pos[2]) + ", echelle " + std::to_string(o.Scale) + (ot ? ", texture" : ", sans texture"));
		G.Enable(GL_DEPTH_TEST);
		G.DrawArrays(GL_TRIANGLES, 0, (GLsizei)o.VertCount);
		G.Disable(GL_DEPTH_TEST);
		G.ActiveTexture(GL_TEXTURE0);
		G.UseProgram(_progScene);   // retour a l'etat des tranches
	};
	const GLint locZ = U("uZ"), locInvN = U("uInvN"), locR = U("uR"), locDepth = U("uDepth");
	const GLint locVolVert = U("uVolVert"), locSideVol = U("uSideVol"), locVol = U("uVol"), locVolThick = U("uVolThick");
	const GLint locKFix = U("uKFix"), locShade = U("uShade"), locZb = U("uZb"), locBg = U("uBg"), locBgP = U("uBgP"), locBgTile = U("uBgTile");
	G.Uniform1i(locBg, 0);
	G.Uniform1i(locZb, 0);
	const GLint locExt = U("uExt"), locExtMode = U("uExtMode"), locExtBand = U("uExtBand"), locExtScroll = U("uExtScroll"), locExtBandY = U("uExtBandY"), locExtEdge = U("uExtEdge"), locExtRangeV = U("uExtRangeV"), locExtRangeH = U("uExtRangeH"), locExtFog = U("uExtFog"), locExtFogDist = U("uExtFogDist"), locExtColT = U("uExtColT"), locExtColB = U("uExtColB"), locExtColL = U("uExtColL"), locExtColR = U("uExtColR");
	G.Uniform1i(locExt, 0);
	G.Uniform1i(U("uExtDebug"), sc.ExtDebug ? 1 : 0);
	G.Uniform1f(U("uZoom"), sc.Zoom > 0.05f ? sc.Zoom : 1.0f);
	G.Uniform1f(locKFix, 0.0f);
	G.Uniform1f(locShade, 1.0f);
	const GLint locHasTop = U("uHasTop"), locHasSide = U("uHasSide"), locPeriods = U("uPeriods"), locOrigins = U("uOrigins"), locScroll = U("uScroll");
	int boundLayer = -1;
	for(int i = 0; i < sliceCount; i++) {
		const Slice& s = slices[i];
		if(s.Layer >= 100 && s.Layer < 300) { drawObject(sc.Objects.O[s.Layer - 100]); boundLayer = -1; if(sc.Objects.O[s.Layer - 100].Occlude && objDepthOn) occlOn = true; G.Uniform1i(locZb, occlOn ? 1 : 0); continue; }
		if(s.Layer != boundLayer) {
			const int lay = s.Layer >= 300 ? 0 : s.Layer; // un fond de L0 partage l'etat de la couche 0
			const bool isBg = s.Layer >= 300;
			if(lay < 4) {
				const bool wideL0 = lay == 0 && l0WideOn && !sc.Ext[0].Active;
				G.BindTexture(GL_TEXTURE_2D, wideL0 ? _l0WideTex : _layerTex[lay]);
				const HdDioramaExtend& ex = sc.Ext[lay];
				if(wideL0) {
					const float fx = sc.L0WideMX / sc.W, fy = sc.L0WideMY / sc.H;
					G.Uniform4f(locR, -fx, -fy, 1.0f + 2.0f * fx, 1.0f + 2.0f * fy);
					G.Uniform1i(locExt, 0);
				} else if(ex.Active) {
					// G4a : quad etendu de la marge de chaque cote actif
					G.Uniform4f(locR, -ex.Margin[2] / sc.W, -ex.Margin[0] / sc.H, 1.0f + (ex.Margin[2] + ex.Margin[3]) / sc.W, 1.0f + (ex.Margin[0] + ex.Margin[1]) / sc.H);
					G.Uniform4f(locExtMode, (float)ex.Mode[0], (float)ex.Mode[1], (float)ex.Mode[2], (float)ex.Mode[3]);
					G.Uniform4f(locExtBand, ex.Band[0], ex.Band[1], ex.Band[2], ex.Band[3]);
					G.Uniform2f(locExtScroll, sc.ExtScrollX, sc.ExtScrollY);
					G.Uniform2f(locExtBandY, 0.0f, 1.0f);
					G.Uniform4f(locExtEdge, ex.Edge[0], ex.Edge[1], ex.Edge[2], ex.Edge[3]);
					G.Uniform4f(locExtFog, ex.Fog[0], ex.Fog[1], ex.Fog[2], ex.Fog[3]);
					G.Uniform4f(locExtFogDist, ex.FogDist[0], ex.FogDist[1], ex.FogDist[2], ex.FogDist[3]);
					G.Uniform4f(locExtColT, ex.Color[0][0], ex.Color[0][1], ex.Color[0][2], ex.Color[0][3]);
					G.Uniform4f(locExtColB, ex.Color[1][0], ex.Color[1][1], ex.Color[1][2], ex.Color[1][3]);
					G.Uniform4f(locExtColL, ex.Color[2][0], ex.Color[2][1], ex.Color[2][2], ex.Color[2][3]);
					G.Uniform4f(locExtColR, ex.Color[3][0], ex.Color[3][1], ex.Color[3][2], ex.Color[3][3]);
					G.Uniform4f(locExtRangeV, ex.Range[0][0], ex.Range[0][1], ex.Range[1][0], ex.Range[1][1]);
					G.Uniform4f(locExtRangeH, ex.Range[2][0], ex.Range[2][1], ex.Range[3][0], ex.Range[3][1]);
					G.Uniform1i(locExt, 1);
				} else {
					G.Uniform4f(locR, 0.0f, 0.0f, 1.0f, 1.0f);
					G.Uniform1i(locExt, 0);
				}
			} else {
				const int g = lay - 4;
				G.BindTexture(GL_TEXTURE_2D, _extraTex[g]);
				const HdDioramaExtend& exg = sc.Ext[4 + g];
				const float by0 = (float)sc.ExtraY0[g] / (float)h, bh = (float)sc.ExtraRows[g] / (float)h;
				if(exg.Active) {
					// G4a : groupe etendu ; la bande de lignes envoyee reste la meme (une ligne repetee hors de la bande est transparente)
					G.Uniform4f(locR, -exg.Margin[2] / sc.W, by0 - exg.Margin[0] / sc.H, 1.0f + (exg.Margin[2] + exg.Margin[3]) / sc.W, bh + (exg.Margin[0] + exg.Margin[1]) / sc.H);
					G.Uniform4f(locExtMode, (float)exg.Mode[0], (float)exg.Mode[1], (float)exg.Mode[2], (float)exg.Mode[3]);
					G.Uniform4f(locExtBand, exg.Band[0], exg.Band[1], exg.Band[2], exg.Band[3]);
					G.Uniform2f(locExtScroll, sc.ExtScrollX, sc.ExtScrollY);
					G.Uniform2f(locExtBandY, by0, bh);
					G.Uniform4f(locExtEdge, exg.Edge[0], exg.Edge[1], exg.Edge[2], exg.Edge[3]);
					G.Uniform4f(locExtFog, exg.Fog[0], exg.Fog[1], exg.Fog[2], exg.Fog[3]);
					G.Uniform4f(locExtFogDist, exg.FogDist[0], exg.FogDist[1], exg.FogDist[2], exg.FogDist[3]);
					G.Uniform4f(locExtColT, exg.Color[0][0], exg.Color[0][1], exg.Color[0][2], exg.Color[0][3]);
					G.Uniform4f(locExtColB, exg.Color[1][0], exg.Color[1][1], exg.Color[1][2], exg.Color[1][3]);
					G.Uniform4f(locExtColL, exg.Color[2][0], exg.Color[2][1], exg.Color[2][2], exg.Color[2][3]);
					G.Uniform4f(locExtColR, exg.Color[3][0], exg.Color[3][1], exg.Color[3][2], exg.Color[3][3]);
					G.Uniform4f(locExtRangeV, exg.Range[0][0], exg.Range[0][1], exg.Range[1][0], exg.Range[1][1]);
					G.Uniform4f(locExtRangeH, exg.Range[2][0], exg.Range[2][1], exg.Range[3][0], exg.Range[3][1]);
					G.Uniform1i(locExt, 1);
				} else {
					G.Uniform1i(locExt, 0);
					G.Uniform4f(locR, 0.0f, by0, 1.0f, bh);
				}
			}
			G.Uniform1f(locInvN, s.InvN);
			// G3b2 : motifs de murs de la couche (unites 1 et 2)
			const HdDioramaWall& wl = sc.Wall[lay];
			G.Uniform1i(locHasTop, wallTop[lay] ? 1 : 0);
			G.Uniform1i(locHasSide, wallSide[lay] ? 1 : 0);
			G.Uniform2f(locOrigins, wl.TopOriginX, wl.SideOriginY);
			G.Uniform4f(locPeriods, wl.TopPerX > 0.01f ? wl.TopPerX : 0.01f, wl.TopPerY > 0.01f ? wl.TopPerY : 0.01f,
				wl.SidePerX > 0.01f ? wl.SidePerX : 0.01f, wl.SidePerY > 0.01f ? wl.SidePerY : 0.01f);
			G.Uniform1f(locScroll, wl.Scroll);
			G.Uniform1i(locVolVert, wl.SideVolVert ? 1 : 0);
			G.Uniform1i(locSideVol, wl.SideVol > 1 && wallSide[lay] ? (int)wl.SideVol : 0);
			G.Uniform4f(locVol, std::max(wl.VolPerX, 0.01f), std::max(wl.VolPerY, 0.01f), wl.VolOriginX, wl.VolOriginY);
			G.Uniform1f(locVolThick, wl.VolThick);
			G.Uniform1i(locShOn, (shadowOn && ((sc.Shadow.OntoMask >> lay) & 1u)) ? 1 : 0);
			G.Uniform1i(U("uFlat"), ((sc.FlatMask >> lay) & 1u) ? 1 : 0); // doc 53 : couche a plat (sol topDown)
			G.Uniform1f(U("uIncC"), 1.0f); G.Uniform1f(U("uIncS"), 0.0f);
			{ // G6d : carte de normales de la couche (L2 -> 0, L3 -> 1, groupes 4 a 7 -> 2 a 5)
				const int ni = lay == 2 ? 0 : (lay == 3 ? 1 : (lay >= 4 ? lay - 2 : -1));
				if(ni >= 0 && nrmOk[ni]) {
					G.Uniform1i(locNrmOn, 1);
					G.Uniform2f(U("uNrmBand"), nrmBand[ni][0], nrmBand[ni][1]);
					G.ActiveTexture(GL_TEXTURE0 + 4);
					G.BindTexture(GL_TEXTURE_2D, _nrmTex[ni]);
					G.ActiveTexture(GL_TEXTURE0);
				} else {
					G.Uniform1i(locNrmOn, 0);
				}
			}
			G.ActiveTexture(GL_TEXTURE0 + 1); G.BindTexture(GL_TEXTURE_2D, wallTop[lay]);
			G.ActiveTexture(GL_TEXTURE0 + 2); G.BindTexture(GL_TEXTURE_2D, wallSide[lay]);
			G.ActiveTexture(GL_TEXTURE0);
			G.Uniform1i(locBg, isBg ? 1 : 0);
			if(isBg) {
				const HdDioramaBgPlane& bp = sc.Bg[s.Layer - 300];
				G.BindTexture(GL_TEXTURE_2D, BgTexture(bp.Pix, bp.W, bp.H, bp.TileX, bp.TileY));
				const float fx = sc.BgMargin / sc.W, fy = sc.BgMargin / sc.H;
				G.Uniform4f(locR, -fx, -fy, 1.0f + 2.0f * fx, 1.0f + 2.0f * fy);
				G.Uniform1i(locExt, 0);
				G.Uniform4f(locBgP, bp.OffX, bp.OffY, sc.BgYStart, bp.Bright);
				G.Uniform2f(locBgTile, bp.TileX ? 1.0f : 0.0f, bp.TileY ? 1.0f : 0.0f);
				G.Uniform1i(U("uFlat"), bp.Flat ? 1 : 0); G.Uniform1f(U("uIncC"), bp.InclC); G.Uniform1f(U("uIncS"), bp.InclS); // doc 53
			}
			boundLayer = s.Layer;
		}
		G.Uniform1f(locDepth, s.Depth);
		G.Uniform1f(locZ, s.Z);
		if(occlOn) { G.Enable(GL_DEPTH_TEST); G.DepthFunc(GL_LEQUAL); G.DepthMask(GL_FALSE); }
		G.DrawArrays(GL_TRIANGLES, 0, 6);
		if(occlOn) { G.Disable(GL_DEPTH_TEST); G.DepthMask(GL_TRUE); }
	}
	G.Uniform1i(locZb, 0); occlOn = false; G.Uniform1i(locBg, 0);
	// G6a : halo des lumieres ponctuelles (ajout a l'ecran, avant la couche fixe)
	if(sc.Lights.Active && !_skyProgFailed) {
		bool drew = false;
		for(int i = 0; i < 8; i++) {
			const HdDioramaLight& l = sc.Lights.L[i];
			if(l.Type != 2 || l.Glow <= 0.0f) continue;
			if(!_progSky && !BuildSkyProgram()) { _skyProgFailed = true; break; }
			const float X = l.Pos[0] - sc.W * 0.5f, Y = l.Pos[1] - sc.H * 0.5f, Z = l.Pos[2];
			const float cx = ca * X + sa * Z;
			const float cy = sb * sa * X + cb * Y - sb * ca * Z;
			const float cz = -cb * sa * X + sb * Y + cb * ca * Z + sc.D;
			if(cz < 1.0f) continue;
			const float pers = sc.D / cz * (sc.Zoom > 0.05f ? sc.Zoom : 1.0f);
			const float k = sc.Scale * (float)aa;
			G.UseProgram(_progSky);
			auto UG = [&](const char* n) { return G.GetUniformLocation(_progSky, n); };
			G.Uniform1i(UG("uMode"), 5);
			G.Uniform2f(UG("uOut"), (float)(ow * (uint32_t)aa), (float)(h * (uint32_t)aa));
			G.Uniform3f(UG("uTop"), l.Col[0], l.Col[1], l.Col[2]);
			G.Uniform4f(UG("uGlow"), (sc.W * 0.5f + oPadNes + cx * pers) * k, (sc.H * 0.5f + cy * pers) * k, l.GlowRadius * pers * k, l.Glow);
			G.BlendFunc(GL_ONE, GL_ONE);
			G.DrawArrays(GL_TRIANGLES, 0, 3);
			drew = true;
		}
		if(drew) { G.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); G.UseProgram(_progScene); }
	}
	// G3c : couche fixe par-dessus la scene, camera neutre (aucune rotation), plan Z = 0 : position 2D d'origine.
	if(hasFixed) {
		G.Uniform1i(locLightOn, 0); // G6a : la couche fixe (HUD) n'est pas eclairee
		G.Uniform1i(locShOn, 0);    // G6c : ni ombragee
		G.Uniform1i(locNrmOn, 0);   // G6d : ni normales
		G.Uniform3f(U("uRt0"), 1.0f, 0.0f, 0.0f);
		G.Uniform3f(U("uRt1"), 0.0f, 1.0f, 0.0f);
		G.Uniform3f(U("uRt2"), 0.0f, 0.0f, 1.0f);
		G.Uniform1i(locExt, 0);
		G.Uniform4f(U("uR"), 0.0f, (float)sc.FixedY0 / (float)h, 1.0f, (float)sc.FixedRows / (float)h);
		G.BindTexture(GL_TEXTURE_2D, _fixedTex);
		G.Uniform1i(locHasTop, 0); G.Uniform1i(locHasSide, 0); G.Uniform1i(locSideVol, 0); // pas de motifs de murs sur la couche fixe
		const float fz = (std::max)(sc.FixedZ, -0.9f * sc.D);
		const float th = sc.FixedThick > 0.0f ? sc.FixedThick : 0.0f;
		const float persp = sc.FixedPersp > 0.0f ? sc.FixedPersp : 0.0f;
		int steps = 0;
		if(th > 0.0f && persp > 0.0f) {
			// decalage a l'ecran du fond par rapport a la face avant, en pixels dessines (marge 25 %)
			const float shift = th * persp / (std::max)(sc.D + fz, 1.0f) * (std::max)(sc.W, sc.H) * 0.5f * sc.Scale * (float)aa;
			steps = (std::min)((std::min)(128, (int)std::ceil(th)), (std::max)(1, (int)std::ceil(shift * 1.25f) + 1));
		}
		// echelle imposee : la face avant garde exactement sa taille 2D, les tranches arriere rapetissent en perspective
		G.Uniform1f(locKFix, (sc.D + fz) / sc.D);
		G.Uniform1f(locInvN, 1.0f / (float)(steps + 1));
		for(int i = steps; i >= 0; i--) {
			const float depth = steps > 0 ? th * (float)i / (float)steps : 0.0f;
			G.Uniform1f(locDepth, depth);
			G.Uniform1f(locShade, th > 0.0f ? 1.0f - (std::min)((std::max)(sc.FixedDark, 0.0f), 1.0f) * depth / th : 1.0f);
			G.Uniform1f(locZ, fz + depth * persp);
			G.DrawArrays(GL_TRIANGLES, 0, 6);
		}
		G.Uniform1f(locKFix, 0.0f);
		G.Uniform1f(locShade, 1.0f);
	}
	if(objDepthOn) G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0); // H1 : retire la profondeur des objets 3D
	// G3f : moyenne des blocs aa x aa vers l'image finale (premultiplie, sans melange)
	if(aa > 1) {
		G.Disable(GL_BLEND);
		G.BindFramebuffer(GL_FRAMEBUFFER, _fbo);
		G.Viewport(0, 0, (GLsizei)ow, (GLsizei)h);
		G.UseProgram(_progResolve);
		G.ActiveTexture(GL_TEXTURE0);
		G.BindTexture(GL_TEXTURE_2D, _aaTex);
		G.Uniform1i(G.GetUniformLocation(_progResolve, "uSrc"), 0);
		G.Uniform1i(G.GetUniformLocation(_progResolve, "uAa"), aa);
		G.DrawArrays(GL_TRIANGLES, 0, 3);
	}
	G.BindVertexArray(0);
	G.Disable(GL_BLEND);

	StartReadback(ow, h);
	const auto tC = std::chrono::steady_clock::now();
	_times.UploadMs += std::chrono::duration<double, std::milli>(tB - tA).count();
	_times.DrawMs += std::chrono::duration<double, std::milli>(tC - tB).count();
	_times.Count++;
	return true;
}

HdGpuTimes HdGpuCore::TakeLayerTimes()
{
	std::lock_guard<std::mutex> lk(_lock);
	HdGpuTimes t = _times;
	_times = HdGpuTimes();
	return t;
}

bool HdGpuCore::FetchOpaque(uint32_t* dst, size_t count)
{
	std::lock_guard<std::mutex> lk(_lock);
	if(!_ok || !_pending || !dst) return false;
	_pending = false;
	if(!MakeCurrent()) return false;
	const int rw = _pendW, rh = _pendH;
	if((size_t)rw * rh != count) return false;
	const size_t bytes = (size_t)rw * rh * 4;
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pbo);
	const auto tM0 = std::chrono::steady_clock::now();
	const uint32_t* pc = (const uint32_t*)G.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
	const auto tM1 = std::chrono::steady_clock::now();
	bool ok = false;
	if(pc) {
		// OpenGL range les lignes du bas vers le haut ; retournement, alpha opaque et copie en une seule passe
		for(int y = 0; y < rh; y++) {
			const uint32_t* s = pc + (size_t)(rh - 1 - y) * rw;
			uint32_t* d = dst + (size_t)y * rw;
			for(int x = 0; x < rw; x++) d[x] = s[x] | 0xFF000000u;
		}
		G.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
		ok = true;
	}
	const auto tM2 = std::chrono::steady_clock::now();
	_times.MapMs += std::chrono::duration<double, std::milli>(tM1 - tM0).count();
	_times.FlipMs += std::chrono::duration<double, std::milli>(tM2 - tM1).count();
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	return ok;
}

bool HdGpuCore::Fetch(std::vector<uint32_t>& outColor)
{
	std::lock_guard<std::mutex> lk(_lock);
	if(!_ok || !_pending) return false;
	_pending = false;
	if(!MakeCurrent()) return false;
	const int rw = _pendW, rh = _pendH;
	const size_t bytes = (size_t)rw * rh * 4;
	outColor.resize((size_t)rw * rh);
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pbo);
	const auto tM0 = std::chrono::steady_clock::now();
	const uint32_t* pc = (const uint32_t*)G.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
	const auto tM1 = std::chrono::steady_clock::now();
	bool ok = false;
	if(pc) {
		// OpenGL range les lignes du bas vers le haut
		for(int y = 0; y < rh; y++) memcpy(&outColor[(size_t)y * rw], pc + (size_t)(rh - 1 - y) * rw, rw * sizeof(uint32_t));
		G.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
		ok = true;
	}
	const auto tM2 = std::chrono::steady_clock::now();
	_times.MapMs += std::chrono::duration<double, std::milli>(tM1 - tM0).count();
	_times.FlipMs += std::chrono::duration<double, std::milli>(tM2 - tM1).count();
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	return ok;
}

#endif // LIBRETRO
