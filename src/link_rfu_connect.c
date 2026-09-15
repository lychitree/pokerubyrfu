#include "global.h"
#include "link.h"
#include "link_rfu.h"
#include "union_room.h"
#include "malloc.h"
#include "task.h"
#include "script.h"
#include "event_data.h"
#include "constants/union_room.h"

// Wireless cable-club trade connection driver.
//
// This is a fresh, pokeruby-native reimplementation of the connection half of
// Emerald's cable-club wireless flow (TryBecomeLinkLeader / TryJoinLinkGroup in
// union_room.c). It drives the already-ported RFU API (AgbRfu_LinkManager +
// link_rfu_2/3) through the exact leader/child handshake a retail Emerald cable
// club uses -- discover via gHostRfuGameData's activity byte (ACTIVITY_TRADE),
// leader accepts a child with RFU_STATUS_JOIN_GROUP_OK, both sides run the
// player-data exchange (gReceivedRemoteLinkPlayers), then hand off to the
// existing (transport-agnostic) Trade Center.
//
// Scope: 2-player trade only (GROUP_MAX=2, GROUP_MIN=0). Emerald's rich list/UI
// (player-select windows, yes/no accept prompts, wireless status sprite) is
// intentionally dropped -- none of it is on the wire, so a real Emerald still
// connects. The leader auto-accepts the first compatible child; the child
// auto-connects to the first ACTIVITY_TRADE parent it discovers.
//
// NOTE: this is first-light connection code. Per CLAUDE.md, RFU handshake bugs
// only surface on hardware; expect to iterate against real-hardware behavior.

#define LINKUP_ONGOING           0
#define LINKUP_SUCCESS           1
#define LINKUP_FAILED            5
#define LINKUP_RETRY_ROLE_ASSIGN 8

// gPlayerCurrActivity is declared in union_room.h and read by the ported
// link_rfu_2/3 code (SetHostRfuGameData callers, etc.); it lived in the
// unported union_room.c, so it's defined here.
u8 gPlayerCurrActivity;

static const struct RfuPlayerData sDummyRfu = {0};

// ---- Leader states ----
enum {
    LEADER_INIT,
    LEADER_AWAIT_CHILD,
    LEADER_ACCEPT,
    LEADER_UPDATE_AFTER_ACCEPT,
    LEADER_FINAL_CHECK,
    LEADER_START_ACTIVITY,
    LEADER_FAIL,
};

// ---- Child states ----
enum {
    CHILD_INIT,
    CHILD_SCAN,
    CHILD_WAIT_JOIN,
    CHILD_START_ACTIVITY,
    CHILD_FAIL,
};

// Fits within gTasks[taskId].data (32 bytes); cast onto it directly, mirroring
// how Emerald stores WirelessLink_Leader/Group inline in task data.
struct WirelessConnectState
{
    /* 0x00 */ struct RfuPlayerList *playerList;
    /* 0x04 */ struct RfuIncomingPlayerList *incomingList;  // leader: children; child: parents
    /* 0x08 */ struct RfuIncomingPlayerList *incomingList2; // child: children (search task needs both)
    /* 0x0C */ u8 state;
    /* 0x0D */ u8 playerCount;
    /* 0x0E */ u8 listenTaskId;
    /* 0x0F */ u8 searchTaskId;
    /* 0x10 */ u8 joinRequestAnswer;
    /* 0x11 */ u8 leaderId;
    /* 0x12 */ u16 timeout;
};

// ------------------------------------------------------------------
// UI-free helpers ported verbatim from union_room.c
// ------------------------------------------------------------------

static bool8 ArePlayersDifferent(struct RfuPlayerData *player1, const struct RfuPlayerData *player2)
{
    s32 i;

    for (i = 0; i < 2; i++)
    {
        if (player1->data.compatibility.playerTrainerId[i] != player2->data.compatibility.playerTrainerId[i])
            return TRUE;
    }
    for (i = 0; i < OT_NAME_LENGTH + 1; i++)
    {
        if (player1->name[i] != player2->name[i])
            return TRUE;
    }
    return FALSE;
}

static void ClearRfuPlayerList(struct RfuPlayer *players, u8 count)
{
    s32 i;

    for (i = 0; i < count; i++)
    {
        players[i].rfu = sDummyRfu;
        players[i].timeoutCounter = 255;
        players[i].groupScheduledAnim = UNION_ROOM_SPAWN_NONE;
        players[i].useRedText = FALSE;
        players[i].newPlayerCountdown = 0;
    }
}

