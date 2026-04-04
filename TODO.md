# TODO

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

这会带来：

- CPU 内存复制次数偏多
- 每次场景重建成本偏高
- 多个实例引用同一个 `UStaticMesh` 时无法复用 BLAS
- 后续很难平滑演进到复用 UE 渲染器内部 ray tracing geometry

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
