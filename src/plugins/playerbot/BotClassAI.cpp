#include "BotClassAI.h"

#include "BotAI.h"
#include "BotCombat.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"
#include "Group.h"
#include "GroupReference.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "SpellMgr.h"

uint32 BotClassAI::RankFrom(uint32 const* ids, std::size_t count) const
{
    Player* bot = _ai->GetBot();
    for (std::size_t i = count; i > 0; --i)
        if (bot->HasSpell(ids[i - 1]))
            return ids[i - 1];
    return 0;
}

bool BotClassAI::CastOnVictim(uint32 spellId)
{
    if (!spellId)
        return false;
    return _ai->GetSpells().Cast(spellId, _ai->GetCombat().GetVictim());
}

bool BotClassAI::CastOnSelf(uint32 spellId)
{
    if (!spellId)
        return false;
    return _ai->GetSpells().CastSelf(spellId);
}

bool BotClassAI::CastOnUnit(Unit* target, uint32 spellId)
{
    if (!spellId || !target)
        return false;
    return _ai->GetSpells().Cast(spellId, target);
}

bool BotClassAI::VictimHasAura(uint32 spellId) const
{
    Unit* victim = _ai->GetCombat().GetVictim();
    return spellId && victim && victim->HasAura(spellId, _ai->GetBot()->GetGUID());
}

bool BotClassAI::SelfHasAura(uint32 spellId) const
{
    return spellId && _ai->GetBot()->HasAura(spellId);
}

Unit* BotClassAI::FindHealTarget(float healthPct, float range) const
{
    Player* bot = _ai->GetBot();

    Unit* best = nullptr;
    float bestHealth = 100.0f;

    auto consider = [&](Unit* who)
    {
        if (!who || !who->IsAlive() || !who->IsInWorld())
            return;
        if (!bot->IsInMap(who))
            return;
        if (bot->GetExactDist(who) > range)
            return;
        float pct = who->GetHealthPct();
        if (pct < healthPct && pct < bestHealth)
        {
            bestHealth = pct;
            best = who;
        }
    };

    consider(bot);
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference const* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && member != bot)
            {
                consider(member);
                if (Pet* pet = member->GetPet())
                    consider(pet->ToUnit());
            }
        }
    }
    else if (Player* master = _ai->GetMaster())
    {
        consider(master);
        if (Pet* pet = master->GetPet())
            consider(pet->ToUnit());
    }

    return best;
}

BotClassAI* CreateBotClassAI(uint8 playerClass, BotAI* ai)
{
    switch (playerClass)
    {
        case CLASS_WARRIOR: return CreateWarriorAI(ai);
        case CLASS_PALADIN: return CreatePaladinAI(ai);
        case CLASS_HUNTER: return CreateHunterAI(ai);
        case CLASS_ROGUE: return CreateRogueAI(ai);
        case CLASS_PRIEST: return CreatePriestAI(ai);
        case CLASS_DEATH_KNIGHT: return CreateDeathKnightAI(ai);
        case CLASS_SHAMAN: return CreateShamanAI(ai);
        case CLASS_MAGE: return CreateMageAI(ai);
        case CLASS_WARLOCK: return CreateWarlockAI(ai);
        case CLASS_DRUID: return CreateDruidAI(ai);
        default: return CreateWarriorAI(ai); // unreachable for valid characters
    }
}
