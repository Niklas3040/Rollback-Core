// Copyright (c) 2026 GregOrigin. All Rights Reserved.

#include "RollbackManager.h"
#include "Algo/Sort.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Crc.h"
#include "RollbackCore.h"
#include "RollbackCoreSettings.h"
#include "RollbackDemoEnvironment.h"
#include "RollbackNetSubsystem.h"

// Entities register either as a URollbackStateComponent or as an actor owning one.
static const URollbackStateComponent* ResolveStateComp(const TScriptInterface<IRollbackEntity>& Entity)
{
    if (const URollbackStateComponent* StateComp = Cast<URollbackStateComponent>(Entity.GetObject()))
    {
        return StateComp;
    }
    if (const AActor* Actor = Cast<AActor>(Entity.GetObject()))
    {
        return Actor->FindComponentByClass<URollbackStateComponent>();
    }
    return nullptr;
}

void URollbackManager::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    CurrentFrame = 0;

    if (const URollbackCoreSettings* Settings = GetDefault<URollbackCoreSettings>())
    {
        FixedTimeStep = Settings->FixedTickRateHz > 0
            ? 1.0f / static_cast<float>(Settings->FixedTickRateHz)
            : 1.0f / 60.0f;
        bEnableVisualDebugging = Settings->bEnableVisualDebuggingByDefault;
        DebugLiveFrameLag = Settings->DebugLiveFrameLag;
    }

    Collection.InitializeDependency(URollbackNetSubsystem::StaticClass());
    if (URollbackNetSubsystem* Net = GetWorld() ? GetWorld()->GetSubsystem<URollbackNetSubsystem>() : nullptr)
    {
        Net->OnStateChecksumMismatch.AddDynamic(this, &URollbackManager::HandleStateChecksumMismatch);
    }

    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &URollbackManager::TickFunction));
}

void URollbackManager::Deinitialize()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    Super::Deinitialize();
}

bool URollbackManager::TickFunction(float DeltaTime)
{
    Tick(DeltaTime);
    return true;
}

void URollbackManager::Tick(float DeltaTime)
{
    if (bHaltedOnDesync)
    {
        return;
    }

    Accumulator += DeltaTime;
    while (Accumulator >= FixedTimeStep)
    {
        Accumulator -= FixedTimeStep;
        if (PendingTimeSyncStallFrames > 0)
        {
            PendingTimeSyncStallFrames--;
            TimeSyncStalledFrameCount++;
            continue;
        }
        AdvanceFrame();
    }
}

void URollbackManager::RequestTimeSyncStall(int32 Frames)
{
    PendingTimeSyncStallFrames = FMath::Clamp(PendingTimeSyncStallFrames + Frames, 0, MaxPendingTimeSyncStallFrames);
}

void URollbackManager::RegisterEntity(TScriptInterface<IRollbackEntity> Entity)
{
    if (!RegisteredEntities.Contains(Entity))
    {
        RegisteredEntities.Add(Entity);
    }

    if (UWorld* World = GetWorld())
    {
        if (URollbackNetSubsystem* Net = World->GetSubsystem<URollbackNetSubsystem>())
        {
            Net->SetEntityCount(RegisteredEntities.Num());
        }
    }
}

void URollbackManager::UnregisterEntity(TScriptInterface<IRollbackEntity> Entity)
{
    RegisteredEntities.Remove(Entity);

    if (UWorld* World = GetWorld())
    {
        if (URollbackNetSubsystem* Net = World->GetSubsystem<URollbackNetSubsystem>())
        {
            Net->SetEntityCount(RegisteredEntities.Num());
        }
    }
}

