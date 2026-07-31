# 实施计划

更新日期：2026-07-30

## 目标

在 UE 5.7 中交付真实可用、硬件路径可观察的光线追踪音频插件，完成 Direct、Occlusion、多 bounce Reflection、Reverb、Realtime/Baked/Hybrid IR、Bake 闭环，以及带界面的 Reference/Direct/Wet/Full 同起点 A/B 和波形对比。

保留蓝/橙密闭空间作为正式的**可交互实时硬件光追验收目标**：橙色对象是播放真实 SoundWave 的 Primary Source，蓝色对象是 Listener，不是第二个声源。自动测试继续使用固定相机；人工实时验收必须把 Listener 绑定到 First Person Pawn/相机，允许 WASD 和鼠标移动，并在主屏显示当前 Realtime/Baked/Hybrid 模式及快捷键。只有 Editor 世界中的静态 A/B/Bake 夹具不得被当作“实时已通过”的证据。

## 当前状态与需求差异

- 核心实时、烘焙、离线渲染和 A/B 播放链路已经存在。
- 已定位“Direct 只响一下”的两条独立路径：运行时首个快照后被错误增益压到近静音；Editor 比较面板使用一次性 Preview。现已分别改为正确的 `1/r` 线性幅度/等距场景，以及四轨循环 Preview。
- Realtime IR 高频更新不再反复清空卷积历史；Baked 检查后恢复 Hybrid late tail，并以 `>= 24` 个输入有效缓冲、`>= 80%` Wet presence、`<= 20%` 最长完全静音段及 `>= 5%` 窗口积分 Wet/Input RMS 拒绝“一次瞬态后归零”。
- 蓝/橙密闭空间存在两种生命周期：固定 `-game`/PIE 版本是实时目标；Editor A/B 版本只是为 Bake 选择 Source、Listener 和 Geometry 的夹具。验收结论必须明确区分二者。
- 原实时房间虽然每帧更新声学结果，但 PlayerController 被固定 Camera 接管，Listener 也没有跟随 Pawn，因此不能完成移动穿墙验收；固定/交互双模式现已实现，并通过本地工程构建、固定运行时门禁、自动交互 smoke 与 Editor readiness，待人工 PIE 移动和听感确认。
- 验证脚本默认只发现当前仓库 `TestProject/**/*.uproject`，不再自动同步到工作区外的同名工程。
- 测试项目已移除 Ravel，实时 A/B 与离线验证的预置真实输入统一为 MarchingBand。
- 原 A/B 界面只有播放按钮和统计文本，没有明确的渲染前/后波形显示；2026-07-29 已开始补齐四轨同轴波形。
- `IMPLEMENTATION_STATUS.md` 曾被删除，`progress_log.md` 和本文件缺失；已恢复状态管理。
- 自动门禁已很完整，但不能代替最终人耳验收。
- Original/Rendered 已改为同起点持续双链路：Original 在硬件预热期间立即可听，Rendered 只在有效 Direct/Indirect/左右 IR 快照后淡入；17 秒多循环观察中 A/B 重启为 0。
- `IndirectMix` 已统一为 Wet Send：`Full = Direct + WetSend × Wet`，默认 `1.0`、范围 `0..4`；Bake 启动时冻结 Source/Listener/输入/Wet Send。运行时专用 `1.75` 保留为明确试听补偿。
- 最终 Full 峰值门禁同时覆盖双声道绕过 Spatialization 与单声道后级 L/R；最新三模式峰值为 `0.032985 / 0.033651 / 0.114308`，越界和非有限样本均为 0。
- 音频回调现通过 lock-free 快照消费模拟结果；卷积 kernel 和切换状态由非音频线程准备，音频线程只采用已准备状态。实时 kernel 限定为每声道 4 个 1024-sample partition，超出部分通过 parametric late reverb 保留尾声语义。
- 最新证据为 Python `50/50`、UE 音频 Automation `23/23`、插件全量 Automation `38/38`、项目/插件完整构建和固定硬件运行门禁通过；hard-real-time 运行时统计为 `197` 次回调、容量违规 `0`、卷积预备丢弃 `0`。
- 尚不能关闭目标：目标设备 Human Pass 与移动穿墙、距离扫描、开放空间、靠墙位置的完整场景试听矩阵仍未完成。

