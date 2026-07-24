# Copyright 2004-2026 Tom Rothamel <pytom@bishoujo.us>
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

# This is the native side of Ren'Py's Effekseer integration ("route A"):
#
# - EffekseerManager wraps an Effekseer manager + GL renderer (see
#   src/effekseerio.cc).
# - EffekseerModel is a leaf in the Render tree. It owns an offscreen
#   framebuffer whose color texture it *is* (it subclasses GLTexture, so the
#   renpy.texture shader samples EffekseerModel.number directly). Its load()
#   -- called by GL2Draw.load_all_textures during draw_screen, when the GL
#   context is current (see renpy/gl2/gl2draw.pyx) -- binds that framebuffer
#   and lets Effekseer draw into it, then restores Ren'Py's framebuffer. The
#   resulting texture is then composited by Ren'Py's normal pipeline.
#
# Effekseer keeps its own GL state inside its framebuffer, so this never
# perturbs the framebuffer or blend state Ren'Py relies on.

from libc.string cimport memcpy

from renpy.uguu.gl cimport *
from renpy.gl2.gl2texture cimport GLTexture
from renpy.gl2.gl2mesh2 cimport Mesh2

import renpy


cdef extern from "effekseerio.h":
    ctypedef struct renpy_effekseer_context:
        pass

    renpy_effekseer_context *renpy_effekseer_create(int max_instances, int device_type)
    void renpy_effekseer_destroy(renpy_effekseer_context *ctx)
    int renpy_effekseer_load_effect(renpy_effekseer_context *ctx, const void *data, int size, const char *root, float magnification)
    void renpy_effekseer_release_effect(renpy_effekseer_context *ctx, int effect)
    int renpy_effekseer_play(renpy_effekseer_context *ctx, int effect, float x, float y, float z)
    void renpy_effekseer_stop(renpy_effekseer_context *ctx, int handle)
    void renpy_effekseer_stop_all(renpy_effekseer_context *ctx)
    int renpy_effekseer_exists(renpy_effekseer_context *ctx, int handle)
    void renpy_effekseer_update(renpy_effekseer_context *ctx, float delta_frames)
    void renpy_effekseer_set_projection(renpy_effekseer_context *ctx, const float *m16)
    void renpy_effekseer_set_camera(renpy_effekseer_context *ctx, const float *m16)
    void renpy_effekseer_draw(renpy_effekseer_context *ctx)


cdef void fill16(float *out, seq) except *:
    """
    Copies a length-16 sequence of numbers into `out`.
    """

    if len(seq) != 16:
        raise ValueError("Expected 16 matrix values, got {}.".format(len(seq)))

    cdef int i
    for i in range(16):
        out[i] = seq[i]


cdef class EffekseerManager:
    """
    Wraps an Effekseer manager and GL renderer. One of these is shared by all
    the effects that play together (they share a simulation and a draw pass).
    """

    cdef renpy_effekseer_context *context

    def __init__(EffekseerManager self, int max_instances=2000, int device_type=0):
        # device_type: 0 = OpenGL 2/3, 1 = OpenGL ES 2, 2 = OpenGL ES 3.
        # Must match the context Ren'Py created; see EffekseerManager.create().
        self.context = renpy_effekseer_create(max_instances, device_type)

        if self.context == NULL:
            raise Exception("Could not create the Effekseer manager.")

    def __dealloc__(EffekseerManager self):
        if self.context != NULL:
            renpy_effekseer_destroy(self.context)
            self.context = NULL

    def load_effect(EffekseerManager self, data, root=None, float magnification=1.0):
        """
        Loads an effect from `data` (the bytes of a .efkefc/.efk file). Returns
        a non-negative effect id.
        """

        if not isinstance(data, bytes):
            data = bytes(data)

        cdef const char *raw = data
        cdef const char *croot = NULL
        cdef bytes broot

        if root is not None:
            broot = root.encode("utf-8")
            croot = broot

        cdef int rv = renpy_effekseer_load_effect(self.context, <const void *> raw, len(data), croot, magnification)

        if rv < 0:
            raise Exception("Could not load the Effekseer effect.")

        return rv

    def release_effect(EffekseerManager self, int effect):
        renpy_effekseer_release_effect(self.context, effect)

    def play(EffekseerManager self, int effect, float x=0.0, float y=0.0, float z=0.0):
        cdef int rv = renpy_effekseer_play(self.context, effect, x, y, z)

        if rv < 0:
            raise Exception("Could not play the Effekseer effect.")

        return rv

    def stop(EffekseerManager self, int handle):
        renpy_effekseer_stop(self.context, handle)

    def stop_all(EffekseerManager self):
        renpy_effekseer_stop_all(self.context)

    def exists(EffekseerManager self, int handle):
        return bool(renpy_effekseer_exists(self.context, handle))

    def update(EffekseerManager self, float delta_frames):
        """
        Advances the simulation by `delta_frames` (seconds * 60). CPU-only.
        """

        renpy_effekseer_update(self.context, delta_frames)

    def set_projection(EffekseerManager self, matrix):
        cdef float m[16]
        fill16(m, matrix)
        renpy_effekseer_set_projection(self.context, m)

    def set_camera(EffekseerManager self, matrix):
        cdef float m[16]
        fill16(m, matrix)
        renpy_effekseer_set_camera(self.context, m)


