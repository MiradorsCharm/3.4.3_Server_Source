#!/usr/bin/env python3
"""
Playerbot configuration regression test.

Validates the AI PLAYERBOT SETTINGS section of the generated
worldserver.conf.dist:

  * every key BotConfig::Load() reads is present exactly once
  * no stale keys from the removed strategy-engine port or AhBot helper
  * values are syntactically valid for the core config parser
    (no inline comments, no signed junk in unsigned fields)

Run:  python tests/playerbot_config_test.py [--config path/to/worldserver.conf.dist] -v
"""

import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = REPO / "src" / "server" / "worldserver" / "worldserver.conf.dist"

# Keys the new AI reads (BotConfig::Load), with a value validator each.
EXPECTED_KEYS = {
    "AiPlayerbot.Enabled": lambda v: v in ("0", "1"),
    "AiPlayerbot.Diagnostics": lambda v: v in ("0", "1"),
    "AiPlayerbot.StallReportMs": lambda v: int(v) >= 1000,
    "AiPlayerbot.StallReportCooldownMs": lambda v: int(v) >= 1000,
    "AiPlayerbot.DebugMove": lambda v: v in ("0", "1"),
    "AiPlayerbot.SightDistance": lambda v: float(v) > 0,
    "AiPlayerbot.SpellDistance": lambda v: float(v) > 0,
    "AiPlayerbot.LootDistance": lambda v: float(v) > 0,
    "AiPlayerbot.FollowDistance": lambda v: float(v) > 0,
    "AiPlayerbot.WanderRadius": lambda v: float(v) > 0,
    "AiPlayerbot.MeleeStopFactor": lambda v: 0.1 < float(v) <= 0.95,
    "AiPlayerbot.CastStandDistance": lambda v: float(v) > 0,
    "AiPlayerbot.CastMinDistance": lambda v: float(v) > 0,
    "AiPlayerbot.AutoAssistMaster": lambda v: v in ("0", "1"),
    "AiPlayerbot.ReviveDelayMs": lambda v: int(v) > 0,
    "AiPlayerbot.RandomBotCount": lambda v: int(v) >= 0,
    "AiPlayerbot.RandomBotMinLevel": lambda v: 1 <= int(v) <= 255,
    "AiPlayerbot.RandomBotMaxLevel": lambda v: 1 <= int(v) <= 255,
    "AiPlayerbot.RandomBotUpdateInterval": lambda v: int(v) > 0,
    "AiPlayerbot.RandomBotAccountPrefix": lambda v: v.startswith('"') and v.endswith('"') and len(v) >= 3,

    # behaviour knobs the AI reads but the first version of this gate missed
    "AiPlayerbot.EatDrinkPct": lambda v: 0 <= int(v) <= 100,
    "AiPlayerbot.Grind": lambda v: v in ("0", "1"),
    "AiPlayerbot.AvoidGroundHazards": lambda v: v in ("0", "1"),
    "AiPlayerbot.HazardSafetyMargin": lambda v: float(v) >= 0,
    "AiPlayerbot.InterruptCasts": lambda v: v in ("0", "1"),

    # persistence / startup pacing: without these the pool logs in all at once
    # and every bot comes back as a fresh character in the starting zone
    "AiPlayerbot.PersistBots": lambda v: v in ("0", "1"),
    "AiPlayerbot.StateSaveIntervalMs": lambda v: int(v) >= 5000,
    "AiPlayerbot.RestoreRandomBots": lambda v: v in ("0", "1"),
    "AiPlayerbot.LoginStaggerMs": lambda v: int(v) >= 0,
    "AiPlayerbot.MaxConcurrentLogins": lambda v: int(v) >= 1,
    "AiPlayerbot.StartupLoginDelayMs": lambda v: int(v) >= 0,

    # world-wide placement
    "AiPlayerbot.RandomBotSpread": lambda v: v in ("0", "1"),
    "AiPlayerbot.RandomBotRelocateMinutes": lambda v: int(v) >= 0,
    "AiPlayerbot.RandomBotCreateBatch": lambda v: int(v) >= 1,

    # combat
    "AiPlayerbot.HopelessLevelGap": lambda v: 0 <= int(v) <= 60,
    "AiPlayerbot.CastRetryMs": lambda v: 50 <= int(v) <= 2000,
}

