# PkMultiplayer compatibility work for Heart & Soul

## Decision: port the multiplayer design, not the Lua mod

The upstream `Mochx07/PkMultiplayer` project is a Gen2Recomped mod. Its
`manifest.json` declares the Gen2Recomped mod API, its entry point is
`main.lua`, and that entry point patches Lua modules such as
`src.world.OverworldController`, `src.battle.BattleState`,
`src.link.Protocol`, and `src.ui.Menu`. Its README explicitly limits the
mod to Gen 1 and Gen 2 games.

HeartSoulRecomp is a native Android port of Pokémon Heart & Soul 2.0.6, a
Gen 3 / pokeemerald-expansion game. It is compiled C with SDL2 and the Android
NDK; it does not run Gen2Recomped, LÖVE, the Gen2Recomped Lua mod loader, or
the Lua module APIs PkMultiplayer patches. Installing the PkMultiplayer ZIP
in HeartSoulRecomp therefore cannot work, and transplanting its generated
Lua patches would not add multiplayer to this engine.

The compatible route is to port PkMultiplayer's *player-facing behaviour and
network/session design* into HeartSoulRecomp's native C implementation. Keep
the PkMultiplayer fork as the reference implementation and keep all HnS
engine-specific changes in this repository.

## Existing HeartSoulRecomp foundation

The `port` branch already contains native co-op game logic in
`src/coop.c`, an encrypted socket transport in
`src/platform/coop_net.c`, the Android ZeroTier bridge, and the Android
co-op UI. The game logic synchronizes shared Johto/Kanto progression,
positions, the partner's lead Pokémon, readiness, and trainer battles. The
phone-host / join flow and lobby directory are implemented separately from
PkMultiplayer's desktop/LÖVE mod.

This means the work is an adaptation and completion of the existing native
co-op, not a rewrite of the Android port and not a second networking stack.

## PkMultiplayer features to map onto HnS

| PkMultiplayer behaviour | HeartSoulRecomp implementation direction |
|---|---|
| Partner avatar and follower | Keep the existing reserved object-event slots and position snapshots. |
| Shared story and world items | Keep HnS's flag/variable allowlists; add idempotent event records for rewards and map/script changes. Personal catches, gifts, purchases and save-specific choices remain local. |
| Lockstep 2-vs-2 trainer battles | Extend the existing trainer double-battle path with explicit turn/action messages and deterministic battle-state checks. Do not import Gen 2 battle structs or AI. |
| Co-op wild encounters and joint catches | New HnS battle messages and an explicit ownership decision; each save must retain its own caught Pokémon unless both peers agree to the transfer. |
| Chat and emotes | Add small bounded message types and Android touch UI; keep them outside the battle simulation. |
| Ready / big-battle invite | Reuse readiness state and add an invite/accept/decline state before shared trainer scenes. |
| Reconnect and replay | Port the session sequence, acknowledgement, bounded outbox, duplicate suppression, and resume handshake described below. |
| Cable Club trade / P1-vs-P2 | Adapt only after the HnS battle/trade data model is mapped; PkMultiplayer's Gen 2 LinkState cannot be reused as-is. |
| Voice chat, visitors, Discord | Out of the first compatibility milestone. These depend on platform APIs or multi-peer assumptions not present in the current two-phone HnS implementation. |

## Network/session protocol work

PkMultiplayer's `src/coop/CoopNet.lua` uses ENet with two channels:
reliable ordered messages for gameplay and unordered/sequenced snapshots for
positions. Its session remains alive across a dropped connection: reliable
messages receive monotonically increasing sequence numbers, remain in a
bounded outbox until acknowledged, and are replayed after a `coop_resume`
exchange. Duplicate sequence numbers are discarded. A reconnect is accepted
only for the same save identity and session.

HeartSoulRecomp currently uses encrypted, length-prefixed TCP frames over the
embedded ZeroTier path. TCP already provides reliable ordered delivery while a
socket is alive, so do not add ENet as a second transport. Port the *session
layer* above the existing TCP framing:

1. Define a protocol version and feature bitset in the initial HELLO/READY
   exchange. Reject incompatible gameplay versions before either side changes
   save state.
2. Give each co-op save a stable identity and each running session a fresh
   session identifier. A restarted or unrelated save must not inherit a
   previous peer's pending events.
