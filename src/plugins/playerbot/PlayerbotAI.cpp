#include "../pchdef.h"
#include "PlayerbotMgr.h"
#include "playerbot.h"

#include "AiFactory.h"

#include "Grids/Notifiers/GridNotifiers.h"
#include "Grids/Notifiers/GridNotifiersImpl.h"
#include "Grids/Cells/CellImpl.h"
#include "strategy/values/LastMovementValue.h"
#include "strategy/actions/LogLevelAction.h"
#include "strategy/values/LastSpellCastValue.h"
#include "LootObjectStack.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotAI.h"
#include "PlayerbotPackets.h"
#include <ctime>
#include "PlayerbotFactory.h"
#include "PlayerbotSecurity.h"
#include "Groups/Group.h"
#include "Entities/Pet/Pet.h"
#include "Spells/Auras/SpellAuraEffects.h"
#include "CombatDiag.h"
#include "Movement/MotionMaster.h"
#include "Movement/Spline/MoveSpline.h"
#include <atomic>
#include <cmath>
#include "StringFormat.h"
#include "Time/GameTime.h"

using namespace ai;
using namespace std;

vector<string>& split(const string &s, char delim, vector<string> &elems);
vector<string> split(const string &s, char delim);
std::string &trim(std::string &s);

uint32 PlayerbotChatHandler::extractQuestId(string str)
{
    char* source = (char*)str.c_str();
    char* cId = extractKeyFromLink(source,"Hquest");
    return cId ? atol(cId) : 0;
}

void PacketHandlingHelper::AddHandler(uint16 opcode, string handler)
{
    handlers[opcode] = handler;
}

void PacketHandlingHelper::Handle(ExternalEventHelper &helper)
{
    // Other map workers may broadcast group packets to this bot concurrently.
    // Detach/dequeue under the lock, but never hold it while dispatching events.
    for (uint32 processed = 0; processed < 256; ++processed)
    {
        WorldPacket packet;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (queue.empty())
                break;
            packet = std::move(queue.front());
            queue.pop_front();
        }
        helper.HandlePacket(handlers, packet);
    }
}

void PacketHandlingHelper::AddPacket(const WorldPacket& packet)
{
    if (handlers.find(packet.GetOpcode()) == handlers.end())
        return;
    if (packet.size() > 1024 * 1024)
    {
        TC_LOG_WARN("playerbot", "Ignoring oversized bot event packet {} ({} bytes)", packet.GetOpcode(), packet.size());
        return;
    }
    std::lock_guard<std::mutex> lock(queueMutex);
    if (queue.size() >= 256)
    {
        TC_LOG_WARN("playerbot", "Bot packet event queue full; discarding oldest event");
        queue.pop_front();
    }
    queue.push_back(packet);
    queue.back().rpos(0);
    queue.back().ResetBitPos();
}


PlayerbotAI::PlayerbotAI() : PlayerbotAIBase(), bot(NULL), aiObjectContext(NULL),
    currentEngine(NULL), chatHelper(this), chatFilter(this), accountId(0), security(NULL), master(NULL)
{
    for (int i = 0 ; i < BOT_STATE_MAX; i++)
        engines[i] = NULL;
}

PlayerbotAI::PlayerbotAI(Player* bot) :
    PlayerbotAIBase(), chatHelper(this), chatFilter(this), security(bot), master(NULL)
{
	this->bot = bot;

	accountId = sCharacterCache->GetCharacterAccountIdByGuid(bot->GetGUID());

    aiObjectContext = AiFactory::createAiObjectContext(bot, this);

    engines[BOT_STATE_COMBAT] = AiFactory::createCombatEngine(bot, this, aiObjectContext);
    engines[BOT_STATE_NON_COMBAT] = AiFactory::createNonCombatEngine(bot, this, aiObjectContext);
    engines[BOT_STATE_DEAD] = AiFactory::createDeadEngine(bot, this, aiObjectContext);
    currentEngine = engines[BOT_STATE_NON_COMBAT];
    currentState = BOT_STATE_NON_COMBAT;

    masterIncomingPacketHandlers.AddHandler(CMSG_GAME_OBJ_REPORT_USE, "use game object");
    masterIncomingPacketHandlers.AddHandler(CMSG_AREA_TRIGGER, "area trigger");
    masterIncomingPacketHandlers.AddHandler(CMSG_GAME_OBJ_USE, "use game object");
    masterIncomingPacketHandlers.AddHandler(CMSG_LOOT_ROLL, "loot roll");
    masterIncomingPacketHandlers.AddHandler(CMSG_TALK_TO_GOSSIP, "gossip hello");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUEST_GIVER_HELLO, "gossip hello");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUEST_GIVER_COMPLETE_QUEST, "complete quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUEST_GIVER_ACCEPT_QUEST, "accept quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_ACTIVATE_TAXI, "activate taxi");
    masterIncomingPacketHandlers.AddHandler(CMSG_MOVE_SPLINE_DONE, "taxi done");
    masterIncomingPacketHandlers.AddHandler(CMSG_PARTY_UNINVITE, "uninvite");
    masterIncomingPacketHandlers.AddHandler(CMSG_PUSH_QUEST_TO_PARTY, "quest share");
    masterIncomingPacketHandlers.AddHandler(CMSG_GUILD_INVITE_BY_NAME, "guild invite");
    masterIncomingPacketHandlers.AddHandler(CMSG_DF_TELEPORT, "lfg teleport");

    botOutgoingPacketHandlers.AddHandler(SMSG_PARTY_INVITE, "group invite");
    botOutgoingPacketHandlers.AddHandler(SMSG_BUY_FAILED, "buy failed");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_NEW_LEADER, "group set leader");
    botOutgoingPacketHandlers.AddHandler(SMSG_MOVE_UPDATE_RUN_SPEED, "check mount state");
    botOutgoingPacketHandlers.AddHandler(SMSG_RESURRECT_REQUEST, "resurrect request");
    botOutgoingPacketHandlers.AddHandler(SMSG_INVENTORY_CHANGE_FAILURE, "cannot equip");
    botOutgoingPacketHandlers.AddHandler(SMSG_TRADE_STATUS, "trade status");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOOT_RESPONSE, "loot response");
    botOutgoingPacketHandlers.AddHandler(SMSG_QUEST_UPDATE_ADD_CREDIT, "quest objective completed");
    botOutgoingPacketHandlers.AddHandler(SMSG_ITEM_PUSH_RESULT, "item push result");
    botOutgoingPacketHandlers.AddHandler(SMSG_PARTY_COMMAND_RESULT, "party command");
    botOutgoingPacketHandlers.AddHandler(SMSG_CAST_FAILED, "cast failed");
    botOutgoingPacketHandlers.AddHandler(SMSG_DUEL_REQUESTED, "duel requested");
    botOutgoingPacketHandlers.AddHandler(SMSG_ROLE_CHOSEN, "lfg role check");
    botOutgoingPacketHandlers.AddHandler(SMSG_LFG_PROPOSAL_UPDATE, "lfg proposal");

    masterOutgoingPacketHandlers.AddHandler(SMSG_PARTY_COMMAND_RESULT, "party command");
    botOutgoingPacketHandlers.AddHandler(SMSG_READY_CHECK_STARTED, "ready check");
    botOutgoingPacketHandlers.AddHandler(SMSG_READY_CHECK_COMPLETED, "ready check finished");
}

PlayerbotAI::~PlayerbotAI()
{
    for (int i = 0 ; i < BOT_STATE_MAX; i++)
    {
        if (engines[i])
            delete engines[i];
    }

    if (aiObjectContext)
        delete aiObjectContext;
}

