#pragma once

#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/Time/Time.h>
#include <GameEngine/PlatformServices/PlatformBackend.h>
#include <GameEngine/SaveGame/SaveGameCloud.h>
#include <SteamPlugin/SteamIncludes.h>
#include <SteamPlugin/SteamPluginDLL.h>

class plSteamPlatformBackend;

struct plSteamNoContext
{
};

/// One outstanding Steam call result. Owned by the backend, swept from Tick once it has fired.
class plSteamPendingCall
{
public:
  virtual ~plSteamPendingCall() = default;

  plPlatformOperationPtr m_pOp;
  bool m_bDone = false;
};

/// Binds a SteamAPICall_t to a backend member function and carries the request context to it. Every request gets
/// its own instance, so any number of calls of the same kind may be in flight.
template <typename TResult, typename TContext = plSteamNoContext>
class plSteamCallResult final : public plSteamPendingCall
{
public:
  using Handler = void (plSteamPlatformBackend::*)(plSteamCallResult& ref_call, TResult* pResult, bool bIOFailure);

  TContext m_Context;

  /// An invalid handle completes the operation with BackendFailure right away.
  void Start(plSteamPlatformBackend* pBackend, SteamAPICall_t hCall, Handler handler)
  {
    m_pBackend = pBackend;
    m_Handler = handler;

    if (hCall == k_uAPICallInvalid)
    {
      if (m_pOp != nullptr)
        m_pOp->Complete(plPlatformError::BackendFailure);

      m_bDone = true;
      return;
    }

    m_CallResult.Set(hCall, this, &plSteamCallResult::OnResult);
  }

private:
  void OnResult(TResult* pResult, bool bIOFailure)
  {
    (m_pBackend->*m_Handler)(*this, pResult, bIOFailure);
    m_bDone = true;
  }

  plSteamPlatformBackend* m_pBackend = nullptr;
  Handler m_Handler = nullptr;
  CCallResult<plSteamCallResult, TResult> m_CallResult;
};

