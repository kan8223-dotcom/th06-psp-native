#include "graphics/FixedFunctionGL.hpp"
#include "graphics/PspGuGfx.hpp"
#include "graphics/WebGL.hpp"

GfxInterface *FixedFunctionGL::Init()
{
    // Reuse GameWindow's existing fixed-function slot, but provide PSP GE/GU
    // underneath it.  This keeps the platform decision out of src/.
    return Th06CreatePspGuBackend();
}

GfxInterface *WebGL::Create()
{
    return nullptr;
}
