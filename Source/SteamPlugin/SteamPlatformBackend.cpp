#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/CommandLineUtils.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <SteamPlugin/SteamPlatformBackend.h>

PL_IMPLEMENT_SINGLETON(plSteamPlatformBackend);

plSteamPlatformBackend::plSteamPlatformBackend()
  : m_SingletonRegistrar(this)
{
}

plSteamPlatformBackend::~plSteamPlatformBackend()
{
  if (m_bInitialized)
  {
    Shutdown();
  }
}

plResult plSteamPlatformBackend::Initialize(plPlatformBackendHost& ref_host)
{
  m_pHost = &ref_host;

  const plVariantDictionary& settings = ref_host.GetBackendSettings();
  if (const plVariant* pAppId = settings.GetValue("AppId"))
  {
    m_uiAppId = pAppId->ConvertTo<plUInt32>();
  }

  if (m_uiAppId != 0)
  {
    WriteAppIdFile(m_uiAppId);

    // True means Steam is relaunching the game itself; this process has to exit and must not talk to Steam.
    if (SteamAPI_RestartAppIfNecessary(m_uiAppId))
    {
      plLog::Warning("Steam: not launched through Steam, relaunching (app id {0}). This process should exit.", m_uiAppId);
      return PL_FAILURE;
    }
  }
  else
  {
    plLog::Info("Steam: no AppId configured in PlatformServices.ddl, relying on steam_appid.txt or the Steam launch environment.");
  }

#if PL_STEAM_HAS_INIT_EX
  SteamErrMsg errorMessage = {};
  const ESteamAPIInitResult initResult = SteamAPI_InitEx(&errorMessage);
  if (initResult != k_ESteamAPIInitResult_OK)
  {
    plLog::Error("Steam: SteamAPI_InitEx failed ({0}): {1}", (int)initResult, errorMessage);
    return PL_FAILURE;
  }
#else
  if (!SteamAPI_Init())
  {
    plLog::Error("Steam: SteamAPI_Init failed. Steam is not running, the app id is unknown (no steam_appid.txt, no AppId setting) or steam_api64.dll is missing beside the executable.");
    return PL_FAILURE;
  }
#endif

  if (SteamUser() == nullptr || SteamFriends() == nullptr || SteamUserStats() == nullptr || SteamMatchmaking() == nullptr)
  {
    plLog::Error("Steam: the client interfaces are unavailable after init.");
    SteamAPI_Shutdown();
    return PL_FAILURE;
  }

  RegisterCallbacks();

  if (SteamNetworkingUtils() != nullptr)
  {
    SteamNetworkingUtils()->InitRelayNetworkAccess();
  }

  RefreshLocalUser();
  m_OnlineState = SteamUser()->BLoggedOn() ? plPlatformOnlineState::Online : plPlatformOnlineState::Offline;

#if PL_STEAM_HAS_REQUEST_CURRENT_STATS
  m_bStatsReceived = false;
  if (!SteamUserStats()->RequestCurrentStats())
  {
    plLog::Warning("Steam: RequestCurrentStats was refused; achievements and stats stay queued until the user is logged on.");
  }
#else
  // Newer SDKs request the stats during init; UserStatsReceived_t still arrives and is harmless.
  m_bStatsReceived = true;
#endif

  ParseLaunchArguments();

  m_bInitialized = true;
  plLog::Info("Steam: initialised as '{0}' ({1}), app id {2}.", m_LocalUser.m_sDisplayName, m_LocalUser.m_Id.m_sCanonical, SteamUtils()->GetAppID());
  return PL_SUCCESS;
}

