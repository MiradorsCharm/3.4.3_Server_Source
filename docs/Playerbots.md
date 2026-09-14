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
| `BotState.{h,cpp}` | the `characters_playerbot` table: roster, owner, orders, `prepared_level` |
| `BotSpawns.{h,cpp}` | world-wide placement derived from the `creature` spawn table |
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

It also creates `characters_playerbot`, the bot state table. One row per bot:
who owns it, whether it is part of the random pool, the roles/orders it had
been given (`tank_mode`, `grind_mode`, `stay` + the stay point), the level it
was last prepared for, and when it was last seen. This is what makes a
restart restore the same roster at the same places with the same orders - the
position itself is the core's own `characters.position_*` and comes back
because the character is saved. Upgrading from the previous build (whose
table carried only `guid`, `master`) needs nothing: the columns are added in
place on startup by `BotState::EnsureSchema`, and the script does the same
migration if you prefer to run it by hand.

Random bots are **every character whose account name starts with
`AiPlayerbot.RandomBotAccountPrefix`** (default `rndbot`). To demote a bot,
rename its account or delete the character (its `characters_playerbot` row is
dropped automatically). Old tables from the previous port
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
AiPlayerbot.RandomBotMinLevel = 1  # level band the pool is kept in
AiPlayerbot.RandomBotMaxLevel = 60
AiPlayerbot.Diagnostics = 1        # stuck-bot reports
AiPlayerbot.MeleeStopFactor = 0.8  # stop at 80% of the live swing range
AiPlayerbot.CastStandDistance = 18 # ranged stand-off
AiPlayerbot.FollowDistance = 4
```

Distances: `SightDistance`, `SpellDistance`, `LootDistance`, `WanderRadius`,
`CastMinDistance`. Timers: `StallReportMs`, `StallReportCooldownMs`,
`ReviveDelayMs`, `RandomBotUpdateInterval`.

**Persistence and startup pacing** - the knobs that decide what a restart
does. Defaults are tuned for a 500-bot pool on a modest machine:

```ini
AiPlayerbot.PersistBots = 1        # save/restore the roster (0 = clean slate)
AiPlayerbot.StateSaveIntervalMs = 60000
AiPlayerbot.RestoreRandomBots = 1  # bring the saved pool back on start
AiPlayerbot.StartupLoginDelayMs = 15000  # grace period after boot
AiPlayerbot.LoginStaggerMs = 400   # minimum pause between two bot logins
AiPlayerbot.MaxConcurrentLogins = 3
```

At the defaults a 500-bot pool takes about **3.5 minutes** to come back after
a restart, three logins at a time, instead of all at once in a single tick.
Lower `LoginStaggerMs` / raise `MaxConcurrentLogins` on a machine with room.

**World-wide placement:**

```ini
AiPlayerbot.RandomBotSpread = 1        # place by level + occupancy, not starter zone
AiPlayerbot.RandomBotRelocateMinutes = 0   # >0: settled bots wander on
AiPlayerbot.RandomBotCreateBatch = 5   # new characters created per audit
```

**Combat:**

```ini
AiPlayerbot.CastRetryMs = 150     # rotation re-evaluation period (was 600)
AiPlayerbot.HopelessLevelGap = 5  # run instead of flailing at a +5 mob (0 = off)
```

## 6. Using bots

* `.bot add <name> [master-name]` - log an existing character in as a bot.
  With a master (or the character of the player running the command), the bot
  is summoned next to the master and accepts their whispers. The binding is
  **persisted** (`characters_playerbot` table): the next time the master logs
  in, their bots re-join automatically. `.bot remove` clears the binding.
* `.bot remove <name>` / `.bot removeall` / `.bot list` / `.bot info <name>`
* `.bot rndbot` - show the random bot target/count.
* `.bot rndbot` selection: the manager audits the pool every
  `RandomBotUpdateInterval` seconds, logs bots in/out to approach
  `RandomBotCount`, and creates fresh bot accounts/characters when short.
  Fresh characters are leveled into the configured band, given class spells,
  a usable weapon (plus shield/ranged/ammo as appropriate), armor, and a
  little money - see `BotFactory`. Random bots wander, **grind nearby
  non-elite mobs**, loot their kills, eat and drink after fights, and level
  from the XP like any player.

### Commands

Say them in **whisper, party or raid chat**, or in `/say` while standing next
to the bot. Anyone sharing a party or raid with the bot may command it (the
master always may, GMs always may) - a bot you are grouped with answers you,
which is what makes leading a raid of bots possible. Prefixes are optional: a
leading `!`, `.`, or `bot ` is stripped, and you can address one bot by name
(`Kaeo, follow`). Matching is case-insensitive prefix matching, so `att` and
`ATTACK` both work.

**Movement and formation**

| Command | Effect |
| --- | --- |
| `follow` | follow the master |
| `follow melee` / `near` / `far` / `ranged` | follow at 2 / 4 / 12 / 20 yards |
| `formation <yards>` | follow at an explicit distance |
| `heel` | back to the configured `FollowDistance` |
| `stay` | hold position |
| `come` / `here` | walk to the sender |
| `move out` / `spread` | take distance from the group |
| `summon` | teleport to the master now |
| `flee` / `runaway` | run away from the current fight |
| `position` / `where` | report map and coordinates |

**Combat**

| Command | Effect |
| --- | --- |
| `attack my target` / `attack` / `kill` | attack the sender's selection |
| `assist` | attack the sender's victim |
| `tank attack` | tank-mode attack on the sender's target |
| `stop attack` | disengage |
| `grind` / `stop grind` | attack nearby mobs while idle |
| `tank` | tank role: taunt/presence upkeep (tank-capable classes) |
| `dps` | back to damage role |
| `max dps` | never hold the rotation back |
| `save mana` | hold casts while mana is low |
| `cast <name>` / `spell <name>` | cast a specific spell by name |
| `spells` | list what the bot knows |

**Party care**

| Command | Effect |
| --- | --- |
| `heal` | run a heal pass (party, not just self) |
| `buff` | cast class buffs on the party now |
| `rez` / `revive` / `res` | resurrect a dead party member (healers) |
| `cure` / `dispel` | cure/dispel the party |
| `eat` / `drink` | consume food/water from bags now |
| `release` | speed up self-resurrection |

**Items, vendors and money**

| Command | Effect |
| --- | --- |
| `loot` | loot our kills |
| `add all loot` / `loot all` / `ll` | loot everything on the corpse |
| `upgrade` / `e` | equip the best item-level gear from the bot's bags |
| `repair` | repair the bot's gear |
| `sell` / `s` | vendor junk / post equipment at the auction house |
| `buy <name>` | buy an item from the vendor we are standing at |
| `talents` | spend free talent points now |

**Social and misc**

| Command | Effect |
| --- | --- |
| `quests` / `quest` / `q` | quest status |
| `accept` | accept the offered quest |
| `invite` | invite the sender to a group |
| `guild` / `guild leave` | join / leave the master's guild |
| `queue` | queue at a nearby battlemaster (auto-enters, fights, `leave` exits) |
| `leave` | leave battleground / queues |
| `emote <text>` | play the emote |
| `who` | the bots online |
| `stats` / `status` | one-line self report |
| `chat` / `chat say` / `chat party` / `chat whisper` | which channel to answer on |
| `reset ai` / `reset` | drop victim, orders and state |
| `help` / `?` | the list |

An unrecognised command answers `I don't know '<x>' - say 'help'` instead of
being silently ignored.

