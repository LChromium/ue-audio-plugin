# Steam Audio GPU 光线追踪技术分析报告

## 1. 概述

Steam Audio 是一款由 Valve 开发的空间音频 SDK，支持实时光线追踪来模拟声音在三维空间中的传播、反射和混响效果。本文档重点分析其 **GPU 光线追踪实现**部分。

### 1.1 支持的 Ray Tracer 类型

Steam Audio 支持多种光线追踪后端：

| 类型 | 实现 | 平台 | 特点 |
|-----|------|------|------|
| **Default** | 内置多线程 CPU 光追 | 全平台 | 兼容性好，无需额外依赖 |
| **Embree** | Intel Embree | Windows/Linux/macOS | CPU 高度优化，使用 ISPC |
| **Radeon Rays** | AMD Radeon Rays | 支持 OpenCL 的平台 | **GPU 加速**，支持 AMD/NVIDIA/Intel GPU |
| **Custom** | 用户自定义回调 | 全平台 | 灵活性最高 |

### 1.2 GPU 光追核心架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Steam Audio GPU 光线追踪架构                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────┐     ┌─────────────────┐     ┌─────────────────────┐   │
│  │   Scene Data    │────▶│  RadeonRays     │────▶│   GPU BVH Build     │   │
│  │  (Mesh/Material)│     │  Device (OpenCL)│     │   (OpenCL Context)  │   │
│  └─────────────────┘     └─────────────────┘     └─────────────────────┘   │
│           │                       │                       │                 │
│           ▼                       ▼                       ▼                 │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    RadeonRaysReflectionSimulator                     │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │   │
│  │  │ generateRays │─▶│QueryIntersection│─▶│ shadeAndBounce │          │   │
│  │  │   (Kernel)   │  │  (RadeonRays)  │  │   (Kernel)     │          │   │
│  │  └──────────────┘  └──────────────┘  └──────────────┘              │   │
│  │           │                │                │                       │   │
│  │           ▼                ▼                ▼                       │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │   │
│  │  │sphereOcclusion│  │QueryOcclusion│  │gatherEnergy  │              │   │
│  │  │   (Kernel)   │  │  (RadeonRays)│  │   (Kernel)   │              │   │
│  │  └──────────────┘  └──────────────┘  └──────────────┘              │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                        │
│                                    ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        EnergyField / Image                          │   │
│  │                    (脉冲响应 / 反射图像)                              │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Radeon Rays 集成详解

### 2.1 核心组件

#### 2.1.1 RadeonRaysDevice (设备管理)

**文件**: `core/src/core/radeonrays_device.cpp/h`

```cpp
class RadeonRaysDevice {
    shared_ptr<OpenCLDevice> mOpenCL;      // OpenCL 设备上下文
    RadeonRays::IntersectionApi* mAPI;      // Radeon Rays 核心 API
    OpenCLProgram mProgram;                 // OpenCL 程序 (自定义 kernels)
};
```

**初始化流程**:
1. 创建 OpenCL 上下文和设备
2. 从 OpenCL 上下文创建 Radeon Rays Intersection API
3. 配置 BVH 构建器参数 (`bvh.builder=sah`, `bvh.usesplits=1`)
4. 编译自定义 OpenCL kernels

#### 2.1.2 RadeonRaysScene (场景管理)

**文件**: `core/src/core/radeonrays_scene.cpp/h`

场景提交到 GPU 的流程：

```cpp
void RadeonRaysScene::commit() {
    // 1. 分离所有旧几何体
    mRadeonRays->api()->DetachAll();
    
    // 2. 附加所有静态网格
    for (const auto& staticMesh : mStaticMeshes) {
        mRadeonRays->api()->AttachShape(
            static_cast<RadeonRaysStaticMesh*>(staticMesh.get())->shape()
        );
    }
    
    // 3. 提交 BVH 构建
    mRadeonRays->api()->Commit();
}
```

#### 2.1.3 RadeonRaysStaticMesh (网格数据)

**文件**: `core/src/core/radeonrays_static_mesh.cpp/h`

网格数据准备：
- 顶点数据上传到 OpenCL Buffer
- 三角形索引创建 Radeon Rays Shape
- 法线和材质索引单独存储用于 shading

```cpp
void RadeonRaysStaticMesh::initialize(...) {
    // 创建 Radeon Rays 网格
    mShape = mRadeonRays->api()->CreateMesh(
        reinterpret_cast<const float*>(vertices), 
        mNumVertices, 
        sizeof(Vector3f),
        reinterpret_cast<const int32_t*>(triangles), 
        0, nullptr, 
        mNumTriangles
    );
    
    // 计算并上传法线
    calcNormals(vertices, triangles);
    
    // 上传材质数据到 OpenCL Buffer
    clEnqueueWriteBuffer(..., mMaterialIndices->buffer(), ...);
    clEnqueueWriteBuffer(..., mMaterials->buffer(), ...);
}
```

---

## 3. GPU 光线追踪核心算法

### 3.1 反射模拟流程详解

**文件**: `core/src/core/radeonrays_reflection_simulator.cpp`

#### 3.1.1 核心数据结构详解

在深入流程之前，必须先理解以下数据结构的含义：

##### 射线 (Ray)

```cpp
typedef struct Ray_t {
    float4  o;      // 射线起点 = (origin.x, origin.y, origin.z, 某个标记)
    float4  d;      // 射线方向 = (dir.x, dir.y, dir.z, 某个标记)
    int2    extra;  // extra.x = 标记位, extra.y = 是否启用 (1=启用, 0=禁用)
    int2    padding; // 填充对齐
} Ray;
```

**形象理解**：一条射线 = 一个带方向的小棍子 + 起点位置 + 启用/禁用标记

##### 命中结果 (Hit)

```cpp
typedef struct Hit_t {
    int       shapeid;    // 击中的几何体ID
    int       primid;     // 击中的是哪个三角形（索引）
    int       padding0;
    int       padding1;
    float4    uvwt;       // uvwt.s3 = 命中点沿射线的距离 (t)，单位是米
} Hit;
```

**形象理解**：命中结果告诉"这条射线击中了什么"：
- `primid = 3` → 击中了第3个三角形
- `uvwt.s3 = 2.5` → 击中点距离射线起点2.5米

##### 能量场 (Energy Field) —— 最核心但最难理解的概念

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    Energy Field 是什么？                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  声学中的"能量场"是一个 3D 声音能量在时间轴上的分布图                        │
│                                                                             │
│  想象：                                                                     │
│  - 你在听者位置放一个麦克风                                                  │
│  - 声源发出一个脉冲（"砰！"）                                               │
│  - 这个脉冲经过墙壁反射，会分批到达听者：                                      │
│      第1批: 0.05秒后到达（直达声）                                          │
│      第2批: 0.12秒后到达（第一次反射）                                       │
│      第3批: 0.18秒后到达（第二次反射）                                       │
│      ...                                                                    │
│      第N批: 越来越弱，越来越密集                                             │
│                                                                             │
│  Energy Field 就是记录"每一时刻有多少能量到达"的表格                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

