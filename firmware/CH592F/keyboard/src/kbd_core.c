/**
 * @file    kbd_core.c
 * @brief   MeowKeyboard 核心处理模块实现
 * @author  MeowKJ
 * @version V2.0.0
 * @date    2024-11-07
 */

#include "kbd_core.h"
#include "kbd_storage.h"
#include "kbd_macro.h"
#include "kbd_rgb.h"
#include "kbd_mode.h"
#include "kbd_log.h"
#include "hal_utils.h"
#include "key.h"
#include "encoder.h"
#include "debug.h"
#include <string.h>

#define TAG "CORE"

/*============================================================================*/
/* 私有变量 */
/*============================================================================*/

/** 当前按下的按键 (用于多键同时按下，HID 最多 6 键) */
static uint8_t s_pressed_keys[6] = {0};
static uint8_t s_pressed_count = 0;
static uint8_t s_current_modifier = 0;
static uint8_t s_current_mouse_buttons = 0;
static uint8_t s_keycode_slots[KBD_MAX_KEYS] = {0};
static uint8_t s_keycode_refcount[KBD_MAX_KEYS] = {0};
static uint8_t s_modifier_refcount[8] = {0};
static uint8_t s_mouse_button_refcount[5] = {0};
static kbd_action_t s_active_actions[KBD_MAX_KEYS];
static uint8_t s_active_action_valid[KBD_MAX_KEYS] = {0};
static uint8_t s_momentary_layer_active[KBD_MAX_KEYS] = {0};
static uint8_t s_momentary_restore_layer[KBD_MAX_KEYS] = {0};
/** Latest complete keyboard state has not yet been accepted by the transport. */
static uint8_t s_keyboard_report_dirty = 1;

/*============================================================================*/
/* 单击 / 双击 / 长按 判定状态机                                              */
/*============================================================================*/

/**
 * @brief 单个按键的判定状态
 */
typedef enum
{
    KSTATE_IDLE = 0,    /**< 空闲 */
    KSTATE_PRESS_HELD,  /**< 已按下，等待长按阈值到期（尚未执行任何动作） */
    KSTATE_ACTIVE,      /**< 动作已按下，等待释放以执行释放 */
    KSTATE_DOUBLE_WAIT, /**< 已松开，等待双击窗口内的第二次按下 */
} kbd_key_state_t;

typedef struct
{
    uint8_t state;        /**< @ref kbd_key_state_t */
    uint8_t slot;         /**< KSTATE_ACTIVE 时生效的动作槽 */
    uint32_t deadline_ms; /**< 到期时刻；0 表示不超时（等待松开） */
} kbd_key_fsm_t;

static kbd_key_fsm_t s_key_fsm[KBD_MAX_KEYS];
/** 上一次观察到的驱动 tick，用于识别休眠唤醒后的计数复位 */
static uint32_t s_last_tick_ms = 0;

static const uint8_t s_modifier_bits[8] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
};

static const uint8_t s_mouse_button_bits[5] = {
    KBD_MOUSE_LEFT,
    KBD_MOUSE_RIGHT,
    KBD_MOUSE_MIDDLE,
    KBD_MOUSE_BACK,
    KBD_MOUSE_FORWARD,
};

/*============================================================================*/
/* 私有函数声明 */
/*============================================================================*/

static void ResetInputState(void);
static void RebuildKeyboardReport(void);
static bool TrySyncKeyboardReport(void);
static void DiscardQueuedInput(void);
static void UpdateKeycodeRefcount(uint8_t keycode, bool pressed);
static void UpdateModifierMask(uint8_t mask, bool pressed);
static void UpdateMouseButtons(uint8_t buttons, bool pressed);
static void SwitchLayer(uint8_t target_layer);
static void ResetKeyFsm(void);
static void KbdCore_TickKeys(uint32_t now_ms);
static bool GestureEnabledForKey(uint8_t key_index);
static bool IsSlotConfigured(uint8_t key_index, uint8_t slot);
static void HandlePlainKey(uint8_t key_index, bool pressed);
static void AbortGesture(uint8_t key_index);
static void GesturePress(uint8_t key_index, uint32_t now_ms);
static void GestureRelease(uint8_t key_index, uint32_t now_ms);
static void StartSlotAction(uint8_t key_index, uint8_t slot);
static void FinishActiveAction(uint8_t key_index);
static void FireTapAction(uint8_t key_index, uint8_t slot);
static void ExecuteKeyAction(uint8_t key_index, const kbd_action_t *action, bool pressed);
static void ExecuteFnAction(kbd_fn_action_t action, uint8_t param);
static void OnModeChange(kbd_work_mode_t new_mode);
static void OnConnStateChange(kbd_conn_state_t state);
static void OnLedReport(uint8_t leds);

