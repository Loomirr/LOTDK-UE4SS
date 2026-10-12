#pragma once
// Guard before argument evaluation, including expensive reflected diagnostics.
#define suit_vlog(...) do { if (SuitSettings.VerboseLogging) { \
    Output::send<LogLevel::Verbose>(__VA_ARGS__); } } while (false)
