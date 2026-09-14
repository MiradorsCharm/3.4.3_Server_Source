#include "../pchdef.h"
#include "playerbot.h"
#include "PlayerbotFactory.h"
#include "DisableMgr.h"
#include "../../server/game/Guilds/GuildMgr.h"
#include "Entities/Item/ItemTemplate.h"
#include "PlayerbotAIConfig.h"
#include "dbcstore_compat.h"
#include "Miscellaneous/SharedDefines.h"
#include "../ahbot/AhBot.h"
#include "Entities/Pet/Pet.h"
#include "RandomPlayerbotFactory.h"

using namespace ai;
using namespace std;

char* strstri(const char* str1, const char* str2);

uint32 PlayerbotFactory::tradeSkills[] =
{
    SKILL_ALCHEMY,
    SKILL_ENCHANTING,
    SKILL_SKINNING,
    SKILL_JEWELCRAFTING,
    SKILL_INSCRIPTION,
    SKILL_TAILORING,
    SKILL_LEATHERWORKING,
    SKILL_ENGINEERING,
    SKILL_HERBALISM,
    SKILL_MINING,
    SKILL_BLACKSMITHING,
    SKILL_COOKING,
    SKILL_FIRST_AID,
    SKILL_FISHING
};

void PlayerbotFactory::Randomize()
{
    Randomize(true);
}

void PlayerbotFactory::Refresh()
{
    Prepare();
    InitEquipment(true);
    InitAmmo();
    InitFood();
    InitPotions();

    uint32 money = urand(level * 1000, level * 5 * 1000);
    if (bot->GetMoney() < money)
        bot->SetMoney(money);
    bot->SaveToDB();
}

void PlayerbotFactory::CleanRandomize()
{
    Randomize(false);
}

void PlayerbotFactory::Prepare()
{
    // SetLevel alone leaves health, mana, skills and talent points at the old
    // level. Keep the requested level in the core's valid range before GiveLevel.
    level = std::clamp<uint32>(level, 1, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));

    if (!itemQuality)
    {
        if (level <= 10)
            itemQuality = urand(ITEM_QUALITY_NORMAL, ITEM_QUALITY_UNCOMMON);
        else if (level <= 20)
            itemQuality = urand(ITEM_QUALITY_UNCOMMON, ITEM_QUALITY_RARE);
        else if (level <= 40)
            itemQuality = urand(ITEM_QUALITY_UNCOMMON, ITEM_QUALITY_EPIC);
        else if (level < 60)
            itemQuality = urand(ITEM_QUALITY_UNCOMMON, ITEM_QUALITY_EPIC);
        else
            itemQuality = urand(ITEM_QUALITY_RARE, ITEM_QUALITY_EPIC);
    }

    if (bot->isDead())
        bot->ResurrectPlayer(1.0f, false);

    bot->CombatStop(true);
    bot->GiveLevel(uint8(level));
    bot->SetPlayerFlagEx(PLAYER_FLAGS_EX_HIDE_HELM);
    bot->SetPlayerFlagEx(PLAYER_FLAGS_EX_HIDE_CLOAK);
}

void PlayerbotFactory::Randomize(bool incremental)
{
    TC_LOG_DEBUG("playerbot",  "Preparing to randomize...");
    Prepare();

    TC_LOG_DEBUG("playerbot",  "Resetting player...");
    bot->ResetTalents(true);
    ClearSpells();
    ClearInventory();
    bot->SaveToDB();

    TC_LOG_DEBUG("playerbot",  "Initializing quests...");
    InitQuests();
    // quest rewards boost bot level, so reduce back
    bot->GiveLevel(uint8(level));
    ClearInventory();
    bot->SetXP(0);
    CancelAuras();
    bot->SaveToDB();

    TC_LOG_DEBUG("playerbot",  "Initializing spells (step 1)...");
    InitAvailableSpells();

    TC_LOG_DEBUG("playerbot",  "Initializing skills (step 1)...");
    InitSkills();
    InitTradeSkills();

    TC_LOG_DEBUG("playerbot",  "Initializing talents...");
    InitTalents();

    TC_LOG_DEBUG("playerbot",  "Initializing spells (step 2)...");
    InitAvailableSpells();
    InitSpecialSpells();

    TC_LOG_DEBUG("playerbot",  "Initializing mounts...");
    InitMounts();

    TC_LOG_DEBUG("playerbot",  "Initializing skills (step 2)...");
    UpdateTradeSkills();
    bot->SaveToDB();

    TC_LOG_DEBUG("playerbot",  "Initializing equipmemt...");
    InitEquipment(incremental);

    TC_LOG_DEBUG("playerbot",  "Initializing bags...");
    InitBags();

    TC_LOG_DEBUG("playerbot",  "Initializing ammo...");
    InitAmmo();

    TC_LOG_DEBUG("playerbot",  "Initializing food...");
    InitFood();

    TC_LOG_DEBUG("playerbot",  "Initializing potions...");
    InitPotions();

    TC_LOG_DEBUG("playerbot",  "Initializing second equipment set...");
    InitSecondEquipmentSet();

    TC_LOG_DEBUG("playerbot",  "Initializing inventory...");
    InitInventory();

    TC_LOG_DEBUG("playerbot",  "Initializing glyphs...");
    InitGlyphs();

    TC_LOG_DEBUG("playerbot",  "Initializing guilds...");
    InitGuild();

    TC_LOG_DEBUG("playerbot",  "Initializing pet...");
    InitPet();

    TC_LOG_DEBUG("playerbot",  "Saving to DB...");
    bot->SetMoney(urand(level * 1000, level * 5 * 1000));
    bot->SaveToDB();

    // One INFO line for the whole pass instead of twenty per bot: "did the factory
    // run for this bot" is the question the step logging exists to answer, and with
    // a few hundred random bots twenty lines each is what made the console unusable.
    TC_LOG_INFO("playerbot", "Randomized bot {} to level {}{}", bot->GetName(), bot->GetLevel(),
            incremental ? " (incremental)" : "");
}

void PlayerbotFactory::InitPet()
{
    Pet* pet = bot->GetPet();
    if (!pet)
    {
        if (bot->GetClass() != CLASS_HUNTER)
            return;

        Map* map = bot->GetMap();
        if (!map || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsPositionValid())
            return;

        // No visible pet does not mean an empty stable: a pet may be dismissed,
        // temporarily unsummoned, or still loading. Never replace that pet.
        if (PetStable const* stable = bot->GetPetStable())
            if (stable->CurrentPet || stable->GetUnslottedHunterPet())
                return;

        vector<uint32> ids;
        for (auto const& pair : sObjectMgr->GetCreatureTemplates())
        {
            CreatureTemplate const& co = pair.second;
            CreatureDifficulty const* creatureDifficulty = co.GetDifficulty(DIFFICULTY_NONE);
            if (!creatureDifficulty || !co.IsTameable(false, creatureDifficulty))
                continue;

            if (creatureDifficulty->MinLevel > bot->GetLevel())
                continue;

            // Hunter stats use pet_levelstats entry 1, not the wild creature's
            // entry. Let the core tame helper initialize stats and fallback data.
            ids.push_back(pair.first);
        }

        if (ids.empty())
        {
            TC_LOG_DEBUG("playerbot", "No pets available for bot {} ({} level)", bot->GetName(), bot->GetLevel());
            return;
        }

        for (int attempt = 0; attempt < 100 && !ids.empty(); ++attempt)
        {
            uint32 index = urand(0, ids.size() - 1);
            uint32 entry = ids[index];
            ids.erase(ids.begin() + index);

            // This initializes the stable's CurrentPet and pet number BEFORE
            // publishing/saving the pet. Ignoring InitTamedPet failure used to
            // reach Pet::SavePetToDB's stable/pet-number assertion.
            pet = bot->CreateTamedPetFrom(entry, 0);
            if (!pet)
                continue;

            if (!map->AddToMap(pet->ToCreature()))
            {
                // The tame helper reserved the current slot but the pet never
                // entered the world; undo only that reservation before deleting.
                PetStable* stable = bot->GetPetStable();
                if (stable && stable->CurrentPet &&
                    stable->CurrentPet->PetNumber == pet->GetCharmInfo()->GetPetNumber())
                    stable->CurrentPet.reset();
                delete pet;
                pet = nullptr;
                continue;
            }

            bot->SetMinion(pet, true);
            pet->InitTalentForLevel();
            pet->SavePetToDB(PET_SAVE_AS_CURRENT);
            bot->PetSpellInitialize();
            TC_LOG_DEBUG("playerbot", "Bot {}: assigned pet {} ({} level)", bot->GetName(), entry, bot->GetLevel());
            break;
        }
    }

    if (!pet)
    {
        TC_LOG_DEBUG("playerbot", "Cannot create pet for bot {}", bot->GetName());
        return;
    }

    for (auto const& pair : pet->m_spells)
    {
        if (pair.second.state == PETSPELL_REMOVED)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(pair.first, DIFFICULTY_NONE);
        if (!spellInfo)
        {
            TC_LOG_DEBUG("playerbot", "Bot {} pet {} has missing spell {}; skipping autocast",
                bot->GetGUID().ToString(), pet->GetEntry(), pair.first);
            continue;
        }
        if (!spellInfo->IsPassive())
            pet->ToggleAutocast(spellInfo, true);
    }
}

