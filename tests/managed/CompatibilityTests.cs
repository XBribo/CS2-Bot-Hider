using System.Reflection;
using BotHiderApi;
using BotHiderImpl;
using Old = DemoTracerBotHiderApi;

namespace BotHider.Tests;

public class CompatibilityTests
{
    [Fact]
    public void LegacyFacadeFailsClosedAfterNativeDisconnect()
    {
        using var client = new NativePresentationClient();
        client.Dispose();
        using var service = new BotHiderPresentationService(client);
        var legacy = new LegacyBotHiderApi(client, service);
        Assert.Empty(legacy.GetManagedSlots());
        Assert.False(legacy.SetBotSteamId(1, 123));
        Assert.False(legacy.SetPersonaName(1, "test"));
        Assert.False(legacy.SetCrosshairCode(1, ""));
        Assert.False(legacy.SetScoreboardFlair(1, 0));
        Assert.False(legacy.SetIdentityMode(BotIdentityMode.Player));
        Assert.False(legacy.SetBotAvatar(1, "0"));
        Assert.True(client.ExternalAvatars);
        Assert.False(client.AutoRespawn);
    }

    [Fact]
    public void CompatibilityFacadeForwardsOwnershipAndExactIdentityToOneService()
    {
        var service = DispatchProxy.Create<IBotHiderPresentationApi, Capture>();
        var capture = (Capture)service;
        var adapter = new DemoTracerCompatibilityApi(service);
        using var owner = new CancellationTokenSource();
        var result = adapter.AcquirePresentationLease("consumer", [new() {
            Slot=5,Incarnation=42,SteamId=123,PlayerName="exact",CrosshairCode="",ScoreboardFlair=7
        }], owner.Token);
        Assert.Equal(2, adapter.ApiVersion);
        Assert.True(result.Ok);
        Assert.Equal("shared-token", result.LeaseToken);
        Assert.Equal(owner.Token, capture.Args![2]);
        var request = Assert.Single((BotHiderPresentationOverride[])capture.Args[1]!);
        Assert.Equal(42UL,request.Incarnation);
        Assert.Equal(123UL,request.SteamId);
        Assert.Equal("",request.CrosshairCode);
        Assert.True(adapter.ReleasePresentationLease(result.LeaseToken));
        Assert.Equal("shared-token",capture.Args![0]);
    }

    [Theory]
    [InlineData("  名字  ", "名字")]
    [InlineData("\u200b\n", "")]
    public void LegacyNamesRetainUpstreamNormalization(string source,string expected)
        => Assert.Equal(expected, PersonaName.Normalize(source));

    [Fact]
    public void LegacyNamesDoNotSplitUnicodeTextElements()
    {
        var normalized=PersonaName.Normalize(new string('a',30)+"😀");
        Assert.Equal(new string('a',30),normalized);
    }

    public class Capture : DispatchProxy
    {
        public object?[]? Args;
        protected override object? Invoke(MethodInfo? targetMethod,object?[]? args)
        {
            Args=args;
            return targetMethod!.Name switch {
                nameof(IBotHiderPresentationApi.AcquirePresentationLease) => new BotHiderPresentationLeaseResult {Ok=true,LeaseToken="shared-token",Slots=[5]},
                nameof(IBotHiderPresentationApi.ReleasePresentationLease) => true,
                _ => throw new NotSupportedException()
            };
        }
    }
}