Energy Field 的数据结构：
```
┌─────────────────────────────────────────────────────────────────┐
│                    Energy Field: [channel][band][bin]           │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  channel: 声道数（通常是4个，对应四面体或立方体排列的麦克风）     │
│  band:    频段数（3个：低频、中频、高频）                         │
│  bin:     时间仓数（256个，每个10ms）                             │
│                                                                 │
│  所以是: 4 × 3 × 256 = 3072 个值                                │
│                                                                 │
│  bin[i] 表示: 从 t=i*10ms 到 t=(i+1)*10ms 这段时间内到达的能量    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

##### 累计能量 (AccumEnergy) vs 当前能量 (Energy)

```
┌─────────────────────────────────────────────────────────────────┐
│         accumEnergy vs energy 的区别                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  accumEnergy (累计能量) = 这条射线从出发到现在积累的总能量         │
│                        = 1.0 × (1-absorption_第1个面) ×          │
│                          (1-absorption_第2个面) × ...            │
│                                                                 │
│  energy (当前能量) = 这一次弹射贡献的能量                         │
│                    = accumEnergy × 这次表面的反射率 × 距离衰减 ×  │
│                      方向性因子                                  │
│                                                                 │
│  简单说:                                                         │
│  - accumEnergy 记录"这趟旅程还剩多少能量"                        │
│  - energy 记录"这次贡献了多少能量"                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

##### 累计延迟 (AccumDelay) vs 当前延迟 (Delay)

```
┌─────────────────────────────────────────────────────────────────┐
│         accumDelay vs delay 的区别                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  SPEED_OF_SOUND = 340 m/s (声速)                                │
│                                                                 │
│  accumDelay = 这条射线从出发到现在用的总时间                      │
│             = 距离1/340 + 距离2/340 + 距离3/340 + ...            │
│                                                                 │
│  delay = 这次到达听者比直达声晚多少                               │
│        = (accumDelay_这次) - (直达距离/340)                      │
│                                                                 │
│  用途: delay 用来决定把这个能量放到哪个时间仓(bin)里               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

#### 3.1.2 反射模拟完整流程

整个反射模拟是一个**多轮迭代**的过程，每轮包含以下步骤：

```cpp
void RadeonRaysReflectionSimulator::simulate(...) {
    // 步骤 1: 重置所有状态
    reset();
    
    // 步骤 2: 把声源和听者位置上传到 GPU
    setSourcesAndListeners(numSources, sources, numListeners, listeners, directivities);
    setNumRays(numSources, numListeners, numRays);
    
    // 步骤 3: 生成初始射线 (从听者向外发射)
    generateListenerRays(numListeners, numRays);
    
    // 步骤 4: 多轮反射弹射
    for (auto i = 0; i < numBounces; ++i) {
        // 4.1 GPU 光线追踪 - 寻找射线与场景的交点
        mRadeonRays->api()->QueryIntersection(...);
        
        // 4.2 球体遮挡检测 (源/听者周围)
        if (i > 0) {
            sphereOcclusion(...);
        }
        
        // 4.3 着色计算 - 计算反射方向和能量衰减
        shadeAndBounce(...);
        
        // 4.4 阴影射线遮挡查询
        mRadeonRays->api()->QueryOcclusion(...);
        
        // 4.5 收集能量场数据
        for (auto j = 0; j < max(numSources, numListeners); ++j) {
            gatherEnergyField(j, numRays, *energyFields[j]);
        }
        
        // 4.6 交换射线缓冲区 (用于下一轮弹射)
        if (i < numBounces - 1) {
            mCurrentRayBuffer = 1 - mCurrentRayBuffer;
        }
    }
}
```

---

##### 步骤 1: reset() —— 重置所有状态

```cpp
void RadeonRaysReflectionSimulator::reset() {
    auto zero = 0.0f;
    auto accum = cl_float4{ 1.0f, 1.0f, 1.0f, 0.0f };  // xyz=能量, w=延迟
    
    // 清空图像缓冲区
    clEnqueueFillBuffer(mRadeonRays->openCL().irUpdateQueue(), 
                         mImage.buffer(), &zero, sizeof(cl_float), ...);
    
    // 重置累计能量: 所有射线初始能量 = 1.0（满分能量）
    clEnqueueFillBuffer(mRadeonRays->openCL().irUpdateQueue(), 
                         mAccumEnergy.buffer(), &accum, sizeof(cl_float4), ...);
}
```

**为什么要重置？**
- `mImage`: 将所有像素清零，准备收集最终图像
- `mAccumEnergy`: 将所有射线的累计能量重置为 `1.0f`（刚出发，满能量）
- `w = 0.0f`: 累计延迟清零（刚出发，还没开始计时）

---

##### 步骤 2: setSourcesAndListeners() —— 把声源和听者位置上传到 GPU

```cpp
void RadeonRaysReflectionSimulator::setSourcesAndListeners(...) {
    // 把声源坐标空间上传到 GPU
    auto* _sources = clEnqueueMapBuffer(..., mSources.buffer(), ...);
    for (int i = 0; i < numSources; ++i) {
        _sources[i].right  = sources[i].right;
        _sources[i].up     = sources[i].up;
        _sources[i].ahead  = sources[i].ahead;
        _sources[i].origin = sources[i].origin;  // 声源位置 (x,y,z)
    }
    clEnqueueUnmapMemObject(..., mSources.buffer(), _sources, ...);
    
    // 同样上传听者坐标
    auto* _listeners = clEnqueueMapBuffer(..., mListeners.buffer(), ...);
    // ...
    
    // 上传声源方向性参数（偶极子什么的）
    auto* _directivities = clEnqueueMapBuffer(..., mDirectivities.buffer(), ...);
    // ...
}
```

**CoordinateSpace 结构**:
```cpp
struct CoordinateSpace {
    float3 right;   // 坐标系右向量
    float3 up;     // 坐标系上向量
    float3 ahead;  // 坐标系前向量
    float3 origin; // 位置（声源或听者的 x,y,z 坐标）
};
```

**为什么要上传这些？** GPU 计算时需要知道：
- 声源在哪 (`origin`)
- 声源朝向（用于计算偶极子方向性）
- 听者位置和朝向

---

##### 步骤 3: generateListenerRays() —— 生成从听者出发的射线

```cpp
void RadeonRaysReflectionSimulator::generateListenerRays(int numListeners, int numRays) {
    // 准备参数
    clSetKernelArg(mGenerateListenerRays.kernel(), 0, sizeof(cl_mem), &mListeners.buffer());
    clSetKernelArg(mGenerateListenerRays.kernel(), 1, sizeof(cl_mem), &mListenerSamples.buffer());
    clSetKernelArg(mGenerateListenerRays.kernel(), 2, sizeof(cl_mem), &mRays[mCurrentRayBuffer].clBuffer());
    
    // 2D 并行: [numRays × numListeners] 个工作项
    size_t globalSize[] = { (size_t)numRays, (size_t)numListeners };
    clEnqueueNDRangeKernel(mRadeonRays->openCL().irUpdateQueue(), 
                           mGenerateListenerRays.kernel(), 
                           2, nullptr, globalSize, nullptr, ...);
}
```

**generateListenerRays kernel 详解**:

```opencl
kernel void generateListenerRays(global CoordinateSpace* listeners,
                                 global float4* sphereSamples,  // 预计算的球面采样点
                                 global Ray* rays) {
    size_t rayIndex = get_global_id(0);       // 0 到 numRays-1
    size_t listenerIndex = get_global_id(1);   // 0 到 numListeners-1
    size_t index = listenerIndex * numRays + rayIndex;
    
    // 设置射线起点 = 听者位置
    rays[index].o = (float4) (listeners[listenerIndex].origin, FLT_MAX);
    
    // 设置射线方向 = 球面上第 rayIndex 个采样点的方向
    rays[index].d = (float4) (sphereSamples[rayIndex].xyz, 0.0f);
    
    // extra.y = 1 表示这条射线启用
    rays[index].extra = (int2) (0xffffffff, 1);
}
```

**球面采样 (sphereSamples) 是什么？**

```cpp
// 初始化时预计算
Sampling::generateSphereSamples(maxNumRays, listenerSamples.data());

