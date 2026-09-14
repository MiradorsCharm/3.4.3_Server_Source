#include "CombatDiag.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace ai;

// How long a bot has to make no progress toward a live target (or no damage
// while it stands in swing range) before the state counts as stuck instead of
// as normal chasing/positioning. Below this the caller stays quiet.
static uint32_t const COMBAT_STALL_MS = 4000;

float ai::ComputeMeleeStopDistance(float swingRange, float currentPlanarDistance, float zGap, float combinedReach, float configured)
{
    // The core's swing envelope is 3D and centre-to-centre:
    // GetExactDist(attacker, victim) <= max(reach + reach + 4/3, NOMINAL_MELEE_RANGE).
    // The vertical gap has to come out of that budget before the planar stop is
    // chosen - otherwise the bot parks exactly where the core still refuses the
    // swing.
    float stop = swingRange - 1.0f - std::fabs(zGap);

    // The configured stance is a gap from the target's *surface*, so the pair's
    // combined combat reach has to be added to it before it can cap a
    // centre-to-centre stop. The legacy math compared a surface-compensated
    // distance (GetDistance()/GetDistance2d() already subtract both reaches)
    // against the raw envelope and then handed the result to MoveTo() as a
    // surface-compensated stop: every bot therefore planted itself two to four
    // yards outside its own swing envelope and swung at nothing forever
    // (AttackSwingErr::NotInRange next to a log line claiming "3D 3.69 yd vs
    // 5.00 yd" - the two numbers were not measured in the same metric).
    stop = std::min(stop, combinedReach + configured);

    // Never inside the model (the swing test is meaningless there and the bot
    // shuffles against the collision box forever); big-reach pairs keep a
    // proportional minimum so a Tauren does not try to stand inside a Kodo.
    stop = std::max(stop, std::max(1.5f, combinedReach - 1.5f));

    // And never further away than it already is: MoveTo() reads a stop distance
    // above the current gap as "back off", which would undo the chase.
    return std::min(stop, currentPlanarDistance);
}

