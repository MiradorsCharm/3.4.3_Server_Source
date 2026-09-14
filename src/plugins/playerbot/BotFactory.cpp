#include "BotFactory.h"

#include "BotConfig.h"
#include "Player.h"
#include "MotionMaster.h"
#include "WorldSession.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "DB2Stores.h"
#include "CharacterCache.h"
#include "AccountMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Packet.h"
#include "Server/Packets/CharacterPackets.h"

#include <vector>

namespace
{
    struct WeaponSpec
    {
        std::vector<uint32> mainHandSubclasses;
        std::vector<uint32> rangedSubclasses;
        bool wantsShield = false;
    };

    // Item subclass tables (ITEM_SUBCLASS_WEAPON_*).
    WeaponSpec WeaponSpecForClass(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:
                return { { 0, 4, 7 }, {}, true };              // axe/mace/sword 1h + shield
            case CLASS_PALADIN:
                return { { 0, 4, 7 }, {}, true };
            case CLASS_DEATH_KNIGHT:
                return { { 0, 4, 7, 6 }, {}, true };           // + polearm
            case CLASS_ROGUE:
                return { { 15, 7, 0, 4 }, { 16 }, false };     // dagger/sword/axe/mace + thrown
            case CLASS_HUNTER:
                return { { 7, 0, 4 }, { 2, 3, 18 }, false };   // 1h + bow/gun/crossbow
            case CLASS_SHAMAN:
                return { { 4, 0 }, {}, true };
            case CLASS_DRUID:
                return { { 10, 6, 4 }, {}, false };            // staff/polearm/mace
            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
                return { { 10 }, { 19 }, false };              // staff + wand
            default:
                return { { 7 }, {}, false };
        }
    }

    // armor subclasses per class, best first (ITEM_SUBCLASS_ARMOR_*)
    std::vector<uint32> ArmorSubclassesForClass(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT:
                return { 4, 3, 2, 1 };                         // plate -> ... -> cloth
            case CLASS_HUNTER:
            case CLASS_SHAMAN:
                return { 3, 2, 1 };
            case CLASS_ROGUE:
            case CLASS_DRUID:
                return { 2, 1 };
            default:
                return { 1 };                                  // cloth users
        }
    }

    std::string InList(std::vector<uint32> const& ids)
    {
        std::string out;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i)
                out += ",";
            out += std::to_string(ids[i]);
        }
        return out;
    }

    // Best usable item of the given class/subclasses for this character.
    uint32 FindBestItem(Player* bot, uint32 itemClass, std::vector<uint32> const& subclasses, uint32 inventoryType)
    {
        uint32 const classMask = (bot->GetClass() - 1) < 11 ? (1u << (bot->GetClass() - 1)) : 0u;
        uint32 const raceMask = (bot->GetRace() - 1) < 32 ? (1u << (bot->GetRace() - 1)) : 0u;
        uint32 const level = bot->GetLevel();

        std::string query = Trinity::StringFormat(
            "SELECT entry FROM item_template WHERE class = {} AND subclass IN ({}) AND InventoryType = {} "
            "AND RequiredLevel <= {} AND (AllowableClass = -1 OR (AllowableClass & {}) <> 0) "
            "AND (AllowableRace = -1 OR (AllowableRace & {}) <> 0) "
            "AND Quality >= 2 ORDER BY RequiredLevel DESC, ItemLevel DESC LIMIT 4",
            itemClass, InList(subclasses), inventoryType, level, classMask, raceMask);

        QueryResult result = WorldDatabase.Query(query.c_str());
        if (!result)
            return 0;

        do
        {
            Field* fields = result->Fetch();
            uint32 entry = fields[0].GetUInt32();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto)
                continue;
            if (bot->CanUseItem(proto, false) == EQUIP_ERR_OK)
                return entry;
        } while (result->NextRow());

        return 0;
    }

    // Weapon skill spells (Swords, Bows, ...) carry SPELL_EFFECT_SKILL and
    // Player::LearnSpell turns them into the actual skill on learn. Grading
    // the skill by the item's own skill line keeps this correct without
    // hardcoding spell ids.
    void LearnWeaponSkill(Player* bot, ItemTemplate const* proto)
    {
        if (!proto || !proto->IsWeapon())
            return;

        uint32 const skill = proto->GetSkill();
        if (!skill || bot->HasSkill(skill))
            return;

        uint32 skillSpell = 0;
        sSpellMgr->ForEachSpellInfo([&](SpellInfo const* info)
        {
            if (skillSpell || info->Difficulty != DIFFICULTY_NONE)
                return;
            if (!info->HasEffect(SPELL_EFFECT_SKILL))
                return;
            SpellLearnSkillNode const* node = sSpellMgr->GetSpellLearnSkill(info->Id);
            if (node && node->skill == skill)
                skillSpell = info->Id;
        });

        if (skillSpell)
            bot->LearnSpell(skillSpell, false, 0, true);
    }

    bool EquipInSlot(Player* bot, uint8 slot, uint32 entry)
    {
        if (!entry)
            return false;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            return false;

        // CanEquipNewItem validates by entry and reports the destination
        // position (bag/slot packed); EquipNewItem equips right there.
        uint16 dest = 0;
        if (bot->CanEquipNewItem(slot, dest, entry, false) != EQUIP_ERR_OK)
            return false;

        Item* item = bot->EquipNewItem(dest, entry, ItemContext::NONE, true);
        if (item)
            LearnWeaponSkill(bot, proto);
        return item != nullptr;
    }

    uint32 ClassSpellFamily(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR: return SPELLFAMILY_WARRIOR;
            case CLASS_PALADIN: return SPELLFAMILY_PALADIN;
            case CLASS_HUNTER: return SPELLFAMILY_HUNTER;
            case CLASS_ROGUE: return SPELLFAMILY_ROGUE;
            case CLASS_PRIEST: return SPELLFAMILY_PRIEST;
            case CLASS_DEATH_KNIGHT: return SPELLFAMILY_DEATHKNIGHT;
            case CLASS_SHAMAN: return SPELLFAMILY_SHAMAN;
            case CLASS_MAGE: return SPELLFAMILY_MAGE;
            case CLASS_WARLOCK: return SPELLFAMILY_WARLOCK;
            case CLASS_DRUID: return SPELLFAMILY_DRUID;
            default: return 0;
        }
    }

    // Random but valid appearance: mirrors what the core's own character
    // creation path validates (both the option and the choice must pass
    // MeetsChrCustomizationReq).
    void FillBotCustomizations(WorldSession* session, uint8 race, uint8 cls, uint8 gender,
        WorldPackets::Array<UF::ChrCustomizationChoice, 250>& customizations)
    {
        std::vector<ChrCustomizationOptionEntry const*> const* options = sDB2Manager.GetCustomiztionOptions(race, gender);
        if (!options)
            return;

        Races const raceId = Races(race);
        Classes const classId = Classes(cls);
        std::vector<UF::ChrCustomizationChoice> selected;

        for (ChrCustomizationOptionEntry const* option : *options)
        {
            ChrCustomizationReqEntry const* optionReq = sChrCustomizationReqStore.LookupEntry(option->ChrCustomizationReqID);
            if (optionReq && !session->MeetsChrCustomizationReq(optionReq, raceId, classId, false, MakeChrCustomizationChoiceRange(selected)))
                continue;

            std::vector<ChrCustomizationChoiceEntry const*> const* choices = sDB2Manager.GetCustomiztionChoices(option->ID);
            if (!choices || choices->empty())
                continue;

            std::vector<ChrCustomizationChoiceEntry const*> usable;
            for (ChrCustomizationChoiceEntry const* choice : *choices)
            {
                ChrCustomizationReqEntry const* choiceReq = sChrCustomizationReqStore.LookupEntry(choice->ChrCustomizationReqID);
                if (choiceReq && !session->MeetsChrCustomizationReq(choiceReq, raceId, classId, true, MakeChrCustomizationChoiceRange(selected)))
                    continue;
                usable.push_back(choice);
            }

            if (usable.empty())
                continue;

            ChrCustomizationChoiceEntry const* picked = usable[urand(0, usable.size() - 1)];
            UF::ChrCustomizationChoice choice;
            choice.ChrCustomizationOptionID = option->ID;
            choice.ChrCustomizationChoiceID = picked->ID;
            selected.push_back(choice);
        }

        for (UF::ChrCustomizationChoice const& choice : selected)
            customizations.push_back(choice);
    }

    void LearnClassSpells(Player* bot)
    {
        uint32 const family = ClassSpellFamily(bot->GetClass());
        if (!family)
            return;

        uint32 const level = bot->GetLevel();
        uint32 learned = 0;
        sSpellMgr->ForEachSpellInfo([&](SpellInfo const* info)
        {
            if (info->SpellFamilyName != family)
                return;
            // real, trainable class spells: rank spells with a level
            // requirement we have met. Hidden/proc/trait spells carry
            // SpellLevel 0 and are picked up automatically as passives of
            // the trainable ones.
            if (info->SpellLevel < 1 || info->SpellLevel > level)
                return;
            if (info->HasAttribute(SPELL_ATTR0_PASSIVE))
                return;
            if (bot->HasSpell(info->Id))
                return;
            bot->LearnSpell(info->Id, false, 0, true);
            ++learned;
        });

        if (learned)
            TC_LOG_DEBUG("playerbot", "{} learned {} class spell(s) for level {}", bot->GetName(), learned, level);
    }

    void RepairGear(Player* bot)
    {
        bool const isRandomBot = BotFactory::IsBotAccount(bot->GetSession()->GetAccountId());
        if (!isRandomBot)
        {
            // Hand-made characters get their gear untouched unless they are
            // literally unable to fight (no weapon).
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
                return;
        }

        // whatever is already equipped: make sure the skill line is learned
    for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED })
        if (Item* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            LearnWeaponSkill(bot, equipped->GetTemplate());

    WeaponSpec spec = WeaponSpecForClass(bot->GetClass());

        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
        {
            if (uint32 weapon = FindBestItem(bot, ITEM_CLASS_WEAPON, spec.mainHandSubclasses, INVTYPE_WEAPON))
                EquipInSlot(bot, EQUIPMENT_SLOT_MAINHAND, weapon);
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
                if (uint32 weapon2 = FindBestItem(bot, ITEM_CLASS_WEAPON, spec.mainHandSubclasses, INVTYPE_WEAPONMAINHAND))
                    EquipInSlot(bot, EQUIPMENT_SLOT_MAINHAND, weapon2);
        }

        if (spec.wantsShield && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            if (uint32 shield = FindBestItem(bot, ITEM_CLASS_ARMOR, { 6 }, INVTYPE_SHIELD))
                EquipInSlot(bot, EQUIPMENT_SLOT_OFFHAND, shield);

        if (!spec.rangedSubclasses.empty() && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
            for (uint32 invType : { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT, INVTYPE_THROWN })
                if (uint32 ranged = FindBestItem(bot, ITEM_CLASS_WEAPON, spec.rangedSubclasses, invType))
                {
                    EquipInSlot(bot, EQUIPMENT_SLOT_RANGED, ranged);
                    break;
                }

        // hunters: ammo for the equipped ranged weapon
        if (bot->GetClass() == CLASS_HUNTER)
        {
            if (Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
            {
                uint32 const sub = ranged->GetTemplate()->GetSubClass();
                uint32 ammoSub = sub == ITEM_SUBCLASS_WEAPON_GUN ? ITEM_SUBCLASS_BULLET : ITEM_SUBCLASS_ARROW;
                if (!bot->m_activePlayerData->AmmoID)
                {
                    if (uint32 ammo = FindBestItem(bot, ITEM_CLASS_PROJECTILE, { ammoSub }, INVTYPE_AMMO))
                    {
                        bot->SetAmmo(ammo);
                        for (int i = 0; i < 4; ++i)
                        {
                            ItemPosCountVec dest;
                            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ammo, 200) == EQUIP_ERR_OK)
                                if (Item* stack = bot->StoreNewItem(dest, ammo, true))
                                    bot->SendNewItem(stack, 200, true, false, true);
                        }
                    }
                }
            }
        }

        // random bots get a full outfit (armor slots) on top of the weapon
        if (isRandomBot)
        {
            static uint32 const slotTypes[][2] =
            {
                { EQUIPMENT_SLOT_HEAD, INVTYPE_HEAD },
                { EQUIPMENT_SLOT_SHOULDERS, INVTYPE_SHOULDERS },
                { EQUIPMENT_SLOT_CHEST, INVTYPE_CHEST },
                { EQUIPMENT_SLOT_WAIST, INVTYPE_WAIST },
                { EQUIPMENT_SLOT_LEGS, INVTYPE_LEGS },
                { EQUIPMENT_SLOT_FEET, INVTYPE_FEET },
                { EQUIPMENT_SLOT_WRISTS, INVTYPE_WRISTS },
                { EQUIPMENT_SLOT_HANDS, INVTYPE_HANDS },
                { EQUIPMENT_SLOT_BACK, INVTYPE_CLOAK },
            };
            for (auto const& [slot, invType] : slotTypes)
            {
                if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    continue;
                bool equipped = false;
                for (uint32 sub : ArmorSubclassesForClass(bot->GetClass()))
                {
                    if (uint32 armor = FindBestItem(bot, ITEM_CLASS_ARMOR, { sub }, invType))
                    {
                        equipped = EquipInSlot(bot, slot, armor);
                        if (equipped)
                            break;
                    }
                }
                (void)equipped;
            }
        }
    }
}

