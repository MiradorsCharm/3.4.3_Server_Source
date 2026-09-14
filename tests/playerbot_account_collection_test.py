"""Guard the account-wide collection path against playerbot (battle.net id 0) writes.

Reported on a live server: the console was unusable because every bot save
appended one INSERT per non-empty appearance block for battle.net account 0:

    [ERROR]: Unhandled MySQL errno 1452 ... INSERT INTO
    `battlenet_item_appearances` (`battlenetAccountId`, `blobIndex`,
    `appearanceMask`) VALUES (0, 3714, 1024) ...

`battlenet_item_appearances.battlenetAccountId` is the only account-wide
collection column with a foreign key to `battlenet_accounts.id` (see
sql/base/auth_database.sql), and a bot's socket-less WorldSession is created
with battle.net account id 0 by PlayerbotHolder::AddPlayerBot - so every row is
rejected, every row is logged, and a bot that has collected appearances saves
thousands of them.

The invariant this pins: a session without a battle.net account has no
account-wide collection at all. It must not track toys, heirlooms, mounts or
appearances, and it must not write (or try to write) any of those tables - which
also means bots never end up with heirlooms or battle pets.

Run: python -m unittest discover -s tests -p "playerbot_*_test.py" -v
"""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COLLECTION_CPP = ROOT / "src" / "server" / "game" / "Entities" / "Player" / "CollectionMgr.cpp"
COLLECTION_H = ROOT / "src" / "server" / "game" / "Entities" / "Player" / "CollectionMgr.h"
BATTLE_PET_CPP = ROOT / "src" / "server" / "game" / "BattlePets" / "BattlePetMgr.cpp"
BATTLE_PET_H = ROOT / "src" / "server" / "game" / "BattlePets" / "BattlePetMgr.h"
ATTACK_ACTION = ROOT / "src" / "plugins" / "playerbot" / "strategy" / "actions" / "AttackAction.cpp"


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def function_body(text, signature):
    """Return the brace-balanced body of a C++ function definition."""
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    raise AssertionError(f"unbalanced braces after {signature}")


class CollectionStorageGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = read(COLLECTION_H)
        cls.text = read(COLLECTION_CPP)

    def test_helper_exists(self):
        self.assertIn("bool HasAccountStorage() const;", self.header,
                      "CollectionMgr needs a single 'is there an account to store into' test")
        self.assertIn("_owner->GetBattlenetAccountId() != 0", self.text,
                      "account storage must be decided by the battle.net account id")

    def test_saves_are_guarded(self):
        for signature in (
            "void CollectionMgr::SaveAccountToys(LoginDatabaseTransaction trans)",
            "void CollectionMgr::SaveAccountHeirlooms(LoginDatabaseTransaction trans)",
            "void CollectionMgr::SaveAccountMounts(LoginDatabaseTransaction trans)",
            "void CollectionMgr::SaveAccountItemAppearances(LoginDatabaseTransaction trans)",
        ):
            body = function_body(self.text, signature)
            guard = "if (!HasAccountStorage())"
            self.assertIn(guard, body, f"{signature} writes account-wide rows unguarded")
            if "trans->Append" in body:
                self.assertLess(body.index(guard), body.index("trans->Append"),
                                f"{signature} must bail out before building any statement")

    def test_collection_is_not_tracked_without_storage(self):
        # Appearances are the FK-carrying table: the bitset must never grow for
        # a session that cannot save it.
        body = function_body(self.text, "void CollectionMgr::AddItemAppearance(ItemModifiedAppearanceEntry const* itemModifiedAppearance)")
        self.assertIn("if (!HasAccountStorage())", body)

        body = function_body(self.text, "void CollectionMgr::AddHeirloom(uint32 itemId, uint32 flags)")
        self.assertIn("if (!HasAccountStorage())", body, "bots must not collect heirlooms")

        body = function_body(self.text, "bool CollectionMgr::AddToy(uint32 itemId, bool isFavourite, bool hasFanfare)")
        self.assertIn("if (!HasAccountStorage())", body)

    def test_mounts_still_learn_the_spell(self):
        body = function_body(self.text, "bool CollectionMgr::AddMount(uint32 spellId, MountStatusFlags flags, bool factionMount /*= false*/, bool learned /*= false*/)")
        self.assertIn("if (HasAccountStorage())", body,
                      "the mount *collection* is account-wide and must be skipped, but...")
        self.assertIn("player->LearnSpell(spellId, true);", body,
                      "...the bot still has to learn the riding spell")


class BattlePetGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = read(BATTLE_PET_H)
        cls.text = read(BATTLE_PET_CPP)

    def test_helper_checks_bot_session_and_account(self):
        body = function_body(self.text, "bool BattlePetMgr::CanStoreBattlePets() const")
        self.assertIn("IsBotSession()", body)
        self.assertIn("GetBattlenetAccountId() != 0", body)

    def test_bots_never_get_a_pet_journal(self):
        body = function_body(self.text, "bool BattlePetMgr::IsBattlePetSystemEnabled()")
        self.assertIn("CanStoreBattlePets()", body,
                      "without this the bot learns SPELL_BATTLE_PET_TRAINING and "
                      "Player::LearnSpell auto-grants it battle pets")

    def test_no_pet_rows_for_account_zero(self):
        for signature in (
            "void BattlePetMgr::SaveToDB(LoginDatabaseTransaction trans)",
            "void BattlePetMgr::LoadFromDB(PreparedQueryResult pets, PreparedQueryResult slots)",
            "void BattlePetMgr::AddPet(uint32 species, uint32 display, uint16 breed, BattlePetBreedQuality quality, uint32 spellId, uint16 level /*= 1*/)",
        ):
            body = function_body(self.text, signature)
            self.assertIn("if (!CanStoreBattlePets())", body, f"{signature} must refuse a session without an account")

    def test_definition_is_out_of_line(self):
        """The header only forward-declares WorldSession, so the check lives in the .cpp."""
        self.assertIn("bool CanStoreBattlePets() const;", self.header)
        self.assertNotIn("IsBotSession()", self.header,
                         "an inline body in this header cannot see WorldSession")


class AttackOrderEngagesTest(unittest.TestCase):
    """An attack order must create a victim, for casters and hunters too."""

    @classmethod
    def setUpClass(cls):
        cls.text = read(ATTACK_ACTION)

    def test_attack_engages_with_the_core(self):
        body = function_body(self.text, "bool AttackAction::Attack(Unit* target)")
        self.assertIn("bool const wantMelee = !ranged || inMeleeRange;", body)
        self.assertIn("bot->Attack(target, wantMelee);", body,
                      "the order must call Unit::Attack(): without it the bot has no "
                      "victim and the core thinks it is fighting nobody")

    def test_ranged_path_does_not_drop_the_victim(self):
        body = function_body(self.text, "bool AttackAction::Attack(Unit* target)")
        engage = body.index("bool const wantMelee")
        self.assertNotIn("AttackStop();", body[engage:],
                         "a ranged bot outside melee reach must be engaged with "
                         "meleeAttack=false, not disengaged")

    def test_direct_order_answers(self):
        body = function_body(self.text, "bool AttackMyTargetAction::Execute(Event event)")
        self.assertIn('"Attacking "', body,
                      "a direct attack order has to say something: silence is "
                      "indistinguishable from a command that never arrived")


if __name__ == "__main__":
    unittest.main()
