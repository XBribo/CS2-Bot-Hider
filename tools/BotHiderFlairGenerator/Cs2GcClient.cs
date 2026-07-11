using QRCoder;
using SteamKit2;
using SteamKit2.Authentication;
using SteamKit2.GC;
using SteamKit2.GC.Internal;
using SteamKit2.Internal;

namespace BotHiderFlairGenerator;

internal sealed class Cs2GcClient : IAsyncDisposable
{
    private const uint AppId = 730;
    private const uint LoginId = 0x42484647;
    private readonly SteamClient _steamClient = new();
    private readonly CallbackManager _callbacks;
    private readonly SteamUser _steamUser;
    private readonly SteamGameCoordinator _gameCoordinator;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly TaskCompletionSource _gcReady =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly object _pendingLock = new();
    private Task? _callbackPump;
    private TaskCompletionSource<uint>? _pendingProfile;
    private uint _pendingAccountId;

    // Initializes Steam handlers and callback subscriptions.
    public Cs2GcClient()
    {
        _callbacks = new CallbackManager(_steamClient);
        _steamUser = _steamClient.GetHandler<SteamUser>() ??
            throw new InvalidOperationException("SteamUser handler is unavailable.");
        _gameCoordinator = _steamClient.GetHandler<SteamGameCoordinator>() ??
            throw new InvalidOperationException("Steam GC handler is unavailable.");
        _callbacks.Subscribe<SteamClient.ConnectedCallback>(OnConnected);
        _callbacks.Subscribe<SteamClient.DisconnectedCallback>(OnDisconnected);
        _callbacks.Subscribe<SteamUser.LoggedOnCallback>(OnLoggedOn);
        _callbacks.Subscribe<SteamUser.LoggedOffCallback>(OnLoggedOff);
        _callbacks.Subscribe<SteamGameCoordinator.MessageCallback>(OnGcMessage);
    }

    // Connects through QR authentication and waits for the CS2 GC session.
    public async Task ConnectAsync(CancellationToken cancellationToken)
    {
        _callbackPump = Task.Run(PumpCallbacks, CancellationToken.None);
        _steamClient.Connect();
        await _gcReady.Task.WaitAsync(TimeSpan.FromMinutes(3), cancellationToken);
    }

    // Queries the featured display item for one Steam account.
    public async Task<uint> GetFeaturedDisplayItemAsync(
        uint accountId,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        TaskCompletionSource<uint> completion;
        lock (_pendingLock)
        {
            if (_pendingProfile is not null)
                throw new InvalidOperationException("Only one profile request may be active.");

            _pendingAccountId = accountId;
            _pendingProfile = new TaskCompletionSource<uint>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            completion = _pendingProfile;
        }

        var request = new ClientGCMsgProtobuf<PlayerProfileRequest>(
            Cs2GcMessage.RequestPlayersProfile);
        request.Body.AccountId = accountId;
        request.Body.RequestLevel = 32;
        _gameCoordinator.Send(request, AppId);

        try
        {
            return await completion.Task.WaitAsync(timeout, cancellationToken);
        }
        finally
        {
            lock (_pendingLock)
            {
                if (ReferenceEquals(_pendingProfile, completion))
                {
                    _pendingProfile = null;
                    _pendingAccountId = 0;
                }
            }
        }
    }

    // Stops Steam and the callback pump.
    public async ValueTask DisposeAsync()
    {
        _shutdown.Cancel();
        _steamUser.LogOff();
        _steamClient.Disconnect();
        if (_callbackPump is not null)
            await _callbackPump.ConfigureAwait(false);
        _shutdown.Dispose();
    }

    // Processes Steam callbacks until shutdown.
    private void PumpCallbacks()
    {
        while (!_shutdown.IsCancellationRequested)
            _callbacks.RunWaitCallbacks(TimeSpan.FromMilliseconds(500));
    }

