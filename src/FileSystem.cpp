#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <direct.h>
#include <new>
#include <windows.h>
#elif __cplusplus >= 201703L && __has_include(<filesystem>) && !defined(__PSL1GHT__) // th06_ps3: GCC7 lacks <filesystem>
#include <filesystem>
#else // Assume POSIX
#include <sys/stat.h>
#endif

#include "FileSystem.hpp"
#include "ArcadeGcmGuard.hpp"
#ifdef TH06_PS3
extern "C" char th06_basedir[512]; // ps3/sentry_guest.cpp (argv[0]解決)
extern "C" void th06_savedata_note_write(const char *path); // ps3/savedata.cpp
#include <lv2/sysfs.h>
#endif
#include "pbg3/Pbg3Archive.hpp"
#include "utils.hpp"

u32 g_LastFileSize;

#ifdef TH06_PS3
static const char *ResolvePs3Path(const char *filepath, char *resolved, size_t resolvedSize)
{
    if (filepath == NULL || filepath[0] == '\0')
        return NULL;
    if (filepath[0] == '/')
        return filepath;
    int n = snprintf(resolved, resolvedSize, "%s/%s", th06_basedir, filepath);
    if (n < 0 || (size_t)n >= resolvedSize)
        return NULL;
    return resolved;
}
#endif

FILE *FileSystem::FopenUTF8(const char *filepath, const char *mode)
{
#if defined(TH06_PS3)
    // th06_ps3: no CWD on PS3 lv2 - resolve relative paths against the game dir
    // (same replace-point as the Switch port's FAT32 alias resolver)
    char resolved[768];
    const char *path = ResolvePs3Path(filepath, resolved, sizeof(resolved));
    if (path == NULL)
        return NULL;
    printf("[th06_ps3] fopen: %s (%s)\n", path, mode); // th06_ps3 dbg
    FILE *r = std::fopen(path, mode);
    printf("[th06_ps3] fopen -> %p\n", (void *)r); // th06_ps3 dbg
    return r;
#elif !defined(_WIN32)
    return std::fopen(filepath, mode);
#else
    u32 filepathWLen = MultiByteToWideChar(CP_UTF8, 0, filepath, -1, NULL, 0) * 2;
    u32 modeWLen = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0) * 2;

    if (filepathWLen == 0 || modeWLen == 0)
    {
        return NULL;
    }

    wchar_t *filepathW = new wchar_t[filepathWLen];
    wchar_t *modeW = new wchar_t[modeWLen];

    MultiByteToWideChar(CP_UTF8, 0, filepath, -1, filepathW, filepathWLen / 2);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, modeW, modeWLen / 2);

    FILE *f = _wfopen(filepathW, modeW);

    delete[] filepathW;
    delete[] modeW;

    return f;
#endif
}

void FileSystem::CreateDir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#elif defined(TH06_PS3)
    // lv2 has no useful process CWD.  ResultScreen used to call
    // mkdir("./replay"), then FopenUTF8 resolved the file itself under
    // USRDIR.  The directory and file therefore referred to different
    // locations; fopen failed and ReplayManager passed NULL to fwrite.
    char resolved[768];
    const char *absolutePath = ResolvePs3Path(path, resolved, sizeof(resolved));
    if (absolutePath != NULL)
    {
        s32 rc = sysFsMkdir(absolutePath, 0755);
        printf("[th06_ps3] mkdir: %s -> 0x%x\n", absolutePath, (u32)rc);
    }
#elif __cplusplus >= 201703L && __has_include(<filesystem>) && !defined(__PSL1GHT__) // th06_ps3: GCC7 lacks <filesystem>
    auto p = std::filesystem::path(path);
    std::filesystem::create_directory(p);
#else
    mkdir(path, 0755);
#endif
}

