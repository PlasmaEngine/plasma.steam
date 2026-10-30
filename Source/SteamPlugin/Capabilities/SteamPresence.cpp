#include <SteamPlugin/SteamPluginPCH.h>

#include <SteamPlugin/SteamPlatformBackend.h>

// Rich presence is a key/value store on the Steam client; the hub caches and re-publishes it, so nothing is kept here.

plResult plSteamPlatformBackend::Set(plStringView sKey, plStringView sValue)
{
  if (!m_bInitialized || sKey.IsEmpty())
    return PL_FAILURE;

  const plString sKeyCopy(sKey);
  const plString sValueCopy(sValue);
  return SteamFriends()->SetRichPresence(sKeyCopy.GetData(), sValueCopy.IsEmpty() ? nullptr : sValueCopy.GetData()) ? PL_SUCCESS : PL_FAILURE;
}

plResult plSteamPlatformBackend::SetJoinString(plStringView sConnect)
{
  // "connect" is the key Steam shows a join button for; a friend accepting it receives GameRichPresenceJoinRequested_t.
  return Set("connect", sConnect);
}

void plSteamPlatformBackend::Clear()
{
  if (m_bInitialized)
  {
    SteamFriends()->ClearRichPresence();
  }
}
