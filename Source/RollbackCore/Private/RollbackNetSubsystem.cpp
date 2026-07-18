// Copyright (c) 2026 GregOrigin. All Rights Reserved.

#include "RollbackNetSubsystem.h"
#include "RollbackCore.h"
#include "RollbackStateComponent.h"

#include "AddressInfoTypes.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace RollbackNet
{
    static constexpr uint32 PacketMagic = 0x52424350; // RBCP
    static constexpr uint8 PacketVersion = 2;

    enum class EWirePacketType : uint8
    {
        Hello = 1,
        Input = 2,
        Ack = 3,
        Heartbeat = 4,
        Ping = 5,
        Pong = 6,
        Checksum = 7,
        GameMsg = 8,
    };

    // Frames of checksum history kept for cross-peer comparison.
    static constexpr int32 ChecksumHistoryFrames = 600;

    static constexpr double PingIntervalSeconds = 0.25;
    static constexpr double LossWindowSeconds = 2.0;

    static constexpr int32 GameMessageChunkBytes = 1024;
    static constexpr int32 MaxGameMessageBytes = 4 * 1024 * 1024;
    static constexpr int32 MaxGameMessageChunks = MaxGameMessageBytes / GameMessageChunkBytes;
}

void URollbackNetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &URollbackNetSubsystem::TickFunction));
    PerfLastBandwidthTime = FPlatformTime::Seconds();
}

void URollbackNetSubsystem::Deinitialize()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    StopTransport();
    Super::Deinitialize();
}

bool URollbackNetSubsystem::StartUdpPeer(const FRollbackTransportConfig& InConfig, FString& OutError)
{
    StopTransport();

    Config = InConfig;
    Stats = FRollbackTransportStats();
    PerfStats = FRollbackPerformanceStats();
    Peers.Reset();
    RemoteInputBuffer.Reset();
    NewestRemoteInputFrames.Reset();
    LocalInputHistory.Reset();
    GameMessageAssemblies.Reset();
    bLocalSimClockValid = false;
    DelayedIncomingPackets.Reset();
    DelayedOutgoingPackets.Reset();
    PerfSimulationTimes.Reset();
    PerfRollbackTimes.Reset();
    PerfSerializeTimes.Reset();
    PerfStateSizes.Reset();
    PerfBytesSentLastWindow = 0;
    PerfBytesReceivedLastWindow = 0;
    PerfLastBandwidthTime = FPlatformTime::Seconds();
    PerfRollbackFrameTimestamps.Reset();
    LocalStateChecksums.Reset();
    RemoteStateChecksums.Reset();
    ChecksumMinValidFrame = 0;
    FirstChecksumMismatchFrame = -1;

    const FName RequestedSubsystem = Config.SocketSubsystemName.IsNone() ? PLATFORM_SOCKETSUBSYSTEM : Config.SocketSubsystemName;
    SocketSubsystem = ISocketSubsystem::Get(RequestedSubsystem);
    if (!SocketSubsystem)
    {
        OutError = FString::Printf(TEXT("Socket subsystem '%s' is not available."), *RequestedSubsystem.ToString());
        OnTransportError.Broadcast(OutError);
        return false;
    }

    Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("RollbackCore UDP Transport"), NAME_None);
    if (!Socket)
    {
        OutError = TEXT("Failed to create UDP socket.");
        OnTransportError.Broadcast(OutError);
        return false;
    }

    Socket->SetNonBlocking(true);
    Socket->SetReuseAddr(true);

    int32 NewBufferSize = 0;
    Socket->SetSendBufferSize(Config.SendBufferBytes, NewBufferSize);
    Socket->SetReceiveBufferSize(Config.ReceiveBufferBytes, NewBufferSize);

    TSharedRef<FInternetAddr> BindAddress = SocketSubsystem->CreateInternetAddr();
    BindAddress->SetAnyAddress();
    BindAddress->SetPort(Config.LocalPort);

    if (!Socket->Bind(*BindAddress))
    {
        OutError = FString::Printf(TEXT("Failed to bind UDP socket to port %d."), Config.LocalPort);
        OnTransportError.Broadcast(OutError);
        StopTransport();
        return false;
    }

    TSharedRef<FInternetAddr> BoundAddress = SocketSubsystem->CreateInternetAddr();
    Socket->GetAddress(*BoundAddress);
    Config.LocalPort = BoundAddress->GetPort();

    UE_LOG(LogRollbackCore, Display, TEXT("Transport started on UDP %d (redundancy %d frames, packet-loss sim %s)."),
        Config.LocalPort, Config.InputRedundancyFrames, Config.bEnablePacketLossSimulation ? TEXT("on") : TEXT("off"));

    if (!Config.RemoteHost.IsEmpty() && Config.RemotePort > 0)
    {
        if (!ConnectToPeer(-1, Config.RemoteHost, Config.RemotePort, OutError))
        {
            return false;
        }
    }

    return true;
}

void URollbackNetSubsystem::StopTransport()
{
    for (FRemotePeer& Peer : Peers)
    {
        if (Peer.bConnected)
        {
            Peer.bConnected = false;
            OnPeerDisconnected.Broadcast(Peer.PlayerId, TEXT("Transport stopped."));
        }
    }
    Peers.Reset();

    if (Socket && SocketSubsystem)
    {
        Socket->Close();
        SocketSubsystem->DestroySocket(Socket);
    }

    Socket = nullptr;
    SocketSubsystem = nullptr;
    DelayedIncomingPackets.Reset();
    DelayedOutgoingPackets.Reset();
    GameMessageAssemblies.Reset();
}

bool URollbackNetSubsystem::ConnectToPeer(int32 PlayerId, const FString& RemoteHost, int32 RemotePort, FString& OutError)
{
    if (!Socket || !SocketSubsystem)
    {
        OutError = TEXT("StartUdpPeer must succeed before connecting to a remote peer.");
        OnTransportError.Broadcast(OutError);
        return false;
    }

    if (Peers.Num() >= Config.MaxPeers)
    {
        OutError = FString::Printf(TEXT("Maximum peer count (%d) reached."), Config.MaxPeers);
        OnTransportError.Broadcast(OutError);
        return false;
    }

    if (PlayerId >= 0)
    {
        if (FRemotePeer* Existing = FindPeerByPlayerId(PlayerId))
        {
            OutError = FString::Printf(TEXT("Already connected to player %d."), PlayerId);
            return false;
        }
    }

    TSharedPtr<FInternetAddr> Endpoint;
    if (!ResolveEndpoint(RemoteHost, RemotePort, Endpoint, OutError))
    {
        OnTransportError.Broadcast(OutError);
        return false;
    }

    // One peer per endpoint: StartUdpPeer's auto-connect (PlayerId -1) followed by an explicit
    // ConnectToPeer must not create a duplicate that double-sends every packet.
    if (FRemotePeer* Existing = FindPeerByEndpoint(*Endpoint))
    {
        if (Existing->PlayerId < 0)
        {
            Existing->PlayerId = PlayerId;
        }
        return true;
    }

    FRemotePeer NewPeer;
    NewPeer.PlayerId = PlayerId;
    NewPeer.Endpoint = Endpoint;
    NewPeer.bConnected = true;
    NewPeer.LastActivityTime = FPlatformTime::Seconds();
    NewPeer.LastHeartbeatSentTime = FPlatformTime::Seconds();
    Peers.Add(MoveTemp(NewPeer));

    Stats.ConnectedPeerCount = Peers.Num();

    FRemotePeer& Peer = Peers.Last();
    const FString EndpointStr = Peer.Endpoint->ToString(true);
    OnTransportConnected.Broadcast(EndpointStr);

    return SendHello(Peer);
}

