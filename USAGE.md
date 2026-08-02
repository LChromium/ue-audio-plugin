# UERayTracingAudio 使用说明

## 当前状态

插件处于重新验收阶段。硬件光追 Direct/Indirect、DSP 和 Bake 代码路径仍在，但旧的合成四模式录音已被人耳判定失败并移除。当前版本不得宣称声音质量已经通过。

## 在 Unreal Editor 中启用

1. 将 `Saved/Packages/UERayTracingAudio-0.2.0-Win64` 复制为项目的 `Plugins/UERayTracingAudio`；也可以直接使用源码插件目录进行开发。
2. 在 Plugins 面板启用 **UE Ray Tracing Audio**。
3. 项目使用 DX12，并启用硬件 Ray Tracing。
4. 在 Windows Target Settings 中选择：
   - Spatialization Plugin：`UE Ray Tracing Audio Spatialization`
   - Occlusion Plugin：`UE Ray Tracing Audio Occlusion`
5. 重启 Editor。

交付包仅面向 UE 5.7 / Win64，descriptor 版本为 `0.2.0` Beta。完整内容、严格 non-unity 构建门禁和已知限制见 `PACKAGE_AUDIT.md`。

## 设置场景

在监听者 Actor 上添加：

- `UUERayTracingAudioListenerComponent`

在声音 Actor 上添加：

- `UAudioComponent`
- `UUERayTracingAudioSourceComponent`

在墙体、地面、天花板等静态几何 Actor 上添加：

- `UUERayTracingAudioGeometryComponent`

Geometry 可先使用 `Bounding Box` 验证链路，再使用 `Static Mesh Triangles` 验证实际网格。设置三频 Absorption、Transmission 和 Scattering 后，运行时会发布新的声学场景版本。

## 可交互实时验收

蓝/橙密闭空间中，橙色球体是 Primary Source，蓝色球体是 Listener，不是两个声源。固定相机只用于自动测试；人工移动验收使用独立交互模式：

当前测试工程的 `Content/FirstPerson/Audio` 只保留 `MarchingBand.uasset`；Ravel 已移除。实时 Rendered 与 Original 两条链路都读取这一个 MarchingBand 输入。

### 持续播放语义

- `MarchingBand`、Rendered 和 Original 两条运行时链路均持续循环。距离、空气吸收和 Soft Occlusion 只改变 Direct 音量，不会停止 AudioComponent；只有显式 Hard Occlusion 的完全遮挡允许 Direct 接近静音。
- Original 与 Rendered 从同一采样零点同时启动；硬件 Direct/Indirect/左右 IR 快照未就绪时 Original 立即可听，Rendered 保持静音。主屏出现 `Rendered playback ready` 后才会自动淡入 Rendered，F3 在此之前不会用未初始化的 Rendered 替换 Original。
- Direct 距离项是线性样本幅度：参考距离内为 `1.0`，2 m 为 `0.5`，4 m 为 `0.25`。反射仿真内部仍使用能量语义，进入音频渲染器时再转换为线性幅度。
- F3 的 Rendered 侧是 **Realtime Full = Direct + early reflections + late reverb**，不是 Direct-only。完成自动门禁后场景会恢复 `HybridReverb` 处理模式和 Realtime 数据源。
- 新 Realtime IR 到达时，正在播放的卷积历史和尾音不会被清空；更新只进入一个 last-wins pending 槽，当前过渡结束后再采用最新 IR。

### Wet Send

Details 面板中的 `Wet Send` 对应原有 C++ 属性 `IndirectMix`，公式是：

`Full = Direct + WetSend × Wet`

- 默认值为 `1.0`，表示 unity Wet Send；`0` 关闭间接声，`0..4` 为允许范围。
- 大于 `1.0` 是显式 makeup gain，不是“100% Wet”的百分比。运行时主验收夹具使用 `1.75`，用于让低能量 Realtime Wet 更容易辨识。
- Wet Send 不会反向降低 Direct。若使用大于 `1.0` 的值，应降低 AudioComponent 或上游电平并检查 Full 峰值，避免削波；不要用硬限幅掩盖 gain-staging 问题。
- 新建 Source 和 Editor 验收场景默认 `1.0`。已有资产若曾显式保存旧值（例如 `0.3`），不会因 C++ 构造默认改变而自动迁移，请在 Details 中检查并重新保存。
- Editor Bake 会在点击 **Bake Selected Source** 时冻结输入 SoundWave、Source/Listener 和 Wet Send；Bake 期间切换选中 Actor 不会改变本次离线 A/B 参数。

```powershell
uv run script\launch_runtime_validation.py `
  --project TestProject\UeVersion1\UeVersion1.uproject `
  --game-seconds 180 `
  --interactive-smoke `
  --interactive-runtime
```

脚本先完成固定 `-game` 硬件门禁，再打开 Editor。点击 **Play** 后等待主屏显示 IR Modes `PASSED` 和 View `INTERACTIVE`，随后使用：

首次启动本地复制的测试工程时，Shader/Asset Registry 冷启动可能接近 60 秒；当前固定流程使用 180 秒窗口，并继续观察约 8 秒的 `MarchingBand` 多次循环与 17 秒 A/B 稳定性。这不是声学计算延迟。

