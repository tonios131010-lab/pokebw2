#include "types.h"
#include "app/key_system_flow.h"
#include "app/unova_link.h"
#include "constants/sound.h"
#include "constants/version.h"
#include "field/zone.h"
#include "gfl/bg_sys.h"
#include "gfl/clact.h"
#include "gfl/graphics.h"
#include "gfl/key.h"
#include "gfl/msg.h"
#include "gfl/net.h"
#include "gfl/sound.h"
#include "gfl/std.h"
#include "gfl/str.h"
#include "nitro/fx.h"
#include "nitro/gx.h"
#include "nitro/hw.h"
#include "save/key_info.h"
#include "save/save_control.h"
#include "save/save_outside.h"
#include "system/bmp_menulist.h"
#include "system/bmp_oam.h"
#include "system/dsi.h"
#include "system/game_data.h"
#include "system/wordset.h"

// The Key System's sequences and scenes: exchanging keys with another Black 2 or White 2 over infrared, and the
// settings that keys unlock, the difficulty, the city and the chamber behind the mystery door. The ROM doesn't name
// this file; its helpers and the scene tables are in key_system_flow_sub.c (the ROM's .rodata order says so: the
// four message tables below pack as one section before the sub file's initializer and scene tables)

static void KeySystemFlow_SeqExchange(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqSettings(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqDifficulty(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqCity(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqChamber(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqReceive(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqSend(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqSave(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartMenuScene(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartConnectScene(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartSettingsScene(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartDifficultyScene(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartCityScene(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartKeyAnimScene(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartSendScene(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartKeysHeldScene(KeySystemSeq *seq, int *state, void *work);
static void KeySystemFlow_SeqStartChamberScene(KeySystemSeq *seq, int *state, void *work);

// What a key that is already enabled says, what an unlocked key says, and the keys' names in the lists of keys
// being sent and held, in each version's order. MWCC heapsorts a unit's data by size, which gives the ROM's
// order of the three equal-size tables (unlocked, send, held) only from this declaration order
static const u16 sKeyEnabledMsgs[KEY_SYSTEM_KEY_COUNT] = { 107, 108, 0, 109, 110 };
static const u16 sHeldKeyNameMsgs[2][KEY_SYSTEM_KEY_COUNT] = {
    { 14, 15, 17, 105, 106 },
    { 14, 15, 16, 105, 106 },
};
static const u16 sSendKeyNameMsgs[2][KEY_SYSTEM_KEY_COUNT] = {
    { 14, 15, 17, 105, 106 },
    { 14, 15, 16, 105, 106 },
};
static const u16 sKeyUnlockedMsgs[2][KEY_SYSTEM_KEY_COUNT] = {
    { 89, 90, 91, 103, 104 },
    { 89, 90, 92, 103, 104 },
};

// The windows of the keys being sent, a title and then a key's name in each
static KeySystemMsgWinTemplate sSendTemplates[] = {
    { 1, 4, 18, 2, 13 }, { 1, 7, 18, 2, 0 }, { 1, 10, 18, 2, 0 }, { 1, 13, 18, 2, 0 }, { 1, 16, 18, 2, 0 },
};

// The windows of the keys held
static KeySystemMsgWinTemplate sKeysHeldTemplates[] = {
    { 1, 1, 18, 2, 13 }, { 1, 4, 18, 2, 0 },  { 1, 7, 18, 2, 0 },  { 1, 10, 18, 2, 0 },
    { 1, 13, 18, 2, 0 }, { 1, 16, 18, 2, 0 }, { 1, 19, 18, 2, 0 },
};

void KeySystemFlow_Init(KeySystemWork *wk, HeapID heapId) {
    KeySystemKeyState_Init(&wk->keys, wk->keyInfo);
    KeySystemSaveInfo_Init(&wk->saveInfo, wk->param->gameData);
    if (wk->param->mode == UNOVA_LINK_MODE_GAME_CLEAR) {
        GFL_SndBGMPush();
        KeySystemBG_StartFade(wk->bg, KEY_SYSTEM_BG_FADE_1_TO_0, 1);
        GFL_NetSetIconScreen(TRUE, heapId);
    }
}

void KeySystemFlow_Exit(KeySystemWork *wk) {
    UnovaLinkParam *param = wk->param;

    if (param->mode == UNOVA_LINK_MODE_GAME_CLEAR) {
        if (param->key <= KEY_SYSTEM_KEY_CHALLENGE) {
            TrainerGameInfo_SetKeyNotice(getTrainerCardDataBlkAddress(param->gameData), 1);
        }
        GFL_SndBGMPop();
    }
}

enum {
    GAME_CLEAR_SEQ_START_SCENE,
    GAME_CLEAR_SEQ_FADE_IN,
    GAME_CLEAR_SEQ_START_ANIM,
    GAME_CLEAR_SEQ_WAIT_ANIM,
    GAME_CLEAR_SEQ_UNLOCK_KEY,
    GAME_CLEAR_SEQ_FADE_OUT,
    GAME_CLEAR_SEQ_END_SCENE,
    GAME_CLEAR_SEQ_END,
};

void KeySystemFlow_SeqGameClear(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;
    StrBuf *str;
    BOOL enabled;
    u16 msgId;

    switch (*state) {
    case GAME_CLEAR_SEQ_START_SCENE:
        KeySystem_CreateMsgWin(wk, HEAPID_KEY_SYSTEM);
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartKeyAnimScene);
        (*state)++;
        break;
    case GAME_CLEAR_SEQ_FADE_IN:
        KeySystemSeq_Push(seq, KeySystem_SeqFadeIn);
        (*state)++;
        break;
    case GAME_CLEAR_SEQ_START_ANIM:
        enabled = FALSE;
        msgId = sKeyNameMsgs[VERSION_INDEX][wk->param->key];
        str = GFL_MsgDataLoadStrbufNew(wk->msgData, sKeyUnlockedMsgs[VERSION_INDEX][wk->param->key]);
        if (wk->param->key == KEY_SYSTEM_KEY_CITY) {
            enabled = TRUE;
        } else if (keyEnabler(wk->keyInfo, wk->param->key)) {
            enabled = TRUE;
            GFL_StrBufFree(str);
            str = GFL_MsgDataLoadStrbufNew(wk->msgData, sKeyEnabledMsgs[wk->param->key]);
        }
        KeySystemKeyAnim_Start(&wk->keyAnim, wk->param->key, wk->msgData, msgId, str, enabled);
        GFL_StrBufFree(str);
        (*state)++;
        break;
    case GAME_CLEAR_SEQ_WAIT_ANIM:
        if (KeySystemKeyAnim_Update(&wk->keyAnim)) {
            (*state)++;
        }
        break;
    case GAME_CLEAR_SEQ_UNLOCK_KEY:
        KeyInfo_UnlockKey(wk->keyInfo, wk->param->key);
        if (wk->param->key != KEY_SYSTEM_KEY_CITY) {
            KeyInfo_EnableKey(wk->keyInfo, wk->param->key);
        }
        (*state)++;
        break;
    case GAME_CLEAR_SEQ_FADE_OUT:
        KeySystemSeq_Push(seq, KeySystem_SeqFadeOut);
        (*state)++;
        break;
    case GAME_CLEAR_SEQ_END_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        KeySystem_FreeMsgWin(wk);
        (*state)++;
        break;
    case GAME_CLEAR_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

enum {
    MENU_SEQ_START_SCENE,
    MENU_SEQ_WAIT_CHOICE,
    MENU_SEQ_END_SCENE,
    MENU_SEQ_RUN_CHOICE,
    MENU_SEQ_END,
};

void KeySystemFlow_SeqMenu(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case MENU_SEQ_START_SCENE:
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartMenuScene);
        (*state)++;
        break;
    case MENU_SEQ_WAIT_CHOICE:
        KeySystemList_Update(wk->list);
        if (KeySystemList_IsChanged(wk->list) && KeySystemMsgWin_IsDone(wk->infoWin)) {
            KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, KeySystemList_GetCursor(wk->list) + 18,
                                     KEY_SYSTEM_MSG_PRINT);
        }
        if (KeySystemList_IsDecided(wk->list)) {
            wk->choice = KeySystemList_GetCursor(wk->list);
            if (wk->choice != KEY_MENU_BACK) {
                KeySystemTags_Set(wk->tags, "KEM", KeySystemList_GetCursor(wk->list));
            }
            (*state)++;
        }
        break;
    case MENU_SEQ_END_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        (*state)++;
        break;
    case MENU_SEQ_RUN_CHOICE:
        switch (wk->choice) {
        case KEY_MENU_EXCHANGE:
            if (isWirelessEnabled()) {
                KeySystemSeq_Push(seq, KeySystemFlow_SeqExchange);
            } else {
                KeySystemSeq_Push(seq, KeySystem_SeqWirelessOff);
            }
            *state = MENU_SEQ_START_SCENE;
            break;
        case KEY_MENU_SETTINGS:
            KeySystemSeq_Push(seq, KeySystemFlow_SeqSettings);
            *state = MENU_SEQ_START_SCENE;
            break;
        case KEY_MENU_BACK:
            (*state)++;
            break;
        }
        break;
    case MENU_SEQ_END:
        KeySystemBG_StartFade(wk->bg, KEY_SYSTEM_BG_FADE_0_TO_1, 30);
        KeySystemSeq_Pop(seq);
        break;
    }
}

enum {
    EXCHANGE_SEQ_START_SCENE,
    EXCHANGE_SEQ_PRINT_PROMPT,
    EXCHANGE_SEQ_WAIT_PROMPT_PRINT,
    EXCHANGE_SEQ_WAIT_MENU_PRINT,
    EXCHANGE_SEQ_WAIT_ANSWER,
    EXCHANGE_SEQ_START_RETRY_SCENE,
    EXCHANGE_SEQ_PRINT_RETRY_PROMPT,
    EXCHANGE_SEQ_START_CONNECT_SCENE,
    EXCHANGE_SEQ_REQUEST_CONNECT,
    EXCHANGE_SEQ_WAIT_CONNECT,
    EXCHANGE_SEQ_WAIT_CANCEL,
    EXCHANGE_SEQ_END_SCENE_RETRY,
    EXCHANGE_SEQ_END_SCENE_CANCELED,
    EXCHANGE_SEQ_SEND_SAVE_INFO,
    EXCHANGE_SEQ_WAIT_SAVE_INFO,
    EXCHANGE_SEQ_SEND_KEYS,
    EXCHANGE_SEQ_WAIT_KEYS,
    EXCHANGE_SEQ_END_CONNECT_SCENE,
    EXCHANGE_SEQ_FIRST_TRANSFER,
    EXCHANGE_SEQ_SECOND_TRANSFER,
    EXCHANGE_SEQ_DISCONNECT,
    EXCHANGE_SEQ_WAIT_DISCONNECT,
    EXCHANGE_SEQ_MERGE_KEYS,
    EXCHANGE_SEQ_PRINT_DONE,
    EXCHANGE_SEQ_WAIT_DONE_PRINT,
    EXCHANGE_SEQ_END,
};

static void KeySystemFlow_SeqExchange(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;
    s32 result;

    switch (*state) {
    case EXCHANGE_SEQ_START_SCENE:
        KeySystem_CreateMsgWin(wk, HEAPID_KEY_SYSTEM);
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartKeysHeldScene);
        (*state)++;
        break;
    case EXCHANGE_SEQ_PRINT_PROMPT:
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 67, KEY_SYSTEM_MSG_STREAM);
        (*state)++;
        break;
    case EXCHANGE_SEQ_WAIT_PROMPT_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            KeySystem_CreateYesNoMenu(wk, HEAPID_KEY_SYSTEM);
            (*state)++;
        }
        break;
    case EXCHANGE_SEQ_WAIT_MENU_PRINT:
        if (KeySystemMenu_UpdatePrint(wk->menu)) {
            GFL_BGSysQueueScrLoad(0);
            (*state)++;
        }
        break;
    case EXCHANGE_SEQ_WAIT_ANSWER:
        result = KeySystemMenu_Update(wk->menu);
        if (result == BMPMENULIST_NULL) {
            break;
        }
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        KeySystem_FreeMenu(wk);
        switch (result) {
        case KEY_SYSTEM_YES_NO_YES:
            *state = EXCHANGE_SEQ_START_CONNECT_SCENE;
            break;
        case KEY_SYSTEM_YES_NO_NO:
        default:
            KeySystemMsgWinGroup_Free(wk->msgWinGroup);
            wk->msgWinGroup = NULL;
            GFL_BGSysClearScr(6);
            GFL_BGSysQueueScrLoad(6);
            GFL_BGSysQueueScrLoad(4);
            *state = EXCHANGE_SEQ_END;
            break;
        }
        break;
    case EXCHANGE_SEQ_START_RETRY_SCENE:
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartKeysHeldScene);
        (*state)++;
        break;
    case EXCHANGE_SEQ_PRINT_RETRY_PROMPT:
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 111, KEY_SYSTEM_MSG_STREAM);
        *state = EXCHANGE_SEQ_WAIT_PROMPT_PRINT;
        break;
    case EXCHANGE_SEQ_START_CONNECT_SCENE:
        wk->choice = 0;
        wk->noCancel = FALSE;
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartConnectScene);
        GFL_NetSetIconScreen(TRUE, HEAPID_KEY_SYSTEM);
        KeySystemNet_SetMode(wk->net, KEY_SYSTEM_NET_MODE_WIRELESS);
        GFL_SndSEPlay(SEQ_SE_SW_KEYSYS_01);
        (*state)++;
        break;
    case EXCHANGE_SEQ_REQUEST_CONNECT: {
        KeySystemNetCallback callback;

        MI_CpuFill8(&callback, 0, sizeof(KeySystemNetCallback));
        callback.arg = wk;
        callback.func = KeySystemFlow_OnNetError;
        wk->choice = 0;
        KeySystemNet_Request(wk->net, KEY_SYSTEM_NET_REQUEST_CONNECT, &callback);
        (*state)++;
        break;
    }
    case EXCHANGE_SEQ_WAIT_CONNECT:
        if (wk->choice) {
            ClActor *actor0 = KeySystemClAct_GetActor(&wk->clact, 0);
            ClActor *actor1 = KeySystemClAct_GetActor(&wk->clact, 1);
            ClActor *actor2 = KeySystemClAct_GetActor(&wk->clact, 2);
            ClActor *actor3 = KeySystemClAct_GetActor(&wk->clact, 3);

            ClActor_SetVisible(actor0, FALSE);
            ClActor_SetVisible(actor1, FALSE);
            ClActor_SetSequence(actor2, 4);
            ClActor_SetSequence(actor3, 5);
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 60, KEY_SYSTEM_MSG_PRINT_WAIT_ICON);
            GFL_SndStop();
            GFL_SndSEPlay(SEQ_SE_SYS_12);
            wk->choice = 0;
        } else if (!wk->noCancel && (GCTX_HIDGetPressedKeys() & PAD_BUTTON_B)) {
            KeySystemNet_Request(wk->net, KEY_SYSTEM_NET_REQUEST_CANCEL, NULL);
        }
        if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_CONNECTED) {
            wk->isParent = GFL_NetIsParent();
            *state = EXCHANGE_SEQ_SEND_SAVE_INFO;
        } else if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_IDLE) {
            GFL_SndStop();
            *state = EXCHANGE_SEQ_END_SCENE_RETRY;
        } else if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_CANCELING) {
            GFL_SndStop();
            *state = EXCHANGE_SEQ_WAIT_CANCEL;
        }
        break;
    case EXCHANGE_SEQ_WAIT_CANCEL:
        if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_IDLE) {
            *state = EXCHANGE_SEQ_END_SCENE_CANCELED;
        }
        break;
    case EXCHANGE_SEQ_END_SCENE_RETRY:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        *state = EXCHANGE_SEQ_START_RETRY_SCENE;
        break;
    case EXCHANGE_SEQ_END_SCENE_CANCELED:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        *state = EXCHANGE_SEQ_END;
        break;
    case EXCHANGE_SEQ_SEND_SAVE_INFO: {
        KeySystemNetSend send;

        send.size = sizeof(KeySystemSaveInfo);
        send.data = &wk->saveInfo;
        KeySystemNet_Request(wk->net, KEY_SYSTEM_NET_REQUEST_SEND, &send);
        (*state)++;
        break;
    }
    case EXCHANGE_SEQ_WAIT_SAVE_INFO:
        if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_CONNECTED) {
            wk->choice = KeySystemNet_GetReceived(wk->net, &wk->partnerSaveInfo, sizeof(KeySystemSaveInfo));
            (*state)++;
        }
        break;
    case EXCHANGE_SEQ_SEND_KEYS: {
        KeySystemNetSend send;

        send.size = sizeof(KeySystemKeyState);
        send.data = &wk->keys;
        KeySystemNet_Request(wk->net, KEY_SYSTEM_NET_REQUEST_SEND, &send);
        (*state)++;
        break;
    }
    case EXCHANGE_SEQ_WAIT_KEYS:
        if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_CONNECTED) {
            wk->choice = KeySystemNet_GetReceived(wk->net, &wk->partnerKeys, sizeof(KeySystemKeyState));
            (*state)++;
        }
        break;
    case EXCHANGE_SEQ_END_CONNECT_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        if (wk->choice) {
            (*state)++;
        } else {
            *state = EXCHANGE_SEQ_DISCONNECT;
        }
        break;
    case EXCHANGE_SEQ_FIRST_TRANSFER:
        if (wk->isParent) {
            KeySystemSeq_Push(seq, KeySystemFlow_SeqReceive);
        } else {
            KeySystemSeq_Push(seq, KeySystemFlow_SeqSend);
        }
        (*state)++;
        break;
    case EXCHANGE_SEQ_SECOND_TRANSFER:
        if (!wk->isParent) {
            KeySystemSeq_Push(seq, KeySystemFlow_SeqReceive);
        } else {
            KeySystemSeq_Push(seq, KeySystemFlow_SeqSend);
        }
        (*state)++;
        break;
    case EXCHANGE_SEQ_DISCONNECT:
        KeySystemNet_Request(wk->net, KEY_SYSTEM_NET_REQUEST_DISCONNECT, NULL);
        (*state)++;
        break;
    case EXCHANGE_SEQ_WAIT_DISCONNECT:
        if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_IDLE) {
            (*state)++;
        }
        break;
    case EXCHANGE_SEQ_MERGE_KEYS:
        if (KeySystemKeyState_CanExchange(&wk->keys, &wk->partnerKeys, TRUE)) {
            KeySystemKeyState_Receive(&wk->keys, &wk->partnerKeys);
            KeySystemKeyState_Save(&wk->keys, wk->keyInfo);
            KeySystemSeq_Push(seq, KeySystemFlow_SeqSave);
        }
        *state = EXCHANGE_SEQ_PRINT_DONE;
        break;
    case EXCHANGE_SEQ_PRINT_DONE:
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 57, KEY_SYSTEM_MSG_STREAM_FAST);
        (*state)++;
        break;
    case EXCHANGE_SEQ_WAIT_DONE_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            (*state)++;
        }
        break;
    case EXCHANGE_SEQ_END:
        KeySystemNet_SetMode(wk->net, KEY_SYSTEM_NET_MODE_NONE);
        KeySystem_FreeMsgWin(wk);
        GFL_BGSysQueueScrLoad(0);
        KeySystemSeq_Pop(seq);
        break;
    }
}

