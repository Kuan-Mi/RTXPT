/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

// Must match Profiler.cpp::g_SectionNames
struct ProfilerSection
{
    enum Enum
    {
        SkinnedBLASUpdates,
        TlasUpdate,
        PreUpdateLighting,
        UpdateLighting,
        PathTraceAll,
        PathTracePrePass,
        VBufferExport,
        LightingUpdateEnd,
        PathTrace,
        RTXDI,
        DenoisingGuidesBake,
        Denoising,
        PostProcessAA,
        ToneMapping,
        TestRaygenPPHDR,
        EdgeDetection,
        DLSS,
        DLSS_RR,
        Blit,
        PrepareInputs,
        Frame,

        Count
    };
};
