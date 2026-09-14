#include "BotAI.h"

#include "BotConfig.h"
#include "BotDiagnostics.h"
#include "BotFactory.h"
#include "BotInteract.h"
#include "BotManager.h"
#include "BotQueues.h"
#include "BotSpawns.h"
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
#include "Server/Packets/ChatPackets.h"
#include "Server/Packets/PartyPackets.h"
#include "Log.h"
#include "Util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

BotAI::BotAI(Player* bot) : _bot(bot)
{
    _movement = std::make_unique<BotMovement>(bot);
    _spells = std::make_unique<BotSpells>(bot);
    _combat = std::make_unique<BotCombat>(this, bot);
    _loot = std::make_unique<BotLoot>(this, bot);
    _hazards = std::make_unique<BotHazards>(this, bot);
    _classAI.reset(CreateBotClassAI(bot->GetClass(), this));
    _followDistance = sBotConfig->FollowDistance;
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
    // Anyone sharing a party/raid with the bot may order it around. This is
    // what makes the group-chat commands actually work: the master is often
    // the raid leader, but the person calling "attack my target" is whoever
    // is looking at the mob.
    if (Group* group = _bot->GetGroup())
        if (group->IsMember(sender->GetGUID()))
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
    // The master normally, but an unbound bot answers whoever ordered it -
    // otherwise a group-chat command from a raid member went nowhere.
    Player* who = GetMaster();
    if (!who)
        who = ObjectAccessor::FindConnectedPlayer(_lastSender);
    if (!who)
        return;
    _bot->Whisper(text, LANG_UNIVERSAL, who);
}

void BotAI::Reply(Player* to, std::string const& text)
{
    if (!to)
        to = GetMaster();
    if (!to)
        return;

    switch (_chatChannel)
    {
        case BotChatChannel::Say:
            _bot->Say(text, LANG_UNIVERSAL);
            return;
        case BotChatChannel::Party:
        {
            // exactly what a player's /p ends in (WorldSession chat handler)
            Group* group = _bot->GetGroup();
            if (!group)
                break;
            ChatMsg const type = group->IsLeader(_bot->GetGUID()) ? CHAT_MSG_PARTY_LEADER : CHAT_MSG_PARTY;
            WorldPackets::Chat::Chat packet;
            packet.Initialize(type, LANG_UNIVERSAL, _bot, nullptr, text);
            group->BroadcastPacket(packet.Write(), false, group->GetMemberGroup(_bot->GetGUID()));
            return;
        }
        case BotChatChannel::Whisper:
        default:
            break;
    }

    _bot->Whisper(text, LANG_UNIVERSAL, to);
}

