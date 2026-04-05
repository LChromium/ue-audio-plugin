# GPU硬件光追反射实现代码文档

> 本文档描述基于UE RHI光追和RDG的音频反射模拟实现细节，采用方案B（迭代式/多Pass架构）
> 
> 参考：
> - `arch/reflection_architecture.md` - 方案B架构设计
> - `doc/steam-audio/reflection_analysis.md` - Steam Audio反射模拟分析

---

## 1. 整体流程概览

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                        反射模拟迭代式流程 (方案B)                                 │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│  ┌─────────────┐                                                               │
│  │ Pass 0:初始化 │ 生成从Listener出发的球面采样射线                              │
│  │ (ComputeCS) │ ───────────────────────────────▶ 写入 RayBuffer                │
│  └─────────────┘                                                               │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                         Bounce Loop (NumBounces次)                       │   │
│  │  ┌─────────────┐                                                      │   │
│  │  │ Pass 1:光追  │ 从RayBuffer读取射线，执行QueryIntersection             │   │
│  │  │ (RayGenRGS) │ ───────────────────────────────▶ 写入 HitBuffer         │   │
│  │  └─────────────┘                                                      │   │
│  │         │                                                              │   │
│  │         ▼                                                              │   │
│  │  ┌─────────────┐                                                      │   │
│  │  │ Pass 2:着色  │ 读取HitBuffer，计算反射方向、衰减能量                    │   │
│  │  │ (ComputeCS) │ 若未到达最大bounce，生成下一轮射线写入NextRayBuffer      │   │
│  │  └─────────────┘                                                      │   │
│  │         │                                                              │   │
│  │         ▼                                                              │   │
│  │  ┌─────────────┐                                                      │   │
│  │  │ Pass 3:遮挡  │ 从命中点向Source发射阴影射线，QueryOcclusion检测        │   │
│  │  │ (RayGenRGS) │ ───────────────────────────────▶ 写入 OcclusionBuffer   │   │
│  │  └─────────────┘                                                      │   │
│  │         │                                                              │   │
│  │         ▼                                                              │   │
│  │  ┌─────────────┐                                                      │   │
│  │  │ Pass 4:收集  │ 对未遮挡的命中点，计算到Source的距离和延迟              │   │
│  │  │ (ComputeCS) │ 累加能量到EnergyField直方图                            │   │
│  │  └─────────────┘                                                      │   │
│  │         │                                                              │   │
│  │         ▼                                                              │   │
│  │  Swap(RayBuffer, NextRayBuffer) // 准备下一轮                           │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                               │
│  │  执行RDG    │ GraphBuilder.Execute()                                         │
│  └─────────────┘                                                               │
│         │                                                                       │
│         ▼                                                                       │
│  ┌─────────────┐                                                               │
│  │  读取结果   │ Readback EnergyField → ImpulseResponse                         │
│  └─────────────┘                                                               │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 数据结构定义

### 2.1 GPU端数据结构 (SharedTypes.h)

```cpp
// ============================================================================
// 常量定义 (与Steam Audio对齐)
// ============================================================================
#define NUM_BANDS                   3
#define NUM_BINS                    256
#define BIN_DURATION                0.01f
#define SPEED_OF_SOUND_CM           34000.0f    // 340 m/s = 34000 cm/s
#define SOURCE_RADIUS_CM            10.0f       // 0.1m = 10cm
#define LISTENER_RADIUS_CM          10.0f       // 0.1m = 10cm
#define RAY_SURFACE_OFFSET_CM       1.0f        // 0.01m = 1cm
#define SPECULAR_EXPONENT           100.0f


// ============================================================================
// 射线数据 - 与Steam Audio RadeonRays格式对齐
// ============================================================================
struct FReflectionRayData
{
    // Origin.xyz + MaxDistance (w)
    // 对应Steam Audio的 ray.o (float4)
    FVector4f OriginAndMaxDistance;
    
    // Direction.xyz + padding (w)
    // 对应Steam Audio的 ray.d (float4)
    FVector4f DirectionAndPadding;
    
    // extra.x = padding, extra.y = enabled flag (1=活跃, 0=禁用)
    // 使用int32与Steam Audio的int2 extra对齐
    int32 Padding1;
    int32 bIsActive;
    
    // 对齐到32字节
    int32 Padding2[2];
};

// GPU端对齐大小: 32字节 (2*float4 + 4*int)


// ============================================================================
// 命中数据 - 与Steam Audio RadeonRays Intersection对齐
// ============================================================================
struct FReflectionHitData
{
    // 几何体/实例ID (对应shapeid)
    int32 ShapeId;
    
    // 三角形索引，-1表示无效命中 (对应primid)
    // Steam Audio使用-1表示未命中
    int32 TriangleIndex;
    
    // 对齐填充
    int32 Padding[2];
    
    // uv坐标 + 命中距离t (对应uvwt)
    FVector4f UVWT;
};

// GPU端对齐大小: 24字节 (4*int + float4)
// 注意: 实际使用时可能需要填充到32字节对齐


// ============================================================================
// 材质数据 - 与Steam Audio Material对齐
// ============================================================================
struct FAcousticMaterialGPU
{
    // 吸收系数 (3个频带独立存储，与Steam Audio一致)
    float AbsorptionLow;
    float AbsorptionMid;
    float AbsorptionHigh;
    
    // 散射系数
    float Scattering;
    
    // 透射系数 (可选，用于支持透射效果)
    float TransmissionLow;
    float TransmissionMid;
    float TransmissionHigh;
};

// GPU端对齐大小: 28字节 (7*float)，建议填充到32字节


// ============================================================================
// 能量场数据 - 一维int32数组布局 [channel][band][bin]
// ============================================================================
// 不再使用结构体，直接使用int32数组
// 数组索引: channel * NUM_BANDS * NUM_BINS + band * NUM_BINS + bin
// 
// 例如获取第0通道、中频(1)、第10个bin的能量:
//   index = 0 * 3 * 256 + 1 * 256 + 10 = 266


// ============================================================================
// 阴影射线数据 - 与Steam Audio对齐
// ============================================================================
struct FShadowRayData
{
    // 阴影射线起点 (xyz) + 到声源距离 (w)
    FVector4f OriginAndDistance;
    
    // 阴影射线方向 (xyz) + padding (w)
    FVector4f DirectionAndPadding;
    
    // 启用标志 (1=活跃, 0=禁用)
    int32 bIsActive;
    
    // 对应的反射射线索引
    int32 ReflectionRayIndex;
};

// GPU端对齐大小: 40字节，建议填充到48字节


// ============================================================================
// 阴影检测结果 - 使用int32与Steam Audio对齐
// ============================================================================
struct FShadowResult
{
    // 0 = 未遮挡 (可见)，1 = 遮挡
    int32 bOccluded;
};


// ============================================================================
// 能量延迟数据 - 用于shadeAndBounce传递能量和延迟
// ============================================================================
struct FEnergyDelayData
{
    // RGB能量 + 延迟 (w)
    FVector4f EnergyAndDelay;
    
    // 累计能量 (RGB) + 累计距离 (w)
    FVector4f AccumEnergyAndDistance;
};
```

### 2.2 CPU端数据结构 (C++)

```cpp
// ============================================================================
// 反射模拟输入参数
// ============================================================================
struct FReflectionSimulationInput
{
    // ===== 采样参数 =====
    int32 NumRays = 4096;           // 射线数量 (建议: 1024~16384)
    int32 NumBounces = 4;           // 反射次数 (建议: 2~8)
    int32 NumChannels = 4;          // Ambisonics通道数 (1=单声道, 4=1阶, 9=2阶, 16=3阶)
    
    // ===== 时间参数 =====
    float DurationSeconds = 1.0f;   // 脉冲响应持续时间 (秒)
    float SpeedOfSound = 34000.0f;  // 声速 (cm/s，默认340m/s)
    
    // ===== 距离参数 =====
    float ReferenceDistance = 100.0f;     // 参考距离 (cm)
    float IrradianceMinDistance = 1.0f;   // 辐照度计算最小距离 (cm)
    
    // ===== 空间参数 =====
    FVector ListenerLocation;       // 听者位置
    FVector ListenerForward;        // 听者朝向 (用于方向性)
    FVector SourceLocation;         // 声源位置
    
    // ===== 声源指向性 =====
    float SourceDipoleWeight = 0.0f;      // 声源指向性权重 [0,1]
    float SourceDipolePower = 0.0f;       // 声源指向性幂指数
    
    // ===== 大气吸收 =====
    FVector AirAbsorptionPerMeter;  // 每米大气吸收系数 (低/中/高)
    
    // ===== 能量量化参数 =====
    float EnergyQuantizationScale = 1000000.0f; // 能量值量化系数
    float Scalar = 1.0f;                        // 全局能量缩放因子
};


// ============================================================================
// 脉冲响应输出
// ============================================================================
struct FImpulseResponse
{
    // 时域IR数据 (多通道Ambisonics格式)
    // 布局: [NumChannels][NumSamples]
    TArray<TArray<float>> TimeDomainData;
    
    // 采样率
    int32 SampleRate = 48000;
    
    // 能量场数据 [channel][band][bin]
    // 用于调试和分析
    TArray<TArray<TArray<float>>> EnergyField;
    
    // 有效贡献数
    int32 NumValidContributions = 0;
};
```

