#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/Utilities/ConversionUtils.h>
#include <SteamPlugin/SteamPlatformBackend.h>

// The backend id of a DLC is its decimal Steam app id.

static bool ParseAppId(plStringView sBackendId, AppId_t& out_appId)
{
  plUInt32 uiValue = 0;
  if (plConversionUtils::StringToUInt(sBackendId, uiValue).Failed() || uiValue == 0)
    return false;

  out_appId = static_cast<AppId_t>(uiValue);
  return true;
}

bool plSteamPlatformBackend::OwnsApp() const
{
  return m_bInitialized && SteamApps() != nullptr && SteamApps()->BIsSubscribed();
}

bool plSteamPlatformBackend::OwnsDlc(plStringView sBackendId) const
{
  if (!m_bInitialized || SteamApps() == nullptr)
    return false;

  AppId_t appId = 0;
  if (!ParseAppId(sBackendId, appId))
    return false;

  // Installed implies owned; a DLC that is owned but not installed still counts as owned.
  return SteamApps()->BIsDlcInstalled(appId) || SteamApps()->BIsSubscribedApp(appId);
}

plPlatformOperationPtr plSteamPlatformBackend::Refresh()
{
  // Ownership is answered by the local Steam client; there is nothing asynchronous to wait for.
  plPlatformOperationPtr pOp = plPlatformOperation::Create("Entitlements.Refresh");
  pOp->MutableResult().m_bFlag = OwnsApp();
  pOp->Complete(m_bInitialized ? plPlatformError::Success : plPlatformError::NotSignedIn);
  return pOp;
}

void plSteamPlatformBackend::OnDlcInstalled(DlcInstalled_t* pParam)
{
  plStringBuilder sId;
  sId.SetFormat("{0}", pParam->m_nAppID);

  plPlatformEvent e;
  e.m_Type = plPlatformEvent::Type::EntitlementsChanged;
  e.m_sId = sId;
  Emit(e);
}