void BotAI::HandleCommand(std::string const& msg, Player* sender)
{
    if (!AcceptsCommandsFrom(sender))
        return;

    _lastSender = sender->GetGUID();

    std::string command = msg;

    // trim
    auto trim = [](std::string& s)
    {
        size_t const b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
        {
            s.clear();
            return;
        }
        size_t const e = s.find_last_not_of(" \t\r\n");
        s = s.substr(b, e - b + 1);
    };
    trim(command);

    // strip a leading "!" if someone types it out of habit
    if (!command.empty() && (command[0] == '!' || command[0] == '.'))
        command.erase(0, 1);

    // a leading "bot" is how people talk to bots in a group ("bot follow")
    if (command.rfind("bot ", 0) == 0)
        command.erase(0, 4);

    // name addressing: "Kaeo attack", "Kaeo, follow". In party chat every bot
    // hears every line, so the name is how you talk to one of them.
    auto iequals = [](std::string const& a, std::string const& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };

    if (size_t const comma = command.find(','); comma != std::string::npos)
    {
        std::string head = command.substr(0, comma);
        trim(head);
        if (iequals(head, _bot->GetName()))
        {
            command = command.substr(comma + 1);
            trim(command);
        }
    }
    else if (command.size() > _bot->GetName().size() + 1
        && iequals(command.substr(0, _bot->GetName().size()), _bot->GetName())
        && command[_bot->GetName().size()] == ' ')
    {
        command = command.substr(_bot->GetName().size() + 1);
        trim(command);
    }

    if (command.empty())
        return;

    // lowercase copy for matching; the original keeps the case of arguments
    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return char(std::tolower(c)); });

    auto startsWith = [&lower](char const* prefix)
    {
        return lower.rfind(prefix, 0) == 0;
    };

    // the argument of "buy <name>" / "cast <name>" / "emote <name>"
    auto argAfter = [&command](size_t prefixLength)
    {
        std::string rest = command.size() > prefixLength ? command.substr(prefixLength) : std::string();
        size_t const b = rest.find_first_not_of(" \t");
        return b == std::string::npos ? std::string() : rest.substr(b);
    };

    // --- movement / formation ------------------------------------------------
    if (startsWith("follow "))
        CommandFollowMode(argAfter(6));
    else if (startsWith("follow") || startsWith("heel"))
        CommandFollow();
    else if (startsWith("formation "))
        CommandFormation(argAfter(9));
    else if (startsWith("formation"))
        CommandFormation("");
    else if (startsWith("stay"))
        CommandStay();
    else if (startsWith("come") || startsWith("here"))
        CommandCome(sender);
    else if (startsWith("move out") || startsWith("spread"))
        CommandMoveOut();
    else if (startsWith("summon"))
        CommandSummon();
    else if (startsWith("flee") || startsWith("runaway"))
        CommandFlee();

    // --- combat --------------------------------------------------------------
    else if (startsWith("stop attack") || startsWith("stopattack"))
        CommandStopAttack();
    else if (startsWith("stop grind"))
        CommandGrind(false);
    else if (startsWith("tank attack"))
        CommandTankAttack(sender);
    else if (startsWith("attack my target") || startsWith("attack") || startsWith("kill"))
        CommandAttackMyTarget(sender);
    else if (startsWith("assist"))
        CommandAssist(sender);
    else if (startsWith("grind"))
        CommandGrind(true);
    else if (startsWith("tank"))
        CommandTank();
    else if (startsWith("dps"))
        CommandDps();
    else if (startsWith("max dps"))
        CommandSaveMana(false);
    else if (startsWith("save mana"))
        CommandSaveMana(true);

    // --- party services ------------------------------------------------------
    else if (startsWith("heal"))
        CommandHeal();
    else if (startsWith("buff"))
        CommandBuff();
    else if (startsWith("rez") || startsWith("resurrect") || startsWith("revive") || lower == "res")
        CommandRez();
    else if (startsWith("cure") || startsWith("dispel"))
        CommandCure();
    else if (startsWith("drink"))
        CommandDrink();
    else if (startsWith("eat"))
        CommandEat();

    // --- loot -----------------------------------------------------------------
    else if (startsWith("add all loot") || startsWith("loot all") || lower == "ll")
        CommandLoot();
    else if (startsWith("loot"))
        CommandLoot();

    // --- character / items ----------------------------------------------------
    else if (startsWith("upgrade") || lower == "e")
        CommandUpgrade();
    else if (startsWith("repair"))
        CommandRepair();
    else if (startsWith("sell") || lower == "s")
        CommandSell();
    else if (startsWith("buy"))
        CommandBuy(argAfter(3));
    else if (startsWith("talents"))
    {
        uint32 spent = BotTalents::SpendPoints(_bot);
        Reply(sender, spent ? ("spent " + std::to_string(spent) + " talent point(s)") : "no free talent points");
    }
    else if (startsWith("spells"))
        CommandSpells(sender);
    else if (startsWith("cast ") || startsWith("spell "))
        CommandCastNamed(argAfter(lower[0] == 'c' ? 4 : 5), sender);
    else if (startsWith("emote"))
        CommandEmote(argAfter(5));

    // --- quests ---------------------------------------------------------------
    else if (startsWith("quests") || startsWith("quest") || lower == "q")
        CommandQuests();
    else if (startsWith("accept"))
        CommandAcceptQuests();

    // --- group / world ---------------------------------------------------------
    else if (startsWith("invite"))
        CommandInvite(sender);
    else if (startsWith("guild leave"))
        CommandGuildLeave();
    else if (startsWith("guild"))
        CommandGuild();
    else if (startsWith("queue"))
        CommandQueue();
    else if (startsWith("leave"))
        CommandLeave();

    // --- reporting -------------------------------------------------------------
    else if (startsWith("position") || startsWith("where"))
        CommandPosition(sender);
    else if (startsWith("who"))
        CommandWho(sender);
    else if (startsWith("stats") || startsWith("status"))
        CommandStats(sender);
    else if (startsWith("chat"))
        CommandChat(argAfter(4));
    else if (startsWith("reset ai") || startsWith("reset"))
        CommandResetAI();
    else if (startsWith("release"))
    {
        if (_bot->getDeathState() == CORPSE || _bot->getDeathState() == DEAD)
            _deadTimer = sBotConfig->ReviveDelayMs;  // fast-path the self-res
        Reply(sender, "releasing");
    }
    else if (startsWith("help") || lower == "?")
        Reply(sender,
            "movement: follow [far|near|melee|ranged], formation, stay, come, move out, summon, flee | "
            "combat: attack my target, assist, tank attack, stop attack, grind, stop grind, tank, dps, "
            "max dps, save mana | "
            "party: heal, buff, rez, cure, eat, drink | "
            "items: loot, upgrade, repair, sell, buy <name>, talents, spells, cast <name> | "
            "quests: quests, accept | "
            "group: invite, leave, guild, queue | "
            "misc: position, who, stats, chat [say|party|whisper], emote, release, reset ai");
    else
        Reply(sender, "I don't know '" + command + "' - say 'help' for the list");
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
    Reply(to, text);
}