void PlayerbotAI::UpdateAI(uint32 elapsed)
{
    // Before anything below can bail out: a bot frozen by a stuck teleport or a
    // stale cast is exactly what needs reporting, and the watchdog has to see the
    // same elapsed time the AI is throttled by.
    UpdateCombatDiagnostics(elapsed);

    if (bot->IsBeingTeleported())
        return;

    if (nextAICheckDelay > sPlayerbotAIConfig.globalCoolDown &&
            bot->IsNonMeleeSpellCast(true, true, false) &&
            *GetAiObjectContext()->GetValue<bool>("invalid target", "current target"))
    {
        Spell* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (spell && !spell->GetSpellInfo()->IsPositive())
        {
            InterruptSpell();
            SetNextCheckDelay(sPlayerbotAIConfig.globalCoolDown);
        }
    }

    if (nextAICheckDelay > sPlayerbotAIConfig.maxWaitForMove && bot->IsInCombat() && !bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        nextAICheckDelay = sPlayerbotAIConfig.maxWaitForMove;
    }

    PlayerbotAIBase::UpdateAI(elapsed);
}

// --------------------------------------------------------------------------------
// Combat diagnostics
//
// "The bots won't attack" is never one bug: a bot needs a target, an attack state,
// a movement generator that actually relocates it, the core's 3D swing-range test,
// a 120 degree facing arc and an expired swing timer, and every one of those is
// owned by a different subsystem. This block reads all of them once per tick, and
// when a bot stops making progress it says which one is holding the bot back
// instead of letting the bot stand in its attack animation forever.
// --------------------------------------------------------------------------------

static char const* BotSwingErrorText(Player* bot)
{
    // SetAttackSwingError() normally only reaches a client (SMSG_ATTACK_SWING_ERROR);
    // a bot session has none, so the reason the core refused the swing is otherwise
    // invisible. Exposed through Player::GetAttackSwingError() for exactly this.
    Optional<AttackSwingErr> err = bot->GetAttackSwingError();
    if (!err)
        return "";

    switch (*err)
    {
        case AttackSwingErr::NotInRange: return "NotInRange";
        case AttackSwingErr::BadFacing:  return "BadFacing";
        case AttackSwingErr::CantAttack: return "CantAttack";
        case AttackSwingErr::DeadTarget: return "DeadTarget";
    }
    return "";
}

static bool IsBotLocomotionGenerator(MovementGeneratorType type)
{
    return type != IDLE_MOTION_TYPE && type != EFFECT_MOTION_TYPE;
}

// Mirrors MovementAction::IsMovingAllowed(): "not moving because I may not move"
// and "not moving because my chase is going nowhere" need different fixes, so the
// snapshot keeps them apart.
static bool BotMayStartMoving(Player* bot)
{
    if (bot->IsFrozen() || bot->IsPolymorphed() ||
            (bot->isDead() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST)) ||
            bot->IsBeingTeleported() ||
            bot->HasUnitState(UNIT_STATE_ROOT) ||
            bot->HasAuraType(SPELL_AURA_MOD_CONFUSE) || bot->IsCharmed() ||
            bot->HasAuraType(SPELL_AURA_MOD_STUN) || bot->IsFlying())
        return false;

    return bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE;
}

static uint32 BotBlockingSpellId(Player* bot)
{
    // CURRENT_MELEE_SPELL (slot 0) is overloaded by the core as the
    // "next auto-swing" / queued-melee-spell holder. It is populated even
    // when the bot has no active cast and is just standing in its attack
    // animation, which used to make the snapshot claim spell 5019
    // (a warlock auto-swing entry) was blocking movement even though
    // castBlocksMove=0. Skip that slot; everything else is a real cast.
    for (uint8_t type = CURRENT_GENERIC_SPELL; type < CURRENT_MAX_SPELL; ++type)
        if (Spell* spell = bot->GetCurrentSpell(static_cast<CurrentSpellTypes>(type)))
            return spell->GetSpellInfo()->Id;

    return 0;
}

CombatSnapshot PlayerbotAI::CaptureCombatSnapshot()
{
    CombatSnapshot snap;
    if (!bot || !bot->IsInWorld())
        return snap;

    Unit* target = aiObjectContext ? aiObjectContext->GetValue<Unit*>("current target")->Get() : nullptr;
    if (!target)
        target = bot->GetVictim();

    snap.alive = bot->IsAlive();
    snap.hasTarget = target != nullptr;
    snap.targetAlive = target && target->IsAlive();

    snap.attackState = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
    snap.swingReady = bot->isAttackReady(BASE_ATTACK);
    snap.swingError = BotSwingErrorText(bot);
    snap.mounted = bot->IsMounted();
    snap.flying = bot->IsFlying();
    snap.pacified = bot->HasUnitFlag(UNIT_FLAG_PACIFIED);
    snap.disarmed = bot->HasUnitFlag(UNIT_FLAG_DISARMED);
    snap.hasMeleeWeapon = bot->GetWeaponForAttack(BASE_ATTACK, true) != nullptr;
    // A bot whose primary damage comes from a ranged attack (wand, bow, gun,
    // or a pure-spell caster with no main-hand melee intent) must not be
    // held to the melee "inMeleeRange && swingReady" gate below: when a
    // caster is at 13 yards the core's melee swing correctly returns
    // NotInRange, but that is not a stall.
    snap.hasRangedAttack = bot->GetWeaponForAttack(RANGED_ATTACK, true) != nullptr
        || (bot->GetClass() == CLASS_WARLOCK) || (bot->GetClass() == CLASS_MAGE)
        || (bot->GetClass() == CLASS_PRIEST)
        || (bot->GetClass() == CLASS_DRUID && !HasAnyAuraOf(bot, "cat form", "bear form", "dire bear form", nullptr))
        || (bot->GetClass() == CLASS_HUNTER);
    snap.movementDisabled = bot->HasUnitState(UNIT_STATE_NOT_MOVE)
            || bot->HasAuraType(SPELL_AURA_MOD_CONFUSE) || bot->IsCharmed();
    snap.casting = bot->HasUnitState(UNIT_STATE_CASTING);
    snap.movementBlockedByCast = bot->IsMovementPreventedByCasting();
    snap.blockingSpell = BotBlockingSpellId(bot);
    snap.teleported = bot->IsBeingTeleported();
    snap.runSpeed = bot->GetSpeed(MOVE_RUN);
    snap.movingNow = !bot->movespline->Finalized()
            || IsBotLocomotionGenerator(bot->GetMotionMaster()->GetCurrentMovementGeneratorType());
    snap.canMoveNow = BotMayStartMoving(bot);
    snap.stallMs = combatStallMs;

    if (currentEngine)
        snap.trace = currentEngine->GetLastAction();

    if (target)
    {
        // Centre-to-centre 3D distance, exactly like the core's own gates
        // (IsWithinMeleeRangeAt tests GetExactDist against GetMeleeRange,
        // Spell::CheckRange tests GetExactDist against the spell range). The
        // old GetDistance() here subtracts both combat reaches, which printed
        // lines like "3D 3.69 yd vs 5.00 yd ... inRange=0" - two numbers in
        // different metrics that made the report contradict itself.
        snap.distance3d = bot->GetExactDist(target);
        // absolute: a bot standing above its target is the same trap as one below
        snap.zGap = std::fabs(bot->GetPositionZ() - target->GetPositionZ());
        snap.meleeRange = bot->GetMeleeRange(target);
        // Not IsWithinDist2d/GetDistance2d: this is the exact call the core makes
        // before it lets a swing land, so the bot and the core can never disagree
        // about "in range" again.
        snap.inMeleeRange = bot->IsWithinMeleeRange(target);
        snap.sameMap = bot->IsInMap(target);
        snap.hasLOS = bot->IsWithinLOSInMap(target);
        snap.inArc = bot->HasInArc(2.0f * static_cast<float>(M_PI) / 3.0f, target);

        // Ranged / caster reach envelope: same logic ReachSpellAction uses, but
        // with the true spell cap so the stall detector can tell "happily casting
        // from 13 yd" apart from "standing still and not doing anything at 13 yd".
        snap.spellRange = sPlayerbotAIConfig.spellDistance;
        // Hunters/wand users auto-shoot from 30+ yards; give casters the same
        // benefit so a warlock who is out of Shadow Bolt range (30 yd) is reported
        // as needing a *spell* approach, not as a melee stall.
        if (snap.hasRangedAttack)
        {
            // Hunters/wand users auto-shoot from 30+ yards; pure casters with
            // Shadow Bolt / Frostbolt / Wrath also top out at 30-40 yards. 30 yd
            // is the Wrath cap for wand/bow/gun and most nukes; using that as the
            // "in spell range" envelope means a warlock at 13 yd is no longer
            // flagged as "out of swing range" by the melee stall detector, and a
            // warlock at 35 yd is correctly reported as needing a spell approach.
            snap.spellRange = 30.0f;
        }
        snap.inSpellRange = snap.distance3d <= snap.spellRange;
    }

    return snap;
}

