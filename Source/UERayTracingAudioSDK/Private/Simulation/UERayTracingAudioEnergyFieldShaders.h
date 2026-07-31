#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

struct FReflectionPathStateGPU
{
    FVector4f OriginAndTravelDistance = FVector4f::Zero();
    FVector4f DirectionAndAlive = FVector4f::Zero();
    FVector4f ThroughputAndPadding = FVector4f::Zero();
};

struct FShadeAndGatherPathInputGPU
{
    FVector4f RayOriginAndTravelDistance = FVector4f::Zero();
    FVector4f RayDirectionAndBounceIndex = FVector4f::Zero();
    FVector4f ThroughputAndHitDistance = FVector4f::Zero();
    FVector4f HitLocationAndHitValid = FVector4f::Zero();
    FVector4f HitNormalAndOccluded = FVector4f::Zero();
    FVector4f AbsorptionAndPadding = FVector4f::Zero();
    FVector4f TransmissionAndScattering = FVector4f::Zero();
    FVector4f ListenerDirectionAndPadding = FVector4f::Zero();
};

struct FShadeAndGatherPathOutputGPU
{
    FVector4f NextOriginAndTravelDistance = FVector4f::Zero();
    FVector4f NextDirectionAndAlive = FVector4f::Zero();
    FVector4f ThroughputAndPadding = FVector4f::Zero();
    FVector4f ListenerDirectionAndContribution = FVector4f::Zero();
};

class FUERayTracingAudioGenerateListenerRaysCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FUERayTracingAudioGenerateListenerRaysCS);
    SHADER_USE_PARAMETER_STRUCT(FUERayTracingAudioGenerateListenerRaysCS, FGlobalShader);

public:
    static constexpr uint32 ThreadGroupSizeX = 64;

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
    }

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FReflectionPathStateGPU>, OutPaths)
        SHADER_PARAMETER(uint32, NumPaths)
        SHADER_PARAMETER(FVector3f, ListenerLocation)
        SHADER_PARAMETER(FVector3f, ListenerForward)
    END_SHADER_PARAMETER_STRUCT()
};

class FUERayTracingAudioShadeAndGatherCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FUERayTracingAudioShadeAndGatherCS);
    SHADER_USE_PARAMETER_STRUCT(FUERayTracingAudioShadeAndGatherCS, FGlobalShader);

public:
    static constexpr uint32 ThreadGroupSizeX = 64;

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
    }

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(StructuredBuffer<FShadeAndGatherPathInputGPU>, PathInputs)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FShadeAndGatherPathOutputGPU>, PathOutputs)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, EnergyBins)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, ContributionCounter)
        SHADER_PARAMETER(uint32, NumPaths)
        SHADER_PARAMETER(uint32, NumDelayBins)
        SHADER_PARAMETER(uint32, NumReflectionRays)
        SHADER_PARAMETER(float, SpeedOfSound)
        SHADER_PARAMETER(float, ReferenceDistance)
        SHADER_PARAMETER(float, DurationSeconds)
        SHADER_PARAMETER(float, DelayBinDurationSeconds)
        SHADER_PARAMETER(float, EnergyQuantizationScale)
        SHADER_PARAMETER(FVector3f, SourceLocation)
        SHADER_PARAMETER(FVector3f, AirAbsorptionPerMeter)
    END_SHADER_PARAMETER_STRUCT()
};