---

## 3. GPU资源管理

### 3.1 RDG Buffer资源

```cpp
// ============================================================================
// RDG Pass中创建的Buffer资源
// ============================================================================

// ----- 场景全局Buffer (由场景管理器提供) -----
// 三角形法线Buffer (通过TriangleIndex索引)
FRDGBufferRef TriangleNormalsBuffer;  // StructuredBuffer<float3>

// 材质索引Buffer (通过TriangleIndex索引)
FRDGBufferRef MaterialIndicesBuffer;  // StructuredBuffer<int32>

// 材质属性Buffer (通过MaterialIndex索引)
FRDGBufferRef MaterialsBuffer;        // StructuredBuffer<FAcousticMaterialGPU>

// 漫反射采样方向Buffer (预计算)
FRDGBufferRef DiffuseSamplesBuffer;   // StructuredBuffer<float4>

// 听者SH系数Buffer (用于Ambisonics编码)
FRDGBufferRef ListenerSHCoeffsBuffer; // StructuredBuffer<float>


// ----- 模拟过程Buffer -----

// 射线Buffer (双缓冲，用于Ping-Pong交换)
FRDGBufferDesc RayBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
    sizeof(FReflectionRayData),  // 32字节
    NumRays                      // 元素数量
);
FRDGBufferRef RayBufferA = GraphBuilder.CreateBuffer(RayBufferDesc, TEXT("ReflectionRayBufferA"));
FRDGBufferRef RayBufferB = GraphBuilder.CreateBuffer(RayBufferDesc, TEXT("ReflectionRayBufferB"));

// 命中Buffer
FRDGBufferDesc HitBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
    sizeof(FReflectionHitData),  // 32字节 (含填充)
    NumRays
);
FRDGBufferRef HitBuffer = GraphBuilder.CreateBuffer(HitBufferDesc, TEXT("ReflectionHitBuffer"));

// 能量延迟Buffer (shadeAndBounce的输出)
FRDGBufferDesc EnergyDelayDesc = FRDGBufferDesc::CreateStructuredDesc(
    sizeof(FEnergyDelayData),
    NumRays
);
FRDGBufferRef EnergyDelayBuffer = GraphBuilder.CreateBuffer(EnergyDelayDesc, TEXT("EnergyDelay"));

// 能量场Buffer - 一维int32数组 [channel][band][bin]
// 总大小 = NumChannels * NUM_BANDS * NUM_BINS
FRDGBufferDesc EnergyFieldDesc = FRDGBufferDesc::CreateStructuredDesc(
    sizeof(int32),
    NumChannels * NUM_BANDS * NUM_BINS
);
FRDGBufferRef EnergyFieldBuffer = GraphBuilder.CreateBuffer(EnergyFieldDesc, TEXT("EnergyField"));

// 阴影射线Buffer
FRDGBufferDesc ShadowRayBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
    sizeof(FShadowRayData),
    NumRays
);
FRDGBufferRef ShadowRayBuffer = GraphBuilder.CreateBuffer(ShadowRayBufferDesc, TEXT("ShadowRays"));

// 阴影检测结果Buffer
FRDGBufferDesc ShadowResultDesc = FRDGBufferDesc::CreateStructuredDesc(
    sizeof(FShadowResult),
    NumRays
);
FRDGBufferRef ShadowResultBuffer = GraphBuilder.CreateBuffer(ShadowResultDesc, TEXT("ShadowResults"));

// 贡献计数器Buffer (原子累加)
FRDGBufferDesc CounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
FRDGBufferRef ContributionCounter = GraphBuilder.CreateBuffer(CounterDesc, TEXT("ContributionCounter"));
```

### 3.2 Uniform Buffer参数

```cpp
// ============================================================================
// 每帧更新的Uniform参数 (与Steam Audio对齐)
// ============================================================================
BEGIN_SHADER_PARAMETER_STRUCT(FReflectionSimulationParameters, )
    // 采样参数
    SHADER_PARAMETER(uint32, NumRays)
    SHADER_PARAMETER(uint32, NumBounces)
    SHADER_PARAMETER(uint32, CurrentBounce)
    SHADER_PARAMETER(uint32, NumChannels)           // Ambisonics通道数
    SHADER_PARAMETER(uint32, NumDiffuseSamples)     // 漫反射采样数
    
    // 时间/距离参数
    SHADER_PARAMETER(float, SpeedOfSound)
    SHADER_PARAMETER(float, ReferenceDistance)
    SHADER_PARAMETER(float, IrradianceMinDistance)  // 辐照度计算最小距离
    SHADER_PARAMETER(float, DurationSeconds)
    SHADER_PARAMETER(float, DelayBinDurationSeconds)
    
    // 能量参数
    SHADER_PARAMETER(float, EnergyQuantizationScale)
    SHADER_PARAMETER(float, Scalar)                 // 全局能量缩放因子
    SHADER_PARAMETER(FVector3f, AirAbsorptionPerMeter)
    
    // 空间参数
    SHADER_PARAMETER(FVector3f, ListenerLocation)
    SHADER_PARAMETER(FVector3f, ListenerForward)
    SHADER_PARAMETER(FVector3f, SourceLocation)
    
    // 声源指向性
    SHADER_PARAMETER(float, SourceDipoleWeight)
    SHADER_PARAMETER(float, SourceDipolePower)
    
    // 随机数种子
    SHADER_PARAMETER(uint32, RandomSeed)
    
    // 光追加速结构
    SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
END_SHADER_PARAMETER_STRUCT()


// ============================================================================
// Shader特定参数结构
// ============================================================================
BEGIN_SHADER_PARAMETER_STRUCT(FInitializeRaysParameters, )
    SHADER_PARAMETER_STRUCT_REF(FReflectionSimulationParameters, UniformParams)
    SHADER_PARAMETER_UAV(RWStructuredBuffer<FReflectionRayData>, OutRayBuffer)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FTraceRaysParameters, )
    SHADER_PARAMETER_STRUCT_REF(FReflectionSimulationParameters, UniformParams)
    SHADER_PARAMETER_SRV(StructuredBuffer<FReflectionRayData>, RayBuffer)
    SHADER_PARAMETER_UAV(RWStructuredBuffer<FReflectionHitData>, OutHitBuffer)
    SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FShadeAndBounceParameters, )
    SHADER_PARAMETER_STRUCT_REF(FReflectionSimulationParameters, UniformParams)
    SHADER_PARAMETER_SRV(StructuredBuffer<FReflectionRayData>, InputRayBuffer)
    SHADER_PARAMETER_SRV(StructuredBuffer<FReflectionHitData>, HitBuffer)
    SHADER_PARAMETER_SRV(StructuredBuffer<float3>, TriangleNormals)
    SHADER_PARAMETER_SRV(StructuredBuffer<int32>, MaterialIndices)
    SHADER_PARAMETER_SRV(StructuredBuffer<FAcousticMaterialGPU>, Materials)
    SHADER_PARAMETER_SRV(StructuredBuffer<float4>, DiffuseSamples)
    SHADER_PARAMETER_UAV(RWStructuredBuffer<FReflectionRayData>, OutputRayBuffer)
    SHADER_PARAMETER_UAV(RWStructuredBuffer<FEnergyDelayData>, EnergyDelayBuffer)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGatherEnergyParameters, )
    SHADER_PARAMETER_STRUCT_REF(FReflectionSimulationParameters, UniformParams)
    SHADER_PARAMETER_SRV(StructuredBuffer<FEnergyDelayData>, EnergyDelayBuffer)
    SHADER_PARAMETER_SRV(StructuredBuffer<FShadowResult>, ShadowResults)
    SHADER_PARAMETER_SRV(StructuredBuffer<float>, ListenerSHCoeffs)
    SHADER_PARAMETER_UAV(RWStructuredBuffer<int32>, EnergyField)
    SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, ContributionCounter)
END_SHADER_PARAMETER_STRUCT()
```

