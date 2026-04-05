# 反射声（Reflection Sound）模块架构设计

## 1. 概述

### 1.1 目标

基于硬件光追模拟声音的多次弹射，生成脉冲响应（IR）用于卷积混响。

### 1.2 核心流程

基于 Steam Audio 的反射模拟流程：

```
多轮弹射循环（numBounces 次）：
    for (int bounce = 0; bounce < numBounces; ++bounce)
    {
        1. QueryIntersection()      // 硬件光追：找射线与场景交点
        2. shadeAndBounce()        // 在命中点计算能量 + 生成反射线
        3. QueryOcclusion()         // 阴影射线：判断命中点到声源是否被挡
        4. gatherEnergyField()      // 收集能量到脉冲响应直方图
        5. Swap Ray Buffers         // 准备下一轮弹射
    }
```

### 1.3 两种架构方案

本模块提供两种实现方案供选择：

| 方案 | 名称 | 核心思想 | 适用场景 |
|------|------|----------|----------|
| **方案A** | 递归式（Recursive） | 在 ClosestHit Shader 中直接调用 `TraceRay` 进行递归弹射 | 简单场景，bounce次数少（≤3） |
| **方案B** | 迭代式（Iterative/Multi-Pass） | 每轮 bounce 分离为独立的 RDG Pass，通过 Buffer 传递射线数据 | 复杂场景，需要更多 bounce，性能可控 |

---

## 2. 方案A：递归式（Recursive）

### 2.1 核心思想

在 `ClosestHit` Shader 中直接调用 `TraceRay` 发射下一条反射射线，硬件自动管理递归栈。

```
RayGen ──TraceRay──▶ ClosestHit ──TraceRay──▶ ClosestHit ──TraceRay──▶ ...
                            ↑___________________|
```

### 2.2 Shader 架构

#### 2.2.1 ReflectionRGS.usf（简化版）

```hlsl
// ReflectionRGS.usf
// 仅负责初始化射线，弹射逻辑在 ClosestHit 中

[shader("raygeneration")]
void ReflectionRGS()
{
    uint rayIndex = DispatchRaysIndex.x;
    
    // 初始化：从听者位置发射球面采样射线
    float3 rayOrigin = GetListenerPosition();
    float3 rayDir = LoadSphereSample(rayIndex);
    
    // 初始化 Payload
    AcousticPayload payload;
    payload.accumEnergy = float3(1.0f, 1.0f, 1.0f);
    payload.accumDelay = 0.0f;
    payload.bounceCount = 0;
    payload.rayIndex = rayIndex;
    
    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = rayDir;
    ray.TMin = 0.0f;
    ray.TMax = FLT_MAX;
    
    // 发射初始射线，后续弹射在 ClosestHit 中处理
    TraceRay(
        AccelerationStructure,
        RAY_FLAG_NONE,
        0xFF, 0, 0, 0,
        ray,
        payload
    );
}
```

#### 2.2.2 AcousticCHS.usf（递归核心）

