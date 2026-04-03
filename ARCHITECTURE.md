# UE 音频插件整体架构设计

## 1. 项目目标

本项目的目标是实现一个 Unreal Engine 插件，用 UE 自身的硬件光线追踪能力替代 Steam Audio 的外部声学后端，完成以下能力：

- 直接声模拟：遮挡、透射、距离衰减、空气吸收、指向性。
- 间接声模拟：早期反射、混响、路径传播。
- 烘焙能力：基于场景几何和材质预计算脉冲响应、探针数据、反射数据，并在运行时复用。
- UE 深度集成：遵循 Steam Audio UE 插件的模块划分和使用方式，但把 `SteamAudioSDK` 替换为项目内建的、基于 UE Ray Tracing RHI 的实现。
- 核心算法分层：参考 Steam Audio `core` 的组织方式，把几何、场景、射线追踪、模拟、卷积、烘焙、序列化拆成独立子系统，而不是把逻辑直接写在 UE Runtime 模块里。

## 2. 对标关系

### 2.1 参考对象

- UE 插件结构参考：`C:\tasks\steam-audio\unreal\src\SteamAudioUnreal\Plugins\SteamAudio`
- 声学核心结构参考：`C:\tasks\steam-audio\core\src\core`

### 2.2 本项目对应关系

建议保持与 Steam Audio 基本一致的三层结构：

| Steam Audio | 本项目建议模块 | 作用 |
| --- | --- | --- |
| SteamAudio | `UERayTracingAudio` | UE 运行时接入层，负责组件、音频插件、场景同步、运行时驱动 |
| SteamAudioEditor | `UERayTracingAudioEditor` | 编辑器工具层，负责烘焙窗口、资产工厂、可视化和调试 |
| SteamAudioSDK | `UERayTracingAudioSDK` | 核心声学与模拟层，负责场景、射线追踪、直接声/间接声/烘焙/卷积 |

其中：

- `UERayTracingAudio` 和 `UERayTracingAudioEditor` 对外保持 Unreal 插件风格。
- `UERayTracingAudioSDK` 不再是外部二进制封装，而是项目源码中的核心模块。
- 如果后续需要更细粒度拆分，可以在 `UERayTracingAudioSDK` 内部按目录做“逻辑模块化”，先不急于拆成过多 UE Module，避免编译链和依赖过度复杂。

## 3. 推荐仓库结构

```text
ue-audio-plugin/
├─ UERayTracingAudio.uplugin
├─ Config/
├─ Content/
├─ Resources/
└─ Source/
   ├─ UERayTracingAudio/
   │  ├─ Public/
   │  ├─ Private/
   │  └─ UERayTracingAudio.Build.cs
   ├─ UERayTracingAudioEditor/
   │  ├─ Public/
   │  ├─ Private/
   │  └─ UERayTracingAudioEditor.Build.cs
   └─ UERayTracingAudioSDK/
      ├─ Public/
      │  ├─ API/
      │  ├─ Audio/
      │  ├─ Baking/
      │  ├─ Geometry/
      │  ├─ RayTracing/
      │  ├─ Scene/
      │  ├─ Simulation/
      │  └─ Serialization/
      ├─ Private/
      │  ├─ API/
      │  ├─ Audio/
      │  ├─ Baking/
      │  ├─ Core/
      │  ├─ Geometry/
      │  ├─ RayTracing/
      │  ├─ Scene/
      │  ├─ Simulation/
      │  └─ Serialization/
      └─ UERayTracingAudioSDK.Build.cs
```

## 4. 模块职责划分

### 4.1 `UERayTracingAudio`

这是插件的运行时接入层，对标 Steam Audio 的 `SteamAudio` 模块。

建议包含以下职责：

- 插件启动与关闭：
  - Module 生命周期管理
  - 初始化全局管理器
  - 注册 Spatialization、Occlusion、Reverb 插件接口
- UE 组件层：
  - `UAudioListenerComponent`
  - `UAudioSourceComponent`
  - `UAudioGeometryComponent`
  - `UAudioDynamicObjectComponent`
  - `UAudioProbeVolumeComponent`
  - `UBakedSourceComponent`
  - `UBakedListenerComponent`
- UE 资源层：
  - 材质资产
  - Spatialization 设置资产
  - Occlusion 设置资产
  - Reverb 设置资产
  - Probe/烘焙结果序列化资产
- 运行时管理层：
  - 全局 `Manager`
  - `Scene` 同步
  - 动态对象注册/卸载
  - 监听器和声源注册
  - 每帧参数收集
  - 异步模拟任务调度
