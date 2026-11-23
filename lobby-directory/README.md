# Heart & Soul phone-host directory

Hosts POST their ZeroTier address here every 30 seconds. Joiners GET the list
and connect to each other. Game bytes do not pass through this worker.

Deploy when you are ready:

```
cd lobby-directory
npx wrangler secret put LOBBY_KEY
npx wrangler deploy
```

The APK learns the worker URL from the Gradle property `hns.lobbyUrl` and the
same secret from `hns.lobbyKey` (CI secret `HNS_LOBBY_KEY`). Leave both empty
until the worker exists. The header the game sends is `X-HeartSoul-Key`.
Do not commit the secret and do not print it.
