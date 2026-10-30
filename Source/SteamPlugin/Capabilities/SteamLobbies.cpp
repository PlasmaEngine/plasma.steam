#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/Utilities/ConversionUtils.h>
#include <SteamPlugin/SteamPlatformBackend.h>

static ELobbyType ToLobbyType(plPlatformLobbyVisibility visibility)
{
  switch (visibility)
  {
    case plPlatformLobbyVisibility::FriendsOnly:
      return k_ELobbyTypeFriendsOnly;
    case plPlatformLobbyVisibility::Private:
      return k_ELobbyTypePrivate;
    case plPlatformLobbyVisibility::Invisible:
      return k_ELobbyTypeInvisible;
    case plPlatformLobbyVisibility::Public:
    default:
      return k_ELobbyTypePublic;
  }
}

static ELobbyComparison ToLobbyComparison(plPlatformFilterComparison comparison)
{
  switch (comparison)
  {
    case plPlatformFilterComparison::NotEqual:
      return k_ELobbyComparisonNotEqual;
    case plPlatformFilterComparison::Less:
      return k_ELobbyComparisonLessThan;
    case plPlatformFilterComparison::Greater:
      return k_ELobbyComparisonGreaterThan;
    case plPlatformFilterComparison::Equal:
    default:
      return k_ELobbyComparisonEqual;
  }
}

static plPlatformError ToEnterError(plUInt32 uiResponse)
{
  switch (uiResponse)
  {
    case k_EChatRoomEnterResponseSuccess:
      return plPlatformError::Success;
    case k_EChatRoomEnterResponseDoesntExist:
      return plPlatformError::NotFound;
    case k_EChatRoomEnterResponseFull:
      return plPlatformError::LobbyFull;
    case k_EChatRoomEnterResponseNotAllowed:
    case k_EChatRoomEnterResponseBanned:
    case k_EChatRoomEnterResponseLimited:
    case k_EChatRoomEnterResponseCommunityBan:
    case k_EChatRoomEnterResponseMemberBlockedYou:
    case k_EChatRoomEnterResponseYouBlockedMember:
      return plPlatformError::PermissionDenied;
    case k_EChatRoomEnterResponseRatelimitExceeded:
      return plPlatformError::RateLimited;
    default:
      return plPlatformError::BackendFailure;
  }
}

plPlatformOperationPtr plSteamPlatformBackend::Create(const plPlatformLobbyDesc& desc)
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Lobbies.Create");

  if (!IsSignedIn())
  {
    pOp->Complete(plPlatformError::NotSignedIn);
    return pOp;
  }

  if (m_CurrentLobby.IsValid())
  {
    pOp->Complete(plPlatformError::AlreadyInLobby);
    return pOp;
  }

  auto* pCall = NewCall<LobbyCreated_t, LobbyCreateContext>(pOp);
  pCall->m_Context.m_Desc = desc;

  const int iMaxMembers = static_cast<int>(plMath::Max<plUInt32>(desc.m_uiMaxMembers, 1));
  pCall->Start(this, SteamMatchmaking()->CreateLobby(ToLobbyType(desc.m_Visibility), iMaxMembers), &plSteamPlatformBackend::OnLobbyCreated);
  return pOp;
}

plPlatformOperationPtr plSteamPlatformBackend::Find(const plPlatformLobbySearch& search)
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Lobbies.Find");

  if (!IsSignedIn())
  {
    pOp->Complete(plPlatformError::NotSignedIn);
    return pOp;
  }

  ApplySearchFilters(search.m_Filters, search.m_uiMaxResults, search.m_bAvailableSlotsOnly);

  auto* pCall = NewCall<LobbyMatchList_t, LobbyFindContext>(pOp);
  pCall->Start(this, SteamMatchmaking()->RequestLobbyList(), &plSteamPlatformBackend::OnLobbyMatchList);
  return pOp;
}

plPlatformOperationPtr plSteamPlatformBackend::Join(const plPlatformLobbyId& lobby)
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Lobbies.Join");

  if (!IsSignedIn())
  {
    pOp->Complete(plPlatformError::NotSignedIn);
    return pOp;
  }

  const CSteamID lobbyId = ParseLobbyId(lobby);
  if (!lobbyId.IsValid())
  {
    pOp->Complete(plPlatformError::InvalidArgument);
    return pOp;
  }

  if (m_CurrentLobby.IsValid())
  {
    if (m_CurrentLobby == lobbyId)
    {
      RefreshCurrentLobby();
      pOp->MutableResult().m_Lobby = m_CurrentLobbyInfo;
      pOp->Complete(plPlatformError::Success);
    }
    else
    {
      pOp->Complete(plPlatformError::AlreadyInLobby);
    }
    return pOp;
  }

  auto* pCall = NewCall<LobbyEnter_t, LobbyJoinContext>(pOp);
  pCall->Start(this, SteamMatchmaking()->JoinLobby(lobbyId), &plSteamPlatformBackend::OnLobbyEntered);
  return pOp;
}

