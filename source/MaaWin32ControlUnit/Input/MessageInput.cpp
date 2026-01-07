#include "MessageInput.h"

#include "MaaUtils/Encoding.h"
#include "MaaUtils/Logger.h"
#include "MaaUtils/Platform.h"
#include "MaaUtils/SafeWindows.hpp"

#include "InputUtils.h"

#include <string>
#include <vector>

MAA_CTRL_UNIT_NS_BEGIN

// Define Pipe Name
constexpr auto LUNA_PIPE_NAME = L"\\\\.\\pipe\\MaaLunaPipe1";

MessageInput::~MessageInput()
{
    if (block_input_) {
        BlockInput(FALSE);
    }
    if (pipe_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }
}

void MessageInput::connect_pipe()
{
    // Try to connect to Luna Pipe
    // We only try once during initialization. If Luna is not running, we degrade to normal behavior.
    pipe_handle_ = CreateFileW(
        LUNA_PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,              // No sharing
        NULL,           // Default security attributes
        OPEN_EXISTING,  // Opens existing pipe
        0,              // Default attributes
        NULL            // No template file
    );

    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        // LogWarning << "Could not connect to Luna Pipe. Background click fix disabled.";
    } else {
        // Change to message-read mode
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe_handle_, &mode, NULL, NULL);
        LogInfo << "Connected to Luna Pipe for background click injection.";
    }
}

void MessageInput::sync_luna_position(int x, int y)
{
    if (pipe_handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    // Protocol: "MOVE x y"
    std::string msg = "MOVE " + std::to_string(x) + " " + std::to_string(y);
    DWORD bytes_written = 0;
    
    BOOL success = WriteFile(
        pipe_handle_,
        msg.c_str(),
        static_cast<DWORD>(msg.length()),
        &bytes_written,
        NULL
    );

    if (!success) {
        // Pipe broken? Try to reconnect or just give up
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
        return;
    }

    // Wait for "OK" response to ensure synchronization
    char buffer[16];
    DWORD bytes_read = 0;
    success = ReadFile(
        pipe_handle_,
        buffer,
        sizeof(buffer) - 1,
        &bytes_read,
        NULL
    );

    if (success && bytes_read > 0) {
        buffer[bytes_read] = '\0';
        // We could check if buffer == "OK", but simply waiting for the read implies Luna processed it.
    }
}

void MessageInput::ensure_foreground()
{
    ::MaaNS::CtrlUnitNs::ensure_foreground(hwnd_);
}

bool MessageInput::send_or_post_w(UINT message, WPARAM wParam, LPARAM lParam)
{
    bool success = false;

    if (mode_ == Mode::PostMessage) {
        success = PostMessageW(hwnd_, message, wParam, lParam) != 0;
    }
    else {
        SendMessageW(hwnd_, message, wParam, lParam);
        success = true; // SendMessage 总是返回，除非窗口句柄无效
    }

    if (!success) {
        DWORD error = GetLastError();
        LogError << "Failed to" << mode_ << VAR(message) << VAR(wParam) << VAR(lParam) << VAR(error);
    }

    return success;
}

POINT MessageInput::client_to_screen(int x, int y)
{
    POINT point = { x, y };
    if (hwnd_) {
        ClientToScreen(hwnd_, &point);
    }
    return point;
}

void MessageInput::save_cursor_pos()
{
    GetCursorPos(&saved_cursor_pos_);
    cursor_pos_saved_ = true;
}

void MessageInput::restore_cursor_pos()
{
    if (cursor_pos_saved_) {
        SetCursorPos(saved_cursor_pos_.x, saved_cursor_pos_.y);
        cursor_pos_saved_ = false;
    }
}

LPARAM MessageInput::prepare_mouse_position(int x, int y)
{
    if (with_cursor_pos_) {
        // Genshin 模式：移动真实光标到目标位置
        POINT screen_pos = client_to_screen(x, y);
        SetCursorPos(screen_pos.x, screen_pos.y);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return MAKELPARAM(x, y);
}

std::pair<int, int> MessageInput::get_target_pos() const
{
    if (last_pos_set_) {
        return last_pos_;
    }

    // 未设置时返回窗口客户区中心
    RECT rect = {};
    if (hwnd_ && GetClientRect(hwnd_, &rect)) {
        return { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
    }
    return { 0, 0 };
}

MaaControllerFeature MessageInput::get_features() const
{
    return MaaControllerFeature_UseMouseDownAndUpInsteadOfClick | MaaControllerFeature_UseKeyboardDownAndUpInsteadOfClick;
}

bool MessageInput::click(int x, int y)
{
    LogError << "deprecated" << VAR(mode_) << VAR(with_cursor_pos_) << VAR(x) << VAR(y);
    return false;
}

bool MessageInput::swipe(int x1, int y1, int x2, int y2, int duration)
{
    LogError << "deprecated" << VAR(mode_) << VAR(with_cursor_pos_) << VAR(x1) << VAR(y1) << VAR(x2) << VAR(y2) << VAR(duration);
    return false;
}

bool MessageInput::touch_down(int contact, int x, int y, int pressure)
{
    LogInfo << VAR(mode_) << VAR(with_cursor_pos_) << VAR(contact) << VAR(x) << VAR(y) << VAR(pressure);

    std::ignore = pressure;

    if (!hwnd_) {
        LogError << "hwnd_ is nullptr";
        return false;
    }

    // [LUNA INTEGRATION] Sync fake coordinates before sending message
    sync_luna_position(x, y);

    MouseMessageInfo move_info;
    if (!contact_to_mouse_move_message(contact, move_info)) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "contact out of range" << VAR(contact);
        return false;
    }

    MouseMessageInfo down_info;
    if (!contact_to_mouse_down_message(contact, down_info)) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "contact out of range" << VAR(contact);
        return false;
    }

    ensure_foreground();

    if (block_input_) {
        BlockInput(TRUE);
    }

    if (with_cursor_pos_) {
        save_cursor_pos();
    }

    // 准备位置（with_cursor_pos_ 模式下会移动光标）并发送 MOVE 消息
    LPARAM lParam = prepare_mouse_position(x, y);

    if (!send_or_post_w(move_info.message, move_info.w_param, lParam)) {
        if (with_cursor_pos_) {
            restore_cursor_pos();
        }
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 发送 DOWN 消息
    if (!send_or_post_w(down_info.message, down_info.w_param, lParam)) {
        if (with_cursor_pos_) {
            restore_cursor_pos();
        }
        return false;
    }

    last_pos_ = { x, y };
    last_pos_set_ = true;

    return true;
}

bool MessageInput::touch_move(int contact, int x, int y, int pressure)
{
    // LogInfo << VAR(contact) << VAR(x) << VAR(y) << VAR(pressure);

    std::ignore = pressure;

    if (!hwnd_) {
        LogError << "hwnd_ is nullptr";
        return false;
    }

    // [LUNA INTEGRATION] Sync fake coordinates for move events too
    sync_luna_position(x, y);

    MouseMessageInfo msg_info;
    if (!contact_to_mouse_move_message(contact, msg_info)) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "contact out of range" << VAR(contact);
        return false;
    }

    // 准备位置（with_cursor_pos_ 模式下会移动光标）并发送 MOVE 消息
    LPARAM lParam = prepare_mouse_position(x, y);

    if (!send_or_post_w(msg_info.message, msg_info.w_param, lParam)) {
        return false;
    }

    last_pos_ = { x, y };
    last_pos_set_ = true;

    return true;
}

bool MessageInput::touch_up(int contact)
{
    LogInfo << VAR(mode_) << VAR(with_cursor_pos_) << VAR(contact);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    OnScopeLeave([this]() {
        if (block_input_) {
            BlockInput(FALSE);
        }
    });

    MouseMessageInfo msg_info;
    if (!contact_to_mouse_up_message(contact, msg_info)) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "contact out of range" << VAR(contact);
        return false;
    }

    auto target_pos = get_target_pos();
    
    // [LUNA INTEGRATION] Usually touch_up happens at the last position.
    // We sync again just to be safe, ensuring the "Release" check passes if the game checks it.
    sync_luna_position(target_pos.first, target_pos.second);

    if (!send_or_post_w(msg_info.message, msg_info.w_param, MAKELPARAM(target_pos.first, target_pos.second))) {
        return false;
    }

    // touch_up 时恢复光标位置（与 touch_down 配对）
    if (with_cursor_pos_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        restore_cursor_pos();
    }

    return true;
}