3. Add a reliable message sequence, highest-contiguous-received ACK,
   bounded retransmission outbox, and duplicate suppression. Persist only
   events that must survive app suspension/restart; never persist ephemeral
   position snapshots.
4. Add a resume handshake carrying session identity and each side's last
   received sequence. Replay only missing reliable messages, in order.
5. Keep position updates coalesced/latest-only so stale movement never blocks
   story, battle, or control messages. Phone protocol v5 now uses an
   authenticated AES-GCM UDP datagram channel for movement, with sequenced
   snapshots and a TCP fallback when the Android bridge cannot carry UDP.
6. Preserve the existing background hold and disconnect/battle-abort safety
   rules. A reconnect must not duplicate rewards, trainer victories, catches,
   or trade commits.
7. Add explicit message-size limits, queue-overflow handling, malformed-frame
   rejection, timeout tests, and logs that never expose session keys or lobby
   secrets.

## Audit of the current native transport (verified on `port`)

The current implementation already separates the game thread from socket
I/O with a worker thread and mutex-protected queues. It encrypts frames with
AES-GCM, carries a per-frame counter in the nonce, and rejects a counter that
is not newer than the last one received on that connection. It sends the
latest position snapshot before draining up to eight queued gameplay
messages per flush. The game layer currently uses these message types:

| Message | Current use |
|---|---|
| 16 `POSITION` | Latest player map / coordinates / facing / follower / avatar snapshot. |
| 17 `WORLD_FLAG` | Shared flag id and value packed into one word. |
| 23 `SIO_MULTI` | Reserved in the header; no producer/consumer found in the audited `src/coop.c`. |
| 24 `WORLD_VAR` | Shared variable id and value packed into one word. |
| 25 `WORLD_READY` | Story-ready, battle-abort, background and foreground signals. |
| 26 `WORLD_CONFIG` | Partner lead Pokémon summary. |
| 27 `WORLD_CONFIG_ACK` | Declared for config acknowledgement; no active consumer found in the audited receive switch. |
| 28 `SIO_RESYNC` | Declared; no active producer/consumer found in the audited `src/coop.c`. |
| 29 `WORLD_CONFIG_BLOB` | Host sends challenge settings with a sequence; guest applies them before adventure start and replies with message 27. |
| 30 `WORLD_RESYNC` | Requests a shared-world rescan. |
| 31 `POSITION_RESYNC` | Requests position resync / shared-world rescan. |

The current direct-phone transport is protocol version 5:

- The handshake carries authenticated process-session IDs and receive
  watermarks. Both peers resume only when each recognizes the other's current
  session; otherwise both discard stale transport queues and begin fresh.

- Every reliable gameplay message receives a process-lifetime 32-bit sequence
  number. The existing frame sequence field is now consumed by the receiver.
- The receiver accepts only the next contiguous message, suppresses duplicates,
  and ACKs the highest contiguous sequence it has actually queued for the game.
- The sender retains unacknowledged messages in a 2,048-entry outbox, replays
  them after reconnect, and retries outstanding messages every second while
  connected. This also recovers from temporary inbound-queue saturation.
- The 48-entry game-thread queues remain bounded. A full phone inbound queue
  no longer discards an older event or ACKs the new one; the sender retains and
  retries it. The legacy Windows PC transport keeps its old framing behaviour.
- The guest retries reconnects with backoff for up to 60 seconds. The host
  continues listening after a disconnect; the peer identity pin is released
  after the same 60-second reconnect window.

Remaining gaps:

Game-layer work now added on top of that transport:

- Full three-Pokemon partner party snapshots are sent as fixed-layout
  `struct Pokemon` records. Both phones run the same HnS build/ABI; the
  payload is bounded by a compile-time 512-byte assertion.
- Shared trainer battles now wait for the partner's selected action on the
  right-side battler. Move, switch, and item selections are carried with the
  turn number; ally move targets are mirrored between each phone's local-left
  and remote-right view.
- The host sends both deterministic RNG stream states at the start of every
  battle turn. The guest pauses battle selection until it has applied the
  authoritative turn seed, so AI and battle resolution start from the same
  random state.