static void ClearIncomingPlayerList(struct RfuIncomingPlayerList *list)
{
    s32 i;

    for (i = 0; i < RFU_CHILD_MAX; i++)
    {
        list->players[i].rfu = sDummyRfu;
        list->players[i].active = FALSE;
    }
}

// Trade-only: accept a partner iff it's advertising ACTIVITY_TRADE.
static bool32 IsTradePartnerAcceptable(u32 activity)
{
    return (activity == ACTIVITY_TRADE);
}

static u8 TryAddIncomingPlayerToList(struct RfuPlayer *players, struct RfuIncomingPlayer *incomingPlayer, u8 max)
{
    s32 i;

    if (incomingPlayer->active)
    {
        for (i = 0; i < max; i++)
        {
            if (players[i].groupScheduledAnim == UNION_ROOM_SPAWN_NONE)
            {
                players[i].rfu = incomingPlayer->rfu;
                players[i].timeoutCounter = 0;
                players[i].groupScheduledAnim = UNION_ROOM_SPAWN_IN;
                players[i].newPlayerCountdown = 64;
                incomingPlayer->active = FALSE;
                return i;
            }
        }
    }
    return 0xFF;
}

static u16 ReadAsU16(const u8 *ptr)
{
    return (ptr[1] << 8) | ptr[0];
}

// ------------------------------------------------------------------
// Background tasks that continuously poll the RFU layer for partners.
// These mirror union_room.c's Task_ListenForCompatiblePartners (leader,
// watches child slots) and Task_SearchForChildOrParent (child, watches both
// parent and child slots so the RFU manager keeps scanning).
// ------------------------------------------------------------------

static void Task_ListenForChildren(u8 taskId)
{
    s32 i, j;
    struct RfuIncomingPlayerList **list = (void *)gTasks[taskId].data;

    for (i = 0; i < RFU_CHILD_MAX; i++)
    {
        Rfu_GetCompatiblePlayerData(&list[0]->players[i].rfu.data, list[0]->players[i].rfu.name, i);
        if (!IsTradePartnerAcceptable(list[0]->players[i].rfu.data.activity))
            list[0]->players[i].rfu = sDummyRfu;
        for (j = 0; j < i; j++)
        {
            if (!ArePlayersDifferent(&list[0]->players[j].rfu, &list[0]->players[i].rfu))
                list[0]->players[i].rfu = sDummyRfu;
        }
        list[0]->players[i].active = ArePlayersDifferent(&list[0]->players[i].rfu, &sDummyRfu);
    }
}

static u8 CreateTask_ListenForChildren(struct RfuIncomingPlayerList *list)
{
    u8 taskId = CreateTask(Task_ListenForChildren, 0);
    struct RfuIncomingPlayerList **data = (void *)gTasks[taskId].data;
    data[0] = list;
    return taskId;
}

static void Task_SearchForParent(u8 taskId)
{
    s32 i, j;
    struct RfuPlayerData rfu;
    struct RfuIncomingPlayerList **list = (void *)gTasks[taskId].data;
    bool8 isParent;

    for (i = 0; i < RFU_CHILD_MAX; i++)
    {
        isParent = Rfu_GetCompatiblePlayerData(&rfu.data, rfu.name, i);
        if (!IsTradePartnerAcceptable(rfu.data.activity))
            rfu = sDummyRfu;
        if (rfu.data.compatibility.language == LANGUAGE_JAPANESE)
            rfu = sDummyRfu;

        if (!isParent)
        {
            for (j = 0; j < i; j++)
            {
                if (!ArePlayersDifferent(&list[1]->players[j].rfu, &rfu))
                    rfu = sDummyRfu;
            }
            list[1]->players[i].rfu = rfu;
            list[1]->players[i].active = ArePlayersDifferent(&list[1]->players[i].rfu, &sDummyRfu);
        }
        else
        {
            list[0]->players[i].rfu = rfu;
            list[0]->players[i].active = ArePlayersDifferent(&list[0]->players[i].rfu, &sDummyRfu);
        }
    }
}

static u8 CreateTask_SearchForParent(struct RfuIncomingPlayerList *parentList, struct RfuIncomingPlayerList *childList)
{
    u8 taskId = CreateTask(Task_SearchForParent, 0);
    struct RfuIncomingPlayerList **data = (void *)gTasks[taskId].data;
    data[0] = parentList;
    data[1] = childList;
    return taskId;
}

// ------------------------------------------------------------------
// Shared completion
// ------------------------------------------------------------------

