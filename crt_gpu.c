/* crt_gpu.c - see crt_gpu.h. GL interop follows runner/cyc/cyc_hd_gpu.c:
 * render into an SDL render-target texture with our own program, saving and
 * restoring the GL state SDL's OpenGL renderer relies on.
 *
 * Two passes (row 0 = top; texelFetch, no filtering):
 *   1 GLOW  sw x sh   the source in linear light, 3x3 box-blurred
 *   2 OUT   dw x dh   beams + mask + glow (crt_filter.c's model) in the
 *                     screen rect of an SDL target texture, black around it
 */
#include "crt_gpu.h"
#include "crt_filter.h"

#ifdef CYC_WITH_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <string.h>

#define GLF(X) \
    X(const GLubyte *, GetString, (GLenum)) X(void, GetIntegerv, (GLenum, GLint *)) \
    X(GLboolean, IsEnabled, (GLenum)) X(GLenum, GetError, (void)) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, Clear, (GLbitfield)) \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) X(void, PixelStorei, (GLenum, GLint)) \
    X(void, ActiveTexture, (GLenum)) X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(GLuint, CreateShader, (GLenum)) X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) X(void, AttachShader, (GLuint, GLuint)) X(void, LinkProgram, (GLuint)) \
    X(void, BindFragDataLocation, (GLuint, GLuint, const GLchar *)) X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform2i, (GLint, GLint, GLint)) X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, Uniform1fv, (GLint, GLsizei, const GLfloat *)) X(void, Uniform3fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) X(void, BindVertexArray, (GLuint)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, GetFramebufferAttachmentParameteriv, (GLenum, GLenum, GLenum, GLint *))

typedef struct {
#define GL_DECL(ret, name, args) ret(APIENTRY *name) args;
    GLF(GL_DECL)
#undef GL_DECL
} Gl;


enum { P_GLOW, P_OUT, P_COUNT };
enum { T_SRC, T_GLOW, T_COUNT };

static struct {
    bool tried, failed;
    Gl gl;
    SDL_Renderer *ren;
    SDL_GLContext ctx;
    GLuint prog[P_COUNT], tex[T_COUNT], fbo[T_COUNT], vao;
    int tw[T_COUNT], th[T_COUNT];
    SDL_Texture *out;
    int out_w, out_h;
} G;

/* ---- shaders ---- */
#define STR_(x) #x
#define STR(x) STR_(x)
static const char *k_vertex =
    "#version 130\n"
    "void main() {\n"
    "    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *k_common =
    "#version 130\n"
    "out vec4 frag;\n"
    "uniform sampler2D src;\n"
    "uniform ivec2 src_size;\n"
    "const float GAMMA = " STR(CRT_GAMMA) ";\n"
    "vec3 srcpx(int x, int y) {\n"
    "    return pow(texelFetch(src, ivec2(clamp(x, 0, src_size.x - 1), clamp(y, 0, src_size.y - 1)), 0).rgb, vec3(GAMMA));\n"
    "}\n";

/* 1: linear-light 3x3 box blur of the source (glow) */
static const char *k_glow =
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    vec3 acc = vec3(0.0);\n"
    "    for (int dy = -1; dy <= 1; dy++)\n"
    "        for (int dx = -1; dx <= 1; dx++) acc += srcpx(p.x + dx, p.y + dy);\n"
    "    frag = vec4(acc / 9.0, 1.0);\n"
    "}\n";