enum {
    SETTINGS_SEQ_START_SCENE,
    SETTINGS_SEQ_WAIT_CHOICE,
    SETTINGS_SEQ_END_SCENE,
    SETTINGS_SEQ_RUN_CHOICE,
    SETTINGS_SEQ_RESTART,
};

static void KeySystemFlow_SeqSettings(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SETTINGS_SEQ_START_SCENE:
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartSettingsScene);
        (*state)++;
        break;
    case SETTINGS_SEQ_WAIT_CHOICE:
        KeySystemList_Update(wk->list);
        if (KeySystemList_IsChanged(wk->list) && KeySystemMsgWin_IsDone(wk->infoWin)) {
            KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData,
                                     KeySystemFlow_GetSettingsMsg(wk->keyInfo, KeySystemList_GetCursor(wk->list)),
                                     KEY_SYSTEM_MSG_PRINT);
        }
        if (KeySystemList_IsDecided(wk->list)) {
            wk->choice = KeySystemList_GetCursor(wk->list);
            if (wk->choice != KEY_LIST_BACK) {
                KeySystemTags_Set(wk->tags, "KLL", KeySystemList_GetCursor(wk->list));
            }
            (*state)++;
        }
        break;
    case SETTINGS_SEQ_END_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        (*state)++;
        break;
    case SETTINGS_SEQ_RUN_CHOICE:
        switch (wk->choice) {
        case KEY_LIST_DIFFICULTY:
            KeySystemSeq_Push(seq, KeySystemFlow_SeqDifficulty);
            *state = SETTINGS_SEQ_RESTART;
            break;
        case KEY_LIST_CITY:
            KeySystemSeq_Push(seq, KeySystemFlow_SeqCity);
            *state = SETTINGS_SEQ_RESTART;
            break;
        case KEY_LIST_CHAMBER:
            KeySystemSeq_Push(seq, KeySystemFlow_SeqChamber);
            *state = SETTINGS_SEQ_RESTART;
            break;
        case KEY_LIST_BACK:
            KeySystemSeq_Pop(seq);
            break;
        }
        break;
    case SETTINGS_SEQ_RESTART:
        *state = SETTINGS_SEQ_START_SCENE;
        break;
    }
}

