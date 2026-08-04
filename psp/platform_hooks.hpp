#pragma once

// A small number of PS3 diagnostic calls escaped their platform guards in the
// inherited engine.  Keep the engine untouched; PSP supplies harmless hooks.
extern "C" void th06_breadcrumb(const char *msg);

// Short-lived trace armed by the title's demo00.rpy load. PSP graphics uses
// this to leave one-shot markers around the first post-load GE operations.
extern "C" int th06_psp_transition_trace_take(unsigned int marker);

// Detailed per-asset tracing is opt-in after bring-up.  Keeping it off avoids
// hundreds of open/write/close cycles against the same Memory Stick that is
// feeding stage data and streaming BGM.
extern "C" int th06_psp_verbose_transition_diag();

// Returns and resets the SC-side chain timing window.  Called only from the
// GE present path when its matching 300-frame PERF window closes.
extern "C" void th06_psp_cpu_perf_window(unsigned long long *calcUs, unsigned long long *drawUs,
                                          unsigned int *calcCalls, unsigned int *drawCalls);

// Independent 60-frame RAM flight recorder.  It never writes during play;
// the GE backend consumes and resets it when publishing a candidate window.
extern "C" void th06_psp_cpu_perf_flight_window(unsigned long long *calcUs, unsigned long long *drawUs);

// Cumulative PSP bullet-render shortcut counts, sampled only on exit.
extern "C" void th06_psp_bullet_fast_stats(unsigned int *fastDraws, unsigned int *fallbackDraws);