void URollbackManager::AdvanceFrame()
{
    const double SimStart = FPlatformTime::Seconds();
    SimulateFrame(CurrentFrame);
    const double SimEnd = FPlatformTime::Seconds();
    const float SimMs = static_cast<float>((SimEnd - SimStart) * 1000.0);

    if (UWorld* World = GetWorld())
    {
        if (URollbackNetSubsystem* Net = World->GetSubsystem<URollbackNetSubsystem>())
        {
            Net->RecordSimulationTime(SimMs);
        }
    }

    if (bDebugScrubFollowsLive)
    {
        DebugScrubFrame = FMath::Max(0, CurrentFrame - DebugLiveFrameLag);
    }
    CurrentFrame++;

    URollbackNetSubsystem* Net = GetWorld() ? GetWorld()->GetSubsystem<URollbackNetSubsystem>() : nullptr;

    if (Net)
    {
        Net->SetLocalSimClock(CurrentFrame, FixedTimeStep > 0.0f ? 1.0f / FixedTimeStep : 60.0f);
    }

    // Stream the checksum of the newest frame old enough that no rollback can rewrite it.
    const int32 FinalizedFrame = CurrentFrame - StateChecksumLagFrames;
    if (Net && FinalizedFrame >= 0 && Net->IsTransportRunning() && Net->IsConnectedToPeer())
    {
        Net->SubmitLocalStateChecksum(FinalizedFrame, ComputeStateChecksum(FinalizedFrame));
    }

    if (CurrentFrame % TelemetryLogIntervalFrames == 0 && RegisteredEntities.Num() > 0)
    {
        UE_LOG(LogRollbackCore, Display, TEXT("f=%d entities=%d rollbacks=%d (+%d in window, max depth %d) desyncs=%d"),
            CurrentFrame, RegisteredEntities.Num(), RollbackCount, RollbackCount - TelemetryLastRollbackCount, TelemetryMaxDepth, DesyncCount);
        TelemetryLastRollbackCount = RollbackCount;
        TelemetryMaxDepth = 0;
    }
}

void URollbackManager::SimulateFrame(int32 Frame)
{
    for (auto& Entity : RegisteredEntities)
    {
        if (Entity)
        {
            Entity->RollbackTick(FixedTimeStep, Frame);
        }
    }
    
    for (auto& Entity : RegisteredEntities)
    {
        if (Entity)
        {
            Entity->SaveRollbackState(Frame);
            if (bIsReplayingRollback)
            {
                LastRollbackSavedStates++;
            }
        }
    }
    
    if (bEnableVisualDebugging)
    {
        DrawDebugState(Frame - 5);
    }
}

void URollbackManager::RollbackToFrame(int32 Frame, int32 EarliestMismatchFrame)
{
    if (Frame >= CurrentFrame || Frame < 0) return;

    const double RollbackStart = FPlatformTime::Seconds();

    TMap<FString, TMap<int32, int32>> PreRollbackChecksums;
    for (auto& Entity : RegisteredEntities)
    {
        const URollbackStateComponent* StateComp = ResolveStateComp(Entity);
        if (!StateComp) continue;

        FString EntityName = StateComp->GetName();
        TMap<int32, int32>& EntityChecksums = PreRollbackChecksums.FindOrAdd(EntityName);
        for (int32 CFrame = Frame; CFrame < CurrentFrame; ++CFrame)
        {
            if (const FRollbackFrameState* State = StateComp->StateBuffer.Find(CFrame))
            {
                int32 Checksum = State->ActorData.Num() > 0
                    ? static_cast<int32>(FCrc::MemCrc32(State->ActorData.GetData(), State->ActorData.Num()))
                    : 0;
                EntityChecksums.Add(CFrame, Checksum);
            }
        }
    }

    RollbackCount++;
    LastRollbackFrame = Frame;
    LastRollbackEndFrame = CurrentFrame;
    LastRollbackFramesReplayed = CurrentFrame - Frame;
    LastRollbackRestoredStates = 0;
    LastRollbackSavedStates = 0;
    TelemetryMaxDepth = FMath::Max(TelemetryMaxDepth, LastRollbackFramesReplayed);
    UE_LOG(LogRollbackCore, Verbose, TEXT("Rollback to frame %d (depth %d)."), Frame, LastRollbackFramesReplayed);

    for (auto& Entity : RegisteredEntities)
    {
        if (Entity)
        {
            Entity->LoadRollbackState(Frame);
            LastRollbackRestoredStates++;
        }
    }
    
    bIsReplayingRollback = true;
    for (int32 SimFrame = Frame; SimFrame < CurrentFrame; ++SimFrame)
    {
        SimulateFrame(SimFrame);

        if (EarliestMismatchFrame < 0 || SimFrame < EarliestMismatchFrame)
        {
            for (auto& Entity : RegisteredEntities)
            {
                const URollbackStateComponent* StateComp = ResolveStateComp(Entity);
                if (!StateComp) continue;

                FString EntityName = StateComp->GetName();
                const TMap<int32, int32>* EntityChecksums = PreRollbackChecksums.Find(EntityName);
                if (!EntityChecksums) continue;

                const int32* PreChecksum = EntityChecksums->Find(SimFrame);
                if (!PreChecksum) continue;

                if (const FRollbackFrameState* State = StateComp->StateBuffer.Find(SimFrame))
                {
                    int32 PostChecksum = State->ActorData.Num() > 0
                        ? static_cast<int32>(FCrc::MemCrc32(State->ActorData.GetData(), State->ActorData.Num()))
                        : 0;
                    if (PostChecksum != *PreChecksum)
                    {
                        DesyncCount++;
                        LastDesyncFrame = SimFrame;
                        OnDesyncDetected.Broadcast(SimFrame, EntityName, PostChecksum ^ (*PreChecksum));

                        if (UWorld* World = GetWorld())
                        {
                            if (URollbackNetSubsystem* Net = World->GetSubsystem<URollbackNetSubsystem>())
                            {
                                Net->RecordDesync();
                            }
                        }
                    }
                }
            }
        }
    }
    bIsReplayingRollback = false;

    const double RollbackEnd = FPlatformTime::Seconds();
    const float RollbackMs = static_cast<float>((RollbackEnd - RollbackStart) * 1000.0);

    if (UWorld* World = GetWorld())
    {
        if (URollbackNetSubsystem* Net = World->GetSubsystem<URollbackNetSubsystem>())
        {
            Net->RecordRollbackTime(RollbackMs, CurrentFrame - Frame);
        }
    }
}

