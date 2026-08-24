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

# The Python side of Ren'Py's Effekseer integration ("route A"): a Displayable
# that owns an Effekseer manager, advances its simulation by the shown-time
# `st`, and presents the result as an ordinary texture (see
# renpy/gl2/effekseermodel.pyx for the native/GL side).
#
# This module is import-safe even when the native module has not been built
# (the default build does not build it -- see setup.py, gated on the EFFEKSEER
# environment variable). Constructing an Effect without the native module
# raises a clear error.

from __future__ import division, absolute_import, with_statement, print_function, unicode_literals

import math
import os

import renpy

try:
    import renpy.gl2.effekseermodel as effekseermodel
except ImportError:
    effekseermodel = None


did_init = False


def init():
    """
    Initializes Effekseer, raising an exception if it has not been built.
    """

    global did_init

    if did_init:
        return

    if effekseermodel is None:
        raise Exception("Effekseer has not been built. Rebuild Ren'Py with the EFFEKSEER environment variable set.")

    did_init = True


def perspective_matrix(fovy, aspect, near, far):
    """
    Returns a column-major perspective projection matrix as a list of 16
    floats. `fovy` is in degrees.

    TODO: the projection and camera together determine how effect-space
    coordinates map onto the displayable. These defaults make a unit-scale
    effect roughly fill the displayable; real games will want to tune them (or
    expose them) to match how their effects were authored.
    """

    f = 1.0 / math.tan(math.radians(fovy) / 2.0)

    return [
        f / aspect, 0.0, 0.0, 0.0,
        0.0, f, 0.0, 0.0,
        0.0, 0.0, (far + near) / (near - far), -1.0,
        0.0, 0.0, (2.0 * far * near) / (near - far), 0.0,
    ]


def translation_matrix(x, y, z):
    """
    Returns a column-major translation matrix as a list of 16 floats.
    """

    return [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        x, y, z, 1.0,
    ]


class Effect(renpy.display.displayable.Displayable):
    """
    A displayable that plays a single Effekseer effect.

    `filename`
        The .efkefc (or .efk) file to play, loaded through Ren'Py's loader.

    `size`
        The (width, height) of the render target and displayable, in pixels.
        Defaults to the game's screen size.

    `loop`
        If true, the effect is replayed whenever it finishes.

    `magnification`
        A uniform scale applied to the effect when it is loaded.
    """

    _duplicatable = False

    def __init__(self, filename, size=None, loop=False, magnification=1.0, camera_distance=10.0, fov=45.0, **properties):
        super(Effect, self).__init__(**properties)

        self.filename = filename
        self.loop = loop
        self.magnification = magnification
        self.camera_distance = camera_distance
        self.fov = fov

        # Resolved lazily in ensure_loaded() so the screen-size default is read
        # at render time, not when the displayable is defined.
        self.size = size

        # Lazily created on first render (needs the GL context).
        self.manager = None
        self.effect = None
        self.handle = None
        self.model = None

        # The shown-time of the previous render, used to compute the
        # simulation delta.
        self.last_st = 0.0

    def ensure_loaded(self):
        """
        Creates the manager, effect and render model on first use. Runs during
        render(), i.e. with the GL context current.
        """

        if self.model is not None:
            return

        init()

        if self.size is None:
            self.size = (renpy.config.screen_width, renpy.config.screen_height)

        # 0 = OpenGL 2/3, 2 = OpenGL ES 3. Match the context Ren'Py created.
        device_type = 2 if getattr(renpy.display.draw, "gles", False) else 0

        self.manager = effekseermodel.EffekseerManager(device_type=device_type)

        with renpy.loader.load(self.filename) as f:
            data = f.read()

        # An .efkefc file does not embed its textures/models; Effekseer's
        # default loaders read them from disk, relative to a material path. So
        # resolve the effect to a real filesystem path and hand over its
        # directory. transfn() raises for a file that exists only inside a .rpa
        # archive -- such effects need the FileInterface bridge noted in
        # src/effekseerio.cc, so report that clearly rather than failing later
        # with missing textures.
        root = None

        try:
            root = os.path.dirname(renpy.loader.transfn(self.filename))
        except Exception:
            if renpy.config.developer:
                raise Exception(
                    "The Effekseer effect {!r} is not an unpacked file. Effekseer resolves its "
                    "textures on disk, so the effect and its resources must not be archived.".format(self.filename)
                    )

        self.effect = self.manager.load_effect(data, root=root, magnification=self.magnification)
        self.handle = self.manager.play(self.effect)

        width, height = self.size
        self.model = effekseermodel.EffekseerModel(self.manager, width, height)

    def render(self, width, height, st, at):
        self.ensure_loaded()

        # Advance the simulation by the elapsed time. Effekseer's timeline is
        # in 60fps frames.
        dt = st - self.last_st
        self.last_st = st

        if dt < 0:
            # st was reset (the displayable was reshown); restart.
            dt = 0.0
            self.handle = self.manager.play(self.effect)

        self.manager.update(dt * 60.0)

        # Loop if requested and the instance has finished.
        if self.loop and not self.manager.exists(self.handle):
            self.handle = self.manager.play(self.effect)

        # Set the projection and camera used by the draw pass (which happens
        # later, in EffekseerModel.load()).
        sw, sh = self.size
        aspect = 1.0 * sw / sh
        self.manager.set_projection(perspective_matrix(self.fov, aspect, 1.0, 500.0))
        self.manager.set_camera(translation_matrix(0.0, 0.0, -self.camera_distance))

        # Keep animating.
        renpy.display.render.redraw(self, 0)

        rv = renpy.exports.Render(sw, sh)
        rv.blit(self.model, (0, 0))

        return rv

    def visit(self):
        return []
