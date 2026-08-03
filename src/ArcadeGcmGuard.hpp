#pragma once

#include "inttypes.hpp"

/*
 * The embedded arcade and TH06 share one libgcm command context.  TH06 owns
 * that context for GameWindow::Render(), but filesystem and audio waits do
 * not emit RSX commands and must not keep the outer arcade from presenting.
 *
 * After the wait the outer renderer may have changed every persistent RSX
 * binding, so reacquiring the mutex is immediately followed by the same full
 * state restore used after closing the XMB.
 */
#if defined(TH06_PS3) && defined(TH06_ARCADE_EMBED) && TH06_ARCADE_EMBED
extern "C" int th06_arcade_rsx_yield_nonrender(u32 phase);
extern "C" void th06_arcade_rsx_resume_nonrender(int token);
extern "C" void th06_arcade_rsx_nonrender_complete(void);
extern "C" void th06_rsx_prepare_xmb_live_frame(void);

class ArcadeGcmNonRenderGuard
{
  public:
    explicit ArcadeGcmNonRenderGuard(u32 phase)
        : token(th06_arcade_rsx_yield_nonrender(phase))
    {
    }

    ~ArcadeGcmNonRenderGuard()
    {
        if (this->token != 0)
        {
            th06_arcade_rsx_resume_nonrender(this->token);
            th06_rsx_prepare_xmb_live_frame();
            th06_arcade_rsx_nonrender_complete();
        }
    }

    ArcadeGcmNonRenderGuard(const ArcadeGcmNonRenderGuard &) = delete;
    ArcadeGcmNonRenderGuard &operator=(const ArcadeGcmNonRenderGuard &) = delete;

  private:
    int token;
};
#else
class ArcadeGcmNonRenderGuard
{
  public:
    explicit ArcadeGcmNonRenderGuard(u32)
    {
    }
};
#endif

enum ArcadeGcmNonRenderPhase
{
    ARCADE_GCM_PHASE_NONE = 0,
    ARCADE_GCM_PHASE_FILE_READ = 1,
    ARCADE_GCM_PHASE_FILE_WRITE = 2,
    ARCADE_GCM_PHASE_BGM_LOAD = 3,
    ARCADE_GCM_PHASE_BGM_STOP = 4,
    ARCADE_GCM_PHASE_MIDI_STOP = 5,
};