void PlayerbotAI::UpdateCombatDiagnostics(uint32 elapsed)
{
    if (sPlayerbotAIConfig.debugCombat == 0 || !bot || !aiObjectContext)
        return;

    if (combatReportCooldownMs > elapsed)
        combatReportCooldownMs -= elapsed;
    else
        combatReportCooldownMs = 0;

    Unit* target = aiObjectContext->GetValue<Unit*>("current target")->Get();
    if (!target)
        target = bot->GetVictim();

    if (!target || !target->IsAlive() || !bot->IsAlive() || bot->IsBeingTeleported() || !bot->IsInMap(target))
    {
        combatStallMs = 0;
        combatBestDistance = 0.0f;
        combatBestHealth = 0;
        combatWatchVictim.Clear();
        return;
    }

    float const distance = bot->GetDistance(target);
    uint32 const health = target->GetHealth();

    if (combatWatchVictim != target->GetGUID())
    {
        combatWatchVictim = target->GetGUID();
        combatBestDistance = distance;
        combatBestHealth = health;
        combatStallMs = 0;
        return;
    }

    // Progress is either closing the gap or hurting the victim. Anything else that
    // looked like fighting - circling, a spline that goes nowhere - is a stall.
    if (distance <= combatBestDistance - 0.75f || health < combatBestHealth)
    {
        combatBestDistance = distance;
        combatBestHealth = health;
        combatStallMs = 0;
        return;
    }

    combatStallMs += elapsed;

    if (sPlayerbotAIConfig.debugCombat >= 2)
    {
        if (combatDebugTickMs > 1000)
        {
            combatDebugTickMs = 0;
            TC_LOG_DEBUG("playerbot", "Combat state of {}: {}", bot->GetName(),
                    FormatCombatSnapshot(CaptureCombatSnapshot()));
        }
        else
            combatDebugTickMs += elapsed;
    }

    CombatSnapshot snap = CaptureCombatSnapshot();
    CombatStall stall = EvaluateCombatStall(snap);
    if (!stall.report)
        return;

    // One word per bot per minute at most...
    combatReportCooldownMs = 60 * 1000;

    // ...and one console line per fifteen seconds for the whole bot population.
    // Without the second limit a realm with two hundred confused bots replaces the
    // log nobody could read with a longer one.
    static std::atomic<time_t> lastReportTime{ 0 };
    static std::atomic<uint32> suppressed{ 0 };

    time_t const now = GameTime::GetGameTime();
    if (now - lastReportTime.load() >= 15)
    {
        lastReportTime = now;
        uint32 const dropped = suppressed.exchange(0);

        TC_LOG_ERROR("playerbot", "Bot {} is locked onto {} and not fighting: {} | {}{}",
                bot->GetName(), target->GetName(), stall.reason, FormatCombatSnapshot(snap),
                dropped ? Trinity::StringFormat(" (+{} further stall report(s) suppressed to keep the console readable)", dropped) : "");
    }
    else
        ++suppressed;

    // A player who ordered this fight deserves to hear why it is not happening;
    // random bots have no master and stay quiet.
    if (master && master->IsInWorld() && !master->GetPlayerbotAI())
    {
        ostringstream out;
        out << "I cannot fight " << target->GetName() << ": " << stall.reason;
        TellMaster(out);
    }

    switch (stall.action)
    {
        case CombatStallAction::ClearCast:
            // The blocking cast holds *both* the movement generator (see
            // Unit::IsMovementPreventedByCasting) and every swing (DoMeleeAttackIfReady
            // returns at its casting check). Nothing else clears a cast a client
            // would have cancelled, because there is no client.
            bot->CastStop();
            bot->InterruptSpell(CURRENT_MELEE_SPELL);
            InterruptSpell();
            break;
        case CombatStallAction::RetryMove:
            // Drop the wedged movement so the next tick starts from scratch; a stale
            // generator that is "moving" without relocating the bot is otherwise
            // permanent. Also wake the AI out of any WaitForReach()-imposed delay
            // that was set against the old spline: without this, the combat tick
            // would clear MM and StopMoving, then sit idle for seconds while
            // nextAICheckDelay counted down, so the *new* approach was never
            // actually issued - producing the exact "no generator and no live
            // spline" message that triggered this retry in the first place.
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
            break;
        case CombatStallAction::DropTarget:
            bot->AttackStop();
            aiObjectContext->GetValue<Unit*>("old target")->Set(target);
            aiObjectContext->GetValue<Unit*>("current target")->Set(nullptr);
            break;
        default:
            break;
    }

    combatStallMs = 0;
}

std::string PlayerbotAI::FormatCombatStatus()
{
    CombatSnapshot snap = CaptureCombatSnapshot();

    // The verdict in EvaluateCombatStall() is gated on a stall window so a normal
    // chase is never reported; an operator asking right now means "tell me now".
    snap.stallMs = 10000;

    CombatStall stall = EvaluateCombatStall(snap);

    ostringstream out;
    out << (stall.report ? stall.reason : "nothing is blocking this bot right now")
        << " | " << FormatCombatSnapshot(snap)
        << " | aiDelay=" << nextAICheckDelay
        << " ms, iterationsPerTick=" << sPlayerbotAIConfig.iterationsPerTick
        << " | melee=" << sPlayerbotAIConfig.meleeDistance
        << " contact=" << sPlayerbotAIConfig.contactDistance
        << " spell=" << sPlayerbotAIConfig.spellDistance
        << " react=" << sPlayerbotAIConfig.reactDistance
        << " sight=" << sPlayerbotAIConfig.sightDistance;

    if (currentEngine)
        out << " | strategies: " << currentEngine->ListStrategies();

    return out.str();
}

void PlayerbotAI::UpdateAIInternal(uint32 elapsed)
{
    ExternalEventHelper helper(aiObjectContext);
    while (!chatCommands.empty())
    {
        ChatCommandHolder holder = chatCommands.top();
        string command = holder.GetCommand();
        Player* owner = holder.GetOwner();
        if (!helper.ParseChatCommand(command, owner) && holder.GetType() == CHAT_MSG_WHISPER)
        {
            ostringstream out; out << "Unknown command " << command;
            TellMaster(out);
            helper.ParseChatCommand("help");
        }
        chatCommands.pop();
    }

    botOutgoingPacketHandlers.Handle(helper);
    masterIncomingPacketHandlers.Handle(helper);
    masterOutgoingPacketHandlers.Handle(helper);

	DoNextAction();
}