// 例如 numRays = 1024
// listenerSamples[0] = 方向(0.1, 0.2, 0.95)  // 向上偏前的方向
// listenerSamples[1] = 方向(-0.3, 0.5, 0.8) // 向左偏上的方向
// ...
// listenerSamples[1023] = 方向(...)
```

**形象理解**：

```
                    听者 (Listener)
                       @
                      /|\
                     / | \
                    /  |  \
                   /   |   \
        射线0 →   ●----●----●   ← 射线1
                 /     |     \
                /      |      \
               ●-------●-------●
              ↑       ↑       ↑
           球面上的采样点，均匀分布
```

这些射线从听者位置向四面八方发射出去，用于"探索"周围环境。

---

##### 步骤 4: QueryIntersection() —— GPU 光线追踪，找射线与场景的交点

```cpp
mRadeonRays->api()->QueryIntersection(
    mRays[mCurrentRayBuffer].rrBuffer(),    // 输入: 射线们
    mNumRays.rrBuffer(),                     // 输入: 有多少条射线
    mMaxNumListeners * mMaxNumRays,         // 输入: 最大输出数量
    mHits.rrBuffer(),                        // 输出: 命中结果
    nullptr, nullptr
);
```

**这行代码干了什么？**

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        QueryIntersection 的作用                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  输入: 成千上万条射线 (从听者出发)                                           │
│                                                                             │
│  过程: Radeon Rays 在 GPU 上用 BVH 加速结构快速判断：                        │
│        "这条射线击中了场景中的哪个三角形？击中距离是多少？"                    │
│                                                                             │
│  输出: mHits[] 数组，每条射线对应一个 Hit 结构                               │
│        - hits[i].primid = 击中的三角形索引                                  │
│        - hits[i].uvwt.s3 = 击中距离（沿射线方向多少米）                      │
│        - 如果没击中任何东西，primid = -1                                     │
│                                                                             │
│  关键: 这是真正的 GPU 并行计算，数万条射线同时追踪！                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

##### 步骤 5: sphereOcclusion() —— 检测射线是否被声源/听者"截获"

```cpp
// 只有第 1 轮弹射之后才需要检测（第 0 轮是初始发射，还不存在"弹射回来"的问题）
if (i > 0) {
    sphereOcclusion(numSources, sources, numListeners, listeners, numRays);
}
```

**为什么要做这个检测？**

```
场景俯视图:
                    声源 (Source)
                       ★
                       │
                       │ ← 这条射线从听者出发，击中墙壁后弹射回来
                       │
                       ▼
    ┌──────────────────┬──────────────────┐
    │                  │                  │
    │      墙壁        │      墙壁        │
    │                  │                  │
    └──────────────────┴──────────────────┘
                       │
                       @
                    听者 (Listener)
                       ↑
                       │
                 弹射回来的这条射线
                 经过声源附近时被"截获"了
                 
含义: 声音从声源出发，经过反射，刚好回到听者位置附近
     但如果它"路过"声源附近，就说明这条路径是可能的传播路径