### What bots do on their own

* **Fight**: assist the master's fight, retaliate against attackers, and
  (when ordered, or always for random bots) grind nearby non-elite mobs.
* **Party care pass** (every 2s): resurrect dead members (healers), cure
  poison/disease/curse/magic debuffs, keep party buffs up out of combat,
  and heal injured party members in and out of combat.
* **Self care**: eat and drink out of combat when below
  `AiPlayerbot.EatDrinkPct` (mages conjure their own food/water), revive
  their hunter pet, resurrect when requested by a healer, self-resurrect
  after `ReviveDelayMs`.
* **Loot**: corpses of their own kills are queued and looted
  automatically.
* **Catch up**: a master who portals away is followed by a paced
  teleport (also available as the `summon` command).
* **Talents**: every earned talent point is spent automatically through the
  core's validated `Player::LearnTalent` (tier/tab order) - on login, on
  level-up (checked every minute), and on the `talents` command.
* **Quests**: `quests` takes everything from the master's quest log the bot
  qualifies for; finished quests are handed in automatically at nearby quest
  givers (kill and loot objectives complete themselves while the bot fights
  and loots beside you).
* **Social**: duels from the master are accepted and fought; trade windows
  from the master are opened and confirmed; `guild` joins the master's
  guild, `guild leave` leaves it.
