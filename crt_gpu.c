/* crt_gpu.c - see crt_gpu.h. GL interop follows runner/cyc/cyc_hd_gpu.c:
 * render into an SDL render-target texture with our own program, saving and
 * restoring the GL state SDL's OpenGL renderer relies on.
 *
 * Passes (all row 0 = top; texelFetch, no filtering):
 *   1 VS   sw x ch   scanlines-vertical-interlacing (beams from 3 source lines)
 *   2 DIM  cw x ch   scanlines-horizontal-apply-mask (+ crt-geom curvature)
 *   3 BP   cw x ch   brightpass
 *   4 BV   cw x ch   bloom-vertical
 *   5 out  dw x dh   bloom-horizontal-reconstitute + lcd gamma, in the screen
 *                    rect of an SDL target texture, black around it
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
    X(GLenum, CheckFramebufferStatus, (GLenum))

typedef struct {
#define GL_DECL(ret, name, args) ret(APIENTRY *name) args;
    GLF(GL_DECL)
#undef GL_DECL
} Gl;

enum { P_VS, P_DIM, P_BP, P_BV, P_OUT, P_COUNT };
enum { T_SRC, T_VS, T_DIM, T_BP, T_BV, T_COUNT };

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
    "uniform ivec2 content;\n"
    "uniform int curved;\n"
    "uniform vec3 stretch;\n"
    "const float PI = 3.14159265358979;\n"
    "vec3 lin(vec3 c) { return pow(c, vec3(2.4)); }\n"
    "vec3 srcpx(int x, int y) {\n"
    "    return texelFetch(src, ivec2(clamp(x, 0, src_size.x - 1), clamp(y, 0, src_size.y - 1)), 0).rgb;\n"
    "}\n"
    "const float R = 4.0, D = 1.5, ASP = 0.75;\n"
    "vec2 bkwtrans(vec2 xy) {\n"
    "    float d2 = D * D;\n"
    "    float A = dot(xy, xy) + d2;\n"
    "    float B = 2.0 * (R * (-D) - d2);\n"
    "    float C = d2 + 2.0 * R * D;\n"
    "    float c = (-B - sqrt(max(B * B - 4.0 * A * C, 0.0))) / (2.0 * A);\n"
    "    vec2 p = c * xy / R;\n"
    "    float a = sqrt(max(1.0 - dot(p, p), 0.0));\n"
    "    float r = max(abs(R * acos(a)), 1e-5);\n"
    "    return p * (r / sin(r / R));\n"
    "}\n"
    "/* screen uv -> picture uv (crt-geom), cval = corner coverage */\n"
    "vec2 geom(vec2 uv, out float cval) {\n"
    "    vec2 xy = (uv - 0.5) * ASP * stretch.z + stretch.xy;\n"
    "    vec2 o = bkwtrans(xy) / ASP + 0.5;\n"
    "    vec2 q = min(o, 1.0 - o) * ASP;\n"
    "    float cd = 0.006;\n"
    "    q = cd - min(q, vec2(cd));\n"
    "    cval = clamp((cd - length(q)) * 400.0, 0.0, 1.0);\n"
    "    return o;\n"
    "}\n"
    "/* picture uv for this content pixel; false = off the screen */\n"
    "bool picture_uv(ivec2 p, out vec2 uv, out float cval) {\n"
    "    uv = (vec2(p) + 0.5) / vec2(content);\n"
    "    cval = 1.0;\n"
    "    if (curved != 0) {\n"
    "        uv = geom(uv, cval);\n"
    "        if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0 || cval <= 0.0) return false;\n"
    "    }\n"
    "    return true;\n"
    "}\n";

/* 1: vertical scanlines, sw x ch */
static const char *k_vs =
    "uniform float ph;\n"
    "uniform vec3 conv;\n"
    "uniform vec3 beam;\n"   /* min sigma, max sigma, spot power */
    "vec3 contrib(vec3 d, vec3 c) {\n"
    "    vec3 sigma = beam.x + (beam.y - beam.x) * pow(c, vec3(beam.z));\n"
    "    vec3 alpha = sqrt(2.0) * sigma;\n"
    "    vec3 scale = c / (alpha * sqrt(PI));\n"
    "    float off = ph / 3.0;\n"
    "    vec3 a = d / alpha, b = (d + off) / alpha, e = abs(d - off) / alpha;\n"
    "    return scale / 3.0 * (exp(-a * a) + exp(-b * b) + exp(-e * e)) * 0.5;\n"
    "}\n"
    "vec3 line(int x, int y) { return (y < 0 || y >= src_size.y) ? vec3(0.0) : lin(srcpx(x, y)); }\n"
    "void main() {\n"
    "    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
    "    float curr = (float(y) + 0.5) * float(src_size.y) / float(content.y);\n"
    "    int idx = int(floor(curr - 0.4995));\n"
    "    float dist = curr - (float(idx) + 0.5);\n"
    "    bool dround = dist >= 0.5;\n"
    "    vec3 d2 = vec3(dist) - conv;\n"
    "    vec3 s = contrib(d2, line(x, idx)) + contrib(abs(1.0 - d2), line(x, idx + 1));\n"
    "    s += dround ? contrib(2.0 - d2, line(x, idx + 2)) : contrib(d2 + 1.0, line(x, idx - 1));\n"
    "    frag = vec4(s, 1.0);\n"
    "}\n";

