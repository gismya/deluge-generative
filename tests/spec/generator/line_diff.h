// A shortest line diff (Myers' O(ND) algorithm) for specs that show where two texts part ways: golden.h's diffOf.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace harness {

/// One line of a line diff: kept (' '), only the first text's ('-') or only the second's ('+'), with its index in
/// each text (the first's for '-' and ' ', the second's for '+').
struct DiffLine {
	char op = ' ';
	size_t a = 0;
	size_t b = 0;
};

/// The shortest edit script from `a` to `b` (Myers' O(ND) diff), or nothing if it needs more than `maxEdits` edits.
/// Memory is O(D²) in the edits D it needs, never more than n + m: round d keeps only the d + 1 diagonals it reached.
inline std::optional<std::vector<DiffLine>> lineDiff(const std::vector<std::string>& a,
                                                     const std::vector<std::string>& b, int32_t maxEdits = 2000) {
	const auto n = static_cast<int32_t>(a.size());
	const auto m = static_cast<int32_t>(b.size());
	const int32_t limit = static_cast<int32_t>(std::min<int64_t>(maxEdits, int64_t{n} + m));
	// rounds[d][(k + d) / 2]: the furthest x on diagonal k (x - y) after round d, for k = -d, -d + 2, ..., d.
	std::vector<std::vector<int32_t>> rounds;
	auto reached = [&](int32_t d, int32_t k) { return rounds[d][(k + d) / 2]; };
	// Whether round d reached diagonal k from k + 1 (an insertion, down) rather than from k - 1 (a deletion, right).
	auto cameDown = [&](int32_t d, int32_t k) {
		return k == -d || (k != d && reached(d - 1, k - 1) < reached(d - 1, k + 1));
	};
	for (int32_t d = 0; d <= limit; d++) {
		rounds.emplace_back(static_cast<size_t>(d) + 1);
		for (int32_t k = -d; k <= d; k += 2) {
			int32_t x = d == 0 ? 0 : cameDown(d, k) ? reached(d - 1, k + 1) : reached(d - 1, k - 1) + 1;
			int32_t y = x - k;
			while (x < n && y < m && a[x] == b[y]) {
				x++;
				y++;
			}
			rounds[d][(k + d) / 2] = x;
			if (x < n || y < m) {
				continue;
			}
			// Walk back from the end through each round's choice.
			std::vector<DiffLine> edits;
			for (int32_t e = d; e >= 0; e--) {
				const int32_t kk = x - y;
				const bool wasDown = e > 0 && cameDown(e, kk);
				const int32_t prevK = e == 0 ? 0 : wasDown ? kk + 1 : kk - 1;
				const int32_t prevX = e == 0 ? 0 : reached(e - 1, prevK);
				const int32_t prevY = prevX - prevK;
				while (x > prevX && y > prevY) {
					x--;
					y--;
					edits.push_back({' ', static_cast<size_t>(x), static_cast<size_t>(y)});
				}
				if (e > 0) {
					if (wasDown) {
						edits.push_back({'+', static_cast<size_t>(x), static_cast<size_t>(prevY)});
					}
					else {
						edits.push_back({'-', static_cast<size_t>(prevX), static_cast<size_t>(y)});
					}
					x = prevX;
					y = prevY;
				}
			}
			std::reverse(edits.begin(), edits.end());
			return edits;
		}
	}
	return std::nullopt;
}

} // namespace harness