// ---------------------------------------------------------------------------
// extended order set
// ---------------------------------------------------------------------------

namespace
{
    // "far|near|melee|ranged" -> a distance in yards. 0 means "whatever the
    // config says".
    float FollowDistanceFor(std::string const& mode)
    {
        if (mode == "melee")
            return 2.0f;
        if (mode == "near")
            return 4.0f;
        if (mode == "far")
            return 12.0f;
        if (mode == "ranged")
            return 20.0f;
        return 0.0f;
    }
}

void BotAI::CommandFollowMode(std::string const& mode)
{
    float const distance = FollowDistanceFor(mode);
    if (distance <= 0.0f)
    {
        _followDistance = sBotConfig->FollowDistance;
        CommandFollow();
        Reply(nullptr, "following at the default distance");
        return;
    }
    _followDistance = distance;
    CommandFollow();
    Reply(nullptr, "following at " + std::to_string(uint32(distance)) + " yards");
}

void BotAI::CommandFormation(std::string const& mode)
{
    // mangosbot wording for the same thing: how tightly the group walks
    CommandFollowMode(mode);
}

void BotAI::CommandMoveOut()
{
    // everyone spreads out around where they stand, on a stable per-bot bearing
    // so two bots do not pick the same spot
    float const slot = float(_bot->GetGUID().GetCounter() % 12) * (float(M_PI) / 6.0f);
    float const distance = 6.0f + float(_bot->GetGUID().GetCounter() % 5) * 2.0f;
    float const x = _bot->GetPositionX() + std::cos(slot) * distance;
    float const y = _bot->GetPositionY() + std::sin(slot) * distance;
    float z = _bot->GetPositionZ();
    _bot->UpdateGroundPositionZ(x, y, z);

    _stay = true;
    _stayPoint = Position(x, y, z, slot);
    _movement->MoveTo(x, y, z, 0.5f);
    _lastOrder = "move out";
    Reply(nullptr, "moving out");
}

void BotAI::CommandFlee()
{
    if (Unit* victim = _combat->GetVictim())
    {
        FleeFrom(victim, "ordered to flee");
        Reply(nullptr, "breaking off");
        return;
    }
    Reply(nullptr, "I am not fighting anything");
}