    // Starts an interactive Steam QR authentication session.
    private async void OnConnected(SteamClient.ConnectedCallback callback)
    {
        try
        {
            var authSession = await _steamClient.Authentication.BeginAuthSessionViaQRAsync(
                new AuthSessionDetails());
            authSession.ChallengeURLChanged = () => DrawQrCode(authSession);
            DrawQrCode(authSession);
            var result = await authSession.PollingWaitForResultAsync();
            Console.WriteLine($"Steam account authorized: {result.AccountName}");
            _steamUser.LogOn(new SteamUser.LogOnDetails
            {
                Username = result.AccountName,
                AccessToken = result.RefreshToken,
                LoginID = LoginId
            });
        }
        catch (Exception exception)
        {
            FailSession($"Steam authentication failed: {exception.Message}");
        }
    }

    // Reports an unexpected Steam disconnect.
    private void OnDisconnected(SteamClient.DisconnectedCallback callback)
    {
        if (!_shutdown.IsCancellationRequested)
            FailSession("Disconnected from Steam.");
    }

    // Launches the CS2 GC session after Steam login succeeds.
    private void OnLoggedOn(SteamUser.LoggedOnCallback callback)
    {
        if (callback.Result != EResult.OK)
        {
            FailSession($"Steam logon failed: {callback.Result}/{callback.ExtendedResult}");
            return;
        }

        var gamesPlayed = new ClientMsgProtobuf<CMsgClientGamesPlayed>(
            EMsg.ClientGamesPlayed);
        gamesPlayed.Body.games_played.Add(new CMsgClientGamesPlayed.GamePlayed
        {
            game_id = new GameID(AppId)
        });
        _steamClient.Send(gamesPlayed);
        _ = SendDelayedGcHelloAsync();
    }

    // Reports a Steam logoff while the generator is active.
    private void OnLoggedOff(SteamUser.LoggedOffCallback callback)
    {
        if (!_shutdown.IsCancellationRequested)
            FailSession($"Logged off from Steam: {callback.Result}");
    }

    // Handles GC readiness and player profile responses.
    private void OnGcMessage(SteamGameCoordinator.MessageCallback callback)
    {
        if (callback.AppID != AppId)
            return;

        if (callback.EMsg == Cs2GcMessage.BaseClientWelcome)
        {
            SendBaseClientHello();
            _gcReady.TrySetResult();
            return;
        }
        if (callback.EMsg == Cs2GcMessage.PlayersProfile)
        {
            var response = new ClientGCMsgProtobuf<PlayersProfile>(callback.Message);
            CompleteProfileRequest(response.Body);
        }
    }

    // Waits briefly before sending the CS2 base client hello.
    private async Task SendDelayedGcHelloAsync()
    {
        try
        {
            await Task.Delay(TimeSpan.FromSeconds(1), _shutdown.Token);
            SendBaseClientHello();
        }
        catch (OperationCanceledException)
        {
        }
    }

    // Sends the CS2 base client hello to establish the GC session.
    private void SendBaseClientHello()
    {
        var hello = new ClientGCMsgProtobuf<BaseClientHello>(
            Cs2GcMessage.BaseClientHello);
        _gameCoordinator.Send(hello, AppId);
    }

    // Completes the active request when its account appears in the response.
    private void CompleteProfileRequest(PlayersProfile response)
    {
        TaskCompletionSource<uint>? completion;
        uint accountId;
        lock (_pendingLock)
        {
            completion = _pendingProfile;
            accountId = _pendingAccountId;
        }

        if (completion is null)
            return;

        PlayerProfile? profile = response.AccountProfiles.FirstOrDefault(
            candidate => candidate.AccountId == accountId);
        if (profile is not null)
            completion.TrySetResult(profile.Medals?.FeaturedDisplayItemDefIndex ?? 0);
    }

    // Draws the current Steam login URL as an ASCII QR code.
    private static void DrawQrCode(QrAuthSession authSession)
    {
        Console.WriteLine("Scan this QR code with the Steam mobile app:");
        using var generator = new QRCodeGenerator();
        using QRCodeData data = generator.CreateQrCode(
            authSession.ChallengeURL,
            QRCodeGenerator.ECCLevel.L);
        using var code = new AsciiQRCode(data);
        Console.WriteLine(code.GetGraphic(1, drawQuietZones: false));
    }

    // Fails connection and any active profile request.
    private void FailSession(string message)
    {
        var exception = new InvalidOperationException(message);
        _gcReady.TrySetException(exception);
        lock (_pendingLock)
            _pendingProfile?.TrySetException(exception);
    }
}
