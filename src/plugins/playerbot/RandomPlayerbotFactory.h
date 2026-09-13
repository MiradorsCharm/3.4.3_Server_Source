#ifndef _RandomPlayerbotFactory_H
#define _RandomPlayerbotFactory_H

#include "Common.h"
#include "PlayerbotAIBase.h"

class WorldPacket;
class Player;
class Unit;
class Object;
class Item;

using namespace std;

class RandomPlayerbotFactory
{
    public:
        RandomPlayerbotFactory(uint32 accountId);
		virtual ~RandomPlayerbotFactory() {}

	public:
        bool CreateRandomBot(uint8 cls);
        static void CreateRandomBots();
        static void CreateRandomGuilds();

	private:
        string CreateRandomBotName();
        static string CreateRandomGuildName();
        // Top the character/guild name pools up to the configured population
        // so operators never have to hand-write hundreds of names.
        static void EnsureNamePool();
        static void EnsureGuildNamePool();

    private:
        uint32 accountId;
        static map<uint8, vector<uint8> > availableRaces;
};

#endif
