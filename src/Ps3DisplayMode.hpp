#pragma once

#include "inttypes.hpp"

enum Ps3DisplayMode : u8
{
    PS3_DISPLAY_VGA_1X = 0,
    PS3_DISPLAY_CURRENT_2X = 1,
    PS3_DISPLAY_ASPECT_FIT = 2,
    PS3_DISPLAY_FULL_STRETCH = 3,
    PS3_DISPLAY_MODE_COUNT = 4,
};

struct Ps3DisplayLayout
{
    f32 scaleX;
    f32 scaleY;
    u32 offsetX;
    u32 offsetY;
    u32 outputWidth;
    u32 outputHeight;
};

inline u8 SanitizePs3DisplayMode(u8 mode)
{
    return mode < PS3_DISPLAY_MODE_COUNT ? mode : PS3_DISPLAY_CURRENT_2X;
}

inline const char *GetPs3DisplayModeName(u8 mode)
{
    static const char *const names[PS3_DISPLAY_MODE_COUNT] = {
        "VGA 640x480",
        "CURRENT 2X",
        "4:3 FIT",
        "FULL STRETCH",
    };
    return names[SanitizePs3DisplayMode(mode)];
}

inline Ps3DisplayLayout ComputePs3DisplayLayout(u32 screenWidth, u32 screenHeight, u8 requestedMode)
{
    const f32 gameWidth = 640.0f;
    const f32 gameHeight = 480.0f;
    const u8 mode = SanitizePs3DisplayMode(requestedMode);
    Ps3DisplayLayout layout = {1.0f, 1.0f, 0, 0, 640, 480};

    if (screenWidth == 0 || screenHeight == 0)
        return layout;

    if (mode == PS3_DISPLAY_FULL_STRETCH)
    {
        layout.scaleX = (f32)screenWidth / gameWidth;
        layout.scaleY = (f32)screenHeight / gameHeight;
        layout.outputWidth = screenWidth;
        layout.outputHeight = screenHeight;
        return layout;
    }

    f32 scale = 1.0f;
    if (mode == PS3_DISPLAY_CURRENT_2X && screenWidth >= 1280 && screenHeight >= 960)
    {
        scale = 2.0f;
    }
    else if (mode == PS3_DISPLAY_ASPECT_FIT || screenWidth < 640 || screenHeight < 480)
    {
        const f32 scaleX = (f32)screenWidth / gameWidth;
        const f32 scaleY = (f32)screenHeight / gameHeight;
        scale = scaleX < scaleY ? scaleX : scaleY;
    }

    layout.scaleX = scale;
    layout.scaleY = scale;
    layout.outputWidth = (u32)(gameWidth * scale + 0.5f);
    layout.outputHeight = (u32)(gameHeight * scale + 0.5f);
    if (layout.outputWidth > screenWidth)
        layout.outputWidth = screenWidth;
    if (layout.outputHeight > screenHeight)
        layout.outputHeight = screenHeight;
    layout.offsetX = (screenWidth - layout.outputWidth) / 2;
    layout.offsetY = (screenHeight - layout.outputHeight) / 2;
    return layout;
}
