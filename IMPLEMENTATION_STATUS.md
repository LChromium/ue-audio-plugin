# 当前实现状态总结

## 1. 已完成范围

当前仓库已经完成了 `ARCHITECTURE.md` 中 Phase 1、Phase 2，以及 Phase 3.1 / 3.2 / 3.3 的当前代码实现，并且已经完成基于 Unreal Build Tool 的实际编译验证，但还没有达到“完整声学功能闭环已在 Unreal Editor 中试听跑通”的状态。

目前完成的内容可以概括为：

- 插件结构已经建立起来。
- Runtime / Editor / SDK 三层模块已经拆开。
- 直接声的最小数据链路已经接通。
- 间接声实时链路已经接入 Minimal EnergyField、IR 重建和 Parametric / Hybrid 第一版实现。
- 编辑器里已经有 Bake 窗口入口骨架。
- 代码层面的静态检查通过。
- 插件模块已经在测试工程环境中完成实际编译。

## 2. 已落地的模块

### 2.1 插件描述

已经创建插件描述文件：

- `UERayTracingAudio.uplugin`

当前插件声明了三个模块：

- `UERayTracingAudioSDK`
- `UERayTracingAudio`
- `UERayTracingAudioEditor`

这说明项目已经从“纯设计文档阶段”进入了“可作为 Unreal 插件组织代码”的阶段。

### 2.2 SDK 模块

已经在 `Source/UERayTracingAudioSDK/` 下建立核心基础层，包含以下对象：

- `FUERayTracingAudioContext`
  - 保存参考距离、最大距离、声速等基础参数。
- `FUERayTracingAudioScene`
  - 保存静态几何导出结果。
  - 支持场景版本号递增。
- `FUERayTracingAudioRayTracingDevice`
  - 暴露可见性查询入口。
  - 当前已经支持批量射线查询。
  - 当前已经接入一版基于 UE Ray Tracing RHI 的硬件光追后端。
  - 当前已经支持返回命中距离、法线和几何索引的间接声详细查询。
  - 当硬件光追不可用时仍会回退到 `LineTraceSingleByChannel`。
- `FUERayTracingAudioSimulator`
  - 负责直接声和间接声最小模拟。
  - 已实现距离衰减、空气吸收、遮挡增益计算。
  - 已实现参考 Steam Audio 的体积遮挡采样逻辑。
  - 已实现基于导出声学场景的实时间接声路径采样、早期反射、参数化混响和 Hybrid 结果生成。
  - 当前在硬件光追可用时会优先使用 UE Ray Tracing RHI 做多跳命中查询，不可用时回退到 CPU 声学场景求交。
  - 当前已实现 Minimal EnergyField：delay bins、3-band energy accumulation、earliest/latest split 和 temporal smoothing。
  - 当前已实现从 Minimal EnergyField 重建第一版 IR，并导出 Parametric / Hybrid 参数。
- `FUERayTracingAudioReflectionSimulator`
  - 已按 Steam Audio `generate rays -> QueryIntersection -> QueryOcclusion -> shadeAndBounce -> gatherEnergyField` 的思路拆出第一阶段流程。
  - 当前已经改为从 listener 侧生成反射射线，再向 source 做可见性 / occlusion 检查。
  - 当前第二阶段已经把 `shadeAndBounce + gatherEnergyField` 迁到 UE compute shader 路径。
  - 当前是 RHI / compute / CPU 混合路径：RHI 返回 hit，compute 负责单 bounce 的 shadeAndBounce 与 gatherEnergyField，CPU 继续负责 bounce 调度与后续导出。
- `FUERayTracingAudioRayTracingDevice`
  - 当前新增了设备级 `SimulateIndirectEnergyField(...)` 硬件路径。
  - 当前会把硬件反射模拟收拢到单次 render command 中执行，避免每个 bounce 都往 game thread 返回 `TArray`。
  - 当前在设备级硬件路径中会复用单次构建的声学 TLAS / BLAS，而不是每个 bounce 重建一次场景。
  - 当前设备级硬件路径已经进一步按 Radeon Rays 循环拆成：`generateListenerRays -> QueryIntersection -> shadeAndBounce -> QueryOcclusion -> gatherEnergyField -> swap buffers`。
  - 当前 `generateListenerRays` 和 `shadeAndBounce + gatherEnergyField` 已经有对应的 UE compute shader 实现。
