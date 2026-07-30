# 可配置 Direct 音频与独立验证夹具设计

更新日期：2026-07-30

## 目标

把当前 Direct、遮挡、距离衰减和空气吸收链路收敛为可用于任意 UE 5.7 关卡的通用插件能力，同时保留一个显式启用、不会影响普通项目行为的硬件光追验证夹具。

本设计解决两个不同问题：

1. 产品能力必须通过 Project Settings、Source、Listener、Geometry、Blueprint/C++ API 配置，不依赖蓝/橙房间、固定距离或固定快捷键。
2. 自动验收必须能稳定复现 Clear → Soft Occluded → Clear 的连续遮挡变化，但它只能消费产品 API，不能把测试数值写进产品 DSP。

## 已确认的边界

- 采用方案 A：验证夹具让橙色 Source 沿以 Listener 为圆心、半径 200 cm 的圆弧运动，保持距离不变并穿过中央声学墙。
- 2 m、圆弧轨迹、F6 和蓝/橙标记仅属于验证场景。
- 普通游戏不创建验证 Actor、不移动用户 Source、不注册验证快捷键，也不显示验证 HUD。
- 固定相机自动门禁继续存在，但不能替代 PIE 中的 WASD/鼠标移动和目标设备试听。
- 开放空间、靠墙位置和完整 R3 间接声环境矩阵不属于本次 Direct 规格；它们作为后续独立特性完成。
- v1 支持每个 `UWorld` 一个活动 Listener；不在本次范围内实现同一 World 内的分屏多 Listener 混音。

## 当前实现与需求差异

### 已有的通用接口

`UUERayTracingAudioSourceComponent` 已公开 Direct 开关、软/硬遮挡、声源半径、遮挡采样数、空气吸收、Realtime/Baked/Hybrid、反射参数、IR 长度和 Wet Send。

`UUERayTracingAudioGeometryComponent` 已公开是否参与声学场景、是否影响 Direct、Bounding Box/Static Mesh Triangles，以及三频吸收、透射和散射。

Editor Bake 已能选择任意 Source、Listener、Geometry 和 SoundWave，并生成 Reference、Direct、Wet、Full 与 IR 资产。

### 必须修正的通用性缺口

- `FUERayTracingAudioContext` 把参考距离 100 cm、最大距离 5000 cm 和声速 34300 cm/s 写死。
- Listener 使用进程级静态单例；同时打开多个 PIE World 时可能选到错误 World 的 Listener。
- Direct 仿真生成低/中/高三频空气吸收，但 Occlusion 音频回调把三个值取平均，只产生宽带音量变化。
- Realtime/Baked/Hybrid 虽然是 `BlueprintReadWrite` 属性，但缺少明确的 Blueprint/C++ 切换函数和一致的状态刷新入口。
- Editor 场景已有未闭环的距离/反射环境参数，但正常面板没有入口；切换环境时也不会删除上一环境遗留的验证几何。
- 当前交互 smoke 的 Pawn 被实体墙碰撞阻挡，只移动约 50.9 cm，不能证明遮挡状态穿越。

## 产品架构

### 1. 项目级声学物理设置

在 Runtime 模块增加 `UUERayTracingAudioProjectSettings : UDeveloperSettings`，显示在：

`Project Settings → Plugins → UE Ray Tracing Audio`

配置项：

| 属性 | 默认值 | 约束 | 用途 |
| --- | ---: | --- | --- |
| `ReferenceDistanceCm` | 100 | `>= 1` | 距离衰减参考距离 |
| `MaxDistanceCm` | 5000 | `>= ReferenceDistanceCm` | Direct/Indirect 最大传播距离 |
| `SpeedOfSoundCmPerSecond` | 34300 | `> 0` | IR delay 与反射延迟 |
| `AirAbsorptionLowMidCrossoverHz` | 500 | `20..SampleRate/2` | Direct 低/中频分界 |
| `AirAbsorptionMidHighCrossoverHz` | 4000 | `LowMid..SampleRate/2` | Direct 中/高频分界 |

