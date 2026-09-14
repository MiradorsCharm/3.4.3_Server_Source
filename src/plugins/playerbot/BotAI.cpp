#include "BotAI.h"

#include "BotConfig.h"
#include "BotDiagnostics.h"
#include "Player.h"
#include "Unit.h"
#include "Creature.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "WorldSession.h"
#include "Server/Packets/PartyPackets.h"
#include "Log.h"
#include "Util.h"

BotAI::BotAI(Player* bot) : _bot(bot)
{
    _movement = std::make_unique<BotMovement>(bot);
    _spells = std::make_unique<BotSpells>(bot);
    _combat = std::make_unique<BotCombat>(this, bot);
    _loot = std::make_unique<BotLoot>(this, bot);
    _classAI.reset(CreateBotClassAI(bot->GetClass(), this));
}

BotAI::~BotAI() = default;

bool BotAI::CanSee(Unit* who) const
{
    return who && who->IsInWorld() && who->GetMapId() == _bot->GetMapId() && _bot->IsWithinDistInMap(who, sBotConfig->SightDistance + 20.0f);
}

void BotAI::SetMaster(Player* master)
{
    _masterGuid = master ? master->GetGUID() : ObjectGuid::Empty;
}

Player* BotAI::GetMaster() const
{
    if (_masterGuid.IsEmpty())
        return nullptr;
    return ObjectAccessor::FindConnectedPlayer(_masterGuid);
}