- `FUERayTracingAudioSerializedObject`
  - 提供后续烘焙 / Probe / IR 数据序列化的占位类型。

这部分的意义是：Runtime 层已经不再直接写死所有声学逻辑，而是开始通过 SDK 层做抽象。

### 2.3 Runtime 模块

已经在 `Source/UERayTracingAudio/` 下建立运行时接入层，包含以下对象：

- `FUERayTracingAudioModule`
  - 模块启动和关闭。
  - 注册 Occlusion / Spatialization 插件工厂。
- `FUERayTracingAudioManager`
  - 管理 Source / Listener / Geometry 注册。
  - 负责重建声学场景。
  - 负责调度直接声和间接声模拟。
- `UUERayTracingAudioSourceComponent`
  - 每帧向 Manager 请求直接声和间接声结果。
  - 保存 `bIsOccluded`、`DistanceAttenuation`、`OverallGain` 等直接声结果。
  - 保存 `IndirectGain`、`EarlyReflectionGain`、`LateReverbGain`、`ReverbTimes` 等间接声结果。
- `UUERayTracingAudioListenerComponent`
  - 提供当前监听器位置。
- `UUERayTracingAudioGeometryComponent`
  - 把 Actor 上的几何信息导出为声学场景中的静态几何。
  - 已支持在组件上选择导出包围盒或真实静态网格三角形。

这部分已经形成一个完整的 Unreal 运行时接入骨架。

### 2.4 音频插件接入

已经建立最小音频插件接入能力：

- `FUERayTracingAudioOcclusionPlugin`
  - 从 `UUERayTracingAudioSourceComponent` 读取模拟结果。
  - 把距离衰减、空气吸收、遮挡组合成目标增益。
  - 对输出 buffer 做平滑增益处理。
  - 已接入第一版 IR 重建播放和参数化尾部混响渲染。
- `FUERayTracingAudioSpatializationPlugin`
  - 目前是占位实现。
  - 已接入 Unreal 插件工厂注册。
  - 处理逻辑当前仍然是直通，不包含 HRTF / 双耳处理。
- `UUERayTracingAudioOcclusionSettings`
  - 提供直接声相关的开关设置。
- `UUERayTracingAudioSpatializationSettings`
  - 提供空间化占位设置。

也就是说，当前已经接进了 Unreal 的音频插件机制，并且已经在 Occlusion 插件路径上接入第一版间接声播放链路。

### 2.5 Editor 模块

已经在 `Source/UERayTracingAudioEditor/` 下建立编辑器模块骨架：

- 注册了一个菜单入口
- 注册了一个 Nomad Tab
- 可以打开一个 Bake 窗口骨架

当前这个窗口还不是烘焙系统，只是为后续 Phase 3 / 4 预留入口。

## 3. 已经实现的直接声能力

从代码角度，当前直接声已经实现了以下最小能力：

### 3.1 Listener / Source 注册

- Listener 组件在 `BeginPlay` 时注册到 Manager。
- Source 组件在 `BeginPlay` 时注册到 Manager。
- Source 每帧请求一次直接声模拟结果。

### 3.2 静态场景同步

- Geometry 组件会把宿主 Actor 上的 `UPrimitiveComponent` 边界信息导出到 SDK 场景。
- 对于 `UStaticMeshComponent`，已经支持导出真实静态网格三角形。
- Manager 会在脏标记后重建静态场景缓存。

注意：

- 当前已经支持两种导出模式：
  - 包围盒导出
  - 真实静态网格三角形导出
- 当选择真实静态网格模式且 CPU 可访问渲染数据时，会导出 LOD0 的世界空间三角形数据。

### 3.3 可见性 / 遮挡

- 当前 `RayTracingDevice` 暴露的是“直接路径查询”接口。
- 当前已经支持批量射线检测。
- 当硬件光追可用时，会基于 UE Ray Tracing RHI 对导出的声学场景进行并行遮挡检测。
- 当硬件光追不可用时，会回退到 `ECC_Visibility` 线性检测。
- 如果 Source 与 Listener 之间有阻挡，则会计算 `0-1` 的可见度结果，并进一步映射为最终遮挡增益。

