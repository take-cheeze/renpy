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
 * Implementation of the C shim declared in effekseerio.h, on top of the
 * Effekseer C++ runtime.
 *
 * BUILD: this file is compiled and linked against the Effekseer runtime
 * (libEffekseer + libEffekseerRendererGL) and their headers. The build is
 * gated on the EFFEKSEER environment variable pointing at an Effekseer SDK
 * root; see setup.py. The Effekseer API used here is the stable public API,
 * but small details (enum names, Manager::Update signature) drift between
 * releases, so places that are version-sensitive are marked with TODO.
 */

#include "effekseerio.h"

#include <cstring>
#include <vector>

#include <Effekseer.h>
#include <EffekseerRendererGL.h>

struct renpy_effekseer_context {
    Effekseer::ManagerRef manager;
    EffekseerRendererGL::RendererRef renderer;

    /* Loaded effects, indexed by the id returned from load_effect. Freed
       slots hold a null ref so ids stay stable. */
    std::vector<Effekseer::EffectRef> effects;
};

/* Map the device_type argument (see effekseerio.h) to Effekseer's enum. */
static EffekseerRendererGL::OpenGLDeviceType device_type_of(int device_type) {
    switch (device_type) {
    case 1:
        return EffekseerRendererGL::OpenGLDeviceType::OpenGLES2;
    case 2:
        return EffekseerRendererGL::OpenGLDeviceType::OpenGLES3;
    case 0:
    default:
        // Ren'Py's desktop context is GL 2.x compatibility; OpenGL2 is the
        // matching Effekseer device. Games running on a GL3 context can pass
        // device_type accordingly.
        return EffekseerRendererGL::OpenGLDeviceType::OpenGL2;
    }
}

/*
 * Convert a UTF-8 string to UTF-16, which is what Effekseer's material path
 * wants. Returns an empty vector for NULL/empty input.
 *
 * Effekseer's default texture/model/material loaders resolve an effect's
 * external resources against this path with ordinary filesystem reads, so it
 * must be a real directory on disk. That is why the Python side passes a path
 * from renpy.loader.transfn() rather than a Ren'Py-relative name: a file that
 * lives only inside a .rpa archive has no such path, and loading those needs a
 * custom Effekseer::FileInterface (see effekseerio.h).
 */
static std::vector<char16_t> utf16_of(const char *utf8) {

    std::vector<char16_t> rv;

    if (utf8 == nullptr || *utf8 == '\0') {
        return rv;
    }

    const unsigned char *p = (const unsigned char *) utf8;

    while (*p) {
        unsigned int cp;
        int extra;

        if (*p < 0x80) {
            cp = *p;
            extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            extra = 3;
        } else {
            /* Invalid lead byte; skip it. */
            p++;
            continue;
        }

        p++;

        for (int i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) {
                /* Truncated sequence. */
                cp = 0xFFFD;
                break;
            }
            cp = (cp << 6) | (*p & 0x3F);
            p++;
        }

        if (cp >= 0x10000) {
            cp -= 0x10000;
            rv.push_back((char16_t) (0xD800 + (cp >> 10)));
            rv.push_back((char16_t) (0xDC00 + (cp & 0x3FF)));
        } else {
            rv.push_back((char16_t) cp);
        }
    }

    rv.push_back(u'\0');

    return rv;
}

/* Copy 16 column-major floats into an Effekseer::Matrix44.
 *
 * TODO: confirm the row/column convention against the Effekseer release in
 * use. Effekseer::Matrix44 stores Values[row][col]; Ren'Py's Matrix (see
 * renpy/display/matrix.pyx) is column-major, so a transpose is applied here.
 * If effects appear mirrored or rotated, this is the first thing to check.
 */
static void fill_matrix(Effekseer::Matrix44 &out, const float *m16) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out.Values[row][col] = m16[col * 4 + row];
        }
    }
}

extern "C" renpy_effekseer_context *renpy_effekseer_create(int max_instances, int device_type) {

    renpy_effekseer_context *ctx = new (std::nothrow) renpy_effekseer_context();
    if (ctx == nullptr) {
        return nullptr;
    }

    ctx->renderer = EffekseerRendererGL::Renderer::Create(max_instances, device_type_of(device_type));
    if (ctx->renderer == nullptr) {
        delete ctx;
        return nullptr;
    }

    ctx->manager = Effekseer::Manager::Create(max_instances);
    if (ctx->manager == nullptr) {
        delete ctx;
        return nullptr;
    }

    // Ren'Py works in a right-handed, y-up space; match it so effect authors
    // get predictable orientation.
    ctx->manager->SetCoordinateSystem(Effekseer::CoordinateSystem::RH);

    // Route the renderer's sub-renderers and loaders into the manager.
    ctx->manager->SetSpriteRenderer(ctx->renderer->CreateSpriteRenderer());
    ctx->manager->SetRibbonRenderer(ctx->renderer->CreateRibbonRenderer());
    ctx->manager->SetRingRenderer(ctx->renderer->CreateRingRenderer());
    ctx->manager->SetTrackRenderer(ctx->renderer->CreateTrackRenderer());
    ctx->manager->SetModelRenderer(ctx->renderer->CreateModelRenderer());

    ctx->manager->SetTextureLoader(ctx->renderer->CreateTextureLoader());
    ctx->manager->SetModelLoader(ctx->renderer->CreateModelLoader());
    ctx->manager->SetMaterialLoader(ctx->renderer->CreateMaterialLoader());

    return ctx;
}

