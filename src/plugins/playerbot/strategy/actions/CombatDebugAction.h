#pragma once

#include "../Action.h"

namespace ai
{
    // "combat debug" - whisper a bot (or say it in party/raid chat) and it answers
    // with the one line that explains why it is or is not fighting: every gate the
    // core checks before it lets a swing land (3D melee range, line of sight, the
    // 120 degree facing arc, the swing timer, disarm/pacify), the bot's own
    // movement, teleport and cast state, the config values those are compared
    // against, and the last action trace of the running engine.
    //
    // It works with AiPlayerbot.DebugCombat = 0 as well, so a live realm never has
    // to be reconfigured and restarted to find out why a particular bot is idle.
    class CombatDebugAction : public Action
    {
    public:
        CombatDebugAction(PlayerbotAI* ai) : Action(ai, "combat debug") {}
        virtual bool Execute(Event event);
    };
}
