/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "Profiler.h"
#include <donut/app/DeviceManager.h>
#include <imgui.h>

// Must match ProfilerSections.h::ProfilerSection::Enum
static const char* g_SectionNames[ProfilerSection::Count] = {


    "SkinnedBLASUpdates",
    "TlasUpdate",
    "PreUpdateLighting",
    "UpdateLighting",
    "PathTraceAll",
    "PathTracePrePass",
    "VBufferExport",
    "LightingUpdateEnd",
    "PathTrace",
    "RTXDI",
    "DenoisingGuidesBake",
    "Denoising",
    "PostProcessAA",
    "ToneMapping",
    "TestRaygenPPHDR",
    "EdgeDetection",
    "DLSS",
    "DLSS_RR",
    "Blit",
    "PrepareInputs",
    "Frame",

};

Profiler::Profiler(donut::app::DeviceManager& deviceManager)
    : m_deviceManager(deviceManager)
    , m_device(deviceManager.GetDevice())
{
    for (auto& query : m_timerQueries)
        query = m_device->createTimerQuery();

    m_timersUsed.fill(false);
    m_timerValues.fill(0.0);
}

bool Profiler::IsEnabled() const
{
    return m_enabled;
}

void Profiler::EnableProfiler(bool enable)
{
    m_enabled = enable;
}

void Profiler::EnableAccumulation(bool enable)
{
    m_isAccumulating = enable;
}

void Profiler::ResetAccumulation()
{
    m_accumulatedFrames = 0;
    m_timerValues.fill(0.0);
}

void Profiler::ResolvePreviousFrame()
{
    m_activeBank = !m_activeBank;

    if (!m_enabled)
        return;

    for (uint32_t section = 0; section < ProfilerSection::Count; section++)
    {
        double time = 0.0;

        const uint32_t timerIndex = section + m_activeBank * ProfilerSection::Count;

        if (m_timersUsed[timerIndex])
        {
            time = double(m_device->getTimerQueryTime(m_timerQueries[timerIndex]));
            time *= 1000.0; // seconds -> milliseconds
        }

        m_timersUsed[timerIndex] = false;

        if (m_isAccumulating)
            m_timerValues[section] += time;
        else
            m_timerValues[section] = time;
    }

    if (m_isAccumulating)
        m_accumulatedFrames += 1;
    else
        m_accumulatedFrames = 1;
}

void Profiler::BeginFrame(nvrhi::ICommandList* commandList)
{
    if (!m_enabled)
        return;

    BeginSection(commandList, ProfilerSection::Frame);
}

void Profiler::EndFrame(nvrhi::ICommandList* commandList)
{
    EndSection(commandList, ProfilerSection::Frame);
}

void Profiler::BeginSection(nvrhi::ICommandList* commandList, ProfilerSection::Enum section)
{
    if (!m_enabled)
        return;

    const uint32_t timerIndex = section + m_activeBank * ProfilerSection::Count;
    commandList->beginTimerQuery(m_timerQueries[timerIndex]);
    m_timersUsed[timerIndex] = true;
}

void Profiler::EndSection(nvrhi::ICommandList* commandList, ProfilerSection::Enum section)
{
    if (!m_enabled)
        return;

    const uint32_t timerIndex = section + m_activeBank * ProfilerSection::Count;
    commandList->endTimerQuery(m_timerQueries[timerIndex]);
}

double Profiler::GetTimer(ProfilerSection::Enum section) const
{
    if (m_accumulatedFrames == 0)
        return 0.0;

    return m_timerValues[section] / double(m_accumulatedFrames);
}

void Profiler::BuildUI()
{
    const float timeColumnWidth = 80.f;

    ImGui::BeginTable("Profiler", 2, ImGuiTableFlags_BordersInnerH);
    ImGui::TableSetupColumn(" Section");
    ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, timeColumnWidth);
    ImGui::TableHeadersRow();

    for (uint32_t section = 0; section < ProfilerSection::Count; section++)
    {
        if (section == ProfilerSection::Frame)
            ImGui::Separator();

        const double time = GetTimer(ProfilerSection::Enum(section));
        if (time == 0.0)
            continue;

        const bool highlight = (section == ProfilerSection::Frame);
        if (highlight)
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0xff, 0xff, 0x40, 0xff));

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s", g_SectionNames[section]);
        ImGui::TableSetColumnIndex(1);

        char text[24];
        snprintf(text, sizeof(text), "%.3f ms", time);
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        ImGui::SameLine(timeColumnWidth - textSize.x);
        ImGui::Text("%s", text);

        if (highlight)
            ImGui::PopStyleColor();
    }

    ImGui::EndTable();
}

// ---- ProfilerScope ----

ProfilerScope::ProfilerScope(Profiler& profiler, nvrhi::ICommandList* commandList, ProfilerSection::Enum section)
    : m_profiler(profiler)
    , m_commandList(commandList)
    , m_section(section)
{
    m_profiler.BeginSection(m_commandList, m_section);
}

ProfilerScope::~ProfilerScope()
{
    m_profiler.EndSection(m_commandList, m_section);
}
