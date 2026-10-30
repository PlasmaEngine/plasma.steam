using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class SteamPluginModule : ModuleRules
{
    public SteamPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/SteamPlugin", "SteamPluginPCH.h",
            "BUILDSYSTEM_BUILDING_STEAMPLUGIN_LIB");

        // Headers and import library from the developer's own Steamworks SDK. No Steam binaries
        // are copied into this package - see Configs/PlasmaPackageSteam.pb.cs.
        PlasmaPackageSteam.ConfigureConsumer(this, context);
    }
}
