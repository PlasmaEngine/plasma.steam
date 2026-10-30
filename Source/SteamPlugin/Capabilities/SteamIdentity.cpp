#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <SteamPlugin/SteamPlatformBackend.h>
#include <Texture/Image/Formats/ImageFileFormat.h>
#include <Texture/Image/Image.h>

static plPlatformPresenceStatus ToPresenceStatus(EPersonaState state)
{
  switch (state)
  {
    case k_EPersonaStateOffline:
    case k_EPersonaStateInvisible:
      return plPlatformPresenceStatus::Offline;
    case k_EPersonaStateBusy:
      return plPlatformPresenceStatus::Busy;
    case k_EPersonaStateAway:
    case k_EPersonaStateSnooze:
      return plPlatformPresenceStatus::Away;
    default:
      return plPlatformPresenceStatus::Online;
  }
}

bool plSteamPlatformBackend::IsSignedIn() const
{
  return m_bInitialized && SteamUser() != nullptr && SteamUser()->BLoggedOn();
}

plPlatformOperationPtr plSteamPlatformBackend::SignIn()
{
  // Steam signs the user in when the client starts; there is nothing to start here.
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Identity.SignIn");
  if (IsSignedIn())
  {
    RefreshLocalUser();
    pOp->MutableResult().m_User = m_LocalUser;
    pOp->Complete(plPlatformError::Success);
  }
  else
  {
    pOp->Complete(plPlatformError::NotSignedIn);
  }
  return pOp;
}

plPlatformOperationPtr plSteamPlatformBackend::SignOut()
{
  return plPlatformOperation::CreateFinished("Identity.SignOut", plPlatformError::Unsupported);
}

plPlatformOperationPtr plSteamPlatformBackend::QueryUser(const plPlatformUserId& user)
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Identity.QueryUser");

  const CSteamID id = ParseUserId(user);
  if (!id.IsValid())
  {
    pOp->Complete(plPlatformError::InvalidArgument);
    return pOp;
  }

  // false = the name is already cached; true = Steam fetches it and PersonaStateChange_t follows.
  if (!SteamFriends()->RequestUserInformation(id, false))
  {
    pOp->MutableResult().m_User = DescribeUser(id);
    pOp->Complete(plPlatformError::Success);
    return pOp;
  }

  PendingUserQuery& query = m_PendingUserQueries.ExpandAndGetRef();
  query.m_Id = id;
  query.m_pOp = pOp;
  return pOp;
}

plPlatformOperationPtr plSteamPlatformBackend::QueryAvatar(const plPlatformUserId& user)
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Identity.QueryAvatar");

  const CSteamID id = ParseUserId(user);
  if (!id.IsValid())
  {
    pOp->Complete(plPlatformError::InvalidArgument);
    return pOp;
  }

  // 0 = no avatar, -1 = not cached yet (AvatarImageLoaded_t follows), otherwise an image handle.
  const int iImage = SteamFriends()->GetMediumFriendAvatar(id);
  if (iImage == 0)
  {
    pOp->Complete(plPlatformError::NotFound);
    return pOp;
  }

  if (iImage == -1)
  {
    PendingUserQuery& query = m_PendingAvatarQueries.ExpandAndGetRef();
    query.m_Id = id;
    query.m_pOp = pOp;
    return pOp;
  }

  if (EncodeAvatar(iImage, pOp->MutableResult().m_Bytes))
    pOp->Complete(plPlatformError::Success);
  else
    pOp->Complete(plPlatformError::BackendFailure);

  return pOp;
}

plPlatformUser plSteamPlatformBackend::DescribeUser(CSteamID id) const
{
  plPlatformUser user;
  user.m_Id = MakeUserId(id);

  if (SteamFriends() == nullptr)
    return user;

  user.m_sDisplayName = SteamFriends()->GetFriendPersonaName(id);
  user.m_Status = ToPresenceStatus(SteamFriends()->GetFriendPersonaState(id));

  FriendGameInfo_t gameInfo;
  if (SteamFriends()->GetFriendGamePlayed(id, &gameInfo))
  {
    user.m_bInSameGame = SteamUtils() != nullptr && gameInfo.m_gameID.AppID() == SteamUtils()->GetAppID();
    if (user.m_Status != plPlatformPresenceStatus::Offline)
    {
      user.m_Status = plPlatformPresenceStatus::InGame;
    }
  }

  return user;
}

