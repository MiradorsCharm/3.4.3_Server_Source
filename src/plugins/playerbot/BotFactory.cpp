#include "BotFactory.h"

#include "BotConfig.h"
#include "BotState.h"
#include "BotTalents.h"
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
#include "Container/Bag.h"

#include <functional>
#include <unordered_map>
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

    // Best usable item of the given class/subclasses for this character.
    //
    // 3.4.3 has NO `world.item_template` table - item data lives in the DB2
    // client stores (Item.db2 / ItemSparse.db2), loaded into
    // ObjectMgr::_itemTemplateStore at startup. The previous port queried the
    // 3.3.5 SQL table, which does not exist here: every FindBestItem call
    // failed with "[1146] Table 'world.item_template' doesn't exist" and the
    // bot ended up naked. We instead scan the in-memory template store, which
    // is both correct for this core and far cheaper than a DB round trip.
    // The item template store is a few ten-thousand entries wide and this scan
    // runs once per equipment slot. Doing that for every bot on every login is
    // what made a large pool turn startup into a freeze (14 scans x 500 bots x
    // ~40k templates). The answer only depends on class/race/level and on what
    // we are looking for, so it is memoised.
    std::unordered_map<uint64, uint32>& ItemLookupCache()
    {
        static std::unordered_map<uint64, uint32> cache;
        return cache;
    }

    uint64 MakeItemCacheKey(uint8 cls, uint8 race, uint32 level, uint32 itemClass,
        uint32 inventoryType, std::vector<uint32> const& subclasses)
    {
        uint64 key = 14695981039346656037ull;                 // FNV-1a offset basis
        auto mix = [&key](uint64 value)
        {
            for (int i = 0; i < 8; ++i)
            {
                key ^= (value >> (i * 8)) & 0xFFull;
                key *= 1099511628211ull;
            }
        };
        mix(cls);
        mix(race);
        mix(level);
        mix(itemClass);
        mix(inventoryType);
        for (uint32 subclass : subclasses)
            mix(subclass);
        return key;
    }

    uint32 FindBestItem(Player* bot, uint32 itemClass, std::vector<uint32> const& subclasses, uint32 inventoryType)
    {
        uint8 const cls = bot->GetClass();
        uint8 const race = bot->GetRace();
        uint32 const classMask = (cls >= 1 && cls <= MAX_CLASSES) ? (1u << (cls - 1)) : 0u;
        uint32 const level = bot->GetLevel();

        auto& cache = ItemLookupCache();
        uint64 const cacheKey = MakeItemCacheKey(cls, race, level, itemClass, inventoryType, subclasses);
        if (auto cached = cache.find(cacheKey); cached != cache.end())
            return cached->second;

        auto wantsSubclass = [&subclasses](uint32 subclass)
        {
            for (uint32 s : subclasses)
                if (s == subclass)
                    return true;
            return false;
        };

        uint32 bestEntry = 0;
        int32 bestReqLevel = -1;
        uint32 bestItemLevel = 0;

        for (auto const& [entry, proto] : sObjectMgr->GetItemTemplateStore())
        {
            if (proto.GetClass() != itemClass)
                continue;
            if (!wantsSubclass(proto.GetSubClass()))
                continue;
            if (uint32(proto.GetInventoryType()) != inventoryType)
                continue;
            if (proto.GetQuality() < ITEM_QUALITY_UNCOMMON)
                continue;
            if (proto.GetBaseRequiredLevel() > int32(level))
                continue;

            int32 const allowableClass = proto.GetAllowableClass();
            if (allowableClass != -1 && classMask && (uint32(allowableClass) & classMask) == 0)
                continue;

            Trinity::RaceMask<int64> const allowableRace = proto.GetAllowableRace();
            if (!allowableRace.IsEmpty() && allowableRace.RawValue != -1 && !allowableRace.HasRace(race))
                continue;

            // Prefer the highest required level, then the highest item level -
            // the same ordering the old SQL query used.
            int32 const reqLevel = proto.GetBaseRequiredLevel();
            uint32 const itemLevel = proto.GetItemLevel();
            if (reqLevel < bestReqLevel)
                continue;
            if (reqLevel == bestReqLevel && itemLevel <= bestItemLevel)
                continue;

            if (bot->CanUseItem(&proto, false) != EQUIP_ERR_OK)
                continue;

            bestEntry = entry;
            bestReqLevel = reqLevel;
            bestItemLevel = itemLevel;
        }

        cache[cacheKey] = bestEntry;
        return bestEntry;
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
        uint32 skipped = 0;
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

            // A spell family is not a class: item, quest and NPC spells share
            // the same families. A fair number of them are pure client-side
            // triggers with no server-side effect, or craft/learn-spell rows
            // that point at data this build does not have. Handing those to
            // Player::LearnSpell made the core reject them one by one and log
            //   Player::AddSpell: Spell (ID: 51266) is invalid
            // for every bot that logged in. Ask the core's own validator first
            // - it is the very predicate Player::AddSpell applies.
            if (!SpellMgr::IsSpellValid(info, bot, false))
            {
                ++skipped;
                return;
            }

            // Nothing the server would ever do with it: no effect at all.
            bool hasRealEffect = false;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect != SPELL_EFFECT_NONE)
                    hasRealEffect = true;
            if (!hasRealEffect)
            {
                ++skipped;
                return;
            }

            bot->LearnSpell(info->Id, false, 0, true);
            ++learned;
        });

        if (learned || skipped)
            TC_LOG_DEBUG("playerbot", "{} learned {} class spell(s) for level {} ({} unusable spell(s) skipped)",
                bot->GetName(), learned, level, skipped);
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

