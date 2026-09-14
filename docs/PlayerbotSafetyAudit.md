# Playerbot initialization safety audit

Date: 2026-09-07

## Scope and confidence

Follow-up to the `SpellMgr::AssertSpellInfo` failure reached from
`PlayerbotFactory::InitQuests` / `Player::RewardQuest`.

Reviewed the adjacent path:

```text
World update
  RandomPlayerbotMgr::ProcessBot
    RandomizeFirst / IncreaseLevel
      PlayerbotFactory::Randomize / Refresh
        level/stat preparation, spell reset, quests, talents, equipment, pets
    RandomTeleport
      combat refresh / revival
```

Also inspected configuration feeding this path, account population, pricing,
and the core spell-dependency loader. The issues below are concrete source-code
failure paths, not claims that every one occurred in the supplied Windows crash.
Regression checks use current production functions/blocks with lightweight API
fakes. No full Windows/MSVC server build or live-database run was performed in
this Linux workspace.

## Findings addressed

| Area | Failure or incorrect behavior | Change |
| --- | --- | --- |
| Spell reset and pet autocast | `ClearSpells` and `InitPet` dereferenced a missing `SpellInfo` before checking it. | Check lookups, log bot/spell identifiers, and skip missing entries. Spell removal still uses a snapshot, because removal can change the spellbook. |
| Hunter pet creation | A pet was published before `InitTamedPet`, and its failure result was ignored. A dismissed/loading pet could already occupy the stable; saving the new, incompletely initialized pet could reach the stable/pet-number assertion. | Use `CreateTamedPetFrom`, honor failure, preserve existing current/unslotted pets, publish only an initialized pet, and undo the newly reserved stable slot if map insertion fails. |
| Hunter pet selection | Candidate creatures were required to have individual `pet_levelstats` rows, although hunter pets use entry **1** and the core has fallback stats. | Delegate stats initialization to the core tame helper instead of rejecting otherwise tameable creatures. |
| Combat refresh | Calling `CombatStop` on opponents while iterating the bot's threatened-by-me list removes/frees the very references being iterated. It also stopped those opponents' combat with unrelated players. | Stop this bot's combat through the core, then remove residual threat references through the core API. Do not iterate/mutate the threat container or stop unrelated combat. |
| Spell prerequisites | Cycles accepted from `spell_required` can recurse indefinitely during spell learning/removal, including bot resets. Rank links can complete a cycle even when the requirement-only graph looks acyclic. | Iteratively detect cycles, including previous-rank edges, and reject/log the offending database edge before inserting it. No blanket disabling of spell assertions. |
| Map configuration and teleport data | Partial numeric tokens and raw `RandomBotMaps` text could reach SQL; an empty list produced `IN ()`. Bad coordinates/tiny cell sizes could reach unsafe grid conversions. | Strict unsigned-decimal parsing, deduplication, loaded non-instance map checks, canonical numeric SQL lists, empty-list guards, coordinate checks, and bounded cell-size conversion. Random teleport no longer converts a float distance to `uint32` for its RNG. |
| Numeric settings and random rolls | Negative settings wrapped into huge unsigned counts/timers. Startup multiplication could overflow. Tiny positive change multipliers could overflow integer RNG bounds; the old trigger used unsafe integer multiplication/modulo. | Clamp negative unsigned settings to zero, normalize ranges, bound update-interval conversion/startup multiplication, honor a zero processing budget, and saturate random-roll ranges before integer conversion. |
| Pricing and generated trade stock | Zero/tiny quality multipliers could create infinite/out-of-range stack counts or extremely long generation loops. Price estimates and buy-price conversion could overflow signed integers. | Require finite positive multipliers, calculate with doubles, saturate signed prices, and bound generated trade stock to seven stacks. |
| Account prefix and population | Empty/quoted/wildcard prefixes could break or broaden account SQL, including optional deletion. `_` in a legitimate prefix acted as a LIKE wildcard. Unsupported retail classes prematurely stopped creation; race lists grew on every constructor call. A failed creation pass also hid later existing accounts. | Validate prefixes before account operations, escape literal underscores, initialize race lists once, restrict generation to implemented classes, enforce the existing ten-character cap during creation, and keep registering existing accounts after creation stops. |
| Levels, stats and equipment | `SetLevel` changed the level field without recalculating stats. Quest XP could leave stats at a temporary higher level. Level zero was accepted by the core maximum-level check, and console refresh temporarily set level-1 bots to zero. Unsigned `level - delta` rejected low-level gear. | Use the core `GiveLevel` path, clamp factory levels, reject zero `MaxPlayerLevel`, refresh at the current level, and avoid unsigned subtraction wrap. |
| Talent weights and item filters | Talent selection ignored the third configured weight and treated relative weights as percentages. Default zero AhBot item-level limits filtered out nearly every guild-task item. | Use all three relative weights, reject negative weights, give all-zero rows equal chances, and treat zero item limits as unlimited. |

Primary files:

- `src/plugins/playerbot/PlayerbotFactory.cpp`
- `src/plugins/playerbot/RandomPlayerbotMgr.cpp`
- `src/plugins/playerbot/RandomPlayerbotFactory.cpp`
- `src/plugins/playerbot/PlayerbotAIConfig.{h,cpp}`
- `src/plugins/playerbot/strategy/triggers/GenericTriggers.cpp`
- `src/plugins/playerbot/strategy/actions/LfgActions.cpp`
- `src/plugins/ahbot/AhBot{,Config}.cpp`
- `src/plugins/playerbot/RandomItemMgr.cpp`
- `src/server/game/Spells/SpellMgr.cpp`
- `src/server/game/World/World.cpp`

