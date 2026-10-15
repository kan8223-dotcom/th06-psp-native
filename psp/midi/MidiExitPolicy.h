#pragma once
// C/C++ pure exit policy, used after acquire loads (and again under owner).
enum Th06MeExitAction { TH06_ME_EXIT_NONE, TH06_ME_EXIT_STOP, TH06_ME_EXIT_HOLD };
static inline enum Th06MeExitAction th06_me_exit_action(int initializing, int started, int poisoned)
{
    if (initializing || poisoned) return TH06_ME_EXIT_HOLD;
    return started ? TH06_ME_EXIT_STOP : TH06_ME_EXIT_NONE;
}
