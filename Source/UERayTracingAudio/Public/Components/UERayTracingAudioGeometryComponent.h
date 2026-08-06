#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Scene/UERayTracingAudioScene.h"
#include "UERayTracingAudioGeometryComponent.generated.h"

class UPrimitiveComponent;

UENUM(BlueprintType)
enum class EUERayTracingAudioGeometryExportMode : uint8
{
    BoundingBox UMETA(DisplayName = "Bounding Box"),
    StaticMeshTriangles UMETA(DisplayName = "Static Mesh Triangles")
};

UCLASS(ClassGroup = (UERayTracingAudio), meta = (BlueprintSpawnableComponent))
class UERAYTRACINGAUDIO_API UUERayTracingAudioGeometryComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UUERayTracingAudioGeometryComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    bool BuildGeometryExport(FUERayTracingAudioGeometryExport& OutGeometryExport) const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Geometry)
    bool bExportToAcousticScene;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Geometry)
    bool bAffectsDirectSound;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Geometry)
    bool bAffectsIndirectSound;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Geometry)
    EUERayTracingAudioGeometryExportMode ExportMode;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustic Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    FVector Absorption;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustic Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    FVector Transmission;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustic Material", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Scattering;
};
