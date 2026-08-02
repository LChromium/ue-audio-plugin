# 实现状态

更新日期：2026-08-01

## 当前结论

插件已经形成 `UERayTracingAudioSDK`、`UERayTracingAudio`、`UERayTracingAudioEditor` 三模块闭环，并已接通 UE 5.7 的硬件 Ray Tracing RHI、Direct/Occlusion、方向性多 bounce 间接声、Realtime/Baked/Hybrid IR、离线卷积和 Editor A/B 验收界面。

蓝/橙密闭空间已拆分为固定相机自动门禁和可交互 PIE 验收：交互模式把 Listener 跟随 First Person 相机，提供 F1/F2/F5 数据源切换、F3 同步 Rendered/Original A/B、F4 烘焙原点复位、F8 视角切换，并在主屏显示实际当前模式、A/B 播放状态和 Baked 资产状态。自动交互 smoke 已证明 Pawn、Listener、快捷键和两条播放链路工作；实际听感仍需人工确认。
F3 的 A/B 辅助逻辑只改变 Original/Rendered 播放链路，不改变 Source 的 `IndirectDataSource`；从 F2 Baked 或 F5 Hybrid 进入 F3 后仍保持原数据源。交互 smoke 现在额外要求 `f3_source_preserved=1`，防止把静默回退到 Realtime 误报为通过。

自动化证据证明的是实现链路、数值语义、音频完整性和硬件路径；最终“听起来正确”仍必须由目标耳机或扬声器上的 Human Pass 证明。未取得该记录前，不宣称最终声质验收完成。

## 2026-07-30 Hard-real-time 与实时卷积预算闭环

- 模拟快照注册表改为 lock-free 生命周期管理；Spatialization/Occlusion 音频回调不再获取注册表 `FRWLock`，也不读取 UObject。
- Realtime/Baked/Hybrid 的卷积 kernel、切换 crossfade 状态和工作区由非音频线程准备；音频线程只采用已准备对象。Bridge 的服务按 source 槽位轮转且受单次 PrepareBudget 和一次表扫描限制，避免高频 source 长期饿死。
- 实时卷积每声道限定为 `4 × 1024 = 4096` 个 IR 样本，48 kHz 时直接卷积头部约为 85.33 ms。完整 Baked/离线 IR 不裁剪；实时头部之外的能量估计会交给 parametric late reverb，避免用无限分区换取不可控音频线程成本。
- Prepared state pool 为活动声道提供至少一次并发切换的 headroom，并在准备完成后按真实 workspace 检查 512 MiB 上限；超额状态在非音频线程拒绝和回收。
- 新的 hard-real-time 源码审计覆盖 31 个关键函数、32 个函数体、1437 行，拒绝回调期锁、heap 操作、共享所有权、阻塞和 UObject 访问；`build_and_validate.py` 在同步/构建前强制执行该审计。
- 运行时门禁公开 `callbacks / callback_capacity_misses / convolution_prepare_drops`。最新固定验证为 `197 / 0 / 0`，因此本次运行没有发现回调容量不足或卷积准备容量丢弃。
- 生产 Publish → Prepare → Bridge Service → audio-thread adopt 路径已经替换 Automation 中的测试专用 owning convolver 旁路；新增实时预算、512-lane 切换 headroom、公平服务和诊断解析回归。
- 当前证据：Python `50/50`、UE 音频 Automation `23/23`、插件全量 Automation `38/38`、规定的项目/插件完整构建与固定运行时验证全部通过。Game 日志为 `TestProject/UeVersion1/Saved/Logs/UERayTracingAudioValidation-Game-1785417909352203200.log`，Editor 日志为 `UERayTracingAudioValidation-Editor-1785418090145276700.log`。

## 2026-07-30 Original / Direct / Wet 连续性与 Full 峰值闭环

- Original 与 Rendered 现在从同一 `MarchingBand` 采样零点同时启动并持续循环。硬件快照尚未就绪时只听 Original；只有目标 AudioComponent 的 Realtime Direct、Indirect 及左右 IR kernel 都有效后，Rendered 才允许自动淡入或由 F3 选择。17 秒多循环 smoke 中 `ab_restart_count=0`，避免把“重启后第一声”误当持续 Direct。
- 首个有效 Direct 快照会从物理增益开始；若快照晚于首个音频缓冲，则在一个缓冲内从 unity 平滑到目标。快照移除时平滑回 unity，零帧回调不会消耗渐变。运行时 Soft Occlusion 的 Direct 不再归零，Hard Occlusion 才允许接近静音。
- `IndirectMix` 的正式语义是 **Wet Send**，不是互斥 dry/wet 比例：`Full = Direct + WetSend × Wet`。新 Source、Editor 验收场景和离线请求默认 `1.0`，范围 `0..4`；运行时主验收夹具显式使用 `1.75` 作为可听补偿。Bake 启动时冻结 Source、Listener、输入 SoundWave、Actor 路径和 Wet Send，避免异步完成前切换选择污染结果。
- Full 峰值诊断覆盖 UE 的两条真实输出路径：双声道 `MarchingBand` 绕过自定义 Spatialization 时使用 Occlusion 的最终输出；单声道进入 Spatialization 时使用最终 L/R 重组后的输出。两条路径都有确定性 RED/GREEN Automation，且非有限最终样本会被计数并清零。
- 最新 180 秒硬件运行门禁中，Baked / Realtime / Hybrid 的 `full_peak` 分别为 `0.032985 / 0.033651 / 0.114308`，`over_unit=0/0/0`，`non_finite=0`；Wet 持续覆盖分别为 `24/24`、`57/57`、`24/24`，积分 Wet/Input RMS 分别为 `0.501271 / 0.050046 / 0.773309`。
- 自动移动 smoke 曾暴露 F4 传送后残余速度令原点漂移 `170.581 cm`；复位现在同时清空 Pawn 移动输入并调用 `StopMovementImmediately()`。重跑结果为移动 `50.900 cm`、Listener/相机误差 `0 cm`、F4 原点误差 `0 cm`，三种 IR、F3 双链路与 F8 双视角全部通过。
- 该阶段验证证据：Python `44/44`、UE 音频 Automation `13/13`、规定的项目/插件完整构建成功；对应运行日志为 `TestProject/UeVersion1/Saved/Logs/UERayTracingAudioValidation-Game-1785409168313219000.log`，Editor readiness 日志为 `UERayTracingAudioValidation-Editor-1785409349036387700.log`。最新完整证据见上一节。

## 2026-07-29 持续 Direct / Indirect 修复