注意：

- 当前体积遮挡逻辑参考了 Steam Audio 的 `Volumetric` 模式：
  - 把声源视为一个球体。
  - 在球体体积内采样多个点。
  - 先检测 sample 是否对声源本身可见。
  - 再检测 sample 是否对监听器可见。
  - 最终用“同时对声源和监听器可见的样本比例”作为直接声可见度。
- 当前 RHI 后端已经兼容两种几何输入：
  - 包围盒近似
  - 真实静态网格三角形
- 其中真实静态网格模式已经明显更接近最终目标，但仍未包含材质面级别声学信息。

### 3.4 距离衰减

当前实现里已经有一版基于参考距离的衰减公式：

- 以 `ReferenceDistanceCm` 为基准。
- 使用平方反比衰减。
- 结果钳制到 `[0, 1]`。

### 3.5 空气吸收

已经按每米衰减系数分别计算高、中、低频的指数衰减，并最终取平均值并入总增益。

### 3.6 最终直接声增益输出

最终增益目前是：

- `DistanceAttenuation`
- `AirAbsorption`
- `Occlusion`

三者相乘后得到 `OverallGain`，再由 Occlusion 插件对输出 buffer 做平滑应用。

### 3.7 间接声实时链路

当前已经实现一版参考 Steam Audio 思路的间接声实时链路：

- 从 listener 侧生成多条反射采样射线。
- 对 bounce rays 做 `QueryIntersection`。
- 对 hit 后的 source 可见性做 `QueryOcclusion`。
- 在 UE compute shader 路径执行单 bounce 的 `shadeAndBounce`。
- 在 UE compute shader 路径执行 `gatherEnergyField`，把有效路径写入 Minimal EnergyField 的 delay bins。
- 对 3 个频段做能量累计。
- 对 EnergyField 做 earliest/latest split 和 temporal smoothing。
- 从 Minimal EnergyField 重建第一版 impulse response。
- 从 Minimal EnergyField 导出 Parametric delay / EQ / RT60。
- 根据间接声模式输出：
  - `MinimalConvolution`
  - `ParametricReverb`
  - `HybridReverb`

当前查询后端策略是：

- 当硬件光追可用时：
  - 使用 UE Ray Tracing RHI shader 返回命中距离、法线和几何索引
  - UE compute shader 负责单 bounce 的 `shadeAndBounce` 与 `gatherEnergyField`
  - 设备级硬件路径会在单次 render command 中推进多 bounce，减少每 bounce 的 game-thread readback
  - CPU 侧负责 temporal smoothing、IR / Parametric / Hybrid 导出，以及 CPU fallback
- 当硬件光追不可用时：
  - 回退到 CPU 场景求交实现

当前模式含义如下：

- `MinimalConvolution`
  - 输出早期反射 delay / gain taps
  - 由音频插件用延迟线方式渲染
- `ParametricReverb`
  - 从路径能量估计 `ReverbTimes`
  - 由音频插件使用参数化尾部混响近似渲染
- `HybridReverb`
  - 同时保留早期反射 taps 和参数化尾部混响

当前 `UUERayTracingAudioSourceComponent` 已暴露的关键参数包括：

- `bEnableIndirectSound`
- `IndirectMode`
- `NumReflectionRays`
- `MaxReflectionBounces`
- `IndirectDurationSeconds`
- `MaxEarlyReflectionTaps`
- `HybridTransitionRatio`
- `IndirectMix`

当前可观察的关键结果包括：

- `bHasIndirectPath`
- `NumValidReflectionPaths`
- `IndirectGain`
- `EarlyReflectionGain`
- `LateReverbGain`
- `AverageReflectionDelaySeconds`
- `ReverbTimes`

当前 Minimal EnergyField 已负责导出：

- `IndirectGain`
- `EarlyReflectionGain`
- `LateReverbGain`
- `ReverbTimes`

当前 3.2 / 3.3 额外负责：

- 从 delay bins 重建第一版 `ReconstructedImpulseResponse`
- 导出 `ParametricDelaySeconds`
- 导出 `ParametricEq`
- 让 `HybridReverb` 同时使用 IR 早期部分和参数化尾部

## 4. 还没有完成的部分