void PlayerbotFactory::ClearSpells()
{
    list<uint32> spells;
    for(PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        uint32 spellId = itr->first;
        if (itr->second.state == PLAYERSPELL_REMOVED || itr->second.disabled)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spellInfo)
        {
            TC_LOG_DEBUG("playerbot", "Bot {} has missing spell {} during spell reset; skipping",
                bot->GetGUID().ToString(), spellId);
            continue;
        }
        if (spellInfo->IsPassive())
            continue;

        spells.push_back(spellId);
    }

    for (list<uint32>::iterator i = spells.begin(); i != spells.end(); ++i)
    {
        bot->RemoveSpell(*i, false, false);
    }
}

void PlayerbotFactory::InitSpells()
{
    for (int i = 0; i < 15; i++)
        InitAvailableSpells();
}

void PlayerbotFactory::InitTalents()
{
    uint8 cls = bot->GetClass();
    if (cls >= MAX_CLASSES)
        return;

    // These are relative weights, not cumulative percentages. Read all three
    // and use doubles so large weights cannot overflow a uint32 sum.
    double weights[3] = {
        double(sPlayerbotAIConfig.specProbability[cls][0]),
        double(sPlayerbotAIConfig.specProbability[cls][1]),
        double(sPlayerbotAIConfig.specProbability[cls][2])
    };
    if (weights[0] + weights[1] + weights[2] == 0.0)
        weights[0] = weights[1] = weights[2] = 1.0;

    uint32 specNo = urandweighted(3, weights);
    InitTalents(specNo);

    if (BotFreeTalentPoints(bot))
        InitTalents(2 - specNo);
}


class DestroyItemsVisitor : public IterateItemsVisitor
{
public:
    DestroyItemsVisitor(Player* bot) : IterateItemsVisitor(), bot(bot) {}

    virtual bool Visit(Item* item)
    {
        uint32 id = item->GetTemplate()->GetId();
        if (CanKeep(id))
        {
            keep.insert(id);
            return true;
        }

        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        return true;
    }

private:
    bool CanKeep(uint32 id)
    {
        if (keep.find(id) != keep.end())
            return false;

        if (sPlayerbotAIConfig.IsInRandomQuestItemList(id))
            return true;


        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id);
        // items in the bot's bags may reference entries that no longer exist
        // in the DB (deleted/changed item templates); treat those as junk
        if (proto && proto->GetClass() == ITEM_CLASS_MISCELLANEOUS && (proto->GetSubClass() == ITEM_SUBCLASS_MISCELLANEOUS_REAGENT || proto->GetSubClass() == ITEM_SUBCLASS_MISCELLANEOUS_JUNK))
            return true;

        return false;
    }

private:
    Player* bot;
    set<uint32> keep;

};

bool PlayerbotFactory::CanEquipArmor(ItemTemplate const* proto)
{
    if (bot->HasSkill(SKILL_SHIELD) && proto->GetSubClass() == ITEM_SUBCLASS_ARMOR_SHIELD)
        return true;

    if (bot->HasSkill(SKILL_PLATE_MAIL))
    {
        if (proto->GetSubClass() != ITEM_SUBCLASS_ARMOR_PLATE)
            return false;
    }
    else if (bot->HasSkill(SKILL_MAIL))
    {
        if (proto->GetSubClass() != ITEM_SUBCLASS_ARMOR_MAIL)
            return false;
    }
    else if (bot->HasSkill(SKILL_LEATHER))
    {
        if (proto->GetSubClass() != ITEM_SUBCLASS_ARMOR_LEATHER)
            return false;
    }

    if (proto->GetQuality() <= ITEM_QUALITY_NORMAL)
        return true;

    uint8 sp = 0, ap = 0, tank = 0;
    for (int j = 0; j < MAX_ITEM_PROTO_STATS; ++j)
    {
        // for ItemStatValue != 0
        if(!proto->GetStatModifierBonusAmount(j))
            continue;

        AddItemStats(proto->GetStatModifierBonusStat(j), sp, ap, tank);
    }

    return CheckItemStats(sp, ap, tank);
}

bool PlayerbotFactory::CheckItemStats(uint8 sp, uint8 ap, uint8 tank)
{
    switch (bot->GetClass())
    {
    case CLASS_PRIEST:
    case CLASS_MAGE:
    case CLASS_WARLOCK:
        if (!sp || ap > sp || tank > sp)
            return false;
        break;
    case CLASS_PALADIN:
    case CLASS_WARRIOR:
        if ((!ap && !tank) || sp > ap || sp > tank)
            return false;
        break;
    case CLASS_HUNTER:
    case CLASS_ROGUE:
        if (!ap || sp > ap || sp > tank)
            return false;
        break;
    }

    return sp || ap || tank;
}

void PlayerbotFactory::AddItemStats(uint32 mod, uint8 &sp, uint8 &ap, uint8 &tank)
{
    switch (mod)
    {
    case ITEM_MOD_HIT_RATING:
    case ITEM_MOD_CRIT_RATING:
    case ITEM_MOD_HASTE_RATING:
    case ITEM_MOD_HEALTH:
    case ITEM_MOD_STAMINA:
    case ITEM_MOD_HEALTH_REGEN:
    case ITEM_MOD_MANA:
    case ITEM_MOD_INTELLECT:
    case ITEM_MOD_SPIRIT:
    case ITEM_MOD_MANA_REGENERATION:
    case ITEM_MOD_SPELL_POWER:
    case ITEM_MOD_SPELL_PENETRATION:
    case ITEM_MOD_HIT_SPELL_RATING:
    case ITEM_MOD_CRIT_SPELL_RATING:
    case ITEM_MOD_HASTE_SPELL_RATING:
        sp++;
        break;
    }

    switch (mod)
    {
    case ITEM_MOD_HIT_RATING:
    case ITEM_MOD_CRIT_RATING:
    case ITEM_MOD_HASTE_RATING:
    case ITEM_MOD_AGILITY:
    case ITEM_MOD_STRENGTH:
    case ITEM_MOD_HEALTH:
    case ITEM_MOD_STAMINA:
    case ITEM_MOD_HEALTH_REGEN:
    case ITEM_MOD_DEFENSE_SKILL_RATING:
    case ITEM_MOD_DODGE_RATING:
    case ITEM_MOD_PARRY_RATING:
    case ITEM_MOD_BLOCK_RATING:
    case ITEM_MOD_HIT_TAKEN_MELEE_RATING:
    case ITEM_MOD_HIT_TAKEN_RANGED_RATING:
    case ITEM_MOD_HIT_TAKEN_SPELL_RATING:
    case ITEM_MOD_CRIT_TAKEN_MELEE_RATING:
    case ITEM_MOD_CRIT_TAKEN_RANGED_RATING:
    case ITEM_MOD_CRIT_TAKEN_SPELL_RATING:
    case ITEM_MOD_HIT_TAKEN_RATING:
    case ITEM_MOD_CRIT_TAKEN_RATING:
    case ITEM_MOD_RESILIENCE_RATING:
    case ITEM_MOD_BLOCK_VALUE:
        tank++;
        break;
    }

    switch (mod)
    {
    case ITEM_MOD_HEALTH:
    case ITEM_MOD_STAMINA:
    case ITEM_MOD_HEALTH_REGEN:
    case ITEM_MOD_AGILITY:
    case ITEM_MOD_STRENGTH:
    case ITEM_MOD_HIT_MELEE_RATING:
    case ITEM_MOD_HIT_RANGED_RATING:
    case ITEM_MOD_CRIT_MELEE_RATING:
    case ITEM_MOD_CRIT_RANGED_RATING:
    case ITEM_MOD_HASTE_MELEE_RATING:
    case ITEM_MOD_HASTE_RANGED_RATING:
    case ITEM_MOD_HIT_RATING:
    case ITEM_MOD_CRIT_RATING:
    case ITEM_MOD_HASTE_RATING:
    case ITEM_MOD_EXPERTISE_RATING:
    case ITEM_MOD_ATTACK_POWER:
    case ITEM_MOD_RANGED_ATTACK_POWER:
    case ITEM_MOD_ARMOR_PENETRATION_RATING:
        ap++;
        break;
    }
}