/* 2: beams of the two nearest source lines, stripe mask, glow, gamma */
static const char *k_out =
    "uniform sampler2D glowt;\n"
    "uniform ivec2 origin;\n"
    "uniform ivec2 content;\n"
    "uniform float BMIN, BMAX, GAIN, GLOW;\n"   /* the current look (crt_set_look) */
    "const float SHARP = " STR(CRT_SHARP) ", MDIM = " STR(CRT_MASK_DIM) ";\n"
    "vec3 line_color(int j, float px) {\n"
    "    float p = px - 0.5;\n"
    "    float i = floor(p);\n"
    "    float f = clamp((p - i - 0.5) * SHARP + 0.5, 0.0, 1.0);\n"
    "    return mix(srcpx(int(i), j), srcpx(int(i) + 1, j), f);\n"
    "}\n"
    "vec3 glow_at(float px, float py) {\n"
    "    vec2 q = vec2(px, py) - 0.5;\n"
    "    vec2 b = floor(q), f = q - b;\n"
    "    ivec2 i = ivec2(b), hi = src_size - 1;\n"
    "    vec3 a = texelFetch(glowt, clamp(i, ivec2(0), hi), 0).rgb;\n"
    "    vec3 c = texelFetch(glowt, clamp(i + ivec2(1, 0), ivec2(0), hi), 0).rgb;\n"
    "    vec3 d = texelFetch(glowt, clamp(i + ivec2(0, 1), ivec2(0), hi), 0).rgb;\n"
    "    vec3 e = texelFetch(glowt, clamp(i + ivec2(1, 1), ivec2(0), hi), 0).rgb;\n"
    "    return mix(mix(a, c, f.x), mix(d, e, f.x), f.y);\n"
    "}\n"
    "vec3 beam(vec3 c, float d) {\n"
    "    vec3 s = BMIN + (BMAX - BMIN) * c;\n"
    "    return c * exp(-(d * d) / (2.0 * s * s));\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy) - origin;\n"
    "    float px = (float(p.x) + 0.5) * float(src_size.x) / float(content.x);\n"
    "    float sy = (float(p.y) + 0.5) * float(src_size.y) / float(content.y);\n"
    "    float q = sy - 0.5;\n"
    "    float j = floor(q), t = q - j;\n"
    "    vec3 a = clamp(line_color(int(j), px), 0.0, 1.0);\n"
    "    vec3 b = clamp(line_color(int(j) + 1, px), 0.0, 1.0);\n"
    "    vec3 v = beam(a, t) + beam(b, 1.0 - t);\n"
    "    int slot = int(gl_FragCoord.x) % 3;\n"
    "    vec3 m = vec3(MDIM);\n"
    "    m[slot] = 1.0;\n"
    "    v = v * m * GAIN + glow_at(px, sy) * GLOW;\n"
    "    frag = vec4(pow(clamp(v, 0.0, 1.0), vec3(1.0 / GAMMA)), 1.0);\n"
    "}\n";

/* ---- GL helpers ---- */
static bool load_gl(Gl *g) {
#define CRT_GL_LOAD(ret, name, args) \
    g->name = (ret(APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name); \
    if (!g->name) { fprintf(stderr, "[CRT GPU] missing gl" #name "\n"); return false; }
    GLF(CRT_GL_LOAD)
#undef CRT_GL_LOAD
    return true;
}

static GLuint compile(GLenum kind, const char *a, const char *b) {
    Gl *g = &G.gl;
    GLuint sh = g->CreateShader(kind);
    const char *srcs[2] = { a, b };
    g->ShaderSource(sh, b ? 2 : 1, srcs, NULL);
    g->CompileShader(sh);
    GLint ok = 0;
    g->GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = { 0 };
        g->GetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[CRT GPU] shader: %s\n", log);
        g->DeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint program(const char *frag) {
    Gl *g = &G.gl;
    GLuint vs = compile(GL_VERTEX_SHADER, k_vertex, NULL), fs = compile(GL_FRAGMENT_SHADER, k_common, frag);
    if (!vs || !fs) { if (vs) g->DeleteShader(vs); if (fs) g->DeleteShader(fs); return 0; }
    GLuint p = g->CreateProgram();
    g->AttachShader(p, vs);
    g->AttachShader(p, fs);
    g->BindFragDataLocation(p, 0, "frag");
    g->LinkProgram(p);
    g->DeleteShader(vs);
    g->DeleteShader(fs);
    GLint ok = 0;
    g->GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = { 0 };
        g->GetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[CRT GPU] link: %s\n", log);
        return 0;
    }
    return p;
}

typedef struct {
    GLint program, active, vao, fbo, viewport[4], tex[4];
    GLboolean scissor, blend, depth, cull, srgb;
} GlState;

static void state_take(GlState *s) {
    Gl *g = &G.gl;
    g->GetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    g->GetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    g->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    g->GetIntegerv(GL_FRAMEBUFFER_BINDING, &s->fbo);
    g->GetIntegerv(GL_VIEWPORT, s->viewport);
    for (int i = 0; i < 4; i++) {
        g->ActiveTexture(GL_TEXTURE0 + i);
        g->GetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex[i]);
    }
    s->scissor = g->IsEnabled(GL_SCISSOR_TEST);
    s->blend = g->IsEnabled(GL_BLEND);
    s->depth = g->IsEnabled(GL_DEPTH_TEST);
    s->cull = g->IsEnabled(GL_CULL_FACE);
    s->srgb = g->IsEnabled(GL_FRAMEBUFFER_SRGB);
    g->Disable(GL_SCISSOR_TEST);
    g->Disable(GL_BLEND);
    g->Disable(GL_DEPTH_TEST);
    g->Disable(GL_CULL_FACE);
    g->Disable(GL_FRAMEBUFFER_SRGB);
    g->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void state_restore(const GlState *s) {
    Gl *g = &G.gl;
    g->UseProgram((GLuint)s->program);
    g->BindVertexArray((GLuint)s->vao);
    g->BindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->fbo);
    for (int i = 0; i < 4; i++) {
        g->ActiveTexture(GL_TEXTURE0 + i);
        g->BindTexture(GL_TEXTURE_2D, (GLuint)s->tex[i]);
    }
    g->ActiveTexture((GLenum)s->active);
    g->Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    if (s->scissor) g->Enable(GL_SCISSOR_TEST);
    if (s->blend) g->Enable(GL_BLEND);
    if (s->depth) g->Enable(GL_DEPTH_TEST);
    if (s->cull) g->Enable(GL_CULL_FACE);
    if (s->srgb) g->Enable(GL_FRAMEBUFFER_SRGB);
}

/* (re)size texture t; the glow texture gets an FBO */
static bool ensure_tex(int t, int w, int h) {
    Gl *g = &G.gl;
    if (G.tw[t] == w && G.th[t] == h) return true;
    g->ActiveTexture(GL_TEXTURE0);
    g->BindTexture(GL_TEXTURE_2D, G.tex[t]);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (t == T_SRC) g->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    else g->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    if (t != T_SRC) {
        g->BindFramebuffer(GL_FRAMEBUFFER, G.fbo[t]);
        g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.tex[t], 0);
        if (g->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "[CRT GPU] framebuffer %d incomplete\n", t);
            return false;
        }
    }
    G.tw[t] = w;
    G.th[t] = h;
    return true;
}

