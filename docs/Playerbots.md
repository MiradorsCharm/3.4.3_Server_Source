# AI Playerbots (ike3/mangosbot port)

This server embeds a port of [ike3/mangosbot](https://github.com/ike3/mangosbot)
("AI Playerbot") adapted to this WoW 3.4.3 TrinityCore-derived core.

Bots are *real characters*: they are loaded from the `characters` database
through a socket-less `WorldSession`, they walk, fight, loot, quest, trade,
train, use the auction house, and answer chat commands from their master.

---

## 1. Layout

| Path | Contents |
| --- | --- |
| `src/plugins/playerbot/` | the bot implementation (AI, strategies, actions, values, triggers, random bot manager) |
| `src/plugins/ahbot/` | minimal item-pricing helpers used by the bots |
| `src/plugins/CMakeLists.txt` | builds everything above into the static `plugins` library |
| `src/server/game/AI/Playerbot/PlayerbotHooks.{h,cpp}` | core-side hook registry (function pointers) |
| `src/plugins/playerbot/PlayerbotHookImpl.cpp` | plugin-side implementation that fills the hooks in |
| `src/server/worldserver/worldserver.conf.dist` | all bot defaults in **AI PLAYERBOT SETTINGS** |
| `sql/custom/playerbot/characters_playerbot.sql` | the `ai_playerbot_*` tables |

### Why hooks instead of direct calls?

`plugins` is linked *after* `game`, so the core must never reference plugin
symbols directly. Instead the plugin registers a `Playerbot::Hooks` struct of
function pointers at startup, and the core calls them through inline,
always-null-safe wrappers. If the bot system is disabled (or the library is not
linked), every hook is a no-op branch.

Core call sites (all marked with a `playerbot mod` comment):

| Core location | Hook |
| --- | --- |
| `World::Update` | `OnWorldUpdate` — drives random and player-owned bot sessions on the world thread |
| `Player::Update` | `OnPlayerUpdate` — drives the bot AI on its player/map update |
| `WorldSession::HandlePlayerLogin` | `OnPlayerLogin` |
| `WorldSession::LogoutPlayer` | `OnPlayerLogout` |
| `Player::~Player` | `OnPlayerDelete` |
| `WorldSession::SendPacket` | `OnBotPacketSent` — bot sessions have no socket, packets go to the AI |
| `WorldSession::HandleChatMessage` | `OnPlayerChat` — whisper/party/raid commands |
| `worldserver/Main.cpp` | `Playerbot::InitializePlayerbots()` / `ShutdownPlayerbots()` / `RegisterPlayerbotScripts()` |

Two small accessors were added to core headers so bots can read data the client
normally receives over the wire; both are marked `// playerbot`:

* `Trainer::Trainer::GetSpells()` / `GetSpellStateForPlayer()` (`Entities/Creature/Trainer.h`)
* `Group::GetRolls()` (`Groups/Group.h`)

---

## 2. Building

Nothing special — the `plugins` target is part of the normal CMake build and is
linked into `worldserver` (link order: `scripts plugins game`).

From a Visual Studio 2022 Developer Command Prompt on Windows:

```bat
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCOPY_CONF=1
cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo
```

With `COPY_CONF=1` (the default), CMake places `worldserver.conf.dist`, including
all playerbot settings, next to `worldserver.exe` in the build output.
`cmake --install` installs that same template. There is no separate bot template.
Configuration values are read at runtime, not compiled into the executable.

> **Note:** build with the default static linking (`WITH_DYNAMIC_LINKING=0`).
> With shared libraries the core is compiled with `-fvisibility=hidden` and the
> bots reference plenty of core functions that are not marked `TC_GAME_API`,
> which would fail to link.

---

## 3. Database setup

Apply the schema to the **characters** database once:

```sh
mysql -u trinity -p characters < sql/custom/playerbot/characters_playerbot.sql
```

It creates:

| Table | Purpose |
| --- | --- |
| `ai_playerbot_random_bots` | which characters are random bots + scheduled events |
| `ai_playerbot_names` | name pool used when generating random bot characters |
| `ai_playerbot_guild_names` | name pool for generated bot guilds |
| `ai_playerbot_custom_strategy` | user-defined strategies (`action_line`) |
| `ai_playerbot_tellitem` | remembers items a bot already reported |
| `ai_playerbot_guild_tasks` | guild task state |
| `ai_playerbot_texts` | localized bot chat lines |

The script is idempotent (`CREATE TABLE IF NOT EXISTS` + `INSERT IGNORE`) so it
can be re-run safely.

`ai_playerbot_names` and `ai_playerbot_guild_names` are plain operator data: the
server reads free rows from them and never writes to them. An earlier revision
generated syllable names and `INSERT`ed them on every bot-creation pass, which
both wrote rows nobody had asked for and printed one validation error per rejected
name on each retry; it was removed. A bot pass therefore needs at least
`AiPlayerbot.RandomBotAccountCount x 10` unused names, and reports the shortfall
once:

```sql
INSERT INTO ai_playerbot_names (name) VALUES ('Nameone'), ('Nametwo');
```

Names already taken by characters are skipped by the picker, so a pool smaller
than the account count only costs you the surplus character slots.

---

## 4. Configuration

Use the **AI PLAYERBOT SETTINGS** section of `worldserver.conf.dist`. It contains
active defaults for every `AiPlayerbot.*` setting, all ten Wrath classes' talent
weights, and the minimal `AhBot.*` pricing helper settings. Keep these in the
existing `[worldserver]` section; do not add an `[AiPlayerbotConf]` section.

### Upgrading an existing server

1. Stop worldserver and back up your live `worldserver.conf`.
2. Regenerate CMake and rebuild to include the C++ fixes. Copy the new executable
   and its matching PDB if you run from a separate server directory.
3. Merge **AI PLAYERBOT SETTINGS** from the new `worldserver.conf.dist` into your
   live `worldserver.conf`. Preserve your database credentials and other server
   settings, and replace any existing bot keys rather than duplicating them.
4. Migrate any custom values from old `aiplayerbot.conf` files. The plugin no
   longer loads a standalone file from the working directory. The core still
   loads normal `*.conf` overrides from its configured config directory (by
   default `worldserver.conf.d`), so remove the old bot override after migration
   or it can override values in `worldserver.conf`.
5. Restart worldserver. Bot configuration changes require a restart.

**Rebuilding/installing updates `.conf.dist`, not your live `.conf`.** On a new
installation, copy `worldserver.conf.dist` to `worldserver.conf` and configure
your database connections before starting the server.

Put comments on separate lines, not after values. For example, `80 # max level`
is not an integer to the config parser and triggers the "Bad value" fallback.

Key settings (edit the existing entries, do not append duplicate keys):

```ini
# Master switch
AiPlayerbot.Enabled = 1
AiPlayerbot.AllowGuildBots = 1
# Keep a population of random bots online
AiPlayerbot.RandomBotAutologin = 1
AiPlayerbot.MinRandomBots = 50
AiPlayerbot.MaxRandomBots = 200
AiPlayerbot.RandomBotAccountPrefix = "rndbot"
AiPlayerbot.RandomBotAccountCount = 50
AiPlayerbot.RandomBotMinLevel = 1
AiPlayerbot.RandomBotMaxLevel = 80
# Use "!" if you want commands such as "!follow"
AiPlayerbot.CommandPrefix = ""
# Disable the optional TCP command server
AiPlayerbot.CommandServerPort = 0
# 0 = silent, 1 = say why a bot that cannot fight is stuck, 2 = per-second snapshots
AiPlayerbot.DebugCombat = 1
```

The template's `RandomBotMaxLevel = 255` is capped by `MaxPlayerLevel` (normally
80); setting an explicit value of 80 as above is also valid. The historical
keys `AiPlayerbot.MaxRandomRandomizeTime` and `AiPlayerbot.MaxRandomReviveTime`
are intentional: adding "Bot" to those names makes them unused settings.