- `WASD + 鼠标`：移动玩家；Listener 每帧跟随玩家相机。
- `F1`：Realtime IR。
- `F2`：Baked IR。
- `F3`：在 **RENDERED DIRECT+WET** 与 **ORIGINAL UNRENDERED** 之间 A/B 切换。
- `F4`：返回本次 Baked IR 的 Listener 烘焙原点。
- `F5`：Hybrid IR。
- `F8`：在固定自动测试相机和交互视角之间切换。

`F3` 比较的是同一个 SoundWave、同一启动时刻的两条同步链路：Rendered 链路经过硬件光追 Direct/Wet，Original 链路关闭空间化、遮挡和间接声插件；切换使用 50 ms 交叉淡变。进入交互模式后其余验证音会静音，避免干扰主声源判断。

主屏的 `CURRENT MODE` 是实际 Source 数据源，`A/B PLAYBACK` 显示当前听到 Rendered 还是 Original；`Baked asset` 显示位置/场景是否仍匹配。移动后 Baked/Hybrid 可能显示 `STALE PLACEMENT`，这是位置相关 IR 的正确警告，不应通过 `bAllowStaleBakedAsset` 掩盖。回到 F4 原点或重新 Bake 后再比较 Baked/Hybrid。

## 在 Editor 中进行真实 SoundWave A/B

1. 在当前测试工程中使用唯一预置输入 `/Game/FirstPerson/Audio/MarchingBand.MarchingBand`。在其他项目中可改用自己的 10–20 秒 PCM16 SoundWave，但不要选择 `/Game/UERayTracingAudio/Validation*` 下由验证器生成的资产作为输入。
2. 打开 **UE Ray Tracing Audio Bake**。若关卡中还没有完整验证场景，点击 **Create / Select Actual A/B Validation Scene**；面板会创建并选中可见的封闭房间、中央遮挡墙、橙色 Source、蓝色 Listener、7 个声学 Geometry、灯光和相机。可用 **Clear Path (2 m)**、**Soft Occluded (2 m)**、**Hard Occluded (2 m)** 切换等距 Direct 预设。
3. 在关卡中选择带 `UUERayTracingAudioSourceComponent` 的 Actor，点击 Source 行的 **Use Selected Actor**。
4. 选择带 `UUERayTracingAudioListenerComponent` 的 Actor，点击 Listener 行的 **Use Selected Actor**；确认 Geometry 摘要包含要参与 Bake 的房间和遮挡体。
5. 在 **Input SoundWave** 选择 `MarchingBand`，设置 IR Asset Package、Baked Audio Folder、Rays、Bounces、Duration 和 Sample Rate。
6. 点击 **Bake Selected Source**。硬件 IR 完成后，面板会在后台从同一采样起点生成、写入并导入等长的 Reference / Direct / Wet / Full。
7. Bake 保存成功后，面板会把新 IR 绑定到所选 Source 并默认选择 **[Active] Hybrid**。也可以用紧邻场景选择区的 **Realtime IR / Baked IR / Hybrid** 按钮切换同一 Source；当前模式和 IR 资产路径显示在按钮下方。
8. 查看 Listening Acceptance 摘要中的自动检查、Direct/Full dry correlation、电平比例、模式差异和公共缩放系数。摘要下方的四轨波形按同一起点、同一长度和同一 full-scale 纵轴显示：Reference 灰色、Direct 蓝色、Wet 紫色、Full 绿色。它用于定位掉音、尾声、削波和时间错位，不代替试听。
9. 分别播放四种模式，使用 **A/B Reference ↔ Direct**、**A/B Reference ↔ Full** 和 **Replay** 比较。点 **Clear Path (2 m)** 验证无遮挡；点 **Soft Occluded (2 m)** 或 **Hard Occluded (2 m)** 后重新 Bake 验证实体墙遮挡。预设保持 200 cm 距离，避免把距离衰减误当遮挡效果。
10. 只有在目标耳机/扬声器上确认 Direct/Full 保留原始主体、Wet 是合理空间尾部，且没有削波、掉音、噪声或时间跳变后，才点击 **Human Pass**；否则点击 **Human Fail**。

Reference / Direct / Wet / Full 四个按钮现在都会从采样零点循环播放，直到切换模式或点击 **Stop**。因此 Direct 播完一次后停止不再是正常行为；若 UI 仍显示模式但听不到声音，应先确认是否选择了 Hard Occluded，再查看运行时的 Direct gain 和持续 Wet 统计。

运行时 `IR Modes PASSED` 中的 80% 指输入有效缓冲里 Wet 真正非静音的 presence，不表示 80% 缓冲都要逐块达到 5%。5% 阈值用于整个观察窗口按帧积分的 Wet/Input RMS（同时要求至少一个缓冲的最大比例达到该值）；这些数值门禁用于排除断流和近静音，最终是否可辨识仍以目标设备上的 Human Pass 为准。

同一门禁还记录最终 Full 输出峰值和绝对值大于 `1.0` 的样本数。双声道源绕过自定义 Spatialization 时读取 Occlusion 最终输出；进入 Spatialization 的源读取 L/R 重组后的最终输出。正式验收要求三种 IR 的 `full_peak <= 1.0`、`over_unit=0`、`non_finite=0`。

### 三种运行时 IR 数据源

- **Realtime IR**：只使用当前硬件光追仿真生成的左右卷积核；不需要 Baked IR 资产。
- **Baked IR**：只使用保存资产中的左右卷积核；资产必须匹配当前 world、scene/material signature 以及 Source/Listener placement。
- **Hybrid**：用 Baked IR 覆盖 early field，用 Realtime IR 覆盖 tail；两段在 `HybridTransitionRatio` 对应时间附近使用互补淡入淡出，因此相加权重保持为 1。没有有效 Baked IR 时，运行时会安全退回完整 Realtime IR，不会丢失 early field。

