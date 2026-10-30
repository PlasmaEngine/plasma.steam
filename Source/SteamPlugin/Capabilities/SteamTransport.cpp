#include <SteamPlugin/SteamPluginPCH.h>

#include <SteamPlugin/SteamPlatformBackend.h>

// Datagrams ride on ISteamNetworkingMessages: connectionless from the caller's view, relayed through SDR, sessions
// created on first send and accepted on the listening side from SteamNetworkingMessagesSessionRequest_t. Peers are
// the users a session is open with; Connected/Disconnected events track that set.

// Messages are received per channel and the API has no "any channel" receive, so a fixed range is polled.
static constexpr int s_iTransportChannels = 8;
static constexpr int s_iReceiveBatch = 32;

static SteamNetworkingIdentity ToIdentity(CSteamID id)
{
  SteamNetworkingIdentity identity;
  identity.SetSteamID(id);
  return identity;
}

plResult plSteamPlatformBackend::Listen()
{
  if (!m_bInitialized || SteamNetworkingMessages() == nullptr)
    return PL_FAILURE;

  m_bListening = true;
  return PL_SUCCESS;
}

void plSteamPlatformBackend::StopListening()
{
  m_bListening = false;
}

plResult plSteamPlatformBackend::Connect(const plPlatformUserId& user)
{
  const CSteamID id = ParseUserId(user);
  if (!m_bInitialized || SteamNetworkingMessages() == nullptr || !id.IsValid())
    return PL_FAILURE;

  // The session opens with the first datagram; the peer counts as connected from here and a failed session
  // arrives later as Disconnected through SteamNetworkingMessagesSessionFailed_t.
  if (!IsPeer(id))
  {
    m_Peers.PushBack(id);
    PushTransportEvent(plPlatformTransportEvent::Type::Connected, id);
  }

  return PL_SUCCESS;
}

void plSteamPlatformBackend::Disconnect(const plPlatformUserId& user)
{
  const CSteamID id = ParseUserId(user);
  if (!m_bInitialized || !id.IsValid())
    return;

  if (SteamNetworkingMessages() != nullptr)
  {
    SteamNetworkingMessages()->CloseSessionWithUser(ToIdentity(id));
  }

  if (m_Peers.RemoveAndSwap(id))
  {
    PushTransportEvent(plPlatformTransportEvent::Type::Disconnected, id);
  }
}

void plSteamPlatformBackend::DisconnectAll()
{
  if (!m_bInitialized)
    return;

  plDynamicArray<CSteamID> peers = m_Peers;
  m_Peers.Clear();

  for (const CSteamID& id : peers)
  {
    if (SteamNetworkingMessages() != nullptr)
    {
      SteamNetworkingMessages()->CloseSessionWithUser(ToIdentity(id));
    }

    PushTransportEvent(plPlatformTransportEvent::Type::Disconnected, id);
  }
}

plResult plSteamPlatformBackend::Send(const plPlatformUserId& user, plArrayPtr<const plUInt8> data, bool bReliable, plUInt8 uiChannel)
{
  const CSteamID id = ParseUserId(user);
  if (!m_bInitialized || SteamNetworkingMessages() == nullptr || !id.IsValid() || data.IsEmpty())
    return PL_FAILURE;

  if (uiChannel >= s_iTransportChannels)
  {
    plLog::Error("Steam: transport channel {0} is outside the polled range 0..{1}.", uiChannel, s_iTransportChannels - 1);
    return PL_FAILURE;
  }

  const int iFlags = bReliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
  const EResult result = SteamNetworkingMessages()->SendMessageToUser(ToIdentity(id), data.GetPtr(), data.GetCount(), iFlags, uiChannel);

  if (result != k_EResultOK)
  {
    plLog::Warning("Steam: SendMessageToUser to {0} failed ({1}).", user.m_sCanonical, (int)result);
    return PL_FAILURE;
  }

  if (!IsPeer(id))
  {
    m_Peers.PushBack(id);
    PushTransportEvent(plPlatformTransportEvent::Type::Connected, id);
  }

  return PL_SUCCESS;
}