// The weapon skill behind a weapon subclass. PlayerbotFactory::InitSkills /
// SetRandomSkill only grant skills the core's SkillRaceClassInfo allows for
// the bot's race/class, so HasSkill() on this mapping is the authoritative
// "can this bot actually attack with this item" check - independent of any
// requirement the item template itself carries (the client data still ships
// Blizzard-internal QA items that carry none at all).
static uint32 WeaponSkillForWeaponSubclass(uint32 weaponSubClass)
{
    switch (weaponSubClass)
    {
        case ITEM_SUBCLASS_WEAPON_AXE:         return SKILL_AXES;
        case ITEM_SUBCLASS_WEAPON_AXE2:        return SKILL_TWO_HANDED_AXES;
        case ITEM_SUBCLASS_WEAPON_BOW:         return SKILL_BOWS;
        case ITEM_SUBCLASS_WEAPON_GUN:         return SKILL_GUNS;
        case ITEM_SUBCLASS_WEAPON_MACE:        return SKILL_MACES;
        case ITEM_SUBCLASS_WEAPON_MACE2:       return SKILL_TWO_HANDED_MACES;
        case ITEM_SUBCLASS_WEAPON_POLEARM:     return SKILL_POLEARMS;
        case ITEM_SUBCLASS_WEAPON_STAFF:       return SKILL_STAVES;
        case ITEM_SUBCLASS_WEAPON_SWORD:       return SKILL_SWORDS;
        case ITEM_SUBCLASS_WEAPON_SWORD2:      return SKILL_TWO_HANDED_SWORDS;
        case ITEM_SUBCLASS_WEAPON_THROWN:      return SKILL_THROWN;
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:    return SKILL_CROSSBOWS;
        case ITEM_SUBCLASS_WEAPON_WAND:        return SKILL_WANDS;
        case ITEM_SUBCLASS_WEAPON_DAGGER:      return SKILL_DAGGERS;
        case ITEM_SUBCLASS_WEAPON_FIST_WEAPON: return SKILL_FIST_WEAPONS;
        default:                               return 0;
    }
}

bool PlayerbotFactory::CanEquipWeapon(ItemTemplate const* proto, uint8 slot)
{
    // Three independent gates, all enforced by the core on a real player, so a
    // bot can never wear something a player could not:
    //  1. class + slot: which weapon subclasses this class may use, and in
    //     which slot (a bow is a ranged-slot weapon for hunters, not a
    //     mainhand for mages - the old check was class-only and slot-blind);
    //  2. proficiency: the bot must actually hold the weapon skill;
    //  3. inventory type vs. slot sanity.
    uint32 const sub = proto->GetSubClass();
    bool const rangedSlot = (slot == EQUIPMENT_SLOT_RANGED);
    bool const handSlot = (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND);

    bool usable = false;
    if (rangedSlot)
    {
        switch (bot->GetClass())
        {
        case CLASS_PRIEST:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
            usable = (sub == ITEM_SUBCLASS_WEAPON_WAND);
            break;
        case CLASS_HUNTER:
            usable = (sub == ITEM_SUBCLASS_WEAPON_BOW || sub == ITEM_SUBCLASS_WEAPON_GUN ||
                    sub == ITEM_SUBCLASS_WEAPON_CROSSBOW);
            break;
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
            usable = (sub == ITEM_SUBCLASS_WEAPON_BOW || sub == ITEM_SUBCLASS_WEAPON_GUN ||
                    sub == ITEM_SUBCLASS_WEAPON_CROSSBOW || sub == ITEM_SUBCLASS_WEAPON_THROWN);
            break;
        default:
            // Paladin/shaman/druid/death knight: that slot is for class
            // relics (armor-class items handled by the armor path), never a
            // weapon.
            usable = false;
            break;
        }
    }
    else if (handSlot)
    {
        switch (bot->GetClass())
        {
        case CLASS_PRIEST:
            usable = (sub == ITEM_SUBCLASS_WEAPON_STAFF || sub == ITEM_SUBCLASS_WEAPON_MACE);
            break;
        case CLASS_MAGE:
        case CLASS_WARLOCK:
            usable = (sub == ITEM_SUBCLASS_WEAPON_STAFF || sub == ITEM_SUBCLASS_WEAPON_SWORD);
            break;
        case CLASS_WARRIOR:
            usable = (sub == ITEM_SUBCLASS_WEAPON_MACE2 || sub == ITEM_SUBCLASS_WEAPON_SWORD2 ||
                    sub == ITEM_SUBCLASS_WEAPON_MACE || sub == ITEM_SUBCLASS_WEAPON_SWORD);
            break;
        case CLASS_PALADIN:
            usable = (sub == ITEM_SUBCLASS_WEAPON_MACE2 || sub == ITEM_SUBCLASS_WEAPON_SWORD2 ||
                    sub == ITEM_SUBCLASS_WEAPON_MACE || sub == ITEM_SUBCLASS_WEAPON_SWORD);
            break;
        case CLASS_SHAMAN:
            usable = (sub == ITEM_SUBCLASS_WEAPON_MACE || sub == ITEM_SUBCLASS_WEAPON_MACE2 ||
                    sub == ITEM_SUBCLASS_WEAPON_STAFF);
            break;
        case CLASS_DRUID:
            usable = (sub == ITEM_SUBCLASS_WEAPON_MACE || sub == ITEM_SUBCLASS_WEAPON_MACE2 ||
                    sub == ITEM_SUBCLASS_WEAPON_DAGGER || sub == ITEM_SUBCLASS_WEAPON_STAFF);
            break;
        case CLASS_HUNTER:
            usable = (sub == ITEM_SUBCLASS_WEAPON_AXE2 || sub == ITEM_SUBCLASS_WEAPON_SWORD2);
            break;
        case CLASS_ROGUE:
            usable = (sub == ITEM_SUBCLASS_WEAPON_DAGGER || sub == ITEM_SUBCLASS_WEAPON_SWORD ||
                    sub == ITEM_SUBCLASS_WEAPON_MACE);
            break;
        case CLASS_DEATH_KNIGHT:
            usable = (sub == ITEM_SUBCLASS_WEAPON_AXE || sub == ITEM_SUBCLASS_WEAPON_AXE2 ||
                    sub == ITEM_SUBCLASS_WEAPON_MACE || sub == ITEM_SUBCLASS_WEAPON_MACE2 ||
                    sub == ITEM_SUBCLASS_WEAPON_SWORD || sub == ITEM_SUBCLASS_WEAPON_SWORD2 ||
                    sub == ITEM_SUBCLASS_WEAPON_POLEARM);
            break;
        default:
            usable = false;
            break;
        }
    }
    else
    {
        // Spare for the bags: anything this class can use in some slot.
        return CanEquipWeapon(proto, EQUIPMENT_SLOT_MAINHAND) || CanEquipWeapon(proto, EQUIPMENT_SLOT_RANGED);
    }
    if (!usable)
        return false;

    // Proficiency: InitSkills/SetRandomSkill only granted the weapon skills
    // the core allows for this race/class. This is what keeps QA/test entries
    // of the client data off bots even when their template carries no
    // requirement of its own (a "TESTING" wand has no skill requirement - a
    // mage's SKILL_WANDS is the real gate).
    uint32 const skill = WeaponSkillForWeaponSubclass(sub);
    if (skill && !bot->HasSkill(skill))
        return false;

    // Inventory type vs. slot: a ranged-type weapon never goes into a hand
    // slot and vice versa, whatever the subclass fields claim.
    uint32 const invType = proto->GetInventoryType();
    if (rangedSlot)
        return invType == INVTYPE_RANGED || invType == INVTYPE_RANGEDRIGHT || invType == INVTYPE_THROWN;
    if (handSlot)
        return invType != INVTYPE_RANGED && invType != INVTYPE_RANGEDRIGHT && invType != INVTYPE_THROWN;

    return true;
}

// The item templates this core loads from the client data still include
// Blizzard-internal QA entries - names like "TESTING ...". They pass every
// numeric gate (level, quality, even class/subclass) and used to be equipped
// on bots; the ones flagged for no class in particular are how a mage ended
// up with a bow in the ranged slot. RandomItemMgr applies the same name
// filter for guild-task items; equipment generation must too.
bool PlayerbotFactory::IsJunkTestItem(ItemTemplate const* proto)
{
    char const* name = proto->GetDefaultLocaleName();
    if (!name || !name[0])
        return false;

    return strstri(name, "test") != nullptr
        || strstri(name, "qa") != nullptr
        || strstri(name, "deprecated") != nullptr;
}

bool PlayerbotFactory::CanEquipItem(ItemTemplate const* proto, uint32 desiredQuality)
{
    if (proto->GetDuration() & 0x80000000)
        return false;

    if (proto->GetQuality() != desiredQuality)
        return false;

    if (proto->GetBonding() == BIND_QUEST || proto->GetBonding() == BIND_ON_USE)
        return false;

    if (proto->GetClass() == ITEM_CLASS_CONTAINER)
        return true;

    uint32 requiredLevel = proto->GetBaseRequiredLevel();
    if (!requiredLevel)
        return false;

    uint32 level = bot->GetLevel();
    uint32 delta = 2;
    if (level < 15)
        delta = urand(7, 15);
    else if (proto->GetClass() == ITEM_CLASS_WEAPON || proto->GetSubClass() == ITEM_SUBCLASS_ARMOR_SHIELD)
        delta = urand(2, 3);
    else if (!(level % 10) || (level % 10) == 9)
        delta = 2;
    else if (level < 40)
        delta = urand(5, 10);
    else if (level < 60)
        delta = urand(3, 7);
    else if (level < 70)
        delta = urand(2, 5);
    else if (level < 80)
        delta = urand(2, 4);

    if (desiredQuality > ITEM_QUALITY_NORMAL &&
            (requiredLevel > level || requiredLevel < (level > delta ? level - delta : 0)))
        return false;

    for (uint32 gap = 60; gap <= 80; gap += 10)
    {
        if (level > gap && requiredLevel <= gap)
            return false;
    }

    return true;
}