/// The Steam backend: one object implementing every provider except voice. Registered as the plPlatformBackend
/// singleton by its constructor; the hub calls Initialize once the project config names "steam".
///
/// All provider calls arrive on the main thread; Steam call results and callbacks fire from Tick's
/// SteamAPI_RunCallbacks, also on the main thread. The cloud provider is the exception: the save system calls it
/// from an I/O task and ISteamRemoteStorage's synchronous file calls are thread-safe.
class PL_STEAMPLUGIN_DLL plSteamPlatformBackend final
  : public plPlatformBackend,
    public plPlatformIdentityProvider,
    public plPlatformAchievementsProvider,
    public plPlatformStatsProvider,
    public plPlatformLeaderboardsProvider,
    public plPlatformFriendsProvider,
    public plPlatformPresenceProvider,
    public plPlatformLobbiesProvider,
    public plPlatformMatchmakingProvider,
    public plPlatformSessionsProvider,
    public plPlatformEntitlementsProvider,
    public plPlatformOverlayProvider,
    public plPlatformTransport,
    public plSaveGameCloudProvider
{
  PL_DECLARE_SINGLETON_OF_INTERFACE(plSteamPlatformBackend, plPlatformBackend);

public:
  plSteamPlatformBackend();
  ~plSteamPlatformBackend();

  /// \name plPlatformBackend
  ///@{
  plStringView GetId() const override { return "steam"; }
  plResult Initialize(plPlatformBackendHost& ref_host) override;
  void Shutdown() override;
  void Tick() override;
  plBitflags<plPlatformCapability> GetCapabilities() const override;
  plPlatformOnlineState GetOnlineState() const override;
  plPlatformIdentityProvider& GetIdentity() override { return *this; }
  plPlatformAchievementsProvider* GetAchievements() override { return this; }
  plPlatformStatsProvider* GetStats() override { return this; }
  plPlatformLeaderboardsProvider* GetLeaderboards() override { return this; }
  plPlatformFriendsProvider* GetFriends() override { return this; }
  plPlatformPresenceProvider* GetPresence() override { return this; }
  plPlatformLobbiesProvider* GetLobbies() override { return this; }
  plPlatformMatchmakingProvider* GetMatchmaking() override { return this; }
  plPlatformSessionsProvider* GetSessions() override { return this; }
  plPlatformEntitlementsProvider* GetEntitlements() override { return this; }
  plPlatformOverlayProvider* GetOverlay() override { return this; }
  plPlatformTransport* GetTransport() override { return this; }
  plPlatformVoice* GetVoice() override { return nullptr; }
  plSaveGameCloudProvider* GetCloudSaveProvider() override { return this; }
  ///@}

  /// \name Identity
  ///@{
  bool IsSignedIn() const override;
  const plPlatformUser& GetLocalUser() const override { return m_LocalUser; }
  plPlatformOperationPtr SignIn() override;
  plPlatformOperationPtr SignOut() override;
  plPlatformOperationPtr QueryUser(const plPlatformUserId& user) override;
  plPlatformOperationPtr QueryAvatar(const plPlatformUserId& user) override;
  ///@}

  /// \name Achievements
  ///@{
  plPlatformOperationPtr Unlock(plStringView sBackendId) override;
  plPlatformOperationPtr SetProgress(plStringView sBackendId, plUInt32 uiCurrent, plUInt32 uiTarget) override;
  plPlatformOperationPtr QueryAchievements() override;
  plPlatformOperationPtr Reset(plStringView sBackendId) override;
  ///@}

  /// \name Stats
  ///@{
  plPlatformOperationPtr Store(plArrayPtr<const plPlatformStatValue> values) override;
  plPlatformOperationPtr QueryStats() override;
  ///@}

  /// \name Leaderboards
  ///@{
  plPlatformOperationPtr UploadScore(plStringView sBackendId, plInt64 iScore, plPlatformLeaderboardUpload method, plArrayPtr<const plInt32> details) override;
  plPlatformOperationPtr Read(plStringView sBackendId, const plPlatformLeaderboardQuery& query) override;
  ///@}

  /// \name Friends
  ///@{
  plPlatformOperationPtr QueryFriends() override;
  bool IsBlocked(const plPlatformUserId& user) const override;
  ///@}

  /// \name Presence
  ///@{
  plResult Set(plStringView sKey, plStringView sValue) override;
  plResult SetJoinString(plStringView sConnect) override;
  void Clear() override;
  ///@}

  /// \name Lobbies
  ///@{
  plPlatformOperationPtr Create(const plPlatformLobbyDesc& desc) override;
  plPlatformOperationPtr Find(const plPlatformLobbySearch& search) override;
  plPlatformOperationPtr Join(const plPlatformLobbyId& lobby) override;
  plPlatformOperationPtr Leave(const plPlatformLobbyId& lobby) override;
  const plPlatformLobbyInfo* GetInfo(const plPlatformLobbyId& lobby) const override;
  plResult SetAttribute(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue) override;
  plResult SetMemberAttribute(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue) override;
  plResult SetOwner(const plPlatformLobbyId& lobby, const plPlatformUserId& user) override;
  plResult SetVisibility(const plPlatformLobbyId& lobby, plPlatformLobbyVisibility visibility) override;
  plResult SendChat(const plPlatformLobbyId& lobby, plStringView sText) override;
  ///@}

  /// \name Matchmaking
  ///@{
  plPlatformOperationPtr Start(const plPlatformMatchmakingRequest& request) override;
  plPlatformOperationPtr Cancel(plStringView sTicket) override;
  ///@}

  /// \name Sessions
  ///@{
  plPlatformOperationPtr Invite(const plPlatformUserId& user, const plPlatformLobbyId& lobby) override;
  bool TakeLaunchJoinRequest(plPlatformJoinRequest& out_request) override;
  ///@}

  /// \name Entitlements
  ///@{
  bool OwnsApp() const override;
  bool OwnsDlc(plStringView sBackendId) const override;
  plPlatformOperationPtr Refresh() override;
  ///@}

  /// \name Overlay
  ///@{
  bool IsVisible() const override { return m_bOverlayVisible; }
  plResult Show(plPlatformOverlayPage page) override;
  plResult ShowUser(const plPlatformUserId& user) override;
  plResult ShowInviteDialog(const plPlatformLobbyId& lobby) override;
  plResult ShowStore(plStringView sDlcBackendId) override;
  ///@}

  /// \name Transport
  ///@{
  plResult Listen() override;
  void StopListening() override;
  plResult Connect(const plPlatformUserId& user) override;
  void Disconnect(const plPlatformUserId& user) override;
  void DisconnectAll() override;
  plResult Send(const plPlatformUserId& user, plArrayPtr<const plUInt8> data, bool bReliable, plUInt8 uiChannel) override;
  bool PollEvent(plPlatformTransportEvent& out_event) override;
  plUInt32 GetMaxPayloadBytes() const override;
  ///@}

  /// \name Cloud saves
  ///@{
  plSaveGameError Read(const plSaveGameCloudKey& key, plSaveGameCloudRecord& out_record) override;
  plSaveGameError Publish(const plSaveGameCloudKey& key, const plSaveGameCloudRecord& record, const plUuid& expectedRemoteRevision) override;
  plSaveGameError Enumerate(plStringView sProject, const plUuid& profile, plDynamicArray<plSaveGameCloudKey>& out_keys) override;
  ///@}

  /// \name Id conversions
  ///@{
  static plPlatformUserId MakeUserId(CSteamID id);
  /// Invalid CSteamID when the id is not a "steam:<number>" id.
  static CSteamID ParseUserId(const plPlatformUserId& user);
  static plPlatformLobbyId MakeLobbyId(CSteamID id);
  static CSteamID ParseLobbyId(const plPlatformLobbyId& lobby);
  static plPlatformError MapResult(EResult result);
  ///@}

private:
  struct PendingUserQuery
  {
    CSteamID m_Id;
    plPlatformOperationPtr m_pOp;
  };

  struct StatsRequest
  {
    enum class Kind : plUInt8
    {
      Unlock,
      Reset,
      QueryAchievements,
      StoreStats,
      QueryStats,
    };

    Kind m_Kind = Kind::Unlock;
    plString m_sId;
    plDynamicArray<plPlatformStatValue> m_Values;
    plPlatformOperationPtr m_pOp;
  };

  struct LeaderboardUploadContext
  {
    plString m_sId;
    plInt64 m_iScore = 0;
    plPlatformLeaderboardUpload m_Method = plPlatformLeaderboardUpload::KeepBest;
    plHybridArray<plInt32, 8> m_Details;
  };

  struct LeaderboardReadContext
  {
    plString m_sId;
    plPlatformLeaderboardQuery m_Query;
  };

  struct LobbyCreateContext
  {
    plPlatformLobbyDesc m_Desc;
    bool m_bForMatchmaking = false;
  };

  struct LobbyFindContext
  {
    bool m_bForMatchmaking = false;
  };

  struct LobbyJoinContext
  {
    bool m_bForMatchmaking = false;
  };

  /// pOp may be null for calls whose outcome is reported elsewhere (matchmaking).
  template <typename TResult, typename TContext = plSteamNoContext>
  plSteamCallResult<TResult, TContext>* NewCall(const plPlatformOperationPtr& pOp)
  {
    using CallType = plSteamCallResult<TResult, TContext>;
    CallType* pCall = PL_DEFAULT_NEW(CallType);
    pCall->m_pOp = pOp;
    m_PendingCalls.PushBack(pCall);
    return pCall;
  }

  void SweepFinishedCalls();
  void CancelPendingCalls();
  void RegisterCallbacks();
  void UnregisterCallbacks();
  void Emit(const plPlatformEvent& event);
  void WriteAppIdFile(plUInt32 uiAppId);
  void RefreshLocalUser();
  void ParseLaunchArguments();

  // Identity
  plPlatformUser DescribeUser(CSteamID id) const;
  bool EncodeAvatar(int iImage, plDynamicArray<plUInt8>& out_png) const;
  void OnPersonaStateChange(PersonaStateChange_t* pParam);
  void OnAvatarImageLoaded(AvatarImageLoaded_t* pParam);

  // Achievements and stats
  plPlatformOperationPtr SubmitStatsRequest(StatsRequest&& request);
  void RunStatsRequest(StatsRequest& ref_request);
  void OnUserStatsReceived(UserStatsReceived_t* pParam);
  void OnUserStatsStored(UserStatsStored_t* pParam);
  void OnUserAchievementStored(UserAchievementStored_t* pParam);

  // Leaderboards
  void StartUpload(SteamLeaderboard_t hLeaderboard, plSteamCallResult<LeaderboardFindResult_t, LeaderboardUploadContext>& ref_call);
  void StartDownload(SteamLeaderboard_t hLeaderboard, plSteamCallResult<LeaderboardFindResult_t, LeaderboardReadContext>& ref_call);
  void OnLeaderboardFoundForUpload(plSteamCallResult<LeaderboardFindResult_t, LeaderboardUploadContext>& ref_call, LeaderboardFindResult_t* pResult, bool bIOFailure);
  void OnLeaderboardFoundForRead(plSteamCallResult<LeaderboardFindResult_t, LeaderboardReadContext>& ref_call, LeaderboardFindResult_t* pResult, bool bIOFailure);
  void OnLeaderboardScoreUploaded(plSteamCallResult<LeaderboardScoreUploaded_t, LeaderboardUploadContext>& ref_call, LeaderboardScoreUploaded_t* pResult, bool bIOFailure);
  void OnLeaderboardScoresDownloaded(plSteamCallResult<LeaderboardScoresDownloaded_t, LeaderboardReadContext>& ref_call, LeaderboardScoresDownloaded_t* pResult, bool bIOFailure);

  // Lobbies
  void FillLobbyInfo(CSteamID lobby, plPlatformLobbyInfo& out_info, bool bWithMembers) const;
  void RefreshCurrentLobby();
  void ApplySearchFilters(const plDynamicArray<plPlatformLobbyFilter>& filters, plUInt32 uiMaxResults, bool bAvailableSlotsOnly);
  void OnLobbyCreated(plSteamCallResult<LobbyCreated_t, LobbyCreateContext>& ref_call, LobbyCreated_t* pResult, bool bIOFailure);
  void OnLobbyMatchList(plSteamCallResult<LobbyMatchList_t, LobbyFindContext>& ref_call, LobbyMatchList_t* pResult, bool bIOFailure);
  void OnLobbyEntered(plSteamCallResult<LobbyEnter_t, LobbyJoinContext>& ref_call, LobbyEnter_t* pResult, bool bIOFailure);
  void OnLobbyChatUpdate(LobbyChatUpdate_t* pParam);
  void OnLobbyDataUpdate(LobbyDataUpdate_t* pParam);
  void OnLobbyChatMsg(LobbyChatMsg_t* pParam);
  void OnLobbyKicked(LobbyKicked_t* pParam);
  void OnGameLobbyJoinRequested(GameLobbyJoinRequested_t* pParam);
  void OnGameRichPresenceJoinRequested(GameRichPresenceJoinRequested_t* pParam);

  // Matchmaking
  void TickMatchmaking();
  void StartTicketSearch();
  void SetTicketState(plPlatformMatchmakingState state, plPlatformError error);
  void OnMatchmakingLobbies(plUInt32 uiCount);
  void OnMatchmakingJoined(bool bSuccess, CSteamID lobby);

  // Entitlements, overlay, online state
  void OnDlcInstalled(DlcInstalled_t* pParam);
  void OnGameOverlayActivated(GameOverlayActivated_t* pParam);
  void OnSteamServersConnected(SteamServersConnected_t* pParam);
  void OnSteamServersDisconnected(SteamServersDisconnected_t* pParam);

  // Transport
  void ReceiveTransportMessages();
  void PushTransportEvent(plPlatformTransportEvent::Type type, CSteamID user);
  bool IsPeer(CSteamID user) const;
  void OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* pParam);
  void OnSessionFailed(SteamNetworkingMessagesSessionFailed_t* pParam);

  // Cloud
  plString CloudFileName(const plSaveGameCloudKey& key) const;
  plString CloudFilePrefix(plStringView sProject, const plUuid& profile) const;
  bool ReadCloudFile(const char* szName, plSaveGameCloudRecord& out_record, plSaveGameError& out_error) const;
  bool IsCloudAvailable() const;

  plPlatformBackendHost* m_pHost = nullptr;
  bool m_bInitialized = false;
  plUInt32 m_uiAppId = 0;
  plPlatformUser m_LocalUser;
  plPlatformOnlineState m_OnlineState = plPlatformOnlineState::Offline;
  bool m_bOverlayVisible = false;
  plDynamicArray<plSteamPendingCall*> m_PendingCalls;

  // Identity
  plDynamicArray<PendingUserQuery> m_PendingUserQueries;
  plDynamicArray<PendingUserQuery> m_PendingAvatarQueries;

  // Achievements and stats
  bool m_bStatsReceived = false;
  plDynamicArray<StatsRequest> m_QueuedStatsRequests;
  plDeque<plPlatformOperationPtr> m_StoreWaiters;

  // Leaderboards
  plMap<plString, SteamLeaderboard_t> m_LeaderboardHandles;

  // Lobbies
  CSteamID m_CurrentLobby;
  plPlatformLobbyInfo m_CurrentLobbyInfo;
  plPlatformLobbyVisibility m_CurrentLobbyVisibility = plPlatformLobbyVisibility::Public; ///< Steam does not report a lobby's type back; remembered from Create.

  // Matchmaking
  plPlatformMatchmakingTicket m_Ticket;
  plPlatformMatchmakingRequest m_TicketRequest;
  plPlatformOperationPtr m_pTicketOp;
  plTime m_TicketStart;
  plTime m_NextTicketSearch;
  plUInt32 m_uiTicketAttempts = 0;
  plUInt32 m_uiTicketCounter = 0;
  bool m_bTicketCallInFlight = false;

  // Sessions
  plPlatformJoinRequest m_LaunchJoin;
  bool m_bLaunchJoin = false;

  // Transport
  bool m_bListening = false;
  plDynamicArray<CSteamID> m_Peers;
  plDeque<plPlatformTransportEvent> m_TransportEvents;

  // Steam callbacks, registered in Initialize and unregistered in Shutdown.
  CCallbackManual<plSteamPlatformBackend, PersonaStateChange_t> m_cbPersonaStateChange;
  CCallbackManual<plSteamPlatformBackend, AvatarImageLoaded_t> m_cbAvatarImageLoaded;
  CCallbackManual<plSteamPlatformBackend, UserStatsReceived_t> m_cbUserStatsReceived;
  CCallbackManual<plSteamPlatformBackend, UserStatsStored_t> m_cbUserStatsStored;
  CCallbackManual<plSteamPlatformBackend, UserAchievementStored_t> m_cbUserAchievementStored;
  CCallbackManual<plSteamPlatformBackend, LobbyChatUpdate_t> m_cbLobbyChatUpdate;
  CCallbackManual<plSteamPlatformBackend, LobbyDataUpdate_t> m_cbLobbyDataUpdate;
  CCallbackManual<plSteamPlatformBackend, LobbyChatMsg_t> m_cbLobbyChatMsg;
  CCallbackManual<plSteamPlatformBackend, LobbyKicked_t> m_cbLobbyKicked;
  CCallbackManual<plSteamPlatformBackend, GameLobbyJoinRequested_t> m_cbGameLobbyJoinRequested;
  CCallbackManual<plSteamPlatformBackend, GameRichPresenceJoinRequested_t> m_cbGameRichPresenceJoinRequested;
  CCallbackManual<plSteamPlatformBackend, DlcInstalled_t> m_cbDlcInstalled;
  CCallbackManual<plSteamPlatformBackend, GameOverlayActivated_t> m_cbGameOverlayActivated;
  CCallbackManual<plSteamPlatformBackend, SteamServersConnected_t> m_cbSteamServersConnected;
  CCallbackManual<plSteamPlatformBackend, SteamServersDisconnected_t> m_cbSteamServersDisconnected;
  CCallbackManual<plSteamPlatformBackend, SteamNetworkingMessagesSessionRequest_t> m_cbSessionRequest;
  CCallbackManual<plSteamPlatformBackend, SteamNetworkingMessagesSessionFailed_t> m_cbSessionFailed;
};
