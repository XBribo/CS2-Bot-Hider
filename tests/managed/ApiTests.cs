using BotHiderApi;
using BotHiderImpl;

namespace BotHider.Tests;

public class ApiTests
{
    [Fact]
    public void ApiFacadeFailsClosedAfterNativeDisconnect()
    {
        using var client = new NativePresentationClient();
        client.Dispose();
        using var service = new BotHiderPresentationService(client);
        var api = new BotHiderApiProvider(client, service);
        Assert.Empty(api.GetManagedSlots());
        Assert.False(api.SetBotSteamId(1, 123));
        Assert.False(api.SetPersonaName(1, "test"));
        Assert.False(api.SetCrosshairCode(1, ""));
        Assert.False(api.SetScoreboardFlair(1, 0));
        Assert.False(api.SetIdentityMode(BotIdentityMode.Player));
        Assert.False(api.SetBotAvatar(1, "0"));
        Assert.False(api.TryPublishAvatarOverride(123, [137, 80, 78, 71, 13, 10, 26, 10], out _));
        Assert.False(api.TryClearAvatarOverride(123, out _));
        Assert.Equal(1, api.ApiVersion);
    }

    [Theory]
    [InlineData("  名字  ", "名字")]
    [InlineData("\u200b\n", "")]
    public void NamesRetainUpstreamNormalization(string source,string expected)
        => Assert.Equal(expected, PersonaName.Normalize(source));

    [Fact]
    public void NamesDoNotSplitUnicodeTextElements()
    {
        var normalized=PersonaName.Normalize(new string('a',30)+"😀");
        Assert.Equal(new string('a',30),normalized);
    }

}
