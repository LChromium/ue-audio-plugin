# Steam Audio Reflection 模拟与 GPU 加速

## 1. Reflection 的物理意义

### 1.1 什么是声音反射

声音反射（Reflection）是声音在传播过程中遇到障碍物表面时，部分声能按一定角度反射回来的物理现象。这是现实环境中普遍存在的声学效果，是**混响（Reverb）**和**早期反射（Early Reflections）**的基础。

### 1.2 Steam Audio 中 Reflection 解决的问题

| 效果类型 | 典型应用场景 |
|---------|-------------|
| **早期反射（Early Reflections）** | 房间中的第一批次反射，如墙壁、天花板、地面反射回来的声音，帮助听众判断空间大小和材质 |
| **后期混响（Late Reverb）** | 大量随机方向的反射叠加，形成扩散的混响尾音 |
| **环境氛围感** | 封闭空间（房间、走廊）与开放空间的听觉区分 |
| **遮挡效果（Occlusion in Reflection）** | 反射路径上被障碍物遮挡时的衰减 |

Reflection 模拟解决的问题：
- **空间定位**：帮助听众感知房间大小和形状
- **材质特性**：不同材质对不同频率的反射率不同
- **遮挡判断**：声音从源到听者的反射路径是否被障碍物阻挡

---

## 2. Reflection 模拟过程概述

### 2.1 整体流程

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Reflection 模拟流程                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. 初始化阶段                                                              │
│     ├── 创建 Simulator (设置 IPL_SIMULATIONFLAGS_REFLECTIONS)               │
│     ├── 创建 ProbeBatch (预计算探针布局)                                     │
│     ├── 提交场景几何数据到 GPU                                              │
│     └── Commit 提交                                                         │
│                                                                             │
│  2. 每帧模拟阶段                                                            │
│     ├── SimulationManager::simulateIndirect()                               │
│     │     └── RadeonRaysReflectionSimulator::simulate()                    │
│     │           ├── 射线生成 (源 → 探针 → 听者)                              │
│     │           ├── GPU 光线追踪 (RadeonRays QueryIntersection)            │
│     │           ├── 命中处理 (着色 + 弹射)                                   │
│     │           ├── 遮挡计算 (GPU sphereOcclusion kernel)                   │
│     │           └── 结果聚合 (生成 IR 脉冲响应)                             │
│     │                                                                         │
│  3. 渲染阶段                                                                │
│     └── ReflectionEffect::Apply() (卷积混响)                                │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 关键参数

- **numRays**: 射线数量（可达 4096 条）
- **numBounces**: 反射次数
- **irDuration**: 脉冲响应持续时间
- **ambisonicsOrder**: Ambisonics 阶数（1-3 阶）

---

## 3. CPU 端调用 GPU 的接口

### 3.1 入口：SimulationManager

**文件**: `core/src/core/simulation_manager.cpp`

```cpp
void SimulationManager::simulateIndirect(const SimulationInputs& inputs,
                                          const std::vector<Source>& sources,
                                          const Listener& listener,
                                          Scene* scene,
                                          ProbeBatchArray* probeBatchArray)
{
    // ... 
    
    // 调用 Indirect Simulator
    if (mIndirectSimulator)
        mIndirectSimulator->simulate(inputs, sources, listener, scene,
                                     mProbeBatchArray.get(), mFrame);
}
```

### 3.2 RadeonRaysReflectionSimulator

**文件**: `core/src/core/radeonrays_reflection_simulator.cpp`

```cpp
void RadeonRaysReflectionSimulator::simulate(const SimulationInputs& inputs,
                                             const std::vector<Source>& sources,
                                             const Listener& listener,
                                             Scene* scene,
                                             ProbeBatchArray* probeBatchArray,
                                             AudioFrame& output)
{
    // 1. 射线生成 - 从源到探针
    generateRays(sources, listeners, probeBatchArray, inputs);

    // 2. GPU 光线追踪 - 寻找射线与场景的交点
    mRadeonRays->api()->QueryIntersection(
        mRays[mCurrentRayBuffer].rrBuffer(),   // 输入射线
        mNumRays.rrBuffer(),                   // 射线数量
        mMaxNumListeners * mMaxNumRays,        // 最大输出数量
        mHits.rrBuffer(),                      // 命中结果
        nullptr, nullptr
    );

    // 3. 多轮反射弹射
    for (auto i = 0; i < numBounces; ++i)
    {
        // GPU 计算球体遮挡 (源/听者周围)
        if (i > 0)
        {
            sphereOcclusion(numSources, sources, numListeners, listeners, numRays);
        }

        // 着色计算 - 计算反射方向和能量衰减
        shadeAndBounce(sources, listeners, i, numBounces, inputs);

        // GPU 阴影射线遮挡查询
        mRadeonRays->api()->QueryOcclusion(
            mShadowRays.rrBuffer(), 
            mNumShadowRays.rrBuffer(),
            mMaxNumSources * mMaxNumRays,
            mOccluded.rrBuffer(), 
            nullptr, nullptr
        );
    }
}
```

### 3.3 核心 GPU API 调用

