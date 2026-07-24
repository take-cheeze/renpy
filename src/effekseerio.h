/*
Copyright 2004-2026 Tom Rothamel <pytom@bishoujo.us>

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
 * C shim exposing a minimal, stable interface to the Effekseer C++ runtime
 * (Effekseer::Manager + EffekseerRendererGL::Renderer) so that it can be
 * called from Cython (renpy/gl2/effekseermodel.pyx).
 *
 * This is the "route A" integration: Effekseer owns its own OpenGL draw
 * calls, but it renders into a framebuffer that the Cython side binds. The
 * Cython side then presents the resulting color texture as an ordinary
 * Ren'Py texture. This shim therefore never touches the framebuffer or the
 * GL state that Ren'Py manages -- it only issues Effekseer's draw calls into
 * whatever framebuffer/viewport is bound when renpy_effekseer_draw() is
 * called.
 */

#ifndef RENPY_EFFEKSEERIO_H
#define RENPY_EFFEKSEERIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to an Effekseer manager together with its GL renderer. */
typedef struct renpy_effekseer_context renpy_effekseer_context;

/*
 * Create an Effekseer manager and a GL renderer.
 *
 * `max_instances` bounds the number of simultaneously-drawable sprites.
 * `device_type` selects the GL device family: 0 for OpenGL 2/3 (desktop),
 * 1 for OpenGL ES 2, 2 for OpenGL ES 3. This must match the context Ren'Py
 * created (see renpy/gl2/gl2draw.pyx select_gl_attributes()).
 *
 * Must be called with the GL context current. Returns NULL on failure.
 */
renpy_effekseer_context *renpy_effekseer_create(int max_instances, int device_type);

/* Destroy a context. Must be called with the GL context current. */
void renpy_effekseer_destroy(renpy_effekseer_context *ctx);

/*
 * Load an effect from an in-memory copy of a .efkefc / .efk file.
 *
 * `root` is a filesystem or virtual directory used to resolve external
 * dependencies (textures, models, materials); it may be NULL if the effect
 * is self-contained. `magnification` scales the effect uniformly.
 *
 * Returns a non-negative effect id, or -1 on failure.
 */
int renpy_effekseer_load_effect(renpy_effekseer_context *ctx, const void *data, int size, const char *root, float magnification);

/* Release a previously loaded effect. */
void renpy_effekseer_release_effect(renpy_effekseer_context *ctx, int effect);

/*
 * Start playing `effect` at world position (x, y, z). Returns a non-negative
 * play handle that can be passed to stop/exists, or -1 on failure.
 */
int renpy_effekseer_play(renpy_effekseer_context *ctx, int effect, float x, float y, float z);

/* Stop a playing instance. */
void renpy_effekseer_stop(renpy_effekseer_context *ctx, int handle);

/* Stop every playing instance. */
void renpy_effekseer_stop_all(renpy_effekseer_context *ctx);

/* Returns 1 if the instance identified by `handle` is still alive. */
int renpy_effekseer_exists(renpy_effekseer_context *ctx, int handle);

/*
 * Advance the simulation. Effekseer's timeline is measured in 60fps frames,
 * so `delta_frames` is seconds_elapsed * 60. This is pure CPU work and may be
 * called without the GL context being current.
 */
void renpy_effekseer_update(renpy_effekseer_context *ctx, float delta_frames);

/*
 * Set the projection and camera (view) matrices. Both are 16 floats in
 * column-major order, matching Effekseer::Matrix44 / Ren'Py's Matrix.
 */
void renpy_effekseer_set_projection(renpy_effekseer_context *ctx, const float *m16);
void renpy_effekseer_set_camera(renpy_effekseer_context *ctx, const float *m16);

/*
 * Issue Effekseer's GL draw calls into the currently-bound framebuffer and
 * viewport. The caller (effekseermodel.pyx) is responsible for binding the
 * target framebuffer and viewport before this call and for restoring any GL
 * state it cares about afterwards. Must be called with the GL context current.
 */
void renpy_effekseer_draw(renpy_effekseer_context *ctx);

#ifdef __cplusplus
}
#endif

#endif
