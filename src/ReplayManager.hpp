#pragma once

#include "Chain.hpp"
#include "ChainPriorities.hpp"
#include "ReplayData.hpp"
#include "inttypes.hpp"

struct ReplayManager
{
    static ZunResult RegisterChain(i32 isDemo, const char *replayFile);
    static ChainCallbackResult OnUpdate(ReplayManager *mgr);
    static ChainCallbackResult OnUpdateDemoHighPrio(ReplayManager *mgr);
    static ChainCallbackResult OnUpdateDemoLowPrio(ReplayManager *mgr);
    static ChainCallbackResult OnDraw(ReplayManager *mgr);
    static ZunResult AddedCallback(ReplayManager *mgr);
    static ZunResult AddedCallbackDemo(ReplayManager *mgr);
    static ZunResult DeletedCallback(ReplayManager *mgr);
    static void StopRecording();
    static void SaveReplay(const char *replay_path, char *param_2);
    static ZunResult ValidateReplayData(const ReplayHeader *data, i32 fileSize);

    ReplayManager()
        : frameId(0), replayData(nullptr), isDemo(0), replayFile(nullptr), unk10{}, unk44(0), replayInputs(nullptr),
          replayInputStageBookmarks{}, calcChain(nullptr), drawChain(nullptr), calcChainDemoHighPrio(nullptr),
          replayStageStopped{}
    {
    }

    i32 IsDemo() const
    {
        return this->isDemo;
    }

    i32 frameId;
    ReplayData *replayData;
    i32 isDemo;
    const char *replayFile;
    u8 unk10[52];
    u16 unk44;
    ReplayDataInput *replayInputs;
    const ReplayDataInput *replayInputStageBookmarks[7];
    ChainElem *calcChain;
    ChainElem *drawChain;
    ChainElem *calcChainDemoHighPrio;

    // StopRecording is reached once at a stage boundary and again from
    // SaveReplay. Keep the state on the manager rather than inferring it from
    // the allocation size so PSP may safely shrink completed stage buffers.
    u8 replayStageStopped[7];
};
