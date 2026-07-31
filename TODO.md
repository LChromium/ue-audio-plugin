# UERayTracingAudio 交付路线

## 当前决策

用户于 2026-07-22 对最新录音完成人耳复核，结论是：处理结果基本听不出原始音频。原来的四模式录音门禁只能证明 WAV 有能量和模式之间形状不同，不能证明输入内容仍可辨识，因此该门禁及监听补偿路线已回退。

后续坚持以下原则：

- 保留 `UERayTracingAudioSDK / UERayTracingAudio / UERayTracingAudioEditor` 三模块结构，以及已经接通的 UE Ray Tracing RHI、Direct/Indirect、DSP 和 Bake 核心。
- 优先完成真实功能，再补完整测试；测试框架可以先简化，但默认必须提供 Editor 界面验证。
- 不再用合成短脉冲、RMS 下限或任意监听增益替代真实输入音频和人耳验收。
- 每个“完成”结论必须同时有代码、构建、运行时、可辨识录音和人耳证据。

## Definition of Done

v1 必须同时满足：

- [x] UE 5.7 / Win64 测试工程与独立插件无编译错误。
- [x] Static Mesh 声学场景优先走硬件 Ray Tracing RHI；fallback 原因和统计可观察。
- [ ] Direct 输出保留可辨识原始内容，并正确表现距离、空气吸收、遮挡与方向。
- [ ] Indirect 输出包含可辨识的早期反射和晚期混响；Full 不得淹没或破坏 Direct 内容。
- [x] 实时音频线程不访问 UObject，Direct/Indirect 查询跨帧完成且无逐 source flush。
- [x] Bake 窗口能把用户选择的真实 SoundWave 与硬件光追 IR 卷积，保存并重新播放空间音频资产。
- [x] 固定 Editor 测试场景有 Listener、真实音频 Source、遮挡墙、封闭房间和可操作界面。
- [x] 自动验收以原始参考音频为基准，校验采样对齐、dry 保留、异常噪声、削波和模式语义。
- [ ] 目标耳机/扬声器上的人耳验收通过，并记录设备、输入资产、场景和结论。

## 当前修复：持续 Direct 与间接声

- [x] 把 Direct 距离样本增益从错误的 `1/r²` 改为线性声压 `1/r`，增加 1 m / 2 m / 4 m 持续非零回归。
- [x] Clear / Soft / Hard 运行时预设统一为 2 m；Soft 保留透声下限，Hard 才允许完全遮挡静音。
- [x] Reference / Direct / Wet / Full Editor Preview 统一循环，并同步 Stop、替换和面板销毁状态。
- [x] Realtime IR 使用 last-wins pending kernel，当前卷积预热、交叉淡化和尾音不被高频 generation 重置。
- [x] Baked 检查后恢复 `HybridReverb`；Realtime / Hybrid 要求 parametric tail 实际启用。
- [x] 数据源门禁增加持续性要求：总缓冲和输入有效缓冲均至少 24，Wet presence `>= 80%`，最长完全静音段 `<= 20%`，窗口积分及最大 Wet/Input RMS 均 `>= 5%`。
- [x] Python 回归 `50/50`。
- [x] 重新完成 UE 5.7 项目/插件完整构建、UE 音频 Automation `23/23`、插件全量 Automation `38/38` 和固定硬件运行时验证。
- [ ] 在 Editor PIE 中人工确认 Direct 持续、Wet/Full 可辨识且切换无 click/pop。

## R0：回退失败的试听验收路线