下面这些是“当前还没有做完，所以不能说已经完整跑通直接声”的关键原因。

### 4.1 UE 硬件光追 RHI 已接入最小版本

当前已经完成：

- 基于 UE Ray Tracing RHI 的批量遮挡检测
- 基于导出声学几何构建最小 BLAS / TLAS
- 遮挡查询结果回读到 CPU 并参与直接声计算
- Phase 3 间接声已经改为“RHI 命中查询优先、CPU fallback 兜底”的后端模式

当前还不是最终目标里的完整版本，原因是：

- 还没有直接复用 Unreal 渲染场景中的正式光追场景数据
- 还没有做异步调度和缓存优化
- 还没有把更多材质和命中信息接入后续传播模型

### 4.2 真实几何导出已部分完成

当前静态场景里保存的是：

- 变换
- Bounds
- Extent
- 吸收参数
- 可选的静态网格顶点和三角形索引

还没有：

- 材质面级别声学参数
- 高精度 BLAS / TLAS 构建输入
- 非静态网格组件的真实三角形导出

### 4.3 Spatialization 还是占位

当前 Spatialization 插件只是注册成功并直通音频数据，没有实现：

- 双耳渲染
- HRTF
- 声像计算
- 与直接声结果联动的位置感知处理

所以现在“直接声”更准确地说是：

- 已经能算出直接路径相关的增益参数
- 但还不是完整空间音频意义上的直接声播放效果

而“间接声”更准确地说是：

- 已经有第一版实时路径采样、早期反射和参数化尾部混响
- 但还不是 Steam Audio 那种完整 `EnergyField -> IR / Hybrid` 级别的生产实现

### 4.4 Unreal Editor 内功能验证已部分完成

当前完成的是：

- 结构正确性检查
- 代码诊断检查
- 模块与入口一致性检查
- 使用 `C:\Projects\ZeroEngine` 对 `C:\Projects\MyProject\MyProject.uproject` 执行了实际编译
- 使用 `-plugin="C:\tasks\ue-audio-plugin\UERayTracingAudio.uplugin"` 和 `-BuildPlugin=UERayTracingAudio` 验证了插件三个模块都能成功编译
- 已修复 `UERayTracingAudioSDK` 因 shader 加载时机过晚导致的模块加载失败问题
- 已修复 RHI 遮挡后端在 ray tracing pipeline 缺少 hit shader 时触发的启动 / 运行时崩溃问题
- 已修复间接声 RHI RTPSO 因 ShaderBindingLayout 不匹配导致的编辑器运行时崩溃问题
- 已验证测试工程可以正常启动，插件模块能够被项目成功加载
- 已验证项目可在 `-game` 模式下成功启动并退出，没有再触发同类崩溃
- 在 Unreal Editor 中验证了 `UUERayTracingAudioSourceComponent` 的运行时调试量可以正确显示
- 已确认 `bIsOccluded`、`DistanceAttenuation`、`OverallGain` 这三个关键直接声结果能够在编辑器中正确观察到

当前还没有完成的是：

- 绑定到 Audio Component 并实际试听
- 在更完整场景里确认参数变化与听感是否一致

### 4.5 没有完成完整的设置链路验证

虽然已经有：

- Occlusion Settings
- Spatialization Settings
- Source / Listener / Geometry Components

但还没有做完：

- 在具体 UE 工程中启用并选择该音频插件
- 组件和资产在编辑器中的完整工作流验证
- 不同场景布局下的试听校验

### 4.6 Phase 3 还不是生产级间接声后端

当前 Phase 3 虽然已经实现，但仍有明显限制：

- 当前虽然已经把设备级硬件路径对齐到 Radeon Rays 的 for 循环步骤，但 render-thread 内部仍有每 bounce 的 trace 结果回读，尚未做到完整 GPU buffer 持续链路
- 当前没有完整的 Steam Audio `EnergyField` 数据结构
- 当前还没有完整方向场 / 高阶球谐版本的 EnergyField
- 当前参数化混响仍是简化模型，不是完整 Steam Audio 级 RT60 / EQ / delay 拟合实现
- 当前 IR 仍是从 Minimal EnergyField 重建的第一版，不是完整长 IR 卷积器

## 5. 现在能不能跑通直接声和间接声

结论分两层说。