- 音频引擎适配层：
  - 直接声参数转音频引擎参数
  - 反射/混响结果转卷积输入
  - 与 Audio Mixer / Audio Extensions 对接

这个模块应该尽量薄，核心原则是：

- 不承担复杂的声学算法实现。
- 只负责把 Unreal 世界、组件、资产、音频回调映射到 SDK 层。

### 4.2 `UERayTracingAudioEditor`

这是编辑器能力层，对标 `SteamAudioEditor` 模块。

建议包含以下职责：

- 烘焙工具：
  - Bake 面板
  - Bake 任务管理
  - 批量烘焙入口
  - 进度显示与取消
- 编辑器资产工厂：
  - 声学材质资产工厂
  - Spatialization/Occlusion/Reverb 设置资产工厂
  - Probe 数据资产工厂
- 可视化与细节面板：
  - 组件 Gizmo / Visualizer
  - Probe Volume 可视化
  - Listener / Source / Baked 数据调试显示
  - Details 自定义面板
- 编辑器导出能力：
  - 静态几何导出
  - 动态对象导出
  - 材质刷新
  - 烘焙输入校验
- 调试支持：
  - 场景统计
  - 射线计数
  - 烘焙结果预览
  - 错误提示与诊断

### 4.3 `UERayTracingAudioSDK`

这是项目最核心的部分，对标 Steam Audio `core` 和原 `SteamAudioSDK` 的组合职责。

建议分成下面几个内部子系统。

## 5. SDK 内部分层

### 5.1 API 层

作用是提供一组稳定的、对 UE Runtime 友好的接口，避免 Runtime 直接依赖底层细节。

建议包含：

- `Context`
  - 全局配置
  - 内存分配器
  - 日志接口
  - 线程池/任务系统句柄
- `SceneHandle`
  - 静态场景
  - 动态实例
  - 材质表
- `Simulator`
  - 直接声模拟
  - 间接声模拟
  - 烘焙任务
- `Effect` / `AudioProcessor`
  - 直接声效果
  - 卷积效果
  - 混响效果
- `SerializedObject`
  - IR、Probe、反射数据、场景缓存

这一层建议提供纯 C++ 接口，不依赖 UCLASS/UObject。

### 5.2 Core 基础层

作用是承载所有底层共用能力，对标 Steam Audio `core` 中的 `context`、`memory_allocator`、`job_graph`、`profiler`、`log` 等基础设施。

建议包含：

- 日志系统
- 内存与对象生命周期管理
- 任务系统
- 统计与性能计数器
- 基础数学类型
- SIMD/平台抽象
- 配置与错误码

### 5.3 Geometry 几何层

作用是统一表达声学模拟所需的几何数据，对标 Steam Audio 的 `mesh`、`static_mesh`、`instanced_mesh`、`material`、`triangle` 等。

建议对象：

- `AcousticMaterial`
  - 吸收系数
  - 散射系数
  - 透射参数
- `TriangleMesh`
- `StaticMesh`
- `InstancedMesh`
- `DynamicObject`
- `BoundingVolume`
- `ProbeVolume`

几何层输出的不是 Unreal 原始数据，而是声学模拟专用格式，便于：

- 构建 BLAS/TLAS
- 做材质索引压缩
- 做烘焙序列化
- 做 CPU/GPU 双端复用

### 5.4 Scene 场景层

作用是管理“可模拟场景”的统一表示，对标 Steam Audio 的 `scene`、`scene_factory`、`custom_scene` 等。

建议职责：

- 静态场景构建
- 动态对象实例化
- 几何脏区更新
- 材质映射更新
- 场景版本号管理
- 射线查询前的只读快照生成

这层不直接做具体的光追 API 调用，而是为光追后端准备可查询数据。

### 5.5 RayTracing 后端层

这是与本项目目标最相关的核心替换层，用 UE Ray Tracing RHI 替代 Embree / Radeon Rays / OpenCL 设备抽象。

建议职责：

- 封装 UE Ray Tracing RHI 能力
- 构建和更新 BLAS / TLAS
- 提供统一的射线查询接口
- 支持单射线、批量射线、反射路径采样
- 输出命中信息：
  - 命中距离
  - 法线
  - 材质索引
  - Instance ID
  - Primitive ID
- 屏蔽 RHI 线程与渲染线程细节

建议内部组件：

- `RayTracingDevice`
- `RayTracingScene`
- `RayTracingQuery`
- `RayTracingBatch`
- `RayTracingHit`
- `RayTracingSceneBuilder`