切换时四个卷积器和总 Wet Send 都会平滑过渡，不应产生 click/pop 或突然切断尾声。若 Details 面板中的 `BakedAssetStatus` 不是 `Ready`，先按状态文本修复 world、scene/material 或 placement 不匹配，再使用 Baked/Hybrid；不要用 `bAllowStaleBakedAsset` 掩盖正式验收中的 stale 资产。

### 实时卷积预算与晚期尾声

- 运行时每个左右声道最多直接卷积 `4 × 1024 = 4096` 个 IR 样本；48 kHz 时约覆盖前 85.33 ms。该限制只约束实时音频线程使用的 kernel，不会裁剪保存的 Baked IR 或离线 Reference/Direct/Wet/Full 资产。
- 如果 IR 超过实时头部，Source 会估计被截断部分的双声道能量，并由 parametric late reverb 承接尾声。因此预期结果是“确定成本的早期卷积 + 持续晚期混响”，不是 85 ms 后突然静音。
- kernel 与 crossfade 状态在非音频线程准备；音频回调只采用已经准备好的状态。Bridge 使用轮转服务和有界 PrepareBudget，在大量 Source 更新时保持公平。
- 运行时日志必须出现 `UERayTracingAudio hard realtime: passed=1`，且 `callback_capacity_misses=0`、`convolution_prepare_drops=0`。任一值非零都视为验证失败。

可独立运行源码审计与脚本回归：

```powershell
uv run script\validate_audio_realtime_safety.py
uv run python -m unittest discover -s script\tests -v
```

完整构建仍必须使用：

```powershell
uv run script\build_and_validate.py
```

已移除且不要再使用：

- `--record-listening`
- `script/analyze_listening_recordings.py`
- `script/run_listening_acceptance.py`
- 旧的 `20260722-*` 四模式 WAV 作为通过证据

## Task 2：在多个 World 中使用 Listener 与声学场景

每个 `UWorld` 放置一个 `UUERayTracingAudioListenerComponent`。同一 World 内按注册顺序保留第一个有效 Listener；后注册的重复 Listener 会被忽略并记录 warning。不同 World（包括 PIE 实例）各自维护 Listener、Geometry、scene signature 和稳定的 acoustic scene。

Source、参与 Bake 的 Source/Listener/Geometry 必须属于同一目标 World。Direct、Indirect、stale-asset 检查和 directional IR 都按 Source 或 Bake Source 的 World 解析；硬件 Direct/Indirect 请求也按 scene 分批。World 销毁后，管理器会清理弱 Listener key，并在该 scene 不再被查询引用后清理 acoustic state。

建议的 Editor 检查：

1. 在每个 PIE World 中只启用一个 Listener，并确认 Source 与声学 Geometry 位于该 World。
2. 若同一 World 有多个 Listener，移除或禁用重复项；不要依赖注册顺序作为关卡设计。
3. 多 PIE 验证时分别移动各 World 的 Listener/Source，并观察另一个 World 的 Listener、遮挡和 scene signature 不受影响。
4. Baked 资产仍需通过当前 World、scene/material signature 和 Source/Listener placement 的 stale 检查。

Task 2 已验证的自动化证据：

- `uv run script\build_and_validate.py`：exit `0`；`31 functions / 32 bodies / 0 forbidden operations`；`Result: Succeeded`；`Build and validation complete.`
- `UERayTracingAudio.Audio.ConfigurableDirect` NullRHI：`3/3`、`0 failed`；日志 `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-WorldScoping-Final.log`。
- `UERayTracingAudio.Audio` NullRHI：`26/26`、`0 failed`；日志 `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task2-Audio-Final.log`。
- `uv run script\launch_runtime_validation.py`：exit `0`；Direct/Indirect batches `4/4`，hardware/CPU paths `171/171`，gain `0.001625/0.001625`，data sources passed，kernels `2/2/4`，`non_finite=0`，hard realtime `169 / 0 / 0`。Game 日志：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785425951315732900.log`；Editor 日志：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785426132061345000.log`。

范围说明：两 World 隔离仅在 NullRHI Automation 中完成，固定硬件运行时为单 World；尚未执行真实 multi-PIE hardware session。Editor PIE 人工试听和 Human Pass 也未执行。以上自动化结果不能替代 Human Pass，不能据此宣称整个插件已完成。

## Task 3: Using three-band direct air absorption

1. Open **Project Settings > Plugins > UE Ray Tracing Audio** and set **Air Absorption Low-Mid Crossover Hz** and **Air Absorption Mid-High Crossover Hz**. The validated defaults are `500 Hz` and `4000 Hz`; restart the Editor after changing them because the module caches the pair at startup.
2. On the source's **UE Ray Tracing Audio Occlusion Settings**, enable **Apply Air Absorption**. Distance attenuation and occlusion remain broadband; the simulation snapshot's low/mid/high air gains shape those bands independently.
3. Keep **Apply Air Absorption** disabled when only broadband distance and occlusion behavior is desired.
4. Run `uv run script\build_and_validate.py`, then `uv run script\launch_runtime_validation.py`. The fixed launcher validates Game mode and leaves the Editor open for listening.