/*============================================================================*/
/* 模式管理回调结构 */
/*============================================================================*/

static kbd_mode_callbacks_t s_callbacks = {
    .onModeChange = OnModeChange,
    .onConnStateChange = OnConnStateChange,
    .onLedReport = OnLedReport,
};

/*============================================================================*/
/* 公共函数实现 */
/*============================================================================*/

/**
 * @brief 初始化键盘核心模块
 */
void KBD_Core_Init(void)
{
    ResetInputState();
    LOG_I(TAG, "Core initialized");
}

/**
 * @brief 核心处理函数（主循环调用）
 */
void KBD_Core_Process(void)
{
    key_event_t key_evt;
    fnkey_event_t fn_evt;

    Key_ServiceDeepWakeKeys(KBD_Mode_IsInputReady() ? 1u : 0u);
    if (Key_IsDeepWakeRecoveryPending())
    {
        DiscardQueuedInput();
        return;
    }

    if (Key_IsDeepWakeSyncPending())
    {
        DiscardQueuedInput();
        ResetInputState();
        if (TrySyncKeyboardReport())
        {
            Key_FinishDeepWakeSync();
        }
        return;
    }

    if (!KBD_Mode_IsInputReady())
    {
        DiscardQueuedInput();
        ResetInputState();
        return;
    }

    if (!TrySyncKeyboardReport())
    {
        return;
    }

    /* 推进单击 / 双击 / 长按 判定的定时器 */
    KbdCore_TickKeys(Key_GetTickMs());

    /* 处理普通按键事件 */
    while (Key_GetEvent(&key_evt))
    {
        KBD_Core_HandleKeyEvent(&key_evt);
        if (!TrySyncKeyboardReport())
        {
            return;
        }
    }

    /* 处理旋钮事件 */
    while (Encoder_GetEvent(&key_evt))
    {
        KBD_Core_HandleKeyEvent(&key_evt);
        if (!TrySyncKeyboardReport())
        {
            return;
        }
    }

    /* 处理 FN 按键事件 */
    while (FnKey_GetEvent(&fn_evt))
    {
        KBD_Core_HandleFnEvent(&fn_evt);
    }
}

/**
 * @brief 检查 BOOT 层切换修饰键是否被按下
 */
static bool IsBootLayerModifierHeld(void) { return (BootKey_IsPressed() == 1); }

/**
 * @brief 处理普通按键事件
 */
void KBD_Core_HandleKeyEvent(const key_event_t *evt)
{
    if (evt == NULL || evt->key >= KBD_MAX_KEYS)
        return;

    KBD_Mode_RecordActivity();

    bool pressed = (evt->type == KEY_EVT_PRESS);
    const uint32_t now_ms = Key_GetTickMs();

    /* 按下效果：记录按键事件到 RGB 引擎 */
    if (pressed)
    {
        KBD_RGB_RegisterKeyPress(evt->key);
    }

    /* 层切换组合键：按住 BOOT + 按键 = 切换到对应层 */
    if (IsBootLayerModifierHeld() && pressed)
    {
        uint8_t target_layer = evt->key; /* 按键0->层0, 按键1->层1... */
        kbd_keymap_t *keymap = KBD_GetKeymap();

        if (target_layer < keymap->num_layers)
        {
            LOG_I(TAG, "BOOT+Key%d -> Layer %d", evt->key, target_layer);
            AbortGesture(evt->key); /* 丢弃该键待定的双击/长按判定 */
            SwitchLayer(target_layer);
            memset(&s_active_actions[evt->key], 0, sizeof(s_active_actions[evt->key]));
            s_active_action_valid[evt->key] = 1;
            return; /* 不执行按键原本的动作 */
        }
    }

    /* 不支持手势的键位（如旋钮虚拟键）保持原有即时行为 */
    if (!GestureEnabledForKey(evt->key))
    {
        HandlePlainKey(evt->key, pressed);
        return;
    }

    if (pressed)
    {
        GesturePress(evt->key, now_ms);
    }
    else
    {
        GestureRelease(evt->key, now_ms);
    }
}

/**
 * @brief 处理 FN 按键事件
 */