bool plSteamPlatformBackend::EncodeAvatar(int iImage, plDynamicArray<plUInt8>& out_png) const
{
  plUInt32 uiWidth = 0, uiHeight = 0;
  if (SteamUtils() == nullptr || !SteamUtils()->GetImageSize(iImage, &uiWidth, &uiHeight) || uiWidth == 0 || uiHeight == 0)
    return false;

  plImageHeader header;
  header.SetWidth(uiWidth);
  header.SetHeight(uiHeight);
  header.SetImageFormat(plImageFormat::R8G8B8A8_UNORM);

  plImage image;
  image.ResetAndAlloc(header);

  const int iBufferSize = static_cast<int>(uiWidth * uiHeight * 4);
  if (!SteamUtils()->GetImageRGBA(iImage, image.GetPixelPointer<plUInt8>(), iBufferSize))
    return false;

  const plImageFileFormat* pFormat = plImageFileFormat::GetWriterFormat("png");
  if (pFormat == nullptr)
    return false;

  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  if (pFormat->WriteImage(writer, image, "png").Failed())
    return false;

  out_png.Clear();
  out_png.Reserve(storage.GetStorageSize32());

  plUInt64 uiOffset = 0;
  while (uiOffset < storage.GetStorageSize64())
  {
    const plArrayPtr<const plUInt8> range = storage.GetContiguousMemoryRange(uiOffset);
    if (range.IsEmpty())
      break;

    out_png.PushBackRange(range);
    uiOffset += range.GetCount();
  }

  return !out_png.IsEmpty();
}

void plSteamPlatformBackend::OnPersonaStateChange(PersonaStateChange_t* pParam)
{
  const CSteamID id(pParam->m_ulSteamID);

  if (id == SteamUser()->GetSteamID())
  {
    RefreshLocalUser();
  }

  for (plUInt32 i = 0; i < m_PendingUserQueries.GetCount();)
  {
    if (m_PendingUserQueries[i].m_Id == id)
    {
      plPlatformOperationPtr pOp = m_PendingUserQueries[i].m_pOp;
      m_PendingUserQueries.RemoveAtAndSwap(i);
      pOp->MutableResult().m_User = DescribeUser(id);
      pOp->Complete(plPlatformError::Success);
    }
    else
    {
      ++i;
    }
  }

  if ((pParam->m_nChangeFlags & k_EPersonaChangeRelationshipChanged) != 0)
  {
    plPlatformEvent e;
    e.m_Type = plPlatformEvent::Type::FriendsChanged;
    Emit(e);
  }

  if (SteamFriends()->HasFriend(id, k_EFriendFlagImmediate))
  {
    plPlatformEvent e;
    e.m_Type = plPlatformEvent::Type::PresenceChanged;
    e.m_User = MakeUserId(id);
    Emit(e);
  }
}

void plSteamPlatformBackend::OnAvatarImageLoaded(AvatarImageLoaded_t* pParam)
{
  const CSteamID id = pParam->m_steamID;

  for (plUInt32 i = 0; i < m_PendingAvatarQueries.GetCount();)
  {
    if (m_PendingAvatarQueries[i].m_Id != id)
    {
      ++i;
      continue;
    }

    plPlatformOperationPtr pOp = m_PendingAvatarQueries[i].m_pOp;
    m_PendingAvatarQueries.RemoveAtAndSwap(i);

    const int iImage = SteamFriends()->GetMediumFriendAvatar(id);
    if (iImage > 0 && EncodeAvatar(iImage, pOp->MutableResult().m_Bytes))
      pOp->Complete(plPlatformError::Success);
    else
      pOp->Complete(iImage == 0 ? plPlatformError::NotFound : plPlatformError::BackendFailure);
  }
}
