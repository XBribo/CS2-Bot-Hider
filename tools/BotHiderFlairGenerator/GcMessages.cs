using ProtoBuf;

namespace BotHiderFlairGenerator;

internal abstract class ExtensibleMessage : IExtensible
{
    private IExtension? _extension;

    // Provides protobuf-net storage for unknown GC fields.
    public IExtension GetExtensionObject(bool createIfMissing)
    {
        return Extensible.GetExtensionObject(ref _extension, createIfMissing);
    }
}


internal static class Cs2GcMessage
{
    internal const uint BaseClientWelcome = 4004;
    internal const uint BaseClientHello = 4006;
    internal const uint RequestPlayersProfile = 9127;
    internal const uint PlayersProfile = 9128;
}

[ProtoContract]
internal sealed class BaseClientHello : ExtensibleMessage
{
    [ProtoMember(1)]
    public uint Version { get; set; } = 2000244;

    [ProtoMember(3)]
    public uint ClientSessionNeed { get; set; }

    [ProtoMember(4)]
    public uint ClientLauncher { get; set; }

    [ProtoMember(9)]
    public uint SteamLauncher { get; set; }
}

[ProtoContract]
internal sealed class PlayerProfileRequest : ExtensibleMessage
{
    [ProtoMember(3)]
    public uint AccountId { get; set; }

    [ProtoMember(4)]
    public uint RequestLevel { get; set; } = 32;
}

[ProtoContract]
internal sealed class PlayersProfile : ExtensibleMessage
{
    [ProtoMember(2)]
    public List<PlayerProfile> AccountProfiles { get; } = [];
}

[ProtoContract]
internal sealed class PlayerProfile : ExtensibleMessage
{
    [ProtoMember(1)]
    public uint AccountId { get; set; }

    [ProtoMember(9)]
    public PlayerMedals? Medals { get; set; }
}

[ProtoContract]
internal sealed class PlayerMedals : ExtensibleMessage
{
    [ProtoMember(8)]
    public uint FeaturedDisplayItemDefIndex { get; set; }
}