```

**sphereOcclusion kernel 详解**:

```opencl
kernel void sphereOcclusion(..., global Ray* rays, global Hit* hits) {
    uint numRays = get_global_size(0);
    uint rayIndex = get_global_id(0);  // 每条射线一个工作项
    
    for (int i = 0; i < numListeners; ++i) {
        uint index = i * numRays + rayIndex;
        
        // 用解析几何检测射线是否与"听者周围的球体"相交
        float listenerSphereHitDistance = raySphereIntersect(
            &rays[index], 
            listeners[i].origin,   // 球心 = 听者位置
            LISTENER_RADIUS         // 球半径 = 0.1米
        );
        
        // 如果:
        // 1. 射线与球体相交了 (listenerSphereHitDistance >= 0)
        // 2. 球体交点比场景几何交点更近 (distance < hits[index].uvwt.s3)
        if (0.0f <= listenerSphereHitDistance && 
            listenerSphereHitDistance < hits[index].uvwt.s3) {
            // 说明这条弹射回来的射线"进入了听者范围"
            rays[index].extra.y = 0;   // 禁用这条射线（不再继续弹射）
            hits[index].primid = -1;   // 标记为被听者吸收
            return;
        }
        
        // 同样检测是否经过声源附近
        for (int j = 0; j < numSources; ++j) {
            float sourceSphereHitDistance = raySphereIntersect(
                &rays[index], 
                sources[j].origin,
                SOURCE_RADIUS
            );
            // ...类似逻辑
        }
    }
}
```

**LISTENER_RADIUS = 0.1m** 是什么概念？
- 听者周围有一个半径 10cm 的"感应球"
- 弹射回来的射线只要进入这个范围，就算"到达了听者"

---

##### 步骤 6: shadeAndBounce() —— 最核心的着色与弹射计算

这是最复杂的 kernel，我把它拆解为8个部分：

```cpp
void RadeonRaysReflectionSimulator::shadeAndBounce(...) {
    // 设置 kernel 参数（省略...)
    
    size_t globalSize[] = { (size_t)(numRays * max(numSources, numListeners)) };
    
    // 动态调整 local size 以提高 GPU 利用率
    do {
        size_t localSize[] = { (size_t)mShadeLocalSize };
        status = clEnqueueNDRangeKernel(..., globalSize, localSize, ...);
        if (status != CL_SUCCESS)
            mShadeLocalSize /= 2;  // 如果失败就减半重试
    } while (mShadeLocalSize > 1 && status != CL_SUCCESS);
}
```

**shadeAndBounce kernel 逐行解析**:

```opencl
kernel void shadeAndBounce(...) {
    // ===== 第1部分: 计算索引 =====
    // numChunks = max(numListeners, numSources)
    // numPrimaryRays = 总工作项数 / numChunks
    // 例如: 1024 射线 × 1 声源 = 1024 个工作项，numChunks=1，numPrimaryRays=1024

    uint rayIndex = get_global_id(0) % numPrimaryRays;   // 哪个射线
    uint sourceIndex = get_global_id(0) / numPrimaryRays; // 哪个声源

    // ===== 第2部分: 跳过无效射线 =====
    if (rays[rayIndex].extra.y == 0 || hits[rayIndex].primid < 0) {
        // 这条射线被禁用了（被听者/声源截获，或根本没击中任何东西）
        shadowRays[shadowRayIndex].extra = 0;  // 不生成阴影射线
        reflectedRays[rayIndex].extra = 0;    // 不生成反射射线
        energyDelay[shadowRayIndex] = (float4) 0.0f;  // 没有能量贡献
        return;
    }
    
    // ===== 第3部分: 准备数学工具 =====
    // 随机数生成器（每个 workgroup 一个）
    local RNG rng;
    if (get_local_id(0) == 0) {
        initRNG(randomNumber + rayIndex, &rng);
    }

    // ===== 第4部分: 计算命中点位置 =====
    float3 rayDirection = rays[rayIndex].d.xyz;
    int triangleIndex = hits[rayIndex].primid;        // 击中哪个三角形
    float hitDistance = hits[rayIndex].uvwt.s3;       // 击中距离（米）
    float3 hitPoint = rays[rayIndex].o.xyz + hitDistance * rayDirection;
    // 命中点 = 射线起点 + 方向 × 距离

    // ===== 第5部分: 计算命中点法线 =====
    float3 hitNormal = normals[triangleIndex].xyz;    // 获取三角形法线
    if (dot(hitNormal, rayDirection) > 0.0f) {
        // 如果法线与射线方向夹角 > 90°，说明击中了背面，翻转法线
        hitNormal = -hitNormal;
    }

    // ===== 第6部分: 获取命中点材质 =====
    Material hitMaterial = materials[materialIndices[triangleIndex]];
    float3 hitMaterialAbsorption = (float3)(
        hitMaterial.absorptionLow, 
        hitMaterial.absorptionMid, 
        hitMaterial.absorptionHigh
    );

    // ===== 第7部分: 生成阴影射线（检测是否被声源照亮） =====
    // 阴影射线 = 从命中点射向声源的射线
    float3 source = sources[sourceIndex].origin;
    float hitToSourceDistance = distance(hitPoint, source);
    float4 hitToSource = (float4) (normalize(source - hitPoint), 0.0f);

    // 判断几种"不算贡献"的情况
    if (hitDistance <= LISTENER_RADIUS ||           // 命中点在听者内部
        hitToSourceDistance <= irradianceMinDistance || // 离声源太近
        dot(hitToSource.xyz, hitNormal) < 0.0f)     // 命中点在声源背面
    {
        shadowRays[shadowRayIndex].extra = 0;        // 不生成阴影射线
        energyDelay[shadowRayIndex] = (float4) 0.0f; // 没有能量
    } 
    else 
    {
        // ===== 生成阴影射线 =====
        shadowRays[shadowRayIndex].o = (float4)(
            hitPoint + RAY_SURFACE_OFFSET * hitToSource.xyz,  // 稍微偏移避免自相交
            hitToSourceDistance                                   // 存储距离
        );
        shadowRays[shadowRayIndex].d = hitToSource;
        shadowRays[shadowRayIndex].extra = (int2) (0xffffffff, 1);
        
        // ===== 计算能量贡献 (Phong 模型) =====
        
        // 漫反射项: 能量 × 散射系数 × cos(入射角)
        float3 energy = (1.0f / PI) * hitMaterial.scattering * 
                        max(0.0f, dot(hitNormal, hitToSource.xyz));
        
        // 镜面反射项: (1 - 散射系数) × 高光系数
        float3 halfVector = normalize((hitToSource.xyz - rayDirection) * 0.5f);
        energy += ((SPECULAR_EXPONENT + 2.0f) / (8.0f * PI)) * 
                  (1.0f - hitMaterial.scattering) * 
                  pow(fabs(dot(halfVector, hitNormal)), SPECULAR_EXPONENT);
        
        // 乘以标量（归一化因子）
        energy *= scalar;
        
        // 乘以声源方向性因子（偶极子等）
        energy *= evaluateDirectivity(hitPoint, sources[sourceIndex], directivities[sourceIndex]);
        
        // 乘以距离衰减因子 (1/(4πr²))
        energy *= pointSourceIrradiance(hitToSourceDistance, irradianceMinDistance);
        
        // 乘以累计能量 × (1 - 材质吸收)
        energy *= accumEnergyDelay[rayIndex].xyz * 
                  ((float3) 1.0f - hitMaterialAbsorption);
        
        // ===== 计算延迟 =====
        // 延迟 = 这条射线路径用的总时间 - 直达声用的时间
        float delay = (hitDistance + hitToSourceDistance) / SPEED_OF_SOUND;
        delay += accumEnergyDelay[rayIndex].w - 
                 (distance(source, listeners[listenerIndex].origin) / SPEED_OF_SOUND);
        
        // ===== 输出: 能量 + 延迟 =====
        energyDelay[shadowRayIndex] = (float4) (energy, delay);
    }

    // ===== 第8部分: 生成下一轮弹射的射线 =====
    accumEnergyDelay[rayIndex].xyz *= ((float3) 1.0f - hitMaterialAbsorption);
    accumEnergyDelay[rayIndex].w += hitDistance / SPEED_OF_SOUND;

    // 随机决定是漫反射还是镜面反射
    if (randomFloat < hitMaterial.scattering) {
        // 漫反射: 在法线半球内随机采样一个方向
        uint sampleIndex = randomUint % numDiffuseSamples;
        float3 transformedDiffuseSample = transformHemisphereSample(
            diffuseSamples[sampleIndex].xyz, hitNormal
        );
        reflectedDirection = (float4) (transformedDiffuseSample, 0.0f);
    } else {
        // 镜面反射: 完美反射
        reflectedDirection = (float4) (reflect(rayDirection, hitNormal), 0.0f);
    }

    // 生成新的反射射线
    reflectedRays[rayIndex].o = (float4)(
        hitPoint + RAY_SURFACE_OFFSET * reflectedDirection.xyz, FLT_MAX
    );
    reflectedRays[rayIndex].d = reflectedDirection;
    reflectedRays[rayIndex].extra = (int2) (0xffffffff, 1);
}
```

**这段代码的物理含义图解**:

```
                    声源 ●━━━━━━━━━━━▶ 命中点 ◆
                    (origin)        ↗     (hitPoint)
                                   /
                                  /
                                 /
                                /
                               /  阴影射线 (shadowRay)
                              /
                             /
    反射线 (reflectedRay) ← ◆━━━━━━━━━━━━━━━━━━━━━━━━━▶ (新的方向)
                         (hitPoint)        ↗
                                          /
                                         /
                                        /
                                       /
                                      ●
                                    听者 (listener)
