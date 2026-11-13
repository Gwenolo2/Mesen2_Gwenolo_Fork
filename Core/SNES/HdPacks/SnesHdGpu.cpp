#include "pch.h"
#include <type_traits>
#include <chrono>
#include <algorithm>
#include "SNES/HdPacks/SnesHdGpu.h"
#include "SNES/HdPacks/SnesHdGl.h"
#include "SNES/HdPacks/SnesHdData.h"
#include "Shared/MessageManager.h"

#ifdef LIBRETRO
// Core libretro : pas de SDL2 ni de contexte OpenGL prive. Le moteur 3D processeur prend le relais
// (meme comportement que <gpu3d>off).
SnesHdGpu& SnesHdGpu::Get()
{
	static SnesHdGpu instance;
	return instance;
}
bool SnesHdGpu::Available() { return false; }
bool SnesHdGpu::SubmitModels(const std::vector<SnesHdGpuView>&, uint32_t, uint32_t, float, float, float, int, int, int, int) { return false; }
bool SnesHdGpu::FetchModels(std::vector<uint32_t>&, std::vector<float>&) { return false; }
#else

#define SDL_MAIN_HANDLED
#include "SDL.h"

#ifdef _WIN32
// Le projet Core est compile en mode conforme (extensions de langage desactivees) : <windows.h> n'y
// compile pas (C2467). On declare donc seulement les 4 fonctions de kernel32 utiles ici.
extern "C" {
	__declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t* name);
	__declspec(dllimport) int __stdcall GetModuleHandleExW(unsigned long flags, const wchar_t* name, void** module);
	__declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* module, wchar_t* path, unsigned long size);
	__declspec(dllimport) void* __stdcall LoadLibraryW(const wchar_t* name);
}
#endif

static SnesHdGlApi G = {};

SnesHdGpu& SnesHdGpu::Get()
{
	static SnesHdGpu instance;
	return instance;
}