Task 3 automated evidence: ConfigurableDirect NullRHI passed `9/9` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task3-ConfigurableDirect-Final-Reviewed.log`; full Audio passed `32/32` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task3-Audio-Final-Reviewed.log`. The spectrum check measured low/high RMS `0.706901057 / 0.314958528`, ratio `2.244426`.

The final fixed runtime passed with Game log `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785461920592378400.log` and Editor log `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785462101170308900.log`.

Manual acceptance remains required: use the target headphones/speakers and the same recognizable input to listen through distance sweeps, audible air absorption, and moving occlusion. Record the device, source asset, scene, settings, and result. These checks and the Task 2 multi-PIE hardware follow-up are still open; automated RMS and runtime diagnostics do not constitute Human Pass.

## Task 4: Switching indirect data at runtime

Use the Blueprint-callable functions in the `UE Ray Tracing Audio|Indirect` category:

1. To use a baked or hybrid mode, call `SetBakedImpulseResponseAsset` on the `UERayTracingAudioSourceComponent` with the desired IR asset.
2. Call `SetIndirectDataSource` with `Realtime`, `Baked`, or `Hybrid`. The component refreshes the applicable kernels and publishes the new audio snapshot on its next tick.
3. Reassigning the baked asset through the setter invalidates cached prepared convolution state. Do not depend on a previous asset's kernel after replacement.
4. The validation scene's F1/F2/F5 controls call the same public setter used by Blueprint/C++ projects: F1 is Realtime, F2 is Baked, and F5 is Hybrid.
5. Check `BakedAssetStatus` before accepting Baked or Hybrid output; fix missing, stale, World, scene, or placement errors rather than overriding them for formal validation.

Task 4 verification evidence:

- RED: the Direct diagnostics API and both setter UFUNCTIONs were absent in `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-RED-ConfigurableDirect.log`.
- GREEN: ConfigurableDirect `11/11` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-GREEN-ConfigurableDirect.log`; full Audio `34/34` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-GREEN-Audio.log`; Python `50/50`; realtime audit `36 functions / 37 bodies / 1634 lines / 0 violations`.
- Fixed hardware runtime exited `0`, reported paths `171/171` and callbacks/misses/drops `169/0/0`, and left the Editor open. Logs: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785466094540462400.log` and `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785466275542564200.log`.

The diagnostics prove bounded numeric observations, not audible quality. Use the open Editor in PIE and target headphones/speakers to verify recognizable content, F1/F2/F5 transitions, moving Direct continuity, and no click/pop; record that separately as Human Pass/Fail.

### Task 4 Fix Round 1 behavior

When selecting a component for Direct diagnostics, call `FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId`, then `ResetDirect`, and read snapshots with `ReadDirect`. Target generations are internal: an audio callback captures a plain identity/generation token and its result is discarded if the target changes before publication, including an A/B/A switch back to the same component ID. No retry, wait, lock, or callback allocation is introduced.

If a callback arrives with more channels than the source prepared, the processor records one capacity miss and uses its scalar broadband fallback. That scalar now interpolates from the last gain actually rendered to the current broadband target on every non-empty buffer, and `MaxBandGainStep` reports that applied scalar step. A zero-frame callback can observe and seed the first valid snapshot but does not consume a pending transition; the first rendered buffer after the first valid snapshot remains transition-free.

Fix Round 1 verification:

- RED: ConfigurableDirect `10/12`, with only stale-target publication and scalar fallback continuity failing, at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix1-RED-ConfigurableDirect.log`.
- GREEN: prescribed build `47/47`; ConfigurableDirect `12/12` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix1-FINAL-ConfigurableDirect.log`; full Audio `35/35` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix1-FINAL-Audio.log`; realtime audit `39/40/1718` with zero violations; Python `51/51`.
- Runtime: the fixed launcher exited `0`, reported Direct/Indirect batches `4/4`, hardware/CPU paths `171/171`, data sources passed, and callbacks/misses/drops `176/0/0`. Logs: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785468883156707700.log` and `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785469063951366500.log`.

The Editor is left open for the same manual PIE and target-device checks. These automated fixes do not close the moving Direct sweep, audible click/pop, Human Pass, or Task 2 multi-PIE hardware gates.

### Task 4 Fix Round 2 reset behavior

`FUERayTracingAudioAudioDiagnostics::ResetDirect` now starts a new diagnostic context as well as a new statistics epoch. The selected audio-component ID does not change, but any token captured before the reset is invalid afterward. This matters when a game/control-thread reset overlaps an audio callback: the old callback is discarded instead of becoming the first buffer in the new epoch, and the next callback captured after reset publishes normally.

Continue to serialize `SetTargetAudioComponentId` and `ResetDirect` on one control/game thread. Reassigning the same target ID remains a no-op; use `ResetDirect` when the intent is to clear the current target's Direct statistics and invalidate already captured callback work. Reads immediately after reset remain empty until a post-reset callback publishes.

Fix Round 2 verification:

- RED: ConfigurableDirect `12/13`; the reset-only generation, empty-epoch, and one-buffer follow-up assertions failed at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix2-RED-ConfigurableDirect.log`.
- GREEN: prescribed build `47/47`; ConfigurableDirect `13/13` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix2-FINAL-ConfigurableDirect.log`; full Audio `36/36` at `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\Task4-Fix2-FINAL-Audio.log`; realtime audit `39/40/1718` with zero violations; Python `51/51`.
- Runtime: the fixed launcher exited `0`, reported paths `171/171`, data sources passed, and callbacks/misses/drops `190/0/0`. Logs: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785470614600787600.log` and `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785470795487644500.log`.