void KBD_Core_HandleFnEvent(const fnkey_event_t *evt)
{
    if (evt == NULL || evt->id >= KBD_MAX_FN_KEYS)
        return;

    KBD_Mode_RecordActivity();

    kbd_fnkey_config_t *fnkey_cfg = KBD_GetFnKeyConfig();
    kbd_fnkey_entry_t *entry = &fnkey_cfg->fn[evt->id];

    if (evt->type == FNKEY_EVT_LONG)
    {
        LOG_D(TAG, "FN%d long", evt->id + 1);
        KBD_Log_FnEvent(evt->id, 1, entry->long_action, entry->long_param);
        if (entry->long_action == KBD_FN_SLEEP)
        {
            KBD_Mode_SuppressWakeForFn(evt->id);
        }
        ExecuteFnAction((kbd_fn_action_t)entry->long_action, entry->long_param);
    }
    else
    {
        LOG_D(TAG, "FN%d click", evt->id + 1);
        KBD_Log_FnEvent(evt->id, 0, entry->click_action, entry->click_param);
        ExecuteFnAction((kbd_fn_action_t)entry->click_action, entry->click_param);
    }
}

/**
 * @brief 释放所有按键
 */
void KBD_Core_ReleaseAll(void)
{
    ResetInputState();
    TrySyncKeyboardReport();
    KBD_Mode_SendMouseReport(0, 0, 0, 0);
    KBD_Mode_SendConsumerReport(0);
}

/**
 * @brief 获取双模回调结构
 */
void *KBD_Core_GetCallbacks(void)
{
    return &s_callbacks;
}

/*============================================================================*/
/* 模式管理回调实现 */
/*============================================================================*/

/**
 * @brief 模式切换回调
 */
static void OnModeChange(kbd_work_mode_t new_mode)
{
    uint8_t old_mode = (new_mode == KBD_WORK_MODE_USB) ? 1 : 0; /* 反推旧模式 */
    LOG_I(TAG, "mode changed: %s", (new_mode == KBD_WORK_MODE_USB) ? "USB" : "BLE");
    KBD_Log_ModeEvent(old_mode, (uint8_t)new_mode);

    /* 切换模式时释放所有按键 */
    KBD_Core_ReleaseAll();

    /* RGB 状态指示 + 切换确认闪烁（200ms） */
    if (new_mode == KBD_WORK_MODE_USB)
    {
        KBD_RGB_Flash(255, 255, 255, 200); /* 白色短闪 = 进入 USB 模式 */
        KBD_RGB_SetState(KBD_STATE_USB_CONNECTED);
    }
    else
    {
        KBD_RGB_Flash(0, 80, 255, 200); /* 蓝色短闪 = 进入 BLE 模式 */
        /* 根据实际连接状态设置指示灯，避免覆盖已进入广播的蓝色状态 */
        kbd_conn_state_t conn = KBD_Mode_GetConnState();
        if (conn == KBD_CONN_ADVERTISING)
        {
            KBD_RGB_SetState(KBD_STATE_BLE_ADVERTISING);
        }
        else
        {
            KBD_RGB_SetState(KBD_STATE_BLE_DISCONNECTED);
        }
    }
}

/**
 * @brief 连接状态变化回调
 */
static void OnConnStateChange(kbd_conn_state_t state)
{
    LOG_I(TAG, "conn state: %d", state);
    KBD_Log_BleEvent((uint8_t)state);

    /* RGB 状态指示 */
    if (KBD_Mode_Get() == KBD_WORK_MODE_USB)
    {
        KBD_RGB_SetState(KBD_STATE_USB_CONNECTED);
    }
    else
    {
        switch (state)
        {
        case KBD_CONN_DISCONNECTED:
            KBD_RGB_SetState(KBD_STATE_BLE_DISCONNECTED);
            break;
        case KBD_CONN_ADVERTISING:
            KBD_RGB_SetState(KBD_STATE_BLE_ADVERTISING);
            break;
        case KBD_CONN_CONNECTED:
            KBD_RGB_SetState(KBD_STATE_BLE_CONNECTED);
            break;
        case KBD_CONN_SUSPENDED:
            KBD_RGB_SetState(KBD_STATE_BLE_DISCONNECTED);
            break;
        }
    }
}

/**
 * @brief LED 报告回调（来自主机的 Caps Lock 等状态）
 */
static void OnLedReport(uint8_t leds)
{
    LOG_D(TAG, "led report: 0x%02X", leds);
    /* 可在此控制物理 LED 指示灯 */
}

/*============================================================================*/
/* 私有函数实现 */
/*============================================================================*/