```hlsl
// AcousticCHS.usf
// 在 ClosestHit 中处理命中、发射阴影射线、递归发射反射射线

[shader("closesthit")]
void AcousticCHS(inout AcousticPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    // ===== 第1步：获取命中点信息 =====
    float3 hitPoint = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 hitNormal = PrimitiveAttribute(Normal, attr);
    float hitDistance = RayTCurrent();
    uint triangleIndex = PrimitiveIndex();
    
    // 反面检测
    if (dot(hitNormal, WorldRayDirection()) > 0.0f) {
        hitNormal = -hitNormal;
    }
    
    // 获取材质
    FAcousticMaterial mat = LoadMaterial(triangleIndex);
    
    // ===== 第2步：阴影射线检测（到声源） =====
    float3 sourcePos = GetSourcePosition();
    float3 hitToSource = normalize(sourcePos - hitPoint);
    float hitToSourceDist = length(sourcePos - hitPoint);
    
    ShadowPayload shadowPayload;
    shadowPayload.occluded = 0;
    
    RayDesc shadowRay;
    shadowRay.Origin = hitPoint + hitNormal * 0.01f;  // 偏移避免自相交
    shadowRay.Direction = hitToSource;
    shadowRay.TMin = 0.0f;
    shadowRay.TMax = hitToSourceDist;
    
    // 发射阴影射线（使用 Shadow Miss Shader index = 1）
    TraceRay(
        AccelerationStructure,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |  // 找到第一个命中就停
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,           // 不需要执行CHS
        0xFF, 0, 0, 1,                              // MissShaderIndex = 1
        shadowRay,
        shadowPayload
    );
    
    // 如果未被遮挡，累加能量到脉冲响应
    if (shadowPayload.occluded == 0)
    {
        float3 energy = payload.accumEnergy * (float3(1.0f) - mat.absorption);
        float delay = payload.accumDelay + hitDistance / SPEED_OF_SOUND;
        
        // 写入全局能量场（通过 UAV）
        WriteToEnergyField(payload.rayIndex, energy, delay);
    }
    
    // ===== 第3步：递归发射反射射线（如果未达到最大bounce） =====
    if (payload.bounceCount < MAX_BOUNCES)
    {
        // 计算反射方向
        float3 reflectDir = reflect(WorldRayDirection(), hitNormal);
        
        // 漫反射 vs 镜面反射
        if (Random(payload.rayIndex, payload.bounceCount) < mat.scattering) {
            reflectDir = SampleHemisphereCosine(hitNormal, payload.rayIndex, payload.bounceCount);
        }
        
        // 更新 Payload
        payload.accumEnergy *= (float3(1.0f) - mat.absorption);
        payload.accumDelay += hitDistance / SPEED_OF_SOUND;
        payload.bounceCount++;
        
        // 发射反射射线（递归调用）
        RayDesc bounceRay;
        bounceRay.Origin = hitPoint + hitNormal * 0.01f;
        bounceRay.Direction = reflectDir;
        bounceRay.TMin = 0.0f;
        bounceRay.TMax = FLT_MAX;
        
        TraceRay(
            AccelerationStructure,
            RAY_FLAG_NONE,
            0xFF, 0, 0, 0,
            bounceRay,
            payload
        );
    }
}
```

#### 2.2.3 ShadowMS.hlsl（阴影 Miss Shader）

```hlsl
// ShadowMS.hlsl
[shader("miss")]
void ShadowMS(inout ShadowPayload payload)
{
    // 阴影射线未命中 = 到达声源 = 未被遮挡
    payload.occluded = 0;
}
```

### 2.3 CPU 端调度

```cpp
// ReflectionSimulator_Recursive.cpp

void FReflectionSimulatorRecursive::Simulate(
    FSceneInterface* Scene,
    const FAcousticCoordinateSpace& Source,
    const FAcousticCoordinateSpace& Listener,
    int32 NumRays,
    int32 NumBounces,
    FImpulseResponse& OutIR)
{
    // 1. 获取 UE 维护的场景 BVH
    FRayTracingScene* RTScene = Scene->GetRayTracingScene();
    
    // 2. 创建能量场 Buffer
    FRDGBuilder GraphBuilder(RHICmdList);
    FRDGBufferDesc EnergyFieldDesc = FRDGBufferDesc::CreateStructuredDesc(
        sizeof(FEnergyFieldBin),
        NumRays * NUM_BINS
    );
    FRDGBufferRef EnergyFieldBuffer = GraphBuilder.CreateBuffer(EnergyFieldDesc, TEXT("EnergyField"));
    
    // 3. 清除能量场
    AddClearBufferPass(GraphBuilder, EnergyFieldBuffer);
    
    // 4. 构建光追管线（设置 MaxTraceRecursionDepth）
    FRayTracingPipelineStateInitializer PipelineInit;
    PipelineInit.SetRayGenShader(ReflectionRGS);
    PipelineInit.SetHitGroupShader(HitGroup);
    PipelineInit.SetMissShader(MissShader);
    PipelineInit.MaxTraceRecursionDepth = NumBounces + 1;  // +1 for RayGen initial call
    PipelineInit.MaxPayloadSize = sizeof(AcousticPayload); // ≤ 32 bytes
    
    FRayTracingPipelineStateRef Pipeline;
    RHICreateRayTracingPipeline(PipelineInit, Pipeline);
    
    // 5. 单次 DispatchRays（所有弹射在 Shader 内递归完成）
    FRayTracingShaderBindings Bindings;
    Bindings.AccelerationStructure = RTScene->GetAccelerationStructure();
    Bindings.UniformBuffer = CreateUniformBuffer(Source, Listener, NumBounces);
    Bindings.OutputBuffers.Add(EnergyFieldBuffer);
    
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("Reflection Recursive Dispatch"),
        Bindings.GetParameters(),
        ERDGPassFlags::Compute,
        [this, Pipeline, Bindings, NumRays](FRHICommandList& RHICmdList)
        {
            RHICmdList.RayTraceDispatch(
                Pipeline,
                ReflectionRGS,
                Bindings.AccelerationStructure,
                Bindings,
                NumRays,  // Width
                1         // Height
            );
        }
    );
    
    // 6. 执行并读取结果
    GraphBuilder.Execute();
    ReadbackEnergyField(EnergyFieldBuffer, OutIR);
}
```