- [x] 复现门禁缺陷：最新目录只有 4 个处理结果，没有原始参考 WAV，无法验证原音可辨识。
- [x] 确认验证输入是代码生成的三频短脉冲，不是用户原始音频。
- [x] 移除 Occlusion `OutputGain`、90× component gain、16× output gain。
- [x] 移除合成四模式录音状态机、波形假通过脚本和交互式假验收脚本。
- [x] 保留固定硬件场景、RHI/CPU 数值对照、Bake 重复性和 Editor 启动流程。
- [x] 回退后的 Python 测试、UE 构建与固定运行时验证全部通过。
- [x] 复现固定启动黑屏，确认根因是加载空的 `/Engine/Maps/Entry`，并改为实际 First Person 关卡。
- [x] 固定 Game 验证中生成可见封闭声学房间、遮挡墙、Listener/Source 标记、灯光、相机和硬件状态叠层。
- [x] 明确蓝/橙密闭空间的实时语义：橙色是 Primary Source、蓝色是 Listener；`-game`/PIE 版本在 BeginPlay 后播放真实输入并使用 Realtime IR，Editor A/B 版本仅是 Bake 夹具，不能冒充实时验收。
- [x] 固定相机自动测试与可交互实时验收已拆分：交互模式由 First Person Pawn 驱动 Listener，主屏显示当前模式；F1/F2/F5 切换 Realtime/Baked/Hybrid，F3 在同步的 Rendered Direct+Wet 与 Original Unrendered 之间 A/B，F4 返回烘焙原点，F8 切换视角。
- [x] 自动交互 smoke 已覆盖 Pawn 移动、Listener/相机逐帧跟随、F1/F2/F5、F3 Rendered/Original、F4 烘焙原点和 F8 双视角；最近结果为移动 `50.900 cm`、Listener/相机与 F4 原点误差均 `0 cm`、A/B 重启 `0`。
- [ ] 在 Editor 中进入 PIE，人工确认 WASD/鼠标移动、Listener 跟随、F1/F2/F3/F4/F5/F8、Rendered/Original 可听差异以及切换期间无 click/pop；该人工结果不能由固定相机自动测试替代。
- [x] 修复固定启动器偶发静音：UE 5.7 默认 `[Audio] UnfocusedVolumeMultiplier=0.0`，工具启动的 Game 窗口失焦时会令 AudioDevice 主音量为 0、pre-distance 输入全零；固定 Game/Editor 命令现使用进程内 `-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0` 覆盖，不修改用户配置。

## R1：真实输入音频验收闭环（当前最高优先级）

已完成的验收基础设施（不等于人耳通过）：

- [x] Bake 面板支持显式选择 Source、Listener、Geometry 和真实输入 SoundWave。
- [x] 一次后台渲染生成同起点、同长度的 Reference / Direct / Wet / Full，并对四种模式只应用一个记录在 manifest 中的公共安全缩放系数。
- [x] 面板支持四模式播放、Reference↔Direct/Full A/B、重播、停止和 Human Pass/Fail JSON 记录。
- [x] UE Automation 覆盖四 WAV 存在/等长、Direct/Full dry correlation、公共缩放和 manifest。
- [x] 固定 Editor 流程创建可见且可操作的实际 A/B 场景：1 个 Source、1 个 Listener、7 个声学几何体、封闭房间、中央遮挡墙、灯光、相机和 Bake UI。
- [x] 使用原始项目资产 `/Game/FirstPerson/Audio/MarchingBand.MarchingBand` 完成一次硬件 IR Bake，保存 IR、Reference / Direct / Wet / Full 四个 SoundWave、四个 WAV 和 manifest；生成资产不会被递归选作下一次输入。
- [x] 固定 Editor A/B 流程会把自动生成并导入的四个 SoundWave 交给已打开的 Bake 面板；Listening Acceptance 控件位于首屏，只有四资产全部加载后才启用，启动器要求 `human_verdict=enabled` 标记。

- [ ] 在 Editor 验证界面中选择一段 10–20 秒、含清晰语音和瞬态/音乐的 SoundWave。
- [x] 录制未经过插件处理的 reference，以及 direct、wet、full；四段来自同一资产和同一采样起点。
- [x] 验收录音禁止逐模式补偿；四种模式只应用同一个记录在 manifest 中的公共安全缩放系数。
- [ ] Direct/Full 必须保留原始语音或音乐主体；Wet-only 应呈现空间尾部但不能冒充完整输出。
- [x] 自动门禁检查 reference 存在、格式/长度一致、起点对齐、Direct/Full dry 保留、电平比例、公共缩放、有限样本、零削波、活动窗零掉音、Direct 时间残差、Full 混音残差和模式差异。
- [x] 在 UI 中提供逐段播放、A/B 切换、重播和通过/失败记录。
- [x] 在 UI 中提供 Reference / Direct / Wet / Full 四条同时间轴、同 full-scale 的波形对比；波形直接来自本次 PCM16 离线渲染结果，不做逐模式归一化。
- [ ] 用目标耳机/扬声器完成首次人耳验收；失败时保留输入、输出和场景作为回归夹具。

