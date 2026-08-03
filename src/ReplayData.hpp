#pragma once

#include "ZunEndian.hpp" // th06_ps3: .rpy is little-endian on-disk data
#include "inttypes.hpp"

struct ReplayDataInput
{
    LE<i32> frameNum;
    LE<u16> inputKey;
    LE<u16> padding;
};

struct StageReplayData
{
    LE<i32> score;
    LE<i16> randomSeed;
    LE<i16> pointItemsCollected;
    u8 power;
    i8 livesRemaining;
    i8 bombsRemaining;
    u8 rank;
    i8 powerItemCountForScore;
    i8 padding[3];
    ReplayDataInput replayInputs[53998];
};

struct ReplayHeader
{
    char magic[4];
    LE<u16> version;
    u8 shottypeChara;
    u8 difficulty;
    LE<i32> checksum;
    u8 rngValue1;
    u8 rngValue2;
    i8 key;
    i8 rngValue3;
    char date[9];
    char name[8];
    LE<i32> score;
    LE<f32> slowdownRate2;
    LE<f32> slowdownRate;
    LE<f32> slowdownRate3;
    LE<u32> stageReplayDataOffsets[7];
};

struct ReplayData
{
    ReplayHeader *header;
    StageReplayData *stageReplayData[7];
};