* **PvP**: `queue` at a battlemaster joins a level-appropriate battleground
  through the real queue; when invited the bot walks the portal, engages
  nearby enemies inside, and `leave` exits (or clears queues).
* **Dungeons**: when the group leader queues the dungeon finder, bots accept
  the proposal automatically and play the dungeon with the normal combat
  and loot brains.
* **Economy**: `sell` vendors gray junk and posts unneeded equipment on the
  auction house (brought to a vendor/auctioneer like a player would). Mail
  sent to a bot (items, gold) is collected automatically once a minute while
  it stands at a mailbox; COD mails are never paid.

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

## 10. Fixes (2026-09-14)

Four bugs that made a freshly ported tree look like "the bot logic was never
implemented" - bots stacked, motionless, ignoring orders, spamming the log:

1. **Bots never moved / ignored follow, come, attack.** `HandleBotPackets()`
   drained the bot's receive queue with a `WorldSessionFilter`. That filter's
   `Process()` returns `false` for `PROCESS_THREADSAFE` opcodes while the
   player is in world, and `LockedQueue::next(result, checker)` does **not**
   pop when the check fails - so the first `CMSG_MOVE_*` packet a bot queued
   stuck at the head of the queue forever and every later packet starved
   behind it. Every movement the mover sent was silently discarded. Spells,
   party-invite accepts and loot still worked because those are direct
   `Handle*()` calls that never touch the queue - which is exactly the
   "casts but won't move, accepts invites but ignores commands" symptom.
   The pump now drains unconditionally (it runs on the world thread before
   the map update, so map opcodes are safe here) and only defers
   `STATUS_LOGGEDIN`/`STATUS_TRANSFER` packets while the bot is still
   loading. *(`WorldSession::HandleBotPackets`)*

2. **`[1146] Table 'world.item_template' doesn't exist` spam.** `BotFactory`
   looked up gear with a 3.3.5-style `SELECT ... FROM item_template` query.
   3.4.3 has no such table - item data is in the DB2 client stores. Gear
   selection now scans `ObjectMgr::GetItemTemplateStore()` in memory (correct
   for this core and cheaper than a DB round trip); bots get dressed again.
   *(`BotFactory::FindBestItem`)*

3. **`(ServerSide check) ... Attempt to cast spell` spam** (Conjure Food /
   Water, Arcane Brilliance chaining). The spell pre-gate never checked
   whether the bot was already mid-cast, so it queued a second cast every
   tick and the core logged the rejection each time (this build logs but does
   not abort the offending cast). The gate now refuses to start a spell while
   a non-instant generic or channeled cast is in progress, matching the
   core's own `Spell::prepare` predicate. *(`BotSpells::Castable`)*

4. **`Could not create bot account rndbot_...`.** Account names are capped at
   `MAX_ACCOUNT_STR` (16). The generator produced `rndbot_<unix-time>` =
   17 chars, so every random-bot account creation failed with
   `AOR_NAME_TOO_LONG`. Names are now built from a base-36 suffix with the
   prefix trimmed to fit, and a name collision is skipped rather than treated
   as a fatal error. *(`BotManager::EnsureRandomBotPool`)*

