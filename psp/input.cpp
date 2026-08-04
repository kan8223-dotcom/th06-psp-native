#include <pspctrl.h>
#include <cstring>

#include "diag.hpp"

extern "C" void th06_psp_input_poll(unsigned char *state, unsigned int count)
{
    static bool initialized;
    if (!initialized)
    {
        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
        initialized = true;
    }
    std::memset(state, 0, count);
    SceCtrlData pad{};
    sceCtrlReadBufferPositive(&pad, 1);
    static bool tracedFirstInput;
    if (!tracedFirstInput &&
        (pad.Buttons != 0 || pad.Lx < 64 || pad.Lx > 192 || pad.Ly < 64 || pad.Ly > 192))
    {
        tracedFirstInput = true;
        th06_psp_boot_notef("INPUT FIRST B%08X A%u,%u", pad.Buttons, pad.Lx, pad.Ly);
    }
    // SDL scancodes used by Controller.cpp / the existing PS3 mapping.
    if (pad.Buttons & PSP_CTRL_UP) state[82] = 1;
    if (pad.Buttons & PSP_CTRL_DOWN) state[81] = 1;
    if (pad.Buttons & PSP_CTRL_LEFT) state[80] = 1;
    if (pad.Buttons & PSP_CTRL_RIGHT) state[79] = 1;
    if (pad.Lx < 64) state[80] = 1;
    if (pad.Lx > 192) state[79] = 1;
    if (pad.Ly < 64) state[82] = 1;
    if (pad.Ly > 192) state[81] = 1;
    if (pad.Buttons & PSP_CTRL_CROSS) { state[29] = 1; state[40] = 1; }
    if (pad.Buttons & PSP_CTRL_CIRCLE) state[27] = 1;
    if (pad.Buttons & (PSP_CTRL_SQUARE | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) state[225] = 1;
    if (pad.Buttons & PSP_CTRL_TRIANGLE) state[224] = 1;
    if (pad.Buttons & PSP_CTRL_START) state[41] = 1;
}