uint32 URollbackManager::ComputeStateChecksum(int32 Frame) const
{
    // SaveGame bytes only: the snapshot transform is captured from engine components, whose
    // storage passes through cached, tolerance-gated conversions and is not bit-stable.
    // Authoritative transform state must live in SaveGame properties.
    // Wrapping sum of per-entity CRCs so peers with different registration order still agree.
    uint32 Sum = 0;
    for (const TScriptInterface<IRollbackEntity>& Entity : RegisteredEntities)
    {
        const URollbackStateComponent* StateComp = ResolveStateComp(Entity);
        if (!StateComp)
        {
            continue;
        }

        const FRollbackFrameState* State = StateComp->StateBuffer.Find(Frame);
        if (!State)
        {
            continue;
        }

        Sum += State->ActorData.Num() > 0
            ? FCrc::MemCrc32(State->ActorData.GetData(), State->ActorData.Num())
            : 0;
    }
    return Sum;
}

void URollbackManager::HandleStateChecksumMismatch(int32 Frame, int32 LocalChecksum, int32 RemoteChecksum)
{
    DesyncCount++;
    LastDesyncFrame = Frame;
    OnDesyncDetected.Broadcast(Frame, TEXT("CrossPeer"), LocalChecksum ^ RemoteChecksum);

    // Once diverged every later frame mismatches too; only the first one is diagnostic.
    if (DesyncCount > 1)
    {
        return;
    }
    FirstDesyncFrame = Frame;

    // Full precision: cross-peer divergence regularly starts below display precision.
    for (const FRollbackDebugFrameRecord& Record : GetDebugFrameRecords(Frame))
    {
        UE_LOG(LogRollbackCore, Error, TEXT("  %s: loc=(%.17g, %.17g, %.17g) vel=(%.17g, %.17g, %.17g) rot=(%.17g, %.17g, %.17g) bytes=%d crc=%08X input[btn=%d axes=(%.17g, %.17g)]"),
            *Record.EntityName,
            Record.Location.X, Record.Location.Y, Record.Location.Z,
            Record.Velocity.X, Record.Velocity.Y, Record.Velocity.Z,
            Record.Rotation.Pitch, Record.Rotation.Yaw, Record.Rotation.Roll,
            Record.SavedByteCount, static_cast<uint32>(Record.SavedChecksum),
            Record.Input.Buttons, Record.Input.Axes.X, Record.Input.Axes.Y);
    }

    ensureAlwaysMsgf(false, TEXT("Cross-peer desync at frame %d (local %08X != remote %08X). The frame is past every correction window and cannot be repaired."),
        Frame, static_cast<uint32>(LocalChecksum), static_cast<uint32>(RemoteChecksum));

    if (bHaltOnDesync)
    {
        bHaltedOnDesync = true;
        if (UWorld* World = GetWorld())
        {
            if (APlayerController* PC = World->GetFirstPlayerController())
            {
                PC->SetPause(true);
            }
        }
        UE_LOG(LogRollbackCore, Error, TEXT("Simulation halted at frame %d; both peers keep their state buffers for inspection."), CurrentFrame);
    }
}