static void ResetInputState(void)
{
    memset(s_pressed_keys, 0, sizeof(s_pressed_keys));
    memset(s_keycode_slots, 0, sizeof(s_keycode_slots));
    memset(s_keycode_refcount, 0, sizeof(s_keycode_refcount));
    memset(s_modifier_refcount, 0, sizeof(s_modifier_refcount));
    memset(s_mouse_button_refcount, 0, sizeof(s_mouse_button_refcount));
    memset(s_active_actions, 0, sizeof(s_active_actions));
    memset(s_active_action_valid, 0, sizeof(s_active_action_valid));
    memset(s_momentary_layer_active, 0, sizeof(s_momentary_layer_active));
    memset(s_momentary_restore_layer, 0, sizeof(s_momentary_restore_layer));
    ResetKeyFsm();
    s_pressed_count = 0;
    s_current_modifier = 0;
    s_current_mouse_buttons = 0;
    s_keyboard_report_dirty = 1;
}

static void RebuildKeyboardReport(void)
{
    s_pressed_count = 0;
    memset(s_pressed_keys, 0, sizeof(s_pressed_keys));

    for (uint8_t i = 0; i < KBD_MAX_KEYS; i++)
    {
        if (s_keycode_refcount[i] == 0 || s_keycode_slots[i] == 0)
        {
            continue;
        }
        if (s_pressed_count >= sizeof(s_pressed_keys))
        {
            break;
        }
        s_pressed_keys[s_pressed_count++] = s_keycode_slots[i];
    }

    s_keyboard_report_dirty = 1;
}

static bool TrySyncKeyboardReport(void)
{
    int ret;

    if (!s_keyboard_report_dirty)
    {
        return true;
    }
    if (!KBD_Mode_IsInputReady())
    {
        return false;
    }

    if (s_pressed_count > 0 || s_current_modifier != 0)
    {
        ret = KBD_Mode_SendKeyboardReport(s_current_modifier,
                                          s_pressed_keys,
                                          s_pressed_count);
    }
    else
    {
        ret = KBD_Mode_ReleaseAllKeys();
    }

    if (ret == 0)
    {
        s_keyboard_report_dirty = 0;
        return true;
    }
    return false;
}

static void DiscardQueuedInput(void)
{
    key_event_t key_evt;
    fnkey_event_t fn_evt;

    while (Key_GetEvent(&key_evt)) {}
    while (Encoder_GetEvent(&key_evt)) {}
    while (FnKey_GetEvent(&fn_evt)) {}
}

static void UpdateKeycodeRefcount(uint8_t keycode, bool pressed)
{
    if (keycode == 0)
    {
        return;
    }

    for (uint8_t i = 0; i < KBD_MAX_KEYS; i++)
    {
        if (s_keycode_slots[i] != keycode)
        {
            continue;
        }

        if (pressed)
        {
            if (s_keycode_refcount[i] < 0xFF)
            {
                s_keycode_refcount[i]++;
            }
            return;
        }

        if (s_keycode_refcount[i] > 0)
        {
            s_keycode_refcount[i]--;
            if (s_keycode_refcount[i] == 0)
            {
                s_keycode_slots[i] = 0;
            }
        }
        return;
    }

    if (!pressed)
    {
        return;
    }

    for (uint8_t i = 0; i < KBD_MAX_KEYS; i++)
    {
        if (s_keycode_refcount[i] != 0)
        {
            continue;
        }
        s_keycode_slots[i] = keycode;
        s_keycode_refcount[i] = 1;
        return;
    }
}

static void UpdateModifierMask(uint8_t mask, bool pressed)
{
    for (uint8_t i = 0; i < sizeof(s_modifier_bits); i++)
    {
        uint8_t bit = s_modifier_bits[i];
        if ((mask & bit) == 0)
        {
            continue;
        }

        if (pressed)
        {
            if (s_modifier_refcount[i] < 0xFF)
            {
                s_modifier_refcount[i]++;
            }
            s_current_modifier |= bit;
        }
        else if (s_modifier_refcount[i] > 0)
        {
            s_modifier_refcount[i]--;
            if (s_modifier_refcount[i] == 0)
            {
                s_current_modifier &= (uint8_t)~bit;
            }
        }
    }
}

static void UpdateMouseButtons(uint8_t buttons, bool pressed)
{
    for (uint8_t i = 0; i < sizeof(s_mouse_button_bits); i++)
    {
        uint8_t bit = s_mouse_button_bits[i];
        if ((buttons & bit) == 0)
        {
            continue;
        }

        if (pressed)
        {
            if (s_mouse_button_refcount[i] < 0xFF)
            {
                s_mouse_button_refcount[i]++;
            }
            s_current_mouse_buttons |= bit;
        }
        else if (s_mouse_button_refcount[i] > 0)
        {
            s_mouse_button_refcount[i]--;
            if (s_mouse_button_refcount[i] == 0)
            {
                s_current_mouse_buttons &= (uint8_t)~bit;
            }
        }
    }
}

