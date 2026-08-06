# Progress Log

## 2026-08-06 - HumanAcceptance schema-v3 结构化人工门禁

- 发现历史 HumanAcceptance PASS 缺少目标设备、完整来源和四模式试听证据，不能证明最终人工验收；未删除或冒充这些历史记录。
- Editor 新增必填目标监听设备、可选备注、按 artifact 重置的试听进度，以及 Direct 保真、Wet/Full 差异、移动遮挡、模式切换、环境差异五项人工确认；Human Pass 仅在四模式均成功启动且 `Confirmed=5/5` 后启用，Human Fail 可在首个问题后提前记录。
- 新记录使用 schema v3，并从 comparison manifest 固化输入/场景/预设/环境和音频指标，另存五项结构化确认；严格 Python 校验器拒绝旧 schema、空设备、缺模式、缺/假确认和来源不一致。
- 规定构建通过；Python `175/175`、HumanAcceptance Automation `5/5`、插件全量 Automation `81/81`、音频回调审计 `39/40/0` 均通过。最终固定硬件运行 Direct `223` generations、三种 IR Wet `24/24`、hard realtime `1578/0/0`、交互 smoke 通过；Game/Editor 日志为 `UERayTracingAudioValidation-Game-1786018541593742800.log` / `UERayTracingAudioValidation-Editor-1786018587178540700.log`，A/B Editor PID `12672` 已保留。
- 可见截图 `Saved/Validation/human-acceptance-schema-v3.png` 已确认设备/备注、五项确认、`Previewed=none`、`Confirmed=0/5`、四轨波形和未试听时禁用的 verdict；仍未完成目标耳机/扬声器 Human Pass，五项确认未由代理代勾。

## 2026-08-06 - 当前工作树 32-bounce 环境矩阵

- 运行 `uv run script\validate_reflection_environment_matrix.py`，OpenSpace / NearWall / Enclosed 三个独立硬件 Editor case 全部通过；汇总为 `TestProject/UeVersion1/Saved/UERayTracingAudio/ListeningAcceptance/ReflectionEnvironmentMatrix/20260806-192007/ReflectionEnvironmentMatrix_Manifest.json`。
- 有效路径为 `0/618/20351`，Wet/Reference RMS 为 `0/0.219827/0.567596`；OpenSpace Wet 精确静音，NearWall 有方向性早期反射，Enclosed early 更强且 late reverb 非零。硬件/CPU reference 一致性门禁通过。
- 独立核对 12/12 WAV 的 SHA-256 与 manifest 一致，格式均为 16 kHz / stereo / PCM16 / 144006 帧；目视三张 `Waveforms=ready` 截图确认 Wet 波形由 OpenSpace 直线、NearWall 小幅到 Enclosed 明显增大。
- 矩阵后固定交互 smoke 退出 `0`：Direct `212` generations、dropout `0`，三种 IR Wet `24/24`，hard realtime `1589/0/0`，F4 即时误差 `0 cm`；Game/Editor 日志分别为 `UERayTracingAudioValidation-Game-1786015496645186200.log` 和 `UERayTracingAudioValidation-Editor-1786015542200797800.log`，Editor PID `44260` 已保留。
- 该结果刷新 T2 自动证据，不代表目标耳机/扬声器 Human Pass；环境听感、移动遮挡和 click/pop 仍待人工结论。

## 2026-08-06 - 交互 F4 测量闭环

- 交互 smoke 复现间歇性 `origin_error_cm=380.551`。按 `workflow/crash-debugging.md` 检查确认无 crash；Direct/Indirect、三种 IR、音频链路和 hard realtime 均已通过，唯一失败是 F4 原点检查。
- 诊断日志证明 F4 phase 4 放置即时误差为 `0 cm`。根因是旧 smoke 到 17 秒 A/B 观察结束才读取 Pawn 位置，把窗口内合法 WASD 输入误当作 F4 漂移。
- RED 要求严格 marker 接受“即时返回成功、之后继续移动”。实现把门禁字段改为 phase 4 立即采样的 `origin_return_error_cm`，后续位移只记录为 `post_return_moved_cm`；完整 Python `163/163` 转为 GREEN。
- 受控硬件复验在 F4 后注入 800 ms 前进输入，结果为 `origin_return_error_cm=0.000`、`post_return_moved_cm=50.900`、smoke `passed=1`。日志：`TestProject/UeVersion1/Saved/Logs/UERayTracingAudioValidation-Game-1786014660179327100.log`。
- 规定构建通过，音频回调审计 `39/40/0`；无参数固定运行 Direct `209` generations、三种 IR Wet `24/24`、hard realtime `1581/0/0`；插件全量 Automation `76/76`。仍未完成目标耳机/扬声器 Human Pass 与 click/pop 人工结论。