Spec probabilities are relative weights configured with
`AiPlayerbot.RandomClassSpecProbability.<class>.<spec>`. Only Wrath class IDs
1–9 and 11 are read; spec indices are 0–2. This does not add random character
generation support for a class that the factory does not already support.
Negative weights become zero; an all-zero row falls back to equal chances.

`RandomBotMaps` accepts unsigned decimal IDs of non-instanced maps present in
loaded client data. Bad tokens/maps are logged and skipped, and an empty valid
list prevents random teleport queries. Map 0 remains valid. The account prefix
must be nonempty ASCII letters/digits/underscores; invalid prefixes disable bot
initialization before account operations. Underscores match literally in account
selection, not as SQL wildcards.

The minimal `AhBot.*` helper also filters guild-task item candidates through
`MaxItemLevel`, `MaxRequiredLevel` and `IgnoreItems`. A maximum of zero means
unlimited. Pricing multipliers must be finite and positive; invalid values use
1.0, and copper prices saturate at the signed 32-bit limit. `AhBot.Enabled` and
`AhBot.UnderPriceProbability` remain compatibility-only fields, not switches for
the core's full AuctionHouseBot module.

### Quest-reward crash protection

Random-bot quest initialization skips disabled quests, including disabled
prerequisites. Disabled templates bypass the core's post-load reward validation
and can contain spell IDs absent from the loaded data. Prerequisite chains are
also traversed without recursion and deduplicated to handle broken/cyclic data.