namespace BotFactory
{
    bool IsBotAccount(uint32 accountId)
    {
        std::string name;
        if (!AccountMgr::GetName(accountId, name))
            return false;
        return name.rfind(sBotConfig->BotAccountPrefix, 0) == 0;
    }

    void PrepareBot(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return;

        bool const isRandomBot = IsBotAccount(bot->GetSession()->GetAccountId());

        // Random bots are leveled up to the configured band on first login.
        if (isRandomBot && bot->GetLevel() < sBotConfig->RandomBotMinLevel)
        {
            uint32 target = urand(sBotConfig->RandomBotMinLevel, sBotConfig->RandomBotMaxLevel);
            bot->GiveLevel(uint8(target));
            bot->UpdateSkillsForLevel();
            TC_LOG_INFO("playerbot", "{} leveled to {} for bot duty", bot->GetName(), target);
        }

        // class spells (trainable ranks up to the current level)
        LearnClassSpells(bot);

        // gear
        RepairGear(bot);

        // a little money so vendors do not refuse us
        if (bot->GetMoney() < 1000)
            bot->ModifyMoney(1000 - bot->GetMoney());

        // full resources before the first fight
        bot->SetFullHealth();
        bot->SetPower(bot->GetPowerType(), bot->GetMaxPower(bot->GetPowerType()));

        bot->SaveToDB();
    }