## 2026-08-06 - 可见 A/B 波形闭环

- 盘点并验证当前工作树：Python 初始 `161/161`、ConfigurableDirect `23/23`、插件全量 Automation `76/76`、UE 5.7 项目/插件构建和固定硬件运行均通过。
- 重跑真实 `MarchingBand` Clear/Soft/Hard 等距配对：Reference SHA-256 一致，距离/衰减差为 `0`，Direct gain 为 `0.499334/0.174767/0`，配对 manifest 为 `TestProject/UeVersion1/Saved/UERayTracingAudio/ListeningAcceptance/DirectOcclusionPair/20260806-174518/DirectOcclusionPair_Manifest.json`。
- 目视截图发现 artifact runner 导入四个资产后仍显示空波形。RED 证明旧门禁会误接收缺失 `waveforms=ready` 的 UI；根因是 `AdoptValidationArtifactResult()` 未调用现有 waveform loader。
- 修复 artifact adoption，使四条 PCM16 envelope 装入可见面板后才成功，并把同一 manifest 的 `waveforms=ready` 加入严格 Python 门禁。新截图 `Saved/Validation/direct-clear-waveforms-ready.png` 已目视确认灰/蓝/紫/绿四条波形实际绘制。
- 首次重建因固定脚本留下的测试工程 Editor 启用 Live Coding 而被 UBT 拒绝；按 `workflow/crash-debugging.md` 确认无新 crash，只关闭已核对命令行的 PID 后重跑成功。
- 最终验证：Python `162/162`、插件 Automation `76/76`、音频审计 `39 functions / 40 bodies / 0 forbidden operations`、规定构建通过。固定硬件运行 Direct sweep `214` generations、dropout `0`，三种 IR Wet 均 `24/24`，hard realtime `1589/0/0`。
- 未完成：目标耳机/扬声器 Human Pass，以及移动穿墙、模式切换 click/pop、开放空间/靠墙的人工试听结论。

## 2026-07-30

- 完成 hard-real-time 收敛：模拟快照改为 lock-free 生命周期；卷积 kernel/crossfade 状态改为非音频线程 Prepare、音频线程 adopt；Bridge 使用有界轮转服务和预备池切换余量。
- 为实时卷积设置每声道 `4 × 1024 = 4096` 样本上限，保留完整 Baked/离线 IR，并把截断尾声能量交给 parametric late reverb。
- 新增源码审计和运行时计数门禁。审计通过：31 个函数、32 个函数体、1437 行，锁/heap/共享所有权/阻塞/UObject 违规均为 0；运行时为 `callbacks=197`、`capacity_misses=0`、`prepare_drops=0`。
- Automation 改走生产 Prepared Renderer 路径，并新增实时卷积预算、512 lane 切换 headroom、公平服务和 hard-real-time 诊断回归。
- Python `50/50`、UE 音频 Automation `23/23`、插件全量 Automation `38/38`、规定的 UE 5.7 项目/插件构建和固定运行时验证全部通过；Editor 已由固定脚本打开。
- 修复 Original/Rendered 启动与循环：两条 `MarchingBand` 链路从采样零点同时启动，硬件快照就绪前保持 Original 可听；Rendered 只在有效 Direct/Indirect/左右 Realtime kernel 后淡入。17 秒多循环 smoke 的 `ab_restart_count=0`。
- 修复 Direct 首快照与移除渐变，Soft Occlusion 持续非零；Realtime/Baked/Hybrid 使用实际 `2/2/4` 左右 kernel，持续 Wet 门禁通过。
- 明确 `IndirectMix` 为 Wet Send：`Full = Direct + WetSend × Wet`；新 Source、Editor 验收场景和离线请求默认 `1.0`，运行时主夹具使用 `1.75`。Bake 启动时冻结 Source、Listener、输入 SoundWave、Actor 路径和 Wet Send。
- 为最终 Full 增加峰值/越界诊断。先用 C++ RED 证明 Spatialization 后峰值可高于 Occlusion 前值，再覆盖双声道 `MarchingBand` 绕过 Spatialization 的 fallback；两条回归转为 GREEN。
- 首次 180 秒运行因 F4 传送后 Pawn 残余速度导致原点漂移 `170.581 cm` 而失败；清空移动输入并立即停止 MovementComponent 后重跑为 `0.000 cm`。
- Python `44/44`、UE 音频 Automation `13/13`、规定的 UE 5.7 项目/插件完整构建通过。
- 180 秒硬件运行门禁通过：4 Source Direct/Indirect batch，GPU/CPU 路径 `171/171`、增益 `0.001625/0.001625`；Baked/Realtime/Hybrid Full 峰值 `0.032985/0.033651/0.114308`，越界 `0/0/0`，非有限样本 `0`，Wet 持续覆盖 `24/24`、`57/57`、`24/24`。
- 自动交互 smoke 通过：移动 `50.900 cm`、Listener/相机误差 `0 cm`、F4 原点误差 `0 cm`、A/B 重启 `0`、外部播放声源 `0`。固定脚本已重新打开 Editor，等待人工 PIE 与 Human A/B。
- 未完成：目标设备 Human Pass，以及移动穿墙/距离/开放空间/靠墙试听矩阵。

