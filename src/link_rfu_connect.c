#include "global.h"
#include "link.h"
#include "link_rfu.h"
#include "union_room.h"
#include "malloc.h"
#include "task.h"
#include "script.h"
#include "event_data.h"
#include "menu.h"
#include "strings.h"
#include "string_util.h"
#include "characters.h"
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
// Scope: 2-player trade only (GROUP_MAX=2, GROUP_MIN=0). Emerald's rich
// leader-side UI (player-select windows, yes/no accept prompts, wireless
// status sprite) is intentionally dropped -- none of it is on the wire, so a
// real Emerald still connects; the leader auto-accepts the first compatible
// child. The child DOES get a minimal leader-picker list (CHILD_CHOOSE_LEADER
// below) built with pokeruby's plain Menu_* API (this project has no
// window.c/list_menu.c -- Ruby/Sapphire predate that Emerald/FRLG subsystem
// -- so this isn't Emerald's real ListMenu, just enough to show discovered
// leaders and let the player pick one, matching retail's
// sText_ChooseTrainerToTradeWith screen instead of silently auto-connecting).
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

// Matches retail's sText_1PlayerNeeded (pokeemerald src/data/union_room.h).
// Our trade group is always exactly 2 players, so "1 player needed" is the
// only count this MVP ever needs -- no need to port the full
// sPlayersNeededOrModeTexts table for other group sizes we don't support.
static const u8 sText_OnePlayerNeeded[] = _("1 player\nneeded.");

// Matches retail's PrintPlayerNameAndIdOnWindow (pokeemerald src/union_room.c),
// which prints the LOCAL player's own name + trainer ID into the group-member
// box the instant it's drawn -- not a network round-trip, just our own save
// data, so there's no reason this should ever be blank.
static const u8 sText_ID[] = _("ID");

// Retail's B-Button-cancel hint (pokeemerald src/data/union_room.h's
// sText_BButtonCancel uses the {B_BUTTON} icon glyph; pokeruby's own cable
// club text already spells this out as plain text elsewhere -- see
// OldaleTown_PokemonCenter_2F_Text_1A490C's "B Button: Cancel" -- so match
// that existing in-project convention instead of a glyph this font may not
// have wired up the same way).
static const u8 sText_BButtonCancel[] = _("B Button: Cancel");

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
    CHILD_CHOOSE_LEADER,
    CHILD_CONNECT,
    CHILD_WAIT_JOIN,
    CHILD_START_ACTIVITY,
    CHILD_FAIL,
};

// Leader-picker list box position/sizing (tile coords). Fixed width rather
// than measuring string width like DrawMultichoiceMenu does (that helper is
// static to script_menu.c) -- wide enough for a 7-char OT name or "CANCEL".
#define LEADER_LIST_LEFT  12
#define LEADER_LIST_TOP   6
#define LEADER_LIST_RIGHT 27

// Small "N player(s) needed" box on the leader (host) side, matching
// retail's sWindowTemplate_NumPlayerMode position (pokeemerald
// src/data/union_room.h: tilemapLeft=16, top=3, width=7, height=4).
#define PLAYERS_NEEDED_LEFT   16
#define PLAYERS_NEEDED_TOP    3
#define PLAYERS_NEEDED_RIGHT  23
#define PLAYERS_NEEDED_BOTTOM 7

// Group-member box to its left, matching retail's sWindowTemplate_PlayerList
// (tilemapLeft=1, top=3, width=13, height=8). Retail fills this with a real
// scrolling ListMenu of every group member; we only ever show ourselves here
// (player[0]) since the connecting child never gets displayed before the
// script hands off to the Trade Center -- close enough for this MVP to stop
// the box from just being blank.
#define PLAYER_LIST_LEFT   1
#define PLAYER_LIST_TOP    3
#define PLAYER_LIST_RIGHT  15
#define PLAYER_LIST_BOTTOM 11

// "B Button: Cancel" hint strip, directly under the two boxes above.
#define B_CANCEL_LEFT   1
#define B_CANCEL_TOP    12
#define B_CANCEL_RIGHT  23
#define B_CANCEL_BOTTOM 14

