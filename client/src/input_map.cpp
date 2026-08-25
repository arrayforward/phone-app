#include "input_map.h"

#include <windows.h>

// VK → android.view.KeyEvent 键码映射表。
// 约定（client/README.md 有同样说明）：
//   - 可打印 ASCII（字母/数字/空格/符号）不在此表，走 WM_CHAR → TEXT(0x03)，
//     避免与 KEY(0x02) 双重注入；
//   - ESC → KEYCODE_BACK（手机返回键），F1 → KEYCODE_HOME（回桌面），
//     F2 → KEYCODE_MENU，F3 → KEYCODE_APP_SWITCH（最近任务），
//     F4 → KEYCODE_POWER（锁屏/电源）；
//   - 其余功能键/控制键按语义一一对应。
struct VkMapEntry {
    unsigned vk;
    int      android_keycode;
};

static const VkMapEntry kVkMap[] = {
    // 方向键
    { VK_UP,     19 },  // KEYCODE_DPAD_UP
    { VK_DOWN,   20 },  // KEYCODE_DPAD_DOWN
    { VK_LEFT,   21 },  // KEYCODE_DPAD_LEFT
    { VK_RIGHT,  22 },  // KEYCODE_DPAD_RIGHT
    // 编辑/导航控制键
    { VK_RETURN,  66 },  // KEYCODE_ENTER
    { VK_BACK,    67 },  // KEYCODE_DEL（退格）
    { VK_TAB,     61 },  // KEYCODE_TAB
    { VK_DELETE, 124 },  // KEYCODE_FORWARD_DEL
    { VK_HOME,   122 },  // KEYCODE_MOVE_HOME
    { VK_END,    123 },  // KEYCODE_MOVE_END
    { VK_PRIOR,   92 },  // KEYCODE_PAGE_UP
    { VK_NEXT,    93 },  // KEYCODE_PAGE_DOWN
    // 修饰键
    { VK_LCONTROL, 113 }, // KEYCODE_CTRL_LEFT
    { VK_RCONTROL, 114 }, // KEYCODE_CTRL_RIGHT
    { VK_LSHIFT,    59 }, // KEYCODE_SHIFT_LEFT
    { VK_RSHIFT,    60 }, // KEYCODE_SHIFT_RIGHT
    { VK_LMENU,     57 }, // KEYCODE_ALT_LEFT
    { VK_RMENU,     58 }, // KEYCODE_ALT_RIGHT
    // 手机系统键约定映射
    { VK_ESCAPE, 4 },   // KEYCODE_BACK
    { VK_F1,     3 },   // KEYCODE_HOME
    { VK_F2,    82 },   // KEYCODE_MENU
    { VK_F3,   187 },   // KEYCODE_APP_SWITCH
    { VK_F4,    26 },   // KEYCODE_POWER
    // F5..F12 → KEYCODE_F5..F12
    { VK_F5,  135 },
    { VK_F6,  136 },
    { VK_F7,  137 },
    { VK_F8,  138 },
    { VK_F9,  139 },
    { VK_F10, 140 },
    { VK_F11, 141 },
    { VK_F12, 142 },
    // 音量/静音
    { VK_VOLUME_UP,   24 },  // KEYCODE_VOLUME_UP
    { VK_VOLUME_DOWN, 25 },  // KEYCODE_VOLUME_DOWN
    { VK_VOLUME_MUTE, 164 }, // KEYCODE_VOLUME_MUTE
};

int vk_to_android_keycode(unsigned vk) {
    for (const auto& e : kVkMap) {
        if (e.vk == vk) return e.android_keycode;
    }
    return -1;
}
