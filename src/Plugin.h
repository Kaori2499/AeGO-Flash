#pragma once
#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "entry.h"

namespace l2dae {
enum ParameterIndex {
    kInput = 0,
    kModelGroupStart,
    kOpenDialog,
    kSelection,
    kImportExpression,
    kImportTransitionFrames,
    kModelGroupEnd,
    kLayoutGroupStart,
    kScale,
    kOffsetX,
    kOffsetY,
    kLayoutGroupEnd,
    kAudioGroupStart,
    kAudioLayer,
    kLipSync,
    kLipSensitivity,
    kAudioGroupEnd,
    kAmbientGroupStart,
    kBreathing,
    kBreathAmount,
    kBreathPeriod,
    kAutoBlink,
    kBlinkInterval,
    kAmbientGroupEnd,
    kLoop,
    kSpeed,
    kStartTime,
    kMotionA,
    kMotionB,
    kManualTime,
    kMotionTimeA,
    kMotionTimeB,
    kBlend,
    kTimelineBinding,
    kExpressionSelection,
    kExpressionA,
    kExpressionB,
    kExpressionWeightA,
    kExpressionWeightB,
    kExpressionBinding,
    kBlinkStrength,
    kBlinkDuration,
    kMotionAIndex,
    kMotionBIndex,
    kExpressionAIndex,
    kExpressionBIndex,
    kParameterCount
};
// Runtime order follows the panel; saved stream IDs remain stable.
constexpr A_long kParameterDiskIds[kParameterCount] = {
    0, 201, 101, 102, 116, 133, 202, 203, 106, 107, 108, 204,
    205, 123, 124, 125, 206, 207, 126, 128, 129, 127, 131, 208,
    103, 104, 105, 109, 110, 111, 112, 113, 114, 115,
    117, 118, 119, 120, 121, 122, 130, 132, 134, 135, 136, 137
};
// Stable disk IDs must not change when parameters are rearranged. Arbitrary
// callback IDs are A_short, so keep this ID below 32768.
constexpr A_short kSelectionDiskId = 102;
constexpr A_short kExpressionDiskId = 117;
// AeGO Flash public 1.0.2 uses a separate monotonic AE compatibility version.
// Match name, disk IDs and saved data stay stable across the product rename.
constexpr A_u_long kPluginVersion = PF_VERSION(1, 13, 2, PF_Stage_RELEASE, 1);
// AE 22.0 introduced API 13.27. Keep discovery compatible with that host.
constexpr A_long kMinimumApiMajor = 13;
constexpr A_long kMinimumApiMinor = 27;
constexpr PF_OutFlags kOutputFlags = PF_OutFlag_DEEP_COLOR_AWARE |
    PF_OutFlag_NON_PARAM_VARY | PF_OutFlag_PIX_INDEPENDENT |
    PF_OutFlag_I_USE_AUDIO | PF_OutFlag_WIDE_TIME_INPUT;
// All render state is local to the call or owned by an isolated renderer worker.
// No sequence_data is used, so MFR needs neither mutable nor flattened sequence flags.
constexpr PF_OutFlags2 kOutputFlags2 = PF_OutFlag2_REVEALS_ZERO_ALPHA |
    PF_OutFlag2_SUPPORTS_THREADED_RENDERING | PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG;
static_assert(kPluginVersion == 955905 && kOutputFlags == 0x02100406 && kOutputFlags2 == 0x08000088,
    "Keep PluginPiPL.rc and PluginPiPL.r in sync with the SDK flags and version.");
}

extern "C" DllExport PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output, void* extra);
