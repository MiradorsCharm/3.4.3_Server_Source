#include "BotTalents.h"

#include "Player.h"
#include "DB2Stores.h"
#include "Log.h"

#include <algorithm>
#include <vector>

namespace
{
    struct TalentPick
    {
        uint32 id;
        uint8 tier;
        uint16 tab;
        uint8 column;
    };
}

uint32 BotTalents::SpendPoints(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return 0;

    uint32 freePoints = bot->m_activePlayerData->CharacterPoints;
    if (!freePoints)
        return 0;

    uint8 const cls = bot->GetClass();

    std::vector<TalentPick> picks;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        if (TalentEntry const* talent = sTalentStore.LookupEntry(i))
            if (talent->ClassID == cls)
                picks.push_back({ talent->ID, talent->TierID, talent->TabID, talent->ColumnIndex });

    // tier by tier, tab by tab: a deterministic hybrid build. LearnTalent
    // itself refuses anything illegal (prereqs, ranks, points), so the worst
    // outcome of the ordering is "wasted point", never "broken character".
    std::sort(picks.begin(), picks.end(), [](TalentPick const& a, TalentPick const& b)
    {
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.tab != b.tab) return a.tab < b.tab;
        return a.column < b.column;
    });

    uint32 spent = 0;
    for (TalentPick const& pick : picks)
    {
        if (!bot->m_activePlayerData->CharacterPoints)
            break;
        // learn rank after rank while the core accepts it
        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
        {
            if (!bot->LearnTalent(pick.id, rank))
                break;
            ++spent;
        }
    }

    if (spent)
    {
        TC_LOG_INFO("playerbot", "{} spent {} talent point(s)", bot->GetName(), spent);
        bot->SaveToDB();
    }
    return spent;
}