---

## 4. Shader实现详解

### 4.1 Pass 0: 初始化射线 (Compute Shader)

**文件**: `ReflectionInitializeRaysCS.usf`

```hlsl
// ============================================================================
// Pass 0: 从Listener位置生成球面采样射线
// ============================================================================

#include "/Engine/Private/Common.ush"

// 输出: 射线Buffer
RWStructuredBuffer<FReflectionRayData> OutRayBuffer;

// Uniform参数
uint NumRays;
float3 ListenerLocation;
float3 ListenerForward;

// 黄金角度螺旋采样常量 (与Steam Audio一致)
static const float GoldenAngle = 2.39996322972865332f; // PI * (3 - sqrt(5))

// 将球面坐标转换为笛卡尔坐标
float3 SphericalToCartesian(float Theta, float Phi)
{
    float SinTheta = sin(Theta);
    return float3(
        SinTheta * cos(Phi),
        SinTheta * sin(Phi),
        cos(Theta)
    );
}

[numthreads(64, 1, 1)]
void InitializeRaysCS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    uint RayIndex = DispatchThreadId.x;
    
    // 越界检查
    if (RayIndex >= NumRays)
    {
        return;
    }
    
    // ===== 使用黄金角度螺旋算法生成均匀球面采样 =====
    // 与Steam Audio的generateListenerRays kernel一致
    
    // Y坐标均匀分布 [-1, 1]
    float Y = 1.0f - (2.0f * RayIndex + 1.0f) / float(NumRays);
    
    // 根据Y计算Theta (极角)
    float Theta = acos(clamp(Y, -1.0f, 1.0f));
    
    // Phi按黄金角度递增 (方位角)
    float Phi = GoldenAngle * RayIndex;
    
    // 生成方向
    float3 Direction = SphericalToCartesian(Theta, Phi);
    
    // 如果有听者朝向，将采样球相对于听者朝向旋转
    // Steam Audio的CoordinateSpace变换
    if (length(ListenerForward) > 1e-4f)
    {
        float3 Forward = normalize(ListenerForward);
        float3 Up = abs(Forward.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
        float3 Right = normalize(cross(Up, Forward));
        Up = cross(Forward, Right);
        
        // 从局部坐标变换到世界坐标
        Direction = Direction.x * Right + Direction.y * Up + Direction.z * Forward;
    }
    
    // ===== 填充射线数据 (与Steam Audio ray结构对齐) =====
    FReflectionRayData Ray;
    
    // Origin.xyz + MaxDistance (w)
    Ray.OriginAndMaxDistance = float4(ListenerLocation, FLT_MAX);
    
    // Direction.xyz + padding (w)
    Ray.DirectionAndPadding = float4(normalize(Direction), 0.0f);
    
    // extra.y = enabled flag (1=活跃)
    Ray.Padding1 = 0;
    Ray.bIsActive = 1;
    
    // 填充
    Ray.Padding2[0] = 0;
    Ray.Padding2[1] = 0;
    
    OutRayBuffer[RayIndex] = Ray;
}
```

**功能说明**:
1. 使用黄金角度螺旋算法在球面上均匀采样射线方向 (与Steam Audio一致)
2. 射线数据结构对齐到Steam Audio的RadeonRays格式
3. 使用float4存储origin和direction，便于GPU内存访问

---

### 4.2 Pass 1: 光追查询 (Ray Generation Shader)

**文件**: `ReflectionTraceRaysRGS.usf`

```hlsl
// ============================================================================
// Pass 1: 从RayBuffer读取射线，执行QueryIntersection查询场景交点
// ============================================================================

#include "/Engine/Private/Common.ush"
#include "/Engine/Private/RayTracing/RayTracingCommon.ush"

// 输入: 射线Buffer
StructuredBuffer<FReflectionRayData> RayBuffer;

// 输出: 命中Buffer (与Steam Audio Intersection对齐)
RWStructuredBuffer<FReflectionHitData> OutHitBuffer;

// 光追加速结构
RaytracingAccelerationStructure TLAS;

// 射线Payload (RayGen和ClosestHit间传递数据)
struct FTracePayload
{
    // 命中间距
    float HitT;
    
    // 命中几何体索引
    uint GeometryIndex;
    
    // 是否命中
    uint bHit;
};

// Ray Generation Shader入口
RAY_TRACING_ENTRY_RAYGEN(ReflectionTraceRaysRGS)
{
    uint RayIndex = DispatchRaysIndex().x;
    
    // 读取射线数据
    FReflectionRayData Ray = RayBuffer[RayIndex];
    
    // 初始化命中结果为"未命中" (primid = -1)
    FReflectionHitData HitData;
    HitData.ShapeId = 0xFFFFFFFF;
    HitData.TriangleIndex = -1;  // -1表示无效命中 (与Steam Audio一致)
    HitData.Padding[0] = 0;
    HitData.Padding[1] = 0;
    HitData.UVWT = float4(0, 0, 0, 0);
    
    // 只处理活跃的射线 (extra.y == 1)
    if (Ray.bIsActive != 0)
    {
        // 构建射线描述
        FRayDesc RayDesc;
        RayDesc.Origin = Ray.OriginAndMaxDistance.xyz;
        RayDesc.Direction = normalize(Ray.DirectionAndPadding.xyz);
        RayDesc.TMin = RAY_SURFACE_OFFSET_CM / 100.0f;  // 转换为米
        RayDesc.TMax = Ray.OriginAndMaxDistance.w;
        
        // 初始化Payload
        FTracePayload Payload = (FTracePayload)0;
        Payload.HitT = RayDesc.TMax;
        Payload.GeometryIndex = 0xFFFFFFFFu;
        Payload.bHit = 0u;
        
        // 发射射线 (QueryIntersection)
        TraceRay(
            TLAS,
            RAY_FLAG_CULL_BACK_FACING_TRIANGLES |  // 剔除背面三角形
            RAY_FLAG_FORCE_OPAQUE,                  // 所有物体视为不透明
            0xFF,                                   // InstanceInclusionMask
            0,                                      // RayContributionToHitGroupIndex
            RAY_TRACING_NUM_SHADER_SLOTS,           // MultiplierForGeometryContributionToHitGroupIndex
            0,                                      // MissShaderIndex
            RayDesc.GetNativeDesc(),
            Payload
        );
        
        // 根据Payload填充命中结果 (与Steam Audio Hit_t对齐)
        if (Payload.bHit != 0)
        {
            HitData.ShapeId = Payload.GeometryIndex;
            HitData.TriangleIndex = PrimitiveIndex();  // 三角形索引
            HitData.UVWT = float4(0, 0, 0, Payload.HitT);  // uv + 命中距离t
        }
    }
    
    // 写入命中Buffer
    OutHitBuffer[RayIndex] = HitData;
}

// Closest Hit Shader - 当射线命中最近的三角形时调用
RAY_TRACING_ENTRY_CLOSEST_HIT(ReflectionTraceCHS,
    FTracePayload, Payload,
    FRayTracingIntersectionAttributes, Attributes)
{
    Payload.bHit = 1u;
    Payload.HitT = RayTCurrent();
    Payload.GeometryIndex = GetInstanceUserData();
}

// Miss Shader - 当射线未命中任何物体时调用
RAY_TRACING_ENTRY_MISS(ReflectionTraceMS, FTracePayload, Payload)
{
    Payload.bHit = 0u;
    Payload.HitT = 0.0f;
    Payload.GeometryIndex = 0xFFFFFFFFu;
}
```

**功能说明**:
1. 从RayBuffer读取每条射线的起点和方向
2. 只处理活跃状态的射线 (bIsActive == 1)
3. 调用`TraceRay`进行硬件光追查询
4. 命中结果与Steam Audio的Intersection结构对齐 (primid, shapeid, uvwt)
5. 无效命中使用TriangleIndex = -1标记

