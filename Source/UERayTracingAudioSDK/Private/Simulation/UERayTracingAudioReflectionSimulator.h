#pragma once

#include "CoreMinimal.h"

#include "API/UERayTracingAudioContext.h"
#include "RayTracing/UERayTracingAudioRayTracingDevice.h"
#include "Simulation/UERayTracingAudioSimulator.h"

class FUERayTracingAudioScene;

class FUERayTracingAudioReflectionSimulator
{
public:
    explicit FUERayTracingAudioReflectionSimulator(const FUERayTracingAudioContext& InContext);

    void Simulate(
        const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
        const FUERayTracingAudioIndirectSimulationInput& Input,
        float EarlyLateSplitSeconds,
        FUERayTracingAudioMinimalEnergyField& OutEnergyField,
        int32& OutNumValidContributions) const;

private:
    const FUERayTracingAudioContext& Context;
};
