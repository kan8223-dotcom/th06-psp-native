#pragma once

// PSP has no portable process CWD.  Writable state resolves beside EBOOT;
// original game assets resolve below the user-supplied th6 directory.
extern "C" void th06_psp_fileio_set_launch_path(const char *argv0);
extern "C" void th06_psp_fileio_init();
extern "C" const char *th06_psp_game_dir();
extern "C" const char *th06_psp_data_dir();
extern "C" int th06_psp_original_data_ready();
extern "C" const char *th06_psp_boot_log_path();

// The boot record itself lives in diag.hpp; it is written here because this
// translation unit owns the storage prefix.
#include "diag.hpp"
