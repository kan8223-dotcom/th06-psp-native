#pragma once

extern "C" int th06_psp_video_init();
extern "C" void th06_psp_video_fill(unsigned int color);
extern "C" void th06_psp_video_present(const unsigned int *src, int width, int height, int pitchBytes);

// Raw display buffer, for the boot overlay and the exception dump.  NULL before
// th06_psp_video_init() has run.  Writing through it needs no kernel call, so
// it stays usable when the rest of the system does not.
extern "C" volatile unsigned int *th06_psp_video_pixels();
extern "C" int th06_psp_video_stride();
extern "C" int th06_psp_video_width();
extern "C" int th06_psp_video_height();
extern "C" unsigned int th06_psp_video_present_count();
// Shared progress hook for both the software bring-up path and native GE.
extern "C" void th06_psp_video_note_presented();
