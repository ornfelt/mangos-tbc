/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MANGOS_BATTLEGROUNDBOTFILL_H
#define MANGOS_BATTLEGROUNDBOTFILL_H

#if defined(USE_CUSTOM_CHANGES) && defined(ENABLE_PLAYERBOTS)

#include "Common.h"
#include "Policies/Singleton.h"
#include "Globals/SharedDefines.h"
#include "BattleGround/BattleGround.h"
#include "BattleGround/BattleGroundDefines.h"

#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>

class Player;

/**
 * Fills the battleground (or arena skirmish) queue a real player joins with dedicated playerbots,
 * so both teams reach their maximum size and the queue pops right away.
 *
 * The bots come from their own account pool (ACCOUNT_PREFIX, kept out of the random bot rotation):
 * offline pool characters are logged in and set to the queuing player's level, new characters are
 * created when the pool runs short. A bot logs out again once its match is over, or when the player
 * leaves the queue before the match started.
 *
 * Active when the playerbots are enabled together with AiPlayerbot.RandomBotJoinBG and
 * AiPlayerbot.RandomBotAutoJoinBG.
 */
class BattleGroundBotFill
{
    public:
        BattleGroundBotFill();

        // a player joined a battleground or arena queue (called from the join handlers)
        void OnQueueJoin(Player* player, BattleGroundQueueTypeId queueTypeId, bool isRated);
        // world thread update
        void Update(uint32 diff);

    private:
        enum class BotState
        {
            WAIT_LOGIN,     // assigned, waiting for its login (new characters wait for their save)
            LOGGING_IN,     // login sent
            PREPARING,      // in world, gets level / gear and joins the queue
            QUEUED,         // queue join sent; in queue, invited or in the match
            LEAVING         // leaves its match before the logout (a logout inside puts it back there at the next login)
        };

        struct FillKey
        {
            BattleGroundQueueTypeId queueTypeId;
            BattleGroundBracketId bracketId;

            bool operator<(FillKey const& other) const
            {
                return std::tie(queueTypeId, bracketId) < std::tie(other.queueTypeId, other.bracketId);
            }
            bool operator==(FillKey const& other) const
            {
                return queueTypeId == other.queueTypeId && bracketId == other.bracketId;
            }
        };

        // a real player in a queue
        struct FillRequest
        {
            FillKey key;
            uint32 level;
            time_t joinTime;
        };

        struct FillBot
        {
            FillKey key;
            Team team;
            uint32 level;
            BotState state;
            time_t stateTime;           // when the state was entered, for WAIT_LOGIN the earliest login time
        };

        struct PoolChar
        {
            uint32 account;
            Team team;
            uint32 level;
            bool busy;
        };

        struct CreateJob
        {
            FillKey key;
            Team team;
            uint32 level;
        };

        typedef std::map<FillKey, uint32> KeyLevelMap;      // key -> level the bots get
        typedef std::set<FillKey> KeySet;

        bool IsEnabled() const;
        void Initialize();
        void RegisterAccount(uint32 accountId);
        void MarkDedicated(uint32 guid);

        void UpdateRequests(time_t now, KeyLevelMap& waitingKeys, KeySet& activeKeys);
        void FillQueue(FillKey const& key, uint32 level, time_t now);
        void CountQueued(FillKey const& key, bool isArena, uint32 (&count)[PVP_TEAM_COUNT]) const;
        void AddBot(FillKey const& key, Team team, uint32 level, time_t now);
        uint32 PickPoolChar(Team team, uint32 level);

        void UpdateCreateJobs(time_t now, KeySet const& activeKeys);
        uint32 GetAccountWithFreeSlot();
        uint32 CreateBotCharacter(uint32 accountId, Team team);

        void UpdateBots(time_t now, KeySet const& activeKeys);
        bool PrepareBot(Player* bot, FillBot const& fillBot);
        void JoinQueue(Player* bot, FillKey const& key);
        bool KeepQueuedBot(Player* bot, FillBot const& fillBot, bool keyActive, time_t now) const;
        void ReleaseBot(uint32 guid, Player* bot);
        void LogoutStrayPoolBots();

        static bool IsRealPlayer(Player* player);
        static bool IsWaitingInQueue(Player* player, BattleGroundQueueTypeId queueTypeId);
        static bool IsInBattleGroundOfQueue(Player* player, BattleGroundQueueTypeId queueTypeId);
        static bool HasRealPlayer(BattleGround* bg);
        static char const* QueueName(BattleGroundQueueTypeId queueTypeId);

        std::map<std::pair<uint32, BattleGroundQueueTypeId>, FillRequest> m_requests;  // (player guid, queue type)
        std::map<uint32, FillBot> m_bots;                                               // bot guid
        std::map<uint32, PoolChar> m_pool;                                              // character guid
        std::map<uint32, uint32> m_accounts;                                            // account id -> characters
        std::deque<CreateJob> m_createJobs;
        std::string m_pendingAccount;                                                   // account being created
        uint32 m_nextAccountIndex;
        uint32 m_updateTimer;
        time_t m_createBlockedUntil;
        bool m_initialized;
};

#define sBattleGroundBotFill MaNGOS::Singleton<BattleGroundBotFill>::Instance()

#endif

#endif