这些属性使用 `Config=Engine, DefaultConfig`，并标记修改后需要重新初始化音频设备。SDK 模块继续保持纯 C++：Runtime 模块在初始化时读取 `UDeveloperSettings`，校验后把普通值类型 `FUERayTracingAudioContextSettings` 传给 `FUERayTracingAudioContext::Configure()`。SDK 不依赖 UObject。

`AirAbsorptionPerMeter` 继续由每个 Source 配置，因为不同声源、环境或艺术需求可能不同；不把验证用的夸张系数设置成产品全局默认。

### 2. World 作用域 Listener

把进程级 `CurrentListener` 改为 Game Thread 上按 `UWorld` 查找的 Listener 注册表：

- `BeginPlay` 注册到其所属 World。
- `EndPlay` 只移除匹配的 World/Component。
- Manager 根据 Source 的 World 获取同 World Listener。
- 同一 World 注册多个 Listener 时记录明确警告，并确定性保留先注册且仍有效的 Listener。
- World 销毁或 Listener 失效后清理弱引用。

这支持 Editor World、PIE World 和多 PIE 实例并存，避免跨 World 声学计算。单个 World 内仍只有一个活动 Listener，符合本次 v1 边界。

### 3. 真正的三频 Direct 空气吸收

增加独立、无 UObject 的 `FUERayTracingAudioThreeBandAirAbsorption` DSP 单元。每个输入声道维护两级一阶低通状态：

```text
low  = LP_low_mid(input)
mid  = LP_mid_high(input) - LP_low_mid(input)
high = input - LP_mid_high(input)
```

三个频带相加严格重建原输入，因此当三频增益都为 1 时，插件不会染色原音。

每帧 Direct 计算为：

```text
broadband = distance_attenuation × occlusion
direct = broadband × (
    low  × air_absorption.low
  + mid  × air_absorption.mid
  + high × air_absorption.high
)
```

如果 Occlusion Settings 关闭空气吸收，三个空气吸收增益统一为 1；距离和遮挡仍正常工作。Indirect Wet 不经过该 Direct 滤波器。

低/中/高目标增益在一个音频缓冲内从上次值线性平滑到新快照值。每个声道的滤波状态在 Source 初始化时准备，`ProcessAudio` 不扩容、不加锁、不访问 UObject。声道数与已准备容量不匹配时：

1. 记录 hard-real-time capacity miss；
2. 对该缓冲使用不分配的宽带安全 fallback；
3. 运行时验证因此失败，而不是静默宣称通过。

### 4. 明确的运行时 API

在 `UUERayTracingAudioSourceComponent` 增加 Blueprint/C++ 方法：

```cpp
UFUNCTION(BlueprintCallable, Category = "UE Ray Tracing Audio|Indirect")
void SetIndirectDataSource(EUERayTracingAudioIndirectDataSource NewDataSource);

UFUNCTION(BlueprintCallable, Category = "UE Ray Tracing Audio|Indirect")
void SetBakedImpulseResponseAsset(
    UUERayTracingAudioImpulseResponseAsset* NewAsset);
```

方法更新公开属性并触发下一次 Game Thread snapshot 刷新。验证场景的 F1/F2/F5 必须调用这些方法，确保测试覆盖用户实际使用的 API。

Direct、Indirect、空气吸收和声学材质参数继续保留为 Blueprint 可编辑属性。普通项目可以自行绑定 Enhanced Input，不依赖插件验证快捷键。

## 验证架构

### 1. 编译和启动隔离

验证场景只在非 Shipping 构建中编译其运行入口，并且只有命令行包含 `-UERayTracingAudioValidationScenario` 时才创建 Actor、HUD 或快捷键。

普通插件启动：

- 不生成蓝/橙房间；
- 不查找 MarchingBand；
- 不修改玩家 ViewTarget；
- 不注册 F1–F8 验证行为；
- 不移动任何 Source 或 Listener。

### 2. 恒定距离 Direct Sweep

固定验证首先完成初始硬件 Direct 结果，然后执行：