## 2026-07-29 - Skill

- Installed the official reference skills `security-best-practices` and `gh-address-comments`.
- Created, validated, and installed `unreal-cpp-code-review` for engineering C++, Unreal, real-time audio, RHI, and hardware ray tracing.

## 2026-07-29

- 确认“Direct 只响一下”不是单一问题：运行时 AudioComponent 始终播放，但首个快照后被约 6 m、穿墙和错误的 `1/r²` 样本增益压到近静音；Editor Direct 则是未循环的一次性 Preview。
- Direct 距离项改为线性声压 `1/r`；运行时 Clear / Soft / Hard 改为等距 2 m，并新增 1 m / 2 m / 4 m 持续非零自动化。
- Editor Reference / Direct / Wet / Full 改为循环 Preview，并跟踪实际 Preview AudioComponent 的 Stop、替换和销毁状态。
- Realtime 卷积切换增加单槽 last-wins pending kernel，保留当前卷积历史、预热、交叉淡化和尾音；新增高频 kernel churn 回归。
- Baked 检查后恢复 `HybridReverb`，Realtime / Hybrid 要求 parametric late tail；持续 Wet 门禁改为至少 24 个输入有效缓冲、80% Wet presence、最长 20% 完全静音段，并要求窗口积分 Wet/Input RMS `>= 5%`（最大比例亦 `>= 5%`）。
- 新增窗口积分 Wet RMS、真正静音连续段和高频 kernel churn 回归；Python 脚本回归 `41/41` 通过。UE 构建和固定硬件运行时验证待本轮继续执行。
- 确认原蓝/橙实时房间使用固定 Camera，Player Pawn 和 Listener 未绑定，因此实时计算成立但无法移动。
- 实现固定/交互双模式：First Person Pawn 驱动 Listener；当前按键为 F1/F2/F5 切换 Realtime/Baked/Hybrid、F3 切换 Rendered/Original、F4 返回烘焙原点、F8 切换视角；主屏显示当前模式、A/B 播放和 Baked 资产状态。
- `launch_runtime_validation.py` 新增 `--interactive-runtime`；该流程不生成静态 Editor A/B 夹具，避免把 Bake 场景误认为实时目标。
- Python 测试更新为 36/36 通过；UE 5.7 项目和插件完整构建通过。
- 首次 60 秒运行时验证因本地测试工程冷启动耗时 56.6 秒而未等到最终标记；日志无 crash、异常或断言。按 crash-debugging 流程检查后使用 120 秒窗口重跑通过。
- 固定运行时硬件门禁通过：4 Source Direct/Indirect、GPU/CPU 对照、Realtime/Baked/Hybrid、真实 MarchingBand 输入与实际非静音音量均有效。
- 交互 Editor readiness 通过并保持 Editor 打开：`static_bake_fixture=0`；待用户进入 PIE 人工确认移动、快捷键和听感。
- 实时验证新增同步双链路试听：主链路使用硬件光追 Direct+Wet，参考链路使用同一 SoundWave 但关闭空间化/遮挡/间接声；F3 用 50 ms 交叉淡变切换，Hybrid 移至 F5，主屏显示 `A/B PLAYBACK`。
- 审计当前工作树、TODO、用法文档和 2026-07-26 验证日志。
- 确认核心 Direct/Indirect/Bake/A/B 实现已存在，当前最终硬门槛是目标设备 Human Pass。
- 发现并记录状态文档缺失：恢复 `IMPLEMENTATION_STATUS.md`，新增 `plan.md` 与本进度日志。
- 开始实现 Editor Reference/Direct/Wet/Full 四轨同时间轴、同 full-scale 波形对比；波形直接读取本次离线渲染的 PCM16 WAV，不使用合成或模式独立归一化。
- 按要求从本地 UE 5.7 测试项目移除 `ravel_-_condition.uasset`、引用它的旧 World Partition `AudioActor`，并清理可再生 Asset Registry 缓存；`Content/FirstPerson/Audio` 现在只保留 `MarchingBand.uasset`，Content 内无 Ravel 引用。
- 修正验证工程自动发现：默认只选择仓库内 `TestProject/**/*.uproject`，避免同步或构建工作区外的同名工程；对应 Python 回归通过。
- 本地 `TestProject/UeVersion1` 完整构建通过；自动交互 smoke 通过，Pawn 移动 `250.901 cm`，Listener/相机与 F4 原点误差均为 `0 cm`，三种 IR、F3 双链路和 F8 双视角均有效。
- `--interactive-runtime` Editor readiness 通过，Editor 已保持打开，等待人工 PIE 移动、F3 听感和 click/pop 验收。
- 待办：Editor PIE 移动/快捷键、波形可见性和目标设备人耳验收。
## 2026-07-30 - Configurable Direct Audio Task 2

