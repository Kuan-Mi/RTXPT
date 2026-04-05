# RTXPT 渲染帧 Pass 分析

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

以 Whitted-style 追踪 delta 路径（完美镜面/折射链），为每像素建立最多 3 层 `StablePlane` 结构。输出：
- `Depth`、`MotionVectors`、`Throughput`、`SpecularHitT`
- `StablePlanesBuffer`

**故意不做 NEE/漫反射采样**，只建立几何结构。

> **注意**：此 Pass 输出的 `Depth` 和 `MotionVectors` 是 `LightingUpdateEnd` 的必要输入，这是它被插在中间的原因。

---

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
