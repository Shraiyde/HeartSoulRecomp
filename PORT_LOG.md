# Port log

## 2026-11-26, pin host UDP candidate before authentication

### Changed

- The host UDP relay previously replaced its unauthenticated candidate endpoint
  whenever any datagram arrived. A second sender could therefore redirect the
  native host's authenticated UDP hello response before the relay pinned the
  peer.
- The relay now holds the first candidate endpoint while the native protocol
  validates its hello. Other endpoints are ignored until that candidate is
  authenticated or its fixed 4.5-second authentication window expires. A failed endpoint is
  quarantined for 30 seconds before it can try again.
- The authenticated peer remains pinned and is released after 4.5 seconds
  without an authenticated packet, allowing a later guest to join.

### Verification

- Reviewed the host relay state transitions in source. No Android build or
  two-device test has run yet.

### Next

1. Check the branch-push Android build for Java compile errors.
2. Test a normal UDP handshake and verify a second source cannot redirect it.
3. Test an invalid hello followed by a valid guest after candidate expiry.
4. Test authenticated peer timeout and reconnect.

---

## 2026-11-26, serialize ZeroTier node initialization

### Changed

- Audited every `ZeroTierNative` call in
  `HeartSoulZeroTier.java` after the socket-lock pass.
- Wrapped node initialization from storage, event-handler registration,
  node start, network join, and assigned-address lookup in the shared
  `ZT` lock. These operations previously ran outside the lock while relay
  threads used the same libzt instance.
- Existing socket creation, bind/connect, read/write, receive/send, readiness
  checks, timeout configuration, and close paths were already serialized.

### Verification

- Source audit completed by inspecting every `ZeroTierNative` call in the
  Java bridge. This is a source-level audit only; no Android build or device
  test has run yet.

### Next

1. Let the branch-push Android CI compile this change and inspect failures.
2. If CI passes, test two-device host/join, UDP HELLO/READY, movement, TCP
   fallback, and disconnect/reconnect.
3. Continue battle ownership and shared-world convergence tests.

---

## 2026-11-26, serialize guest UDP socket cleanup

### Changed

- Fixed the guest ZeroTier UDP relay's final cleanup path: its
  `ZeroTierDatagramSocket.close()` now runs under the shared `ZT` lock,
  matching the host relay and the other libzt socket operations.
- This closes a gap in the earlier socket-serialization pass. libzt exposes
  shared native error state, so even cleanup on a different relay thread must
  not overlap another thread's libzt call.

### Not verified

- The Android NDK/Gradle build has not yet been run after this change.
- No two-phone UDP handshake, disconnect, or reconnect test has been run.

### Next

1. Check the Android co-op build for Java/NDK errors.
2. Exercise guest UDP shutdown while the TCP bridge is active.
3. Verify UDP HELLO/READY, movement packets, and TCP fallback on two devices.

---

## 2026-11-26, serialize ZeroTier socket operations

### Changed

- Both UDP relay directions now serialize libzt socket creation, connect,
  receive, send, timeout configuration, and close through the same lock used
  by the TCP bridge. This avoids concurrent libzt calls racing its shared
  error state.
- ZeroTier UDP receives use a 1 ms timeout while holding that lock; the local
  loopback side keeps its independent 100 ms timeout. TCP I/O is therefore
  not held behind a long UDP receive.
- Readiness/status queries that can run concurrently with the relay are also
  serialized through the same lock.
- TCP listener setup and guest TCP connect operations now use that same lock.

### Not verified

- This concurrency change has not been built or exercised on Android.
- A real two-phone UDP test is still required.

---


## 2026-11-26, prevent shared-world queue overflow loss

### Changed

- The game-side flag/variable queue now coalesces pending updates for the
  same state key. It removes the older queued update and appends the newest
  value at the tail, preserving ordering relative to other story changes.
- If the queue is full and a new distinct state key cannot be queued, the
  story-state scan is restarted. The scanner waits for queue space and
  republishes the current shared flags and variables instead of silently
  losing the state change.
- The scan's own bounded enqueue path remains capacity-guarded.

### Not verified

- This change has not been compiled or exercised in-game.
- No Android build or two-device test is verified.

### Next

1. Build the native Android target and fix any compile issues.
2. Stress the shared-state queue with repeated flag/variable updates while
   the peer is disconnected, then reconnect and verify convergence.
3. Continue protocol and battle-action tests before publishing.

---


## 2026-11-26, ZeroTier UDP position relay

State: the Android bridge and native transport now have a UDP relay path in
source. The Android build and two-device path are still unverified.

### Changed

- Added a host-side `ZeroTierDatagramSocket` relay. It forwards ZeroTier UDP
  datagrams to the native host's loopback UDP socket and returns game replies
  to the sending ZeroTier peer.
- Added a guest-side relay with a loopback UDP port allocated before SDL
  starts. `HeartSoulActivity` passes that port as
  `HNS_COOP_UDP_PORT`; the native guest sends its position datagrams to the
  relay instead of the unrelated TCP loopback endpoint.
- The guest ZeroTier datagram socket binds to the same allowed game port as
  the host, keeping return traffic within the ad-hoc network's port range.
- The host relay pins its remote UDP endpoint only after the native host
  replies to an authenticated UDP hello, rejects other endpoints while pinned,
  and releases the pin after 4.5 seconds without peer datagrams so a new guest
  can join after the previous session ends.
- The guest relay also pins the local native UDP sender and releases that pin
  after the same idle window, preventing another local socket from taking over
  an active game's relay endpoint.
- UDP is optional: if the guest cannot bind its loopback relay, the TCP
  connection still opens and the native transport falls back to TCP positions.
- Native UDP socket creation now uses the Java-provided relay port for guests.
  The existing TCP stream remains the reliable gameplay transport and
  automatic fallback when UDP cannot be established.
- Documented the bridge as carrying both TCP and UDP.
- Kept the producer-facing outbound staging queue at 48 entries while the
  reliable replay outbox remains 2,048 entries, avoiding a redundant 8 MiB
  staging allocation.

### Not verified

- The Java UDP relay has not yet been compiled against the exact libzt AAR.
- No Android NDK/Gradle build or two-phone UDP handshake has been run.
- The UDP relay is single-peer by design; it must be tested with reconnects
  and stale peer datagrams before release.
- No APK has been published.

### Next

1. Confirm the branch-push Android build; fix any Java/NDK compile errors.
2. Verify UDP HELLO/READY and position packets cross the embedded ZeroTier
   bridge in both directions.
3. Verify TCP fallback still works when UDP setup fails.
4. Add protocol tests for ACK/replay/resume and battle-action ownership.
5. Test disconnect/reconnect and a trainer battle on two Android devices.

---


## 2026-11-26, bound the co-op staging queue

State: transport work is present on `coop/pkmultiplayer-hns-adapter`,
but no Android build or two-device test is verified.

### Changed

- `src/platform/coop_net.c` now keeps the producer-facing outbound staging
  queue at 48 entries instead of allocating another 2,048-entry queue.
- The 2,048-entry reliable replay outbox remains separate and retains sent
  phone-session messages until acknowledged. The change removes roughly
  8 MiB of static queue storage without reducing the replay outbox capacity.
- The current phone transport on this branch is protocol v6: authenticated
  client/session handshake, reconnect resume, ordered reliable TCP messages,
  ACK/replay, and latest-only UDP position snapshots. The PC World Host
  handshake remains a separate legacy path.

### Not verified

- The queue-capacity change has not yet been compiled.
- No Android device or two-phone test has run in this session.
- The reliable outbox is in-memory; process death starts a new session and
  must be treated as a session reset, not durable save-backed delivery.

### Next

1. Confirm the branch-push Android compile and fix any compiler errors.
2. Add host/guest transport tests for ACK, duplicate/reordered frames,
   reconnect/resume, changed peer identity, and full queues.
3. Audit battle state transfer and game-side retry paths before a release.
4. Test on two Android devices before publishing an APK.

---


Newest entry first. One entry per working session. Each entry says what was
done, what was learned, and exactly where to pick up. If you are a model
starting a session: read the top entry and `PORT_PLAN.md`, then continue from
"Next".

Rules for this log:

- Record facts you verified, and say how. Mark guesses as guesses.
- Record dead ends so nobody repeats them.
- End every entry with a "Next" list, most important first.

---

## 2026-11-26, phone protocol v2 identity handshake

State: native handshake change committed on
`coop/pkmultiplayer-hns-adapter`. Commits:
- `820bddd10c24cff1d32426b6e9295045c74841ed`: protocol version and
  authenticated peer identity pinning.
- `cfd7945ca52b917cd4a9f12049603b6d3fb7af20`: simplified version parsing.
- `32c9485b893ce0fd039b2389dc29b41ffb38689c`: added this branch to the
  Android co-op compile workflow's push filter.
- `3eef91dad27b0b0cdbdd1efcb0b31121ffd6f24b`: documented the new handshake.

### Changed

- Phone host now requires `HELLO 2 <client-id> <HMAC-proof>`.
- Phone host replies with `READY 2 <host-id> <HMAC-proof>`.
- Both sides authenticate the other peer's persistent client id using the
  existing HMAC challenge. Host and guest pin that peer id for the lifetime
  of the process, so a different save cannot silently take over the same
  running co-op session after a disconnect.
- The PC World Host path retains its legacy `HELLO <id> <proof>` handshake
  and `READY <role>` response.
- The new branch is included in the Co-op APK workflow push filter so a
  subsequent branch push should run the native Android compile.

