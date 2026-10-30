# plasma.steam

Steam platform backend for PlasmaEngine's Platform Services, published as `plasma.steam`.

It implements `plPlatformBackend` on the Steamworks SDK: identity, achievements, stats, leaderboards,
friends, rich presence, lobbies, lobby-based matchmaking, invites and launch joins, DLC entitlements,
the Steam overlay, peer-to-peer datagrams over Steam Datagram Relay, and Steam Cloud saves through the
save system's `plSaveGameCloudProvider` seam. Voice is not provided (the hub answers Unsupported).

## Layout

```
Package.plPackage        the manifest - identity, compatibility, components
Steam.plPluginBundle     what plugin discovery reads
plasma.steam.workspace.cs
Configs/
  PlasmaPackageSdk.pb.cs   build rules against an installed engine SDK (stamped from the engine)
  PlasmaPackageSteam.pb.cs locates the Steamworks SDK through STEAM_SDK_PATH
Source/SteamPlugin/      runtime -> plSteamPlugin
  SteamPlatformBackend.h/.cpp   the backend object, lifecycle, callback registry, id helpers
  Capabilities/                 one file per provider interface
Branding/                icon, banner and the script that renders them
```

Runtime only: no editor plugin. The backend is never created in tool processes (editor, engine process,
command-line tools); those keep the local backend.

## What you still need

The Steamworks SDK is **not redistributed**. Download it from partner.steamgames.com and:

- To **build**: set `STEAM_SDK_PATH` to the SDK root - the directory containing
  `public/steam/steam_api.h` and `redistributable_bin/win64/steam_api64.lib` (the `sdk` folder
  inside the zip; the folder above it is accepted too).
- To **run**: copy `redistributable_bin/win64/steam_api64.dll` next to the game executable. Without
  it `plSteamPlugin.dll` fails to load and the hub falls back to the local backend.

## Configuration

`RuntimeConfigs/PlatformServices.ddl` names the backend and its app id:

```
BackendOrder { "steam", "local" }
BackendSettings
{
  steam { AppId = 480 }
}
```

With `AppId` set the backend calls `SteamAPI_RestartAppIfNecessary`; a build not started through the
Steam client is relaunched by Steam and `Initialize` fails so the hub falls back while this process
exits. Development builds write `steam_appid.txt` beside the executable when it is missing, which
is what lets a build started from the editor or a debugger talk to a running Steam client. Shipping
builds never write it.

Per-backend ids in `PlatformDefinitions.ddl` use the backend name `steam`: achievement and stat ids
are the API names from the Steamworks admin, leaderboard ids are leaderboard names, DLC ids are the
decimal DLC app ids.

## Behaviour notes

- Ids: users are `steam:<SteamID64>`, lobbies are the decimal lobby `CSteamID`.
- Achievements and stats wait for `UserStatsReceived_t`; requests made before that are queued and run
  when it arrives. On SDKs that removed `RequestCurrentStats` (1.61+) the stats are already loaded by
  init; the build detects which SDK it has (`PL_STEAM_HAS_REQUEST_CURRENT_STATS`).
- Matchmaking is a loop: search with the request's filters, join the first lobby with room, and after
  three empty rounds host a public lobby with the request's attributes and report Found for it.
- Transport uses `ISteamNetworkingMessages`. Channels 0..7 are polled; a session with a peer is
  accepted while `Listen()` is active and refused otherwise.
- Cloud saves are one file per slot, `sav_<project>_<profile>_<slot>.plsave`, with a small header in
  front of the envelope carrying revision, parent revision and the deleted flag. `Publish` reads the
  remote header first and answers `Conflict` when the revision is not the expected one; Steam has no
  server-side compare, so the check is client-side.
- Lobby member attributes are read for the keys the lobby itself carries, since Steam cannot
  enumerate member data keys.

## Building

Requires an engine SDK - a built engine checkout - named by `PLASMA_SDK_ROOT`, and the Steamworks SDK
named by `STEAM_SDK_PATH`:

```bash
PLASMA_SDK_ROOT=/path/to/engine STEAM_SDK_PATH=/path/to/steamworks/sdk plasmabuild build SteamPlugin
```

Output lands in `Bin/Windows_x64_Development/`.

## Artwork

`Branding/make_art.py` composes the icon (512x512) and banner (1100x512) from the official Steam
marks in `Branding/source/` (renders of Valve's `Steam_icon_logo.svg` and `Steam_2016_logo_black.svg`),
white on charcoal, with a pure-Python PNG codec. Steam and the Steam logo are trademarks of Valve
Corporation, used to identify a Steam integration.

## Publishing a new version

Staging and publishing are driven from the engine and registry repositories:

```bash
# engine repo - stage and validate, emitting machine-readable output
python Utilities/PackageInspector.py <staged-package-dir> --json <out>.json

# registry repo - upload blobs and record the release
python tools/publish_package.py <out>.json
```

Versions are immutable. A mistake is fixed by publishing a new patch version, never by replacing an
existing one.
