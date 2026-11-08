#pragma once
// =====================================================================================
// OpenGL 3.3 "core" minimal pour le moteur 3D GPU des packs HD (aucune dependance : glad inutile).
// Les fonctions sont chargees a l'execution par SDL_GL_GetProcAddress (SnesHdGpu.cpp).
// =====================================================================================
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define SNESHD_GLAPI __stdcall
#else
#define SNESHD_GLAPI
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_TRIANGLES 0x0004
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_ONE 1
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR 0x1800
#define GL_DEPTH 0x1801
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1
#define GL_RED 0x1903
#define GL_R32F 0x822E
#define GL_RGBA32F 0x8814
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_MAX_SAMPLES 0x8D57
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001

struct SnesHdGlApi
{
	const GLubyte* (SNESHD_GLAPI* GetString)(GLenum);
	void (SNESHD_GLAPI* GetIntegerv)(GLenum, GLint*);
	GLenum (SNESHD_GLAPI* GetError)();
	void (SNESHD_GLAPI* Viewport)(GLint, GLint, GLsizei, GLsizei);
	void (SNESHD_GLAPI* Scissor)(GLint, GLint, GLsizei, GLsizei);
	void (SNESHD_GLAPI* Enable)(GLenum);
	void (SNESHD_GLAPI* Disable)(GLenum);
	void (SNESHD_GLAPI* ClearBufferfv)(GLenum, GLint, const GLfloat*);
	void (SNESHD_GLAPI* BlendFunc)(GLenum, GLenum);
	void (SNESHD_GLAPI* DepthFunc)(GLenum);
	void (SNESHD_GLAPI* DepthMask)(GLboolean);
	void (SNESHD_GLAPI* GenTextures)(GLsizei, GLuint*);
	void (SNESHD_GLAPI* DeleteTextures)(GLsizei, const GLuint*);
	void (SNESHD_GLAPI* BindTexture)(GLenum, GLuint);
	void (SNESHD_GLAPI* ActiveTexture)(GLenum);
	void (SNESHD_GLAPI* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
	void (SNESHD_GLAPI* TexParameteri)(GLenum, GLenum, GLint);
	void (SNESHD_GLAPI* GenerateMipmap)(GLenum);
	void (SNESHD_GLAPI* PixelStorei)(GLenum, GLint);
	void (SNESHD_GLAPI* GenFramebuffers)(GLsizei, GLuint*);
	void (SNESHD_GLAPI* DeleteFramebuffers)(GLsizei, const GLuint*);
	void (SNESHD_GLAPI* BindFramebuffer)(GLenum, GLuint);
	void (SNESHD_GLAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	void (SNESHD_GLAPI* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
	GLenum (SNESHD_GLAPI* CheckFramebufferStatus)(GLenum);
	void (SNESHD_GLAPI* GenRenderbuffers)(GLsizei, GLuint*);
	void (SNESHD_GLAPI* DeleteRenderbuffers)(GLsizei, const GLuint*);
	void (SNESHD_GLAPI* BindRenderbuffer)(GLenum, GLuint);
	void (SNESHD_GLAPI* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
	void (SNESHD_GLAPI* RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
	void (SNESHD_GLAPI* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
	void (SNESHD_GLAPI* ReadBuffer)(GLenum);
	void (SNESHD_GLAPI* DrawBuffers)(GLsizei, const GLenum*);
	void (SNESHD_GLAPI* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
	GLuint (SNESHD_GLAPI* CreateShader)(GLenum);
	void (SNESHD_GLAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
	void (SNESHD_GLAPI* CompileShader)(GLuint);
	void (SNESHD_GLAPI* GetShaderiv)(GLuint, GLenum, GLint*);
	void (SNESHD_GLAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
	void (SNESHD_GLAPI* DeleteShader)(GLuint);
	GLuint (SNESHD_GLAPI* CreateProgram)();
	void (SNESHD_GLAPI* AttachShader)(GLuint, GLuint);
	void (SNESHD_GLAPI* LinkProgram)(GLuint);
	void (SNESHD_GLAPI* GetProgramiv)(GLuint, GLenum, GLint*);
	void (SNESHD_GLAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
	void (SNESHD_GLAPI* UseProgram)(GLuint);
	GLint (SNESHD_GLAPI* GetUniformLocation)(GLuint, const GLchar*);
	void (SNESHD_GLAPI* Uniform1i)(GLint, GLint);
	void (SNESHD_GLAPI* Uniform1f)(GLint, GLfloat);
	void (SNESHD_GLAPI* Uniform2f)(GLint, GLfloat, GLfloat);
	void (SNESHD_GLAPI* Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
	void (SNESHD_GLAPI* Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
	void (SNESHD_GLAPI* GenVertexArrays)(GLsizei, GLuint*);
	void (SNESHD_GLAPI* BindVertexArray)(GLuint);
	void (SNESHD_GLAPI* DeleteVertexArrays)(GLsizei, const GLuint*);
	void (SNESHD_GLAPI* GenBuffers)(GLsizei, GLuint*);
	void (SNESHD_GLAPI* BindBuffer)(GLenum, GLuint);
	void (SNESHD_GLAPI* BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
	void (SNESHD_GLAPI* DeleteBuffers)(GLsizei, const GLuint*);
	void (SNESHD_GLAPI* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
	void (SNESHD_GLAPI* EnableVertexAttribArray)(GLuint);
	void (SNESHD_GLAPI* DrawArrays)(GLenum, GLint, GLsizei);
	void (SNESHD_GLAPI* Finish)();
	void (SNESHD_GLAPI* Flush)();
	void* (SNESHD_GLAPI* MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
	GLboolean (SNESHD_GLAPI* UnmapBuffer)(GLenum);
};
