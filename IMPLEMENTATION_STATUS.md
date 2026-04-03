# 当前实现状态总结

## 1. 已完成范围

当前仓库已经完成了 `ARCHITECTURE.md` 中 Phase 1 和 Phase 2 的第一版代码骨架与最小链路实现，并且已经完成一次基于 Unreal Build Tool 的实际编译验证，但还没有达到“功能闭环已在 Unreal Editor 中试听跑通”的状态。

目前完成的内容可以概括为：

- 插件结构已经建立起来。
- Runtime / Editor / SDK 三层模块已经拆开。
- 直接声的最小数据链路已经接通。
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
  - 当硬件光追不可用时仍会回退到 `LineTraceSingleByChannel`。
- `FUERayTracingAudioSimulator`
  - 负责直接声最小模拟。
  - 已实现距离衰减、空气吸收、遮挡增益计算。
  - 已实现参考 Steam Audio 的体积遮挡采样逻辑。
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
  - 负责调度直接声模拟。
- `UUERayTracingAudioSourceComponent`
  - 每帧向 Manager 请求直接声结果。
  - 保存 `bIsOccluded`、`DistanceAttenuation`、`OverallGain` 等运行时结果。
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
- `FUERayTracingAudioSpatializationPlugin`
  - 目前是占位实现。
  - 已接入 Unreal 插件工厂注册。
  - 处理逻辑当前仍然是直通，不包含 HRTF / 双耳处理。
- `UUERayTracingAudioOcclusionSettings`
  - 提供直接声相关的开关设置。
- `UUERayTracingAudioSpatializationSettings`
  - 提供空间化占位设置。

也就是说，当前已经接进了 Unreal 的音频插件机制，但只在 Occlusion 方向有最小可工作的处理逻辑。

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

## 4. 还没有完成的部分

下面这些是“当前还没有做完，所以不能说已经完整跑通直接声”的关键原因。

### 4.1 UE 硬件光追 RHI 已接入最小版本

当前已经完成：

- 基于 UE Ray Tracing RHI 的批量遮挡检测
- 基于导出声学几何构建最小 BLAS / TLAS
- 遮挡查询结果回读到 CPU 并参与直接声计算

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

### 4.4 Unreal Editor 内功能验证已部分完成

当前完成的是：

- 结构正确性检查
- 代码诊断检查
- 模块与入口一致性检查
- 使用 `C:\Projects\ZeroEngine` 对 `C:\Projects\MyProject\MyProject.uproject` 执行了实际编译
- 使用 `-plugin="C:\tasks\ue-audio-plugin\UERayTracingAudio.uplugin"` 和 `-BuildPlugin=UERayTracingAudio` 验证了插件三个模块都能成功编译
- 已修复 `UERayTracingAudioSDK` 因 shader 加载时机过晚导致的模块加载失败问题
- 已修复 RHI 遮挡后端在 ray tracing pipeline 缺少 hit shader 时触发的启动 / 运行时崩溃问题
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

## 5. 现在能不能跑通直接声

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

### 5.2 从“项目目标意义上的直接声是否已跑通”来说

现在还不能说：

- **还不能算真正跑通。**

原因是：

- 虽然已经完成实际编译，并且已经确认关键直接声参数可在 Unreal Editor 中正确显示，但还没有完成试听验证。
- 当前虽然已经接入最小 UE Ray Tracing RHI 后端，并且已经支持真实静态网格三角形导出，但材质和更复杂几何类型还没有接入，不是最终生产级实现。
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

## 8. 一句话结论

当前已经完成了：

- **Phase 1 全部骨架**
- **Phase 2 的最小直接声代码链路**
- **测试工程中的实际编译验证**
- **Unreal Editor 中关键直接声参数显示验证**
- **基于 RHI 的最小体积遮挡后端**
- **GeometryComponent 的包围盒 / 真实静态网格双导出模式**

当前还没有完成：

- **Unreal Editor 内试听与场景验证**
- **生产级 UE Ray Tracing RHI 几何与场景接入**
- **完整空间化直接声实现**

所以现在最准确的说法是：

- **你已经把直接声和最小 RHI 遮挡后端都写出来了，但还不能说已经把最终形态的直接声完全跑通了。**
