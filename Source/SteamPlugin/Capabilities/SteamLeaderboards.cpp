#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/Math/Math.h>
#include <SteamPlugin/SteamPlatformBackend.h>

// Every leaderboard call starts with FindLeaderboard unless the handle is cached; the found handle is then fed to
// the upload or download call with the same context and operation.

plPlatformOperationPtr plSteamPlatformBackend::UploadScore(plStringView sBackendId, plInt64 iScore, plPlatformLeaderboardUpload method, plArrayPtr<const plInt32> details)
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Leaderboards.UploadScore");

  if (sBackendId.IsEmpty())
  {
    pOp->Complete(plPlatformError::InvalidArgument);
    return pOp;
  }

  if (!IsSignedIn())
  {
    pOp->Complete(plPlatformError::NotSignedIn);
    return pOp;
  }

  auto* pCall = NewCall<LeaderboardFindResult_t, LeaderboardUploadContext>(pOp);
  pCall->m_Context.m_sId = sBackendId;
  pCall->m_Context.m_iScore = iScore;
  pCall->m_Context.m_Method = method;
  pCall->m_Context.m_Details = details;

  SteamLeaderboard_t hLeaderboard = 0;
  if (m_LeaderboardHandles.TryGetValue(pCall->m_Context.m_sId, hLeaderboard))
  {
    StartUpload(hLeaderboard, *pCall);
    pCall->m_bDone = true;
    return pOp;
  }

  pCall->Start(this, SteamUserStats()->FindLeaderboard(pCall->m_Context.m_sId.GetData()), &plSteamPlatformBackend::OnLeaderboardFoundForUpload);
  return pOp;
}

plPlatformOperationPtr plSteamPlatformBackend::Read(plStringView sBackendId, const plPlatformLeaderboardQuery& query)
{
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Leaderboards.Read");

  if (sBackendId.IsEmpty())
  {
    pOp->Complete(plPlatformError::InvalidArgument);
    return pOp;
  }

  if (!IsSignedIn())
  {
    pOp->Complete(plPlatformError::NotSignedIn);
    return pOp;
  }

  auto* pCall = NewCall<LeaderboardFindResult_t, LeaderboardReadContext>(pOp);
  pCall->m_Context.m_sId = sBackendId;
  pCall->m_Context.m_Query = query;

  SteamLeaderboard_t hLeaderboard = 0;
  if (m_LeaderboardHandles.TryGetValue(pCall->m_Context.m_sId, hLeaderboard))
  {
    StartDownload(hLeaderboard, *pCall);
    pCall->m_bDone = true;
    return pOp;
  }

  pCall->Start(this, SteamUserStats()->FindLeaderboard(pCall->m_Context.m_sId.GetData()), &plSteamPlatformBackend::OnLeaderboardFoundForRead);
  return pOp;
}

void plSteamPlatformBackend::StartUpload(SteamLeaderboard_t hLeaderboard, plSteamCallResult<LeaderboardFindResult_t, LeaderboardUploadContext>& ref_call)
{
  const LeaderboardUploadContext& ctx = ref_call.m_Context;

  const ELeaderboardUploadScoreMethod method = ctx.m_Method == plPlatformLeaderboardUpload::ForceUpdate ? k_ELeaderboardUploadScoreMethodForceUpdate : k_ELeaderboardUploadScoreMethodKeepBest;

  // Steam scores are 32 bit.
  const plInt32 iScore = static_cast<plInt32>(plMath::Clamp<plInt64>(ctx.m_iScore, plMath::MinValue<plInt32>(), plMath::MaxValue<plInt32>()));
  const int iDetailCount = static_cast<int>(plMath::Min<plUInt32>(ctx.m_Details.GetCount(), k_cLeaderboardDetailsMax));

  auto* pUpload = NewCall<LeaderboardScoreUploaded_t, LeaderboardUploadContext>(ref_call.m_pOp);
  pUpload->m_Context = ctx;

  const SteamAPICall_t hCall = SteamUserStats()->UploadLeaderboardScore(hLeaderboard, method, iScore, iDetailCount > 0 ? ctx.m_Details.GetData() : nullptr, iDetailCount);
  pUpload->Start(this, hCall, &plSteamPlatformBackend::OnLeaderboardScoreUploaded);
}

