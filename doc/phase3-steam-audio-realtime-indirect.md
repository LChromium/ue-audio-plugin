# Phase 3 调研：Steam Audio 的实时间接声链路

## 1. 先说结论

Steam Audio 的实时间接声并不是像直接声遮挡那样，最终只给出一个 `0-1` 的单值。

更准确地说：

- 直接声输出里会有类似 `occlusion` 这样的标量。
- 实时间接声输出里，核心结果是 **反射相关的时域 / 能量域数据**。
- 对于不同反射算法，最终输出形式不同：
  - `Convolution` / `TAN`：输出 **Impulse Response (IR)**。
  - `Parametric`：输出 **3 个频段的 reverb times (RT60)**。
  - `Hybrid`：同时输出 **IR + parametric reverb 参数**。

所以如果问：

- “实时间接声光追得到的是不是一个 `0-1` 值？”

答案是：

- **不是。**

更接近的答案是：

- **Steam Audio 的实时间接声会先通过路径追踪得到能量场，再重建为 IR，或估计为参数化混响参数。**

## 2. Steam Audio 在 API 层暴露的输出是什么

Steam Audio 的模拟输出结构里，反射结果直接放在：

- `IPLSimulationOutputs.reflections`

定义可以看这里：

- [phonon.h](file:///c:/tasks/steam-audio/core/src/core/phonon.h#L4104-L4114)

其中：

- `direct` 是直接声输出
- `reflections` 是间接声输出
- `pathing` 是路径规划输出

而 `reflections` 的类型是 `IPLReflectionEffectParams`，这个结构明确包含：

- `ir`
- `reverbTimes`
- `eq`
- `delay`
- `numChannels`
- `irSize`

定义可以看这里：

- [phonon.h](file:///c:/tasks/steam-audio/core/src/core/phonon.h#L2500-L2533)

## 3. Steam Audio 的反射算法类型

Steam Audio 的反射效果类型在 `IPLReflectionEffectType` 里定义，主要有四种：

- `IPL_REFLECTIONEFFECTTYPE_CONVOLUTION`
- `IPL_REFLECTIONEFFECTTYPE_PARAMETRIC`
- `IPL_REFLECTIONEFFECTTYPE_HYBRID`
- `IPL_REFLECTIONEFFECTTYPE_TAN`

定义与注释在这里：

- [phonon.h](file:///c:/tasks/steam-audio/core/src/core/phonon.h#L2461-L2486)

这些类型的意义非常关键：

### 3.1 Convolution

- 反射到达监听器的结果被编码成 **Impulse Response**
- 这是细节最完整的方式
- CPU 开销较高

### 3.2 Parametric

- 不直接保存每个回声
- 把反射声场压缩成少量参数
- 主要是 **reverb decay / RT60** 一类参数
- CPU 开销更低

### 3.3 Hybrid

- 前半段用 IR
- 后半段估计成参数化混响
- 是效果与开销之间的折中

### 3.4 TAN

- 本质仍然是卷积型 IR
- 只是把卷积处理放到 AMD TrueAudio Next / GPU 路径

## 4. 实时间接声在 Steam Audio 内部怎么跑

Steam Audio 的 `SimulationManager::simulateIndirect()` 基本概括了整条间接声链路：

- 先做实时反射模拟
- 再查 baked reflections
- 然后按反射类型重建 IR 或估计 reverb
- 最后把结果分区 / 上传，供后续 effect 使用

关键代码在：

- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L195-L257)

### 4.1 实时反射不是直接出 IR，而是先出 EnergyField

`simulateRealTimeReflections()` 会调用 `ReflectionSimulator::simulate(...)`

- 输入是 source / listener / numRays / numBounces / duration / order
- 输出目标是 `EnergyField`

相关代码：

- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L260-L308)
- [reflection_simulator.h](file:///c:/tasks/steam-audio/core/src/core/reflection_simulator.h#L56-L70)

而 `reflection_simulator.h` 的注释写得很清楚：

- 反射模拟是用 **Monte Carlo path tracing**
- 输出之一是每个 source 对应的 `EnergyField`

这意味着：

- **实时光追阶段本身并不是直接生成最终音频 IR**
- 而是先得到一个“按时间 / 方向 / 频带组织的反射能量表示”

## 5. EnergyField 之后如何变成可渲染结果

### 5.1 Convolution / TAN / Hybrid 会重建 IR

在 `simulateIndirect()` 里，如果不是纯 parametric：

- 会调用 `reconstructImpulseResponses()`

相关代码：

- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L215-L219)
- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L442-L491)

这个阶段会：

- 从 `EnergyField`
- 加上距离修正和空气吸收
- 重建成 `ImpulseResponse`

之后在 `partitionImpulseResponses()` 中：

- 对 IR 做 partition
- 生成 overlap-save FIR
- 或上传到 TAN

相关代码：

- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L563-L599)

所以：

- **Convolution / TAN / Hybrid 的实时反射最终确实会得到 IR**

### 5.2 Parametric / Hybrid 会估计 Reverb Times

如果反射类型是 `Parametric` 或 `Hybrid`：

- 会调用 `estimateReverb()`

相关代码：

- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L221-L223)
- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L493-L527)

这里会从 `EnergyField` 估计：

- `reverb.reverbTimes[i]`

而 `Hybrid` 还会进一步：

