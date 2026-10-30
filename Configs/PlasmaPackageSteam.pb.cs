// #pl-version 1
//
// Locates a Steamworks SDK to build against.
//
// This package ships no Steamworks binaries. The SDK is distributed by Valve under its own terms
// and the package registry is public, so the package carries Plasma's plugin code only; the SDK
// comes from the developer's own download, named by STEAM_SDK_PATH.
//
// Consequence at runtime: plSteamPlugin.dll imports from steam_api64.dll, so it will not load
// unless that DLL sits beside the executable. That is stated in the package description rather
// than left to be discovered as a load failure.

using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaPackageSteam
{
    /// \brief Finds the SDK root: the directory containing "public" and "redistributable_bin".
    public static bool TryGetSdk(out string sdkRoot, out string reason)
    {
        sdkRoot = string.Empty;
        reason = string.Empty;

        var fromEnv = Environment.GetEnvironmentVariable("STEAM_SDK_PATH");
        if (string.IsNullOrWhiteSpace(fromEnv))
        {
            reason = "STEAM_SDK_PATH is not set. Point it at the Steamworks SDK root - the directory containing " +
                     "public/steam/steam_api.h and redistributable_bin/win64/steam_api64.lib. This package ships " +
                     "no Steamworks binaries by design.";
            return false;
        }

        fromEnv = Path.GetFullPath(fromEnv.Trim());

        // A zip extracted with its top-level "sdk" folder intact is the common case; accept both.
        foreach (var candidate in new[] { fromEnv, Path.Combine(fromEnv, "sdk") })
        {
            if (HasSdkFiles(candidate))
            {
                sdkRoot = candidate;
                return true;
            }
        }

        reason = $"STEAM_SDK_PATH is '{fromEnv}', but no Steamworks SDK was found there. It should name the SDK " +
                 "root - the directory containing public/steam/steam_api.h and redistributable_bin/win64/steam_api64.lib.";
        return false;
    }

    /// \brief Include path, import library and the API-shape defines. Deliberately copies no runtime DLLs.
    public static void ConfigureConsumer(ModuleRules rules, BuildContext context)
    {
        if (context.Platform != TargetPlatform.Windows)
        {
            throw new PlatformNotSupportedException("The Steam plugin is only supported on Windows desktop.");
        }

        if (!TryGetSdk(out var sdkRoot, out var reason))
        {
            throw new DirectoryNotFoundException(reason);
        }

        rules.PublicIncludePaths.Add(Path.Combine(sdkRoot, "public"));
        rules.PublicLibraries.Add(Path.Combine(sdkRoot, "redistributable_bin", "win64", "steam_api64.lib"));

        // The SDK moved two entry points between releases. Probing the headers keeps one source tree
        // compiling against 1.5x and 1.6x alike.
        var steamApiHeader = File.ReadAllText(Path.Combine(sdkRoot, "public", "steam", "steam_api.h"));
        rules.PrivateDefinitions.Add(steamApiHeader.Contains("SteamAPI_InitEx")
            ? "PL_STEAM_HAS_INIT_EX=1"
            : "PL_STEAM_HAS_INIT_EX=0");

        // 1.61+ keeps RequestCurrentStats in the header as a commented-out line, so only live declarations count.
        rules.PrivateDefinitions.Add(DeclaresOutsideComment(Path.Combine(sdkRoot, "public", "steam", "isteamuserstats.h"), "RequestCurrentStats(")
            ? "PL_STEAM_HAS_REQUEST_CURRENT_STATS=1"
            : "PL_STEAM_HAS_REQUEST_CURRENT_STATS=0");
    }

    private static bool DeclaresOutsideComment(string headerPath, string token)
    {
        foreach (var line in File.ReadLines(headerPath))
        {
            var index = line.IndexOf(token, StringComparison.Ordinal);
            if (index < 0)
            {
                continue;
            }

            var commentIndex = line.IndexOf("//", StringComparison.Ordinal);
            if (commentIndex < 0 || commentIndex > index)
            {
                return true;
            }
        }

        return false;
    }

    private static bool HasSdkFiles(string sdkRoot)
    {
        return File.Exists(Path.Combine(sdkRoot, "public", "steam", "steam_api.h")) &&
               File.Exists(Path.Combine(sdkRoot, "redistributable_bin", "win64", "steam_api64.lib"));
    }
}