void PlayerbotAI::DropCommandsFrom(Player* player)
{
    if (!player || chatCommands.empty())
        return;

    // chatCommands is a stack; drain it, keep unrelated entries, rebuild it
    vector<ChatCommandHolder> keep;
    while (!chatCommands.empty())
    {
        ChatCommandHolder holder = chatCommands.top();
        chatCommands.pop();
        if (holder.GetOwner() != player)
            keep.push_back(holder);
    }

    for (vector<ChatCommandHolder>::reverse_iterator i = keep.rbegin(); i != keep.rend(); ++i)
        chatCommands.push(*i);
}

void PlayerbotAI::HandleTeleportAck()
{
	bot->GetMotionMaster()->Clear();
	if (bot->IsBeingTeleportedNear())
	{
        WorldPackets::Movement::MoveTeleportAck p{WorldPacket(CMSG_MOVE_TELEPORT_ACK)};
		p.MoverGUID = bot->GetGUID();
		bot->GetSession()->HandleMoveTeleportAck(p);
	}
	else if (bot->IsBeingTeleportedFar())
	{
		bot->GetSession()->HandleMoveWorldportAck();
		SetNextCheckDelay(1000);
	}
}

void PlayerbotAI::Reset()
{
    if (bot->IsFlying())
        return;

    currentEngine = engines[BOT_STATE_NON_COMBAT];
    nextAICheckDelay = 0;

    // Same stuck-swing class as drop target: clearing the target value
    // without AttackStop leaves the client's attack animation and the
    // server-side victim running. Safe no-op when idle.
    bot->AttackStop();

    aiObjectContext->GetValue<Unit*>("old target")->Set(NULL);
    aiObjectContext->GetValue<Unit*>("current target")->Set(NULL);
    aiObjectContext->GetValue<LootObject>("loot target")->Set(LootObject());
    aiObjectContext->GetValue<uint32>("lfg proposal")->Set(0);

    LastSpellCast & lastSpell = aiObjectContext->GetValue<LastSpellCast& >("last spell cast")->Get();
    lastSpell.Reset();

    LastMovement & lastMovement = aiObjectContext->GetValue<LastMovement& >("last movement")->Get();
    lastMovement.Set(NULL);

    // MotionMaster::Clear drops the generators but leaves a running spline
    // alive, so without StopMoving the bot glides on to its old destination
    // after every follow/stay/grind order. Stop it dead here.
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    bot->m_taxi.ClearTaxiDestinations();
    InterruptSpell();

    for (int i = 0 ; i < BOT_STATE_MAX; i++)
    {
        engines[i]->Init();
    }
}

void PlayerbotAI::HandleCommand(uint32 type, const string& text, Player& fromPlayer)
{
    if (!GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, type != CHAT_MSG_WHISPER, &fromPlayer))
        return;

    if (type == CHAT_MSG_ADDON)
        return;

    string filtered = text;
    if (!sPlayerbotAIConfig.commandPrefix.empty())
    {
        if (filtered.find(sPlayerbotAIConfig.commandPrefix) != 0)
            return;

        filtered = filtered.substr(sPlayerbotAIConfig.commandPrefix.size());
    }

    filtered = chatFilter.Filter(trim((string&)filtered));
    if (filtered.empty())
        return;

    if (filtered.find("who") != 0 && !GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_ALLOW_ALL, type != CHAT_MSG_WHISPER, &fromPlayer))
        return;

    if (type == CHAT_MSG_RAID_WARNING && filtered.find(bot->GetName()) != string::npos && filtered.find("award") == string::npos)
    {
        ChatCommandHolder cmd("warning", &fromPlayer, type);
        chatCommands.push(cmd);
        return;
    }

    if (filtered.size() > 2 && filtered.substr(0, 2) == "d " || filtered.size() > 3 && filtered.substr(0, 3) == "do ")
    {
        std::string action = filtered.substr(filtered.find(" ") + 1);
        DoSpecificAction(action);
    }
    else if (filtered == "reset")
    {
        Reset();
    }
    else
    {
        ChatCommandHolder cmd(filtered, &fromPlayer, type);
        chatCommands.push(cmd);
    }
}

void PlayerbotAI::HandleBotOutgoingPacket(const WorldPacket& packet)
{
    switch (packet.GetOpcode())
    {
    case SMSG_MOVE_SET_CAN_FLY:
    case SMSG_MOVE_UNSET_CAN_FLY:
        // Unit::SetCanFly has already updated authoritative movement flags.
        // CAN_FLY does not imply FLYING, and must not erase other movement state.
        return;
    case SMSG_CAST_FAILED:
    case SMSG_SPELL_FAILURE:
        {
            packets::CastFailure failure;
            if (!packets::ReadCastFailure(packet, failure))
                return;
            if (packet.GetOpcode() == SMSG_SPELL_FAILURE && failure.Caster != bot->GetGUID())
                return;
            if (packet.GetOpcode() == SMSG_SPELL_FAILURE || failure.Reason != SPELL_CAST_OK)
            {
                SpellInterrupted(failure.SpellID > 0 ? uint32(failure.SpellID) : 0);
                if (packet.GetOpcode() == SMSG_CAST_FAILED)
                    botOutgoingPacketHandlers.AddPacket(packet);
            }
            return;
        }
    case SMSG_SPELL_DELAYED:
        {
            WorldPacket p(packet);
            p.rpos(0);
            ObjectGuid casterGuid;
            p >> casterGuid;

            if (casterGuid != bot->GetGUID())
                return;

            uint32 delaytime;
            p >> delaytime;
            if (delaytime <= 1000)
                IncreaseNextCheckDelay(delaytime);
            return;
        }
    default:
        botOutgoingPacketHandlers.AddPacket(packet);
    }
}

void PlayerbotAI::SpellInterrupted(uint32 spellid)
{
    LastSpellCast& lastSpell = aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get();
    if (!spellid || lastSpell.id != spellid)
        return;

    time_t started = lastSpell.time;
    int32 cooldown = CalculateGlobalCooldown(lastSpell.id);
    lastSpell.Reset();

    time_t now = time(0);
    double elapsed = now > started ? std::difftime(now, started) * 1000.0 : 0.0;
    SetNextCheckDelay(cooldown > 0 && elapsed < cooldown ? uint32(cooldown - elapsed) : 0);
}

int32 PlayerbotAI::CalculateGlobalCooldown(uint32 spellid)
{
    if (!spellid)
        return 0;

    if (bot->GetSpellHistory()->HasCooldown(spellid))
        return sPlayerbotAIConfig.globalCoolDown;

    return sPlayerbotAIConfig.reactDelay;
}

void PlayerbotAI::HandleMasterIncomingPacket(const WorldPacket& packet)
{
    masterIncomingPacketHandlers.AddPacket(packet);
}

void PlayerbotAI::HandleMasterOutgoingPacket(const WorldPacket& packet)
{
    masterOutgoingPacketHandlers.AddPacket(packet);
}

void PlayerbotAI::ChangeEngine(BotState type)
{
    Engine* engine = engines[type];

    if (currentEngine != engine)
    {
        currentEngine = engine;
        currentState = type;
        ReInitCurrentEngine();

        switch (type)
        {
        case BOT_STATE_COMBAT:
            TC_LOG_DEBUG("playerbot",  "=== {} COMBAT ===", bot->GetName().c_str());
            break;
        case BOT_STATE_NON_COMBAT:
            TC_LOG_DEBUG("playerbot",  "=== {} NON-COMBAT ===", bot->GetName().c_str());
            break;
        case BOT_STATE_DEAD:
            TC_LOG_DEBUG("playerbot",  "=== {} DEAD ===", bot->GetName().c_str());
            break;
        }
    }
}

