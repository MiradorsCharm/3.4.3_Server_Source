/*
 * Playerbot AI - dungeon / raid mechanic awareness.
 *
 * Bosses and trash in this core drop damage on the ground through two object
 * kinds a real player learns to read and step out of:
 *
 *   - AreaTrigger  (Blizzard's modern ground effects: Defile, Flame patches,
 *                   Sha spikes, most WotLK+ "get out of the fire" mechanics)
 *   - DynamicObject(the classic persistent-area spells: Rain of Fire, Blizzard,
 *                   Death and Decay, Consecration-style ground AoE)
 *
 * This module is the bot's "eyes on the floor". Once per tick it scans the
 * grid around the bot for hostile ground effects, decides whether the bot is
 * standing in (or about to path through) one, and offers the nearest safe spot
 * to step to. It also exposes small queries the combat brain uses to keep the
 * bot from *walking into* a hazard while chasing or holding a caster band.
 *
 * Everything is read-only against the core objects and uses the same LOS /
 * collision helpers the movement code already trusts (GetFirstCollisionPosition
 * + UpdateAllowedPositionZ), so the escape point is always somewhere the
 * client-faithful mover can actually reach.
 *
 * On top of the ground-effect avoidance the module answers two boss-fight
 * questions the class brains use: "should I interrupt what my target is
 * casting right now?" (a dangerous, interruptible cast) and a light
 * spread/stack hint for raid positioning.
 */

#ifndef PLAYERBOT_BOT_HAZARDS_H
#define PLAYERBOT_BOT_HAZARDS_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <vector>

class Player;
class Unit;
class BotAI;
class AreaTrigger;
class DynamicObject;
class SpellInfo;

/// One dangerous ground effect near the bot: a circle (centre + radius) the
/// bot should not stand in. Polygon area triggers are approximated by their
/// bounding radius, which is the conservative (safe) choice.
struct BotHazard
{
    Position center;
    float radius = 0.0f;      // includes the bot's own body + a safety margin
    ObjectGuid source;        // the AreaTrigger / DynamicObject guid (debug)
    uint32 spellId = 0;
};

class BotHazards
{
public:
    BotHazards(BotAI* ai, Player* bot) : _ai(ai), _bot(bot) { }

    /// Rescan the floor around the bot. Cheap and throttled internally; call it
    /// every tick. Returns true when at least one hazard is currently tracked.
    bool Scan(uint32 diff);

    /// Is the bot currently standing inside a tracked hazard?
    bool InDanger() const { return _standingIn; }

    /// The hazard the bot is standing in (or the most urgent nearby one), or
    /// nullptr when the floor is clear.
    BotHazard const* CurrentDanger() const { return _standingIn ? &_worst : nullptr; }

    /// Would this world position sit inside any tracked hazard (plus margin)?
    /// Used by the combat brain to reject a chase/kite destination.
    bool IsSpotDangerous(float x, float y, float z, float extraMargin = 0.0f) const;

    /// Would the straight segment from (ax,ay) to (bx,by) clip a hazard? Used
    /// to veto a movement goal that would drag the bot through fire.
    bool IsPathDangerous(float ax, float ay, float bx, float by) const;

    /// Find the closest reachable position clear of every tracked hazard,
    /// preferring to stay near 'anchor' (usually the current victim) so the bot
    /// does not run out of the fight while dodging. Returns false when no safe
    /// spot could be found (caller should just keep still).
    bool FindSafeSpot(Position& out, Unit const* anchor) const;

    /// Boss-cast interrupt helper: the unit is casting a dangerous, interruptible
    /// spell right now (used by class scripts that own an interrupt ability).
    static bool ShouldInterrupt(Unit const* caster);

    /// Number of tracked hazards (diagnostics / status line).
    std::size_t Count() const { return _hazards.size(); }

private:
    void AddHazardFromAreaTrigger(AreaTrigger* at);
    void AddHazardFromDynObject(DynamicObject* dyn);
    /// Is this ground-effect spell actually harmful to the bot? (deals periodic
    /// damage / applies a damaging or controlling aura and is hostile to us)
    bool IsHarmful(SpellInfo const* info) const;

    BotAI* _ai;
    Player* _bot;

    std::vector<BotHazard> _hazards;
    BotHazard _worst;              // the one the bot stands in / nearest
    bool _standingIn = false;
    uint32 _scanCooldown = 0;
};

#endif