void PlayerbotFactory::InitEquipment(bool incremental)
{
    DestroyItemsVisitor visitor(bot);
    IterateItems(&visitor, ITERATE_ALL_ITEMS);

    map<uint8, vector<uint32> > items;
    for(uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_TABARD || slot == EQUIPMENT_SLOT_BODY)
            continue;

        uint32 desiredQuality = itemQuality;
        if (urand(0, 100) < 100 * sPlayerbotAIConfig.randomGearLoweringChance && desiredQuality > ITEM_QUALITY_NORMAL) {
            desiredQuality--;
        }

        do
        {
            ItemTemplateContainer const& itemTemplates = sObjectMgr->GetItemTemplateStore();
            for (ItemTemplateContainer::const_iterator i = itemTemplates.begin(); i != itemTemplates.end(); ++i)
            {
                uint32 itemId = i->first;
                ItemTemplate const* proto = &i->second;
                if (!proto)
                    continue;

                if (proto->GetClass() != ITEM_CLASS_WEAPON &&
                    proto->GetClass() != ITEM_CLASS_ARMOR &&
                    proto->GetClass() != ITEM_CLASS_CONTAINER &&
                    proto->GetClass() != ITEM_CLASS_PROJECTILE)
                    continue;

                if (IsJunkTestItem(proto))
                    continue;

                if (!CanEquipItem(proto, desiredQuality))
                    continue;

                if (proto->GetClass() == ITEM_CLASS_ARMOR && (
                    slot == EQUIPMENT_SLOT_HEAD ||
                    slot == EQUIPMENT_SLOT_SHOULDERS ||
                    slot == EQUIPMENT_SLOT_CHEST ||
                    slot == EQUIPMENT_SLOT_WAIST ||
                    slot == EQUIPMENT_SLOT_LEGS ||
                    slot == EQUIPMENT_SLOT_FEET ||
                    slot == EQUIPMENT_SLOT_WRISTS ||
                    slot == EQUIPMENT_SLOT_HANDS) && !CanEquipArmor(proto))
                        continue;

                if (proto->GetClass() == ITEM_CLASS_WEAPON && !CanEquipWeapon(proto, slot))
                    continue;

                if (slot == EQUIPMENT_SLOT_OFFHAND && bot->GetClass() == CLASS_ROGUE && proto->GetClass() != ITEM_CLASS_WEAPON)
                    continue;

                uint16 dest = 0;
                if (CanEquipUnseenItem(slot, dest, itemId))
                    items[slot].push_back(itemId);
            }
        } while (items[slot].empty() && desiredQuality-- > ITEM_QUALITY_NORMAL);
    }

    for(uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_TABARD || slot == EQUIPMENT_SLOT_BODY)
            continue;

        vector<uint32>& ids = items[slot];
        if (ids.empty())
        {
            TC_LOG_DEBUG("playerbot",   "{}: no items to equip for slot {}", bot->GetName().c_str(), slot);
            continue;
        }

        for (int attempts = 0; attempts < 15; attempts++)
        {
            uint32 index = urand(0, ids.size() - 1);
            uint32 newItemId = ids[index];
            Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);

            if (incremental && !IsDesiredReplacement(oldItem)) {
                continue;
            }

            uint16 dest;
            if (!CanEquipUnseenItem(slot, dest, newItemId))
                continue;

            if (oldItem)
            {
                bot->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
                oldItem->DestroyForPlayer(bot);
            }

            Item* newItem = bot->EquipNewItem(dest, newItemId, ItemContext::NONE, true);
            if (newItem)
            {
                newItem->AddToWorld();
                bot->AutoUnequipOffhandIfNeed();
                EnchantItem(newItem);
                break;
            }
        }
    }
}

bool PlayerbotFactory::IsDesiredReplacement(Item* item)
{
    if (!item)
        return true;

    ItemTemplate const* proto = item->GetTemplate();
    int delta = 1 + (80 - bot->GetLevel()) / 10;
    return (int)bot->GetLevel() - (int)proto->GetBaseRequiredLevel() > delta;
}

void PlayerbotFactory::InitSecondEquipmentSet()
{
    if (bot->GetClass() == CLASS_MAGE || bot->GetClass() == CLASS_WARLOCK || bot->GetClass() == CLASS_PRIEST)
        return;

    map<uint32, vector<uint32> > items;

    uint32 desiredQuality = itemQuality;
    while (urand(0, 100) < 100 * sPlayerbotAIConfig.randomGearLoweringChance && desiredQuality > ITEM_QUALITY_NORMAL) {
        desiredQuality--;
    }

    do
    {
        ItemTemplateContainer const& itemTemplates = sObjectMgr->GetItemTemplateStore();
        for (ItemTemplateContainer::const_iterator i = itemTemplates.begin(); i != itemTemplates.end(); ++i)
        {
            uint32 itemId = i->first;
            ItemTemplate const* proto = &i->second;
            if (!proto)
                continue;

            if (!CanEquipItem(proto, desiredQuality))
                continue;

            if (IsJunkTestItem(proto))
                continue;

            if (proto->GetClass() == ITEM_CLASS_WEAPON)
            {
                // Spare set for the bags: anything usable in any slot.
                if (!CanEquipWeapon(proto, EQUIPMENT_SLOT_END))
                    continue;

                Item* existingItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                if (existingItem)
                {
                    switch (existingItem->GetTemplate()->GetSubClass())
                    {
                    case ITEM_SUBCLASS_WEAPON_AXE:
                    case ITEM_SUBCLASS_WEAPON_DAGGER:
                    case ITEM_SUBCLASS_WEAPON_FIST_WEAPON:
                    case ITEM_SUBCLASS_WEAPON_MACE:
                    case ITEM_SUBCLASS_WEAPON_SWORD:
                        if (proto->GetSubClass() == ITEM_SUBCLASS_WEAPON_AXE || proto->GetSubClass() == ITEM_SUBCLASS_WEAPON_DAGGER ||
                            proto->GetSubClass() == ITEM_SUBCLASS_WEAPON_FIST_WEAPON || proto->GetSubClass() == ITEM_SUBCLASS_WEAPON_MACE ||
                            proto->GetSubClass() == ITEM_SUBCLASS_WEAPON_SWORD)
                            continue;
                        break;
                    default:
                        if (proto->GetSubClass() != ITEM_SUBCLASS_WEAPON_AXE && proto->GetSubClass() != ITEM_SUBCLASS_WEAPON_DAGGER &&
                            proto->GetSubClass() != ITEM_SUBCLASS_WEAPON_FIST_WEAPON && proto->GetSubClass() != ITEM_SUBCLASS_WEAPON_MACE &&
                            proto->GetSubClass() != ITEM_SUBCLASS_WEAPON_SWORD)
                            continue;
                        break;
                    }
                }
            }
            else if (proto->GetClass() == ITEM_CLASS_ARMOR && proto->GetSubClass() == ITEM_SUBCLASS_ARMOR_SHIELD)
            {
                if (!CanEquipArmor(proto))
                    continue;

                Item* existingItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
                if (existingItem && existingItem->GetTemplate()->GetSubClass() == ITEM_SUBCLASS_ARMOR_SHIELD)
                    continue;
            }
            else
                continue;

            items[proto->GetClass()].push_back(itemId);
        }
    } while (items[ITEM_CLASS_ARMOR].empty() && items[ITEM_CLASS_WEAPON].empty() && desiredQuality-- > ITEM_QUALITY_NORMAL);

    for (map<uint32, vector<uint32> >::iterator i = items.begin(); i != items.end(); ++i)
    {
        vector<uint32>& ids = i->second;
        if (ids.empty())
        {
            TC_LOG_DEBUG("playerbot",   "{}: no items to make second equipment set for slot {}", bot->GetName().c_str(), i->first);
            continue;
        }

        for (int attempts = 0; attempts < 15; attempts++)
        {
            uint32 index = urand(0, ids.size() - 1);
            uint32 newItemId = ids[index];

            ItemPosCountVec sDest;
            Item* newItem = StoreItem(newItemId, 1);
            if (newItem)
            {
                EnchantItem(newItem);
                newItem->AddToWorld();
                break;
            }
        }
    }
}