### Not verified

- No NDK/Gradle build result is available yet for this branch.
- No two-device host/join, reconnect, or PC-host regression test has run.
- This is not yet the PkMultiplayer session replay implementation. Sequence
  ACKs, a durable outbox, resume handshake, and duplicate suppression remain.

### Next

1. Check the Android co-op workflow result for this branch and fix any
   compile errors.
2. Add automated host/guest tests for protocol v2, invalid HMAC, wrong
   protocol version, and changed peer identity.
3. Port the PkMultiplayer sequence/ACK/outbox/resume semantics above the
   existing TCP transport; preserve the PC-host protocol.
4. Test on two Android devices before adding chat, emotes, joint catches or
   publishing an APK.

---

## 2026-11-26, native transport audit

Updated `docs/PKMULTIPLAYER_HNS_PORT.md` in commit
`65d90972ee7d3cafea732c4290011e10c03451aa` with the audited message map
and transport gaps. Read the document before changing the wire protocol.

### Verified on `port`

- `include/platform/coop_net.h` declares messages 16, 17, 23, 24-31.
  Active `src/coop.c` handling covers POSITION, WORLD_FLAG, WORLD_VAR,
  WORLD_READY, WORLD_CONFIG, WORLD_CONFIG_BLOB, WORLD_RESYNC and
  POSITION_RESYNC. SIO_MULTI, WORLD_CONFIG_ACK and SIO_RESYNC have no
  active general receive path in the inspected code; config ACK is sent by
  the guest but not consumed by the host in the inspected switch.
- `coop_net.c` frames contain a 32-bit sequence, but the game-message
  receive path does not use it for ACK, replay, or duplicate suppression.
  AES-GCM nonce counters reject replay only within the active connection.
- Outbound queue capacity is 48. Inbound overflow drops the oldest message.
  A failed outbound send can also lose a message if requeueing finds the
  outbound queue full.
- Position and gameplay traffic share one TCP stream. Position is emitted
  first per flush, but there is no independent fast channel.
- The current shared-world rescan and settings blob are not a durable,
  idempotent event journal.

### Next

1. Implement reliable session sequencing, ACKs, replay and resume in the
   existing transport without adding ENet or replacing ZeroTier.
2. Make queue overflow explicit and prevent loss of critical world/battle
   messages. Keep position snapshots latest-only.
3. Add protocol/feature negotiation and stable save/session identities.
4. Add host/guest transport tests before touching battle behaviour.
5. Run two-device tests before publishing any APK.

## 2026-11-26, PkMultiplayer compatibility audit

State: created branch `coop/pkmultiplayer-hns-adapter` from `port`.
Added `docs/PKMULTIPLAYER_HNS_PORT.md` (commit
`1784df568278498937e0ee3c69ab30eec7755865`). No game code was changed
and no APK was built in this audit.

### Verified

- `Mochx07/PkMultiplayer` is a Lua mod for Gen2Recomped, not a standalone
  multiplayer library. Its `manifest.json` declares the Gen2Recomped mod
  API and its entry point patches Gen 1/2 Lua engine modules.
- HeartSoulRecomp is a native C / SDL2 Android port of Heart & Soul 2.0.6,
  based on the Gen 3 pokeemerald engine. It has no Gen2Recomped Lua mod
  loader, so the PkMultiplayer ZIP and generated Lua patches cannot be
  installed into this game.
- The compatible engineering path is to port the feature behaviour and
  session/reliability design into the existing native co-op, not to replace
  the HnS engine or add a second incompatible game runtime.
- PkMultiplayer's `src/coop/CoopNet.lua` provides useful protocol design:
  reliable ordered gameplay messages, latest-only movement snapshots,
  sequence numbers, ACKs, bounded outbox, duplicate suppression and a
  resume handshake. HnS already has encrypted TCP frames and embedded
  ZeroTier; TCP can remain the transport while those session semantics are
  ported above it.
- The new compatibility document maps the PkMultiplayer feature set to HnS
  and records an implementation/test order.

### Next

1. Audit the current `port` branch transport and every `COOP_MSG_*`
   producer/consumer, including Android host/join bridge and UI.
2. Add protocol negotiation and reliable session sequence/ACK/resume to the
   existing transport, with host/guest tests for replay and duplicate frames.
3. Test two Android devices, including Wi-Fi loss/reconnect and a disconnect
   during a trainer battle, before expanding to chat, emotes or joint catches.
4. Keep APK distribution on GitHub Releases and do not call a compile-only
   build a verified co-op release.

---

## 2026-11-24, fullscreen touch layout

State at end of session: `recomp-phone-2` (`108a25e9`) did contain the new
`sdl2.c` (its `libmain.so` has the 4096-sample audio buffer). The picture
was still a band because the window was then resized to the game aspect.
Not played on a phone.

### Done

- Android no longer calls `SDL_SetWindowSize` with the game dimensions.
  The window is the display, and the picture is scaled evenly at the top.
  L, R, the D-pad, A, B, Select and Start are in the area below it.
- Touches use `SDL_TouchFingerEvent` 0..1 times the renderer output size,
  the same pixels the buttons are drawn in, and stay held until the finger lifts.

### Next

- Tag `recomp-phone-3` after the `port` compile is green.

---

## 2026-11-23, phone controls and audio

State at end of session: `recomp-phone-1` (`4017e2b982`) crackled and its
on-screen buttons did nothing and covered the picture. Not played on a phone.

### Done

- Audio cause: the device callback buffer was still 1024 frames (~24 ms).
  The touch overlay is drawn on the game thread before vsync, so that
  callback runs late. The 100 ms SDL queue from `recomp-coop-1` was
  unchanged and never reached the speaker in time. The callback buffer is
  now 4096 frames, and the queue is filled again after the overlay is drawn.
- Buttons are laid out in the band under a uniformly scaled picture. L is
  above the D-pad and R is above A/B. A finger sets the matching GBA key
  until it lifts. The old hit test mapped touches through the letterboxed
  logical rect, which put them outside 0..1, so every button was ignored.

### Next

- Tag `recomp-phone-2` after the `port` compile is green.

---

## 2026-11-23, phone APK compile

State at end of session: the phone host, joiner, and lobby client are in
`port`. Not played on a phone.

### Done

- The assemble step was rebuilding libzt from `third_party/libzt`, which
  CI does not check out. The AAR from the earlier step already sits in
  `android/app/libs/zerotier/`. The extra Gradle dependency on
  `libs/libzt-release.aar` is gone.
- Leaving the activity now marks the session backgrounded, flushes the
  outbound queue, and stretches the TCP timeout. Coming back marks it
  foregrounded. The network thread keeps the 400 ms keepalive either way.
- `CoopLobby.access` had a C `(void)` cast. javac rejected it once the
  native build got far enough to compile Java. The unused context
  argument is gone. The full job log for run 45189432643 ends on that
  error. The Actions page stops earlier, in the arm compile, which looks
  like a killed runner.
- The announcer's HTTP status string shadowed `CoopNetwork.Status status`.
  javac reported that as the only error on run 45192265910.
- The co-op APK job now builds the arm and arm64 data with `make -j2`
  and runs Gradle with two workers. A local build still uses every core
  unless `HNS_MAKE_JOBS` or `HNS_GRADLE_WORKERS` is set.

### Next

- Confirm the `port` Co-op APK run is green, then tag `recomp-phone-1`
  so that same workflow publishes the debug APK.

---

## 2026-11-23, default lobby URL

State at end of session: the directory is deployed and answers GET
`/v1/lobbies` with an empty list. Not played on a phone.

### Done

- When `HNS_LOBBY_URL` is unset, the APK's `LOBBY_URL` is
  `https://heartsoul-coop-directory.heartsoulcoop.workers.dev`.
  `CoopLobby` appends `/v1/lobbies`, which is the path in
  `tools/lobby-worker/src/directory.js`. `LOBBY_KEY` stays empty, and
  `HeartSoulZeroTier` omits `X-HeartSoul-Key` in that case.
- The phone co-op release tag `recomp-phone-1` is the only ref that
  uploads an APK. Branch compiles still do not.
- JOIN PC HOST still has the baked Windows host address and port. The
  phone-host merge had dropped those two helpers, and the first `port`
  compile stopped there.

### Next

- Wait for the `port` compile to go green, then tag `recomp-phone-1`
  if the phone host, joiner, and lobby path are in this commit.

---

## 2026-11-23, merge phone host and own lobby

State at end of session: `coop/direct-phone-host` is merged into `port`
with a merge commit. Not played on a phone. No APK was uploaded.

### Done

- Merge kept the phone host and joiner from `coop/direct-phone-host`
  (`ae2d14f8`) and the PC-host client from `13a91a0a18`. Phone sessions
  use `HNS_COOP_MODE`. **JOIN PC HOST** sets `HNS_COOP_JOIN=1` and does
  not start the phone bridge.
- Deleted `android/app/src/main/assets/lobby_server.txt`. The PokeWilds
  directory URL is no longer in the tree tip. The old commit that added
  it is still in history. `CoopLobby` reads `BuildConfig.LOBBY_URL` and
  `BuildConfig.LOBBY_KEY`, filled from `HNS_LOBBY_URL` / `HNS_LOBBY_KEY`
  at build time. Those values are not committed and are not logged.
- `tools/lobby-worker/` is the directory. POST and GET `/v1/lobbies`,
  DELETE or POST `{remove:true}` on exit, 90 second expiry, header
  `X-HeartSoul-Key`.