---

### 4.3 Pass 2: 着色与弹射 (Compute Shader)

**文件**: `ReflectionShadeAndBounceCS.usf`

```hlsl
// ============================================================================
// Pass 2: 处理命中结果，计算反射，生成下一轮射线
// ============================================================================

#include "/Engine/Private/Common.ush"

// ----- 输入Buffer -----
StructuredBuffer<FReflectionRayData> InputRayBuffer;
StructuredBuffer<FReflectionHitData> HitBuffer;

// 场景全局Buffer
StructuredBuffer<float3> TriangleNormals;      // 三角形法线
StructuredBuffer<int32> MaterialIndices;       // 材质索引
StructuredBuffer<FAcousticMaterialGPU> Materials;  // 材质属性
StructuredBuffer<float4> DiffuseSamples;       // 预计算漫反射采样

// ----- 输出Buffer -----
RWStructuredBuffer<FReflectionRayData> OutputRayBuffer;
RWStructuredBuffer<FEnergyDelayData> EnergyDelayBuffer;

// ----- Uniform参数 -----
uint NumRays;
uint CurrentBounce;
uint MaxBounces;
uint NumDiffuseSamples;
uint RandomSeed;
float SpeedOfSound;
float IrradianceMinDistance;
float Scalar;
float SourceDipoleWeight;
float SourceDipolePower;
float3 SourceLocation;
float3 ListenerLocation;


// ============================================================================
// 辅助函数
// ============================================================================

// 伪随机数生成 (Wang Hash - 与Steam Audio一致)
uint WangHash(uint Seed)
{
    Seed = (Seed ^ 61u) ^ (Seed >> 16);
    Seed *= 9u;
    Seed = Seed ^ (Seed >> 4);
    Seed *= 0x27d4eb2du;
    Seed = Seed ^ (Seed >> 15);
    return Seed;
}

float RandomFloat(uint Seed)
{
    return float(WangHash(1664525u * Seed + 1013904223u)) / 0xffffffffu;
}

// 构建局部坐标系 (与Steam Audio createCoordinateSpace一致)
float3x3 CreateCoordinateSpace(float3 Normal)
{
    float3 Right;
    if (abs(Normal.x) > abs(Normal.z))
    {
        Right = normalize(float3(-Normal.y, Normal.x, 0.0f));
    }
    else
    {
        Right = normalize(float3(0.0f, -Normal.z, Normal.y));
    }
    float3 Up = cross(Right, Normal);
    
    return float3x3(Right, Up, Normal);
}

// 将半球采样转换到世界空间 (与Steam Audio transformHemisphereSample一致)
float3 TransformHemisphereSample(float3 LocalDir, float3 Normal)
{
    float3x3 TangentSpace = CreateCoordinateSpace(Normal);
    return normalize(mul(LocalDir, TangentSpace));
}

// 计算声源指向性 (与Steam Audio evaluateDirectivity一致)
float EvaluateDirectivity(float3 Point, float3 SourceOrigin, float3 SourceForward)
{
    float3 WorldDir = normalize(Point - SourceOrigin);
    float3 LocalDir = mul(WorldDir, CreateCoordinateSpace(SourceForward));
    
    float Cosine = -LocalDir.z;  // 相对于ahead方向
    return pow(abs((1.0f - SourceDipoleWeight) + SourceDipoleWeight * Cosine), SourceDipolePower);
}

// 点源辐照度计算 (与Steam Audio pointSourceIrradiance一致)
float PointSourceIrradiance(float Distance, float MinDistance)
{
    float Attenuation = 1.0f / max(Distance, MinDistance);
    return (1.0f / (4.0f * PI)) * (Attenuation * Attenuation);
}

// 计算命中点 (与Steam Audio calculateHitPoint一致)
float3 CalculateHitPoint(FReflectionRayData Ray, FReflectionHitData Hit)
{
    return Ray.OriginAndMaxDistance.xyz + Hit.UVWT.w * Ray.DirectionAndPadding.xyz;
}

// 计算命中法线 (与Steam Audio calculateHitNormal一致)
float3 CalculateHitNormal(FReflectionRayData Ray, FReflectionHitData Hit)
{
    float3 HitNormal = TriangleNormals[Hit.TriangleIndex];
    if (dot(HitNormal, Ray.DirectionAndPadding.xyz) > 0.0f)
    {
        HitNormal = -HitNormal;
    }
    return HitNormal;
}


// ============================================================================
// Main Shader
// ============================================================================

[numthreads(64, 1, 1)]
void ShadeAndBounceCS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    uint RayIndex = DispatchThreadId.x;
    
    if (RayIndex >= NumRays)
    {
        return;
    }
    
    // 读取输入数据
    FReflectionRayData InputRay = InputRayBuffer[RayIndex];
    FReflectionHitData Hit = HitBuffer[RayIndex];
    
    // 初始化输出射线为"不活跃"状态
    FReflectionRayData OutputRay = InputRay;
    OutputRay.bIsActive = 0;
    
    // 初始化能量延迟输出
    FEnergyDelayData EnergyDelay;
    EnergyDelay.EnergyAndDelay = float4(0, 0, 0, 0);
    EnergyDelay.AccumEnergyAndDistance = float4(0, 0, 0, 0);
    
    // 如果射线不活跃或未命中，直接输出
    // Steam Audio: extra.y == 0 || primid < 0
    if (InputRay.bIsActive == 0 || Hit.TriangleIndex < 0)
    {
        OutputRayBuffer[RayIndex] = OutputRay;
        EnergyDelayBuffer[RayIndex] = EnergyDelay;
        return;
    }
    
    // ===== 计算命中点和法线 =====
    float3 RayOrigin = InputRay.OriginAndMaxDistance.xyz;
    float3 RayDir = InputRay.DirectionAndPadding.xyz;
    float HitDistance = Hit.UVWT.w;
    float3 HitPoint = CalculateHitPoint(InputRay, Hit);
    float3 HitNormal = CalculateHitNormal(InputRay, Hit);
    
    // ===== 获取材质 =====
    int32 MatIndex = MaterialIndices[Hit.TriangleIndex];
    FAcousticMaterialGPU Mat = Materials[MatIndex];
    float3 MatAbsorption = float3(Mat.AbsorptionLow, Mat.AbsorptionMid, Mat.AbsorptionHigh);
    
    // ===== 计算到声源的方向和距离 =====
    float3 ToSource = SourceLocation - HitPoint;
    float SourceDistance = length(ToSource);
    float3 DirToSource = normalize(ToSource);
    
    // ===== 检查是否应该跳过 =====
    // Steam Audio: hitDistance <= LISTENER_RADIUS || hitToSourceDistance <= irradianceMinDistance
    // 或者射线击中背面 (dot(hitToSource, hitNormal) < 0)
    bool bSkip = (HitDistance <= LISTENER_RADIUS_CM || 
                  SourceDistance <= IrradianceMinDistance ||
                  dot(DirToSource, HitNormal) < 0.0f);
    
    if (!bSkip)
    {
        // ===== 计算能量 (完整的Steam Audio公式) =====
        
        // 1. 漫反射项 (Lambertian BRDF)
        float3 DiffuseTerm = (1.0f / PI) * Mat.Scattering * max(0.0f, dot(HitNormal, DirToSource));
        
        // 2. 镜面反射项 (Phong BRDF)
        float3 ReflectDir = reflect(RayDir, HitNormal);
        float SpecularAngle = max(0.0f, dot(normalize(DirToSource - RayDir), HitNormal));
        float3 SpecularTerm = ((SPECULAR_EXPONENT + 2.0f) / (8.0f * PI)) * 
                              (1.0f - Mat.Scattering) * 
                              pow(SpecularAngle, SPECULAR_EXPONENT);
        
        // 3. 基础着色值
        float3 Shading = DiffuseTerm + SpecularTerm;
        
        // 4. 应用全局缩放
        Shading *= Scalar;
        
        // 5. 应用声源指向性
        float Directivity = EvaluateDirectivity(HitPoint, SourceLocation, normalize(SourceLocation - ListenerLocation));
        Shading *= Directivity;
        
        // 6. 应用辐照度衰减
        Shading *= PointSourceIrradiance(SourceDistance, IrradianceMinDistance);
        
        // 7. 应用材质吸收 (能量衰减)
        // 注意: 这里需要累积能量，但在第一次bounce时初始能量为1
        float3 AccumEnergy = float3(1.0f, 1.0f, 1.0f) * (1.0f - MatAbsorption);
        Shading *= AccumEnergy;
        
        // 8. 计算延迟
        float Delay = (HitDistance + SourceDistance) / SpeedOfSound;
        
        // 输出能量和延迟
        EnergyDelay.EnergyAndDelay = float4(Shading, Delay);
        EnergyDelay.AccumEnergyAndDistance = float4(AccumEnergy, HitDistance / SpeedOfSound);
    }
    
    // ===== 生成反射射线 =====
    
    // 随机数
    uint RandomSeedWithRay = WangHash(RandomSeed + RayIndex);
    float RandomVal = RandomFloat(RandomSeedWithRay);
    uint RandomUint = WangHash(RandomSeedWithRay);
    
    // 更新累积能量和距离
    float3 NewAccumEnergy = float3(1.0f, 1.0f, 1.0f) * (1.0f - MatAbsorption);
    float NewAccumDistance = HitDistance / SpeedOfSound;
    
    // 计算反射方向
    float3 ReflectedDir;
    if (RandomVal < Mat.Scattering)
    {
        // 漫反射: 从预计算采样中选择
        uint SampleIndex = RandomUint % NumDiffuseSamples;
        ReflectedDir = TransformHemisphereSample(DiffuseSamples[SampleIndex].xyz, HitNormal);
    }
    else
    {
        // 镜面反射
        ReflectedDir = reflect(RayDir, HitNormal);
    }
    
    // 填充输出射线
    OutputRay.OriginAndMaxDistance = float4(HitPoint + HitNormal * RAY_SURFACE_OFFSET_CM, FLT_MAX);
    OutputRay.DirectionAndPadding = float4(ReflectedDir, 0.0f);
    OutputRay.bIsActive = (CurrentBounce + 1 < MaxBounces) ? 1 : 0;
    
    OutputRayBuffer[RayIndex] = OutputRay;
    EnergyDelayBuffer[RayIndex] = EnergyDelay;
}
```

