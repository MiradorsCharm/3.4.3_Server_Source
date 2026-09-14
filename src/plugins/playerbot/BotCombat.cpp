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

    // A fight we cannot win is not a stall, it is a mistake. Retaliating
    // against something fifteen levels above us is what filled the log with
    // "Bot X stalled ... reason=retaliate": the bot was in range, facing,
    // attacking - and every swing missed. Run instead.
    if (sBotConfig->HopelessLevelGap > 0 && victim->ToCreature() && !victim->ToPlayer() && !victim->ToPet())
    {
        if (int32(victim->GetLevel()) - int32(_bot->GetLevel()) > sBotConfig->HopelessLevelGap)
        {
            _ai->FleeFrom(victim, "hopeless fight");
            return;
        }
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

    // Dungeon/raid mechanic: interrupt a dangerous boss/trash cast the instant
    // it starts, ahead of the normal rotation and off the rotation's pacing so
    // a short cast is not missed. The class supplies its own interrupt kit
    // (Counterspell/Kick/Pummel/Mind Freeze/Wind Shear/...); classes without
    // one no-op here.
    if (_interruptPaceCooldown > diff)
        _interruptPaceCooldown -= diff;
    else
        _interruptPaceCooldown = 0;
    if (sBotConfig->InterruptCasts && _interruptPaceCooldown == 0)
    {
        if (_ai->GetClassAI().TryInterruptVictim())
            _interruptPaceCooldown = 800;   // brief pace so we don't fan the whole kit at once
    }

    // Class script runs on a short retry window whenever we are not standing
    // in a hard cast (the cast holds the floor by itself - the script's casts
    // set CURRENT_GENERIC_SPELL and the next window sees IsCasting()). The
    // window used to be a fixed 600ms, which was long enough that an ability
    // coming off a 1.5s GCD sat idle for nearly half a second every time; the
    // core's own cooldown/GCD gate is what actually paces the casts, so the
    // retry can be much tighter.
    if (_castPaceCooldown == 0 && !_ai->GetSpells().IsHardCasting())
    {
        _castPaceCooldown = sBotConfig->CastRetryMs;

        // "save mana": hold the rotation back while resources run low and let
        // the ranged attack carry the fight. "max dps" never holds back.
        bool const conserving = _ai->IsConservingMana() && !_ai->IsMaxDps()
            && _bot->GetPowerType() == POWER_MANA && _bot->GetPowerPct(POWER_MANA) < 30.0f;
        if (!conserving)
        {
            if (_ai->IsTankMode())
                _ai->GetClassAI().TankTick(*_ai);   // taunts/presence before the rotation
            _ai->GetClassAI().CombatTick(*_ai);
        }
        else if (_stance == BotCombatStance::Ranged)
            _ai->GetSpells().MaintainAutoRepeat(victim);
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
    // The dead zone is the larger of what the class/weapon needs (a hunter
    // cannot shoot inside 5yd) and the configured stand-off floor. Reading only
    // the class value left AiPlayerbot.CastMinDistance dead: casters happily
    // stood inside a mob's swing range and got chewed while their rotation
    // fought the melee hits for the GCD.
    float const minRange = std::max(_ai->GetClassAI().GetMinRange(), std::max(0.5f, sBotConfig->CastMinDistance));

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

        // Pick a backpedal direction that does not put us into a ground effect:
        // try straight back first, then fan out to either side.
        float const backTo = std::min(standRange, minRange + 4.0f);
        float const baseAway = _bot->GetAbsoluteAngle(victim) + float(M_PI);
        BotHazards& hazards = _ai->GetHazards();

        static float const fan[] = { 0.0f, 0.6f, -0.6f, 1.2f, -1.2f, float(M_PI) };
        for (float off : fan)
        {
            float const away = baseAway + off;
            float const x = _bot->GetPositionX() + std::cos(away) * (backTo - dist);
            float const y = _bot->GetPositionY() + std::sin(away) * (backTo - dist);
            float z = victim->GetPositionZ();
            _bot->UpdateGroundPositionZ(x, y, z);

            if (hazards.IsSpotDangerous(x, y, z) ||
                hazards.IsPathDangerous(_bot->GetPositionX(), _bot->GetPositionY(), x, y))
                continue;

            mover.MoveTo(x, y, z, 0.5f, true);
            StartedRetreat();
            return;
        }
        // Every backpedal is into fire; hold and let the hazard pass handle it.
        return;
    }

    // Inside the band: stand, face, shoot.
    mover.Stop();
    mover.Face(victim);

    // Keep the wand/auto-shot loop alive. It has to be re-armed from here and
    // not only from the class script: Unit::_UpdateAutoRepeatSpell drops the
    // loop every time the bot moves or hard-casts, and a caster whose whole
    // rotation is on cooldown (or out of mana) would otherwise fall completely
    // silent instead of shooting.
    _ai->GetSpells().MaintainAutoRepeat(victim);
}