```

---

##### 步骤 7: QueryOcclusion() —— 阴影射线遮挡检测

```cpp
mRadeonRays->api()->QueryOcclusion(
    mShadowRays.rrBuffer(),              // 阴影射线们
    mNumShadowRays.rrBuffer(),            // 数量
    maxNumSources * maxNumRays,           // 最大数量
    mOccluded.rrBuffer(),                 // 输出: 是否被遮挡
    nullptr, nullptr
);
```

**这在检查什么？**

```
命中点 ◆ 发出的阴影射线是否能到达声源？

情况A (不被遮挡):                    情况B (被遮挡):
                                       
声源 ●                              声源 ●
   ↑                                  │
   │                                  │
   | 命中点 ◆ ──────────────────▶     | 命中点 ◆ ──✗──▶ (被墙挡住)
   │ (阴影射线)                        │ (阴影射线)
   
结果: occluded[i] = -1 (未被遮挡)     结果: occluded[i] = 0 (被遮挡)
意味着: 这次贡献是有效的              意味着: 这次贡献无效（声源被遮挡）
```

**为什么要这个检测？**
- 声源到命中点的直线路径可能被其他物体挡住
- 如果被挡住，声音传不过去，这次反射就不能贡献能量

---

##### 步骤 8: gatherEnergyField() / gatherImage() —— 收集结果

```cpp
for (int j = 0; j < max(numSources, numListeners); ++j) {
    gatherEnergyField(j, numRays, *energyFields[j]);
}
```

**gatherEnergyField kernel 详解**:

```opencl
kernel void gatherEnergyField(float scale,       // = 1e+10 (量化因子)
                              global const float4* totalEnergy,  // energyDelay[]
                              uint offset,       // 源索引偏移
                              global const int* occluded,        // 是否被遮挡
                              global const float* shCoefficients, // SH 系数
                              global int* energy) {  // 输出的直方图
    uint rayIndex = get_global_id(0);  // 0 到 255 (时间仓数)
    uint band = get_global_id(1);      // 0-2 (低/中/高频)
    uint channel = get_global_id(2);   // 0-3 (声道)
    
    bool isOccluded = (occluded[offset + rayIndex] >= 0);
    
    // 如果没被遮挡，把这条射线的能量加到对应的时间仓里
    if (!isOccluded) {
        float time = totalEnergy[offset + rayIndex].w;  // 延迟时间
        uint bin = convert_uint_sat(floor(time / BIN_DURATION));
        // BIN_DURATION = 0.01s
        // time = 0.05s → bin = 5
        
        if (bin < NUM_BINS) {  // NUM_BINS = 256
            // 能量值 × SH 系数（用于空间音频渲染）
            float energyValue = scale * rayEnergy[band] * 
                               shCoefficients[channel * numRays + rayIndex];
            
            // 量化后原子累加到直方图
            int quantizedEnergyValue = convert_int_sat(floor(energyValue));
            atomic_add(energy + channel * NUM_BANDS * NUM_BINS + 
                      band * NUM_BINS + bin, quantizedEnergyValue);
        }
    }
}
```

**最终输出是什么？**

```
energy[channel][band][bin] 是一个 3D 直方图：
- channel: 4个（4声道麦克风）
- band: 3个（低频、中频、高频）
- bin: 256个（每个 bin = 10ms）

energy[0][1][5] = 5.2e8
含义: 第0声道的麦克风接收到的中频能量，
     在 50ms-60ms 这个时间段内有 5.2×10⁸ 的累计能量

这个直方图就是"脉冲响应"（Impulse Response），
用来做卷积混响！
```

---

#### 3.1.3 完整一轮弹射的数据流总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        一轮弹射的数据流                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ① generateListenerRays()                                                   │
│     输入: listeners[], sphereSamples[]                                       │
│     输出: rays[] (从听者出发的射线)                                          │
│                                                                             │
│  ② QueryIntersection()                                                      │
│     输入: rays[]                                                             │
│     输出: hits[] (击中了哪些三角形，距离多少)                                 │
│                                                                             │
│  ③ sphereOcclusion()                                                        │
│     输入: rays[], hits[], sources[], listeners[]                            │
│     输出: 更新 rays[].extra.y 和 hits[].primid（标记被截获的射线）           │
│                                                                             │
│  ④ shadeAndBounce()                                                         │
│     输入: rays[], hits[], sources[], listeners[], materials[],              │
│           accumEnergy[], accumDelay[], normals[], diffuseSamples[]           │
│     输出:                                                                    │
│     - shadowRays[]: 阴影射线（从命中点到声源）                                │
│     - reflectedRays[]: 反射射线（下一轮弹射用）                              │
│     - energyDelay[]: 当前弹射贡献的能量和延迟                                │
│     - accumEnergy[]: 更新累计能量（乘以(1-吸收)）                             │
│     - accumDelay[]: 更新累计延迟                                             │
│                                                                             │
│  ⑤ QueryOcclusion()                                                         │
│     输入: shadowRays[]                                                       │
│     输出: occluded[] (哪些阴影射线被遮挡)                                    │
│                                                                             │
│  ⑥ gatherEnergyField()                                                       │
│     输入: energyDelay[], occluded[], shCoefficients[]                        │
│     输出: energy[channel][band][bin] 直方图（脉冲响应）                       │
│                                                                             │
│  ⑦ 交换缓冲区                                                                │
│     mCurrentRayBuffer = 1 - mCurrentRayBuffer                                │
│     → reflectedRays[] 成为下一轮的 rays[]                                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

#### 3.1.4 核心思想总结

整个反射模拟流程的核心思想是：

1. **从听者发出射线探索环境**
2. **射线击中墙面/地面/物体，产生反射**
3. **每次反射计算：贡献多少能量 + 经过多少时间**
4. **弹射回来的射线如果进入听者范围，就算"听到"了这次反射**
5. **把所有"听到"的反射按时间排列，就得到脉冲响应**

**物理模型**：
- **Phong 着色**：漫反射 + 镜面反射
- **距离衰减**：`1/(4πr²)`
- **材质吸收**：低/中/高频分别计算
- **声速延迟**：`340 m/s`

**为什么叫"存算一体"？**
- 传统方式：先读取场景数据，再计算
- 这里：Radeon Rays 在 GPU 显存中直接完成光线追踪计算，无需 CPU 介入
```

### 3.2 OpenCL Kernels 详解

