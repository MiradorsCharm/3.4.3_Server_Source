#pragma once

#include "../pchdef.h"
#include "PlayerbotMgr.h"
#include "PlayerbotAIBase.h"
#include "strategy/AiObjectContext.h"
#include "strategy/Engine.h"
#include "strategy/ExternalEventHelper.h"
#include "ChatFilter.h"
#include "CombatSnapshotSource.h"
#include "PlayerbotSecurity.h"
#include "Entities/Player/Player.h"
#include <stack>
#include <deque>
#include <mutex>

class Player;
class PlayerbotMgr;
class ChatHandler;

using namespace std;
using namespace ai;

bool IsAlliance(uint8 race);

class PlayerbotChatHandler: protected ChatHandler
{
public:
    explicit PlayerbotChatHandler(Player* pMasterPlayer) : ChatHandler(pMasterPlayer->GetSession()) {}
    void sysmessage(string str) { SendSysMessage(str.c_str()); }
    uint32 extractQuestId(string str);
    uint32 extractSpellId(string str)
    {
        string::size_type pos = str.find("|Hspell:");
        if (pos == string::npos)
            return 0;
        pos += 8;
        uint32 id = 0;
        while (pos < str.size() && isdigit((unsigned char)str[pos]))
        {
            id = id * 10 + (str[pos] - '0');
            ++pos;
        }
        return id;
    }
};

namespace ai
{
	class MinValueCalculator {
	public:
		MinValueCalculator(float def = 0.0f) {
			param = NULL;
			minValue = def;
		}

	public:
		void probe(float value, void* p) {
			if (!param || minValue >= value) {
				minValue = value;
				param = p;
			}
		}

	public:
		void* param;
		float minValue;
	};
};

enum BotState
{
    BOT_STATE_COMBAT = 0,
    BOT_STATE_NON_COMBAT = 1,
    BOT_STATE_DEAD = 2
};

#define BOT_STATE_MAX 3

class PacketHandlingHelper
{
public:
    void AddHandler(uint16 opcode, string handler);
    void Handle(ExternalEventHelper &helper);
    void AddPacket(const WorldPacket& packet);

private:
    map<uint16, string> handlers;
    std::deque<WorldPacket> queue;
    std::mutex queueMutex;
};

class ChatCommandHolder
{
public:
    ChatCommandHolder(string command, Player* owner = NULL, uint32 type = CHAT_MSG_WHISPER) : command(command), owner(owner), type(type) {}
    ChatCommandHolder(ChatCommandHolder const& other)
    {
        this->command = other.command;
        this->owner = other.owner;
        this->type = other.type;
    }

public:
    string GetCommand() { return command; }
    Player* GetOwner() { return owner; }
    uint32 GetType() { return type; }

private:
    string command;
    Player* owner;
    uint32 type;
};

class PlayerbotAI : public PlayerbotAIBase
{
public:
	PlayerbotAI();
	PlayerbotAI(Player* bot);
	virtual ~PlayerbotAI();

public:
	virtual void UpdateAI(uint32 elapsed);
	virtual void UpdateAIInternal(uint32 elapsed);
	string HandleRemoteCommand(string command);
    void HandleCommand(uint32 type, const string& text, Player& fromPlayer);
    /// drop queued chat commands that reference a specific player (logged out)
    void DropCommandsFrom(Player* player);
	void HandleBotOutgoingPacket(const WorldPacket& packet);
    void HandleMasterIncomingPacket(const WorldPacket& packet);
    void HandleMasterOutgoingPacket(const WorldPacket& packet);
	void HandleTeleportAck();
    void ChangeEngine(BotState type);
    void DoNextAction();
    void DoSpecificAction(string name);
    void ChangeStrategy(string name, BotState type);
    bool ContainsStrategy(StrategyType type);
    bool HasStrategy(string name, BotState type);
    void ResetStrategies();
    void ReInitCurrentEngine();
    void Reset();
    bool IsTank(Player* player);
    bool IsHeal(Player* player);
    bool IsRanged(Player* player);

    // --- core-native ranged combat -------------------------------------------
    // The true reach of this bot's ranged attacks, measured the way the core
    // measures it (SpellInfo::GetMaxRange() is the very number
    // Spell::CheckRange() compares against GetExactDist()). Computed from what
    // the bot ACTUALLY has right now - learned damaging spells with a range,
    // plus a ranged weapon it is proficient with - never from class or config
    // guesses. Returns 0 when the bot has no usable ranged attack at all; such
    // a bot fights in melee (the core auto-drives swings, never spells).
    float GetBotAttackRange(Unit* target);