The previous quest-reward guards and the unified `worldserver.conf.dist` remain
in place. All 110 bot settings still have active entries in that template.

## Validation performed

```bat
python -m unittest discover -s tests -p "playerbot_*_test.py" -v
```

**At the time of this audit: 10 tests passed.** The subsequent MMAP/world-data
follow-up adds real loader/Detour coverage. With the subsequent
[packet audit](PacketCompatibility.md) and [login audit](FreshLoginSafety.md),
the combined suite now has 23 tests. The compiled harnesses cover multiple scenarios per
test, including:

- Missing main/display reward spells, continued finalization, and preserved
  caster/difficulty behavior.
- Disabled/shared/cyclic/deep quest chains and prerequisite eligibility.
- Missing, removed, disabled and passive spellbook/pet entries.
- Occupied/unslotted pet stables, tame failure, map insertion failure, cleanup,
  valid publication/save order, and unavailable/teleporting players.
- Threat-reference deletion while a creature remains in combat with another
  player; bot revival and absent-AI handling.
- Invalid/empty/duplicate map tokens, map 0, instanced/missing maps, bad
  coordinates, negative/non-finite distance values and empty-list query guards.
- Negative/huge counts, zero/overflowing update intervals, NaN/infinite/tiny
  multipliers, price saturation, bounded stock, and zero/unlimited item limits.
- Invalid account prefixes, literal underscore matching, supported generation
  classes, character cap, and idempotent race initialization.
- Valid/invalid levels, synchronized level/stat updates, low-level equipment,
  third/all-zero/large talent weights, and unsigned startup-budget bounds.
- Self/multi-node/rank-linked spell prerequisite cycles, shared prerequisites,
  valid cross-rank progression and deep iterative dependency checking.

GCC harnesses run with checked STL iterators and undefined-behavior /
floating-to-integer conversion sanitizers. Twelve generated-code mutation checks
also confirmed that removing key guards makes the regressions fail, including
the original threat-list iterator mutation. Those checks did not modify the
working sources or connect to a database.

The existing Windows CI command automatically discovers these additional tests.
The tests do **not** prove complete core/API integration, MSVC compilation,
multithreaded safety, or compatibility with a particular database/client build.

## Deployment and Windows smoke test

1. Back up the characters database and live configuration. Prefer a disposable
   test realm: bot initialization intentionally replaces bot inventory/talents.
   Keep both `DeleteRandomBotAccounts` and `DeleteRandomBotGuilds` at **0**.
2. Regenerate CMake, rebuild x64 **RelWithDebInfo**, and deploy `worldserver.exe`
   with its matching PDB. Keep the existing unified-config migration instructions
   in [Playerbots.md](Playerbots.md); rebuilding never overwrites live `.conf`.
3. Begin with a small population and low `MinRandomBotsPerInterval` /
   `MaxRandomBotsPerInterval`; turn `RandomBotLoginAtStartup` off during the
   smoke test to avoid its startup multiplier.
4. Exercise first initialization, later level increases, `.rndbot refresh`,
   teleport/revival, and hunter bots with both empty and already occupied pet
   stables. Check level-appropriate health/mana, saved pet numbers, and normal
   spell/talent/gear behavior. Reconnect a bot to verify persisted state.
5. Verify that refreshing/reviving a bot disengages that bot without forcing a
   creature out of combat with unrelated players. Test map transitions and
   missing-map-data handling on the test realm.
6. Watch `playerbot` and `sql.sql` logs. Missing spell/quest/dependency entries
   should identify the bad data rather than reach the fixed assertion paths.
   Keep any new crash stack together with the matching executable/PDB and logs.

## Remaining limits and data work

- This is a focused audit, not a claim that every bot action or every core path
  is crash-free. Other packet layouts, scripts and map-thread object lifetimes
  need live/integration coverage.
- Randomization still does synchronous world-thread work, including quest
  rewards and database saves. Large batches or a slow database can still stall
  the server and trip the freeze detector. Keep batches small while measuring;
  do not disable the detector to conceal a stall.
- MMAP load warnings did not establish that files were missing. The follow-up
  [MMAP/world-data fixes](MapData.md) address duplicate-load false failures,
  filename/parent resolution, generator coordinate errors and bot spawn queries.
  Remaining opcode/script/data warnings still need their affected IDs; no
  blanket data replacement is justified from a file-placement warning.
- The bundled 50-name pool cannot supply a 200-character population, and the bots
  no longer generate names to cover the difference (that auto-fill was removed:
  it wrote rows on every creation retry and spammed a validation error per rejected
  name). Add valid unused names to `ai_playerbot_names` as needed; no
  account/database reset is required by these fixes.
- Some compatibility AhBot fields still do nothing: `Enabled` and
  `UnderPriceProbability` do not implement the unported auction-house bot.
  `MaxItemLevel`, `MaxRequiredLevel` and `IgnoreItems` **do** filter guild-task
  item candidates; use explicit caps if the loaded item data includes items
  unsuitable for your realm.