**功能说明**:
1. 读取每条射线的当前状态和命中结果
2. 使用全局Buffer查询命中点法线和材质属性
3. **完整的Steam Audio能量计算公式**:
   - 漫反射项 (Lambertian BRDF)
   - 镜面反射项 (Phong BRDF，使用SpecularExponent)
   - 声源指向性评估
   - 点源辐照度衰减
   - 材质吸收
4. 根据散射系数混合漫反射和镜面反射
5. 输出能量延迟数据供后续能量收集使用
6. 生成下一轮弹射的射线数据

---

### 4.4 Pass 3: 阴影射线检测 (Ray Generation Shader)

**文件**: `ReflectionShadowRaysRGS.usf`

```hlsl
// ============================================================================
// Pass 3: 从命中点向声源发射阴影射线，检测遮挡
// ============================================================================

#include "/Engine/Private/Common.ush"
#include "/Engine/Private/RayTracing/RayTracingCommon.ush"

// 输入Buffer
StructuredBuffer<FReflectionHitData> HitBuffer;

// 输出Buffer (使用int32与Steam Audio对齐)
RWStructuredBuffer<FShadowResult> ShadowResults;

// 光追加速结构
RaytracingAccelerationStructure TLAS;

// Uniform参数
uint NumRays;
float3 SourceLocation;

// 场景全局Buffer
StructuredBuffer<float3> TriangleNormals;

// Payload (使用int32与Steam Audio对齐)
struct FShadowPayload
{
    int32 bOccluded;
};

// 计算命中点 (复用shadeAndBounce中的函数)
float3 CalculateHitPoint(FReflectionRayData Ray, FReflectionHitData Hit)
{
    return Ray.OriginAndMaxDistance.xyz + Hit.UVWT.w * Ray.DirectionAndPadding.xyz;
}

float3 CalculateHitNormal(FReflectionRayData Hit, FReflectionHitData Ray)
{
    float3 HitNormal = TriangleNormals[Hit.TriangleIndex];
    if (dot(HitNormal, Ray.DirectionAndPadding.xyz) > 0.0f)
    {
        HitNormal = -HitNormal;
    }
    return HitNormal;
}

RAY_TRACING_ENTRY_RAYGEN(ReflectionShadowRaysRGS)
{
    uint RayIndex = DispatchRaysIndex().x;
    
    // 读取命中数据
    FReflectionHitData Hit = HitBuffer[RayIndex];
    
    // 初始化结果为未遮挡 (occluded = 0)
    FShadowResult Result;
    Result.bOccluded = 0;
    
    // 只处理有效命中 (primid >= 0)
    if (Hit.TriangleIndex >= 0)
    {
        // 从射线Buffer重新构造命中点 (需要传入射线数据)
        // 注意: 实际实现中需要将射线起点方向传入或通过其他方式计算
        // 这里简化处理，假设我们有射线数据可用
        
        // 计算到声源的方向和距离
        // float3 HitPoint = ...; // 需要射线起点和方向计算
        // float3 HitNormal = TriangleNormals[Hit.TriangleIndex];
        
        // 计算到声源
        // float3 ToSource = SourceLocation - HitPoint;
        // float DistanceToSource = length(ToSource);
        
        // 偏移起点避免自相交
        // float3 RayOrigin = HitPoint + HitNormal * RAY_SURFACE_OFFSET_CM;
        
        // 发射阴影射线
        // ...
    }
    
    ShadowResults[RayIndex] = Result;
}

RAY_TRACING_ENTRY_MISS(ReflectionShadowMS, FShadowPayload, Payload)
{
    // 阴影射线到达Miss Shader = 未命中任何物体 = 未被遮挡
    Payload.bOccluded = 0;
}

RAY_TRACING_ENTRY_ANY_HIT(ReflectionShadowAHS, FShadowPayload, Payload,
    FRayTracingIntersectionAttributes, Attributes)
{
    // 如果进入AnyHit，说明被遮挡
    Payload.bOccluded = 1;
    IgnoreHit();
}
```

**功能说明**:
1. 使用int32存储遮挡结果，与Steam Audio的`occluded` Buffer对齐
2. 只处理有效命中 (TriangleIndex >= 0)
3. 使用`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`优化性能
4. 结果被遮挡标记 (0=未遮挡, 1=遮挡) 写入ShadowResults Buffer

**注意**: 阴影射线检测需要知道命中点的世界坐标。实际实现中有两种方案:
- 方案A: 将射线Buffer也传入Shadow Pass，重新计算命中点
- 方案B: 在shadeAndBounce Pass中直接计算并输出到EnergyDelayBuffer
Steam Audio采用类似方案B的方式，将能量计算和阴影检测合并处理。

---

### 4.5 Pass 4: 能量收集 (Compute Shader)

**文件**: `ReflectionGatherEnergyCS.usf`