static void Finish(u8 taskId, struct WirelessConnectState *s, u32 result)
{
    if (s->listenTaskId != 0xFF && FuncIsActiveTask(Task_ListenForChildren))
        DestroyTask(s->listenTaskId);
    if (s->searchTaskId != 0xFF && FuncIsActiveTask(Task_SearchForParent))
        DestroyTask(s->searchTaskId);
    if (s->playerList != NULL)
        Free(s->playerList);
    if (s->incomingList != NULL)
        Free(s->incomingList);
    if (s->incomingList2 != NULL)
        Free(s->incomingList2);

    gSpecialVar_Result = result;
    DestroyTask(taskId);
    ScriptContext_Enable();
}

// ------------------------------------------------------------------
// Leader (host) task
// ------------------------------------------------------------------

static void Task_WirelessLeader(u8 taskId)
{
    struct WirelessConnectState *s = (void *)gTasks[taskId].data;
    s32 i;
    u32 val;

    switch (s->state)
    {
    case LEADER_INIT:
        gPlayerCurrActivity = ACTIVITY_TRADE;
        SetHostRfuGameData(ACTIVITY_TRADE, 0, FALSE);
        SetWirelessCommType1();
        OpenLink();
        InitializeRfuLinkManager_LinkLeader(2);
        s->playerList = AllocZeroed(sizeof(struct RfuPlayerList));
        s->incomingList = AllocZeroed(sizeof(struct RfuIncomingPlayerList));
        s->incomingList2 = NULL;
        ClearIncomingPlayerList(s->incomingList);
        ClearRfuPlayerList(s->playerList->players, MAX_RFU_PLAYER_LIST_SIZE);
        CopyHostRfuGameDataAndUsername(&s->playerList->players[0].rfu.data, s->playerList->players[0].rfu.name);
        s->playerList->players[0].groupScheduledAnim = UNION_ROOM_SPAWN_IN;
        s->playerList->players[0].newPlayerCountdown = 0;
        s->listenTaskId = CreateTask_ListenForChildren(s->incomingList);
        s->searchTaskId = 0xFF;
        s->playerCount = 1;
        s->timeout = 0;
        s->state = LEADER_AWAIT_CHILD;
        break;
    case LEADER_AWAIT_CHILD:
        // Merge any newly discovered child into the group list. For a 2-player
        // trade the first free slot is index 1 (== playerCount), so a slotted
        // member lands exactly where the accept step expects it.
        for (i = 0; i < RFU_CHILD_MAX; i++)
            TryAddIncomingPlayerToList(s->playerList->players, &s->incomingList->players[i], MAX_RFU_PLAYERS);

        if (s->playerList->players[s->playerCount].groupScheduledAnim == UNION_ROOM_SPAWN_IN)
            s->state = LEADER_ACCEPT;
        else if (RfuHasErrored())
            s->state = LEADER_FAIL;
        break;
    case LEADER_ACCEPT:
        s->joinRequestAnswer = RFU_STATUS_JOIN_GROUP_OK;
        SendRfuStatusToPartner(RFU_STATUS_JOIN_GROUP_OK,
            ReadAsU16(s->playerList->players[s->playerCount].rfu.data.compatibility.playerTrainerId),
            s->playerList->players[s->playerCount].rfu.name);
        s->state = LEADER_UPDATE_AFTER_ACCEPT;
        break;
    case LEADER_UPDATE_AFTER_ACCEPT:
        val = WaitSendRfuStatusToPartner(
            ReadAsU16(s->playerList->players[s->playerCount].rfu.data.compatibility.playerTrainerId),
            s->playerList->players[s->playerCount].rfu.name);
        if (val == 1) // send complete
        {
            s->playerList->players[s->playerCount].newPlayerCountdown = 0;
            s->playerCount++;
            if (s->playerCount == 2)
            {
                LinkRfu_StopManagerAndFinalizeSlots();
                s->timeout = 0;
                s->state = LEADER_FINAL_CHECK;
            }
            else
            {
                s->state = LEADER_AWAIT_CHILD;
            }
        }
        else if (val == 2) // partner disconnected mid-send
        {
            RfuSetStatus(RFU_STATUS_OK, 0);
            s->state = LEADER_AWAIT_CHILD;
        }
        break;
    case LEADER_FINAL_CHECK:
        if (LmanAcceptSlotFlagIsNotZero())
        {
            if (WaitRfuState(FALSE))
                s->state = LEADER_START_ACTIVITY;
            else if (++s->timeout > 300)
                s->state = LEADER_FAIL;
        }
        else
        {
            s->state = LEADER_FAIL;
        }
        break;
    case LEADER_START_ACTIVITY:
        if (RfuHasErrored())
            s->state = LEADER_FAIL;
        else if (gReceivedRemoteLinkPlayers)
        {
            UpdateGameData_GroupLockedIn(TRUE);
            Finish(taskId, s, LINKUP_SUCCESS);
        }
        break;
    case LEADER_FAIL:
        Finish(taskId, s, LINKUP_FAILED);
        break;
    }
}

