#include "BotAI.h"

#include "BotConfig.h"
#include "BotDiagnostics.h"
#include "BotFactory.h"
#include "BotInteract.h"
#include "BotQueues.h"
#include "BotTalents.h"
#include "Player.h"
#include "Unit.h"
#include "Creature.h"
#include "Pet.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Container/Bag.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellAuraDefines.h"
#include "Grids/Notifiers/GridNotifiers.h"
#include "Grids/Cells/Cell.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "GroupReference.h"
#include "WorldSession.h"
#include "Server/Packets/PartyPackets.h"
#include "Log.h"
#include "Util.h"

#include <algorithm>

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

namespace
{
    // the use-spell of a food (drink=false) / water (drink=true) item in the
    // bags, resolved through the same ItemEffect entries CastItemUseSpell
    // walks - 0 when none found
    uint32 ConsumableSpellFor(Player* bot, Item* item, bool drink)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || proto->GetClass() != ITEM_CLASS_CONSUMABLE)
            return 0;
        if (bot->CanUseItem(proto, false) != EQUIP_ERR_OK)
            return 0;

        for (ItemEffectEntry const* effect : proto->Effects)
        {
            if (!effect->SpellID || effect->TriggerType != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(effect->SpellID, DIFFICULTY_NONE);
            if (!info)
                continue;
            if (drink ? info->HasAura(SPELL_AURA_MOD_POWER_REGEN) : info->HasAura(SPELL_AURA_MOD_REGEN))
                return effect->SpellID;
        }
        return 0;
    }

    uint32 FindConsumableSpell(Player* bot, bool drink)
    {
        for (uint8 i = 0; i < 4; ++i)                       // equipped bags
            if (Bag* bag = bot->GetBagByPos(i))
                for (uint8 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(slot))
                        if (uint32 spell = ConsumableSpellFor(bot, item, drink))
                            return spell;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)  // backpack
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (uint32 spell = ConsumableSpellFor(bot, item, drink))
                    return spell;

        return 0;
    }

    // nearest grindable mob: alive non-elite creature in a sane level window,
    // not already fighting, attackable by us
    struct GrindTargetCheck
    {
        WorldObject const* obj;
        Player const* bot;
        mutable float range;

        GrindTargetCheck(WorldObject const* o, Player const* b, float r) : obj(o), bot(b), range(r) { }

        bool operator()(Unit* u) const
        {
            if (!u || !u->IsAlive() || !u->IsInWorld())
                return false;
            Creature* c = u->ToCreature();
            if (!c || c->IsCritter() || c->IsTotem() || c->IsPet() || c->IsElite())
                return false;
            if (c->IsInCombat())
                return false;
            if (int32(u->GetLevel()) - int32(bot->GetLevel()) > 3)
                return false;
            if (int32(u->GetLevel()) + 15 < int32(bot->GetLevel()))
                return false;
            if (!bot->IsValidAttackTarget(u))
                return false;
            if (!obj->IsWithinDist(u, range) || !obj->CanSeeOrDetect(u))
                return false;
            range = obj->GetDistance(u);        // narrow: the next hit must be closer
            return true;
        }
    };
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
    else if (startsWith("stop grind"))
        CommandGrind(false);
    else if (startsWith("loot"))
        CommandLoot();
    else if (startsWith("heal"))
        CommandHeal();
    else if (startsWith("buff"))
        CommandBuff();
    else if (startsWith("rez") || startsWith("resurrect") || command == "res")
        CommandRez();
    else if (startsWith("cure") || startsWith("dispel"))
        CommandCure();
    else if (startsWith("drink"))
        CommandDrink();
    else if (startsWith("eat"))
        CommandEat();
    else if (startsWith("upgrade"))
        CommandUpgrade();
    else if (startsWith("repair"))
        CommandRepair();
    else if (startsWith("tank"))
        CommandTank();
    else if (startsWith("dps"))
        CommandDps();
    else if (startsWith("grind"))
        CommandGrind(true);
    else if (startsWith("summon"))
        CommandSummon();
    else if (startsWith("guild leave"))
        CommandGuildLeave();
    else if (startsWith("guild"))
        CommandGuild();
    else if (startsWith("queue"))
        CommandQueue();
    else if (startsWith("leave"))
        CommandLeave();
    else if (startsWith("sell"))
        CommandSell();
    else if (startsWith("quests") || startsWith("quest"))
        CommandQuests();
    else if (startsWith("talents"))
    {
        uint32 spent = BotTalents::SpendPoints(_bot);
        WhisperMaster(spent ? ("spent " + std::to_string(spent) + " talent point(s)") : "no free talent points");
    }
    else if (startsWith("status"))
        CommandStatus(sender);
    else if (startsWith("release"))
    {
        if (_bot->getDeathState() == CORPSE || _bot->getDeathState() == DEAD)
            _deadTimer = sBotConfig->ReviveDelayMs;  // fast-path the self-res
    }
    else if (startsWith("help"))
        WhisperMaster("I understand: follow, stay, come, summon, attack my target, assist, stop attack, loot, heal, buff, rez, cure, eat, drink, upgrade, repair, tank, dps, grind, stop grind, queue, leave, guild, sell, quests, talents, status, release");
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