1. 保存 Source Transform、`bHardOcclusion`、`OccludedGain`、`IndirectMix` 和数据源。
2. 把 Listener 放在验证原点，Source 放在半径 200 cm 的无遮挡端点。
3. 使用 Soft Occlusion，沿圆弧在 3 秒内移动到墙后端点。
4. 保持 0.5 秒。
5. 沿相同圆弧在 3 秒内返回无遮挡端点。
6. 保持 0.5 秒并恢复全部保存状态。
7. 等待新的 Direct snapshot 确认恢复位置后，才继续 Bake/Realtime/Baked/Hybrid 门禁。

圆弧几何保证 Source/Listener 距离恒定；Source Actor 使用验证控制的 Transform 更新，不受 Pawn 或墙体碰撞影响。玩家的交互移动逻辑不参与自动 sweep。

### 3. 自动门禁

只在 Direct generation 变化时采样硬件结果。结果必须同时满足：

- 至少观察到 8 个不同 Direct generation；
- Source/Listener 距离始终为 `200 ± 2 cm`；
- 最大 visibility `>= 0.90`；
- 最小 visibility `<= 0.10`；
- Soft Occlusion 最小 Direct target gain `> 0`；
- 至少观察到一次 Clear → Occluded 和一次 Occluded → Clear；
- 音频回调中 Direct 输入有效缓冲持续存在；
- 非零目标 Direct 下不存在 Direct-only 完全静音连续段；
- 所有 Direct 样本有限且无超过 1.0 的新增越界；
- 音频回调报告的最大逐样本三频增益步长 `<= 0.01`；
- `callback_capacity_misses=0`；
- sweep 完成后 Source Transform 和配置与开始前一致。

日志输出单一可解析摘要：

```text
UERayTracingAudio direct sweep: passed=1 generations=... distance_min_cm=...
distance_max_cm=... visibility_min=... visibility_max=...
gain_min=... gain_max=... max_gain_step=...
direct_dropouts=0 restored=1 hardware=1
```

`launch_runtime_validation.py` 必须解析并要求 `passed=1`。缺失 marker、CPU-only 冒充硬件、容量违规、未恢复状态或数值不满足门禁都使固定运行验证失败。

### 4. PIE 可听界面

验证场景增加 F6：

- 仅在硬件初始化完成且当前没有 sweep 时启动同一恒距轨迹；
- 开始前把 Listener 返回验证原点；
- 允许 F3 在 Original 与 Rendered 之间比较；
- sweep 期间主屏显示 `DIRECT SWEEP: CLEAR / ENTERING WALL / OCCLUDED / RETURNING / COMPLETE`；
- 同时显示距离、visibility、Direct gain 和 Air Absorption 三频值；
- 完成或失败都恢复 Source 状态。

F6 不加入插件的通用输入映射；它只存在于验证控制器。用户项目通过 Blueprint API 自行决定输入。

### 5. Editor Direct 验收界面

Bake 面板的验证场景区域增加：

- `Clear 1 m`
- `Clear 2 m`
- `Clear 4 m`
- `Air Absorption: Off`
- `Air Absorption: Default`
- `Air Absorption: Stress`

前三个按钮只移动验证 Source，不修改用户选择的普通 Source。空气吸收按钮只作用于带 `VRTA_EditorValidationScene` 标签的 Source：

- Off：`(0, 0, 0)`
- Default：恢复 Source 构造默认值 `(0.0002, 0.0006, 0.0012)`
- Stress：使用明确标注为测试用的 `(0.01, 0.04, 0.12)`

Stress 用于在短距离内明显听出高频衰减，不改变项目设置或新建 Source 的产品默认值。面板显示当前距离、空气吸收配置和“Validation fixture only”提示。

## 数据流

```text
Project Settings
  → Runtime module validates plain context settings
  → SDK Context
  → Manager builds Direct/Indirect inputs

World Source + same-World Listener + Geometry
  → hardware RHI query
  → stabilized Direct result
  → lock-free audio snapshot
  → per-source three-band Direct DSP
  → Direct + Wet Send × Indirect

Optional validation command
  → validation Source trajectory / UI
  → same public component and Blueprint APIs
  → runtime diagnostics and parser gate
```

