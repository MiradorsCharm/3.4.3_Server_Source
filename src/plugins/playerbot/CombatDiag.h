#pragma once

#include "CombatSnapshotSource.h"

#include <cstdint>
#include <string>

namespace ai
{
    // ---------------------------------------------------------------------------
    // Combat diagnostics (AiPlayerbot.DebugCombat, the in-game "combat debug" bot
    // command, and the automatic "stuck in an attack stance" report).
    //
    // A snapshot is every fact that can stop a bot from closing on - or hitting -
    // its victim. PlayerbotAI fills the struct from live core objects
    // (CombatSnapshotSource.h); the decision logic below only ever reads that
    // plain struct, so it can be (and is) exercised by the CI harness with fake
    // snapshots instead of a running server. Keep both halves free of core calls.
    // ---------------------------------------------------------------------------

    enum class CombatStallAction : uint8_t
    {
        None,         // nothing actionable (or the bot is simply fighting)
        ClearCast,    // cancel what blocks movement/swings, then let the AI retry
        RetryMove,    // re-issue the approach toward the victim
        DropTarget    // this victim is unreachable: disengage and say why
    };

    struct CombatStall
    {
        bool report = false;                  // worth a console line + a word to the master
        CombatStallAction action = CombatStallAction::None;
        std::string reason;
    };

    // Decides whether a bot that has a target is actually stuck, what to do about
    // it, and what to tell the operator. Returns report == false when the state is
    // normal, so the caller stays silent.
    CombatStall EvaluateCombatStall(CombatSnapshot const& snap);

    // One compact, grep-friendly line with every number in the snapshot.
    std::string FormatCombatSnapshot(CombatSnapshot const& snap);

    // Where a melee bot should end up: never further out than the core's own
    // swing envelope (3D and reach-based), never closer than 1.5 yards so it does
    // not walk into the target's collision box, and never further away than it
    // already is (that would back the bot up instead of approaching).
    float ComputeMeleeStopDistance(float swingRange, float distance3d, float zGap, float configured);
}
