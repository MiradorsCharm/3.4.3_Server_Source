"""Guards for the bot combat path and the "why is this bot not fighting" report.

The report this replaces was: bots engage, wave their weapon (a wand-equipped bot
repeats its shoot animation) and never deal damage, and nothing in the server said
why. Two source-level facts caused it, and both are pinned here:

- the core only lets a melee swing land inside Unit::IsWithinMeleeRange() (3D and
  combat-reach based), while every melee threshold in the bot was 2D ("distance"
  is GetDistance2d) - on a slope, a ramp or a ledge the bot obeyed its own number,
  stopped walking, and was then never in range for the swing;
- nothing was obliged to close that gap for an attack order: "reach melee" is
  pushed by a trigger at *lower* priority than the attack action, and the attack
  action returns success every tick, so the bot kept its attack stance and never
  chased.

Also pinned here: the stall report is rate limited (a stuck realm must stay
readable), and the random-bot name pool is operator data - the auto-filler that
generated and INSERTed names was removed because it spammed the console.

Run: python -m unittest discover -s tests -p "playerbot_*_test.py" -v
"""

import re
import unittest
from pathlib import Path

from playerbot_logic_test import ROOT, run_cpp


PLUGIN = "src/plugins/playerbot"
HARNESS = Path(__file__).with_name("playerbot_combat_diag_harness.cpp.in")
FACTORIES = (
    f"{PLUGIN}/RandomPlayerbotFactory.cpp",
    f"{PLUGIN}/RandomPlayerbotFactory.h",
)