// The items that go back, which come last in the lists of the key selections
#define DIFFICULTY_LIST_BACK 3
#define CITY_LIST_BACK 2
#define CHAMBER_LIST_BACK 3

enum {
    KEY_SELECT_SEQ_START_SCENE,
    KEY_SELECT_SEQ_WAIT_SELECT,
    KEY_SELECT_SEQ_SAVE,
    KEY_SELECT_SEQ_PRINT_INFO,
    KEY_SELECT_SEQ_WAIT_SAVED,
    KEY_SELECT_SEQ_PRINT_CHOSEN,
    KEY_SELECT_SEQ_WAIT_CHOSEN_PRINT,
    KEY_SELECT_SEQ_RESET_SELECT,
    KEY_SELECT_SEQ_PRINT_LOCKED,
    KEY_SELECT_SEQ_WAIT_LOCKED_PRINT,
    KEY_SELECT_SEQ_END_SCENE,
    KEY_SELECT_SEQ_END,
};

static void KeySystemFlow_SeqDifficulty(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case KEY_SELECT_SEQ_START_SCENE:
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartDifficultyScene);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_SELECT:
        KeySystemKeySelect_Update(&wk->keySelect);
        if (KeySystemKeySelect_IsDecided(&wk->keySelect)) {
            if (KeySystemKeySelect_GetCursor(&wk->keySelect) == DIFFICULTY_LIST_BACK) {
                *state = KEY_SELECT_SEQ_END_SCENE;
            } else {
                *state = KEY_SELECT_SEQ_SAVE;
            }
        } else if (KeySystemKeySelect_GetResult(&wk->keySelect) == KEY_SELECT_LOCKED) {
            *state = KEY_SELECT_SEQ_PRINT_LOCKED;
        }
        break;
    case KEY_SELECT_SEQ_SAVE:
        KeySystem_CreateMsgWin(wk, HEAPID_KEY_SYSTEM);
        KeySystemSeq_Push(seq, KeySystemFlow_SeqSave);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_PRINT_INFO:
        KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, KeySystemFlow_GetDifficultyMsg(wk->keyInfo),
                                 KEY_SYSTEM_MSG_PRINT);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_SAVED:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            (*state)++;
        }
        break;
    case KEY_SELECT_SEQ_PRINT_CHOSEN:
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, GetGameDifficulty(wk->keyInfo) + 112, KEY_SYSTEM_MSG_STREAM);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_CHOSEN_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            *state = KEY_SELECT_SEQ_RESET_SELECT;
        }
        break;
    case KEY_SELECT_SEQ_RESET_SELECT:
        KeySystem_FreeMsgWin(wk);
        GFL_BGSysQueueScrLoad(0);
        KeySystemKeySelect_Reset(&wk->keySelect);
        *state = KEY_SELECT_SEQ_WAIT_SELECT;
        break;
    case KEY_SELECT_SEQ_PRINT_LOCKED:
        KeySystem_CreateMsgWin(wk, HEAPID_KEY_SYSTEM);
        switch (KeySystemKeySelect_GetIndex(&wk->keySelect)) {
        case GAME_DIFFICULTY_EASY:
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 61, KEY_SYSTEM_MSG_PRINT);
            break;
        case GAME_DIFFICULTY_CHALLENGE:
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 62, KEY_SYSTEM_MSG_PRINT);
            break;
        }
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_LOCKED_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            GFL_BGSysQueueScrLoad(0);
            if (GCTX_HIDGetPressedKeys()) {
                *state = KEY_SELECT_SEQ_RESET_SELECT;
            }
        }
        break;
    case KEY_SELECT_SEQ_END_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqCity(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case KEY_SELECT_SEQ_START_SCENE:
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartCityScene);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_SELECT:
        KeySystemKeySelect_Update(&wk->keySelect);
        if (KeySystemKeySelect_IsDecided(&wk->keySelect)) {
            if (KeySystemKeySelect_GetCursor(&wk->keySelect) == CITY_LIST_BACK) {
                *state = KEY_SELECT_SEQ_END_SCENE;
            } else {
                *state = KEY_SELECT_SEQ_SAVE;
            }
        } else if (KeySystemKeySelect_GetResult(&wk->keySelect) == KEY_SELECT_LOCKED ||
                   KeySystemKeySelect_GetResult(&wk->keySelect) == KEY_SELECT_IN_AREA) {
            *state = KEY_SELECT_SEQ_PRINT_LOCKED;
        }
        break;
    case KEY_SELECT_SEQ_SAVE:
        KeySystem_CreateMsgWin(wk, HEAPID_KEY_SYSTEM);
        KeySystemSeq_Push(seq, KeySystemFlow_SeqSave);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_PRINT_INFO:
        KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, KeySystemFlow_GetCityMsg(wk->keyInfo), KEY_SYSTEM_MSG_PRINT);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_SAVED:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            (*state)++;
        }
        break;
    case KEY_SELECT_SEQ_PRINT_CHOSEN:
#ifdef BLACK2
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, KeyInfo_GetCityKey(wk->keyInfo) + 115, KEY_SYSTEM_MSG_STREAM);
#else
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, (KeyInfo_GetCityKey(wk->keyInfo) == 0 ? 1 : 0) + 115,
                                 KEY_SYSTEM_MSG_STREAM);
#endif
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_CHOSEN_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            *state = KEY_SELECT_SEQ_RESET_SELECT;
        }
        break;
    case KEY_SELECT_SEQ_RESET_SELECT:
        KeySystem_FreeMsgWin(wk);
        GFL_BGSysQueueScrLoad(0);
        KeySystemKeySelect_Reset(&wk->keySelect);
        *state = KEY_SELECT_SEQ_WAIT_SELECT;
        break;
    case KEY_SELECT_SEQ_PRINT_LOCKED:
        KeySystem_CreateMsgWin(wk, HEAPID_KEY_SYSTEM);
        if (KeySystemKeySelect_GetResult(&wk->keySelect) == KEY_SELECT_IN_AREA) {
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 68, KEY_SYSTEM_MSG_PRINT);
        } else if (KeySystemKeySelect_GetIndex(&wk->keySelect) == 1) {
#ifdef BLACK2
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 63, KEY_SYSTEM_MSG_PRINT);
#else
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 64, KEY_SYSTEM_MSG_PRINT);
#endif
        }
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_LOCKED_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            GFL_BGSysQueueScrLoad(0);
            if (GCTX_HIDGetPressedKeys()) {
                *state = KEY_SELECT_SEQ_RESET_SELECT;
            }
        }
        break;
    case KEY_SELECT_SEQ_END_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqChamber(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case KEY_SELECT_SEQ_START_SCENE:
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartChamberScene);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_SELECT:
        KeySystemKeySelect_Update(&wk->keySelect);
        if (KeySystemKeySelect_IsDecided(&wk->keySelect)) {
            if (KeySystemKeySelect_GetCursor(&wk->keySelect) == CHAMBER_LIST_BACK) {
                *state = KEY_SELECT_SEQ_END_SCENE;
            } else {
                *state = KEY_SELECT_SEQ_SAVE;
            }
        } else if (KeySystemKeySelect_GetResult(&wk->keySelect) == KEY_SELECT_LOCKED) {
            *state = KEY_SELECT_SEQ_PRINT_LOCKED;
        }
        break;
    case KEY_SELECT_SEQ_SAVE:
        KeySystem_CreateMsgWin(wk, HEAPID_KEY_SYSTEM);
        KeySystemSeq_Push(seq, KeySystemFlow_SeqSave);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_PRINT_INFO:
        KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, KeySystemFlow_GetChamberMsg(wk->keyInfo),
                                 KEY_SYSTEM_MSG_PRINT);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_SAVED:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            (*state)++;
        }
        break;
    case KEY_SELECT_SEQ_PRINT_CHOSEN:
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, KeyInfo_GetChamberKey(wk->keyInfo) + 117,
                                 KEY_SYSTEM_MSG_STREAM);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_CHOSEN_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            *state = KEY_SELECT_SEQ_RESET_SELECT;
        }
        break;
    case KEY_SELECT_SEQ_RESET_SELECT:
        KeySystem_FreeMsgWin(wk);
        GFL_BGSysQueueScrLoad(0);
        KeySystemKeySelect_Reset(&wk->keySelect);
        *state = KEY_SELECT_SEQ_WAIT_SELECT;
        break;
    case KEY_SELECT_SEQ_PRINT_LOCKED:
        KeySystem_CreateMsgWin(wk, HEAPID_KEY_SYSTEM);
        switch (KeySystemKeySelect_GetIndex(&wk->keySelect)) {
        case KEY_INFO_CHAMBER_IRON:
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 97, KEY_SYSTEM_MSG_PRINT);
            break;
        case KEY_INFO_CHAMBER_ICEBERG:
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 98, KEY_SYSTEM_MSG_PRINT);
            break;
        }
        (*state)++;
        break;
    case KEY_SELECT_SEQ_WAIT_LOCKED_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            GFL_BGSysQueueScrLoad(0);
            if (GCTX_HIDGetPressedKeys()) {
                *state = KEY_SELECT_SEQ_RESET_SELECT;
            }
        }
        break;
    case KEY_SELECT_SEQ_END_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        (*state)++;
        break;
    case KEY_SELECT_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