plPlatformOperationPtr plSteamPlatformBackend::Leave(const plPlatformLobbyId& lobby)
{
  const CSteamID lobbyId = ParseLobbyId(lobby);
  if (!m_bInitialized || !lobbyId.IsValid() || lobbyId != m_CurrentLobby)
    return plPlatformOperation::CreateFinished("Lobbies.Leave", plPlatformError::NotFound);

  SteamMatchmaking()->LeaveLobby(lobbyId);
  m_CurrentLobby = CSteamID();

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::LobbyLeft;
  e.m_Lobby = lobby;
  Emit(e);

  return plPlatformOperation::CreateFinished("Lobbies.Leave", plPlatformError::Success);
}

const plPlatformLobbyInfo* plSteamPlatformBackend::GetInfo(const plPlatformLobbyId& lobby) const
{
  if (!m_CurrentLobby.IsValid() || ParseLobbyId(lobby) != m_CurrentLobby)
    return nullptr;

  return &m_CurrentLobbyInfo;
}

plResult plSteamPlatformBackend::SetAttribute(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue)
{
  const CSteamID lobbyId = ParseLobbyId(lobby);
  if (!m_bInitialized || !lobbyId.IsValid() || sKey.IsEmpty())
    return PL_FAILURE;

  const plString sKeyCopy(sKey);
  const plString sValueCopy(sValue);
  return SteamMatchmaking()->SetLobbyData(lobbyId, sKeyCopy.GetData(), sValueCopy.GetData()) ? PL_SUCCESS : PL_FAILURE;
}

plResult plSteamPlatformBackend::SetMemberAttribute(const plPlatformLobbyId& lobby, plStringView sKey, plStringView sValue)
{
  const CSteamID lobbyId = ParseLobbyId(lobby);
  if (!m_bInitialized || !lobbyId.IsValid() || sKey.IsEmpty())
    return PL_FAILURE;

  const plString sKeyCopy(sKey);
  const plString sValueCopy(sValue);
  SteamMatchmaking()->SetLobbyMemberData(lobbyId, sKeyCopy.GetData(), sValueCopy.GetData());
  return PL_SUCCESS;
}

plResult plSteamPlatformBackend::SetVisibility(const plPlatformLobbyId& lobby, plPlatformLobbyVisibility visibility)
{
  const CSteamID lobbyId = ParseLobbyId(lobby);
  if (!m_bInitialized || !lobbyId.IsValid())
    return PL_FAILURE;

  if (!SteamMatchmaking()->SetLobbyType(lobbyId, ToLobbyType(visibility)))
    return PL_FAILURE;

  m_CurrentLobbyVisibility = visibility;
  m_CurrentLobbyInfo.m_Visibility = visibility;
  return PL_SUCCESS;
}

plResult plSteamPlatformBackend::SetOwner(const plPlatformLobbyId& lobby, const plPlatformUserId& user)
{
  const CSteamID lobbyId = ParseLobbyId(lobby);
  const CSteamID userId = ParseUserId(user);
  if (!m_bInitialized || !lobbyId.IsValid() || !userId.IsValid())
    return PL_FAILURE;

  return SteamMatchmaking()->SetLobbyOwner(lobbyId, userId) ? PL_SUCCESS : PL_FAILURE;
}

plResult plSteamPlatformBackend::SendChat(const plPlatformLobbyId& lobby, plStringView sText)
{
  const CSteamID lobbyId = ParseLobbyId(lobby);
  if (!m_bInitialized || !lobbyId.IsValid() || sText.IsEmpty())
    return PL_FAILURE;

  // Sent with the terminator so the receiving side gets a C string back from GetLobbyChatEntry.
  const plString sTextCopy(sText);
  return SteamMatchmaking()->SendLobbyChatMsg(lobbyId, sTextCopy.GetData(), static_cast<int>(sTextCopy.GetElementCount() + 1)) ? PL_SUCCESS : PL_FAILURE;
}

//////////////////////////////////////////////////////////////////////////