namespace
{
    uint8 EquipSlotForInventoryType(uint32 inventoryType)
    {
        switch (inventoryType)
        {
            case INVTYPE_HEAD: return EQUIPMENT_SLOT_HEAD;
            case INVTYPE_NECK: return EQUIPMENT_SLOT_NECK;
            case INVTYPE_SHOULDERS: return EQUIPMENT_SLOT_SHOULDERS;
            case INVTYPE_CLOAK: return EQUIPMENT_SLOT_BACK;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE: return EQUIPMENT_SLOT_CHEST;
            case INVTYPE_WRISTS: return EQUIPMENT_SLOT_WRISTS;
            case INVTYPE_HANDS: return EQUIPMENT_SLOT_HANDS;
            case INVTYPE_WAIST: return EQUIPMENT_SLOT_WAIST;
            case INVTYPE_LEGS: return EQUIPMENT_SLOT_LEGS;
            case INVTYPE_FEET: return EQUIPMENT_SLOT_FEET;
            case INVTYPE_FINGER: return EQUIPMENT_SLOT_FINGER1;
            case INVTYPE_TRINKET: return EQUIPMENT_SLOT_TRINKET1;
            case INVTYPE_SHIELD:
            case INVTYPE_HOLDABLE:
            case INVTYPE_WEAPONOFFHAND: return EQUIPMENT_SLOT_OFFHAND;
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND:
            case INVTYPE_2HWEAPON:
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_THROWN: return EQUIPMENT_SLOT_MAINHAND;
            default: return uint8(-1);
        }
    }

