#include <SteamPlugin/SteamPluginPCH.h>

#include <Foundation/Configuration/Startup.h>
#include <GameEngine/PlatformServices/PlatformServices.h>
#include <SteamPlugin/SteamPlatformBackend.h>
#include <SteamPlugin/SteamPluginDLL.h>

// The backend registers itself as the plPlatformBackend singleton in its constructor; the hub finds it through
// plSingletonRegistry and drives Initialize/Tick/Shutdown. Tool processes (editor, engine process, command-line
// tools) never get a store backend, so nothing is created there.
static plSteamPlatformBackend* s_pSteamBackend = nullptr;

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(Steam, SteamPlugin)

  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation",
    "Core"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    if (plPlatformServices::IsToolProcess())
    {
      plLog::Dev("Steam: tool process, backend not created.");
      return;
    }

    s_pSteamBackend = PL_DEFAULT_NEW(plSteamPlatformBackend);
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    if (s_pSteamBackend != nullptr)
    {
      PL_DEFAULT_DELETE(s_pSteamBackend);
      s_pSteamBackend = nullptr;
    }
  }

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on

PL_PLUGIN_ON_LOADED()
{
}

PL_PLUGIN_ON_UNLOADED()
{
}

PL_STATICLINK_FILE(SteamPlugin, SteamPlugin_SteamPlugin);
