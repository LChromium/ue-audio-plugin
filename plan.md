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