- At every turn boundary, the host sends a canonical battle snapshot: both
  player parties, the enemy party, all active battlers, side statuses/timers,
  protection and special-status flags, action/party bookkeeping, delayed
  effects, and battle metadata. The guest remaps host/guest battler and party
  indexes into its local layout before applying the snapshot and checking the
  turn checksum. The snapshot is bounded by a 4 KiB compile-time assertion
  and runtime payload checks, and repairs state before RNG/AI work.
- Battle action and seed packets are tagged with a battle epoch and turn.
  One-turn-ahead packets are buffered to tolerate small timing differences
  between phones.
- A peer process restart is distinguished from a reconnect. The old
  in-memory session is discarded, both games receive a session-reset event,
  and a battle in progress is aborted rather than continuing with mismatched
  battle state.
- Each game also publishes a save fingerprint derived from the trainer ID,
  player name/gender and save encryption key. The peer pins that identity;
  changing save slots resets the co-op game session and aborts an active battle.
- Movement now has a separate AES-GCM UDP channel with session-bound endpoint
  authentication, latest-only sequence numbers and replay/order rejection.
  Gameplay remains on reliable ordered TCP. If the UDP path cannot be opened,
  the transport falls back to TCP position snapshots.

- The session state and outbox live in process memory; a full app/process
  restart starts a new transport session and does not restore unacknowledged
  events from disk.
- The Android ZeroTier bridge path has not yet been verified to forward UDP.
  If it does not, the built-in TCP fallback preserves movement but can still
  experience head-of-line blocking.
- Message-specific idempotency keys for irreversible game effects (rewards,
  catches and trade commits) are still needed above transport. Battle state is
  repaired and checksummed at turn boundaries; mid-move rollback is not yet
  implemented.
- The existing story rescan and config blob are useful recovery mechanisms,
  but they do not replace per-event identity and replay protection for
  one-time rewards or battle outcomes.

The remaining transport findings and game-layer gaps need host/guest tests.
The new ACK/replay code is a first port, not a verified replacement for every
PkMultiplayer transport behaviour.

## Implementation order

1. Audit the current `port` branch's `coop_net.c`, Android ZeroTier bridge,
   `coop.c`, and host/join UI together. Document every message type and its
   producer/consumer before changing the wire format.
2. Implement protocol negotiation and session sequence/ACK/resume in the
   existing native transport. Add host/guest tests for duplicate, missing,
   reordered, oversized, and replayed messages.
3. Verify two Android devices can host/join, exchange positions and story
   state, reconnect after Wi-Fi loss, and reject a different save/session.
4. Stabilize the existing shared trainer battle before adding new battle
   actions. Test disconnects during each battle phase and verify both saves
   roll back safely.
5. Add PkMultiplayer-style chat, emotes, ready/invite and joint wild catches
   incrementally, each behind negotiated feature bits.
6. Test on actual Android devices. Only publish an APK through a GitHub
   Release after the two-phone test passes; a green compile alone is not a
   multiplayer test.

## Current phone protocol

The Android phone-to-phone handshake now advertises protocol version 5.
Both peers authenticate persistent device identity and process-session
identity with HMAC proofs. Receive watermarks allow same-process reconnects to
resume and replay unacknowledged events; a restarted process begins a clean
session. Save fingerprints are pinned at the game layer, and the device pin is
released after the 60-second reconnect window so a new partner can join.
The independent UDP movement channel uses AES-GCM and authenticates the
peer's session/device IDs before accepting its endpoint. The Windows PC-host
handshake remains on its legacy protocol. Both phones must use the same
protocol-v5 build. No Android build or two-device test has yet been verified.

## Reference source

- PkMultiplayer fork: https://github.com/Shraiyde/PkMultiplayer
- Upstream PkMultiplayer: https://github.com/Mochx07/PkMultiplayer
- Its transport/session reference: `src/coop/CoopNet.lua`
- Its game-specific co-op logic: `src/coop/CoopManager.lua`,
  `CoopStory.lua`, `CoopBattle.lua`, and `CoopLink.lua`
- HeartSoulRecomp native co-op: `src/coop.c`,
  `src/platform/coop_net.c`, Android ZeroTier bridge, and Android co-op UI

PkMultiplayer is MIT-licensed according to its repository README. Any source
code copied or adapted from that project must retain its required copyright
and license notices. This document describes the compatibility boundary; it
does not claim that the two projects already interoperate.
