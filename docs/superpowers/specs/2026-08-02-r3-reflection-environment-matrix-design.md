# R3 三场景反射环境矩阵设计

日期：2026-08-02

状态：用户已批准方案 A，并指定 32 bounce

## 目标

建立一个独立、可重复、可听且可审计的 R3 验收矩阵，证明硬件光线追踪产生的间接声会随真实场景几何发生变化，而不是固定 Wet 效果。矩阵覆盖：

- `OpenSpace`：无声学几何；
- `NearWall`：单面近墙；
- `Enclosed`：七个几何体组成的封闭验证空间。

每个场景使用同一份项目内 `MarchingBand` 原音、同一 Source/Listener 起点和同一 Direct 设置，生成 `Reference`、`Direct`、`Wet`、`Full` 四条 WAV、可见 Editor 截图和机器可读 manifest。所有场景统一请求并实际执行 **32 次反射 bounce**；直达声段不计入 bounce。

本特性的完成只证明 R3 自动化矩阵及其可听产物成立。人耳是否无 click/pop、空间感是否符合预期，仍必须由用户在目标设备上明确记录 Human Pass，自动化不得替代该结论。

## 已确认边界

- 采用方案 A：扩展现有 Editor 验证夹具、Bake 四轨产物链和独立 Python 验证器。
- 不改变普通项目的默认 Source、场景、快捷键或 DSP 参数；三场景和 32-bounce 默认值只属于显式启用的验证流程。
- 不把 `ravel.wav` 或任何 Ravel 资产重新引入项目；输入必须解析为项目中的 `MarchingBand` SoundWave。
- 不降低现有通用 `automatic_checks_passed` 门禁。该门禁要求 Wet 可听，因而在正确的 OpenSpace 零 Wet 场景中应为 false。
- OpenSpace 使用单独的“零间接声”语义；NearWall 和 Enclosed 继续要求 Wet 可听并满足现有通用门禁。
- 现有 1-bounce/8-bounce Enclosed 对比继续保留；它验证 bounce 增量，不替代本次固定 32-bounce 的环境矩阵。
- 如果矩阵暴露产品 DSP 或硬件/CPU 路径的真实缺陷，必须先增加能复现该缺陷的失败测试再修复；不得为了通过矩阵向结果注入测试专用反射或混响。

## 固定测试配置

三种环境除几何外必须保持以下参数一致：

| 参数 | 固定值 |
| --- | --- |
| Input SoundWave | `/Game/.../MarchingBand.MarchingBand` |
| Direct preset | `clear` |
| Source/Listener distance | 200 cm |
| Air absorption profile | `default` |
| Reflection rays | 4096 |
| Maximum reflection bounces | 32 |
| Bounce 定义 | 最多 32 次反射；Direct 不计入 |
| Bake IR duration | 1.0 s |
| Bake sample rate | 16000 Hz |
| IR channel count | 2（directional stereo） |
| Validation Wet mix | 0.8 |

Editor 验证 Source 的实时 `MaxReflectionBounces` 和传给 Bake 的 `MaxBounces` 必须来自同一个已 clamp 的有效值 32，范围仍为 `[1, 64]`。日志和 manifest 都必须记录实际值，不能只记录请求值。

## 场景契约

### OpenSpace

- 恰好 0 个带验证标签的声学 Geometry；
- 硬件与 CPU 的有效间接路径数都为 0；
- indirect、early、late、IR energy 和 Wet RMS 在绝对容差内为 0；
- Wet WAV 的 PCM 样本全零，Full WAV 与 Direct WAV 相同；
- `modes_are_distinct=false`、`directional_wet_is_distinct=false`、`automatic_checks_passed=false`；
- Direct 语义、有限样本、无削波和无 Direct dropout 仍必须通过。

OpenSpace 的零 Wet 是正确结果，不允许通过最小增益、合成尾音或固定混响让旧门禁变绿。

### NearWall

- 恰好 1 个验证 Geometry，角色为 `VRTA_AB_NearWall`；
- 硬件与 CPU 都必须得到正的有效路径、IR energy、indirect gain 和 early-reflection gain；
- directional bin 数大于 0，directional energy ratio 至少为 0.05；
- Wet 为双声道且 `wet_stereo_normalized_difference >= 0.01`；
- `wet_to_reference_rms_ratio >= 0.05`，四轨模式可听区分，通用自动门禁通过；
- 不要求 NearWall 具有非零 late reverb，因为单墙的物理正确结果可以主要由早期反射构成。

### Enclosed