    bool CreateBotCharacter(uint32 accountId, std::string const& name, uint8 playerClass, uint8 race, uint8 gender)
    {
        std::string accountName;
        if (!AccountMgr::GetName(accountId, accountName))
            accountName = "playerbot";

        WorldSession* session = new WorldSession(accountId, std::string(accountName), 0, nullptr, SEC_PLAYER,
            uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, "", Minutes(0), LOCALE_enUS, 0, false);
        session->SetBotSession(true);

        Player* player = new Player(session);
        player->GetMotionMaster()->Initialize();

        WorldPackets::Character::CharacterCreateInfo cci;
        cci.Name = name;
        cci.Race = race;
        cci.Class = playerClass;
        cci.Sex = gender;

        FillBotCustomizations(session, race, playerClass, gender, cci.Customizations);

        bool ok = false;
        if (player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &cci))
        {
            player->setCinematic(2);
            player->SetAtLoginFlag(AT_LOGIN_NONE);
            player->SaveToDB(true);
            sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), accountId, player->GetName(),
                player->GetNativeGender(), player->GetRace(), player->GetClass(), player->GetLevel(), false);
            TC_LOG_INFO("playerbot", "Created bot character {} (class {} race {})", name, playerClass, race);
            ok = true;
        }
        else
            TC_LOG_ERROR("playerbot", "Failed to create bot character {} (class {} race {})", name, playerClass, race);

        player->CleanupsBeforeDelete();
        delete player;
        delete session;
        return ok;
    }
}