void plSteamPlatformBackend::Shutdown()
{
  if (!m_bInitialized)
    return;

  if (m_pTicketOp != nullptr)
  {
    SetTicketState(plPlatformMatchmakingState::Cancelled, plPlatformError::Cancelled);
  }

  if (m_CurrentLobby.IsValid())
  {
    SteamMatchmaking()->LeaveLobby(m_CurrentLobby);
    m_CurrentLobby = CSteamID();
  }

  DisconnectAll();
  CancelPendingCalls();

  for (const PendingUserQuery& query : m_PendingUserQueries)
    query.m_pOp->Complete(plPlatformError::Cancelled);
  m_PendingUserQueries.Clear();

  for (const PendingUserQuery& query : m_PendingAvatarQueries)
    query.m_pOp->Complete(plPlatformError::Cancelled);
  m_PendingAvatarQueries.Clear();

  for (StatsRequest& request : m_QueuedStatsRequests)
    request.m_pOp->Complete(plPlatformError::Cancelled);
  m_QueuedStatsRequests.Clear();

  while (!m_StoreWaiters.IsEmpty())
  {
    m_StoreWaiters.PeekFront()->Complete(plPlatformError::Cancelled);
    m_StoreWaiters.PopFront();
  }

  UnregisterCallbacks();
  SteamAPI_Shutdown();

  m_LeaderboardHandles.Clear();
  m_TransportEvents.Clear();
  m_bStatsReceived = false;
  m_bInitialized = false;
  m_pHost = nullptr;
}

void plSteamPlatformBackend::Tick()
{
  if (!m_bInitialized)
    return;

  SteamAPI_RunCallbacks();
  SweepFinishedCalls();
  TickMatchmaking();
  ReceiveTransportMessages();
}

plBitflags<plPlatformCapability> plSteamPlatformBackend::GetCapabilities() const
{
  plBitflags<plPlatformCapability> caps;
  caps.Add(plPlatformCapability::Achievements);
  caps.Add(plPlatformCapability::Stats);
  caps.Add(plPlatformCapability::Leaderboards);
  caps.Add(plPlatformCapability::Friends);
  caps.Add(plPlatformCapability::Presence);
  caps.Add(plPlatformCapability::Lobbies);
  caps.Add(plPlatformCapability::Matchmaking);
  caps.Add(plPlatformCapability::Sessions);
  caps.Add(plPlatformCapability::CloudStorage);
  caps.Add(plPlatformCapability::Entitlements);
  caps.Add(plPlatformCapability::Overlay);
  caps.Add(plPlatformCapability::Transport);
  return caps;
}

plPlatformOnlineState plSteamPlatformBackend::GetOnlineState() const
{
  return m_bInitialized ? m_OnlineState : plPlatformOnlineState::Offline;
}

//////////////////////////////////////////////////////////////////////////
// Helpers

plPlatformUserId plSteamPlatformBackend::MakeUserId(CSteamID id)
{
  plStringBuilder sNative;
  sNative.SetFormat("{0}", id.ConvertToUint64());
  return plPlatformUserId::Make("steam", sNative);
}

CSteamID plSteamPlatformBackend::ParseUserId(const plPlatformUserId& user)
{
  if (!user.IsValid() || !user.GetBackend().IsEqual("steam"))
    return CSteamID();

  plInt64 iValue = 0;
  if (plConversionUtils::StringToInt64(user.GetNativeId(), iValue).Failed() || iValue <= 0)
    return CSteamID();

  return CSteamID(static_cast<uint64>(iValue));
}

plPlatformLobbyId plSteamPlatformBackend::MakeLobbyId(CSteamID id)
{
  plPlatformLobbyId lobby;
  plStringBuilder sId;
  sId.SetFormat("{0}", id.ConvertToUint64());
  lobby.m_sId = sId;
  return lobby;
}

CSteamID plSteamPlatformBackend::ParseLobbyId(const plPlatformLobbyId& lobby)
{
  plInt64 iValue = 0;
  if (!lobby.IsValid() || plConversionUtils::StringToInt64(lobby.m_sId, iValue).Failed() || iValue <= 0)
    return CSteamID();

  return CSteamID(static_cast<uint64>(iValue));
}

plPlatformError plSteamPlatformBackend::MapResult(EResult result)
{
  switch (result)
  {
    case k_EResultOK:
      return plPlatformError::Success;
    case k_EResultNoConnection:
    case k_EResultServiceUnavailable:
      return plPlatformError::Offline;
    case k_EResultNotLoggedOn:
      return plPlatformError::NotSignedIn;
    case k_EResultInvalidParam:
      return plPlatformError::InvalidArgument;
    case k_EResultFileNotFound:
      return plPlatformError::NotFound;
    case k_EResultBusy:
    case k_EResultPending:
      return plPlatformError::Busy;
    case k_EResultCancelled:
      return plPlatformError::Cancelled;
    case k_EResultTimeout:
      return plPlatformError::Timeout;
    case k_EResultLimitExceeded:
    case k_EResultRateLimitExceeded:
      return plPlatformError::RateLimited;
    case k_EResultAccessDenied:
    case k_EResultInsufficientPrivilege:
    case k_EResultBanned:
      return plPlatformError::PermissionDenied;
    default:
      return plPlatformError::BackendFailure;
  }
}