void PlayerbotFactory::InitBags()
{
    vector<uint32> ids;

    ItemTemplateContainer const& itemTemplates = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator i = itemTemplates.begin(); i != itemTemplates.end(); ++i)
    {
        uint32 itemId = i->first;
        ItemTemplate const* proto = &i->second;
        if (!proto || proto->GetClass() != ITEM_CLASS_CONTAINER)
            continue;

        if (IsJunkTestItem(proto))
            continue;

        if (!CanEquipItem(proto, ITEM_QUALITY_NORMAL))
            continue;

        ids.push_back(itemId);
    }

    if (ids.empty())
    {
        TC_LOG_DEBUG("playerbot",  "{}: no bags found", bot->GetName().c_str());
        return;
    }

    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        for (int attempts = 0; attempts < 15; attempts++)
        {
            uint32 index = urand(0, ids.size() - 1);
            uint32 newItemId = ids[index];

            uint16 dest;
            if (!CanEquipUnseenItem(slot, dest, newItemId))
                continue;

            Item* newItem = bot->EquipNewItem(dest, newItemId, ItemContext::NONE, true);
            if (newItem)
            {
                newItem->AddToWorld();
                break;
            }
        }
    }
}

void PlayerbotFactory::EnchantItem(Item* item)
{
    if (urand(0, 100) < 100 * sPlayerbotAIConfig.randomGearLoweringChance)
        return;

    if (bot->GetLevel() < urand(40, 50))
        return;

    ItemTemplate const* proto = item->GetTemplate();
    int32 itemLevel = proto->GetItemLevel();

    static std::vector<uint32> const enchantSpellIds = []() {
        std::vector<uint32> spells;
        for (uint32 id = 0; id < sSpellStore.GetNumRows(); ++id)
        {
            SpellInfo const *entry = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE);
            if (!entry)
                continue;

            for (int j = 0; j < 3; ++j)
            {
                if (entry->GetEffect(SpellEffIndex(j)).Effect == SPELL_EFFECT_ENCHANT_ITEM &&
                    entry->GetEffect(SpellEffIndex(j)).MiscValue)
                {
                    spells.push_back(id);
                    break;
                }
            }
        }
        return spells;
    }();

    vector<uint32> ids;
    for (uint32 id : enchantSpellIds)
    {
        SpellInfo const *entry = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE);
        if (!entry)
            continue;

        int32 requiredLevel = (int32)entry->BaseLevel;
        if (requiredLevel && (requiredLevel > itemLevel || requiredLevel < itemLevel - 35))
            continue;

        if (entry->MaxLevel && level > entry->MaxLevel)
            continue;

        uint32 spellLevel = entry->SpellLevel;
        if (spellLevel && (spellLevel > level || spellLevel < level - 10))
            continue;

        for (int j = 0; j < 3; ++j)
        {
            if (entry->GetEffect(SpellEffIndex(j)).Effect != SPELL_EFFECT_ENCHANT_ITEM)
                continue;

            uint32 enchant_id = entry->GetEffect(SpellEffIndex(j)).MiscValue;
            if (!enchant_id)
                continue;

            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
            if (!enchant)
                continue;

            if (enchant->MinLevel && enchant->MinLevel > level)
                continue;

            if (enchant->MaxLevel && enchant->MaxLevel < level)
                continue;

            if (enchant->RequiredSkillID && bot->GetSkillValue(enchant->RequiredSkillID) < enchant->RequiredSkillRank)
                continue;

            uint8 sp = 0, ap = 0, tank = 0;
            for (uint32 i = 0; i < MAX_ITEM_ENCHANTMENT_EFFECTS; ++i)
            {
                if (enchant->Effect[i] != ITEM_ENCHANTMENT_TYPE_STAT)
                    continue;

                AddItemStats(enchant->EffectArg[i], sp, ap, tank);
            }

            if (!CheckItemStats(sp, ap, tank))
                continue;

            if (enchant->ConditionID && !bot->EnchantmentFitsRequirements(enchant->ConditionID, -1))
                continue;

            if (!item->IsFitToSpellRequirements(entry))
                continue;

            ids.push_back(enchant_id);
        }
    }

    if (ids.empty())
    {
        TC_LOG_DEBUG("playerbot",   "{}: no enchantments found for item {}", bot->GetName().c_str(), item->GetTemplate()->GetId());
        return;
    }

    int index = urand(0, ids.size() - 1);
    uint32 id = ids[index];

    SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(id);
    if (!enchant)
        return;

    bot->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, false);
    item->SetEnchantment(PERM_ENCHANTMENT_SLOT, id, 0, 0);
    bot->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, true);
}

bool PlayerbotFactory::CanEquipUnseenItem(uint8 slot, uint16 &dest, uint32 item)
{
    dest = 0;
    Item *pItem = Item::CreateItem(item, 1, ItemContext::NONE, bot);
    if (pItem)
    {
        InventoryResult result = bot->CanEquipItem(slot, dest, pItem, true, false);
        delete pItem;
        return result == EQUIP_ERR_OK;
    }

    return false;
}

void PlayerbotFactory::InitTradeSkills()
{
    for (int i = 0; i < sizeof(tradeSkills) / sizeof(uint32); ++i)
    {
        // Player::SetSkill(id, 0, 0, 0) on an unknown skill learns it (the
        // "new skill" branch), which can fill PLAYER_MAX_SKILLS and persist
        // zero-value professions. Only clear trades the bot already has.
        if (bot->HasSkill(tradeSkills[i]))
            bot->SetSkill(tradeSkills[i], 0, 0, 0);
    }

    vector<uint32> firstSkills;
    vector<uint32> secondSkills;
    switch (bot->GetClass())
    {
    case CLASS_WARRIOR:
    case CLASS_PALADIN:
        firstSkills.push_back(SKILL_MINING);
        secondSkills.push_back(SKILL_BLACKSMITHING);
        secondSkills.push_back(SKILL_ENGINEERING);
        break;
    case CLASS_SHAMAN:
    case CLASS_DRUID:
    case CLASS_HUNTER:
    case CLASS_ROGUE:
        firstSkills.push_back(SKILL_SKINNING);
        secondSkills.push_back(SKILL_LEATHERWORKING);
        break;
    default:
        firstSkills.push_back(SKILL_TAILORING);
        secondSkills.push_back(SKILL_ENCHANTING);
    }

    SetRandomSkill(SKILL_FIRST_AID);
    SetRandomSkill(SKILL_FISHING);
    SetRandomSkill(SKILL_COOKING);

    switch (urand(0, 3))
    {
    case 0:
        SetRandomSkill(SKILL_HERBALISM);
        SetRandomSkill(SKILL_ALCHEMY);
        break;
    case 1:
        SetRandomSkill(SKILL_HERBALISM);
        SetRandomSkill(SKILL_INSCRIPTION);
        break;
    case 2:
        SetRandomSkill(SKILL_MINING);
        SetRandomSkill(SKILL_JEWELCRAFTING);
        break;
    case 3:
        SetRandomSkill(firstSkills[urand(0, firstSkills.size() - 1)]);
        SetRandomSkill(secondSkills[urand(0, secondSkills.size() - 1)]);
        break;
    }
}

void PlayerbotFactory::UpdateTradeSkills()
{
    for (int i = 0; i < sizeof(tradeSkills) / sizeof(uint32); ++i)
    {
        if (bot->GetSkillValue(tradeSkills[i]) == 1)
            bot->SetSkill(tradeSkills[i], 0, 0, 0);
    }
}

void PlayerbotFactory::InitSkills()
{
    SetRandomSkill(SKILL_DEFENSE);
    SetRandomSkill(SKILL_SWORDS);
    SetRandomSkill(SKILL_AXES);
    SetRandomSkill(SKILL_BOWS);
    SetRandomSkill(SKILL_GUNS);
    SetRandomSkill(SKILL_MACES);
    SetRandomSkill(SKILL_TWO_HANDED_SWORDS);
    SetRandomSkill(SKILL_STAVES);
    SetRandomSkill(SKILL_TWO_HANDED_MACES);
    SetRandomSkill(SKILL_TWO_HANDED_AXES);
    SetRandomSkill(SKILL_DAGGERS);
    SetRandomSkill(SKILL_THROWN);
    SetRandomSkill(SKILL_CROSSBOWS);
    SetRandomSkill(SKILL_WANDS);
    SetRandomSkill(SKILL_POLEARMS);
    SetRandomSkill(SKILL_FIST_WEAPONS);

    if (sDB2Manager.GetSkillRaceClassInfo(SKILL_RIDING, bot->GetRace(), bot->GetClass()))
    {
        if (bot->GetLevel() >= 70)
            bot->SetSkill(SKILL_RIDING, 0, 300, 300);
        else if (bot->GetLevel() >= 60)
            bot->SetSkill(SKILL_RIDING, 0, 225, 225);
        else if (bot->GetLevel() >= 40)
            bot->SetSkill(SKILL_RIDING, 0, 150, 150);
        else if (bot->GetLevel() >= 20)
            bot->SetSkill(SKILL_RIDING, 0, 75, 75);
        else
            bot->SetSkill(SKILL_RIDING, 0, 0, 0);
    }

    uint32 skillLevel = bot->GetLevel() < 40 ? 0 : 1;
    switch (bot->GetClass())
    {
    case CLASS_DEATH_KNIGHT:
    case CLASS_WARRIOR:
    case CLASS_PALADIN:
        if (sDB2Manager.GetSkillRaceClassInfo(SKILL_PLATE_MAIL, bot->GetRace(), bot->GetClass()))
            bot->SetSkill(SKILL_PLATE_MAIL, 0, skillLevel, skillLevel);
        break;
    case CLASS_SHAMAN:
    case CLASS_HUNTER:
        if (sDB2Manager.GetSkillRaceClassInfo(SKILL_MAIL, bot->GetRace(), bot->GetClass()))
            bot->SetSkill(SKILL_MAIL, 0, skillLevel, skillLevel);
        break;
    }
}

