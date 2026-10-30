#include <SteamPlugin/SteamPluginPCH.h>

#include <SteamPlugin/SteamPlatformBackend.h>

plPlatformOperationPtr plSteamPlatformBackend::QueryFriends()
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Friends.Query");

  if (!IsSignedIn())
  {
    pOp->Complete(plPlatformError::NotSignedIn);
    return pOp;
  }

  // The friend list is local to the Steam client; no round trip is needed.
  plDynamicArray<plPlatformUser>& users = pOp->MutableResult().m_Users;
  const int iCount = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
  users.Reserve(static_cast<plUInt32>(plMath::Max(iCount, 0)));

  for (int i = 0; i < iCount; ++i)
  {
    const CSteamID id = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
    if (!id.IsValid())
      continue;

    users.PushBack(DescribeUser(id));
  }

  pOp->Complete(plPlatformError::Success);
  return pOp;
}

bool plSteamPlatformBackend::IsBlocked(const plPlatformUserId& user) const
{
  if (!m_bInitialized || SteamFriends() == nullptr)
    return false;

  const CSteamID id = ParseUserId(user);
  if (!id.IsValid())
    return false;

  return SteamFriends()->HasFriend(id, k_EFriendFlagBlocked) || SteamFriends()->HasFriend(id, k_EFriendFlagIgnored);
}