**文件**: `core/src/core/radeonrays_reflection_simulator.cl`

#### 3.2.1 generateListenerRays (射线生成)

```opencl
kernel void generateListenerRays(global CoordinateSpace* listeners,
                                 global float4* sphereSamples,
                                 global Ray* rays) {
    size_t rayIndex = get_global_id(0);       // 射线索引
    size_t listenerIndex = get_global_id(1);  // 听者索引
    size_t index = listenerIndex * get_global_size(0) + rayIndex;
    
    // 从听者位置向球面采样方向发射射线
    rays[index].o = (float4) (listeners[listenerIndex].origin, FLT_MAX);
    rays[index].d = (float4) (sphereSamples[rayIndex].xyz, 0.0f);
    rays[index].extra = (int2) (0xffffffff, 1);
}
```

**功能**: 从听者位置向预计算的球面采样点发射射线，用于探索周围环境。

#### 3.2.2 sphereOcclusion (球体遮挡)

```opencl
kernel void sphereOcclusion(uint numSources,
                            global const CoordinateSpace* sources,
                            uint numListeners,
                            global const CoordinateSpace* listeners,
                            global Ray* rays,
                            global Hit* hits) {
    uint numRays = get_global_size(0);
    uint rayIndex = get_global_id(0);
    
    // 检测射线是否与听者球体相交
    for (int i = 0; i < numListeners; ++i) {
        uint index = i * numRays + rayIndex;
        
        float listenerSphereHitDistance = raySphereIntersect(
            &rays[index], listeners[i].origin, LISTENER_RADIUS
        );
        
        // 如果射线击中 listener 球体，且距离比场景交点更近
        if (0.0f <= listenerSphereHitDistance && 
            listenerSphereHitDistance < hits[index].uvwt.s3) {
            // 标记为被"吸收"
            rays[index].extra.y = 0;
            hits[index].primid = -1;
            return;
        }
        
        // 同样检测源球体
        for (int j = 0; j < numSources; ++j) {
            float sourceSphereHitDistance = raySphereIntersect(
                &rays[index], sources[j].origin, SOURCE_RADIUS
            );
            
            if (0.0f <= sourceSphereHitDistance && 
                sourceSphereHitDistance < hits[index].uvwt.s3) {
                rays[index].extra.y = 0;
                hits[index].primid = -1;
                return;
            }
        }
    }
}
```

**核心逻辑**:
1. 每个 work-item 处理一条射线
2. 检测射线是否与听者/源周围的球体相交
3. 如果球体交点比场景几何更近，则标记射线为"被吸收"
4. 使用 `raySphereIntersect` 进行射线-球体相交测试

**设计意图深度分析**:

源码中 `sphereOcclusion` 只在 `i > 0`（第 2 次弹射及之后）才调用，检测的是 **bounce 射线**（从墙面弹射出来的射线），而非初始射线。

| 检测对象 | 含义 | 单听者场景 |
|---------|------|-----------|
| **声源 (Source)** | bounce 射线在飞向下一面墙的途中，是否先经过了声源球区域 → 确认"声源→墙面→听者"路径有效 | **有意义** |
| **听者 (Listener)** | bounce 射线在飞向下一面墙的途中，是否经过了另一个听者球区域 → 为多听者场景预留 | **多余** |

**为什么对听者的检测在单听者场景下是多余的？**

射线从听者出发，第 1 轮弹射后生成的 bounce 射线从墙面弹射出来，出发位置就在墙面附近，而听者在另一个位置。bounce 射线飞向下一面墙的途中，理论上不可能"飞回"到起点听者自己。

**为什么还要保留对听者的检测？** 推测原因：
1. **多听者 avatar 场景**：VR 或走廊复听者场景中，bounce 射线可能中途被另一个听者 avatar 截获
2. **防御性设计**：保守地保留双向检测逻辑，确保没有遗漏

**调用链**（`radeonrays_reflection_simulator.cpp`）：
```cpp
for (int i = 0; i < numBounces; ++i) {
    QueryIntersection(...);      // GPU 光追：射线与场景求交
    if (i > 0)                   // ← 只在第2次及之后
        sphereOcclusion(...);    // 声源/听者球体截获检测
    shadeAndBounce(...);          // 能量计算 + 生成下一轮反射线
}
```

#### 3.2.3 shadeAndBounce (着色与弹射)

这是最复杂的 kernel，负责：
1. 计算命中点的材质属性
2. 生成阴影射线检测遮挡
3. 计算反射能量和延迟
4. 生成下一轮弹射的反射射线

