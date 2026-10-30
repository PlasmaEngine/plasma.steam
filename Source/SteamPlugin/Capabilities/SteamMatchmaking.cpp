#include <SteamPlugin/SteamPluginPCH.h>

#include <SteamPlugin/SteamPlatformBackend.h>

// Steam has no matchmaking service for lobbies, so a ticket is a loop over lobby search + join. After
// s_uiSearchAttempts empty or failed rounds the ticket hosts: it creates a public lobby with the request's attributes
// and reports Found for it, leaving later searchers to find it.

static constexpr plUInt32 s_uiSearchAttempts = 3;
static constexpr plUInt32 s_uiSearchResults = 10;
static const plTime s_SearchInterval = plTime::MakeFromSeconds(2.0);

plPlatformOperationPtr plSteamPlatformBackend::Start(const plPlatformMatchmakingRequest& request)
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Matchmaking.Start");

  if (!IsSignedIn())
  {
    pOp->Complete(plPlatformError::NotSignedIn);
    return pOp;
  }

  if (m_pTicketOp != nullptr)
  {
    pOp->Complete(plPlatformError::Busy);
    return pOp;
  }

  if (m_CurrentLobby.IsValid())
  {
    pOp->Complete(plPlatformError::AlreadyInLobby);
    return pOp;
  }

  ++m_uiTicketCounter;

  m_TicketRequest = request;
  m_Ticket = plPlatformMatchmakingTicket();
  plStringBuilder sId;
  sId.SetFormat("steam-ticket-{0}", m_uiTicketCounter);
  m_Ticket.m_sId = sId;
  m_Ticket.m_State = plPlatformMatchmakingState::Searching;
  m_TicketStart = plTime::Now();
  m_NextTicketSearch = m_TicketStart;
  m_uiTicketAttempts = 0;
  m_bTicketCallInFlight = false;
  m_pTicketOp = pOp;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::MatchmakingStateChanged;
  e.m_sId = m_Ticket.m_sId;
  e.m_Value = static_cast<plInt32>(m_Ticket.m_State);
  Emit(e);

  return pOp;
}

plPlatformOperationPtr plSteamPlatformBackend::Cancel(plStringView sTicket)
{
  if (m_pTicketOp == nullptr || (!sTicket.IsEmpty() && !m_Ticket.m_sId.IsEqual(sTicket)))
    return plPlatformOperation::CreateFinished("Matchmaking.Cancel", plPlatformError::NotFound);

  SetTicketState(plPlatformMatchmakingState::Cancelled, plPlatformError::Cancelled);
  return plPlatformOperation::CreateFinished("Matchmaking.Cancel", plPlatformError::Success);
}

void plSteamPlatformBackend::TickMatchmaking()
{
  if (m_pTicketOp == nullptr)
    return;

  const plTime now = plTime::Now();
  m_Ticket.m_Elapsed = now - m_TicketStart;

  if (m_pTicketOp->IsCancelRequested())
  {
    SetTicketState(plPlatformMatchmakingState::Cancelled, plPlatformError::Cancelled);
    return;
  }

  if (m_bTicketCallInFlight)
    return;

  if (m_TicketRequest.m_Timeout.IsPositive() && m_Ticket.m_Elapsed > m_TicketRequest.m_Timeout)
  {
    SetTicketState(plPlatformMatchmakingState::Failed, plPlatformError::Timeout);
    return;
  }

  if (now >= m_NextTicketSearch)
  {
    StartTicketSearch();
  }
}

void plSteamPlatformBackend::StartTicketSearch()
{
  ++m_uiTicketAttempts;

  if (m_uiTicketAttempts > s_uiSearchAttempts)
  {
    auto* pCall = NewCall<LobbyCreated_t, LobbyCreateContext>(nullptr);
    pCall->m_Context.m_bForMatchmaking = true;
    pCall->m_Context.m_Desc.m_uiMaxMembers = plMath::Max<plUInt32>(m_TicketRequest.m_uiPlayersWanted, 2);
    pCall->m_Context.m_Desc.m_Visibility = plPlatformLobbyVisibility::Public;
    pCall->m_Context.m_Desc.m_Attributes = m_TicketRequest.m_Attributes;

    const int iMaxMembers = static_cast<int>(pCall->m_Context.m_Desc.m_uiMaxMembers);
    pCall->Start(this, SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, iMaxMembers), &plSteamPlatformBackend::OnLobbyCreated);

    if (pCall->m_bDone)
    {
      SetTicketState(plPlatformMatchmakingState::Failed, plPlatformError::BackendFailure);
      return;
    }

    m_bTicketCallInFlight = true;
    return;
  }

  ApplySearchFilters(m_TicketRequest.m_Filters, s_uiSearchResults, true);

  auto* pCall = NewCall<LobbyMatchList_t, LobbyFindContext>(nullptr);
  pCall->m_Context.m_bForMatchmaking = true;
  pCall->Start(this, SteamMatchmaking()->RequestLobbyList(), &plSteamPlatformBackend::OnLobbyMatchList);

  if (pCall->m_bDone)
  {
    m_NextTicketSearch = plTime::Now() + s_SearchInterval;
    return;
  }

  m_bTicketCallInFlight = true;
}

void plSteamPlatformBackend::OnMatchmakingLobbies(plUInt32 uiCount)
{
  m_bTicketCallInFlight = false;

  if (m_pTicketOp == nullptr)
    return;

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const CSteamID lobby = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i));
    if (!lobby.IsValid())
      continue;

    auto* pCall = NewCall<LobbyEnter_t, LobbyJoinContext>(nullptr);
    pCall->m_Context.m_bForMatchmaking = true;
    pCall->Start(this, SteamMatchmaking()->JoinLobby(lobby), &plSteamPlatformBackend::OnLobbyEntered);

    if (!pCall->m_bDone)
    {
      m_bTicketCallInFlight = true;
      return;
    }
  }

  m_NextTicketSearch = plTime::Now() + s_SearchInterval;
}

void plSteamPlatformBackend::OnMatchmakingJoined(bool bSuccess, CSteamID lobby)
{
  m_bTicketCallInFlight = false;

  if (m_pTicketOp == nullptr)
    return;

  if (bSuccess && lobby.IsValid())
  {
    m_Ticket.m_Lobby = MakeLobbyId(lobby);
    SetTicketState(plPlatformMatchmakingState::Found, plPlatformError::Success);
    return;
  }

  m_NextTicketSearch = plTime::Now() + s_SearchInterval;
}

void plSteamPlatformBackend::SetTicketState(plPlatformMatchmakingState state, plPlatformError error)
{
  m_Ticket.m_State = state;
  m_Ticket.m_Elapsed = plTime::Now() - m_TicketStart;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::MatchmakingStateChanged;
  e.m_sId = m_Ticket.m_sId;
  e.m_Value = static_cast<plInt32>(state);
  e.m_Lobby = m_Ticket.m_Lobby;
  Emit(e);

  if (m_pTicketOp != nullptr)
  {
    plPlatformOperationPtr pOp = m_pTicketOp;
    m_pTicketOp = nullptr;
    pOp->MutableResult().m_Ticket = m_Ticket;
    pOp->Complete(error);
  }
}
