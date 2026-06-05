# RTXPT 渲染帧 Pass 分析
总体流程是

- LightingUpdateBegin
    - ControlDataSetup
    - ResetLightProxyCounters
    - ResetPastToCurrentHistory
    - EnvLightsBackupPast
    - EnvmapAndAnalyticLightBuffers
    - EnvLightsSubdivideBase
    - EnvLightsSubdivideBoost
    - BakeEmissiveTriangles
    - EnvLightFillLookupMap
    - EnvLightsMapPastToCurrent
    - ProcessFeedbackHistoryPreFilter
    - ProcessFeedbackHistoryP0
    - ComputeWeights
    - ComputeProxyCounts
    - ComputeProxyBaselineOffsets
    - CreateProxyJobs
    - ExecuteProxyJobs
- PathTracePrePass
- VBufferExport
- LightingUpdateEnd
    - ProcessFeedbackHistoryP1a
    - ProcessFeedbackHistoryP1b
    - ProcessFeedbackHistoryP2
    - ProcessFeedbackHistoryP3
    - ClearFeedbackHistory
- PathTrace
- DenoisingGuidesBake
    - DenoiseSpecHitT
    - ComputeAvgLayerRadiance
- DLSS-RR
    - DLSS-RR_PrepareInputs
    - NVSDK
- Bloom
- Luminance
- ToneMapping
- ShaderDebug
- Blit

## 1. updateLighting → LightingUpdateBegin

整个光照系统的前半段，在 PathTracePrePass **之前**执行。核心目的是把当前帧所有灯光的元数据构建好，让 GPU 能正确采样。

### 1.1 ControlDataSetup
CPU→GPU 数据上传。把 CPU 上计算好的 `LightingControlData` 控制结构体写入 `m_controlBuffer`，包含：
- 当前帧灯光总数（EnvMap 四叉树节点数 + 解析光 + 发光三角形数）
- 历史帧灯光数
- 样本缓冲区偏移（ping-pong 索引）

是后续所有 Compute Pass 的数据依赖。

### 1.2 ResetLightProxyCounters
**清零采样代理计数器。** 把 `m_perLightProxyCounters` 缓冲区清零（每盏灯一个计数器，表示它需要多少个采样代理位置）。为后续 `ComputeProxyCounts` 做准备。

### 1.3 ResetPastToCurrentHistory
**清理历史映射表。** 把上一帧→当前帧的灯光 ID 映射表（`historyRemapPastToCurrent`）重置为无效值，避免旧映射污染当前帧。帧间灯光数量/顺序变化时（如场景变化）尤为重要。

### 1.4 EnvLightsBackupPast
**备份上一帧的 EnvMap 四叉树光节点。** 在当前帧重新细分 EnvMap 之前，把上一帧 `lightsBuffer` 中的 EnvMap 部分拷贝到历史缓冲区，以便后续 `EnvLightsMapPastToCurrent` 进行时序映射。

### 1.5 EnvmapAndAnalyticLightBuffers
**批量上传灯光数据到 GPU。** 把 CPU 端已处理好的解析光（点光/聚光/方向光）和 EnvMap 四叉树节点的 `PolymorphicLightInfo` structs 写入 `m_lightsBuffer` 和 `m_lightsExBuffer`，同时上传灯光历史映射索引表。

### 1.6 EnvLightsSubdivideBase
**EnvMap 四叉树基础细分。** 将 EnvMap Cubemap 的重要性图做正四叉树细分，把高亮区域分解成较小的叶节点。每个叶节点代表一个"环境光源"，存储其方向范围和辐射功率，输出到 scratchBuffer。

### 1.7 EnvLightsSubdivideBoost
**EnvMap 四叉树 Boost 细分。** 对摄像机视锥体内可见度高的区域进行额外的自适应细分，提高这些区域的光源采样密度。这是让 NEE 采样更高效的关键：把 budget 分配到真正重要的方向。

### 1.8 BakeEmissiveTriangles
**发光三角形数据 Bake。** CPU 端已找出场景中所有发光材质的三角形并打包为 `EmissiveTrianglesProcTask` 任务数组上传到 GPU。这个 Pass 在 GPU 上并行计算每个发光三角形的世界空间参数（位置、法线、面积、功率）并写入 `m_lightsBuffer`。

