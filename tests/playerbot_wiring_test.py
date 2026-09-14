#!/usr/bin/env python3
"""
Playerbot wiring regression test.

The rewrite's core rule is: bots are driven through the *player* pipeline
(their own socket-less session), never through server-side movement
generators, and every core hook the design needs is actually registered.

These are grep-level guarantees over the sources - cheap, but they pin the
invariants that the "bot stands in swing range facing the wrong way" class
of bug violated:

  1. no server-side movement generator calls (MoveChase/MovePoint/...)
     anywhere in the plugin
  2. movement goes through the bot's own session (CMSG_MOVE_* packets)
  3. melee starts via Unit::Attack and faces via heartbeat orientation
  4. the auto-repeat wand/auto-shot loop is started through the real spell
  5. every core hook is registered in BotManager::Initialize
  6. the config loader reads every key documented in worldserver.conf.dist
  7. the party services (rez/cure/buff/tank/grind/consume), persistence and
     class-script override points exist

Run:  python tests/playerbot_wiring_test.py -v
"""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PLUGIN = REPO / "src" / "plugins" / "playerbot"
CORE_HOOKS = REPO / "src" / "server" / "game" / "AI" / "Playerbot" / "PlayerbotHooks.h"
CONF = REPO / "src" / "server" / "worldserver" / "worldserver.conf.dist"


def plugin_sources():
    return sorted(PLUGIN.rglob("*.cpp")) + sorted(PLUGIN.rglob("*.h"))


def plugin_text():
    return "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in plugin_sources())