extern "C" void renpy_effekseer_destroy(renpy_effekseer_context *ctx) {
    if (ctx == nullptr) {
        return;
    }

    // The RefPtr members release the Effekseer objects as the struct is
    // destroyed.
    delete ctx;
}

extern "C" int renpy_effekseer_load_effect(renpy_effekseer_context *ctx, const void *data, int size, const char *root, float magnification) {
    if (ctx == nullptr) {
        return -1;
    }

    // `root` is the directory Effekseer's default loaders resolve the effect's
    // external textures/models/materials against. It must be a real on-disk
    // directory (see utf16_of above); NULL means the effect is self-contained.
    //
    // TODO: to support effects inside a .rpa archive, install a custom
    // Effekseer::FileInterface backed by Ren'Py's loader, mirroring
    // src/assimpio.cc which bridges a C++ IO interface onto SDL_IOStream.
    std::vector<char16_t> material_path = utf16_of(root);

    Effekseer::EffectRef effect = Effekseer::Effect::Create(
        ctx->manager,
        data,
        size,
        magnification,
        material_path.empty() ? nullptr : material_path.data());
    if (effect == nullptr) {
        return -1;
    }

    // Reuse a freed slot if one exists, otherwise append.
    for (size_t i = 0; i < ctx->effects.size(); i++) {
        if (ctx->effects[i] == nullptr) {
            ctx->effects[i] = effect;
            return (int) i;
        }
    }

    ctx->effects.push_back(effect);
    return (int) (ctx->effects.size() - 1);
}

extern "C" void renpy_effekseer_release_effect(renpy_effekseer_context *ctx, int effect) {
    if (ctx == nullptr) {
        return;
    }

    if (effect < 0 || (size_t) effect >= ctx->effects.size()) {
        return;
    }

    ctx->effects[effect] = nullptr;
}

extern "C" int renpy_effekseer_play(renpy_effekseer_context *ctx, int effect, float x, float y, float z) {
    if (ctx == nullptr) {
        return -1;
    }

    if (effect < 0 || (size_t) effect >= ctx->effects.size() || ctx->effects[effect] == nullptr) {
        return -1;
    }

    return (int) ctx->manager->Play(ctx->effects[effect], x, y, z);
}

extern "C" void renpy_effekseer_stop(renpy_effekseer_context *ctx, int handle) {
    if (ctx != nullptr) {
        ctx->manager->StopEffect((Effekseer::Handle) handle);
    }
}

extern "C" void renpy_effekseer_stop_all(renpy_effekseer_context *ctx) {
    if (ctx != nullptr) {
        ctx->manager->StopAllEffects();
    }
}

extern "C" int renpy_effekseer_exists(renpy_effekseer_context *ctx, int handle) {
    if (ctx == nullptr) {
        return 0;
    }

    return ctx->manager->Exists((Effekseer::Handle) handle) ? 1 : 0;
}

extern "C" void renpy_effekseer_update(renpy_effekseer_context *ctx, float delta_frames) {
    if (ctx == nullptr) {
        return;
    }

    // TODO: newer Effekseer releases take an Effekseer::Manager::UpdateParameter
    // struct; older ones take a float delta. Adapt to the SDK in use.
    Effekseer::Manager::UpdateParameter param;
    param.DeltaFrame = delta_frames;
    ctx->manager->Update(param);
}

extern "C" void renpy_effekseer_set_projection(renpy_effekseer_context *ctx, const float *m16) {
    if (ctx == nullptr) {
        return;
    }

    Effekseer::Matrix44 m;
    fill_matrix(m, m16);
    ctx->renderer->SetProjectionMatrix(m);
}

extern "C" void renpy_effekseer_set_camera(renpy_effekseer_context *ctx, const float *m16) {
    if (ctx == nullptr) {
        return;
    }

    Effekseer::Matrix44 m;
    fill_matrix(m, m16);
    ctx->renderer->SetCameraMatrix(m);
}

extern "C" void renpy_effekseer_draw(renpy_effekseer_context *ctx) {
    if (ctx == nullptr) {
        return;
    }

    // The caller has already bound the target framebuffer and viewport.
    // Effekseer records and restores the GL state it touches inside
    // Begin/EndRendering, but Ren'Py-owned state (bound program, blend
    // equation, active texture) is re-established by Ren'Py on its next draw.
    ctx->renderer->BeginRendering();
    ctx->manager->Draw();
    ctx->renderer->EndRendering();
}