void BotAI::CommandBuff()
{
    _partyCareCooldown = 0;
    _classAI->BuffTick(*this);
    _lastOrder = "buff";
}

void BotAI::CommandRez()
{
    _partyCareCooldown = 0;
    _classAI->RezTick(*this);
    _lastOrder = "rez";
}

void BotAI::CommandCure()
{
    _partyCareCooldown = 0;
    _classAI->CureTick(*this);
    _lastOrder = "cure";
}

void BotAI::CommandEat()
{
    _forceConsumeTimer = 30000;
    _consumeCooldown = 0;
    _lastOrder = "eat";
}

void BotAI::CommandDrink()
{
    _forceConsumeTimer = 30000;
    _consumeCooldown = 0;
    _lastOrder = "drink";
}

void BotAI::CommandUpgrade()
{
    WhisperMaster(BotFactory::UpgradeGear(_bot) ? "upgraded my gear from my bags"
                                                : "nothing in my bags is an upgrade");
    _lastOrder = "upgrade";
}

void BotAI::CommandRepair()
{
    _bot->DurabilityRepairAll(true, 1.0f, false);
    WhisperMaster("gear repaired");
    _lastOrder = "repair";
}

void BotAI::CommandTank()
{
    if (!CanTank())
    {
        WhisperMaster("I am not built for tanking");
        return;
    }
    _tankMode = true;
    WhisperMaster("tank mode on - keep the fight on me");
    _lastOrder = "tank";
}

void BotAI::CommandDps()
{
    _tankMode = false;
    WhisperMaster("dps mode");
    _lastOrder = "dps";
}

void BotAI::CommandGrind(bool on)
{
    _grindMode = on;
    WhisperMaster(on ? "grinding mobs nearby" : "grind off");
    _lastOrder = on ? "grind" : "stop grind";
}

void BotAI::CommandSummon()
{
    _masterTeleportCooldown = 0;
    if (Player* master = GetMaster())
    {
        _movement->Stop();
        _bot->TeleportTo(master->GetMapId(), master->GetPositionX() + frand(-1.5f, 1.5f),
            master->GetPositionY() + frand(-1.5f, 1.5f), master->GetPositionZ(), master->GetOrientation());
        WhisperMaster("on my way to you");
    }
    _lastOrder = "summon";
}

void BotAI::CommandQueue()
{
    std::string reply;
    if (BotQueues::QueueBattleMaster(_bot, reply))
        WhisperMaster(reply + " - I will enter when it is ready");
    else
        WhisperMaster(reply);
    _lastOrder = "queue";
}

void BotAI::CommandLeave()
{
    if (_bot->InBattleground())
    {
        _bot->LeaveBattleground(true);
        WhisperMaster("left the battleground");
    }
    else if (BotQueues::LeaveQueues(_bot))
        WhisperMaster("left the queue(s)");
    else
        WhisperMaster("I am not in a battleground or queue");
    _lastOrder = "leave";
}

void BotAI::CommandGuild()
{
    Player* master = GetMaster();
    WhisperMaster(BotInteract::JoinMastersGuild(_bot, master)
        ? "joined your guild" : "you are not in a guild (or I already am)");
    _lastOrder = "guild";
}

void BotAI::CommandGuildLeave()
{
    WhisperMaster(BotInteract::LeaveGuild(_bot) ? "left the guild" : "I am not in a guild");
    _lastOrder = "guild leave";
}