void PlayerbotFactory::SetRandomSkill(uint16 id)
{
    // Player::_LoadSkills rejects skills with no SkillRaceClassInfo for this
    // race/class (ERROR + SKILL_DELETED). Writing them here floods the console
    // on every subsequent bot login.
    if (!sDB2Manager.GetSkillRaceClassInfo(id, bot->GetRace(), bot->GetClass()))
        return;

    uint32 maxValue = level * 5;
    uint32 curValue = urand(maxValue - level, maxValue);
    bot->SetSkill(id, 0, curValue, maxValue);
}

void PlayerbotFactory::InitAvailableSpells()
{
    bot->LearnDefaultSkills();

    static std::vector<uint32> const trainerCreatureIds = []() {
        std::vector<uint32> ids;
        for (auto const& [creatureId, _] : sObjectMgr->GetCreatureTemplates())
        {
            if (sObjectMgr->GetTrainer(creatureId))
                ids.push_back(creatureId);
        }
        return ids;
    }();

    for (uint32 creatureId : trainerCreatureIds)
    {
        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(creatureId);
        if (!trainer)
            continue;

        Trainer::Type trainerType = trainer->GetTrainerType();
        if (trainerType != Trainer::Type::Tradeskill && trainerType != Trainer::Type::Class)
            continue;

        if (trainerType == Trainer::Type::Class && trainer->GetTrainerRequirement() != bot->GetClass())
            continue;

        for (Trainer::Spell const& tSpell : trainer->GetSpells())
        {
            if (!tSpell.SpellId)
                continue;

            if (!bot->IsSpellFitByClassAndRace(tSpell.SpellId))
                continue;

            if (trainer->GetSpellStateForPlayer(bot, tSpell) != Trainer::SpellState::Available)
                continue;

            bot->LearnSpell(tSpell.SpellId, false);
        }
    }
}

void PlayerbotFactory::InitSpecialSpells()
{
    for (list<uint32>::iterator i = sPlayerbotAIConfig.randomBotSpellIds.begin(); i != sPlayerbotAIConfig.randomBotSpellIds.end(); ++i)
    {
        uint32 spellId = *i;
        bot->LearnSpell(spellId, false);
    }
}

void PlayerbotFactory::InitTalents(uint32 specNo)
{
    uint32 classMask = bot->GetClassMask();

    map<uint32, vector<TalentEntry const*> > spells;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const *talentInfo = sTalentStore.LookupEntry(i);
        if(!talentInfo)
            continue;

        TalentTabEntry const *talentTabInfo = sTalentTabStore.LookupEntry( talentInfo->TabID );
        if(!talentTabInfo || talentTabInfo->OrderIndex != specNo)
            continue;

        if( (classMask & talentTabInfo->ClassMask) == 0 )
            continue;

        spells[talentInfo->TierID].push_back(talentInfo);
    }

    uint32 freePoints = BotFreeTalentPoints(bot);
    for (map<uint32, vector<TalentEntry const*> >::iterator i = spells.begin(); i != spells.end(); ++i)
    {
        vector<TalentEntry const*> &spells = i->second;
        if (spells.empty())
        {
            TC_LOG_DEBUG("playerbot",  "{}: No spells for talent row {}", bot->GetName().c_str(), i->first);
            continue;
        }

        int attemptCount = 0;
        while (!spells.empty() && (int)freePoints - (int)BotFreeTalentPoints(bot) < 5 && attemptCount++ < 3 && BotFreeTalentPoints(bot))
        {
            int index = urand(0, spells.size() - 1);
            TalentEntry const *talentInfo = spells[index];
            int maxRank = 0;
            for (int rank = 0; rank < min((uint32)MAX_TALENT_RANK, BotFreeTalentPoints(bot)); ++rank)
            {
                uint32 spellId = talentInfo->SpellRank[rank];
                if (!spellId)
                    continue;

                maxRank = rank;
            }

            bot->LearnTalent(talentInfo->ID, maxRank);
			spells.erase(spells.begin() + index);
        }

        freePoints = BotFreeTalentPoints(bot);
    }

    for (uint8 i = 0; i < MAX_SPECIALIZATIONS; ++i)
    {
        PlayerTalentMap& talents = bot->GetPlayerTalentMap(i);
        for (PlayerTalentMap::iterator itr = talents.begin(); itr != talents.end(); ++itr)
        {
            if (itr->second.State != PLAYERSPELL_REMOVED)
                itr->second.State = PLAYERSPELL_CHANGED;
        }
    }
}

ObjectGuid PlayerbotFactory::GetRandomBot()
{
    vector<ObjectGuid> guids;
    for (list<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin(); i != sPlayerbotAIConfig.randomBotAccounts.end(); i++)
    {
        uint32 accountId = *i;
        if (!sAccountMgr->GetCharactersCount(accountId))
            continue;

        QueryResult result = CharacterDatabase.PQuery("SELECT guid FROM characters WHERE account = '{}'", accountId);
        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(fields[0].GetUInt32());
            if (!ObjectAccessor::FindPlayer(guid))
                guids.push_back(guid);
        } while (result->NextRow());
    }

    if (guids.empty())
        return ObjectGuid();

    int index = urand(0, guids.size() - 1);
    return guids[index];
}

static void AddQuestChain(uint32 questId, list<uint32>& questIds, set<uint32>& visited)
{
    // Walk iteratively so broken/cyclic PrevQuestId chains cannot overflow the
    // stack. A shared visited set also prevents rewarding common ancestors twice.
    list<uint32> chain;
    while (questId && visited.insert(questId).second)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || DisableMgr::IsDisabledFor(DISABLE_TYPE_QUEST, questId, nullptr))
            break;

        chain.push_front(questId);
        questId = uint32(std::abs(int64(quest->GetPrevQuestId())));
    }

    questIds.splice(questIds.end(), chain);
}

void PlayerbotFactory::InitQuests()
{
    ObjectMgr::QuestContainer const& questTemplates = sObjectMgr->GetQuestTemplates();
    list<uint32> questIds;
    set<uint32> visited;
    for (ObjectMgr::QuestContainer::const_iterator i = questTemplates.begin(); i != questTemplates.end(); ++i)
    {
        uint32 questId = i->first;
        Quest const* quest = &i->second;

        if (!quest->GetAllowableClasses() ||
                quest->GetQuestMinLevel() > int32(level) ||
                quest->IsDailyOrWeekly() || quest->IsRepeatable() || quest->IsMonthly() ||
                DisableMgr::IsDisabledFor(DISABLE_TYPE_QUEST, questId, bot))
            continue;

        AddQuestChain(questId, questIds, visited);
    }

    vector<Quest const*> toReward;
    for (uint32 questId : questIds)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);

        // Disabled quests bypass ObjectMgr's post-load validation and may still
        // contain invalid spell/item/mail rewards. Never force-reward them,
        // including quests reached through another quest's prerequisite chain.
        if (!quest || DisableMgr::IsDisabledFor(DISABLE_TYPE_QUEST, questId, bot))
            continue;

        // Apply these checks to prerequisites as well as the class quests.
        // Use the requested level, not levels temporarily gained from rewards.
        if (quest->GetQuestMinLevel() > int32(level) ||
                quest->IsDailyOrWeekly() || quest->IsRepeatable() || quest->IsMonthly() ||
                !bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false))
            continue;

        toReward.push_back(quest);
    }

    // RewardQuest always writes QUEST_DEFAULT_SAVE_TYPE (INSERT). That overwrites
    // RemoveRewardedQuest's QUEST_FORCE_DELETE_SAVE_TYPE, so a later SaveToDB
    // inserts character_queststatus_rewarded rows that already exist.
    for (Quest const* quest : toReward)
        bot->RemoveRewardedQuest(quest->GetQuestId(), false);
    if (!toReward.empty())
        bot->SaveToDB();

    for (Quest const* quest : toReward)
    {
        bot->RemoveActiveQuest(quest->GetQuestId(), false);
        bot->SetQuestStatus(quest->GetQuestId(), QUEST_STATUS_COMPLETE);
        bot->RewardQuest(quest, LootItemType::Item, 0, bot, false);
        ClearInventory();
    }
}

void PlayerbotFactory::ClearInventory()
{
    DestroyItemsVisitor visitor(bot);
    IterateItems(&visitor);
}