# Keys from the removed port / helper that must not come back silently.
FORBIDDEN_KEYS = (
    "AiPlayerbot.CommandPrefix",
    "AiPlayerbot.CommandServerPort",
    "AiPlayerbot.GlobalCooldown",
    "AiPlayerbot.MaxWaitForMove",
    "AiPlayerbot.ReactDelay",
    "AiPlayerbot.IterationsPerTick",
    "AiPlayerbot.ReactDistance",
    "AiPlayerbot.GrindDistance",
    "AiPlayerbot.FleeDistance",
    "AiPlayerbot.TooCloseDistance",
    "AiPlayerbot.MeleeDistance",
    "AiPlayerbot.WhisperDistance",
    "AiPlayerbot.ContactDistance",
    "AiPlayerbot.FleeingEnabled",
    "AiPlayerbot.CriticalHealth",
    "AiPlayerbot.LowHealth",
    "AiPlayerbot.MediumHealth",
    "AiPlayerbot.AlmostFullHealth",
    "AiPlayerbot.LowMana",
    "AiPlayerbot.MediumMana",
    "AiPlayerbot.CombatStrategies",
    "AiPlayerbot.NonCombatStrategies",
    "AiPlayerbot.RandomBotCombatStrategies",
    "AiPlayerbot.RandomBotNonCombatStrategies",
    "AiPlayerbot.RandomBotAutologin",
    "AiPlayerbot.RandomBotLoginAtStartup",
    "AiPlayerbot.RandomBotJoinLfg",
    "AiPlayerbot.MinRandomBots",
    "AiPlayerbot.MaxRandomBots",
    "AiPlayerbot.RandomBotMaps",
    "AiPlayerbot.RandomBotTeleLevel",
    "AiPlayerbot.RandomBotTeleportDistance",
    "AiPlayerbot.RandomBotQuestItems",
    "AiPlayerbot.RandomBotSpellIds",
    "AiPlayerbot.RandomBotGuildCount",
    "AiPlayerbot.EnableGuildTasks",
    "AiPlayerbot.RandomClassSpecProbability",
    "AhBot.",
)


def parse_config(path: Path):
    """Return {key: [values]} in file order; inline comments make a value
    invalid by definition of the core parser."""
    entries = {}
    for lineno, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("["):
            continue
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        entries.setdefault(key, []).append((value, lineno))
    return entries


class PlayerbotConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cfg = sys.argv[sys.argv.index("--config") + 1] if "--config" in sys.argv else None
        cls.path = Path(cfg) if cfg else DEFAULT_CONFIG
        cls.entries = parse_config(cls.path)

    def test_config_file_exists(self):
        self.assertTrue(self.path.exists(), f"{self.path} is missing")

    def test_all_expected_keys_present_and_valid(self):
        problems = []
        for key, check in EXPECTED_KEYS.items():
            values = self.entries.get(key)
            if not values:
                problems.append(f"{key}: MISSING from {self.path}")
                continue
            if len(values) > 1:
                problems.append(f"{key}: defined {len(values)} times (lines {[l for _, l in values]}) - "
                                "the config parser would silently use the first")
                continue
            value, lineno = values[0]
            try:
                if not check(value):
                    problems.append(f"{key}: bad value '{value}' (line {lineno})")
            except ValueError:
                problems.append(f"{key}: unparseable value '{value}' (line {lineno})")
        self.assertEqual(problems, [], "configuration problems:\n  " + "\n  ".join(problems))

    def test_no_stale_keys(self):
        stale = [key for key in self.entries
                 if any(key.startswith(prefix) for prefix in FORBIDDEN_KEYS)]
        self.assertEqual(stale, [], f"stale keys still in the template: {stale}")

    def test_no_inline_comments_after_values(self):
        offenders = []
        for lineno, raw in enumerate(self.path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            if key.strip().startswith("AiPlayerbot.") and "#" in value:
                offenders.append((key.strip(), lineno))
        self.assertEqual(offenders, [], "inline comments after values break the parser")

    def test_min_max_pairs_consistent(self):
        def val(key):
            values = self.entries.get(key)
            return int(values[0][0]) if values else None
        lo, hi = val("AiPlayerbot.RandomBotMinLevel"), val("AiPlayerbot.RandomBotMaxLevel")
        self.assertIsNotNone(lo)
        self.assertIsNotNone(hi)
        self.assertLessEqual(lo, hi)
        self.assertLessEqual(float(self.entries["AiPlayerbot.CastMinDistance"][0][0]),
                             float(self.entries["AiPlayerbot.CastStandDistance"][0][0]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