static bool create(SDL_Renderer *ren) {
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(ren, &info) || !info.name || strcmp(info.name, "opengl") ||
        !(info.flags & SDL_RENDERER_TARGETTEXTURE)) {
        fprintf(stderr, "[CRT GPU] renderer is %s, not opengl: CPU filter\n", info.name ? info.name : "?");
        return false;
    }
    /* make SDL's context current by binding a target once */
    SDL_Texture *probe = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 4, 4);
    if (!probe) return false;
    SDL_Texture *old = SDL_GetRenderTarget(ren);
    SDL_SetRenderTarget(ren, probe);
    SDL_RenderFlush(ren);
    bool ok = load_gl(&G.gl);
    G.ctx = SDL_GL_GetCurrentContext();
    if (ok) {
        Gl *g = &G.gl;
        const char *ver = (const char *)g->GetString(GL_VERSION);
        int major = 0, minor = 0;
        if (!ver || sscanf(ver, "%d.%d", &major, &minor) != 2 || major < 3) {
            fprintf(stderr, "[CRT GPU] GL %s too old: CPU filter\n", ver ? ver : "?");
            ok = false;
        } else {
            GlState st;
            state_take(&st);
            const char *frags[P_COUNT] = { k_glow, k_out };
            for (int i = 0; i < P_COUNT && ok; i++) ok = (G.prog[i] = program(frags[i])) != 0;
            if (ok) {
                g->GenTextures(T_COUNT, G.tex);
                g->GenFramebuffers(T_COUNT, G.fbo);
                g->GenVertexArrays(1, &G.vao);
            }
            state_restore(&st);
            if (ok) fprintf(stderr, "[CRT GPU] ready: %s / %s, GL %s\n", g->GetString(GL_VENDOR),
                            g->GetString(GL_RENDERER), ver);
        }
    }
    SDL_SetRenderTarget(ren, old);
    SDL_DestroyTexture(probe);
    G.ren = ren;
    return ok;
}

#define U(p, name) G.gl.GetUniformLocation(G.prog[p], name)