### 2.4 优缺点分析

#### 优点

| 优点 | 说明 |
|------|------|
| **代码简洁** | 所有逻辑集中在 ClosestHit Shader，无需多 Pass 协调 |
| **延迟更低** | 无 Pass 间同步开销，射线连续追踪 |
| **内存简单** | 不需要额外的射线 Buffer，递归状态在硬件栈上 |

#### 缺点

| 缺点 | 说明 |
|------|------|
| **递归深度限制** | DXR MaxTraceRecursionDepth 通常 ≤ 31，实际可用 bounce ≤ 30 |
| **线程发散** | 不同射线在不同 bounce 深度终止，导致 Wavefront 利用率下降 |
| **寄存器压力** | 深层递归导致寄存器溢出到内存，性能下降 |
| **调试困难** | 递归调用栈深，难以追踪问题 |
| **UE RDG 支持有限** | RDG 更擅长管理多 Pass 依赖，递归模式与 RDG 理念不完全契合 |

### 2.5 适用场景

- ✅ 简单场景，bounce 次数少（1-3次）
- ✅ 原型开发，快速验证算法
- ✅ 对延迟敏感的场景
- ❌ 需要大量 bounce（>5次）的复杂场景
- ❌ 需要精细调试和性能分析的场景

---

## 3. 方案B：迭代式/多Pass（Iterative/Multi-Pass）

### 3.1 核心思想

每轮 bounce 分离为独立的 RDG Pass，射线数据通过 UAV Buffer 在 Pass 间传递，CPU 控制循环。

```
Pass 1: RayGen发射初始射线 ──▶ ClosestHit记录命中 ──▶ 写回HitBuffer
                                    ↓
Pass 2: Compute Shader读取HitBuffer ──▉ 着色计算 ──▉ 生成新射线Buffer
                                    ↓
Pass 3: RayGen从Buffer读取射线 ──▶ ClosestHit记录命中 ──▶ 写回HitBuffer
                                    ↓
Pass 4: Compute Shader批量发射阴影射线
                                    ↓
Pass 5: Compute Shader收集能量到直方图
                                    ↓
循环: Pass 2-5 重复 numBounces 次
```

### 3.2 Shader 架构

#### 3.2.1 数据结构

