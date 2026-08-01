#include "Bake/UERayTracingAudioBakeJob.h"

void FUERayTracingAudioBakeJob::Cancel()
{
    if (State == EUERayTracingAudioBakeJobState::Pending || State == EUERayTracingAudioBakeJobState::Running)
    {
        State = EUERayTracingAudioBakeJobState::Cancelled;
        Progress = 0.0f;
        StatusText = TEXT("Cancelled");
    }
    if (DirectQuery.IsValid())
    {
        DirectQuery->Cancel();
        DirectQuery.Reset();
    }
    if (Query.IsValid())
    {
        Query->Cancel();
        Query.Reset();
    }
}

EUERayTracingAudioBakeJobState FUERayTracingAudioBakeJob::GetState() const
{
    return State;
}

float FUERayTracingAudioBakeJob::GetProgress() const
{
    return Progress;
}

const FString& FUERayTracingAudioBakeJob::GetStatusText() const
{
    return StatusText;
}

const FString& FUERayTracingAudioBakeJob::GetError() const
{
    return Error;
}

bool FUERayTracingAudioBakeJob::GetResult(FUERayTracingAudioBakeResult& OutResult) const
{
    if (State != EUERayTracingAudioBakeJobState::Completed)
    {
        return false;
    }

    OutResult = Result;
    return true;
}

void FUERayTracingAudioBakeJob::SetFailed(FString InError)
{
    if (DirectQuery.IsValid())
    {
        DirectQuery->Cancel();
        DirectQuery.Reset();
    }
    if (Query.IsValid())
    {
        Query->Cancel();
        Query.Reset();
    }
    State = EUERayTracingAudioBakeJobState::Failed;
    Progress = 0.0f;
    StatusText = TEXT("Failed");
    Error = MoveTemp(InError);
}