这里要特别注意：

- UE 的硬件光追访问往往需要与渲染线程同步，不能把 Runtime 音频线程直接绑死到渲染线程。
- 最好设计成“场景构建在渲染侧、查询任务异步提交、结果回传到模拟线程”的模式。

### 5.6 Simulation 模拟层

这是声学行为实现层，对标 Steam Audio `direct_simulator`、`reflection_simulator`、`path_simulator`、`simulation_manager`、`reverb_estimator` 等。

建议拆成以下子模块：

- `DirectSimulation`
  - 可见性
  - 遮挡
  - 透射
  - 空气吸收
  - 距离衰减
  - 指向性
- `ReflectionSimulation`
  - 早期反射
  - 多次反弹采样
  - 能量累积
  - 反射路径过滤
- `ReverbSimulation`
  - 晚期混响参数估计
  - 带宽衰减
  - 时间常数拟合
- `PathSimulation`
  - 绕射/路径搜索预留
  - 复杂传播路径抽象
- `SimulationManager`
  - Source × Listener 任务编排
  - 场景快照绑定
  - 帧间缓存
  - 降采样与预算控制

设计原则：

- 模拟层只依赖场景层和 RayTracing 抽象层。
- 不能反向依赖 UE 组件类型。
- 所有输出都应归一到统一结果结构，便于 Runtime 和 Baking 复用。

### 5.7 Audio 音频效果层

作用是把模拟结果转换成可播放的音频处理结果，对标 Steam Audio 的 `direct_effect`、`indirect_effect`、`impulse_response`、`convolution_effect`、`hrtf` 等。

建议包含：

- HRTF 数据接口
- 直接声滤波器
- 反射 IR 生成
- 卷积器
- 参数化混响
- Ambisonics 预留接口

如果第一阶段目标聚焦于 UE 插件可用性，建议优先级如下：

1. 直接声参数输出
2. 单 IR 或分段 IR 生成
3. 基于卷积的反射/混响播放
4. HRTF 与 Ambisonics 扩展

### 5.8 Baking 烘焙层

作用是把离线高质量模拟结果保存为运行时可复用数据，对标 Steam Audio 的 `reflection_baker`、`probe_generator`、`baked_reflection_simulator` 等。

建议职责：

- Probe 生成
- Probe 数据布局
- Listener 点位烘焙
- Source 点位烘焙
- Baked IR 生成
- Baked 反射能量场生成
- 烘焙结果压缩与分块

建议输出的数据对象：

- `BakedImpulseResponse`
- `BakedReflectionSet`
- `ProbeBatch`
- `ProbeTree`
- `EnergyField`
- `PathVisibilityData`

### 5.9 Serialization 序列化层

作用是连接 UE 资产系统和 SDK 内部数据，对标 Steam Audio 的 `serialized_object`、`probe_batch`、`energy_field` 等 FlatBuffer 风格对象。

建议职责：

- 场景缓存序列化
- IR 序列化
- Probe 数据序列化
- 材质表序列化
- 版本兼容
- 资源分块加载

建议原则：

- 运行时读取尽量零拷贝或少拷贝。
- Editor 输出尽量稳定、可增量更新。
- 序列化格式与 UE 资产外壳解耦，避免后续更换存储方案时大面积改动。

## 6. 推荐目录细分

### 6.1 Runtime 模块目录

```text
Source/UERayTracingAudio/
├─ Public/
│  ├─ Components/
│  ├─ Settings/
│  ├─ Assets/
│  ├─ AudioEngine/
│  └─ UERayTracingAudioModule.h
└─ Private/
   ├─ Components/
   ├─ Settings/
   ├─ Assets/
   ├─ AudioEngine/
   ├─ Managers/
   ├─ Scene/
   └─ UERayTracingAudioModule.cpp
```

### 6.2 Editor 模块目录

```text
Source/UERayTracingAudioEditor/
├─ Public/
│  └─ UERayTracingAudioEditorModule.h
└─ Private/
   ├─ AssetFactories/
   ├─ Baking/
   ├─ DetailCustomizations/
   ├─ Visualizers/
   ├─ Windows/
   └─ UERayTracingAudioEditorModule.cpp
```

### 6.3 SDK 模块目录

