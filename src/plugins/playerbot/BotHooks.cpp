/*
 * Playerbot AI - worldserver entry points.
 *
 * Main.cpp calls Playerbot::InitializePlayerbots()/ShutdownPlayerbots()/
 * RegisterPlayerbotScripts(). The hooks themselves are filled in by
 * BotManager::Initialize.
 */

#include "BotManager.h"
#include "BotConfig.h"
#include "Scripting/ScriptMgr.h"
#include "Log.h"

void AddSC_bot_commandscript();

namespace Playerbot
{
    void InitializePlayerbots()
    {
        if (!sBotManager->Initialize())
        {
            TC_LOG_WARN("playerbot", "Playerbots are disabled (AiPlayerbot.Enabled is off) - no bots will appear");
            return;
        }
    }

    void ShutdownPlayerbots()
    {
        sBotManager->Shutdown();
    }

    void RegisterPlayerbotScripts()
    {
        AddSC_bot_commandscript();
    }
}