u8 *FileSystem::OpenPath(const char *filepath, int isExternalResource)
{
    ArcadeGcmNonRenderGuard arcadeGcmGuard(ARCADE_GCM_PHASE_FILE_READ);
    u8 *data;
    FILE *file;
    size_t fsize;
    i32 entryIdx;
    const char *entryname;
    i32 pbg3Idx;

    entryIdx = -1;
    if (isExternalResource == 0)
    {
        entryname = std::strrchr(filepath, '\\');
        if (entryname == (char *)0x0)
        {
            entryname = filepath;
        }
        else
        {
            entryname = entryname + 1;
        }
        entryname = std::strrchr(entryname, '/');
        if (entryname == (char *)0x0)
        {
            entryname = filepath;
        }
        else
        {
            entryname = entryname + 1;
        }
        if (g_Pbg3Archives != NULL)
        {
            for (pbg3Idx = 0; pbg3Idx < 0x10; pbg3Idx += 1)
            {
                if (g_Pbg3Archives[pbg3Idx] != NULL)
                {
                    entryIdx = g_Pbg3Archives[pbg3Idx]->FindEntry(entryname);
                    if (entryIdx >= 0)
                    {
                        break;
                    }
                }
            }
        }
        if (entryIdx < 0)
        {
            return NULL;
        }
    }
    if (entryIdx >= 0)
    {
        utils::DebugPrint2("%s Decode ... \n", entryname);
        data = g_Pbg3Archives[pbg3Idx]->ReadDecompressEntry(entryIdx, entryname);
        g_LastFileSize = g_Pbg3Archives[pbg3Idx]->GetEntrySize(entryIdx);
    }
    else
    {
        utils::DebugPrint2("%s Load ... \n", filepath);
        file = FopenUTF8(filepath, "rb");
        if (file == NULL)
        {
            utils::DebugPrint2("error : %s is not found.\n", filepath);
            return NULL;
        }
        else
        {
            std::fseek(file, 0, SEEK_END);
            fsize = std::ftell(file);
            g_LastFileSize = fsize;
            std::fseek(file, 0, SEEK_SET);
            data = (u8 *)std::malloc(fsize);
            std::fread(data, 1, fsize, file);
            std::fclose(file);
        }
    }
    return data;
}

int FileSystem::WriteDataToFile(const char *path, const void *data, size_t size)
{
#ifdef TH06_PS3
    ArcadeGcmNonRenderGuard arcadeGcmGuard(ARCADE_GCM_PHASE_FILE_WRITE);
    // TanakaDoomPS3-proven shape: finish the image in memory, then use
    // sysFs directly and verify the number of bytes accepted by LV2.
    char resolved[768];
    const char *absolutePath = ResolvePs3Path(path, resolved, sizeof(resolved));
    if (absolutePath == NULL || (data == NULL && size != 0))
        return -1;

    s32 fd = -1;
    s32 rc = sysFsOpen(absolutePath, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, NULL, 0);
    if (rc != 0 || fd < 0)
    {
        printf("[th06_ps3] sysFsOpen write NG: %s rc=0x%x fd=%d\n", absolutePath, (u32)rc, fd);
        return -1;
    }

    const u8 *cursor = (const u8 *)data;
    u64 totalWritten = 0;
    while (totalWritten < size)
    {
        u64 written = 0;
        rc = sysFsWrite(fd, cursor + totalWritten, (u64)size - totalWritten, &written);
        if (rc != 0 || written == 0)
            break;
        totalWritten += written;
    }
    s32 closeRc = sysFsClose(fd);
    if (rc != 0 || totalWritten != size)
    {
        printf("[th06_ps3] sysFsWrite NG: %s rc=0x%x wrote=%llu/%llu\n", absolutePath, (u32)rc,
               (unsigned long long)totalWritten, (unsigned long long)size);
        return -2;
    }
    if (closeRc != 0)
        return -3;
    printf("[th06_ps3] sysFsWrite OK: %s bytes=%llu\n", absolutePath, (unsigned long long)totalWritten);
    th06_savedata_note_write(path);
    return 0;
#else
    FILE *f;

    f = FopenUTF8(path, "wb");
    if (f == NULL)
    {
        return -1;
    }
    else
    {
        if (std::fwrite(data, 1, size, f) != size)
        {
            std::fclose(f);
            return -2;
        }
        else
        {
            std::fclose(f);
            return 0;
        }
    }
#endif
}