void PlayerbotAI::DoNextAction()
{
    if (bot->IsBeingTeleported() || (GetMaster() && GetMaster()->IsBeingTeleported()))
        return;

    currentEngine->DoNextAction(NULL);

    if (bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
    {
        bot->m_movementInfo.SetMovementFlags((MovementFlags)(MOVEMENTFLAG_FLYING|MOVEMENTFLAG_CAN_FLY));

        // TODO
        //WorldPacket packet(CMSG_MOVE_SET_FLY);
        //packet << bot->GetGUID();
        //packet << bot->m_movementInfo;
        
        //bot->GetSession()->HandleMovementOpcodes(packet);
    }

    Player* master = GetMaster();
    if (bot->IsMounted() && bot->IsFlying())
    {
        bot->m_movementInfo.SetMovementFlags((MovementFlags)(MOVEMENTFLAG_FLYING|MOVEMENTFLAG_CAN_FLY));

        bot->SetSpeed(MOVE_FLIGHT, 1.0f);
        bot->SetSpeed(MOVE_RUN, 1.0f);

        if (master)
        {
            bot->SetSpeed(MOVE_FLIGHT, master->GetSpeedRate(MOVE_FLIGHT));
            bot->SetSpeed(MOVE_RUN, master->GetSpeedRate(MOVE_FLIGHT));
        }

    }

    if (currentEngine != engines[BOT_STATE_DEAD] && !bot->IsAlive())
        ChangeEngine(BOT_STATE_DEAD);

    if (currentEngine == engines[BOT_STATE_DEAD] && bot->IsAlive())
        ChangeEngine(BOT_STATE_NON_COMBAT);

    Group *group = bot->GetGroup();
    if (!master && group)
    {
        for (GroupReference *gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            PlayerbotAI* ai = bot->GetPlayerbotAI();
            if (member && member->IsInWorld() && !member->GetPlayerbotAI() && (!master || master->GetPlayerbotAI()))
            {
                ai->SetMaster(member);
                ai->ResetStrategies();
                ai->TellMaster("Hello");
                break;
            }
        }
    }
}

void PlayerbotAI::ReInitCurrentEngine()
{
    InterruptSpell();
    currentEngine->Init();
}

void PlayerbotAI::ChangeStrategy(string names, BotState type)
{
    Engine* e = engines[type];
    if (!e)
        return;

    e->ChangeStrategy(names);
}

void PlayerbotAI::DoSpecificAction(string name)
{
    for (int i = 0 ; i < BOT_STATE_MAX; i++)
    {
        ostringstream out;
        ActionResult res = engines[i]->ExecuteAction(name);
        switch (res)
        {
        case ACTION_RESULT_UNKNOWN:
            continue;
        case ACTION_RESULT_OK:
            out << name << ": done";
            TellMaster(out);
            PlaySound(TEXT_EMOTE_NOD);
            return;
        case ACTION_RESULT_IMPOSSIBLE:
            out << name << ": impossible";
            TellMaster(out);
            PlaySound(TEXT_EMOTE_NO);
            return;
        case ACTION_RESULT_USELESS:
            out << name << ": useless";
            TellMaster(out);
            PlaySound(TEXT_EMOTE_NO);
            return;
        case ACTION_RESULT_FAILED:
            out << name << ": failed";
            TellMaster(out);
            return;
        }
    }
    ostringstream out;
    out << name << ": unknown action";
    TellMaster(out);
}

bool PlayerbotAI::PlaySound(uint32 emote)
{
    if (EmotesTextSoundEntry const* soundEntry = sDB2Manager.GetTextSoundEmoteFor(emote, bot->GetRace(), bot->GetNativeGender(), bot->GetClass()))
    {
        bot->PlayDistanceSound(soundEntry->SoundID);
        return true;
    }

    return false;
}

bool PlayerbotAI::ContainsStrategy(StrategyType type)
{
    for (int i = 0 ; i < BOT_STATE_MAX; i++)
    {
        if (engines[i]->ContainsStrategy(type))
            return true;
    }
    return false;
}

bool PlayerbotAI::HasStrategy(string name, BotState type)
{
    return engines[type]->HasStrategy(name);
}

void PlayerbotAI::ResetStrategies()
{
    for (int i = 0 ; i < BOT_STATE_MAX; i++)
        engines[i]->removeAllStrategies();

    AiFactory::AddDefaultCombatStrategies(bot, this, engines[BOT_STATE_COMBAT]);
    AiFactory::AddDefaultNonCombatStrategies(bot, this, engines[BOT_STATE_NON_COMBAT]);
    AiFactory::AddDefaultDeadStrategies(bot, this, engines[BOT_STATE_DEAD]);
}

bool PlayerbotAI::IsRanged(Player* player)
{
    PlayerbotAI* botAi = player->GetPlayerbotAI();
    if (botAi)
        return botAi->ContainsStrategy(STRATEGY_TYPE_RANGED);

    switch (player->GetClass())
    {
    case CLASS_DEATH_KNIGHT:
    case CLASS_PALADIN:
    case CLASS_WARRIOR:
    case CLASS_ROGUE:
        return false;
    case CLASS_DRUID:
        return !HasAnyAuraOf(player, "cat form", "bear form", "dire bear form", NULL);
    }
    return true;
}

bool PlayerbotAI::IsTank(Player* player)
{
    PlayerbotAI* botAi = player->GetPlayerbotAI();
    if (botAi)
        return botAi->ContainsStrategy(STRATEGY_TYPE_TANK);

    switch (player->GetClass())
    {
    case CLASS_DEATH_KNIGHT:
    case CLASS_PALADIN:
    case CLASS_WARRIOR:
        return true;
    case CLASS_DRUID:
        return HasAnyAuraOf(player, "bear form", "dire bear form", NULL);
    }
    return false;
}

bool PlayerbotAI::IsHeal(Player* player)
{
    PlayerbotAI* botAi = player->GetPlayerbotAI();
    if (botAi)
        return botAi->ContainsStrategy(STRATEGY_TYPE_HEAL);

    switch (player->GetClass())
    {
    case CLASS_PRIEST:
        return true;
    case CLASS_DRUID:
        return HasAnyAuraOf(player, "tree of life form", NULL);
    }
    return false;
}



namespace MaNGOS
{

    class UnitByGuidInRangeCheck
    {
    public:
        UnitByGuidInRangeCheck(WorldObject const* obj, ObjectGuid guid, float range) : i_obj(obj), i_range(range), i_guid(guid) {}
        WorldObject const& GetFocusObject() const { return *i_obj; }
        bool operator()(Unit* u)
        {
            return u->GetGUID() == i_guid && i_obj->IsWithinDistInMap(u, i_range);
        }
    private:
        WorldObject const* i_obj;
        float i_range;
        ObjectGuid i_guid;
    };

    class GameObjectByGuidInRangeCheck
    {
    public:
        GameObjectByGuidInRangeCheck(WorldObject const* obj, ObjectGuid guid, float range) : i_obj(obj), i_range(range), i_guid(guid) {}
        WorldObject const& GetFocusObject() const { return *i_obj; }
        bool operator()(GameObject* u)
        {
            if (u && i_obj->IsWithinDistInMap(u, i_range) && u->isSpawned() && u->GetGOInfo() && u->GetGUID() == i_guid)
                return true;

            return false;
        }
    private:
        WorldObject const* i_obj;
        float i_range;
        ObjectGuid i_guid;
    };

};


Unit* PlayerbotAI::GetUnit(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    Map* map = bot->GetMap();
    if (!map)
        return NULL;

    return ObjectAccessor::GetUnit(*bot, guid);
}


Creature* PlayerbotAI::GetCreature(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    Map* map = bot->GetMap();
    if (!map)
        return NULL;

    return map->GetCreature(guid);
}

GameObject* PlayerbotAI::GetGameObject(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    Map* map = bot->GetMap();
    if (!map)
        return NULL;

    return map->GetGameObject(guid);
}

bool PlayerbotAI::TellMasterNoFacing(string text, PlayerbotSecurityLevel securityLevel)
{
    Player* master = GetMaster();
    if (!master)
        return false;

    if (!GetSecurity()->CheckLevelFor(securityLevel, true, master))
        return false;

    if (sPlayerbotAIConfig.whisperDistance && !bot->GetGroup() && sRandomPlayerbotMgr.IsRandomBot(bot) &&
            master->GetSession()->GetSecurity() < SEC_GAMEMASTER &&
            (bot->GetMapId() != master->GetMapId() || bot->GetDistance(master) > sPlayerbotAIConfig.whisperDistance))
        return false;

    bot->Whisper(text, LANG_UNIVERSAL, master);
    return true;
}

bool PlayerbotAI::TellMaster(string text, PlayerbotSecurityLevel securityLevel)
{
    if (!TellMasterNoFacing(text, securityLevel))
        return false;

    if (!bot->isMoving() && !bot->IsInCombat() && bot->GetMapId() == master->GetMapId())
    {
        if (!bot->isInFront(master, M_PI / 2))
            bot->SetFacingTo(bot->GetAbsoluteAngle(master));

        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
    }

    return true;
}

bool IsRealAura(Player* bot, Aura const* aura, Unit* unit)
{
    if (!aura)
        return false;

    if (!unit->IsHostileTo(bot))
        return true;

    uint32 stacks = aura->GetStackAmount();
    if (stacks >= aura->GetSpellInfo()->StackAmount)
        return true;

    if (aura->GetCaster() == bot || aura->GetSpellInfo()->IsPositive() || aura->IsArea())
        return true;

    return false;
}

bool PlayerbotAI::HasAura(string name, Unit* unit)
{
    if (!unit)
        return false;

    uint32 spellId = aiObjectContext->GetValue<uint32>("spell id", name)->Get();
    if (spellId)
        return HasAura(spellId, unit);

    wstring wnamepart;
    if (!Utf8toWStr(name, wnamepart))
        return 0;

    wstrToLower(wnamepart);

    Unit::AuraApplicationMap& map = unit->GetAppliedAuras();
    for (Unit::AuraApplicationMap::iterator i = map.begin(); i != map.end(); ++i)
    {
        Aura const* aura  = i->second->GetBase();
        if (!aura)
            continue;

        const string auraName = aura->GetSpellInfo()->SpellName->Str[LOCALE_enUS];
        if (auraName.empty() || auraName.length() != wnamepart.length() || !Utf8FitTo(auraName, wnamepart))
            continue;

        if (IsRealAura(bot, aura, unit))
            return true;
    }

    return false;
}

bool PlayerbotAI::HasAura(uint32 spellId, const Unit* unit)
{
    if (!spellId || !unit)
        return false;

    Aura* aura = const_cast<Unit*>(unit)->GetAura(spellId);
    return IsRealAura(bot, aura, const_cast<Unit*>(unit));
}

bool PlayerbotAI::HasAnyAuraOf(Unit* player, ...)
{
    if (!player)
        return false;

    va_list vl;
    va_start(vl, player);

    const char* cur;
    do {
        cur = va_arg(vl, const char*);
        if (cur && HasAura(cur, player)) {
            va_end(vl);
            return true;
        }
    }
    while (cur);

    va_end(vl);
    return false;
}

bool PlayerbotAI::CanCastSpell(string name, Unit* target)
{
    return CanCastSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get(), target);
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, Unit* target, bool checkHasSpell)
{
    if (!spellid)
        return false;

    if (!target)
        target = bot;

    if (checkHasSpell && !bot->HasSpell(spellid))
        return false;

    if (bot->GetSpellHistory()->HasCooldown(spellid))
        return false;

    SpellInfo const *spellInfo = sSpellMgr->GetSpellInfo(spellid, DIFFICULTY_NONE);
    if (!spellInfo)
        return false;

    bool positiveSpell = spellInfo->IsPositive();
    if (positiveSpell && bot->IsHostileTo(target))
        return false;

    if (!positiveSpell && bot->IsFriendlyTo(target))
        return false;

    if (target->IsImmunedToSpell(spellInfo, bot))
        return false;

    if (bot != target && bot->GetDistance(target) > sPlayerbotAIConfig.sightDistance)
        return false;

    Unit* oldSel = bot->GetSelectedUnit();
    bot->SetSelection(target->GetGUID());
    Spell *spell = new Spell(bot, spellInfo, TRIGGERED_NONE);

    spell->m_targets.SetUnitTarget(target);
    spell->m_CastItem = aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get();
    spell->m_targets.SetItemTarget(spell->m_CastItem);
    SpellCastResult result = spell->CheckCast(false);
    delete spell;
	if (oldSel)
		bot->SetSelection(oldSel->GetGUID());

    switch (result)
    {
    case SPELL_FAILED_NOT_INFRONT:
    case SPELL_FAILED_NOT_STANDING:
    case SPELL_FAILED_UNIT_NOT_INFRONT:
    case SPELL_FAILED_SUCCESS:
    case SPELL_FAILED_MOVING:
    case SPELL_FAILED_TRY_AGAIN:
    case SPELL_FAILED_NOT_IDLE:
    case SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW:
    case SPELL_FAILED_SUMMON_PENDING:
    case SPELL_FAILED_BAD_IMPLICIT_TARGETS:
    case SPELL_FAILED_BAD_TARGETS:
    case SPELL_FAILED_ITEM_NOT_FOUND:
        return true;
    default:
        return false;
    }
}


