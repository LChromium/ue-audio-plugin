# 间接声硬件光追实现说明

本文记录当前按照 `TODO.md` 推进的间接声硬件光追实现，包括设计、运行流程、接口和启动方式。

## 1. 设计目标

当前目标不是直接接管 Unreal Renderer 的正式光追场景，而是在插件自建的声学场景里完成一条稳定的硬件光追链路：

- `UUERayTracingAudioGeometryComponent` 负责把场景几何导出成声学场景。
- `FUERayTracingAudioScene` 保存每个声学几何实例的变换、吸收参数、可见性标记和静态网格缓存键。
- `FUERayTracingAudioRayTracingDevice` 在硬件光追可用时构建声学 BLAS / TLAS，并执行间接声多 bounce 查询。
- `FUERayTracingAudioReflectionSimulator` 优先调用设备级 `SimulateIndirectEnergyField(...)`，失败或不可用时回退到 CPU 声学场景求交。

这次实现重点解决 `TODO.md` 里的 Static Mesh BLAS 缓存问题：

- Static Mesh 三角形导出改为保存本地空间顶点。
- 每个 Static Mesh 实例保留 `LocalToWorld` 变换和 `StaticMesh + LOD` 缓存键。
- Render thread 侧按缓存键复用 `FRayTracingGeometryRHIRef`、顶点 buffer、索引 buffer。
- TLAS 只添加 instance transform，不再在 cache hit 时重新拼接世界空间顶点。
- CPU fallback 通过 `GetVertexWorldPosition(...)` 仍然得到世界空间三角形，保证旧路径行为不变。

## 2. 数据结构

核心结构是 `FUERayTracingAudioGeometryExport`：

- `Transform`：声学几何实例的 LocalToWorld。
- `Bounds` / `Extent`：包围盒导出模式继续使用的轻量几何。
- `Absorption`：三频段吸收参数。
- `bVisibleForDirectSound`：是否参与直接声可见性。
- `bUseStaticMeshTriangles`：是否使用真实静态网格三角形。
- `bVerticesAreLocalSpace`：顶点数组是否为本地空间。
- `StaticMeshCacheKey`：Static Mesh 资源路径。
- `StaticMeshLODIndex`：当前导出的 LOD。
- `Vertices` / `Indices`：Static Mesh 模式下保存本地顶点和索引；Bounding Box 模式下由 RHI 后端临时生成盒体三角形。

辅助接口：

- `HasCachedStaticMeshSource()`：判断该几何是否可以走 Static Mesh BLAS cache。
- `GetRayTracingGeometryCacheKey()`：生成 RHI cache key。
- `GetVertexWorldPosition(VertexIndex)`：供 CPU fallback 和调试路径取世界空间顶点。

## 3. 运行流程

### 3.1 场景导出

入口文件：

- `Source/UERayTracingAudio/Private/Components/UERayTracingAudioGeometryComponent.cpp`

流程：

1. `UUERayTracingAudioGeometryComponent::BuildGeometryExport(...)` 读取宿主 Actor 的 `UPrimitiveComponent`。
2. `ExportMode = StaticMeshTriangles` 时读取 `UStaticMesh` LOD0 的 CPU 顶点/索引。
3. 顶点以本地空间写入 `Vertices`。
4. 写入 `StaticMeshCacheKey`、`StaticMeshLODIndex` 和 `bVerticesAreLocalSpace = true`。
5. Manager 重建场景时把这些实例放进 `FUERayTracingAudioScene`。

### 3.2 间接声调度

入口文件：

- `Source/UERayTracingAudio/Private/Components/UERayTracingAudioSourceComponent.cpp`
- `Source/UERayTracingAudio/Private/Managers/UERayTracingAudioManager.cpp`
- `Source/UERayTracingAudioSDK/Private/Simulation/UERayTracingAudioSimulator.cpp`
- `Source/UERayTracingAudioSDK/Private/Simulation/UERayTracingAudioReflectionSimulator.cpp`

流程：

1. `UUERayTracingAudioSourceComponent::TickComponent(...)` 每帧请求间接声结果。
2. `FUERayTracingAudioManager::SimulateIndirectSource(...)` 组装 `FUERayTracingAudioIndirectSimulationInput`。
3. `FUERayTracingAudioSimulator::SimulateIndirectSound(...)` 创建 Minimal EnergyField。
4. `FUERayTracingAudioReflectionSimulator::Simulate(...)` 判断 `RayTracingDevice.IsRayTracingAvailable()`。
5. 硬件光追可用时优先调用 `RayTracingDevice.SimulateIndirectEnergyField(...)`。
6. 调用失败或硬件不可用时回退到 CPU 多 bounce 声学场景求交。

### 3.3 RHI 场景构建

入口文件：

- `Source/UERayTracingAudioSDK/Private/RayTracing/UERayTracingAudioRayTracingDevice.cpp`

关键函数：

- `BuildRayTracingSceneFromGeometry(...)`
- `GetOrCreateCachedStaticMeshRayTracingGeometry(...)`
- `CreateRayTracingGeometryFromArrays(...)`

流程：

1. 进入 render thread 后复制当前声学几何快照。
2. 对每个可见几何实例：
   - Static Mesh cache hit：直接复用缓存中的 `FRayTracingGeometryRHIRef`。
   - Static Mesh cache miss：用本地空间顶点创建 BLAS，并写入 cache。
   - Bounding Box 或非缓存几何：临时生成世界空间三角形并创建瞬时 BLAS。
3. Static Mesh 实例写入 `GeometryExport.Transform.ToMatrixWithScale()` 作为 TLAS transform。
4. 非本地空间几何使用 identity transform。
5. 构建声学 TLAS，供本次直接声或间接声查询使用。

