#include <SteamPlugin/SteamPluginPCH.h>

#include <SteamPlugin/SteamPlatformBackend.h>

plPlatformOperationPtr plSteamPlatformBackend::Invite(const plPlatformUserId& user, const plPlatformLobbyId& lobby)
{
  if (!IsSignedIn())
    return plPlatformOperation::CreateFinished("Sessions.Invite", plPlatformError::NotSignedIn);

  const CSteamID userId = ParseUserId(user);
  CSteamID lobbyId = ParseLobbyId(lobby);
  if (!lobbyId.IsValid())
    lobbyId = m_CurrentLobby;

  if (!userId.IsValid() || !lobbyId.IsValid())
    return plPlatformOperation::CreateFinished("Sessions.Invite", plPlatformError::InvalidArgument);

  // The invitee gets GameLobbyJoinRequested_t if the game is running, or a launch with +connect_lobby if not.
  const bool bOk = SteamMatchmaking()->InviteUserToLobby(lobbyId, userId);
  return plPlatformOperation::CreateFinished("Sessions.Invite", bOk ? plPlatformError::Success : plPlatformError::BackendFailure);
}

bool plSteamPlatformBackend::TakeLaunchJoinRequest(plPlatformJoinRequest& out_request)
{
  if (!m_bLaunchJoin)
    return false;

  out_request = m_LaunchJoin;
  m_LaunchJoin = plPlatformJoinRequest();
  m_bLaunchJoin = false;
  return true;
}