#ifdef _WIN32
// SDL2.dll est chargee "a la demande" (/DELAYLOAD) : on la cherche d'abord a cote de MesenCore.dll.
// Absente -> pas d'exception, juste le moteur processeur.
static int s_moduleAnchor = 0; // adresse situee dans MesenCore.dll (pour retrouver son dossier)
static bool SnesHdLoadSdlDll()
{
	if(GetModuleHandleW(L"SDL2.dll")) return true;
	void* self = nullptr;
	const unsigned long kFromAddress = 0x4, kUnchangedRefCount = 0x2; // GET_MODULE_HANDLE_EX_FLAG_*
	if(GetModuleHandleExW(kFromAddress | kUnchangedRefCount, (const wchar_t*)&s_moduleAnchor, &self)) {
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

bool SnesHdGpu::Available()
{
	std::lock_guard<std::mutex> lk(_lock);
	if(!_tried) {
		_tried = true;
		_ok = Init();
		MessageManager::Log(_ok ? ("[HD-GPU] Moteur 3D GPU actif : " + _info) : ("[HD-GPU] GPU indisponible (" + _info + ") : moteur 3D processeur."));
	}
	return _ok;
}

bool SnesHdGpu::Init()
{
#ifdef _WIN32
	if(!SnesHdLoadSdlDll()) { _info = "SDL2.dll introuvable"; return false; }
#endif
#ifdef __APPLE__
	// macOS : les fenetres ne se creent que sur le thread principal -> contexte CGL sans fenetre a ecrire
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
#ifdef __APPLE__
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
	SDL_Window* win = SDL_CreateWindow("MesenHdGpu", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
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
	load(G.GetString, "glGetString"); load(G.GetIntegerv, "glGetIntegerv"); load(G.GetError, "glGetError");
	load(G.Viewport, "glViewport"); load(G.Scissor, "glScissor"); load(G.Enable, "glEnable"); load(G.Disable, "glDisable");
	load(G.ClearBufferfv, "glClearBufferfv"); load(G.BlendFunc, "glBlendFunc"); load(G.DepthFunc, "glDepthFunc"); load(G.DepthMask, "glDepthMask");
	load(G.GenTextures, "glGenTextures"); load(G.DeleteTextures, "glDeleteTextures"); load(G.BindTexture, "glBindTexture");
	load(G.ActiveTexture, "glActiveTexture"); load(G.TexImage2D, "glTexImage2D"); load(G.TexParameteri, "glTexParameteri");
	load(G.GenerateMipmap, "glGenerateMipmap"); load(G.PixelStorei, "glPixelStorei");
	load(G.GenFramebuffers, "glGenFramebuffers"); load(G.DeleteFramebuffers, "glDeleteFramebuffers"); load(G.BindFramebuffer, "glBindFramebuffer");
	load(G.FramebufferTexture2D, "glFramebufferTexture2D"); load(G.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
	load(G.CheckFramebufferStatus, "glCheckFramebufferStatus"); load(G.GenRenderbuffers, "glGenRenderbuffers");
	load(G.DeleteRenderbuffers, "glDeleteRenderbuffers"); load(G.BindRenderbuffer, "glBindRenderbuffer");
	load(G.RenderbufferStorage, "glRenderbufferStorage"); load(G.RenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
	load(G.BlitFramebuffer, "glBlitFramebuffer"); load(G.ReadBuffer, "glReadBuffer"); load(G.DrawBuffers, "glDrawBuffers");
	load(G.ReadPixels, "glReadPixels"); load(G.CreateShader, "glCreateShader"); load(G.ShaderSource, "glShaderSource");
	load(G.CompileShader, "glCompileShader"); load(G.GetShaderiv, "glGetShaderiv"); load(G.GetShaderInfoLog, "glGetShaderInfoLog");
	load(G.DeleteShader, "glDeleteShader"); load(G.CreateProgram, "glCreateProgram"); load(G.AttachShader, "glAttachShader");
	load(G.LinkProgram, "glLinkProgram"); load(G.GetProgramiv, "glGetProgramiv"); load(G.GetProgramInfoLog, "glGetProgramInfoLog");
	load(G.UseProgram, "glUseProgram"); load(G.GetUniformLocation, "glGetUniformLocation"); load(G.Uniform1i, "glUniform1i");
	load(G.Uniform1f, "glUniform1f"); load(G.Uniform2f, "glUniform2f"); load(G.Uniform3f, "glUniform3f"); load(G.Uniform4f, "glUniform4f");
	load(G.GenVertexArrays, "glGenVertexArrays"); load(G.BindVertexArray, "glBindVertexArray"); load(G.DeleteVertexArrays, "glDeleteVertexArrays");
	load(G.GenBuffers, "glGenBuffers"); load(G.BindBuffer, "glBindBuffer"); load(G.BufferData, "glBufferData"); load(G.DeleteBuffers, "glDeleteBuffers");
	load(G.VertexAttribPointer, "glVertexAttribPointer"); load(G.EnableVertexAttribArray, "glEnableVertexAttribArray");
	load(G.DrawArrays, "glDrawArrays"); load(G.Finish, "glFinish");
	load(G.MapBufferRange, "glMapBufferRange"); load(G.UnmapBuffer, "glUnmapBuffer"); load(G.Flush, "glFlush");
	if(missing) { ReleaseCurrent(); return false; }

	const char* ver = (const char*)G.GetString(GL_VERSION);
	const char* ren = (const char*)G.GetString(GL_RENDERER);
	_info = std::string("OpenGL ") + (ver ? ver : "?") + " - " + (ren ? ren : "?");

	GLint maxSamples = 0;
	G.GetIntegerv(GL_MAX_SAMPLES, &maxSamples);
	_samples = std::min<GLint>(4, maxSamples);

	if(!BuildPrograms()) { ReleaseCurrent(); return false; }
	G.GenTextures(1, &_tableTex);
	G.GenTextures(1, &_boneTex);
	ReleaseCurrent();
	return true;
}

bool SnesHdGpu::MakeCurrent()
{
	// le contexte reste actif sur le thread video ; on ne le reactive que si le thread a change
	// (Mesen peut recreer son thread video : l'ancien thread termine libere le contexte)
	if(_currentThread == std::this_thread::get_id()) return true;
	if(SDL_GL_MakeCurrent((SDL_Window*)_window, (SDL_GLContext)_context) != 0) return false;
	_currentThread = std::this_thread::get_id();
	return true;
}

void SnesHdGpu::ReleaseCurrent()
{
	// liberation seulement en cas d'echec / d'abandon (pas a chaque frame)
	if(_window) SDL_GL_MakeCurrent((SDL_Window*)_window, nullptr);
	_currentThread = std::thread::id();
}

static const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aNrm;
layout(location = 3) in vec4 aJoints;
layout(location = 4) in vec4 aWeights;
uniform sampler2D uTable;     // par ligne : (Px, Py, Sx, Sy) puis (Nx, Ny, Inv, 0)
uniform sampler2D uBones;     // os : 3 texels (lignes d'une matrice 3x4) par emplacement
uniform int uSkin;
uniform vec4 uModel0, uModel1, uModel2; // placement -> (u, v, hauteur), u et v relatifs a la camera
uniform int uN;
uniform float uY0;
uniform int uHasPin;
uniform float uHorizon, uFocal, uHeight, uAxisX;
uniform vec2 uC, uF, uR;
uniform float uScale;
uniform vec2 uDst, uOut;
uniform float uAmbient;
uniform vec3 uLight;
out vec2 vUV;
out float vLit;
out float vValid;
out float vGR;
vec4 L0(int i) { return texelFetch(uTable, ivec2(2 * i, 0), 0); }
vec4 L1(int i) { return texelFetch(uTable, ivec2(2 * i + 1, 0), 0); }
float distL(int i, vec2 q) { vec4 a = L0(i); vec4 b = L1(i); return dot(q - a.xy, b.xy); }
void main()
{
	vec3 pos = aPos, nrm = aNrm;
	if(uSkin == 1) {
		vec3 sp = vec3(0.0), sn = vec3(0.0);
		vec4 p4 = vec4(aPos, 1.0);
		for(int k = 0; k < 4; k++) {
			float w = aWeights[k];
			if(w <= 0.0) continue;
			int j = int(aJoints[k] + 0.5);
			vec4 r0 = texelFetch(uBones, ivec2(3 * j, 0), 0), r1 = texelFetch(uBones, ivec2(3 * j + 1, 0), 0), r2 = texelFetch(uBones, ivec2(3 * j + 2, 0), 0);
			sp += w * vec3(dot(r0, p4), dot(r1, p4), dot(r2, p4));
			sn += w * vec3(dot(r0.xyz, aNrm), dot(r1.xyz, aNrm), dot(r2.xyz, aNrm));
		}
		pos = sp; nrm = sn;
	}
	vec4 pw = vec4(pos, 1.0);
	vec2 q = vec2(dot(uModel0, pw), dot(uModel1, pw));
	float h = dot(uModel2, pw);
	float sx = 0.0, sy = 0.0, ppt = 0.0;
	bool ok = true;
	float D = dot(q - uC, uF);
	float dFar = distL(0, q), dNear = distL(uN - 1, q);
	if(dFar >= 0.0) {
		ok = false;                       // au-dela de la ligne la plus lointaine
	} else if(dNear < 0.0) {
		if(uHasPin == 0 || D < 1.0) ok = false;
		else { float lt = dot(q - uC, uR); ppt = uFocal / D; sx = uAxisX + lt * ppt; sy = uHorizon + (uHeight - h) * ppt; }
	} else {
		int lo = 0, hi = uN - 1;
		for(int it = 0; it < 10; it++) {
			if(hi - lo <= 1) break;
			int mid = (lo + hi) / 2;
			if(distL(mid, q) >= 0.0) hi = mid; else lo = mid;
		}
		float da = distL(lo, q), db = distL(hi, q);
		float t = (da - db) != 0.0 ? da / (da - db) : 0.0;
		vec4 a0 = L0(lo), a1 = L1(lo), b0 = L0(hi), b1 = L1(hi);
		float xa = dot(q - a0.xy, a0.zw) / dot(a0.zw, a0.zw);
		float xb = dot(q - b0.xy, b0.zw) / dot(b0.zw, b0.zw);
		sx = mix(xa, xb, t);
		ppt = mix(a1.z, b1.z, t);
		sy = uY0 + float(lo) + t - h * ppt;
	}
	if(D < 2.0) ok = false;
	D = max(D, 1.0);
	vValid = ok ? 1.0 : 0.0;
	vGR = sy + h * ppt;
	float hx = (sx + uDst.x) * uScale, hy = (sy + uDst.y) * uScale;
	vec2 ndc = vec2(hx / uOut.x * 2.0 - 1.0, 1.0 - hy / uOut.y * 2.0);
	float z = clamp(D / 8192.0, 0.0, 1.0) * 2.0 - 1.0;
	gl_Position = vec4(ndc * D, z * D, D);   // w = profondeur -> interpolation corrigee en perspective
	vUV = aUV;
	vec3 n = vec3(dot(uModel0.xyz, nrm), dot(uModel1.xyz, nrm), dot(uModel2.xyz, nrm));
	float nl = length(n);
	vLit = uAmbient + (1.0 - uAmbient) * (nl > 1e-6 ? abs(dot(n / nl, uLight)) : 1.0);
}
)";

static const char* kFragmentColor = R"(#version 330 core
in vec2 vUV; in float vLit; in float vValid; in float vGR;
uniform sampler2D uTex;
uniform int uHasTex;
uniform vec4 uKd;   // couleur diffuse + alpha (materiau x fondu)
layout(location = 0) out vec4 oColor;
void main()
{
	if(vValid < 0.999) discard;
	vec4 c = uKd;
	if(uHasTex == 1) { vec4 t = texture(uTex, vec2(vUV.x, 1.0 - vUV.y)); if(t.a < 0.5) discard; c.rgb *= t.rgb; }
	c.rgb = min(c.rgb * vLit, vec3(1.0));
	oColor = vec4(c.rgb * c.a, c.a);   // premultiplie
}
)";

static const char* kFragmentGround = R"(#version 330 core
in vec2 vUV; in float vLit; in float vValid; in float vGR;
uniform sampler2D uTex;
uniform int uHasTex;
uniform vec4 uKd;
layout(location = 0) out vec4 oGround;
void main()
{
	if(vValid < 0.999 || uKd.a < 0.5) discard;
	if(uHasTex == 1) { vec4 t = texture(uTex, vec2(vUV.x, 1.0 - vUV.y)); if(t.a < 0.5) discard; }
	oGround = vec4(vGR, 0.0, 0.0, 1.0);
}
)";

static GLuint SnesHdCompile(GLenum type, const char* src, std::string& err)
{
	GLuint s = G.CreateShader(type);
	G.ShaderSource(s, 1, &src, nullptr);
	G.CompileShader(s);
	GLint ok = 0; G.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if(!ok) { char log[1024] = {}; G.GetShaderInfoLog(s, sizeof(log) - 1, nullptr, log); err = log; G.DeleteShader(s); return 0; }
	return s;
}

static GLuint SnesHdLink(const char* vs, const char* fs, std::string& err)
{
	GLuint v = SnesHdCompile(GL_VERTEX_SHADER, vs, err); if(!v) return 0;
	GLuint f = SnesHdCompile(GL_FRAGMENT_SHADER, fs, err); if(!f) { G.DeleteShader(v); return 0; }
	GLuint p = G.CreateProgram();
	G.AttachShader(p, v); G.AttachShader(p, f); G.LinkProgram(p);
	G.DeleteShader(v); G.DeleteShader(f);
	GLint ok = 0; G.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if(!ok) { char log[1024] = {}; G.GetProgramInfoLog(p, sizeof(log) - 1, nullptr, log); err = log; return 0; }
	return p;
}

bool SnesHdGpu::BuildPrograms()
{
	std::string err;
	_progColor = SnesHdLink(kVertexShader, kFragmentColor, err);
	if(_progColor) _progGround = SnesHdLink(kVertexShader, kFragmentGround, err);
	if(!_progColor || !_progGround) { _info = "shaders : " + err; return false; }
	return true;
}

bool SnesHdGpu::EnsureTargets(uint32_t w, uint32_t h)
{
	if(w == _w && h == _h && _rsFbo) return true;
	if(_msFbo) { G.DeleteFramebuffers(1, &_msFbo); G.DeleteRenderbuffers(1, &_msColor); G.DeleteRenderbuffers(1, &_msDepth); _msFbo = 0; }
	if(_rsFbo) { G.DeleteFramebuffers(1, &_rsFbo); G.DeleteTextures(1, &_rsColor); _rsFbo = 0; }
	if(_grFbo) { G.DeleteFramebuffers(1, &_grFbo); G.DeleteTextures(1, &_grTex); G.DeleteRenderbuffers(1, &_grDepth); _grFbo = 0; }
	_w = w; _h = h;

	auto colorTex = [&](GLuint& tex, GLint fmt, GLenum f, GLenum t) {
		G.GenTextures(1, &tex); G.BindTexture(GL_TEXTURE_2D, tex);
		G.TexImage2D(GL_TEXTURE_2D, 0, fmt, (GLsizei)w, (GLsizei)h, 0, f, t, nullptr);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	};
	// couleur resolue
	colorTex(_rsColor, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
	G.GenFramebuffers(1, &_rsFbo); G.BindFramebuffer(GL_FRAMEBUFFER, _rsFbo);
	G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _rsColor, 0);
	if(G.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
	// anticrenelage
	if(_samples > 1) {
		G.GenRenderbuffers(1, &_msColor); G.BindRenderbuffer(GL_RENDERBUFFER, _msColor);
		G.RenderbufferStorageMultisample(GL_RENDERBUFFER, _samples, GL_RGBA8, (GLsizei)w, (GLsizei)h);
		G.GenRenderbuffers(1, &_msDepth); G.BindRenderbuffer(GL_RENDERBUFFER, _msDepth);
		G.RenderbufferStorageMultisample(GL_RENDERBUFFER, _samples, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
		G.GenFramebuffers(1, &_msFbo); G.BindFramebuffer(GL_FRAMEBUFFER, _msFbo);
		G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _msColor);
		G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _msDepth);
		if(G.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			G.DeleteFramebuffers(1, &_msFbo); G.DeleteRenderbuffers(1, &_msColor); G.DeleteRenderbuffers(1, &_msDepth);
			_msFbo = 0; _samples = 0;
		}
	}
	// ligne du sol (1 echantillon) + profondeur
	colorTex(_grTex, GL_R32F, GL_RED, GL_FLOAT);
	G.GenRenderbuffers(1, &_grDepth); G.BindRenderbuffer(GL_RENDERBUFFER, _grDepth);
	G.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
	G.GenFramebuffers(1, &_grFbo); G.BindFramebuffer(GL_FRAMEBUFFER, _grFbo);
	G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _grTex, 0);
	G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _grDepth);
	if(G.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
	// sans anticrenelage, la couleur est dessinee directement dans _rsFbo : il lui faut une profondeur
	if(!_msFbo) {
		G.GenRenderbuffers(1, &_msDepth); G.BindRenderbuffer(GL_RENDERBUFFER, _msDepth);
		G.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
		G.BindFramebuffer(GL_FRAMEBUFFER, _rsFbo);
		G.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _msDepth);
	}
	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

unsigned int SnesHdGpu::GetTexture(SnesHdPackBitmapInfo* bmp)
{
	if(!bmp) return 0;
	auto it = _textures.find(bmp);
	if(it != _textures.end()) return it->second;
	GLuint tex = 0;
	if(bmp->PixelData.empty()) bmp->Init();
	if(!bmp->PixelData.empty() && bmp->Width > 0 && bmp->Height > 0) {
		G.GenTextures(1, &tex); G.BindTexture(GL_TEXTURE_2D, tex);
		G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
		// PixelData = 0xAARRGGBB -> octets B, G, R, A ; ligne 0 = haut de l'image
		G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)bmp->Width, (GLsizei)bmp->Height, 0, GL_BGRA, GL_UNSIGNED_BYTE, bmp->PixelData.data());
		G.GenerateMipmap(GL_TEXTURE_2D);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
	_textures[bmp] = tex;
	return tex;
}

