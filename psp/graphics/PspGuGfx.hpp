#pragma once

#include "graphics/GfxInterface.hpp"

// Native PSP GE backend.  GameWindow selects this through the legacy
// FixedFunctionGL slot so the inherited engine does not need a PSP branch.
GfxInterface *Th06CreatePspGuBackend();