void BotAI::CommandTankAttack(Player* sender)
{
    if (!sender)
        return;

    bool const canTank = CanTank();
    if (canTank)
        _tankMode = true;

    if (Unit* target = sender->GetSelectedUnit())
        Attack(target, "ordered: tank the sender's target");

    _lastOrder = "tank attack";
    Reply(sender, canTank ? "tanking your target" : "I am not built to tank - attacking it anyway");
}

void BotAI::CommandPosition(Player* to)
{
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "map %u zone %u at %.1f %.1f %.1f",
        _bot->GetMapId(), _bot->GetZoneId(),
        _bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ());
    Reply(to, buffer);
}

void BotAI::CommandWho(Player* to)
{
    if (!to)
        return;

    Group* group = _bot->GetGroup();
    uint32 listed = 0;
    for (Player* bot : sBotManager->GetAllBots())
    {
        if (bot->GetGroup() != group)
            continue;
        if (listed >= 10)
        {
            Reply(to, "... and more");
            return;
        }
        BotAI* ai = bot->GetBotAI();
        std::string activity = "idle";
        if (ai)
            if (Unit* victim = ai->GetCombat().GetVictim())
                activity = "fighting " + victim->GetName();
        Reply(to, bot->GetName() + " - level " + std::to_string(uint32(bot->GetLevel())) + ", " + activity);
        ++listed;
    }
    if (!listed)
        Reply(to, "I am the only bot in this group");
}

void BotAI::CommandStats(Player* to)
{
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
        "%s - level %u class %u, health %u/%u, power %u/%u, %s, %s, %s%s",
        _bot->GetName().c_str(), uint32(_bot->GetLevel()), uint32(_bot->GetClass()),
        uint32(_bot->GetHealth()), uint32(_bot->GetMaxHealth()),
        uint32(_bot->GetPower(_bot->GetPowerType())), uint32(_bot->GetMaxPower(_bot->GetPowerType())),
        _combat->HasVictim() ? "in combat" : "out of combat",
        _tankMode ? "tank" : (_grindMode ? "grinding" : "dps"),
        _saveMana ? "saving mana" : "spending freely",
        _stay ? ", holding position" : "");
    Reply(to, buffer);
}

void BotAI::CommandSpells(Player* to)
{
    if (!to)
        return;

    uint32 count = 0;
    std::string list;
    for (auto const& [spellId, playerSpell] : _bot->GetSpellMap())
    {
        if (playerSpell.disabled || !playerSpell.active)
            continue;
        ++count;
        if (count <= 12)
        {
            if (!list.empty())
                list += ", ";
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
            char const* name = (info && info->SpellName) ? (*info->SpellName)[LOCALE_enUS] : nullptr;
            list += (name && *name) ? name : std::to_string(spellId);
        }
    }

    Reply(to, std::to_string(count) + " spell(s) known" + (list.empty() ? "" : (": " + list)));
}

void BotAI::CommandBuy(std::string const& what)
{
    std::string reply;
    BotInteract::BuyItemByName(_bot, what, reply);
    Reply(nullptr, reply);
    _lastOrder = "buy " + what;
}

void BotAI::CommandAcceptQuests()
{
    Player* who = ObjectAccessor::FindConnectedPlayer(_lastSender);
    if (!who)
        who = GetMaster();
    uint32 const taken = who ? BotInteract::TakeMastersQuests(_bot, who) : 0;
    Reply(nullptr, taken ? ("accepted " + std::to_string(taken) + " quest(s)") : "nothing to accept");
    _lastOrder = "accept";
}