/*============================================================================*/
/* 单击 / 双击 / 长按 判定实现                                                */
/*============================================================================*/

/**
 * @brief 清空所有按键的判定状态
 */
static void ResetKeyFsm(void)
{
    memset(s_key_fsm, 0, sizeof(s_key_fsm));
    s_last_tick_ms = 0;
}

/**
 * @brief 判断指定键位是否参与双击 / 长按判定
 */
static bool GestureEnabledForKey(uint8_t key_index)
{
#if defined(KBD_LAYOUT_5KEY)
    /* 五键款：全部物理按键支持双击 / 长按 */
    return key_index < KBD_TOTAL_KEYS;
#else
    /* 旋钮款保持原有行为，旋钮虚拟键位不参与手势判定 */
    (void)key_index;
    return false;
#endif
}

/**
 * @brief 判断指定键位的某个动作槽是否已配置
 */
static bool IsSlotConfigured(uint8_t key_index, uint8_t slot)
{
    const kbd_action_t *action = KBD_GetKeyActionSlot(key_index, slot);
    return (action != NULL && action->type != KBD_ACTION_NONE);
}

/**
 * @brief 未启用手势的键位：沿用按下即触发、松开即释放的原始逻辑
 */
static void HandlePlainKey(uint8_t key_index, bool pressed)
{
    const kbd_action_t *action = NULL;

    if (pressed)
    {
        s_active_action_valid[key_index] = 0;
        memset(&s_active_actions[key_index], 0, sizeof(s_active_actions[key_index]));
        action = KBD_GetKeyAction(key_index);
        if (action != NULL)
        {
            s_active_actions[key_index] = *action;
            s_active_action_valid[key_index] = 1;
            action = &s_active_actions[key_index];
        }
    }
    else if (s_active_action_valid[key_index])
    {
        action = &s_active_actions[key_index];
    }
    else
    {
        action = KBD_GetKeyAction(key_index);
    }

    if (action == NULL)
        return;

    LOG_D(TAG, "key %d %s", key_index, pressed ? "press" : "release");
    KBD_Log_KeyEvent(key_index, pressed ? 1 : 0, action->type, action->param1);
    ExecuteKeyAction(key_index, action, pressed);

    if (!pressed)
    {
        s_active_action_valid[key_index] = 0;
        memset(&s_active_actions[key_index], 0, sizeof(s_active_actions[key_index]));
    }
}

/**
 * @brief 丢弃指定键位待定的双击 / 长按判定（不补发任何动作）
 */
static void AbortGesture(uint8_t key_index)
{
    s_key_fsm[key_index].state = KSTATE_IDLE;
    s_key_fsm[key_index].deadline_ms = 0;
}

/**
 * @brief 执行某个动作槽的按下，并缓存该动作供释放时使用
 */
static void StartSlotAction(uint8_t key_index, uint8_t slot)
{
    const kbd_action_t *action = KBD_GetKeyActionSlot(key_index, slot);
    if (action == NULL || action->type == KBD_ACTION_NONE)
    {
        s_key_fsm[key_index].state = KSTATE_IDLE;
        return;
    }

    s_active_actions[key_index] = *action;
    s_active_action_valid[key_index] = 1;

    s_key_fsm[key_index].state = KSTATE_ACTIVE;
    s_key_fsm[key_index].slot = slot;
    s_key_fsm[key_index].deadline_ms = 0;

    LOG_D(TAG, "key %d slot %d down", key_index, slot);
    KBD_Log_KeyEvent(key_index, 1, action->type, action->param1);
    ExecuteKeyAction(key_index, &s_active_actions[key_index], true);
}

/**
 * @brief 释放当前生效的动作，回到空闲状态
 */
static void FinishActiveAction(uint8_t key_index)
{
    if (s_key_fsm[key_index].state != KSTATE_ACTIVE)
    {
        return;
    }

    const kbd_action_t *action = NULL;
    if (s_active_action_valid[key_index])
    {
        action = &s_active_actions[key_index];
    }
    else
    {
        action = KBD_GetKeyActionSlot(key_index, s_key_fsm[key_index].slot);
    }

    s_key_fsm[key_index].state = KSTATE_IDLE;
    s_key_fsm[key_index].slot = KBD_SLOT_CLICK;

    if (action != NULL)
    {
        LOG_D(TAG, "key %d up", key_index);
        KBD_Log_KeyEvent(key_index, 0, action->type, action->param1);
        ExecuteKeyAction(key_index, action, false);
    }

    s_active_action_valid[key_index] = 0;
    memset(&s_active_actions[key_index], 0, sizeof(s_active_actions[key_index]));
}