```hlsl
// SharedTypes.h

// ===== 射线数据（用于 Buffer 传递）=====
struct FRayData {
    float3 Origin;       // 射线起点
    float3 Direction;    // 射线方向
    float3 Energy;       // 当前能量
    float Delay;         // 累计延迟
    uint BounceCount;    // 当前bounce数
    uint RayIndex;       // 原始射线索引（用于能量收集）
    uint IsActive;       // 是否仍然有效（未丢失）
};

// ===== 命中数据（ClosestHit 输出）=====
struct FHitData {
    float3 HitPoint;     // 命中点
    float3 HitNormal;    // 命中法线
    float HitDistance;   // 命中距离
    uint TriangleIndex;  // 三角形索引
    uint IsValid;        // 是否有效命中
};

// ===== 能量场直方图 =====
struct FEnergyFieldBin {
    float LowFreq;       // 低频能量
    float MidFreq;       // 中频能量
    float HighFreq;      // 高频能量
    uint SampleCount;    // 采样计数（用于平均）
};
```

#### 3.2.2 Pass 1: InitialRaysRGS.usf

```hlsl
// InitialRaysRGS.usf
// 从听者位置发射初始射线，写入 RayBuffer

RWStructuredBuffer<FRayData> RayBuffer;

[shader("raygeneration")]
void InitialRaysRGS()
{
    uint rayIndex = DispatchRaysIndex.x;
    
    // 从听者位置发射球面采样射线
    float3 listenerPos = GetListenerPosition();
    float3 rayDir = LoadSphereSample(rayIndex);
    
    RayBuffer[rayIndex].Origin = listenerPos;
    RayBuffer[rayIndex].Direction = rayDir;
    RayBuffer[rayIndex].Energy = float3(1.0f, 1.0f, 1.0f);
    RayBuffer[rayIndex].Delay = 0.0f;
    RayBuffer[rayIndex].BounceCount = 0;
    RayBuffer[rayIndex].RayIndex = rayIndex;
    RayBuffer[rayIndex].IsActive = 1;
}
```

#### 3.2.3 Pass 2: TraceRaysRGS.usf

```hlsl
// TraceRaysRGS.usf
// 从 RayBuffer 读取射线，执行光追，输出到 HitBuffer

StructuredBuffer<FRayData> RayBuffer;
RWStructuredBuffer<FHitData> HitBuffer;

[shader("raygeneration")]
void TraceRaysRGS()
{
    uint rayIndex = DispatchRaysIndex.x;
    FRayData ray = RayBuffer[rayIndex];
    
    // 只处理活跃的射线
    if (ray.IsActive == 0)
    {
        HitBuffer[rayIndex].IsValid = 0;
        return;
    }
    
    RayDesc rayDesc;
    rayDesc.Origin = ray.Origin;
    rayDesc.Direction = ray.Direction;
    rayDesc.TMin = 0.0f;
    rayDesc.TMax = FLT_MAX;
    
    // 简单的 Payload，只传递 rayIndex
    uint payload = rayIndex;
    
    TraceRay(
        AccelerationStructure,
        RAY_FLAG_NONE,
        0xFF, 0, 0, 0,
        rayDesc,
        payload
    );
}

[shader("closesthit")]
void TraceCHS(inout uint payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint rayIndex = payload;
    
    HitBuffer[rayIndex].HitPoint = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    HitBuffer[rayIndex].HitNormal = PrimitiveAttribute(Normal, attr);
    HitBuffer[rayIndex].HitDistance = RayTCurrent();
    HitBuffer[rayIndex].TriangleIndex = PrimitiveIndex();
    HitBuffer[rayIndex].IsValid = 1;
}

[shader("miss")]
void TraceMS(inout uint payload)
{
    uint rayIndex = payload;
    HitBuffer[rayIndex].IsValid = 0;  // 射线丢失
}
```

#### 3.2.4 Pass 3: ShadeAndBounceCS.usf

