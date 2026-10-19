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

#if defined(USE_CUSTOM_CHANGES) && defined(ENABLE_PLAYERBOTS)

#include "Custom/BattleGroundBotFill.h"
#include "Accounts/AccountMgr.h"
#include "BattleGround/BattleGroundMgr.h"
#include "BattleGround/BattleGroundQueue.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Globals/ObjectMgr.h"
#include "Groups/Group.h"
#include "Log/Log.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "World/World.h"

#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/RandomPlayerbotFactory.h"
#include "playerbot/RandomPlayerbotMgr.h"

#include <algorithm>
#include <vector>

INSTANTIATE_SINGLETON_1(BattleGroundBotFill);

namespace
{
    char const* const ACCOUNT_PREFIX            = "BGBOT";          // dedicated accounts, not matched by the random bot prefix
    uint32 const FIRST_ACCOUNT_INDEX            = 1;
    uint32 const CHARS_PER_ACCOUNT              = 9;                // same limit as the random bot accounts
    uint32 const PASSWORD_LENGTH                = 10;
    uint32 const MAX_CLASS_RACE_TRIES           = 10;

    uint32 const UPDATE_INTERVAL                = 500;              // ms
    uint32 const MAX_LOGINS_PER_UPDATE          = 20;
    uint32 const MAX_PREPARES_PER_UPDATE        = 5;                // level / gear setup is the expensive step
    uint32 const MAX_CREATES_PER_UPDATE         = 5;

    time_t const QUEUE_JOIN_GRACE               = 10;               // s, a queue join reaches the player through the bg queue thread
    time_t const NEW_CHAR_LOGIN_DELAY           = 2;                // s, lets the character save reach the database first
    time_t const LOGIN_TIMEOUT                  = 60;               // s
    time_t const LEAVE_TIMEOUT                  = 30;               // s, for the port out of a match
    time_t const CREATE_RETRY_DELAY             = 30;               // s, after a failed character / account creation

    // "logout" event on the pool characters: RandomPlayerbotMgr::AddRandomBots skips characters that have it,
    // so the random bot rotation never logs them in
    char const* const DEDICATED_MARK_EVENT      = "logout";
    int32 const DEDICATED_MARK_VALID            = 10 * 365 * DAY;
    char const* const ADD_EVENT                 = "add";
    char const* const LEVEL_VALUE               = "level";
}

BattleGroundBotFill::BattleGroundBotFill() :
    m_nextAccountIndex(FIRST_ACCOUNT_INDEX), m_updateTimer(0), m_createBlockedUntil(0), m_initialized(false)
{
}

bool BattleGroundBotFill::IsEnabled() const
{
    return sPlayerbotAIConfig.enabled && sPlayerbotAIConfig.randomBotJoinBG && sPlayerbotAIConfig.randomBotAutoJoinBG;
}

void BattleGroundBotFill::OnQueueJoin(Player* player, BattleGroundQueueTypeId queueTypeId, bool isRated)
{
    // rated arenas need arena teams, only battlegrounds and skirmishes are filled
    if (!player || isRated || !IsEnabled() || !IsRealPlayer(player))
        return;

    BattleGroundTypeId bgTypeId = BattleGroundMgr::BgTemplateId(queueTypeId);
    if (!sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId))
        return;

    FillRequest request;
    request.key.queueTypeId = queueTypeId;
    request.key.bracketId = sBattleGroundMgr.GetBattleGroundBracketIdFromLevel(bgTypeId, player->GetLevel());
    request.level = player->GetLevel();
    request.joinTime = time(nullptr);
    m_requests[std::make_pair(player->GetGUIDLow(), queueTypeId)] = request;

    sLog.outString("BattleGroundBotFill: %s (level %u) queued for %s, filling the queue with bots", player->GetName(), request.level, QueueName(queueTypeId));
}