enum {
    RECEIVE_SEQ_CHECK_EXCHANGE,
    RECEIVE_SEQ_PRINT_NOTHING,
    RECEIVE_SEQ_WAIT_NOTHING_PRINT,
    RECEIVE_SEQ_START_SCENE,
    RECEIVE_SEQ_SYNC,
    RECEIVE_SEQ_WAIT_SYNC,
    RECEIVE_SEQ_NEXT_KEY,
    RECEIVE_SEQ_WAIT_KEY_ANIM,
    RECEIVE_SEQ_END_SCENE,
    RECEIVE_SEQ_END,
};

static void KeySystemFlow_SeqReceive(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case RECEIVE_SEQ_CHECK_EXCHANGE:
        if (KeySystemKeyState_CanExchange(&wk->keys, &wk->partnerKeys, TRUE)) {
            *state = RECEIVE_SEQ_START_SCENE;
        } else {
            *state = RECEIVE_SEQ_PRINT_NOTHING;
        }
        break;
    case RECEIVE_SEQ_PRINT_NOTHING:
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 59, KEY_SYSTEM_MSG_STREAM_FAST);
        (*state)++;
        break;
    case RECEIVE_SEQ_WAIT_NOTHING_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            *state = RECEIVE_SEQ_END;
        }
        break;
    case RECEIVE_SEQ_START_SCENE:
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartKeyAnimScene);
        wk->timer = 0;
        wk->choice = 0;
        (*state)++;
        break;
    case RECEIVE_SEQ_SYNC:
        KeySystemNet_Request(wk->net, KEY_SYSTEM_NET_REQUEST_SYNC, NULL);
        (*state)++;
        break;
    case RECEIVE_SEQ_WAIT_SYNC:
        if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_CONNECTED) {
            (*state)++;
        }
        break;
    case RECEIVE_SEQ_NEXT_KEY:
        // The key that is looked at
        while (TRUE) {
            if (wk->choice >= KEY_SYSTEM_KEY_COUNT) {
                *state = RECEIVE_SEQ_END_SCENE;
                return;
            }
            if (KeySystemFlow_FormatKeyMsg(wk, wk->choice, TRUE)) {
                KeySystemKeyAnim_Start(&wk->keyAnim, wk->choice, wk->msgData,
                                       sReceivedKeyNameMsgs[VERSION_INDEX][wk->choice], wk->strBuf, FALSE);
                KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 43, KEY_SYSTEM_MSG_STREAM_FAST);
                (*state)++;
                return;
            }
            wk->choice++;
        }
    case RECEIVE_SEQ_WAIT_KEY_ANIM:
        if (KeySystemKeyAnim_Update(&wk->keyAnim)) {
            wk->timer = 0;
            wk->choice++;
            *state = RECEIVE_SEQ_SYNC;
        }
        break;
    case RECEIVE_SEQ_END_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        (*state)++;
        break;
    case RECEIVE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

