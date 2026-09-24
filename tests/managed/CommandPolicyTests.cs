using System.Reflection;
using BotHiderImpl;
using CounterStrikeSharp.API.Core.Attributes.Registration;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.Admin;

namespace BotHider.Tests;

public class CommandPolicyTests
{
    [Fact]
    public void BotHiderCommandsRequireServerConsoleOrRoot()
    {
        var unsafeCommands = new List<string>();
        var commandCount = 0;

        foreach (var method in DeclaredMethods(typeof(BotHiderImplPlugin)))
        {
            var commands = method.GetCustomAttributes<ConsoleCommandAttribute>().ToArray();
            commandCount += commands.Length;
            if (commands.Length == 0)
                continue;

            var helper = method.CustomAttributes.SingleOrDefault(attribute =>
                attribute.AttributeType == typeof(CommandHelperAttribute));
            var serverOnly = helper is { ConstructorArguments.Count: >= 3 } &&
                helper.ConstructorArguments[2].Value is int value &&
                (CommandUsage)value == CommandUsage.SERVER_ONLY;
            var rootOnly = method.CustomAttributes.Any(attribute =>
                attribute.AttributeType == typeof(RequiresPermissions) &&
                attribute.ConstructorArguments[0].Value is IReadOnlyCollection<CustomAttributeTypedArgument> permissions &&
                permissions.Any(permission => (string?)permission.Value == "@css/root"));
            if (!serverOnly && !rootOnly)
                unsafeCommands.AddRange(commands.Select(command => command.Command));
        }

        Assert.True(commandCount > 0, "No BotHider commands were discovered.");
        Assert.True(
            unsafeCommands.Count == 0,
            $"BotHider commands without server-only or root permission policy: {string.Join(", ", unsafeCommands)}");
    }

    private static IEnumerable<MethodInfo> DeclaredMethods(Type type)
        => type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
}