## 执行顺序

1. 保持当前已经通过的 Direct、持续 Wet、A/B、Full 峰值、构建和 180 秒硬件门禁基线。
2. 在已打开的 Editor 中进入 PIE，用 F3 比较 Original 与 Realtime Full，并用 F1/F2/F5、WASD、F4、F8 完成人工交互试听。
3. 在 Bake 面板循环播放 Reference / Direct / Wet / Full，在目标设备上记录 Human Pass/Fail。
4. 若 Human Fail，保留输入、四 WAV、manifest、场景和设备记录，按失败现象回到 Direct、Wet 或切换平滑链路修复。
5. 完成移动穿墙、距离扫描、开放空间和靠墙位置矩阵，再做逐项最终完成审计。
6. 持续执行 hard-real-time 静态审计和运行时 `capacity_misses=0 / prepare_drops=0` 门禁，防止后续改动重新引入回调期锁、所有权或分配。

## 不变约束

- 不用合成短脉冲或仅 RMS 阈值替代真实输入与人耳判断。
- 四模式必须来自同一输入、同一起点、同一长度，并只使用一个公共安全缩放。
- 音频线程不得读取 UObject 或等待 RHI。
- 硬件路径及 fallback 原因必须可观察。
- 每个主要特性同步更新文档，并运行固定构建与运行时验证。

## Task 2：World-scoping 验证与后续计划

已完成：Listener 改为 per-World first-wins；acoustic scenes 改为稳定的 per-World heap ownership；Geometry/signature、Source/Bake、stale/directional IR 以及 Direct/Indirect hardware batches 按 World/scene 隔离；dead World state 在没有相关 in-flight scene reference 后清理。

已验证自动化基线：

1. `uv run script\build_and_validate.py` exit `0`；`31 functions / 32 bodies / 0 forbidden operations`；`Result: Succeeded`；`Build and validation complete.`
2. Focused NullRHI `UERayTracingAudio.Audio.ConfigurableDirect` 为 `3/3`、`0 failed`；日志 `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-WorldScoping-Final.log`。
3. Full NullRHI `UERayTracingAudio.Audio` 为 `26/26`、`0 failed`；日志 `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-Audio-Final.log`。
4. `uv run script\launch_runtime_validation.py` exit `0`；Direct/Indirect batches `4/4`，hardware/CPU paths `171/171`，gain `0.001625/0.001625`，data sources passed，kernels `2/2/4`，`non_finite=0`，hard realtime callbacks/misses/drops `169/0/0`。Game：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785425951315732900.log`；Editor：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785426132061345000.log`。

后续：

1. 在 final review 保留 P3 coverage follow-up；本轮 docs-only 修复不增加测试。
2. 执行真实 multi-PIE hardware isolation，补充当前 two-World NullRHI 与 single-World hardware runtime 证据。
3. 执行 Editor PIE/hardware listening 并记录 Human Pass/Fail。
4. 在上述人工门禁及既有总体验收项完成前，不宣称整个插件完成。

## Task 3: Frequency-dependent direct air absorption

Completed:

1. Added a complementary three-band, per-channel processor with project-configured crossovers and source-initialization-only allocation.
2. Integrated vector gain interpolation into the real occlusion callback while preserving broadband distance/occlusion, Wet mixing, and a non-allocating capacity fallback.
3. Added unity reconstruction, spectrum separation, stereo isolation, disabled-setting, prepared-capacity, and non-finite recovery coverage.
4. Extended the realtime audit to the new processor and callback reset/process helpers.
5. Verified the final bytes with the prescribed build, `50/50` Python tests, ConfigurableDirect `9/9`, Audio `32/32`, and the fixed hardware runtime. Spectrum ratio was `2.244426`; runtime hard-realtime callbacks/misses/drops were `179/0/0`.