```hlsl
// ShadeAndBounceCS.usf
// Compute Shader：读取 HitBuffer，计算着色，生成下一轮射线

StructuredBuffer<FRayData> InputRayBuffer;
StructuredBuffer<FHitData> HitBuffer;
RWStructuredBuffer<FRayData> OutputRayBuffer;
RWStructuredBuffer<FEnergyFieldBin> EnergyField;

[numthreads(64, 1, 1)]
void ShadeAndBounceCS(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint rayIndex = DispatchThreadID.x;
    uint numRays = GetNumRays();
    
    if (rayIndex >= numRays) return;
    
    FRayData ray = InputRayBuffer[rayIndex];
    FHitData hit = HitBuffer[rayIndex];
    
    // 如果射线不活跃或丢失，保持不活跃状态
    if (ray.IsActive == 0 || hit.IsValid == 0)
    {
        OutputRayBuffer[rayIndex] = ray;
        OutputRayBuffer[rayIndex].IsActive = 0;
        return;
    }
    
    // 获取材质
    FAcousticMaterial mat = LoadMaterial(hit.TriangleIndex);
    
    // 计算反射方向
    float3 reflectDir = reflect(ray.Direction, hit.HitNormal);
    if (Random(rayIndex, ray.BounceCount) < mat.scattering) {
        reflectDir = SampleHemisphereCosine(hit.HitNormal, rayIndex, ray.BounceCount);
    }
    
    // 更新能量和延迟
    float3 newEnergy = ray.Energy * (float3(1.0f) - mat.absorption);
    float newDelay = ray.Delay + hit.HitDistance / SPEED_OF_SOUND;
    
    // 写入输出射线 Buffer（用于下一轮）
    OutputRayBuffer[rayIndex].Origin = hit.HitPoint + hit.HitNormal * 0.01f;
    OutputRayBuffer[rayIndex].Direction = reflectDir;
    OutputRayBuffer[rayIndex].Energy = newEnergy;
    OutputRayBuffer[rayIndex].Delay = newDelay;
    OutputRayBuffer[rayIndex].BounceCount = ray.BounceCount + 1;
    OutputRayBuffer[rayIndex].RayIndex = ray.RayIndex;
    OutputRayBuffer[rayIndex].IsActive = (ray.BounceCount + 1 < MAX_BOUNCES) ? 1 : 0;
}
```

#### 3.2.5 Pass 4: ShadowRaysCS.usf

```hlsl
// ShadowRaysCS.usf
// Compute Shader：批量处理阴影射线检测
// 注意：DXR 不支持 Compute Shader 直接调用 TraceRay
// 所以需要使用 RayGen 或 RHI 的 QueryOcclusion

// 方案A：使用单独的 RayGen Pass
StructuredBuffer<FHitData> HitBuffer;
StructuredBuffer<float3> SourcePositions;  // 声源位置
RWStructuredBuffer<uint> ShadowResults;    // 0=未遮挡, 1=遮挡

[numthreads(64, 1, 1)]
void PrepareShadowRaysCS(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint hitIndex = DispatchThreadID.x;
    FHitData hit = HitBuffer[hitIndex];
    
    if (hit.IsValid == 0) return;
    
    // 准备阴影射线数据（写入全局 Buffer，供后续 RayGen 使用）
    float3 sourcePos = GetSourcePosition();
    float3 hitToSource = normalize(sourcePos - hit.HitPoint);
    float hitToSourceDist = length(sourcePos - hit.HitPoint);
    
    WriteShadowRayData(hitIndex, hit.HitPoint, hitToSource, hitToSourceDist);
}

// 然后使用单独的 RayGen Pass 发射所有阴影射线
```

#### 3.2.6 Pass 5: GatherEnergyCS.usf