- 恰好 7 个验证 Geometry；
- NearWall 的所有非零、安全、方向性和硬件/CPU 一致性条件继续成立；
- late-reverb gain 必须为正；
- 相比 NearWall，有效路径数、IR energy、directional delay-bin 数和 late-reverb gain 必须增加；
- `IR energy` 和 `wet_to_reference_rms_ratio` 必须分别达到 NearWall 的 1.10 倍；当 NearWall late gain 大于 `1e-9` 时，Enclosed late gain 也必须达到其 1.10 倍，否则只要求 Enclosed late gain 大于 `1e-9`；
- early-reflection gain 不做跨场景大小假设，因为近墙的首达反射可能更强。

## 架构

### 1. 单一配置入口

`script/launch_runtime_validation.py::build_editor_command` 增加 `editor_reflection_environment` 参数，默认仍为 `enclosed`，仅接受已有的 `EDITOR_REFLECTION_ENVIRONMENTS`。命令必须显式携带：

```text
-UERayTracingAudioValidationReflectionEnvironment=<environment>
-UERayTracingAudioValidationReflectionBounces=32
```

`script/validate_visible_editor_ab_scene.py` 增加 `--reflection-environment`，并把环境与 bounce 一起传给上述构造器。固定的普通启动流程保持 Enclosed 默认行为，不因 R3 矩阵改变。

### 2. Editor 验证夹具

Editor 模块先解析并 clamp bounce，再把同一有效值传入 `FUERayTracingAudioEditorValidationScene::EnsureScene` 和 `FUERayTracingAudioEditorArtifactRunner::Start`。验证场景结果新增实际 bounce 字段，并把 Source 的实时 `MaxReflectionBounces` 设置为该值。

严格 scene-ready marker 增加 `reflection_bounces=32`。Python parser 同时校验：

- 环境名称；
- 预期 Geometry 数；
- Source/Listener 距离；
- air profile；
- 实际 bounce。

每次环境运行使用独立 Editor 进程；夹具仍需删除上次环境留下的已标记 Geometry，并拒绝重复或不明确的验证角色。未标记的用户 Actor 不得被修改。

### 3. 四轨产物

现有 Artifact Runner 继续通过真实硬件 Bake 获得 directional stereo IR，并通过 Offline Renderer 生成：

```text
Reference = imported MarchingBand PCM
Direct    = Reference × Direct model
Wet       = Reference convolved with the baked IR × Wet mix
Full      = Direct + Wet
```

每个场景保留自身四条 WAV、IR 资产、四个导入的 SoundWave 资产、截图、Editor 日志和原始 manifest。四轨必须同起点、同长度，并使用各自 manifest 中记录的同一 `common_output_scale`。矩阵还要求三个场景的 `common_output_scale` 一致；如果不一致则失败，不能用不同响度归一化掩盖环境差异。Reference 与 Direct WAV 在三个场景间必须分别字节一致。

### 4. 独立矩阵验证器

新增 `script/validate_reflection_environment_matrix.py`。默认依次运行：

1. `open_space`；
2. `near_wall`；
3. `enclosed`。

每个 case 调用可见 Editor A/B 入口，固定 `clear`、200 cm、default air、4096 rays 和 32 bounce，等待 Bake 完成后解析严格 marker、manifest 与截图。验证器也支持传入三个既有 manifest，便于快速复查和单元测试，但不能在缺少截图、日志 provenance 或 WAV 时宣称完成一次新的端到端矩阵。

最终输出：

```text
Saved/UERayTracingAudio/ListeningAcceptance/
  ReflectionEnvironmentMatrix/<timestamp>/
    OpenSpace.png
    NearWall.png
    Enclosed.png
    ReflectionEnvironmentMatrix_Manifest.json
```

汇总 manifest 保存阈值、三个原始 manifest/日志/截图/WAV 路径、文件 SHA-256、固定配置、每场景指标、跨场景比值和最终 `passed`。成功输出单一严格摘要：

```text
R3_REFLECTION_MATRIX_PASS bounces=32 ... manifest="..."
```

任何失败输出 `R3_REFLECTION_MATRIX_FAIL`、具体字段和对应 case，退出码非零。

## 数值门禁

### 通用门禁

三个场景均要求：

- `hardware_ray_tracing=true`，CPU fallback 不能冒充硬件通过；
- `has_cpu_reference=true`；
- `reflection_bounce_count=32`、`reflection_ray_count=4096`；
- `impulse_response_channels=2`；
- `samples_finite=true`、`audio_safety_checks_passed=true`；
- `clipped_sample_count=0`、`post_scale_peak <= 0.99001`；
- `direct_dropout_window_count=0`、`direct_semantics_passed=true`；
- 所有路径、增益、能量、延迟、方向和 RMS 字段存在且为有限数；
- 四个 WAV、IR 资产、四个导入资产、截图和日志都存在。