void URollbackNetSubsystem::DisconnectPeer(int32 PlayerId)
{
    int32 Index = INDEX_NONE;
    for (int32 i = 0; i < Peers.Num(); ++i)
    {
        if (Peers[i].PlayerId == PlayerId)
        {
            Index = i;
            break;
        }
    }

    if (Index == INDEX_NONE)
    {
        return;
    }

    FRemotePeer Peer = MoveTemp(Peers[Index]);
    Peers.RemoveAtSwap(Index);
    Stats.ConnectedPeerCount = Peers.Num();

    OnPeerDisconnected.Broadcast(Peer.PlayerId, TEXT("Disconnected by local request."));
}

bool URollbackNetSubsystem::SendInputFrame(int32 PlayerId, int32 Frame, FRollbackInput Input, bool bReliable)
{
    if (!Socket || Peers.Num() == 0)
    {
        return false;
    }

    LocalInputHistory.Add(Frame, Input);
    LocalInputHistory.Remove(Frame - FMath::Max(Config.InputRedundancyFrames + 120, 180));

    TArray<FNetworkInputFrame> Frames;
    for (int32 HistoryFrame = Frame - Config.InputRedundancyFrames; HistoryFrame <= Frame; ++HistoryFrame)
    {
        if (const FRollbackInput* FoundInput = LocalInputHistory.Find(HistoryFrame))
        {
            FNetworkInputFrame FrameInput;
            FrameInput.PlayerId = PlayerId;
            FrameInput.Frame = HistoryFrame;
            FrameInput.Input = *FoundInput;
            Frames.Add(FrameInput);
        }
    }

    bool bAllSent = true;
    for (FRemotePeer& Peer : Peers)
    {
        if (Peer.bConnected && Peer.Endpoint.IsValid())
        {
            if (!SendInputPacket(Peer, Frames, bReliable))
            {
                bAllSent = false;
            }
        }
    }

    return bAllSent;
}

bool URollbackNetSubsystem::ConsumeRemoteInput(int32 PlayerId, int32 Frame, FRollbackInput& OutInput) const
{
    if (const TMap<int32, FRollbackInput>* PlayerInputs = RemoteInputBuffer.Find(PlayerId))
    {
        if (const FRollbackInput* FoundInput = PlayerInputs->Find(Frame))
        {
            OutInput = *FoundInput;
            return true;
        }
    }

    OutInput = FRollbackInput();
    return false;
}

void URollbackNetSubsystem::BufferRemoteInputForRollback(int32 PlayerId, int32 Frame, FRollbackInput Input)
{
    CacheRemoteInput(PlayerId, Frame, Input);
}

int32 URollbackNetSubsystem::GetNewestRemoteInputFrame(int32 PlayerId) const
{
    const int32* Newest = NewestRemoteInputFrames.Find(PlayerId);
    return Newest ? *Newest : -1;
}