验收：用户能明确辨认原始内容，并能听出无遮挡、遮挡、wet-only 和 full 的合理差异。

## R2：Direct 声学行为收敛

- [ ] 使用同一真实输入分别验证无遮挡、软遮挡、硬遮挡和移动穿墙。
- [x] Editor 实际场景提供 Clear / Soft Occluded / Hard Occluded 三个严格等距 2 m 预设，中央实体墙控制遮挡。
- [x] 使用同一原始 MarchingBand 完成 Clear / Soft / Hard 三档自动配对：Reference 字节一致、距离/距离衰减一致、visibility `1→0→0`、Direct gain `0.249667→0.087383→0`，三侧硬件湿声、安全门禁和非黑截图通过。
- [ ] Hard Occluded 静态语义已完成；仍需补完移动穿墙的连续音频验收。
- [ ] 静态 Clear/Soft/Hard 已证明零削波、零意外掉音窗和零异常时间/混音残差；距离扫描、空气吸收和移动遮挡仍待验证。
- [x] 检查 Occlusion 到 Spatialization 的 direct/wet 拆分与重组；实际离线证据中 Direct dry correlation 为 `1.0`，遮挡只按明确的 Soft floor 改变 Direct gain。
- [x] 增加低能量 IR 重建、16 kHz 高分辨率 CPU fallback 和硬件/CPU path/gain/IR energy ≤5% 差异门禁。

## R3：Indirect 与空间化收敛

- [ ] 在开放空间、靠墙位置和封闭房间验证早反、晚混响及方向性。
- [x] 封闭实际房间完成 1-bounce / 8-bounce 对照：8 次反射增加有效路径、IR 能量、平均延迟、方向 delay bin 和非零晚期尾声；GPU/CPU 指标一致，并生成具有可测左右差异的 Stereo 湿声。开放空间与靠墙位置仍待补齐。
- [x] GPU EnergyField 保留每条路径的监听者到达方向并回读到 delay-bin direction；Bake IR、离线 Wet/Full WAV 与运行时 Baked 卷积均保留左右声道，不再把方向统计压回 Mono。
- [x] Full 使用明确的 Wet Send 语义：`Full = Direct + WetSend × Wet`；新 Source 默认 `1.0`、范围 `0..4`，切换时平滑。双声道 Spatialization-bypass 与单声道最终 L/R 都有峰值诊断；最新三模式 `full_peak=0.032985/0.033651/0.114308`、`over_unit=0/0/0`。
- [x] Realtime、Baked、Hybrid 三种数据源使用独立左右核；Hybrid 同时渲染互补 Baked early 与 Realtime tail。Automation 覆盖切换/关闭无瞬时跳变；固定实际 Game 使用真实项目 SoundWave `/Game/FirstPerson/Audio/MarchingBand.MarchingBand`，证明三模式输入与 Wet 均非静音、核数 `2/2/4`、非有限样本为 0，并要求源增益和 pre-distance 音频均为非零。
- [ ] HRTF 属于质量升级；在 stereo panning 与真实输入闭环通过后再决定是否纳入 v1。

## R4：硬件光追 IR Bake 闭环

