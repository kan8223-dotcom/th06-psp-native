#pragma once

#include "graphics/GfxInterface.hpp"

// GameWindow still lists this legacy fixed-function slot when TH06_PS3 is not
// set. PSP shadows the header and routes the slot to its native GE backend,
// keeping the inherited engine source unchanged.
struct FixedFunctionGL
{
    static GfxInterface *Init();
};