enum {
    SEND_SEQ_CHECK_EXCHANGE,
    SEND_SEQ_PRINT_NOTHING,
    SEND_SEQ_WAIT_NOTHING_PRINT,
    SEND_SEQ_START_SCENE,
    SEND_SEQ_WAIT_PRINT,
    SEND_SEQ_SYNC,
    SEND_SEQ_WAIT_SYNC,
    SEND_SEQ_NEXT_KEY,
    SEND_SEQ_WAIT_KEY_PRINT,
    SEND_SEQ_END_SCENE,
    SEND_SEQ_END,
};

static void KeySystemFlow_SeqSend(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SEND_SEQ_CHECK_EXCHANGE:
        if (KeySystemKeyState_CanExchange(&wk->keys, &wk->partnerKeys, FALSE)) {
            *state = SEND_SEQ_START_SCENE;
        } else {
            *state = SEND_SEQ_PRINT_NOTHING;
        }
        break;
    case SEND_SEQ_PRINT_NOTHING:
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 58, KEY_SYSTEM_MSG_STREAM_FAST);
        (*state)++;
        break;
    case SEND_SEQ_WAIT_NOTHING_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            *state = SEND_SEQ_END;
        }
        break;
    case SEND_SEQ_START_SCENE:
        KeySystemSeq_Push(seq, KeySystemFlow_SeqStartSendScene);
        wk->timer = 0;
        wk->choice = 0;
        (*state)++;
        break;
    case SEND_SEQ_WAIT_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            wk->timer = 0;
            (*state)++;
        }
        break;
    case SEND_SEQ_SYNC:
        KeySystemNet_Request(wk->net, KEY_SYSTEM_NET_REQUEST_SYNC, NULL);
        (*state)++;
        break;
    case SEND_SEQ_WAIT_SYNC:
        if (KeySystemNet_GetState(wk->net) == KEY_SYSTEM_NET_STATE_CONNECTED) {
            (*state)++;
        }
        break;
    case SEND_SEQ_NEXT_KEY:
        while (TRUE) {
            if (wk->choice >= KEY_SYSTEM_KEY_COUNT) {
                *state = SEND_SEQ_END_SCENE;
                return;
            }
            if (KeySystemFlow_FormatKeyMsg(wk, wk->choice, FALSE)) {
                KeySystemMsgWin_PrintStr(wk->msgWin, wk->strBuf, KEY_SYSTEM_MSG_STREAM_FAST);
                (*state)++;
                return;
            }
            wk->choice++;
        }
    case SEND_SEQ_WAIT_KEY_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            wk->timer = 0;
            wk->choice++;
            *state = SEND_SEQ_SYNC;
        }
        break;
    case SEND_SEQ_END_SCENE:
        KeySystemSeq_Push(seq, KeySystem_SeqEndScene);
        (*state)++;
        break;
    case SEND_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

enum {
    SAVE_SEQ_PRINT_SAVING,
    SAVE_SEQ_START_SAVE,
    SAVE_SEQ_WAIT_SAVE,
    SAVE_SEQ_PLAY_SE,
    SAVE_SEQ_PAUSE,
    SAVE_SEQ_PRINT_SAVED,
    SAVE_SEQ_WAIT_SAVED_PRINT,
    SAVE_SEQ_END,
};

static void KeySystemFlow_SeqSave(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;
    u32 mode;

    switch (*state) {
    case SAVE_SEQ_PRINT_SAVING:
        KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 29, KEY_SYSTEM_MSG_PRINT_WAIT_ICON);
        (*state)++;
        break;
    case SAVE_SEQ_START_SAVE:
        if (KeySystemMsgWin_IsDone(wk->msgWin)) {
            GFL_BGSysQueueScrLoad(0);
            if (SaveControl_IsDataAlreadyPresent(GameData_GetSaveControl(wk->param->gameData))) {
                GameData_StartSave(wk->param->gameData);
            } else {
                SaveOutside_StartSave(wk->ov331Work);
            }
            wk->timer = 0;
            (*state)++;
        }
        break;
    case SAVE_SEQ_WAIT_SAVE:
        wk->timer++;
        if (SaveControl_IsDataAlreadyPresent(GameData_GetSaveControl(wk->param->gameData))) {
            if (GameData_UpdateSave(wk->param->gameData) == 2) {
                (*state)++;
            }
        } else if (SaveOutside_Save(wk->ov331Work)) {
            (*state)++;
        }
        break;
    case SAVE_SEQ_PLAY_SE:
        GFL_SndSEPlay(SEQ_SE_SAVE);
        *state = SAVE_SEQ_PAUSE;
        break;
    case SAVE_SEQ_PAUSE:
        *state = SAVE_SEQ_PRINT_SAVED;
        break;
    case SAVE_SEQ_PRINT_SAVED:
        if (wk->timer++ > 60) {
            mode = KEY_SYSTEM_MSG_STREAM;
            if (GFL_NetIsInitialized()) {
                mode = KEY_SYSTEM_MSG_STREAM_FAST;
            }
            wk->timer = 0;
            KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 56, mode);
            (*state)++;
        }
        break;
    case SAVE_SEQ_WAIT_SAVED_PRINT:
        if (KeySystemMsgWin_IsDone(wk->msgWin) && !GFL_SndPlayerIsActiveAny()) {
            (*state)++;
        }
        break;
    case SAVE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

enum {
    SCENE_SEQ_START,
    SCENE_SEQ_WAIT_IDLE,
    SCENE_SEQ_END,
};

static void KeySystemFlow_SeqStartMenuScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sMenuSceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqStartConnectScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sConnectSceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqStartSettingsScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sSettingsSceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqStartDifficultyScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sDifficultySceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqStartCityScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sCitySceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqStartKeyAnimScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sKeyAnimSceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqStartSendScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sSendSceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqStartKeysHeldScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sKeysHeldSceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

static void KeySystemFlow_SeqStartChamberScene(KeySystemSeq *seq, int *state, void *work) {
    KeySystemWork *wk = work;

    switch (*state) {
    case SCENE_SEQ_START:
        KeySystemScene_Start(wk->scene, &sChamberSceneFuncs, HEAPID_KEY_SYSTEM);
        (*state)++;
        break;
    case SCENE_SEQ_WAIT_IDLE:
        if (KeySystemScene_IsIdle(wk->scene)) {
            (*state)++;
        }
        break;
    case SCENE_SEQ_END:
        KeySystemSeq_Pop(seq);
        break;
    }
}