void BotAI::CommandSell()
{
    std::string reply;
    BotInteract::SellJunk(_bot, reply);
    WhisperMaster(reply);
    _lastOrder = "sell";
}

void BotAI::CommandQuests()
{
    Player* master = GetMaster();
    uint32 taken = BotInteract::TakeMastersQuests(_bot, master);
    if (taken)
        WhisperMaster("took " + std::to_string(taken) + " of your quests - I will hand them in when done");
    else
        WhisperMaster("nothing of your quest log I can take");
    _lastOrder = "quests";
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
// party services
// ---------------------------------------------------------------------------

std::vector<Unit*> BotAI::GetPartyUnits(float maxDist, bool includePets) const
{
    std::vector<Unit*> roster;
    auto consider = [&](Unit* who)
    {
        if (!who || !who->IsInWorld() || !who->IsAlive() || !_bot->IsInMap(who))
            return;
        if (_bot->GetExactDist(who) > maxDist)
            return;
        if (std::find(roster.begin(), roster.end(), who) != roster.end())
            return;
        roster.push_back(who);
        if (!includePets)
            return;
        if (Player* p = who->ToPlayer())
            if (Pet* pet = p->GetPet())
                if (pet->IsInWorld() && pet->IsAlive() && _bot->IsInMap(pet) && _bot->GetExactDist(pet) <= maxDist)
                    roster.push_back(pet->ToUnit());
    };

    if (Group* group = _bot->GetGroup())
        for (GroupReference const* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            consider(itr->GetSource());
    else if (Player* master = GetMaster())
        consider(master);

    return roster;
}

Unit* BotAI::FindDeadPartyMember(float range) const
{
    Unit* best = nullptr;
    float bestDist = range;
    auto consider = [&](Unit* who)
    {
        if (!who || who->IsAlive() || !who->IsInWorld() || !_bot->IsInMap(who))
            return;
        if (Player* p = who->ToPlayer())
            if (p->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                return;     // released spirit - no body to resurrect
        float const d = _bot->GetExactDist(who);
        if (d <= range && d < bestDist)
        {
            bestDist = d;
            best = who;
        }
    };

    if (Group* group = _bot->GetGroup())
        for (GroupReference const* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            consider(itr->GetSource());
    else if (Player* master = GetMaster())
        consider(master);

    return best;
}

Unit* BotAI::FindDispelTarget(uint32 dispelMask, float range) const
{
    for (Unit* who : GetPartyUnits(range, false))
    {
        DispelChargesList dispelList;
        who->GetDispellableAuraList(_bot, dispelMask, dispelList);
        if (!dispelList.empty())
            return who;
    }
    return nullptr;
}

bool BotAI::HasConsumable(bool drink) const
{
    return FindConsumableSpell(_bot, drink) != 0;
}

bool BotAI::TryConsume(bool drink)
{
    uint32 const spellId = FindConsumableSpell(_bot, drink);
    if (!spellId)
        return false;

    // sit down like a real player; the next SendStart stands us back up
    _bot->SetStandState(UNIT_STAND_STATE_SIT);
    _bot->CastSpell(_bot, spellId, true);
    return true;
}

bool BotAI::CanTank() const
{
    return _classAI->CanTank();
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

    // A healer's resurrection beats the self-res timer; bot members accept it
    // right away (a real player clicks the dialog - ResurrectUsingRequestData
    // is the handler behind that click).
    if (_bot->IsInWorld() && _bot->IsResurrectRequested())
    {
        _bot->ResurrectUsingRequestData();
        _deadTimer = 0;
        return;
    }

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

    // Duels: fight the opponent until the duel is over
    if (_bot->duel && _bot->duel->Opponent)
    {
        if (_bot->duel->Opponent->IsAlive())
        {
            _combat->SetStance(_classAI->IsMeleeClass() ? BotCombatStance::Melee : BotCombatStance::Ranged);
            _combat->SetVictim(_bot->duel->Opponent, "duel");
            return;
        }
    }

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

    // party services first (rez > cure > buffs), then keep ourselves fed,
    // then pick a fight (grind mode) - the follow/stay/wander pass runs last
    UpdatePartyCare(diff);
    UpdateConsume(diff);
    UpdateGrind(diff);

    UpdateNonCombat(diff);
}

void BotAI::UpdatePartyCare(uint32 diff)
{
    if (_partyCareCooldown > diff)
    {
        _partyCareCooldown -= diff;
        return;
    }
    _partyCareCooldown = 2000;

    // resurrection outranks everything: a dead member contributes nothing
    _classAI->RezTick(*this);

    // debuffs next - they do damage or disable
    _classAI->CureTick(*this);

    // buffs only out of combat; in combat the GCDs belong to the rotation
    if (!_combat->HasVictim() && !_bot->IsInCombat())
        _classAI->BuffTick(*this);

    // level-ups grant talent points no client would ever spend for us
    _talentTimer += 2000;
    if (_talentTimer >= 60000)
    {
        _talentTimer = 0;
        if (_bot->GetLevel() >= 10 && _bot->m_activePlayerData->CharacterPoints)
            BotTalents::SpendPoints(_bot);
    }

    // hand finished quests in at nearby quest givers (kill/loot objectives
    // complete themselves while grinding alongside the master)
    _questTimer += 2000;
    if (_questTimer >= 6000)
    {
        _questTimer = 0;
        if (!_combat->HasVictim())
            BotInteract::TurnInCompletedQuests(_bot);
    }
}

void BotAI::UpdateConsume(uint32 diff)
{
    if (_consumeCooldown > diff)
    {
        _consumeCooldown -= diff;
        return;
    }
    _consumeCooldown = 2000;

    bool const forced = _forceConsumeTimer > 0;
    if (forced)
        _forceConsumeTimer = _forceConsumeTimer > diff ? _forceConsumeTimer - diff : 0;

    if (_combat->HasVictim() || _bot->IsInCombat())
        return;
    if (_movement->IsMoving() || _movement->HasLiveGoal())
        return;     // eat/drink only while safely standing still

    float const threshold = forced ? 100.0f : float(sBotConfig->EatDrinkPct);
    bool const hungry = forced || _bot->GetHealthPct() < threshold;
    bool const thirsty = forced || (_bot->GetPowerType() == POWER_MANA && _bot->GetPowerPct(POWER_MANA) < threshold);
    if (!hungry && !thirsty)
        return;

    bool used = false;
    if (thirsty)
        used = TryConsume(true);
    if (!used && hungry)
        TryConsume(false);
}

void BotAI::UpdateGrind(uint32 diff)
{
    // battlegrounds: always engage nearby enemies, no grind mode needed
    if (_bot->InBattleground())
    {
        if (_combat->HasVictim() || _movement->HasLiveGoal())
            return;
        if (_grindScanCooldown > diff)
        {
            _grindScanCooldown -= diff;
            return;
        }
        _grindScanCooldown = 2000;
        if (Unit* enemy = BotQueues::FindEnemyPlayer(_bot, 45.0f))
            Attack(enemy, "battleground");
        return;
    }

    if (!_grindMode || _stay || _combat->HasVictim())
        return;
    if (_movement->HasLiveGoal())
        return;
    if (Player* master = GetMaster())
        if (_bot->IsInMap(master) && master->IsInCombat())
            return;     // the master's fight comes first (assist handles it)

    if (_grindScanCooldown > diff)
    {
        _grindScanCooldown -= diff;
        return;
    }
    _grindScanCooldown = 3000;

    Unit* target = nullptr;
    GrindTargetCheck check(_bot, _bot, sBotConfig->SightDistance);
    Trinity::UnitLastSearcher<GrindTargetCheck> checker(_bot, target, check);
    Cell::VisitAllObjects(_bot, checker, sBotConfig->SightDistance);

    if (target)
        Attack(target, "grind");
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
        if (_masterTeleportCooldown > diff)
            _masterTeleportCooldown -= diff;
        else
            _masterTeleportCooldown = 0;

        // the master took a portal / flight / long road: catch up instead of
        // standing behind forever. Paced so a wandering master cannot yo-yo
        // the bot through instances.
        if (!_bot->IsInMap(master) || _bot->GetDistance(master) > 250.0f)
        {
            if (_masterTeleportCooldown == 0 && !_movement->HasLiveGoal())
            {
                _masterTeleportCooldown = 30000;
                _movement->Stop();
                _bot->TeleportTo(master->GetMapId(), master->GetPositionX() + frand(-1.5f, 1.5f),
                    master->GetPositionY() + frand(-1.5f, 1.5f), master->GetPositionZ(), master->GetOrientation());
                return;
            }
        }

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
