// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaSteamWorkspace : WorkspaceRules
{
    public PlasmaSteamWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("SteamPlugin");
    }
}