void plSteamPlatformBackend::Emit(const plPlatformEvent& event)
{
  if (m_pHost != nullptr)
  {
    m_pHost->Emit(event);
  }
}

void plSteamPlatformBackend::SweepFinishedCalls()
{
  for (plUInt32 i = 0; i < m_PendingCalls.GetCount();)
  {
    if (m_PendingCalls[i]->m_bDone)
    {
      PL_DEFAULT_DELETE(m_PendingCalls[i]);
      m_PendingCalls.RemoveAtAndSwap(i);
    }
    else
    {
      ++i;
    }
  }
}

void plSteamPlatformBackend::CancelPendingCalls()
{
  for (plSteamPendingCall* pCall : m_PendingCalls)
  {
    if (!pCall->m_bDone && pCall->m_pOp != nullptr)
    {
      pCall->m_pOp->Complete(plPlatformError::Cancelled);
    }

    PL_DEFAULT_DELETE(pCall);
  }

  m_PendingCalls.Clear();
}

void plSteamPlatformBackend::RegisterCallbacks()
{
  m_cbPersonaStateChange.Register(this, &plSteamPlatformBackend::OnPersonaStateChange);
  m_cbAvatarImageLoaded.Register(this, &plSteamPlatformBackend::OnAvatarImageLoaded);
  m_cbUserStatsReceived.Register(this, &plSteamPlatformBackend::OnUserStatsReceived);
  m_cbUserStatsStored.Register(this, &plSteamPlatformBackend::OnUserStatsStored);
  m_cbUserAchievementStored.Register(this, &plSteamPlatformBackend::OnUserAchievementStored);
  m_cbLobbyChatUpdate.Register(this, &plSteamPlatformBackend::OnLobbyChatUpdate);
  m_cbLobbyDataUpdate.Register(this, &plSteamPlatformBackend::OnLobbyDataUpdate);
  m_cbLobbyChatMsg.Register(this, &plSteamPlatformBackend::OnLobbyChatMsg);
  m_cbLobbyKicked.Register(this, &plSteamPlatformBackend::OnLobbyKicked);
  m_cbGameLobbyJoinRequested.Register(this, &plSteamPlatformBackend::OnGameLobbyJoinRequested);
  m_cbGameRichPresenceJoinRequested.Register(this, &plSteamPlatformBackend::OnGameRichPresenceJoinRequested);
  m_cbDlcInstalled.Register(this, &plSteamPlatformBackend::OnDlcInstalled);
  m_cbGameOverlayActivated.Register(this, &plSteamPlatformBackend::OnGameOverlayActivated);
  m_cbSteamServersConnected.Register(this, &plSteamPlatformBackend::OnSteamServersConnected);
  m_cbSteamServersDisconnected.Register(this, &plSteamPlatformBackend::OnSteamServersDisconnected);
  m_cbSessionRequest.Register(this, &plSteamPlatformBackend::OnSessionRequest);
  m_cbSessionFailed.Register(this, &plSteamPlatformBackend::OnSessionFailed);
}

void plSteamPlatformBackend::UnregisterCallbacks()
{
  m_cbPersonaStateChange.Unregister();
  m_cbAvatarImageLoaded.Unregister();
  m_cbUserStatsReceived.Unregister();
  m_cbUserStatsStored.Unregister();
  m_cbUserAchievementStored.Unregister();
  m_cbLobbyChatUpdate.Unregister();
  m_cbLobbyDataUpdate.Unregister();
  m_cbLobbyChatMsg.Unregister();
  m_cbLobbyKicked.Unregister();
  m_cbGameLobbyJoinRequested.Unregister();
  m_cbGameRichPresenceJoinRequested.Unregister();
  m_cbDlcInstalled.Unregister();
  m_cbGameOverlayActivated.Unregister();
  m_cbSteamServersConnected.Unregister();
  m_cbSteamServersDisconnected.Unregister();
  m_cbSessionRequest.Unregister();
  m_cbSessionFailed.Unregister();
}

