#pragma once

#include "graphics/GfxInterface.hpp"
#include <SDL2/SDL_surface.h>

// Native PSP GE backend.  GameWindow selects this through the legacy
// FixedFunctionGL slot so the inherited engine does not need a PSP branch.
GfxInterface *Th06CreatePspGuBackend();

// Static 640x480/640x780 surface caches are useful while a menu or staff roll
// redraws the same image, but must not occupy PSP-1000 RAM throughout gameplay.
// Called only at the synchronous GameManager registration boundary.
extern "C" unsigned int th06_psp_ge_trim_surface_cache();

// Complete pending GE work, then release only recycled, unowned pixel blocks.
// MIDI scene boundaries reuse this standby WAV texture budget for decoding.
extern "C" unsigned int th06_psp_ge_release_free_texture_pool();

// Full-resolution JPEG decode directly into the normal filtered GE RGB565
// cache. The returned logical RGB24 SDL surface borrows no CPU pixel buffer.
extern "C" SDL_Surface *th06_psp_ge_load_jpeg_cached(const unsigned char *data, std::size_t size);

// A WAV-loaded immutable JPEG can keep its already rendered cache when the
// title switches to MIDI, releasing only the redundant full RGB source.
extern "C" unsigned int th06_psp_ge_promote_jpeg_cache_only(SDL_Surface *surface);

#if defined(TH06_PSP_RECORD_LAYOUT)
// Test-only, read-only ownership metadata at an already synchronous scene
// boundary. Does not flush/sync GE, inspect pixels, or allocate/free memory.
extern "C" void th06_psp_ge_dump_texture_layout(const char *scene);
#endif