// Fits within gTasks[taskId].data (32 bytes); cast onto it directly, mirroring
// how Emerald stores WirelessLink_Leader/Group inline in task data.
struct WirelessConnectState
{
    /* 0x00 */ struct RfuPlayerList *playerList;
    /* 0x04 */ struct RfuIncomingPlayerList *incomingList; // leader: children; child: parents
    /* 0x08 */ u8 state;
    /* 0x09 */ u8 playerCount;
    /* 0x0A */ u8 listenTaskId;
    /* 0x0B */ u8 joinRequestAnswer;
    /* 0x0C */ u8 leaderId;
    /* 0x0E */ u16 timeout;
    /* 0x10 */ u8 menuDrawnCount; // child: how many rows the leader-list box currently shows, 0 = not drawn yet
    /* 0x11 */ u8 menuBottom;     // child: bottom row of the currently-drawn leader-list box (for erasing before a redraw)
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
// Background task that continuously polls the RFU layer for nearby
// beacons. Mirrors union_room.c's Task_ListenForCompatiblePartners --
// which retail uses for BOTH TryBecomeLinkLeader and TryJoinLinkGroup.
//
// Rfu_GetCompatiblePlayerData's bool8 return does NOT classify the entry at
// index i as parent-or-child; it just echoes the LOCAL device's own current
// lman.parent_child mode back to the caller (constant across the whole scan
// loop). It only matters to retail's *dual*-list scan
// (Task_SearchForChildOrParent), which is Union-Room-only (used exclusively
// via InitializeRfuLinkManager_EnterUnionRoom's MODE_P_C_SWITCH) -- not what
// either TryBecomeLinkLeader or TryJoinLinkGroup use. An earlier version of
// this file wrongly modeled the CHILD scan on that dual-list function,
// which meant every discovered parent got routed into a "children" list
// nothing ever read from, and the child's own leader-picker list stayed
// permanently empty -- the actual reason two real consoles never found each
// other. Fixed by using this single-list scan for both roles, exactly like
// retail does.
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

// ------------------------------------------------------------------
// Shared completion
// ------------------------------------------------------------------

static void Finish(u8 taskId, struct WirelessConnectState *s, u32 result)
{
    if (s->listenTaskId != 0xFF && FuncIsActiveTask(Task_ListenForChildren))
        DestroyTask(s->listenTaskId);
    if (s->playerList != NULL)
        Free(s->playerList);
    if (s->incomingList != NULL)
        Free(s->incomingList);

    // Harmless no-op on the child path, which never draws here.
    Menu_EraseWindowRect(PLAYERS_NEEDED_LEFT, PLAYERS_NEEDED_TOP, PLAYERS_NEEDED_RIGHT, PLAYERS_NEEDED_BOTTOM);
    Menu_EraseWindowRect(PLAYER_LIST_LEFT, PLAYER_LIST_TOP, PLAYER_LIST_RIGHT, PLAYER_LIST_BOTTOM);
    Menu_EraseWindowRect(B_CANCEL_LEFT, B_CANCEL_TOP, B_CANCEL_RIGHT, B_CANCEL_BOTTOM);

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
        ClearIncomingPlayerList(s->incomingList);
        ClearRfuPlayerList(s->playerList->players, MAX_RFU_PLAYER_LIST_SIZE);
        CopyHostRfuGameDataAndUsername(&s->playerList->players[0].rfu.data, s->playerList->players[0].rfu.name);
        s->playerList->players[0].groupScheduledAnim = UNION_ROOM_SPAWN_IN;
        s->playerList->players[0].newPlayerCountdown = 0;
        s->listenTaskId = CreateTask_ListenForChildren(s->incomingList);
        s->playerCount = 1;
        s->timeout = 0;
        Menu_DrawStdWindowFrame(PLAYERS_NEEDED_LEFT, PLAYERS_NEEDED_TOP, PLAYERS_NEEDED_RIGHT, PLAYERS_NEEDED_BOTTOM);
        Menu_PrintText(sText_OnePlayerNeeded, PLAYERS_NEEDED_LEFT + 1, PLAYERS_NEEDED_TOP + 1);
        Menu_DrawStdWindowFrame(B_CANCEL_LEFT, B_CANCEL_TOP, B_CANCEL_RIGHT, B_CANCEL_BOTTOM);
        Menu_PrintText(sText_BButtonCancel, B_CANCEL_LEFT + 1, B_CANCEL_TOP + 1);
        {
            // Single Menu_PrintText call with an embedded newline, matching
            // the one other multi-line label in this file (sText_OnePlayerNeeded)
            // exactly, rather than two separate calls into the same window.
            u8 text[24];
            u8 *txtPtr;

            Menu_DrawStdWindowFrame(PLAYER_LIST_LEFT, PLAYER_LIST_TOP, PLAYER_LIST_RIGHT, PLAYER_LIST_BOTTOM);
            txtPtr = StringCopy(text, gSaveBlock2.playerName);
            *txtPtr++ = CHAR_NEWLINE;
            txtPtr = StringCopy(txtPtr, sText_ID);
            ConvertIntToDecimalStringN(txtPtr, ReadAsU16(gSaveBlock2.playerTrainerId), STR_CONV_MODE_LEADING_ZEROS, 5);
            Menu_PrintText(text, PLAYER_LIST_LEFT + 1, PLAYER_LIST_TOP + 1);
        }
        s->state = LEADER_AWAIT_CHILD;
        break;
    case LEADER_AWAIT_CHILD:
        if (JOY_NEW(B_BUTTON))
        {
            s->state = LEADER_FAIL;
            break;
        }
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
        s->incomingList = AllocZeroed(sizeof(struct RfuIncomingPlayerList)); // discovered parents
        s->playerList = AllocZeroed(sizeof(struct RfuPlayerList));
        ClearIncomingPlayerList(s->incomingList);
        ClearRfuPlayerList(s->playerList->players, MAX_RFU_PLAYER_LIST_SIZE);
        // Same single-list scan the leader uses (Task_ListenForChildren) --
        // see its header comment for why the child previously used the
        // wrong (dual-list, Union-Room-only) scan and never actually found
        // any parents.
        s->listenTaskId = CreateTask_ListenForChildren(s->incomingList);
        s->timeout = 0;
        s->menuDrawnCount = 0;
        s->state = CHILD_CHOOSE_LEADER;
        break;
    case CHILD_CHOOSE_LEADER:
    {
        // Minimal leader-picker: list every currently-discovered, trade-
        // compatible parent by OT name (rfu.name is already charmap-encoded,
        // transmitted as-is -- same representation retail prints directly),
        // plus a trailing CANCEL row. Redrawn only when the active count
        // changes, so it doesn't fight the player's cursor every frame.
        struct MenuAction items[RFU_CHILD_MAX + 1];
        u8 activeCount = 0;
        s8 selection;

        if (RfuHasErrored())
        {
            if (s->menuDrawnCount != 0)
            {
                Menu_DestroyCursor();
                Menu_EraseWindowRect(LEADER_LIST_LEFT, LEADER_LIST_TOP, LEADER_LIST_RIGHT, s->menuBottom);
            }
            s->state = CHILD_FAIL;
            break;
        }

        for (i = 0; i < RFU_CHILD_MAX; i++)
        {
            if (s->incomingList->players[i].active
             && IsTradePartnerAcceptable(s->incomingList->players[i].rfu.data.activity))
            {
                items[activeCount].text = s->incomingList->players[i].rfu.name;
                items[activeCount].func = NULL;
                activeCount++;
            }
        }
        items[activeCount].text = gOtherText_CancelNoTerminator;
        items[activeCount].func = NULL;

        if (activeCount + 1 != s->menuDrawnCount)
        {
            u8 bottom = LEADER_LIST_TOP + (2 * (activeCount + 1) + 1);

            if (s->menuDrawnCount != 0)
            {
                Menu_DestroyCursor();
                Menu_EraseWindowRect(LEADER_LIST_LEFT, LEADER_LIST_TOP, LEADER_LIST_RIGHT, s->menuBottom);
            }
            Menu_DrawStdWindowFrame(LEADER_LIST_LEFT, LEADER_LIST_TOP, LEADER_LIST_RIGHT, bottom);
            Menu_PrintItems(LEADER_LIST_LEFT + 1, LEADER_LIST_TOP + 1, activeCount + 1, items);
            InitMenu(0, LEADER_LIST_LEFT + 1, LEADER_LIST_TOP + 1, activeCount + 1, 0, LEADER_LIST_RIGHT - LEADER_LIST_LEFT - 1);
            s->menuDrawnCount = activeCount + 1;
            s->menuBottom = bottom;
            break;
        }

        selection = Menu_ProcessInputNoWrap();
        if (selection == -2)
            break; // no input yet

        Menu_DestroyCursor();
        Menu_EraseWindowRect(LEADER_LIST_LEFT, LEADER_LIST_TOP, LEADER_LIST_RIGHT, s->menuBottom);

        if (selection == -1 || selection == activeCount)
        {
            // B pressed, or the CANCEL row was chosen.
            s->state = CHILD_FAIL;
        }
        else
        {
            s->leaderId = selection;
            s->state = CHILD_CONNECT;
        }
        break;
    }
    case CHILD_CONNECT:
        // NOT TryConnectToUnionRoomParent -- that function (and its
        // Task_TryConnectToUnionRoomParent/IsPartnerActivityIncompatible
        // helpers) is Union-Room-only: IsPartnerActivityIncompatible
        // hard-requires the partner's activity to carry the IN_UNION_ROOM
        // flag, which our plain ACTIVITY_TRADE beacon never does, so it
        // would reject every connection as "incompatible" unconditionally
        // -- this was the actual cause of "Please come again" after
        // selecting a leader from the (now correctly populated) list.
        // Retail's real TryJoinLinkGroup (union_room.c's AskToJoinRfuGroup)
        // calls CreateTask_RfuReconnectWithParent(name, trainerId) instead;
        // match that.
        i = s->leaderId;
        s->playerList->players[i].rfu = s->incomingList->players[i].rfu;
        UpdateGameData_SetActivity(ACTIVITY_TRADE, 0, TRUE);
        CreateTask_RfuReconnectWithParent(
            s->playerList->players[i].rfu.name,
            ReadAsU16(s->playerList->players[i].rfu.data.compatibility.playerTrainerId));
        s->state = CHILD_WAIT_JOIN;
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
    s->listenTaskId = 0xFF;
    gSpecialVar_Result = LINKUP_ONGOING;
}

void ScrSpecial_TryJoinLinkGroupWireless(void)
{
    u8 taskId = CreateTask(Task_WirelessJoin, 80);
    struct WirelessConnectState *s = (void *)gTasks[taskId].data;

    s->state = CHILD_INIT;
    s->playerList = NULL;
    s->incomingList = NULL;
    s->listenTaskId = 0xFF;
    gSpecialVar_Result = LINKUP_ONGOING;
}