/**
 * @brief 立即完成一次点按（按下后马上释放）
 *
 * 用于双击窗口超时后的单击补发。
 */
static void FireTapAction(uint8_t key_index, uint8_t slot)
{
    StartSlotAction(key_index, slot);
    FinishActiveAction(key_index);
}

/**
 * @brief 处理按下事件
 */
static void GesturePress(uint8_t key_index, uint32_t now_ms)
{
    kbd_key_fsm_t *fsm = &s_key_fsm[key_index];

    /* 双击窗口内的第二次按下 */
    if (fsm->state == KSTATE_DOUBLE_WAIT &&
        (int32_t)(now_ms - fsm->deadline_ms) <= 0)
    {
        if (IsSlotConfigured(key_index, KBD_SLOT_DOUBLE))
        {
            StartSlotAction(key_index, KBD_SLOT_DOUBLE);
            return;
        }
        fsm->state = KSTATE_IDLE;
    }

    /* 缺少释放事件的异常状态：先收尾再开始新一轮 */
    if (fsm->state == KSTATE_ACTIVE)
    {
        FinishActiveAction(key_index);
    }

    if (IsSlotConfigured(key_index, KBD_SLOT_LONG))
    {
        /* 等待长按阈值，达到阈值前不触发任何动作 */
        fsm->state = KSTATE_PRESS_HELD;
        fsm->deadline_ms = now_ms + KBD_GetLongPressMs();
        return;
    }

    if (IsSlotConfigured(key_index, KBD_SLOT_DOUBLE))
    {
        /* 需要区分单击与双击，按下期间不动作，松开后进入双击窗口 */
        fsm->state = KSTATE_PRESS_HELD;
        fsm->deadline_ms = 0u;
        return;
    }

    /* 未配置长按与双击：保持零延迟的原始手感 */
    StartSlotAction(key_index, KBD_SLOT_CLICK);
}

/**
 * @brief 处理松开事件
 */
static void GestureRelease(uint8_t key_index, uint32_t now_ms)
{
    kbd_key_fsm_t *fsm = &s_key_fsm[key_index];

    switch (fsm->state)
    {
    case KSTATE_ACTIVE:
        FinishActiveAction(key_index);
        break;

    case KSTATE_PRESS_HELD:
        if (IsSlotConfigured(key_index, KBD_SLOT_DOUBLE))
        {
            fsm->state = KSTATE_DOUBLE_WAIT;
            fsm->deadline_ms = now_ms + KBD_GetDoubleClickMs();
        }
        else
        {
            /* 因等待长按而未在按下时触发，此处补发单击 */
            FireTapAction(key_index, KBD_SLOT_CLICK);
        }
        break;

    case KSTATE_DOUBLE_WAIT:
        /* 第二次按下已由 GesturePress 处理，此处不额外动作 */
        break;

    default:
        break;
    }
}

/**
 * @brief 推进判定定时器（长按阈值、双击窗口）
 */
static void KbdCore_TickKeys(uint32_t now_ms)
{
    if (now_ms < s_last_tick_ms)
    {
        /* 驱动 tick 被复位（退出低功耗）：丢弃全部待定判定并释放按住的键 */
        for (uint8_t i = 0; i < KBD_MAX_KEYS; i++)
        {
            if (s_key_fsm[i].state == KSTATE_ACTIVE)
            {
                FinishActiveAction(i);
            }
            s_key_fsm[i].state = KSTATE_IDLE;
            s_key_fsm[i].deadline_ms = 0;
        }
        s_last_tick_ms = now_ms;
        return;
    }
    s_last_tick_ms = now_ms;

    for (uint8_t i = 0; i < KBD_MAX_KEYS; i++)
    {
        kbd_key_fsm_t *fsm = &s_key_fsm[i];

        if (fsm->state == KSTATE_PRESS_HELD && fsm->deadline_ms != 0u &&
            (int32_t)(now_ms - fsm->deadline_ms) >= 0)
        {
            StartSlotAction(i, KBD_SLOT_LONG);
        }
        else if (fsm->state == KSTATE_DOUBLE_WAIT &&
                 (int32_t)(now_ms - fsm->deadline_ms) >= 0)
        {
            /* 双击窗口超时：确认为单击 */
            FireTapAction(i, KBD_SLOT_CLICK);
        }
    }
}