void BattleGroundBotFill::Update(uint32 diff)
{
    if (!IsEnabled())
        return;

    m_updateTimer += diff;
    if (m_updateTimer < UPDATE_INTERVAL)
        return;
    m_updateTimer = 0;

    if (!m_initialized)
        Initialize();

    time_t now = time(nullptr);

    KeyLevelMap waitingKeys;
    KeySet activeKeys;
    UpdateRequests(now, waitingKeys, activeKeys);

    for (auto const& itr : waitingKeys)
        FillQueue(itr.first, itr.second, now);

    UpdateCreateJobs(now, activeKeys);
    UpdateBots(now, activeKeys);
    LogoutStrayPoolBots();
}

void BattleGroundBotFill::Initialize()
{
    m_initialized = true;

    auto accounts = LoginDatabase.PQuery("SELECT id, username FROM account WHERE username LIKE '%s%%'", ACCOUNT_PREFIX);
    if (accounts)
    {
        uint32 const prefixLength = strlen(ACCOUNT_PREFIX);
        do
        {
            Field* fields = accounts->Fetch();
            uint32 accountId = fields[0].GetUInt32();
            std::string name = fields[1].GetCppString();

            uint32 index = uint32(atoi(name.substr(prefixLength).c_str()));
            m_nextAccountIndex = std::max(m_nextAccountIndex, index + 1);
            RegisterAccount(accountId);
        }
        while (accounts->NextRow());
    }

    if (!m_accounts.empty())
    {
        std::string accountList;
        for (auto const& itr : m_accounts)
        {
            if (!accountList.empty())
                accountList += ",";
            accountList += std::to_string(itr.first);
        }

        auto chars = CharacterDatabase.PQuery("SELECT guid, account, race, level FROM characters WHERE account IN (%s)", accountList.c_str());
        if (chars)
        {
            do
            {
                Field* fields = chars->Fetch();
                uint32 guid = fields[0].GetUInt32();

                PoolChar& poolChar = m_pool[guid];
                poolChar.account = fields[1].GetUInt32();
                poolChar.team = Player::TeamForRace(fields[2].GetUInt8());
                poolChar.level = fields[3].GetUInt32();
                poolChar.busy = false;

                ++m_accounts[poolChar.account];
                MarkDedicated(guid);
            }
            while (chars->NextRow());
        }
    }

    sLog.outString("BattleGroundBotFill: %u accounts with %u characters in the battleground bot pool", uint32(m_accounts.size()), uint32(m_pool.size()));
}

void BattleGroundBotFill::RegisterAccount(uint32 accountId)
{
    m_accounts.emplace(accountId, 0);

    // bots only get an AI when their account is a random bot account
    if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
        sPlayerbotAIConfig.randomBotAccounts.push_back(accountId);
}

void BattleGroundBotFill::MarkDedicated(uint32 guid)
{
    if (!sRandomPlayerbotMgr.GetValue(guid, DEDICATED_MARK_EVENT))
        sRandomPlayerbotMgr.SetValue(guid, DEDICATED_MARK_EVENT, 1, "", DEDICATED_MARK_VALID);

    // an "add" event would put the character into the random bot rotation at the next startup
    if (sRandomPlayerbotMgr.GetValue(guid, ADD_EVENT))
        sRandomPlayerbotMgr.SetValue(guid, ADD_EVENT, 0);
}

void BattleGroundBotFill::UpdateRequests(time_t now, KeyLevelMap& waitingKeys, KeySet& activeKeys)
{
    for (auto itr = m_requests.begin(); itr != m_requests.end();)
    {
        Player* owner = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr->first.first), false);
        BattleGroundQueueTypeId queueTypeId = itr->first.second;
        FillRequest const& request = itr->second;

        bool inQueue = owner && owner->InBattleGroundQueueForBattleGroundQueueType(queueTypeId);
        bool joining = owner && !inQueue && now - request.joinTime < QUEUE_JOIN_GRACE;
        if (!inQueue && !joining)
        {
            // left the queue or its match: the bots follow once their own match is over
            itr = m_requests.erase(itr);
            continue;
        }

        activeKeys.insert(request.key);
        if (joining || IsWaitingInQueue(owner, queueTypeId))
            waitingKeys.emplace(request.key, request.level);

        ++itr;
    }
}