    void ForEachBagItem(Player* bot, std::function<bool(Item*)> const& visit)
    {
        for (uint8 i = 0; i < 4; ++i)                       // equipped bags
            if (Bag* bag = bot->GetBagByPos(i))
                for (uint8 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(slot))
                        if (!visit(item))
                            return;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (!visit(item))
                    return;
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
        BotSavedState const state = BotState::Load(bot->GetGUID());

        // --- level -----------------------------------------------------------
        // The old rule ("raise a bot only while it sits below the minimum")
        // never fired with the shipped defaults, so the whole pool stayed
        // level 1: no class abilities worth the name, and every human bot
        // still standing in Northshire. The band in the config is what the
        // operator asked for, so a random bot that has never been prepared -
        // or that ended up outside the band - is drawn from it. A bot that was
        // already prepared keeps the level it ground its way to.
        bool leveled = false;
        if (isRandomBot)
        {
            uint32 const minLevel = sBotConfig->RandomBotMinLevel;
            uint32 const maxLevel = sBotConfig->RandomBotMaxLevel;
            uint32 const current = bot->GetLevel();
            bool const outOfBand = current < minLevel || current > maxLevel;

            if (state.preparedLevel == 0 || outOfBand)
            {
                uint32 const target = minLevel >= maxLevel ? minLevel : urand(minLevel, maxLevel);
                if (current != target)
                {
                    bot->GiveLevel(uint8(target));
                    leveled = true;
                }
                bot->UpdateSkillsForLevel();
                TC_LOG_INFO("playerbot", "{} prepared at level {} for bot duty (band {}-{})",
                    bot->GetName(), uint32(bot->GetLevel()), minLevel, maxLevel);
            }
        }

        // --- spells / gear ----------------------------------------------------
        // Re-granting the class kit and re-dressing the bot means scanning the
        // whole item template store again and writing a row per spell. Doing
        // that for every bot on every login is the other half of the startup
        // freeze, and it also threw away what the character had earned. Only
        // prepare when something actually changed.
        bool const needsPrep = state.preparedLevel == 0
            || leveled
            || state.preparedLevel != bot->GetLevel()
            || !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);

        if (!needsPrep)
            return;

        // class spells (trainable ranks up to the current level)
        LearnClassSpells(bot);

        // spend every earned talent point legally
        BotTalents::SpendPoints(bot);

        // gear
        RepairGear(bot);

        // a little money so vendors do not refuse us
        if (bot->GetMoney() < 1000)
            bot->ModifyMoney(1000 - bot->GetMoney());

        // full resources before the first fight
        bot->SetFullHealth();
        bot->SetPower(bot->GetPowerType(), bot->GetMaxPower(bot->GetPowerType()));

        BotState::SetPreparedLevel(bot->GetGUID(), bot->GetLevel());
        bot->SaveToDB();
    }

    bool UpgradeGear(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld())
            return false;

        uint32 upgraded = 0;
        for (;;)
        {
            // one equip per pass: equipping can invalidate bag positions,
            // so re-scan from scratch until nothing improves anymore
            Item* bestItem = nullptr;
            uint8 bestSlot = 255;
            int32 bestDelta = 0;

            ForEachBagItem(bot, [&](Item* item) -> bool
            {
                ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                if (!proto || proto->GetBonding() == BIND_QUEST)
                    return true;
                if (bot->CanUseItem(proto, false) != EQUIP_ERR_OK)
                    return true;

                uint8 const slot = EquipSlotForInventoryType(proto->GetInventoryType());
                if (slot == uint8(-1))
                    return true;

                int32 delta = int32(proto->GetItemLevel());
                if (Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    delta -= int32(current->GetTemplate()->GetItemLevel());
                if (delta > bestDelta)
                {
                    bestDelta = delta;
                    bestItem = item;
                    bestSlot = slot;
                }
                return true;
            });

            if (!bestItem)
                break;

            uint32 const entry = bestItem->GetEntry();
            uint8 const srcBag = bestItem->GetBagSlot();
            uint8 const srcSlot = bestItem->GetSlot();
            uint16 dest = 0;
            if (bot->CanEquipNewItem(bestSlot, dest, entry, false) != EQUIP_ERR_OK)
            {
                // cannot wear it in that slot after all: skip without looping
                TC_LOG_DEBUG("playerbot", "UpgradeGear: {} cannot equip {}", bot->GetName(), entry);
                // blacklist this attempt by removing the item from the scan is
                // not possible without state; break to stay safe
                break;
            }
            bot->DestroyItem(srcBag, srcSlot, true);
            if (bot->EquipNewItem(dest, entry, ItemContext::NONE, true))
                ++upgraded;
            LearnWeaponSkill(bot, sObjectMgr->GetItemTemplate(entry));
        }

        if (upgraded)
            bot->SaveToDB();
        TC_LOG_INFO("playerbot", "UpgradeGear: {} equipped {} upgrade(s)", bot->GetName(), upgraded);
        return upgraded > 0;
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