Next:

1. Perform and record target-device Human Pass for the distance sweep, audible air absorption, and moving occlusion.
2. Perform the Task 2 multi-PIE hardware isolation follow-up; Task 3's fixed runtime evidence is single-World.
3. Keep automated metrics and manual listening conclusions distinct; do not claim overall plugin completion until the remaining acceptance gates pass.

## Task 4: Direct continuity diagnostics and runtime setters

Completed:

1. Added a separate lock-free Direct diagnostics epoch with bounded readers and a non-waiting single-writer CAS.
2. Measured Direct-only continuity, numeric safety, and per-sample band-gain steps before the callback adds Wet.
3. Added the approved Blueprint/C++ setters and invalidated baked caches without breaking monotonic convolution revisions.
4. Routed validation F1/F2/F5 and all internal validation data-source transitions through the public API.
5. Extended the callback source audit to `RecordDirectBuffer`.
6. Recorded RED (`9 passed / 2 failed`) and final GREEN evidence: build `47/47`, ConfigurableDirect `11/11`, Audio `34/34`, audit `36/37/1634` with zero violations, Python `50/50`.
7. Ran the exact fixed runtime flow successfully: paths `171/171`, data sources passed, callbacks/misses/drops `169/0/0`; the final Editor remains open.

Next:

1. Continue with the planned moving Direct sweep gate that consumes `FUERayTracingAudioDirectAudioStats`; this Task 4 runtime only proves no regression in the existing fixed hardware flow.
2. In PIE on target headphones/speakers, verify recognizable Direct content, F1/F2/F5 transitions, moving occlusion/air absorption, and no click/pop. Record Human Pass/Fail separately.
3. Retain the Task 2 multi-PIE hardware follow-up and do not claim overall completion from automated numeric evidence.

### Task 4 Fix Round 1

Completed:

1. Made Direct-diagnostics publication target-generation-safe across reset and A/B/A component switches without adding callback retries, waits, locks, or allocation.
2. Replaced the prepared-capacity fallback's constant target gain with an actual scalar per-frame ramp and made its diagnostic step match the gain applied to samples.
3. Preserved zero-frame and first-valid-snapshot semantics with focused regression coverage.
4. Recorded behavioral RED `10/12`, then final GREEN: build `47/47`, ConfigurableDirect `12/12`, Audio `35/35`, callback audit `39/40/1718` with zero violations, and Python `51/51`.
5. Ran the exact fixed runtime flow successfully: Direct/Indirect batches `4/4`, paths `171/171`, data sources passed, callbacks/misses/drops `176/0/0`; Editor PID `41028` remains open.

Next:

1. Continue the planned moving Direct sweep and target-device PIE listening; record click/pop and Human Pass/Fail separately.
2. Retain the Task 2 multi-PIE hardware isolation follow-up. The Fix Round 1 automated/runtime gates do not complete either manual acceptance item.

### Task 4 Fix Round 2

Completed:

1. Added deterministic reset-only coverage: capture a valid token, reset without changing target ID, reject the stale publication, then prove the sequence is reusable with one post-reset publication.
2. Made `ResetDirect` advance the diagnostic-context generation around its epoch change while preserving same-ID setter no-op and A/B/A behavior.
3. Recorded RED `12/13`, then final GREEN: build `47/47`, ConfigurableDirect `13/13`, Audio `36/36`, callback audit `39/40/1718` with zero violations, and Python `51/51`.
4. Ran the exact fixed runtime successfully: Direct/Indirect batches `4/4`, paths `171/171`, data sources passed, callbacks/misses/drops `190/0/0`; Editor PID `22952` remains open.

Next:

1. Continue the planned moving Direct sweep and target-device PIE listening; record click/pop and Human Pass/Fail separately.
2. Retain Task 2 multi-PIE hardware isolation and the three ledgered Task 4 Minors as separate follow-ups.

### Task 5: isolated hardware Direct sweep fixture

Completed:

1. Recorded behavioral RED against the newest fixed Game log and unit RED against the absent trajectory/metrics helper.
2. Added the deterministic 200 cm clear -> occluded -> clear quarter arc and pure pass/fail metrics under focused TDD.
3. Added the validation-only, non-reentrant automatic/F6 state machine, HUD observations, strict single terminal summary, and exact one-time restoration with a fresh post-restore Direct generation.
4. Guarded the implementation with `WITH_UERAYTRACINGAUDIO_VALIDATION` and the validation-scenario gate; Shipping and ordinary projects remain inert.
5. Closed review findings with a pure owner/hardware/deadline policy: one World owns process-global diagnostics, hardware wait terminates once, and accepted CPU fallback invalidates the sweep. Non-owner/contention checks precede F6 Pawn motion and only the owner renders the HUD.
6. Passed the final prescribed build (`48/48`), hard-realtime audit (`39/40`, zero forbidden operations), and ConfigurableDirect (`15/15`).

Next:

1. Task 6 has added `-UERayTracingAudioValidationDirectSweep`, the exact strict terminal parser, and passing Game/Editor evidence.
2. Record target-device PIE Human Pass/Fail for audible recovery and click/pop absence. Automated numeric evidence does not close this gate.
3. Retain Task 2 multi-PIE hardware isolation and the ledgered Task 4 Minors as separate follow-ups.

### Task 6: fixed-launcher Direct sweep parser gate

Completed:

1. Added the automatic sweep flag to fixed Game commands only and a strict exactly-one marker parser with all approved thresholds, non-finite rejection, and combined failure reasons.
2. Made a passing hardware Direct sweep a prerequisite for accepting the existing Baked/Realtime/Hybrid and hard-real-time evidence.
3. Recorded Python RED (`42` tests, `16` expected failures), then Python GREEN `58/58` and callback audit `39/40/1718` with zero forbidden operations.
4. Followed crash-debugging when the first integrated run exposed Source-motion/CPU-reference temporal mismatch. Added a pure baseline-admission RED (C2660), required `bResultLogged`, and passed build `48/48` plus ConfigurableDirect `15/15`.
5. Ran the exact fixed runtime successfully: baseline paths `171/171`; Direct sweep `passed=1` with `213` generations, fixed `200 cm`, endpoint visibility, nonzero gain, bounded step, no dropout, restoration, and hardware provenance; data sources and `1583/0/0` hard-realtime counters passed. Editor PID `35468` remains open.

Next:

1. Continue Task 7 Editor controls and fixture maintenance from the approved plan.
2. Record target-device PIE Human Pass/Fail for Direct recovery and click/pop absence.
3. Retain Task 2 multi-PIE hardware isolation and the ledgered Task 4 Minors as separate follow-ups.

### Task 6 Fix Round 1

Completed:

1. Added RED coverage for all non-physical/inverted visibility and gain ranges, negative gain step, and each later IR marker preceding the Direct terminal (`12` intended failures out of `47` focused tests).
2. Enforced normalized ordered extrema and a non-negative bounded step with explicit combined errors.
3. Required data-source Bake-start evidence and enforced `Direct terminal -> Bake start -> later runtime evidence` at the Direct gate.
4. Passed focused RuntimeValidation `47/47`, full Python `62/62`, callback audit `39/40/1718` with zero violations, and the previous real-log parser check.
5. Ran the exact fixed launcher successfully. Game ordering was Direct `1118`, Bake `1126`, hard realtime `1130`, final data source `1132`; Direct and all existing runtime gates passed. Editor PID `42300` remains open.

Next:

1. Continue Task 7 from the approved plan.
2. Record target-device Human Pass/Fail; retain multi-PIE and Task 4 Minor follow-ups.

### Task 7: Editor Direct fixture controls

Completed:

1. Added transient Editor-World RED/GREEN coverage for `100 / 200 / 400 cm`, Off/Default/Stress, tagged Source reuse, stale tagged Geometry destruction, tagged non-geometry preservation, and untagged Source/Geometry preservation.
2. Added transactional tagged-only fixture mutation and exact air-profile values. Environment switches now remove obsolete tagged Geometry actors rather than only changing `GeometryCount`.
3. Added the required Slate distance and air rows, effective fixture summary, `Validation fixture only`, and Bake/offline-render disable state.
4. Added Editor-only CLI plumbing and an exactly-one strict scene-ready parser that validates Direct preset, fixed enclosed environment, effective distance, profile, and vector.
5. Passed build `48/48`, focused Editor `1/1`, full Automation `54/54`, focused Python `2/2`, full Python `64/64`, audit `39/40/1718`, and the exact Game/Editor runtime launcher. Editor PID `34840` remains open at `200 cm / default`.

Next:

1. Execute Task 8 documentation, Shipping isolation, and final verification from the approved plan.
2. Record target-device Human Pass/Fail for distance, air absorption, moving Direct recovery, click/pop, and R3 reflection environments.
3. Retain Task 2 multi-PIE hardware isolation and the ledgered Task 4 Minors as explicit follow-ups.

### Task 7 Fix Round 1

Completed:

1. Removed implicit adoption of an arbitrary untagged Source from panel initialization and `StartBake`; automatic lookup is restricted to the tagged fixture role, while **Use Selected Source** remains explicit opt-in.
2. Centralized persistent instance-component creation so the owner is modified before component construction/addition, creation notification, and registration. Added full Undo/Redo/dirty assertions and an independent Audio-only mutation test.
3. Made the managed transient Editor World allocate its context only after World creation succeeds, then destroy World/context and collect garbage. Added injected-failure context-count and weak-lifetime coverage.
4. Recorded three attributable REDs: missing pre-construction owner modification, failed-World context leakage, and a targeted Audio-owner mutation. Restored final production code with no mutation bypass.
5. Passed final build `48/48`, focused Editor `2/2`, full Automation `55/55`, Python `64/64`, audit `39/40/1718`, and the exact launcher. Direct/data-source/HardRT gates passed; Editor PID `13344` remains open at `200 cm / default`.

Next:

1. Keep Task 8 work separate from this review-fix round.
2. Record target-device Human Pass/Fail for distance, air absorption, moving Direct recovery, click/pop, and R3 reflection environments.
3. Retain Task 2 multi-PIE hardware isolation and the ledgered Task 4 Minors as explicit follow-ups.

### Task 8: documentation, Shipping isolation, and final technical verification

Completed:

1. Closed the five-document technical record for product settings, per-Source and per-World behavior, complementary frequency-dependent Direct processing, Wet bypass, validation-scenario isolation, F6 phases/marker, and Editor 1/2/4 m plus Off/Default/Stress controls.
2. Passed the final static gates: callback audit `39/40/1718` with all forbidden-operation counts `0`, Python `64/64`, and whitespace validation.
3. Passed the prescribed Development build, then fresh NullRHI Automation at ConfigurableDirect `15/15`, Audio `38/38`, and full plugin `55/55`, all with zero failed tests.
4. Passed the prescribed Shipping build and proved `WITH_UERAYTRACINGAUDIO_VALIDATION=0`; exact executable/receipt scans contain no validation entry, HUD, F6, or strict-marker strings, while the receipt and Direct/Indirect/SDK artifacts retain the normal runtime.
5. Passed the exact launcher and extra raw-log gates: both non-fallback hardware markers, one passing strict Direct marker, `200 cm`, clear/occluded visibility, positive soft gain, bounded step, zero dropout, restored Source, continuous three-mode Wet, and hard realtime `1600/0/0`. Editor PID `10276` remains open and ready.

Next:

1. The user enters PIE, waits for hardware/data-source gates, presses F8, then uses F3/F6 and records target-device Human Pass/Fail for recovery, restart, click/pop, silence/timing, moving occlusion, and audible distance/air differences.
2. Run true multi-PIE hardware isolation and the OpenSpace/NearWall/Enclosed R3 matrix; neither automated R2 evidence nor fixture telemetry closes them.
3. Triage the enumerated Task 1-4 and Task 7 deferred technical items. Do not declare the overall plugin complete while these manual/R3 gates remain open.