- Added transient two-World RED coverage for listener and acoustic-geometry isolation; both failed for the expected global-state reason.
- Replaced global listener/scene state with World-keyed weak listener entries and stable heap-owned acoustic scene states.
- Propagated Source/Bake Worlds through direct, indirect, stale-asset, directional-IR, and hardware batching paths; partitioned direct and indirect batches by scene.
- Final verification passed: prescribed UE 5.7 project/plugin build, ConfigurableDirect 3/3, Audio 26/26, and fixed runtime Game/Editor validation.
- Created isolated commit `d0a194f` while preserving authoritative baseline-dependent working changes.

## 2026-07-31 - Task 2 Documentation Fix Round 1

- Documented per-World first-wins listeners, stable per-World acoustic scenes, Source/Bake World propagation, per-scene Direct/Indirect batching, and dead-World cleanup in all repository-required status/usage/planning files.
- Recorded verified automated evidence: `uv run script\build_and_validate.py` exit `0` (`31 functions / 32 bodies / 0 forbidden operations`, `Result: Succeeded`); focused NullRHI `3/3` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-WorldScoping-Final.log`; full Audio NullRHI `26/26` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-Audio-Final.log`.
- Recorded fixed runtime exit `0`: Direct/Indirect batches `4/4`, hardware/CPU paths `171/171`, gain `0.001625/0.001625`, data sources passed, kernels `2/2/4`, `non_finite=0`, and hard realtime callbacks/misses/drops `169/0/0`. Logs: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785425951315732900.log` and `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785426132061345000.log`.
- Scope remains explicit: two-World isolation is verified under NullRHI, hardware runtime is single-World, multi-PIE hardware and Human Pass are unperformed, P3 coverage remains ledgered for final review, and the overall plugin is not complete.

## 2026-07-31 - Configurable Direct Audio Task 3

- RED proved the scalar defect: equal-RMS `100 Hz` and `10 kHz` inputs produced low/high ratio `1.000000`, failing the required `>= 2` spectrum separation.
- Added preallocated per-source, per-channel complementary three-band air absorption; cached project crossovers flow through module, factory, plugin, and source initialization. The callback interpolates one gain vector per frame, preserves Wet mixing, and uses a non-allocating broadband fallback with one capacity-miss record per buffer.
- Independent Unreal review found that a non-finite input could poison persistent filter state. A focused RED reproduced it; a finite guard before state mutation fixed it without callback allocation.
- Final verification passed: prescribed build `47/47`; static audit `35 functions / 36 bodies / 1510 lines / 0 forbidden operations`; Python `50/50`; ConfigurableDirect `9/9`; full Audio `32/32`; spectrum ratio `2.244426`.
- Fixed hardware runtime passed: Direct/Indirect batches `4/4`, paths `171/171`, gain `0.001625/0.001625`, data sources passed, kernels `2/2/4`, `non_finite=0`, and callbacks/misses/drops `179/0/0`. Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785461920592378400.log`; Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785462101170308900.log`.
- The Editor is left running for manual validation. Target-device Human Pass, distance/air/moving-occlusion listening, and Task 2 multi-PIE hardware isolation remain open; automated evidence is not Human Pass.

## 2026-07-31 - Configurable Direct Audio Task 4

- RED passed the build and failed only the two intended contracts: Direct diagnostics API missing and both runtime setter UFUNCTIONs absent (`9 passed / 2 failed`).
- Added lock-free, epoch-consistent Direct continuity diagnostics and measured Direct samples before Wet mixing. The callback skips a busy writer and performs no allocation, wait, logging, UObject access, or shared-ownership change.
- Added Blueprint-callable runtime data-source and baked-asset setters; validation F1/F2/F5 and internal phases now use them. Baked replacement invalidates cached kernels while keeping published lane revisions monotonic.
- Final verification passed: build `47/47`; ConfigurableDirect `11/11`; Audio `34/34`; callback audit `36 functions / 37 bodies / 1634 lines / 0 violations`; Python `50/50`.
- Fixed hardware runtime passed with paths `171/171`, all data sources ready, and callbacks/misses/drops `169/0/0`. Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785466094540462400.log`; Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785466275542564200.log`.
- The Editor is left running. Target-device PIE Human Pass, moving Direct sweep/listening, click/pop confirmation, and Task 2 multi-PIE hardware isolation remain open; automated metrics are not Human Pass.

## 2026-07-31 - Configurable Direct Audio Task 4 Fix Round 1

- RED reproduced both review defects: an old target token could publish into a reset/A/B/A Direct-diagnostics epoch, and the unsupported-channel fallback jumped directly to its target while reporting an interpolated band step (`10 passed / 2 failed`).
- Added target generations with callback-captured plain tokens and publication-time revalidation. Stale writers leave the sequence readable and cannot expose counters in the current target generation.
- Added scalar broadband history and per-frame fallback interpolation; diagnostics now measure the scalar step actually applied. Zero-frame and first-valid-snapshot behavior have dedicated regressions.
- Final verification passed: prescribed build `47/47`; ConfigurableDirect `12/12`; Audio `35/35`; callback audit `39 functions / 40 bodies / 1718 lines / 0 violations`; Python `51/51`.
- Fixed hardware runtime passed: Direct/Indirect batches `4/4`, paths `171/171`, gain `0.001625/0.001625`, data sources passed, kernels `2/2/4`, `non_finite=0`, and callbacks/misses/drops `176/0/0`. Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785468883156707700.log`; Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785469063951366500.log`.
- Editor PID `41028` was left running for that historical run and is superseded by PID `500`. Moving Direct sweep, target-device Human Pass/click-pop listening, and Task 2 multi-PIE hardware isolation remain open.

## 2026-07-31 - Configurable Direct Audio Task 4 Fix Round 2

- RED reproduced the reset-only race: a token captured for the current component remained valid after `ResetDirect`, published one stale buffer into the new epoch, and made the valid follow-up total two buffers (`12 passed / 1 failed`).
- `ResetDirect` now brackets its epoch advance with the existing odd/even diagnostic-context generation while keeping the selected component ID unchanged. Same-ID target-set no-op, A/B/A rejection, sequence recovery, and bounded callback behavior are preserved.
- Final verification passed: prescribed build `47/47`; ConfigurableDirect `13/13`; Audio `36/36`; callback audit `39 functions / 40 bodies / 1718 lines / 0 violations`; Python `51/51`.
- Fixed hardware runtime passed: Direct/Indirect batches `4/4`, paths `171/171`, gain `0.001625/0.001625`, data sources passed, kernels `2/2/4`, `non_finite=0`, and callbacks/misses/drops `190/0/0`. Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785470614600787600.log`; Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785470795487644500.log`.
- Editor PID `22952` was left running for that historical run and is superseded by PID `500`. Moving Direct sweep, target-device Human Pass/click-pop listening, Task 2 multi-PIE hardware isolation, and the ledgered Task 4 Minors remain open.

## 2026-07-31 - Configurable Direct Audio Task 5

- Behavioral RED: the newest fixed Game log lacked exact marker `UERayTracingAudio direct sweep: passed=1`; the check exited `1`.
- Unit RED: the new tests failed at the intended missing seam, `Validation/UERayTracingAudioDirectSweep.h`.
- Added the pure 200 cm quarter-arc trajectory/metrics helper and a single automatic/F6 runtime sweep state machine with HUD observations, one terminal summary, and exact one-time configuration restoration.
- Added non-Shipping compile isolation and validation-scenario/command-line gates; the hard-realtime callback path was not changed.
- Review-fix RED failed on the intentionally absent owner/hardware/deadline policy (`C2653`/`C3861`). The implemented policy bounds no-hardware wait, accepts hardware evidence only after warmup, rejects any accepted CPU fallback, and gives one World exclusive process diagnostics/sweep/HUD ownership.
- Final verification passed: prescribed build `48/48`; callback audit `39 functions / 40 bodies / 0 forbidden operations`; ConfigurableDirect `15/15` at `D:\Labs\2602-unreal\ue-audio-plugin\.worktrees\configurable-direct-audio-validation\TestProject\UeVersion1\Saved\Logs\Task5-FINAL-ConfigurableDirect.log`.
- The Task 6 launcher flag/parser and hardware terminal-marker evidence are pending. Human Pass for target-device recovery and click/pop absence is pending.

## 2026-07-31 - Configurable Direct Audio Task 6

- Python RED produced `16` expected failures: the Game flag and strict Direct-sweep parser/gate were absent.
- Added the Game-only automatic flag, exactly-one strict parser, finite/threshold/restoration/hardware checks, and Direct-first data-source gating. Full Python passed `58/58`; callback audit remained `39/40/1718` with zero violations.
- The first runtime run failed the existing CPU-reference gate without a crash. Crash-workflow evidence found automatic Source motion preceding baseline CPU comparison (`171` hardware paths versus `953` CPU paths).
- Added a pure baseline-admission regression (RED C2660) and required baseline result logging before sweep movement. Final build passed `48/48`; ConfigurableDirect passed `15/15`.
- Final fixed runtime passed: baseline `171/171`; Direct sweep `213` generations at `200 cm`, endpoint visibility, nonzero gain, bounded step, no dropout, restored/hardware; data sources passed; callbacks/misses/drops `1583/0/0`.
- Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785475390548759300.log`; Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785475571210643500.log`; Editor PID `35468` responded for that historical run and is superseded by PID `500`.
- Target-device Human Pass/click-pop listening, Task 2 multi-PIE hardware isolation, and the ledgered Task 4 Minors remain open.

## 2026-07-31 - Configurable Direct Audio Task 6 Fix Round 1

- RED: focused RuntimeValidation ran `47` tests with `12` intended failures for out-of-domain/inverted extrema, negative gain step, and Direct evidence occurring too late.
- Enforced ordered normalized visibility/gain domains and a non-negative bounded gain step. Added required data-source Bake evidence and Direct-before-Bake/HardRealtime/DataSource ordering.
- GREEN: focused `47/47`, full Python `62/62`, callback audit `39/40/1718` with zero violations, and the previous real Game log passed the hardened parser.
- Exact runtime passed: baseline `171/171`; Direct `214` generations at `200 cm`, visibility `0.000038-0.997131`, gain `0.174779-0.498402`, step `0.00005548`, no dropout, restored/hardware; data sources passed; hard realtime `1580/0/0`.
- Ordered lines: Direct `1118`, Bake `1126`, hard realtime `1130`, final data source `1132`.
- Game: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785476653651929300.log`; Editor: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785476834237184600.log`; Editor PID `42300` responded for that historical run and is superseded by PID `500`.
- Human Pass, multi-PIE hardware isolation, and Task 4 Minors remain open.

## 2026-07-31 - Configurable Direct Audio Task 7

- C++ RED first failed on the missing air-profile seam; behavioral RED then showed Off/Stress unchanged and seven stale Enclosed geometry actors surviving OpenSpace. Python RED showed missing CLI/parser seams.
- Added exact 1/2/4 m and Off/Default/Stress tagged-fixture controls, transactional stale tagged Geometry deletion, Slate controls/disable state, and effective fixture telemetry. Untagged Source/Geometry and tagged non-geometry actors are preserved.
- Added Editor-only CLI parameters and an exactly-one strict scene marker parser with Direct preset, enclosed environment, distance, profile, and vector validation.
- Final verification passed: build `48/48`; focused Editor `1/1`; full Automation `54/54`; focused/full Python `2/2` and `64/64`; callback audit `39/40/1718` with zero violations.
- Exact runtime passed: Direct `206` generations; data sources passed; hard realtime `1584/0/0`. Game `UERayTracingAudioValidation-Game-1785478813724452300.log`; Editor `UERayTracingAudioValidation-Editor-1785478994289043000.log`.
- Editor PID `34840` was open at `200 cm / default / (0.0002,0.0006,0.0012)` for that historical run and is superseded by PID `500`. Human Pass, R3 listening, multi-PIE hardware isolation, and Task 4 Minors remain open.

## 2026-07-31 - Configurable Direct Audio Task 7 Fix Round 1

- Restricted automatic Source discovery to the tagged validation Source and removed `StartBake`'s implicit generic-selection fallback; ordinary Sources now require **Use Selected Source**.
- Added owner-before-component creation for every persistent fixture instance component, plus Undo/Redo/dirty checks. A pre-existing-Source Audio-only scenario catches removal of the Audio owner mutation independently.
- Added managed test-World context failure coverage and complete World/context/GC + weak-lifetime assertions. Final focused/full logs have zero `World has no context!` warnings.
- RED: original owner-order regression; injected failed-World context count `1 -> 2`; targeted Audio mutation. GREEN: build `48/48`, focused `2/2`, full Automation `55/55`, Python `64/64`, audit `39/40/1718` with zero forbidden operations.
- Exact launcher exit `0`: Direct `231` generations, all data sources passed, hard realtime `1580/0/0`; Game `UERayTracingAudioValidation-Game-1785491460999842900.log`, Editor `UERayTracingAudioValidation-Editor-1785491641704854400.log`.
- Editor PID `13344` responded at `200 cm / default / (0.0002,0.0006,0.0012)` for that historical run and is superseded by PID `500`. Human Pass, R3 listening, multi-PIE hardware isolation, and Task 4 Minors remain open.

## 2026-07-31 - Configurable Direct Audio Task 8

- Verified the assigned worktree at base `6d0a10c22f9d09978bdb0545630589fe657efdfd` and retired only legacy Editor PID `13344` before building.
- Static gates passed: realtime audit `39 functions / 40 bodies / 1718 lines`, all forbidden-operation counts `0`; Python `64/64`; initial `git diff --check` exit `0`.
- Prescribed Development build exited `0` (`48/48` plugin actions). Fresh NullRHI Automation passed ConfigurableDirect `15/15`, Audio `38/38`, and full plugin `55/55`, with zero failed tests and unique absolute logs.
- Prescribed Shipping build exited `0`. Shipping macro is `WITH_UERAYTRACINGAUDIO_VALIDATION=0`; executable/receipt string hits for scenario/HUD/F6/strict entry points were all `0`; receipt and Direct/Indirect/SDK artifacts prove the normal runtime remains built.
- Exact launcher exited `0`: non-fallback Direct/Indirect markers each `1`; strict sweep passed at `200.000 cm`, visibility `0.000038-0.996866`, gain `0.174779-0.498316`, step `0.00005484`, dropout `0`, restored/hardware `1/1`; three data sources had continuous `24/24` Wet and hard realtime was `1600/0/0`.
- Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785493624391644500.log`; Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785493804953148700.log`; Editor PID `10276` responded and remained open for that historical run, and is superseded by PID `500`.
- At that historical checkpoint, user-performed Human Pass, moving-player/moving-occlusion and audible distance/air/click-pop checks, true multi-PIE hardware isolation, automated OpenSpace/NearWall/Enclosed R3, the enumerated Task 1-4 minors, and Task 7 duplicate tagged-Source ambiguity were open; later entries supersede the automated R3 and ambiguity status.

## 2026-07-31 - Final Whole-Branch Fix Wave

- Closed the final review's one P1 and twelve P2 findings: synchronized/cancellable RHI query lifetime, Direct distance and actual-hardware semantics, Listener-loss invalidation, finite Wet state, bounded snapshot/convolver reclamation, artifact contract, side-effect-free Bake admission, sequential PIE ownership, deterministic fixture normalization, and strict launcher evidence.
- RED/GREEN caught two integration defects beyond focused tests: full Automation exposed retired-slot accounting in snapshot reclamation, and the first artifact flow exposed a partial marker-read race plus a Full/Dry correlation failure. Bounded current-entry reclaim, complete-line readiness, and fixture-only Wet send `0.8` fixed them without relaxing thresholds.
- Final static gates passed: audit `39 functions / 40 bodies / 1775 lines / 0 forbidden`; Python `70/70`; `git diff --check` exit `0`.
- Final Development build passed. Focused Automation passed ConfigurableDirect `15/15`, Audio `40/40`, Editor `9/9`; full plugin Automation passed `62/62`.
- Final Shipping build passed `34/34`; macro `WITH_UERAYTRACINGAUDIO_VALIDATION=0`, strict validation strings absent, plugin receipt present, and seven normal Direct/Indirect/RHI/Simulator/SDK objects non-empty.
- Exact artifact launcher passed: hardware/automatic/distinct/imported/directional/common-scale `1/1/1/4/1/1`; Wet ratio `0.567596`, Full ratio `0.549702`, Direct/Wet difference `1.615526`.
- Exact default launcher passed: paths `171/171`, Direct `215` generations with no dropout and restored/hardware `1/1`, all data sources passed, hard realtime `1581/0/0`. Game `UERayTracingAudioValidation-Game-1785502210674966300.log`; Editor `UERayTracingAudioValidation-Editor-1785502391254800900.log`.
- Editor PID `13228` responded for the historical 2026-07-31 run and is superseded by PID `500`. Oversized-callback P3, prior Task 1-4 minors, Human/R3 listening, and true multi-PIE hardware isolation remain open; duplicate tagged-Source ambiguity is closed by exact-one normalization.

## 2026-08-01 - Acoustic geometry readiness

- 修复声学 Geometry Actor 同时持有多个 Geometry Component 时的 false-ready：运行时不再导出重叠表面，并确保验证场景只保留一个可用组件。
- 重新验证 Reference/Direct/Wet/Full 产物，Wet/reference 为 `0.567596`、Full/reference 为 `0.549702`，Direct/Wet 差异为 `1.615526`。

## 2026-08-02 - R3 32-bounce indirect matrix

- 完成 OpenSpace/NearWall/Enclosed 的 `0/1/7` Geometry、32-bounce 矩阵，使用同源 MarchingBand 生成 Reference/Direct/Wet/Full 产物，并验证 OpenSpace Wet 为物理零输出。
- Geometry 导出失败改为原子拒绝；仅逻辑空场景允许 masked DXR sentinel，避免不完整场景产生错误反射结果。
- 补齐实际 IR 帧数、时长和 Wet provenance，拒绝歧义验证角色，并明确证据比较容差。
- 最终矩阵 `20260802-074606` 生成 27 个非空产物、12/12 匹配 WAV hash、12 个导入 SoundWave 和 3 个 IR Asset；格式统一为 16 kHz、stereo、16000 帧、1 秒、Wet `0.8`。

## 2026-08-02 - Source mode and rendered path

- F3 现在只切换同步的 Original/Rendered A/B，不再把 F2 Baked 或 F5 Hybrid 静默重置为 Realtime。
- Rendered Source 显式报告实际 Direct、Early Reflection 和 Late Reverb 增益，输出路径标记为 `DIRECT+EARLY+LATE`。
- 交互验证确认 Realtime/Baked/Hybrid 与 A/B 状态保持，`f3_source_preserved=1`，并确认 Rendered 路径同时具有有限且非零的 Direct/Early/Late 输出。

## 2026-08-02 - Direct three-band robustness

- 三频空气吸收 crossover 在音频设备初始化时按实际采样率/Nyquist 约束；NaN/Inf 或乱序配置回退到 `500 Hz / 4000 Hz`。
- 非有限 low/mid/high band gain 在 Direct 重建前回退到 unity；重建使用 double precision，并在返回音频路径前饱和为有限 float。
- 回归覆盖非有限 band gain、运行时采样率 crossover、有限输入溢出和无效 Project Settings crossover；硬件运行中 Direct 持续输出且无 dropout。

## 2026-08-02 - World and Listener lifecycle

- 补充重复 Listener 移除身份、声学 Scene 地址稳定性和 World 失效时 pending request 清理回归，确认 Direct/Indirect 状态不会跨 World 串扰或遗留。

## 2026-08-02 - Public Source configuration

- Runtime validation、Editor scene 和 artifact flow 统一通过公开 Source setter 配置 Direct occlusion、Indirect mode、reflection settings、Wet Send、data source 和 baked asset。
- Blueprint-callable setter 对外部输入做有限值与范围归一化，同时保留 32-bounce 间接声验证配置。