void BattleGroundBotFill::FillQueue(FillKey const& key, uint32 level, time_t now)
{
    BattleGroundTypeId bgTypeId = BattleGroundMgr::BgTemplateId(key.queueTypeId);
    BattleGround* bgTemplate = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
    if (!bgTemplate)
        return;

    ArenaType arenaType = BattleGroundMgr::BgArenaType(key.queueTypeId);
    bool isArena = arenaType != ARENA_TYPE_NONE;
    uint32 teamSize = isArena ? uint32(arenaType) : bgTemplate->GetMaxPlayersPerTeam();

    uint32 count[PVP_TEAM_COUNT] = { 0, 0 };
    CountQueued(key, isArena, count);

    uint32 added[PVP_TEAM_COUNT] = { 0, 0 };
    size_t jobsBefore = m_createJobs.size();
    for (uint8 i = 0; i < PVP_TEAM_COUNT; ++i)
    {
        Team team = i == TEAM_INDEX_ALLIANCE ? ALLIANCE : HORDE;
        for (; count[i] + added[i] < teamSize; ++added[i])
            AddBot(key, team, level, now);
    }

    if (added[TEAM_INDEX_ALLIANCE] || added[TEAM_INDEX_HORDE])
        sLog.outString("BattleGroundBotFill: %s level %u - queued A:%u H:%u of %u, adding A:%u H:%u bots (%u new characters)",
                       QueueName(key.queueTypeId), level, count[TEAM_INDEX_ALLIANCE], count[TEAM_INDEX_HORDE], teamSize,
                       added[TEAM_INDEX_ALLIANCE], added[TEAM_INDEX_HORDE], uint32(m_createJobs.size() - jobsBefore));
}

void BattleGroundBotFill::CountQueued(FillKey const& key, bool isArena, uint32 (&count)[PVP_TEAM_COUNT]) const
{
    BattleGroundQueueTypeId queueTypeId = key.queueTypeId;

    // own bots still on their way into the queue (and in arenas the waiting ones too)
    for (auto const& itr : m_bots)
    {
        FillBot const& fillBot = itr.second;
        if (!(fillBot.key == key))
            continue;

        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(itr.first);
        bool inQueue = fillBot.state == BotState::QUEUED && bot && bot->InBattleGroundQueueForBattleGroundQueueType(queueTypeId);
        if (!inQueue)
        {
            if (fillBot.state != BotState::QUEUED || (bot && !bot->InBattleGround()))
                ++count[GetTeamIndexByTeamId(fillBot.team)];
        }
        else if (isArena && IsWaitingInQueue(bot, queueTypeId))
            ++count[GetTeamIndexByTeamId(fillBot.team)];
    }

    for (CreateJob const& job : m_createJobs)
        if (job.key == key)
            ++count[GetTeamIndexByTeamId(job.team)];

    if (!isArena)
    {
        // everyone waiting in this battleground bracket, own bots included
        BattleGroundTypeId bgTypeId = BattleGroundMgr::BgTemplateId(queueTypeId);
        sObjectAccessor.ExecuteOnAllPlayers([&](Player* player)
        {
            if (!player->IsInWorld() || !IsWaitingInQueue(player, queueTypeId))
                return;

            if (sBattleGroundMgr.GetBattleGroundBracketIdFromLevel(bgTypeId, player->GetLevel()) != key.bracketId)
                return;

            ++count[GetTeamIndexByTeamId(player->GetTeam())];
        });

        // requesting players whose queue join has not reached them yet
        for (auto const& itr : m_requests)
        {
            if (!(itr.second.key == key))
                continue;

            Player* owner = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr.first.first), false);
            if (owner && !owner->InBattleGroundQueueForBattleGroundQueueType(queueTypeId))
                ++count[GetTeamIndexByTeamId(owner->GetTeam())];
        }
        return;
    }

    // skirmish queues hold rated teams as well, so only the requesting players and their groups count
    std::set<ObjectGuid> counted;
    for (auto const& itr : m_requests)
    {
        if (!(itr.second.key == key))
            continue;

        Player* owner = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr.first.first), false);
        if (!owner)
            continue;

        std::vector<Player*> members = { owner };
        if (Group* group = owner->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->getSource())
                    members.push_back(member);

        for (Player* member : members)
        {
            bool joining = member == owner && !owner->InBattleGroundQueueForBattleGroundQueueType(queueTypeId);
            if ((joining || IsWaitingInQueue(member, queueTypeId)) && counted.insert(member->GetObjectGuid()).second)
                ++count[GetTeamIndexByTeamId(member->GetTeam())];
        }
    }
}