bool URollbackNetSubsystem::SendGameMessage(int32 PlayerId, uint8 MessageType, const TArray<uint8>& Data)
{
    FRemotePeer* Peer = FindPeerByPlayerId(PlayerId);
    if (!Socket || !Peer || !Peer->bConnected || !Peer->Endpoint.IsValid() || Data.Num() > RollbackNet::MaxGameMessageBytes)
    {
        return false;
    }

    const uint32 MessageId = NextGameMessageId++;
    const int32 ChunkCount = FMath::Max(1, FMath::DivideAndRoundUp(Data.Num(), RollbackNet::GameMessageChunkBytes));
    const double NowSeconds = FPlatformTime::Seconds();

    bool bAllSent = true;
    for (int32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
    {
        const int32 Offset = ChunkIndex * RollbackNet::GameMessageChunkBytes;
        const int32 ChunkBytes = FMath::Min(RollbackNet::GameMessageChunkBytes, Data.Num() - Offset);

        TArray<uint8> Payload;
        FMemoryWriter Writer(Payload);

        uint32 Magic = RollbackNet::PacketMagic;
        uint8 Version = RollbackNet::PacketVersion;
        uint8 PacketType = static_cast<uint8>(RollbackNet::EWirePacketType::GameMsg);
        uint32 Sequence = Peer->NextOutgoingSequence++;
        uint32 AckSequence = Peer->HighestContiguousReceivedSequence;
        int32 InputCount = 0;
        uint32 MsgId = MessageId;
        uint8 MsgType = MessageType;
        int32 MsgChunkIndex = ChunkIndex;
        int32 MsgChunkCount = ChunkCount;
        int32 MsgChunkBytes = ChunkBytes;

        Writer << Magic;
        Writer << Version;
        Writer << PacketType;
        Writer << Sequence;
        Writer << AckSequence;
        Writer << InputCount;
        Writer << MsgId;
        Writer << MsgType;
        Writer << MsgChunkIndex;
        Writer << MsgChunkCount;
        Writer << MsgChunkBytes;
        if (ChunkBytes > 0)
        {
            Writer.Serialize(const_cast<uint8*>(Data.GetData() + Offset), ChunkBytes);
        }

        FPendingReliablePacket PendingPacket;
        PendingPacket.Payload = Payload;
        PendingPacket.Destination = Peer->Endpoint->Clone();
        PendingPacket.LastSentSeconds = NowSeconds;
        PendingPacket.SendAttempts = 1;
        Peer->PendingReliable.Add(Sequence, MoveTemp(PendingPacket));

        Stats.LastSentSequence = Sequence;
        Peer->PacketsSentToPeer++;
        if (!SendRawPacket(Payload, Peer->Endpoint.ToSharedRef(), true))
        {
            bAllSent = false;
        }
    }

    return bAllSent;
}

void URollbackNetSubsystem::SetLocalSimClock(int32 Frame, float TickRateHz)
{
    LocalSimFrame = Frame;
    if (TickRateHz > 0.0f)
    {
        LocalSimTickRateHz = TickRateHz;
    }
}

void URollbackNetSubsystem::SetLocalSimClockValidForTimeSync(bool bValid)
{
    bLocalSimClockValid = bValid;

    // Samples measured against the old clock (or against no clock) are meaningless now.
    for (FRemotePeer& Peer : Peers)
    {
        Peer.bHasLeadSample = false;
        Peer.LeadFramesSmoothed = 0.0f;
    }
}

bool URollbackNetSubsystem::GetPeerLeadFrames(int32 PlayerId, float& OutLeadFrames) const
{
    OutLeadFrames = 0.0f;
    for (const FRemotePeer& Peer : Peers)
    {
        if (Peer.PlayerId == PlayerId && Peer.bConnected && Peer.bHasLeadSample)
        {
            OutLeadFrames = Peer.LeadFramesSmoothed;
            return true;
        }
    }
    return false;
}

void URollbackNetSubsystem::UpdatePeerLeadSample(FRemotePeer& Peer, int32 RemoteFrame, float RoundTripMs)
{
    // -1 stamps mean the remote clock is not yet on the shared timeline (joiner mid-rebase);
    // an invalid local clock makes the comparison equally meaningless.
    if (RemoteFrame < 0 || RoundTripMs < 0.0f || !bLocalSimClockValid)
    {
        return;
    }

    const float HalfRttFrames = 0.5f * RoundTripMs * LocalSimTickRateHz / 1000.0f;
    const float Sample = (static_cast<float>(RemoteFrame) + HalfRttFrames) - static_cast<float>(LocalSimFrame);
    Peer.LeadFramesSmoothed = Peer.bHasLeadSample ? 0.6f * Peer.LeadFramesSmoothed + 0.4f * Sample : Sample;
    Peer.bHasLeadSample = true;
}

void URollbackNetSubsystem::ApplyBufferedInputsToState(URollbackStateComponent* StateComponent, int32 PlayerId, int32 FromFrame, int32 ToFrame, bool& bOutChanged, int32& OutEarliestChangedFrame)
{
    bOutChanged = false;
    OutEarliestChangedFrame = -1;

    if (!StateComponent)
    {
        return;
    }

    const TMap<int32, FRollbackInput>* PlayerInputs = RemoteInputBuffer.Find(PlayerId);
    if (!PlayerInputs)
    {
        return;
    }

    const int32 StartFrame = FMath::Min(FromFrame, ToFrame);
    const int32 EndFrame = FMath::Max(FromFrame, ToFrame);
    for (int32 Frame = StartFrame; Frame <= EndFrame; ++Frame)
    {
        if (const FRollbackInput* BufferedInput = PlayerInputs->Find(Frame))
        {
            if (StateComponent->GetInputForFrame(Frame) != *BufferedInput)
            {
                StateComponent->InjectInputForFrame(Frame, *BufferedInput);
                bOutChanged = true;
                if (OutEarliestChangedFrame < 0)
                {
                    OutEarliestChangedFrame = Frame;
                }
            }
        }
    }
}

bool URollbackNetSubsystem::IsConnectedToPeer() const
{
    for (const FRemotePeer& Peer : Peers)
    {
        if (Peer.bConnected)
        {
            return true;
        }
    }
    return false;
}

FRollbackTransportStats URollbackNetSubsystem::GetTransportStats() const
{
    FRollbackTransportStats CurrentStats = Stats;
    int32 TotalPending = 0;
    for (const FRemotePeer& Peer : Peers)
    {
        TotalPending += Peer.PendingReliable.Num();
    }
    CurrentStats.PendingReliablePackets = TotalPending;
    CurrentStats.ConnectedPeerCount = Peers.Num();
    return CurrentStats;
}

FString URollbackNetSubsystem::GetRemoteEndpointString() const
{
    for (const FRemotePeer& Peer : Peers)
    {
        if (Peer.bConnected && Peer.Endpoint.IsValid())
        {
            return Peer.Endpoint->ToString(true);
        }
    }
    return FString();
}

FString URollbackNetSubsystem::GetLocalEndpointString() const
{
    if (!Socket || !SocketSubsystem)
    {
        return FString();
    }

    TSharedRef<FInternetAddr> LocalAddress = SocketSubsystem->CreateInternetAddr();
    Socket->GetAddress(*LocalAddress);
    return LocalAddress->ToString(true);
}

TArray<int32> URollbackNetSubsystem::GetConnectedPeerIds() const
{
    TArray<int32> Ids;
    for (const FRemotePeer& Peer : Peers)
    {
        if (Peer.bConnected && Peer.PlayerId >= 0)
        {
            Ids.Add(Peer.PlayerId);
        }
    }
    return Ids;
}

TArray<FRollbackPeerInfo> URollbackNetSubsystem::GetAllPeerInfo() const
{
    TArray<FRollbackPeerInfo> Infos;
    const double Now = FPlatformTime::Seconds();
    for (const FRemotePeer& Peer : Peers)
    {
        FRollbackPeerInfo Info;
        Info.PlayerId = Peer.PlayerId;
        Info.EndpointString = Peer.Endpoint.IsValid() ? Peer.Endpoint->ToString(true) : FString();
        Info.bIsConnected = Peer.bConnected;
        Info.LastActivitySecondsAgo = Peer.bConnected ? static_cast<float>(Now - Peer.LastActivityTime) : 0.0f;
        Info.RoundTripMs = Peer.LastRoundTripMs;
        Info.PingMs = FMath::Max(0.0f, Peer.MeasuredPingMs);
        Info.IncomingLossPercent = Peer.MeasuredIncomingLossPercent;
        Info.PacketsSentToPeer = Peer.PacketsSentToPeer;
        Info.PacketsReceivedFromPeer = Peer.PacketsReceivedFromPeer;
        Infos.Add(MoveTemp(Info));
    }
    return Infos;
}

bool URollbackNetSubsystem::GetMeasuredNetQuality(float& OutPingMs, float& OutIncomingLossPercent) const
{
    OutPingMs = 0.0f;
    OutIncomingLossPercent = 0.0f;

    bool bHasPingSample = false;
    for (const FRemotePeer& Peer : Peers)
    {
        if (!Peer.bConnected)
        {
            continue;
        }
        if (Peer.MeasuredPingMs >= 0.0f)
        {
            OutPingMs = FMath::Max(OutPingMs, Peer.MeasuredPingMs);
            bHasPingSample = true;
        }
        OutIncomingLossPercent = FMath::Max(OutIncomingLossPercent, Peer.MeasuredIncomingLossPercent);
    }
    return bHasPingSample;
}

void URollbackNetSubsystem::FlushTransport()
{
    const double NowSeconds = FPlatformTime::Seconds();
    ReleaseDelayedOutgoing(NowSeconds);
    PumpSocket(NowSeconds);
    ReleaseDelayedIncoming(NowSeconds);
    ResendReliablePackets(NowSeconds);
    CheckPeerTimeouts(NowSeconds);
    SendHeartbeats(NowSeconds);
    UpdateNetQuality(NowSeconds);
    UpdatePerformanceStats(NowSeconds);

    for (FRemotePeer& Peer : Peers)
    {
        if (Peer.bAckDirty)
        {
            SendAck(Peer);
            Peer.bAckDirty = false;
        }
    }
}

bool URollbackNetSubsystem::TickFunction(float DeltaTime)
{
    FlushTransport();
    return true;
}

void URollbackNetSubsystem::PumpSocket(double NowSeconds)
{
    if (!Socket || !SocketSubsystem)
    {
        return;
    }

    static constexpr int32 MaxPacketsPerPump = 64;
    for (int32 PacketIndex = 0; PacketIndex < MaxPacketsPerPump; ++PacketIndex)
    {
        TArray<uint8> Payload;
        Payload.SetNumUninitialized(Config.MaxPacketBytes);

        int32 BytesRead = 0;
        TSharedRef<FInternetAddr> Sender = SocketSubsystem->CreateInternetAddr();
        if (!Socket->RecvFrom(Payload.GetData(), Payload.Num(), BytesRead, *Sender) || BytesRead <= 0)
        {
            break;
        }

        Payload.SetNum(BytesRead);
        Stats.PacketsReceived++;
        Stats.BytesReceived += BytesRead;
        PerfBytesReceivedLastWindow += BytesRead;

        if (Config.bEnablePacketLossSimulation && FMath::FRandRange(0.0f, 100.0f) < Config.SimulatedIncomingPacketLossPercent)
        {
            Stats.PacketsDroppedBySimulation++;
            continue;
        }

        const int32 MaxLatencyMs = FMath::Max(Config.SimulatedMinLatencyMs, Config.SimulatedMaxLatencyMs);
        if (Config.bEnablePacketLossSimulation && MaxLatencyMs > 0)
        {
            const int32 DelayMs = FMath::RandRange(FMath::Min(Config.SimulatedMinLatencyMs, MaxLatencyMs), MaxLatencyMs);
            FDelayedPacket Delayed;
            Delayed.Payload = MoveTemp(Payload);
            Delayed.Endpoint = Sender->Clone();
            Delayed.ReleaseSeconds = NowSeconds + static_cast<double>(DelayMs) / 1000.0;
            DelayedIncomingPackets.Add(MoveTemp(Delayed));
            continue;
        }

        ProcessIncomingPayload(Payload, Sender, NowSeconds);
    }
}

void URollbackNetSubsystem::ProcessIncomingPayload(const TArray<uint8>& Payload, TSharedRef<FInternetAddr> Sender, double NowSeconds)
{
    FMemoryReader Reader(Payload);

    uint32 Magic = 0;
    uint8 Version = 0;
    uint8 PacketType = 0;
    uint32 Sequence = 0;
    uint32 AckSequence = 0;
    int32 InputCount = 0;

    Reader << Magic;
    Reader << Version;
    Reader << PacketType;
    Reader << Sequence;
    Reader << AckSequence;
    Reader << InputCount;

    if (Reader.IsError() || Magic != RollbackNet::PacketMagic || Version != RollbackNet::PacketVersion || InputCount < 0 || InputCount > 64)
    {
        return;
    }

    FRemotePeer* Peer = FindPeerByEndpoint(*Sender);

    if (!Peer && Config.bAcceptFirstRemotePeer && Peers.Num() < Config.MaxPeers)
    {
        FRemotePeer NewPeer;
        NewPeer.PlayerId = -1;
        NewPeer.Endpoint = Sender->Clone();
        NewPeer.bConnected = true;
        NewPeer.LastActivityTime = NowSeconds;
        NewPeer.LastHeartbeatSentTime = NowSeconds;
        Peers.Add(MoveTemp(NewPeer));
        Peer = &Peers.Last();
        Stats.ConnectedPeerCount = Peers.Num();
        UE_LOG(LogRollbackCore, Display, TEXT("Accepted peer %s."), *Sender->ToString(true));
        OnTransportConnected.Broadcast(Sender->ToString(true));
    }

    if (!Peer)
    {
        return;
    }

    Peer->LastActivityTime = NowSeconds;
    Peer->PacketsReceivedFromPeer++;
    Stats.LastReceivedSequence = Sequence;

    // Unique-sequence bookkeeping feeds the incoming loss estimate; resends and duplicates must
    // not count twice, and must be classified before MarkSequenceReceived mutates the set.
    const bool bDuplicateSequence = Sequence == 0
        || Sequence <= Peer->HighestContiguousReceivedSequence
        || Peer->ReceivedSequences.Contains(Sequence);

    RemoveAckedPackets(*Peer, AckSequence, NowSeconds);
    MarkSequenceReceived(*Peer, Sequence);

    if (!bDuplicateSequence)
    {
        Peer->LossTrackingHighestSequence = FMath::Max(Peer->LossTrackingHighestSequence, Sequence);
        Peer->LossWindowPacketsReceived++;
    }

    if (PacketType == static_cast<uint8>(RollbackNet::EWirePacketType::Hello))
    {
        Peer->bAckDirty = true;
        return;
    }

    if (PacketType == static_cast<uint8>(RollbackNet::EWirePacketType::Heartbeat))
    {
        Peer->bAckDirty = true;
        return;
    }

    if (PacketType == static_cast<uint8>(RollbackNet::EWirePacketType::Ack))
    {
        return;
    }

    if (PacketType == static_cast<uint8>(RollbackNet::EWirePacketType::Ping))
    {
        double EchoTimestampSeconds = 0.0;
        int32 RemoteSimFrame = -1;
        Reader << EchoTimestampSeconds;
        Reader << RemoteSimFrame;
        if (!Reader.IsError())
        {
            SendTimestampPacket(*Peer, static_cast<uint8>(RollbackNet::EWirePacketType::Pong), EchoTimestampSeconds);
            UpdatePeerLeadSample(*Peer, RemoteSimFrame, Peer->MeasuredPingMs);
        }
        return;
    }

    if (PacketType == static_cast<uint8>(RollbackNet::EWirePacketType::Pong))
    {
        // The echoed timestamp is our own clock, so NowSeconds - Echo is the true wire round trip,
        // including any simulated latency on either side (delayed packets are processed late).
        double EchoTimestampSeconds = 0.0;
        int32 RemoteSimFrame = -1;
        Reader << EchoTimestampSeconds;
        Reader << RemoteSimFrame;
        if (!Reader.IsError())
        {
            const float SampleMs = static_cast<float>((NowSeconds - EchoTimestampSeconds) * 1000.0);
            if (SampleMs >= 0.0f)
            {
                Peer->MeasuredPingMs = Peer->MeasuredPingMs < 0.0f ? SampleMs : FMath::Lerp(Peer->MeasuredPingMs, SampleMs, 0.25f);
                UpdatePeerLeadSample(*Peer, RemoteSimFrame, SampleMs);
            }
        }
        return;
    }

    if (PacketType == static_cast<uint8>(RollbackNet::EWirePacketType::Checksum))
    {
        int32 Frame = -1;
        uint32 Checksum = 0;
        Reader << Frame;
        Reader << Checksum;
        if (!Reader.IsError() && Frame >= ChecksumMinValidFrame)
        {
            RemoteStateChecksums.Add(Frame, Checksum);
            RemoteStateChecksums.Remove(Frame - RollbackNet::ChecksumHistoryFrames);
            CompareStateChecksums(Frame);
        }
        return;
    }

    if (PacketType == static_cast<uint8>(RollbackNet::EWirePacketType::GameMsg))
    {
        Peer->bAckDirty = true;
        // A reliable resend carries the exact sequence already processed; reprocessing it could
        // re-open a completed assembly and double-deliver the message.
        if (!bDuplicateSequence)
        {
            ProcessGameMessageChunk(*Peer, Reader, Sender->ToString(true));
        }
        return;
    }

    if (PacketType == static_cast<uint8>(RollbackNet::EWirePacketType::Input))
    {
        // Cache the whole packet before any delegate fires: a listener reacting to the packet's
        // oldest redundancy frame (e.g. a timeline rebase) must already see the newest one.
        TArray<FNetworkInputFrame, TInlineAllocator<64>> PacketFrames;
        for (int32 Index = 0; Index < InputCount; ++Index)
        {
            FNetworkInputFrame InputFrame;
            float AxisX = 0.0f;
            float AxisY = 0.0f;
            float AxisZ = 0.0f;
            Reader << InputFrame.PlayerId;
            Reader << InputFrame.Frame;
            Reader << InputFrame.Input.Buttons;
            Reader << AxisX;
            Reader << AxisY;
            Reader << AxisZ;

            if (Reader.IsError())
            {
                return;
            }

            InputFrame.Input.Axes = FVector(AxisX, AxisY, AxisZ);

            if (Peer->PlayerId < 0 && InputFrame.PlayerId >= 0)
            {
                Peer->PlayerId = InputFrame.PlayerId;
            }

            PacketFrames.Add(InputFrame);
        }

        Peer->bAckDirty = true;

        TArray<bool, TInlineAllocator<64>> bFrameChanged;
        for (const FNetworkInputFrame& InputFrame : PacketFrames)
        {
            bFrameChanged.Add(CacheRemoteInputSilent(InputFrame.PlayerId, InputFrame.Frame, InputFrame.Input));
        }
        for (int32 Index = 0; Index < PacketFrames.Num(); ++Index)
        {
            if (bFrameChanged[Index])
            {
                const FNetworkInputFrame& InputFrame = PacketFrames[Index];
                OnRemoteInputReceived.Broadcast(InputFrame.PlayerId, InputFrame.Frame, InputFrame.Input);
            }
        }
    }
}

void URollbackNetSubsystem::ProcessGameMessageChunk(FRemotePeer& Peer, FMemoryReader& Reader, const FString& PeerKey)
{
    uint32 MessageId = 0;
    uint8 MessageType = 0;
    int32 ChunkIndex = -1;
    int32 ChunkCount = 0;
    int32 ChunkBytes = -1;

    Reader << MessageId;
    Reader << MessageType;
    Reader << ChunkIndex;
    Reader << ChunkCount;
    Reader << ChunkBytes;

    if (Reader.IsError()
        || ChunkCount < 1 || ChunkCount > RollbackNet::MaxGameMessageChunks
        || ChunkIndex < 0 || ChunkIndex >= ChunkCount
        || ChunkBytes < 0 || ChunkBytes > RollbackNet::GameMessageChunkBytes
        || Reader.TotalSize() - Reader.Tell() < ChunkBytes)
    {
        return;
    }

    TArray<uint8> ChunkData;
    ChunkData.SetNumUninitialized(ChunkBytes);
    if (ChunkBytes > 0)
    {
        Reader.Serialize(ChunkData.GetData(), ChunkBytes);
    }

    FGameMessageAssembly& Assembly = GameMessageAssemblies.FindOrAdd(PeerKey).FindOrAdd(MessageId);
    if (Assembly.ChunkCount == 0)
    {
        Assembly.MessageType = MessageType;
        Assembly.ChunkCount = ChunkCount;
    }
    else if (Assembly.MessageType != MessageType || Assembly.ChunkCount != ChunkCount)
    {
        return;
    }
    Assembly.Chunks.Add(ChunkIndex, MoveTemp(ChunkData));

    if (Assembly.Chunks.Num() < Assembly.ChunkCount)
    {
        return;
    }

    TArray<uint8> FullData;
    for (int32 Index = 0; Index < Assembly.ChunkCount; ++Index)
    {
        FullData.Append(Assembly.Chunks.FindChecked(Index));
    }
    GameMessageAssemblies.FindChecked(PeerKey).Remove(MessageId);

    // Broadcast last: handlers may connect peers and reallocate the peer array.
    const int32 SenderPlayerId = Peer.PlayerId;
    OnGameMessageReceived.Broadcast(SenderPlayerId, MessageType, FullData);
    OnGameMessageReceivedNative.Broadcast(SenderPlayerId, MessageType, FullData);
}

void URollbackNetSubsystem::ReleaseDelayedIncoming(double NowSeconds)
{
    for (int32 Index = DelayedIncomingPackets.Num() - 1; Index >= 0; --Index)
    {
        if (DelayedIncomingPackets[Index].ReleaseSeconds <= NowSeconds && DelayedIncomingPackets[Index].Endpoint.IsValid())
        {
            FDelayedPacket Delayed = MoveTemp(DelayedIncomingPackets[Index]);
            DelayedIncomingPackets.RemoveAtSwap(Index);
            ProcessIncomingPayload(Delayed.Payload, Delayed.Endpoint.ToSharedRef(), NowSeconds);
        }
    }
}

void URollbackNetSubsystem::ReleaseDelayedOutgoing(double NowSeconds)
{
    for (int32 Index = DelayedOutgoingPackets.Num() - 1; Index >= 0; --Index)
    {
        if (DelayedOutgoingPackets[Index].ReleaseSeconds <= NowSeconds && DelayedOutgoingPackets[Index].Endpoint.IsValid())
        {
            FDelayedPacket Delayed = MoveTemp(DelayedOutgoingPackets[Index]);
            DelayedOutgoingPackets.RemoveAtSwap(Index);
            SendRawPacketNow(Delayed.Payload, *Delayed.Endpoint);
        }
    }
}

void URollbackNetSubsystem::ResendReliablePackets(double NowSeconds)
{
    if (!Socket)
    {
        return;
    }

    for (FRemotePeer& Peer : Peers)
    {
        TArray<uint32> ExceededSequences;

        for (TPair<uint32, FPendingReliablePacket>& Pending : Peer.PendingReliable)
        {
            FPendingReliablePacket& Packet = Pending.Value;
            if (Packet.Destination.IsValid() && NowSeconds - Packet.LastSentSeconds >= Config.ResendAfterSeconds)
            {
                if (Packet.SendAttempts >= Config.MaxReliableRetryCount)
                {
                    ExceededSequences.Add(Pending.Key);
                    continue;
                }

                SendRawPacket(Packet.Payload, Packet.Destination.ToSharedRef(), true);
                Packet.LastSentSeconds = NowSeconds;
                Packet.SendAttempts++;
                Stats.PacketsResent++;
                Peer.PacketsSentToPeer++;
            }
        }

        for (uint32 Seq : ExceededSequences)
        {
            Peer.PendingReliable.Remove(Seq);
            OnPeerMaxRetriesExceeded.Broadcast(Peer.PlayerId, static_cast<int32>(Seq));
        }
    }
}

void URollbackNetSubsystem::CheckPeerTimeouts(double NowSeconds)
{
    for (int32 i = Peers.Num() - 1; i >= 0; --i)
    {
        FRemotePeer& Peer = Peers[i];
        if (Peer.bConnected && (NowSeconds - Peer.LastActivityTime) >= static_cast<double>(Config.PeerTimeoutSeconds))
        {
            const int32 DisconnectedPlayerId = Peer.PlayerId;
            UE_LOG(LogRollbackCore, Warning, TEXT("Peer %d timed out (no activity for %.1f s)."), DisconnectedPlayerId, Config.PeerTimeoutSeconds);
            OnPeerDisconnected.Broadcast(DisconnectedPlayerId, TEXT("Peer timed out (no activity)."));
            Peers.RemoveAtSwap(i);
            Stats.ConnectedPeerCount = Peers.Num();
        }
    }
}

void URollbackNetSubsystem::SendHeartbeats(double NowSeconds)
{
    for (FRemotePeer& Peer : Peers)
    {
        if (Peer.bConnected && (NowSeconds - Peer.LastHeartbeatSentTime) >= static_cast<double>(Config.HeartbeatIntervalSeconds))
        {
            SendHeartbeat(Peer);
            Peer.LastHeartbeatSentTime = NowSeconds;
        }
    }
}

void URollbackNetSubsystem::UpdateNetQuality(double NowSeconds)
{
    if (!Socket)
    {
        return;
    }

    for (FRemotePeer& Peer : Peers)
    {
        if (!Peer.bConnected)
        {
            continue;
        }

        if (NowSeconds - Peer.LastPingSentTime >= RollbackNet::PingIntervalSeconds)
        {
            Peer.LastPingSentTime = NowSeconds;
            SendTimestampPacket(Peer, static_cast<uint8>(RollbackNet::EWirePacketType::Ping), NowSeconds);
        }

        if (Peer.LossWindowStartTime <= 0.0)
        {
            Peer.LossWindowStartTime = NowSeconds;
            Peer.LossWindowBaseSequence = Peer.LossTrackingHighestSequence;
            Peer.LossWindowPacketsReceived = 0;
        }
        else if (NowSeconds - Peer.LossWindowStartTime >= RollbackNet::LossWindowSeconds)
        {
            // Expected = span of sequence numbers the remote sent this window; anything missing
            // from the unique-receive count was lost on the wire (or dropped by simulation).
            const int64 Expected = static_cast<int64>(Peer.LossTrackingHighestSequence) - static_cast<int64>(Peer.LossWindowBaseSequence);
            if (Expected > 0)
            {
                const int64 Lost = FMath::Clamp<int64>(Expected - Peer.LossWindowPacketsReceived, 0, Expected);
                Peer.MeasuredIncomingLossPercent = 100.0f * static_cast<float>(Lost) / static_cast<float>(Expected);
            }
            Peer.LossWindowStartTime = NowSeconds;
            Peer.LossWindowBaseSequence = Peer.LossTrackingHighestSequence;
            Peer.LossWindowPacketsReceived = 0;
        }
    }
}

bool URollbackNetSubsystem::SendHello(FRemotePeer& Peer)
{
    static const TArray<FNetworkInputFrame> EmptyInputs;
    return SendPacket(Peer, static_cast<uint8>(RollbackNet::EWirePacketType::Hello), EmptyInputs, true);
}

bool URollbackNetSubsystem::SendHeartbeat(FRemotePeer& Peer)
{
    static const TArray<FNetworkInputFrame> EmptyInputs;
    return SendPacket(Peer, static_cast<uint8>(RollbackNet::EWirePacketType::Heartbeat), EmptyInputs, false);
}

bool URollbackNetSubsystem::SendAck(FRemotePeer& Peer)
{
    static const TArray<FNetworkInputFrame> EmptyInputs;
    return SendPacket(Peer, static_cast<uint8>(RollbackNet::EWirePacketType::Ack), EmptyInputs, false);
}

bool URollbackNetSubsystem::SendInputPacket(FRemotePeer& Peer, const TArray<FNetworkInputFrame>& InputFrames, bool bReliable)
{
    return SendPacket(Peer, static_cast<uint8>(RollbackNet::EWirePacketType::Input), InputFrames, bReliable);
}

void URollbackNetSubsystem::SubmitLocalStateChecksum(int32 Frame, uint32 Checksum)
{
    if (!Socket || Peers.Num() == 0 || Frame < ChecksumMinValidFrame)
    {
        return;
    }

    LocalStateChecksums.Add(Frame, Checksum);
    LocalStateChecksums.Remove(Frame - RollbackNet::ChecksumHistoryFrames);

    for (FRemotePeer& Peer : Peers)
    {
        if (Peer.bConnected && Peer.Endpoint.IsValid())
        {
            SendChecksumPacket(Peer, Frame, Checksum);
        }
    }

    CompareStateChecksums(Frame);
}

void URollbackNetSubsystem::ResetStateChecksums(int32 MinValidFrame)
{
    LocalStateChecksums.Reset();
    RemoteStateChecksums.Reset();
    ChecksumMinValidFrame = MinValidFrame;
    FirstChecksumMismatchFrame = -1;
    UE_LOG(LogRollbackCore, Display, TEXT("State checksum stream reset; comparing frames >= %d."), MinValidFrame);
}

bool URollbackNetSubsystem::SendChecksumPacket(FRemotePeer& Peer, int32 Frame, uint32 Checksum)
{
    if (!Peer.Endpoint.IsValid())
    {
        return false;
    }

    TArray<uint8> Payload;
    FMemoryWriter Writer(Payload);

    uint32 Magic = RollbackNet::PacketMagic;
    uint8 Version = RollbackNet::PacketVersion;
    uint8 PacketType = static_cast<uint8>(RollbackNet::EWirePacketType::Checksum);
    uint32 Sequence = Peer.NextOutgoingSequence++;
    uint32 AckSequence = Peer.HighestContiguousReceivedSequence;
    int32 InputCount = 0;

    Writer << Magic;
    Writer << Version;
    Writer << PacketType;
    Writer << Sequence;
    Writer << AckSequence;
    Writer << InputCount;
    Writer << Frame;
    Writer << Checksum;

    Stats.LastSentSequence = Sequence;
    Peer.PacketsSentToPeer++;
    return SendRawPacket(Payload, Peer.Endpoint.ToSharedRef(), true);
}

void URollbackNetSubsystem::CompareStateChecksums(int32 Frame)
{
    const uint32* Local = LocalStateChecksums.Find(Frame);
    const uint32* Remote = RemoteStateChecksums.Find(Frame);
    if (!Local || !Remote || *Local == *Remote)
    {
        return;
    }

    PerfStats.DesyncCount++;

    // The first mismatch is the interesting one; later frames inherit the divergence.
    const double NowSeconds = FPlatformTime::Seconds();
    const bool bFirstMismatch = FirstChecksumMismatchFrame < 0;
    if (bFirstMismatch)
    {
        FirstChecksumMismatchFrame = Frame;
    }
    if (bFirstMismatch || NowSeconds - LastChecksumMismatchLogTime >= 1.0)
    {
        LastChecksumMismatchLogTime = NowSeconds;
        UE_LOG(LogRollbackCore, Error, TEXT("DESYNC: frame %d state checksum local %08X != remote %08X (first mismatch at frame %d)."),
            Frame, *Local, *Remote, FirstChecksumMismatchFrame);
    }

    OnStateChecksumMismatch.Broadcast(Frame, static_cast<int32>(*Local), static_cast<int32>(*Remote));
}

bool URollbackNetSubsystem::SendPacket(FRemotePeer& Peer, uint8 PacketType, const TArray<FNetworkInputFrame>& InputFrames, bool bReliable)
{
    if (!Peer.Endpoint.IsValid())
    {
        return false;
    }

    TArray<uint8> Payload;
    FMemoryWriter Writer(Payload);

    uint32 Magic = RollbackNet::PacketMagic;
    uint8 Version = RollbackNet::PacketVersion;
    uint32 Sequence = Peer.NextOutgoingSequence++;
    uint32 AckSequence = Peer.HighestContiguousReceivedSequence;
    int32 InputCount = InputFrames.Num();

    Writer << Magic;
    Writer << Version;
    Writer << PacketType;
    Writer << Sequence;
    Writer << AckSequence;
    Writer << InputCount;

    for (const FNetworkInputFrame& InputFrame : InputFrames)
    {
        int32 PId = InputFrame.PlayerId;
        int32 Frame = InputFrame.Frame;
        int32 Buttons = InputFrame.Input.Buttons;
        float AxisX = InputFrame.Input.Axes.X;
        float AxisY = InputFrame.Input.Axes.Y;
        float AxisZ = InputFrame.Input.Axes.Z;

        Writer << PId;
        Writer << Frame;
        Writer << Buttons;
        Writer << AxisX;
        Writer << AxisY;
        Writer << AxisZ;
    }

    if (Payload.Num() > Config.MaxPacketBytes)
    {
        const FString Error = FString::Printf(TEXT("Rollback packet is %d bytes, above MaxPacketBytes=%d."), Payload.Num(), Config.MaxPacketBytes);
        OnTransportError.Broadcast(Error);
        return false;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    if (bReliable)
    {
        FPendingReliablePacket PendingPacket;
        PendingPacket.Payload = Payload;
        PendingPacket.Destination = Peer.Endpoint->Clone();
        PendingPacket.LastSentSeconds = NowSeconds;
        PendingPacket.SendAttempts = 1;
        Peer.PendingReliable.Add(Sequence, MoveTemp(PendingPacket));
    }

    Stats.LastSentSequence = Sequence;
    Peer.PacketsSentToPeer++;
    return SendRawPacket(Payload, Peer.Endpoint.ToSharedRef(), true);
}

bool URollbackNetSubsystem::SendTimestampPacket(FRemotePeer& Peer, uint8 PacketType, double TimestampSeconds)
{
    if (!Peer.Endpoint.IsValid())
    {
        return false;
    }

    TArray<uint8> Payload;
    FMemoryWriter Writer(Payload);

    uint32 Magic = RollbackNet::PacketMagic;
    uint8 Version = RollbackNet::PacketVersion;
    uint32 Sequence = Peer.NextOutgoingSequence++;
    uint32 AckSequence = Peer.HighestContiguousReceivedSequence;
    int32 InputCount = 0;

    // -1 marks a clock that is not yet on the shared timeline; receivers skip the lead sample.
    int32 SimFrame = bLocalSimClockValid ? LocalSimFrame : -1;

    Writer << Magic;
    Writer << Version;
    Writer << PacketType;
    Writer << Sequence;
    Writer << AckSequence;
    Writer << InputCount;
    Writer << TimestampSeconds;
    Writer << SimFrame;

    Stats.LastSentSequence = Sequence;
    Peer.PacketsSentToPeer++;
    return SendRawPacket(Payload, Peer.Endpoint.ToSharedRef(), true);
}

bool URollbackNetSubsystem::SendRawPacket(const TArray<uint8>& Payload, TSharedRef<FInternetAddr> Destination, bool bApplySimulation)
{
    if (!Socket)
    {
        return false;
    }

    if (bApplySimulation && Config.bEnablePacketLossSimulation && FMath::FRandRange(0.0f, 100.0f) < Config.SimulatedOutgoingPacketLossPercent)
    {
        Stats.PacketsDroppedBySimulation++;
        return true;
    }

    const int32 MaxLatencyMs = FMath::Max(Config.SimulatedMinLatencyMs, Config.SimulatedMaxLatencyMs);
    if (bApplySimulation && Config.bEnablePacketLossSimulation && MaxLatencyMs > 0)
    {
        const int32 DelayMs = FMath::RandRange(FMath::Min(Config.SimulatedMinLatencyMs, MaxLatencyMs), MaxLatencyMs);
        FDelayedPacket Delayed;
        Delayed.Payload = Payload;
        Delayed.Endpoint = Destination->Clone();
        Delayed.ReleaseSeconds = FPlatformTime::Seconds() + static_cast<double>(DelayMs) / 1000.0;
        DelayedOutgoingPackets.Add(MoveTemp(Delayed));
        return true;
    }

    return SendRawPacketNow(Payload, *Destination);
}

bool URollbackNetSubsystem::SendRawPacketNow(const TArray<uint8>& Payload, const FInternetAddr& Destination)
{
    int32 BytesSent = 0;
    const bool bSent = Socket && Socket->SendTo(Payload.GetData(), Payload.Num(), BytesSent, Destination);
    if (bSent)
    {
        Stats.PacketsSent++;
        Stats.BytesSent += BytesSent;
        PerfBytesSentLastWindow += BytesSent;
    }
    return bSent && BytesSent == Payload.Num();
}

void URollbackNetSubsystem::MarkSequenceReceived(FRemotePeer& Peer, uint32 Sequence)
{
    if (Sequence == 0)
    {
        return;
    }

    Peer.ReceivedSequences.Add(Sequence);
    while (Peer.ReceivedSequences.Contains(Peer.HighestContiguousReceivedSequence + 1))
    {
        Peer.HighestContiguousReceivedSequence++;
        Peer.ReceivedSequences.Remove(Peer.HighestContiguousReceivedSequence - 512);
    }
}

void URollbackNetSubsystem::RemoveAckedPackets(FRemotePeer& Peer, uint32 AckSequence, double NowSeconds)
{
    TArray<uint32> AckedSequences;
    for (const TPair<uint32, FPendingReliablePacket>& Pending : Peer.PendingReliable)
    {
        if (Pending.Key <= AckSequence)
        {
            AckedSequences.Add(Pending.Key);
            // Resends reset LastSentSeconds, so only first-attempt acks are valid RTT samples.
            if (Pending.Value.SendAttempts == 1)
            {
                Peer.LastRoundTripMs = static_cast<float>((NowSeconds - Pending.Value.LastSentSeconds) * 1000.0);
            }
        }
    }

    for (uint32 AckedSequence : AckedSequences)
    {
        Peer.PendingReliable.Remove(AckedSequence);
    }

    if (AckSequence > static_cast<uint32>(Stats.LastAckedSequence))
    {
        Stats.LastAckedSequence = AckSequence;
    }

    float MaxRtt = 0.0f;
    for (const FRemotePeer& P : Peers)
    {
        MaxRtt = FMath::Max(MaxRtt, P.LastRoundTripMs);
    }
    Stats.LastRoundTripMs = MaxRtt;
}

void URollbackNetSubsystem::CacheRemoteInput(int32 PlayerId, int32 Frame, const FRollbackInput& Input)
{
    if (CacheRemoteInputSilent(PlayerId, Frame, Input))
    {
        OnRemoteInputReceived.Broadcast(PlayerId, Frame, Input);
    }
}

bool URollbackNetSubsystem::CacheRemoteInputSilent(int32 PlayerId, int32 Frame, const FRollbackInput& Input)
{
    TMap<int32, FRollbackInput>& PlayerInputs = RemoteInputBuffer.FindOrAdd(PlayerId);
    const FRollbackInput* ExistingInput = PlayerInputs.Find(Frame);
    const bool bChanged = !ExistingInput || *ExistingInput != Input;
    if (bChanged)
    {
        PlayerInputs.Add(Frame, Input);
        Stats.LastReceivedFrame = FMath::Max(Stats.LastReceivedFrame, Frame);
        int32& NewestFrame = NewestRemoteInputFrames.FindOrAdd(PlayerId, -1);
        NewestFrame = FMath::Max(NewestFrame, Frame);
    }

    PlayerInputs.Remove(Frame - 600);
    return bChanged;
}

URollbackNetSubsystem::FRemotePeer* URollbackNetSubsystem::FindPeerByEndpoint(const FInternetAddr& Addr)
{
    const FString AddrStr = Addr.ToString(true);
    for (FRemotePeer& Peer : Peers)
    {
        if (Peer.Endpoint.IsValid() && Peer.Endpoint->ToString(true) == AddrStr)
        {
            return &Peer;
        }
    }
    return nullptr;
}

URollbackNetSubsystem::FRemotePeer* URollbackNetSubsystem::FindPeerByPlayerId(int32 PlayerId)
{
    for (FRemotePeer& Peer : Peers)
    {
        if (Peer.PlayerId == PlayerId)
        {
            return &Peer;
        }
    }
    return nullptr;
}

bool URollbackNetSubsystem::ResolveEndpoint(const FString& Host, int32 Port, TSharedPtr<FInternetAddr>& OutEndpoint, FString& OutError) const
{
    if (!SocketSubsystem)
    {
        OutError = TEXT("Socket subsystem is not initialized.");
        return false;
    }

    if (Host.IsEmpty() || Port <= 0 || Port > 65535)
    {
        OutError = FString::Printf(TEXT("Invalid endpoint '%s:%d'."), *Host, Port);
        return false;
    }

    OutEndpoint = SocketSubsystem->GetAddressFromString(Host);
    if (!OutEndpoint.IsValid())
    {
        FAddressInfoResult AddressInfo = SocketSubsystem->GetAddressInfo(*Host, nullptr, EAddressInfoFlags::Default, NAME_None, ESocketType::SOCKTYPE_Datagram);
        if (AddressInfo.Results.Num() > 0)
        {
            OutEndpoint = AddressInfo.Results[0].Address->Clone();
        }
    }

    if (!OutEndpoint.IsValid())
    {
        OutError = FString::Printf(TEXT("Could not resolve host '%s'."), *Host);
        return false;
    }

    OutEndpoint->SetPort(Port);
    return true;
}

void URollbackNetSubsystem::UpdatePerformanceStats(double NowSeconds)
{
    const double Elapsed = NowSeconds - PerfLastBandwidthTime;
    if (Elapsed >= 1.0)
    {
        PerfStats.BytesSentPerSecond = static_cast<float>(PerfBytesSentLastWindow) / static_cast<float>(Elapsed);
        PerfStats.BytesReceivedPerSecond = static_cast<float>(PerfBytesReceivedLastWindow) / static_cast<float>(Elapsed);
        PerfBytesSentLastWindow = 0;
        PerfBytesReceivedLastWindow = 0;
        PerfLastBandwidthTime = NowSeconds;
    }

    if (PerfSimulationTimes.Num() > 0)
    {
        float Sum = 0.0f;
        float Max = 0.0f;
        for (float V : PerfSimulationTimes)
        {
            Sum += V;
            if (V > Max) Max = V;
        }
        PerfStats.AvgSimulationTimeMs = Sum / static_cast<float>(PerfSimulationTimes.Num());
        PerfStats.MaxSimulationTimeMs = Max;
    }

    if (PerfRollbackTimes.Num() > 0)
    {
        float Sum = 0.0f;
        float Max = 0.0f;
        for (float V : PerfRollbackTimes)
        {
            Sum += V;
            if (V > Max) Max = V;
        }
        PerfStats.AvgRollbackTimeMs = Sum / static_cast<float>(PerfRollbackTimes.Num());
        PerfStats.MaxRollbackTimeMs = Max;
    }

    if (PerfSerializeTimes.Num() > 0)
    {
        float Sum = 0.0f;
        float Max = 0.0f;
        for (float V : PerfSerializeTimes)
        {
            Sum += V;
            if (V > Max) Max = V;
        }
        PerfStats.AvgStateSerializeTimeMs = Sum / static_cast<float>(PerfSerializeTimes.Num());
        PerfStats.MaxStateSerializeTimeMs = Max;
    }

    if (PerfStateSizes.Num() > 0)
    {
        float Sum = 0.0f;
        for (float V : PerfStateSizes) Sum += V;
        PerfStats.AvgStateSnapshotBytes = Sum / static_cast<float>(PerfStateSizes.Num());
    }

    const int32 CurrentSecond = static_cast<int32>(NowSeconds);
    int32 CountThisSecond = 0;
    for (int32 Ts : PerfRollbackFrameTimestamps)
    {
        if (Ts >= CurrentSecond) CountThisSecond++;
    }
    PerfStats.RollbacksInLastSecond = CountThisSecond;

    PerfStats.ConnectedPeerCount = Peers.Num();
    PerfStats.DesyncCount = PerfStats.DesyncCount;
}

void URollbackNetSubsystem::RecordSimulationTime(float Ms)
{
    PerfSimulationTimes.Add(Ms);
    if (PerfSimulationTimes.Num() > PerfHistorySize) PerfSimulationTimes.RemoveAt(0);
    PerfStats.FramesSimulated++;
}

void URollbackNetSubsystem::RecordRollbackTime(float Ms, int32 DepthFrames)
{
    PerfRollbackTimes.Add(Ms);
    if (PerfRollbackTimes.Num() > PerfHistorySize) PerfRollbackTimes.RemoveAt(0);
    PerfRollbackFrameTimestamps.Add(static_cast<int32>(FPlatformTime::Seconds()));
    if (PerfRollbackFrameTimestamps.Num() > 120) PerfRollbackFrameTimestamps.RemoveAt(0);
    PerfStats.MaxRollbackDepthFrames = FMath::Max(PerfStats.MaxRollbackDepthFrames, DepthFrames);
    PerfStats.TotalRollbackCount++;
    PerfStats.FramesWithRollback++;
}

void URollbackNetSubsystem::RecordSerializeTime(float Ms, int32 StateBytes)
{
    PerfSerializeTimes.Add(Ms);
    if (PerfSerializeTimes.Num() > PerfHistorySize) PerfSerializeTimes.RemoveAt(0);
    PerfStateSizes.Add(StateBytes);
    if (PerfStateSizes.Num() > PerfHistorySize) PerfStateSizes.RemoveAt(0);
}

void URollbackNetSubsystem::RecordDesync()
{
    PerfStats.DesyncCount++;
}

void URollbackNetSubsystem::SetEntityCount(int32 Count)
{
    PerfStats.RegisteredEntityCount = Count;
}
