#pragma once

#include <cstdint>
#include <string>

namespace ai
{
    // Plain-data view of "why is this bot not hitting its target". Filled in by
    // PlayerbotAI from live core objects; evaluated by CombatDiag.cpp. It is kept
    // in its own header, free of any core include, so the regression harness can
    // build the decision logic against fake snapshots (see
    // tests/playerbot_combat_diag_test.py).
    struct CombatSnapshot
    {
        // victim side
        bool hasTarget = false;
        bool targetAlive = false;
        bool sameMap = false;
        bool hasLOS = false;
        float distance3d = 0.0f;
        float zGap = 0.0f;                    // |bot.z - target.z|: the classic 2D-vs-3D trap
        float meleeRange = 0.0f;              // Unit::GetMeleeRange() of the attacker
        bool inMeleeRange = false;            // the core's own swing-range test
        bool inArc = false;                   // a swing also needs the 120 degree facing arc

        // bot side
        bool alive = false;
        bool attackState = false;             // UNIT_STATE_MELEE_ATTACKING
        bool swingReady = false;              // base swing timer elapsed
        bool mounted = false;
        bool flying = false;
        bool pacified = false;                // UNIT_FLAG_PACIFIED: every swing returns
        bool disarmed = false;                // UNIT_FLAG_DISARMED: no main-hand attack
        bool hasMeleeWeapon = false;
        bool movementDisabled = false;        // root/stun/dead/confuse/charm
        bool casting = false;
        bool movementBlockedByCast = false;   // Unit::IsMovementPreventedByCasting()
        uint32_t blockingSpell = 0;           // spell sitting in a current-cast slot
        bool teleported = false;              // Player::IsBeingTeleported()
        bool movingNow = false;               // generator or live spline is driving the bot
        bool canMoveNow = false;              // the AI's own "am I allowed to start moving"
        float runSpeed = 0.0f;

        std::string swingError;               // the core's AttackSwingErr, as text
        std::string trace;                    // AI action trace of the last tick
        uint32_t stallMs = 0;                 // how long the bot has made no progress
    };
}
