#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/Utilities/ConversionUtils.h>
#include <SteamPlugin/SteamPlatformBackend.h>

plResult plSteamPlatformBackend::Show(plPlatformOverlayPage page)
{
  if (!m_bInitialized || SteamUtils() == nullptr || !SteamUtils()->IsOverlayEnabled())
    return PL_FAILURE;

  switch (page)
  {
    case plPlatformOverlayPage::Store:
      SteamFriends()->ActivateGameOverlayToStore(SteamUtils()->GetAppID(), k_EOverlayToStoreFlag_None);
      return PL_SUCCESS;

    case plPlatformOverlayPage::Profile:
      SteamFriends()->ActivateGameOverlayToUser("steamid", SteamUser()->GetSteamID());
      return PL_SUCCESS;

    case plPlatformOverlayPage::InviteDialog:
      return ShowInviteDialog(plPlatformLobbyId());

    case plPlatformOverlayPage::Community:
      SteamFriends()->ActivateGameOverlay("Community");
      return PL_SUCCESS;

    case plPlatformOverlayPage::Players:
      SteamFriends()->ActivateGameOverlay("Players");
      return PL_SUCCESS;

    case plPlatformOverlayPage::Settings:
      SteamFriends()->ActivateGameOverlay("Settings");
      return PL_SUCCESS;

    case plPlatformOverlayPage::Achievements:
      SteamFriends()->ActivateGameOverlay("Achievements");
      return PL_SUCCESS;

    case plPlatformOverlayPage::Friends:
    case plPlatformOverlayPage::Default:
    default:
      SteamFriends()->ActivateGameOverlay("Friends");
      return PL_SUCCESS;
  }
}

plResult plSteamPlatformBackend::ShowUser(const plPlatformUserId& user)
{
  const CSteamID id = ParseUserId(user);
  if (!m_bInitialized || !id.IsValid() || !SteamUtils()->IsOverlayEnabled())
    return PL_FAILURE;

  SteamFriends()->ActivateGameOverlayToUser("steamid", id);
  return PL_SUCCESS;
}

plResult plSteamPlatformBackend::ShowInviteDialog(const plPlatformLobbyId& lobby)
{
  if (!m_bInitialized || !SteamUtils()->IsOverlayEnabled())
    return PL_FAILURE;

  CSteamID lobbyId = ParseLobbyId(lobby);
  if (!lobbyId.IsValid())
    lobbyId = m_CurrentLobby;

  if (!lobbyId.IsValid())
    return PL_FAILURE;

  SteamFriends()->ActivateGameOverlayInviteDialog(lobbyId);
  return PL_SUCCESS;
}

plResult plSteamPlatformBackend::ShowStore(plStringView sDlcBackendId)
{
  if (!m_bInitialized || !SteamUtils()->IsOverlayEnabled())
    return PL_FAILURE;

  // An empty or unparsable id opens the game's own store page.
  plUInt32 uiAppId = 0;
  if (sDlcBackendId.IsEmpty() || plConversionUtils::StringToUInt(sDlcBackendId, uiAppId).Failed() || uiAppId == 0)
    uiAppId = SteamUtils()->GetAppID();

  SteamFriends()->ActivateGameOverlayToStore(static_cast<AppId_t>(uiAppId), k_EOverlayToStoreFlag_None);
  return PL_SUCCESS;
}

void plSteamPlatformBackend::OnGameOverlayActivated(GameOverlayActivated_t* pParam)
{
  m_bOverlayVisible = pParam->m_bActive != 0;

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::OverlayToggled;
  e.m_bFlag = m_bOverlayVisible;
  Emit(e);
}