void plSteamPlatformBackend::FillLobbyInfo(CSteamID lobby, plPlatformLobbyInfo& out_info, bool bWithMembers) const
{
  ISteamMatchmaking* pMatchmaking = SteamMatchmaking();

  out_info = plPlatformLobbyInfo();
  out_info.m_Id = MakeLobbyId(lobby);
  out_info.m_Owner = MakeUserId(pMatchmaking->GetLobbyOwner(lobby));
  out_info.m_uiMaxMembers = static_cast<plUInt32>(plMath::Max(pMatchmaking->GetLobbyMemberLimit(lobby), 0));
  out_info.m_uiMemberCount = static_cast<plUInt32>(plMath::Max(pMatchmaking->GetNumLobbyMembers(lobby), 0));

  const int iDataCount = pMatchmaking->GetLobbyDataCount(lobby);
  for (int i = 0; i < iDataCount; ++i)
  {
    char szKey[k_nMaxLobbyKeyLength + 1] = {};
    char szValue[k_cubChatMetadataMax] = {};
    if (!pMatchmaking->GetLobbyDataByIndex(lobby, i, szKey, sizeof(szKey), szValue, sizeof(szValue)))
      continue;

    plPlatformLobbyAttribute& attribute = out_info.m_Attributes.ExpandAndGetRef();
    attribute.m_sKey = szKey;
    attribute.m_sValue = szValue;
  }

  if (!bWithMembers)
    return;

  // Steam has no enumeration of member keys, so member attributes are read for the keys the lobby itself uses.
  const int iMemberCount = pMatchmaking->GetNumLobbyMembers(lobby);
  for (int i = 0; i < iMemberCount; ++i)
  {
    const CSteamID memberId = pMatchmaking->GetLobbyMemberByIndex(lobby, i);
    if (!memberId.IsValid())
      continue;

    plPlatformLobbyMember& member = out_info.m_Members.ExpandAndGetRef();
    member.m_User = DescribeUser(memberId);

    for (const plPlatformLobbyAttribute& lobbyAttribute : out_info.m_Attributes)
    {
      const char* szValue = pMatchmaking->GetLobbyMemberData(lobby, memberId, lobbyAttribute.m_sKey.GetData());
      if (szValue == nullptr || szValue[0] == '\0')
        continue;

      plPlatformLobbyAttribute& attribute = member.m_Attributes.ExpandAndGetRef();
      attribute.m_sKey = lobbyAttribute.m_sKey;
      attribute.m_sValue = szValue;
    }
  }
}

void plSteamPlatformBackend::RefreshCurrentLobby()
{
  if (!m_CurrentLobby.IsValid())
  {
    m_CurrentLobbyInfo = plPlatformLobbyInfo();
    return;
  }

  FillLobbyInfo(m_CurrentLobby, m_CurrentLobbyInfo, true);
  m_CurrentLobbyInfo.m_Visibility = m_CurrentLobbyVisibility;
}

void plSteamPlatformBackend::ApplySearchFilters(const plDynamicArray<plPlatformLobbyFilter>& filters, plUInt32 uiMaxResults, bool bAvailableSlotsOnly)
{
  ISteamMatchmaking* pMatchmaking = SteamMatchmaking();

  for (const plPlatformLobbyFilter& filter : filters)
  {
    if (filter.m_sKey.IsEmpty())
      continue;

    const ELobbyComparison comparison = ToLobbyComparison(filter.m_Comparison);

    if (filter.m_bNumeric)
    {
      plInt32 iValue = 0;
      if (plConversionUtils::StringToInt(filter.m_sValue, iValue).Failed())
        continue;

      pMatchmaking->AddRequestLobbyListNumericalFilter(filter.m_sKey.GetData(), iValue, comparison);
    }
    else
    {
      pMatchmaking->AddRequestLobbyListStringFilter(filter.m_sKey.GetData(), filter.m_sValue.GetData(), comparison);
    }
  }

  if (bAvailableSlotsOnly)
  {
    pMatchmaking->AddRequestLobbyListFilterSlotsAvailable(1);
  }

  if (uiMaxResults > 0)
  {
    pMatchmaking->AddRequestLobbyListResultCountFilter(static_cast<int>(uiMaxResults));
  }
}

