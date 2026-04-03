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