SnesHdGpu::GpuMesh* SnesHdGpu::GetMesh(const SnesHdModelMesh* mesh)
{
	auto it = _meshes.find(mesh);
	if(it != _meshes.end()) return &it->second;
	GpuMesh gm;
	// sommets tries par materiau : position(3) uv(2) normale(3) os(4) poids(4)
	const int kStride = 16;
	std::vector<float> data;
	data.reserve(mesh->Tris.size() * 3 * kStride);
	int nMat = (int)mesh->Mats.size();
	for(int m = 0; m < nMat; m++) {
		int first = (int)(data.size() / kStride);
		for(const SnesHdModelTri& t : mesh->Tris) {
			if(std::clamp(t.Mat, 0, nMat - 1) != m) continue;
			for(int k = 0; k < 3; k++) {
				data.push_back(t.P[k][0]); data.push_back(t.P[k][1]); data.push_back(t.P[k][2]);
				data.push_back(t.UV[k][0]); data.push_back(t.UV[k][1]);
				data.push_back(t.N[k][0]); data.push_back(t.N[k][1]); data.push_back(t.N[k][2]);
				for(int j = 0; j < 4; j++) data.push_back((float)t.J[k][j]);
				for(int j = 0; j < 4; j++) data.push_back(t.Wt[k][j]);
			}
		}
		int count = (int)(data.size() / kStride) - first;
		if(count > 0) gm.Ranges.push_back({ m, first, count });
	}
	G.GenVertexArrays(1, &gm.Vao); G.BindVertexArray(gm.Vao);
	G.GenBuffers(1, &gm.Vbo); G.BindBuffer(GL_ARRAY_BUFFER, gm.Vbo);
	G.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
	const GLsizei st = kStride * sizeof(float);
	G.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (const void*)0);
	G.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (const void*)(3 * sizeof(float)));
	G.VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, st, (const void*)(5 * sizeof(float)));
	G.VertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, st, (const void*)(8 * sizeof(float)));
	G.VertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, (const void*)(12 * sizeof(float)));
	for(GLuint a = 0; a < 5; a++) G.EnableVertexAttribArray(a);
	G.BindVertexArray(0);
	auto res = _meshes.emplace(mesh, std::move(gm));
	return &res.first->second;
}

