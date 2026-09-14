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

    return Attack(target);
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

    // Starting the swing is not starting the fight. The core only lets a melee hit
    // land inside Unit::IsWithinMeleeRange() (3D and combat-reach based), and until
    // this patch nothing at all was obliged to close that gap: the "enemy out of
    // melee" trigger pushed "reach melee" at a *lower* priority than the attack
    // action, which returned true every tick, so a bot that was out of reach (or
    // that the 2D distance value claimed was in reach) kept its attack state, kept
    // claiming success, and never swung. That is the "they just stand there doing
    // the attack animation" report. The order that engages therefore also walks,
    // and a bot that provably cannot walk says so instead of posing forever.
    ai->ChangeEngine(BOT_STATE_COMBAT);

    if (!IsInMeleeRange(target) && !ai->IsRanged(bot))
    {
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

    // Unit::Attack is a no-op when the same melee swing is already running,
    // but skip the call entirely so we never spam AttackStart packets.
    if (bot->GetVictim() != target || !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        bot->Attack(target, true);

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
