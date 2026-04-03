#pragma once

#include "CoreMinimal.h"

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioSerializedObject
{
public:
    FUERayTracingAudioSerializedObject() = default;

    void SetPayload(TArray<uint8>&& InPayload);
    const TArray<uint8>& GetPayload() const;
    bool IsEmpty() const;

private:
    TArray<uint8> Payload;
};