Plus a quality-of-life change: followers used the master's exact position as
their goal, so a party of bots piled onto one tile ("stacked"). Each bot now
takes a stable formation slot in an arc behind the master
(`BotMovement::Follow` gains an angle offset; `BotAI` derives the slot from
the bot's GUID).

## 11. Dungeon / raid mechanic awareness (2026-09-14)

Bots now read the floor and react to two of the mechanics a real player is
expected to handle: **standing in the fire** and **letting a boss finish a
dangerous cast**.

**`BotHazards` (new module, `BotHazards.{h,cpp}`).** Owned by `BotAI`, it is
the bot's "eyes on the floor". Once per tick (the expensive grid sweep is
throttled to ~250 ms; the "am I standing in it?" test runs every tick) it
scans the grid around the bot for hostile ground effects and tracks each as a
circle (`BotHazard{center, radius, source, spellId}`):

* **What it looks for.** Both kinds of ground spell this core uses -
  `AreaTrigger` (modern "get out of the fire": Defile, flame patches, ...)
  and `DynamicObject` (classic persistent-area spells: Rain of Fire,
  Blizzard, Death and Decay, ...). Both are grid objects, gathered in one
  `Cell::VisitGridObjects` sweep with a
  `GRID_MAP_TYPE_MASK_AREATRIGGER | GRID_MAP_TYPE_MASK_DYNAMICOBJECT` searcher.
* **What counts as harmful.** The spell is inspected, not hardcoded: it must
  be non-positive and either deal (periodic) damage or apply a damaging /
  controlling aura (stun, fear, root, silence, ...). Friendly/own zones
  (Consecration from a party pal, Healing Rain) are ignored. Polygon area
  triggers are approximated by their bounding radius - the conservative,
  always-safe choice - plus a configurable safety margin.
* **What it offers.** `InDanger()`, `IsSpotDangerous()`, `IsPathDangerous()`
  (segment sampled every ~2 yd so no circle slips between samples), and
  `FindSafeSpot()` which fans out in rings of directions using
  `GetFirstCollisionPosition` (so the escape point is reachable and never
  through a wall) and prefers the spot closest to the current victim - a melee
  bot sidesteps rather than sprinting out of the fight.

**Integration.**

* **`BotAI::UpdateHazardAvoidance`** runs *first* each tick, ahead of the
  combat/loot/follow brain: standing in fire kills faster than any rotation
  helps, so dodging preempts everything and owns movement for that tick. It
  yields control back cleanly once clear, and stands down when the core has
  taken control of the bot (stun/knockback/vehicle) since it cannot walk out
  on its own then.
* **`BotCombat::UpdateRanged`** picks a backpedal direction that is not into a
  hazard (it fans out and rejects dangerous spots/paths) instead of always
  stepping straight back.