void BotAI::CommandCastNamed(std::string const& name, Player* sender)
{
    if (name.empty())
    {
        Reply(sender, "cast what? give me the spell name");
        return;
    }

    std::string needle = name;
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](unsigned char c) { return char(std::tolower(c)); });

    uint32 found = 0;
    for (auto const& [spellId, playerSpell] : _bot->GetSpellMap())
    {
        if (playerSpell.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info || !info->SpellName)
            continue;
        char const* raw = (*info->SpellName)[LOCALE_enUS];
        if (!raw || !*raw)
            continue;
        std::string spellName = raw;
        std::transform(spellName.begin(), spellName.end(), spellName.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        if (spellName.find(needle) != std::string::npos)
        {
            found = spellId;
            break;
        }
    }

    if (!found)
    {
        Reply(sender, "I do not know a spell called '" + name + "'");
        return;
    }

    Unit* target = sender ? sender->GetSelectedUnit() : nullptr;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(found, DIFFICULTY_NONE);
    if (!target || (info && info->IsPositive()))
        target = _bot;

    if (_spells->Cast(found, target))
        Reply(sender, "casting " + std::string((*info->SpellName)[LOCALE_enUS]));
    else
        Reply(sender, "I cannot cast that right now");
}

void BotAI::CommandInvite(Player* sender)
{
    if (!sender)
        return;
    if (_bot->GetGroup())
    {
        Reply(sender, "I am already in a group");
        return;
    }

    WorldPackets::Party::PartyInviteClient invite{WorldPacket(CMSG_PARTY_INVITE)};
    invite.TargetName = sender->GetName();   // the handler resolves by name
    _bot->GetSession()->HandlePartyInviteOpcode(invite);
    Reply(sender, "inviting you");
}

void BotAI::CommandEmote(std::string const& name)
{
    if (name.empty())
    {
        Reply(nullptr, "emote what?");
        return;
    }
    _bot->TextEmote(name);
}

void BotAI::CommandSaveMana(bool on)
{
    _saveMana = on;
    _maxDps = !on;
    Reply(nullptr, on ? "conserving resources" : "going all out");
    _lastOrder = on ? "save mana" : "max dps";
}

void BotAI::CommandChat(std::string const& mode)
{
    if (mode == "say")
        _chatChannel = BotChatChannel::Say;
    else if (mode == "party")
        _chatChannel = BotChatChannel::Party;
    else
        _chatChannel = BotChatChannel::Whisper;
    Reply(nullptr, "answers go to " + mode);
}

void BotAI::CommandResetAI()
{
    _combat->ClearVictim("reset");
    _movement->Stop();
    _stay = false;
    _stayPoint.reset();
    _tankMode = false;
    _saveMana = false;
    _maxDps = false;
    _fleeTarget.Clear();
    _fleeTimer = 0;
    _followDistance = sBotConfig->FollowDistance;
    _chatChannel = BotChatChannel::Whisper;
    _grindMode = _randomBot && sBotConfig->Grind;
    _lastOrder = "reset";
    Reply(nullptr, "orders cleared");
}

// ---------------------------------------------------------------------------
// running away
// ---------------------------------------------------------------------------

void BotAI::FleeFrom(Unit* attacker, std::string reason)
{
    if (!attacker)
        return;

    _combat->ClearVictim("fleeing: " + reason);
    _movement->Stop();
    _stay = false;
    _stayPoint.reset();
    _fleeTarget = attacker->GetGUID();
    _fleeTimer = 10000;
    _fleeCooldown = 0;
    _lastOrder = std::move(reason);
}

bool BotAI::UpdateFlee(uint32 diff)
{
    if (_fleeTimer == 0 || _fleeTarget.IsEmpty())
        return false;

    if (_fleeTimer > diff)
        _fleeTimer -= diff;
    else
    {
        _fleeTimer = 0;
        _fleeTarget.Clear();
        return false;
    }

    Unit* threat = ObjectAccessor::GetUnit(*_bot, _fleeTarget);
    if (!threat || !threat->IsAlive() || !_bot->IsInMap(threat))
    {
        _fleeTarget.Clear();
        _fleeTimer = 0;
        return false;
    }

    // already running: let the path play out
    if (_movement->IsMoving())
        return true;

    if (_fleeCooldown > diff)
        _fleeCooldown -= diff;
    else
        _fleeCooldown = 0;
    if (_fleeCooldown != 0)
        return true;

    float const away = _bot->GetAbsoluteAngle(threat) + float(M_PI);
    static float const fan[] = { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.6f, -1.6f };
    for (float offset : fan)
    {
        float const angle = away + offset;
        float const x = _bot->GetPositionX() + std::cos(angle) * 30.0f;
        float const y = _bot->GetPositionY() + std::sin(angle) * 30.0f;
        float z = _bot->GetPositionZ();
        _bot->UpdateGroundPositionZ(x, y, z);
        if (std::fabs(z - _bot->GetPositionZ()) > 20.0f)
            continue;                       // would walk off something
        if (_hazards->IsSpotDangerous(x, y, z) ||
            _hazards->IsPathDangerous(_bot->GetPositionX(), _bot->GetPositionY(), x, y))
            continue;

        _movement->MoveTo(x, y, z, 1.0f, true);
        _fleeCooldown = 2000;
        return true;
    }

    return true;    // boxed in: keep the fight away from the normal brain
}

