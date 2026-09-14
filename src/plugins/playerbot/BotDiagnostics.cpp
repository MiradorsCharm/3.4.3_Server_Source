#include "BotDiagnostics.h"

#include "BotAI.h"
#include "BotConfig.h"
#include "BotMovement.h"
#include "BotSpells.h"
#include "Player.h"
#include "Unit.h"
#include "SpellInfo.h"
#include "Spell.h"
#include "Log.h"

#include <cstdio>

namespace
{
    float AbsFloat(float v) { return v < 0.0f ? -v : v; }
}

void BotDiagnostics::ReportStall(BotAI& ai)
{
    Player* bot = ai.GetBot();
    BotCombat& combat = ai.GetCombat();
    BotMovement& mover = ai.GetMovement();

    Unit* victim = combat.GetVictim();
    if (!victim || !victim->IsAlive())
    {
        ai._stallSeconds = 0;
        ai.GetStallTracker().hasMark = false;
        return;
    }

    BotAI::StallTracker& tracker = ai.GetStallTracker();

    // Progress = the gap moved or the victim's health moved since last second.
    float dist = bot->GetExactDist(victim);
    uint32 victimHealth = victim->GetHealth();
    bool progress = !tracker.hasMark ||
        AbsFloat(dist - tracker.lastDistance) > 0.35f ||
        victimHealth != tracker.lastVictimHealth;

    tracker.lastDistance = dist;
    tracker.lastVictimHealth = victimHealth;
    tracker.hasMark = true;

    if (progress)
    {
        ai._stallSeconds = 0;
        return;
    }

    ++ai._stallSeconds;
    if (ai._stallSeconds * 1000 < sBotConfig->StallReportMs)
        return;
    if (ai._stallReportCooldown != 0)
        return;
    ai._stallReportCooldown = sBotConfig->StallReportCooldownMs;

    bool const inMeleeRange = bot->IsWithinMeleeRange(victim);
    bool const inArc = bot->HasInArc(2.0f * float(M_PI) / 3.0f, victim);
    bool const attackState = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
    char swingErr[16] = "none";
    if (Optional<AttackSwingErr> err = bot->GetAttackSwingError())
    {
        switch (*err)
        {
            case AttackSwingErr::NotInRange: std::snprintf(swingErr, sizeof(swingErr), "NotInRange"); break;
            case AttackSwingErr::BadFacing: std::snprintf(swingErr, sizeof(swingErr), "BadFacing"); break;
            case AttackSwingErr::CantAttack: std::snprintf(swingErr, sizeof(swingErr), "CantAttack"); break;
            case AttackSwingErr::DeadTarget: std::snprintf(swingErr, sizeof(swingErr), "DeadTarget"); break;
        }
    }

    char spellState[48] = "none";
    if (Spell* cur = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        std::snprintf(spellState, sizeof(spellState), "cast %u", cur->GetSpellInfo()->Id);
    else if (Spell* chan = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        std::snprintf(spellState, sizeof(spellState), "channel %u", chan->GetSpellInfo()->Id);
    else if (Spell* ar = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        std::snprintf(spellState, sizeof(spellState), "autorepeat %u", ar->GetSpellInfo()->Id);

    char line[512];
    std::snprintf(line, sizeof(line),
        "Bot %s stalled %us on %s: d3d=%.2f meleeRange=%.2f inRange=%d arc=%d atk=%d swingErr=%s "
        "moving=%d mode=%d los=%d combat=%d spell=%s stance=%s reason=%s",
        bot->GetName().c_str(), ai._stallSeconds, victim->GetName().c_str(),
        dist, bot->GetMeleeRange(victim), int(inMeleeRange), int(inArc),
        int(attackState), swingErr,
        int(mover.IsMoving()), int(mover.GetMode()),
        int(bot->IsWithinLOSInMap(victim)), int(bot->IsInCombat()),
        spellState,
        combat.GetStance() == BotCombatStance::Melee ? "melee" : "ranged",
        combat.GetVictimReason().c_str());

    TC_LOG_ERROR("playerbot", "{}", line);

    if (Player* master = ai.GetMaster())
        bot->Whisper(line, LANG_UNIVERSAL, master);
}
