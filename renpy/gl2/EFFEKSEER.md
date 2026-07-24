# Effekseer integration (route A)

This is the initial scaffold for playing [Effekseer](https://effekseer.github.io/)
particle effects inside Ren'Py. It uses the **"route A"** strategy: Effekseer
renders into its *own* offscreen framebuffer, and Ren'Py composites the
resulting texture like any other image. Effekseer's OpenGL state therefore
stays sandboxed inside that framebuffer and never perturbs the framebuffer,
blend state, or shader program that Ren'Py's GL2 renderer relies on.

This is deliberately the lowest-risk of the possible integrations. The two
alternatives — letting Effekseer draw straight into Ren'Py's main framebuffer,
or extracting particle geometry into Ren'Py meshes — are both harder and were
set aside for now.

## Pieces

| File | Role |
| --- | --- |
| `src/effekseerio.h` / `src/effekseerio.cc` | A small C shim over the Effekseer C++ runtime (`Effekseer::Manager` + `EffekseerRendererGL::Renderer`). It never touches the framebuffer — it only issues Effekseer's draw calls into whatever framebuffer the caller has bound. |
| `renpy/gl2/effekseermodel.pyx` | `EffekseerManager` (wraps the shim) and `EffekseerModel`, a Render-tree leaf that owns the offscreen framebuffer and presents its color texture as an ordinary Ren'Py texture. |
| `renpy/gl2/effekseer.py` | The `Effekseer` displayable (`class Effect`): loads an effect, advances the simulation by the shown-time `st`, and schedules redraws. |
| `setup.py` | Build wiring, gated on the `EFFEKSEER` environment variable. |

## How a frame flows

1. `Effect.render(w, h, st, at)` runs while the tree is built. It advances the
   Effekseer simulation on the CPU (`manager.update(dt * 60)`), sets the
   projection/camera, calls `renpy.display.render.redraw()` to keep animating,
   and blits a persistent `EffekseerModel` into the returned `Render`.
2. Later, inside `GL2Draw.draw_screen()`, `load_all_textures()` reaches the
   `EffekseerModel` leaf and calls its `load()` — this is the one place where
   the GL context is guaranteed current (`renpy/gl2/gl2draw.pyx`, the
   `isinstance(what, GL2Model): what.load()` branch).
3. `EffekseerModel.load()` binds its own framebuffer, clears it, calls
   `renpy_effekseer_draw()` (Effekseer issues its GL draws here), then restores
   Ren'Py's framebuffer/viewport.
4. `EffekseerModel` subclasses `GLTexture`, so its `get_texture(0)` returns
   `self` and the stock `renpy.texture` shader samples the framebuffer's color
   texture. Ren'Py composites it normally.

## Building

Effekseer is not built by default. Point `EFFEKSEER` at a runtime root that has
an `include/` directory (Effekseer headers) and a `lib/` directory
(`libEffekseer`, `libEffekseerRendererGL`), then rebuild:

```
EFFEKSEER=/path/to/effekseer python setup.py build_ext --inplace
```

## Using it (once built)

```renpy
image fireworks = Effekseer("fireworks.efkefc", loop=True)

label start:
    show fireworks
    "..."
```

## Known follow-ups (marked as TODO in the code)

- **Premultiplied alpha.** Effekseer draws with straight alpha; Ren'Py
  composites premultiplied. The result should be premultiplied, either with a
  small shader on `EffekseerModel` or a premultiply pass in `load()`.
- **Projection / camera.** `effekseer.py` ships placeholder perspective +
  camera matrices. They need to be matched to how effects were authored (and
  probably exposed as parameters), and the matrix row/column convention in
  `fill_matrix()` (`src/effekseerio.cc`) confirmed against the SDK in use.
- **Dependency loading.** External textures/models/materials referenced by an
  effect are not yet routed through Ren'Py's loader. Bridge an Effekseer
  `FileInterface` onto `SDL_IOStream`, as `src/assimpio.cc` does for Assimp.
- **Resource cleanup.** The framebuffer is freed in `__dealloc__`; it should be
  deferred to `GL2Draw`'s per-frame free list so deletion always happens with
  the context current.
- **`Manager::Update` / device-type / enum names** vary across Effekseer
  releases; adapt the shim to the SDK version.