- 运行时“一开始有声、随后消失”不是正常距离衰减：音频组件和 `MarchingBand` 实际仍在循环，旧场景在首个光追快照到达后把 Direct 压到了近静音。Direct 距离项现按线性声压幅度使用 `1/r`，不再把声强的 `1/r²` 直接乘到样本上；Soft Occlusion 保留明确的透声下限，Hard Occlusion 才允许完全遮挡接近静音。
- 固定门禁中的 Clear / Soft / Hard Direct 预设统一为 2 m：Clear 位于墙体同侧，Soft / Hard 位于墙体两侧，避免用距离差冒充遮挡效果。自动化覆盖 1 m / 2 m / 4 m 幅度 `1 / 0.5 / 0.25` 且持续非零。
- Editor 的 Reference / Direct / Wet / Full 比较资产改为循环预听，并跟踪实际 Preview AudioComponent；Stop、外部替换、面板销毁或播放结束都会同步清理状态。
- Realtime IR 高频更新改为有界的单槽 last-wins pending：当前卷积核继续保留历史和尾音，预热/交叉淡化完成后才切到最新 IR，避免每次 GPU generation 都重新进入静音预热。
- Baked 数据源检查结束后恢复 `HybridReverb`，Realtime / Hybrid 必须实际启用 parametric late tail。Wet 门禁不再接受一次峰值：每种数据源的总缓冲和输入 RMS 有效缓冲均至少 24；有效缓冲中 Wet RMS presence（`> 1e-8`）至少覆盖 80%，最长 Wet 完全静音连续段不超过有效观察窗的 20%；按帧积分的窗口 Wet/Input RMS 与最大 Wet/Input RMS 均须 `>= 5%`。逐缓冲 `audible_wet` / `max_inaudible_run` 仅作诊断，不作为通过条件。
- Python 脚本回归当前为 `41/41`；本轮 UE 构建、硬件运行时与人工试听结果在完成后补记。

## 功能矩阵

| 范围 | 实现状态 | 当前证据 | 剩余验收 |
| --- | --- | --- | --- |
| Direct | 已实现 | 距离衰减、空气吸收、方向、硬件可见性；Direct dry correlation 门禁 | 距离扫描、移动穿墙和目标设备试听 |
| Occlusion | 已实现 | Clear/Soft/Hard 等距预设；visibility `1/0/0`，Direct gain 随遮挡收敛 | 连续移动遮挡无 click/pop 试听 |
| Reflections | 已实现 | GPU EnergyField，多 bounce 路径和方向 delay bins | 开放空间、靠墙场景补充验收 |
| Reverb | 已实现 | early/late 分离、Stereo Wet、`Full = Direct + Wet Send × Wet` | 目标设备确认 Wet 尾声合理且不淹没 Direct |
| Realtime IR | 已实现 | 硬件 RHI 路径可观察，Realtime Stereo kernel 非静音 | 实际关卡长期运行验收 |
| Baked IR | 已实现 | Editor 异步 Bake、Stereo IR 资产保存/加载/stale 校验 | 用户场景资产验收 |
| Hybrid IR | 已实现 | Baked early + Realtime tail 互补交叉渐变 | 移动和切换时的人耳 click/pop 验收 |
| 离线对比 | 已实现 | 同输入、同起点、同长度 Reference/Direct/Wet/Full，共用安全缩放 | 10–20 秒语音/音乐素材试听 |
| 波形对比 | 已实现并通过构建 | Editor 面板四条同时间轴、同 full-scale 波形 | 在 Editor 中确认显示并完成人耳 A/B |
| Human A/B | 界面已实现 | 四模式播放、Reference↔Direct/Full、Replay、Human Pass/Fail JSON | 目标耳机/扬声器记录 PASS |
| 交互实时验收 | 已实现并通过自动交互 smoke | Pawn 移动 `50.900 cm`；Listener/相机和 F4 原点误差均 `0 cm`；F1/F2/F5、F3 双链路和 F8 均通过 | 实际 PIE 移动穿墙和人耳 click/pop 验收 |
| Hard realtime | 已实现并通过门禁 | lock-free 快照、非音频线程 Prepared state、源码审计；运行时 `197 / 0 / 0` | 后续每次发布持续保持零违规 |

## 硬件光追路径

- 静态网格声学几何优先提交 UE Ray Tracing RHI。
- Direct 可见性和 Indirect EnergyField 都通过跨帧渲染线程查询；音频线程只消费快照，不读取 UObject。
- 日志公开硬件/CPU fallback 路径、fallback 原因、batch source 数、有效路径、IR energy 和方向统计。
- 固定验证把硬件结果与 CPU acoustic-scene reference 对照；当前门禁要求路径、增益、IR 能量和方向指标处于容差内。

## 音频渲染链路

1. Source/Listener/Geometry 在 Game Thread 注册并发布场景快照。
2. Manager 批量提交 Direct 与 Indirect RHI 查询。
3. 模拟结果通过双缓冲快照交给音频线程。
4. Spatialization/Occlusion 分离 Direct 与 Wet，避免同一衰减重复应用。
5. Indirect Renderer 根据 Realtime、Baked 或 Hybrid 数据源更新左右卷积核，并平滑切换。
6. Editor Offline Renderer 使用同一原始 SoundWave 生成对齐的 Reference、Direct、Wet、Full WAV/SoundWave 和 manifest。

## 最近已知验证

- 2026-07-30：Python `50/50`、UE 音频 Automation `23/23`、插件全量 Automation `38/38`、UE 5.7 项目/插件完整构建通过。
- 2026-07-30：固定硬件门禁通过；4 Source Direct/Indirect batch 均为 4，GPU/CPU 有效路径均为 171，增益均为 `0.001625`；Baked/Realtime/Hybrid 的 Wet 有效缓冲为 `24/24`、`54/54`、`24/24`，Full 峰值为 `0.032427 / 0.033646 / 0.114014`，越界与非有限样本均为 0。
- 2026-07-30：hard-real-time 门禁通过：源码审计 `31 functions / 32 bodies / 1437 lines`，运行时 `callbacks=197`、`callback_capacity_misses=0`、`convolution_prepare_drops=0`。
- 2026-07-30：自动交互 smoke 通过：移动 `50.900 cm`、Listener/相机误差 `0 cm`、F4 原点误差 `0 cm`、A/B 重启 `0`、外部播放声源 `0`；Editor 已由固定脚本打开等待人工 PIE 试听。
- 2026-07-29：Python `36/36` 通过，包含工作区内项目发现、交互启动命令、F3 双链路解析和 Editor/Bake 夹具隔离。
- 2026-07-29：工作区内 `TestProject/UeVersion1` 的 UE 5.7 Development Editor 项目与插件完整构建通过；默认验证不再选择工作区外的同名工程。
- 2026-07-29：固定 `-game` 验证通过：4 个 Source 的 Direct/Indirect 硬件批次、GPU/CPU 路径与增益、Realtime/Baked/Hybrid、真实 MarchingBand 输入及实际非静音音量均满足门禁。
- 2026-07-29：自动交互 smoke 通过：移动 `250.901 cm`、Listener/相机误差 `0 cm`、F4 原点误差 `0 cm`，Realtime/Baked/Hybrid、Rendered/Original、固定/交互视角及双音频播放均通过。
- 2026-07-29：`--interactive-runtime` Editor readiness 通过，确认 `static_bake_fixture=0`，Editor 已留给用户进入 PIE。
- 2026-07-29：测试项目已移除 Ravel 资产、引用它的旧 World Partition `AudioActor` 及 Asset Registry 缓存记录；`Content/FirstPerson/Audio` 仅保留 `MarchingBand.uasset`。
- 2026-07-26：Python `32/32` 通过。
- 2026-07-26：UE Automation `21/21` 通过。
- 2026-07-26：UE 5.7 Development Editor 项目与插件构建通过。
- 已有 Clear/Soft/Hard、1/8 bounce、Realtime/Baked/Hybrid、Bake repeatability、8/16/32 source 性能证据。
- 波形 UI 与交互代码已纳入本轮构建；仍需在 Editor 中完成人工可见性、移动和听感验收。

## 已知未完成项