```opencl
kernel void shadeAndBounce(uint numSources,
                           global const CoordinateSpace* sources,
                           uint numListeners,
                           global const CoordinateSpace* listeners,
                           global const Directivity* directivities,
                           uint numRays,
                           uint numBounces,
                           float irradianceMinDistance,
                           global const Ray* rays,
                           global const Hit* hits,
                           global const float3* normals,
                           global const int* materialIndices,
                           global const Material* materials,
                           uint numDiffuseSamples,
                           global const float4* diffuseSamples,
                           uint randomNumber,
                           float scalar,
                           global Ray* shadowRays,      // 输出: 阴影射线
                           global Ray* reflectedRays,   // 输出: 反射射线
                           global float4* energyDelay,  // 输出: 能量和延迟
                           global float4* accumEnergyDelay) {
    
    // 计算射线索引
    size_t numChunks = max(numListeners, numSources);
    size_t numPrimaryRays = get_global_size(0) / numChunks;
    uint rayIndex = (numListeners > 1) ? get_global_id(0) : (get_global_id(0) % numPrimaryRays);
    uint chunkIndex = get_global_id(0) / numPrimaryRays;
    
    // 跳过被禁用的射线
    if (rays[rayIndex].extra.y == 0 || hits[rayIndex].primid < 0) {
        shadowRays[shadowRayIndex].extra = 0;
        reflectedRays[rayIndex].extra = 0;
        energyDelay[shadowRayIndex] = (float4) 0.0f;
        return;
    }
    
    // 计算命中点
    float3 rayDirection = rays[rayIndex].d.xyz;
    int triangleIndex = hits[rayIndex].primid;
    float hitDistance = hits[rayIndex].uvwt.s3;
    float3 hitPoint = rays[rayIndex].o.xyz + hitDistance * rayDirection;
    
    // 计算命中法线
    float3 hitNormal = normals[triangleIndex].xyz;
    if (dot(hitNormal, rayDirection) > 0.0f) {
        hitNormal = -hitNormal;  // 确保法线朝外
    }
    
    // 获取材质
    Material hitMaterial = materials[materialIndices[triangleIndex]];
    float3 hitMaterialAbsorption = (float3) (
        hitMaterial.absorptionLow, 
        hitMaterial.absorptionMid, 
        hitMaterial.absorptionHigh
    );
    
    // 生成阴影射线 (检测是否被源照亮)
    float3 source = sources[sourceIndex].origin;
    float hitToSourceDistance = distance(hitPoint, source);
    float4 hitToSource = (float4) (normalize(source - hitPoint), 0.0f);
    
    // 跳过无效情况
    if (hitDistance <= LISTENER_RADIUS ||
        hitToSourceDistance <= irradianceMinDistance ||
        dot(hitToSource.xyz, hitNormal) < 0.0f) {
        shadowRays[shadowRayIndex].extra = 0;
        energyDelay[shadowRayIndex] = (float4) 0.0f;
    } else {
        // 生成阴影射线
        shadowRays[shadowRayIndex].o = (float4) (
            hitPoint + RAY_SURFACE_OFFSET * hitToSource.xyz, 
            hitToSourceDistance
        );
        shadowRays[shadowRayIndex].d = hitToSource;
        shadowRays[shadowRayIndex].extra = (int2) (0xffffffff, 1);
        
        // 计算着色值 (漫反射 + 镜面反射)
        float3 energy = (1.0f / PI) * hitMaterial.scattering * 
                        max(0.0f, dot(hitNormal, hitToSource.xyz));
        energy += ((SPECULAR_EXPONENT + 2.0f) / (8.0f * PI)) * 
                  (1.0f - hitMaterial.scattering) * 
                  pow(fabs(dot(normalize(hitToSource.xyz - rayDirection), hitNormal)), 
                      SPECULAR_EXPONENT);
        energy *= scalar;
        energy *= evaluateDirectivity(hitPoint, sources[sourceIndex], 
                                      directivities[sourceIndex]);
        energy *= pointSourceIrradiance(hitToSourceDistance, irradianceMinDistance);
        energy *= accumEnergyDelay[rayIndex].xyz * ((float3) 1.0f - hitMaterialAbsorption);
        
        // 计算延迟
        float delay = (hitDistance + hitToSourceDistance) / SPEED_OF_SOUND;
        delay += accumEnergyDelay[rayIndex].w - 
                 (distance(source, listeners[listenerIndex].origin) / SPEED_OF_SOUND);
        
        energyDelay[shadowRayIndex] = (float4) (energy, delay);
    }
    
    // 生成反射射线 (用于下一轮弹射)
    accumEnergyDelay[rayIndex].xyz *= ((float3) 1.0f - hitMaterialAbsorption);
    accumEnergyDelay[rayIndex].w += hitDistance / SPEED_OF_SOUND;
    
    float4 reflectedDirection;
    if (randomFloat < hitMaterial.scattering) {
        // 漫反射: 随机半球采样
        uint sampleIndex = randomUint % numDiffuseSamples;
        float3 transformedDiffuseSample = transformHemisphereSample(
            diffuseSamples[sampleIndex].xyz, hitNormal
        );
        reflectedDirection = (float4) (transformedDiffuseSample, 0.0f);
    } else {
        // 镜面反射
        reflectedDirection = (float4) (reflect(rayDirection, hitNormal), 0.0f);
    }
    
    reflectedRays[rayIndex].o = (float4) (
        hitPoint + RAY_SURFACE_OFFSET * reflectedDirection.xyz, FLT_MAX
    );
    reflectedRays[rayIndex].d = reflectedDirection;
    reflectedRays[rayIndex].extra = (int2) (0xffffffff, 1);
}
```

**核心算法**:
1. **Phong 着色模型**: 结合漫反射 (`scattering`) 和镜面反射
2. **能量衰减**: 根据材质吸收系数计算能量损失
3. **距离衰减**: 使用 `1/(4πr²)` 物理模型
4. **直接性计算**: 支持自定义源方向性模式
5. **延迟计算**: 基于声速 (340m/s) 计算传播时间

#### 3.2.4 gatherEnergyField (能量场收集)

```opencl
kernel void gatherEnergyField(float scale,
                              global const float4* totalEnergy,
                              uint offset,
                              global const int* occluded,
                              global const float* shCoefficients,
                              global int* energy) {
    // 三阶段处理:
    // 1. 局部直方图累积 (减少原子操作竞争)
    // 2. 局部直方图合并
    // 3. 全局直方图写入
    
    uint rayIndex = get_global_id(0);
    uint band = get_global_id(1);
    uint channel = get_global_id(2);
    
    bool isOccluded = (occluded[offset + rayIndex] >= 0);
    
    if (!isOccluded) {
        // 计算时间 bin
        float time = totalEnergy[offset + rayIndex].w;
        uint bin = convert_uint_sat(floor(time / BIN_DURATION)) * NUM_LOCAL_HISTOGRAMS;
        
        if (bin < NUM_BINS) {
            // 量化能量值并原子累加
            float energyValue = scale * rayEnergy[band] * 
                               shCoefficients[channel * numRays + rayIndex];
            int quantizedEnergyValue = convert_int_sat(floor(energyValue));
            atomic_add(shiftedLocalEnergy + bin, quantizedEnergyValue);
        }
    }
}
```

**优化技巧**:
1. 使用多个局部直方图 (`NUM_LOCAL_HISTOGRAMS=2`) 减少原子操作竞争
2. 直方图交错存储提高内存访问效率
3. 能量值量化后使用整数原子操作

---

## 4. Embree CPU 光追对比

### 4.1 架构差异

| 特性 | Radeon Rays (GPU) | Embree (CPU) |
|-----|-------------------|--------------|
| **并行模型** | OpenCL/SIMT | ISPC/SIMD |
| **射线批量** | 数千条并行 | 64条批次 |
| **内存模型** | 显存 + 统一内存 | 主内存 |
| **BVH 构建** | GPU SAH | CPU SAH |
| **代码形式** | OpenCL C | ISPC |

### 4.2 ISPC 实现 (Embree)

**文件**: `core/src/core/embree_reflection_simulator.ispc`

ISPC (Intel SPMD Program Compiler) 允许用类似 C 的语法编写 SIMD 代码：