// ... (Rest of the file: click_key, input_text, key_down, key_up, scroll remain unchanged)
// To save space, I assume the rest of the functions are identical to your provided code.
// If you copy this, please ensure the rest of the file is included.

bool MessageInput::click_key(int key)
{
    LogError << "deprecated" << VAR(mode_) << VAR(with_cursor_pos_) << VAR(key);
    return false;
}

bool MessageInput::input_text(const std::string& text)
{
    LogInfo << VAR(mode_) << VAR(with_cursor_pos_) << VAR(text);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    // 文本输入仅发送 WM_CHAR
    for (const auto ch : to_u16(text)) {
        if (!send_or_post_w(WM_CHAR, static_cast<WPARAM>(ch), 0)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

bool MessageInput::key_down(int key)
{
    LogInfo << VAR(mode_) << VAR(key);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    LPARAM lParam = make_keydown_lparam(key);
    return send_or_post_w(WM_KEYDOWN, static_cast<WPARAM>(key), lParam);
}

bool MessageInput::key_up(int key)
{
    LogInfo << VAR(mode_) << VAR(key);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    LPARAM lParam = make_keyup_lparam(key);
    return send_or_post_w(WM_KEYUP, static_cast<WPARAM>(key), lParam);
}

bool MessageInput::scroll(int dx, int dy)
{
    LogInfo << VAR(mode_) << VAR(with_cursor_pos_) << VAR(dx) << VAR(dy);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    if (block_input_) {
        BlockInput(TRUE);
    }

    OnScopeLeave([this]() {
        if (block_input_) {
            BlockInput(FALSE);
        }
    });

    auto target_pos = get_target_pos();

    if (with_cursor_pos_) {
        // 保存当前光标位置，并移动到上次记录的位置
        save_cursor_pos();
        POINT screen_pos = client_to_screen(target_pos.first, target_pos.second);
        SetCursorPos(screen_pos.x, screen_pos.y);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // WM_MOUSEWHEEL 的 lParam 应为屏幕坐标
    POINT screen_pos = client_to_screen(target_pos.first, target_pos.second);
    LPARAM lParam = MAKELPARAM(screen_pos.x, screen_pos.y);

    if (dy != 0) {
        WPARAM wParam = MAKEWPARAM(0, static_cast<short>(dy));
        if (!send_or_post_w(WM_MOUSEWHEEL, wParam, lParam)) {
            if (with_cursor_pos_) {
                restore_cursor_pos();
            }
            return false;
        }
    }

    if (dx != 0) {
        WPARAM wParam = MAKEWPARAM(0, static_cast<short>(dx));
        if (!send_or_post_w(WM_MOUSEHWHEEL, wParam, lParam)) {
            if (with_cursor_pos_) {
                restore_cursor_pos();
            }
            return false;
        }
    }

    if (with_cursor_pos_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // 恢复光标位置
        restore_cursor_pos();
    }

    return true;
}

MAA_CTRL_UNIT_NS_END