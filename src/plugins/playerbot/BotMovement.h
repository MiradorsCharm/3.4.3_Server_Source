/*
 * Playerbot AI - movement.
 *
 * Bots are real players without a client, and in this core a player is moved
 * by its client: WorldSession::HandleMovementOpcode() is the only path that
 * both updates the server position (Unit::UpdatePosition -> PlayerRelocation)
 * and tells everyone else about it (MoveUpdate broadcast). Server-side
 * MotionMaster splines on a Player fight this pipeline - HandleMovementOpcode
 * drops packets while a spline runs, and nothing ever broadcasts the resulting
 * position - which is exactly what the previous implementation got wrong.
 *
 * So this module plays the part of the missing client: it plans a path with
 * the core's own PathGenerator (mmaps when available, straight fallback when
 * not), advances along it at the bot's real run speed, and feeds the result
 * back through the bot's own socket-less WorldSession as CMSG_MOVE_* packets.
 * Everything downstream - collision-free relocation, MoveUpdate broadcast,
 * Player::isMoving(), cast interruption, the auto-repeat wand/auto-shot gate
 * in Unit::_UpdateAutoRepeatSpell - then behaves as it would for a real
 * player, because it IS the real player pipeline.
 */

#ifndef PLAYERBOT_BOT_MOVEMENT_H
#define PLAYERBOT_BOT_MOVEMENT_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Optional.h"
#include "Position.h"

#include <G3D/Vector3.h>
#include <vector>

class Player;
class Unit;
class WorldPacket;

enum class BotMoveMode
{
    None,       // standing still
    Point,      // walk to a fixed point
    Chase,      // pursue a unit until it is within a given stop distance
    Follow      // stay behind a unit (master)
};

class BotMovement
{
public:
    explicit BotMovement(Player* bot) : _bot(bot) { }

    /// Advance the mover one AI tick. Called from BotAI::Update on the map
    /// thread; never touches shared manager state.
    void Update(uint32 diff);

    // --- goals -------------------------------------------------------------
    void MoveTo(float x, float y, float z, float stopDistance, bool faceGoal = false);
    void Chase(Unit* target, float stopDistance, float maxDistance = 0.0f);
    void Follow(Unit* target, float distance);
    void Stop();
    /// Turn in place while stationary (sends heartbeats with a new
    /// orientation - the only facing mechanism that sticks for a player).
    void Face(Unit* target);
    void Face(float orientation);
    void Face(float x, float y);

    // --- queries -----------------------------------------------------------
    BotMoveMode GetMode() const { return _mode; }
    bool IsMoving() const { return _moving; }
    bool HasLiveGoal() const;
    /// true when the current goal has been reached (or no goal is set)
    bool Arrived() const;
    ObjectGuid GetGoalUnit() const { return _goalUnit; }
    float GetStopDistance() const { return _stopDistance; }
    /// Where the mover wants to end up (target position for Chase/Follow).
    bool GetGoalPosition(float& x, float& y, float& z) const;

    /// Called by BotAI when something (root, cast, teleport, death) means the
    /// bot must not walk this tick. Sends the stop packet once.
    void HaltExternal();

private:
    // --- packet plumbing (the "client" half) --------------------------------
    bool SendMovementPacket(uint32 opcode, bool forward, float x, float y, float z, float orientation, bool swimming);
    bool SendStart(float x, float y, float z, float orientation, bool swimming);
    bool SendHeartbeat(float x, float y, float z, float orientation, bool swimming);
    void SendStop(float x, float y, float z, float orientation, bool swimming);

    // --- pathing -------------------------------------------------------------
    bool EnsurePath(float gx, float gy, float gz, bool forceRebuild);
    void InvalidatePath();
    /// advance by 'step' yards along the path; returns false when the goal is reached
    bool AdvanceAlongPath(float step, float& nx, float& ny, float& nz, float& no);
    void ClampToMap(float& x, float& y, float& z) const;

    static float AngleToTarget(float fromX, float fromY, float toX, float toY);

    float SpeedFor(bool swimming) const;
    bool AllowedToMove() const;
    Unit* ResolveUnit(ObjectGuid guid) const;

    Player* _bot;

    BotMoveMode _mode = BotMoveMode::None;
    ObjectGuid _goalUnit;               // Chase/Follow
    Position _goalPoint;                // Point (and fallback for unresolved units)
    float _stopDistance = 0.0f;
    float _followDistance = 0.0f;       // Follow
    float _maxDistance = 0.0f;          // Chase: never chase further than this
    bool _faceGoal = false;             // turn toward the goal on arrival

    // path following state
    std::vector<G3D::Vector3> _path;    // waypoints, [0] is the start position
    size_t _pathIndex = 0;
    uint32 _pathAge = 0;
    uint32 _repathCooldown = 0;
    Position _pathGoal;

    // packet cadence state
    bool _moving = false;
    bool _stoppedPending = false;
    uint32 _heartbeatCooldown = 0;
    uint32 _moveBudgetMs = 0;           // unreported movement time (real clients report every heartbeat)
    float _lastSentOrientation = -1000.0f;
    uint32 _turnCooldown = 0;
    Optional<float> _wantedFacing;      // set while standing still and asked to face
    bool _waitingForTeleport = false;
};

#endif