bool SnesHdGpu::SubmitModels(const std::vector<SnesHdGpuView>& views, uint32_t w, uint32_t h, float scale, float dstX, float dstY,
	int rx0, int ry0, int rx1, int ry1)
{
	std::lock_guard<std::mutex> lk(_lock);
	rx0 = std::max(rx0, 0); ry0 = std::max(ry0, 0); rx1 = std::min(rx1, (int)w - 1); ry1 = std::min(ry1, (int)h - 1);
	if(!_ok || w == 0 || h == 0 || rx0 > rx1 || ry0 > ry1) return false;
	const int rw = rx1 - rx0 + 1, rh = ry1 - ry0 + 1;
	const GLint glY0 = (GLint)h - 1 - ry1; // OpenGL : origine en bas
	auto t0 = std::chrono::high_resolution_clock::now();
	if(!MakeCurrent()) return false;
	if(!EnsureTargets(w, h)) { ReleaseCurrent(); _ok = false; MessageManager::Log("[HD-GPU] Images cachees impossibles : moteur processeur."); return false; }

	const float lx = -0.45f, lv = -0.35f, lh = 0.82f, ln = std::sqrt(lx * lx + lv * lv + lh * lh);
	G.Viewport(0, 0, (GLsizei)w, (GLsizei)h);
	G.Disable(GL_CULL_FACE);
	G.Enable(GL_DEPTH_TEST); G.DepthFunc(GL_LEQUAL); G.DepthMask(GL_TRUE);

	for(int pass = 0; pass < 2; pass++) {
		// passe 0 : couleur (anticrenelage, melange premultiplie) ; passe 1 : ligne du sol (opaque)
		GLuint prog = pass == 0 ? _progColor : _progGround;
		G.BindFramebuffer(GL_FRAMEBUFFER, pass == 0 ? (_msFbo ? _msFbo : _rsFbo) : _grFbo);
		// effacement limite au rectangle utile (le ciseau s'applique aussi a l'effacement)
		G.Enable(GL_SCISSOR_TEST);
		G.Scissor(rx0, glY0, rw, rh);
		const GLfloat clearColor[4] = { 0, 0, 0, 0 }, clearGround[4] = { -1, 0, 0, 0 }, one = 1.0f;
		G.ClearBufferfv(GL_COLOR, 0, pass == 0 ? clearColor : clearGround);
		G.ClearBufferfv(GL_DEPTH, 0, &one);
		if(pass == 0) { G.Enable(GL_BLEND); G.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); } else G.Disable(GL_BLEND);
		G.UseProgram(prog);
		auto U = [&](const char* n) { return G.GetUniformLocation(prog, n); };
		G.Uniform1i(U("uTable"), 0); G.Uniform1i(U("uTex"), 1); G.Uniform1i(U("uBones"), 2);
		G.Uniform1f(U("uScale"), scale); G.Uniform2f(U("uDst"), dstX, dstY); G.Uniform2f(U("uOut"), (float)w, (float)h);
		G.Uniform3f(U("uLight"), lx / ln, lv / ln, lh / ln);
		G.Enable(GL_SCISSOR_TEST);

		for(const SnesHdGpuView& v : views) {
			const SnesHdM7Zone& zn = *v.Zone;
			const int n = zn.Count();
			if(n < 2 || v.Instances.empty()) continue;
			// table de la vue
			std::vector<float> table((size_t)n * 8);
			for(int i = 0; i < n; i++) {
				float* t = &table[(size_t)i * 8];
				t[0] = (float)zn.Px[i]; t[1] = (float)zn.Py[i]; t[2] = (float)zn.Sx[i]; t[3] = (float)zn.Sy[i];
				t[4] = (float)zn.Nx[i]; t[5] = (float)zn.Ny[i]; t[6] = (float)zn.Inv[i]; t[7] = 0;
			}
			// positions relatives a la camera : la carte "depliee" peut depasser 8192 texels -> precision float
			for(int i = 0; i < n; i++) { table[(size_t)i * 8] -= (float)zn.Cx; table[(size_t)i * 8 + 1] -= (float)zn.Cy; }
			G.ActiveTexture(GL_TEXTURE0); G.BindTexture(GL_TEXTURE_2D, _tableTex);
			G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
			G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, n * 2, 1, 0, GL_RGBA, GL_FLOAT, table.data());
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			G.Uniform1i(U("uN"), n); G.Uniform1f(U("uY0"), (float)zn.Y0);
			G.Uniform1i(U("uHasPin"), zn.HasPinhole ? 1 : 0);
			G.Uniform1f(U("uHorizon"), (float)zn.Horizon); G.Uniform1f(U("uFocal"), (float)zn.Focal);
			G.Uniform1f(U("uHeight"), (float)zn.Height); G.Uniform1f(U("uAxisX"), (float)zn.AxisX);
			G.Uniform2f(U("uC"), 0.0f, 0.0f); G.Uniform2f(U("uF"), (float)zn.Fx, (float)zn.Fy); G.Uniform2f(U("uR"), (float)zn.Rx, (float)zn.Ry);
			{
				// ciseau = bande de la vue inter rectangle utile
				int cy0 = std::max(v.ClipY0, ry0), cy1 = std::min(v.ClipY1, ry1);
				if(cy0 > cy1) continue;
				G.Scissor(rx0, (GLint)h - 1 - cy1, rw, cy1 - cy0 + 1);
			}

			for(const SnesHdGpuInstance& in : v.Instances) {
				GpuMesh* gm = GetMesh(in.Mesh);
				if(!gm || gm->Ranges.empty()) continue;
				G.Uniform4f(U("uModel0"), in.Model[0], in.Model[1], in.Model[2], in.Model[3]);
				G.Uniform4f(U("uModel1"), in.Model[4], in.Model[5], in.Model[6], in.Model[7]);
				G.Uniform4f(U("uModel2"), in.Model[8], in.Model[9], in.Model[10], in.Model[11]);
				G.Uniform1f(U("uAmbient"), in.Ambient);
				if(in.Palette && !in.Palette->empty()) {
					// os animes : 3 texels RGBA32F (lignes de la matrice) par emplacement
					G.ActiveTexture(GL_TEXTURE0 + 2); G.BindTexture(GL_TEXTURE_2D, _boneTex);
					G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
					G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (GLsizei)(in.Palette->size() / 4), 1, 0, GL_RGBA, GL_FLOAT, in.Palette->data());
					G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					G.Uniform1i(U("uSkin"), 1);
				} else {
					G.Uniform1i(U("uSkin"), 0);
				}
				G.BindVertexArray(gm->Vao);
				for(const GpuMesh::Range& r : gm->Ranges) {
					const SnesHdModelMaterial& mat = in.Mesh->Mats[r.Mat];
					GLuint tex = GetTexture(mat.Texture);
					G.ActiveTexture(GL_TEXTURE1); G.BindTexture(GL_TEXTURE_2D, tex);
					G.Uniform1i(U("uHasTex"), tex ? 1 : 0);
					G.Uniform4f(U("uKd"), ((mat.Kd >> 16) & 0xFF) / 255.0f, ((mat.Kd >> 8) & 0xFF) / 255.0f, (mat.Kd & 0xFF) / 255.0f, mat.Alpha * in.Fade);
					G.DrawArrays(GL_TRIANGLES, r.First, r.Count);
				}
			}
		}
		G.BindVertexArray(0);
		G.Disable(GL_SCISSOR_TEST);
	}

	// resolution de l'anticrenelage puis relecture
	G.Disable(GL_SCISSOR_TEST);
	if(_msFbo) {
		G.BindFramebuffer(GL_READ_FRAMEBUFFER, _msFbo); G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, _rsFbo);
		G.BlitFramebuffer(rx0, glY0, rx0 + rw, glY0 + rh, rx0, glY0, rx0 + rw, glY0 + rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	// relecture ASYNCHRONE dans deux tampons PBO : glReadPixels rend la main tout de suite,
	// la copie se fait pendant que le processeur dessine la piste et les sprites
	const size_t bytes = (size_t)rw * rh * 4;
	if(!_pboColor) { G.GenBuffers(1, &_pboColor); G.GenBuffers(1, &_pboGround); }
	if(bytes > _pboCapacity) {
		G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pboColor); G.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_READ);
		G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pboGround); G.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_READ);
		_pboCapacity = bytes;
	}
	G.PixelStorei(GL_PACK_ALIGNMENT, 4);
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pboColor);
	G.BindFramebuffer(GL_READ_FRAMEBUFFER, _rsFbo);
	G.ReadPixels(rx0, glY0, rw, rh, GL_BGRA, GL_UNSIGNED_BYTE, (void*)0);
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pboGround);
	G.BindFramebuffer(GL_READ_FRAMEBUFFER, _grFbo);
	G.ReadPixels(rx0, glY0, rw, rh, GL_RED, GL_FLOAT, (void*)0);
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	G.Flush(); // la carte graphique commence tout de suite
	GLenum err = G.GetError();
	_pending = true; _pendW = rw; _pendH = rh;

	_timeAcc += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
	_pixAcc += (uint64_t)rw * rh;
	if(err != GL_NO_ERROR) {
		static int logged = 0;
		if(logged++ < 3) MessageManager::Log("[HD-GPU] Erreur OpenGL " + std::to_string(err));
	}
	(void)_timeFrames;
	return true;
}