```ispc
export void simulateEnergyField(const uniform EmbreeScene* uniform scene,
                                const uniform EmbreeReflectionSimulator* uniform simulator,
                                uniform int startIndex,
                                uniform int endIndex,
                                uniform int threadIndex,
                                uniform EnergyField* uniform energyFields,
                                uniform int numBands) {
    const uniform float scalar = (4.0f * PI) / simulator->numRays;
    
    uniform RandomSampler rng;
    RandomSampler_init(rng, clock());
    
    // foreach 生成 SIMD 代码
    foreach (i = startIndex ... endIndex) {
        float3 rayDirection = { 
            simulator->listenerSamples[0][i], 
            simulator->listenerSamples[1][i], 
            simulator->listenerSamples[2][i] 
        };
        RTCRayHit ray = createRay(simulator->listener->origin, rayDirection, 0.0f, FLT_MAX);
        
        float accumEnergy[IPL_MAX_NUM_BANDS];
        for (uniform int band = 0; band < numBands; ++band) {
            accumEnergy[band] = 1.0f;
        }
        
        float accumDistance = 0.0f;
        
        // 多轮反射
        for (uniform int bounce = 0; bounce < simulator->numBounces; ++bounce) {
            float3 hitPoint, hitNormal;
            Material hitMaterial;
            
            if (!tracePath(*scene, *simulator, ray, bounce, accumDistance, 
                          hitPoint, hitNormal, hitMaterial))
                break;
            
            // 处理每个源
            for (uniform int j = 0; j < simulator->numSources; ++j) {
                float energy[IPL_MAX_NUM_BANDS];
                float delay = 0.0f;
                
                if (!shade(*scene, *simulator, ray, bounce, j, hitPoint, hitNormal,
                          hitMaterial, accumEnergy, accumDistance, scalar, 
                          energy, delay, numBands))
                    continue;
                
                // 原子累加到能量场
                uniform int energyFieldIndex = threadIndex * simulator->numSources + j;
                uniform EnergyField& energyField = energyFields[energyFieldIndex];
                
                uniform int numBins = energyField.numBins;
                int bin = (int) floor(delay / IPL_BIN_DURATION);
                if (bin < 0 || numBins <= bin) continue;
                
                for (uniform int band = 0; band < numBands; ++band) {
                    for (uniform int k = 0; k < energyField.numChannels; ++k) {
                        atomic_add_local(&energyField.data[k][band][bin], 
                                        simulator->listenerCoeffs[k][i] * energy[band]);
                    }
                }
            }
            
            // 弹射
            if (bounce < simulator->numBounces - 1) {
                bouncePath(*scene, *simulator, rng, bounce, hitPoint, hitNormal,
                          hitMaterial, ray, accumEnergy, accumDistance, numBands);
            }
        }
    }
}
```

**关键差异**:
1. ISPC 的 `foreach` 自动生成 SIMD 代码
2. 使用 `uniform` 和 `varying` 区分标量和向量变量
3. 直接调用 Embree C API (`rtcIntersectV`, `rtcOccludedV`)

---

## 5. 性能优化策略

### 5.1 GPU 端优化

1. **射线批量处理**: 一次提交数千条射线，最大化 GPU 利用率
2. **双缓冲**: 使用两个射线缓冲区交替读写，避免数据依赖
3. **局部内存使用**: Kernel 中使用 `local` 内存减少全局内存访问
4. **原子操作优化**: 多直方图策略减少竞争
5. **工作项大小自适应**: `shadeAndBounce` kernel 动态调整 local size

### 5.2 数据传输优化

```cpp
// 使用 CL_MAP_WRITE_INVALIDATE_REGION 避免不必要的数据拷贝
auto* _sources = reinterpret_cast<cl::CoordinateSpace*>(
    clEnqueueMapBuffer(mRadeonRays->openCL().irUpdateQueue(),
                       mSources.buffer(), 
                       CL_TRUE, 
                       CL_MAP_WRITE_INVALIDATE_REGION,  // 关键标志
                       0, mSources.size(), 
                       0, nullptr, nullptr, nullptr)
);

// 写入数据...

clEnqueueUnmapMemObject(...);  // 解除映射
```

### 5.3 BVH 优化

```cpp
// Radeon Rays BVH 配置
mAPI->SetOption("bvh.builder", "sah");      // 表面积启发式构建
mAPI->SetOption("bvh.usesplits", "1");      // 启用分割
```

---

## 6. 数据流总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            完整数据流                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  CPU 端                                                                      │
│  ┌─────────────────┐     ┌─────────────────┐     ┌─────────────────────┐   │
│  │  游戏引擎       │────▶│  Steam Audio    │────▶│  Scene/Mesh Data    │   │
│  │  (Unity/Unreal) │     │  Context        │     │  Material/Listener  │   │
│  └─────────────────┘     └─────────────────┘     └─────────────────────┘   │
│                                   │                                         │
│                                   ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    RadeonRaysReflectionSimulator                     │   │
│  │         (管理 GPU 内存、Kernel 调用、数据同步)                        │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                   │                                         │
│  ═══════════════════════════════════════════════════════════════════════   │
│                                   │                                         │
│  GPU 端 (OpenCL)                                                            │
│  ┌─────────────────┐     ┌─────────────────┐     ┌─────────────────────┐   │
│  │  Scene BVH      │◀───▶│  Radeon Rays    │◀───▶│  Ray/Hit Buffers    │   │
│  │  (GPU Memory)   │     │  Intersection   │     │  (OpenCL Buffers)   │   │
│  └─────────────────┘     └─────────────────┘     └─────────────────────┘   │
│           │                       ▲                       │                 │
│           ▼                       │                       ▼                 │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        OpenCL Kernels                               │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌────────────┐ │   │
│  │  │generateRays │  │sphereOcclus │  │shadeAndBoun │  │gatherEnergy│ │   │
│  │  │             │  │ion          │  │ce          │  │Field       │ │   │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └────────────┘ │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                   │                                         │
│                                   ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        EnergyField / IR                             │   │
│  │                    (脉冲响应结果，传回 CPU)                           │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 7. 相关源文件索引

| 文件路径 | 说明 |
|---------|------|
| `core/src/core/radeonrays_device.cpp/h` | Radeon Rays 设备封装 |
| `core/src/core/radeonrays_scene.cpp/h` | GPU 场景管理 |
| `core/src/core/radeonrays_static_mesh.cpp/h` | GPU 网格数据 |
| `core/src/core/radeonrays_reflection_simulator.cpp/h` | 反射模拟器主逻辑 |
| `core/src/core/radeonrays_reflection_simulator.cl` | OpenCL Kernels |
| `core/src/core/embree_device.cpp/h` | Embree 设备封装 |
| `core/src/core/embree_reflection_simulator.cpp/h` | CPU 反射模拟器 |
| `core/src/core/embree_reflection_simulator.ispc` | ISPC SIMD 代码 |
| `core/src/core/opencl_device.cpp/h` | OpenCL 设备管理 |
| `core/src/core/opencl_buffer.cpp/h` | OpenCL 缓冲区封装 |

---

## 8. 总结

Steam Audio 的 GPU 光线追踪实现基于 **AMD Radeon Rays** 库，通过 OpenCL 在 GPU 上执行大规模并行光线追踪。核心特点：

1. **混合架构**: CPU 负责场景管理和调度，GPU 负责光线追踪计算
2. **多轮弹射**: 支持多次反射模拟，每轮使用双缓冲交替
3. **物理准确**: 基于 Phong 模型、距离衰减、材质吸收的物理模拟
4. **高效并行**: 利用 GPU 的 SIMD 架构批量处理数千条射线
5. **灵活配置**: 支持射线数量、反射次数、时间分辨率等参数调节

GPU 光追相比 CPU 实现可处理 **10-100 倍**的射线数量，适合复杂场景的实时音频模拟。