`Player::RewardQuest` checks both completion and display reward spells instead
of asserting on a missing spell. Missing spells are logged with the quest ID,
spell ID, difficulty and player GUID; other rewards and quest-save/teleport
cleanup still run. These errors still indicate data that needs investigating;
config migration alone does not repair spell data. No bot-account deletion or
database reset is required for this fix.

The follow-up [safety audit](PlayerbotSafetyAudit.md) covers spell resets, hunter
pet/stable initialization, combat refresh, spell-dependency cycles, teleport and
numeric configuration, and related population/level/talent errors. It includes
validation results and a Windows smoke-test checklist; it is not a guarantee
that incompatible client/world data or all other server paths are safe.

See [MMAP loading and world-data fixes](MapData.md) for false load warnings even
with correctly placed files, parent/filename resolution, generator fixes and
core-derived bot spawn selection. There is no blanket requirement to move or
re-extract existing maps for those code fixes.

The subsequent [packet/byte-level audit](PacketCompatibility.md) fixes packet-type
assertions, GUID reuse, bit/bounds errors, FIFO event delivery, ready responses,
and loot/trade handling. It documents the exact wire fixtures and remaining
end-to-end/client-capture limits.

The [fresh-start/login audit](FreshLoginSafety.md) follows authentication, saved
character loading, first world entry, pending bot logins and disconnect/teardown.
It covers the new connection-state, character-data and session-ownership guards
and provides a cold-start smoke-test checklist.

---

## 5. Using bots

### In-game commands

```
.bot add <name>          add one of your own characters as a bot
.bot addclass <class>    add a random bot of the given class
.bot remove <name>       log the bot out
.bot list                list your bots
.bot init=<level>        gear/level a bot up
.rndbot <subcommand>     control the random bot population (GM/console)
```

`.bot` requires GM permission by default (`RBAC_PERM_COMMAND_GM`); the security
level required for *other people's* bots is additionally checked by
`PlayerbotSecurity`.

### Chat commands

Whisper a bot, or talk in party/raid chat, to steer it:

```
follow            follow the master
stay              hold position
flee              run away when in trouble
attack my target  focus the master's target
grind             roam on their own and kill everything they see
                (unless too strong); bots walk back if they wander
                too far from the master (say "follow" or "stay"
                afterwards for escorted or stationary grinding)
summon            teleport the bot to the master (own bots and grouped
                bots; GMs can summon any bot; not while in combat)
los / nc          list strategies (combat / non-combat)
+dps -threat      enable / disable a strategy
quests            report quest log
talk              talk to the selected quest giver
trade / buy / sell / repair / train
stats / spells / items
```

Prefix them with `AiPlayerbot.CommandPrefix` if you configured one.

### A bot that will not fight: `combat debug`

Whisper a bot `combat debug` (or say it in party/raid chat). It answers with one
line describing every gate that has to be open before the bot can hurt anything:

```
in swing range (3.00 yd of 5.00 allowed) with the swing timer still running |
tgt=alive d3d=3.00 dz=0.10 melee=5.00 inRange=1 los=1 ... atk=1 swingReady=0
swingErr=none ... cast=0 tele=0 moving=1 mayMove=1 run=4.57 | stalled=6s |
trace: A:reach melee - OK|A:melee - OK
```