### 1.9 EnvLightFillLookupMap
**填充 EnvMap 查找纹理。** 把 EnvMap 四叉树结构"投影"到一张二维查找图（`m_envLightLookupMap`），使得给定一个方向能快速找到对应的四叉树叶节点 ID。路径追踪器在 NEE 采样 EnvMap 时用它。

### 1.10 EnvLightsMapPastToCurrent
**历史 EnvMap 节点 ID 映射。** 将上一帧的四叉树叶节点 ID 映射到当前帧对应节点，填充 `historyRemapPastToCurrent` 中的 EnvMap 部分。时序重用灯光重要性权重时需要这个映射来保持连续性。

### 1.11 ProcessFeedbackHistoryPreFilter
**反馈历史预滤波（可选）。** 对上一帧路径追踪器写回的"采样反馈"做空间预滤波，用于减少高频噪声后再输入到权重计算。只有启用 `m_importanceBoost_PreFilter` 时才执行。

### 1.12 ProcessFeedbackHistoryP0
**反馈历史处理 Phase 0。** 将上一帧的反馈数据（`FeedbackTotalWeight` + `FeedbackCandidates`）应用历史→当前帧的灯光 ID 重映射，转换为当前帧坐标系下的有效反馈。这是 **NEE Adaptive Training (NEE-AT)** 系统的核心：用路径追踪器实际用到的灯光数据来指导本帧的灯光权重。

### 1.13 ComputeWeights
**计算灯光采样权重。** 综合三部分信息为每盏灯计算最终权重：
1. 基于功率的先验权重
2. 基于历史反馈的统计权重（哪些灯实际贡献了亮度）
3. 基于历史时序权重的混合比例

结果写入 `m_lightWeights` 缓冲区，后续的 Proxy 系统和路径追踪器的 NEE 采样都用它。

### 1.14 ComputeProxyCounts
**计算每盏灯的采样代理数量。** 根据 `m_lightWeights` 中的权重，把有限的采样代理 slot 总数（`RTXPT_LIGHTING_MAX_SAMPLING_PROXIES`）分配给各灯，权重越大获得越多代理，输出到 `m_perLightProxyCounters`。

### 1.15 ComputeProxyBaselineOffsets
**前缀和：计算代理基地址。** Parallel prefix sum 计算每盏灯的代理在 `m_lightSamplingProxies` 缓冲区中的起始偏移量。单线程（`dispatch(1,1,1)`）在 GPU 上做串行累加。

### 1.16 CreateProxyJobs
**生成代理构建任务列表。** 为每盏灯生成"我需要往哪些 proxy slot 里写入自己 ID"的任务，存储到 scratch buffer，供 `ExecuteProxyJobs` 并行执行。

### 1.17 ExecuteProxyJobs
**执行代理写入。** 并行填充 `m_lightSamplingProxies`（即 `t_LightLocalSamplingBuffer`）：每个 proxy slot 写入对应灯光的 ID。路径追踪器的 NEE 在采样时直接从这个表里根据当前 tile 的局部分布随机选取灯光，实现**自适应局部灯光采样**。

---

## 2. PathTracePrePass（BuildStablePlanes RT Pass）

宏：`PATH_TRACER_MODE = PATH_TRACER_MODE_BUILD_STABLE_PLANES`

以 Whitted-style 只追踪 **delta 路径**（完美镜面 / 折射链），不做 NEE 或漫反射采样，目的是为每像素建立最多 3 层 `StablePlane` 几何骨架，供后续 `PathTrace`（FillStablePlanes）复用。

### 输出详解

#### 2.1 `u_Depth`（`RWTexture2D<float>`, register u6）
**NDC 深度（clip-space z/w）。**

写入位置：`Bridge::ExportSurface` / `Bridge::ExportNonSurface`

- **命中表面**：取 dominant StablePlane 的 *virtualWorldPos*（沿摄像机光线走 `sceneLengthForMVs` 的虚拟世界坐标，穿透了镜面/折射链后的等效位置）变换到 clip space，写 `clipPos.z / clipPos.w`。  
- **Miss（天空）**：用天空等效距离 `kEnvironmentMapSceneDistance` 作 virtualWorldPos，同样输出 clip-space z/w。  
- **无效帧**：初始化为 `0`（`ExportSurfaceInit`），作为下游判断数据是否有效的信号。

**用途**：DLSS-RR / TAA 的深度输入；`LightingUpdateEnd` 的 `ProcessFeedbackHistoryP1a~P3` 用它进行深度比较和 tile 反馈聚合。

