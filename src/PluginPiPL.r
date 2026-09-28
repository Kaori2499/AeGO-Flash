#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
    #include "AE_General.r"
#endif

resource 'PiPL' (16000) {
    {
        Kind { AEEffect },
        Name { "AeGO Flash" },
        Category { "AeGO" },
        CodeWin64X86 { "EffectMain" },
        AE_PiPL_Version { 2, 0 },
        AE_Effect_Spec_Version { 13, 27 },
        /* Public AeGO Flash 1.0.0; independent AE compatibility version.
           PF_VERSION(1, 13, 0, PF_Stage_RELEASE, 1). */
        AE_Effect_Version { 951809 },
        AE_Effect_Info_Flags { 0 },
        /* DEEP_COLOR_AWARE | NON_PARAM_VARY | PIX_INDEPENDENT | I_USE_AUDIO | WIDE_TIME_INPUT. */
        AE_Effect_Global_OutFlags { 0x02100406 },
        /* REVEALS_ZERO_ALPHA | SUPPORTS_THREADED_RENDERING | PARAM_GROUP_START_COLLAPSED_FLAG. No SmartFX or float claim. */
        AE_Effect_Global_OutFlags_2 { 0x08000088 },
        AE_Effect_Match_Name { "L2DAE Native Renderer" },
        AE_Reserved_Info { 0 }
    }
};
