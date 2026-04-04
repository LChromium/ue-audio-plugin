#include "Simulation/UERayTracingAudioEnergyFieldShaders.h"

IMPLEMENT_GLOBAL_SHADER(FUERayTracingAudioGenerateListenerRaysCS, "/Plugin/UERayTracingAudio/Private/Simulation/UERayTracingAudioEnergyField.usf", "GenerateListenerRaysCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUERayTracingAudioShadeAndGatherCS, "/Plugin/UERayTracingAudio/Private/Simulation/UERayTracingAudioEnergyField.usf", "ShadeAndGatherCS", SF_Compute);