Editor PID `22952` was left open for that historical run and is superseded by PID `500`. The moving Direct sweep, Human Pass/click-pop checks, Task 2 multi-PIE hardware isolation, and ledgered Task 4 Minors remain separate follow-ups.

### Task 5 Direct sweep fixture

The validation-only Direct sweep uses a fixed listener origin and moves the validation source along a 200 cm quarter arc from the clear side of the wall to the occluded side and back. It holds each endpoint long enough to collect fresh Direct generations, displays phase/distance/visibility/gain/air bands in the HUD, and emits exactly one terminal line beginning with `UERayTracingAudio direct sweep:`.

For automatic Game validation, both `-UERayTracingAudioValidationScenario` and `-UERayTracingAudioValidationDirectSweep` are required. `script\launch_runtime_validation.py` now supplies both flags to every fixed Game command. It deliberately omits the automatic sweep flag from the Editor command, where F6 remains the explicit user action.

The first successfully created validation scenario is the process diagnostics owner. Only that World runs the automatic sweep, subsequent data-source diagnostics, F6 sweep, and sweep HUD, preventing process-global audio counters or terminal markers from being mixed across multi-world sessions. If no hardware Direct generation arrives within the acoustic-startup deadline, the owner emits one failed strict summary with `hardware=0` and marks the automatic sweep terminal instead of waiting indefinitely.

In the interactive validation scenario, press F6 after the first hardware Direct result to start the same state machine. F6 is non-reentrant. During the sweep, source/data-source controls and camera-follow motion are suppressed; F3 remains available for the existing debug visualization.

On success, timeout, actor destruction, world teardown, or validation stop, the fixture restores the source transform, hard-occlusion state, `OccludedGain`, `IndirectMix`, and indirect data source exactly once. A normal success waits for a fresh post-restore Direct generation before reporting. The fixture is compiled only when `WITH_UERAYTRACINGAUDIO_VALIDATION=1`, is additionally gated by the validation scenario, and does nothing in ordinary projects.

Automated Task 5 evidence is build `48/48`, callback audit `39/40` with zero forbidden operations, and ConfigurableDirect `15/15` at `D:\Labs\2602-unreal\ue-audio-plugin\.worktrees\configurable-direct-audio-validation\TestProject\UeVersion1\Saved\Logs\Task5-FINAL-ConfigurableDirect.log`. Task 6 has now supplied the hardware runtime marker/parser gate; target-device click/pop and audible-quality Human Pass remains pending.

### Task 6 fixed-launcher Direct gate

Run the normal fixed flow:

```powershell
uv run script\build_and_validate.py
uv run script\launch_runtime_validation.py
```

The Game phase must produce exactly one fully parseable `UERayTracingAudio direct sweep:` terminal line. The launcher rejects a failed marker, fewer than eight generations, distance outside `198-202 cm`, visibility that does not reach both `<= 0.10` and `>= 0.90`, zero Soft Occlusion gain, a gain step above `0.01`, any Direct dropout, unrestored Source state, CPU fallback, non-finite numbers, partial lines, or multiple marker lines. This gate runs before the existing Baked/Realtime/Hybrid and hard-real-time acceptance checks.

The first integrated run correctly failed instead of weakening those gates: the sweep moved Source before the baseline CPU reference was calculated, so an old hardware Indirect result (`171` paths) was compared with a different-position CPU result (`953` paths). Automatic movement now waits until the baseline validation result and CPU reference have been logged. The final Game log proves the intended order:

```text
baseline result -> CPU reference -> Direct sweep -> data-source Bake -> data-source result
```

Final automatic evidence:

- baseline hardware/CPU paths `171/171`, gain `0.001625/0.001625`, both relative deltas `0`;
- Direct sweep: `213` generations, distance `200.000 / 200.000 cm`, visibility `0.000040 / 0.996630`, gain `0.174780 / 0.498240`, maximum step `0.00004591`, dropouts `0`, restored `1`, hardware `1`;
- Baked/Realtime/Hybrid passed with kernels `2/2/4`, `non_finite=0`;
- hard realtime callbacks/misses/drops `1583/0/0`;
- Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785475390548759300.log`;
- Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785475571210643500.log`.

Editor PID `35468` was left initialized for that historical run and is superseded by PID `500`. Use the current Editor for an interactive repeat, then record target-device recovery and click/pop as Human Pass/Fail. Automated marker success does not replace that Human Pass.

### Task 6 Fix Round 1: strict domains and sequencing

The Direct terminal now has physical-domain constraints in addition to endpoint thresholds:

- `0 <= visibility_min <= visibility_max <= 1`;
- `0 < gain_min <= gain_max <= 1`;
- `0 <= max_gain_step <= 0.01`.

The fixed Game run also requires the marker `UERayTracingAudio validation data-source bake started`. The Direct terminal must appear strictly before that Bake marker, the hard-real-time marker, and the final data-source marker. This prevents valid-looking Direct extrema from being collected after Bake or final runtime evidence has already begun.

Fix Round 1 verification:

- RED: focused RuntimeValidation `35/47`, with `12` intended failures covering domain, ordering, and error-message contracts.
- GREEN: focused RuntimeValidation `47/47`; full Python `62/62`; callback audit `39/40/1718` with zero forbidden operations.
- Runtime: the exact launcher exited `0`. Direct line `1118` preceded Bake `1126`, hard realtime `1130`, and final data source `1132`. Direct metrics were `214` generations, `200.000 cm`, visibility `0.000038-0.997131`, gain `0.174779-0.498402`, maximum step `0.00005548`, dropout `0`, restored `1`, hardware `1`.
- Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785476653651929300.log`.
- Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785476834237184600.log`.

Editor PID `42300` was left initialized for that historical run and is superseded by PID `500`. The automatic proof is stricter, but target-device recovery/click-pop still requires Human Pass.

### Task 7: Editor validation distance and air-absorption controls

Open **Window -> UE Ray Tracing Audio Bake**. The validation fixture section now contains:

```text
Validation Distance: Clear 1 m | Clear 2 m | Clear 4 m
Validation Air Absorption: Off | Default | Stress
```

The distance buttons select the Clear preset and move only the Source actor tagged `VRTA_EditorValidationScene`. The air buttons apply only to that same tagged fixture Source:

- Off: `(0, 0, 0)`
- Default: `(0.0002, 0.0006, 0.0012)`
- Stress: `(0.01, 0.04, 0.12)`

The panel shows the effective distance and vector beside `Validation fixture only`. All fixture-mutation controls are disabled while a Bake job or offline comparison render is active. Generic selected Sources remain available for the normal Bake workflow, but these validation controls never select, move, configure, or destroy an untagged Source.

Changing the fixture reflection environment removes stale tagged acoustic Geometry actors that do not belong to the selected definition set. It preserves tagged Source/Listener/lights/camera actors, every untagged actor, and normal component teardown/Editor transactions.

The fixed launcher accepts:

```powershell
uv run script\launch_runtime_validation.py `
  --editor-distance-cm 400 `
  --editor-air-absorption-profile stress
```

Allowed values are `100`, `200`, or `400` and `off`, `default`, or `stress`. The resulting Unreal parameters are added only to the Editor command; the preceding Game validation command remains independent.

The fixed Editor gate requires one marker containing the effective fixture fields:

```text
UERayTracingAudioEditor validation scene ready: ... source_listener_distance_cm=400.00 air_absorption_profile=stress air_absorption_per_meter=(0.010000,0.040000,0.120000).
```

It rejects missing, malformed, duplicate, or request-mismatched Direct preset, reflection environment, distance, profile, or vector evidence. This is configuration evidence only: it does not claim an audible distance/air result, an OpenSpace/NearWall/Enclosed R3 comparison, or Human Pass.

Task 7 verification used the exact default launcher and left Editor PID `34840` open for that historical run; PID `34840` is superseded by PID `500`. The parsed fixture was `200 cm / default / (0.0002,0.0006,0.0012)`. Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785478813724452300.log`. Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785478994289043000.log`.

#### Task 7 Fix Round 1: selecting a Source safely

Opening the Bake panel no longer adopts an ordinary Source automatically, even when that Source is selected in the level. Automatic fixture discovery accepts only the actor tagged with both `VRTA_EditorValidationScene` and `VRTA_AB_Source`.

To bake an ordinary project Source:

1. Select its actor in the level.
2. Click **Use Selected Source** in the Bake panel.
3. Start the Bake.

That explicit action is the only way an untagged Source enters the panel's selected-Source state. If neither an explicit selection nor the tagged validation Source is available, **Bake Selected Source** refuses to start instead of falling back to the current generic selection.

Persistent fixture component additions are normal Editor transactions: Geometry, Source, Audio, and Listener components can be undone/redone, and the level is marked dirty. The transient Automation fixture creates a World context only after the World exists and fully destroys the World/context before garbage collection.

