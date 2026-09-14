#include "../../../pchdef.h"
#include "../../playerbot.h"
#include "strategy/actions/AttackAction.h"
#include "../../PlayerbotAIConfig.h"
#include "Movement/MovementGenerator.h"
#include "AI/CreatureAI.h"
#include "Entities/Pet/Pet.h"
#include "../../LootObjectStack.h"

using namespace ai;

bool AttackAction::Execute(Event event)
{
    Unit* target = GetTarget();

    if (!target)
        return false;

    return Attack(target);
}

bool AttackMyTargetAction::Execute(Event event)
{
    // This action only runs as a direct chat order ("attack"), never from
    // grind/combat triggers, so failure reasons must reach the master -
    // trigger-executed actions are non-verbose by default and would fail
    // silently here.
    MakeVerbose();

    Player* master = GetMaster();
    if (!master)
        return false;

    Unit* target = master->GetSelectedUnit();
    if (!target)
    {
        ai->TellMaster("You have no target");
        return false;
    }

    if (!Attack(target))
        return false;

    // Answer the order. Grinding bots stay silent (their attack calls are not
    // verbose), but a direct order must be visibly accepted - "the bot said
    // nothing and did nothing" is indistinguishable from "the command never
    // reached the bot" otherwise.
    ostringstream out;
    out << "Attacking " << target->GetName();
    if (ai->GetBotAttackRange(target) > 0.0f && !IsInMeleeRange(target))
        out << " at range";
    ai->TellMaster(out);
    return true;
}

