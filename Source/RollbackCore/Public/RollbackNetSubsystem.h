// Copyright (c) 2026 GregOrigin. MIT Licensed - see LICENSE for details.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/WorldSubsystem.h"
#include "RollbackNetTypes.h"
#include "RollbackNetSubsystem.generated.h"

class FInternetAddr;
class FSocket;
class ISocketSubsystem;
class URollbackStateComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRollbackTransportConnectedSignature, const FString&, RemoteEndpoint);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRollbackTransportErrorSignature, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FRollbackRemoteInputReceivedSignature, int32, PlayerId, int32, Frame, FRollbackInput, Input);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRollbackPeerDisconnectedSignature, int32, PlayerId, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRollbackPeerMaxRetriesSignature, int32, PlayerId, int32, FailedSequence);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FRollbackChecksumMismatchSignature, int32, Frame, int32, LocalChecksum, int32, RemoteChecksum);

UCLASS()
class ROLLBACKCORE_API URollbackNetSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network")
    bool StartUdpPeer(const FRollbackTransportConfig& InConfig, FString& OutError);

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network")
    void StopTransport();

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network")
    bool ConnectToPeer(int32 PlayerId, const FString& RemoteHost, int32 RemotePort, FString& OutError);

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network")
    void DisconnectPeer(int32 PlayerId);

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network")
    bool SendInputFrame(int32 PlayerId, int32 Frame, FRollbackInput Input, bool bReliable);

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network")
    bool ConsumeRemoteInput(int32 PlayerId, int32 Frame, FRollbackInput& OutInput) const;

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network")
    void BufferRemoteInputForRollback(int32 PlayerId, int32 Frame, FRollbackInput Input);

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network")
    void ApplyBufferedInputsToState(URollbackStateComponent* StateComponent, int32 PlayerId, int32 FromFrame, int32 ToFrame, bool& bOutChanged, int32& OutEarliestChangedFrame);

    UFUNCTION(BlueprintPure, Category = "Rollback|Network")
    bool IsTransportRunning() const { return Socket != nullptr; }

    UFUNCTION(BlueprintPure, Category = "Rollback|Network")
    bool IsConnectedToPeer() const;

    UFUNCTION(BlueprintPure, Category = "Rollback|Network")
    FRollbackTransportStats GetTransportStats() const;

    UFUNCTION(BlueprintPure, Category = "Rollback|Network")
    FString GetRemoteEndpointString() const;

    UFUNCTION(BlueprintPure, Category = "Rollback|Network")
    FString GetLocalEndpointString() const;

    UFUNCTION(BlueprintPure, Category = "Rollback|Network")
    TArray<int32> GetConnectedPeerIds() const;

    UFUNCTION(BlueprintPure, Category = "Rollback|Network")
    TArray<FRollbackPeerInfo> GetAllPeerInfo() const;

    /** Worst measured wire round trip (periodic ping/pong echo) and incoming packet loss (sequence
        gaps over a rolling window) across connected peers. Returns false until the first pong. */
    UFUNCTION(BlueprintPure, Category = "Rollback|Network")
    bool GetMeasuredNetQuality(float& OutPingMs, float& OutIncomingLossPercent) const;

    UFUNCTION(BlueprintCallable, Category = "Rollback|Network|Debug", meta = (DevelopmentOnly))
    void FlushTransport();

    /** Records the local sim checksum for a finalized frame, streams it to all peers, and
        compares it against any checksum already received for that frame. */
    void SubmitLocalStateChecksum(int32 Frame, uint32 Checksum);

    /** Drops all stored checksums and ignores frames below MinValidFrame from now on.
        Call whenever the shared timeline is (re)based. */
    void ResetStateChecksums(int32 MinValidFrame);

    UFUNCTION(BlueprintPure, Category = "Rollback|Performance")
    FRollbackPerformanceStats GetPerformanceStats() const { return PerfStats; }

    UPROPERTY(BlueprintAssignable, Category = "Rollback|Network")
    FRollbackTransportConnectedSignature OnTransportConnected;

    UPROPERTY(BlueprintAssignable, Category = "Rollback|Network")
    FRollbackTransportErrorSignature OnTransportError;

    UPROPERTY(BlueprintAssignable, Category = "Rollback|Network")
    FRollbackRemoteInputReceivedSignature OnRemoteInputReceived;

    UPROPERTY(BlueprintAssignable, Category = "Rollback|Network")
    FRollbackPeerDisconnectedSignature OnPeerDisconnected;

    UPROPERTY(BlueprintAssignable, Category = "Rollback|Network")
    FRollbackPeerMaxRetriesSignature OnPeerMaxRetriesExceeded;

    /** Fired when a finalized frame's state checksum differs from the peer's — a confirmed cross-peer desync. */
    UPROPERTY(BlueprintAssignable, Category = "Rollback|Desync")
    FRollbackChecksumMismatchSignature OnStateChecksumMismatch;

