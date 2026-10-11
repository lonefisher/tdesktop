#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Core::FishGramUpdates {

[[nodiscard]] inline std::optional<std::uint64_t> ParseVersion(
		std::string_view text) {
	if (text.empty() || text.front() == '0') {
		return std::nullopt;
	}
	for (const auto ch : text) {
		if (ch < '0' || ch > '9') {
			return std::nullopt;
		}
	}
	std::uint64_t value = 0;
	const auto parsed = std::from_chars(
		text.data(), text.data() + text.size(), value);
	return (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size())
		? std::optional<std::uint64_t>(value)
		: std::nullopt;
}

[[nodiscard]] inline constexpr std::uint64_t MakeVersion(
		std::uint32_t base,
		std::uint32_t revision) {
	return (std::uint64_t(base) << 32) | revision;
}

[[nodiscard]] inline constexpr bool IsNewer(
		std::uint32_t base,
		std::uint32_t revision,
		std::uint64_t running) {
	return base > 0 && revision > 0 && MakeVersion(base, revision) > running;
}

// Program updates are deliberately flat. Account data and metadata are never
// payload files, including when an authenticated archive contains such names.
[[nodiscard]] inline bool IsSafePayloadName(std::string_view name) {
	if (name.empty() || name.size() > 240) {
		return false;
	}
	std::string lower;
	lower.reserve(name.size());
	for (const auto ch : name) {
		const auto byte = static_cast<unsigned char>(ch);
		if (byte < 32 || byte > 126
			|| ch == '/' || ch == '\\' || ch == ':' || ch == '<'
			|| ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
			return false;
		}
		lower.push_back((ch >= 'A' && ch <= 'Z') ? char(ch + ('a' - 'A')) : ch);
	}
	if (lower.back() == ' ' || lower.back() == '.' || lower.front() == '.') {
		return false;
	}
	const auto dot = lower.find('.');
	const auto stem = lower.substr(0, dot);
	if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul"
		|| (stem.size() == 4
			&& (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt")
			&& stem[3] >= '0' && stem[3] <= '9')) {
		return false;
	}
	return lower == "telegram.exe" || lower == "updater.exe"
		|| (lower.size() > 4 && lower.ends_with(".dll"));
}

} // namespace Core::FishGramUpdates