- `CheckTogepi` uses `FLAG_RECEIVED_TOGEPI_EGG` only when `IS_HNS`.
  FireRed, LeafGreen, and Emerald keep the species check without that flag.

### Next

- Deploy `tools/lobby-worker/` and set `HNS_LOBBY_URL` plus `HNS_LOBBY_KEY`
  before expecting JOIN A GAME to list hosts. Steps are in
  `tools/lobby-worker/README.md`.
- CI may compile. Do not publish an APK. Stephen plays on the Thor before
  any release.

---

## 2026-11-22, embedded ZeroTier phone-to-phone co-op

State at end of session: implementation is on branch `coop/direct-phone-host`.
It has **not** been compiled or tested on Android devices yet. Do not call it
working until the Android build and two-phone connection have both passed.

### Done

- Replaced the baked PC World Host with a phone-hosted TCP listener on port
  54555. The hosting phone is role 0 and runs the world; the joining phone is
  role 1. Existing shared-world flags, variables, positions, lead Pokémon,
  readiness sync, and battle-abort behavior remain in `src/coop.c`.
- Adapted the embedded ZeroTier Android bridge from PokeWilds Mobile. Both
  phones join the same controller-less ad-hoc network derived from port 54555.
  The host forwards ZeroTier TCP to its local game listener; the guest opens
  a loopback forwarder to the host's IPv6 address. No lobby directory or
  dedicated game server is added.
- Added HOST A GAME / JOIN A GAME to the Android CO-OP menu. The host shares
  its ZeroTier IPv6 address; the guest enters that address directly.
- Added the libzt Android AAR build script, source patches, Gradle dependency,
  CI build step, and the ZeroTier BSL 1.1 notice.
- Removed the hard-coded ZeroTier World Host endpoint. The native game socket
  now binds only to loopback; peer traffic reaches it through the embedded
  ZeroTier bridge.

### Not yet verified

- Android NDK/Gradle compilation with the newly built libzt AAR.
- Two-phone connection, reconnect behavior, and shared trainer battles.
- ZeroTier's peer path can use relay fallback when direct traversal fails;
  this is the same embedded peer-network approach as PokeWilds Mobile.

### Next

1. Run the Android CI build with libzt and fix build errors.
2. Test two phones joining the ad-hoc network and exchanging TCP data.
3. Test the full co-op flow on the same map and during shared trainer battles.
4. Only then mark co-op as working or publish an APK.


---

## 2026-11-21, co-op coverage and APK compile

State at end of session: still not played on a phone or emulator. Compared
`Shraiyde/pokehns-expansion` `268417e60` (hns-coop-135 / mailbox v14) with
upstream `167aa6d53` (2.0.6) and with this port.

### Done

- The shared flag and var lists already matched the co-op flag audit
  (story hides, badges, trainers, HMs, safari expansions, Alola totem
  gates, monotonic vars). Legendaries, item balls, gifts, and phone-gift
  vars stay on each save.
- A script or cutscene on one phone locks the other phone's field controls
  on the same map and pauses the partner sprite. Wild encounters and
  trainer approach wait until that script ends. Opening the bag does not
  lock the other phone.
- Wild, safari, ghost, and legendary battles clear the co-op battle bit.
  Each player fights those alone.
- Joined trainer battles still use the partner's lead as an AI ally.
  Nuzlocke faint rules apply to this save's Pokémon and skip the injected
  lead. After the battle the real party is put back, with EXP kept on the
  mons that fought. The pre-battle snapshot is no longer overwritten when
  battle init runs a second time.
- The partner's follower stands one tile behind them instead of on the
  same tile.
- The APK job's native compile (32-bit and 64-bit, including `coop.c` and
  `sdl2.c`) succeeded on run 44925213115. Gradle then died on Maven Central
  HTTP 429. The build uses the Google Maven Central mirror and retries only
  that rate limit. The Android library also needs the SDL2 submodule, which
  checkout now fetches.

### Not applicable here

- Battle Tower Link Multi, Safari engineer text, and New Sinjoh script
  mirrors. Those are map scripts and flags (`FLAG_COOP_TOWER_LINK_MULTI_*`,
  `FLAG_RECEIVED_HM_WHIRLPOOL`) that exist only in the co-op ROM fork, not
  in this 2.0.6 data set. Adding them would change the GBA ROM.
- Cable-link party merge and trading over the co-op socket. The World Host
  protocol has no trade payload, and `BATTLE_TYPE_LINK` does not run in
  this port. The partner's lead is the stand-in for a shared trainer battle.
- Link-cable stubs stay as they were.

### Release

- Tag `recomp-coop-1` is `77df6dc8b1`. The tag workflow
  https://github.com/Shraiyde/HeartSoulRecomp/actions/runs/44929485832
  finished green and published the release.
- APK: https://github.com/Shraiyde/HeartSoulRecomp/releases/download/recomp-coop-1/HeartSoulRecomp-recomp-coop-1.apk
  HTTP 200, 97,568,648 bytes. Release page:
  https://github.com/Shraiyde/HeartSoulRecomp/releases/tag/recomp-coop-1

### Next

- Stephen plays that APK on the Thor. No blind adb input.

---

## 2026-11-21, native co-op client

State at end of session: co-op is in the tree and has not been played. No
phone, emulator, or headless game run. The compile check is the Co-op APK
workflow. A GitHub release is published only from the tag `recomp-coop-1`,
and only after that workflow is green.

### Done

- Native World Host client (`src/platform/coop_net.c`, `coop_aes.c`). One TCP
  socket to the baked ZeroTier address, port 27770. HELLO / READY, then
  length-prefixed AES-256-GCM frames with the host stamp (sender 2). The
  join secret is the XOR-obfuscated array shared with world-host-13/14.
  Nothing secret is logged. No mailbox, MQTT, TURN, or Tailscale.
- The socket stays up through short stalls. It does not open a second
  connection while one is up. After a real drop it resumes that same client
  id once, then stops.
- Game session (`src/coop.c`): shared story flags and vars (the same set the
  emulator co-op treated as shared), partner plus follower in reserved
  object slots, rebuilt whenever the overworld is showing, including after
  the sprite was destroyed. Widescreen and tall use the same object camera
  as the player.
- A link that was up at the start of a battle, and then drops or goes quiet,
  ends that battle for both players. Party, money, tile, and facing are put
  back. The trainer flag is cleared if this battle was what set it. The
  battle is not resumed. A later solo battle, after the link is already
  down, plays normally.
- Start screen: JOIN PC HOST, one tap. Sets `HNS_COOP_JOIN=1` for that
  launch. Link-cable code was not retargeted.
- Trainer fights wait until the shared story snapshot has finished. If both
  players are in a trainer's sight, role 0 starts the battle and role 1
  does not, so it is not fought twice. When the partner is nearby their
  lead Pokémon joins as the second battler (double, no link cable). Wild
  battles stay solo. The bottom screen shows P2 while the partner is on
  the same map. A won co-op battle keeps this player's own experience and
  puts the partner's borrowed Pokémon back.

### Known

- Stephen has not played this build. Do not call it working until he has.
- Ruleset blob is applied only before the adventure flag is set, so a
  mid-save join does not rewrite a challenge.
- Legendary and personal gift flags stay local, matching the earlier co-op.

### Next

1. Stephen installs the `recomp-coop-1` APK and joins the PC World Host.
2. Two phones, same map: partner and their Pokémon stay visible through a
   door, a battle, and a menu.
3. Pull the cable mid-battle: both return to the tile they were on, no
   white-out and no prize money.

---

## 2026-10-11, session 8: every PokeDaisy tab, smart fast forward, v0.4.0

