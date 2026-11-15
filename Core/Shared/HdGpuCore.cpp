#include "pch.h"
#include <type_traits>
#include <algorithm>
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
bool HdGpuCore::Fetch(std::vector<uint32_t>&) { return false; }
#else

#define SDL_MAIN_HANDLED
#include "SDL.h"

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
	load(G.ClearBufferfv, "glClearBufferfv"); load(G.BlendFunc, "glBlendFunc");
	load(G.GenTextures, "glGenTextures"); load(G.DeleteTextures, "glDeleteTextures"); load(G.BindTexture, "glBindTexture");
	load(G.TexImage2D, "glTexImage2D"); load(G.TexParameteri, "glTexParameteri"); load(G.PixelStorei, "glPixelStorei");
	load(G.GenFramebuffers, "glGenFramebuffers"); load(G.DeleteFramebuffers, "glDeleteFramebuffers"); load(G.BindFramebuffer, "glBindFramebuffer");
	load(G.FramebufferTexture2D, "glFramebufferTexture2D"); load(G.CheckFramebufferStatus, "glCheckFramebufferStatus");
	load(G.ReadPixels, "glReadPixels"); load(G.CreateShader, "glCreateShader"); load(G.ShaderSource, "glShaderSource");
	load(G.CompileShader, "glCompileShader"); load(G.GetShaderiv, "glGetShaderiv"); load(G.GetShaderInfoLog, "glGetShaderInfoLog");
	load(G.DeleteShader, "glDeleteShader"); load(G.CreateProgram, "glCreateProgram"); load(G.AttachShader, "glAttachShader");
	load(G.LinkProgram, "glLinkProgram"); load(G.GetProgramiv, "glGetProgramiv"); load(G.GetProgramInfoLog, "glGetProgramInfoLog");
	load(G.UseProgram, "glUseProgram"); load(G.GetUniformLocation, "glGetUniformLocation"); load(G.Uniform1i, "glUniform1i");
	load(G.Uniform1f, "glUniform1f"); load(G.Uniform2f, "glUniform2f"); load(G.Uniform4f, "glUniform4f");
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

	// relecture ASYNCHRONE dans un tampon PBO : glReadPixels rend la main tout de suite
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
	return true;
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
	const uint32_t* pc = (const uint32_t*)G.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
	bool ok = false;
	if(pc) {
		// OpenGL range les lignes du bas vers le haut
		for(int y = 0; y < rh; y++) memcpy(&outColor[(size_t)y * rw], pc + (size_t)(rh - 1 - y) * rw, rw * sizeof(uint32_t));
		G.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
		ok = true;
	}
	G.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	return ok;
}

#endif // LIBRETRO