- [x] 从 Editor 选择真实 Source、Listener、Geometry 和输入 SoundWave。
- [x] 异步生成、保存、加载 Stereo IR 资产；运行时校验 world、scene/material signature 与 Source/Listener placement 并公开 stale 状态。Editor Bake 完成后把资产绑定到所选 Source，默认切到 Hybrid。
- [x] 用硬件 IR 卷积真实输入，导入新的 Reference / Direct / Wet / Full SoundWave，并能够在 Editor 中播放和 A/B。
- [x] 对同一场景重复硬件 Stereo Bake：两次均为 4000 samples / 0.25 s，能量相对差和逐样本相对 RMS 差均为 0；最终可听结论仍由 Human Pass 单独负责。

## R5：测试、性能与发布

- [x] 当前 Python `50/50`、UE 音频 Automation `23/23`、插件全量 Automation `38/38`；覆盖工作区内测试项目发现、交互 smoke、低能量方向/早晚分界、Directional Stereo、Occlusion/Spatialization 重叠缓冲、最终 Full 峰值两条路径、真实 Prepared Renderer 路径、实时卷积预算/切换容量/公平服务，以及失焦导致 pre-distance 静音的启动器拒绝回归。
- [x] 完成 hard-real-time 安全收敛：音频回调通过 lock-free 快照读取，不再经过注册表 `FRWLock`；卷积 kernel 与切换状态在非音频线程预备，回调只采用预备状态；静态审计覆盖 31 个关键函数/32 个函数体，并由运行时 `callbacks / callback_capacity_misses / convolution_prepare_drops` 门禁证明本次验证为 `197 / 0 / 0`。
- [x] 为实时卷积设置确定性预算：每声道最多 `4 × 1024 = 4096` 个 IR 样本；保留完整 Baked/离线 IR，超出实时头部的尾声能量由 parametric late reverb 承接。Bridge 预备池按活动声道提供切换余量，并以 512 MiB 实际工作区上限拒绝超额状态。
- [x] 记录 8/16/32 source 的 Game、Render、GPU、AudioMixer 与显存数据；三档均在实际可见场景中通过真实输入三模式门禁，完整结果见 `PERFORMANCE.md`。
- [x] Development Editor、Development Game、Shipping Game 均通过；BuildPlugin 使用 `-StrictIncludes` 完成 NoPCH / non-unity 三配置构建。
- [x] 审计 Win64 `0.2.0` Beta 交付包内容、版本号和已知限制，见 `PACKAGE_AUDIT.md`。

## 当前执行顺序

1. 在目标耳机/扬声器上播放当前 Reference/Direct/Wet/Full，记录 Human Pass/Fail；只有真实人耳通过后才结束目标。
2. 完成移动穿墙、距离扫描、开放空间和靠墙位置的人工试听矩阵。
3. 保持 hard-real-time 静态审计、运行时零容量违规门禁、项目/插件构建和 Automation 为发布回归基线。

## Task 2：World-scoped listener and acoustic scene state（2026-07-30）