/* 2: horizontal + mask (+ curvature), cw x ch */
static const char *k_dim =
    "uniform sampler2D vs;\n"
    "uniform vec3 mask[3];\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    vec2 uv; float cval;\n"
    "    if (!picture_uv(p, uv, cval)) { frag = vec4(0.0); return; }\n"
    "    int sw = src_size.x;\n"
    "    float curr = uv.x * float(sw);\n"
    "    int i = int(floor(curr - 0.4995));\n"
    "    float t = clamp(curr - (float(i) + 0.5), 0.0, 1.0);\n"
    "    float w = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);\n"
    "    if (i < 0) { i = 0; w = 0.0; }\n"
    "    if (i > sw - 2) i = max(sw - 2, 0);\n"
    "    int row = curved != 0 ? min(int(uv.y * float(content.y)), content.y - 1) : p.y;\n"
    "    vec3 a = texelFetch(vs, ivec2(i, row), 0).rgb;\n"
    "    vec3 b = texelFetch(vs, ivec2(min(i + 1, sw - 1), row), 0).rgb;\n"
    "    vec3 col = max(mix(a, b, w), 0.0);\n"
    "    frag = vec4(col * mask[p.x % 3] * cval, 1.0);\n"
    "}\n";

/* 3: brightpass, cw x ch */
static const char *k_bp =
    "uniform sampler2D dimt;\n"
    "uniform float undim, cweight, contrast;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    vec3 dim = texelFetch(dimt, p, 0).rgb;\n"
    "    vec2 uv; float cval;\n"
    "    if (!picture_uv(p, uv, cval)) { frag = vec4(0.0); return; }\n"
    "    int ox = int(uv.x * float(src_size.x));\n"
    "    int oy = curved != 0 ? int(uv.y * float(src_size.y))\n"
    "                         : int((float(p.y) + 0.5) * float(src_size.y) / float(content.y));\n"
    "    vec3 orig = contrast * lin(srcpx(ox, oy));\n"
    "    vec3 inten = dim * undim;\n"
    "    vec3 area = max(orig - cweight * inten, 0.0);\n"
    "    vec3 safe = max(inten, vec3(1e-20));\n"
    "    vec3 ratio = clamp(((1.0 - area) / safe - 1.0) / (cweight - 1.0), 0.0, 1.0);\n"
    "    frag = vec4(dim * ratio, 1.0);\n"
    "}\n";

/* 4: bloom vertical, cw x ch */
static const char *k_bv =
    "uniform sampler2D bpt;\n"
    "uniform float bw[5];\n"
    "vec3 at(ivec2 p) { return texelFetch(bpt, ivec2(p.x, clamp(p.y, 0, content.y - 1)), 0).rgb; }\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    vec3 s = bw[0] * at(p);\n"
    "    for (int k = 1; k < 5; k++) s += bw[k] * (at(p - ivec2(0, k)) + at(p + ivec2(0, k)));\n"
    "    frag = vec4(s, 1.0);\n"
    "}\n";

/* 5: bloom horizontal + reconstitute + lcd gamma, into the screen rect */
static const char *k_out =
    "uniform sampler2D dimt, bpt, bvt;\n"
    "uniform float bw[5];\n"
    "uniform float undim;\n"
    "uniform ivec2 origin;\n"
    "vec3 at(ivec2 p) { return texelFetch(bvt, ivec2(clamp(p.x, 0, content.x - 1), p.y), 0).rgb; }\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy) - origin;\n"
    "    vec3 bl = bw[0] * at(p);\n"
    "    for (int k = 1; k < 5; k++) bl += bw[k] * (at(p - ivec2(k, 0)) + at(p + ivec2(k, 0)));\n"
    "    vec3 v = (texelFetch(dimt, p, 0).rgb - texelFetch(bpt, p, 0).rgb + bl) * undim;\n"
    "    frag = vec4(pow(clamp(v, 0.0, 1.0), vec3(1.0 / 2.4)), 1.0);\n"
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

/* (re)size texture t; intermediate textures get an FBO */
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
            const char *frags[P_COUNT] = { k_vs, k_dim, k_bp, k_bv, k_out };
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