- 目标耳机/扬声器 Human Pass 尚未记录。
- 10–20 秒且同时含清晰语音和瞬态/音乐的最终验收输入尚未由用户确认。
- 移动穿墙、距离扫描、开放空间、靠墙位置的完整试听矩阵尚未完成。
- HRTF 暂不作为 v1 硬门槛；当前方向输出为 Stereo panning/方向性 IR。

## 完成标准

只有以下条件同时成立才可关闭总目标：

- 当前工作树重新构建、自动化和固定运行时验证全部通过；
- Editor 中硬件路径、四模式资产、同轴波形和 A/B 控件可见；
- Direct/Full 保留可辨识原始主体，Wet-only 是合理空间尾声；
- 目标设备上无削波、异常噪声、掉音、click/pop 或时间跳变；
- Human Pass JSON 记录输入、场景、设备和结论；
- `TODO.md`、`USAGE.md`、`IMPLEMENTATION_STATUS.md`、`progress_log.md` 与证据一致。

## Task 2：World-scoped listener and acoustic scene state

实现状态：Task 2 代码已实现并通过自动化验证。Manager 使用 per-World weak Listener entry（同一 World first-wins）和稳定 heap-owned acoustic state；Geometry/signature、Source/Bake 查询、stale/directional IR 与 Direct/Indirect hardware batching 都按 World/scene 隔离；dead World state 在相关 in-flight scene 引用结束后清理。

已验证证据（2026-07-30）：

- `uv run script\build_and_validate.py` exit `0`：`31 functions / 32 bodies / 0 forbidden operations`，`Result: Succeeded`，`Build and validation complete.`
- Focused NullRHI `UERayTracingAudio.Audio.ConfigurableDirect`：`3/3`、`0 failed`；`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-WorldScoping-Final.log`。
- Full NullRHI `UERayTracingAudio.Audio`：`26/26`、`0 failed`；`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-Audio-Final.log`。
- `uv run script\launch_runtime_validation.py` exit `0`：Direct/Indirect batches `4/4`，hardware/CPU paths `171/171`，gain `0.001625/0.001625`，data sources passed，`non_finite=0`，kernels `2/2/4`，hard realtime passed with callbacks/misses/drops `169/0/0`。Game：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785425951315732900.log`；Editor：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785426132061345000.log`。

剩余限制：两 World 行为由 NullRHI Automation 验证，硬件 runtime 只覆盖单 World；尚未执行 multi-PIE hardware isolation。人工 Editor PIE/hardware listening 与 Human Pass 未执行，P3 coverage follow-up 保留到 final review。因此 Task 2 的自动化验证已完成，但整个插件仍未完成。

## Task 3: Frequency-dependent, real-time-safe direct DSP

Implementation status: complete and automatically verified. Each occlusion source owns preallocated per-channel low/mid filter state plus previous three-band gains. Module startup validates and caches the project crossover pair, the factory/plugin copy it, and source initialization prepares the processor off the callback. The callback applies broadband distance times occlusion, multiplies by the snapshot's low/mid/high air gains, interpolates one gain vector per frame, processes channels independently, and preserves the existing Wet path. Capacity mismatch records one miss per buffer and uses a scalar non-allocating fallback. Non-finite input is rejected before persistent state mutation.

TDD evidence: the original plugin failed `FrequencyDependentAirAbsorption` with low/high ratio `1.000000` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task3-FrequencyDependentAirAbsorption-Red-Detailed.log`. Review-driven `NonFiniteInputRecovery` then failed because a NaN poisoned subsequent state at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task3-NonFiniteInputRecovery-Red-Detailed.log`; the finite-input guard made it green.

Final evidence (2026-07-31):

- `uv run script\build_and_validate.py` exit `0`: `35 functions / 36 bodies / 1510 lines / 0 forbidden operations`, `47/47` build actions, `Result: Succeeded`, and `Build and validation complete.`
- `uv run python -m unittest discover -s script\tests -v`: `50/50` passed.
- ConfigurableDirect NullRHI: `9/9`, `0 failed`; low/high RMS `0.706901057 / 0.314958528`, ratio `2.244426`; `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task3-ConfigurableDirect-Final-Reviewed.log`.
- Full Audio NullRHI: `32/32`, `0 failed`; `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task3-Audio-Final-Reviewed.log`.
- `uv run script\launch_runtime_validation.py` exit `0`: Direct/Indirect batches `4/4`, hardware/CPU paths `171/171`, gain `0.001625/0.001625`, data sources passed, kernels `2/2/4`, `non_finite=0`, and hard realtime callbacks/misses/drops `179/0/0`. Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785461920592378400.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785462101170308900.log`.

Remaining limitations: target-headphone/speaker Human Pass, distance-sweep listening, audible air-absorption listening, and moving-occlusion listening are not performed. Hardware runtime remains single-World, so Task 2 multi-PIE hardware isolation is also open. Automation and runtime metrics do not close those manual gates or the overall plugin.

## Task 4: Direct continuity observability and runtime mode API

Implementation status: complete and automatically verified. Direct diagnostics use their own requested/published epoch and odd/even snapshot sequence. The audio callback records Direct-only RMS, input peak presence, non-finite/over-unit Direct samples, and the maximum per-frame three-band interpolation step before adding Wet. A busy diagnostics writer causes the buffer to be skipped rather than waited on; reads are bounded to eight attempts.

`UUERayTracingAudioSourceComponent` now exposes Blueprint-callable `SetIndirectDataSource` and `SetBakedImpulseResponseAsset`. Mode changes are published by the next normal component tick. Asset replacement clears cached asset identity, sample-rate/transition cache, baked kernels, and tail state; published kernel identities and lane revisions remain monotonic so the next snapshot invalidates stale prepared convolution correctly. Validation hotkeys and internal mode phases use these setters.

TDD and final evidence (2026-07-31):

- RED build passed, then ConfigurableDirect reported `9 passed / 2 failed`: the Direct diagnostics reset/record/read API probe was false and both setter UFUNCTION lookups were null. `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-RED-ConfigurableDirect.log`.
- `uv run script\build_and_validate.py` exit `0`: audit `36 functions / 37 bodies / 1634 lines / 0 forbidden operations`, build `47/47`, `Result: Succeeded`, and `Build and validation complete.`
- ConfigurableDirect NullRHI: `11/11`, `0 failed`; `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-GREEN-ConfigurableDirect.log`.
- Full Audio NullRHI: `34/34`, `0 failed`; `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-GREEN-Audio.log`.
- Python: `50/50`; standalone callback audit: `36 functions / 37 bodies / 1634 lines`, all five violation categories zero.
- `uv run script\launch_runtime_validation.py` exit `0`: hardware/CPU paths `171/171`, all three data sources passed, kernels `2/2/4`, `non_finite=0`, and hard-realtime callbacks/misses/drops `169/0/0`. Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785466094540462400.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785466275542564200.log`.

Remaining limitations: the fixed runtime is regression evidence and does not itself constitute the later moving Direct sweep gate or Human Pass. Target-device PIE listening for Direct continuity, audible air absorption, F1/F2/F5 transitions, and click/pop absence remains unperformed. Task 2 multi-PIE hardware isolation also remains open; the overall plugin is not complete.

### Task 4 Fix Round 1: target generations and actual fallback ramps