bool PlayerbotAI::CastSpell(string name, Unit* target)
{
    bool result = CastSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get(), target);
    if (result)
    {
        aiObjectContext->GetValue<time_t>("last spell cast time", name)->Set(time(0));
    }

    return result;
}

bool PlayerbotAI::CastSpell(uint32 spellId, Unit* target)
{
    if (!spellId)
        return false;

    if (!target)
        target = bot;

    Pet* pet = bot->GetPet();
    const SpellInfo* const pSpellInfo = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
    if (pet && pet->HasSpell(spellId))
    {
        pet->GetCharmInfo()->SetSpellAutocast(pSpellInfo, true);
        pet->GetCharmInfo()->ToggleCreatureAutocast(pSpellInfo, true);
        TellMaster("My pet will auto-cast this spell");
        return true;
    }

    aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, target->GetGUID(), time(0));
    aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);

    if (bot->IsFlying())
        return false;

    // Never blanket-clear unit states here. The legacy
    // ClearUnitState(UNIT_STATE_ALL_STATE_SUPPORTED) line used to sit here,
    // and on this core that mask covers EVERY state - including
    // UNIT_STATE_MELEE_ATTACKING. Every special ability therefore silently
    // stopped the bot's autoattack (without the AttackStop packet, so the
    // client stayed frozen in attack stance), and the next melee tick
    // re-sent AttackStart - the "first attack frame over and over, never
    // actually swinging" loop. The spell system sets/clears CASTING itself,
    // and movement is handled by the reach actions, so there is nothing to
    // reset here at all.

    Unit* oldSel = bot->GetSelectedUnit();
    bot->SetSelection(target->GetGUID());

    Spell *spell = new Spell(bot, pSpellInfo, TRIGGERED_NONE);
    if (bot->isMoving() && spell->GetCastTime())
    {
        delete spell;
        return false;
    }

    SpellCastTargets targets;
    WorldObject* faceTo = target;

    if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION ||
            pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        targets.SetDst(target->GetPosition());
    }
    else
    {
        targets.SetUnitTarget(target);
    }

    if (pSpellInfo->Targets & TARGET_FLAG_ITEM)
    {
        spell->m_CastItem = aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get();
        targets.SetItemTarget(spell->m_CastItem);
    }

    if (pSpellInfo->GetEffect(SpellEffIndex(0)).Effect == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->GetEffect(SpellEffIndex(0)).Effect == SPELL_EFFECT_SKINNING)
    {
        LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
        if (!loot.IsLootPossible(bot))
        {
            delete spell;
            return false;
        }

        GameObject* go = GetGameObject(loot.guid);
        if (go && go->isSpawned())
        {
            WorldPacket* const packetgouse = new WorldPacket(CMSG_GAME_OBJ_REPORT_USE, 8);
            *packetgouse << loot.guid;
            bot->GetSession()->QueuePacket(packetgouse);
            targets.SetGOTarget(go);
            faceTo = go;
        }
        else
        {
            Unit* creature = GetUnit(loot.guid);
            if (creature)
            {
                targets.SetUnitTarget(creature);
                faceTo = creature;
            }
        }
    }


    if (!bot->isInFront(faceTo, M_PI / 2))
    {
        bot->SetFacingTo(bot->GetAbsoluteAngle(faceTo));
        delete spell;
        SetNextCheckDelay(sPlayerbotAIConfig.globalCoolDown);
        return false;
    }

	spell->prepare(targets);
	WaitForSpellCast(spell);

    if (oldSel)
        bot->SetSelection(oldSel->GetGUID());

    LastSpellCast& lastSpell = aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get();
    return lastSpell.id == spellId;
}