void KeySystemFlow_MenuSceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;
    KeySystemListSetup setup;
    u32 i;

    KeySystemMsgWin_PrintMsg(wk->titleWin, wk->msgData, 1, KEY_SYSTEM_MSG_PRINT);
    MI_CpuFill8(&setup, 0, sizeof(KeySystemListSetup));
    setup.bg = 1;
    setup.unk04 = 14;
    setup.frameChar = 10;
    setup.palette = 1;
    setup.msgData = wk->msgData;
    setup.font = wk->font;
    setup.count = 3;
    if (KeySystemTags_Has(wk->tags, "KEM")) {
        setup.cursor = KeySystemTags_Get(wk->tags, "KEM");
        KeySystemTags_Remove(wk->tags, "KEM");
    } else {
        setup.cursor = 0;
    }
    for (i = 0; i < setup.count; i++) {
        setup.items[i].width = 26;
        setup.items[i].height = 2;
        setup.items[i].x = 3;
        setup.items[i].y = (setup.items[i].height + 3) * i + 5;
    }
    setup.items[0].msgId = 2;
    setup.items[1].msgId = 3;
    setup.items[2].msgId = 7;
    wk->list = KeySystemList_Create(&setup, heapId);
    KeySystem_CreateInfoWin(wk, heapId);
    KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, setup.cursor + 18, KEY_SYSTEM_MSG_PRINT);
}

BOOL KeySystemFlow_MenuSceneMain(void *work) {
    KeySystemWork *wk = work;
    BOOL titleDone = KeySystemMsgWin_IsDone(wk->titleWin);
    BOOL infoDone = KeySystemMsgWin_IsDone(wk->infoWin);
    BOOL listDone = KeySystemList_IsPrinted(wk->list);

    if (titleDone && infoDone && listDone) {
        GFL_BGSysQueueScrLoad(0);
        GFL_BGSysQueueScrLoad(1);
        GFL_BGSysQueueScrLoad(4);
        return TRUE;
    }
    return FALSE;
}

void KeySystemFlow_MenuSceneExit(void *work) {
    KeySystemWork *wk = work;

    KeySystem_FreeInfoWin(wk);
    KeySystemList_Free(wk->list);
    wk->list = NULL;
    GFL_BGSysQueueScrLoad(0);
    GFL_BGSysQueueScrLoad(1);
}

void KeySystemFlow_ConnectSceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;

    KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 21, KEY_SYSTEM_MSG_PRINT_WAIT_ICON);
    KeySystemClAct_Create(&wk->clact, 0, heapId);
    KeySystemClAct_Create(&wk->clact, 1, heapId);
    KeySystemClAct_Create(&wk->clact, 2, heapId);
    KeySystemClAct_Create(&wk->clact, 3, heapId);
    G2x_SetBlendAlpha_(REG_BLDCNT_ADDR, GX_PLANEMASK_OBJ, GX_PLANEMASK_BG1 | GX_PLANEMASK_BG2 | GX_PLANEMASK_BG3, 16,
                       6);
    GX_SetVisibleWnd(GX_WNDMASK_W0);
    G2_SetWnd0Position(224, 0, 0, 32);
    G2_SetWnd0InsidePlane(GX_PLANEMASK_ALL, FALSE);
    G2_SetWndOutsidePlane(GX_PLANEMASK_ALL, TRUE);
}

BOOL KeySystemFlow_ConnectSceneMain(void *work) {
    KeySystemWork *wk = work;

    if (KeySystemMsgWin_IsDone(wk->msgWin)) {
        GFL_BGSysQueueScrLoad(0);
        return TRUE;
    }
    return FALSE;
}

void KeySystemFlow_ConnectSceneExit(void *work) {
    KeySystemWork *wk = work;

    if (wk->msgWinGroup != NULL) {
        GFL_BGSysClearScr(6);
        KeySystemMsgWinGroup_Free(wk->msgWinGroup);
        wk->msgWinGroup = NULL;
    }
    G2_BlendNone();
    GX_SetVisibleWnd(GX_WNDMASK_NONE);
    KeySystemClAct_Delete(&wk->clact, 3);
    KeySystemClAct_Delete(&wk->clact, 2);
    KeySystemClAct_Delete(&wk->clact, 1);
    KeySystemClAct_Delete(&wk->clact, 0);
    KeySystemMsgWin_StopWaitIcon(wk->msgWin);
    GFL_BGSysQueueScrLoad(4);
    GFL_BGSysQueueScrLoad(6);
}

void KeySystemFlow_SettingsSceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;
    KeySystemListSetup setup;
    u32 i;

    MI_CpuFill8(&setup, 0, sizeof(KeySystemListSetup));
    setup.bg = 1;
    setup.unk04 = 14;
    setup.frameChar = 10;
    setup.palette = 1;
    setup.msgData = wk->msgData;
    setup.font = wk->font;
    setup.count = 4;
    if (KeySystemTags_Has(wk->tags, "KLL")) {
        setup.cursor = KeySystemTags_Get(wk->tags, "KLL");
        KeySystemTags_Remove(wk->tags, "KLL");
    } else {
        setup.cursor = 0;
    }
    for (i = 0; i < setup.count; i++) {
        setup.items[i].width = 26;
        setup.items[i].height = 2;
        setup.items[i].x = 3;
        setup.items[i].y = (setup.items[i].height + 3) * i + 5;
    }
    setup.items[0].msgId = 5;
    setup.items[1].msgId = 6;
    setup.items[2].msgId = 99;
    setup.items[3].msgId = 7;
    wk->list = KeySystemList_Create(&setup, heapId);
    KeySystem_CreateInfoWin(wk, heapId);
    KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, KeySystemFlow_GetSettingsMsg(wk->keyInfo, setup.cursor),
                             KEY_SYSTEM_MSG_PRINT);
}

BOOL KeySystemFlow_SettingsSceneMain(void *work) {
    KeySystemWork *wk = work;
    BOOL infoDone = KeySystemMsgWin_IsDone(wk->infoWin);
    BOOL listDone = KeySystemList_IsPrinted(wk->list);

    if (infoDone && listDone) {
        GFL_BGSysQueueScrLoad(0);
        GFL_BGSysQueueScrLoad(1);
        GFL_BGSysQueueScrLoad(4);
        return TRUE;
    }
    return FALSE;
}

void KeySystemFlow_SettingsSceneExit(void *work) {
    KeySystemWork *wk = work;

    KeySystemList_Free(wk->list);
    wk->list = NULL;
    KeySystem_FreeInfoWin(wk);
    GFL_BGSysQueueScrLoad(0);
    GFL_BGSysQueueScrLoad(1);
}

void KeySystemFlow_DifficultySceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;

    KeySystem_CreateInfoWin(wk, heapId);
    KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, KeySystemFlow_GetDifficultyMsg(wk->keyInfo),
                             KEY_SYSTEM_MSG_PRINT);
    KeySystemKeySelect_Init(&wk->keySelect, KEY_LIST_DIFFICULTY, &wk->clact, &wk->list, wk->msgData, wk->font,
                            wk->keyInfo, wk->param->gameData, heapId);
}

BOOL KeySystemFlow_DifficultySceneMain(void *work) {
    KeySystemWork *wk = work;
    BOOL infoDone = KeySystemMsgWin_IsDone(wk->infoWin);
    BOOL listDone = KeySystemKeySelect_IsPrinted(&wk->keySelect);

    if (infoDone && listDone) {
        GFL_BGSysQueueScrLoad(1);
        GFL_BGSysQueueScrLoad(4);
        return TRUE;
    }
    return FALSE;
}