void BattleGroundBotFill::AddBot(FillKey const& key, Team team, uint32 level, time_t now)
{
    uint32 guid = PickPoolChar(team, level);
    if (!guid)
    {
        CreateJob job;
        job.key = key;
        job.team = team;
        job.level = level;
        m_createJobs.push_back(job);
        return;
    }

    m_pool[guid].busy = true;

    FillBot& fillBot = m_bots[guid];
    fillBot.key = key;
    fillBot.team = team;
    fillBot.level = level;
    fillBot.state = BotState::WAIT_LOGIN;
    fillBot.stateTime = now;
}

uint32 BattleGroundBotFill::PickPoolChar(Team team, uint32 level)
{
    // characters already at the wanted level skip the level / gear setup
    std::vector<uint32> sameLevel;
    std::vector<uint32> otherLevel;
    for (auto const& itr : m_pool)
    {
        PoolChar const& poolChar = itr.second;
        if (poolChar.busy || poolChar.team != team)
            continue;

        if (sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr.first), false))
            continue;

        (poolChar.level == level ? sameLevel : otherLevel).push_back(itr.first);
    }

    std::vector<uint32> const& candidates = sameLevel.empty() ? otherLevel : sameLevel;
    if (candidates.empty())
        return 0;

    return candidates[urand(0, candidates.size() - 1)];
}

void BattleGroundBotFill::UpdateCreateJobs(time_t now, KeySet const& activeKeys)
{
    for (uint32 created = 0; created < MAX_CREATES_PER_UPDATE && !m_createJobs.empty() && now >= m_createBlockedUntil;)
    {
        CreateJob job = m_createJobs.front();
        if (!activeKeys.count(job.key))
        {
            m_createJobs.pop_front();
            continue;
        }

        uint32 accountId = GetAccountWithFreeSlot();
        if (!accountId)
            break;

        m_createJobs.pop_front();

        uint32 guid = CreateBotCharacter(accountId, job.team);
        if (!guid)
        {
            sLog.outError("BattleGroundBotFill: could not create a %s bot character on account %u", job.team == ALLIANCE ? "alliance" : "horde", accountId);
            m_createBlockedUntil = now + CREATE_RETRY_DELAY;
            break;
        }

        FillBot& fillBot = m_bots[guid];
        fillBot.key = job.key;
        fillBot.team = job.team;
        fillBot.level = job.level;
        fillBot.state = BotState::WAIT_LOGIN;
        fillBot.stateTime = now + NEW_CHAR_LOGIN_DELAY;
        ++created;
    }
}

uint32 BattleGroundBotFill::GetAccountWithFreeSlot()
{
    for (auto const& itr : m_accounts)
        if (itr.second < CHARS_PER_ACCOUNT)
            return itr.first;

    if (m_pendingAccount.empty())
    {
        m_pendingAccount = ACCOUNT_PREFIX + std::to_string(m_nextAccountIndex++);

        std::string password;
        for (uint32 i = 0; i < PASSWORD_LENGTH; ++i)
            password += char(urand('a', 'z'));

        AccountOpResult result = sAccountMgr.CreateAccount(m_pendingAccount, password, MAX_EXPANSION);
        if (result != AOR_OK && result != AOR_NAME_ALREADY_EXIST)
        {
            sLog.outError("BattleGroundBotFill: could not create account %s (error %u)", m_pendingAccount.c_str(), uint32(result));
            m_pendingAccount.clear();
            m_createBlockedUntil = time(nullptr) + CREATE_RETRY_DELAY;
        }
        return 0;
    }

    // the account insert is asynchronous, wait until it can be found
    uint32 accountId = sAccountMgr.GetId(m_pendingAccount);
    if (!accountId)
        return 0;

    sLog.outString("BattleGroundBotFill: created account %s for battleground bots", m_pendingAccount.c_str());
    m_pendingAccount.clear();
    RegisterAccount(accountId);
    return accountId;
}

