#include "BotCombat.h"

#include "BotAI.h"
#include "BotConfig.h"
#include "BotMovement.h"
#include "BotSpells.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Creature.h"
#include "Unit.h"
#include "Map.h"
#include "Util.h"
#include "Log.h"

Unit* BotCombat::GetVictim() const
{
    if (_victimGuid.IsEmpty())
        return nullptr;
    return ObjectAccessor::GetUnit(*_bot, _victimGuid);
}

bool BotCombat::AttackStartedFor(Unit* victim) const
{
    return victim && victim->GetGUID() == _attackStartedGuid;
}

void BotCombat::SetVictim(Unit* victim, std::string reason)
{
    if (!victim || !victim->IsAlive())
        return;
    if (victim->GetGUID() == _victimGuid)
        return;

    _victimGuid = victim->GetGUID();
    _victimReason = std::move(reason);
    _attackStartedGuid.Clear();
    _retreatCooldown = 0;
}

void BotCombat::ClearVictim(std::string reason)
{
    if (_victimGuid.IsEmpty())
        return;

    // Stop the melee attack state and the wand/auto-shot loop; the movement
    // goal is dropped by the AI's next planning pass.
    _bot->AttackStop();
    _bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
    _victimGuid.Clear();
    _victimReason.clear();
    _attackStartedGuid.Clear();
    _retreatCooldown = 0;
}

bool BotCombat::IsValidVictim(Unit* victim) const
{
    if (!victim)
        return false;
    if (!victim->IsAlive() || !victim->IsInWorld())
        return false;
    if (victim->GetMapId() != _bot->GetMapId() || !_bot->InSamePhase(victim))
        return false;
    // GM-flagged players and critters are not fightable.
    if (Player* victimPlayer = victim->ToPlayer())
    {
        if (victimPlayer->IsGameMaster())
            return false;
        if (!_bot->IsValidAttackTarget(victim))
            return false;
    }
    else if (victim->ToCreature())
    {
        if (victim->ToCreature()->IsCritter() || victim->IsTotem())
            return false;
        if (!victim->IsValidAttackTarget(_bot))
            return false;
    }
    return true;
}

void BotCombat::Update(uint32 diff)
{
    if (_retreatCooldown > diff)
        _retreatCooldown -= diff;
    else
        _retreatCooldown = 0;
    if (_castPaceCooldown > diff)
        _castPaceCooldown -= diff;
    else
        _castPaceCooldown = 0;

    Unit* victim = GetVictim();
    if (!IsValidVictim(victim))
    {
        if (victim && !victim->IsAlive())
        {
            // Notify the AI while the guid still resolves: OnVictimDied
            // queues the corpse and drops the fight itself.
            _ai->OnVictimDied(victim);
            return;
        }
        ClearVictim("target gone");
        return;
    }

    // Start the melee attack state once per victim. Starting it early is what
    // a player client does: the server shows the combat stance while we run
    // in, and Player::Update's DoMeleeAttackIfReady simply waits for range.
    if (_attackStartedGuid != victim->GetGUID())
    {
        if (_bot->Attack(victim, true))
            _attackStartedGuid = victim->GetGUID();
    }

    if (_stance == BotCombatStance::Melee)
        UpdateMelee(victim, diff);
    else
        UpdateRanged(victim, diff);

    // Class script runs once per GCD-ish window whenever we are not standing
    // in a hard cast (the cast holds the floor by itself - the script's casts
    // set CURRENT_GENERIC_SPELL and the next window sees IsCasting()). The
    // window also keeps failed casts (out of range by a hair, NO_AMMO, ...)
    // from being re-attempted every tick.
    if (_castPaceCooldown == 0 && !_ai->GetSpells().IsHardCasting())
    {
        _castPaceCooldown = 600;
        _ai->GetClassAI().CombatTick(*_ai);
    }
}

void BotCombat::UpdateMelee(Unit* victim, uint32 /*diff*/)
{
    BotMovement& mover = _ai->GetMovement();

    // The core's own swing gate - the exact pair DoMeleeAttackIfReady checks.
    bool const inRange = _bot->IsWithinMeleeRange(victim);

    if (!inRange)
    {
        // Stop distance inside the swing envelope: GetMeleeRange is the live
        // centre-to-centre 3D envelope of this pair (combat reaches + 4/3,
        // min 5yd), so a stop at ~80% of it always leaves the swing gate
        // green - including the Z gap the old code kept forgetting about.
        float const stopRange = std::max(1.0f, _bot->GetMeleeRange(victim) * sBotConfig->MeleeStopFactor);
        mover.Chase(victim, stopRange);
        return;
    }

    // In swing range: plant the feet and face the victim. Both the arc fixup
    // and the plain facing are done by the movement's in-place turn, which
    // updates the orientation the arc check reads on the next swing tick.
    mover.Stop();
    mover.Face(victim);
}

void BotCombat::UpdateRanged(Unit* victim, uint32 /*diff*/)
{
    BotMovement& mover = _ai->GetMovement();
    float const dist = _bot->GetExactDist(victim);

    float const standRange = std::min(sBotConfig->CastStandDistance, sBotConfig->SpellDistance);
    float const minRange = std::max(_ai->GetClassAI().GetMinRange(), 0.5f);

    if (dist > standRange)
    {
        mover.Chase(victim, standRange * 0.9f);
        return;
    }

    if (dist < minRange)
    {
        // Dead zone: back straight away from the victim, occasionally
        // sidestepping so bots don't walk in perfect lockstep.
        if (mover.IsMoving() || !CanRetreat())
            return;

        float const away = _bot->GetAbsoluteAngle(victim) + float(M_PI) + frand(-0.5f, 0.5f);
        float const backTo = std::min(standRange, minRange + 4.0f);
        float const x = _bot->GetPositionX() + std::cos(away) * (backTo - dist);
        float const y = _bot->GetPositionY() + std::sin(away) * (backTo - dist);
        float z = victim->GetPositionZ();
        _bot->UpdateGroundPositionZ(x, y, z);
        mover.MoveTo(x, y, z, 0.5f, true);
        StartedRetreat();
        return;
    }

    // Inside the band: stand, face, shoot.
    mover.Stop();
    mover.Face(victim);
}