CombatStall ai::EvaluateCombatStall(CombatSnapshot const& snap)
{
    CombatStall stall;

    if (!snap.hasTarget || !snap.targetAlive || !snap.alive)
        return stall;                                          // nothing to report

    // A normal fight looks like this: in swing range, attack state on, swing
    // timer expired, no core-side complaint - and nothing that lets the swing
    // *look* healthy while the core drops it anyway. Pacify, disarm and a pending
    // teleport are listed here on purpose: none of them show up in the range/arc/
    // timer numbers, and a bot sitting in one of them is exactly the "standing in
    // its attack animation, doing nothing" report.
    bool const meleeCoreIsHappy = snap.inMeleeRange && snap.attackState && snap.swingReady
        && snap.hasLOS && snap.inArc && snap.swingError.empty()
        && !snap.pacified && !snap.disarmed && !snap.teleported;
    // Ranged/caster bots never enter melee range by design. A caster standing
    // at spell range casting bolts or auto-shot is "in combat" even though
    // every melee flag in the snapshot is unhappy; do not treat that as a
    // stall. UNIT_STATE_MELEE_ATTACKING can still be set because Attack() was
    // called on pull - the server shows the melee-attacking animation while
    // auto-shoot / wand / spells do the real damage.
    // A ranged bot "in spell range" but actually *inside* melee range is in the
    // 5-yd dead zone (auto-shot cannot fire, melee interrupts casts) - that is
    // not a happy state, it is its own stall (handled below as the dead-zone
    // branch). Exclude inMeleeRange from the happy gate.
    bool const rangedCoreIsHappy = snap.hasRangedAttack && snap.inSpellRange
        && !snap.inMeleeRange
        && snap.hasLOS && snap.inArc
        && !snap.pacified && !snap.disarmed && !snap.teleported;
    if (meleeCoreIsHappy || rangedCoreIsHappy)
        return stall;

    if (snap.stallMs < COMBAT_STALL_MS)
        return stall;                                          // still time to close the gap

    /*
     * The first explanation that fits wins: several of these are true at once
     * while only one of them is the reason the bot is standing still.
     */
    if (snap.teleported)
    {
        // Not actionable and not worth a console line: the AI is correctly waiting
        // for the teleport that a bot session has no client to acknowledge.
        stall.reason = "waiting for a teleport to finish (a bot has no client to ack it)";
        return stall;
    }

    if (snap.pacified)
    {
        stall.report = true;
        stall.action = CombatStallAction::DropTarget;
        stall.reason = "UNIT_FLAG_PACIFIED is set: the core drops every swing in AttackerStateUpdate()";
        return stall;
    }

    if (snap.disarmed)
    {
        // An *empty* main hand is not a stall: unarmed bots still swing (that is
        // what the hasMeleeWeapon field in the line below is for - it is a hint
        // for the reader, never a reason to give a bot orders it cannot do).
        stall.report = true;
        stall.action = CombatStallAction::DropTarget;
        stall.reason = "main hand disarmed (UNIT_FLAG_DISARMED): BASE_ATTACK is not allowed";
        return stall;
    }

    if (snap.mounted)
    {
        stall.report = true;
        stall.action = CombatStallAction::ClearCast;
        stall.reason = "still mounted: Unit::Attack() refuses mounted players, the mount was never cancelled";
        return stall;
    }

    if (snap.movementBlockedByCast)
    {
        stall.report = true;
        stall.action = CombatStallAction::ClearCast;
        std::ostringstream out;
        out << "a spell cast is blocking movement and swings";
        if (snap.blockingSpell)
            out << " (blocking spell " << snap.blockingSpell << ')';
        out << " - the cast outlived its own duration and nothing else clears it";
        stall.reason = out.str();
        return stall;
    }

    if (snap.movementDisabled || snap.flying)
    {
        stall.report = true;
        stall.reason = snap.flying
            ? "the bot is flying, which the movement gate refuses to override"
            : "movement is disabled (rooted, stunned, confused, charmed or dying)";
        return stall;
    }

    if (!snap.inMeleeRange && (!snap.hasRangedAttack || !snap.inSpellRange))
    {
        bool const outOfSpellRange = snap.hasRangedAttack && !snap.inSpellRange;
        float const allowedRange = outOfSpellRange ? snap.spellRange : snap.meleeRange;
        char const* rangeKind = outOfSpellRange ? "spell" : "swing";

        std::ostringstream out;
        // Two decimals: these numbers are compared against the swing envelope by
        // eye in the log, and "3.000000" pushes the useful half of the line off it.
        out.setf(std::ios::fixed, std::ios::floatfield);
        out.precision(2);
        if (!snap.canMoveNow)
        {
            stall.report = true;
            // Melee-only bots that cannot move at all cannot ever reach their
            // victim; ranged/caster bots outside spell range *and* unable to move
            // are in the same boat - drop target rather than retry forever.
            stall.action = CombatStallAction::DropTarget;
            out << "out of " << rangeKind << " range (3D " << snap.distance3d << " yd vs " << allowedRange
                << " yd) and the bot may not start moving at all";
        }
        else if (!snap.movingNow)
        {
            stall.report = true;
            stall.action = CombatStallAction::RetryMove;
            out << "out of " << rangeKind << " range (3D " << snap.distance3d << " yd vs " << allowedRange
                << " yd) and no approach is running: no movement generator and no live spline";
            if (snap.runSpeed < 0.5f)
                out << ", and MOVE_RUN speed is " << snap.runSpeed << " - the bot cannot walk anywhere";
        }
        else
        {
            stall.report = true;
            stall.action = CombatStallAction::RetryMove;
            out << "the approach is running but the gap is not closing (3D " << snap.distance3d
                << " yd vs " << allowedRange << " yd allowed";
            if (snap.zGap > 1.5f)
                out << ", and the target is " << snap.zGap
                    << " yd above/below: the " << rangeKind << " test is 3D while the AI steers by planar distance";
            out << " - a chase stuck against terrain looks exactly like a bot that refuses to fight";
        }

        stall.reason = out.str();
        return stall;
    }

    if (!snap.sameMap || !snap.hasLOS)
    {
        stall.report = true;
        stall.action = CombatStallAction::DropTarget;
        stall.reason = snap.sameMap
            ? "in swing range but there is no line of sight: AttackerStateUpdate() returns before any damage"
            : "the target is not on this map anymore";
        return stall;
    }

    if (!snap.attackState && !snap.hasRangedAttack)
    {
        stall.report = true;
        stall.action = CombatStallAction::RetryMove;
        stall.reason = "in swing range with no melee-attack state: AttackStart() never reached the core, "
            "so DoMeleeAttackIfReady() returns at its first line";
        return stall;
    }

    // If a bot with a ranged weapon actually made it INTO melee range, it
    // cannot shoot/cast (5-yd dead zone on ranged weapons, melee interrupts
    // on casts) - but it also isn't swinging. Walk it back out to spell
    // range instead of reporting it as a mysterious "nothing happening"
    // stall. Reported as an actionable RetryMove so the next tick issues
    // a kite/back-off.
    if (snap.hasRangedAttack && snap.inMeleeRange)
    {
        stall.report = true;
        stall.action = CombatStallAction::RetryMove;
        stall.reason = "ranged bot inside melee range (5-yd dead zone); cannot shoot and cannot swing";
        return stall;
    }

    if (!snap.inArc)
    {
        stall.report = true;
        stall.action = CombatStallAction::RetryMove;
        stall.reason = "in swing range but facing the wrong way: a swing needs the target inside the 120 degree "
            "arc and the orientation the AI set did not stick";
        return stall;
    }

    if (!snap.swingReady)
    {
        stall.report = true;
        stall.reason = "the swing timer has not elapsed for over four seconds; the attack timer is being reset "
            "by something other than a swing (see the action trace)";
        return stall;
    }

    stall.report = true;
    stall.action = CombatStallAction::DropTarget;
    stall.reason = "every core gate is satisfied and no damage was dealt; the swing is being consumed elsewhere "
        "(a queued next-melee-swing spell, zero attack power, or the victim is not in the attacker list)";
    return stall;
}