void PlayerbotFactory::InitAmmo()
{
    if (bot->GetClass() != CLASS_HUNTER && bot->GetClass() != CLASS_ROGUE && bot->GetClass() != CLASS_WARRIOR)
        return;

    Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!pItem)
        return;

    uint32 subClass = 0;
    switch (pItem->GetTemplate()->GetSubClass())
    {
    case ITEM_SUBCLASS_WEAPON_GUN:
        subClass = ITEM_SUBCLASS_BULLET;
        break;
    case ITEM_SUBCLASS_WEAPON_BOW:
    case ITEM_SUBCLASS_WEAPON_CROSSBOW:
        subClass = ITEM_SUBCLASS_ARROW;
        break;
    }

    if (!subClass)
        return;

    // Item templates in this core are built from DB2/hotfix data (ObjectMgr::LoadItemTemplates);
    // there is no world item_template table with class/subclass/RequiredLevel columns to query -
    // a bad reference raises ER_NO_SUCH_TABLE/ER_BAD_FIELD_ERROR which ABORTs the server via
    // MySQLConnection::_HandleMySQLErrno. Search the in-memory store instead: pick the ammo with
    // the highest usable RequiredLevel, tie-broken on the higher entry, same as the old SQL did.
    uint32 entry = 0;
    int32 bestLevel = -1;
    for (auto const& pair : sObjectMgr->GetItemTemplateStore())
    {
        ItemTemplate const& proto = pair.second;
        if (proto.GetClass() != ITEM_CLASS_PROJECTILE || proto.GetSubClass() != subClass)
            continue;

        int32 requiredLevel = proto.GetBaseRequiredLevel();
        if (requiredLevel > (int32)bot->GetLevel())
            continue;

        if (requiredLevel > bestLevel || (requiredLevel == bestLevel && pair.first > entry))
        {
            bestLevel = requiredLevel;
            entry = pair.first;
        }
    }

    if (entry)
    {
        for (int i = 0; i < 5; i++)
        {
            bot->StoreNewItemInBestSlots(entry, 1000, ItemContext::NONE);
        }
        bot->SetAmmo(entry);
    }
}

void PlayerbotFactory::InitMounts()
{
    map<uint32, map<int32, vector<uint32> > > allSpells;

    for (uint32 spellId = 0; spellId < sSpellStore.GetNumRows(); ++spellId)
    {
        SpellInfo const *spellInfo = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spellInfo || spellInfo->GetEffect(SpellEffIndex(0)).ApplyAuraName != SPELL_AURA_MOUNTED)
            continue;

        if (spellInfo->GetDuration() != -1)
            continue;

        int32 effect = max(spellInfo->GetEffect(SpellEffIndex(1)).BasePoints, spellInfo->GetEffect(SpellEffIndex(2)).BasePoints);
        if (effect < 50)
            continue;

        uint32 index = (spellInfo->GetEffect(SpellEffIndex(1)).ApplyAuraName == SPELL_AURA_MOD_MOUNTED_FLIGHT_SPEED_ALWAYS ||
                spellInfo->GetEffect(SpellEffIndex(2)).ApplyAuraName == SPELL_AURA_MOD_MOUNTED_FLIGHT_SPEED_ALWAYS) ? 1 : 0;
        allSpells[index][effect].push_back(spellId);
    }

    for (uint32 type = 0; type < 2; ++type)
    {
        map<int32, vector<uint32> >& spells = allSpells[type];
        for (map<int32, vector<uint32> >::iterator i = spells.begin(); i != spells.end(); ++i)
        {
            int32 effect = i->first;
            vector<uint32>& ids = i->second;
            if (ids.empty())
                continue;

            uint32 index = urand(0, ids.size() - 1);

            bot->LearnSpell(ids[index], false);
        }
    }
}

void PlayerbotFactory::InitPotions()
{
    map<uint32, vector<uint32> > items;
    ItemTemplateContainer const& itemTemplateContainer = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator i = itemTemplateContainer.begin(); i != itemTemplateContainer.end(); ++i)
    {
        ItemTemplate const& itemTemplate = i->second;
        uint32 itemId = i->first;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        if (proto->GetClass() != ITEM_CLASS_CONSUMABLE ||
            proto->GetSubClass() != ITEM_SUBCLASS_POTION ||
            ItemSpellCategory(proto, 0) != 4 ||
            proto->GetBonding() != BIND_NONE)
            continue;

        if (proto->GetBaseRequiredLevel() > bot->GetLevel() || proto->GetBaseRequiredLevel() < bot->GetLevel() - 10)
            continue;

        if (proto->GetRequiredSkill() && !bot->HasSkill(proto->GetRequiredSkill()))
            continue;

        if (proto->GetArea(0) || proto->GetMap())
            continue;

        for (int j = 0; j < MAX_ITEM_PROTO_EFFECTS; j++)
        {
            const SpellInfo* const spellInfo = sSpellMgr->GetSpellInfo(ItemSpellId(proto, j), DIFFICULTY_NONE);
            if (!spellInfo)
                continue;

            for (int i = 0 ; i < 3; i++)
            {
                if (spellInfo->GetEffect(SpellEffIndex(i)).Effect == SPELL_EFFECT_HEAL || spellInfo->GetEffect(SpellEffIndex(i)).Effect == SPELL_EFFECT_ENERGIZE)
                {
                    items[spellInfo->GetEffect(SpellEffIndex(i)).Effect].push_back(itemId);
                    break;
                }
            }
        }
    }

    uint32 effects[] = { SPELL_EFFECT_HEAL, SPELL_EFFECT_ENERGIZE };
    for (int i = 0; i < sizeof(effects) / sizeof(uint32); ++i)
    {
        uint32 effect = effects[i];
        vector<uint32>& ids = items[effect];
        if (ids.empty())
            continue;

        uint32 index = urand(0, ids.size() - 1);

        uint32 itemId = ids[index];
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;
        bot->StoreNewItemInBestSlots(itemId, urand(1, proto->GetMaxStackSize()), ItemContext::NONE);
   }
}

void PlayerbotFactory::InitFood()
{
    map<uint32, vector<uint32> > items;
    ItemTemplateContainer const& itemTemplateContainer = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator i = itemTemplateContainer.begin(); i != itemTemplateContainer.end(); ++i)
    {
        ItemTemplate const& itemTemplate = i->second;
        uint32 itemId = i->first;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        if (proto->GetClass() != ITEM_CLASS_CONSUMABLE ||
            proto->GetSubClass() != ITEM_SUBCLASS_FOOD_DRINK ||
            (ItemSpellCategory(proto, 0) != 11 && ItemSpellCategory(proto, 0) != 59) ||
            proto->GetBonding() != BIND_NONE)
            continue;

        if (proto->GetBaseRequiredLevel() > bot->GetLevel() || proto->GetBaseRequiredLevel() < bot->GetLevel() - 10)
            continue;

        if (proto->GetRequiredSkill() && !bot->HasSkill(proto->GetRequiredSkill()))
            continue;

        if (proto->GetArea(0) || proto->GetMap())
            continue;

        items[ItemSpellCategory(proto, 0)].push_back(itemId);
    }

    uint32 categories[] = { 11, 59 };
    for (int i = 0; i < sizeof(categories) / sizeof(uint32); ++i)
    {
        uint32 category = categories[i];
        vector<uint32>& ids = items[category];
        if (ids.empty())
            continue;

        uint32 index = urand(0, ids.size() - 1);

        uint32 itemId = ids[index];
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;
        bot->StoreNewItemInBestSlots(itemId, urand(1, proto->GetMaxStackSize()), ItemContext::NONE);
   }
}


void PlayerbotFactory::CancelAuras()
{
    bot->RemoveAllAuras();
}

void PlayerbotFactory::InitInventory()
{
    InitInventoryTrade();
    InitInventoryEquip();
    InitInventorySkill();
}

void PlayerbotFactory::InitInventorySkill()
{
    if (bot->HasSkill(SKILL_MINING)) {
        StoreItem(2901, 1); // Mining Pick
    }
    if (bot->HasSkill(SKILL_JEWELCRAFTING)) {
        StoreItem(20815, 1); // Jeweler's Kit
        StoreItem(20824, 1); // Simple Grinder
    }
    if (bot->HasSkill(SKILL_BLACKSMITHING) || bot->HasSkill(SKILL_ENGINEERING)) {
        StoreItem(5956, 1); // Blacksmith Hammer
    }
    if (bot->HasSkill(SKILL_ENGINEERING)) {
        StoreItem(6219, 1); // Arclight Spanner
    }
    if (bot->HasSkill(SKILL_ENCHANTING)) {
        StoreItem(44452, 1); // Runed Titanium Rod
    }
    if (bot->HasSkill(SKILL_INSCRIPTION)) {
        StoreItem(39505, 1); // Virtuoso Inking Set
    }
    if (bot->HasSkill(SKILL_SKINNING)) {
        StoreItem(7005, 1); // Skinning Knife
    }
}