Read it outside-in: `tgt=none` means target selection, not combat, is broken;
`mayMove=0`/`tele=1` means the bot is not allowed to move (rooted, mid-teleport,
stunned); `castBlocksMove=1` means a cast is holding both movement and swings;
`inRange=0` with `moving=1` and a `dz` bigger than ~1.5 is terrain (the swing test
is 3D); `inRange=1 swingErr=BadFacing` is orientation; `stalled` says how long the
state has lasted. `trace` is the engine's own per-tick decision log for that bot -
`A:<action> - USELESS|IMPOSSIBLE|FAILED|OK` is the shortest way to see which action
the AI keeps choosing.

The same line is written automatically when a bot makes no progress toward a live
target for four seconds, under `AiPlayerbot.DebugCombat = 1` (the default), as an
`ERROR` in the `playerbot` channel - so it shows with the stock `Logger.root=5` and
needs no configuration to start working. It is capped to one line per bot per
minute and one line per fifteen seconds for the whole realm; `DebugCombat = 2`
additionally logs a snapshot once per second per bot at debug level, which needs
`Logger.playerbot=2,Console Server` and is a one-bot-at-a-time tool.

### Bots have no account-wide collections

Bots log in through a socket-less `WorldSession` created with battle.net account
id `0` (`PlayerbotHolder::AddPlayerBot`). Id `0` is not a row in
`battlenet_accounts`, and `battlenet_item_appearances.battlenetAccountId` carries
a foreign key to it, so **every account-wide collection write by a bot is a
failed statement**. Players found this the hard way: each bot save appended one
`INSERT INTO battlenet_item_appearances ... VALUES (0, <block>, <mask>)` per
non-empty appearance block, MySQL rejected all of them (errno 1452), and the
worldserver console was filled with `Unhandled MySQL errno 1452` until it was
unusable. The same saves also wrote toys, heirlooms, mounts and battle pets under
account `0`.

A session without a battle.net account therefore has no account-wide collection
at all: `CollectionMgr` and `BattlePetMgr` skip such sessions, so bots

* never collect appearances (no transmog bitset, no `Transmog` update-field
  growth either - both of which cost memory and bandwidth for a client that does
  not exist),
* never collect heirlooms, toys or mounts (a bot still *learns* a mount spell it
  is given, it is only the collection entry that is skipped),
* never own battle pets: the journal stays disabled, so the bot is not taught
  `SPELL_BATTLE_PET_TRAINING` and neither trainers nor summon spells can grant it
  a pet.

Hunter/warlock **combat** pets are a different system (per character, stored in
`character_pet`) and are untouched - the AI still sends them in and they still
fight.

Cleanup for a realm that ran the affected builds: the FK rejection means
`battlenet_item_appearances` itself never kept a zero row, but the other
collection tables did. Rows for account `0` there are junk and can be removed
with, for example,
`DELETE FROM battle_pets WHERE battlenetAccountId = 0;` (repeat for
`battle_pet_slots` and `bnet_account_*` tables), which is a cleanup, not a fix -
only the code change above stops the rows from coming back.

### Random bots

With `AiPlayerbot.RandomBotAutologin = 1` the `RandomPlayerbotMgr` keeps
`MinRandomBots`..`MaxRandomBots` characters online, creating accounts named
`<RandomBotAccountPrefix><n>` and characters from `ai_playerbot_names` as
needed, then teleporting/levelling/gearing them and cycling them over time.

---

## 6. Porting notes (3.3.5 → 3.4.3)

The upstream mangosbot code targets MaNGOS/TrinityCore 3.3.5. The main API
migrations applied during the port:

* **Packets** — every `WorldSession::Handle*Opcode(WorldPacket&)` call became a
  typed `WorldPackets::X::Y` structure, e.g.
  `WorldPackets::NPC::Hello hello{WorldPacket(CMSG_TALK_TO_GOSSIP)}; hello.Unit = guid;`
* **GUIDs** — 64-bit `uint64` GUIDs replaced by 128-bit `ObjectGuid`
  (`IsEmpty()`, `GetCounter()`, `ObjectGuid::Create<HighGuid::Player>(...)`),
  including the chat-link (`|Hitem:`/`|Hplayer:`) parsing in `ChatHelper`.
* **Items** — `ItemTemplate` is accessor-only (`GetClass()`, `GetSubClass()`,
  `GetBonding()`, `GetAllowableRace()` → `Trinity::RaceMask`); item spells are
  read through the `ItemEffect*` helpers added in `playerbot.h`.
