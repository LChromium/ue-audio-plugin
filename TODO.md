# TODO

## 1. 当前阶段的重要限制

### 1.1 当前是最小硬件光追后端，不是最终形态

当前已经接入一版基于 UE Ray Tracing RHI 的遮挡检测实现，并支持并行射线批处理。

当前实现方式是：

- 支持把 `UUERayTracingAudioGeometryComponent` 导出的包围盒或真实静态网格三角形作为光追输入
- `Static Mesh Triangles` 会以本地空间顶点导出，并保留实例 `LocalToWorld`
- RHI 后端会按 `StaticMesh + LOD` 缓存 Static Mesh BLAS，cache hit 时 TLAS 只消费实例 transform
- Bounding Box 和非缓存几何仍会按当前实例临时构建独立 BLAS / TLAS
- 对声源球体采样得到一批遮挡射线
- 通过 RHI 光追后端并行求交
- 在硬件光追不可用时回退到 `LineTraceSingleByChannel`

这说明它已经不是纯占位实现，但还不是最终生产级版本。

对于间接声，当前后端策略是：

- 硬件光追可用时：
  - 用 UE Ray Tracing RHI shader 返回每条反射射线的命中距离、法线和几何索引
  - UE compute shader 负责单 bounce 的 shadeAndBounce、gatherEnergyField
  - 设备级硬件路径会在单次 render command 中推进多 bounce
  - Static Mesh BLAS 会跨查询复用，TLAS 按当前声学实例重新组织
  - CPU 侧继续负责 IR 重建和 Parametric / Hybrid 参数生成，以及 CPU fallback
- 硬件光追不可用时：
  - 回退到 CPU 声学场景求交

### 1.2 当前不是完整空间音频

目前真正生效的是：

- 直接路径相关增益
- 基于 Minimal EnergyField 的第一版 IR / Parametric / Hybrid 间接声链路
- Phase 3 的 RHI 优先命中查询 + compute 单 bounce 累积 + CPU 后处理

目前还没有：

- HRTF
- 双耳空间化
- 生产级早期反射
- 生产级混响
- 完整 GPU EnergyField / IR 重建链路
- 完整方向场 / 高阶球谐 EnergyField
- 生产级长 IR 卷积器
- 声像计算

### 1.3 当前 Editor 窗口只是骨架

菜单中已经有 Bake 窗口入口，但现在它只是占位，不代表烘焙功能已经可用。

## 2. 下一步推荐使用顺序

如果你想继续验证当前插件，建议顺序是：

2.1. 先确认插件在测试工程中可启用
2.2. 再确认场景内组件能正常挂载
2.3. 对遮挡物优先试 `ExportMode = Static Mesh Triangles`
2.4. Source 先试 `IndirectMode = Hybrid Reverb`
2.5. 先观察 `bIsOccluded / DirectVisibility / OverallGain / IndirectGain / LateReverbGain`
2.6. 再做遮挡试听和房间感验证

## 1. Static Mesh BLAS 缓存优化

### 背景

当前 `GeometryComponent -> Scene Export -> RHI Backend` 的路径已经支持：

- `BoundingBox` 导出
- `StaticMeshTriangles` 导出

但当前 `StaticMeshTriangles` 的实现仍然存在明显的中间复制成本：

1. 从 `UStaticMesh` 的 LOD0 CPU 可访问数据读取顶点和索引
2. 把数据复制到 `FUERayTracingAudioGeometryExport.Vertices / Indices`
3. 在 `UERayTracingAudioRayTracingDevice.cpp` 中再次把这些数据拼接到统一数组
4. 再用统一数组创建当前查询所需的 BLAS / TLAS


### 目标

把当前“世界空间三角形复制 + 每次重建”的路径升级为：

- 按 `UStaticMesh + LODIndex` 缓存 BLAS
- 场景侧只维护实例信息和变换
- TLAS 构建只消费实例，不再重复复制整份三角形数据

### 推荐实现方向

#### 阶段 A：去掉重复拼接复制

- 在 `FUERayTracingAudioGeometryExport` 中，不再优先保存世界空间三角形数组
- 改为优先保存：
  - `UStaticMesh` 标识或可缓存键
  - `LODIndex`
  - `LocalToWorld`
  - 是否使用真实静态网格模式
- 对 `BoundingBox` 模式保留现有轻量导出

#### 阶段 B：建立 BLAS 缓存

- 在 SDK / RHI 层新增静态网格 BLAS 缓存管理器
- 缓存键建议包含：
  - `UStaticMesh*`
  - `LODIndex`
  - 必要时包含 section/layout 相关信息
- 每个缓存项至少维护：
  - 顶点/索引底层来源
  - `FRayTracingGeometryRHIRef`
  - 有效性状态
  - 最近使用信息

#### 阶段 C：TLAS 只处理实例

- Scene 导出保留每个几何实例的：
  - `LocalToWorld`
  - 吸收参数
  - 直接声可见性标记
  - 指向缓存 BLAS 的句柄或缓存键
- 每次查询时：
  - 不再拼接世界空间顶点
  - 只基于实例构建 TLAS