std::string ai::FormatCombatSnapshot(CombatSnapshot const& snap)
{
    std::ostringstream out;

    out.setf(std::ios::fixed, std::ios::floatfield);
    out.precision(2);

    out << "tgt=" << (snap.hasTarget ? (snap.targetAlive ? "alive" : "dead") : "none")
        << " d3d=" << snap.distance3d
        << " dz=" << snap.zGap
        << " melee=" << snap.meleeRange
        << " inRange=" << (snap.inMeleeRange ? 1 : 0)
        << " los=" << (snap.hasLOS ? 1 : 0)
        << " sameMap=" << (snap.sameMap ? 1 : 0)
        << " arc=" << (snap.inArc ? 1 : 0)
        << " | atk=" << (snap.attackState ? 1 : 0)
        << " swingReady=" << (snap.swingReady ? 1 : 0)
        << " swingErr=" << (snap.swingError.empty() ? "none" : snap.swingError)
        << " pacified=" << (snap.pacified ? 1 : 0)
        << " disarmed=" << (snap.disarmed ? 1 : 0)
        << " weapon=" << (snap.hasMeleeWeapon ? 1 : 0)
        << " ranged=" << (snap.hasRangedAttack ? 1 : 0)
        << " inSpellRange=" << (snap.inSpellRange ? 1 : 0)
        << " spellRange=" << snap.spellRange
        << " | cast=" << (snap.casting ? 1 : 0)
        << " castBlocksMove=" << (snap.movementBlockedByCast ? 1 : 0)
        << " spell=" << snap.blockingSpell
        << " | tele=" << (snap.teleported ? 1 : 0)
        << " mount=" << (snap.mounted ? 1 : 0)
        << " fly=" << (snap.flying ? 1 : 0)
        << " moveOff=" << (snap.movementDisabled ? 1 : 0)
        << " moving=" << (snap.movingNow ? 1 : 0)
        << " mayMove=" << (snap.canMoveNow ? 1 : 0)
        << " run=" << snap.runSpeed
        << " | stalled=" << snap.stallMs / 1000 << "s";

    if (!snap.trace.empty())
        out << " | trace: " << snap.trace;

    return out.str();
}