bool plSteamPlatformBackend::PollEvent(plPlatformTransportEvent& out_event)
{
  if (m_TransportEvents.IsEmpty())
    return false;

  out_event = m_TransportEvents.PeekFront();
  m_TransportEvents.PopFront();
  return true;
}

plUInt32 plSteamPlatformBackend::GetMaxPayloadBytes() const
{
  return k_cbMaxSteamNetworkingSocketsMessageSizeSend;
}

void plSteamPlatformBackend::ReceiveTransportMessages()
{
  ISteamNetworkingMessages* pMessages = SteamNetworkingMessages();
  if (pMessages == nullptr)
    return;

  for (int iChannel = 0; iChannel < s_iTransportChannels; ++iChannel)
  {
    int iReceived = 0;
    do
    {
      SteamNetworkingMessage_t* messages[s_iReceiveBatch] = {};
      iReceived = pMessages->ReceiveMessagesOnChannel(iChannel, messages, s_iReceiveBatch);

      for (int i = 0; i < iReceived; ++i)
      {
        SteamNetworkingMessage_t* pMessage = messages[i];
        if (pMessage == nullptr)
          continue;

        const CSteamID sender = pMessage->m_identityPeer.GetSteamID();

        if (sender.IsValid() && !IsPeer(sender))
        {
          m_Peers.PushBack(sender);
          PushTransportEvent(plPlatformTransportEvent::Type::Connected, sender);
        }

        plPlatformTransportEvent& e = m_TransportEvents.ExpandAndGetRef();
        e.m_Type = plPlatformTransportEvent::Type::Data;
        e.m_User = MakeUserId(sender);
        e.m_uiChannel = static_cast<plUInt8>(iChannel);
        e.m_Data.SetCountUninitialized(static_cast<plUInt32>(pMessage->m_cbSize));
        if (pMessage->m_cbSize > 0)
        {
          plMemoryUtils::Copy(e.m_Data.GetData(), static_cast<const plUInt8*>(pMessage->m_pData), static_cast<plUInt32>(pMessage->m_cbSize));
        }

        pMessage->Release();
      }
    } while (iReceived == s_iReceiveBatch);
  }
}

void plSteamPlatformBackend::PushTransportEvent(plPlatformTransportEvent::Type type, CSteamID user)
{
  plPlatformTransportEvent& e = m_TransportEvents.ExpandAndGetRef();
  e.m_Type = type;
  e.m_User = MakeUserId(user);

  if (type == plPlatformTransportEvent::Type::Connected || type == plPlatformTransportEvent::Type::Disconnected)
  {
    plPlatformEvent hubEvent;
    hubEvent.m_Type = plPlatformEvent::Type::TransportConnection;
    hubEvent.m_User = e.m_User;
    hubEvent.m_bFlag = type == plPlatformTransportEvent::Type::Connected;
    Emit(hubEvent);
  }
}

bool plSteamPlatformBackend::IsPeer(CSteamID user) const
{
  return m_Peers.Contains(user);
}

void plSteamPlatformBackend::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* pParam)
{
  const CSteamID id = pParam->m_identityRemote.GetSteamID();
  if (!id.IsValid() || SteamNetworkingMessages() == nullptr)
    return;

  if (!m_bListening || IsBlocked(MakeUserId(id)))
  {
    SteamNetworkingMessages()->CloseSessionWithUser(pParam->m_identityRemote);
    return;
  }

  if (!SteamNetworkingMessages()->AcceptSessionWithUser(pParam->m_identityRemote))
    return;

  if (!IsPeer(id))
  {
    m_Peers.PushBack(id);
    PushTransportEvent(plPlatformTransportEvent::Type::Connected, id);
  }
}

void plSteamPlatformBackend::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t* pParam)
{
  const CSteamID id = pParam->m_info.m_identityRemote.GetSteamID();
  if (!id.IsValid())
    return;

  plLog::Warning("Steam: session with {0} failed ({1}): {2}", id.ConvertToUint64(), (int)pParam->m_info.m_eEndReason, pParam->m_info.m_szEndDebug);

  if (m_Peers.RemoveAndSwap(id))
    PushTransportEvent(plPlatformTransportEvent::Type::Disconnected, id);
  else
    PushTransportEvent(plPlatformTransportEvent::Type::ConnectFailed, id);
}
