#pragma once

// M5: stable, versioned text representation of configs, commands and events
// (DESIGN.md §6.3). One record per line, ASCII, single spaces, '\n' line
// ends; '\r', tabs, empty fields, leading zeros and '+' are rejected so that
// every value has exactly one spelling and files compare byte for byte.
//
// Line parsers take the line WITHOUT its '\n' and the 1-based line number
// used in error messages.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/core/command.hpp"
#include "lob/core/config.hpp"
#include "lob/engine/events.hpp"

namespace lob::io {

inline constexpr std::string_view kCommandsMagic = "LOB-COMMANDS v1";
inline constexpr std::string_view kJournalMagic = "LOB-JOURNAL v1";
inline constexpr std::string_view kSnapshotMagic = "LOB-SNAPSHOT v1";

/// Command identity, assigned by the input side; contiguous from 1.
using CmdSeq = std::uint64_t;

struct SequencedCommand {
  CmdSeq cmd_seq{0};
  Command command;

  friend bool operator==(const SequencedCommand&, const SequencedCommand&) = default;
};

/// Malformed input. what() is "line N: <message>".
class ParseError : public std::runtime_error {
 public:
  ParseError(std::size_t line, const std::string& message);
  [[nodiscard]] std::size_t line() const noexcept { return line_; }

 private:
  std::size_t line_;
};

// ---- formatting (no trailing '\n') ----------------------------------------
std::string format_config(const EngineConfig& config);     // CONFIG min_price=…
std::string format_command(const SequencedCommand& command);  // NEW … / CANCEL …
std::string format_event(const Event& event);              // ACCEPTED … etc.

// ---- line parsers ----------------------------------------------------------
EngineConfig parse_config(std::string_view line, std::size_t line_no);
SequencedCommand parse_command(std::string_view line, std::size_t line_no);
Event parse_event(std::string_view line, std::size_t line_no);

/// Strict decimal integers (exposed for the journal and snapshot parsers).
std::uint64_t parse_u64(std::string_view token, std::size_t line_no, std::string_view field);
std::int64_t parse_i64(std::string_view token, std::size_t line_no, std::string_view field);

/// Splits a line into fields separated by exactly one space. Rejects empty
/// fields (double / leading / trailing spaces), '\r', tabs and control bytes.
std::vector<std::string_view> split_fields(std::string_view line, std::size_t line_no);

// ---- command files -----------------------------------------------------------
struct CommandFile {
  EngineConfig config;
  std::vector<SequencedCommand> commands;  // cmd_seq = 1, 2, 3, ...
};

/// Magic line, CONFIG line, then commands. '#' comment lines and empty lines
/// are allowed after the magic line. Every line must end with '\n'. cmd_seq
/// must start at 1 and increase by exactly 1.
CommandFile parse_command_file(std::string_view text);
std::string format_command_file(const EngineConfig& config, std::span<const SequencedCommand> commands);

}  // namespace lob::io