void plSteamPlatformBackend::OnLobbyCreated(plSteamCallResult<LobbyCreated_t, LobbyCreateContext>& ref_call, LobbyCreated_t* pResult, bool bIOFailure)
{
  const LobbyCreateContext& ctx = ref_call.m_Context;

  if (bIOFailure || pResult == nullptr || pResult->m_eResult != k_EResultOK)
  {
    const plPlatformError error = bIOFailure || pResult == nullptr ? plPlatformError::Offline : MapResult(pResult->m_eResult);
    if (ref_call.m_pOp != nullptr)
      ref_call.m_pOp->Complete(error == plPlatformError::Success ? plPlatformError::BackendFailure : error);
    if (ctx.m_bForMatchmaking)
      OnMatchmakingJoined(false, CSteamID());
    return;
  }

  const CSteamID lobby(pResult->m_ulSteamIDLobby);

  // A matchmaking ticket cancelled while the create was in flight: the lobby is not wanted any more.
  if (ctx.m_bForMatchmaking && m_pTicketOp == nullptr)
  {
    SteamMatchmaking()->LeaveLobby(lobby);
    return;
  }

  for (const plPlatformLobbyAttribute& attribute : ctx.m_Desc.m_Attributes)
  {
    if (!attribute.m_sKey.IsEmpty())
      SteamMatchmaking()->SetLobbyData(lobby, attribute.m_sKey.GetData(), attribute.m_sValue.GetData());
  }

  m_CurrentLobby = lobby;
  m_CurrentLobbyVisibility = ctx.m_Desc.m_Visibility;
  RefreshCurrentLobby();

  if (ref_call.m_pOp != nullptr)
  {
    ref_call.m_pOp->MutableResult().m_Lobby = m_CurrentLobbyInfo;
    ref_call.m_pOp->Complete(plPlatformError::Success);
  }

  if (ctx.m_bForMatchmaking)
    OnMatchmakingJoined(true, lobby);
}

void plSteamPlatformBackend::OnLobbyMatchList(plSteamCallResult<LobbyMatchList_t, LobbyFindContext>& ref_call, LobbyMatchList_t* pResult, bool bIOFailure)
{
  const plUInt32 uiCount = (bIOFailure || pResult == nullptr) ? 0 : pResult->m_nLobbiesMatching;

  if (ref_call.m_Context.m_bForMatchmaking)
  {
    OnMatchmakingLobbies(uiCount);
    return;
  }

  if (ref_call.m_pOp == nullptr)
    return;

  if (bIOFailure || pResult == nullptr)
  {
    ref_call.m_pOp->Complete(plPlatformError::Offline);
    return;
  }

  plDynamicArray<plPlatformLobbyInfo>& lobbies = ref_call.m_pOp->MutableResult().m_Lobbies;
  lobbies.Reserve(uiCount);

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const CSteamID lobby = SteamMatchmaking()->GetLobbyByIndex(static_cast<int>(i));
    if (!lobby.IsValid())
      continue;

    FillLobbyInfo(lobby, lobbies.ExpandAndGetRef(), false);
  }

  ref_call.m_pOp->Complete(plPlatformError::Success);
}

void plSteamPlatformBackend::OnLobbyEntered(plSteamCallResult<LobbyEnter_t, LobbyJoinContext>& ref_call, LobbyEnter_t* pResult, bool bIOFailure)
{
  const bool bForMatchmaking = ref_call.m_Context.m_bForMatchmaking;

  if (bIOFailure || pResult == nullptr)
  {
    if (ref_call.m_pOp != nullptr)
      ref_call.m_pOp->Complete(plPlatformError::Offline);
    if (bForMatchmaking)
      OnMatchmakingJoined(false, CSteamID());
    return;
  }

  const CSteamID lobby(pResult->m_ulSteamIDLobby);
  const plPlatformError error = ToEnterError(pResult->m_EChatRoomEnterResponse);

  if (error != plPlatformError::Success)
  {
    if (ref_call.m_pOp != nullptr)
      ref_call.m_pOp->Complete(error);
    if (bForMatchmaking)
      OnMatchmakingJoined(false, lobby);
    return;
  }

  if (bForMatchmaking && m_pTicketOp == nullptr)
  {
    SteamMatchmaking()->LeaveLobby(lobby);
    return;
  }

  m_CurrentLobby = lobby;
  m_CurrentLobbyVisibility = plPlatformLobbyVisibility::Public;
  RefreshCurrentLobby();

  if (ref_call.m_pOp != nullptr)
  {
    ref_call.m_pOp->MutableResult().m_Lobby = m_CurrentLobbyInfo;
    ref_call.m_pOp->Complete(plPlatformError::Success);
  }

  if (bForMatchmaking)
    OnMatchmakingJoined(true, lobby);
}