Implementation status: complete and automatically verified. Direct-diagnostics target selection now has a monotonically changing odd/even generation. `ProcessAudio` captures a plain component-id/generation token once, and the diagnostics writer revalidates it after writer admission and immediately before publication. Published snapshots carry the target generation, so stale callbacks cannot contaminate a reset or A/B/A target epoch and readers cannot expose a mismatched generation. The callback remains bounded: one writer CAS, no retry loop, and no lock, allocation, logging, wait, UObject access, or shared-ownership mutation.

The unsupported-channel fallback now stores the broadband gain that was actually rendered. Non-empty buffers interpolate that scalar from its previous value to the snapshot target per frame, use it for output, and diagnose its actual maximum step. Zero-frame callbacks preserve pending interpolation, while first-valid-snapshot seeding remains transition-free.

Fix Round 1 TDD and final evidence (2026-07-31):

- Behavioral RED: ConfigurableDirect `10 passed / 2 failed`; the stale generation write became visible and the two-frame fallback applied a constant target instead of the required `0.625 -> 0.25` ramp. `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix1-RED-ConfigurableDirect.log`.
- `uv run script\build_and_validate.py` exit `0`: callback audit `39 functions / 40 bodies / 1718 lines / 0 forbidden operations`, build `47/47`, `Result: Succeeded`, and `Build and validation complete.`
- ConfigurableDirect NullRHI: `12/12`, `0 failed`; `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix1-FINAL-ConfigurableDirect.log`.
- Full Audio NullRHI: `35/35`, `0 failed`; `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix1-FINAL-Audio.log`.
- Python: `51/51`; standalone callback audit: `39 functions / 40 bodies / 1718 lines`, all five violation categories zero.
- `uv run script\launch_runtime_validation.py` exit `0`: hardware/CPU paths `171/171`, all three data sources passed, kernels `2/2/4`, `non_finite=0`, and hard-realtime callbacks/misses/drops `176/0/0`. Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785468883156707700.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785469063951366500.log`.

Remaining limitations are unchanged: the moving Direct sweep, target-device PIE listening, click/pop and audible-quality Human Pass, and Task 2 multi-PIE hardware isolation are still open. The overall plugin is not complete.

### Task 4 Fix Round 2: reset-versioned diagnostic contexts

Implementation status: complete and automatically verified. `ResetDirect` now transitions the shared diagnostic-context generation to odd, advances `RequestedEpoch`, then publishes the next even generation. The target component ID is unchanged. A pre-reset callback token therefore fails the writer's generation revalidation even if it acquires the sequence after reset and reads the new epoch. The rejected writer restores the even sequence, and a post-reset token can publish one new buffer normally.

`SetTargetAudioComponentId` still treats a same-ID assignment as a no-op, and A/B/A target switches retain their generation protection. Both target changes and resets rely on the existing one serialized control/game-thread writer. Audio callback work is unchanged: one non-waiting CAS attempt, bounded reads, and no lock, allocation, logging, wait, UObject access, or shared-ownership mutation.

Fix Round 2 TDD and final evidence (2026-07-31):

- Behavioral RED built `47/47`, then ConfigurableDirect reported `12 passed / 1 failed`. Pre/post-reset generations were equal; the stale writer initialized the reset epoch with one buffer; the post-reset follow-up produced totals of two buffers and two Direct-present buffers. `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix2-RED-ConfigurableDirect.log`.
- `uv run script\build_and_validate.py` exit `0`: callback audit `39 functions / 40 bodies / 0 forbidden operations`, build `47/47`, `Result: Succeeded`, and `Build and validation complete.`
- ConfigurableDirect NullRHI: `13/13`, `0 failed`; `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix2-FINAL-ConfigurableDirect.log`.
- Full Audio NullRHI: `36/36`, `0 failed`; `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix2-FINAL-Audio.log`.
- Python: `51/51`; standalone callback audit: `39 functions / 40 bodies / 1718 lines`, all five violation categories zero.
- `uv run script\launch_runtime_validation.py` exit `0`: Direct/Indirect batches `4/4`, hardware/CPU paths `171/171`, all three data sources passed, kernels `2/2/4`, `non_finite=0`, and hard-realtime callbacks/misses/drops `190/0/0`. Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785470614600787600.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785470795487644500.log`.

Remaining limitations are unchanged. The moving Direct sweep, target-device PIE listening, click/pop and audible-quality Human Pass, Task 2 multi-PIE hardware isolation, and the three ledgered Task 4 Minors remain open; the overall plugin is not complete.

### Task 5: validation-only hardware Direct sweep

Implementation status: code-complete, build/test verified, and accepted by the Task 6 fixed hardware runtime gate.

The runtime validator now owns one non-reentrant Direct-sweep state machine with clear hold, outbound motion, occluded hold, reverse motion, final clear hold, restoration, and terminal phases. The source follows a deterministic 200 cm quarter arc around a fixed listener. Pure helpers accumulate fresh-generation observations and require clear -> occluded -> clear ordering, near-200 cm distance, visibility range, non-zero Direct gain, bounded per-sample gain steps, no Direct dropout/non-finite/over-unit samples, hardware execution, and a fresh post-restore Direct result.

The fixture saves and restores the validation source transform, hard occlusion, `OccludedGain`, `IndirectMix`, and indirect data source exactly once on all terminal paths. Automatic execution is ordered after the first hardware Direct result and before baked/data-source validation; F6 starts the same machine interactively. Exactly one `UERayTracingAudio direct sweep:` summary is emitted. All declarations and runtime integration are guarded by `WITH_UERAYTRACINGAUDIO_VALIDATION`, which is `0` for Shipping, and execution additionally requires the existing validation scenario.

Review hardening selects the first successful scenario as the sole process diagnostics owner, so other Worlds cannot retarget/reset/read the process-global counters, start automatic/F6 sweeps, overwrite the sweep HUD, or emit extra terminal markers. Hardware readiness has a bounded terminal failure. Hardware provenance begins only after the discarded warmup generation, and every accepted sweep observation must report hardware ray tracing; any accepted CPU fallback forces failure and `hardware=0`.

Task 5 evidence:

- Behavioral RED: newest fixed Game log lacked exact marker `UERayTracingAudio direct sweep: passed=1`; the check exited `1`.
- Unit RED: the new trajectory/metrics tests failed to compile because `Validation/UERayTracingAudioDirectSweep.h` did not exist.
- GREEN: prescribed build completed `48/48`; callback audit remained `39 functions / 40 bodies / 0 forbidden operations`.
- Review-fix RED: the prescribed build failed on the intentionally absent pure owner/hardware/deadline policy (`C2653`/`C3861`) before that policy was implemented; the metrics regression also requires mixed hardware/CPU accepted generations to fail.
- GREEN: ConfigurableDirect passed `15/15`, including trajectory, metrics, all-hardware provenance, owner admission, and deadline assertions; `D:\Labs\2602-unreal\ue-audio-plugin\.worktrees\configurable-direct-audio-validation\TestProject\UeVersion1\Saved\Logs\Task5-FINAL-ConfigurableDirect.log`.

The fixed runtime launcher was deliberately not run in Task 5. Task 6 has now added its Direct-sweep flag and exact terminal-marker parser and recorded passing hardware Game-log evidence. Target-device audible quality, recovery, and click/pop Human Pass are still pending; the overall plugin is not complete.

### Task 6: strict fixed-launcher Direct sweep acceptance

