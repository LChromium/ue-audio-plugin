#include "Serialization/UERayTracingAudioSerializedObject.h"

void FUERayTracingAudioSerializedObject::SetPayload(TArray<uint8>&& InPayload)
{
    Payload = MoveTemp(InPayload);
}

const TArray<uint8>& FUERayTracingAudioSerializedObject::GetPayload() const
{
    return Payload;
}

bool FUERayTracingAudioSerializedObject::IsEmpty() const
{
    return Payload.IsEmpty();
}