// ---------------------------------------------------------------------------
// persistent state
// ---------------------------------------------------------------------------

void BotAI::ApplySavedState(BotSavedState const& state)
{
    _tankMode = state.tankMode;
    _grindMode = state.grindMode || (_randomBot && sBotConfig->Grind);
    _stay = state.stay;
    if (_stay && (state.stayX != 0.0f || state.stayY != 0.0f))
        _stayPoint = Position(state.stayX, state.stayY, state.stayZ, 0.0f);
    _lastOrder = "restored";
}

BotSavedState BotAI::BuildState() const
{
    BotSavedState state = BotState::Load(_bot->GetGUID());
    state.guid = _bot->GetGUID();
    state.random = _randomBot;
    state.tankMode = _tankMode;
    state.grindMode = _grindMode;
    state.stay = _stay;
    if (_stayPoint)
    {
        state.stayX = _stayPoint->GetPositionX();
        state.stayY = _stayPoint->GetPositionY();
        state.stayZ = _stayPoint->GetPositionZ();
    }
    else
    {
        state.stayX = state.stayY = state.stayZ = 0.0f;
    }
    state.preparedLevel = _bot->GetLevel();
    state.lastSeen = uint32(time(nullptr));
    state.present = true;
    return state;
}

void BotAI::SaveState()
{
    if (!sBotConfig->PersistBots)
        return;
    BotSavedState state = BuildState();
    if (!state.master.IsPlayer())
        state.master = _masterGuid;
    BotState::Save(state);
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

    // Dungeon/raid mechanic awareness runs before everything else: standing in
    // fire kills faster than any rotation helps, so dodging a hazard preempts
    // the combat/loot/follow brain for this tick. When it takes control it owns
    // the movement goal and we skip the normal brain so the two do not fight.
    if (UpdateHazardAvoidance(diff))
    {
        _movement->Update(diff);
        UpdateDiagnostics(diff);
        return;
    }

    // Brain order matters: combat first (it owns the movement goal), then
    // loot (out of combat only), then follow/stay/wander, then diagnostics.
    UpdateBrain(diff);

    // Movement runs after the brain so a freshly issued goal is acted on in
    // the same tick.
    _movement->Update(diff);

    UpdateDiagnostics(diff);
}

// ---------------------------------------------------------------------------
// dungeon / raid mechanic awareness
// ---------------------------------------------------------------------------