uint32 BattleGroundBotFill::CreateBotCharacter(uint32 accountId, Team team)
{
    RandomPlayerbotFactory factory(accountId);

    uint8 cls = 0;
    uint8 race = 0;
    for (uint32 i = 0; i < MAX_CLASS_RACE_TRIES && !race; ++i)
    {
        cls = factory.GetRandomClass();
        race = factory.GetRandomRace(cls, team);
        if (!RandomPlayerbotFactory::isRaceForTeam(race, team) || !RandomPlayerbotFactory::isAvailableRace(cls, race))
            race = 0;
    }

    if (!race || !factory.CreateRandomBot(cls, race))
        return 0;

    // CreateRandomBot leaves the new character in the object accessor, with a session that has no player
    Player* created = nullptr;
    sObjectAccessor.ExecuteOnAllPlayers([&](Player* player)
    {
        WorldSession* session = player->GetSession();
        if (!created && !player->IsInWorld() && session && session->GetAccountId() == accountId && session->GetPlayer() != player)
            created = player;
    });

    if (!created)
        return 0;

    created->SaveToDB();

    uint32 guid = created->GetGUIDLow();
    PoolChar& poolChar = m_pool[guid];
    poolChar.account = accountId;
    poolChar.team = team;
    poolChar.level = created->GetLevel();
    poolChar.busy = true;
    ++m_accounts[accountId];
    MarkDedicated(guid);

    sLog.outDetail("BattleGroundBotFill: created bot character %s (guid %u) on account %u", created->GetName(), guid, accountId);

    WorldSession* session = created->GetSession();
    sObjectAccessor.RemoveObject(created);
    delete created;
    delete session;
    return guid;
}

void BattleGroundBotFill::UpdateBots(time_t now, KeySet const& activeKeys)
{
    uint32 logins = 0;
    uint32 prepares = 0;

    for (auto itr = m_bots.begin(); itr != m_bots.end();)
    {
        uint32 guid = itr->first;
        FillBot& fillBot = itr->second;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid);
        bool keyActive = activeKeys.count(fillBot.key) != 0;
        bool keep = true;

        switch (fillBot.state)
        {
            case BotState::WAIT_LOGIN:
                if (!keyActive || (!bot && sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false)))
                {
                    keep = false;
                    break;
                }
                if (now < fillBot.stateTime || logins >= MAX_LOGINS_PER_UPDATE)
                    break;

                if (!bot)
                {
                    sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
                    ++logins;
                }
                fillBot.state = BotState::LOGGING_IN;
                fillBot.stateTime = now;
                break;
            case BotState::LOGGING_IN:
                if (bot && bot->IsInWorld() && bot->GetPlayerbotAI())
                {
                    fillBot.state = BotState::PREPARING;
                    fillBot.stateTime = now;
                }
                else if (now - fillBot.stateTime > LOGIN_TIMEOUT)
                {
                    sLog.outError("BattleGroundBotFill: bot %u did not log in", guid);
                    keep = false;
                }
                break;
            case BotState::PREPARING:
                if (!bot || !keyActive)
                {
                    keep = false;
                    break;
                }
                if (prepares >= MAX_PREPARES_PER_UPDATE || !bot->IsInWorld() || bot->IsBeingTeleported())
                    break;

                // logged in inside a match that still runs (it logged out there): leave it first
                if (bot->InBattleGround())
                {
                    bot->LeaveBattleground();
                    break;
                }

                ++prepares;
                if (!PrepareBot(bot, fillBot))
                {
                    keep = false;
                    break;
                }
                JoinQueue(bot, fillBot.key);
                fillBot.state = BotState::QUEUED;
                fillBot.stateTime = now;
                break;
            case BotState::QUEUED:
                keep = bot && KeepQueuedBot(bot, fillBot, keyActive, now);
                break;
            case BotState::LEAVING:
                keep = bot && now - fillBot.stateTime < LEAVE_TIMEOUT &&
                       (bot->InBattleGround() || !bot->IsInWorld() || bot->IsBeingTeleported());
                break;
        }

        if (!keep && bot && bot->InBattleGround() && fillBot.state != BotState::LEAVING)
        {
            bot->LeaveBattleground();
            fillBot.state = BotState::LEAVING;
            fillBot.stateTime = now;
            keep = true;
        }

        if (keep)
        {
            ++itr;
            continue;
        }

        ReleaseBot(guid, bot);
        itr = m_bots.erase(itr);
    }
}

