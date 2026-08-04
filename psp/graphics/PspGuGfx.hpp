#pragma once

#include "graphics/GfxInterface.hpp"

// Native PSP GE backend.  GameWindow selects this through the legacy
// FixedFunctionGL slot so the inherited engine does not need a PSP branch.
GfxInterface *Th06CreatePspGuBackend();

// Static 640x480/640x780 surface caches are useful while a menu or staff roll
// redraws the same image, but must not occupy PSP-1000 RAM throughout gameplay.
// Called only at the synchronous GameManager registration boundary.
extern "C" unsigned int th06_psp_ge_trim_surface_cache();
