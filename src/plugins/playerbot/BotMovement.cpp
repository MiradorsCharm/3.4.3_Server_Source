#include "BotMovement.h"

#include "BotConfig.h"
#include "Player.h"
#include "Unit.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "MovementDefines.h"
#include "GameTime.h"
#include "Log.h"
#include "MoveSpline.h"

#include <G3D/Vector3.h>
#include <cmath>

namespace
{
    float const BOT_ARRIVAL_EPS = 0.05f;      // yards
    float const BOT_REPATH_DRIFT = 3.0f;     // rebuild the path when the goal moved this far
    uint32 const BOT_REPATH_MS = 400;   // ms between two path builds
    uint32 const BOT_PATH_MAX_AGE_MS = 1500;         // ms
    uint32 const BOT_HEARTBEAT_MS = 150;    // ms between two progress packets while walking
    uint32 const BOT_TURN_MS = 200;         // ms between two in-place turn packets
    float const TURN_EPSILON = 0.05f;         // radians (~3 degrees)

    float AngleTo(float fromX, float fromY, float toX, float toY)
    {
        return std::atan2(toY - fromY, toX - fromX);
    }

    float NormalizeAngle(float a)
    {
        while (a < 0.0f)
            a += 2.0f * float(M_PI);
        while (a > 2.0f * float(M_PI))
            a -= 2.0f * float(M_PI);
        return a;
    }

    float AngleDelta(float from, float to)
    {
        float d = NormalizeAngle(to) - NormalizeAngle(from);
        if (d > float(M_PI))
            d -= 2.0f * float(M_PI);
        if (d < -float(M_PI))
            d += 2.0f * float(M_PI);
        return d;
    }
}

bool BotMovement::AllowedToMove() const
{
    if (!_bot->IsAlive() || !_bot->IsInWorld())
        return false;

    // While a near/far teleport is in flight the core waits for the ack our
    // session pump synthesizes; moving now would fight the relocation.
    if (_bot->IsBeingTeleported())
        return false;

    // Rooted, stunned, feared, confused, charmed, on a vehicle: a real client
    // would not be in control, so neither are we.
    if (_bot->HasUnitState(UNIT_STATE_NOT_MOVE) ||
        _bot->HasUnitState(UNIT_STATE_LOST_CONTROL) ||
        _bot->HasUnitState(UNIT_STATE_FLEEING) ||
        _bot->HasUnitState(UNIT_STATE_CONFUSED) ||
        _bot->IsCharmed() ||
        _bot->GetVehicle())
        return false;

    return true;
}

float BotMovement::SpeedFor(bool swimming) const
{
    return swimming ? _bot->GetSpeed(MOVE_SWIM) : _bot->GetSpeed(MOVE_RUN);
}

Unit* BotMovement::ResolveUnit(ObjectGuid guid) const
{
    if (guid.IsEmpty())
        return nullptr;
    return ObjectAccessor::GetUnit(*_bot, guid);
}

// ---------------------------------------------------------------------------
// goals
// ---------------------------------------------------------------------------

void BotMovement::MoveTo(float x, float y, float z, float stopDistance, bool faceGoal)
{
    _mode = BotMoveMode::Point;
    _goalUnit.Clear();
    _goalPoint.Relocate(x, y, z);
    _stopDistance = std::max(0.0f, stopDistance);
    _faceGoal = faceGoal;
    _maxDistance = 0.0f;
    _followDistance = 0.0f;
    _wantedFacing.reset();
}