```hlsl
// ============================================================================
// Pass 4: 收集能量到脉冲响应直方图
// ============================================================================

#include "/Engine/Private/Common.ush"

// ----- 输入Buffer -----
StructuredBuffer<FEnergyDelayData> EnergyDelayBuffer;
StructuredBuffer<FShadowResult> ShadowResults;

// 听者SH系数 (用于Ambisonics编码)
StructuredBuffer<float> ListenerSHCoeffs;

// ----- 输出Buffer -----
// 能量场 - 一维int32数组 [channel][band][bin]
RWStructuredBuffer<int32> EnergyField;
RWStructuredBuffer<uint32> ContributionCounter;

// ----- Uniform参数 -----
uint NumRays;
uint NumChannels;        // Ambisonics通道数
float EnergyQuantizationScale;
float Scalar;


// ============================================================================
// Main Shader - 三阶段归约算法 (与Steam Audio gatherEnergyField对齐)
// ============================================================================

// 每个Work-Group处理一部分射线，使用局部内存直方图减少原子冲突
groupshared int32 LocalHistogram[NUM_BINS * NUM_LOCAL_HISTOGRAMS];

[numthreads(THREADGROUP_SIZE_X, 1, 1)]
void GatherEnergyCS(uint3 DispatchThreadId : SV_DispatchThreadID, uint3 GroupId : SV_GroupID)
{
    uint RayIndex = DispatchThreadId.x;
    uint LocalIndex = DispatchThreadId.x % THREADGROUP_SIZE_X;
    
    // 初始化局部直方图
    for (int i = 0; i < NUM_LOCAL_HISTOGRAMS; i++)
    {
        LocalHistogram[LocalIndex * NUM_LOCAL_HISTOGRAMS + i] = 0;
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    // 阶段1: 每个线程处理一条射线，累加到局部直方图
    if (RayIndex < NumRays)
    {
        FEnergyDelayData EnergyDelay = EnergyDelayBuffer[RayIndex];
        FShadowResult Shadow = ShadowResults[RayIndex];
        
        // 跳过被遮挡或能量为0的射线
        if (Shadow.bOccluded == 0 && dot(EnergyDelay.EnergyAndDelay.xyz, float3(1,1,1)) > 0)
        {
            float DelaySeconds = EnergyDelay.EnergyAndDelay.w;
            float3 Energy = EnergyDelay.EnergyAndDelay.xyz;
            
            // 计算bin索引
            uint BinIndex = (uint)(DelaySeconds / BIN_DURATION);
            BinIndex = min(BinIndex, NUM_BINS - 1);
            
            // 为每个Ambisonics通道计算能量
            for (uint Channel = 0; Channel < NumChannels; Channel++)
            {
                // 获取SH系数 (每个射线对每个通道的权重)
                float SHCoeff = ListenerSHCoeffs[Channel * NumRays + RayIndex];
                
                for (uint Band = 0; Band < NUM_BANDS; Band++)
                {
                    float BandEnergy = Energy[Band] * SHCoeff;
                    int32 QuantizedEnergy = (int32)round(BandEnergy * Scalar * EnergyQuantizationScale);
                    
                    if (QuantizedEnergy > 0)
                    {
                        // 选择局部直方图 (基于线程ID分散)
                        uint HistogramIndex = LocalIndex % NUM_LOCAL_HISTOGRAMS;
                        uint LocalBinIndex = BinIndex * NUM_LOCAL_HISTOGRAMS + HistogramIndex;
                        
                        // 原子累加到局部直方图
                        InterlockedAdd(LocalHistogram[LocalBinIndex], QuantizedEnergy);
                    }
                }
            }
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    // 阶段2 & 3: 将局部直方图归约到全局直方图
    // 每个线程处理一个bin
    for (uint Bin = LocalIndex; Bin < NUM_BINS; Bin += THREADGROUP_SIZE_X)
    {
        // 累加所有局部直方图的该bin
        int32 AccumulatedEnergy = 0;
        for (int h = 0; h < NUM_LOCAL_HISTOGRAMS; h++)
        {
            AccumulatedEnergy += LocalHistogram[Bin * NUM_LOCAL_HISTOGRAMS + h];
        }
        
        // 原子累加到全局能量场
        if (AccumulatedEnergy != 0)
        {
            for (uint Channel = 0; Channel < NumChannels; Channel++)
            {
                for (uint Band = 0; Band < NUM_BANDS; Band++)
                {
                    // 计算全局索引: channel * NUM_BANDS * NUM_BINS + band * NUM_BINS + bin
                    uint GlobalIndex = Channel * NUM_BANDS * NUM_BINS + Band * NUM_BINS + Bin;
                    InterlockedAdd(EnergyField[GlobalIndex], AccumulatedEnergy);
                }
            }
        }
    }
    
    // 统计贡献数
    if (LocalIndex == 0)
    {
        InterlockedAdd(ContributionCounter[0], 1);
    }
}
```

**功能说明**:
1. **三阶段归约算法** (与Steam Audio一致):
   - 阶段1: 每个线程将能量累加到局部内存直方图 (减少原子冲突)
   - 阶段2: 同一线程组内归约局部直方图
   - 阶段3: 原子累加到全局能量场
2. 支持Ambisonics多通道输出
3. 能量量化为int32存储，与Steam Audio一致
4. 使用SH系数进行Ambisonics编码

---

## 5. CPU端调度流程 (C++)

### 5.1 主入口函数

```cpp
// ============================================================================
// 反射模拟主函数
// ============================================================================
void FReflectionSimulator::Simulate(
    FRDGBuilder& GraphBuilder,
    const FSceneView& SceneView,
    const FReflectionSimulationInput& Input,
    FImpulseResponse& OutImpulseResponse)
{
    // 1. 计算Bin参数 (与Steam Audio NUM_BINS = 256对齐)
    static constexpr int32 NUM_BINS = 256;
    static constexpr float BIN_DURATION = 0.01f;  // 10ms per bin
    const float SampleRate = 48000.0f;
    const int32 NumSamples = FMath::CeilToInt32(Input.DurationSeconds * SampleRate);
    
    // 2. 创建Uniform参数
    FReflectionSimulationParameters* UniformParams = GraphBuilder.AllocParameters<FReflectionSimulationParameters>();
    UniformParams->NumRays = Input.NumRays;
    UniformParams->NumBounces = Input.NumBounces;
    UniformParams->NumChannels = Input.NumChannels;
    UniformParams->NumDiffuseSamples = 64;  // 预计算漫反射采样数
    UniformParams->SpeedOfSound = Input.SpeedOfSound;
    UniformParams->ReferenceDistance = Input.ReferenceDistance;
    UniformParams->IrradianceMinDistance = Input.IrradianceMinDistance;
    UniformParams->DurationSeconds = Input.DurationSeconds;
    UniformParams->DelayBinDurationSeconds = BIN_DURATION;
    UniformParams->EnergyQuantizationScale = Input.EnergyQuantizationScale;
    UniformParams->Scalar = Input.Scalar;
    UniformParams->AirAbsorptionPerMeter = FVector3f(Input.AirAbsorptionPerMeter);
    UniformParams->ListenerLocation = FVector3f(Input.ListenerLocation);
    UniformParams->ListenerForward = FVector3f(Input.ListenerForward);
    UniformParams->SourceLocation = FVector3f(Input.SourceLocation);
    UniformParams->SourceDipoleWeight = Input.SourceDipoleWeight;
    UniformParams->SourceDipolePower = Input.SourceDipolePower;
    UniformParams->RandomSeed = FMath::Rand();
    UniformParams->TLAS = SceneView.GetRayTracingScene() ? 
        SceneView.GetRayTracingScene()->GetLayerView(FRayTracingScene::ELayer::Base) : nullptr;
    
    // 3. 获取场景全局Buffer (从AcousticScene获取)
    FRDGBufferRef TriangleNormalsBuffer = AcousticScene->GetTriangleNormalsBuffer();
    FRDGBufferRef MaterialIndicesBuffer = AcousticScene->GetMaterialIndicesBuffer();
    FRDGBufferRef MaterialsBuffer = AcousticScene->GetMaterialsBuffer();
    FRDGBufferRef DiffuseSamplesBuffer = AcousticScene->GetDiffuseSamplesBuffer();
    FRDGBufferRef ListenerSHCoeffsBuffer = AcousticScene->GetListenerSHCoeffsBuffer(Input.NumRays, Input.NumChannels);
    
    // 4. 创建模拟过程Buffer
    // 射线Buffer (双缓冲，32字节/射线)
    FRDGBufferDesc RayBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
        sizeof(FReflectionRayData), Input.NumRays);
    FRDGBufferRef RayBufferA = GraphBuilder.CreateBuffer(RayBufferDesc, TEXT("RayBufferA"));
    FRDGBufferRef RayBufferB = GraphBuilder.CreateBuffer(RayBufferDesc, TEXT("RayBufferB"));
    
    // 命中Buffer (32字节/射线)
    FRDGBufferDesc HitBufferDesc = FRDGBufferDesc::CreateStructuredDesc(
        sizeof(FReflectionHitData), Input.NumRays);
    FRDGBufferRef HitBuffer = GraphBuilder.CreateBuffer(HitBufferDesc, TEXT("HitBuffer"));
    
    // 能量延迟Buffer
    FRDGBufferDesc EnergyDelayDesc = FRDGBufferDesc::CreateStructuredDesc(
        sizeof(FEnergyDelayData), Input.NumRays);
    FRDGBufferRef EnergyDelayBuffer = GraphBuilder.CreateBuffer(EnergyDelayDesc, TEXT("EnergyDelay"));
    
    // 阴影检测结果Buffer
    FRDGBufferDesc ShadowResultDesc = FRDGBufferDesc::CreateStructuredDesc(
        sizeof(FShadowResult), Input.NumRays);
    FRDGBufferRef ShadowResultBuffer = GraphBuilder.CreateBuffer(ShadowResultDesc, TEXT("ShadowResults"));
    
    // 能量场Buffer - int32数组 [channel][band][bin]
    FRDGBufferDesc EnergyFieldDesc = FRDGBufferDesc::CreateStructuredDesc(
        sizeof(int32), Input.NumChannels * NUM_BANDS * NUM_BINS);
    FRDGBufferRef EnergyFieldBuffer = GraphBuilder.CreateBuffer(EnergyFieldDesc, TEXT("EnergyField"));
    
    // 贡献计数器Buffer
    FRDGBufferDesc CounterDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1);
    FRDGBufferRef ContributionCounter = GraphBuilder.CreateBuffer(CounterDesc, TEXT("ContributionCounter"));
    
    // 5. Pass 0: 初始化射线
    AddInitializeRaysPass(GraphBuilder, UniformParams, RayBufferA);
    
    // 6. Bounce循环
    FRDGBufferRef CurrentRayBuffer = RayBufferA;
    FRDGBufferRef NextRayBuffer = RayBufferB;
    
    for (int32 Bounce = 0; Bounce < Input.NumBounces; ++Bounce)
    {
        UniformParams->CurrentBounce = Bounce;
        
        // Pass 1: 光追查询
        AddTraceRaysPass(GraphBuilder, UniformParams, CurrentRayBuffer, HitBuffer);
        
        // Pass 2: 着色与弹射 (同时计算能量和生成反射射线)
        AddShadeAndBouncePass(GraphBuilder, UniformParams, CurrentRayBuffer, HitBuffer, 
                             TriangleNormalsBuffer, MaterialIndicesBuffer, MaterialsBuffer, DiffuseSamplesBuffer,
                             NextRayBuffer, EnergyDelayBuffer);
        
        // Pass 3: 阴影射线检测
        // 注意: Steam Audio在此处使用QueryOcclusion，我们需要实现等效功能
        AddShadowRaysPass(GraphBuilder, UniformParams, HitBuffer, ShadowResultBuffer);
        
        // Pass 4: 能量收集
        AddGatherEnergyPass(GraphBuilder, UniformParams, EnergyDelayBuffer, ShadowResultBuffer, 
                           ListenerSHCoeffsBuffer, EnergyFieldBuffer, ContributionCounter);
        
        // 交换射线Buffer (Ping-Pong)
        if (Bounce < Input.NumBounces - 1)
        {
            Swap(CurrentRayBuffer, NextRayBuffer);
        }
    }
    
    // 7. 添加读取回传Pass
    AddReadbackPass(GraphBuilder, EnergyFieldBuffer, ContributionCounter, 
                    Input.NumChannels, NUM_BINS, OutImpulseResponse);
}
```