### 5.1 从“代码链路是否已经接通”来说

可以说：

- **最小直接声代码链路已经接通了。**

因为现在已经有：

- Listener / Source / Geometry 组件
- 场景收集
- 单条可见性检测
- 距离衰减
- 空气吸收
- 遮挡增益
- Occlusion 插件把结果作用到音频 buffer

### 5.2 从“项目目标意义上的直接声和间接声是否已跑通”来说

现在还不能说：

- **还不能算真正跑通。**

原因是：

- 虽然已经完成实际编译，并且已经确认关键直接声参数可在 Unreal Editor 中正确显示，但还没有完成试听验证。
- 当前虽然已经接入最小 UE Ray Tracing RHI 后端，并且已经支持真实静态网格三角形导出，但材质和更复杂几何类型还没有接入，不是最终生产级实现。
- 虽然已经实现 Phase 3.1 / 3.2 / 3.3 第一版，并且查询优先切到 UE Ray Tracing RHI，但仍不是完整 Steam Audio 等级的方向场 / IR 重建系统。
- Spatialization 仍然是占位实现。

所以更准确的判断是：

- **当前处于“直接声最小实现已完成，已完成参数显示验证，并已接入最小 RHI 遮挡后端，但还未完成试听验证和生产级几何/空间化实现”的状态。**

## 6. 如果你现在就想在 UE 里试

理论上你下一步应该做的是：

1. 在 Unreal 工程中加载这个插件。
2. 确认三个模块能成功编译并被引擎识别。
3. 在场景中放置：
   - 一个带 `UUERayTracingAudioListenerComponent` 的 Actor
   - 一个带 `UUERayTracingAudioSourceComponent` 的 Actor
   - 若干带 `UUERayTracingAudioGeometryComponent` 的遮挡物
4. 在项目音频设置里把 Occlusion / Spatialization 插件切到这个插件。
5. 播放音源并观察：
   - `bIsOccluded`
   - `DistanceAttenuation`
   - `OverallGain`
6. 确认遮挡物插入/移除时听感和参数会变化。

但在做完这些之前，当前状态仍然只能叫：

- “代码层面基本接通”
- 不能叫“功能已经实机跑通”

## 7. 最推荐的下一步

如果目标是尽快把“直接声真的跑起来”，下一步最应该做的不是继续补文档，而是做下面三件事：

### 7.1 先完成可编译验证

优先确认：

- 插件能否被 Unreal 正常编译
- 头文件 / 模块依赖 / 生成代码是否都正确

这一步现在已经完成。

### 7.2 做一个最小 UE 场景验证

建立最小验证场景：

- Listener
- 单个 Source
- 单个遮挡墙体

只验证一件事：

- 墙体挡住时音量明显下降
- 移开墙体时音量恢复

### 7.3 把查询后端替换成真正的 UE Ray Tracing RHI

这一步的最小版本现在已经完成，后续重点变成：

- 用真实三角网格替换当前包围盒近似
- 让 RHI 查询与场景更新做缓存和异步化
- 把命中材质和传播信息继续接入后续声学计算
- 让 Phase 3 从“RHI 命中查询 + CPU 路径累积”进一步走向“更完整的 GPU energy field / IR 重建链路”

## 8. 一句话结论

当前已经完成了：

- **Phase 1 全部骨架**
- **Phase 2 的最小直接声代码链路**
- **Phase 3 的第一版实时间接声链路**
- **Phase 3.1 的 Minimal EnergyField**
- **Phase 3.2 的 IR 重建**
- **Phase 3.3 的 Parametric / Hybrid 第一版**
- **测试工程中的实际编译验证**
- **Unreal Editor 中关键直接声参数显示验证**
- **基于 RHI 的最小体积遮挡后端**
- **GeometryComponent 的包围盒 / 真实静态网格双导出模式**

当前还没有完成：

- **Unreal Editor 内试听与场景验证**
- **生产级 UE Ray Tracing RHI 几何与场景接入**
- **完整空间化直接声实现**
- **生产级 Steam Audio 风格间接声系统**

所以现在最准确的说法是：

- **你已经把直接声、最小 RHI 遮挡后端和第一版间接声链路都写出来了，但还不能说已经把最终形态的完整声学系统完全跑通了。**