```hlsl
// GatherEnergyCS.usf
// Compute Shader：收集能量到脉冲响应直方图

StructuredBuffer<FRayData> RayBuffer;
StructuredBuffer<FHitData> HitBuffer;
StructuredBuffer<uint> ShadowResults;
RWStructuredBuffer<FEnergyFieldBin> EnergyField;

[numthreads(64, 1, 1)]
void GatherEnergyCS(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint rayIndex = DispatchThreadID.x;
    uint numRays = GetNumRays();
    
    if (rayIndex >= numRays) return;
    
    FRayData ray = RayBuffer[rayIndex];
    FHitData hit = HitBuffer[rayIndex];
    
    if (hit.IsValid == 0) return;
    
    // 检查阴影结果
    bool occluded = (ShadowResults[rayIndex] != 0);
    if (occluded) return;
    
    // 计算到声源的距离和延迟
    float3 sourcePos = GetSourcePosition();
    float distToSource = length(sourcePos - hit.HitPoint);
    float totalDelay = ray.Delay + distToSource / SPEED_OF_SOUND;
    
    // 计算bin索引
    uint binIndex = GetBinIndex(totalDelay);
    if (binIndex >= NUM_BINS) return;
    
    // 原子累加能量
    uint fieldIndex = ray.RayIndex * NUM_BINS + binIndex;
    InterlockedAdd(EnergyField[fieldIndex].LowFreq,  asuint(ray.Energy.r));
    InterlockedAdd(EnergyField[fieldIndex].MidFreq,   asuint(ray.Energy.g));
    InterlockedAdd(EnergyField[fieldIndex].HighFreq,  asuint(ray.Energy.b));
    InterlockedAdd(EnergyField[fieldIndex].SampleCount, 1);
}
```

### 3.3 CPU 端调度

```cpp
// ReflectionSimulator_Iterative.cpp

void FReflectionSimulatorIterative::Simulate(
    FSceneInterface* Scene,
    const FAcousticCoordinateSpace& Source,
    const FAcousticCoordinateSpace& Listener,
    int32 NumRays,
    int32 NumBounces,
    FImpulseResponse& OutIR)
{
    FRayTracingScene* RTScene = Scene->GetRayTracingScene();
    FRDGBuilder GraphBuilder(RHICmdList);
    
    // ===== 创建 Buffer =====
    FRDGBufferDesc RayDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FRayData), NumRays);
    FRDGBufferDesc HitDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FHitData), NumRays);
    FRDGBufferDesc EnergyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FEnergyFieldBin), NumRays * NUM_BINS);
    
    FRDGBufferRef RayBufferA = GraphBuilder.CreateBuffer(RayDesc, TEXT("RayBufferA"));
    FRDGBufferRef RayBufferB = GraphBuilder.CreateBuffer(RayDesc, TEXT("RayBufferB"));
    FRDGBufferRef HitBuffer = GraphBuilder.CreateBuffer(HitDesc, TEXT("HitBuffer"));
    FRDGBufferRef EnergyField = GraphBuilder.CreateBuffer(EnergyDesc, TEXT("EnergyField"));
    
    // ===== Pass 1: 初始化射线 =====
    {
        FInitialRaysRGS::FParameters* PassParams = GraphBuilder.AllocParameters<FInitialRaysRGS::FParameters>();
        PassParams->ListenerPosition = Listener.Origin;
        PassParams->NumRays = NumRays;
        PassParams->RayBuffer = GraphBuilder.CreateUAV(RayBufferA);
        
        GraphBuilder.AddPass(
            RDG_EVENT_NAME("InitializeRays"),
            PassParams,
            ERDGPassFlags::Compute,
            [this, PassParams, NumRays](FRHICommandList& RHICmdList)
            {
                // 使用 Compute Shader 初始化
                FComputeShaderUtils::Dispatch(RHICmdList, InitialRaysCS, *PassParams, 
                    FIntVector(FMath::DivideAndRoundUp(NumRays, 64), 1, 1));
            }
        );
    }
    
    // ===== 多轮弹射循环 =====
    FRDGBufferRef CurrentRayBuffer = RayBufferA;
    FRDGBufferRef NextRayBuffer = RayBufferB;
    
    for (int32 bounce = 0; bounce < NumBounces; ++bounce)
    {
        // Pass 2: 追踪射线
        AddTraceRaysPass(GraphBuilder, RTScene, CurrentRayBuffer, HitBuffer, NumRays);
        
        // Pass 3: 着色计算 + 生成反射射线
        AddShadeAndBouncePass(GraphBuilder, CurrentRayBuffer, HitBuffer, NextRayBuffer, 
            EnergyField, Source, NumRays, bounce);
        
        // Pass 4: 阴影射线检测（批量）
        AddShadowRaysPass(GraphBuilder, RTScene, HitBuffer, Source, NumRays);
        
        // Pass 5: 收集能量
        AddGatherEnergyPass(GraphBuilder, CurrentRayBuffer, HitBuffer, ShadowResults, 
            EnergyField, Source, NumRays);
        
        // 交换 Buffer
        Swap(CurrentRayBuffer, NextRayBuffer);
    }
    
    // ===== 执行并读取结果 =====
    GraphBuilder.Execute();
    ReadbackEnergyField(EnergyField, OutIR);
}
```