- [x] Listener registration is first-wins per `UWorld`; a duplicate in the same World is ignored, while listeners in other Worlds remain independent.
- [x] Acoustic geometry/signatures use stable heap-owned per-World scenes; Source and Bake queries select their own World, direct/indirect hardware batches are partitioned by scene, and dead World state is cleaned after its in-flight references drain.
- [x] Required build passed: `uv run script\build_and_validate.py` exited `0`, with `31 functions / 32 bodies / 0 forbidden operations`, `Result: Succeeded`, and `Build and validation complete.`
- [x] Focused NullRHI Automation passed `3/3` (`0 failed`): `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-WorldScoping-Final.log`.
- [x] Full Audio NullRHI Automation passed `26/26` (`0 failed`): `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-Audio-Final.log`.
- [x] Fixed runtime flow `uv run script\launch_runtime_validation.py` exited `0`: Direct/Indirect batches `4/4`, hardware/CPU paths `171/171`, gain `0.001625/0.001625`, data sources passed, kernels `2/2/4`, `non_finite=0`, and hard realtime passed with `169 / 0 / 0` callbacks/capacity misses/prepare drops. Logs: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785425951315732900.log` and `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785426132061345000.log`.
- [ ] Run a true multi-PIE hardware session to supplement the deterministic two-World NullRHI tests and the single-World hardware runtime. The P3 coverage follow-up remains ledgered for final review; this documentation fix adds no tests.
- [ ] Complete Editor PIE/manual hardware listening and record Human Pass/Fail. Automated evidence does not constitute Human Pass, and the overall plugin is not complete.

## Task 3: Frequency-dependent, real-time-safe direct DSP (2026-07-31)

- [x] Added a per-source, per-channel complementary three-band processor using the configured `500 / 4000 Hz` defaults; channel storage is allocated during source initialization and never resized in the audio callback.
- [x] Direct distance and occlusion remain broadband, while the low/mid/high air-absorption gains are interpolated once per frame and applied independently. Wet mixing is unchanged.
- [x] Unsupported callback channel capacity records one hard-real-time miss per buffer and uses a non-allocating scalar broadband fallback.
- [x] Non-finite input samples are zeroed before they can poison persistent filter state.
- [x] Focused NullRHI Automation passed `9/9`; full Audio NullRHI Automation passed `32/32`. The frequency test measured low/high RMS `0.706901057 / 0.314958528`, ratio `2.244426`.
- [x] The prescribed build, `50/50` Python tests, static hard-real-time audit, and fixed hardware runtime passed. Runtime reported Direct/Indirect batches `4/4`, paths `171/171`, `non_finite=0`, and callbacks/capacity misses/prepare drops `179/0/0`.
- [ ] Complete target-headphone/speaker Human Pass for the distance sweep, audible air absorption, and moving occlusion matrix. Automation/runtime metrics are not Human Pass.
- [ ] Complete the Task 2 multi-PIE hardware isolation follow-up; this Task 3 runtime remains single-World.

## Task 4: Direct continuity diagnostics and public runtime setters (2026-07-31)

- [x] Added epoch-consistent Direct-only audio diagnostics for input-bearing buffers, Direct presence/silent runs, non-finite and over-unit Direct samples, and maximum per-sample three-band gain step.
- [x] Kept `RecordDirectBuffer` bounded and lock-free: one non-waiting writer CAS, plain-value inputs, bounded reads, and no allocation, free, logging, wait, UObject access, or shared-ownership mutation in the callback path.
- [x] Added Blueprint-callable `SetIndirectDataSource` and `SetBakedImpulseResponseAsset` under `UE Ray Tracing Audio|Indirect`. Baked-asset changes invalidate cached kernels while preserving monotonic published lane revisions.
- [x] Routed validation F1/F2/F5 and internal Realtime/Baked/Hybrid transitions through the public setters.
- [x] RED recorded the exact missing contracts: Direct diagnostics API absent and both setter UFUNCTION lookups null. Log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-RED-ConfigurableDirect.log`.
- [x] Final automated gates passed: build `47/47`; ConfigurableDirect `11/11`; Audio `34/34`; audit `36 functions / 37 bodies / 1634 lines / 0 violations`; Python `50/50`.
- [x] Fixed runtime exited `0`: hardware/CPU paths `171/171`, data-source gate passed, and callbacks/capacity misses/prepare drops were `169/0/0`. Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785466094540462400.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785466275542564200.log`.
- [ ] In the Editor left open by the fixed launcher, complete PIE/target-device listening and record Human Pass/Fail for F1/F2/F5 transitions, Direct continuity, moving occlusion, audible air absorption, and click/pop absence. Automated counters and the regression runtime are not Human Pass.

## Task 4 Fix Round 1: diagnostics target isolation and fallback continuity (2026-07-31)

- [x] Versioned Direct-diagnostics target changes and carried a plain component-id/generation token through each audio callback. A callback captured for an old target cannot publish after a target reset or an A/B/A target switch, and readers require the published generation to match the current target.
- [x] Added an actual scalar broadband history for the unsupported-channel fallback. The fallback now ramps the scalar gain per frame and reports the step it actually applied; zero-frame callbacks do not consume a pending ramp, and the first valid snapshot still seeds without a transition.
- [x] Fix RED recorded `10 passed / 2 failed`: stale-target publication and the constant-gain fallback both violated their new regressions. Log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix1-RED-ConfigurableDirect.log`.
- [x] Final gates passed: prescribed build `47/47`; ConfigurableDirect `12/12`; full Audio `35/35`; callback audit `39 functions / 40 bodies / 1718 lines / 0 violations`; Python `51/51`.
- [x] The exact runtime launcher exited `0`: Direct/Indirect batches `4/4`, hardware/CPU paths `171/171`, data sources passed, and callbacks/capacity misses/prepare drops were `176/0/0`. Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785468883156707700.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785469063951366500.log`.
- [ ] Manual target-device PIE listening, the moving Direct sweep, click/pop confirmation, and Task 2 multi-PIE hardware isolation remain open. Automated evidence is not Human Pass.

## Task 4 Fix Round 2: reset-only diagnostic isolation (2026-07-31)

- [x] Made `ResetDirect` advance the plain diagnostic-context generation around its epoch change while leaving the selected component ID unchanged. A callback token captured before reset can no longer publish into the new epoch.
- [x] Preserved same-ID `SetTargetAudioComponentId` no-op behavior, A/B/A rejection, one serialized control-thread writer, and the callback's one-attempt, lock-free, allocation-free publication path.
- [x] Deterministic RED found `13` ConfigurableDirect tests with `12 passed / 1 failed`: pre/post-reset generations were equal, the stale writer published one buffer, and the valid follow-up produced two instead of one. Log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix2-RED-ConfigurableDirect.log`.
- [x] Final gates passed: prescribed build `47/47`; ConfigurableDirect `13/13`; full Audio `36/36`; callback audit `39 functions / 40 bodies / 1718 lines / 0 violations`; Python `51/51`.
- [x] The exact runtime launcher exited `0`: Direct/Indirect batches `4/4`, hardware/CPU paths `171/171`, data sources passed, and callbacks/capacity misses/prepare drops were `190/0/0`. Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785470614600787600.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785470795487644500.log`.
- [ ] Manual target-device PIE listening, the moving Direct sweep, click/pop confirmation, and Task 2 multi-PIE hardware isolation remain open. The ledgered Task 4 Minors were not addressed in this round.

## Task 5: Isolated hardware Direct sweep fixture (2026-07-31)

- [x] Recorded behavioral RED from the newest fixed Game log: the exact marker `UERayTracingAudio direct sweep: passed=1` was absent, so the check exited `1` with `RED: current runtime has no passing hardware Direct sweep`.
- [x] Added TDD-covered 200 cm quarter-arc trajectory and sweep metrics for clear -> occluded -> clear ordering, visibility and gain ranges, continuity, bounded gain steps, hardware execution, and post-restore confirmation.
- [x] Added one non-reentrant runtime state machine shared by automatic validation and F6, with fixed listener origin, live HUD bands, one terminal summary, and exact one-time restoration of source transform and Direct/Indirect settings.
- [x] Isolated the fixture behind `WITH_UERAYTRACINGAUDIO_VALIDATION` in non-Shipping builds and the validation-scenario command line; ordinary projects remain inert.
- [x] Prescribed build passed with `48/48` actions and the hard-realtime callback audit remained `39 functions / 40 bodies / 0 forbidden operations`.
- [x] Closed review integrity findings with regression coverage: hardware wait terminates once with `hardware=0`; only accepted post-warmup generations establish all-hardware provenance; one process-owner World owns sweep/data-source diagnostics and HUD; rejected F6 cannot move a non-owner Pawn.
- [x] Focused ConfigurableDirect NullRHI verification passed `15/15`; log: `D:\Labs\2602-unreal\ue-audio-plugin\.worktrees\configurable-direct-audio-validation\TestProject\UeVersion1\Saved\Logs\Task5-FINAL-ConfigurableDirect.log`.
- [ ] Task 6 must add the fixed-launcher flag/parser and produce the required hardware Game-log terminal marker. Task 5 intentionally did not run the full runtime launcher.
- [ ] Human Pass is pending for target-device PIE listening, click/pop absence, and audible clear -> occluded -> clear recovery. Automated metrics are not Human Pass.