static void SwitchLayer(uint8_t target_layer)
{
    uint8_t old_layer = KBD_GetCurrentLayer();

    /* 切换层会改变键位含义，丢弃尚未确认的双击 / 长按判定 */
    for (uint8_t i = 0; i < KBD_MAX_KEYS; i++)
    {
        if (s_key_fsm[i].state == KSTATE_PRESS_HELD ||
            s_key_fsm[i].state == KSTATE_DOUBLE_WAIT)
        {
            s_key_fsm[i].state = KSTATE_IDLE;
            s_key_fsm[i].deadline_ms = 0;
        }
    }

    if (target_layer == old_layer)
    {
        KBD_RGB_FlashLayer(target_layer);
        return;
    }

    if (KBD_SetCurrentLayer(target_layer) != 0)
    {
        return;
    }

    LOG_I(TAG, "Layer -> %d", target_layer);
    KBD_Log_LayerEvent(old_layer, target_layer);

    if (KBD_Macro_IsRunning())
    {
        KBD_Macro_Cancel();
    }

    KBD_RGB_FlashLayer(target_layer);
}

/**
 * @brief 执行按键动作
 */
static void ExecuteKeyAction(uint8_t key_index, const kbd_action_t *action, bool pressed)
{
    if (action == NULL || action->type == KBD_ACTION_NONE)
        return;

    switch (action->type)
    {
    case KBD_ACTION_KEYBOARD:
        if (pressed)
        {
            UpdateKeycodeRefcount(action->param1, true);
            UpdateModifierMask(action->modifier, true);
        }
        else
        {
            UpdateKeycodeRefcount(action->param1, false);
            UpdateModifierMask(action->modifier, false);
        }
        RebuildKeyboardReport();
        break;

    case KBD_ACTION_MOUSE_BTN:
        UpdateMouseButtons(action->param1, pressed);
        KBD_Mode_SendMouseReport(s_current_mouse_buttons, 0, 0, 0);
        break;

    case KBD_ACTION_MOUSE_WHEEL:
        if (pressed)
        {
            int8_t wheel = 0;
            switch (action->param1)
            {
            case KBD_WHEEL_UP:
                wheel = 1;
                break;
            case KBD_WHEEL_DOWN:
                wheel = -1;
                break;
            case KBD_WHEEL_CLICK:
                KBD_Mode_SendMouseReport((uint8_t)(s_current_mouse_buttons | KBD_MOUSE_MIDDLE), 0, 0, 0);
                mDelaymS(50);
                KBD_Mode_SendMouseReport(s_current_mouse_buttons, 0, 0, 0);
                return;
            }
            if (wheel != 0)
            {
                KBD_Mode_SendMouseReport(s_current_mouse_buttons, 0, 0, wheel);
            }
        }
        break;

    case KBD_ACTION_CONSUMER:
    {
        uint16_t consumer_code = action->param1 | (action->param2 << 8);
        if (pressed)
        {
            KBD_Mode_SendConsumerReport(consumer_code);
        }
        else
        {
            KBD_Mode_SendConsumerReport(0);
        }
    }
    break;

    case KBD_ACTION_LAYER:
        if (pressed)
        {
            switch ((kbd_layer_op_t)action->modifier)
            {
            case KBD_LAYER_MOMENTARY:
                s_momentary_layer_active[key_index] = 1;
                s_momentary_restore_layer[key_index] = KBD_GetCurrentLayer();
                SwitchLayer(action->param1);
                break;

            case KBD_LAYER_TOGGLE:
            {
                uint8_t current_layer = KBD_GetCurrentLayer();
                uint8_t fallback_layer = KBD_GetKeymap()->default_layer;
                uint8_t target_layer =
                    (current_layer == action->param1) ? fallback_layer : action->param1;
                SwitchLayer(target_layer);
                break;
            }

            case KBD_LAYER_SET:
            default:
                SwitchLayer(action->param1);
                break;
            }
        }
        else if ((kbd_layer_op_t)action->modifier == KBD_LAYER_MOMENTARY &&
                 s_momentary_layer_active[key_index])
        {
            s_momentary_layer_active[key_index] = 0;
            SwitchLayer(s_momentary_restore_layer[key_index]);
        }
        break;

    case KBD_ACTION_MACRO:
        if (pressed)
        {
            kbd_macro_trigger_t trig = (kbd_macro_trigger_t)action->modifier;
            if (trig == KBD_MACRO_TRIG_TOGGLE && KBD_Macro_IsRunning())
            {
                KBD_Macro_Cancel(); /* Toggle 模式: 再按 -> 停 */
            }
            else
            {
                int ret = KBD_Macro_Execute(action->param1, trig);
                if (ret != 0)
                {
                    LOG_W(TAG, "Macro %d exec fail: %d", action->param1, ret);
                }
            }
        }
        else
        {
            /* 松开事件通知宏引擎 */
            KBD_Macro_OnKeyRelease();
        }
        break;

    default:
        break;
    }
}

