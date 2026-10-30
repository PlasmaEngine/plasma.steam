#include <SteamPlugin/SteamPluginPCH.h>

#include <SteamPlugin/SteamPlatformBackend.h>
#include <GameEngine/PlatformServices/PlatformDefinitions.h>

// Steam serves achievements and stats out of a per-user cache that is only valid after UserStatsReceived_t. Every
// request goes through SubmitStatsRequest, which runs it now or holds it until that callback.

plPlatformOperationPtr plSteamPlatformBackend::Unlock(plStringView sBackendId)
{
  StatsRequest request;
  request.m_Kind = StatsRequest::Kind::Unlock;
  request.m_sId = sBackendId;
  request.m_pOp = plPlatformOperation::Create("Achievements.Unlock");
  return SubmitStatsRequest(std::move(request));
}

plPlatformOperationPtr plSteamPlatformBackend::SetProgress(plStringView sBackendId, plUInt32 uiCurrent, plUInt32 uiTarget)
{
  if (sBackendId.IsEmpty() || uiTarget == 0)
    return plPlatformOperation::CreateFinished("Achievements.SetProgress", plPlatformError::InvalidArgument);

  // Shows the progress toast; it never unlocks, the hub calls Unlock for that. Progress at or past the target is a
  // no-op for Steam, so it answers success without a toast.
  if (uiCurrent >= uiTarget)
    return plPlatformOperation::CreateFinished("Achievements.SetProgress", plPlatformError::Success);

  const plString sId(sBackendId);
  const bool bOk = SteamUserStats()->IndicateAchievementProgress(sId.GetData(), uiCurrent, uiTarget);
  return plPlatformOperation::CreateFinished("Achievements.SetProgress", bOk ? plPlatformError::Success : plPlatformError::BackendFailure);
}

plPlatformOperationPtr plSteamPlatformBackend::QueryAchievements()
{
  StatsRequest request;
  request.m_Kind = StatsRequest::Kind::QueryAchievements;
  request.m_pOp = plPlatformOperation::Create("Achievements.Query");
  return SubmitStatsRequest(std::move(request));
}

plPlatformOperationPtr plSteamPlatformBackend::Reset(plStringView sBackendId)
{
  StatsRequest request;
  request.m_Kind = StatsRequest::Kind::Reset;
  request.m_sId = sBackendId;
  request.m_pOp = plPlatformOperation::Create("Achievements.Reset");
  return SubmitStatsRequest(std::move(request));
}

plPlatformOperationPtr plSteamPlatformBackend::SubmitStatsRequest(StatsRequest&& request)
{
  plPlatformOperationPtr pOp = request.m_pOp;

  if (!IsSignedIn())
  {
    pOp->Complete(plPlatformError::NotSignedIn);
    return pOp;
  }

  if (m_bStatsReceived)
  {
    RunStatsRequest(request);
  }
  else
  {
    m_QueuedStatsRequests.PushBack(std::move(request));
  }

  return pOp;
}