    // The best hostile spell this bot can cast at the target right now
    // (cooldown ready, power affordable, core trial-cast passes), highest
    // spell level first. 0 when there is nothing to cast - callers fall back
    // to melee instead of standing still.
    uint32 FindBestAttackSpell(Unit* target);
    Creature* GetCreature(ObjectGuid guid);
    Unit* GetUnit(ObjectGuid guid);
    GameObject* GetGameObject(ObjectGuid guid);
    bool TellMaster(ostringstream &stream, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL) { return TellMaster(stream.str(), securityLevel); }
    bool TellMaster(string text, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    bool TellMasterNoFacing(string text, PlayerbotSecurityLevel securityLevel = PLAYERBOT_SECURITY_ALLOW_ALL);
    void SpellInterrupted(uint32 spellid);
    int32 CalculateGlobalCooldown(uint32 spellid);
    void InterruptSpell();
    void RemoveAura(string name);
    void RemoveShapeshift();
    void WaitForSpellCast(Spell *spell);
    bool PlaySound(uint32 emote);

    virtual bool CanCastSpell(string name, Unit* target);
    virtual bool CastSpell(string name, Unit* target);
    virtual bool HasAura(string spellName, Unit* player);
    virtual bool HasAnyAuraOf(Unit* player, ...);

    virtual bool IsInterruptableSpellCasting(Unit* player, string spell);
    virtual bool HasAuraToDispel(Unit* player, uint32 dispelType);
    bool CanCastSpell(uint32 spellid, Unit* target, bool checkHasSpell = true);

    // --- combat diagnostics -------------------------------------------------
    // See AiPlayerbot.DebugCombat and the "combat debug" bot chat command.
    // A bot that has a victim but cannot reach or hit it used to stand in its
    // attack animation forever, which is indistinguishable from "the bots are
    // lazy" from outside. These three calls gather the state the core actually
    // checks before it lets a swing land, explain it in one line, and take the
    // bot out of the zombie stance so it can try something else.
    void UpdateCombatDiagnostics(uint32 elapsed);
    CombatSnapshot CaptureCombatSnapshot();
    std::string FormatCombatStatus();

    bool HasAura(uint32 spellId, const Unit* player);
    bool CastSpell(uint32 spellId, Unit* target);
    bool canDispel(const SpellInfo* entry, uint32 dispelType);

    uint32 GetEquipGearScore(Player* player, bool withBags, bool withBank);

private:
    void _fillGearScoreData(Player *player, Item* item, std::vector<uint32>* gearScore, uint32& twoHandScore);

public:
	Player* GetBot() { return bot; }
    Player* GetMaster() { return master; }
    void SetMaster(Player* master) { this->master = master; }
    AiObjectContext* GetAiObjectContext() { return aiObjectContext; }
    ChatHelper* GetChatHelper() { return &chatHelper; }
    bool IsOpposing(Player* player);
    static bool IsOpposing(uint8 race1, uint8 race2);
    PlayerbotSecurity* GetSecurity() { return &security; }

protected:
	Player* bot;
	Player* master;
	uint32 accountId;
    AiObjectContext* aiObjectContext;
    Engine* currentEngine;
    Engine* engines[BOT_STATE_MAX];
    BotState currentState;
    ChatHelper chatHelper;
    stack<ChatCommandHolder> chatCommands;
    PacketHandlingHelper botOutgoingPacketHandlers;
    PacketHandlingHelper masterIncomingPacketHandlers;
    PacketHandlingHelper masterOutgoingPacketHandlers;
    CompositeChatFilter chatFilter;
    PlayerbotSecurity security;

    // Combat diagnostics state (see UpdateCombatDiagnostics). The watchdog tracks
    // how long the bot has been making no progress against one specific victim;
    // combatReportCooldown keeps a permanently broken bot from repeating itself
    // into an unreadable console.
    ObjectGuid combatWatchVictim;
    uint32 combatStallMs = 0;
    float combatBestDistance = 0.0f;
    uint32 combatBestHealth = 0;
    uint32 combatReportCooldownMs = 0;
    uint32 combatDebugTickMs = 0;
};

