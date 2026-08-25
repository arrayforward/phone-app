#pragma once
// Win32 虚拟键码 → android.view.KeyEvent 键码映射（见 input_map.cpp 表头注释）。

// 返回 Android 键码；未映射（含可打印字符，走 WM_CHAR/TEXT）返回 -1。
int vk_to_android_keycode(unsigned vk);