bool SnesHdGpu::FetchModels(std::vector<uint32_t>& outColor, std::vector<float>& outGround)
{
	std::lock_guard<std::mutex> lk(_lock);
	if(!_ok || !_pending) return false;
	_pending = false;
	if(!MakeCurrent()) return false;
	auto t0 = std::chrono::high_resolution_clock::now();
	const int rw = _pendW, rh = _pendH;
	const size_t bytes = (size_t)rw * rh * 4;
	outColor.resize((size_t)rw * rh); outGround.resize((size_t)rw * rh);
	bool ok = true;
	// OpenGL range les lignes du bas vers le haut
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pboColor);
	const uint32_t* pc = (const uint32_t*)G.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
	if(pc) {
		for(int y = 0; y < rh; y++) memcpy(&outColor[(size_t)y * rw], pc + (size_t)(rh - 1 - y) * rw, rw * sizeof(uint32_t));
		G.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
	} else ok = false;
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, _pboGround);
	const float* pg = ok ? (const float*)G.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT) : nullptr;
	if(pg) {
		for(int y = 0; y < rh; y++) memcpy(&outGround[(size_t)y * rw], pg + (size_t)(rh - 1 - y) * rw, rw * sizeof(float));
		G.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
	} else ok = false;
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	// mesure du cout (journal toutes les 300 frames) : temps passe par le thread video (envoi + recuperation)
	_timeAcc += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
	if(++_timeFrames >= 300) {
		char buf[160];
		snprintf(buf, sizeof(buf), "[HD-GPU] modeles : %.2f ms / frame en moyenne, zone relue %.0f%% de l'image",
			_timeAcc / _timeFrames, 100.0 * (double)_pixAcc / ((double)_timeFrames * _w * _h));
		MessageManager::Log(buf);
		_timeAcc = 0; _timeFrames = 0; _pixAcc = 0;
	}
	return ok;
}

#endif // LIBRETRO
