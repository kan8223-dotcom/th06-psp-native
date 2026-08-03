#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "Controller.hpp"
#include "FileSystem.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ReplayManager.hpp"
#include "Rng.hpp"
#include "Supervisor.hpp"
#include "utils.hpp"

ReplayManager *g_ReplayManager;

#ifdef TH06_PS3
extern "C" void th06_breadcrumb(const char *msg);
#endif

ZunResult ReplayManager::ValidateReplayData(const ReplayHeader *data, i32 fileSize)
{
    u8 *checksumCursor;
    u32 checksum;
    u8 *obfuscateCursor;
    u8 obfOffset;
    i32 idx;

    if (data == NULL || fileSize < (i32)sizeof(ReplayHeader))
    {
        return ZUN_ERROR;
    }

    /* "T6RP" magic bytes */
    if (std::memcmp(data->magic, "T6RP", 4) != 0)
    {
        return ZUN_ERROR;
    }

    /* Deobfuscate the replay decryptedData */
    obfuscateCursor = (u8 *)&data->rngValue3;
    obfOffset = data->key;
    for (idx = 0; idx < fileSize - (i32)offsetof(ReplayHeader, rngValue3); idx += 1, obfuscateCursor += 1)
    {
        *obfuscateCursor -= obfOffset;
        obfOffset += 7;
    }

    /* Calculate the checksum */
    /* (0x3f000318 + key + sum(c for c in decryptedData)) % (2 ** 32) */
    checksumCursor = (u8 *)&data->key;
    checksum = 0x3f000318;
    for (idx = 0; idx < fileSize - (i32)offsetof(ReplayHeader, key); idx += 1, checksumCursor += 1)
    {
        checksum += *checksumCursor;
    }

    if (checksum != data->checksum)
    {
        return ZUN_ERROR;
    }

    if (data->version != GAME_VERSION)
    {
        return ZUN_ERROR;
    }

    return ZUN_SUCCESS;
}

ZunResult ReplayManager::RegisterChain(i32 isDemo, const char *replayFile)
{
    ReplayManager *replayMgr;

    if (g_Supervisor.framerateMultiplier < 0.99f && !isDemo)
    {
        return ZUN_SUCCESS;
    }
    g_Supervisor.framerateMultiplier = 1.0f;
    if (g_ReplayManager == NULL)
    {
        replayMgr = new ReplayManager();
        g_ReplayManager = replayMgr;
        replayMgr->replayData = NULL;
        replayMgr->isDemo = isDemo;
        replayMgr->replayFile = replayFile;
        switch (isDemo)
        {
        case false:
            replayMgr->calcChain = g_Chain.CreateElem((ChainCallback)ReplayManager::OnUpdate);
            replayMgr->calcChain->addedCallback = (ChainAddedCallback)AddedCallback;
            replayMgr->calcChain->deletedCallback = (ChainDeletedCallback)DeletedCallback;
            replayMgr->drawChain = g_Chain.CreateElem((ChainCallback)ReplayManager::OnDraw);
            replayMgr->calcChain->arg = replayMgr;
            if (g_Chain.AddToCalcChain(replayMgr->calcChain, TH_CHAIN_PRIO_CALC_REPLAYMANAGER))
            {
                return ZUN_ERROR;
            }
            replayMgr->calcChainDemoHighPrio = NULL;
            break;
        case true:
            replayMgr->calcChain = g_Chain.CreateElem((ChainCallback)ReplayManager::OnUpdateDemoHighPrio);
            replayMgr->calcChain->addedCallback = (ChainAddedCallback)AddedCallbackDemo;
            replayMgr->calcChain->deletedCallback = (ChainDeletedCallback)DeletedCallback;
            replayMgr->drawChain = g_Chain.CreateElem((ChainCallback)ReplayManager::OnDraw);
            replayMgr->calcChain->arg = replayMgr;
            if (g_Chain.AddToCalcChain(replayMgr->calcChain, TH_CHAIN_PRIO_CALC_LOW_PRIO_REPLAYMANAGER_DEMO))
            {
                return ZUN_ERROR;
            }
            replayMgr->calcChainDemoHighPrio = g_Chain.CreateElem((ChainCallback)ReplayManager::OnUpdateDemoLowPrio);
            replayMgr->calcChainDemoHighPrio->arg = replayMgr;
            g_Chain.AddToCalcChain(replayMgr->calcChainDemoHighPrio, TH_CHAIN_PRIO_CALC_HIGH_PRIO_REPLAYMANAGER_DEMO);
            break;
        }
        replayMgr->drawChain->arg = replayMgr;
        g_Chain.AddToDrawChain(replayMgr->drawChain, TH_CHAIN_PRIO_DRAW_REPLAYMANAGER);
    }
    else
    {
        switch (isDemo)
        {
        case false:
            AddedCallback(g_ReplayManager);
            break;
        case true:
            return AddedCallbackDemo(g_ReplayManager);
            break;
        }
    }
    return ZUN_SUCCESS;
}