bool BotAI::AcceptsCommandsFrom(Player* sender) const
{
    if (!sender)
        return false;
    if (sender->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
        return true;
    Player* master = GetMaster();
    if (master && sender->GetGUID() == master->GetGUID())
        return true;
    // A bot without a bound master accepts its summoner's account.
    if (!master && sender->GetSession()->GetAccountId() == _bot->GetSession()->GetAccountId())
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// orders
// ---------------------------------------------------------------------------

void BotAI::WhisperMaster(std::string const& text)
{
    Player* master = GetMaster();
    if (!master)
        return;
    _bot->Whisper(text, LANG_UNIVERSAL, master);
}

void BotAI::HandleCommand(std::string const& msg, Player* sender)
{
    if (!AcceptsCommandsFrom(sender))
        return;

    // strip a leading "!" if someone types it out of habit
    std::string command = msg;
    if (!command.empty() && command[0] == '!')
        command.erase(0, 1);

    auto startsWith = [&command](char const* prefix)
    {
        return command.rfind(prefix, 0) == 0;
    };

    if (startsWith("follow"))
        CommandFollow();
    else if (startsWith("stay"))
        CommandStay();
    else if (startsWith("come") || startsWith("here"))
        CommandCome(sender);
    else if (startsWith("attack my target") || startsWith("attack") || startsWith("kill"))
        CommandAttackMyTarget(sender);
    else if (startsWith("assist"))
        CommandAssist(sender);
    else if (startsWith("stop attack") || startsWith("stopattack"))
        CommandStopAttack();
    else if (startsWith("loot"))
        CommandLoot();
    else if (startsWith("heal"))
        CommandHeal();
    else if (startsWith("status"))
        CommandStatus(sender);
    else if (startsWith("release"))
    {
        if (_bot->getDeathState() == CORPSE || _bot->getDeathState() == DEAD)
            _deadTimer = sBotConfig->ReviveDelayMs;  // fast-path the self-res
    }
    else if (startsWith("help"))
        WhisperMaster("I understand: follow, stay, come, attack my target, assist, stop attack, loot, heal, status, release");
    else
        WhisperMaster("I don't know that command - whisper 'help' for the list");
}

void BotAI::Attack(Unit* target, std::string reason)
{
    if (!target || !target->IsAlive())
        return;
    _stay = false;
    _combat->SetStance(_classAI->IsMeleeClass() ? BotCombatStance::Melee : BotCombatStance::Ranged);
    if (!_combat->HasVictim() || _combat->GetVictim() != target)
        _combat->SetVictim(target, std::move(reason));
}

void BotAI::StopAttacking(std::string /*reason*/)
{
    _combat->ClearVictim("ordered stop");
    _movement->Stop();
}

void BotAI::OnVictimDied(Unit* victim)
{
    // Queue the corpse for looting and drop out of combat. This runs from
    // BotCombat::Update at the moment the death is detected - the only place
    // the victim guid is still valid (ClearVictim clears it right after).
    if (victim)
        if (Creature* corpse = victim->ToCreature())
            _loot->QueueCorpse(corpse);
    _combat->ClearVictim("target dead");
    _movement->Stop();
}

void BotAI::CommandFollow()
{
    _stay = false;
    _stayPoint.reset();
    _lastOrder = "follow";
}

void BotAI::CommandStay()
{
    _stay = true;
    _stayPoint = _bot->GetPosition();
    _movement->Stop();
    _lastOrder = "stay";
}

void BotAI::CommandCome(Player* sender)
{
    if (!sender)
        return;
    _stay = false;
    _stayPoint.reset();
    _movement->MoveTo(sender->GetPositionX(), sender->GetPositionY(), sender->GetPositionZ(), 2.0f, false);
    _lastOrder = "come";
}

void BotAI::CommandAttackMyTarget(Player* sender)
{
    if (!sender)
        return;
    if (Unit* target = sender->GetSelectedUnit())
        Attack(target, "ordered: attack master's target");
    _lastOrder = "attack";
}

void BotAI::CommandAssist(Player* sender)
{
    if (!sender)
        return;
    if (Unit* victim = sender->GetVictim())
        Attack(victim, "ordered: assist master");
    _lastOrder = "assist";
}

void BotAI::CommandStopAttack()
{
    StopAttacking("ordered stop");
    _lastOrder = "stop attacking";
}

void BotAI::CommandLoot()
{
    // Corpses from our own kills are queued automatically after combat; this
    // command just marks the intent (and stops any fight).
    StopAttacking("loot");
    _lastOrder = "loot";
}

void BotAI::CommandHeal()
{
    _classAI->HealTick(*this);
    _lastOrder = "heal";
}

void BotAI::CommandStatus(Player* to)
{
    if (!to)
        return;
    std::string text = "status: ";
    if (Unit* victim = _combat->GetVictim())
    {
        text += "fighting " + victim->GetName();
        text += " (" + std::to_string(uint32(_bot->GetExactDist(victim))) + "yd)";
    }
    else
        text += "no victim";
    text += _stay ? ", staying" : ", following";
    if (Unit* master = GetMaster())
        text += ", master " + master->GetName();
    _bot->Whisper(text, LANG_UNIVERSAL, to);
}

// ---------------------------------------------------------------------------
// pipeline
// ---------------------------------------------------------------------------

void BotAI::Update(uint32 diff)
{
    if (!_bot->IsInWorld())
        return;

    UpdateDeath(diff);

    // pending group invite from our master (or a GM): accept it
    if (_bot->GetGroupInvite())
    {
        Group* invite = _bot->GetGroupInvite();
        Player* inviter = ObjectAccessor::FindPlayer(invite->GetLeaderGUID());
        WorldPackets::Party::PartyInviteResponse response{ WorldPacket(CMSG_PARTY_INVITE_RESPONSE) };
        response.Accept = AcceptsCommandsFrom(inviter);
        _bot->GetSession()->HandlePartyInviteResponseOpcode(response);
    }

    if (!_bot->IsAlive())
        return;

    // Brain order matters: combat first (it owns the movement goal), then
    // loot (out of combat only), then follow/stay/wander, then diagnostics.
    UpdateBrain(diff);

    // Movement runs after the brain so a freshly issued goal is acted on in
    // the same tick.
    _movement->Update(diff);

    UpdateDiagnostics(diff);
}

void BotAI::UpdateDeath(uint32 diff)
{
    if (_bot->IsAlive())
    {
        _deadTimer = 0;
        return;
    }

    _deadTimer += diff;
    if (_deadTimer < sBotConfig->ReviveDelayMs)
        return;

    // Self-resurrect in place after the delay. Bot sessions have no client,
    // so nothing drives the JUST_DIED -> CORPSE transition (that is a client
    // release/repop action); ResurrectPlayer is valid from every dead state
    // and SpawnCorpseBones no-ops when there is no corpse to convert.
    _bot->ResurrectPlayer(0.5f, false);
    _bot->SpawnCorpseBones(false);
    _bot->SaveToDB();
    _deadTimer = 0;
}

void BotAI::UpdateRetaliate()
{
    if (_combat->HasVictim())
        return;

    // Fight back: whoever pulled us into combat.
    if (_bot->IsInCombat())
    {
        if (Unit* attacker = _bot->getAttackerForHelper())
        {
            _combat->SetStance(_classAI->IsMeleeClass() ? BotCombatStance::Melee : BotCombatStance::Ranged);
            _combat->SetVictim(attacker, "retaliate");
        }
    }
}

void BotAI::UpdateBrain(uint32 diff)
{
    (void)diff;

    UpdateRetaliate();

    // Assist the master's fight (group play): if the master is in combat and
    // we are healthy enough, join in.
    if (Unit* master = GetMaster())
    {
        if (sBotConfig->AutoAssistMaster && master->IsInCombat() && !_combat->HasVictim()
            && _bot->GetHealthPct() > 40.0f && _bot->IsInMap(master))
        {
            if (Unit* masterVictim = master->GetVictim())
                if (_bot->IsValidAttackTarget(masterVictim))
                {
                    _combat->SetStance(_classAI->IsMeleeClass() ? BotCombatStance::Melee : BotCombatStance::Ranged);
                    _combat->SetVictim(masterVictim, "assist master");
                }
        }
    }

    // --- combat ------------------------------------------------------------
    if (_combat->HasVictim())
    {
        _combat->Update(diff);

        // Class self-preservation runs before the damage rotation (already
        // inside BotCombat::Update -> CombatTick); heals are prioritized by
        // the script itself. A victim death is handled inside Update()
        // through OnVictimDied (while the guid is still resolvable).
        return;
    }

    // --- out of combat ------------------------------------------------------
    _classAI->IdleTick(*this);

    // loot nearby corpses
    _loot->Update(diff);
    if (_loot->HasWork())
        return;

    UpdateNonCombat(diff);
}

void BotAI::UpdateNonCombat(uint32 diff)
{
    // 1. stay mode
    if (_stay)
    {
        if (_stayPoint && _bot->GetExactDist2d(&*_stayPoint) > 2.0f)
            _movement->MoveTo(_stayPoint->GetPositionX(), _stayPoint->GetPositionY(), _stayPoint->GetPositionZ(), 0.5f);
        return;
    }

    // 2. follow the master
    if (Unit* master = GetMaster())
    {
        if (CanSee(master))
        {
            float dist = _bot->GetExactDist2d(master);
            if (dist > sBotConfig->FollowDistance + 2.0f)
                _movement->Follow(master, sBotConfig->FollowDistance);
            else
            {
                _movement->Stop();
                // face the same way as the master while idle, like real party
                // members stand
                if (dist <= sBotConfig->FollowDistance + 1.0f && !_movement->HasLiveGoal())
                    _movement->Face(master);
            }
            return;
        }
        // master not visible: keep the current position (no teleporting to
        // the master in v1 - that is what the summon command is for)
        _movement->Stop();
        return;
    }

    // 3. random bots wander around their area
    if (_randomBot)
        UpdateWander(diff);
}

void BotAI::UpdateWander(uint32 diff)
{
    if (_wanderTimer > diff)
    {
        _wanderTimer -= diff;
        return;
    }
    _wanderTimer = urand(20000, 60000);

    float const radius = sBotConfig->WanderRadius;
    float const angle = frand(0.0f, 2.0f * float(M_PI));
    float const dist = frand(radius * 0.3f, radius);
    float x = _bot->GetPositionX() + std::cos(angle) * dist;
    float y = _bot->GetPositionY() + std::sin(angle) * dist;
    float z = _bot->GetPositionZ();
    _bot->UpdateGroundPositionZ(x, y, z);
    if (std::fabs(z - _bot->GetPositionZ()) > 15.0f)
        return; // would walk off a cliff - pick another time
    _movement->MoveTo(x, y, z, 1.0f);
}

void BotAI::UpdateDiagnostics(uint32 diff)
{
    if (!sBotConfig->Diagnostics)
        return;

    if (_stallReportCooldown > diff)
        _stallReportCooldown -= diff;
    else
        _stallReportCooldown = 0;

    _diagnosticsTimer += diff;
    if (_diagnosticsTimer < 1000)
        return;
    _diagnosticsTimer -= 1000;

    BotDiagnostics::ReportStall(*this);
}

void BotAI::HandleBotOutgoingPacket(WorldPacket const* /*packet*/)
{
    // The new AI does not parse server->client packets; loot and group state
    // are read directly from the core objects. Kept as a seam for future
    // features (trades, duel requests).
}
