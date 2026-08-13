#pragma once

#include <optional>
#include <string>

namespace RC::GUI::LOTDKConsoleBridge
{
    auto request_console_tab_and_input_focus() -> void;
    auto consume_console_tab_request() -> bool;
    auto consume_input_focus_request() -> bool;
    auto queue_command(std::string command) -> void;
    auto take_pending_command() -> std::optional<std::string>;
} // namespace RC::GUI::LOTDKConsoleBridge