### 5.2 各Pass添加函数

```cpp
// ============================================================================
// Pass 0: 添加初始化射线Pass
// ============================================================================
void AddInitializeRaysPass(
    FRDGBuilder& GraphBuilder,
    FReflectionSimulationParameters* UniformParams,
    FRDGBufferRef RayBuffer)
{
    // 获取Compute Shader
    TShaderMapRef<FInitializeRaysCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    
    // 分配Pass参数
    FInitializeRaysCS::FParameters* PassParams = GraphBuilder.AllocParameters<FInitializeRaysCS::FParameters>();
    PassParams->UniformParams = UniformParams;
    PassParams->OutRayBuffer = GraphBuilder.CreateUAV(RayBuffer);
    
    // 计算Dispatch尺寸
    FIntVector DispatchSize = FComputeShaderUtils::GetGroupCount(UniformParams->NumRays, 64);
    
    // 添加RDG Pass
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("Reflection.InitializeRays(NumRays=%u)", UniformParams->NumRays),
        PassParams,
        ERDGPassFlags::Compute,
        [ComputeShader, PassParams, DispatchSize](FRHICommandList& RHICmdList)
        {
            FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParams, DispatchSize);
        }
    );
}


// ============================================================================
// Pass 1: 添加光追查询Pass
// ============================================================================
void AddTraceRaysPass(
    FRDGBuilder& GraphBuilder,
    FReflectionSimulationParameters* UniformParams,
    FRDGBufferRef RayBuffer,
    FRDGBufferRef HitBuffer)
{
    // 获取RayTracing Shader
    TShaderMapRef<FReflectionTraceRGS> RayGenShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    TShaderMapRef<FReflectionTraceCHS> ClosestHitShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    TShaderMapRef<FReflectionTraceMS> MissShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    
    // 构建Pipeline State
    FRayTracingPipelineStateInitializer PipelineStateInitializer;
    PipelineStateInitializer.SetRayGenShader(RayGenShader.GetRayTracingShader());
    PipelineStateInitializer.SetHitGroupShader(0, ClosestHitShader.GetRayTracingShader());
    PipelineStateInitializer.SetMissShader(0, MissShader.GetRayTracingShader());
    PipelineStateInitializer.MaxPayloadSizeInBytes = sizeof(FTracePayload);
    PipelineStateInitializer.MaxAttributeSizeInBytes = sizeof(float) * 2;  // Barycentrics
    PipelineStateInitializer.MaxRecursiveDepth = 1;  // 非递归，每次只有一层
    
    FRayTracingPipelineState* PipelineState = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(
        RHICmdList, PipelineStateInitializer);
    
    // 分配Pass参数
    FReflectionTraceRGS::FParameters* PassParams = GraphBuilder.AllocParameters<FReflectionTraceRGS::FParameters>();
    PassParams->UniformParams = UniformParams;
    PassParams->RayBuffer = GraphBuilder.CreateSRV(RayBuffer);
    PassParams->OutHitBuffer = GraphBuilder.CreateUAV(HitBuffer);
    
    // 添加RDG Pass
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("Reflection.TraceRays(NumRays=%u)", UniformParams->NumRays),
        PassParams,
        ERDGPassFlags::Compute,
        [PipelineState, PassParams, NumRays = UniformParams->NumRays](FRHICommandList& RHICmdList)
        {
            FRHIRayTracingScene* RayTracingScene = PassParams->UniformParams->TLAS->GetRHI();
            
            FRayTracingShaderBindings ShaderBindings;
            ShaderBindings.Pipeline = PipelineState;
            ShaderBindings.SetRayGenShader(PassParams);
            
            // 设置HitGroup和Miss Shader的Uniform Buffer
            // ...
            
            // 发射射线
            RHICmdList.RayTraceDispatch(
                ShaderBindings.Pipeline,
                ShaderBindings.RayGenShaderTable,
                RayTracingScene,
                ShaderBindings,
                NumRays,  // Width = NumRays
                1         // Height = 1
            );
        }
    );
}


// ============================================================================
// Pass 2: 添加着色与弹射Pass
// ============================================================================
void AddShadeAndBouncePass(
    FRDGBuilder& GraphBuilder,
    FReflectionSimulationParameters* UniformParams,
    FRDGBufferRef InputRayBuffer,
    FRDGBufferRef HitBuffer,
    FRDGBufferRef OutputRayBuffer)
{
    TShaderMapRef<FShadeAndBounceCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    
    FShadeAndBounceCS::FParameters* PassParams = GraphBuilder.AllocParameters<FShadeAndBounceCS::FParameters>();
    PassParams->UniformParams = UniformParams;
    PassParams->InputRayBuffer = GraphBuilder.CreateSRV(InputRayBuffer);
    PassParams->HitBuffer = GraphBuilder.CreateSRV(HitBuffer);
    PassParams->OutputRayBuffer = GraphBuilder.CreateUAV(OutputRayBuffer);
    
    FIntVector DispatchSize = FComputeShaderUtils::GetGroupCount(UniformParams->NumRays, 64);
    
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("Reflection.ShadeAndBounce(Bounce=%u)", UniformParams->CurrentBounce),
        PassParams,
        ERDGPassFlags::Compute,
        [ComputeShader, PassParams, DispatchSize](FRHICommandList& RHICmdList)
        {
            FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParams, DispatchSize);
        }
    );
}


// ============================================================================
// Pass 3: 添加阴影射线Pass
// ============================================================================
void AddShadowRaysPass(
    FRDGBuilder& GraphBuilder,
    FReflectionSimulationParameters* UniformParams,
    FRDGBufferRef HitBuffer,
    FRDGBufferRef ShadowResultBuffer)
{
    TShaderMapRef<FReflectionShadowRGS> RayGenShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    TShaderMapRef<FReflectionShadowAHS> AnyHitShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    TShaderMapRef<FReflectionShadowMS> MissShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    
    // 构建Pipeline State...
    
    FReflectionShadowRGS::FParameters* PassParams = GraphBuilder.AllocParameters<FReflectionShadowRGS::FParameters>();
    PassParams->UniformParams = UniformParams;
    PassParams->HitBuffer = GraphBuilder.CreateSRV(HitBuffer);
    PassParams->ShadowResults = GraphBuilder.CreateUAV(ShadowResultBuffer);
    
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("Reflection.ShadowRays"),
        PassParams,
        ERDGPassFlags::Compute,
        // ...
    );
}


// ============================================================================
// Pass 4: 添加能量收集Pass
// ============================================================================
void AddGatherEnergyPass(
    FRDGBuilder& GraphBuilder,
    FReflectionSimulationParameters* UniformParams,
    FRDGBufferRef RayBuffer,
    FRDGBufferRef HitBuffer,
    FRDGBufferRef ShadowResultBuffer,
    FRDGBufferRef EnergyFieldBuffer,
    FRDGBufferRef ContributionCounter)
{
    TShaderMapRef<FGatherEnergyCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    
    FGatherEnergyCS::FParameters* PassParams = GraphBuilder.AllocParameters<FGatherEnergyCS::FParameters>();
    PassParams->UniformParams = UniformParams;
    PassParams->RayBuffer = GraphBuilder.CreateSRV(RayBuffer);
    PassParams->HitBuffer = GraphBuilder.CreateSRV(HitBuffer);
    PassParams->ShadowResults = GraphBuilder.CreateSRV(ShadowResultBuffer);
    PassParams->EnergyField = GraphBuilder.CreateUAV(EnergyFieldBuffer);
    PassParams->ContributionCounter = GraphBuilder.CreateUAV(ContributionCounter);
    
    FIntVector DispatchSize = FComputeShaderUtils::GetGroupCount(UniformParams->NumRays, 64);
    
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("Reflection.GatherEnergy"),
        PassParams,
        ERDGPassFlags::Compute,
        [ComputeShader, PassParams, DispatchSize](FRHICommandList& RHICmdList)
        {
            // 先清空能量场Buffer
            RHICmdList.ClearUAVUint(PassParams->EnergyField->GetRHI(), FUintVector4(0, 0, 0, 0));
            RHICmdList.ClearUAVUint(PassParams->ContributionCounter->GetRHI(), FUintVector4(0, 0, 0, 0));
            
            FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParams, DispatchSize);
        }
    );
}
```

