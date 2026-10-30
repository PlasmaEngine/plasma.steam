#include <SteamPlugin/SteamPluginPCH.h>

#include <SteamPlugin/SteamPlatformBackend.h>

// Stats share the request queue with achievements (SteamAchievements.cpp): both live in the same Steam cache and
// need UserStatsReceived_t before they can be touched.

plPlatformOperationPtr plSteamPlatformBackend::Store(plArrayPtr<const plPlatformStatValue> values)
{
  StatsRequest request;
  request.m_Kind = StatsRequest::Kind::StoreStats;
  request.m_Values = values;
  request.m_pOp = plPlatformOperation::Create("Stats.Store");
  return SubmitStatsRequest(std::move(request));
}

plPlatformOperationPtr plSteamPlatformBackend::QueryStats()
{
  StatsRequest request;
  request.m_Kind = StatsRequest::Kind::QueryStats;
  request.m_pOp = plPlatformOperation::Create("Stats.Query");
  return SubmitStatsRequest(std::move(request));
}