```text
Source/UERayTracingAudioSDK/
├─ Public/
│  ├─ API/
│  ├─ Audio/
│  ├─ Baking/
│  ├─ Geometry/
│  ├─ RayTracing/
│  ├─ Scene/
│  ├─ Simulation/
│  └─ Serialization/
└─ Private/
   ├─ API/
   ├─ Audio/
   ├─ Baking/
   ├─ Core/
   ├─ Geometry/
   ├─ RayTracing/
   ├─ Scene/
   ├─ Simulation/
   └─ Serialization/
```

## 7. 关键数据流

### 7.1 运行时数据流

```text
UE Actor / Component
   -> Runtime Manager 收集参数
   -> Scene 层同步静态/动态几何
   -> RayTracing 后端执行射线查询
   -> Simulation 生成直接声/间接声结果
   -> Audio 层生成滤波、IR、卷积参数
   -> UE Audio Mixer 播放
```

### 7.2 烘焙数据流

```text
Editor 触发 Bake
   -> 导出场景与材质
   -> 构建光追场景
   -> 高质量多采样模拟
   -> 生成 Probe / IR / Reflection 数据
   -> Serialization 写入 UE 资产
   -> Runtime 加载并查询
```

## 8. 线程模型建议

由于 UE 硬件光追与渲染线程关系紧密，建议采用四层线程职责：

- Game Thread
  - 组件注册
  - 场景脏标记
  - Bake 请求发起
- Render Thread / RHI Thread
  - BLAS / TLAS 构建
  - RayTracing Query 提交
- Simulation Worker Thread
  - 声学采样调度
  - 结果聚合
  - 预算控制
- Audio Thread
  - 消费最终模拟结果
  - 更新 Spatialization / Occlusion / Reverb 参数

关键原则：

- 音频线程只消费已经准备好的结果，不直接等待光追查询。
- 场景更新与射线查询之间使用快照或双缓冲。
- 烘焙任务与实时模拟任务共享同一套场景与射线接口，但调度策略不同。

## 9. 依赖方向约束

推荐严格遵守以下依赖方向：

```text
UERayTracingAudioEditor
          ↓
UERayTracingAudio
          ↓
UERayTracingAudioSDK
```

并保证：

- `UERayTracingAudioSDK` 不依赖 Editor。
- `UERayTracingAudioSDK` 不依赖 UObject 层类型。
- `UERayTracingAudio` 不直接实现核心声学算法。
- Editor 只调用 Runtime 和 SDK 暴露出的正式接口。

## 10. 分阶段落地建议

### Phase 1：搭骨架

- 建立 `.uplugin`
- 建立三个模块
- 打通最小依赖关系
- 建立 Manager、Scene、RayTracingDevice、Simulator 的空实现

### Phase 2：直接声最小可用

- 静态场景导出
- 基于 UE Ray Tracing 的可见性测试
- Listener/Source 注册
- 遮挡、距离衰减、空气吸收
- 接入 UE Audio Occlusion / Spatialization

### Phase 3：间接声实时链路

- 反射射线采样
- 早期反射能量估计
- IR 生成
- 卷积播放链路

### Phase 4：烘焙链路

- Probe Volume
- Bake Window
- Baked IR / Reflection 数据资产
- 运行时加载与查询

### Phase 5：高级能力

- 动态对象增量更新
- 更高质量混响估计
- 路径传播
- 性能预算与质量分级

## 11. 当前最适合先实现的最小骨架

如果马上开始编码，建议第一批文件只覆盖下面这些核心入口：

- `UERayTracingAudio.uplugin`
- `Source/UERayTracingAudio/`
  - Module
  - Manager
  - Listener Component
  - Source Component
  - Geometry Component
- `Source/UERayTracingAudioEditor/`
  - Editor Module
  - 简单 Bake 面板骨架
- `Source/UERayTracingAudioSDK/`
  - `Context`
  - `Scene`
  - `RayTracingDevice`
  - `Simulator`
  - `SerializedObject`

这样可以尽快验证三件最重要的事：

- 插件模块关系是否正确
- UE 场景数据是否能进入 SDK
- UE 硬件光追查询是否能驱动音频模拟

## 12. 总结

这个项目最核心的架构思想不是“重写一个 Steam Audio UE 外壳”，而是：

- 在 UE 插件层复用 Steam Audio 的模块组织方式；
- 在核心层复用 Steam Audio `core` 的算法分层思想；
- 在射线求交后端上彻底替换为 UE 自身的硬件光追能力；
- 让实时模拟和烘焙共享同一套场景、射线查询、序列化和音频处理基础设施。

按这个结构推进，后续可以先跑通直接声，再逐步扩展到反射、混响、烘焙，而不会在项目早期把 Runtime、Editor、算法实现、光追后端全部耦合在一起。
