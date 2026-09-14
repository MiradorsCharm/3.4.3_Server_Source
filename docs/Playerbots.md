# AI Playerbots

This server embeds a from-scratch playerbot AI, written **for this core** (a
WoW 3.4.3 TrinityCore-derived tree). It is not a port of ike3/mangosbot or of
any other bot codebase - the design uses the core's own player pipeline
instead of the strategy/action engine the old mangosbot line was built on.

Bots are *real characters*: they are loaded from the `characters` database
through a socket-less `WorldSession`, they walk, fight, loot, level, answer
whisper commands from their master, and show up to other players as normal
characters.

---

## 1. Why a rewrite (design)

In this core a **player is client-authoritative**. The only pipeline that
both moves a player and tells the world about it is the one a real client
drives:

```
client CMSG_MOVE_*  ->  WorldSession::HandleMovementOpcode
                        -> Unit::UpdatePosition  -> Map::PlayerRelocation
                        -> SMSG_MOVE_UPDATE broadcast to observers
```

Anything the server runs *instead* of that pipeline fights the core:
server-side splines make `HandleMovementOpcode` drop packets,
`PlayerRelocation` never broadcasts on its own, and facing commands for
players are only messages *to a client* that does not exist. The previous
port (a mangosbot strategy engine) drove bots with server-side
`MotionMaster` splines and orientation sets - and produced bots that chased
forever without closing the gap, stood in swing range facing the wrong way,
and never fired their wands.

The rewritten AI therefore makes the bot **its own client**:

| Concern | Implementation |
| --- | --- |
| Movement | `BotMovement` plans a path with the core's `PathGenerator` (mmaps), advances along it at the bot's real run speed, and feeds every step back through the bot's own session as `CMSG_MOVE_START_FORWARD` / `CMSG_MOVE_HEARTBEAT` / `CMSG_MOVE_STOP` |
| Facing | heartbeats with a new orientation while standing (the only facing that "sticks" for a player - there is no client to receive `SMSG_MOVE_SET_FACING`) |
| Melee | `Unit::Attack(victim, true)` starts the combat state; `Player::Update` swings when the core's own gate (`IsWithinMeleeRange` + 120-degree arc) is green |
| Ranged | cast wand shoot (5019) / auto shot (75) once; `Unit::_UpdateAutoRepeatSpell` then owns the loop, exactly as for a real hunter/wand user |
| Spells | `Player::CastSpell` with a cheap pre-gate (known, off cooldown, affordable, in range, LOS) that reads the same metrics `Spell::CheckCast` will use |
| Teleports | the manager synthesizes the worldport/teleport acks a bot's missing client would send |
| Looting | `Player::SendLoot` / `Player::StoreLootItem` / `WorldSession::DoLootRelease` - the same APIs a client click drives |

One rule follows from this: **the plugin never starts a server-side movement
generator for a bot.** `MoveChase`, `MovePoint`, `MoveFollow` and friends do
not appear in the bot code.

## 2. Layout

| Path | Contents |
| --- | --- |
| `src/plugins/playerbot/` | the whole AI |
| `BotConfig.{h,cpp}` | `AiPlayerbot.*` settings |
| `BotManager.{h,cpp}` | session ownership, login/logout, random bot pool, hook registration, whisper routing |
| `BotAI.{h,cpp}` | per-bot pipeline: death -> brain -> movement -> diagnostics |
| `BotMovement.{h,cpp}` | the client-faithful mover (path + packets) |
| `BotCombat.{h,cpp}` | melee/ranged stance machine around the core's swing gate |
| `BotSpells.{h,cpp}` | spell pre-checks + auto-repeat start |
| `BotClassAI.{h,cpp}` + `BotClass*.cpp` | one small priority-list script per class (all ten) |
| `BotLoot.{h,cpp}` | corpse queue -> SendLoot/StoreLootItem |
| `BotDiagnostics.{h,cpp}` | the "Bot X is stuck" watchdog |
| `BotFactory.{h,cpp}` | character prep (spells/gear/ammo) and fresh random bot characters |
| `BotCommands.cpp` | `.bot` console/GM commands |
| `BotHooks.cpp` | `Playerbot::InitializePlayerbots` and friends for `worldserver/Main.cpp` |
| `src/server/game/AI/Playerbot/PlayerbotHooks.{h,cpp}` | the core-side hook registry (function pointers, null-safe inline wrappers) |

### Core-side integration points

| Core location | Hook |
| --- | --- |
| `World::Update` | `OnWorldUpdate` - drives the manager (session pump, random pool) |
| `Player::Update` | `OnPlayerUpdate` - drives the bot AI on the map thread |
| `WorldSession::SendPacket` | `OnBotPacketSent` - bot sessions have no socket |
| `WorldSession::HandlePlayerChat` | `OnPlayerChat` - whisper/party commands |
| `Player::~Player` | `OnPlayerDelete` - detaches the AI |
| `WorldSession::LogoutPlayer` | `OnPlayerLogout` (no-op in this version) |
| `worldserver/Main.cpp` | `Playerbot::InitializePlayerbots()` / `ShutdownPlayerbots()` / `RegisterPlayerbotScripts()` |

`Player` carries a single `BotAI*` (accessors `SetBotAI`/`GetBotAI`), and
`WorldSession` two small additions (`SetBotSession`/`IsBotSession`,
`HandleBotPackets`, `LoginBotPlayer`).

## 3. Building