class NoServerSideMovementTest(unittest.TestCase):
    """The invariant the previous port violated."""

    FORBIDDEN = (
        r"\bGetMotionMaster\(\)",
        r"\bMoveChase\b",
        r"\bMoveFollow\b",
        r"\bMovePoint\b",
        r"\bMoveJump\b",
        r"\bMoveCloserAndStop\b",
        r"\bLaunchMoveSpline\b",
        r"\bMoveSplineInit\b",
    )

    # Player::Create requires an initialized motion master exactly like the
    # core's character-creation handler does it (on a player that is never
    # in the world). That one call is legitimate and must not regress away.
    ALLOWED = ("player->GetMotionMaster()->Initialize();",)

    def test_plugin_never_starts_movement_generators(self):
        offenders = []
        for pattern in self.FORBIDDEN:
            for path in plugin_sources():
                for lineno, line in enumerate(
                        path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                    stripped = line.strip()
                    if stripped in self.ALLOWED:
                        continue
                    if re.search(pattern, line):
                        offenders.append(f"{path.name}:{lineno}: {stripped}")
        self.assertEqual(offenders, [],
                         "the bot code must drive the player, not a movement generator:\n  "
                         + "\n  ".join(offenders))

    def test_movement_goal_api_has_no_core_name_collisions(self):
        text = plugin_text()
        self.assertNotIn("void MovePoint(", text)
        self.assertIn("void MoveTo(", text)


class ClientFaithfulMovementTest(unittest.TestCase):
    def test_movement_packets_are_sent(self):
        text = (PLUGIN / "BotMovement.cpp").read_text(encoding="utf-8", errors="replace")
        for opcode in ("CMSG_MOVE_START_FORWARD", "CMSG_MOVE_HEARTBEAT", "CMSG_MOVE_STOP"):
            self.assertIn(opcode, text, f"BotMovement must send {opcode}")

    def test_packets_go_through_the_bot_session(self):
        text = (PLUGIN / "BotMovement.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("QueuePacket", text,
                      "movement packets must be queued on the bot's own session")

    def test_packet_layout_covers_movement_info(self):
        text = (PLUGIN / "BotMovement.cpp").read_text(encoding="utf-8", errors="replace")
        # the fields operator>>(ByteBuffer&, MovementInfo&) reads, in order
        for needle in ("GetGUID()", "MOVEMENTFLAG", "GameTime::GetGameTimeMS()",
                       "PositionXYZOStream()", "removeMovementForcesCount", "WriteBit"):
            self.assertIn(needle, text, f"movement packet writer missing {needle}")

    def test_facing_uses_orientation_heartbeat(self):
        text = (PLUGIN / "BotMovement.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("_wantedFacing", text)
        self.assertIn("CMSG_MOVE_HEARTBEAT", text)


class CombatWiringTest(unittest.TestCase):
    def test_melee_starts_with_unit_attack(self):
        text = (PLUGIN / "BotCombat.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertRegex(text, r"->Attack\(victim,\s*true\)")

    def test_melee_gate_matches_the_core(self):
        # The melee path must gate ONLY on the core's own swing range check
        # (IsWithinMeleeRange) - the swing arc is never gated here: an arc
        # failure is fixed by the movement turn packet (mover.Face), not by
        # re-chasing. Gating on HasInArc here re-created the wrong-facing
        # stall the old system had.
        text = (PLUGIN / "BotCombat.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("IsWithinMeleeRange", text)
        self.assertIn("GetMeleeRange", text)
        self.assertIn("mover.Face(victim)", text)
        # the in-range branch must not re-issue a Chase (that was the old bug)
        melee_block = text.split("void BotCombat::UpdateMelee")[1].split("void BotCombat::UpdateRanged")[0]
        in_range_block = melee_block.split("// In swing range:")[1]
        self.assertNotIn("Chase", in_range_block)

    def test_ranged_uses_auto_repeat_spell(self):
        text = plugin_text()
        self.assertIn("StartAutoRepeat", text)
        self.assertIn("CURRENT_AUTOREPEAT_SPELL", text)

    def test_casts_go_through_castspell(self):
        text = (PLUGIN / "BotSpells.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("CastSpell", text)


class HookRegistrationTest(unittest.TestCase):
    HOOKS = ("OnWorldUpdate", "OnPlayerUpdate", "OnBotPacketSent", "OnPlayerChat", "OnPlayerDelete")

    def test_all_hooks_registered(self):
        text = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        for hook in self.HOOKS:
            self.assertIn(f"hooks.{hook} =", text, f"hook {hook} is not registered")

    def test_enabled_flag_is_set(self):
        text = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("Playerbot::SetEnabled(true)", text)
        self.assertIn("Playerbot::SetEnabled(false)", text)

    def test_core_hook_header_declares_the_wrappers(self):
        text = CORE_HOOKS.read_text(encoding="utf-8", errors="replace")
        for hook in self.HOOKS + ("OnPlayerLogin", "OnPlayerLogout"):
            self.assertIn(f"inline void {hook}", text)

    def test_core_player_carries_the_ai(self):
        player_h = (REPO / "src" / "server" / "game" / "Entities" / "Player" / "Player.h"
                    ).read_text(encoding="utf-8", errors="replace")
        self.assertIn("BotAI* GetBotAI()", player_h)
        self.assertIn("void SetBotAI(BotAI*", player_h)


class ConfigWiringTest(unittest.TestCase):
    def test_every_conf_key_is_read_by_the_loader(self):
        loader = (PLUGIN / "BotConfig.cpp").read_text(encoding="utf-8", errors="replace")
        conf = CONF.read_text(encoding="utf-8", errors="replace")
        documented = set(re.findall(r"^(AiPlayerbot\.[A-Za-z0-9.]+)\s*=", conf, re.M))
        missing = []
        for key in sorted(documented):
            # the RandomBotMinLevel/MaxLevel pair shares one comment block
            if f'"{key}"' not in loader:
                missing.append(key)
        self.assertEqual(missing, [],
                         f"keys documented in worldserver.conf.dist but never read: {missing}")

    def test_every_read_key_is_documented(self):
        loader = (PLUGIN / "BotConfig.cpp").read_text(encoding="utf-8", errors="replace")
        conf = CONF.read_text(encoding="utf-8", errors="replace")
        documented = set(re.findall(r"^(AiPlayerbot\.[A-Za-z0-9.]+)\s*=", conf, re.M))
        read = set(re.findall(r'"(AiPlayerbot\.[A-Za-z0-9.]+)"', loader))
        undocumented = sorted(read - documented)
        self.assertEqual(undocumented, [],
                         f"keys read by BotConfig but missing from worldserver.conf.dist: {undocumented}")


class PartyServicesTest(unittest.TestCase):
    """The 'full rewrite' feature set: party care, consume, grind, roles,
    persistence. These are the guarantees that the expansion actually wired
    everything into the per-tick brain."""

    def test_party_care_pipeline_is_wired(self):
        text = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        for needle in ("UpdatePartyCare", "UpdateConsume", "UpdateGrind",
                       "FindDeadPartyMember", "FindDispelTarget", "TryConsume",
                       "RezTick", "CureTick", "BuffTick"):
            self.assertIn(needle, text, f"party service {needle} missing from the brain")

    def test_class_scripts_have_override_points(self):
        text = (PLUGIN / "BotClassAI.h").read_text(encoding="utf-8", errors="replace")
        for needle in ("virtual void RezTick", "virtual void CureTick",
                       "virtual void BuffTick", "virtual void TankTick",
                       "virtual bool CanTank"):
            self.assertIn(needle, text)

    def test_healers_resurrect(self):
        for cls in ("Priest", "Paladin", "Shaman", "Druid"):
            text = (PLUGIN / f"BotClass{cls}.cpp").read_text(encoding="utf-8", errors="replace")
            self.assertIn("void RezTick(BotAI& ai) override", text, f"{cls} cannot resurrect")

    def test_tanks_have_tank_tick(self):
        for cls in ("Warrior", "Paladin", "DeathKnight", "Druid"):
            text = (PLUGIN / f"BotClass{cls}.cpp").read_text(encoding="utf-8", errors="replace")
            self.assertIn("bool CanTank() const override { return true; }", text)
            self.assertIn("void TankTick(BotAI& ai) override", text)

    def test_tank_tick_runs_in_combat_pipeline(self):
        text = (PLUGIN / "BotCombat.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("IsTankMode()", text)
        self.assertIn("TankTick", text)

    def test_mage_conjures_food_and_water(self):
        text = (PLUGIN / "BotClassMage.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("HasConsumable(false)", text)
        self.assertIn("HasConsumable(true)", text)

    def test_hunter_pet_care(self):
        text = (PLUGIN / "BotClassHunter.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("REVIVE_PET", text)

    def test_bot_bindings_persist_and_relogin(self):
        manager = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("LoadBotsForMaster", manager)
        self.assertIn("PersistBot", manager)
        self.assertIn("hooks.OnPlayerLogin = &HookPlayerLogin", manager)
        commands = (PLUGIN / "BotCommands.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("PersistBot", commands)
        self.assertIn("ForgetBot", commands)
        sql = (REPO / "sql" / "custom" / "playerbot" / "characters_playerbot.sql").read_text(encoding="utf-8", errors="replace")
        self.assertIn("characters_playerbot", sql)

    def test_upgrade_and_repair_commands_exist(self):
        factory = (PLUGIN / "BotFactory.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("bool UpgradeGear(Player* bot)", factory)
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("DurabilityRepairAll", ai)


class AdvancedServicesTest(unittest.TestCase):
    """The 'playable world' systems: talents, duels/trade/guild, battlegrounds,
    dungeon finder, auction house - all driven through real session handlers."""

    def test_talents_module_exists_and_is_used(self):
        self.assertTrue((PLUGIN / "BotTalents.cpp").exists())
        factory = (PLUGIN / "BotFactory.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("BotTalents::SpendPoints(bot)", factory)          # on login
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("BotTalents::SpendPoints(_bot)", ai)              # on level up (periodic)
        talents = (PLUGIN / "BotTalents.cpp").read_text(encoding="utf-8", errors="replace")
        # must go through the core's validated learn path, never poked maps
        self.assertIn("LearnTalent", talents)
        self.assertNotIn("GetTalentMap", talents)

    def test_trade_and_duel_accepts_run_through_session_handlers(self):
        interact = (PLUGIN / "BotInteract.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("HandleBeginTradeOpcode", interact)
        self.assertIn("HandleAcceptTradeOpcode", interact)
        self.assertIn("HandleDuelResponseOpcode", interact)
        # only trusted players are answered
        self.assertIn("AcceptsCommandsFrom", interact)

    def test_guild_join_uses_guild_add_member(self):
        interact = (PLUGIN / "BotInteract.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("AddMember", interact)
        self.assertIn("HandleGuildLeave", interact)

    def test_bg_queue_uses_battlemaster_handler(self):
        queues = (PLUGIN / "BotQueues.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("HandleBattlemasterJoinOpcode", queues)
        self.assertIn("HandleBattleFieldPortOpcode", queues)            # enter + leave
        self.assertIn("LeaveBattleground", queues)
        self.assertIn("IsBattleMaster", queues)                         # proximity like a real player

    def test_lfg_proposal_is_accepted(self):
        queues = (PLUGIN / "BotQueues.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("SMSG_LFG_PROPOSAL_UPDATE", queues)
        self.assertIn("HandleLfgProposalResultOpcode", queues)
        self.assertIn("LFG_STATE_PROPOSAL", queues)

    def test_bg_combat_targeting_is_wired(self):
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("FindEnemyPlayer", ai)
        self.assertIn("InBattleground()", ai)

    def test_duel_fighting_is_wired(self):
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("duel->Opponent", ai)

    def test_auction_and_vendor_sell(self):
        interact = (PLUGIN / "BotInteract.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("HandleAuctionSellItem", interact)
        self.assertIn("HandleSellItemOpcode", interact)
        self.assertIn("UNIT_NPC_FLAG_AUCTIONEER", interact)

    def test_pumps_are_driven_by_the_manager(self):
        manager = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("BotInteract::PumpSession(bot)", manager)
        self.assertIn("BotQueues::PumpQueues(bot)", manager)
        self.assertIn("BotQueues::NotePacket(bot, packet)", manager)


    def test_mail_is_collected_automatically(self):
        interact = (PLUGIN / "BotInteract.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("HandleMailTakeItem", interact)
        self.assertIn("HandleMailTakeMoney", interact)
        self.assertIn("HandleMailDelete", interact)
        self.assertIn("GAMEOBJECT_TYPE_MAILBOX", interact)
        # a bot never pays COD
        self.assertIn("!mail->COD", interact)


    def test_quests_are_taken_and_turned_in(self):
        interact = (PLUGIN / "BotInteract.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("TakeMastersQuests", interact)
        self.assertIn("TurnInCompletedQuests", interact)
        self.assertIn("HandleQuestgiverHelloOpcode", interact)
        self.assertIn("HandleQuestgiverChooseRewardOpcode", interact)
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("TurnInCompletedQuests(_bot)", ai)    # automatic sweep
        self.assertIn("CommandQuests", ai)


class RestartAndSpreadTest(unittest.TestCase):
    """A restart must bring the same bots back where they were, and the pool
    must not arrive all at once or all in the same starter zone.

    These are the invariants behind 'every server start spawns 500 bots and
    the world freezes' and 'all 50 bots stand in Goldshire'."""

    def test_logins_are_paced_not_bursted(self):
        manager = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        # AddBot must queue; only StartLogin touches the session
        self.assertIn("_loginQueue.push_back(entry)", manager,
                      "AddBot logs in immediately - the startup burst is back")
        self.assertIn("void BotManager::StartLogin", manager)
        for needle in ("MaxConcurrentLogins", "LoginStaggerMs", "_startupDelayMs"):
            self.assertIn(needle, manager, f"login pacing knob {needle} is not used")
        # the audit must not re-queue what is already on the way
        self.assertIn("for (QueuedLogin const& queued : _loginQueue)", manager)

    def test_a_wedged_login_cannot_stall_the_queue(self):
        # MaxConcurrentLogins slots are a hard limit, so a login that never
        # finishes would permanently block the rest of the pool.
        manager = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("PENDING_LOGIN_TIMEOUT_MS", manager)
        self.assertIn("pending.ageMs >= PENDING_LOGIN_TIMEOUT_MS", manager)
        self.assertIn("releasing the slot", manager)
        # the half-open session must go too, or the character stays "online"
        self.assertIn("for (ObjectGuid guid : timedOut)", manager)
        self.assertIn("DestroySession(guid)", manager)

    def test_state_is_saved_and_restored(self):
        manager = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("BotState::EnsureSchema()", manager)
        self.assertIn("SaveAllStates()", manager)
        self.assertIn("RestoreRoster()", manager)
        self.assertIn("ai->ApplySavedState(saved)", manager,
                      "a bot comes back with no memory of its orders")
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("BotState::Save", ai)
        # a bot's position/level only survives if the character is actually saved
        self.assertIn("bot->SaveToDB()", manager)

    def test_pool_is_placed_by_level_across_the_world(self):
        manager = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("BotSpawns::PlaceRandomBot(bot, 250.0f)", manager,
                      "random bots are never moved off the starter-zone crowd")
        spawns = (PLUGIN / "BotSpawns.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("GetAllCreatureData", spawns,
                      "placement must come from the world's own spawn table")
        self.assertIn("Instanceable", spawns)
        self.assertIn("CREATURE_TYPE_CRITTER", spawns)
        # no hardcoded coordinates: placement is derived, never baked in
        self.assertNotRegex(spawns, r"\b-?8949\.\d", "hardcoded Goldshire-ish coordinate found")
        header = (PLUGIN / "BotSpawns.h").read_text(encoding="utf-8", errors="replace")
        self.assertIn("PlaceRandomBot", header)

    def test_state_schema_matches_the_sql_file(self):
        state = (PLUGIN / "BotState.cpp").read_text(encoding="utf-8", errors="replace")
        sql = (REPO / "sql" / "custom" / "playerbot" / "characters_playerbot.sql").read_text(
            encoding="utf-8", errors="replace")
        for column in ("random_bot", "tank_mode", "grind_mode", "stay_x", "stay_y",
                       "stay_z", "prepared_level", "last_seen"):
            self.assertIn(column, state, f"{column} is written by the code but not in the schema")
            self.assertIn(column, sql, f"{column} is missing from characters_playerbot.sql")


class RangedCombatTest(unittest.TestCase):
    """'None of the bots cast, they only auto-attack.'

    The ranged attack spell belongs to the equipped weapon, not the class, and
    it is not in the spell book - so it has to be cast triggered, and the
    auto-repeat loop has to be re-asserted while the bot stands in band."""

    def test_ranged_attack_spell_is_resolved_from_the_weapon(self):
        spells = (PLUGIN / "BotSpells.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("RangedAttackSpell", spells)
        self.assertIn("SPELL_ID_SHOOT", spells, "shoot (3018) must be the fallback for bow/gun/crossbow")
        for weapon_case in ("ITEM_SUBCLASS_WEAPON_BOW", "ITEM_SUBCLASS_WEAPON_GUN",
                            "ITEM_SUBCLASS_WEAPON_CROSSBOW", "ITEM_SUBCLASS_WEAPON_THROWN",
                            "ITEM_SUBCLASS_WEAPON_WAND"):
            self.assertIn(weapon_case, spells, f"{weapon_case} is not handled")
        header = (PLUGIN / "BotSpells.h").read_text(encoding="utf-8", errors="replace")
        self.assertIn("RangedAttackSpell", header)

    def test_ranged_attack_is_cast_triggered_when_unknown(self):
        spells = (PLUGIN / "BotSpells.cpp").read_text(encoding="utf-8", errors="replace")
        # shoot spells are not in the spell book, so a plain cast would be rejected
        self.assertIn("bool const known = Known(shootSpellId)", spells)
        self.assertIn("CastSpell(target, shootSpellId, !known)", spells,
                      "the shoot spell must be cast triggered when the bot does not know it")

    def test_auto_repeat_loop_is_maintained_in_band(self):
        combat = (PLUGIN / "BotCombat.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("MaintainAutoRepeat(victim)", combat,
                      "the wand/auto-shot loop is cancelled on movement and never restarted")
        self.assertIn("CastRetryMs", combat, "the rotation must not idle 600ms between shots")
        self.assertIn("CastMinDistance", combat, "minRange must be fed by the config")


class StalledFightTest(unittest.TestCase):
    """'Bot Perena stalled 4s on Demolitionist Legoso: ... atk=1 swingErr=none
    ... reason=retaliate' - the bot was in range, attacking, and could not
    hurt the mob. A player runs from a fight like that."""

    def test_hopeless_fights_are_abandoned(self):
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("HopelessLevelGap", ai)
        self.assertIn("FleeFrom(attacker", ai,
                      "UpdateRetaliate must give up instead of standing there")
        combat = (PLUGIN / "BotCombat.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("FleeFrom", combat)
        self.assertIn("UpdateFlee", ai)

    def test_fleeing_blocks_immediate_reengagement(self):
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertRegex(ai, r"if \(_fleeTimer > 0\)\s*\n\s*return;",
                         "UpdateRetaliate re-acquires the victim it is running from")

    def test_invalid_spells_are_never_learned(self):
        factory = (PLUGIN / "BotFactory.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("SpellMgr::IsSpellValid", factory,
                      "AddSpell(ID) is invalid spam: the spell list is not validated")
        self.assertIn("SPELL_EFFECT_NONE", factory)
        # SPELL_EFFECT_NULL does not exist in this core - it would not compile
        self.assertNotIn("SPELL_EFFECT_NULL", plugin_text())


class GroupChatCommandTest(unittest.TestCase):
    """'All commands must work in group chat, not only whisper.'"""

    def test_group_members_may_command_the_bot(self):
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("group->IsMember(sender->GetGUID())", ai,
                      "only the bound master can command the bot")

    def test_chat_is_routed_beyond_the_master(self):
        manager = (PLUGIN / "BotManager.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("ai->AcceptsCommandsFrom(sender)", manager,
                      "party/raid chat still only reaches the master's own bots")

    def test_bot_can_reply_on_the_chat_channel(self):
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("CHAT_MSG_PARTY_LEADER", ai,
                      "party replies need the core's leader/normal distinction")
        self.assertIn("BroadcastPacket", ai)
        self.assertIn("_bot->Whisper(text, LANG_UNIVERSAL, to)", ai)

    def test_command_set_covers_the_mangosbot_vocabulary(self):
        ai = (PLUGIN / "BotAI.cpp").read_text(encoding="utf-8", errors="replace")
        lowered = ai.lower()
        # adapted from ike3/mangosbot's ChatTriggerContext creator map
        for word in ("follow", "stay", "flee", "grind", "tank attack", "attack my target",
                     "max dps", "save mana", "add all loot", "repair", "talents", "spells",
                     "revive", "release", "summon", "who", "position", "formation",
                     "reset ai", "invite", "emote", "buy "):
            self.assertIn(word, lowered, f"command '{word}' is missing from the vocabulary")


class SplinePathTest(unittest.TestCase):
    """'MoveSplineInitArgs::Validate: expression _checkPathLengths() failed'.

    The validator rejects any two consecutive middle points closer than 0.1yd,
    and a failed Validate makes Launch return 0 - the bot never moves at all."""

    def test_degenerate_path_points_are_pruned_at_the_source(self):
        spline = (REPO / "src" / "server" / "game" / "Movement" / "Spline"
                  / "MoveSplineInit.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn("MIN_SPLINE_SEGMENT_LENGTH", spline)
        self.assertIn("PruneDegeneratePathPoints", spline)
        self.assertIn("MovebyPath", spline,
                      "MoveTo must prune before handing the path to the spline")
        self.assertRegex(spline, r"0\.1f",
                         "the prune threshold must match the core's 0.1 yard check")


if __name__ == "__main__":
    unittest.main(verbosity=2)
