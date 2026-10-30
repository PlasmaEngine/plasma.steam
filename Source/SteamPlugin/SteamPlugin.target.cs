using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class SteamPluginTarget : TargetRules
{
    public SteamPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plSteamPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        // One translation unit per capability keeps the Steam callback plumbing readable; the
        // module is small enough that a unity build buys nothing.
        UseUnityBuild = false;
        UseIncrementalLinking = true;

        ExtraModules.Add("SteamPluginModule");
    }
}