bool BattleGroundBotFill::PrepareBot(Player* bot, FillBot const& fillBot)
{
    BattleGroundTypeId bgTypeId = BattleGroundMgr::BgTemplateId(fillBot.key.queueTypeId);

    if (!bot->GetPlayerbotAI() || bot->InBattleGround() || bot->InBattleGroundQueueForBattleGroundQueueType(fillBot.key.queueTypeId))
        return false;

    if (bot->GetGroup())
        bot->RemoveFromGroup();

    if (bot->GetLevel() != fillBot.level)
    {
        PlayerbotFactory factory(bot, fillBot.level);
        factory.Randomize(false, false);
        sRandomPlayerbotMgr.SetValue(bot, LEVEL_VALUE, fillBot.level);
    }

    // revive, repair, full health / mana, consumables
    sRandomPlayerbotMgr.Refresh(bot);
    // left over from logging out of an earlier match
    bot->RemoveAurasDueToSpell(SPELL_ID_BATTLEGROUND_DESERTER);

    m_pool[bot->GetGUIDLow()].level = bot->GetLevel();

    if (bot->GetLevel() != fillBot.level || !bot->HasFreeBattleGroundQueueId() || !bot->GetBGAccessByLevel(bgTypeId))
    {
        sLog.outError("BattleGroundBotFill: bot %s (level %u) can not join %s for level %u", bot->GetName(), bot->GetLevel(), QueueName(fillBot.key.queueTypeId), fillBot.level);
        return false;
    }

    sLog.outDetail("BattleGroundBotFill: bot %s (%s, level %u) joins %s", bot->GetName(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), QueueName(fillBot.key.queueTypeId));
    return true;
}

void BattleGroundBotFill::JoinQueue(Player* bot, FillKey const& key)
{
    // same as a solo join at a battlemaster (WorldSession::HandleBattlemasterJoinOpcode / HandleBattlemasterJoinArena)
    BattleGroundQueueTypeId queueTypeId = key.queueTypeId;
    BattleGroundTypeId bgTypeId = BattleGroundMgr::BgTemplateId(queueTypeId);
    ArenaType arenaType = BattleGroundMgr::BgArenaType(queueTypeId);
    BattleGround* bgTemplate = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
    BattleGroundBracketId bracketId = sBattleGroundMgr.GetBattleGroundBracketIdFromLevel(bgTypeId, bot->GetLevel());

    AddGroupToQueueInfo info;
    info.team = bot->GetTeam();
    info.clientInstanceId = 0;
    info.mapId = bgTemplate->GetMapId();

    sWorld.GetBGQueue().GetMessager().AddMessage([queueTypeId, botGuid = bot->GetObjectGuid(), info, bgTypeId, bracketId, arenaType](BattleGroundQueue* queue)
    {
        BattleGroundQueueItem& queueItem = queue->GetBattleGroundQueue(queueTypeId);
        GroupQueueInfo* groupInfo = queueItem.AddGroup(botGuid, info, bgTypeId, bracketId, arenaType, false, false, 0, 0);
        uint32 avgTime = queueItem.GetAverageQueueWaitTime(groupInfo, bracketId);

        sWorld.GetMessager().AddMessage([botGuid, queueTypeId, bgTypeId, avgTime, arenaType, mapId = info.mapId](World* /*world*/)
        {
            if (Player* bot = sObjectMgr.GetPlayer(botGuid))
            {
                uint32 queueSlot = bot->AddBattleGroundQueueId(queueTypeId);
                bot->SetBattleGroundEntryPoint();

                WorldPacket data;
                sBattleGroundMgr.BuildBattleGroundStatusPacket(data, true, bgTypeId, 0, false, mapId, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, arenaType, TEAM_NONE);
                bot->GetSession()->SendPacket(data);
            }
        });
        queue->ScheduleQueueUpdate(0, arenaType, queueTypeId, bgTypeId, bracketId);
    });
}