### 5.3 结果回传

```cpp
// ============================================================================
// 添加结果回传Pass
// ============================================================================
void AddReadbackPass(
    FRDGBuilder& GraphBuilder,
    FRDGBufferRef EnergyFieldBuffer,
    FRDGBufferRef ContributionCounter,
    int32 NumDelayBins,
    FImpulseResponse& OutImpulseResponse)
{
    // 创建Staging Buffer用于回传
    FRDGBufferRef ReadbackBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumDelayBins * 3),
        TEXT("EnergyFieldReadback"));
    
    // 添加回传Pass
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("Reflection.Readback"),
        ERDGPassFlags::Readback,
        [EnergyFieldBuffer, ReadbackBuffer, NumDelayBins, &OutImpulseResponse](FRHICommandList& RHICmdList)
        {
            // 等待GPU完成
            RHICmdList.SubmitCommandsHint();
            
            // 锁定Buffer读取数据
            const uint32* EnergyData = static_cast<const uint32*>(RHICmdList.LockBuffer(
                EnergyFieldBuffer->GetRHI()->GetBuffer(), 0, NumDelayBins * 3 * sizeof(uint32)));
            
            // 转换为脉冲响应
            OutImpulseResponse.EnergyBins.SetNum(NumDelayBins);
            OutImpulseResponse.TimeDomainData.SetNum(NumDelayBins);
            
            const float InvQuantizationScale = 1.0f / EnergyQuantizationScale;
            
            for (int32 BinIdx = 0; BinIdx < NumDelayBins; ++BinIdx)
            {
                FVector& Bin = OutImpulseResponse.EnergyBins[BinIdx];
                Bin.X = EnergyData[BinIdx * 3 + 0] * InvQuantizationScale;  // Low
                Bin.Y = EnergyData[BinIdx * 3 + 1] * InvQuantizationScale;  // Mid
                Bin.Z = EnergyData[BinIdx * 3 + 2] * InvQuantizationScale;  // High
                
                // 转换为单声道时域数据 (简单平均)
                OutImpulseResponse.TimeDomainData[BinIdx] = (Bin.X + Bin.Y + Bin.Z) / 3.0f;
            }
            
            RHICmdList.UnlockBuffer(EnergyFieldBuffer->GetRHI()->GetBuffer());
        }
    );
}
```

---

## 6. 优化策略

### 6.1 性能优化

| 优化点 | 实现方式 | 效果 |
|--------|----------|------|
| **射线合并** | 将多个射线的阴影检测合并为单次DispatchRays | 减少API调用开销 |
| **Early Out** | 能量低于阈值时标记射线为不活跃 | 减少无效计算 |
| **Wave优化** | 使用Wave指令减少原子操作冲突 | 提升能量累加效率 |
| **异步执行** | 反射模拟与渲染并行执行 | 隐藏延迟 |
| **LOD系统** | 根据距离动态调整NumRays和NumBounces | 平衡质量与性能 |

### 6.2 内存优化

```cpp
// 使用Transient Buffer减少显存占用
FRDGBufferRef RayBuffer = GraphBuilder.CreateTransientBuffer(
    FRDGBufferDesc::CreateStructuredDesc(sizeof(FReflectionRayData), NumRays),
    TEXT("RayBuffer")
);

// 延迟创建Buffer (只在需要时分配)
if (bNeedShadowPass)
{
    ShadowResultBuffer = GraphBuilder.CreateBuffer(...);
}
```

### 6.3 数值稳定性

```hlsl
// 能量使用对数域存储避免下溢
float LogEnergy = log(max(Energy, 1e-10));

// 延迟计算使用高精度
float PreciseDelay = double(TravelDistance) / double(SpeedOfSound);

// 原子累加前检查数值范围
if (EnergyInt > 0 && EnergyInt < UINT_MAX)
{
    InterlockedAdd(EnergyField[BinIndex], EnergyInt);
}
```

---

## 7. 调试与可视化

### 7.1 RDG调试

```cpp
// 使用RDG Event标记Pass
GraphBuilder.AddPass(
    RDG_EVENT_NAME("Reflection.Bounce[%d].Trace", Bounce),
    // ...
);

// 在RenderDoc/PIX中查看Buffer内容
GraphBuilder.SetBufferAccessFinal(EnergyFieldBuffer, ERHIAccess::CopySrc);
```

### 7.2 可视化射线

```cpp
// 将射线数据复制到可视化Buffer
void VisualizeRays(FRDGBuilder& GraphBuilder, FRDGBufferRef RayBuffer, int32 NumRays)
{
    // 使用Debug Draw或Line Batch绘制射线
    // 需要读取回CPU或使用Compute Shader写入Line Vertex Buffer
}
```

---

## 8. 参考文件

| 文件 | 说明 |
|------|------|
| `doc/reflection_architecture_explore.md` | 架构设计文档探索 |
| `doc/steam-audio/reflection_analysis.md` | Steam Audio反射分析 |
| `Shaders/Private/Simulation/UERayTracingAudioEnergyField.usf` | 现有能量场Shader |
| `Source/UERayTracingAudioSDK/Private/Simulation/UERayTracingAudioReflectionSimulator.cpp` | 反射模拟器实现 |
| `steam-audio/core/src/core/radeonrays_reflection_simulator.cl` | Steam Audio OpenCL实现参考 |

---