* **Interrupts.** `BotHazards::ShouldInterrupt` judges whether a unit's
  current cast is worth stopping (a real, interruptible, non-positive cast, or
  any heal - respecting the core's own `CanBeInterrupted` rules).
  `BotClassAI::TryInterruptVictim` pairs that judgement with each class's
  interrupt kit, declared via the new `GetInterruptSpells()` virtual - Mage
  Counterspell, Rogue Kick, Warrior Pummel/Shield Bash, Shaman Wind Shear,
  Death Knight Mind Freeze - and `BotCombat` fires it on its own short pacing,
  ahead of and independent of the normal rotation, so a short cast is not
  missed. The old per-class "interrupt anything being cast" checks (Mage,
  Rogue) were removed in favour of this shared, smarter path.

**Config** (`worldserver.conf.dist`, all default on):
`AiPlayerbot.AvoidGroundHazards`, `AiPlayerbot.HazardSafetyMargin` (yards,
0..15), `AiPlayerbot.InterruptCasts`.

## 12. Restarts, world spread, ranged attacks and group chat (2026-09-14)

Four complaints, one root cause and three independent ones.

### 12.1 The real reason most bots were useless

`BotFactory::PrepareBot` only levelled a bot when
`isRandomBot && GetLevel() < RandomBotMinLevel`, and `RandomBotMinLevel`
defaults to **1**. A 500-bot pool therefore came up as 500 level-1 characters:
no spells beyond the starting handful, no usable gear, standing in the
starting zones, and - the part that produced the stall reports - unable to
damage anything above level ~6. `Bot Perena stalled 4s ... atk=1
swingErr=none` is exactly that: the bot *was* in range, *was* facing, *did*
land swings, and every swing did nothing.

`PrepareBot` now levels into the configured `RandomBotMinLevel` ..
`RandomBotMaxLevel` band, learns the class spell list for that level, and
re-prepares when the band moves. It skips the whole pass when
`BotState.prepared_level` already matches (so a restart costs nothing).
`LearnClassSpells` also pre-validates every spell, which is 12.5.

### 12.2 Bots survive a restart, and the restart no longer freezes the world

**State.** New module `BotState.{h,cpp}`, one row per bot in
`characters_playerbot`: owner, random-pool flag, `tank_mode`, `grind_mode`,
`stay` + the stay point, `prepared_level`, `last_seen`. A bot's *position*
needs no new table - it is the core's own `characters.position_*`, and the bot
session now calls `Player::SaveToDB()` on logout, so it is actually written.
`BotAI::SaveState` / `ApplySavedState` convert to and from that row; the
manager saves periodically (`StateSaveIntervalMs`) and again in `Shutdown`.

**Pacing.** `AddBot` no longer logs a bot in - it queues it. `BotManager::Update`
releases the queue at `MaxConcurrentLogins` in flight, one every
`LoginStaggerMs`, and not at all until `StartupLoginDelayMs` has passed. The
pool audit counts queued logins toward its target so it does not re-queue the
whole gap every 30 s. `RestoreRoster()` re-adds the saved roster on the same
paced queue; a bot whose master is offline waits for that master's login
instead of appearing on its own. `LoadBotsForMaster` uses the new
`AddBotNow` so a player's own bots are not made to sit behind the pool.

500 bots ≈ 3.5 minutes to come back, three at a time, instead of one
database-thrash tick.

### 12.3 500 bots no longer pile into Goldshire

New module `BotSpawns.{h,cpp}`. `BuildSpots()` walks the core's own
`sObjectMgr->GetAllCreatureData()` (capped at 30 000 spawns) and keeps a spawn
only if: the map exists and is not instanceable, phase 0/1, the template and
its `DIFFICULTY_NONE` difficulty exist, classification is `Normal`, not a
civilian, not a critter or non-combat pet, and no service NPC flag. Each
surviving spawn is bucketed by `diff->MinLevel`/`MaxLevel` into
`_byLevel[1..128]`.

`PickSpot` widens the level window by ±5, samples 192 candidates, and scores
each by how many other bots already stand in its 200-yard cell
(`mapId<<42 | cx<<21 | cy`), with a random tie-break. `PlaceRandomBot` jitters
the point ±4 yd, teleports and records the occupancy. It is called when a
random bot logs in (250 yd exclusion), and again - optionally - from
`UpdateWander` after `RandomBotRelocateMinutes` so a settled bot drifts on.

Nothing is hardcoded: there is not one coordinate in the plugin.

### 12.4 Ranged bots never cast

The ranged attack spell is a property of the **equipped ranged weapon**, not
of the class - the core's own `PlayerAI::DoRangedAttackIfReady` switches on
`rangedTemplate->GetSubClass()`: bow/gun/crossbow → `SPELL_SHOOT` 3018,
thrown → 2764, wand → 5019; a hunter's Auto Shot is 75. And those spells are
**not in the spell book**, which is why the core casts them triggered.

New `BotSpells::RangedAttackSpell()` implements that table (75 when the bot
knows Auto Shot, else 3018), and `StartAutoRepeat` casts with
`triggered = !known`. `Spell::GetCurrentContainer()` returns
`CURRENT_AUTOREPEAT_SPELL` purely from `IsAutoRepeat()`
(`SPELL_ATTR2_AUTO_REPEAT`), so a triggered cast still installs the loop.
`Unit::_UpdateAutoRepeatSpell` cancels the wand loop on movement, so
`BotCombat::UpdateRanged` now calls `MaintainAutoRepeat(victim)` every tick in
band and restarts the loop when its `m_targets.GetUnitTarget()` is not the
victim. The dead `CastMinDistance` config now feeds `minRange`, and the
rotation re-evaluation period dropped from a hardcoded 600 ms to
`CastRetryMs` (150).

### 12.5 `AddSpell: Spell (ID: 51266) is invalid`

`LearnClassSpells` handed `Player::LearnSpell` every id from the class spell
list, including ones this build's DBC does not carry or that the character
cannot hold. The log line comes from `Player::AddSpell`. Spells are now
pre-filtered with `SpellMgr::IsSpellValid(info, bot, false)` - the very
predicate `AddSpell` applies - and any spell whose effects are all
`SPELL_EFFECT_NONE` is rejected. Note `SpellInfo` in 3.4.3 has **no class or
race mask**, so class filtering is not possible here.

### 12.6 `MoveSplineInitArgs::Validate: expression '_checkPathLengths()' failed`

`_checkPathLengths` (`MoveSpline.cpp:271`) rejects a spline when any two
consecutive **middle** points (`path.size() > 2`, `i >= 1`) are closer than
0.1 yard. A failed `Validate` makes `Launch` return 0, so the bot never moves
at all. Fixed at the path source: `MoveSplineInit::MoveTo` runs
`PruneDegeneratePathPoints` (threshold `MIN_SPLINE_SEGMENT_LENGTH = 0.1f`)
before `MovebyPath`.

### 12.7 Hopeless fights

A bot in combat with a creature `HopelessLevelGap` levels above it now runs
instead of flailing: `UpdateRetaliate` calls `FleeFrom(attacker, ...)` and
`BotCombat` bails the same way. `UpdateFlee` owns the tick for a 10 s window
(30 yd legs, fan offsets `{0, ±0.5, ±1.0, ±1.6}` rad, rejects |Δz| > 20 and
hazards), and `UpdateRetaliate` returns early while `_fleeTimer > 0` so the
bot does not instantly re-acquire what it is running from.

### 12.8 Commands in group chat, mangosbot vocabulary

`AcceptsCommandsFrom` now also accepts **any member of the bot's party/raid**
(mirroring `ike3/mangosbot`'s `PlayerbotSecurity.cpp`), and
`BotManager::RouteChat` routes party/raid/say to those bots rather than only
to the master's own. `HandleCommand` was rewritten around the mangosbot
creator map (see section 6 for the full vocabulary), with optional `!` / `.` /
`bot ` prefixes and `<Name>, ` addressing. New `BotAI::Reply` answers on the
channel the `chat` command selected: `/say`, party (`Group::BroadcastPacket`
with `CHAT_MSG_PARTY_LEADER` when leading, exactly the pattern from
`ChatHandler.cpp`), or whisper. `BuyItemByName` walks
`sObjectMgr->GetNpcVendorItemList`, matches `ItemTemplate::GetName`
case-insensitively, checks `CanUseItem`, and buys through the real
`HandleBuyItemOpcode`.

### Still open / worth verifying next

* **mmaps required for good pathing.** The mover uses `PathGenerator`
  (mmaps); without extracted mmap tiles it falls back to a straight-line
  glide, which is fine in the open but will hug walls indoors. Ship mmaps for
  best results.
* **Random-bot levelling/gear** is a light pass (class spells + best usable
  vendor-grade item per slot); it is not a full talent/enchant/gem build.
* **Group formation is cosmetic**, not role-aware (no tank-in-front melee
  positioning yet).
* **`BuyItemByName` needs a vendor within interact range.** There is no
  "walk to the nearest vendor first" step yet.

### Verification status

Every plugin source and `MoveSplineInit.cpp` compiles clean with
`g++ -std=c++20 -fsyntax-only` against the real 3.4.3 headers (game files with
`PrecompiledHeaders/gamePCH.h` force-included, exactly as CMake does). That
checks syntax, name lookup and types - it is **not** a link or a runtime test,
and this environment cannot run the server. Both Python suites pass:
`tests/playerbot_config_test.py` 5/5 (all 36 `AiPlayerbot.*` keys the loader
reads are present, documented exactly once) and
`tests/playerbot_wiring_test.py` 52/52 (16 new invariants pinning restart
pacing, persistence, world spread, ranged casting, the hopeless-fight bail,
group-chat routing and the spline prune).
