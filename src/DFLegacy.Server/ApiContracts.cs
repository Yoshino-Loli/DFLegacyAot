using System.Text.Json.Serialization;
using DFLegacy.Protocol;

namespace DFLegacy.Server;

// Response and request shapes that used to be anonymous types. Native AOT cannot
// build JSON metadata for anonymous types at runtime, so every shape the HTTP
// surface returns has to be a named type registered on ApiJsonContext.
public sealed record ErrorResponse(string? Error);

public sealed record HealthResponse(string Status, string Service);

public sealed record ServerStatusResponse(
    DateTimeOffset StartedAt,
    long TotalConnections,
    long TotalPackets,
    int ActiveSessions,
    IEnumerable<string> Entrance,
    string CharacterDatagram,
    IEnumerable<string> GameplayDatagram,
    string GameProbe,
    GameRandomStatus Random,
    bool ChannelScriptConfigured,
    int CachedItems);

public sealed record AdminCharacterSummary(
    Guid Id,
    uint CharacterNo,
    string Name,
    int Job,
    int GrowType,
    int Level,
    string UserName,
    uint AccountUid,
    bool Online);

public sealed record AdminItemSummary(
    ushort Id,
    string Name,
    AdminMailAttachmentKind AttachmentKind,
    string ScriptKind,
    string InventoryCategory,
    string TypeTag,
    string AttachType,
    int? StackLimit,
    ushort Durability,
    string ScriptPath);

public sealed record AdminDungeonSummary(
    ushort DungeonId,
    byte MinimumLevel,
    byte BasisLevel,
    bool IsHellDungeon,
    bool GoldCardUse);

public sealed record AdminMessageTypeOption(byte Value, string Name);

public sealed record BroadcastMessageResponse(
    bool Queued,
    int Recipients,
    GameMessageType MessageType,
    ushort TargetAreaUserId,
    int EncodedLength);

public sealed record BroadcastPopupResponse(
    bool Queued,
    int Recipients,
    int EncodedLength);

public sealed record DungeonDifficultyResponse(
    Guid CharacterId,
    ushort DungeonId,
    byte MaximumDifficulty,
    bool NotifiedOnlineCharacter);

public sealed record CharacterMessageResponse(
    bool Queued,
    Guid CharacterId,
    GameMessageType MessageType,
    ushort TargetAreaUserId,
    int EncodedLength);

public sealed record CharacterPopupResponse(
    bool Queued,
    Guid CharacterId,
    int EncodedLength);

public sealed record MailSentResponse(
    CharacterMailRecord Mail,
    bool NotifiedOnlineCharacter);

/// <summary>
/// Serialization for data/state.json. Keeps PascalCase property names and
/// indented output so existing save files stay byte-compatible.
/// </summary>
[JsonSourceGenerationOptions(WriteIndented = true)]
[JsonSerializable(typeof(JsonGameStore.JsonGameStoreState))]
[JsonSerializable(typeof(List<AccountRecord>))]
internal sealed partial class StorageJsonContext : JsonSerializerContext;

/// <summary>
/// Serialization for the admin HTTP API. Mirrors the ASP.NET Core web defaults
/// (camelCase, case-insensitive input, numbers readable from strings) so the
/// embedded admin page and the launcher keep working unchanged.
/// </summary>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    PropertyNameCaseInsensitive = true,
    NumberHandling = JsonNumberHandling.AllowReadingFromString)]
[JsonSerializable(typeof(ErrorResponse))]
[JsonSerializable(typeof(HealthResponse))]
[JsonSerializable(typeof(ServerStatusResponse))]
[JsonSerializable(typeof(AdminCharacterSummary))]
[JsonSerializable(typeof(AdminCharacterSummary[]))]
[JsonSerializable(typeof(AdminItemSummary))]
[JsonSerializable(typeof(AdminItemSummary[]))]
[JsonSerializable(typeof(AdminDungeonSummary))]
[JsonSerializable(typeof(AdminDungeonSummary[]))]
[JsonSerializable(typeof(AdminMessageTypeOption))]
[JsonSerializable(typeof(AdminMessageTypeOption[]))]
[JsonSerializable(typeof(BroadcastMessageResponse))]
[JsonSerializable(typeof(BroadcastPopupResponse))]
[JsonSerializable(typeof(DungeonDifficultyResponse))]
[JsonSerializable(typeof(CharacterMessageResponse))]
[JsonSerializable(typeof(CharacterPopupResponse))]
[JsonSerializable(typeof(MailSentResponse))]
[JsonSerializable(typeof(GameRandomStatus))]
[JsonSerializable(typeof(PublicAccountRecord))]
[JsonSerializable(typeof(PublicAccountRecord[]))]
[JsonSerializable(typeof(CharacterRecord))]
[JsonSerializable(typeof(CharacterRecord[]))]
[JsonSerializable(typeof(CharacterMailRecord))]
[JsonSerializable(typeof(CharacterMailRecord[]))]
[JsonSerializable(typeof(SessionSnapshot))]
[JsonSerializable(typeof(SessionSnapshot[]))]
[JsonSerializable(typeof(PacketTraceSnapshot))]
[JsonSerializable(typeof(PacketTraceSnapshot[]))]
[JsonSerializable(typeof(CreateAccountRequest))]
[JsonSerializable(typeof(ChangeAccountPasswordRequest))]
[JsonSerializable(typeof(ResetAccountPasswordRequest))]
[JsonSerializable(typeof(CreateCharacterRequest))]
[JsonSerializable(typeof(SetCharacterFatigueRequest))]
[JsonSerializable(typeof(CreateCharacterMailRequest))]
[JsonSerializable(typeof(SendCharacterMessageRequest))]
[JsonSerializable(typeof(SendCharacterPopupRequest))]
[JsonSerializable(typeof(AdminSendMailRequest))]
[JsonSerializable(typeof(AdminBroadcastMessageRequest))]
[JsonSerializable(typeof(AdminBroadcastPopupRequest))]
[JsonSerializable(typeof(AdminSetDungeonDifficultyRequest))]
[JsonSerializable(typeof(ClientCeraTopUpRequest))]
[JsonSerializable(typeof(ClientDiamondGrantRequest))]
[JsonSerializable(typeof(ClientPremiumGrantRequest))]
[JsonSerializable(typeof(ClientCeraTopUpResult))]
[JsonSerializable(typeof(ClientDiamondGrantResult))]
[JsonSerializable(typeof(ClientPremiumGrantResult))]
[JsonSerializable(typeof(AccountPasswordUpdateResult))]
[JsonSerializable(typeof(CreateCharacterResult))]
[JsonSerializable(typeof(CharacterMailSendResult))]
public sealed partial class ApiJsonContext : JsonSerializerContext;