def read(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


def strip_comments(text):
    """Blank out // and /* */ comments (string literals are preserved)."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)

    def repl(match):
        chunk = match.group()
        if chunk.startswith(('"', "'")):
            return chunk
        return re.sub(r"[^\n]", " ", chunk)

    return pattern.sub(repl, text)


def function_body(path, signature):
    """One whole function/method, brace-matched from the signature."""
    text = read(path)
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def class_body(path, marker):
    """One whole class definition, brace-matched from `marker`."""
    text = strip_comments(read(path))
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class CombatDiagCompileTest(unittest.TestCase):
    def test_stall_classifier_contract(self):
        """The production CombatDiag.cpp, compiled and executed as-is."""
        harness = HARNESS.read_text(encoding="utf-8")
        self.assertNotIn("@", harness, "the harness must not need substitution")
        run_cpp(self, harness, sources=[f"{PLUGIN}/CombatDiag.cpp"], includes=[PLUGIN])


class MeleeRangeSourceTest(unittest.TestCase):
    def test_melee_reach_asks_the_core_about_swing_range(self):
        body = class_body(f"{PLUGIN}/strategy/actions/ReachTargetActions.h", "class ReachMeleeAction")
        self.assertIn("IsInMeleeRange", body)
        self.assertIn("GetMeleeApproachDistance", body)
        # The 2D value is what made bots stop out of reach; melee must not use it.
        self.assertNotIn('AI_VALUE2(float, "distance"', body)

    def test_is_in_melee_range_is_the_core_predicate(self):
        header = strip_comments(read(f"{PLUGIN}/strategy/actions/MovementActions.h"))
        self.assertIn("bool IsInMeleeRange(Unit* target) const;", header)
        body = function_body(f"{PLUGIN}/strategy/actions/MovementActions.cpp",
                             "bool MovementAction::IsInMeleeRange(Unit* target) const")
        self.assertIn("bot->IsWithinMeleeRange(target)", body,
                      "the bot's melee gate must be the same call the core makes "
                      "before a swing lands (Unit::DoMeleeAttackIfReady)")

    def test_attack_order_closes_the_distance(self):
        body = function_body(f"{PLUGIN}/strategy/actions/AttackAction.cpp", "bool AttackAction::Attack(Unit* target)")
        self.assertIn("ApproachForMelee(target)", body)
        # ...and a bot that provably cannot walk must not keep a swing running it
        # can never land: that is the zombie attack-animation state users reported.
        self.assertIn("bot->AttackStop()", body)
        self.assertIn("IsInMeleeRange(target)", body)
        # Ranged bots chase with their own spell-range prereq, not into melee.
        self.assertIn("ai->IsRanged(bot)", body)

    def test_stop_distance_is_shared_by_chase_and_attack(self):
        """One place decides where a melee bot plants itself."""
        movement = function_body(f"{PLUGIN}/strategy/actions/MovementActions.cpp",
                                 "float MovementAction::GetMeleeApproachDistance(Unit* target) const")
        self.assertIn("ComputeMeleeStopDistance", movement)
        reach = class_body(f"{PLUGIN}/strategy/actions/ReachTargetActions.h", "class ReachMeleeAction")
        self.assertIn("GetMeleeApproachDistance", reach)
        attack = function_body(f"{PLUGIN}/strategy/actions/AttackAction.cpp", "bool AttackAction::Attack(Unit* target)")
        self.assertIn("ApproachForMelee", attack)


class StrategyWiringTest(unittest.TestCase):
    """Every TriggerNode/NextAction name must resolve, or the AI silently skips it.

    `Engine::ProcessTriggers()` and `Engine::CreateActionNode()` both answer an
    unknown name by *moving on*: no error, no log line at default level, just a bot
    that never chases, never leashes or never attacks. The melee chase and the
    "out of react range" leash have both been lost this way, so the wiring is
    checked wholesale instead of one name at a time.
    """

    PREFIX_EXEMPT = ("say::", "custom::")

    def _registry(self):
        names = set()
        for path in (ROOT / PLUGIN).rglob("*"):
            if path.suffix not in (".cpp", ".h"):
                continue
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            names.update(re.findall(r'creators\["([^"]+)"\]', text))
        return names

    def _used(self, pattern):
        used = {}
        for path in (ROOT / PLUGIN).rglob("*"):
            if path.suffix not in (".cpp", ".h"):
                continue
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            for match in re.finditer(pattern, text):
                used.setdefault(match.group(1), set()).add(str(path.relative_to(ROOT)))
        return used

    def test_every_trigger_node_resolves(self):
        registry = self._registry()
        used = self._used(r'new TriggerNode\(\s*"([^"]+)"')
        self.assertGreater(len(used), 50, "the trigger scan found nothing - fix the pattern")
        missing = {name: sorted(where) for name, where in used.items() if name not in registry}
        self.assertFalse(missing, "TriggerNode names with no registered trigger: %s" % missing)

    def test_every_next_action_resolves(self):
        registry = self._registry()
        # Strategy-local ActionNode factories also define names (melee, food, ...).
        for path in (ROOT / PLUGIN).rglob("*"):
            if path.suffix not in (".cpp", ".h"):
                continue
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            registry.update(re.findall(r'new ActionNode\s*\(\s*"([^"]+)"', text))
        used = self._used(r'new NextAction\(\s*"([^"]+)"')
        self.assertGreater(len(used), 100, "the action scan found nothing - fix the pattern")
        missing = {name: sorted(where) for name, where in used.items()
                   if name not in registry and not name.startswith(self.PREFIX_EXEMPT)}
        self.assertFalse(missing, "NextAction names with no registered action or action node: %s" % missing)


class CombatWatchdogTest(unittest.TestCase):
    def test_watchdog_runs_before_the_ai_can_bail_out(self):
        body = function_body(f"{PLUGIN}/PlayerbotAI.cpp", "void PlayerbotAI::UpdateAI(uint32 elapsed)")
        self.assertLess(body.index("UpdateCombatDiagnostics(elapsed)"),
                        body.index("IsBeingTeleported()"),
                        "a bot frozen by a stuck teleport or cast is exactly what has to "
                        "be reported, so the watchdog must not sit behind the early return")

    def test_reports_are_rate_limited(self):
        body = function_body(f"{PLUGIN}/PlayerbotAI.cpp", "void PlayerbotAI::UpdateCombatDiagnostics(uint32 elapsed)")
        self.assertIn("combatReportCooldownMs = 60 * 1000", body,
                      "per-bot cadence: one line per stuck bot per minute at most")
        self.assertIn("suppressed", body,
                      "population-wide cadence: hundreds of confused bots must not "
                      "produce hundreds of lines")
        self.assertRegex(body, r">=\s*15", "the global window is 15 seconds")

    def test_watchdog_is_on_by_default_and_documented(self):
        config = read(f"{PLUGIN}/PlayerbotAIConfig.cpp")
        self.assertRegex(config, r'GetIntDefault\("AiPlayerbot\.DebugCombat",\s*1\)')
        dist = read("src/server/worldserver/worldserver.conf.dist")
        self.assertIn("AiPlayerbot.DebugCombat = 1", dist)

    def test_combat_debug_command_is_fully_wired(self):
        """A command that is only half registered silently does nothing."""
        self.assertIn('creators["combat debug"]', read(f"{PLUGIN}/strategy/triggers/ChatTriggerContext.h"))
        self.assertIn('creators["combat debug"]', read(f"{PLUGIN}/strategy/actions/ChatActionContext.h"))
        self.assertIn('supported.push_back("combat debug")',
                      read(f"{PLUGIN}/strategy/generic/ChatCommandHandlerStrategy.cpp"))
        self.assertIn("CombatDebugAction", read(f"{PLUGIN}/strategy/actions/ChatActionContext.h"))


class BotConsoleNoiseTest(unittest.TestCase):
    def test_name_pool_is_data_not_generated_rows(self):
        """The auto-filler is gone (and must not come back)."""
        for path in FACTORIES:
            text = strip_comments(read(path))
            for banned in ("EnsureNamePool", "EnsureGuildNamePool", "GenerateBotName",
                           "GenerateGuildName", "CheckPlayerName", "ai_playerbot_names (name, gender)"):
                self.assertNotIn(banned, text, f"{path} still touches {banned}")

    def test_exhausted_name_pool_is_reported_once(self):
        body = function_body(FACTORIES[0], "void RandomPlayerbotFactory::CreateRandomBots()")
        self.assertIn("failedSlots", body)
        # Two lines for the whole pass, both after every loop: what the pool could
        # not supply, and what is left online. Nothing per character.
        self.assertLessEqual(body.count("TC_LOG_ERROR"), 2,
                             "the creation pass gets a summary, not one error per bot")
        for name in ("string RandomPlayerbotFactory::CreateRandomBotName()",
                     "bool RandomPlayerbotFactory::CreateRandomBot(uint8 cls)"):
            part = function_body(FACTORIES[0], name)
            self.assertNotIn("TC_LOG_ERROR", part,
                             f"{name} runs per account per class: a failure there is "
                             "counted by CreateRandomBots(), never printed here")

    def test_per_bot_lifecycle_and_stats_are_not_info(self):
        mgr = f"{PLUGIN}/RandomPlayerbotMgr.cpp"
        # PrintStats() stays at INFO because it *is* the .rndbot stats output - but it
        # must not be spammed out on every population tick, which is what buried the
        # console (twenty lines per minute per realm, forever).
        tick = function_body(mgr, "void RandomPlayerbotMgr::UpdateAIInternal(uint32")
        self.assertNotIn("PrintStats()", tick)
        stats = function_body(mgr, "void RandomPlayerbotMgr::PrintStats()")
        self.assertIn("TC_LOG_INFO", stats)
        for noisy in ('"Processing random bots..."', '"Randomizing bot {}"', '"Logging out bot {}"',
                      '"Refreshing bot {}"', '"Random teleporting bot {}"'):
            self.assertNotIn(f'TC_LOG_INFO("playerbot",  {noisy}', read(mgr),
                             f"{noisy} fires per bot or per tick; it belongs in debug")

        factory = f"{PLUGIN}/PlayerbotFactory.cpp"
        body = function_body(factory, "void PlayerbotFactory::Randomize(bool incremental)")
        self.assertEqual(1, body.count("TC_LOG_INFO"),
                         "Randomize() used to print twenty step lines per bot; keep one")


class DiagnosticHeaderStaysFakeableTest(unittest.TestCase):
    def test_snapshot_headers_have_no_core_includes(self):
        """The harness compiles the real code with no core headers; keep it that way."""
        for path in (f"{PLUGIN}/CombatSnapshotSource.h", f"{PLUGIN}/CombatDiag.h"):
            text = read(path)
            self.assertNotIn('#include "Common.h"', text, f"{path} must stay core-free")
            self.assertNotIn("Entities/", text, f"{path} must stay core-free")


if __name__ == "__main__":
    unittest.main()