static void use(int p, int tex_units[][2], int n) {
    Gl *g = &G.gl;
    g->UseProgram(G.prog[p]);
    for (int i = 0; i < n; i++) {
        g->ActiveTexture(GL_TEXTURE0 + tex_units[i][1]);
        g->BindTexture(GL_TEXTURE_2D, G.tex[tex_units[i][0]]);
    }
}

#define U(p, name) G.gl.GetUniformLocation(G.prog[p], name)

static void common_uniforms(int p, int sw, int sh, const CrtParams *cp, bool curved) {
    Gl *g = &G.gl;
    g->Uniform1i(U(p, "src"), 0);
    g->Uniform2i(U(p, "src_size"), sw, sh);
    g->Uniform2i(U(p, "content"), cp->cw, cp->ch);
    g->Uniform1i(U(p, "curved"), curved ? 1 : 0);
    g->Uniform3f(U(p, "stretch"), cp->stretch[0], cp->stretch[1], cp->stretch[2]);
}

static void pass_to(int t, int w, int h) {
    G.gl.BindFramebuffer(GL_FRAMEBUFFER, G.fbo[t]);
    G.gl.Viewport(0, 0, w, h);
    G.gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

void *crt_gpu_render(void *renderer, const uint32_t *src, int sw, int sh, int dw, int dh, bool curved) {
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
    bool ok = ensure_tex(T_SRC, sw, sh) && ensure_tex(T_VS, sw, cp.ch) && ensure_tex(T_DIM, cp.cw, cp.ch) &&
              ensure_tex(T_BP, cp.cw, cp.ch) && ensure_tex(T_BV, cp.cw, cp.ch);
    if (ok) {
        g->ActiveTexture(GL_TEXTURE0);
        g->BindTexture(GL_TEXTURE_2D, G.tex[T_SRC]);
        g->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        g->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        g->TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_BGRA, GL_UNSIGNED_BYTE, src);
        g->BindVertexArray(G.vao);

        int u_vs[][2] = { { T_SRC, 0 } };
        use(P_VS, u_vs, 1);
        common_uniforms(P_VS, sw, sh, &cp, curved);
        g->Uniform1f(U(P_VS, "ph"), (float)sh / cp.ch);
        g->Uniform3f(U(P_VS, "conv"), cp.conv_y[0], cp.conv_y[1], cp.conv_y[2]);
        g->Uniform3f(U(P_VS, "beam"), cp.beam_min, cp.beam_max, cp.beam_power);
        pass_to(T_VS, sw, cp.ch);

        int u_dim[][2] = { { T_SRC, 0 }, { T_VS, 1 } };
        use(P_DIM, u_dim, 2);
        common_uniforms(P_DIM, sw, sh, &cp, curved);
        g->Uniform1i(U(P_DIM, "vs"), 1);
        g->Uniform3fv(U(P_DIM, "mask"), 3, &cp.mask[0][0]);
        pass_to(T_DIM, cp.cw, cp.ch);

        int u_bp[][2] = { { T_SRC, 0 }, { T_DIM, 1 } };
        use(P_BP, u_bp, 2);
        common_uniforms(P_BP, sw, sh, &cp, curved);
        g->Uniform1i(U(P_BP, "dimt"), 1);
        g->Uniform1f(U(P_BP, "undim"), cp.undim);
        g->Uniform1f(U(P_BP, "cweight"), cp.center_weight);
        g->Uniform1f(U(P_BP, "contrast"), cp.contrast);
        pass_to(T_BP, cp.cw, cp.ch);

        int u_bv[][2] = { { T_SRC, 0 }, { T_BP, 1 } };
        use(P_BV, u_bv, 2);
        common_uniforms(P_BV, sw, sh, &cp, curved);
        g->Uniform1i(U(P_BV, "bpt"), 1);
        g->Uniform1fv(U(P_BV, "bw"), 5, cp.bloom_w);
        pass_to(T_BV, cp.cw, cp.ch);

        int u_out[][2] = { { T_SRC, 0 }, { T_DIM, 1 }, { T_BP, 2 }, { T_BV, 3 } };
        use(P_OUT, u_out, 4);
        common_uniforms(P_OUT, sw, sh, &cp, curved);
        g->Uniform1i(U(P_OUT, "dimt"), 1);
        g->Uniform1i(U(P_OUT, "bpt"), 2);
        g->Uniform1i(U(P_OUT, "bvt"), 3);
        g->Uniform1fv(U(P_OUT, "bw"), 5, cp.bloom_w);
        g->Uniform1f(U(P_OUT, "undim"), cp.undim);
        g->Uniform2i(U(P_OUT, "origin"), cp.cx0, cp.cy0);
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