bool BattleGroundBotFill::KeepQueuedBot(Player* bot, FillBot const& fillBot, bool keyActive, time_t now) const
{
    BattleGroundQueueTypeId queueTypeId = fillBot.key.queueTypeId;

    // stays for the whole match while it has real players
    if (bot->InBattleGround())
    {
        BattleGround* bg = bot->GetBattleGround();
        return keyActive || (bg && HasRealPlayer(bg));
    }

    // the AI accepts the invitation
    if (bot->IsInvitedForBattleGroundQueueType(queueTypeId))
        return true;

    if (bot->InBattleGroundQueueForBattleGroundQueueType(queueTypeId))
        return keyActive;

    // the queue join is still on its way - otherwise the match is over
    return now - fillBot.stateTime < QUEUE_JOIN_GRACE;
}

void BattleGroundBotFill::ReleaseBot(uint32 guid, Player* bot)
{
    auto itr = m_pool.find(guid);
    if (itr != m_pool.end())
    {
        itr->second.busy = false;
        if (bot)
            itr->second.level = bot->GetLevel();
    }

    if (bot)
    {
        sLog.outDetail("BattleGroundBotFill: bot %s logs out", bot->GetName());
        sRandomPlayerbotMgr.LogoutPlayerBot(guid);
    }
}

void BattleGroundBotFill::LogoutStrayPoolBots()
{
    // pool characters online without a job, e.g. a login that finished after its timeout
    for (auto const& itr : m_pool)
    {
        if (itr.second.busy)
            continue;

        if (Player* bot = sRandomPlayerbotMgr.GetPlayerBot(itr.first))
            if (bot->IsInWorld() && !bot->GetSession()->isLogingOut())
                sRandomPlayerbotMgr.LogoutPlayerBot(itr.first);
    }
}

bool BattleGroundBotFill::IsRealPlayer(Player* player)
{
    PlayerbotAI* ai = player->GetPlayerbotAI();
    return !ai || ai->IsRealPlayer();
}

bool BattleGroundBotFill::IsWaitingInQueue(Player* player, BattleGroundQueueTypeId queueTypeId)
{
    return player->InBattleGroundQueueForBattleGroundQueueType(queueTypeId) &&
           !player->IsInvitedForBattleGroundQueueType(queueTypeId) &&
           !IsInBattleGroundOfQueue(player, queueTypeId);
}

bool BattleGroundBotFill::IsInBattleGroundOfQueue(Player* player, BattleGroundQueueTypeId queueTypeId)
{
    if (!player->InBattleGround())
        return false;

    BattleGroundTypeId bgTypeId = player->GetBattleGroundTypeId();
    if (BattleGroundMgr::BgArenaType(queueTypeId) != ARENA_TYPE_NONE)
        return bgTypeId == BATTLEGROUND_AA || bgTypeId == BATTLEGROUND_NA || bgTypeId == BATTLEGROUND_BE || bgTypeId == BATTLEGROUND_RL;

    return bgTypeId == BattleGroundMgr::BgTemplateId(queueTypeId);
}

bool BattleGroundBotFill::HasRealPlayer(BattleGround* bg)
{
    for (auto const& itr : bg->GetPlayers())
        if (Player* player = sObjectMgr.GetPlayer(itr.first, false))
            if (IsRealPlayer(player))
                return true;

    return false;
}

char const* BattleGroundBotFill::QueueName(BattleGroundQueueTypeId queueTypeId)
{
    switch (queueTypeId)
    {
        case BATTLEGROUND_QUEUE_AV:  return "Alterac Valley";
        case BATTLEGROUND_QUEUE_WS:  return "Warsong Gulch";
        case BATTLEGROUND_QUEUE_AB:  return "Arathi Basin";
        case BATTLEGROUND_QUEUE_EY:  return "Eye of the Storm";
        case BATTLEGROUND_QUEUE_2v2: return "2v2 skirmish";
        case BATTLEGROUND_QUEUE_3v3: return "3v3 skirmish";
        case BATTLEGROUND_QUEUE_5v5: return "5v5 skirmish";
        default:                     return "unknown queue";
    }
}

#endif