void plSteamPlatformBackend::OnLobbyChatUpdate(LobbyChatUpdate_t* pParam)
{
  const CSteamID lobby(pParam->m_ulSteamIDLobby);
  if (!m_CurrentLobby.IsValid() || lobby != m_CurrentLobby)
    return;

  const CSteamID changed(pParam->m_ulSteamIDUserChanged);
  const bool bEntered = (pParam->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered) != 0;
  const CSteamID previousOwner = ParseUserId(m_CurrentLobbyInfo.m_Owner);

  if (!bEntered && changed == SteamUser()->GetSteamID())
  {
    m_CurrentLobby = CSteamID();
    RefreshCurrentLobby();

    plPlatformEvent e;
    e.m_Type = plPlatformEvent::Type::LobbyLeft;
    e.m_Lobby = MakeLobbyId(lobby);
    Emit(e);
    return;
  }

  RefreshCurrentLobby();

  plPlatformEvent e;
  e.m_Type = bEntered ? plPlatformEvent::Type::LobbyMemberJoined : plPlatformEvent::Type::LobbyMemberLeft;
  e.m_Lobby = m_CurrentLobbyInfo.m_Id;
  e.m_User = MakeUserId(changed);
  Emit(e);

  const CSteamID owner = SteamMatchmaking()->GetLobbyOwner(lobby);
  if (owner.IsValid() && owner != previousOwner)
  {
    plPlatformEvent ownerEvent;
    ownerEvent.m_Type = plPlatformEvent::Type::LobbyOwnerChanged;
    ownerEvent.m_Lobby = m_CurrentLobbyInfo.m_Id;
    ownerEvent.m_User = MakeUserId(owner);
    Emit(ownerEvent);
  }
}

void plSteamPlatformBackend::OnLobbyDataUpdate(LobbyDataUpdate_t* pParam)
{
  const CSteamID lobby(pParam->m_ulSteamIDLobby);
  if (!m_CurrentLobby.IsValid() || lobby != m_CurrentLobby || pParam->m_bSuccess == 0)
    return;

  const CSteamID previousOwner = ParseUserId(m_CurrentLobbyInfo.m_Owner);
  RefreshCurrentLobby();

  // Both lobby data and member data changes land here; the info is refreshed either way.
  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::LobbyUpdated;
  e.m_Lobby = m_CurrentLobbyInfo.m_Id;
  Emit(e);

  const CSteamID owner = ParseUserId(m_CurrentLobbyInfo.m_Owner);
  if (owner.IsValid() && previousOwner.IsValid() && owner != previousOwner)
  {
    plPlatformEvent ownerEvent;
    ownerEvent.m_Type = plPlatformEvent::Type::LobbyOwnerChanged;
    ownerEvent.m_Lobby = m_CurrentLobbyInfo.m_Id;
    ownerEvent.m_User = MakeUserId(owner);
    Emit(ownerEvent);
  }
}

void plSteamPlatformBackend::OnLobbyChatMsg(LobbyChatMsg_t* pParam)
{
  const CSteamID lobby(pParam->m_ulSteamIDLobby);
  if (!m_CurrentLobby.IsValid() || lobby != m_CurrentLobby)
    return;

  constexpr int iBufferSize = 4096;
  char szBuffer[iBufferSize] = {};
  CSteamID sender;
  EChatEntryType type = k_EChatEntryTypeInvalid;
  const int iSize = SteamMatchmaking()->GetLobbyChatEntry(lobby, static_cast<int>(pParam->m_iChatID), &sender, szBuffer, iBufferSize - 1, &type);
  if (iSize <= 0 || type != k_EChatEntryTypeChatMsg)
    return;

  szBuffer[plMath::Min(iSize, iBufferSize - 1)] = '\0';

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::LobbyChatMessage;
  e.m_Lobby = m_CurrentLobbyInfo.m_Id;
  e.m_User = MakeUserId(sender);
  e.m_sText = szBuffer;
  Emit(e);
}

void plSteamPlatformBackend::OnLobbyKicked(LobbyKicked_t* pParam)
{
  const CSteamID lobby(pParam->m_ulSteamIDLobby);
  if (!m_CurrentLobby.IsValid() || lobby != m_CurrentLobby)
    return;

  m_CurrentLobby = CSteamID();
  RefreshCurrentLobby();

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::LobbyLeft;
  e.m_Lobby = MakeLobbyId(lobby);
  Emit(e);
}

void plSteamPlatformBackend::OnGameLobbyJoinRequested(GameLobbyJoinRequested_t* pParam)
{
  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::JoinRequested;
  e.m_Lobby = MakeLobbyId(pParam->m_steamIDLobby);
  e.m_User = MakeUserId(pParam->m_steamIDFriend);
  Emit(e);
}

void plSteamPlatformBackend::OnGameRichPresenceJoinRequested(GameRichPresenceJoinRequested_t* pParam)
{
  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::JoinRequested;
  e.m_User = MakeUserId(pParam->m_steamIDFriend);
  e.m_sText = pParam->m_rgchConnect;
  Emit(e);
}