### 硬件/CPU 一致性

对非零路径数、indirect gain、early gain、late gain、IR energy 和 directional energy ratio 使用：

```text
relative_delta = abs(hardware - cpu) / max(abs(hardware), abs(cpu), 1e-12)
relative_delta <= 0.05
```

零结果使用绝对容差 `1e-9`。NearWall 与 Enclosed 的硬件/CPU 主到达方向归一化点积必须 `>= 0.99`。任一侧缺少本应存在的非零值都失败，不能以相对误差公式掩盖零值。

### 跨场景一致性与差异

- input asset、Direct preset、距离、air profile、rays、bounces、采样率、声道和帧数一致；
- 三份 Reference SHA-256 相同，三份 Direct SHA-256 相同；
- OpenSpace 的 Direct 与 Full SHA-256 相同；
- NearWall 和 Enclosed 的 Wet/Full 与 OpenSpace 不同；
- Enclosed 的路径、能量、late tail 和 Wet 响度满足前述相对 NearWall 的增长门禁。

## 错误处理

- 未知环境、非法 bounce、缺少或重复严格 marker、manifest provenance 不匹配时立即失败；
- 临时 Editor 提前退出、超时、黑屏、崩溃或无硬件证据时保留日志和已有产物，终止该进程并停止矩阵；
- 若发生 UE 崩溃，立即转入 `workflow/crash-debugging.md`；
- OpenSpace 的 `automatic_checks_passed=false` 由 R3 专用语义接管，但其他安全和 Direct 门禁仍严格执行；
- NearWall/Enclosed 必须保持 `automatic_checks_passed=true`，不得全局放宽 Offline Renderer；
- 验证器不得改写 manifest 以制造通过，也不得自动把 32 降为更小 bounce。

## 测试策略

### C++ Automation（先 RED 后 GREEN）

1. fixture 接收 32 bounce，Source 实际值与结果字段均为 32；
2. 1 和 64 是合法边界，越界值按统一入口 clamp；
3. OpenSpace/NearWall/Enclosed 分别产生 0/1/7 个已标记 Geometry；
4. 环境切换删除陈旧验证 Geometry，同时保留未标记 Actor；
5. scene-ready 和 artifact provenance 都记录相同的实际 bounce 与环境。

### Python（先 RED 后 GREEN）

1. Editor 命令携带环境和 32-bounce 参数，未知环境被拒绝；
2. 可见场景入口正确透传环境；
3. 通过 fixture 覆盖三个环境的完整成功矩阵；
4. 分别拒绝错误输入资产、错误 Geometry 数、非 32 bounce、CPU-only、硬件/CPU 偏差超过 5%、OpenSpace 非零 Wet、NearWall 零反射、Enclosed 无 late tail、跨场景增长不足、不同 Reference/Direct、缺文件和非有限样本；
5. OpenSpace 不要求通用 audible-Wet 门禁，NearWall/Enclosed 仍要求；
6. 汇总 manifest 与成功/失败 marker 可严格解析且不可重复。

### 端到端验证

主要特性完成后按项目约束执行：

```powershell
uv run script\build_and_validate.py
uv run script\validate_reflection_environment_matrix.py
uv run script\launch_runtime_validation.py
```

最后一个固定启动脚本必须保留 Editor 给用户。用户按 `USAGE.md` 打开三场景产物，逐组试听 Reference/Direct/Wet/Full，并在目标设备上判断反射方向、封闭空间 late tail、click/pop 和响度连续性。只有用户本人可以记录 Human Pass。

## 文档与完成标准

实现完成后更新：

- `TODO.md`；
- `USAGE.md`；
- `IMPLEMENTATION_STATUS.md`；
- `progress_log.md`。

R3 矩阵只有在以下条件全部成立时才能标记自动化完成：

- 三个独立 Editor case 都使用实际 32 bounce 和硬件光追；
- 0/1/7 Geometry provenance 正确；
- 三场景四轨、截图、日志、IR 与 manifest 齐全；
- OpenSpace 零 Wet、NearWall 方向性 early reflection、Enclosed early + late reverb 均满足环境专用门禁；
- 硬件/CPU 一致性与跨场景差异门禁通过；
- Python、C++ Automation、规定构建和固定运行验证全部通过；
- 文档明确区分自动化通过与尚待用户确认的 Human Pass。

该 R3 特性完成后，整体插件 goal 仍需继续审计 Realtime/Baked/Hybrid 闭环、波形对比、true multi-PIE 和目标设备人耳验收，不能因本矩阵通过而提前宣称整体完成。