Nothing special - the `plugins` target is part of the normal CMake build and
is linked into `worldserver`. Build with static linking
(`WITH_DYNAMIC_LINKING=0`, the default); the plugin calls core functions that
are not exported with `-fvisibility=hidden`.

```bat
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCOPY_CONF=1
cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo
```

## 4. Database setup

Apply once to the **characters** database (it is also auto-created at
startup):

```sh
mysql -u trinity -p characters < sql/custom/playerbot/characters_playerbot.sql
```

It creates `ai_playerbot_names`, the name pool used when random bot
characters are generated. Fill it with operator-chosen names:

```sql
INSERT INTO ai_playerbot_names (name) VALUES ('Nameone'), ('Nametwo');
```

Random bots are **every character whose account name starts with
`AiPlayerbot.RandomBotAccountPrefix`** (default `rndbot`). No per-bot
bookkeeping tables exist; to demote a bot, rename its account or delete the
character. Old tables from the previous port
(`ai_playerbot_random_bots`, `ai_playerbot_custom_strategy`,
`ai_playerbot_tellitem`, `ai_playerbot_guild_tasks`,
`ai_playerbot_texts`) are no longer read and can be dropped.

## 5. Configuration

Everything lives under **AI PLAYERBOT SETTINGS** in `worldserver.conf.dist`.
Rebuilding updates `.conf.dist`, never your live `.conf`. Restart
worldserver after changing values.

Key settings:

```ini
AiPlayerbot.Enabled = 1            # master switch
AiPlayerbot.RandomBotCount = 0     # how many random bots to keep online (0 = off)
AiPlayerbot.Diagnostics = 1        # stuck-bot reports
AiPlayerbot.MeleeStopFactor = 0.8  # stop at 80% of the live swing range
AiPlayerbot.CastStandDistance = 18 # ranged stand-off
AiPlayerbot.FollowDistance = 4
```

Distances: `SightDistance`, `SpellDistance`, `LootDistance`, `WanderRadius`,
`CastMinDistance`. Timers: `StallReportMs`, `StallReportCooldownMs`,
`ReviveDelayMs`, `RandomBotUpdateInterval`.

## 6. Using bots

* `.bot add <name> [master-name]` - log an existing character in as a bot.
  With a master (or the character of the player running the command), the bot
  is summoned next to the master and accepts their whispers.
* `.bot remove <name>` / `.bot removeall` / `.bot list` / `.bot info <name>`
* `.bot rndbot` - show the random bot target/count.
* `.bot rndbot` selection: the manager audits the pool every
  `RandomBotUpdateInterval` seconds, logs bots in/out to approach
  `RandomBotCount`, and creates fresh bot accounts/characters when short.
  Fresh characters are leveled into the configured band, given class spells,
  a usable weapon (plus shield/ranged/ammo as appropriate), armor, and a
  little money - see `BotFactory`.

### Whisper commands

Whisper the bot (or use party/raid chat if you are its master):

| Command | Effect |
| --- | --- |
| `follow` | follow the master |
| `stay` | hold position |
| `come` | walk to the sender |
| `attack my target` / `attack` | attack the sender's selection |
| `assist` | attack the sender's victim |
| `stop attack` | disengage |
| `loot` | loot our kills |
| `heal` | run a heal pass |
| `status` | one-line self report |
| `release` | speed up self-resurrection |
| `help` | the list |

Without a bound master, a bot accepts commands from anyone on its own
account; GMs can always command.

## 7. Combat behaviour

* **Melee classes** run to the victim and stop at `MeleeStopFactor` times the
  *live* swing range (`Unit::GetMeleeRange` of the pair - combat reaches +
  4/3 yd, minimum 5 yd, measured 3D). Because the stop is inside the same
  envelope the swing check uses, the bot cannot end up "in range but
  not swinging"; on arrival it faces the victim with heartbeat-orientation,
  which is the check `DoMeleeAttackIfReady` itself performs.
* **Ranged classes** hold the band between `CastMinDistance` and
  `CastStandDistance`: they chase out to stand-off, back off (with a small
  random sidestep) when something closes inside the minimum, and otherwise
  stand still and shoot - a standing player is what keeps the core's
  auto-repeat loop firing.
* The class scripts (`BotClass*.cpp`) are explicit hand-written priority
  lists over the real spell ranks, paced to roughly one attempt per GCD. No
  strategy engine, no values/triggers graph - just `CastOnVictim(Rank(...))`
  chains that are easy to read and tune.
* Dead bots resurrect themselves in place after `ReviveDelayMs`.

## 8. Diagnostics

With `AiPlayerbot.Diagnostics = 1` a bot that holds a target without making
progress (distance not shrinking, victim health not dropping, no casts) for
`StallReportMs` prints one console line and whispers the same line to its
master, then stays quiet for `StallReportCooldownMs`:

```
Bot Lyanna stalled 4s on Veruwin: d3d=7.36 meleeRange=5.00 inRange=1 arc=1 atk=1 swingErr=none moving=1 mode=2 los=1 combat=1 spell=cast 348 stance=melee reason=assist master
```

Every number in the line is read from the same core objects the swing/cast
code will use on the next tick, so a report is directly actionable.
`AiPlayerbot.DebugMove = 1` additionally traces every movement packet.

## 9. Extending

* New class behaviour: edit the class script in `BotClass<Class>.cpp` - a
  combat priority list is a few `if (CastOnVictim(Rank(ABILITY)))  return;`
  lines; add rank ids to the table at the top.
* New behaviour overall: `BotAI::UpdateBrain` is the place. Keep the rule
  from section 1 - drive the player, never a movement generator.
