#include "BotHazards.h"

#include "BotAI.h"
#include "BotConfig.h"
#include "Player.h"
#include "Unit.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "AreaTrigger.h"
#include "AreaTriggerTemplate.h"
#include "DynamicObject.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "Grids/Notifiers/GridNotifiers.h"
#include "Grids/Cells/CellImpl.h"
#include "Grids/Notifiers/GridNotifiersImpl.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    // How far around the bot we look for ground effects. Beyond this a hazard is
    // not close enough to matter this tick; the mover walks slowly enough that
    // the next scan catches anything we approach.
    float const HAZARD_SCAN_RADIUS = 45.0f;

    // Re-scan cadence. Ground effects do not appear and expire faster than this,
    // and the escape logic re-checks the *tracked* set every tick regardless.
    uint32 const HAZARD_SCAN_INTERVAL_MS = 250;

    float PlanarDist(float ax, float ay, float bx, float by)
    {
        float dx = ax - bx;
        float dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }
}

bool BotHazards::IsHarmful(SpellInfo const* info) const
{
    if (!info)
        return false;

    // Positive (friendly) area spells - Consecration cast by a friendly pal,
    // Healing Rain, Efflorescence, sanctuary zones - are never a reason to move.
    if (info->IsPositive())
        return false;

    // A ground effect matters to the bot when it deals periodic damage or lays
    // down a damaging / controlling aura. We look at the effect list rather
    // than hardcoding spell ids so this works for any encounter's fire.
    for (SpellEffectInfo const& eff : info->GetEffects())
    {
        if (!eff.IsEffect())
            continue;

        switch (eff.Effect)
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_TRIGGER_MISSILE:
            case SPELL_EFFECT_TRIGGER_MISSILE_SPELL_WITH_VALUE:
                return true;
            default:
                break;
        }

        if (eff.IsAura())
        {
            switch (eff.ApplyAuraName)
            {
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                case SPELL_AURA_PERIODIC_LEECH:
                case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_MOD_ROOT:
                case SPELL_AURA_MOD_SILENCE:
                    return true;
                default:
                    break;
            }
        }
    }

    return false;
}

void BotHazards::AddHazardFromAreaTrigger(AreaTrigger* at)
{
    if (!at || at->IsRemoved())
        return;

    // Our own / friendly triggers are not hazards. When the caster is gone we
    // fall back to the spell's own hostility judgement (a boss's leftover fire
    // is still fire), so a de-spawned caster does not make the floor "safe".
    if (Unit* caster = at->GetCaster())
    {
        if (caster == _bot || caster->IsFriendlyTo(_bot))
            return;
    }

    SpellInfo const* info = sSpellMgr->GetSpellInfo(at->GetSpellId(), _bot->GetMap()->GetDifficultyID());
    if (!IsHarmful(info))
        return;

    float radius = at->GetMaxSearchRadius();
    if (radius <= 0.0f)
        return;

    BotHazard h;
    h.center.Relocate(at->GetPositionX(), at->GetPositionY(), at->GetPositionZ());
    h.radius = radius + sBotConfig->HazardSafetyMargin;
    h.source = at->GetGUID();
    h.spellId = at->GetSpellId();
    _hazards.push_back(h);
}

void BotHazards::AddHazardFromDynObject(DynamicObject* dyn)
{
    if (!dyn)
        return;

    if (Unit* caster = dyn->GetCaster())
    {
        if (caster == _bot || caster->IsFriendlyTo(_bot))
            return;
    }

    SpellInfo const* info = dyn->GetSpellInfo();
    if (!IsHarmful(info))
        return;

    float radius = dyn->GetRadius();
    if (radius <= 0.0f)
        return;

    BotHazard h;
    h.center.Relocate(dyn->GetPositionX(), dyn->GetPositionY(), dyn->GetPositionZ());
    h.radius = radius + sBotConfig->HazardSafetyMargin;
    h.source = dyn->GetGUID();
    h.spellId = dyn->GetSpellId();
    _hazards.push_back(h);
}

