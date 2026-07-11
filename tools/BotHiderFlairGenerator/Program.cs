using System.Diagnostics;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BotHiderFlairGenerator;

internal static class Program
{
    private const string DefaultInputPath = "configs/addons/BotHider/bot_info.json";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        IndentCharacter = ' ',
        IndentSize = 4
    };

    // Runs the standalone flair generator.
    private static async Task<int> Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        using var cancellation = new CancellationTokenSource();
        Console.CancelKeyPress += (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            cancellation.Cancel();
        };

        try
        {
            GeneratorOptions options = ParseArguments(args);
            JsonObject root = await LoadJsonAsync(options.InputPath, cancellation.Token);
            List<BotEntry> entries = ReadEntries(root, options.Overwrite);
            if (entries.Count == 0)
            {
                Console.WriteLine("No entries require a scoreboard flair query.");
                return 0;
            }

            Console.WriteLine($"Profiles to query: {entries.Count}");
            await using var gcClient = new Cs2GcClient();
            await gcClient.ConnectAsync(cancellation.Token);
            Console.WriteLine("CS2 Game Coordinator is ready.");
            await PopulateFlairsAsync(
                gcClient,
                root,
                entries,
                options,
                cancellation.Token);
            await SaveJsonAsync(options.InputPath, root, cancellation.Token);
            return 0;
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("Cancelled.");
            return 2;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 1;
        }
    }

    // Parses the input path, overwrite flag, and request delay.
    private static GeneratorOptions ParseArguments(string[] args)
    {
        string inputPath = DefaultInputPath;
        bool overwrite = false;
        int delayMs = 1200;
        for (int index = 0; index < args.Length; index++)
        {
            string argument = args[index];
            if (argument.Equals("--overwrite", StringComparison.OrdinalIgnoreCase))
            {
                overwrite = true;
            }
            else if (argument.Equals("--delay-ms", StringComparison.OrdinalIgnoreCase))
            {
                if (++index >= args.Length ||
                    !int.TryParse(args[index], out delayMs) ||
                    delayMs < 250)
                {
                    throw new ArgumentException("--delay-ms must be at least 250.");
                }
            }
            else if (argument.StartsWith('-'))
            {
                throw new ArgumentException($"Unknown option: {argument}");
            }
            else
            {
                inputPath = argument;
            }
        }

        return new GeneratorOptions(Path.GetFullPath(inputPath), overwrite, delayMs);
    }

    // Loads and validates the bot information JSON object.
    private static async Task<JsonObject> LoadJsonAsync(
        string path,
        CancellationToken cancellationToken)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("bot_info.json was not found.", path);

        string json = await File.ReadAllTextAsync(path, Encoding.UTF8, cancellationToken);
        return JsonNode.Parse(json) as JsonObject ??
            throw new JsonException("The root of bot_info.json must be an object.");
    }

    // Selects valid entries that still need a flair lookup.
    private static List<BotEntry> ReadEntries(JsonObject root, bool overwrite)
    {
        var entries = new List<BotEntry>();
        foreach ((string name, JsonNode? node) in root)
        {
            if (node is not JsonObject item ||
                !TryReadAccountId(item["steamid"], out uint accountId))
            {
                Console.Error.WriteLine($"Skipping '{name}': invalid steamid.");
                continue;
            }

            uint currentFlair = ReadUInt32(item["scoreboard_flair"]);
            if (!overwrite && currentFlair != 0)
                continue;
            entries.Add(new BotEntry(name, accountId, item));
        }

        return entries;
    }

    // Queries profiles sequentially and checkpoints updated flair values.
    private static async Task PopulateFlairsAsync(
        Cs2GcClient gcClient,
        JsonObject root,
        IReadOnlyList<BotEntry> entries,
        GeneratorOptions options,
        CancellationToken cancellationToken)
    {
        var cache = new Dictionary<uint, uint>();
        var stopwatch = Stopwatch.StartNew();
        int updated = 0;
        for (int index = 0; index < entries.Count; index++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            BotEntry entry = entries[index];
            if (!cache.TryGetValue(entry.AccountId, out uint flair))
            {
                try
                {
                    flair = await QueryWithRetriesAsync(
                        gcClient,
                        entry.AccountId,
                        cancellationToken);
                }
                catch (TimeoutException exception)
                {
                    WriteProgress(
                        index + 1,
                        entries.Count,
                        entry.Name,
                        $"failed ({exception.Message})",
                        stopwatch.Elapsed);
                    continue;
                }

                cache[entry.AccountId] = flair;
                await Task.Delay(options.DelayMs, cancellationToken);
            }

            entry.Json["scoreboard_flair"] = flair;
            updated++;
            WriteProgress(
                index + 1,
                entries.Count,
                entry.Name,
                $"account={entry.AccountId}, flair={flair}",
                stopwatch.Elapsed);
            if (updated % 25 == 0)
                await SaveJsonAsync(options.InputPath, root, cancellationToken);
        }
    }

    // Prints timestamped progress with average speed and estimated time remaining.
    private static void WriteProgress(
        int processed,
        int total,
        string name,
        string result,
        TimeSpan elapsed)
    {
        double speed = elapsed.TotalSeconds > 0
            ? processed / elapsed.TotalSeconds
            : 0;
        TimeSpan eta = speed > 0
            ? TimeSpan.FromSeconds((total - processed) / speed)
            : TimeSpan.Zero;
        string timestamp = DateTimeOffset.Now.ToString("yyyy-MM-dd HH:mm:ss");
        Console.WriteLine(
            $"[{timestamp}] [{processed}/{total}] {name}: {result} | " +
            $"{speed:F2}/s | ETA {FormatDuration(eta)}");
    }

    // Formats a duration without wrapping total hours at 24.
    private static string FormatDuration(TimeSpan duration)
    {
        int totalHours = (int)duration.TotalHours;
        return $"{totalHours:00}:{duration.Minutes:00}:{duration.Seconds:00}";
    }

    // Retries transient GC profile timeouts up to three times.
    private static async Task<uint> QueryWithRetriesAsync(
        Cs2GcClient gcClient,
        uint accountId,
        CancellationToken cancellationToken)
    {
        for (int attempt = 1; attempt <= 3; attempt++)
        {
            try
            {
                return await gcClient.GetFeaturedDisplayItemAsync(
                    accountId,
                    TimeSpan.FromSeconds(15),
                    cancellationToken);
            }
            catch (TimeoutException) when (attempt < 3)
            {
                Console.Error.WriteLine(
                    $"Profile {accountId} timed out, retry {attempt}/3.");
                await Task.Delay(TimeSpan.FromSeconds(attempt * 2), cancellationToken);
            }
        }

        throw new TimeoutException($"Profile {accountId} did not respond after 3 attempts.");
    }

    // Atomically writes the updated JSON file in UTF-8 without a BOM.
    private static async Task SaveJsonAsync(
        string path,
        JsonObject root,
        CancellationToken cancellationToken)
    {
        string directory = Path.GetDirectoryName(path) ?? Directory.GetCurrentDirectory();
        string temporaryPath = Path.Combine(
            directory,
            $".{Path.GetFileName(path)}.{Environment.ProcessId}.tmp");
        try
        {
            string json = root.ToJsonString(JsonOptions) + Environment.NewLine;
            await File.WriteAllTextAsync(
                temporaryPath,
                json,
                new UTF8Encoding(false),
                cancellationToken);
            File.Move(temporaryPath, path, true);
        }
        finally
        {
            if (File.Exists(temporaryPath))
                File.Delete(temporaryPath);
        }
    }

    // Reads either a 32-bit account ID or a full individual SteamID64.
    private static bool TryReadAccountId(JsonNode? node, out uint accountId)
    {
        accountId = 0;
        if (node is null || !ulong.TryParse(node.ToString(), out ulong value))
            return false;

        const ulong steamId64Base = 76561197960265728UL;
        if (value >= steamId64Base)
            value -= steamId64Base;
        if (value == 0 || value > uint.MaxValue)
            return false;

        accountId = (uint)value;
        return true;
    }

    // Reads an unsigned integer from an optional JSON value.
    private static uint ReadUInt32(JsonNode? node)
    {
        return node is not null && uint.TryParse(node.ToString(), out uint value)
            ? value
            : 0;
    }

    private sealed record GeneratorOptions(string InputPath, bool Overwrite, int DelayMs);
    private sealed record BotEntry(string Name, uint AccountId, JsonObject Json);
}