验证代码不得绕过 Manager、snapshot、Occlusion DSP 或公开 Source API。

## 错误处理与可观察性

- 非法 Project Settings 在 Game Thread clamp，并记录原值和有效值；音频回调不记录日志。
- Source 所属 World 没有 Listener 时 Direct/Indirect 结果标记无 Listener，并公开原因。
- 同 World 多 Listener 时明确警告所选 Listener。
- 硬件 Ray Tracing 不可用时保留现有 CPU fallback，但验证摘要必须显示 `hardware=0` 并失败。
- 三频滤波状态容量不足时记录现有 hard-real-time capacity miss。
- F6 重入被拒绝并显示当前阶段，不创建第二条轨迹。
- World 结束、Source/Listener 销毁或验证超时都会恢复可恢复的 Source 状态并记录失败原因。
- Baked 资产的位置/场景 stale 检查保持不变；sweep 恢复后必须重新确认 Baked status。

## 测试策略

### C++ Automation

1. Project Settings 到 SDK Context 的值传递和非法值 clamp。
2. 两个 PIE World 的 Source 只能选择各自 World 的 Listener。
3. 三频滤波在 `(1,1,1)` 时逐样本重建输入。
4. 低频正弦在只衰减 high 时基本保留，高频正弦明显衰减。
5. stereo/multichannel 状态独立，无串扰。
6. snapshot 三频目标变化在缓冲内平滑，最大步长符合门槛。
7. 关闭空气吸收后只保留距离与遮挡宽带增益。
8. Blueprint setter 触发 Realtime/Baked/Hybrid 状态刷新。
9. Direct sweep 轨迹的所有点距 Listener 为 200 cm，并跨过墙体平面后返回。
10. sweep 状态机在完成、超时和 Actor 销毁时都恢复原设置。

每项先写能因缺失功能而失败的测试，再写最小实现使其通过。

### Python

1. 启动命令默认请求 Direct sweep 门禁。
2. parser 接受完整的硬件通过摘要。
3. parser 拒绝距离漂移、visibility 未跨越、Direct dropout、过大 gain step、CPU-only、未恢复和缺失 marker。
4. Editor 命令正确传递 1/2/4 m 与验证空气吸收 profile。

### 完整验证

主要特性完成后按项目约束执行：

```powershell
uv run script\build_and_validate.py
uv run script\launch_runtime_validation.py
```

随后在 Editor PIE 中执行 F3/F6，使用 MarchingBand 确认：

- Original 保持未渲染；
- Rendered 在 Clear、进入墙体、Occluded、返回 Clear 的变化连续；
- Soft Occluded 不停止播放；
- 没有 click/pop、异常噪声或重启采样位置。

自动门禁只证明代码路径和数值连续性；最终可听结论仍需要 Human Pass。

## 文档与交付

实现完成后同步：

- `TODO.md`
- `USAGE.md`
- `IMPLEMENTATION_STATUS.md`
- `plan.md`
- `progress_log.md`

文档必须分别说明：

- 普通项目如何配置通用插件；
- 如何通过 Blueprint 切换运行时 IR；
- 验证夹具为何不是产品依赖；
- 如何运行 F6 Direct Sweep 和解释门禁；
- 当前仍未完成的人耳与 R3 场景矩阵。

## 完成标准

本特性只有在以下条件同时成立时完成：

- 普通关卡无需验证 Actor 或快捷键即可配置并运行 Direct/Indirect；
- Project Settings 和 per-source/per-geometry 参数真正影响同一生产链路；
- Direct 空气吸收是频率相关处理，unity 时不改变原音；
- Listener 不会跨 World 误绑定；
- 方案 A 使用硬件结果完成恒距双向遮挡 sweep；
- 验证结束完整恢复 Source；
- C++ Automation、Python、规定构建和固定运行时验证全部通过；
- Editor 保留可操作界面等待人工 F3/F6 试听；
- 文档不把自动测试冒充 Human Pass。
