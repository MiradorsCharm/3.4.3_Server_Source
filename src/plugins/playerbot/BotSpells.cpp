#include "BotSpells.h"

#include "Player.h"
#include "Unit.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "Spell.h"
#include "Map.h"
#include "Log.h"

bool BotSpells::Known(uint32 spellId) const
{
    return spellId && _bot->HasSpell(spellId);
}

bool BotSpells::Ready(uint32 spellId) const
{
    return Known(spellId) && !_bot->GetSpellHistory()->HasCooldown(spellId);
}

bool BotSpells::Affordable(uint32 spellId) const
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
    if (!info)
        return false;

    auto costs = info->CalcPowerCost(_bot, info->GetSchoolMask());
    for (SpellPowerCost const& cost : costs)
    {
        if (cost.Amount <= 0)
            continue;
        if (_bot->GetPower(cost.Power) < cost.Amount)
            return false;
    }
    return true;
}

bool BotSpells::BasicTargetCheck(Unit* target) const
{
    if (!target || !_bot->IsInMap(target) || !_bot->InSamePhase(target))
        return false;
    if (!target->IsInWorld())
        return false;
    return true;
}

bool BotSpells::InRange(uint32 spellId, Unit* target) const
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
    if (!info || !BasicTargetCheck(target))
        return false;

    // Positive (buff/heal) spells measure with the positive flag, exactly
    // like Spell::CheckRange will a moment later.
    bool const positive = info->IsPositive();
    float const maxRange = info->GetMaxRange(positive);
    float const minRange = info->GetMinRange(positive);
    float const dist = _bot->GetExactDist(target);
    if (maxRange > 0.0f && dist > maxRange)
        return false;
    if (minRange > 0.0f && dist < minRange)
        return false;
    return true;
}

bool BotSpells::Castable(uint32 spellId, Unit* target) const
{
    if (!Ready(spellId) || !Affordable(spellId))
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
    if (!info)
        return false;

    // A spell with a cast time cannot be prepared while the player is still
    // flagged as moving (Spell::CheckMovement); firing it now would only
    // produce a SPELL_FAILED_MOVING. Instants are fine.
    if (info->CalcCastTime(nullptr) > 0 && _bot->isMoving())
        return false;

    if (!target)
    {
        // self cast: no range, just castability
        return true;
    }

    if (!BasicTargetCheck(target))
        return false;

    if (!info->IsPositive() && !target->IsValidAttackTarget(_bot))
        return false;
    if (info->IsPositive() && !target->IsFriendlyTo(_bot) && target != _bot)
        return false;

    if (!InRange(spellId, target))
        return false;

    if (!info->IsPositive() && !_bot->IsWithinLOSInMap(target))
        return false;

    return true;
}

bool BotSpells::Cast(uint32 spellId, Unit* target)
{
    if (!Castable(spellId, target))
        return false;

    if (target && target != _bot)
        _bot->SetSelection(target->GetGUID());
    _bot->CastSpell(target, spellId, false);
    return true;
}

bool BotSpells::CastSelf(uint32 spellId)
{
    return Cast(spellId, _bot);
}

uint32 BotSpells::HighestKnown(std::vector<uint32> const& rankIds) const
{
    for (auto it = rankIds.rbegin(); it != rankIds.rend(); ++it)
        if (Known(*it))
            return *it;
    return 0;
}

bool BotSpells::IsAutoRepeating() const
{
    return _bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr;
}

void BotSpells::StartAutoRepeat(uint32 shootSpellId, Unit* target)
{
    if (!shootSpellId || IsAutoRepeating())
        return;
    if (!Known(shootSpellId))
        return;
    if (!Castable(shootSpellId, target))
        return;
    _bot->SetSelection(target->GetGUID());
    _bot->CastSpell(target, shootSpellId, false);
}

void BotSpells::StopAutoRepeat()
{
    if (IsAutoRepeating())
        _bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
}

bool BotSpells::IsCasting() const
{
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
        if (Spell* spell = _bot->GetCurrentSpell(CurrentSpellTypes(i)))
            if (spell->getState() != SPELL_STATE_FINISHED)
                return true;
    return false;
}

bool BotSpells::IsHardCasting() const
{
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
    {
        Spell* spell = _bot->GetCurrentSpell(CurrentSpellTypes(i));
        if (!spell || spell->getState() == SPELL_STATE_FINISHED)
            continue;
        if (i == CURRENT_AUTOREPEAT_SPELL)
            continue;
        if (spell->GetSpellInfo()->CalcCastTime(spell) > 0)
            return true;
    }
    return false;
}

bool BotSpells::IsChanneling() const
{
    return _bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr;
}

void BotSpells::Interrupt()
{
    _bot->CastStop();
    _bot->InterruptSpell(CURRENT_GENERIC_SPELL, false);
    _bot->InterruptSpell(CURRENT_CHANNELED_SPELL, true);
}

bool BotSpells::IsEnemySpell(uint32 spellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
    return info && !info->IsPositive();
}