#### 阶段 D：向 UE 内部缓存几何靠拢

- 调研并评估是否能安全地复用 UE 渲染器内部的：
  - `FPrimitiveSceneInfo::GetStaticRayTracingGeometry`
  - `GetCachedRayTracingGeometry`
- 这一阶段要重点确认：
  - 生命周期
  - 游戏线程 / 渲染线程访问边界
  - 插件模块是否能在不深度侵入 Renderer 的情况下拿到有效句柄

### 当前已知限制

- UE 内部确实存在缓存的 ray tracing geometry / instance
- 但当前插件架构仍是“插件自建声学场景”，还没有直接接到渲染器的 cached scene
- 因此短期最稳妥的优化路径不是直接复用最终 handle，而是：
  - 先做 `StaticMesh BLAS` 缓存
  - 再评估是否进一步复用 UE 内部缓存几何

### 预期收益

- 减少三角形 CPU 复制
- 减少场景更新和查询前准备成本
- 同一静态网格多实例场景下显著降低 BLAS 重建开销
- 为后续真实生产级 RHI 后端打基础

### 相关代码位置

- `Source/UERayTracingAudio/Private/Components/UERayTracingAudioGeometryComponent.cpp`
- `Source/UERayTracingAudioSDK/Public/Scene/UERayTracingAudioScene.h`
- `Source/UERayTracingAudioSDK/Private/RayTracing/UERayTracingAudioRayTracingDevice.cpp`

## 2. Direct / Indirect 声学行为修正

### 背景

当前在 UE 中实测发现两个明显行为问题：

1. 直接声在“完全遮挡”时，`OverallGain` 仍然不是 `0`
2. 间接声在 listener 和 source 位于墙体同侧时，靠墙时可能有有效反射路径，但离墙稍远后又会突然掉成 `0`

这两个现象都符合当前实现逻辑，但不符合更理想的听感目标，因此应当作为后续修正项。

### 直接声问题分析

当前 `OverallGain` 的定义是：

- `DistanceAttenuation * AirAbsorption * Occlusion`

其中 `Occlusion` 当前不是硬遮挡，而是：

- `Lerp(OccludedGain, 1.0, DirectVisibility)`

这意味着：

- 当 `DirectVisibility = 0` 时，结果不是 `0`
- 而是退到 `OccludedGain`

当前默认 `OccludedGain = 0.2`，所以“完全遮挡不为 0”是当前设定导致的，不是数值错误。

### 直接声后续改进方向

- 增加可选模式：
  - 保留当前 soft occlusion
  - 新增 hard occlusion，使完全遮挡时直接声更接近 `0`
- 重新设计 `OccludedGain` 的语义：
  - 作为艺术参数保留
  - 或只对部分遮挡生效
- 检查 `DirectVisibility` 的采样稳定性：
  - 增加 sample 数
  - 调整 source 半径与 sample 分布

### 间接声问题分析

当前间接声还不是基于 UE RHI shader 的多跳反射链路，而是：

- CPU 侧简化多跳路径追踪
- 稀疏球面方向采样
- 多次镜面反射
- 每帧独立求解

当前会突然掉到 `0` 的主要原因包括：

- `NumReflectionRays` 默认值较低，采样过 sparse
- 没有针对 listener 的 importance sampling
- 没有 image-source 一阶反射几何求解
- 没有 temporal accumulation / smoothing
- 小路径贡献会被阈值裁掉

因此“远离墙后突然为 0”通常不是物理上真的没有反射，而是当前采样原型没有采到有效路径。

### 间接声后续改进方向

#### 阶段 A：先解决突变问题

- 提高默认 `NumReflectionRays`
- 给 `IndirectGain / EarlyReflectionGain / LateReverbGain` 增加时间平滑
- 让 late reverb 有最小残留尾部而不是单帧直接清零
- 降低或重构当前小路径裁剪逻辑

#### 阶段 B：提升一阶反射稳定性

- 对第一反射实现 image-source 或 listener-aware sampling
- 针对“source / listener 同侧靠墙”场景提升稳定命中率
- 区分早期反射和尾部反射的采样策略

#### 阶段 C：迁移到正式 RHI 多跳反射链路

- 使用 UE Ray Tracing RHI 的自定义 shader 做多 bounce 查询
- 在 hit payload 中返回：
  - hit distance
  - normal
  - material / absorption
  - primitive / instance 信息
- 在 GPU 上累计 path contributions / energy histogram
- CPU 侧仅负责生成：
  - early taps
  - RT60 / EQ / delay
  - hybrid 参数

### 目标

- 让直接声遮挡更符合“完全挡住时几乎听不到直达声”的预期
- 让间接声不会因为 listener 稍微移动就突然归零
- 让 Phase 3 从“可运行原型”升级为“更稳定、可试听调参”的实时反射系统

### 相关代码位置

- `Source/UERayTracingAudioSDK/Private/Simulation/UERayTracingAudioSimulator.cpp`
- `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.cpp`
- `Source/UERayTracingAudio/Private/Components/UERayTracingAudioSourceComponent.cpp`