void PlayerbotAI::WaitForSpellCast(Spell *spell)
{
    const SpellInfo* const pSpellInfo = spell->GetSpellInfo();

    float castTime = spell->GetCastTime();
    if (pSpellInfo->IsChanneled())
    {
        int32 duration = pSpellInfo->GetDuration();
        if (duration > 0)
            castTime += duration;
    }

    castTime = ceil(castTime);

    uint32 globalCooldown = CalculateGlobalCooldown(pSpellInfo->Id);
    if (castTime < globalCooldown)
        castTime = globalCooldown;

    SetNextCheckDelay(castTime + sPlayerbotAIConfig.reactDelay);
}

void PlayerbotAI::InterruptSpell()
{
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return;

    LastSpellCast& lastSpell = aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get();

    for (int type = CURRENT_MELEE_SPELL; type < CURRENT_CHANNELED_SPELL; type++)
    {
        Spell* spell = bot->GetCurrentSpell((CurrentSpellTypes)type);
        if (!spell)
            continue;

        if (spell->m_spellInfo->IsPositive())
            continue;

        // The core cancel path emits the real 3.4.3 SpellFailure and
        // SpellFailedOther packets, with cast GUID and visual/reason fields.
        // Do not fabricate a second pair with the obsolete 3.3.5 byte layout.
        uint32 spellId = spell->m_spellInfo->Id;
        bot->InterruptSpell((CurrentSpellTypes)type);
        SpellInterrupted(spellId);
    }

    SpellInterrupted(lastSpell.id);
}


void PlayerbotAI::RemoveAura(string name)
{
    uint32 spellid = aiObjectContext->GetValue<uint32>("spell id", name)->Get();
    if (spellid && HasAura(spellid, bot))
        bot->RemoveAurasDueToSpell(spellid);
}

bool PlayerbotAI::IsInterruptableSpellCasting(Unit* target, string spell)
{
    uint32 spellid = aiObjectContext->GetValue<uint32>("spell id", spell)->Get();
    if (!spellid || !target->IsNonMeleeSpellCast(true))
        return false;

    SpellInfo const *spellInfo = sSpellMgr->GetSpellInfo(spellid, DIFFICULTY_NONE);
    if (!spellInfo)
        return false;

    if (target->IsImmunedToSpell(spellInfo, bot))
        return false;

    for (uint32 i = EFFECT_0; i <= EFFECT_2; i++)
    {
        if (spellInfo->InterruptFlags.HasFlag(SpellInterruptFlags::DamageCancels) && spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
            return true;

        if ((spellInfo->GetEffect(SpellEffIndex(i)).Effect == SPELL_EFFECT_REMOVE_AURA || spellInfo->GetEffect(SpellEffIndex(i)).Effect == SPELL_EFFECT_INTERRUPT_CAST) &&
                !target->IsImmunedToSpellEffect(spellInfo, spellInfo->GetEffect(SpellEffIndex(i)), bot))
            return true;
    }

    return false;
}

bool PlayerbotAI::HasAuraToDispel(Unit* target, uint32 dispelType)
{
    if (!target)
        return false;

    Unit::AuraApplicationMap const& auras = target->GetAppliedAuras();
    for (auto const& pair : auras)
    {
        AuraApplication const* app = pair.second;
        if (!app)
            continue;

        Aura const* aura = app->GetBase();
        if (!aura)
            continue;

        SpellInfo const* entry = aura->GetSpellInfo();
        if (!entry)
            continue;

        bool isPositiveSpell = app->IsPositive();
        if (isPositiveSpell && bot->IsFriendlyTo(target))
            continue;

        if (!isPositiveSpell && bot->IsHostileTo(target))
            continue;

        if (canDispel(entry, dispelType))
            return true;
    }
    return false;
}


#ifndef WIN32
inline int strcmpi(const char* s1, const char* s2)
{
    for (; *s1 && *s2 && (toupper(*s1) == toupper(*s2)); ++s1, ++s2);
    return *s1 - *s2;
}
#endif

bool PlayerbotAI::canDispel(const SpellInfo* entry, uint32 dispelType)
{
    if (entry->Dispel != dispelType)
        return false;

    return !entry->SpellName->Str[LOCALE_enUS] ||
        (strcmpi((const char*)entry->SpellName->Str[LOCALE_enUS], "demon skin") &&
        strcmpi((const char*)entry->SpellName->Str[LOCALE_enUS], "mage armor") &&
        strcmpi((const char*)entry->SpellName->Str[LOCALE_enUS], "frost armor") &&
        strcmpi((const char*)entry->SpellName->Str[LOCALE_enUS], "wavering will") &&
        strcmpi((const char*)entry->SpellName->Str[LOCALE_enUS], "chilled") &&
        strcmpi((const char*)entry->SpellName->Str[LOCALE_enUS], "ice armor"));
}

bool IsAlliance(uint8 race)
{
    return race == RACE_HUMAN || race == RACE_DWARF || race == RACE_NIGHTELF ||
            race == RACE_GNOME || race == RACE_DRAENEI;
}

bool PlayerbotAI::IsOpposing(Player* player)
{
    return IsOpposing(player->GetRace(), bot->GetRace());
}

bool PlayerbotAI::IsOpposing(uint8 race1, uint8 race2)
{
    return (IsAlliance(race1) && !IsAlliance(race2)) || (!IsAlliance(race1) && IsAlliance(race2));
}

void PlayerbotAI::RemoveShapeshift()
{
    RemoveAura("bear form");
    RemoveAura("dire bear form");
    RemoveAura("moonkin form");
    RemoveAura("travel form");
    RemoveAura("cat form");
    RemoveAura("flight form");
    RemoveAura("swift flight form");
    RemoveAura("aquatic form");
    RemoveAura("ghost wolf");
    RemoveAura("tree of life");
}

uint32 PlayerbotAI::GetEquipGearScore(Player* player, bool withBags, bool withBank)
{
    std::vector<uint32> gearScore(EQUIPMENT_SLOT_END);
    uint32 twoHandScore = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            _fillGearScoreData(player, item, &gearScore, twoHandScore);
    }

    if (withBags)
    {
        // check inventory
        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore);
        }

        // check bags
        for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Bag* pBag = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* item2 = pBag->GetItemByPos(j))
                        _fillGearScoreData(player, item2, &gearScore, twoHandScore);
                }
            }
        }
    }

    if (withBank)
    {
        for (uint8 i = BANK_SLOT_ITEM_START; i < BANK_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore);
        }

        for (uint8 i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (item->IsBag())
                {
                    Bag* bag = (Bag*)item;
                    for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    {
                        if (Item* item2 = bag->GetItemByPos(j))
                            _fillGearScoreData(player, item2, &gearScore, twoHandScore);
                    }
                }
            }
        }
    }

    uint8 count = EQUIPMENT_SLOT_END - 2;   // ignore body and tabard slots
    uint32 sum = 0;

    // check if 2h hand is higher level than main hand + off hand
    if (gearScore[EQUIPMENT_SLOT_MAINHAND] + gearScore[EQUIPMENT_SLOT_OFFHAND] < twoHandScore * 2)
    {
        gearScore[EQUIPMENT_SLOT_OFFHAND] = 0;  // off hand is ignored in calculations if 2h weapon has higher score
        --count;
        gearScore[EQUIPMENT_SLOT_MAINHAND] = twoHandScore;
    }

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
       sum += gearScore[i];
    }

    if (count)
    {
        uint32 res = uint32(sum / count);
        return res;
    }
    else
        return 0;
}

