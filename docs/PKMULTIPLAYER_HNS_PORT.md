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
3. Add a 64-bit reliable message sequence, highest-contiguous-received ACK,
   bounded retransmission outbox, and duplicate suppression. Persist only
   events that must survive app suspension/restart; never persist ephemeral
   position snapshots.
4. Add a resume handshake carrying session identity and each side's last
   received sequence. Replay only missing reliable messages, in order.
5. Keep position updates coalesced/latest-only so stale movement never blocks
   story, battle, or control messages. TCP cannot provide ENet's independent
   unreliable channel, so use a separate datagram path only if testing shows
   that the current transport causes movement head-of-line stalls.
6. Preserve the existing background hold and disconnect/battle-abort safety
   rules. A reconnect must not duplicate rewards, trainer victories, catches,
   or trade commits.
7. Add explicit message-size limits, queue-overflow handling, malformed-frame
   rejection, timeout tests, and logs that never expose session keys or lobby
   secrets.

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
