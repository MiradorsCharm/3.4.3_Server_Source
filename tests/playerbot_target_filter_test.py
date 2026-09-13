"""Regression checks for the bot's "which units can I actually attack" filter.

`PossibleTargetsValue` feeds the "possible targets" value that the grind/pull
strategies and the `no possible targets` leash trigger read.  The upstream 3.3.5
bot filtered out untargetable units with `UNIT_FLAG_NOT_SELECTABLE`; that identifier
does not exist in this core (the same bit, 0x02000000, is `UNIT_FLAG_UNINTERACTIBLE`
and the flag set as a whole is what `Unit::isTargetableForAttack()` answers), so the
bot either failed to build or happily re-targeted quest/event actors and bosses that
are mid-script, producing an attack -> instantly-dropped cycle on every AI tick.

The production body of `PossibleTargetsValue::AcceptUnit` is compiled verbatim
against API-contract fakes (see the .cpp.in), so:

- it cannot drift back to a core API 3.4.3 does not declare, and
- the filtering decisions themselves (alive, attackable, selectable, not a GM,
  hostile, or a neutral above level 1) stay pinned.

Run: python -m unittest discover -s tests -p "playerbot_*_test.py" -v
"""

import re
import unittest
from pathlib import Path

from playerbot_logic_test import ROOT, run_cpp


TARGETS_CPP = "src/plugins/playerbot/strategy/values/PossibleTargetsValue.cpp"
PLUGIN_ROOTS = ("src/plugins/playerbot", "src/plugins/ahbot")
UNIT_FLAGS = "src/server/game/Entities/Unit/UnitDefines.h"

# 3.3.5 / mangos identifiers the bot used to lean on that this core does not
# declare under these names.  `test_banned_names_are_really_absent` keeps this
# list honest: if the core ever grows one of them back, the guard is dropped for
# that name instead of failing the build for no reason.
REMOVED_335_NAMES = (
    "UNIT_FLAG_NOT_SELECTABLE",
    "UNIT_FLAG_DIZZY",
    "UNIT_FLAG_CREEPING",
    "UNIT_FLAG_DISABLE_MOVE",
    "UNIT_FLAG_TAXI_FLIGHT",
    "UNIT_FLAG_PVP",
    "UNIT_FLAG_UNK_14",
    "UNIT_BYTE2_FLAG_NOT_SELECTABLE",
)


def strip_comments(text):
    """Blank out // and /* */ comments (string/char literals are preserved)."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)

    def repl(match):
        chunk = match.group()
        if chunk.startswith(('"', "'")):
            return chunk
        return re.sub(r"[^\n]", " ", chunk)

    return pattern.sub(repl, text)


def whole_function(path, signature):
    """Extract one whole function, brace-matched from the signature."""
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def plugin_sources():
    for name in PLUGIN_ROOTS:
        for path in sorted((ROOT / name).rglob("*.cpp")):
            yield path
        for path in sorted((ROOT / name).rglob("*.h")):
            yield path


class AcceptUnitCompileTest(unittest.TestCase):
    """The real AcceptUnit body, compiled and executed against core-shaped fakes."""

    def test_accept_unit_contract(self):
        harness = Path(__file__).with_name("playerbot_targetability_harness.cpp.in").read_text(encoding="utf-8")
        harness = harness.replace("@ACCEPT_UNIT@", whole_function(TARGETS_CPP, "bool PossibleTargetsValue::AcceptUnit"))
        self.assertNotIn("@ACCEPT_UNIT@", harness)
        run_cpp(self, harness)


class TargetFilterSourceTest(unittest.TestCase):
    def test_accept_unit_uses_the_core_targetability_predicate(self):
        body = strip_comments(whole_function(TARGETS_CPP, "bool PossibleTargetsValue::AcceptUnit"))
        self.assertIn("isTargetableForAttack", body,
                      "AcceptUnit must answer targetability with the core's own "
                      "Unit::isTargetableForAttack(), not a hand-rolled flag test")
        # The core predicate already covers "is it alive"; repeating it here only
        # invites the two checks to disagree.
        self.assertNotIn("IsAlive()", body)

    def test_no_removed_335_names_in_the_plugins(self):
        offenders = {}
        for path in plugin_sources():
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            for name in REMOVED_335_NAMES:
                if re.search(r"\b%s\b" % name, text):
                    offenders.setdefault(name, []).append(str(path.relative_to(ROOT)))
        self.assertFalse(offenders, "plugin code uses core identifiers this tree does not declare: %s" % offenders)

    def test_quest_status_is_never_compared_to_a_slot_state(self):
        """`QuestStatus` and `QuestSlotStateMask` are unrelated enums.

        `GetQuestStatus()`/`QuestStatus` values must be compared against
        QUEST_STATUS_*, never against the QUEST_STATE_* slot bits - today both
        "complete" constants happen to be 1, so a mix-up compiles, warns on
        GCC (-Wenum-compare) and silently breaks the day the values move.
        """
        offenders = []
        comparison = re.compile(r"(?:==|!=)\s*QUEST_STATE_\w+|QUEST_STATE_\w+\s*(?:==|!=)")
        for path in plugin_sources():
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            if comparison.search(text):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertFalse(offenders,
                         "QuestStatus compared against a QuestSlotStateMask bit in: %s" % offenders)

    def test_banned_names_are_really_absent(self):
        """Guard the guard: every banned name must be undeclared in the core."""
        flags = (ROOT / UNIT_FLAGS).read_text(encoding="utf-8", errors="replace")
        still_missing = [name for name in REMOVED_335_NAMES
                         if not re.search(r"\b%s\b\s*=" % name, flags)]
        self.assertEqual(still_missing, list(REMOVED_335_NAMES),
                         "some guarded name is now declared in UnitDefines.h - drop it from "
                         "REMOVED_335_NAMES instead of failing the port")


if __name__ == "__main__":
    unittest.main()
