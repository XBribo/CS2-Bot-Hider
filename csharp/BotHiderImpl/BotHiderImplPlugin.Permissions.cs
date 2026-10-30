using System.Text.Json;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Admin;
using CounterStrikeSharp.API.Modules.Commands;

namespace BotHiderImpl;

public partial class BotHiderImplPlugin
{
    private CommandPermissionsConfig _commandPermissions = new(true);

    // Load permissions; fail closed on config errors.
    private void LoadCommandPermissions()
    {
        _commandPermissions = new(true);
        try
        {
            string path = Path.Combine(Server.GameDirectory, "csgo", "addons", "BotHider", "config.json");
            _commandPermissions = CommandPermissionsConfig.Load(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            Server.PrintToConsole("[BotHider] command permission config invalid or unreadable: using restricted defaults");
        }
    }

    // Console and RCON bypass client permissions.
    private bool CanExecuteCommand(CCSPlayerController? player, CommandInfo cmd)
    {
        if (player == null || !_commandPermissions.RequirePermissions) return true;
        // CSS admins bypass the configured flag.
        if (player.IsValid && player.AuthorizedSteamID != null &&
            (AdminManager.PlayerHasPermissions(player, "@css/root") ||
             AdminManager.PlayerHasPermissions(player, "@css/admin") ||
             AdminManager.PlayerHasPermissions(player, _commandPermissions.Permission))) return true;

        cmd.ReplyToCommand($"[BotHider] permission required: {_commandPermissions.Permission}, @css/root or @css/admin");
        return false;
    }
}

internal sealed record CommandPermissionsConfig(bool RequirePermissions = false,
    string Permission = "@bothider/admin")
{
    // Missing settings are public; invalid settings fail closed.
    internal static CommandPermissionsConfig Load(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (FileNotFoundException) { return new(); }
        catch (DirectoryNotFoundException) { return new(); }

        using var document = JsonDocument.Parse(text);
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) throw new JsonException("Expected config object.");
        if (!root.TryGetProperty("commands", out var commands)) return new();
        if (commands.ValueKind != JsonValueKind.Object) throw new JsonException("Expected commands object.");

        bool requirePermissions = false;
        if (commands.TryGetProperty("require_permissions", out var required))
        {
            if (required.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                throw new JsonException("Expected require_permissions boolean.");
            requirePermissions = required.GetBoolean();
        }

        string permission = "@bothider/admin";
        if (commands.TryGetProperty("permission", out var configured))
        {
            if (configured.ValueKind != JsonValueKind.String)
                throw new JsonException("Expected permission string.");
            permission = configured.GetString()!;
            int separator = permission.IndexOf('/');
            if (permission.Length < 4 || permission[0] != '@' || separator < 2 ||
                separator == permission.Length - 1 || permission.Any(char.IsWhiteSpace))
                throw new JsonException("Expected CSS permission flag @domain/name.");
        }

        return new(requirePermissions, permission);
    }
}