### 3.4 优缺点分析

#### 优点

| 优点 | 说明 |
|------|------|
| **无递归深度限制** | 理论上 bounce 次数只受内存限制 |
| **性能可控** | 每轮独立调度，便于分析和优化 |
| **调试友好** | 每轮 Pass 可单独调试、可视化 |
| **RDG 集成好** | 自然利用 UE 的渲染依赖图优化 |
| **灵活性高** | 可动态调整每轮的参数、算法 |
| **批量优化** | 阴影射线可批量发射，减少发散 |

#### 缺点

| 缺点 | 说明 |
|------|------|
| **代码复杂** | 需要管理多个 Shader 和 Buffer |
| **内存占用** | 需要额外的射线/命中 Buffer |
| **Pass 开销** | 每轮有额外的调度开销 |
| **延迟稍高** | Pass 间同步引入延迟 |

### 3.5 适用场景

- ✅ 需要大量 bounce（>5次）的复杂场景
- ✅ 需要精细性能分析和调试的场景
- ✅ 与 UE 渲染管线深度集成的场景
- ✅ 需要动态调整参数的场景
- ❌ 对延迟极度敏感的场景
- ❌ 简单原型验证

---

## 4. 方案对比总结

| 对比项 | 递归式（方案A） | 迭代式（方案B） |
|--------|----------------|----------------|
| **代码复杂度** | 低（单 Shader） | 高（多 Shader + Buffer 管理） |
| **最大 Bounce** | 受硬件限制（~30） | 理论上无限制 |
| **性能稳定性** | 深层递归性能下降 | 每轮稳定 |
| **线程发散** | 严重 | 可控 |
| **内存使用** | 低（硬件栈） | 高（显式 Buffer） |
| **调试难度** | 高 | 低 |
| **RDG 集成** | 一般 | 优秀 |
| **延迟** | 低 | 稍高 |
| **灵活性** | 低 | 高 |

---

## 5. 推荐选择

### 5.1 决策流程

```
开始
  │
  ▼
是否需要 >5 次 bounce？
  │
  ├── 是 ──▶ 选择迭代式（方案B）
  │
  └── 否 ──▶ 是否需要精细调试/性能分析？
              │
              ├── 是 ──▶ 选择迭代式（方案B）
              │
              └── 否 ──▶ 是否追求最低延迟？
                          │
                          ├── 是 ──▶ 选择递归式（方案A）
                          │
                          └── 否 ──▶ 推荐迭代式（方案B）
```

### 5.2 最终建议

**推荐选择：迭代式（方案B）**

理由：
1. **与 UE 生态更契合**：RDG 是 UE5 的核心架构，迭代式更符合其设计理念
2. **可扩展性更好**：未来可能需要更多 bounce 或更复杂的算法
3. **调试友好**：开发阶段更容易定位和解决问题
4. **性能可控**：便于后续优化和调优

**例外情况选择递归式（方案A）**：
- 快速原型验证
- 明确只需要 1-3 次 bounce 的简单场景
- 对延迟极度敏感的特殊场景

---

## 6. 参考资料

- Steam Audio GPU 光追分析：`../doc/gpu_raytracing_analysis.md`
- Steam Audio 反射模拟分析：`../doc/reflection_analysis.md`
- DXR 规范：[DirectX Raytracing (DXR) Functional Spec](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)
- UE RHI 光追文档：[Hardware Ray Tracing in Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-ray-tracing-in-unreal-engine)