- 从 IR 估计 hybrid EQ 和 delay

相关代码：

- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L529-L544)

所以：

- **Parametric 不是输出 IR**
- **而是输出 3 频段的 RT60 等参数**

## 6. API 最终返回给上层的是什么

在 `CSource::getOutputs()` 里，Steam Audio 会把内部结果拷贝到 `IPLSimulationOutputs.reflections`：

- `ir`
- `numChannels`
- `irSize`
- `reverbTimes`
- `eq`
- `delay`
- `tanSlot`

代码在：

- [api_simulator.cpp](file:///c:/tasks/steam-audio/core/src/core/api_simulator.cpp#L457-L469)

这已经明确说明：

- 反射输出不是一个 `0-1`
- 而是一个“供 reflection effect 消费”的结构体

## 7. UE 插件层如何消费这些实时反射结果

在 Steam Audio 的 Unreal 插件里：

- SourceComponent 会把 `IPL_SIMULATIONFLAGS_REFLECTIONS` 打开
- 然后通过 `iplSourceGetOutputs(...)` 拿到 `Outputs.reflections`

代码在：

- [SteamAudioSourceComponent.cpp](file:///c:/tasks/steam-audio/unreal/src/SteamAudioUnreal/Plugins/SteamAudio/Source/SteamAudio/Private/SteamAudioSourceComponent.cpp#L63-L114)
- [SteamAudioSourceComponent.cpp](file:///c:/tasks/steam-audio/unreal/src/SteamAudioUnreal/Plugins/SteamAudio/Source/SteamAudio/Private/SteamAudioSourceComponent.cpp#L117-L146)

随后在 Reverb 插件里：

- `ReflectionParams = Outputs.reflections`
- 再调用 `iplReflectionEffectApply(...)`

代码在：

- [SteamAudioReverb.cpp](file:///c:/tasks/steam-audio/unreal/src/SteamAudioUnreal/Plugins/SteamAudio/Source/SteamAudio/Private/SteamAudioReverb.cpp#L434-L442)

这说明 UE 插件层并不是自己解释一个标量，而是：

- 把反射模拟输出作为 reflection effect 的参数直接喂给音频处理链

## 8. IR 是不是只用于烘焙

答案也是：

- **不是。**

实时反射在 `Convolution` / `Hybrid` / `TAN` 模式下，同样会生成或使用 IR。

证据：

- `simulateIndirect()` 里实时路径会走 `reconstructImpulseResponses()`
- `getOutputs()` 里会把 `reflectionOutputs.overlapSaveFIR` 作为 `outputs->reflections.ir`

可参考：

- [simulation_manager.cpp](file:///c:/tasks/steam-audio/core/src/core/simulation_manager.cpp#L215-L219)
- [api_simulator.cpp](file:///c:/tasks/steam-audio/core/src/core/api_simulator.cpp#L457-L469)

更准确地说：

- **IR 既用于实时，也用于烘焙**

区别在于：

- **实时**：每次运行模拟后重建 / 更新 IR
- **烘焙**：预先把反射相关数据存到 probe / baked data 中，运行时只做查找和应用

## 9. 烘焙到底保存什么

`IPLReflectionsBakeFlags` 明确区分了两类可烘焙数据：

- `BAKECONVOLUTION`
- `BAKEPARAMETRIC`

定义在：

- [phonon.h](file:///c:/tasks/steam-audio/core/src/core/phonon.h#L3480-L3488)

也就是说，Steam Audio 的 baked reflections 也不是只存一种东西：

- 可以烘焙用于卷积 / 混合模式的 IR 数据
- 也可以烘焙用于 parametric / hybrid 的混响参数

而 `IPLReflectionsBakeParams` 里又区分：

- `simulatedDuration`
- `savedDuration`

并且注释明确说：

- 可以用更长的 IR 来估计 parametric reverb
- 但只保存 IR 的前半段

相关位置：

- [phonon.h](file:///c:/tasks/steam-audio/core/src/core/phonon.h#L3520-L3533)

这也进一步说明：

- **IR 不是“只在 bake 时才存在”**
- **IR 是 Steam Audio 反射系统中的基础中间 / 最终表示之一**

## 10. 对我们做 Phase 3 的启发

如果我们要做自己的 Phase 3：间接声实时链路，不能把目标理解成：

- “做一个类似 direct occlusion 的 `0-1` 间接声值”

更合理的目标应当是：

### 10.1 最小版本

- 光追得到多条反射路径或能量累积结果
- 先构造一个简化版 `EnergyField` / histogram / delay-energy buffer
- 再从这个结果重建：
  - 简化 IR
  - 或 3-band RT60

### 10.2 中间版本

- 支持两条渲染路径：
  - `Convolution` 风格：输出 IR
  - `Parametric` 风格：输出 RT60 / EQ / delay

### 10.3 最终版本

- 做 `Hybrid`
- 早期反射保留时域细节
- 尾部混响转参数化，降低实时成本

## 11. 一句话结论

如果只用一句话总结 Steam Audio 的 Phase 3 思路：

- **Steam Audio 的实时间接声不是输出一个 `0-1` 值，而是先通过 Monte Carlo 路径追踪生成能量场，再根据反射算法类型重建为 IR、RT60，或 IR+参数化混响的组合；IR 并不只用于烘焙，实时反射同样会使用 IR。**