#define TH_BUTTON_REPLAY_CAPTURE                                                                                       \
    (TH_BUTTON_SHOOT | TH_BUTTON_BOMB | TH_BUTTON_FOCUS | TH_BUTTON_SKIP | TH_BUTTON_DIRECTION)

ChainCallbackResult ReplayManager::OnUpdate(ReplayManager *mgr)
{
    u16 inputs;

    if (!g_GameManager.isInMenu)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    inputs = IS_PRESSED(TH_BUTTON_REPLAY_CAPTURE);
    if (inputs != mgr->replayInputs->inputKey)
    {
        i32 stageIdx = g_GameManager.currentStage - 1;
        if (stageIdx >= 0 && stageIdx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData))
        {
            StageReplayData *stage = mgr->replayData->stageReplayData[stageIdx];
            ReplayDataInput *end = stage ? stage->replayInputs + ARRAY_SIZE(stage->replayInputs) : NULL;
            // Reserve two records for StopRecording's final state and sentinel.
            if (stage != NULL && mgr->replayInputs < end - 3)
            {
                mgr->replayInputs += 1;
                mgr->replayInputStageBookmarks[stageIdx] = mgr->replayInputs + 1;
                mgr->replayInputs->frameNum = mgr->frameId;
                mgr->replayInputs->inputKey = inputs;
            }
        }
    }
    mgr->frameId += 1;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::OnUpdateDemoLowPrio(ReplayManager *mgr)
{
    if (!g_GameManager.isInMenu)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    if (g_Gui.HasCurrentMsgIdx() && g_Gui.IsDialogueSkippable() && mgr->frameId % 3 != 2)
    {
        return CHAIN_CALLBACK_RESULT_RESTART_FROM_FIRST_JOB;
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::OnUpdateDemoHighPrio(ReplayManager *mgr)
{
    if (!g_GameManager.isInMenu)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    while (mgr->frameId >= mgr->replayInputs[1].frameNum)
    {
        mgr->replayInputs += 1;
    }
    g_CurFrameInput = IS_PRESSED(0xFFFFFFFF & ~TH_BUTTON_REPLAY_CAPTURE) | mgr->replayInputs->inputKey;
    g_IsEigthFrameOfHeldInput = 0;
    if (g_LastFrameInput == g_CurFrameInput)
    {
        if (30 <= g_NumOfFramesInputsWereHeld)
        {
            if (g_NumOfFramesInputsWereHeld % 8 == 0)
            {
                g_IsEigthFrameOfHeldInput = 1;
            }
            if (38 <= g_NumOfFramesInputsWereHeld)
            {
                g_NumOfFramesInputsWereHeld = 30;
            }
        }
        g_NumOfFramesInputsWereHeld++;
    }
    else
    {
        g_NumOfFramesInputsWereHeld = 0;
    }
    mgr->frameId += 1;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ChainCallbackResult ReplayManager::OnDraw(ReplayManager *mgr)
{
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

inline StageReplayData *AllocateStageReplayData(i32 size)
{
    return (StageReplayData *)std::malloc(size);
}

inline void ReleaseReplayData(void *data)
{
    return std::free(data);
}

inline void ReleaseStageReplayData(void *data)
{
    return std::free(data);
}

ZunResult ReplayManager::AddedCallback(ReplayManager *mgr)
{
    StageReplayData *stageReplayData;
    StageReplayData *oldStageReplayData;
    i32 idx;

    mgr->frameId = 0;
    if (mgr->replayData == NULL)
    {
        mgr->replayData = new ReplayData();
        mgr->replayData->header = new ReplayHeader();
        std::memcpy(&mgr->replayData->header->magic[0], "T6RP", 4);
        mgr->replayData->header->shottypeChara = g_GameManager.character * 2 + g_GameManager.shotType;
        mgr->replayData->header->version = 0x102;
        mgr->replayData->header->difficulty = g_GameManager.difficulty;
        std::memcpy(&mgr->replayData->header->name, "NO NAME", 4);
        for (idx = 0; idx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); idx += 1)
        {
            mgr->replayData->stageReplayData[idx] = NULL;
        }
    }
    else
    {
        oldStageReplayData = mgr->replayData->stageReplayData[g_GameManager.currentStage - 2];
        if (oldStageReplayData == NULL)
        {
            return ZUN_ERROR;
        }
        oldStageReplayData->score = g_GameManager.score;
    }
    if (mgr->replayData->stageReplayData[g_GameManager.currentStage - 1] != NULL)
    {
        utils::DebugPrint2("error : replay.cpp");
    }
    mgr->replayData->stageReplayData[g_GameManager.currentStage - 1] = AllocateStageReplayData(sizeof(StageReplayData));
    stageReplayData = mgr->replayData->stageReplayData[g_GameManager.currentStage - 1];
    stageReplayData->bombsRemaining = g_GameManager.bombsRemaining;
    stageReplayData->livesRemaining = g_GameManager.livesRemaining;
    stageReplayData->power = g_GameManager.currentPower;
    stageReplayData->rank = g_GameManager.rank;
    stageReplayData->pointItemsCollected = g_GameManager.pointItemsCollected;
    stageReplayData->randomSeed = g_GameManager.randomSeed;
    stageReplayData->powerItemCountForScore = g_GameManager.powerItemCountForScore;
    mgr->replayInputs = stageReplayData->replayInputs;
    mgr->replayInputs->frameNum = 0;
    mgr->replayInputs->inputKey = 0;
    mgr->unk44 = 0;
    return ZUN_SUCCESS;
}

ZunResult ReplayManager::AddedCallbackDemo(ReplayManager *mgr)
{
    i32 idx;
    StageReplayData *replayData;

    mgr->frameId = 0;
    if (mgr->replayData == NULL)
    {
        mgr->replayData = (ReplayData *)std::malloc(sizeof(ReplayData));

        mgr->replayData->header = (ReplayHeader *)FileSystem::OpenPath(mgr->replayFile, g_GameManager.demoMode == 0);
        if (ValidateReplayData(mgr->replayData->header, g_LastFileSize) != ZUN_SUCCESS)
        {
            return ZUN_ERROR;
        }
        for (idx = 0; idx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); idx += 1)
        {
            if (mgr->replayData->header->stageReplayDataOffsets[idx] != 0)
            {
                mgr->replayData->stageReplayData[idx] =
                    (StageReplayData *)(((u8 *)mgr->replayData->header) +
                                        mgr->replayData->header->stageReplayDataOffsets[idx]);
            }
            else
            {
                mgr->replayData->stageReplayData[idx] = NULL;
            }
        }
    }
    if (mgr->replayData->stageReplayData[g_GameManager.currentStage - 1] == NULL)
    {
        return ZUN_ERROR;
    }
    replayData = mgr->replayData->stageReplayData[g_GameManager.currentStage - 1];
    g_GameManager.character = mgr->replayData->header->shottypeChara / 2;
    g_GameManager.shotType = mgr->replayData->header->shottypeChara % 2;
    g_GameManager.difficulty = (Difficulty)mgr->replayData->header->difficulty;
    g_GameManager.pointItemsCollected = replayData->pointItemsCollected;
    g_Rng.Initialize(replayData->randomSeed);
    g_GameManager.rank = replayData->rank;
    g_GameManager.livesRemaining = replayData->livesRemaining;
    g_GameManager.bombsRemaining = replayData->bombsRemaining;
    g_GameManager.currentPower = replayData->power;
    mgr->replayInputs = replayData->replayInputs;
    g_GameManager.powerItemCountForScore = replayData->powerItemCountForScore;
    if (2 <= g_GameManager.currentStage && mgr->replayData->stageReplayData[g_GameManager.currentStage - 2] != NULL)
    {
        g_GameManager.guiScore = mgr->replayData->stageReplayData[g_GameManager.currentStage - 2]->score;
        g_GameManager.score = g_GameManager.guiScore;
    }
    return ZUN_SUCCESS;
}

ZunResult ReplayManager::DeletedCallback(ReplayManager *mgr)
{
    g_Chain.Cut(mgr->drawChain);
    mgr->drawChain = NULL;
    if (mgr->calcChainDemoHighPrio != NULL)
    {
        g_Chain.Cut(mgr->calcChainDemoHighPrio);
        mgr->calcChainDemoHighPrio = NULL;
    }
    if (mgr->replayData != NULL)
    {
        if (mgr->IsDemo())
        {
            // OpenPath and the demo ReplayData wrapper both use malloc.
            std::free(mgr->replayData->header);
            ReleaseReplayData(mgr->replayData);
        }
        else
        {
            // Recording uses new ReplayHeader/new ReplayData.
            delete mgr->replayData->header;
            delete mgr->replayData;
        }
        mgr->replayData = NULL;
    }
    if (g_ReplayManager == mgr)
        g_ReplayManager = NULL;
    delete mgr;
    return ZUN_SUCCESS;
}

void ReplayManager::StopRecording()
{
    ReplayManager *mgr = g_ReplayManager;
    if (mgr != NULL && mgr->replayData != NULL)
    {
        i32 stageIdx = g_GameManager.currentStage - 1;
        if (stageIdx < 0 || stageIdx >= ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData))
            return;
        StageReplayData *stage = mgr->replayData->stageReplayData[stageIdx];
        ReplayDataInput *end = stage ? stage->replayInputs + ARRAY_SIZE(stage->replayInputs) : NULL;
        if (stage == NULL || mgr->replayInputs < stage->replayInputs || mgr->replayInputs > end - 3)
            return;

        mgr->replayInputs += 1;
        mgr->replayInputs->frameNum = mgr->frameId;
        mgr->replayInputs->inputKey = 0;
        mgr->replayInputs += 1;
        mgr->replayInputs->frameNum = 9999999;
        mgr->replayInputs->inputKey = 0;
        mgr->replayInputStageBookmarks[stageIdx] = mgr->replayInputs + 1;
    }
}

void ReplayManager::SaveReplay(const char *replayPath, char *replayName)
{
    if (g_ReplayManager == NULL)
        return;

    ReplayManager *mgr = g_ReplayManager;
    if (!mgr->IsDemo())
    {
        if (replayPath != NULL && mgr->replayData != NULL && mgr->replayData->header != NULL)
        {
            ReplayHeader replayCopy;
            size_t stageSizes[7] = {};
            size_t replaySize = sizeof(ReplayHeader);
            bool layoutValid = true;

            replayCopy = *mgr->replayData->header;
            ReplayManager::StopRecording();
            for (i32 stageIdx = 0; stageIdx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); stageIdx++)
            {
                replayCopy.stageReplayDataOffsets[stageIdx] = 0;
                StageReplayData *stage = mgr->replayData->stageReplayData[stageIdx];
                if (stage == NULL)
                    continue;

                uintptr_t begin = (uintptr_t)stage;
                uintptr_t end = (uintptr_t)mgr->replayInputStageBookmarks[stageIdx];
                if (end < begin || end > begin + sizeof(StageReplayData))
                {
                    layoutValid = false;
                    break;
                }
                stageSizes[stageIdx] = (size_t)(end - begin);
                if (stageSizes[stageIdx] == 0 || replaySize > 0xffffffffu - stageSizes[stageIdx])
                {
                    layoutValid = false;
                    break;
                }
                replayCopy.stageReplayDataOffsets[stageIdx] = (u32)replaySize;
                replaySize += stageSizes[stageIdx];
            }

            i32 currentStage = g_GameManager.currentStage - 1;
            if (currentStage < 0 || currentStage >= ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData) ||
                mgr->replayData->stageReplayData[currentStage] == NULL)
            {
                layoutValid = false;
            }

            if (layoutValid)
            {
                utils::DebugPrint2("%s write ...\n", replayPath);
                replayCopy.score = g_GameManager.guiScore;
                f32 slowDown = 0.0f;
                if (g_Supervisor.unk1b8 > 0.0f)
                    slowDown = (g_Supervisor.unk1b4 / g_Supervisor.unk1b8 - 0.5f) * 2.0f;
                if (slowDown < 0.0f)
                    slowDown = 0.0f;
                else if (slowDown >= 1.0f)
                    slowDown = 1.0f;
                replayCopy.slowdownRate = (1.0f - slowDown) * 100.0f;
                replayCopy.slowdownRate2 = (f32)replayCopy.slowdownRate + 1.12f;
                replayCopy.slowdownRate3 = (f32)replayCopy.slowdownRate + 2.34f;
                mgr->replayData->stageReplayData[currentStage]->score = g_GameManager.score;

                // The on-disk field is exactly eight bytes and is not required
                // to be NUL terminated.  replayName is also an eight-byte UI
                // buffer, so strcpy could read and write past both arrays.
                std::memset(replayCopy.name, ' ', sizeof(replayCopy.name));
                if (replayName != NULL)
                {
                    for (size_t i = 0; i < sizeof(replayCopy.name) && replayName[i] != '\0'; i++)
                        replayCopy.name[i] = replayName[i];
                }

                std::time_t replayTime = std::time(NULL);
                const std::tm *tm = std::localtime(&replayTime);
                if (tm != NULL)
                    ::strftime(replayCopy.date, sizeof(replayCopy.date), "%m/%d/%y", tm);
                else
                    std::memcpy(replayCopy.date, "00/00/00", sizeof(replayCopy.date));

                replayCopy.key = g_Rng.GetRandomU16InRange(128) + 64;
                replayCopy.rngValue3 = g_Rng.GetRandomU16InRange(256);
                replayCopy.rngValue1 = g_Rng.GetRandomU16InRange(256);
                replayCopy.rngValue2 = g_Rng.GetRandomU16InRange(256);

                // TanakaDoomPS3-proven pattern: finish one contiguous image in
                // memory, then perform one checked LV2 write.  This also makes
                // the checksum/obfuscation stream exactly match the file.
                u8 *replayBuffer = (u8 *)std::malloc(replaySize);
                if (replayBuffer != NULL)
                {
                    std::memcpy(replayBuffer, &replayCopy, sizeof(replayCopy));
                    size_t writePos = sizeof(replayCopy);
                    for (i32 stageIdx = 0; stageIdx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData);
                         stageIdx++)
                    {
                        if (stageSizes[stageIdx] == 0)
                            continue;
                        std::memcpy(replayBuffer + writePos, mgr->replayData->stageReplayData[stageIdx],
                                    stageSizes[stageIdx]);
                        writePos += stageSizes[stageIdx];
                    }

                    ReplayHeader *diskHeader = (ReplayHeader *)replayBuffer;
                    u32 checksum = 0x3f000318;
                    for (size_t i = offsetof(ReplayHeader, key); i < replaySize; i++)
                        checksum += replayBuffer[i];
                    diskHeader->checksum = checksum;

                    u8 obfOffset = (u8)diskHeader->key;
                    for (size_t i = offsetof(ReplayHeader, rngValue3); i < replaySize; i++)
                    {
                        replayBuffer[i] += obfOffset;
                        obfOffset += 7;
                    }

                    int writeRc = FileSystem::WriteDataToFile(replayPath, replayBuffer, replaySize);
                    std::free(replayBuffer);
                    if (writeRc == 0)
                    {
                        printf("[th06_ps3] replay save OK: %s bytes=%llu\n", replayPath,
                               (unsigned long long)replaySize);
#ifdef TH06_PS3
                        th06_breadcrumb("REPLAY SAVE OK");
#endif
                    }
                    else
                    {
                        printf("[th06_ps3] replay save NG: %s rc=%d\n", replayPath, writeRc);
#ifdef TH06_PS3
                        th06_breadcrumb("REPLAY SAVE NG");
#endif
                    }
                }
                else
                {
                    layoutValid = false;
                }
            }

            if (!layoutValid)
            {
                printf("[th06_ps3] replay save rejected: invalid layout or allocation\n");
#ifdef TH06_PS3
                th06_breadcrumb("REPLAY SAVE NG");
#endif
            }
        }

        for (i32 stageIdx = 0; stageIdx < ARRAY_SIZE_SIGNED(mgr->replayData->stageReplayData); stageIdx++)
        {
            if (mgr->replayData->stageReplayData[stageIdx] != NULL)
            {
                utils::DebugPrint2("Replay Size %lld\n",
                                   (long long)((iptr)mgr->replayInputStageBookmarks[stageIdx] -
                                               (iptr)mgr->replayData->stageReplayData[stageIdx]));
                ReleaseStageReplayData(mgr->replayData->stageReplayData[stageIdx]);
                mgr->replayData->stageReplayData[stageIdx] = NULL;
            }
        }
    }
    g_Chain.Cut(mgr->calcChain);
}