cdef class EffekseerModel(GLTexture):
    """
    A Render-tree leaf that presents an Effekseer draw pass as a texture.

    It subclasses GLTexture so that Ren'Py's renpy.texture shader samples its
    `number` (the framebuffer's color attachment) with no extra shader work.
    """

    # The Effekseer manager whose current frame this model draws.
    cdef EffekseerManager manager

    # Our offscreen framebuffer (0 until allocated).
    cdef GLuint fbo

    def __init__(EffekseerModel self, EffekseerManager manager, int width, int height):
        loader = renpy.display.draw.texture_loader
        GLTexture.__init__(self, (width, height), loader)

        self.manager = manager
        self.fbo = 0

        # Full-surface quad. The texture coords are flipped vertically because
        # the framebuffer's origin is bottom-left while Ren'Py samples from the
        # top-left.
        self.mesh = Mesh2.texture_rectangle(
            0.0, 0.0, width, height,
            0.0, 1.0, 1.0, 0.0,
            )

        self.properties = { "mipmap" : False }

        # We manage our own GL texture and framebuffer, so mark ourselves
        # loaded and let load() below drive the per-frame render.
        self.loaded = True

    cdef void allocate(EffekseerModel self) except *:
        """
        Lazily creates the color texture and framebuffer. Must run with the GL
        context current.
        """

        if self.fbo:
            return

        cdef GLuint tex = 0
        cdef GLuint fbo = 0

        glGenTextures(1, &tex)
        glBindTexture(GL_TEXTURE_2D, tex)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, self.width, self.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE)

        glGenFramebuffers(1, &fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0)

        # TODO: Effekseer effects with depth-tested geometry (3D models) need a
        # depth attachment here. 2D sprite/ribbon effects do not.

        self.number = tex
        self.fbo = fbo
        self.loader.allocated.add(tex)

    def load(EffekseerModel self):
        """
        Called by GL2Draw.load_all_textures during draw_screen, with the GL
        context current. Renders the manager's current frame into our
        framebuffer.
        """

        cdef GLint prev_fbo = 0
        cdef GLint prev_viewport[4]

        if self.manager is None or self.manager.context == NULL:
            return

        self.allocate()

        # Save the framebuffer and viewport Ren'Py had bound.
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo)
        glGetIntegerv(GL_VIEWPORT, prev_viewport)

        # Render Effekseer into our framebuffer.
        glBindFramebuffer(GL_FRAMEBUFFER, self.fbo)
        glViewport(0, 0, self.width, self.height)
        glClearColor(0.0, 0.0, 0.0, 0.0)
        glClear(GL_COLOR_BUFFER_BIT)

        renpy_effekseer_draw(self.manager.context)

        # Restore Ren'Py's framebuffer and viewport.
        glBindFramebuffer(GL_FRAMEBUFFER, <GLuint> prev_fbo)
        glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3])

        # NOTE: Effekseer draws with straight (non-premultiplied) alpha, while
        # Ren'Py composites premultiplied. A follow-up should premultiply the
        # result -- either by rendering through a small premultiply shader here
        # or by giving EffekseerModel a shader other than renpy.texture.

    def __dealloc__(EffekseerModel self):
        # Best-effort: the framebuffer is freed here, the color texture through
        # GLTexture's normal accounting. Deleting GL objects requires the
        # context; on shutdown this may be a no-op.
        # TODO: defer framebuffer deletion to GL2Draw's per-frame free list, as
        # is done for textures, so deletion always happens with the context
        # current.
        if self.fbo:
            glDeleteFramebuffers(1, &self.fbo)
            self.fbo = 0