/**
 * @brief 执行 FN 动作
 */
static void ExecuteFnAction(kbd_fn_action_t action, uint8_t param)
{
    int ret;

    switch (action)
    {
    case KBD_FN_NONE:
        break;

    /* 模式控制 */
    case KBD_FN_MODE_TOGGLE:
        LOG_I(TAG, "FN: mode toggle");
        ret = KBD_Mode_Toggle();
        if (ret != 0)
        {
            LOG_W(TAG, "mode toggle failed: %d", ret);
            KBD_RGB_Flash(255, 0, 0, 200);
        }
        break;

    case KBD_FN_BLE_ADV:
        if (KBD_Mode_Get() == KBD_WORK_MODE_BLE && !KBD_Mode_IsConnected())
        {
            LOG_I(TAG, "FN: start adv");
            ret = KBD_Mode_BLE_StartAdvertising();
            if (ret != 0)
            {
                LOG_W(TAG, "start adv failed: %d", ret);
                KBD_RGB_Flash(255, 0, 0, 200);
            }
        }
        break;

    case KBD_FN_BLE_DISCONNECT:
        LOG_I(TAG, "FN: disconnect");
        ret = KBD_Mode_BLE_Disconnect();
        if (ret != 0)
        {
            LOG_W(TAG, "disconnect failed: %d", ret);
        }
        break;

    case KBD_FN_BLE_CLEAR_BONDS:
        if (KBD_Mode_Get() != KBD_WORK_MODE_BLE)
        {
            /* 非 BLE 模式：按用户要求静默忽略 */
            break;
        }
        LOG_I(TAG, "FN: clear bonds");
        ret = KBD_Mode_BLE_ClearBonds();
        if (ret == 0)
        {
            KBD_RGB_Flash(0, 120, 255, 300);
        }
        else
        {
            LOG_W(TAG, "clear bonds failed: %d", ret);
            KBD_RGB_Flash(255, 0, 0, 300);
        }
        break;

    /* RGB 控制 */
    case KBD_FN_RGB_TOGGLE:
        KBD_RGB_Toggle();
        KBD_Config_Save();
        break;

    case KBD_FN_RGB_MODE_NEXT:
        KBD_RGB_NextMode();
        KBD_Config_Save();
        break;

    case KBD_FN_RGB_MODE_PREV:
        KBD_RGB_PrevMode();
        KBD_Config_Save();
        break;

    case KBD_FN_RGB_BRIGHT_UP:
        KBD_RGB_BrightnessUp(16);
        KBD_Config_Save();
        break;

    case KBD_FN_RGB_BRIGHT_DOWN:
        KBD_RGB_BrightnessDown(16);
        KBD_Config_Save();
        break;

    /* 层控制 */
    case KBD_FN_LAYER_NEXT:
    {
        if (KBD_Macro_IsRunning())
            KBD_Macro_Cancel();
        uint8_t layer = KBD_NextLayer();
        LOG_I(TAG, "FN: layer next -> %d", layer);
        KBD_RGB_FlashLayer(layer);
    }
    break;

    case KBD_FN_LAYER_PREV:
    {
        if (KBD_Macro_IsRunning())
            KBD_Macro_Cancel();
        uint8_t layer = KBD_PrevLayer();
        LOG_I(TAG, "FN: layer prev -> %d", layer);
        KBD_RGB_FlashLayer(layer);
    }
    break;

    case KBD_FN_LAYER_SET:
        if (KBD_Macro_IsRunning())
            KBD_Macro_Cancel();
        LOG_I(TAG, "FN: layer set %d", param);
        KBD_SetCurrentLayer(param);
        KBD_RGB_FlashLayer(param);
        break;

    /* 系统 */
    case KBD_FN_SLEEP:
        LOG_I(TAG, "FN: sleep");
        KBD_Mode_EnterSleep();
        break;

    case KBD_FN_BOOTLOADER:
        LOG_W(TAG, "FN: enter IAP");
        Hal_JumpToBootloader();
        break;

    /* 宏 */
    case KBD_FN_MACRO:
        LOG_I(TAG, "FN: macro %d", param);
        KBD_Macro_Execute(param, KBD_MACRO_TRIG_ONCE);
        break;

    default:
        break;
    }
}