bool BotHazards::Scan(uint32 diff)
{
    if (_scanCooldown > diff)
    {
        _scanCooldown -= diff;
        // Keep the "standing in it" flag current between full scans: hazards
        // move (Defile, rolling fire) and the bot moves, so re-evaluate cheaply.
        _standingIn = false;
        float bestOverlap = 0.0f;
        for (BotHazard const& h : _hazards)
        {
            float d = PlanarDist(_bot->GetPositionX(), _bot->GetPositionY(),
                                 h.center.GetPositionX(), h.center.GetPositionY());
            float overlap = h.radius - d;
            if (overlap > 0.0f && overlap > bestOverlap)
            {
                bestOverlap = overlap;
                _worst = h;
                _standingIn = true;
            }
        }
        return !_hazards.empty();
    }
    _scanCooldown = HAZARD_SCAN_INTERVAL_MS;

    _hazards.clear();
    _standingIn = false;

    if (!_bot->IsInWorld())
        return false;

    // Collect nearby area triggers and dynamic objects in one grid sweep.
    std::vector<WorldObject*> found;
    Trinity::AllWorldObjectsInRange check(_bot, HAZARD_SCAN_RADIUS);
    Trinity::WorldObjectListSearcher<Trinity::AllWorldObjectsInRange> searcher(
        _bot, found, check,
        GRID_MAP_TYPE_MASK_AREATRIGGER | GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
    Cell::VisitGridObjects(_bot, searcher, HAZARD_SCAN_RADIUS);

    for (WorldObject* obj : found)
    {
        if (AreaTrigger* at = obj->ToAreaTrigger())
            AddHazardFromAreaTrigger(at);
        else if (DynamicObject* dyn = obj->ToDynObject())
            AddHazardFromDynObject(dyn);
    }

    // Decide whether the bot is standing in the worst hazard.
    float bestOverlap = 0.0f;
    for (BotHazard const& h : _hazards)
    {
        float d = PlanarDist(_bot->GetPositionX(), _bot->GetPositionY(),
                             h.center.GetPositionX(), h.center.GetPositionY());
        float overlap = h.radius - d;
        if (overlap > 0.0f && overlap > bestOverlap)
        {
            bestOverlap = overlap;
            _worst = h;
            _standingIn = true;
        }
    }

    if (_standingIn && sBotConfig->DebugMove)
        TC_LOG_DEBUG("playerbot", "[hazard] {} standing in spell {} (r={:.1f})",
            _bot->GetName(), _worst.spellId, _worst.radius);

    return !_hazards.empty();
}

bool BotHazards::IsSpotDangerous(float x, float y, float z, float extraMargin) const
{
    for (BotHazard const& h : _hazards)
    {
        float d = PlanarDist(x, y, h.center.GetPositionX(), h.center.GetPositionY());
        // Only treat as dangerous when roughly on the same vertical level: a
        // hazard one floor below should not pin a bot on the walkway above it.
        if (std::fabs(z - h.center.GetPositionZ()) > 6.0f)
            continue;
        if (d < h.radius + extraMargin)
            return true;
    }
    return false;
}

bool BotHazards::IsPathDangerous(float ax, float ay, float bx, float by) const
{
    if (_hazards.empty())
        return false;

    float const segLen = PlanarDist(ax, ay, bx, by);
    if (segLen < 0.01f)
        return IsSpotDangerous(bx, by, _bot->GetPositionZ());

    // Sample the segment every ~2 yards; a hazard radius is always larger than
    // that, so no circle can slip between two samples.
    int const steps = std::max(1, int(segLen / 2.0f));
    for (int i = 0; i <= steps; ++i)
    {
        float t = float(i) / float(steps);
        float px = ax + (bx - ax) * t;
        float py = ay + (by - ay) * t;
        for (BotHazard const& h : _hazards)
            if (PlanarDist(px, py, h.center.GetPositionX(), h.center.GetPositionY()) < h.radius)
                return true;
    }
    return false;
}

bool BotHazards::FindSafeSpot(Position& out, Unit const* anchor) const
{
    if (_hazards.empty())
        return false;

    float const startX = _bot->GetPositionX();
    float const startY = _bot->GetPositionY();
    float const startZ = _bot->GetPositionZ();

    // Prefer a spot that keeps us near the anchor (the victim) so a melee bot
    // does not sprint out of the fight when it only needs to sidestep. When
    // there is no anchor, just get clear.
    float const anchorX = anchor ? anchor->GetPositionX() : startX;
    float const anchorY = anchor ? anchor->GetPositionY() : startY;

    // Try progressively larger hops in a ring of directions, picking the first
    // safe, reachable, in-LOS spot that minimises distance to the anchor. Using
    // GetFirstCollisionPosition means the candidate never sits through a wall.
    float bestScore = std::numeric_limits<float>::max();
    bool found = false;

    static float const distances[] = { 6.0f, 9.0f, 13.0f, 18.0f, 24.0f };
    int const kDirs = 16;

    for (float dist : distances)
    {
        for (int d = 0; d < kDirs; ++d)
        {
            float angle = (2.0f * float(M_PI) * float(d)) / float(kDirs);

            Position candidate = _bot->GetFirstCollisionPosition(dist, angle);
            float cx = candidate.GetPositionX();
            float cy = candidate.GetPositionY();
            float cz = candidate.GetPositionZ();

            // The collision helper may have stopped short of 'dist' at a wall;
            // require that we actually gained ground away from the hazard.
            if (IsSpotDangerous(cx, cy, cz))
                continue;
            // Don't hop through fire to reach clear ground.
            if (IsPathDangerous(startX, startY, cx, cy))
                continue;

            float score = PlanarDist(cx, cy, anchorX, anchorY);
            if (score < bestScore)
            {
                bestScore = score;
                out.Relocate(cx, cy, cz);
                found = true;
            }
        }

        // A near ring that already yields a safe spot is preferred over sprinting
        // far; stop widening once we have a candidate.
        if (found)
            break;
    }

    return found;
}

bool BotHazards::ShouldInterrupt(Unit const* caster)
{
    if (!caster)
        return false;

    // Only bother with a real, non-instant cast that is flagged interruptible.
    // We inspect the generic and channeled slots the interrupt kit can actually
    // stop - never the autorepeat slot (wand / auto-shot is not "a cast").
    for (uint32 i = CURRENT_GENERIC_SPELL; i <= CURRENT_CHANNELED_SPELL; ++i)
    {
        Spell* spell = caster->GetCurrentSpell(CurrentSpellTypes(i));
        if (!spell)
            continue;

        SpellInfo const* info = spell->GetSpellInfo();
        if (!info)
            continue;

        // Positive casts (a boss self-buffing is annoying but not the classic
        // "interrupt the heal/nuke" case we key an interrupt on) are skipped
        // unless they are a heal, which is always worth stopping.
        bool const isHeal = info->HasEffect(SPELL_EFFECT_HEAL)
            || info->HasEffect(SPELL_EFFECT_HEAL_PCT);
        if (info->IsPositive() && !isHeal)
            continue;

        // Respect the core's own interruptibility rules (channel vs cast, the
        // uninterruptible attribute, immunities).
        if (!info->CanBeInterrupted(nullptr, caster))
            continue;

        return true;
    }

    return false;
}
