/*
 * Playerbot AI - chat commands.
 *
 * ".bot" for operators (in game or console): add/remove existing characters
 * as bots and inspect them. The day-to-day orders are whispered to the bot
 * itself (see BotAI::HandleCommand).
 */

#include "BotManager.h"
#include "BotAI.h"
#include "BotConfig.h"
#include "Chat/Chat.h"
#include "Chat/ChatCommands/ChatCommand.h"
#include "Scripting/ScriptMgr.h"
#include "ObjectAccessor.h"
#include "CharacterCache.h"
#include "Player.h"

using namespace Trinity::ChatCommands;

class bot_commandscript : public CommandScript
{
public:
    bot_commandscript() : CommandScript("bot_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable botCommandTable =
        {
            { "add",     HandleAdd,      rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "remove",  HandleRemove,   rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "removeall", HandleRemoveAll, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "list",    HandleList,     rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "info",    HandleInfo,     rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "rndbot",  HandleRndBot,   rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };
        return botCommandTable;
    }

    static bool HandleAdd(ChatHandler* handler, Tail args)
    {
        std::string name(args);
        if (name.empty())
        {
            handler->SendSysMessage("Usage: .bot add <name> [master-name]");
            return false;
        }

        std::string masterName;
        if (auto space = name.find(' '); space != std::string::npos)
        {
            masterName = name.substr(space + 1);
            name = name.substr(0, space);
        }

        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByName(name);
        if (!cache)
        {
            handler->PSendSysMessage("No character named '{}'.", name);
            return false;
        }

        ObjectGuid masterGuid;
        if (!masterName.empty())
        {
            if (CharacterCacheEntry const* masterCache = sCharacterCache->GetCharacterCacheByName(masterName))
                masterGuid = masterCache->Guid;
            else
            {
                handler->PSendSysMessage("No master character named '{}'.", masterName);
                return false;
            }
        }
        else if (Player* invoker = handler->GetPlayer())
            masterGuid = invoker->GetGUID();

        if (sBotManager->AddBot(cache->Guid, masterGuid, false))
        {
            handler->PSendSysMessage("Bot {} is logging in. Whisper it 'follow' or 'attack my target'.", name);
            return true;
        }
        handler->PSendSysMessage("Could not add bot {}.", name);
        return false;
    }

    static bool HandleRemove(ChatHandler* handler, Tail args)
    {
        std::string name(args);
        if (name.empty())
        {
            handler->SendSysMessage("Usage: .bot remove <name>");
            return false;
        }
        if (sBotManager->RemoveBot(name))
        {
            handler->PSendSysMessage("Bot {} is logging out.", name);
            return true;
        }
        handler->PSendSysMessage("No online bot named '{}'.", name);
        return false;
    }

    static bool HandleRemoveAll(ChatHandler* handler, Tail /*args*/)
    {
        sBotManager->RemoveAllBots();
        handler->SendSysMessage("All bots are logging out.");
        return true;
    }

    static bool HandleList(ChatHandler* handler, Tail /*args*/)
    {
        auto bots = sBotManager->GetAllBots();
        if (bots.empty())
        {
            handler->SendSysMessage("No bots online.");
            return true;
        }
        for (Player* bot : bots)
        {
            BotAI* ai = bot->GetBotAI();
            if (!ai)
                continue;
            std::string activity = "idle";
            if (Unit* victim = ai->GetCombat().GetVictim())
                activity = "fighting " + victim->GetName();
            handler->PSendSysMessage("{} [{}] level {} - {} - {}",
                bot->GetName(), ai->IsRandomBot() ? "random" : "owned",
                uint32(bot->GetLevel()), bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", activity);
        }
        return true;
    }

    static bool HandleInfo(ChatHandler* handler, Tail args)
    {
        std::string name(args);
        BotAI* ai = sBotManager->GetAIByName(name);
        if (!ai)
        {
            handler->SendSysMessage("Usage: .bot info <name>");
            return false;
        }
        Player* bot = ai->GetBot();
        handler->PSendSysMessage("Bot {}: health {}/{} mana {}/{} mover mode {} moving {}",
            bot->GetName(), uint32(bot->GetHealth()), uint32(bot->GetMaxHealth()),
            uint32(bot->GetPower(POWER_MANA)), uint32(bot->GetMaxPower(POWER_MANA)),
            uint32(ai->GetMovement().GetMode()), ai->GetMovement().IsMoving() ? "yes" : "no");
        return true;
    }

    static bool HandleRndBot(ChatHandler* handler, Tail args)
    {
        std::string arg(args);
        if (arg.empty())
        {
            handler->PSendSysMessage("Random bot target: {} (AiPlayerbot.RandomBotCount). Online random bots: {}.",
                sBotManager->GetRandomBotTarget(),
                std::to_string(std::count_if(sBotManager->GetAllBots().begin(), sBotManager->GetAllBots().end(),
                    [](Player* b) { return b->GetBotAI() && b->GetBotAI()->IsRandomBot(); })));
            return true;
        }
        handler->SendSysMessage("Set AiPlayerbot.RandomBotCount in worldserver.conf and restart.");
        return true;
    }
};

void AddSC_bot_commandscript()
{
    new bot_commandscript();
}