void *crt_gpu_render(void *renderer, const uint32_t *src, int sw, int sh, int dw, int dh, bool curved) {
    (void)curved;
    SDL_Renderer *ren = (SDL_Renderer *)renderer;
    if (G.failed || !ren || !src || sw < 2 || sh < 2 || dw < 2 || dh < 2) return NULL;
    if (!G.tried) {
        G.tried = true;
        if (!create(ren)) { G.failed = true; return NULL; }
    }
    if (ren != G.ren) { G.failed = true; return NULL; }
    if (!G.out || G.out_w != dw || G.out_h != dh) {
        if (G.out) SDL_DestroyTexture(G.out);
        G.out = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, dw, dh);
        if (!G.out) { G.failed = true; return NULL; }
        SDL_SetTextureBlendMode(G.out, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(G.out, SDL_ScaleModeLinear);
        G.out_w = dw;
        G.out_h = dh;
    }
    CrtParams cp;
    crt_params(dw, dh, sw, sh, &cp);
    if (cp.cw < 2 || cp.ch < 2) return NULL;

    SDL_Texture *old = SDL_GetRenderTarget(ren);
    if (SDL_SetRenderTarget(ren, G.out) || SDL_RenderFlush(ren)) { SDL_SetRenderTarget(ren, old); return NULL; }
    if (SDL_GL_GetCurrentContext() != G.ctx) { SDL_SetRenderTarget(ren, old); G.failed = true; return NULL; }
    Gl *g = &G.gl;
    GlState st;
    state_take(&st);
    GLint sdl_fbo = st.fbo;
    while (g->GetError() != GL_NO_ERROR) {}   /* not ours */
    /* The GL name of G.out: SDL has it attached to its target framebuffer. */
    GLint out_name = 0;
    g->GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                           GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &out_name);
    bool ok = ensure_tex(T_SRC, sw, sh) && ensure_tex(T_GLOW, sw, sh);
    if (ok) {
        g->ActiveTexture(GL_TEXTURE0);
        g->BindTexture(GL_TEXTURE_2D, G.tex[T_SRC]);
        g->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        g->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        g->TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_BGRA, GL_UNSIGNED_BYTE, src);
        g->BindVertexArray(G.vao);

        g->UseProgram(G.prog[P_GLOW]);
        g->Uniform1i(U(P_GLOW, "src"), 0);
        g->Uniform2i(U(P_GLOW, "src_size"), sw, sh);
        g->BindFramebuffer(GL_FRAMEBUFFER, G.fbo[T_GLOW]);
        g->Viewport(0, 0, sw, sh);
        g->DrawArrays(GL_TRIANGLES, 0, 3);

        g->UseProgram(G.prog[P_OUT]);
        g->ActiveTexture(GL_TEXTURE1);
        g->BindTexture(GL_TEXTURE_2D, G.tex[T_GLOW]);
        g->Uniform1i(U(P_OUT, "src"), 0);
        g->Uniform1i(U(P_OUT, "glowt"), 1);
        g->Uniform2i(U(P_OUT, "src_size"), sw, sh);
        g->Uniform2i(U(P_OUT, "content"), cp.cw, cp.ch);
        g->Uniform2i(U(P_OUT, "origin"), cp.cx0, cp.cy0);
        const CrtLook *lk = crt_look();
        g->Uniform1f(U(P_OUT, "BMIN"), lk->beam_min);
        g->Uniform1f(U(P_OUT, "BMAX"), lk->beam_max);
        g->Uniform1f(U(P_OUT, "GAIN"), lk->gain);
        g->Uniform1f(U(P_OUT, "GLOW"), lk->glow);
        g->BindFramebuffer(GL_FRAMEBUFFER, (GLuint)sdl_fbo);
        g->Viewport(0, 0, dw, dh);
        g->ClearColor(0, 0, 0, 1);
        g->Clear(GL_COLOR_BUFFER_BIT);
        g->Viewport(cp.cx0, cp.cy0, cp.cw, cp.ch);
        g->DrawArrays(GL_TRIANGLES, 0, 3);
        GLenum err = g->GetError();
        if (err != GL_NO_ERROR) { fprintf(stderr, "[CRT GPU] GL error 0x%x: CPU filter\n", (unsigned)err); ok = false; }
    }
    state_restore(&st);
    /* SDL's OpenGL renderer remembers the texture it last bound on unit 0 and
     * skips rebinding when it draws that texture again. The host draws G.out
     * every frame, so SDL believes it is still bound; but host calls between
     * frames (e.g. SDL_SetTextureScaleMode on its streaming texture, which
     * runs when the filter or display settings change) bind other textures
     * behind SDL's back, and the next copy of G.out then samples the wrong
     * texture: a black screen (seen switching CRT -> CRT Strong). Leave G.out
     * bound on unit 0 so the copy is right whatever SDL believes. */
    if (out_name > 0) {
        GLint active = GL_TEXTURE0;
        g->GetIntegerv(GL_ACTIVE_TEXTURE, &active);
        g->ActiveTexture(GL_TEXTURE0);
        g->BindTexture(GL_TEXTURE_2D, (GLuint)out_name);
        g->ActiveTexture((GLenum)active);
    }
    SDL_SetRenderTarget(ren, old);
    if (!ok) { G.failed = true; return NULL; }
    return G.out;
}

bool crt_gpu_failed(void) { return G.failed; }

#else
void *crt_gpu_render(void *renderer, const uint32_t *src, int sw, int sh, int dw, int dh, bool curved) {
    (void)renderer; (void)src; (void)sw; (void)sh; (void)dw; (void)dh; (void)curved;
    return NULL;
}
bool crt_gpu_failed(void) { return true; }
#endif