private:
    struct FPendingReliablePacket
    {
        TArray<uint8> Payload;
        TSharedPtr<FInternetAddr> Destination;
        double LastSentSeconds = 0.0;
        int32 SendAttempts = 0;
    };

    struct FDelayedPacket
    {
        TArray<uint8> Payload;
        TSharedPtr<FInternetAddr> Endpoint;
        double ReleaseSeconds = 0.0;
    };

    struct FNetworkInputFrame
    {
        int32 PlayerId = 0;
        int32 Frame = 0;
        FRollbackInput Input;
    };

    struct FRemotePeer
    {
        int32 PlayerId = -1;
        TSharedPtr<FInternetAddr> Endpoint;
        bool bConnected = false;
        uint32 NextOutgoingSequence = 1;
        uint32 HighestContiguousReceivedSequence = 0;
        bool bAckDirty = false;
        TMap<uint32, FPendingReliablePacket> PendingReliable;
        TSet<uint32> ReceivedSequences;
        double LastActivityTime = 0.0;
        float LastRoundTripMs = 0.0f;
        int32 PacketsSentToPeer = 0;
        int32 PacketsReceivedFromPeer = 0;
        double LastHeartbeatSentTime = 0.0;
        double LastPingSentTime = 0.0;
        float MeasuredPingMs = -1.0f;
        float MeasuredIncomingLossPercent = 0.0f;
        uint32 LossTrackingHighestSequence = 0;
        uint32 LossWindowBaseSequence = 0;
        int32 LossWindowPacketsReceived = 0;
        double LossWindowStartTime = 0.0;
    };

    bool TickFunction(float DeltaTime);
    void PumpSocket(double NowSeconds);
    void ProcessIncomingPayload(const TArray<uint8>& Payload, TSharedRef<FInternetAddr> Sender, double NowSeconds);
    void ReleaseDelayedIncoming(double NowSeconds);
    void ReleaseDelayedOutgoing(double NowSeconds);
    void ResendReliablePackets(double NowSeconds);
    void CheckPeerTimeouts(double NowSeconds);
    void SendHeartbeats(double NowSeconds);
    void UpdateNetQuality(double NowSeconds);

    bool SendHello(FRemotePeer& Peer);
    bool SendHeartbeat(FRemotePeer& Peer);
    bool SendAck(FRemotePeer& Peer);
    bool SendInputPacket(FRemotePeer& Peer, const TArray<FNetworkInputFrame>& InputFrames, bool bReliable);
    bool SendChecksumPacket(FRemotePeer& Peer, int32 Frame, uint32 Checksum);
    void CompareStateChecksums(int32 Frame);
    bool SendPacket(FRemotePeer& Peer, uint8 PacketType, const TArray<FNetworkInputFrame>& InputFrames, bool bReliable);
    bool SendTimestampPacket(FRemotePeer& Peer, uint8 PacketType, double TimestampSeconds);
    bool SendRawPacket(const TArray<uint8>& Payload, TSharedRef<FInternetAddr> Destination, bool bApplySimulation);
    bool SendRawPacketNow(const TArray<uint8>& Payload, const FInternetAddr& Destination);

    void MarkSequenceReceived(FRemotePeer& Peer, uint32 Sequence);
    void RemoveAckedPackets(FRemotePeer& Peer, uint32 AckSequence, double NowSeconds);
    void CacheRemoteInput(int32 PlayerId, int32 Frame, const FRollbackInput& Input);

    FRemotePeer* FindPeerByEndpoint(const FInternetAddr& Addr);
    FRemotePeer* FindPeerByPlayerId(int32 PlayerId);

    bool ResolveEndpoint(const FString& Host, int32 Port, TSharedPtr<FInternetAddr>& OutEndpoint, FString& OutError) const;

    void UpdatePerformanceStats(double NowSeconds);

public:
    void RecordSimulationTime(float Ms);
    void RecordRollbackTime(float Ms, int32 DepthFrames);
    void RecordSerializeTime(float Ms, int32 StateBytes);
    void RecordDesync();
    void SetEntityCount(int32 Count);

private:
    FRollbackTransportConfig Config;
    FRollbackTransportStats Stats;
    FRollbackPerformanceStats PerfStats;
    FSocket* Socket = nullptr;
    ISocketSubsystem* SocketSubsystem = nullptr;
    TArray<FRemotePeer> Peers;

    TMap<int32, TMap<int32, FRollbackInput>> RemoteInputBuffer;
    TMap<int32, FRollbackInput> LocalInputHistory;
    TMap<int32, uint32> LocalStateChecksums;
    TMap<int32, uint32> RemoteStateChecksums;
    int32 ChecksumMinValidFrame = 0;
    int32 FirstChecksumMismatchFrame = -1;
    double LastChecksumMismatchLogTime = 0.0;
    TArray<FDelayedPacket> DelayedIncomingPackets;
    TArray<FDelayedPacket> DelayedOutgoingPackets;

    FTSTicker::FDelegateHandle TickHandle;

    static constexpr int32 PerfHistorySize = 60;
    TArray<float> PerfSimulationTimes;
    TArray<float> PerfRollbackTimes;
    TArray<float> PerfSerializeTimes;
    TArray<int32> PerfStateSizes;
    double PerfLastBandwidthTime = 0.0;
    int32 PerfBytesSentLastWindow = 0;
    int32 PerfBytesReceivedLastWindow = 0;
    TArray<int32> PerfRollbackFrameTimestamps;
};
