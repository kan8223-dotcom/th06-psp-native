#include "fileio.hpp"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "diag.hpp"

extern "C" FILE *__real_fopen(const char *path, const char *mode);

namespace {
char gGameDir[256] = "ms0:/PSP/GAME/TH06PSP";
char gDataDir[272] = "ms0:/PSP/GAME/TH06PSP";
char gBootLog[320] = "ms0:/TH06PSP_BOOT.LOG";
bool gInitialized;
bool gLaunchDirSet;
bool gPeriodicDiagEnabled;
bool gOriginalDataReady;
unsigned int gStartUs;
SceUID gLogSema = -1;

struct StablePathAlias
{
    const char *engineName;
    const char *storageName;
};

struct RetailDataFile
{
    const char *engineName;
    const char *legacyAsciiName;
    unsigned int bytes;
};

// PSP IoFileMgr does not provide the UTF-8 filename semantics used by the
// portable engine.  Keep the engine-visible names intact and use 8.3-safe
// ASCII names only at the PSP filesystem boundary.  This is also stable when
// a Memory Stick is moved between Windows, PPSSPP and different PSP models.
constexpr StablePathAlias kStablePathAliases[] = {
    {u8"東方紅魔郷.cfg", "TH06.CFG"},
};

// TH06 1.02f retail archives.  The sizes are also the fallback identity when
// PSP's FAT driver cannot address the Japanese long filename using UTF-8.
// Every file is additionally checked for the PBG3 magic before being used.
constexpr RetailDataFile kRetailDataFiles[] = {
    {u8"紅魔郷CM.DAT", "TH06CM.DAT", 935275u},
    {u8"紅魔郷ED.DAT", "TH06ED.DAT", 2017777u},
    {u8"紅魔郷IN.DAT", "TH06IN.DAT", 760223u},
    {u8"紅魔郷MD.DAT", "TH06MD.DAT", 306003u},
    {u8"紅魔郷ST.DAT", "TH06ST.DAT", 2964458u},
    {u8"紅魔郷TL.DAT", "TH06TL.DAT", 1040706u},
};

constexpr unsigned int kRetailDataFileCount = sizeof(kRetailDataFiles) / sizeof(kRetailDataFiles[0]);
char gRetailDataPaths[kRetailDataFileCount][512]{};

bool JoinPath(char *out, size_t outSize, const char *root, const char *relative)
{
    const int length = std::snprintf(out, outSize, "%s/%s", root, relative);
    return length >= 0 && static_cast<size_t>(length) < outSize;
}

bool IsDirectory(const char *path)
{
    const SceUID directory = sceIoDopen(path);
    if (directory < 0)
        return false;
    sceIoDclose(directory);
    return true;
}

bool IsPbg3File(const char *path, unsigned int expectedBytes)
{
    SceIoStat stat{};
    if (sceIoGetstat(path, &stat) < 0 || static_cast<unsigned long long>(stat.st_size) != expectedBytes)
        return false;

    const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return false;
    char magic[4]{};
    const int read = sceIoRead(fd, magic, sizeof(magic));
    sceIoClose(fd);
    return read == static_cast<int>(sizeof(magic)) && std::memcmp(magic, "PBG3", sizeof(magic)) == 0;
}

int RetailDataIndex(const char *path)
{
    if (path == nullptr)
        return -1;
    for (unsigned int i = 0; i < kRetailDataFileCount; ++i)
        if (std::strcmp(path, kRetailDataFiles[i].engineName) == 0 ||
            std::strcmp(path, kRetailDataFiles[i].legacyAsciiName) == 0)
            return static_cast<int>(i);
    return -1;
}

void DiscoverRetailDataFiles()
{
    for (unsigned int i = 0; i < kRetailDataFileCount; ++i)
    {
        gRetailDataPaths[i][0] = '\0';
        char candidate[512];
        if (JoinPath(candidate, sizeof(candidate), gDataDir, kRetailDataFiles[i].engineName) &&
            IsPbg3File(candidate, kRetailDataFiles[i].bytes))
        {
            std::snprintf(gRetailDataPaths[i], sizeof(gRetailDataPaths[i]), "%s", candidate);
            continue;
        }
        if (JoinPath(candidate, sizeof(candidate), gDataDir, kRetailDataFiles[i].legacyAsciiName) &&
            IsPbg3File(candidate, kRetailDataFiles[i].bytes))
            std::snprintf(gRetailDataPaths[i], sizeof(gRetailDataPaths[i]), "%s", candidate);
    }

    // On real FAT media the long Japanese name returned by sceIoDread may be
    // Shift-JIS, UTF-8, or an implementation-specific alias.  Use the exact
    // bytes returned by the driver, then identify the six unique retail files
    // by size and PBG3 header.  No rename or copy of original data is needed.
    const SceUID directory = sceIoDopen(gDataDir);
    if (directory >= 0)
    {
        SceIoDirent entry{};
        while (sceIoDread(directory, &entry) > 0)
        {
            for (unsigned int i = 0; i < kRetailDataFileCount; ++i)
            {
                if (gRetailDataPaths[i][0] != '\0' ||
                    static_cast<unsigned long long>(entry.d_stat.st_size) != kRetailDataFiles[i].bytes)
                    continue;
                char candidate[512];
                if (JoinPath(candidate, sizeof(candidate), gDataDir, entry.d_name) &&
                    IsPbg3File(candidate, kRetailDataFiles[i].bytes))
                    std::snprintf(gRetailDataPaths[i], sizeof(gRetailDataPaths[i]), "%s", candidate);
            }
            std::memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(directory);
    }
}

bool IsBgmPath(const char *path)
{
    while (path != nullptr && path[0] == '.' && path[1] == '/')
        path += 2;
    return path != nullptr && std::strncmp(path, "bgm/", 4) == 0;
}

bool ModeMayWrite(const char *mode)
{
    return mode != nullptr && (std::strchr(mode, 'w') != nullptr || std::strchr(mode, 'a') != nullptr ||
                               std::strchr(mode, '+') != nullptr);
}

FILE *OpenBelow(const char *root, const char *path, const char *mode, char *resolved, size_t resolvedSize)
{
    if (!JoinPath(resolved, resolvedSize, root, path))
        return nullptr;
    return __real_fopen(resolved, mode);
}

const char *StableStoragePath(const char *path)
{
    if (path == nullptr)
        return nullptr;
    for (const StablePathAlias &alias : kStablePathAliases)
        if (std::strcmp(path, alias.engineName) == 0)
            return alias.storageName;
    return path;
}

bool IsAbsolutePspPath(const char *path)
{
    return path != nullptr && std::strchr(path, ':') != nullptr;
}

void WriteBootNote(const char *message)
{
    if (gLogSema >= 0)
        sceKernelWaitSema(gLogSema, 1, nullptr);
    const SceUID fd = sceIoOpen(gBootLog, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0)
    {
        if (gLogSema >= 0)
            sceKernelSignalSema(gLogSema, 1);
        return;
    }
    // Every line carries the elapsed time, so a boot that stalls is visible as
    // a gap rather than having to be inferred from where the log stops.
    char stamp[16];
    const unsigned int elapsedMs = (sceKernelGetSystemTimeLow() - gStartUs) / 1000u;
    const int stampLength = std::snprintf(stamp, sizeof(stamp), "[%6u] ", elapsedMs);
    if (stampLength > 0)
        sceIoWrite(fd, stamp, static_cast<SceSize>(stampLength));
    sceIoWrite(fd, message, std::strlen(message));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
    if (gLogSema >= 0)
        sceKernelSignalSema(gLogSema, 1);
}
}

extern "C" void th06_psp_fileio_set_launch_path(const char *argv0)
{
    if (gInitialized || argv0 == nullptr)
        return;

    // XMB passes the launched EBOOT path (ms0:/.../EBOOT.PBP or
    // ef0:/.../EBOOT.PBP).  Resolve relative game files beside that exact
    // EBOOT.  Merely probing whether an ef0 directory exists selected stale
    // installs when the user had launched the memory-stick copy.
    const char *colon = std::strchr(argv0, ':');
    const char *slash = std::strrchr(argv0, '/');
    if (colon == nullptr || slash == nullptr || colon > slash)
        return;

    const size_t dirLength = static_cast<size_t>(slash - argv0);
    if (dirLength == 0 || dirLength >= sizeof(gGameDir))
        return;
    std::memcpy(gGameDir, argv0, dirLength);
    gGameDir[dirLength] = '\0';

    const int deviceLength = static_cast<int>(colon - argv0 + 1);
    const int logLength = std::snprintf(gBootLog, sizeof(gBootLog), "%.*s/TH06PSP_BOOT.LOG", deviceLength, argv0);
    if (logLength < 0 || static_cast<size_t>(logLength) >= sizeof(gBootLog))
        return;
    gLaunchDirSet = true;
}

extern "C" void th06_psp_fileio_init()
{
    if (gInitialized)
        return;

    gStartUs = sceKernelGetSystemTimeLow();

    // argv[0] is authoritative.  The directory probe remains only as a
    // fallback for unusual launchers that provide no process argument.
    if (!gLaunchDirSet)
    {
        const SceUID dir = sceIoDopen("ef0:/PSP/GAME/TH06PSP");
        if (dir >= 0)
        {
            sceIoDclose(dir);
            std::strcpy(gGameDir, "ef0:/PSP/GAME/TH06PSP");
            std::strcpy(gBootLog, "ef0:/TH06PSP_BOOT.LOG");
        }
    }

    // Public-install contract: place the user's untouched PC TH6 directory at
    // PSP/GAME/TH06PSP/th6.  Keep the previous flat layout as a development
    // fallback so existing test installations remain usable.
    char nestedDataDir[sizeof(gDataDir)];
    if (JoinPath(nestedDataDir, sizeof(nestedDataDir), gGameDir, "th6") && IsDirectory(nestedDataDir))
        std::snprintf(gDataDir, sizeof(gDataDir), "%s", nestedDataDir);
    else
        std::snprintf(gDataDir, sizeof(gDataDir), "%s", gGameDir);
    DiscoverRetailDataFiles();

    unsigned int archivesFound = 0;
    for (unsigned int i = 0; i < kRetailDataFileCount; ++i)
        if (gRetailDataPaths[i][0] != '\0')
            ++archivesFound;
    unsigned int bgmFound = 0;
    for (unsigned int i = 1; i <= 17; ++i)
    {
        char relative[32];
        char absolute[512];
        std::snprintf(relative, sizeof(relative), "bgm/th06_%02u.wav", i);
        SceIoStat stat{};
        if (JoinPath(absolute, sizeof(absolute), gDataDir, relative) && sceIoGetstat(absolute, &stat) >= 0 &&
            stat.st_size > 44)
            ++bgmFound;
    }
    gOriginalDataReady = archivesFound == kRetailDataFileCount && bgmFound == 17;

    gLogSema = sceKernelCreateSema("th06_log", 0, 1, 1, nullptr);
    char periodicDiagMarker[96];
    std::snprintf(periodicDiagMarker, sizeof(periodicDiagMarker), "%s/TH06PSP_DIAG.ON", gGameDir);
    SceIoStat markerStat{};
    gPeriodicDiagEnabled = sceIoGetstat(periodicDiagMarker, &markerStat) >= 0;
    gInitialized = true;

    // Keep one generation.  A second run to reproduce something must not be the
    // thing that destroys the evidence from the first.
    char previous[96];
    std::snprintf(previous, sizeof(previous), "%.*s_PREV.LOG",
                  static_cast<int>(std::strlen(gBootLog) - 4), gBootLog);
    sceIoRemove(previous);
    sceIoRename(gBootLog, previous);

    const SceUID fd = sceIoOpen(gBootLog, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0)
        sceIoClose(fd);
    th06_psp_boot_note(gGameDir);
    th06_psp_boot_notef("DATA %s DAT%u/%u WAV%u/17", gDataDir, archivesFound, kRetailDataFileCount, bgmFound);
}

extern "C" const char *th06_psp_game_dir()
{
    th06_psp_fileio_init();
    return gGameDir;
}

extern "C" const char *th06_psp_data_dir()
{
    th06_psp_fileio_init();
    return gDataDir;
}

extern "C" int th06_psp_original_data_ready()
{
    th06_psp_fileio_init();
    return gOriginalDataReady ? 1 : 0;
}

extern "C" const char *th06_psp_boot_log_path()
{
    return gBootLog;
}

extern "C" int th06_psp_periodic_diag_enabled()
{
    th06_psp_fileio_init();
    return gPeriodicDiagEnabled ? 1 : 0;
}

extern "C" void th06_psp_boot_note(const char *message)
{
    th06_psp_fileio_init();
    WriteBootNote(message);
    th06_psp_overlay_push(message);
    th06_psp_watchdog_pet();
}

extern "C" void th06_psp_boot_notef(const char *fmt, ...)
{
    char message[128];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    th06_psp_boot_note(message);
}

// GNU ld --wrap=fopen keeps src/FileSystem.cpp unchanged while applying the
// PSP's required path rule to both FileSystem and SDL RWops.
extern "C" FILE *__real_fopen(const char *path, const char *mode);
extern "C" FILE *__wrap_fopen(const char *path, const char *mode)
{
    th06_psp_fileio_init();

    // The per-file trace is a boot bring-up aid and nothing else.  It costs an
    // open/write/close per call, so it stops with the boot phase and never runs
    // while the game is loading stage data.
    const bool trace = th06_psp_boot_phase() != 0;

    if (path == nullptr || IsAbsolutePspPath(path))
    {
        FILE *result = __real_fopen(path, mode);
        if (trace)
            th06_psp_boot_notef("FOPEN ABS %s %s", path ? path : "(null)", result ? "OK" : "NG");
        return result;
    }

    const char *relativePath = path;
    while (relativePath[0] == '.' && relativePath[1] == '/')
        relativePath += 2;
    const char *storagePath = StableStoragePath(relativePath);
    char resolved[512];
    FILE *result = nullptr;
    const char *source = "STATE";

    // Never modify the user's original PC directory.  Config, score, replay
    // and logs are written beside EBOOT.  Reads prefer that writable state and
    // fall back to the copied TH6 directory for existing PC saves/replays.
    if (ModeMayWrite(mode))
    {
        result = OpenBelow(gGameDir, storagePath, mode, resolved, sizeof(resolved));
    }
    else
    {
        const int retailIdx = RetailDataIndex(relativePath);
        if (retailIdx >= 0)
        {
            source = "DATA";
            if (gRetailDataPaths[retailIdx][0] != '\0')
            {
                std::snprintf(resolved, sizeof(resolved), "%s", gRetailDataPaths[retailIdx]);
                result = __real_fopen(resolved, mode);
            }
        }
        else if (IsBgmPath(relativePath))
        {
            source = "DATA";
            result = OpenBelow(gDataDir, relativePath, mode, resolved, sizeof(resolved));
        }
        else
        {
            result = OpenBelow(gGameDir, storagePath, mode, resolved, sizeof(resolved));
            if (result == nullptr && std::strcmp(gDataDir, gGameDir) != 0)
            {
                source = "DATA-FALLBACK";
                result = OpenBelow(gDataDir, relativePath, mode, resolved, sizeof(resolved));
            }
        }
    }
    if (trace)
    {
        th06_psp_boot_notef("FOPEN %s %s %s", source, relativePath, result ? "OK" : "NG");
    }
    return result;
}