Item* PlayerbotFactory::StoreItem(uint32 itemId, uint32 count)
{
    ItemPosCountVec sDest;
    InventoryResult msg = bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, sDest, itemId, count);
    if (msg != EQUIP_ERR_OK)
        return NULL;

    return bot->StoreNewItem(sDest, itemId, true);
}

void PlayerbotFactory::InitInventoryTrade()
{
    vector<uint32> ids;
    ItemTemplateContainer const& itemTemplateContainer = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator i = itemTemplateContainer.begin(); i != itemTemplateContainer.end(); ++i)
    {
        ItemTemplate const& itemTemplate = i->second;
        uint32 itemId = i->first;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        if (proto->GetClass() != ITEM_CLASS_TRADE_GOODS || proto->GetBonding() != BIND_NONE)
            continue;

        if (proto->GetItemLevel() < bot->GetLevel())
            continue;

        if (proto->GetBaseRequiredLevel() > bot->GetLevel() || proto->GetBaseRequiredLevel() < bot->GetLevel() - 10)
            continue;

        if (proto->GetRequiredSkill() && !bot->HasSkill(proto->GetRequiredSkill()))
            continue;

        ids.push_back(itemId);
    }

    if (ids.empty())
    {
        TC_LOG_DEBUG("playerbot",  "No trade items available for bot {} ({} level)", bot->GetName().c_str(), bot->GetLevel());
        return;
    }

    uint32 index = urand(0, ids.size() - 1);
    if (index >= ids.size())
        return;

    uint32 itemId = ids[index];
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return;

    uint32 count = 1, stacks = 1;
    switch (proto->GetQuality())
    {
    case ITEM_QUALITY_NORMAL:
        count = proto->GetMaxStackSize();
        // A tiny positive pricing multiplier must not turn this into billions
        // of StoreItem calls (or an out-of-range floating-to-integer conversion).
        stacks = uint32(std::min(7.0, urand(1, 7) / auctionbot.GetRarityPriceMultiplier(proto)));
        break;
    case ITEM_QUALITY_UNCOMMON:
        stacks = 1;
        count = urand(1, proto->GetMaxStackSize());
        break;
    case ITEM_QUALITY_RARE:
        stacks = 1;
        count = urand(1, min(uint32(3), proto->GetMaxStackSize()));
        break;
    }

    for (uint32 i = 0; i < stacks; i++)
        StoreItem(itemId, count);
}

void PlayerbotFactory::InitInventoryEquip()
{
    vector<uint32> ids;

    uint32 desiredQuality = itemQuality;
    if (urand(0, 100) < 100 * sPlayerbotAIConfig.randomGearLoweringChance && desiredQuality > ITEM_QUALITY_NORMAL) {
        desiredQuality--;
    }

    ItemTemplateContainer const& itemTemplateContainer = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator i = itemTemplateContainer.begin(); i != itemTemplateContainer.end(); ++i)
    {
        ItemTemplate const& itemTemplate = i->second;
        uint32 itemId = i->first;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        if (proto->GetClass() != ITEM_CLASS_ARMOR && proto->GetClass() != ITEM_CLASS_WEAPON || (proto->GetBonding() == BIND_ON_ACQUIRE ||
                proto->GetBonding() == BIND_ON_USE))
            continue;

        if (IsJunkTestItem(proto))
            continue;

        if (proto->GetClass() == ITEM_CLASS_ARMOR && !CanEquipArmor(proto))
            continue;

        // Spare equipment for the bags: anything usable in any slot.
        if (proto->GetClass() == ITEM_CLASS_WEAPON && !CanEquipWeapon(proto, EQUIPMENT_SLOT_END))
            continue;

        if (!CanEquipItem(proto, desiredQuality))
            continue;

        ids.push_back(itemId);
    }

    if (ids.empty())
    {
        TC_LOG_DEBUG("playerbot", "{}: no equipment items to add to inventory", bot->GetName());
        return;
    }

    int maxCount = urand(0, 3);
    int count = 0;
    for (int attempts = 0; attempts < 15; attempts++)
    {
        uint32 index = urand(0, ids.size() - 1);

        uint32 itemId = ids[index];
        if (StoreItem(itemId, 1) && count++ >= maxCount)
            break;
   }
}

void PlayerbotFactory::InitGlyphs()
{
    bot->InitGlyphsForLevel();

    for (uint32 slotIndex = 0; slotIndex < MAX_GLYPH_SLOT_INDEX; ++slotIndex)
    {
        bot->SetGlyph(slotIndex, 0);
    }

    uint32 level = bot->GetLevel();
    uint32 maxSlot = 0;
    if (level >= 15)
        maxSlot = 2;
    if (level >= 30)
        maxSlot = 3;
    if (level >= 50)
        maxSlot = 4;
    if (level >= 70)
        maxSlot = 5;
    if (level >= 80)
        maxSlot = 6;

    if (!maxSlot)
        return;

    list<uint32> glyphs;
    ItemTemplateContainer const& itemTemplates = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator i = itemTemplates.begin(); i != itemTemplates.end(); ++i)
    {
        uint32 itemId = i->first;
        ItemTemplate const* proto = &i->second;
        if (!proto)
            continue;

        if (proto->GetClass() != ITEM_CLASS_GLYPH)
            continue;

        if ((proto->GetAllowableClass() & bot->GetClassMask()) == 0 || !proto->GetAllowableRace().HasRace(bot->GetRace()))
            continue;

        for (uint32 spell = 0; spell < MAX_ITEM_PROTO_EFFECTS; spell++)
        {
            uint32 spellId = ItemSpellId(proto, spell);
            SpellInfo const *entry = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
            if (!entry)
                continue;

            for (uint32 effect = 0; effect <= EFFECT_2; ++effect)
            {
                if (entry->GetEffect(SpellEffIndex(effect)).Effect != SPELL_EFFECT_APPLY_GLYPH)
                    continue;

                uint32 glyph = entry->GetEffect(SpellEffIndex(effect)).MiscValue;
                glyphs.push_back(glyph);
            }
        }
    }

    if (glyphs.empty())
    {
        TC_LOG_DEBUG("playerbot",  "No glyphs found for bot {}", bot->GetName().c_str());
        return;
    }

    set<uint32> chosen;
    for (uint32 slotIndex = 0; slotIndex < maxSlot; ++slotIndex)
    {
        uint32 slot = bot->GetGlyphSlot(slotIndex);
        GlyphSlotEntry const *gs = sGlyphSlotStore.LookupEntry(slot);
        if (!gs)
            continue;

        vector<uint32> ids;
        for (list<uint32>::iterator i = glyphs.begin(); i != glyphs.end(); ++i)
        {
            uint32 id = *i;
            GlyphPropertiesEntry const *gp = sGlyphPropertiesStore.LookupEntry(id);
            if (!gp || gp->GlyphType != gs->Type)
                continue;

            ids.push_back(id);
        }

        // no glyphs for this slot's glyph type (fresh, per-type list):
        // indexing it would underflow urand()
        if (ids.empty())
        {
            TC_LOG_DEBUG("playerbot", "No glyphs found for bot {} index {} slot {}", bot->GetName().c_str(), slotIndex, slot);
            continue;
        }

        int maxCount = urand(0, 3);
        int count = 0;
        bool found = false;
        for (int attempts = 0; attempts < 15; ++attempts)
        {
            uint32 index = urand(0, ids.size() - 1);

            uint32 id = ids[index];
            if (chosen.find(id) != chosen.end())
                continue;

            chosen.insert(id);

            bot->SetGlyph(slotIndex, id);
            found = true;
            break;
        }
        if (!found)
            TC_LOG_DEBUG("playerbot",  "No glyphs found for bot {} index {} slot {}", bot->GetName().c_str(), slotIndex, slot);
    }
}

void PlayerbotFactory::InitGuild()
{
    if (bot->GetGuildId())
        return;

    if (sPlayerbotAIConfig.randomBotGuilds.empty())
        RandomPlayerbotFactory::CreateRandomGuilds();

    vector<uint32> guilds;
    for(list<uint32>::iterator i = sPlayerbotAIConfig.randomBotGuilds.begin(); i != sPlayerbotAIConfig.randomBotGuilds.end(); ++i)
        guilds.push_back(*i);

    if (guilds.empty())
    {
        TC_LOG_DEBUG("playerbot",  "No random guilds available");
        return;
    }

    int index = urand(0, guilds.size() - 1);
    uint32 guildId = guilds[index];
    Guild* guild = sGuildMgr->GetGuildById(guildId);
    if (!guild)
    {
        TC_LOG_ERROR("playerbot",  "Invalid guild {}", guildId);
        return;
    }

    if (guild->GetMembersCount() < 10)
    {
        // Guild::AddMember queues SQL (guild member insert/event log) on the
        // passed transaction - a null transaction dereferences inside the
        // core, so supply and commit a real one.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (guild->AddMember(trans, bot->GetGUID()))
            CharacterDatabase.CommitTransaction(trans);
    }
}