* **Quests** — `Quest::GetObjectives()` (`QuestObjective`) replaced the fixed
  `RequiredItemId[]`/`RequiredNpcOrGo[]` arrays; progress is read with
  `Player::GetQuestObjectiveData()`; `CanRewardQuest`/`RewardQuest` take a
  `LootItemType`.
* **Trainers** — `sObjectMgr->GetTrainer(entry)` plus the new `Trainer` accessors.
* **Spells** — `sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE)`,
  `SpellInfo::SpellName->Str[loc]`, `SpellInfo::CalcPowerCost(...)`.
* **Loot** — `WorldObject::HasDynamicFlag()`, `CreatureTemplate::GetDifficulty(DIFFICULTY_NONE)->GetRequiredLootSkill()`,
  `Group::CountRollVote(playerGuid, lootObjGuid, lootListId, vote)`.
* **Talents/specs** — `ActivateTalentGroup()` / `GetActiveTalentGroup()`.
* **Unit flags** — the 3.3.5 `UNIT_FLAG_NOT_SELECTABLE` (bit 0x02000000) is
  `UNIT_FLAG_UNINTERACTIBLE` / `Unit::IsUninteractible()` here, and other bits were
  renamed the same way (`UNIT_FLAG_PVP` → `UNIT_FLAG_PVP_ENABLING`,
  `UNIT_FLAG_TAXI_FLIGHT` → `UNIT_FLAG_ON_TAXI`,
  `UNIT_FLAG_DISABLE_MOVE` → `UNIT_FLAG_REMOVE_CLIENT_CONTROL`).  Bot code that asks
  "can I actually attack this unit?" goes through `Unit::isTargetableForAttack()`
  instead of hand-rolling the flag test (`PossibleTargetsValue::AcceptUnit`,
  mirroring the core's own `NearestAttackableNoTotemUnitInObjectRangeCheck`).
* **Melee range** — a swing is gated by the core on `Unit::IsWithinMeleeRange()`,
  which is 3D and reach based (`max(attackerReach + victimReach + 4/3,
  NOMINAL_MELEE_RANGE)`), while the bot's `distance` value is `GetDistance2d()`.
  Every "close enough to hit" decision in the plugin therefore asks the core
  (`MovementAction::IsInMeleeRange`) and plants the bot *inside* that envelope
  (`ComputeMeleeStopDistance`) rather than comparing the 2D number with
  `AiPlayerbot.MeleeDistance`. The mismatch is not theoretical: a bot on a slope
  stopped at 3 yards of planar distance, the core reported `NotInRange`, and the
  bot stood in its attack animation indefinitely.
* **Attack orders chase** — `AttackAction::Attack()` used to start the swing and
  return success, while the chase it depends on (`reach melee`) is pushed by the
  "enemy out of melee" trigger at *lower* priority than the attack action itself;
  a bot that was out of reach re-declared success every tick and never walked.
  The order that engages now also starts the approach, and a bot that cannot move
  says so (and drops the stance) instead of posing.
* **Database** — `PQuery`/`PExecute` use `{}` fmt placeholders instead of `%s`/`%u`.
* **Bit-packed server packets** (e.g. `SMSG_TRADE_STATUS`) are parsed with
  `ResetBitPos()` / `ReadBit()` / `ReadBits(n)`.

---

## 7. Swapping bot implementations (why not AzerothCore's mod-playerbots)

The obvious candidate for "a better set of bots" is
[`mod-playerbots`](https://github.com/mod-playerbots/mod-playerbots). Reaching
for it costs more than it buys here, for three reasons:

1. **Same lineage.** mod-playerbots *is* the same ike3/mangosbot AI that this
   plugin is - it descends from the mangosbot/celguar branch, and the module's
   own README says so. `PlayerbotAI`, the engine/strategy/value/trigger layer,
   action names, `AiPlayerbot.*` config keys and the `!follow`/`grind` command
   set are all shared. Swapping would not change how a bot decides to walk at a
   mob; it would change which fork of the same decision engine asks for the walk.
2. **It is not a drop-in module.** It requires a patched core
   (`mod-playerbots/azerothcore-wotlk`, branch `Playerbot`) whose *core*
   modifications are exactly the socketless-session/player-bypass plumbing that
   lets a bot move, act and skip client-side expectations. Adopting it means
   re-implementing those core patches for this tree anyway - which is the part
   this repository already has, hook-based and marked with `playerbot mod`.
3. **Different target.** It is written for AzerothCore 3.3.5: `ScriptedAI`/
   `PlayerScript` hook signatures, `ChatThrottleMgr`, its own `Trainer`/`Group`
   APIs, Eluna-free but AC-specific spell and packet helpers. The porting work in
   [section 6](#6-porting-notes-335--343) would be redone against a third API
   surface, and the 110 config keys, 6 SQL tables, unified
   `worldserver.conf.dist` block and the CI regression suites would all have to
   be rebuilt from scratch.

The real failure mode - "bots do not attack" - is a *core-integration* bug in
the movement/combat path (see the melee-range notes in
[section 6](#6-porting-notes-335--343)), and any bot system on a
client-authoritative-player core hits it in the same place. Use
[`combat debug`](#a-bot-that-will-not-fight-combat-debug) to locate it before
concluding the AI layer is at fault.

---

## 8. Limitations & known issues

* **Spell rotations need tuning** — the class combat strategies (warrior, mage,
  priest, etc.) were ported from the 3.3.5 mangosbot codebase and still
  reference spell names that may differ between 3.3.5 and 3.4.3.  The bots will
  fight, but their rotations may not be optimal.  The spell names are defined
  in the strategy files under `strategy/<class>/` and can be adjusted.
* **Combat is not verified end-to-end from this workspace** — the melee gate, the
  chase and the stall watchdog are covered by `tests/playerbot_combat_test.py`
  (the classifier itself is compiled and executed against fake snapshots), but no
  full Windows build or live-realm run happens here. If a bot still will not
  fight, `combat debug` and the automatic stall line say which gate is closed;
  if the answer is `tgt=none` the problem is target selection, not combat, and if
  it names terrain or a zero `run=` speed the cause is outside the bot AI.
* **Static linking required** — the plugin calls many core functions that lack
  `TC_GAME_API` exports.  Build with `-DWITH_DYNAMIC_LINKING=0` (the default).
  If `BUILD_SHARED_LIBS` is detected, CMake will emit a warning.
* **3.3.5 features without 3.4.3 equivalents** — ranged ammo checks, numeric
  gossip `OptionType` matching, and `GameObject` spellcaster teleports were
  removed rather than emulated.
* **No AhBot** — ike3's auction-house bot is not ported.  Use the core's own
  `AuctionHouseBot` module.  Only the item-pricing helpers the bots need were
  kept in `src/plugins/ahbot/`.
* **Windows only** — this source tree builds with MSVC on Windows. See the
  root README for dependencies; Linux and macOS builds are not supported.

---

## 9. CI/CD

This repository includes two GitHub Actions workflows:

### Build (`build.yml`)

Runs automatically on every push to `main` and on every pull request.

* **Windows** (Windows Server 2022, MSVC 2022): blocking build using vcpkg
  dependencies. The build must produce both worldserver and bnetserver.
* Bot regression checks run before the full build. They validate config coverage,
  types/defaults, talent weights, and compile actual quest, pet, combat-refresh,
  config/pricing and dependency-validation code against lightweight fakes (no
  database/client data needed). MMAP tests additionally compile the actual loader
  and bundled Detour and perform file loading/navigation on generated fixtures.
  Packet tests use the real byte buffer, GUID codecs and packet writers/readers,
  including exhaustive GUID mask combinations and byte-boundary truncations.
  Login tests cover frame fragmentation, auth state/ownership, corrupted saved
  character state and pending/active bot lifecycle using deterministic fakes.
  After the build, the generated `worldserver.conf.dist` is checked against the
  source template so a missing or stale config fails CI.

Run the focused checks locally with Python 3.9+ and MSVC (or g++ for the standalone
logic harness):

```bat
python -m unittest discover -s tests -p "playerbot_*_test.py" -v
python tests/playerbot_config_test.py --config build/bin/RelWithDebInfo/worldserver.conf.dist
```

These tests do not replace a full Windows build or a live-server smoke test.

### Release (`release.yml`)

Triggered **manually** from the Actions tab (`Run workflow` button).

Inputs:
* **tag** — release tag name (e.g. `v3.4.3-bots-1`), or leave empty for auto
* **prerelease** — mark as pre-release (default: true)
* **build_type** — `RelWithDebInfo` or `Release`

The workflow builds the Windows server and packages binaries, SQL schemas,
configuration files (including the unified `worldserver.conf.dist`), required
DLLs and documentation in a downloadable `.zip`.
