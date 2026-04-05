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

#include <nvrhi/nvrhi.h>
#include <array>

#include "ProfilerSections.h"

namespace donut::app
{
    class DeviceManager;
}

// GPU per-pass timer profiler. Adapted from FullSample/Source/Profiler.h.
// Uses nvrhi::ITimerQuery with double-buffering to avoid CPU/GPU sync stalls.
class Profiler
{
public:
    explicit Profiler(donut::app::DeviceManager& deviceManager);

    bool IsEnabled() const;
    void EnableProfiler(bool enable);
    void EnableAccumulation(bool enable);
    void ResetAccumulation();

    // Must be called once per frame BEFORE BeginFrame, to copy previous frame's resolved results.
    void ResolvePreviousFrame();

    void BeginFrame(nvrhi::ICommandList* commandList);
    void EndFrame(nvrhi::ICommandList* commandList);

    void BeginSection(nvrhi::ICommandList* commandList, ProfilerSection::Enum section);
    void EndSection(nvrhi::ICommandList* commandList, ProfilerSection::Enum section);

    // Returns the averaged timer value (ms) for a section.
    double GetTimer(ProfilerSection::Enum section) const;

    void BuildUI();

private:
    bool    m_enabled = true;
    bool    m_isAccumulating = false;
    uint32_t m_accumulatedFrames = 0;
    uint32_t m_activeBank = 0;

    std::array<nvrhi::TimerQueryHandle, ProfilerSection::Count * 2> m_timerQueries;
    std::array<double, ProfilerSection::Count>  m_timerValues{};
    std::array<bool,   ProfilerSection::Count * 2> m_timersUsed{};

    donut::app::DeviceManager& m_deviceManager;
    nvrhi::DeviceHandle m_device;
};

// RAII helper: calls BeginSection on construction, EndSection on destruction.
class ProfilerScope
{
public:
    ProfilerScope(Profiler& profiler, nvrhi::ICommandList* commandList, ProfilerSection::Enum section);
    ~ProfilerScope();

    ProfilerScope(const ProfilerScope&) = delete;
    ProfilerScope(ProfilerScope&&) = delete;
    ProfilerScope& operator=(const ProfilerScope&) = delete;
    ProfilerScope& operator=(ProfilerScope&&) = delete;

private:
    Profiler& m_profiler;
    nvrhi::ICommandList* m_commandList;
    ProfilerSection::Enum m_section;
};