void PlayerbotAI::_fillGearScoreData(Player *player, Item* item, std::vector<uint32>* gearScore, uint32& twoHandScore)
{
    if (!item)
        return;

    if (player->CanUseItem(item->GetTemplate()) != EQUIP_ERR_OK)
        return;

    uint8 type   = item->GetTemplate()->GetInventoryType();
    uint32 level = item->GetTemplate()->GetItemLevel();

    switch (type)
    {
        case INVTYPE_2HWEAPON:
            twoHandScore = std::max(twoHandScore, level);
            break;
        case INVTYPE_WEAPON:
        case INVTYPE_WEAPONMAINHAND:
            (*gearScore)[EQUIPMENT_SLOT_MAINHAND] = std::max((*gearScore)[EQUIPMENT_SLOT_MAINHAND], level);
            break;
        case INVTYPE_SHIELD:
        case INVTYPE_WEAPONOFFHAND:
            (*gearScore)[EQUIPMENT_SLOT_OFFHAND] = std::max((*gearScore)[EQUIPMENT_SLOT_OFFHAND], level);
            break;
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_RANGED:
        case INVTYPE_QUIVER:
        case INVTYPE_RELIC:
            (*gearScore)[EQUIPMENT_SLOT_RANGED] = std::max((*gearScore)[EQUIPMENT_SLOT_RANGED], level);
            break;
        case INVTYPE_HEAD:
            (*gearScore)[EQUIPMENT_SLOT_HEAD] = std::max((*gearScore)[EQUIPMENT_SLOT_HEAD], level);
            break;
        case INVTYPE_NECK:
            (*gearScore)[EQUIPMENT_SLOT_NECK] = std::max((*gearScore)[EQUIPMENT_SLOT_NECK], level);
            break;
        case INVTYPE_SHOULDERS:
            (*gearScore)[EQUIPMENT_SLOT_SHOULDERS] = std::max((*gearScore)[EQUIPMENT_SLOT_SHOULDERS], level);
            break;
        case INVTYPE_BODY:
            (*gearScore)[EQUIPMENT_SLOT_BODY] = std::max((*gearScore)[EQUIPMENT_SLOT_BODY], level);
            break;
        case INVTYPE_CHEST:
            (*gearScore)[EQUIPMENT_SLOT_CHEST] = std::max((*gearScore)[EQUIPMENT_SLOT_CHEST], level);
            break;
        case INVTYPE_WAIST:
            (*gearScore)[EQUIPMENT_SLOT_WAIST] = std::max((*gearScore)[EQUIPMENT_SLOT_WAIST], level);
            break;
        case INVTYPE_LEGS:
            (*gearScore)[EQUIPMENT_SLOT_LEGS] = std::max((*gearScore)[EQUIPMENT_SLOT_LEGS], level);
            break;
        case INVTYPE_FEET:
            (*gearScore)[EQUIPMENT_SLOT_FEET] = std::max((*gearScore)[EQUIPMENT_SLOT_FEET], level);
            break;
        case INVTYPE_WRISTS:
            (*gearScore)[EQUIPMENT_SLOT_WRISTS] = std::max((*gearScore)[EQUIPMENT_SLOT_WRISTS], level);
            break;
        case INVTYPE_HANDS:
            (*gearScore)[EQUIPMENT_SLOT_HEAD] = std::max((*gearScore)[EQUIPMENT_SLOT_HEAD], level);
            break;
        // equipped gear score check uses both rings and trinkets for calculation, assume that for bags/banks it is the same
        // with keeping second highest score at second slot
        case INVTYPE_FINGER:
        {
            if ((*gearScore)[EQUIPMENT_SLOT_FINGER1] < level)
            {
                (*gearScore)[EQUIPMENT_SLOT_FINGER2] = (*gearScore)[EQUIPMENT_SLOT_FINGER1];
                (*gearScore)[EQUIPMENT_SLOT_FINGER1] = level;
            }
            else if ((*gearScore)[EQUIPMENT_SLOT_FINGER2] < level)
                (*gearScore)[EQUIPMENT_SLOT_FINGER2] = level;
            break;
        }
        case INVTYPE_TRINKET:
        {
            if ((*gearScore)[EQUIPMENT_SLOT_TRINKET1] < level)
            {
                (*gearScore)[EQUIPMENT_SLOT_TRINKET2] = (*gearScore)[EQUIPMENT_SLOT_TRINKET1];
                (*gearScore)[EQUIPMENT_SLOT_TRINKET1] = level;
            }
            else if ((*gearScore)[EQUIPMENT_SLOT_TRINKET2] < level)
                (*gearScore)[EQUIPMENT_SLOT_TRINKET2] = level;
            break;
        }
        case INVTYPE_CLOAK:
            (*gearScore)[EQUIPMENT_SLOT_BACK] = std::max((*gearScore)[EQUIPMENT_SLOT_BACK], level);
            break;
        default:
            break;
    }
}

string PlayerbotAI::HandleRemoteCommand(string command)
{
    if (command == "state")
    {
        switch (currentState)
        {
        case BOT_STATE_COMBAT:
            return "combat";
        case BOT_STATE_DEAD:
            return "dead";
        case BOT_STATE_NON_COMBAT:
            return "non-combat";
        default:
            return "unknown";
        }
    }
    else if (command == "position")
    {
        ostringstream out; out << bot->GetPositionX() << " " << bot->GetPositionY() << " " << bot->GetPositionZ() << " " << bot->GetMapId() << " " << bot->GetOrientation();
        return out.str();
    }
    else if (command == "tpos")
    {
        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return "";
        }

        ostringstream out; out << target->GetPositionX() << " " << target->GetPositionY() << " " << target->GetPositionZ() << " " << target->GetMapId() << " " << target->GetOrientation();
        return out.str();
    }
    else if (command == "movement")
    {
        LastMovement& data = *GetAiObjectContext()->GetValue<LastMovement&>("last movement");
        ostringstream out; out << data.lastMoveToX << " " << data.lastMoveToY << " " << data.lastMoveToZ << " " << bot->GetMapId() << " " << data.lastMoveToOri;
        return out.str();
    }
    else if (command == "target")
    {
        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return "";
        }

        return target->GetName();
    }
    else if (command == "hp")
    {
        int pct = (int)((static_cast<float> (bot->GetHealth()) / bot->GetMaxHealth()) * 100);
        ostringstream out; out << pct << "%";

        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return out.str();
        }

        pct = (int)((static_cast<float> (target->GetHealth()) / target->GetMaxHealth()) * 100);
        out << " / " << pct << "%";
        return out.str();
    }
    else if (command == "strategy")
    {
        return currentEngine->ListStrategies();
    }
    else if (command == "action")
    {
        return currentEngine->GetLastAction();
    }
    else if (command == "values")
    {
        return GetAiObjectContext()->FormatValues();
    }
    ostringstream out; out << "invalid command: " << command;
    return out.str();
}