Implementation status: complete and automatically verified. Every fixed Game command now requests the automatic Direct sweep, while Editor commands remain manual through F6. `validate_direct_sweep` requires exactly one complete marker line, rejects malformed/partial/ambiguous evidence and non-finite floats, aggregates all threshold failures, and is evaluated before the existing Realtime/Baked/Hybrid gate.

The first integrated runtime run entered the prescribed crash-debugging workflow after the pre-existing CPU-reference gate failed. There was no crash artifact or Fatal/Assertion. Historical evidence showed stationary runs at hardware/CPU `171/171`; the failing run started the sweep before baseline logging and compared asynchronous hardware `171` against the moved Source's CPU `953`. The pure admission policy now requires `bResultLogged`, so baseline result/CPU evidence is captured before Source motion and the sweep still completes before data-source Bake.

Task 6 evidence:

- Python RED: `42` tests, `16` expected failures for the absent flag/parser.
- C++ regression RED: prescribed build failed with C2660 at the new baseline-admission parameter.
- GREEN: Python `58/58`; callback audit `39 functions / 40 bodies / 1718 lines / 0 forbidden operations`; prescribed build `48/48`; ConfigurableDirect `15/15`.
- Fixed launcher exit `0`: baseline hardware/CPU paths `171/171`, gain `0.001625/0.001625`, both deltas `0`; Direct sweep `passed=1`, `213` generations, `200.000 cm`, visibility `0.000040-0.996630`, gain `0.174780-0.498240`, maximum step `0.00004591`, no dropout, restored and hardware-backed.
- Data sources passed with kernels `2/2/4` and `non_finite=0`; hard realtime callbacks/misses/drops were `1583/0/0`.
- Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785475390548759300.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785475571210643500.log`.

Remaining limitations: Editor PID `35468` was ready for that historical run and is superseded by PID `500`. Audible recovery and click/pop have not been judged by a person. Task 2 multi-PIE hardware isolation, the ledgered Task 4 Minors, and later plan tasks also remain open; the overall plugin is not complete.

### Task 6 Fix Round 1: parser domain and temporal integrity

Implementation status: complete and automatically verified. The strict parser now rejects negative, over-one, or inverted visibility extrema; non-positive, over-one, or inverted gain extrema; and negative maximum gain steps. Its accepted domains are `0 <= visibility_min <= visibility_max <= 1`, `0 < gain_min <= gain_max <= 1`, and `0 <= max_gain_step <= 0.01`.

The launcher now treats data-source Bake start as required Game evidence. The unique Direct terminal must precede that marker, the hard-real-time result, and the final data-source result. Ordering errors join other Direct failures in one explicit error rather than being silently ignored.

Fix Round 1 evidence:

- RED: focused RuntimeValidation `47` tests with `12` intended failures.
- GREEN: focused `47/47`; full Python `62/62`; callback audit `39 functions / 40 bodies / 1718 lines / 0 forbidden operations`.
- The previous Game log passed the hardened parser before a fresh runtime run.
- Exact launcher exit `0`: baseline hardware/CPU `171/171`; Direct `214` generations, `200.000 cm`, visibility `0.000038-0.997131`, gain `0.174779-0.498402`, step `0.00005548`, no dropout, restored/hardware; data sources passed; hard realtime `1580/0/0`.
- Ordered Game lines: Direct `1118`, Bake `1126`, hard realtime `1130`, final data source `1132`.
- Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785476653651929300.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785476834237184600.log`.

Editor PID `42300` responded and was left open for that historical run; it is superseded by PID `500`. Human Pass, Task 2 multi-PIE hardware isolation, and the ledgered Task 4 Minors remain open; the overall plugin is not complete.

### Task 7: Editor Direct fixture controls

Implementation status: complete and automatically verified.

The Editor validation fixture now owns exact `100 / 200 / 400 cm` Clear distances and three per-Source air-absorption profiles: Off `(0,0,0)`, Default `(0.0002,0.0006,0.0012)`, and Stress `(0.01,0.04,0.12)`. Repeated use moves the existing tagged Source instead of selecting a generic Source. Environment changes enumerate tagged acoustic Geometry, collect stale actors, and destroy them through normal teardown while preserving tagged non-geometry fixture actors and all untagged actors. Persistent Source/profile/light changes and geometry deletion participate in Editor transactions and dirty the level.

The Bake panel exposes the two required rows, displays effective distance/vector plus `Validation fixture only`, and disables fixture mutation throughout Bake and offline rendering. Existing public Source setters are used for runtime data-source and baked-asset changes.

The launcher adds the two options only to Editor. The Editor scene-ready marker carries distance, profile, and exact vector. Fixed validation requires one and only one marker-bearing line, full-matches it, and validates Direct preset, enclosed reflection environment, requested distance/profile, and the profile's exact vector. Automatic marker evidence is not an R3 environment comparison or Human Pass.

Task 7 evidence:

- C++ compile RED: missing air-profile interface (`C2653/C2065`).
- Behavioral RED: Off and Stress retained Default values, and OpenSpace retained seven tagged Enclosed geometry actors.
- Python RED: unrecognized CLI options and absent strict parser.
- GREEN: prescribed build `48/48`; focused Editor Automation `1/1`; full plugin Automation `54/54`; focused Python `2/2`; full Python `64/64`; callback audit `39/40/1718`, zero forbidden operations.
- Exact launcher exit `0`: baseline `171/171`; Direct sweep `206` generations, no dropout, restored/hardware; data sources passed; hard realtime callbacks/misses/drops `1584/0/0`.
- Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785478813724452300.log`.
- Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785478994289043000.log`; parsed fixture `200 cm`, `default`, `(0.0002,0.0006,0.0012)`.

Editor PID `34840` responded and was left open for that historical run; it is superseded by PID `500`. Target-device distance/air/moving-occlusion listening, R3 OpenSpace/NearWall/Enclosed listening, Human Pass, Task 2 multi-PIE hardware isolation, and Task 4 Minors remain open; the overall plugin is not complete.

#### Task 7 Fix Round 1: review hardening

Implementation status: complete and automatically verified.

Automatic panel discovery is now role-strict: it resolves only `VRTA_EditorValidationScene + VRTA_AB_Source`, and `StartBake` does not fall back to an arbitrary selected Source. Explicit **Use Selected Source** remains the opt-in path for normal project Sources.

All persistent validation instance-component paths modify the owner before `NewObject`/`AddInstanceComponent`, call `OnComponentCreated`, register the component, and participate in the surrounding transaction/dirtying flow. The regression separates Audio from Source creation by pre-installing the Source component, so deleting the Audio-path owner mutation is caught independently rather than being masked by the Source actor's earlier modification callback.

The managed Automation World creates a `FWorldContext` only after `UWorld::CreateWorld` succeeds, binds it immediately, and destroys World/context before GC. A test-only injected creation failure proves no context-count delta. Tagged actor/component teardown is checked before and after GC through weak pointers; focused/full logs contain zero `World has no context!` warnings.

Fix Round 1 evidence:

- RED: `Task7-Fix1-RED-OwnerBeforeComponent.log` failed Geometry/Source/Listener pre-construction owner checks; `Task7-Fix1-RED-WorldContextFailure.log` measured context count `1 -> 2`; the Audio-only mutation log failed only `Audio owner is independently modified before Audio construction`.
- One temporary mutation-injection build failed with C3861 because `TIsSame` was not visible in that translation unit; this was test instrumentation, not the final implementation. It was replaced with a component-name-only mutation, then fully removed.
- GREEN: prescribed build `48/48`; focused Editor `2/2`; full Automation `55/55`; Python `64/64`; callback audit `39/40/1718`, zero forbidden operations; zero World-context warnings. UE still emits two pre-engine-init `LogAutomationTest: Error: Condition failed` lines, but all Automation tests pass and there is no Fatal/Assertion.
- Exact launcher exit `0`: baseline paths `171/171`; Direct `231` generations, visibility `0.000027-0.997022`, gain `0.174776-0.498368`, step `0.00004127`, dropout `0`, restored/hardware `1/1`; data sources passed; hard realtime `1580/0/0`.
- Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785491460999842900.log`.
- Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785491641704854400.log`; strict fixture `200 cm / default / (0.0002,0.0006,0.0012)`; PID `13344` responded and was left open for that historical run, and is superseded by PID `500`.

Human Pass, R3 listening, Task 2 multi-PIE hardware isolation, and Task 4 Minors remain open; the overall plugin is not complete.

### Task 8: final technical verification and Shipping isolation

Historical 2026-07-31 checkpoint: automatic/technical R2 verification was complete; Human listening, true multi-PIE hardware isolation, and R3 were still unverified. The current R3 result is recorded in the final 2026-08-02 section below.

The normal product surface now has a single documented configuration contract: Project Settings cache `ReferenceDistanceCm=100` (`>=1`), `MaxDistanceCm=5000` (`>= ReferenceDistanceCm`), `SpeedOfSoundCmPerSecond=34300` (`>0`), `AirAbsorptionLowMidCrossoverHz=500` (`20..Nyquist`), and `AirAbsorptionMidHighCrossoverHz=4000` (`LowMid..Nyquist`); changes require Editor restart or audio-device reinitialization. Per-Source `AirAbsorptionPerMeter` / **Apply Air Absorption**, Blueprint-callable `SetIndirectDataSource` / `SetBakedImpulseResponseAsset`, first-valid Listener ownership per World, complementary three-band unity reconstruction, frequency-dependent Direct, and Wet bypass are documented independently of the opt-in validation fixture.

Final static/build/Automation evidence:

- `uv run script\validate_audio_realtime_safety.py` exit `0`: `39 functions / 40 bodies / 1718 lines`; lock, heap, shared-ownership, blocking, and UObject operations all `0`.
- `uv run python -m unittest discover -s script\tests -v` exit `0`: `64/64`. `git diff --check` exit `0` before documentation edits.
- `uv run script\build_and_validate.py` exit `0`: sync, UE 5.7 `UeVersion1Editor Win64 Development`, and standalone plugin build succeeded; plugin build ran `48/48` actions.
- Fresh ConfigurableDirect `15/15`, Audio `38/38`, and full `UERayTracingAudio` `55/55`, each with `0` failed tests, one `TEST COMPLETE. EXIT CODE: 0`, and no Fatal/Assertion. Logs: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FINAL-ConfigurableDirect.log`, `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FINAL-Audio.log`, and `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FINAL-Full.log`. The known two pre-engine-init `LogAutomationTest: Error: Condition failed` lines remain startup noise, not failed tests.

Shipping isolation evidence:

- `uv run script\build_and_validate.py --target UeVersion1 --configuration Shipping` exit `0`; project and plugin builds both reported `Result: Succeeded`. UBT log: `C:\Users\splay\AppData\Local\UnrealBuildTool\Log.txt`.
- Shipping executable: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Binaries\Win64\UeVersion1-Win64-Shipping.exe` (`143925760` bytes). Receipt: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Binaries\Win64\UeVersion1-Win64-Shipping.target`; configuration is Shipping and `BuildPlugins` contains `UERayTracingAudio`.
- Generated macro proof: `D:\Labs\2602-unreal\ue-audio-plugin\.worktrees\configurable-direct-audio-validation\Intermediate\Build\Win64\x64\UnrealGame\Shipping\UERayTracingAudio\Definitions.h` defines `WITH_UERAYTRACINGAUDIO_VALIDATION 0`; the runtime entry/HUD/F6 and Direct-sweep implementations are source-guarded by that macro.
- Exact ASCII and UTF-16LE scans of the executable, plus receipt scans, found `0` hits for `UERayTracingAudioValidationScenario`, the validation HUD title, F6 control strings, and `UERayTracingAudio direct sweep: passed=`. Normal binary strings still include `/Script/UERayTracingAudio`, Source/Project/Spatialization types, `EUERayTracingAudioIndirectDataSource`, and `UERayTracingAudioSDK`.
- Normal Shipping objects remain present for Direct (`...\UERayTracingAudio\UERayTracingAudioSpatialization.cpp.obj`), Indirect (`...\UERayTracingAudio\UERayTracingAudioIndirectRenderer.cpp.obj`), and SDK RHI (`...\UERayTracingAudioSDK\UERayTracingAudioRayTracingDevice.cpp.obj`) under `D:\Labs\2602-unreal\ue-audio-plugin\.worktrees\configurable-direct-audio-validation\Intermediate\Build\Win64\x64\UnrealGame\Shipping`. The pure validation sound-wave helper is not claimed to be byte-empty; the proof concerns excluded runtime entry points.

Final fixed-runtime evidence:

- `uv run script\launch_runtime_validation.py` exit `0`. Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785493624391644500.log`. Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785493804953148700.log`.
- The exact non-fallback SDK branches `submits direct sound visibility queries asynchronously to the render thread` and `submits indirect sound energy-field queries asynchronously to the render thread` each occur once. Baseline hardware/CPU paths are `171/171`, gain `0.001625/0.001625`, both relative deltas `0`.
- The unique strict marker is `passed=1`: `200` generations, distance `200.000 / 200.000 cm`, visibility `0.000038 / 0.996866`, Direct gain `0.174779 / 0.498316`, maximum band-gain step `0.00005484`, dropouts `0`, restored `1`, hardware `1`.
- Baked/Realtime/Hybrid each recorded `24/24` non-silent buffers, `24/24` Wet-present buffers, and maximum silent run `0`; data sources passed with kernels `2/2/4` and `non_finite=0`. Hard realtime passed at callbacks/capacity misses/prepare drops `1600/0/0`.
- Editor scene/UI readiness was `source=1 listener=1 geometry=7 lighting=1 bake_ui=1`, `200 cm / default / (0.0002,0.0006,0.0012)`. PID `10276` responded for that historical run and is superseded by PID `500`.

Historical open ledger at this checkpoint: Task 1's effective-crossover startup log hard-coded 48 kHz; Task 2 lacked focused ignored-listener-removal identity, scene-address-stability, and pending-request World-cleanup coverage; Task 3 needed invalid/out-of-order crossover, non-finite band-gain, and finite-input-overflow hardening; Task 4 needed transitive callback-safety/logging audit coverage, remaining Editor mode/bake/artifact/validation assignments routed through public Source setters, and saturating Direct diagnostic counters. Validation-role ambiguity and automated R3 are closed in the final 2026-08-02 section below. Human Pass, moving-player/moving-occlusion listening, audible distance/air comparison, click/pop judgment, and true multi-PIE hardware isolation remain open; the overall plugin goal is not complete.

### Task 8 Final Fix Wave: whole-branch hardening

Implementation status: all one P1 and twelve P2 findings from the final whole-branch review are fixed and automatically verified. This hardening does not close the remaining Human/R3 acceptance work.

RHI query publication now uses a synchronized immutable state handoff instead of concurrently accessing one `TSharedPtr`. Query back-references from readback items/segments are weak, cancellation is explicit, render-thread retirement is bounded, and Manager listener loss/removal cancels affected World queries. Direct `MaxDistanceCm` is enforced at the boundary. Direct and Indirect results report actual hardware use only after successful RHI dispatch, so a capable device, fallback, or empty scene cannot falsely satisfy the hardware gate.

No-listener publication now restores Direct bypass/unity and zeroes realtime Wet instead of replaying stale snapshots. Non-finite samples are sanitized before every stateful Wet boundary. Snapshot removal/reset immediately reclaims unpinned kernels, defers pinned reclamation to the Manager service point, and performs bounded current-entry reclamation. A convolver whose return mailbox is full suppresses output from its stale owner until the return can be retried, preventing Wet leakage after SourceId reuse.

Editor Bake admission no longer changes an ordinary actor's `AudioComponent.Sound`. Validation ownership is released only when its owning World tears down, allowing sequential PIE without transferring ownership while another World is active. The tagged validation fixture enforces exactly one deterministic actor per role and exactly one acoustic Geometry component per Geometry actor, then normalizes transforms, mesh/collision, and acoustic state before reporting ready; this closes duplicate-role and overlapping-geometry false readiness. Artifact production/parsing now agrees on field order and environment, waits for a complete terminal line, and requires unique strict Direct/HardRT/data-source markers plus the actual Direct and Indirect hardware submission markers.

Final-fix verification on 2026-07-31:

- TDD regressions cover async cancellation/retirement, Direct max-distance boundaries and provenance, listener removal/loss, non-finite Wet recovery, snapshot reclamation, convolver release/reuse, side-effect-free Bake admission, sequential PIE ownership, exact fixture normalization, and strict launcher/artifact contracts.
- Realtime audit: `39 functions / 40 bodies / 1775 lines`, with lock, heap, shared-ownership, blocking, and UObject callback violations all `0`. Python: `70/70`. `git diff --check`: exit `0`.
- Prescribed Development build: exit `0`. Focused Automation: ConfigurableDirect `15/15`, Audio `40/40`, Editor `9/9`; full `UERayTracingAudio` Automation `62/62`.
- Prescribed Shipping build: exit `0`, `34/34` plugin actions. `WITH_UERAYTRACINGAUDIO_VALIDATION` is `0`; exact scenario/HUD/Direct-sweep/F6 entry strings are absent from executable and receipt, while the plugin receipt and seven non-empty Direct/Indirect/RHI/Simulator/SDK objects retain the product runtime.
- Exact `--editor-ab-artifacts` flow: exit `0`; `hardware=1`, `auto_checks=1`, `distinct=1`, `imported=4`, `directional_wet=1`, `common_scale=1`. The fixture-only Wet send is `0.8`, preserving a recognizable Full dry cue without weakening acceptance thresholds; Wet/reference ratio is `0.567596`, Full/input ratio `0.549702`, and Direct/Wet difference `1.615526`.
- Exact default launcher: exit `0`. Game log `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785502210674966300.log`; Editor log `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785502391254800900.log`. Actual Direct/Indirect hardware markers were present; paths/gain matched CPU `171/171` and `0.001625/0.001625`; Direct passed with `215` generations, zero dropout, restored/hardware `1/1`; all three data sources passed; hard realtime was `1581/0/0` callbacks/misses/drops.
- Editor PID `13228` responded for that historical 2026-07-31 run and is superseded by PID `500`.

At this historical checkpoint, the callback-capacity P3, prior Task 1-4 minor ledger, target-device Human Pass, click/pop and audible-quality judgment, true multi-PIE hardware isolation, and the OpenSpace/NearWall/Enclosed R3 matrix were still open. Automated R3 is closed in the final 2026-08-02 section below; the human and other technical items remain open. The rejected Bake UObject-UAF candidate remains unchanged because the reviewed path does not retain the suspected UObject access.

#### 2026-08-01 final self-review addendum

A final Unreal C++ self-review found that duplicate tagged actors were normalized, but two `UUERayTracingAudioGeometryComponent` instances on the retained actor could still export overlapping geometry while first-match validation reported ready. The regression first failed with expected component count `1` and actual count `2`. The fixture now deterministically retains one safe component, transactionally destroys duplicate instance components, rejects multiple non-removable native components, and explicitly requires a final component count of one.

- Prescribed Development build: exit `0`. Realtime audit: `39/40/1775`, all forbidden-operation counts `0`; Python `70/70`; `git diff --check` exit `0`.
- Fresh Automation: ConfigurableDirect `15/15`, Audio `40/40`, Editor `9/9`, full plugin `62/62`; all four logs contain one `TEST COMPLETE. EXIT CODE: 0`, zero failed tests, and zero Fatal/Assertion markers. Logs are `FinalFix2-ConfigurableDirect.log`, `FinalFix2-Audio.log`, `FinalFix2-Editor.log`, and `FinalFix2-Full.log` under the project `Saved/Logs` directory.
- Fresh `--editor-ab-artifacts`: exit `0`; hardware/automatic/distinct/imported/directional/common-scale `1/1/1/4/1/1`, Wet/reference `0.567596`, Full/reference `0.549702`, Direct/Wet difference `1.615526`.
- Fresh default launcher: exit `0`. Game log `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785594301982190200.log`; Editor log `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785594482652644800.log`. Hardware/CPU paths and gain match at `171/171` and `0.001625/0.001625`; Direct passed with `199` generations, zero dropout, restored/hardware `1/1`; all data sources passed; hard realtime was `1596/0/0`.
- Editor PID `36896` responded for the historical 2026-08-01 run; Game and Editor logs contained zero Fatal, Unhandled Exception, or Assertion markers. That PID is superseded by PID `500`.

At that historical checkpoint, the R3 matrix was still open. The automated R3 status is superseded by the 2026-08-02 section below; target-device Human A/B, click/pop and audible-quality judgment, moving-player/occlusion listening, and true multi-PIE hardware isolation remain open.

## 2026-08-02 R3 32-bounce reflection environment matrix

Implementation status: the automated OpenSpace/NearWall/Enclosed R3 matrix is complete. Target-device listening and Human Pass remain open.

Chronology: the completion audit reopened the earlier Task 7 claim and rejected `20260802-063000` as final because it did not prove actual IR frame/duration/Wet-mix provenance and did not reject ambiguous validation-role topology before mutation. Commit `84830a3` added actual `impulse_response_frames` / `impulse_response_duration_seconds` provenance and strict 16 kHz/stereo/16000-frame/1.0-second/Wet-0.8 validation; `26dcf11` added a mutation-free preflight that rejects duplicate, multi-role, and no-role validation topology; `94b3537` exposed the actually enforced `fixed_float_tolerance=1e-6` and `ir_duration_consistency_tolerance=1e-9`. Only the post-fix rerun below closes the audit.

Authoritative summary:

`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\ReflectionEnvironmentMatrix_Manifest.json`

This `passed=true`, `end_to_end=true` run used only `/Game/FirstPerson/Audio/MarchingBand.MarchingBand`, clear Direct, 200 cm, default air absorption, 4096 reflection rays, 32 reflected segments/max bounces, and fixed validation Wet mix `0.8`. The Direct segment is not counted as a bounce. Every raw manifest and retained case summary records actual output `sample_rate=16000` / `channels=2`, `impulse_response_channels=2`, `impulse_response_frames=16000`, `impulse_response_duration_seconds=1.0`, and `wet_mix=0.800000011920929`. The Wet-mix absolute error is about `1.19e-8` under tolerance `1e-6`; frame/rate/duration consistency error is `0` under tolerance `1e-9`. Every case has actual hardware-tracing and CPU-reference provenance.

| Case | Geometry | HW/CPU paths | Gain / early / late | Direction ratio / bins | IR energy | Wet/reference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| OpenSpace | 0 | 0 / 0 | 0 / 0 / 0 | 0 / 0 | 0 | 0 |
| NearWall | 1 | 618 / 618 | 0.00677068205550313 / 0.00677068205550313 / 0 | 0.9340865 / 218 | 0.0067706802382897354 | 0.2198273241519928 |
| Enclosed | 7 | 20351 / 20351 | 0.046069249510765076 / 0.04604591429233551 / 0.000023299071472138166 | 0.6602507 / 1634 | 0.046069239603751005 | 0.5675959587097168 |

OpenSpace is the required physical-zero case: all hardware/CPU indirect, early, late, IR, and Wet values are exactly zero, and Direct/Full hashes match. Its generic `automatic_checks_passed=false` is therefore correct; NearWall and Enclosed are `true`. NearWall has directional early energy. Enclosed adds nonzero late energy and grows over NearWall by paths `32.93042071197411`, directional bins `7.495412844036697`, IR energy `6.804226160795334`, Wet/reference `2.582008223496685`, and late reverb `23299.071472138166`.

Hardware/CPU numerical agreement is effectively exact. OpenSpace relative deltas are all zero. NearWall paths/gain/early/late/bins deltas are zero, IR-energy delta is `1.07124056e-8`, directional-ratio delta is `1.27621253e-7`, and direction dot is approximately `1.0`. Enclosed paths/gain/early/directional-ratio/bins deltas are zero, late-gain delta is `7.80713199e-8`, IR-energy delta is `1.67794713e-9`, and direction dot is approximately `1.0`.

Exact per-case evidence:

- OpenSpace result: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\OpenSpace_Result.json`
- OpenSpace screenshot: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\OpenSpace.png`
- OpenSpace retained log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioEditorVisible-1785627966963000800.log`
- OpenSpace hardware manifest: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234624\MarchingBand_clear_HardwareIR_clear_20260801-234624_Manifest.json`
- OpenSpace IR: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Content\UERayTracingAudio\Validation\HardwareIR_clear_20260801-234624.uasset`
- NearWall result: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\NearWall_Result.json`
- NearWall screenshot: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\NearWall.png`
- NearWall retained log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioEditorVisible-1785627993652716000.log`
- NearWall hardware manifest: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234649\MarchingBand_clear_HardwareIR_clear_20260801-234649_Manifest.json`
- NearWall IR: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Content\UERayTracingAudio\Validation\HardwareIR_clear_20260801-234649.uasset`
- Enclosed result: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\Enclosed_Result.json`
- Enclosed screenshot: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\Enclosed.png`
- Enclosed retained log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioEditorVisible-1785628018462952000.log`
- Enclosed hardware manifest: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234734\MarchingBand_clear_HardwareIR_clear_20260801-234734_Manifest.json`
- Enclosed IR: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Content\UERayTracingAudio\Validation\HardwareIR_clear_20260801-234734.uasset`

