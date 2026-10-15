#pragma once

// PSP has no portable process CWD.  Writable state resolves beside EBOOT;
// original game assets resolve below the user-supplied th6 directory.
extern "C" void th06_psp_fileio_set_launch_path(const char *argv0);
extern "C" void th06_psp_fileio_init();
extern "C" const char *th06_psp_game_dir();
extern "C" const char *th06_psp_data_dir();
extern "C" int th06_psp_original_data_ready();
extern "C" const char *th06_psp_retail_data_path(const char *engineName);
extern "C" const char *th06_psp_boot_log_path();

// Complete original-data roots found during the bounded boot scan.  Index 0
// is the automatic choice (EBOOT-local first, then the launch device, then the
// other PSP Go device).  Passing -1 to select restores automatic mode.
extern "C" unsigned int th06_psp_data_candidate_count();
extern "C" unsigned int th06_psp_data_candidate_selected();
extern "C" const char *th06_psp_data_candidate_path(unsigned int index);
extern "C" int th06_psp_data_preference_active();
extern "C" int th06_psp_select_data_candidate(int index);

// The boot record itself lives in diag.hpp; it is written here because this
// translation unit owns the storage prefix.
#include "diag.hpp"