void plSteamPlatformBackend::StartDownload(SteamLeaderboard_t hLeaderboard, plSteamCallResult<LeaderboardFindResult_t, LeaderboardReadContext>& ref_call)
{
  const plPlatformLeaderboardQuery& query = ref_call.m_Context.m_Query;

  ELeaderboardDataRequest request = k_ELeaderboardDataRequestGlobal;
  int iStart = query.m_iRangeStart;
  int iEnd = query.m_iRangeEnd;

  switch (query.m_Window)
  {
    case plPlatformLeaderboardWindow::Friends:
      request = k_ELeaderboardDataRequestFriends;
      break;
    case plPlatformLeaderboardWindow::AroundUser:
      // Steam takes the window as offsets from the user's own rank: negative before, positive after.
      request = k_ELeaderboardDataRequestGlobalAroundUser;
      iStart = -plMath::Abs(query.m_iRangeStart);
      iEnd = plMath::Abs(query.m_iRangeEnd);
      break;
    case plPlatformLeaderboardWindow::Global:
    default:
      break;
  }

  auto* pDownload = NewCall<LeaderboardScoresDownloaded_t, LeaderboardReadContext>(ref_call.m_pOp);
  pDownload->m_Context = ref_call.m_Context;

  const SteamAPICall_t hCall = SteamUserStats()->DownloadLeaderboardEntries(hLeaderboard, request, iStart, iEnd);
  pDownload->Start(this, hCall, &plSteamPlatformBackend::OnLeaderboardScoresDownloaded);
}

void plSteamPlatformBackend::OnLeaderboardFoundForUpload(plSteamCallResult<LeaderboardFindResult_t, LeaderboardUploadContext>& ref_call, LeaderboardFindResult_t* pResult, bool bIOFailure)
{
  if (bIOFailure || pResult == nullptr || pResult->m_bLeaderboardFound == 0)
  {
    ref_call.m_pOp->Complete(bIOFailure ? plPlatformError::Offline : plPlatformError::NotFound);
    return;
  }

  m_LeaderboardHandles[ref_call.m_Context.m_sId] = pResult->m_hSteamLeaderboard;
  StartUpload(pResult->m_hSteamLeaderboard, ref_call);
}

void plSteamPlatformBackend::OnLeaderboardFoundForRead(plSteamCallResult<LeaderboardFindResult_t, LeaderboardReadContext>& ref_call, LeaderboardFindResult_t* pResult, bool bIOFailure)
{
  if (bIOFailure || pResult == nullptr || pResult->m_bLeaderboardFound == 0)
  {
    ref_call.m_pOp->Complete(bIOFailure ? plPlatformError::Offline : plPlatformError::NotFound);
    return;
  }

  m_LeaderboardHandles[ref_call.m_Context.m_sId] = pResult->m_hSteamLeaderboard;
  StartDownload(pResult->m_hSteamLeaderboard, ref_call);
}

void plSteamPlatformBackend::OnLeaderboardScoreUploaded(plSteamCallResult<LeaderboardScoreUploaded_t, LeaderboardUploadContext>& ref_call, LeaderboardScoreUploaded_t* pResult, bool bIOFailure)
{
  if (bIOFailure || pResult == nullptr || pResult->m_bSuccess == 0)
  {
    ref_call.m_pOp->Complete(bIOFailure ? plPlatformError::Offline : plPlatformError::BackendFailure);
    return;
  }

  plPlatformResult& result = ref_call.m_pOp->MutableResult();
  result.m_iNumber = pResult->m_nGlobalRankNew;
  result.m_bFlag = pResult->m_bScoreChanged != 0;
  ref_call.m_pOp->Complete(plPlatformError::Success);
}

void plSteamPlatformBackend::OnLeaderboardScoresDownloaded(plSteamCallResult<LeaderboardScoresDownloaded_t, LeaderboardReadContext>& ref_call, LeaderboardScoresDownloaded_t* pResult, bool bIOFailure)
{
  if (bIOFailure || pResult == nullptr)
  {
    ref_call.m_pOp->Complete(plPlatformError::Offline);
    return;
  }

  plDynamicArray<plPlatformLeaderboardEntry>& entries = ref_call.m_pOp->MutableResult().m_LeaderboardEntries;
  entries.Reserve(static_cast<plUInt32>(plMath::Max(pResult->m_cEntryCount, 0)));

  for (int i = 0; i < pResult->m_cEntryCount; ++i)
  {
    LeaderboardEntry_t steamEntry;
    plInt32 details[k_cLeaderboardDetailsMax] = {};
    if (!SteamUserStats()->GetDownloadedLeaderboardEntry(pResult->m_hSteamLeaderboardEntries, i, &steamEntry, details, k_cLeaderboardDetailsMax))
      continue;

    plPlatformLeaderboardEntry& entry = entries.ExpandAndGetRef();
    entry.m_User = MakeUserId(steamEntry.m_steamIDUser);
    entry.m_sDisplayName = SteamFriends()->GetFriendPersonaName(steamEntry.m_steamIDUser);
    entry.m_iRank = steamEntry.m_nGlobalRank;
    entry.m_iScore = steamEntry.m_nScore;

    const plInt32 iDetailCount = plMath::Clamp<plInt32>(steamEntry.m_cDetails, 0, k_cLeaderboardDetailsMax);
    for (plInt32 d = 0; d < iDetailCount; ++d)
    {
      entry.m_Details.PushBack(details[d]);
    }
  }

  ref_call.m_pOp->Complete(plPlatformError::Success);
}