All 27 required case artifacts exist and are non-empty; all 12 WAV SHA-256 hashes match; all 12 imported SoundWave assets and all 3 IR assets exist. The exact WAV paths and listening order are recorded in `USAGE.md`. The three screenshots passed nonblack/luma visibility checks but are byte-identical, so they prove visible Editor output only; strict markers/results/manifests, not screenshots, prove Geometry `0/1/7`.

Final gates:

- Python `158/158` passed. Log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FinalValidation-Python-20260802-074109.log`
- Realtime safety passed: `passed=1 functions=39 bodies=40 lines=1775`; lock, heap, shared-ownership, blocking, and UObject counts are all zero.
- Realtime safety log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FinalValidation-Safety-20260802-074109.log`
- Prescribed `uv run script\build_and_validate.py` exited `0`; project and standalone plugin builds passed. Log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FinalValidation-Build-20260802-074109.log`
- Role-ambiguity Automation `1/1`: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FinalValidation-Automation-RoleAmbiguity-20260802-074109.log`
- Offline-manifest-contract Automation `1/1`: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FinalValidation-Automation-OfflineContract-20260802-074109.log`
- Editor Automation `12/12`: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FinalValidation-Automation-EditorFull-20260802-074109.log`
- Full plugin Automation `66/66`: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FinalValidation-Automation-PluginFull-20260802-074109.log`
- All Automation runs exited `0` and all prohibited failure-marker scans were clean.
- Matrix driver: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task8-FinalValidation-Matrix-Driver-20260802-074109.log`; exactly one PASS points to `20260802-074606`.

The fixed `uv run script\launch_runtime_validation.py` flow also exited `0`. Its Game log is `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785628395987303100.log`; its Editor log is `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785628576602470800.log`. Game evidence passed paths `171/171`, gain `0.001625/0.001625`, Direct `221` generations with zero dropout and restored/hardware `1/1`, hard realtime `1579` callbacks / `0` misses / `0` drops, and all Baked/Realtime/Hybrid data sources with kernels `2/2/4` and `nonfinite=0`. Editor PID `500` is the sole relevant Unreal process, responding on the exact validation project/scenario with DX12 ray tracing, and was intentionally left open. This interactive launcher uses its established default eight bounces; only the separate `074606` matrix is the authoritative 32-bounce gate.

The rejected `20260802-052854` handled-ensure manifest, previously accepted `20260802-054250` and `20260802-063000` manifests, PID `16804`, PID `42028`, and every earlier “final/open” Editor PID stated above are superseded historical evidence. Only matrix `20260802-074606` and Editor PID `500` are current.

Deferred pre-existing limitations remain open:

- Mixed valid/invalid RHI geometry exports may still produce a partial TLAS because an invalid export can be skipped when another export builds an instance.
- Geometry excluded from direct visibility cannot yet act as indirect-only geometry in the shared TLAS builder.

The overall goal remains open for target-device Human A/B, click/pop and audible-quality judgment, moving-player/moving-occlusion listening, true multi-PIE hardware isolation, and every other unchecked ledger item. Automation does not record Human Pass.

## 2026-08-02 - F3 source-preservation fix

- F3 now only toggles synchronized Original/Rendered A/B playback. It no longer calls the Realtime data-source setter when entering Rendered from F2 Baked or F5 Hybrid.
- The interactive-smoke contract and parser require `f3_source_preserved=1`; the RED regression rejects `0`.
- Prescribed build passed after clearing stale generated plugin intermediates (`49/49` actions). Interactive smoke passed with movement `50.900 cm`, zero listener/camera/origin error, Realtime/Baked/Hybrid `1/1/1`, A/B `1/1`, `f3_source_preserved=1`, and no foreign audio. The exact default launcher also exited `0` with paths/gain `171/171`, `0.001625/0.001625`, Direct `170` generations, hard realtime `1605/0/0`, and all three data sources passing.
- Human PIE listening, click/pop/audible-quality judgment, moving-player/moving-occlusion listening, true multi-PIE hardware isolation, and target-device R3 listening remain open.
