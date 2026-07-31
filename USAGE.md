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
