# UERayTracingAudio 当前使用说明

## 1. 当前可用范围

当前插件处于 Phase 1、Phase 2 和 Phase 3 的第一版可用阶段，已经具备：

- 插件模块可编译
- Listener / Source / Geometry 组件
- 最小直接声参数计算
- 基于 UE Ray Tracing RHI 的最小体积遮挡后端
- 第一版实时间接声链路
- Occlusion 插件增益处理
- Editor 中的 Bake 窗口骨架入口

当前还不具备：

- 生产级的 UE Ray Tracing RHI 场景接入
- 完整双耳空间化
- 生产级反射、混响、烘焙链路

## 2. 已验证的编译方式

当前已经用以下环境验证过插件编译：

- Unreal Engine: `C:\Projects\ZeroEngine`
- 测试工程: `C:\Projects\MyProject\MyProject.uproject`

使用的编译命令是：

```powershell
& 'C:\Projects\ZeroEngine\Engine\Build\BatchFiles\Build.bat' `
  MyProjectEditor Win64 Development `
  -Project="C:\Projects\MyProject\MyProject.uproject" `
  -plugin="C:\tasks\ue-audio-plugin\UERayTracingAudio.uplugin" `
  -BuildPlugin=UERayTracingAudio `
  -WaitMutex -FromMsBuild
```

这个命令已经验证通过，并实际编译了：

- `UERayTracingAudioSDK`
- `UERayTracingAudio`
- `UERayTracingAudioEditor`

## 3. 如何在测试工程中挂接插件

如果要在 `MyProject` 里实际启用并测试，建议：

1. 把当前仓库同步到：
   - `C:\Projects\MyProject\Plugins\UERayTracingAudio`
2. 在 `MyProject.uproject` 中启用插件 `UERayTracingAudio`
3. 重新生成工程文件或直接重新编译 Editor
4. 启动 Unreal Editor

## 4. 当前阶段在 UE Editor 中的最小使用方法

### 4.1 放置 Listener

在场景中创建一个 Actor，并给它添加：

- `UUERayTracingAudioListenerComponent`

当前这个组件负责：

- 向 Manager 注册当前监听器
- 提供监听器位置

### 4.2 放置 Source

在场景中创建一个带 `AudioComponent` 的 Actor，并给它添加：

- `UUERayTracingAudioSourceComponent`

当前这个组件负责：

- 每帧请求直接声模拟结果
- 每帧请求间接声模拟结果
- 保存当前直接声状态
- 保存当前间接声状态
- 提供体积遮挡相关参数配置

当前可观察的关键量包括：

- `bIsOccluded`
- `DistanceAttenuation`
- `DirectVisibility`
- `OverallGain`
- `IndirectGain`
- `EarlyReflectionGain`
- `LateReverbGain`
- `AverageReflectionDelaySeconds`
- `ReverbTimes`

当前可调的关键直接声参数包括：

- `OccludedGain`
- `SourceRadiusCm`
- `NumOcclusionSamples`
- `bUseVolumetricOcclusion`

当前可调的关键间接声参数包括：

- `bEnableIndirectSound`
- `IndirectMode`
  - `Minimal Convolution`
  - `Parametric Reverb`
  - `Hybrid Reverb`
- `NumReflectionRays`
- `MaxReflectionBounces`
- `IndirectDurationSeconds`
- `MaxEarlyReflectionTaps`
- `HybridTransitionRatio`
- `IndirectMix`

### 4.3 放置遮挡物

在用于遮挡声源和监听器的 Actor 上添加：

- `UUERayTracingAudioGeometryComponent`

当前这个组件会把宿主 Actor 的 `UPrimitiveComponent` 信息导出到声学场景中。

当前几何组件的重要选项包括：

- `ExportMode`
  - `Bounding Box`
  - `Static Mesh Triangles`

建议：

- 快速验证链路时先用 `Bounding Box`
- 想验证更接近真实遮挡时，对 `UStaticMeshComponent` 使用 `Static Mesh Triangles`

建议先用最简单的静态网格墙体测试。

### 4.4 当前间接声模式说明

`UUERayTracingAudioSourceComponent` 的 `IndirectMode` 当前支持三种模式：

- `Minimal Convolution`
  - 输出早期反射 taps
  - 适合先验证路径延迟和早反能量是否存在
- `Parametric Reverb`
  - 输出参数化尾部混响估计
  - 适合先验证尾部混响量级是否变化
- `Hybrid Reverb`
  - 同时启用早期反射和参数化尾部
  - 是当前阶段最接近完整间接声体验的模式

## 5. 当前阶段推荐测试场景

建议先做一个最小验证：

- 一个 Listener
- 一个带音频的 Source
- 一面可以挡在两者之间的墙
- 一个更大的封闭或半封闭房间

验证目标：

- 墙挡住时，`bIsOccluded` 变为 true
- 墙挡住时，`DirectVisibility` 下降到小于 `1.0`
- 挡住时 `OverallGain` 下降
- 挪开墙之后 `OverallGain` 恢复
- 在封闭空间里 `IndirectGain` 和 `LateReverbGain` 高于开阔空间
- 增加 `NumReflectionRays` 后，早期反射与尾部结果更稳定

## 6. 当前阶段的重要限制

### 6.1 当前是最小硬件光追后端，不是最终形态

当前已经接入一版基于 UE Ray Tracing RHI 的遮挡检测实现，并支持并行射线批处理。

当前实现方式是：

- 支持把 `UUERayTracingAudioGeometryComponent` 导出的包围盒或真实静态网格三角形作为光追输入
- 用这些几何数据构建独立的 BLAS / TLAS
- 对声源球体采样得到一批遮挡射线
- 通过 RHI 光追后端并行求交
- 在硬件光追不可用时回退到 `LineTraceSingleByChannel`

这说明它已经不是纯占位实现，但还不是最终生产级版本。

对于间接声，当前后端策略是：

- 硬件光追可用时：
  - 用 UE Ray Tracing RHI shader 返回每条反射射线的命中距离、法线和几何索引
  - CPU 侧继续负责多 bounce 调度、路径贡献累计和早反 / 混响参数生成
- 硬件光追不可用时：
  - 回退到 CPU 声学场景求交

### 6.2 当前不是完整空间音频

目前真正生效的是：

- 直接路径相关增益
- 第一版间接声早期反射 / 参数化尾部混响
- Phase 3 的 RHI 优先命中查询 + CPU 路径累积

目前还没有：

- HRTF
- 双耳空间化
- 生产级早期反射
- 生产级混响
- 完整 GPU EnergyField / IR 重建链路

### 6.3 当前 Editor 窗口只是骨架

菜单中已经有 Bake 窗口入口，但现在它只是占位，不代表烘焙功能已经可用。

## 7. 下一步推荐使用顺序

如果你想继续验证当前插件，建议顺序是：

1. 先确认插件在测试工程中可启用
2. 再确认场景内组件能正常挂载
3. 对遮挡物优先试 `ExportMode = Static Mesh Triangles`
4. Source 先试 `IndirectMode = Hybrid Reverb`
5. 先观察 `bIsOccluded / DirectVisibility / OverallGain / IndirectGain / LateReverbGain`
6. 再做遮挡试听和房间感验证