void KeySystemFlow_DifficultySceneExit(void *work) {
    KeySystemWork *wk = work;

    KeySystemKeySelect_Free(&wk->keySelect);
    KeySystem_FreeInfoWin(wk);
    GFL_BGSysQueueScrLoad(0);
    GFL_BGSysQueueScrLoad(1);
}

void KeySystemFlow_CitySceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;

    KeySystem_CreateInfoWin(wk, heapId);
    KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, KeySystemFlow_GetCityMsg(wk->keyInfo), KEY_SYSTEM_MSG_PRINT);
    KeySystemKeySelect_Init(&wk->keySelect, KEY_LIST_CITY, &wk->clact, &wk->list, wk->msgData, wk->font, wk->keyInfo,
                            wk->param->gameData, heapId);
}

BOOL KeySystemFlow_CitySceneMain(void *work) {
    KeySystemWork *wk = work;
    BOOL infoDone = KeySystemMsgWin_IsDone(wk->infoWin);
    BOOL listDone = KeySystemKeySelect_IsPrinted(&wk->keySelect);

    if (infoDone && listDone) {
        GFL_BGSysQueueScrLoad(1);
        GFL_BGSysQueueScrLoad(4);
        return TRUE;
    }
    return FALSE;
}

void KeySystemFlow_CitySceneExit(void *work) {
    KeySystemWork *wk = work;

    KeySystemKeySelect_Free(&wk->keySelect);
    KeySystem_FreeInfoWin(wk);
    GFL_BGSysQueueScrLoad(0);
    GFL_BGSysQueueScrLoad(1);
}

void KeySystemFlow_KeyAnimSceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;

    KeySystemKeyAnim_Init(&wk->keyAnim, &wk->clact, wk->msgWin, wk->font, heapId);
}

BOOL KeySystemFlow_KeyAnimSceneMain(void *work) {
    return TRUE;
}

void KeySystemFlow_KeyAnimSceneExit(void *work) {
    KeySystemWork *wk = work;

    KeySystemKeyAnim_Free(&wk->keyAnim);
}

void KeySystemFlow_SendSceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;
    u32 count = 0;
    int i;

    for (i = 0; i < KEY_SYSTEM_KEY_COUNT; i++) {
        if (wk->keys.unlocked[i]) {
            // BUG: The fifth key's name goes past the table, into sKeysHeldTemplates' title
            sSendTemplates[1 + count++].msgId = sSendKeyNameMsgs[VERSION_INDEX][i];
        }
    }
    if (count + 1 > NELEMS(sSendTemplates)) {
        count = NELEMS(sSendTemplates) - 1;
    }
    wk->msgWinGroup = KeySystemMsgWinGroup_Create(sSendTemplates, count + 1, 0, 14, wk->font, wk->msgData, heapId);
    KeySystemBG_LoadScreen(wk->bg, 2, 1);
    KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 30, KEY_SYSTEM_MSG_STREAM_FAST);
}

BOOL KeySystemFlow_SendSceneMain(void *work) {
    KeySystemWork *wk = work;

    if (KeySystemMsgWinGroup_IsDone(wk->msgWinGroup)) {
        GFL_BGSysQueueScrLoad(0);
        GFL_BGSysQueueScrLoad(2);
        return TRUE;
    }
    return FALSE;
}

void KeySystemFlow_SendSceneExit(void *work) {
    KeySystemWork *wk = work;

    KeySystemMsgWinGroup_Free(wk->msgWinGroup);
    wk->msgWinGroup = NULL;
    if (wk->msgWin != NULL) {
        KeySystemMsgWin_DrawFrame(wk->msgWin, 1, 15);
        KeySystemMsgWin_StopWaitIcon(wk->msgWin);
    }
    KeySystemBG_LoadScreen(wk->bg, 2, 0);
    GFL_BGSysQueueScrLoad(0);
    GFL_BGSysQueueScrLoad(2);
}

void KeySystemFlow_KeysHeldSceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;
    u32 count = 0;
    int i;

    KeySystemMsgWin_PrintMsg(wk->msgWin, wk->msgData, 0, KEY_SYSTEM_MSG_PRINT);
    if (wk->msgWinGroup == NULL) {
        for (i = 0; i < KEY_SYSTEM_KEY_COUNT; i++) {
            if (wk->keys.unlocked[i]) {
                sKeysHeldTemplates[1 + count++].msgId = sHeldKeyNameMsgs[VERSION_INDEX][i];
            }
        }
        wk->msgWinGroup =
            KeySystemMsgWinGroup_Create(sKeysHeldTemplates, count + 1, 4, 14, wk->font, wk->msgData, heapId);
    }
    KeySystemBG_LoadScreen(wk->bg, 6, 2);
}

BOOL KeySystemFlow_KeysHeldSceneMain(void *work) {
    KeySystemWork *wk = work;
    BOOL groupDone = KeySystemMsgWinGroup_IsDone(wk->msgWinGroup);
    BOOL msgDone = KeySystemMsgWin_IsDone(wk->msgWin);

    if (groupDone && msgDone) {
        GFL_BGSysQueueScrLoad(0);
        GFL_BGSysQueueScrLoad(4);
        GFL_BGSysQueueScrLoad(6);
        return TRUE;
    }
    return FALSE;
}

void KeySystemFlow_KeysHeldSceneExit(void *work) {
    GFL_BGSysQueueScrLoad(0);
}

void KeySystemFlow_ChamberSceneInit(void *work, HeapID heapId) {
    KeySystemWork *wk = work;

    KeySystem_CreateInfoWin(wk, heapId);
    KeySystemMsgWin_PrintMsg(wk->infoWin, wk->msgData, KeySystemFlow_GetChamberMsg(wk->keyInfo), KEY_SYSTEM_MSG_PRINT);
    KeySystemKeySelect_Init(&wk->keySelect, KEY_LIST_CHAMBER, &wk->clact, &wk->list, wk->msgData, wk->font, wk->keyInfo,
                            wk->param->gameData, heapId);
}

BOOL KeySystemFlow_ChamberSceneMain(void *work) {
    KeySystemWork *wk = work;
    BOOL infoDone = KeySystemMsgWin_IsDone(wk->infoWin);
    BOOL listDone = KeySystemKeySelect_IsPrinted(&wk->keySelect);

    if (infoDone && listDone) {
        GFL_BGSysQueueScrLoad(1);
        GFL_BGSysQueueScrLoad(4);
        return TRUE;
    }
    return FALSE;
}

void KeySystemFlow_ChamberSceneExit(void *work) {
    KeySystemWork *wk = work;

    KeySystemKeySelect_Free(&wk->keySelect);
    KeySystem_FreeInfoWin(wk);
    GFL_BGSysQueueScrLoad(0);
    GFL_BGSysQueueScrLoad(1);
}