bool BotAI::UpdateHazardAvoidance(uint32 diff)
{
    if (!sBotConfig->AvoidGroundHazards)
        return false;

    if (_hazardReactCooldown > diff)
        _hazardReactCooldown -= diff;
    else
        _hazardReactCooldown = 0;

    // Scan the floor every tick (the module throttles the expensive grid sweep
    // internally and keeps the "am I standing in it" flag fresh in between).
    _hazards->Scan(diff);

    // A bot the core has taken control of (stunned in the fire, knocked back,
    // on a vehicle) cannot walk out on its own; let the core resolve it.
    if (_bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || _bot->GetVehicle())
    {
        _dodging = false;
        return false;
    }

    if (!_hazards->InDanger())
    {
        // We just finished escaping: clear the dodge goal so the normal brain
        // takes over cleanly next tick.
        if (_dodging)
        {
            _dodging = false;
            _movement->Stop();
        }
        return false;
    }

    // Standing in something harmful. Find a safe spot near our current fight
    // (so a melee bot only sidesteps) and go there. Repath is paced so we do
    // not spam new destinations every tick while already running clear.
    if (!_dodging || _hazardReactCooldown == 0)
    {
        Unit* anchor = _combat->GetVictim();
        if (!anchor)
            anchor = GetMaster();

        Position safe;
        if (_hazards->FindSafeSpot(safe, anchor))
        {
            _hazardReactCooldown = 500;
            _dodging = true;
            // Face the spot and run flat-out; do not stop short (a hazard edge
            // still ticks), so use a tight arrival distance.
            _movement->MoveTo(safe.GetPositionX(), safe.GetPositionY(), safe.GetPositionZ(), 0.5f);

            // Keep the melee auto-swing / auto-shot state alive against the
            // current victim while we relocate - we are dodging, not dropping
            // the fight.
            if (sBotConfig->DebugMove)
                TC_LOG_DEBUG("playerbot", "[hazard] {} dodging to {:.1f},{:.1f}",
                    _bot->GetName(), safe.GetPositionX(), safe.GetPositionY());
            return true;
        }

        // No safe spot found (boxed in). Fall through to the normal brain rather
        // than freezing - at least the bot keeps fighting.
        _dodging = false;
        return false;
    }

    // Already running to a safe spot this pass.
    return true;
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

    // Running away: do not immediately re-acquire whatever we are running from.
    if (_fleeTimer > 0)
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
            // Retaliating against something far above our level is what filled
            // the log with stall reports: the bot stood in range, facing,
            // attacking - and could not land a single hit. A player runs.
            if (sBotConfig->HopelessLevelGap > 0 && attacker->ToCreature() && !attacker->ToPlayer()
                && !attacker->ToPet()
                && int32(attacker->GetLevel()) - int32(_bot->GetLevel()) > sBotConfig->HopelessLevelGap)
            {
                FleeFrom(attacker, "attacker is out of my league");
                return;
            }

            _combat->SetStance(_classAI->IsMeleeClass() ? BotCombatStance::Melee : BotCombatStance::Ranged);
            _combat->SetVictim(attacker, "retaliate");
        }
    }
}

void BotAI::UpdateBrain(uint32 diff)
{
    // Running away from a fight we cannot win owns the tick: re-engaging would
    // just put us back where we started.
    if (UpdateFlee(diff))
        return;

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
            // A stable per-bot formation slot: spread followers around a ~150
            // degree arc behind the master (derived from the low bits of the
            // GUID so it never changes) instead of stacking on one point.
            float const slot = (float(_bot->GetGUID().GetCounter() % 7) - 3.0f) * (float(M_PI) / 8.0f);

            // "follow far|near|melee|ranged" / "formation ..." change this
            float const followAt = GetFollowDistance();

            float dist = _bot->GetExactDist2d(master);
            if (dist > followAt + 2.0f)
                _movement->Follow(master, followAt, slot);
            else
            {
                _movement->Stop();
                // face the same way as the master while idle, like real party
                // members stand
                if (dist <= followAt + 1.0f && !_movement->HasLiveGoal())
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
    // Roaming: a bot that has settled in one area moves on to another
    // level-matching part of the world after the configured time, so the pool
    // drifts instead of camping one clearing forever.
    if (sBotConfig->RandomBotRelocateMinutes > 0 && !_combat->HasVictim() && !_bot->IsInCombat())
    {
        _relocateTimer += diff;
        uint32 const relocateMs = sBotConfig->RandomBotRelocateMinutes * 60 * IN_MILLISECONDS;
        if (_relocateTimer >= relocateMs)
        {
            _relocateTimer = 0;
            if (BotSpawns::PlaceRandomBot(_bot, sBotConfig->WanderRadius * 4.0f))
            {
                _movement->Stop();
                return;
            }
        }
    }

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