---

#### 2.2 `u_MotionVectors`（`RWTexture2D<float4>`, register u5）
**屏幕空间运动向量（xy 为像素偏移，zw 预留为 0）。**

写入位置：`Bridge::ExportSurface` / `Bridge::ExportNonSurface`

计算过程（`StablePlanesHandleHit` → `setAsBase` 分支）：
1. `virtualWorldPos = cameraRay.origin + cameraRay.dir * sceneLengthForMVs`  
   ↳ `sceneLengthForMVs` 取自 `MotionVectorSceneLength`（若某顶点标记了 PSD 截断点则在该处锁住）或完整路径长度；对 miss 则用天空距离。
2. `worldMotion = surfaceData.prevPosW − posW`（表面本身在上一帧的世界位移）
3. 通过累积的 `imageXform`（镜面/折射堆叠的旋转矩阵）将 `worldMotion` 变换为等效虚拟运动：`virtualWorldMotion = imageXform × worldMotion`
4. `motionVectors = computeMotionVector(virtualWorldPos, virtualWorldPos + virtualWorldMotion)` → 两帧投影差

**特殊情况**：若路径曾经过标记了 `isPSDBlockMotionVectorsAtSurface()` 的高曲率表面（`blockedAtSurface = true`），则 roughness 被强制压低（当作镜面），让 DLSS-RR 用 specular MV 路径推断运动。

**用途**：DLSS-RR / TAA 的运动向量输入；`LightingUpdateEnd` 中的反馈重映射（P1a~P3）需要它。

---

#### 2.3 `u_Throughput`（`RWTexture2D<uint>`, register u4，Pack_R11G11B10_FLOAT）
**穿透率（throughput）：路径从摄像机到 dominant StablePlane 基点所有镜面/折射界面的累积能量透过率。**

写入位置：`Bridge::ExportSurface`

- **命中表面**：`Pack_R11G11B10_FLOAT(saturate(path.GetThp()))` — 把 `float3 thp` 压缩为 R11G11B10 编码写入。  
- **Miss / 无表面**：写 `0`（ExportNonSurface）。

`thp` 的含义：  
每次 delta lobe scatter 时路径会执行 `SplitDeltaPath`，其中对 throughput 乘以 BSDF lobe 的 `thp`（`lobe.thp`），等效于镜面菲涅耳 × 折射 IOR 校正。最终写入的是 **从相机到这个 stable plane 基点的累积 spectral throughput**。

**用途**：FillStablePlanes pass 的 BSDF estimate / denoising weight；ReSTIR DI/GI 的可见性估计（G-buffer weight）。

---

#### 2.4 `u_SpecularHitT`（`RWTexture2D<float>`, register u3）
**镜面链命中距离（specular hit distance），用于降噪器的 specular lobe hit-T 输入。**

写入时序分两步（仅 dominant StablePlane 路径）：

1. **ExportSpecHitTStart**（命中 dominant 表面时）：写入 `−path.GetSceneLength()`（负值，作 flag 标记"已记录起点"）。  
2. **ExportSpecHitTStop**（漫反射弹射首次命中时）：读出负值 `denoisingSceneLength`，计算 `specHitT = max(0, currentSceneLength + denoisingSceneLength)`，即 **镜面链结束点到首次漫反射命中点的光线段长度**，再写回正值。  
   若仍为负（路径未找到漫反射端点，如进入天空），则保持不变；下游 `DenoiseSpecHitT` pass 会对结果做空间滤波。

**用途**：DLSS-RR 和 NRD 降噪器使用 SpecularHitT 估算镜面反射的命中深度，改善时序稳定性和 specular denoising 质量。

---

#### 2.5 `u_StablePlanesHeader`（`RWTexture2DArray<uint>`, register u40）
**StablePlane 分支 ID 头表，每像素最多 3 层。**

格式：`[W × H × 4]` 的 uint 数组  
- Slice 0～2：每个 plane 对应的 `stableBranchID`  
  - `cStablePlaneInvalidBranchID (0xFFFFFFFF)`：该 plane 未使用  
  - `cStablePlaneEnqueuedBranchID (0xFFFFFFFE)`：已入队等待探索  
  - 正常值：delta 路径的位编码分支 ID（每 2 bit 代表一个 delta lobe 选择）  
- Slice 3：`asuint(FirstHitRayLength)` — 第一次表面命中的光线长度（由 `StoreFirstHitRayLengthAndClearDominantToZero` 写入）