bool AttackAction::Attack(Unit* target)
{
    if (bot->IsFlying())
    {
        if (verbose) ai->TellMaster("I cannot attack in flight");
        return false;
    }

    if (!target)
    {
        if (verbose) ai->TellMaster("I have no target");
        return false;
    }

    ostringstream msg;
    msg << target->GetName();
    if (bot->IsFriendlyTo(target))
    {
        msg << " is friendly to me";
        if (verbose) ai->TellMaster(msg.str());
        return false;
    }
    if (!target->IsAlive())
    {
        msg << " is already dead";
        if (verbose) ai->TellMaster(msg.str());
        // A dead current target would just trip the invalid-target trigger
        // next tick; report it now instead of "successfully" attacking air.
        return false;
    }
    if (!bot->IsWithinLOSInMap(target))
    {
        msg << " is not on my sight";
        if (verbose) ai->TellMaster(msg.str());
        return false;
    }
    if (!bot->IsWithinDistInMap(target, sPlayerbotAIConfig.sightDistance))
    {
        // Same rule InvalidTargetValue enforces: the combat engine drops an
        // out-of-sight target at ACTION_HIGH+9 before any chase action
        // (ACTION_NORMAL+8) can run, so starting the swing here would only
        // produce an Attack -> instant drop cycle. Refuse with a reason.
        msg << " is too far away";
        if (verbose) ai->TellMaster(msg.str());
        return false;
    }

    if (bot->IsMounted())
    {
        WorldPackets::Spells::CancelMountAura cancelMount{WorldPacket(CMSG_CANCEL_MOUNT_AURA)};
        bot->GetSession()->HandleCancelMountAuraOpcode(cancelMount);
    }

    // An eating/drinking bot ordered straight into a fight (already in range,
    // so no reach action stands it up) would try to swing from its chair.
    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // Face the target: melee swings require a 120-degree front arc and spell
    // casts a 90-degree one, and after a chase the server-side orientation
    // can lag behind. Facing here makes the very next swing/cast land.
    if (!bot->isInFront(target, M_PI / 2))
        bot->SetFacingTo(bot->GetAbsoluteAngle(target));

    // This runs every melee tick for the same victim, so only do the
    // target-switch work when the victim actually changed. Re-sending
    // AttackStart (and restarting the pet's chase) every tick is what used
    // to visibly restart the attack animation from frame zero.
    Unit* oldTarget = context->GetValue<Unit*>("current target")->Get();
    if (oldTarget != target)
    {
        ObjectGuid guid = target->GetGUID();
        bot->SetSelection(guid);

        context->GetValue<Unit*>("old target")->Set(oldTarget);
        context->GetValue<Unit*>("current target")->Set(target);
        context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);

        if (Pet* pet = bot->GetPet())
        {
            pet->SetTarget(target->GetGUID());
            pet->AI()->JustEngagedWith(target);
            pet->GetCharmInfo()->SetIsCommandAttack(true);
            pet->AI()->AttackStart(target);
        }
    }

    ai->ChangeEngine(BOT_STATE_COMBAT);

    // --- Engage and fight, core-native ---------------------------------------
    //
    // The only attack this core executes by itself is the melee swing:
    // Unit::Attack(victim, meleeAttack=true) arms UNIT_STATE_MELEE_ATTACKING
    // and Unit::DoMeleeAttackIfReady() then swings every update on its own.
    // Every ranged attack - a caster nuke, a wand shoot, a hunter auto-shot -
    // is a SPELL that the AI must actively cast; nothing in the core will do
    // it for the bot. That is why a ranged bot must never just "be engaged":
    //
    //  * the approach distance is what the bot can actually attack with,
    //    computed from its own spellbook and equipped weapon
    //    (PlayerbotAI::GetBotAttackRange - the core's own spell ranges, not a
    //    per-class guess), so the bot walks into range instead of stopping
    //    wherever a heuristic says;
    //  * a bot with NO usable ranged attack (unlearned spells, unusable item
    //    in the ranged slot) is a melee bot for this fight - it closes in and
    //    swings, because standing at spell range waiting for a shot it cannot
    //    fire is exactly the "ranged bot does nothing" bug;
    //  * once in range the attack itself is issued immediately. For ranged
    //    bots that is the best ready spell cast straight at the victim -
    //    waiting for the trigger/queue machinery to get around to it was the
    //    other half of the same bug. Melee bots need no such push: the swing
    //    loop above takes over.
    float const attackRange = ai->GetBotAttackRange(target);
    bool const ranged = attackRange > 0.0f;
    bool const inMeleeRange = IsInMeleeRange(target);

    if (!inMeleeRange)
    {
        if (ranged && bot->GetExactDist(target) > attackRange + sPlayerbotAIConfig.contactDistance)
        {
            // Stop a couple of yards inside the attack envelope so target
            // movement does not instantly push the bot out of range again.
            float const stop = std::max(attackRange - 2.0f, sPlayerbotAIConfig.meleeDistance);
            if (!MoveTo(target, stop))
            {
                if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    bot->AttackStop();

                if (verbose)
                {
                    ostringstream out;
                    out << "I cannot get in range of " << target->GetName();
                    ai->TellMaster(out);
                }
                return false;
            }
        }
        else if (!ranged)
        {
            // Starting the swing is not starting the fight. The core only lets
            // a melee hit land inside Unit::IsWithinMeleeRange() (3D and
            // combat-reach based), so the order that engages also walks: a bot
            // that provably cannot walk says so instead of posing forever.
            if (!ApproachForMelee(target))
            {
                if (bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    bot->AttackStop();

                if (verbose)
                {
                    ostringstream out;
                    out << "I cannot get to " << target->GetName();
                    ai->TellMaster(out);
                }
                return false;
            }
        }
    }

    // Engaging is not optional. Unit::Attack() is what creates the victim
    // link ("I am fighting that thing") the rest of the fight is built on:
    // the melee swing loop, the pet's own attack order, the threat/attacker
    // bookkeeping and every trigger that reads GetVictim(). Ordering
    // "attack" - or grinding - must never leave the bot with only an AI-side
    // target name while the core still believes the bot is fighting nobody.
    //
    // meleeAttack=false is the core's own "engage at range" mode: the victim
    // is set exactly as for a melee attack, but UNIT_STATE_MELEE_ATTACKING
    // stays clear, so Unit::DoMeleeAttackIfReady() never runs its range/arc
    // test and a caster or hunter cannot end up reporting
    // AttackSwingErr::NotInRange from 13 yards. A bot that ended up inside
    // melee range engages in melee mode instead - swings are the one attack
    // the core will drive for it.
    bool const wantMelee = !ranged || inMeleeRange;
    bool const meleeAttacking = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);

    if (bot->GetVictim() != target || meleeAttacking != wantMelee)
        bot->Attack(target, wantMelee);

    // Strike right now if possible. For a ranged bot this IS the attack:
    // FindBestAttackSpell only returns spells the core's own trial cast
    // accepts for this target (cooldown ready, power affordable, in range,
    // not immune), and CastSpell reports honestly when the cast still fails,
    // so the engine falls through to alternatives instead of pretending.
    if (ranged && !bot->IsNonMeleeSpellCast(false, true, true)
            && bot->GetExactDist(target) <= attackRange + sPlayerbotAIConfig.contactDistance)
    {
        if (uint32 const spellId = ai->FindBestAttackSpell(target))
            ai->CastSpell(spellId, target);
    }

    return true;
}

bool AttackDuelOpponentAction::isUseful()
{
	return AI_VALUE(Unit*, "duel target");
}

bool AttackDuelOpponentAction::Execute(Event event)
{
	return Attack(AI_VALUE(Unit*, "duel target"));
}