| API | 作用 | 对应硬件 |
|-----|------|---------|
| `QueryIntersection` | 寻找射线与场景几何的最近交点 | GPU (RadeonRays) |
| `QueryOcclusion` | 检测射线是否被遮挡 | GPU (RadeonRays) |
| `QueryAnyHit` | 检测射线是否与任何物体相交 | 可选 CPU 回退 |

---

## 4. GPU 代码解析

### 4.1 OpenCL Kernel: sphereOcclusion

**文件**: `core/src/core/radeonrays_reflection_simulator.cl`

```opencl
kernel void sphereOcclusion(uint numSources,
                            global const CoordinateSpace* sources,
                            uint numListeners,
                            global const CoordinateSpace* listeners,
                            global Ray* rays,
                            global Hit* hits)
{
    uint numRays = get_global_size(0);
    uint rayIndex = get_global_id(0);

    // 遍历所有听者
    for (int i = 0; i < numListeners; ++i)
    {
        uint index = i * numRays + rayIndex;

        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        // 检测是否与 Listener 球体相交
        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        float listenerSphereHitDistance = raySphereIntersect(
            &rays[index], 
            listeners[i].origin, 
            LISTENER_RADIUS
        );
        
        // 如果射线击中 listener 球体，且距离比场景交点更近
        if (0.0f <= listenerSphereHitDistance && 
            listenerSphereHitDistance < hits[index].uvwt.s3)
        {
            // 清除交点标记，表示该射线被 listener "吸收"
            rays[index].extra.y = 0;
            hits[index].primid = -1;
            return;
        }

        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        // 检测是否与 Source 球体相交
        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        for (int j = 0; j < numSources; ++j)
        {
            float sourceSphereHitDistance = raySphereIntersect(
                &rays[index], 
                sources[j].origin, 
                SOURCE_RADIUS
            );
            
            if (0.0f <= sourceSphereHitDistance && 
                sourceSphereHitDistance < hits[index].uvwt.s3)
            {
                rays[index].extra.y = 0;
                hits[index].primid = -1;
                return;
            }
        }
    }
}
```

**核心逻辑解析**：

1. **并行处理**: 每个工作项处理一条射线，`get_global_id(0)` 获取射线索引
2. **双重循环**: 对所有源-听者组合进行检测
3. **球体相交检测**: 使用 `raySphereIntersect` 判断射线是否穿过源/听者周围的球体
4. **距离比较**: 只有当球体交点比场景几何交点更近时才生效（`hitDistance < hits[index].uvwt.s3`）
5. **结果标记**: 命中后清除 `primid`，表示该射线不再参与后续反射计算

### 4.2 辅助函数: raySphereIntersect

```opencl
// 射线-球体相交检测
float raySphereIntersect(global const Ray* ray, float3 sphereCenter, float sphereRadius)
{
    float3 oc = ray->origin - sphereCenter;
    float a = dot(ray->direction, ray->direction);
    float b = 2.0f * dot(oc, ray->direction);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - 4 * a * c;

    if (discriminant < 0)
        return -1.0f;
    else
        return (-b - sqrt(discriminant)) / (2.0f * a);
}
```

### 4.3 GPU 加速的优势

| 指标 | CPU 实现 | GPU 实现 |
|-----|---------|---------|
| 射线数量 | 数百条 | 数万条 |
| 并行度 | 1-8 核 | 数千计算单元 |
| 延迟 | 高 | 低 |
| 适用场景 | 少量动态源 | 复杂场景多源 |

**GPU 加速的核心**：
1. **批量光线追踪**: 一次提交数千条射线，GPU 并行计算交点
2. **内存局部性**: 场景几何数据一次性加载到 GPU 显存
3. **SIMD 执行**: 每条射线执行相同指令，适合 GPU 并行架构

---

## 5. 数据流总结

```
┌─────────────────┐      ┌──────────────────┐      ┌─────────────────┐
│   Audio Engine  │ ──▶  │ SimulationManager│ ──▶  │ RadeonRays GPU  │
│  (Unity/Unreal) │      │ (CPU)            │      │ (OpenCL)        │
└─────────────────┘      └──────────────────┘      └─────────────────┘
                                  │                         │
                                  ▼                         ▼
                         ┌──────────────────┐      ┌─────────────────┐
                         │ Probe Batch      │      │ Scene BVH       │
                         │ (空间采样点)     │      │ (GPU显存)       │
                         └──────────────────┘      └─────────────────┘
                                  │                         │
                                  ▼                         ▼
                         ┌──────────────────────────────────────────────┐
                         │           Reflection IR 输出                 │
                         │   (各源-听者对的脉冲响应)                     │
                         └──────────────────────────────────────────────┘
```

---

## 6. 相关源文件索引

| 文件 | 说明 |
|-----|------|
| `simulation_manager.cpp/h` | 模拟管理器入口 |
| `radeonrays_reflection_simulator.cpp/h` | 反射模拟器实现 |
| `radeonrays_reflection_simulator.cl` | OpenCL Kernel |
| `radeonrays_device.cpp/h` | RadeonRays 设备封装 |
| `probe_batch.cpp/h` | 探针批处理 |