**用途**：FillStablePlanes pass 通过 Header 中的 branchID 判断每条路径属于哪个 plane（`StablePlaneIsOnPlane`），并决定 radiance 沉积目标。

---

#### 2.6 `u_StablePlanesBuffer`（`RWStructuredBuffer<StablePlane>`, register u42）
**每个 StablePlane 的完整几何与辐射数据。**

`StablePlane` 结构体字段（`StablePlanes.hlsli`）：

| 字段 | 含义 |
|------|------|
| `RayOrigin` | 该 plane 基点的世界坐标（表面命中点，减去偏移以便重新追踪） |
| `RayDir` | 到达该基点的入射方向 |
| `SceneLength` | 从相机到该基点的累积光线长度 |
| `LastRayTCurrent` | 最后一次光线 t 值（预留字段） |
| `VertexIndexAndRoughness` | 高 16 bit = 顶点深度索引；低 16 bit = f16 roughness |
| `PackedThpAndMVs` | fp16 packed：`thp.xyz`（3通道）+ `motionVectors.xyz`（3通道） |
| `PackedDenoiserSigmaAndCounts` | 降噪 sigma 估计 + bounce 计数 |
| `PackedBSDFEstimate` | diff/spec BSDF estimate（fp16 packed，供降噪器用作 albedo 分解） |
| `PackedNoisyRadianceAndSpecAvg` | FillStablePlanes 写入的含噪辐射（fp16 accumulate） |
| `FlagsAndVertexIndex / PackedCounters` | flags（dominant、onBranch 等）和各类计数器 |

**写入**：`StablePlanesContext::StoreStablePlane()` 在每条 delta 路径到达基点时调用。  
**用途**：FillStablePlanes pass 从每个 plane 的 `RayOrigin + RayDir` 出发，发射漫反射/NEE 光线；降噪器读取 roughness、worldNormal、BSDFEstimate 作 guide buffer。

---

#### 2.7 `u_StableRadiance`（`RWTexture2D<float4>`）
**稳定辐射（StableRadiance）：delta 路径沿途所有无噪声辐射的累积。**

写入路径（`PathTracer.hlsli` → `AccumulatePathRadiance`）：

```hlsl
#elif PATH_TRACER_MODE==PATH_TRACER_MODE_BUILD_STABLE_PLANES
    workingContext.StablePlanes.AccumulateStableRadiance(path.GetPixelPos(), radiance);
```

在 BUILD pass 中，**所有 `AccumulatePathRadiance` 调用都写到这里**，包括：
- delta 路径途经的 **emissive 表面**直接发光
- delta 路径最终 miss 时命中的**天空 / EnvMap 辐射**

初始化：`StartPixel` 中先调用 `StoreStableRadiance(pixelPos, 0)` 清零，再逐段累加。

对比 FILL pass：当 `stablePlaneOnBranch == true` 时跳过写入 `path.L`，因为 BUILD pass 已将该段收入 `StableRadiance`；只有离开 delta 分支（`stablePlaneOnBranch == false`）的漫反射辐射才进入含噪的 `path.L`。

**用途**：PostProcess / Blit 合成时直接叠加到最终图像，不经过 denoiser。由于 delta 路径是确定性的（无随机采样），这部分辐射本身无噪且时序稳定。

---

### 输出总结

| 输出 | 格式 | 写入条件 | 主要下游消费者 |
|------|------|---------|----------------|
| `Depth` | float (clip-space z/w) | dominant plane 命中/miss | LightingUpdateEnd、DLSS-RR |
| `MotionVectors` | float4 (xy=屏幕偏移) | dominant plane 命中/miss | LightingUpdateEnd、DLSS-RR/TAA |
| `Throughput` | uint (R11G11B10) | dominant plane 命中 | FillStablePlanes、ReSTIR |
| `SpecularHitT` | float | dominant plane + 后续漫反射命中 | DenoiseSpecHitT、DLSS-RR |
| `StablePlanesHeader` | uint[W×H×4] | 每个 delta 分支 | FillStablePlanes |
| `StablePlanesBuffer` | struct[W×H×3] | 每个 delta 分支 | FillStablePlanes、Denoiser |
| `StableRadiance` | float4 | delta 路径上的 emissive 表面 | PostProcess/Blit |