void BotMovement::Chase(Unit* target, float stopDistance, float maxDistance)
{
    if (!target)
    {
        Stop();
        return;
    }
    _mode = BotMoveMode::Chase;
    _goalUnit = target->GetGUID();
    _goalPoint.Relocate(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    _stopDistance = std::max(0.0f, stopDistance);
    _maxDistance = maxDistance;
    _faceGoal = true;
    _followDistance = 0.0f;
    _wantedFacing.reset();
}

void BotMovement::Follow(Unit* target, float distance, float angleOffset)
{
    if (!target)
    {
        Stop();
        return;
    }
    _mode = BotMoveMode::Follow;
    _goalUnit = target->GetGUID();
    _goalPoint.Relocate(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    _stopDistance = std::max(0.5f, distance);
    _followDistance = _stopDistance;
    _followAngleOffset = angleOffset;
    _maxDistance = 0.0f;
    _faceGoal = false;
    _wantedFacing.reset();
}

void BotMovement::Stop()
{
    _mode = BotMoveMode::None;
    _goalUnit.Clear();
    _stopDistance = 0.0f;
    _followDistance = 0.0f;
    _faceGoal = false;
    _wantedFacing.reset();
    _moveBudgetMs = 0;
    InvalidatePath();
}

bool BotMovement::HasLiveGoal() const
{
    return _mode != BotMoveMode::None;
}

bool BotMovement::GetGoalPosition(float& x, float& y, float& z) const
{
    if (_mode == BotMoveMode::None)
        return false;
    if (!_goalUnit.IsEmpty())
        if (Unit* unit = ResolveUnit(_goalUnit))
        {
            x = unit->GetPositionX();
            y = unit->GetPositionY();
            z = unit->GetPositionZ();
            // Follow: aim for a formation slot behind the leader instead of the
            // leader's exact position, so several bots spread into an arc rather
            // than stacking on one tile.
            if (_mode == BotMoveMode::Follow && _followDistance > 0.0f)
            {
                float const slot = unit->GetOrientation() + float(M_PI) + _followAngleOffset;
                x += std::cos(slot) * _followDistance;
                y += std::sin(slot) * _followDistance;
            }
            return true;
        }
    x = _goalPoint.GetPositionX();
    y = _goalPoint.GetPositionY();
    z = _goalPoint.GetPositionZ();
    return true;
}

bool BotMovement::Arrived() const
{
    if (_mode == BotMoveMode::None)
        return true;

    float gx, gy, gz;
    if (!GetGoalPosition(gx, gy, gz))
        return true;

    float dx = _bot->GetPositionX() - gx;
    float dy = _bot->GetPositionY() - gy;
    float dz = _bot->GetPositionZ() - gz;
    return (dx * dx + dy * dy + dz * dz) <= (_stopDistance * _stopDistance + BOT_ARRIVAL_EPS);
}

void BotMovement::Face(Unit* target)
{
    if (!target)
        return;
    _wantedFacing = _bot->GetAbsoluteAngle(target);
}

void BotMovement::Face(float orientation)
{
    _wantedFacing = orientation;
}

void BotMovement::Face(float x, float y)
{
    _wantedFacing = AngleToTarget(_bot->GetPositionX(), _bot->GetPositionY(), x, y);
}

float BotMovement::AngleToTarget(float fromX, float fromY, float toX, float toY)
{
    return std::atan2(toY - fromY, toX - fromX);
}

void BotMovement::HaltExternal()
{
    _waitingForTeleport = true;
    InvalidatePath();
}

// ---------------------------------------------------------------------------
// packets - the "client" side
// ---------------------------------------------------------------------------

bool BotMovement::SendMovementPacket(uint32 opcode, bool forward, float x, float y, float z, float orientation, bool swimming)
{
    WorldSession* session = _bot->GetSession();
    if (!session || !_bot->IsInWorld() || _bot->IsBeingTeleported())
        return false;

    // The movement handler ignores packets while a server-side spline runs
    // (charges, knockbacks, core-initiated jumps). Finalize it and skip this
    // packet; the next tick drives the player again from the settled position.
    if (!_bot->movespline->Finalized())
    {
        _bot->StopMoving();
        return false;
    }

    WorldPacket packet(opcode);
    packet << _bot->GetGUID();                                  // packed mover guid
    packet << uint32((forward ? MOVEMENTFLAG_FORWARD : MOVEMENTFLAG_NONE) |
        (swimming ? MOVEMENTFLAG_SWIMMING : MOVEMENTFLAG_NONE));
    packet << uint32(0);                                        // flags2
    packet << uint32(0);                                        // flags3
    packet << uint32(GameTime::GetGameTimeMS());
    packet << Position(x, y, z, orientation).PositionXYZOStream();
    packet << float(0.0f);                                      // pitch
    packet << float(0.0f);                                      // stepUpStartElevation
    packet << uint32(0);                                        // removeMovementForcesCount
    packet << uint32(0);                                        // moveIndex
    for (uint32 i = 0; i < 8; ++i)                              // hasTransport/fall/inertia/... - none
        packet.WriteBit(0);
    packet.FlushBits();

    session->QueuePacket(new WorldPacket(std::move(packet)));

    _lastSentOrientation = orientation;
    return true;
}

bool BotMovement::SendStart(float x, float y, float z, float orientation, bool swimming)
{
    // a resting bot stands up before it walks (the client does the same)
    if (_bot->IsSitState())
        _bot->SetStandState(UNIT_STAND_STATE_STAND);

    bool const sent = SendMovementPacket(uint32(CMSG_MOVE_START_FORWARD), true, x, y, z, orientation, swimming);
    _moving = true;
    _heartbeatCooldown = BOT_HEARTBEAT_MS;
    return sent;
}

bool BotMovement::SendHeartbeat(float x, float y, float z, float orientation, bool swimming)
{
    bool const sent = SendMovementPacket(uint32(CMSG_MOVE_HEARTBEAT), true, x, y, z, orientation, swimming);
    _moving = true;
    return sent;
}

void BotMovement::SendStop(float x, float y, float z, float orientation, bool swimming)
{
    SendMovementPacket(uint32(CMSG_MOVE_STOP), false, x, y, z, orientation, swimming);
    _moving = false;
}

// ---------------------------------------------------------------------------
// pathing
// ---------------------------------------------------------------------------

void BotMovement::InvalidatePath()
{
    _path.clear();
    _pathIndex = 0;
    _pathAge = 0;
}

bool BotMovement::EnsurePath(float gx, float gy, float gz, bool forceRebuild)
{
    if (!_path.empty())
    {
        bool stale = false;
        // goal moved away from where the path points?
        G3D::Vector3 const& end = _path.back();
        float dx = end.x - gx, dy = end.y - gy, dz = end.z - gz;
        if (dx * dx + dy * dy + dz * dz > BOT_REPATH_DRIFT * BOT_REPATH_DRIFT)
            stale = true;
        if (_pathAge > BOT_PATH_MAX_AGE_MS)
            stale = true;
        if (_pathIndex >= _path.size())
            stale = true;

        if (!stale && !forceRebuild)
            return true;
        if (_repathCooldown > 0 && !forceRebuild)
            return true;    // keep the current path until the throttle expires
    }

    if (_repathCooldown > 0 && !forceRebuild)
        return !_path.empty();

    _repathCooldown = BOT_REPATH_MS;

    PathGenerator path(_bot);
    path.SetUseStraightPath(false);
    if (!path.CalculatePath(gx, gy, gz, true))
    {
        // mmaps missing or unreachable: fall back to a straight-line glide so
        // an open-field chase still works on any setup.
        _path.clear();
        _path.push_back(G3D::Vector3(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ()));
        _path.push_back(G3D::Vector3(gx, gy, gz));
        _pathIndex = 1;
        _pathAge = 0;
        _pathGoal.Relocate(gx, gy, gz);
        return true;
    }

    Movement::PointsArray const& points = path.GetPath();
    if (points.size() < 2)
    {
        _path.clear();
        _path.push_back(G3D::Vector3(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ()));
        _path.push_back(G3D::Vector3(gx, gy, gz));
    }
    else
        _path = points;

    _pathIndex = 1;
    _pathAge = 0;
    _pathGoal.Relocate(gx, gy, gz);
    return true;
}

bool BotMovement::AdvanceAlongPath(float step, float& nx, float& ny, float& nz, float& no)
{
    if (step <= 0.0f)
        step = 0.01f;

    float px = _bot->GetPositionX();
    float py = _bot->GetPositionY();
    float pz = _bot->GetPositionZ();

    float budget = step;
    bool advanced = false;
    bool finished = false;
    float lastHeading = _bot->GetOrientation();

    while (budget > 0.0f)
    {
        if (_pathIndex >= _path.size())
        {
            finished = true;
            break;
        }

        G3D::Vector3 const& wp = _path[_pathIndex];
        float dx = wp.x - px;
        float dy = wp.y - py;
        float dz = wp.z - pz;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (dist < BOT_ARRIVAL_EPS)
        {
            ++_pathIndex;
            continue;
        }

        lastHeading = AngleTo(px, py, wp.x, wp.y);

        if (dist <= budget)
        {
            px = wp.x;
            py = wp.y;
            pz = wp.z;
            budget -= dist;
            ++_pathIndex;
            advanced = true;
            if (_pathIndex >= _path.size())
            {
                finished = true;
                break;
            }
        }
        else
        {
            float const t = budget / dist;
            px += dx * t;
            py += dy * t;
            pz += dz * t;
            budget = 0.0f;
            advanced = true;
            finished = true;    // ran out of budget mid-segment
        }
    }

    nx = px;
    ny = py;
    nz = pz;
    no = lastHeading;
    return !finished || !advanced;
}

// Never report a position below the map floor: the movement handler kills
// players it sees under GetMinHeight (DAMAGE_FALL_TO_VOID). This only matters
// for the straight-line fallback over rough terrain, but the failure mode is
// a dead bot, so it is guarded unconditionally.
void BotMovement::ClampToMap(float& x, float& y, float& z) const
{
    float const minHeight = _bot->GetMap()->GetMinHeight(_bot->GetPhaseShift(), x, y);
    if (z < minHeight + 1.0f)
        z = minHeight + 1.0f;
}

// ---------------------------------------------------------------------------
// per-tick update
// ---------------------------------------------------------------------------

void BotMovement::Update(uint32 diff)
{
    if (_repathCooldown > diff)
        _repathCooldown -= diff;
    else
        _repathCooldown = 0;
    if (_heartbeatCooldown > diff)
        _heartbeatCooldown -= diff;
    else
        _heartbeatCooldown = 0;
    if (_turnCooldown > diff)
        _turnCooldown -= diff;
    else
        _turnCooldown = 0;
    if (_pathAge + diff > _pathAge)
        _pathAge += diff;

    // Right after login or a map change the core waits for a worldport ack;
    // the manager pump sends it. Do not fight that relocation.
    if (_bot->IsBeingTeleported())
    {
        if (!_waitingForTeleport)
            HaltExternal();
        return;
    }
    _waitingForTeleport = false;

    if (!AllowedToMove())
    {
        if (_moving)
        {
            bool swimming = _bot->IsInWater();
            SendStop(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ(), _bot->GetOrientation(), swimming);
        }
        InvalidatePath();
        _wantedFacing.reset();
        return;
    }

    // No goal? Idle: apply any requested in-place facing.
    if (_mode == BotMoveMode::None)
    {
        if (_moving)
        {
            SendStop(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ(), _bot->GetOrientation(), _bot->IsInWater());
            InvalidatePath();
        }

        if (_wantedFacing && _turnCooldown == 0)
        {
            float delta = std::fabs(AngleDelta(_bot->GetOrientation(), *_wantedFacing));
            if (delta > TURN_EPSILON)
            {
                _turnCooldown = BOT_TURN_MS;
                // A standing player reports its new orientation with a plain
                // heartbeat: HandleMovementOpcode -> UpdatePosition ->
                // UpdateOrientation sets the server orientation and the
                // MoveUpdate broadcast tells every observer. This is the only
                // facing that "sticks" for a player, because there is no
                // client to receive SMSG_MOVE_SET_FACING.
                SendMovementPacket(uint32(CMSG_MOVE_HEARTBEAT), false,
                    _bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ(),
                    *_wantedFacing, _bot->IsInWater());
            }
            else
                _wantedFacing.reset();
        }
        return;
    }

    // Resolve the goal.
    float gx, gy, gz;
    if (!GetGoalPosition(gx, gy, gz))
    {
        Stop();
        return;
    }

    // Chase/Follow: the goal moves with its unit.
    if (_mode == BotMoveMode::Chase || _mode == BotMoveMode::Follow)
    {
        if (Unit* goal = ResolveUnit(_goalUnit))
        {
            if (!goal->IsInWorld() || goal->GetMapId() != _bot->GetMapId())
            {
                Stop();
                return;
            }
            // Follow keeps its distance; chase wants to be inside stopDistance.
            // Measure against the formation slot (gx,gy) rather than the leader,
            // so an offset follower settles into its slot instead of piling onto
            // the leader's tile.
            if (_mode == BotMoveMode::Follow)
            {
                float dx = _bot->GetPositionX() - gx;
                float dy = _bot->GetPositionY() - gy;
                float planar = std::sqrt(dx * dx + dy * dy);
                if (planar <= 1.5f)
                {
                    if (_moving)
                        SendStop(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ(), _bot->GetOrientation(), _bot->IsInWater());
                    InvalidatePath();
                    _wantedFacing = _bot->GetAbsoluteAngle(goal);
                    return;
                }
            }
        }
        else
        {
            Stop();
            return;
        }
    }

    // Reached?
    if (Arrived())
    {
        if (_moving)
            SendStop(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ(), _bot->GetOrientation(), _bot->IsInWater());

        // Hand over to the idle state (mode None) so a requested facing is
        // actually applied by the turn packet - an Arrived() branch that
        // keeps the goal alive would spin here forever and the orientation
        // would never be sent (exactly the "facing the wrong way" stall the
        // old system produced).
        bool const face = _faceGoal;
        float const fx = gx, fy = gy;
        Unit* goalUnit = ResolveUnit(_goalUnit);
        Stop();
        if (face)
        {
            if (goalUnit)
                _wantedFacing = _bot->GetAbsoluteAngle(goalUnit);
            else if (!_goalUnit.IsEmpty())
                _wantedFacing.reset();      // goal unit is gone, nothing to face
            else
                _wantedFacing = AngleTo(_bot->GetPositionX(), _bot->GetPositionY(), fx, fy);
        }
        if (sBotConfig->DebugMove)
            TC_LOG_DEBUG("playerbot", "[move] {} arrived at goal", _bot->GetName());
        return;
    }

    // Walk. Position reports happen on the heartbeat cadence; the step for
    // each report is computed from ALL time accumulated since the last report
    // (a real client moves continuously between its movement packets). The
    // budget is capped so a single report can never jump more than a few
    // heartbeats' worth of distance.
    bool forceRebuild = _path.empty();
    EnsurePath(gx, gy, gz, forceRebuild);
    if (_path.empty())
        return;

    _moveBudgetMs = std::min(_moveBudgetMs + diff, 4u * BOT_HEARTBEAT_MS);
    if (!_moving || _heartbeatCooldown == 0)
    {
        float nx, ny, nz, no;
        AdvanceAlongPath(SpeedFor(_bot->IsInWater()) * (_moveBudgetMs / 1000.0f), nx, ny, nz, no);
        ClampToMap(nx, ny, nz);

        bool swimming = _bot->IsInWater();
        bool sent = false;
        if (!_moving)
            sent = SendStart(nx, ny, nz, no, swimming);
        else
        {
            _heartbeatCooldown = BOT_HEARTBEAT_MS;
            sent = SendHeartbeat(nx, ny, nz, no, swimming);
        }

        // budget spent only if the position actually got reported
        if (sent)
            _moveBudgetMs = 0;

        if (sBotConfig->DebugMove)
            TC_LOG_DEBUG("playerbot", "[move] {} -> {} {} {} (mode {}, wp {}/{})",
                _bot->GetName(), nx, ny, nz, uint32(_mode), _pathIndex, _path.size());
    }
}
