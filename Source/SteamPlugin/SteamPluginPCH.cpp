#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/Configuration/Plugin.h>
#include <SteamPlugin/SteamPluginDLL.h>

PL_STATICLINK_LIBRARY(SteamPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(SteamPlugin_SteamPlugin);
}