Fix Round 1 verification used the exact default launcher. It exited `0` and left Editor PID `13344` responding for that historical run; PID `13344` is superseded by PID `500`. It parsed `200 cm / default / (0.0002,0.0006,0.0012)`. Game log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785491460999842900.log`. Editor log: `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785491641704854400.log`. This remains configuration/runtime evidence, not a target-device Human Pass.

## Task 8: Product configuration and final validation workflow

### Normal product configuration

In **Project Settings -> Plugins -> UE Ray Tracing Audio**, configure these cached physical parameters:

- `ReferenceDistanceCm=100`, constrained to `>= 1`.
- `MaxDistanceCm=5000`, constrained to `>= ReferenceDistanceCm`.
- `SpeedOfSoundCmPerSecond=34300`, constrained to `> 0`.
- `AirAbsorptionLowMidCrossoverHz=500`, constrained to `20..Nyquist`.
- `AirAbsorptionMidHighCrossoverHz=4000`, constrained to `LowMid..Nyquist`.

Restart the Editor or reinitialize the audio device after changing them; existing audio-plugin instances retain their startup configuration. On each `UUERayTracingAudioSourceComponent`, set `AirAbsorptionPerMeter` and enable **Apply Air Absorption** when frequency-dependent Direct attenuation is wanted. `SetIndirectDataSource` and `SetBakedImpulseResponseAsset` are Blueprint-callable for Realtime/Baked/Hybrid selection and baked-asset replacement.

Listener ownership is per `UWorld`: the first valid registered `UUERayTracingAudioListenerComponent` owns that World, and later duplicates are ignored until ownership changes. The Direct processor uses complementary low/mid/high bands, so three unity gains reconstruct the input; distance/occlusion remain broadband while air absorption shapes the three Direct bands. Wet/Indirect audio bypasses this Direct air-absorption filter.

### Opt-in validation fixture

`-UERayTracingAudioValidationScenario` is a non-Shipping validation fixture, not a product requirement for ordinary projects. The fixed Game flow also supplies `-UERayTracingAudioValidationDirectSweep`; the Editor omits that automatic flag and leaves F6 as the explicit sweep action. The sweep runs clear hold -> outbound wall traversal -> Soft Occluded hold -> return traversal -> final clear hold -> exact Source/settings restoration, then emits one parsed `UERayTracingAudio direct sweep:` marker.

The Bake panel's fixture-only controls are **Clear 1 m / Clear 2 m / Clear 4 m** and **Off / Default / Stress** air absorption. Their exact per-meter vectors are Off `(0,0,0)`, Default `(0.0002,0.0006,0.0012)`, and Stress `(0.01,0.04,0.12)`. These controls mutate only the tagged validation fixture and are disabled during Bake/offline rendering; they are not general Source presets.

Run the technical flow with:

```powershell
uv run script\build_and_validate.py
uv run script\launch_runtime_validation.py
```

For the remaining Human check in the Editor left open by the launcher:

1. Enter PIE and wait for the hardware Direct/Indirect and data-source gates.
2. Press F8 to enter the interactive view.
3. Press F3 to compare Original and Rendered, then F6 to listen through Clear -> wall -> Soft Occluded -> Clear.
4. Confirm there is no playback restart, click/pop, unexpected silence, timing jump, or failed recovery while moving the player/occlusion path.
5. In the Bake panel, compare Clear 1/2/4 m and Default/Stress air absorption on the same target headphones/speakers.

Only the user performing that listening may record Human Pass. True multi-PIE hardware isolation, moving-player/moving-occlusion listening, audible distance/air comparison, click/pop judgment, and target-device OpenSpace/NearWall/Enclosed listening remain open. The automated R3 matrix is documented below.

### Final hardening behavior

The final review fixes do not add a second product configuration surface. Existing Project Settings and per-Source controls remain the reusable interface. Their runtime guarantees are now stricter:

- Direct queries strictly beyond the configured maximum distance are silent and skip propagation work; the exact boundary remains included, as covered by the boundary regression.
- Removing or losing the active Listener cancels pending queries for that World, publishes Direct bypass/unity, and publishes zero realtime Wet. Registering a valid Listener resumes from fresh results rather than a stale room snapshot.
- A NaN/Inf input sample is sanitized before convolution, pre-delay, and feedback state, so later finite audio can recover instead of inheriting poisoned Wet state.
- Hardware validation means an RHI dispatch actually succeeded. Capability alone and CPU fallback no longer count as hardware evidence.
- Removing/resetting Sources reclaims unpinned IR snapshots promptly. Reused SourceIds cannot emit a previous voice's Wet while a convolver return is waiting for Game Thread service.

For an ordinary project Source, **Bake Selected Source** consumes the chosen input SoundWave without silently replacing `AudioComponent.Sound`. Use **Use Selected Source** to opt an untagged Source into the panel. The validation fixture is normalized to exactly one actor for each tagged role and exactly one acoustic Geometry component per Geometry actor; duplicate or modified tagged geometry is repaired transactionally before readiness is reported.

Sequential PIE is supported: when the validation-owner World tears down, only that World's ownership and delegates are released. A later PIE session can become owner; a concurrent non-owner World cannot steal ownership.

To run the strict offline A/B artifact gate:

```powershell
uv run script\launch_runtime_validation.py --editor-ab-artifacts
```

The command requires actual Direct and Indirect hardware submission, one complete/unique result for each terminal contract, four imported WAV artifacts, a directional Wet result, and the final `common_scale` field. The validation fixture uses `IndirectMix=0.8` only for this comparison so Full retains an audible/detectable dry cue; it is not a product default and the acceptance thresholds are unchanged. A partial marker still being written, any duplicate/malformed marker, fallback-only evidence, or `auto_checks=0` fails the command.

The 2026-08-01 automatic runs passed both artifact and default flows after exact-one Geometry-component normalization. Their Editor PID `36896` and logs are historical and superseded by PID `500` and the final R3 evidence below. These checks establish technical behavior, not a target-device Human Pass.

## R3：32 次反射环境矩阵

在仓库根目录运行：

```powershell
uv run script\validate_reflection_environment_matrix.py
```

脚本为 OpenSpace、NearWall、Enclosed 分别启动独立 Editor，并在以下时间戳目录写入汇总、结果 JSON 和截图：

`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\<timestamp>`

每个环境都从同一个 `/Game/FirstPerson/Audio/MarchingBand.MarchingBand` 采样零点生成四条同长度、同公共缩放的轨道：

- **Reference**：未经插件处理的原音。
- **Direct**：清晰无遮挡的 Direct 渲染。
- **Wet**：只包含反射和混响，不含 Direct；导出的 Wet 已经是“Reference 与 IR 卷积后再乘验证 Wet mix `0.8`”的结果。
- **Full**：`Direct + Wet`。这里的 Wet 已包含 `0.8`，不要对导出的 Wet 再乘一次。

R3 固定参数为 200 cm、默认空气吸收、4096 条反射射线、最多 32 个反射段、双声道 16 kHz、1 秒 IR。Direct 段不计入 bounce，所以“32 bounces”表示最多 32 个反射段。三个夹具分别包含 0 / 1 / 7 个声学 Geometry：

- OpenSpace：没有反射面，Wet 必须精确静音，Full 与 Direct 完全相同。此物理零 Wet 场景中 `automatic_checks_passed=false` 是正确结果。
- NearWall：一面墙产生有方向性的第一/早期反射，`automatic_checks_passed` 必须为 `true`。
- Enclosed：七个 Geometry 产生更强早期能量和更长、非零的晚期尾声，`automatic_checks_passed` 必须为 `true`。

最终 provenance 合同要求原始硬件 manifest 和每个 case 汇总同时报告 `sample_rate=16000`、输出 `channels=2`、`impulse_response_channels=2`、`impulse_response_frames=16000`、`impulse_response_duration_seconds=1.0`。实际 `wet_mix=0.800000011920929`，在汇总声明的 `fixed_float_tolerance=1e-6` 内等于固定配置 `0.8`；`frames / sample_rate` 与 IR 时长的误差必须在 `ir_duration_consistency_tolerance=1e-9` 内，本次误差为 `0`。输出 WAV 的整段 `frames` 与 IR 的 `impulse_response_frames` 是不同字段，不能互相替代。

完成审计曾因旧 `20260802-063000` 缺少上述实际 IR/Wet provenance，且夹具未在任何事务或修改前拒绝重复角色、多角色和无角色拓扑，而拒绝将其视为最终证据。`84830a3`、`26dcf11`、`94b3537` 关闭这些缺口后，重新运行全部门禁。唯一权威汇总是：

`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\ReflectionEnvironmentMatrix_Manifest.json`

三张可见性截图：

- OpenSpace：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\OpenSpace.png`
- NearWall：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\NearWall.png`
- Enclosed：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix\20260802-074606\Enclosed.png`