// ------------------------------------------------------------------
// Child (joiner) task
// ------------------------------------------------------------------

static void Task_WirelessJoin(u8 taskId)
{
    struct WirelessConnectState *s = (void *)gTasks[taskId].data;
    s32 i;

    switch (s->state)
    {
    case CHILD_INIT:
        gPlayerCurrActivity = ACTIVITY_TRADE;
        SetHostRfuGameData(ACTIVITY_TRADE, 0, FALSE);
        SetWirelessCommType1();
        OpenLink();
        InitializeRfuLinkManager_JoinGroup();
        s->incomingList = AllocZeroed(sizeof(struct RfuIncomingPlayerList));  // parents
        s->incomingList2 = AllocZeroed(sizeof(struct RfuIncomingPlayerList)); // children
        s->playerList = AllocZeroed(sizeof(struct RfuPlayerList));
        ClearIncomingPlayerList(s->incomingList);
        ClearIncomingPlayerList(s->incomingList2);
        ClearRfuPlayerList(s->playerList->players, MAX_RFU_PLAYER_LIST_SIZE);
        s->searchTaskId = CreateTask_SearchForParent(s->incomingList, s->incomingList2);
        s->listenTaskId = 0xFF;
        s->timeout = 0;
        s->state = CHILD_SCAN;
        break;
    case CHILD_SCAN:
        for (i = 0; i < RFU_CHILD_MAX; i++)
        {
            if (s->incomingList->players[i].active
             && IsTradePartnerAcceptable(s->incomingList->players[i].rfu.data.activity))
            {
                s->leaderId = i;
                s->playerList->players[i].rfu = s->incomingList->players[i].rfu;
                UpdateGameData_SetActivity(ACTIVITY_TRADE, 0, TRUE);
                TryConnectToUnionRoomParent(
                    s->playerList->players[i].rfu.name,
                    &s->playerList->players[i].rfu.data,
                    ACTIVITY_TRADE);
                s->state = CHILD_WAIT_JOIN;
                break;
            }
        }
        if (RfuHasErrored())
            s->state = CHILD_FAIL;
        break;
    case CHILD_WAIT_JOIN:
        // Mirrors LG_STATE_MAIN in union_room.c, trade path only.
        if (gReceivedRemoteLinkPlayers)
        {
            RfuSetStatus(RFU_STATUS_OK, 0);
            s->state = CHILD_START_ACTIVITY;
            break;
        }
        switch (RfuGetStatus())
        {
        case RFU_STATUS_FATAL_ERROR:
        case RFU_STATUS_CONNECTION_ERROR:
        case RFU_STATUS_JOIN_GROUP_NO:
        case RFU_STATUS_LEAVE_GROUP:
            s->state = CHILD_FAIL;
            break;
        case RFU_STATUS_JOIN_GROUP_OK:
            // Leader accepted us; acknowledge and wait for the player-data
            // exchange to complete (gReceivedRemoteLinkPlayers, handled above).
            RfuSetStatus(RFU_STATUS_WAIT_ACK_JOIN_GROUP, 0);
            break;
        }
        break;
    case CHILD_START_ACTIVITY:
        Finish(taskId, s, LINKUP_SUCCESS);
        break;
    case CHILD_FAIL:
        Finish(taskId, s, LINKUP_FAILED);
        break;
    }
}

// ------------------------------------------------------------------
// Script specials
// ------------------------------------------------------------------

void ScrSpecial_TryBecomeLinkLeaderWireless(void)
{
    u8 taskId = CreateTask(Task_WirelessLeader, 80);
    struct WirelessConnectState *s = (void *)gTasks[taskId].data;

    s->state = LEADER_INIT;
    s->playerList = NULL;
    s->incomingList = NULL;
    s->incomingList2 = NULL;
    s->listenTaskId = 0xFF;
    s->searchTaskId = 0xFF;
    gSpecialVar_Result = LINKUP_ONGOING;
}

void ScrSpecial_TryJoinLinkGroupWireless(void)
{
    u8 taskId = CreateTask(Task_WirelessJoin, 80);
    struct WirelessConnectState *s = (void *)gTasks[taskId].data;

    s->state = CHILD_INIT;
    s->playerList = NULL;
    s->incomingList = NULL;
    s->incomingList2 = NULL;
    s->listenTaskId = 0xFF;
    s->searchTaskId = 0xFF;
    gSpecialVar_Result = LINKUP_ONGOING;
}
