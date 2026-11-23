# Heart & Soul lobby directory

Hosts POST `/v1/lobbies` about every 30 seconds. Joiners GET the same path.
A host leaves with POST `{ "key", "remove": true }` or DELETE. Entries expire
after 90 seconds without a new announce. Game traffic does not pass through
this worker.

The worker secret is `LOBBY_KEY`. The APK build reads the same value from the
GitHub Actions secret `HNS_LOBBY_KEY`, and the worker URL from the repository
variable `HNS_LOBBY_URL`. Neither value is committed.

## Deploy

```
cd tools/lobby-worker
npx wrangler login
npx wrangler secret put LOBBY_KEY
npx wrangler deploy
```

`wrangler deploy` prints the worker URL. Put that URL, with no trailing path,
in the GitHub repository variable `HNS_LOBBY_URL`. Put the same secret you
typed for `LOBBY_KEY` in the Actions secret `HNS_LOBBY_KEY`.

The game sends the secret in the `X-HeartSoul-Key` header. Until `LOBBY_KEY`
is set, the worker accepts requests with no header.
