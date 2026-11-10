/* crt_gpu.h - the crt_filter.c CRT (crt-royale-fast port) as OpenGL shaders.
 *
 * Runs on the GPU through SDL's OpenGL renderer (the hook asks SDL for it):
 * the game canvas is uploaded to a texture, the passes render through
 * framebuffers of their own, and the last pass draws into an SDL
 * render-target texture that the host presents (CycHostExtras.present_texture).
 * Same geometry and math as crt_filter.c, so crt_map_point still maps the
 * mouse. Returns NULL when OpenGL is unavailable; the caller then falls back
 * to the CPU filter.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* renderer: SDL_Renderer*. Renders src (sw x sh ARGB) as a dw x dh picture
 * (the screen fitted to the src aspect, black outside) and returns the
 * SDL_Texture* holding it, or NULL. */
void *crt_gpu_render(void *renderer, const uint32_t *src, int sw, int sh, int dw, int dh, bool curved);
/* True once creation failed for good (use the CPU filter). */
bool crt_gpu_failed(void);