void plSteamPlatformBackend::RunStatsRequest(StatsRequest& ref_request)
{
  ISteamUserStats* pStats = SteamUserStats();
  plPlatformOperation& op = *ref_request.m_pOp;
  const plString sId = ref_request.m_sId;

  switch (ref_request.m_Kind)
  {
    case StatsRequest::Kind::Unlock:
    {
      if (sId.IsEmpty())
      {
        op.Complete(plPlatformError::InvalidArgument);
        return;
      }

      bool bAchieved = false;
      if (!pStats->GetAchievement(sId.GetData(), &bAchieved))
      {
        op.Complete(plPlatformError::NotFound);
        return;
      }

      if (bAchieved)
      {
        op.Complete(plPlatformError::Conflict);
        return;
      }

      if (!pStats->SetAchievement(sId.GetData()) || !pStats->StoreStats())
      {
        op.Complete(plPlatformError::BackendFailure);
        return;
      }

      m_StoreWaiters.PushBack(ref_request.m_pOp);
      return;
    }

    case StatsRequest::Kind::Reset:
    {
      if (sId.IsEmpty() || !pStats->ClearAchievement(sId.GetData()) || !pStats->StoreStats())
      {
        op.Complete(sId.IsEmpty() ? plPlatformError::InvalidArgument : plPlatformError::BackendFailure);
        return;
      }

      m_StoreWaiters.PushBack(ref_request.m_pOp);
      return;
    }

    case StatsRequest::Kind::QueryAchievements:
    {
      plDynamicArray<plPlatformAchievementState>& states = op.MutableResult().m_Achievements;
      const plUInt32 uiCount = pStats->GetNumAchievements();
      states.Reserve(uiCount);

      for (plUInt32 i = 0; i < uiCount; ++i)
      {
        const char* szName = pStats->GetAchievementName(i);
        if (szName == nullptr || szName[0] == '\0')
          continue;

        bool bAchieved = false;
        plUInt32 uiUnlockTime = 0;
        pStats->GetAchievementAndUnlockTime(szName, &bAchieved, &uiUnlockTime);

        plPlatformAchievementState& state = states.ExpandAndGetRef();
        state.m_sId = szName;
        state.m_bUnlocked = bAchieved;
        state.m_iUnlockTimeUtcMicroseconds = bAchieved ? static_cast<plInt64>(uiUnlockTime) * 1000000 : 0;
      }

      op.Complete(plPlatformError::Success);
      return;
    }

    case StatsRequest::Kind::StoreStats:
    {
      for (const plPlatformStatValue& value : ref_request.m_Values)
      {
        if (value.m_sId.IsEmpty())
          continue;

        bool bOk = false;
        if (value.m_Value.IsFloatingPoint())
          bOk = pStats->SetStat(value.m_sId.GetData(), value.m_Value.ConvertTo<float>());
        else
          bOk = pStats->SetStat(value.m_sId.GetData(), value.m_Value.ConvertTo<plInt32>());

        if (!bOk)
        {
          plLog::Warning("Steam: SetStat('{0}') refused; the stat is not defined for this app or has another type.", value.m_sId);
        }
      }

      if (!pStats->StoreStats())
      {
        op.Complete(plPlatformError::BackendFailure);
        return;
      }

      m_StoreWaiters.PushBack(ref_request.m_pOp);
      return;
    }

    case StatsRequest::Kind::QueryStats:
    {
      plDynamicArray<plPlatformStatValue>& values = op.MutableResult().m_Stats;

      if (m_pHost != nullptr)
      {
        for (const plPlatformStatDef& def : m_pHost->GetDefinitions().m_Stats)
        {
          const plString sBackendId(def.m_Backend.Resolve("steam", def.m_sId));
          if (sBackendId.IsEmpty())
            continue;

          plPlatformStatValue value;
          value.m_sId = sBackendId;

          if (def.m_Type == plPlatformStatType::Float)
          {
            float fValue = 0.0f;
            if (!pStats->GetStat(sBackendId.GetData(), &fValue))
              continue;
            value.m_Value = static_cast<double>(fValue);
          }
          else
          {
            plInt32 iValue = 0;
            if (!pStats->GetStat(sBackendId.GetData(), &iValue))
              continue;
            value.m_Value = static_cast<plInt64>(iValue);
          }

          values.PushBack(value);
        }
      }

      op.Complete(plPlatformError::Success);
      return;
    }
  }

  op.Complete(plPlatformError::InvalidArgument);
}

static bool IsOwnGame(plUInt64 uiGameId)
{
  return SteamUtils() == nullptr || CGameID(SteamUtils()->GetAppID()).ToUint64() == uiGameId;
}

void plSteamPlatformBackend::OnUserStatsReceived(UserStatsReceived_t* pParam)
{
  if (!IsOwnGame(pParam->m_nGameID))
    return;

  if (pParam->m_eResult != k_EResultOK)
  {
    plLog::Warning("Steam: stats could not be received ({0}); queued achievement and stat requests fail.", (int)pParam->m_eResult);

    const plPlatformError error = MapResult(pParam->m_eResult);
    for (StatsRequest& request : m_QueuedStatsRequests)
      request.m_pOp->Complete(error == plPlatformError::Success ? plPlatformError::BackendFailure : error);
    m_QueuedStatsRequests.Clear();
    return;
  }

  m_bStatsReceived = true;

  plDynamicArray<StatsRequest> queued = std::move(m_QueuedStatsRequests);
  m_QueuedStatsRequests.Clear();
  for (StatsRequest& request : queued)
  {
    RunStatsRequest(request);
  }
}

void plSteamPlatformBackend::OnUserStatsStored(UserStatsStored_t* pParam)
{
  if (!IsOwnGame(pParam->m_nGameID))
    return;

  // StoreStats calls and their UserStatsStored_t answers arrive in order, so the oldest waiter is the one answered.
  if (m_StoreWaiters.IsEmpty())
    return;

  plPlatformOperationPtr pOp = m_StoreWaiters.PeekFront();
  m_StoreWaiters.PopFront();

  // k_EResultInvalidParam means a stat broke a server-side constraint; the other values were still stored.
  const plPlatformError error = pParam->m_eResult == k_EResultInvalidParam ? plPlatformError::InvalidArgument : MapResult(pParam->m_eResult);
  pOp->Complete(error);
}

void plSteamPlatformBackend::OnUserAchievementStored(UserAchievementStored_t* pParam)
{
  if (!IsOwnGame(pParam->m_nGameID))
    return;

  plPlatformEvent e;
  e.m_sId = pParam->m_rgchAchievementName;

  if (pParam->m_nMaxProgress == 0)
  {
    e.m_Type = plPlatformEvent::Type::AchievementUnlocked;
  }
  else
  {
    e.m_Type = plPlatformEvent::Type::AchievementProgress;
    e.m_Value = pParam->m_nCurProgress;
    e.m_uiExtra = pParam->m_nMaxProgress;
  }

  Emit(e);
}