void URollbackManager::DrawDebugState(int32 Frame)
{
    UWorld* World = GetWorld();
    if (!World) return;

    for (auto& Entity : RegisteredEntities)
    {
        const URollbackStateComponent* StateComp = ResolveStateComp(Entity);
        if (const FRollbackFrameState* State = StateComp ? StateComp->StateBuffer.Find(Frame) : nullptr)
        {
            DrawDebugBox(World, State->Location, FVector(50.f), State->Rotation, FColor::Cyan, false, 1.0f, 0, 2.f);
        }
    }
}

TArray<FRollbackDebugFrameRecord> URollbackManager::GetDebugFrameRecords(int32 Frame) const
{
    TArray<FRollbackDebugFrameRecord> Records;

    for (const TScriptInterface<IRollbackEntity>& Entity : RegisteredEntities)
    {
        const URollbackStateComponent* StateComp = ResolveStateComp(Entity);
        if (!StateComp)
        {
            continue;
        }

        FRollbackDebugFrameRecord Record;
        Record.Frame = Frame;
        if (const AActor* Owner = StateComp->GetOwner())
        {
            Record.EntityName = Owner->GetName();
        }
        else
        {
            Record.EntityName = StateComp->GetName();
        }

        Record.Input = StateComp->GetInputForFrame(Frame);
        if (const FRollbackFrameState* State = StateComp->StateBuffer.Find(Frame))
        {
            Record.bStateAvailable = true;
            Record.Location = State->Location;
            Record.Rotation = State->Rotation.Rotator();
            Record.Velocity = State->Velocity;
            Record.SavedByteCount = State->ActorData.Num();
            Record.SavedChecksum = State->ActorData.Num() > 0
                ? static_cast<int32>(FCrc::MemCrc32(State->ActorData.GetData(), State->ActorData.Num()))
                : 0;
        }

        Records.Add(MoveTemp(Record));
    }

    return Records;
}

TArray<int32> URollbackManager::GetAvailableDebugFrames() const
{
    TSet<int32> UniqueFrames;
    for (const TScriptInterface<IRollbackEntity>& Entity : RegisteredEntities)
    {
        const URollbackStateComponent* StateComp = ResolveStateComp(Entity);
        if (!StateComp)
        {
            continue;
        }

        for (const TPair<int32, FRollbackFrameState>& StatePair : StateComp->StateBuffer)
        {
            UniqueFrames.Add(StatePair.Key);
        }
    }

    TArray<int32> Frames = UniqueFrames.Array();
    Algo::Sort(Frames);
    return Frames;
}

int32 URollbackManager::GetOldestAvailableDebugFrame() const
{
    const TArray<int32> Frames = GetAvailableDebugFrames();
    return Frames.Num() > 0 ? Frames[0] : -1;
}

int32 URollbackManager::GetNewestAvailableDebugFrame() const
{
    const TArray<int32> Frames = GetAvailableDebugFrames();
    return Frames.Num() > 0 ? Frames.Last() : -1;
}

bool URollbackManager::SetDebugScrubFrame(int32 Frame)
{
    const int32 OldestFrame = GetOldestAvailableDebugFrame();
    const int32 NewestFrame = GetNewestAvailableDebugFrame();
    if (OldestFrame < 0 || NewestFrame < 0)
    {
        DebugScrubFrame = -1;
        return false;
    }

    DebugScrubFrame = FMath::Clamp(Frame, OldestFrame, NewestFrame);
    bDebugScrubFollowsLive = false;
    return DebugScrubFrame == Frame;
}

bool URollbackManager::StepDebugScrubFrame(int32 FrameDelta)
{
    const int32 BaseFrame = DebugScrubFrame >= 0 ? DebugScrubFrame : GetNewestAvailableDebugFrame();
    if (BaseFrame < 0)
    {
        return false;
    }

    return SetDebugScrubFrame(BaseFrame + FrameDelta);
}

void URollbackManager::SetDebugScrubFollowLive(bool bInFollowLive)
{
    bDebugScrubFollowsLive = bInFollowLive;
    if (bDebugScrubFollowsLive)
    {
        DebugScrubFrame = FMath::Max(0, CurrentFrame - DebugLiveFrameLag);
    }
}

static FAutoConsoleCommandWithWorld CVarSpawnRollbackDemo(
    TEXT("Rollback.SpawnDemo"),
    TEXT("Spawns the Rollback Core Spectacular Demo Environment"),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
    {
        if (World)
        {
            World->SpawnActor<ARollbackDemoEnvironment>(FVector(0,0,100), FRotator::ZeroRotator);
        }
    })
);