void plSteamPlatformBackend::WriteAppIdFile(plUInt32 uiAppId)
{
#if PL_ENABLED(PL_COMPILE_FOR_DEVELOPMENT) && !defined(BUILDSYSTEM_BUILDTYPE_Shipping)
  plStringBuilder sPath = plOSFile::GetApplicationDirectory();
  sPath.AppendPath("steam_appid.txt");

  if (plOSFile::ExistsFile(sPath))
    return;

  plOSFile file;
  if (file.Open(sPath, plFileOpenMode::Write).Failed())
  {
    plLog::Warning("Steam: could not write '{0}'.", sPath);
    return;
  }

  plStringBuilder sContent;
  sContent.SetFormat("{0}", uiAppId);
  file.Write(sContent.GetData(), sContent.GetElementCount()).IgnoreResult();
  plLog::Info("Steam: wrote '{0}' for development runs outside the Steam client.", sPath);
#else
  PL_IGNORE_UNUSED(uiAppId);
#endif
}

void plSteamPlatformBackend::RefreshLocalUser()
{
  const CSteamID id = SteamUser()->GetSteamID();
  m_LocalUser = plPlatformUser();
  m_LocalUser.m_Id = MakeUserId(id);
  m_LocalUser.m_sDisplayName = SteamFriends()->GetPersonaName();
  m_LocalUser.m_Status = SteamUser()->BLoggedOn() ? plPlatformPresenceStatus::InGame : plPlatformPresenceStatus::Offline;
  m_LocalUser.m_bInSameGame = true;
}

void plSteamPlatformBackend::ParseLaunchArguments()
{
  // Tokens from the process command line and from Steam's launch line (an invite accepted while the game was not
  // running arrives there). "+connect_lobby <id>" names a lobby, "+connect <string>" a rich-presence connect string.
  plDynamicArray<plString> tokens;

  if (const plCommandLineUtils* pCmd = plCommandLineUtils::GetGlobalInstance())
  {
    for (plUInt32 i = 0; i < pCmd->GetParameterCount(); ++i)
    {
      tokens.PushBack(pCmd->GetParameter(i));
    }
  }

  if (SteamApps() != nullptr)
  {
    char szLaunch[1024] = {};
    if (SteamApps()->GetLaunchCommandLine(szLaunch, sizeof(szLaunch)) > 0)
    {
      plStringBuilder sLaunch = szLaunch;
      plHybridArray<plStringView, 16> parts;
      sLaunch.GetView().Split(false, parts, " ");
      for (plStringView part : parts)
      {
        tokens.PushBack(part);
      }
    }
  }

  for (plUInt32 i = 0; i + 1 < tokens.GetCount(); ++i)
  {
    if (tokens[i].IsEqual_NoCase("+connect_lobby"))
    {
      plPlatformLobbyId lobby;
      lobby.m_sId = tokens[i + 1];
      if (ParseLobbyId(lobby).IsValid())
      {
        m_LaunchJoin = plPlatformJoinRequest();
        m_LaunchJoin.m_Lobby = lobby;
        m_bLaunchJoin = true;
        plLog::Info("Steam: launch argument asks to join lobby {0}.", lobby.m_sId);
        return;
      }
    }
    else if (tokens[i].IsEqual_NoCase("+connect"))
    {
      m_LaunchJoin = plPlatformJoinRequest();
      m_LaunchJoin.m_sConnectString = tokens[i + 1];
      m_bLaunchJoin = true;
      plLog::Info("Steam: launch argument carries connect string '{0}'.", m_LaunchJoin.m_sConnectString);
      return;
    }
  }
}

void plSteamPlatformBackend::OnSteamServersConnected(SteamServersConnected_t* pParam)
{
  PL_IGNORE_UNUSED(pParam);
  m_OnlineState = plPlatformOnlineState::Online;
  RefreshLocalUser();

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::OnlineStateChanged;
  e.m_Value = static_cast<plInt32>(m_OnlineState);
  Emit(e);
}

void plSteamPlatformBackend::OnSteamServersDisconnected(SteamServersDisconnected_t* pParam)
{
  PL_IGNORE_UNUSED(pParam);
  m_OnlineState = plPlatformOnlineState::Offline;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::OnlineStateChanged;
  e.m_Value = static_cast<plInt32>(m_OnlineState);
  Emit(e);
}