State at end of session: **v0.4.0 published as the Latest release**, asset
`HeartSoulRecomp-0.4.0.apk`, sha256 f0171a1f...c92c3585 (versionCode 5).
The owner played the build before the version bump on the Thor ("it was
good") and asked for the release. The 0.4.0 APK itself was installed on the
Thor and launched to a clean start screen by Claude (launch and logcat only;
the owner allowed that), but the owner has not played that exact build. His
save is unchanged throughout (sha 7c704e7a..., backups
`port/out/thor-save-backup-2026-10-11*.sav`). Nothing has run on an RG DS.

### Done

- Session 7's work (fast forward toggle, PARTY / BATTLE / BAG) played on
  the Thor: fine. The owner then asked for the rest of PokeDaisy's features.
- **Tabs MAP, DEX, GUIDE, CARD** and seven tabs in the small font. See the
  README section "The bottom screen" and `port/screenshots/bottom-*.png`.
- **Document channel** (`src/platform/dualscreen_bridge.c`): the view asks
  for one document (`DS_DOC_BAG/GUIDE/DEX/DEX_ENTRY/MAP`, param = species for
  an entry) with `nativeSetWantedDoc`; the game thread builds it at the next
  frame and every 30-240 frames after, with a picture for the entry (front
  pic 64x64, `LoadSpecialPokePic`) and the map (8bpp region map, palette
  from colour 112, 256x160). Pictures carry the doc kind, param and key they
  were drawn for. `HNS_DOC_DUMP=1:0,2:0,3:0,4:155,5:0` dumps them headless
  and writes `doc_K_P.ppm`. The map grid comes through
  `RegionMap_MapSecAt` (new, `src/region_map.c`, PORTABLE) because the
  Johto entries table is sparse and its size is unknown outside.
- **GUIDE** bosses: badge order, the three middle gyms' order variants,
  `VAR_LEAGUE_STATE` for the League, Kanto badges, the League rematch, Red.
  Randomized wild or trainer Pokemon say so instead of guessing (calling the
  randomizer would draw from the game's RNG).
- **Battle extras**: BEST move and switch (power x type chart x STAB x
  attacking stat, switch also divides by the foe's own types against it,
  needs 1.5x the current mon); YOU / FOE column with the trainer's party.
- **Fast forward SMART**: toggle, but normal speed unless the field (without
  the start menu) or a battle's own screen is running
  (`DualScreen_IsSlowScreen`). Not tested with a real trigger.
- **Codex adversarial review x2** (model gpt-6-astra). Fixed: GUIDE called
  `GetTimeOfDay`, which updates time-of-day state and the lighting blend (now
  reads `gTimeOfDay`); a picture could be cached under the wrong document.
  Earlier: disabled species assert in `SanitizeSpeciesId`, so every species
  walk checks `IsSpeciesEnabled`; comma bugs when the first entry is skipped.
- Emulator testing with the owner's save: push it, launch once, then `cat` it
  into the app's save (`adb shell "cat /data/local/tmp/s.sav > ..."`). The
  emulator has no gamepad: use `KEYCODE_ENTER` (START) and `KEYCODE_Z` (A)
  with `--longpress`; `KEYCODE_BUTTON_A` is dropped by SDL.

### Known issues

- The 640x480 layout of the new tabs (MAP, DEX, GUIDE, CARD) was not looked
  at; PARTY / BATTLE / BAG were in session 7.
- Map position for indoor and dungeon maps uses the escape warp's map,
  simplified from `InitMapBasedOnPlayerLocation`; untested inside buildings.
- Matchups and BEST ignore abilities, items and weather.
- RG DS bottom screen over Daijisho still unexplained (session 7).

### Next

1. RG DS: the friend tries 0.4.0; if the launcher still shows, get
   `adb logcat -s HeartSoul` (the displays line).
2. Smart fast forward and the new tabs on a real trainer battle and later
   in the game (Kanto map, national dex).
3. 640x480 pass over MAP, DEX, GUIDE and CARD.

---

## 2026-10-11, session 7: fast forward toggle, bottom screen v2 (tabs)

State at end of session: committed on `port` (not pushed, no release). Debug
APK built and checked on the emulator only. **Nothing from this session has
run on the Thor or an RG DS.** Version is still 0.3.0.

### Feedback that started it

A friend played 0.3.0 widescreen on an RG DS and loved it ("it FEELS
widescreen"). Asks: fast forward as a toggle, not hold. The owner asked for
the bottom screen to be improved after PokeDaisy
(lidor30/pokedaisy v1.0.4, GPL-3.0, Kotlin, a GBA player with a companion
screen that supports HnS 2.0.6). Their photo shows the RG DS bottom screen
**still on the Daijisho launcher with 0.3.0** (black bars, so it is 0.3.0):
the BottomScreenActivity fallback from session 6 did not take over there.

### Done

- **Fast forward toggle** (`src/platform/sdl2.c`, start screen row FAST
  FORWARD, default TOGGLE, `HNS_FAST_FORWARD=toggle|hold`). Each pull of R2
  flips it; release counts only below 8000 so a resting trigger cannot
  double toggle. Not tested with a real trigger (the emulator has none).
  Start screen design height 192 -> 210 for the fourth row (Thor still 5x,
  640x480 still 2x).
- **Bottom screen data** (`src/platform/dualscreen_bridge.c`): party mon
  details (types, ability, nature and its stats, stats, IVs, EVs, EXP into
  the level, friendship, gender, shiny), move category/power/accuracy, foe
  battle types, trainer team count/left, fast forward flag. The bag is a
  second JSON (192K buffer) published every 30 frames only while the BAG
  tab is open. Type chart from `gTypeEffectivenessTable`. Mon icons read
  straight from const data (any thread); item icons are smol-compressed and
  the decoder has static state, so the game thread decodes them on request
  (queue 16, cache 128). Verified headless (64-bit): JSON parses, Mild =
  +SpA -Def. `HNS_BAG_DUMP=1` added.
- **Bottom screen UI** (`BottomScreenView`, `TypeChart`, `BagState`,
  `IconCache`): status line with a fast forward mark, tabs PARTY / BATTLE /
  BAG. PARTY cards with game icons, touch for a summary (stats with IV/EV,
  nature colors, moves, EXP, type matchups, with a TYPE MATCHUPS page when
  they do not fit). BATTLE opens itself on a battle start, marks moves
  SUPER / RESISTED / NO EFFECT by type chart against the foe's battle types
  (abilities like Levitate are not counted), shows foe types and trainer
  balls. BAG: pockets, scrolling list, icon and description. Our own code,
  PokeDaisy is the design reference only (credited in README); none of its
  GPL code is copied (this repo has no license of its own).
- **Debug tooling** (`DebugFiles`): `bottom_bag_preview.json`, and the flag
  file `dump_bottom_screen` saves `bottom_screen.png` each second (overlay
  displays cannot be screencapped). Touch an overlay display with
  `adb shell input -d <displayId> tap x y`; the id is in the `HeartSoul:
  displays` log line and changes when the overlay is recreated.
- **Codex review + adversarial review** (model `gpt-6-astra`; the configured
  `gpt-6.1-sol` is refused on a ChatGPT login). Six findings, all verified
  real and fixed: gLastUsedAbility clobbered by GetMonAbility, bag JSON
  could overflow 64K, TM/HM icons (use GetItemIconPic/Palette), Unown icon
  form (GetIconSpecies), battle cards lost HP at 640x480, summary matchups
  truncated at 640x480.

Checked on the emulator with preview JSON at 1240x1080 (Thor) and 640x480
(RG DS): battle, party, summary, matchups page, bag, TM pocket. Screenshots
were in the session scratchpad, not kept.

### Known issues

- Not run against a live game with a full bag or a real trainer battle on a
  device; the trainer balls and a 100-TM bag are untested live.
- Effectiveness ignores abilities and Tera; it says "type matchups" for that
  reason.
- The 640x480 summary is dense; the matchups need their own page there.
- Session 6's RG DS bottom screen problem is still open (see above).

### Next

1. Owner plays the debug build on the Thor: R2 toggle, the three tabs, a
   trainer battle (balls, SUPER/RESISTED), the bag with real items, and
   that fast forward still sounds right.
2. RG DS: get `adb logcat -s HeartSoul` (the `displays` line) from the
   friend on 0.3.0 to see why the bottom screen stays on Daijisho.
3. Then version 0.4.0 (include/port_version.h, versionCode 5) and a release
   only after the owner plays that exact build.
4. Later tabs from the plan: DEX and GUIDE (0.5), MAP (0.6).

---

## 2026-10-04, session 6: start screen, zoomed out 4:3, RG DS bottom screen

State at end of session: **v0.3.0 published as the Latest release**
(the first full release; 0.1.x and 0.2.0 stay pre-releases), asset
`HeartSoulRecomp-0.3.0.apk`, sha256 a1cecd98...6f09b5. The owner played
0.3.0 on the Thor (widescreen fit, widescreen pixel perfect, zoomed out
pixel perfect; log clean, Presentation path used, 120 loop passes/s) and then
asked for the title screen version and the release. The title change came
after his play-test: checked headless and on the emulator with the exact
release APK, and installed on the Thor, but he has not opened that exact
build. Nothing has run on an RG DS.

### The RG DS report that started it

A friend ran 0.2.0 on an RG DS: it runs, buttons map, fast forward works.
Three problems: the bottom screen stayed on the launcher (Daijisho), the
picture could not be changed from 240x160, and the bars above and below
were white (photo in the owner's chat).

### Done

- **Start screen** (`StartActivity`, `StartScreenView`, `Settings`): the
  launcher opens it before the game. Picture: original 240x160, widescreen
  288x160, zoomed out 288x216. Scaling: fit, pixel perfect (integer),
  stretch. Bottom screen on or off. A preview box and a line of numbers show
  how the picture sits on this screen ("288x216 ON 640x480, AT 2.2x").
  Drawn in the game fonts, d-pad / A / START / touch; the cursor starts on
  START GAME. Choices persist (SharedPreferences "settings") and reach the
  game as `HNS_WIDESCREEN`, `HNS_TALL`, `HNS_SCALE`, set with `Os.setenv` in
  `HeartSoulActivity.onCreate` before SDL starts. Defaults: widescreen on a
  16:10 or wider screen, zoomed out on anything squarer. If the game is
  already running, the start screen forwards straight to it.
- **Black bars**: the renderer cleared to white once and never again. Now
  black, cleared every frame (`src/platform/sdl2.c`).
- **Scale modes**: `HNS_SCALE=fit|integer|stretch` (`ApplyScaleMode`).
  Verified on the emulator: integer gives 480x320 at 2x on 640x480, stretch
  fills 640 columns.
- **Zoomed out / tall screen** (agent, branch `port-tall`, merged):
  `HNS_TALL=1` with widescreen renders 288x216, 28 more lines above and
  below. Only the overworld (`CB2_Overworld` sets `gRenderMarginYLive` each
  frame) draws there; every other screen gets black bars. HBlank DMA and
  interrupts still run once per real line. The field camera ring already
  keeps the 16 metatile rows the tall view needs once the default camera pan
  of 32 is counted; `sMapViewTopRow` in `src/field_camera.c` tracks which way
  the shared row is used. Objects spawn 2 metatiles further out vertically;
  `IsTallSpriteOffscreen` widens the sprite culls. Evidence (agent's
  worktree, gitignored): `~/HeartSoulRecomp-tall/port/out/tall-evidence/`.
  Tall off is byte-identical to before (5850 shots, 32 and 64-bit); margin
  rows checked against the centre of other frames, 0 mismatches over about
  92,000 rows including map connection crossings.
- **Dark cave margins**: an empty window (WIN0H 0..0, which the cave flash
  sets outside its circle) was stretched into the left margin and lit it.
  Fixed in `winCheckHorizontalBounds`. Affected widescreen on the Thor too.
- **Bottom screen on displays without FLAG_PRESENTATION**: the lookup asked
  only for presentation displays, the likely reason the RG DS kept its
  launcher (guess: its second panel is an ordinary display). `SecondScreen`
  now takes any other public display and logs every display once
  (`HeartSoul: displays (game on N): [...]`). Where the Presentation is
  refused, `BottomScreenActivity` is launched onto that display over its
  launcher, window `FLAG_NOT_FOCUSABLE`. Launching it moves key focus to its
  display; `moveTaskToFront` does not bring it back (the game is already the
  front task of its own display), re-starting `HeartSoulActivity` with
  `REORDER_TO_FRONT` does, retried at 0.3/0.8/1.5/3 s while the game lacks
  focus. Verified on the emulator with the debug flag file
  `files/force_bottom_activity`: focus returns to display 0, a tap on the
  bottom screen does not take it, a key press reaches the game.
- **Bottom screen size**: design height 240 instead of 250, so 640x480 draws
  at 2x (was 1x, tiny). Thor stays at 4x. Checked with the battle and
  overworld preview JSONs at 640x480.
- **Title screen version**: "v2.0.6 RECOMP 0.3.0" under PRESS START, drawn
  at startup in the press start graphic's style from `HNS_PORT_VERSION` in
  `include/port_version.h` (`CreatePortVersionBanner`, `src/title_screen.c`).
  `build.gradle` reads versionName from that header: bump it there for a
  release (and versionCode in build.gradle). Headless, 64-bit, against the
  previous build: walk and menus (wide and tall) and a wild battle through
  to a level-up are byte-identical outside the title frames, except one
  24px row of left margin during the battle transition (the window fix).
  The owner's Thor save loads headless in zoomed out.
- Version 0.3.0 (versionCode 4). README updated for 0.3.0.

### Known issues

- Tall screen, from the agent's report: the top margin can show 64px-tall
  objects (SS Anne, cable car) as a strip at once; weather sprites do not
  cover the margins; scripted camera pans larger than about 12px may show a
  stale row at a margin edge; the saved map view covers rows 0..13 only, so
  a script-changed tile in the bottom 4 lines may revert after a menu. Not
  seen in tests; not tested either.
- Tall costs about 25% more render time headless. Not measured on an RG DS.
- The bottom screen activity path has run only on the emulator. If the RG DS
  still shows its launcher, its logcat line `HeartSoul: displays` says why.
- adb `input keyevent` presses down and up within one frame, which the game
  misses; use `--longpress` when testing on the emulator.
- The debug APK grows on incremental builds (packaging leaves gaps); the
  release APK is about 95 MB.

### Next

1. RG DS report on 0.3.0 from the friend: zoomed out, speed (tall costs
   about 25% more render time), and whether the bottom screen opens over
   Daijisho. If not, get `adb logcat -s HeartSoul` (the displays line).
2. Owner's first look at the final title screen on the Thor.
3. The tall-screen known issues above, if anyone hits them.

---

## 2026-10-03, session 5: 64-bit (arm64-v8a), v0.1.1 icon, v0.2.0

State at end of session: **v0.2.0 published** (pre-release) with both
`arm64-v8a` and `armeabi-v7a` in one APK. The owner played it on the Thor,
where Android picks the 64-bit build: continue from a 0.1.x save, walking,
doors, battles with bottom-screen taps, save and reload. v0.1.1 only changed
the icon (dual-screen handheld with a gold heart and a silver soul,
`port/icon/make_icon.py`).

### Why 64-bit

A friend's Anbernic RG DS on GammaOS refused 0.1.x with "isn't compatible
with your phone". The RK3568 can run 32-bit code, so the likely cause is a
64-bit-only firmware; not confirmed (ask for "Supported ABIs" in CPU-Z). He
also hit a battle crash under BlueStacks on a PC, which runs ARM code through
translation and is not a reliable test.

### How it was done

Three agents: data (`port-arm64-data`: asm/data pointers to `ptrvalue`,
`ptr_align`, `space64`, mapjson and mid2agb output, all 394 GBS song
headers), C (`port-arm64-c`: pointer truncation, script/anim/GBS stepping by
`DSIZEPTR`, `SCRIPT_EFFECT_TAG` width), and a lead (`port-arm64`: build,
`port/build64.sh`, `port/gas-at-comments.py` for the AArch64 assembler,
dual-ABI CMake with `game_data.o` and `game_data64.o`, `_setjmp` instead of
`__builtin_setjmp`, text shift-by-32 fix, 16 KB page alignment). All merged
into `port`. Headless 64-bit runs match 32-bit frame for frame.

- **Saves:** the 64-bit build writes SaveBlock1 in the 32-bit layout
  (`save.c`), and `SECRET_BASES_COUNT` is back to 20, so saves move freely
  between the builds. The only pointer in saved data is
  `ObjectEventTemplate.script`, saved as 0 and restored on Continue.
- **32-bit bug fixed on the way:** `SetWordTaskArg` with index 2 or more
  wrote into the next task.
- **Stale generated data:** the first 0.2.0 crashed continuing an outdoor
  save, because this checkout still had `data/layouts/layouts.inc` from the
  old mapjson (8-byte map sizes). Generated map and music data now depend on
  `$(MAPJSON)` and `$(MID)` (`map_data_rules.mk`, `audio_rules.mk`). After
  pulling changes to a generator, a normal build now regenerates.
- Door animation over-read fixed (`src/field_door.c`).

### Tools added

`port/build64.sh` builds `pokehns64` natively in the arm64 Docker image;
`HNS_BIN=pokehns64` runs it with the headless scripts (no qemu).
`port/check64.sh` checks every source for 64-bit pointer diagnostics.
An Android emulator AVD `hns64` (arm64 Android 14, 64-bit only) is on the
Mac for testing what 64-bit-only devices do.

### Known issues

- Not tried on an RG DS. Whether its bottom screen works is unknown.
- `getmoverelearnerstate` emits 4 bytes where C reads 2 (upstream bug).
- `GetSrcPtrFromSprite` still keeps a pointer in halfwords (unreached).
- Earlier known issues from session 4 still apply.

### Next

1. RG DS report from the friend on 0.2.0.
2. Keep collecting crash reports from players.
3. Brightness or colour option for the "dim" night look.

---

## 2026-10-03, session 4: on the Thor, crash fixes, v0.1.0 published

State at end of session: **v0.1.0 is published** as a pre-release at
https://github.com/aabrole/HeartSoulRecomp/releases/tag/v0.1.0 and the repo
is public (the owner's decision, made knowing the APK contains the game's
assets). The owner played the exact release APK on the Thor: continue from
a save, three battles, saved again, no crash, no audio underruns.

### Fixed on the device, with causes

- **Library would not load**: the ROM header structs sat in `.text.*`
  sections; with pointers in a PIC library that made the code segment
  writable, which Android rejects. Kept out of `.text` natively.
- **NPCs piled in a corner, crash leaving the house**: the NTx86 patch padded
  `object_event` for an unpacked struct; Heart & Soul's
  `ObjectEventTemplate` is packed. Padding removed (`asm/macros/map.inc`).
- **Choppy audio**: 47 ms queue topped up once per pass. Now ~100 ms, refilled
  fully each pass. A `perf:` line in logcat every 5 s shows underruns.
- **A on the wrong button**: controllers now map by label.
- **Saves lost**: writes stayed in the stdio buffer and Android kills apps.
  Now fflush + fsync.
- **Crash at the boot intro**: `IsPokemonCryPlaying(NULL)` before any cry.
- **Battles crashed (the "null task 39" corruption)**: found with
  AddressSanitizer on the device. `InitBtlControllersInternal` assigned
  controllers to battlers 2 and 3 in single battles, whose positions are
  `B_POSITION_ABSENT` (0xFF), writing `gBattlerControllerFuncs[255]`, which
  landed in `gTasks` in the clang build. Now bounds-checked under UBFIX.
- New-game preset names were read past their end (harmless, fixed).

### Added

- `port/build-apk.sh --asan`: AddressSanitizer build (wrap.sh, recover mode,
  reports to logcat). `--release`: signed release APK. Key in
  `android/keystore/`, `android/keystore.properties` (ignored; do not lose).
- Bottom screen redrawn in the game's fonts with GBA-style panels
  (`GbaFont.java`, `GbaText.java`, `BottomScreenView.java`); fonts and
  widths are copied from `graphics/fonts` and `src/fonts.c` at build time.
- Widescreen battle margins: BG3's hidden filler rows are replaced with
  ground (`WidescreenFixBattleBg3Map` in `src/battle_bg.c`); summary screen
  pillarboxed (`gRenderPillarbox`). Headless `HNS_CLOCK`, `HNS_LAYER_DEBUG`,
  `HNS_LAYER_HIDE`.
- Android chooses widescreen from the display's shape (4:3 gets 240x160).
- App icon (`port/icon/make_icon.py`), README section, release notes.
- Null-task diagnostics in `RunTasks`, game log lines to logcat.
- Commit email scrubbed to the noreply address before going public.

### Known issues

- Once, after 40 minutes, Android aborted in `ViewRootImpl` /
  `BLASTBufferQueue` ("decStrong() called too many times") on the UI thread.
  Only seen in the ASan build so far; cause unknown. Watch for it.
- Harmless over-reads still present: `sDoorAnimTiles_HnsCerulean` in
  `src/field_door.c` (door animation reads past its tiles).
- Widescreen: battle intro slide shows repeated ground in the margins for a
  second; one-screen move backgrounds do not fill the margins.
- Not tested on an RG DS. Areas past Route 29 untested.
- Release APK cannot install over a debug build (different keys). On the
  owner's Thor: back up the save, uninstall, install, launch once, then
  `cat` the backup into the app-owned save file.

### Next

1. Collect reports from players and from the owner's continued play.
2. Clean up the remaining ASan reads; keep an ASan build handy for crashes.
3. Look into the BLASTBufferQueue abort if it shows again outside ASan.
4. Test on an RG DS through a friend.
5. Day/night "dim" colours: offer a brightness or colour-correction option.

---

## 2026-10-02, session 3: Android app, audio, widescreen, bottom screen

State at end of session: a debug APK builds with all four pieces in it
(`port/build-apk.sh --data`, output
`android/app/build/outputs/apk/debug/app-debug.apk`, about 46 MB). **It has
never run on a device.** Everything below was verified on the Linux build
under qemu, from screenshots, JSON dumps and WAV measurements. Nobody has
listened to the audio yet; captures are in `port/out/audio/`.

Audio, widescreen and the bottom screen were each done by a separate agent
in its own worktree and merged into `port`. Their branches are still there:
`port-audio`, `port-widescreen`, `port-bottomscreen`.

### Android app

- `android/`: Gradle project (AGP 8.5.0, Gradle 8.7 wrapper, compileSdk 34,
  NDK 27.2.12479018, minSdk 26, `armeabi-v7a` only). SDL 2.30.7 is a git
  submodule at `android/SDL2`: run `git submodule update --init --depth 1`
  on a fresh clone.
- All 403 sources compile with NDK clang. One fix was needed: clang ignores a
  transparent union whose members differ in size
  (`union StatChangeFlags`, `include/battle_script_commands.h`).
  `port/clang-check.sh` reruns the check on the Mac.
- C is compiled by CMake (`android/app/src/main/cpp/CMakeLists.txt`) with
  `-marm`. Data is assembled in Docker by `port/android-data.sh` into
  `build/android/game_data.o`. lld cannot apply 16-bit relocations, so
  `port/resolve-abs-relocs.py` resolves the constant ones first.
- `src/platform/sdl2.c`: game controller input (east button is A, south is B,
  right trigger fast-forwards), fullscreen, save in the app's external files
  folder. Widescreen defaults to on for Android.
- A worktree build needs `touch .histignore` because Docker cannot see the
  git history there.

### Audio (`port-audio`)

- The GBS (Game Boy) engine was never dispatched by the C player, and the CGB
  emulation ignored register writes, which is all `src/gbs.c` does. Both
  fixed. Mixer rewritten after Heart & Soul's HQ mixer; output saturates
  instead of exceeding full scale.
- Measured: title, overworld and battle music non-silent and in tune, cries
  correlate with the source samples (r 0.88 and 0.70), no NaN, no sample over
  1.0. Tools: `HNS_WAV`, `HNS_AUDIO_LOG`, `HNS_GBS`, `HNS_SONG`,
  `port/wav-stats.py`, `port/wav-pitch.py`, `port/wav-find-sample.py`.
- Judgment call to confirm by ear: no-resample voices now play 1.357x fast to
  match the GBA build (18157 Hz mixer, 13379 Hz samples). To revert, set
  `GBA_SAMPLE_RATE` equal to `GBA_FIXED_SAMPLE_RATE` in `src/sound_mixer.c`.
- Not done: DC blocking on CGB channels (possible pops), tempo is 0.46% fast
  (60 fps against 59.7275), `MP2K_event_port` is a no-op, GBS stereo untested.

### Widescreen (`port-widescreen`)

- `HNS_WIDESCREEN=1` renders 288x160. Heart & Soul's overworld BG maps are
  256px wide, not 512 as session 1 assumed, so the map layers are widened to
  64x32 tiles behind `PORTABLE` while widescreen is on (`src/fieldmap.c`,
  `src/field_camera.c`, `src/overworld.c`, `src/event_object_movement.c`).
- Verified: outdoors both margins show real map, battle backgrounds fill the
  margins, and the centre 240 columns are byte-identical to the 240-wide run
  over 286 frames. With widescreen off, 33 of 36 frames match the old build;
  the other 3 are the intro, which a ported WIN1 fix now draws (it was the
  black stretch noted in session 2).
- Title, intro, main menu, bag and wall clock are 240 wide with black margins.
- Risks: field window tiles must stay below 0x240 in widescreen (not audited
  for every Heart & Soul window); the battle transition looks different in
  the left and right margins; weather, followers, flash, Pokédex and region
  map are untested. `HNS_WARP=N:G:M:X:Y` warps on a frame, because scripted
  input could not get past the wall-clock event to leave the house.

### Bottom screen (`port-bottomscreen`)

- `src/platform/dualscreen_bridge.c`: snapshot of player, party and battle
  state as JSON, published under a mutex every 4 frames; a tap driver that
  presses the real buttons one frame at a time while watching the actual
  cursor. `HNS_STATE_DUMP=N` and `HNS_TAP=frame:MOVE1..4|FIGHT|BAG|POKEMON|RUN`
  test it headlessly. A tapped move was chosen correctly and won a battle.
- Java: `BottomScreenPresentation`, `BottomScreenView`, `BottomScreenState`,
  `DualScreenBridge`, polled every 100 ms from `HeartSoulActivity`. Does
  nothing on a single-screen device. None of the Java has run.
- Not ported from the reference: the SDL lifecycle patch
  (`android/patches/sdl2-android-lifecycle.patch` in `dualscreen/main`), touch
  bag and party screens, hiding the top-screen battle menu, game font and
  icons, region map.

### Next

1. First device run on the Thor: `adb install -r` the APK, then
   `adb logcat -s SDL libmain AndroidRuntime`. Check in order: boots to the
   title, controls, sound, save and reload, widescreen framing, then the
   bottom screen. If the top screen stays black only when the bottom screen is
   active, apply the SDL lifecycle patch.
2. Have a person listen to `port/out/audio/*.wav` and settle the no-resample
   pitch question.
3. Expect more null-read crashes in areas not yet exercised. Each has a clear
   backtrace under `port/debug-headless.sh`.
4. Widescreen follow-ups listed above, then touch bag and party screens.
5. RG DS: untested, and it is unknown how GammaOS exposes the second screen.
6. Release signing and a distribution decision (see `PORT_PLAN.md`).

---

## 2026-10-02, session 2: native build boots and plays

State at end of session: Heart & Soul builds as a native 32-bit ARM Linux
program and plays under qemu with no display. Verified from screenshots
(`port/screenshots/`): copyright and credit screens, title screen (v2.0.6),
new-game intro, challenge settings menu, overworld in New Bark Town, start
menu, bag, saving, Continue from the save, walking, and a full wild battle
through to experience gain. No Android build yet. Nobody has heard the audio.

### How it was done

The plan changed from session 1. Instead of redoing the dual-screen repo's
693-file delta by hand, the port-only changes of
`ntx86/pc_port-expansion-test-attempt` (an expansion 1.17 native port, work
in progress) were isolated by diffing it against its rh-hideout base
`c681a75d43` and applied with `git apply --3way`. 283 files, 27 conflicted.
Conflicts were resolved by keeping Heart & Soul's code and taking the port's
pointer handling. That branch was never finished upstream, so several of the
fixes below are bugs it still has.

### How to build and test

```
docker build -t hns-port port/docker        # once
port/build.sh                               # builds ./pokehns32
HNS_SHOT_EVERY=100 port/run-headless.sh 3000   # screenshots in port/out
port/sheet.py port/out/sheet.png 100 200 300   # contact sheet (macOS)
port/debug-headless.sh 3000                 # same run under gdb, backtrace on a fault
```

Headless controls are environment variables, documented at the top of
`src/platform/sdl2.c`: `HNS_INPUT` (scripted buttons), `HNS_SHOTS`,
`HNS_SHOT_EVERY`, `HNS_TEST_BATTLE` (gives a Cyndaquil and starts a wild
battle on that frame). A frame that takes over 20 seconds is reported with
the stuck program counter; resolve it with `port/out/symbols.txt` or
`arm-linux-gnueabihf-nm -n pokehns32`.

The save file is `port/out/pokeemerald.sav`. It currently holds a save in the
player's bedroom, which the battle and Continue tests start from. Input
scripts that reach each state are in `port/out/*.txt` (not committed, since
`port/out` is ignored; the timings are in this log's git history of commands
only, so regenerate them if lost: mash A to frame 4900, R ten times from 5000
to leave the challenge menu, then A with an occasional START).

### Fixes made, with the cause of each

- `tools/preproc`: any identifier ending in `DUMMY(` was treated as an
  incbin. Now requires a whole identifier.
- Makefile: cross-compile support (`PREFIX`, `ARCH_CFLAGS`, `ARCH_ASFLAGS`),
  no `-m32` or leading underscores off x86 and Windows.
- `src/decompress.c`: the smol decoders were copied to the stack and run
  from there. Native builds now call them in place.
- `src/platform/gba_easy_draw.c`, `gba_fast_draw.c`: loop counters were plain
  `char`, which is unsigned on ARM, so the scanline loop never ended.
- `include/sound_mixer.h`: the native mixer assumed 12 channels. Heart & Soul
  uses 15, and the struct is a second view of `struct SoundInfo`.
- `src/m4a.c`: 16, 3, 11, 1 tracks for the four music players, matching
  `sound/music_player_table.inc`.
- Script engine: commands, specials and natives that report effects were
  marked by adding `ROM_SIZE` to their function pointer. Natively that jumps
  into data. They are now marked with `SCRIPT_EFFECT_TAG` (bit 1) and
  untagged before the call. See `include/script.h`. This relies on functions
  being 4-byte aligned, so ARM builds must use `-marm`.
- Null reads the GBA tolerates because address 0 is the BIOS:
  `gSaveBlock1Ptr` on the copyright screen (`src/main.c`) and `gBattleStruct`
  in `Task_HandleMonAnimation` on the main menu. Expect more of these. Each
  shows up as a segfault with a clear backtrace.
- GBA-only code guarded or replaced: mGBA debug registers (log lines now go
  to stderr), the RAM copy of the sound mixer, multiboot, `ReInitializeEWRAM`,
  RTC writes, linker-script symbols. `BitUnPack` added to
  `src/platform/bios.c`.

### Known gaps

- **Audio is unverified.** The mixer runs every frame without crashing, but
  the native sound engine is a C port of the vanilla engine and Heart & Soul
  changed its assembly engine heavily (1,871 lines in `src/m4a_1.s`,
  including the GBS Game Boy music engine in `src/gbs.c`). Next step is to
  write the mixer output to a WAV in headless mode and listen.
- **Saves are not GBA-compatible.** The native build does not use the GBA
  struct ABI (`-mabi=apcs-gnu`), so struct sizes differ. The size assertion
  for `ChallengeSettings` is skipped natively.
- **The clock cannot be set.** RTC writes are no-ops; time comes from the
  host.
- About 280 frames after the credit screens are black before the title
  appears (frames 420 to 700). Not checked whether the Heart & Soul intro
  should be drawing there.
- Only the first map, one wild battle and the bag were exercised. Warps,
  trainers, followers, the Pokédex, shops, evolution and link features are
  untested.
- `port/clang-check.sh` does not work yet: it preprocesses with gcc's glibc
  headers, which clang rejects. It needs to preprocess with clang. Whether
  expansion's GCC-specific code compiles with NDK clang is still unknown and
  is the main risk for the Android step.
- 64-bit is not attempted. Heart & Soul's own data still has `.4byte`
  pointers in places.

### Next

1. Audio: dump to WAV, listen, then port the engine differences that matter.
2. Android: get the tree compiling with NDK clang for `armeabi-v7a` with
   `-marm`, using `dualscreen/main`'s `android/` project and CMake file as the
   starting point. Needs `arm-none-eabi` binutils on the Mac or the Docker
   image for the data objects. Mac has NDK 27.2.12479018 at
   `/opt/homebrew/share/android-commandlinetools`.
3. Bring over `dualscreen/main`'s widescreen PPU (`gRenderWidth`,
   `gRenderMargin` in `include/platform.h`, `src/platform/sdl2.c`,
   `gba_easy_draw.c`). Its platform files descend from the same NTx86 layer,
   so diff them file by file against ours.
4. Then the bottom-screen bridge.
5. Keep extending the headless tests: leave the house, a trainer battle, a
   Pokémon Center.

---

## 2026-10-02, session 1: project setup and research

State at end of session: repo created, nothing built yet, no code changed.

### Done

- Cloned PokemonHnS-Development/pokehns-expansion (full history, 21,250
  commits) to `~/HeartSoulRecomp`. `master` is at `167aa6d537` "2.0.6".
- Remotes set up: `upstream` (HnS), `dualscreen` (Goldoire), `native`
  (fuddlesworth), `ntx86` (NTx86), `origin` (aabrole/HeartSoulRecomp,
  private). Reference branches fetched: `dualscreen/main`, `native/vanilla`,
  `ntx86/pc_port-expansion-test-attempt`,
  `ntx86/pc-port-widescreen-extended-tilemap`,
  `ntx86/pc-port-widescreen-hackjob2`, `ntx86/pc_port-64-bit-lactozilla`.
- Created `port` branch with `PORT_PLAN.md`, this log, and `CLAUDE.md`.

### Verified facts

- HnS source is fully public and is a fork of pokeemerald-expansion.
  `include/constants/expansion.h` says 1.15.2, untagged. 388 files in
  `src/*.c`. Builds with `MODERN=1`, gnu17. agbcc is not supported.
- `DISPLAY_WIDTH` is 240 in `include/gba/defines.h`, unchanged from vanilla.
- `dualscreen/main` is 331 commits past its pret base, builds Android
  `armeabi-v7a` only (`android/app/build.gradle`), NDK 26.3.11579264, minSdk
  21, and its desktop `Makefile_pc` uses `-m32`. So it is a 32-bit port.
- Its widescreen is a PPU margin: `WIDESCREEN_MARGIN 24` in
  `include/platform.h`, giving 288x160.
- Its shared-code delta against its pret base is 693 files, 14,671
  insertions, 3,942 deletions (excluding `src/platform`, `include/platform`).
- Its `mods/pokemonHnS` is a data-only import: 0 maps, 0 scripts. Not usable
  as a route to real HnS.
- HnS and `dualscreen/main` share pret history. Merge base is `208f97e1e3`
  (2025-08-21). HnS and `native/vanilla` merge base is `041ff613ca`
  (2026-04-07).
- This Mac has cmake, ninja, sdl2, adb and Homebrew. It does not have
  `arm-none-eabi-gcc`. No Android NDK found under `~/Library/Android/sdk/ndk`.

### Not verified

- Whether a plain `git merge dualscreen/main` into HnS is workable. Guess: no,
  too many conflicts in expansion-rewritten files. A file-by-file transplant
  is the assumed approach.
- How GammaOS on the RG DS exposes its second screen to Android apps.
- Whether the Thor needs anything beyond what `dualscreen/main` already does.
  Its README names the Thor as the target, so probably not.

### Next

1. Install the ARM toolchain (`brew install --cask gcc-arm-embedded` or
   `brew install arm-none-eabi-gcc`, check `INSTALL.md` for what expansion
   wants) and libpng, then build the stock ROM from `master`. This is M0.
2. Install Android NDK 26.3.11579264 and build `dualscreen/main` unchanged in
   a separate worktree (`git worktree add ../hns-dualscreen-ref
   dualscreen/main`). Install on the Thor. This proves the Android toolchain
   and gives a working reference to compare against. It needs a vanilla
   Emerald ROM on the device.
3. Produce the transplant inventory: `git diff 208f97e1e3 dualscreen/main
   --stat -- src include data asm sound` and sort the 693 files into (a)
   mechanical guards, (b) real logic changes, (c) dual-screen hooks. Save it
   as `docs/port/transplant_inventory.md`.
4. Copy `src/platform`, `include/platform*`, `android/` onto `port` and start
   on the build system (M1).


## 2026-11-26: PkMultiplayer native transport port

### Implemented on `coop/pkmultiplayer-hns-adapter`

- Direct-phone handshake advanced to protocol v4; the legacy Windows PC-host
  handshake remains unchanged.
- Added an application-level reliable sequence independent of the AES-GCM
  per-connection nonce counter.
- Added contiguous receive sequencing, duplicate suppression, highest-
  contiguous ACKs, and rejection of ACKs beyond the last transmitted sequence.
- Added a 2,048-message unacknowledged outbox. Messages are retained until ACK,
  replayed after reconnect, and retried every second on a live connection.
- Phone inbound queue overflow no longer evicts an older gameplay event or
  ACKs a message it could not enqueue. The sender's replay recovers it once
  the game thread drains the queue.
- Preserved the legacy PC transport's previous message framing and queue
  semantics by guarding the new ACK/replay path to direct-phone sessions.
- Guest reconnect attempts now use backoff and continue for up to 60 seconds;
  the host continues accepting the pinned peer after a disconnect.
- Added the branch to the Android co-op compile workflow earlier in this port.

### Not verified

- No NDK/Gradle compile result is available from the connected GitHub status
  endpoint for these commits.
- No two-device Android session, forced disconnect, queue-saturation, or
  reconnect test has been run.
- The outbox is process-memory only; process death loses the session.
- The transport still uses one TCP stream, not PkMultiplayer's separate
  reliable and fast ENet channels.
- Game-layer parity is incomplete: synchronized battle actions, deterministic
  lockstep battle RNG/state reconciliation, chat/emotes, ready/invite flows,
  joint wild catches, feature negotiation, and event-specific idempotency
  still need to be ported and tested.


## 2026-11-26: Native battle and party synchronization pass

- Phone protocol advanced to v4 with authenticated per-process session IDs and
  receive watermarks. A reconnect resumes only when both peers recognize the
  same process sessions; a restarted peer starts a clean transport session.
- Added a session-reset notification. A peer restart clears stale story/party
  readiness and aborts a battle already in progress instead of resuming it
  against a process that lost its battle state.
- Raised the phone payload ceiling to 512 bytes and added a complete three-mon
  partner-party snapshot. Shared trainer battles now inject the partner's
  full three-Pokemon team, not a generated copy of only the lead.
- Added battle epoch/turn-tagged action messages. The local-left player's
  selected move, switch or item is sent to the other phone and injected into
  its remote-right battler. Ally move targets are mirrored between the two
  local-left/remote-right views.
- Added host-authoritative turn-start synchronization for both SFC32 RNG
  streams. The guest pauses the battle tick until the host's seed for that
  turn arrives; turn-tagged packets one step ahead are buffered.
- These changes are source-level implementation only. They have not yet
  passed an Android NDK/Gradle build or a two-device battle test. Battle
  checksums/resync, complete game-event idempotency, and a separate fast
  movement channel remain outstanding.


## 2026-11-26: Authoritative battle setup and divergence guard

- Expanded the phone payload cap to 1,024 bytes for battle setup snapshots.
- Before the first shared turn, the host now sends the generated enemy party,
  active opponent BattlePokemon records, battler count, battle flags/weather,
  and the battler party-order/switch/action bookkeeping arrays. The guest
  applies these in canonical host/guest order before the first turn seed.
- Added a canonical FNV-1a battle-state checksum covering both player teams
  in host/guest order, active battlers, opponent party, battler party indices
  and order, selected-move/target/item state, weather, battle flags and result.
- The guest compares the checksum at every turn start before applying the
  host's RNG state. A mismatch triggers the existing safe battle-abort path.
  Full mid-battle state repair is still outstanding.
- The trainer approach now uses either player's remote/local approach tile so
  both phones trigger the same shared trainer battle. It waits for story sync
  and both full party snapshots when the partner is nearby, while preserving
  solo trainer battles when the partner is elsewhere.
- These are still uncompiled source changes. No Android build or two-device
  co-op test has been verified.


## 2026-11-26: Save identity and battle setup parity

- Added a save fingerprint using trainer ID, player name/gender, and the save
  encryption key. Both peers pin the other save identity; a save-slot change
  sends a session reset, invalidates stale party/story readiness, and aborts
  any battle that was running against the old save.
- The host-authoritative first-turn setup now includes the generated enemy
  party, active opponent battlers, battler count, battle flags/weather, and
  per-battler party order/index, switch, move-target and item-target arrays.
  The guest applies the host state with player battlers mirrored into its
  local-left/remote-right layout.
- The setup packet is bounded to 1,024 bytes with a compile-time size check.
  The guest validates the double-battle shape before accepting it.
- Per-turn checksum validation now covers the canonical player teams, active
  battlers, enemy party, battle weather/flags/outcome, and battler party/action
  bookkeeping. A mismatch safely aborts rather than letting two divergent
  battles continue.
- No Android compile or two-device verification has been completed.


## 2026-11-26: Independent movement channel and partner lease

- Advanced the phone wire protocol to v5.
- Added a separate AES-GCM UDP channel for latest-only movement snapshots.
  UDP HELLO/READY packets prove the current device and process-session IDs;
  movement packets have their own nonce domain and monotonically increasing
  snapshot sequence. Replayed/out-of-order snapshots are dropped.
- The host learns the guest's UDP endpoint only from an authenticated HELLO;
  the guest pins the host endpoint from an authenticated READY. The channel
  is polled at 20 Hz and sends snapshots at up to 10 Hz.
- TCP remains the reliable gameplay channel. If UDP cannot be established or
  goes stale, movement falls back to the existing TCP snapshot path.
- The peer device pin now expires after the same 60-second reconnect window,
  allowing a different partner to join after the previous session expires.
- UDP forwarding through the Android ZeroTier bridge is not yet verified.
  The fallback keeps the existing connection usable if the bridge does not
  carry UDP. No Android compile or two-device test has been completed.


## 2026-11-26: Per-turn authoritative battle-state snapshots

- Increased the reliable payload budget to 4,096 bytes.
- The host now sends a canonical battle snapshot at every turn boundary,
  rather than only the initial setup. It includes both player parties, enemy
  party, all active battlers, side state/timers, protect/special status,
  event and battler state, delayed effects, party/battler indexes, action
  bookkeeping and battle metadata.
- The guest remaps host/guest battlers and party slots to its local layout
  before applying the snapshot. The turn seed/checksum is then validated
  against the repaired state before the guest continues.
- The snapshot is bounded by a compile-time size assertion and runtime
  payload checks. This is turn-boundary repair; it does not roll back a move
  already executing.
- Android compile and device testing remain unverified; the next required
  validation is a real two-phone battle covering switches, items, status
  effects, delayed attacks and reconnect during a turn.


## 2026-11-26: Final battle-state barrier

- Added a host-authored final battle snapshot with a checksum. It carries the
  same canonical party, battler, side-effect and action state as the per-turn
  snapshots, including the final battle outcome.
- The guest applies the final snapshot, sends a reliable ACK, and waits for the
  host's reliable commit before leaving the battle. This closes the gap where
  the last turn could diverge without another turn-start snapshot to repair it.
- The final wait is bounded at 60 seconds; failure enters the existing safe
  battle-abort/party-restore path rather than hanging indefinitely.
- Still requires Android build and two-device validation.


## 2026-11-26: Shared-battle action ownership and private inventory

- Remote switch actions now translate the sender's local party slot (0-2) into
  the partner's shared party slots (3-5).
- Battle item actions are simulated by both peers, but the remote battler's
  RemoveBagItem call is suppressed on the non-owning phone. The player who
  selected the item consumes it from their own bag; the other phone mirrors
  the battle effect without consuming its local inventory.
- The reliable staging queue now holds 2,048 messages, matching the session
  journal capacity, rather than the former 48-message staging limit.

- Phone protocol bumped to v6 after adding the canonical per-turn battle snapshot and final snapshot ACK/commit barrier; v5 clients are intentionally rejected.


## 2026-11-26: UDP candidate reservation hardening

- The host Android UDP relay now gives an unauthenticated endpoint a fixed
  4.5-second authentication window. Continuous invalid datagrams no longer
  refresh the candidate timeout and reserve the relay against the real guest.
- Once the native protocol returns its authenticated UDP READY response, the
  endpoint is pinned as before and uses the normal 4.5-second idle timeout.
- Corrected the libzt locking comment: native calls are serialized, but not
  every operation is non-blocking.
- Still source-reviewed only; Android compilation and two-device UDP tests
  remain outstanding.


- Follow-up hardening: after its 4.5-second candidate window expires, that
  unauthenticated source endpoint is quarantined for 30 seconds. This prevents
  a continuous sender from immediately reacquiring the candidate slot on its
  next datagram; a different legitimate endpoint can still authenticate.


## 2026-11-26: Keep ZeroTier status checks off the UI thread

- `status()` now reads a cached transport state instead of calling libzt
  directly. A background monitor refreshes the cache once per second.
- This prevents the status/UI polling path from blocking behind the serialized
  native lock when a ZeroTier connect operation is waiting on its timeout.
- Source-level change; Android build and device responsiveness remain untested.


## 2026-11-26: Verify UDP relay against native authentication

- Traced the Android host relay through the native `HandleUdpDatagram`
  implementation. The native host accepts UDP HELLO only after AES-GCM
  authentication, protocol/version/length checks, and matching peer process
  session plus client identity; only then does it emit UDP READY.
- Confirmed the guest accepts UDP READY only when the echoed host process
  session and client identity match its pinned TCP peer. Movement datagrams
  are then accepted only from that authenticated endpoint.
- Confirmed the Android relay's first-candidate reservation is compatible
  with this handshake: the host cannot pin a candidate merely because it
  sent an unauthenticated datagram; the first local response that causes
  pinning is the native READY response after validation.
- Reviewed reliable TCP queue behavior under receive-queue saturation:
  phone sessions advance their receive watermark only after successful queue
  insertion, so the sender receives no ACK for a dropped event and replays
  it. Duplicate events are acknowledged without being enqueued twice.
- This is a source-level protocol audit. No compiler, CI result, or physical
  two-device test is available yet.


## 2026-11-26: Audit battle snapshot bounds and remote action mapping

- Traced the host/guest canonical battler and six-slot party mapping through
  snapshot serialization and application. Guest snapshots swap the local
  player halves and mirror the two active player battlers into the same
  canonical order on both devices.
- Checked every battle snapshot append/apply helper: appends are capped at
  4,096 bytes, incoming copies check the remaining payload length, the
  compile-time aggregate bound reserves four bytes for the final checksum,
  and the receiver rejects wrong epoch, turn, battler count, or non-co-op
  battle flags.
- Confirmed remote switch actions map the sender's local party slot 0-2 into
  the receiver's partner half 3-5 on either host or guest. Item consumption
  remains owner-only in the battle action path.
- No new source change was justified by this review. The remaining risk is
  behavioral: these invariants still need an actual two-device battle covering
  move targets, switches, items, status effects, and battle completion.


## 2026-11-26: Reject unsupported lobby ports

- The lobby parser previously accepted any valid TCP port and exposed it in
  the lobby model, but the join bridge always connects to the fixed native
  co-op port 54555. A stale or malformed directory entry could therefore be
  listed even though the app would silently join the wrong port.
- The parser now accepts only the port this build actually listens on.
- This is a defensive compatibility check; branch-push CI is compiling the
  current branch, but the new Java change has not yet been verified by CI.