### 3.4 间接声硬件光追循环

入口函数：

- `FUERayTracingAudioRayTracingDevice::SimulateIndirectEnergyField(...)`
- `SimulateIndirectEnergyFieldWithHardwareRayTracing_RenderThread(...)`

流程：

1. 构建或复用声学 BLAS，并构建本次查询的 TLAS。
2. 生成 listener 侧反射射线：
   - 优先 `DispatchGenerateListenerRaysOnGPU_RenderThread(...)`
   - 失败时 CPU 生成相同路径状态。
3. 对每个 bounce：
   - `TraceDetailedRaysWithHardwareRayTracing_RenderThread(...)` 查询反射命中。
   - 从命中点向 source 发 shadow ray。
   - `TraceRaysWithHardwareRayTracing_RenderThread(...)` 查询 source 可见性。
   - `DispatchShadeAndGatherOnGPU_RenderThread(...)` 执行 shade、bounce、gatherEnergyField。
   - 写入 delay bin 三频段能量，并生成下一跳 path buffer。
4. 返回 `FUERayTracingAudioEnergyFieldTraceResult`。
5. Simulator 从 Minimal EnergyField 导出：
   - `IndirectGain`
   - `EarlyReflectionGain`
   - `LateReverbGain`
   - `ReverbTimes`
   - `ReconstructedImpulseResponse`
   - Parametric / Hybrid 参数

## 4. 对外接口

### 4.1 组件接口

`UUERayTracingAudioSourceComponent`：

- `bEnableIndirectSound`
- `IndirectMode`
- `NumReflectionRays`
- `MaxReflectionBounces`
- `IndirectDurationSeconds`
- `MaxEarlyReflectionTaps`
- `HybridTransitionRatio`
- `IndirectMix`（Editor 显示为 `Wet Send`）：`Full = Direct + WetSend × Wet`，默认 `1.0`，范围 `0..4`；大于 `1.0` 是显式 makeup gain，需配合 Full 峰值检查和 gain staging。

`UUERayTracingAudioGeometryComponent`：

- `ExportMode = Bounding Box`
- `ExportMode = Static Mesh Triangles`
- `Absorption`
- `bVisibleForDirectSound`

### 4.2 SDK 接口

`FUERayTracingAudioRayTracingDevice`：

- `IsRayTracingAvailable()`
- `TraceRays(...)`
- `TraceDetailedRays(...)`
- `SimulateIndirectEnergyField(...)`

`SimulateIndirectEnergyField(...)` 的输入为 `FUERayTracingAudioEnergyFieldTraceRequest`，输出为 `FUERayTracingAudioEnergyFieldTraceResult`。

### 4.3 Shader 接口

Compute shader 文件：

- `Shaders/Private/Simulation/UERayTracingAudioEnergyField.usf`

当前 compute shader 负责：

- `GenerateListenerRaysCS`
- `ShadeAndGatherCS`

硬件 ray tracing shader 使用 UE 内建 ray tracing shader 路径完成命中查询和遮挡查询。

## 5. 启动方式

### 5.1 Unreal 项目设置

建议测试环境：

- RHI：DirectX 12
- Shader Model：SM6
- Project Settings 中启用 Hardware Ray Tracing
- 使用支持 DXR 的 GPU 和驱动
- 插件启用 `UERayTracingAudio`

最小场景：

1. 放置一个带 `UUERayTracingAudioListenerComponent` 的 Actor。
2. 放置一个带 `AudioComponent` 和 `UUERayTracingAudioSourceComponent` 的 Actor。
3. 放置一组带 `UUERayTracingAudioGeometryComponent` 的墙体或房间几何。
4. 对真实网格测试，把 Geometry 的 `ExportMode` 设为 `Static Mesh Triangles`。
5. Source 上开启 `bEnableIndirectSound`，先用 `IndirectMode = Hybrid Reverb`。
6. 提高 `NumReflectionRays` 可以减少稀疏采样导致的间接声抖动。

### 5.2 本仓库验证命令

在仓库根目录执行：

```powershell
uv run script\build_and_validate.py
```

构建通过后执行运行时验证：

```powershell
uv run script\launch_runtime_validation.py
```

脚本默认使用：


如果测试工程路径或 Engine 路径不同，需要先调整脚本或保持对应目录存在。

本次独立插件打包验证使用 UE 5.7 的 `RunUAT BuildPlugin`，输出保留在当前仓库的 `BuildValidation/` 下：

```powershell
& "D:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\RunUAT.bat" `
  BuildPlugin `
  -Plugin="D:\Labs\2602-unreal\ue-audio-plugin\UERayTracingAudio.uplugin" `
  -Package="D:\Labs\2602-unreal\ue-audio-plugin\BuildValidation\UERayTracingAudio" `
  -TargetPlatforms=Win64 `
  -Rocket
```

该目录是构建产物目录，已在 `.gitignore` 中忽略。

## 6. 当前限制

- Static Mesh BLAS cache 目前按资源路径、LOD、顶点数、索引数生成 key，还没有接入 UE Renderer 内部的正式 cached ray tracing geometry。
- TLAS 仍然按当前声学场景实例构建，尚未做到跨帧 TLAS 异步复用。
- `ShadeAndGatherCS` 已经在 GPU 上执行单 bounce 的 shading 和 energy gather，但 CPU 仍负责 bounce 调度、结果回读、IR 重建和 Parametric / Hybrid 导出。
- 材质面级别声学参数还没有接入，每个几何实例仍使用统一三频段吸收。
- 这仍是插件自建声学光追场景，不是最终生产级 UE Renderer scene integration。