三张截图均通过非黑和亮度可见性检查，但它们字节完全相同；因此只能证明 Editor 画面可见，不能作为 0 / 1 / 7 Geometry 的视觉证明。Geometry 数量由严格 scene marker、结果 JSON 和 manifest 证明。

用于人耳验收的 12 条 WAV：

- OpenSpace Reference：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234624\MarchingBand_clear_HardwareIR_clear_20260801-234624_Reference.wav`
- OpenSpace Direct：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234624\MarchingBand_clear_HardwareIR_clear_20260801-234624_Direct.wav`
- OpenSpace Wet：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234624\MarchingBand_clear_HardwareIR_clear_20260801-234624_Wet.wav`
- OpenSpace Full：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234624\MarchingBand_clear_HardwareIR_clear_20260801-234624_Full.wav`
- NearWall Reference：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234649\MarchingBand_clear_HardwareIR_clear_20260801-234649_Reference.wav`
- NearWall Direct：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234649\MarchingBand_clear_HardwareIR_clear_20260801-234649_Direct.wav`
- NearWall Wet：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234649\MarchingBand_clear_HardwareIR_clear_20260801-234649_Wet.wav`
- NearWall Full：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234649\MarchingBand_clear_HardwareIR_clear_20260801-234649_Full.wav`
- Enclosed Reference：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234734\MarchingBand_clear_HardwareIR_clear_20260801-234734_Reference.wav`
- Enclosed Direct：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234734\MarchingBand_clear_HardwareIR_clear_20260801-234734_Direct.wav`
- Enclosed Wet：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234734\MarchingBand_clear_HardwareIR_clear_20260801-234734_Wet.wav`
- Enclosed Full：`D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\UERayTracingAudio\ListeningAcceptance\HardwareValidation\20260801-234734\MarchingBand_clear_HardwareIR_clear_20260801-234734_Full.wav`

人工试听顺序：

1. 先在三个环境中分别比较 Reference 与 Direct，确认它们来自同一个 MarchingBand 内容和采样起点，主体连续可辨。
2. 播放 OpenSpace Wet，确认它是精确静音；再确认 OpenSpace Full 与 Direct 听感一致。
3. 播放 NearWall Wet/Full，确认能听到相对 Direct 延后的、具有方向感的第一反射。
4. 播放 Enclosed Wet/Full，并与 NearWall 对比，确认早期能量更强且晚期尾声更长、非零。
5. 全程确认 MarchingBand 连续，没有只响一次、意外掉音、click/pop 或时间跳变。

只有实际完成这组目标耳机/扬声器试听的用户可以记录 Human Pass；自动化结果不能代替该结论。

`uv run script\launch_runtime_validation.py` 是另一条固定交互流程，使用既定默认 `8` bounces，不是上述 32-bounce 离线矩阵。它最终留下的当前 Editor PID 为 `500`，正在响应、运行精确测试项目/验证场景和 DX12 光追，并有意保持打开；Game 日志为 `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-1785628395987303100.log`，Editor 日志为 `D:\Labs\2602-unreal\ue-audio-plugin\TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Editor-1785628576602470800.log`。该运行通过 paths `171/171`、gain `0.001625/0.001625`、Direct `221` generations、零 dropout、restored/hardware `1/1`、hard realtime `1579/0/0`，以及 Baked/Realtime/Hybrid kernels `2/2/4`、`nonfinite=0`。在该 Editor 中，F1/F2/F5 分别选择 Realtime/Baked/Hybrid，F3 比较 Original Unrendered 与 Rendered Direct+Wet；这些快捷键用于交互试听，不应被描述成正在运行 32-bounce 矩阵。