> **注意**：此 Pass 输出的 `Depth` 和 `MotionVectors` 是 `LightingUpdateEnd` 的必要输入，这是它被插在 `LightingUpdateBegin` 和 `LightingUpdateEnd` 之间的原因。

---

少了一个 VBufferExport

## 3. LightingUpdateEnd

### 3.1 ProcessFeedbackHistoryP1a
**反馈聚合（全局，低分辨率）。** 把上一帧路径追踪器写入的全分辨率反馈图降采样/聚合到低分辨率的 Blended 版本（`NEE_AT_FeedbackTotalWeightBlended`），用于全局灯光采样权重更新。

### 3.2 ProcessFeedbackHistoryP1b
**反馈聚合（局部，全分辨率）。** 对全分辨率反馈做时序混合，生成 Scratch 版本供 P2 读取。

### 3.3 ProcessFeedbackHistoryP2
**构建局部采样缓冲区（Local Sampling Buffer）。** 按屏幕 tile 划分，为每个 tile 从当前反馈中选出最重要的若干盏灯，写入 `m_NEE_AT_LocalSamplingBuffer`（即 `t_LightLocalSamplingBuffer`）。路径追踪器 NEE 时优先查询本 tile 的局部采样列表，实现**屏幕自适应局部重要性采样**。

### 3.4 ProcessFeedbackHistoryP3
**历史深度更新。** 更新 `NEE_AT_HistoryDepth` 缓冲区，记录每个 tile 的反馈积累帧数（类似 TAA 的历史置信度），用于下一帧混合权重时的方差估计。

### 3.5 ClearFeedbackHistory
**清零当前帧反馈缓冲区。** 把 `FeedbackTotalWeight` 和 `FeedbackCandidates` 清零，准备接收当前帧路径追踪器的新反馈写入。

---

## 4. PathTrace（FillStablePlanes RT Pass）

从每个 StablePlane 的端点出发，追踪含噪声的漫反射路径，使用 NEE（配合上面构建的局部采样代理）采样直接光。同时：
- 将辐射结果沉积到各 StablePlane 的辐射缓冲区
- 写入 DLSS-RR 所需的 albedo 分解数据（`RRDiffuseAlbedo` / `RRSpecAlbedo` / `RRNormalsAndRoughness`）
- **向 `FeedbackTotalWeight/FeedbackCandidates` 写入当前帧的灯光使用反馈**，供下一帧 `LightingUpdateEnd` 消费

---

## 5. Denoising Guides Bake

### 5.1 DenoiseSpecHitT
**滤波 SpecularHitT 缓冲区。** 对路径追踪器输出的 `SpecularHitT`（镜面弹射命中距离）做空间滤波，提供给降噪器作为镜面 hit distance 估计输入，改善时序稳定性。

### 5.2 ComputeAvgLayerRadiance
**计算各 StablePlane 的平均辐射亮度（半分辨率）。** 将每个 StablePlane 的 Denoiser 输入辐射在半分辨率下求平均，写入 `DenoiserAvgLayerRadianceHalfRes`，用于 DLSS-RR 的输入准备和 firefly 过滤参考。

---

## 6. DLSS-RR

### 6.1 DLSS-RR_PrepareInputs
**应用层封装准备。** 整理并绑定 Streamline 所需的所有输入纹理：

| 输入 | 说明 |
|------|------|
| `OutputColor` | 含噪声 HDR 颜色 |
| `RRDiffuseAlbedo / RRSpecAlbedo` | 辐射分解 Albedo |
| `RRNormalsAndRoughness` | 法线 + 粗糙度（Packed 格式） |
| `RRSpecMotionVectors` | 镜面专用运动向量 |
| `DenoiserViewspaceZ` | 视空间深度 |
| `ScreenMotionVectors` | 漫反射运动向量 |

同时填写 `DLSSRROptions`（分辨率、预曝光等）并调用 Streamline API。

### 6.2 NVSDK
**驱动侧 DLSS-RR 执行（不透明）。** NVIDIA Streamline/DLSS SDK 内部完成：
1. **光线重建（Ray Reconstruction）**：利用 Albedo 分解和法线/粗糙度进行含噪声 specular/diffuse 辐射的时序重建，效果优于传统 TAA
2. **超分辨率上采样**：从渲染分辨率（如 0.67×）上采样到显示分辨率
3. 输出 `ProcessedOutputColor`（去噪 + 超分辨率后的 HDR 图像）

---

## 关键时序依赖
