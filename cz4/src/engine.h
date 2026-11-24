#pragma once
// 판 공용 상수·좌표 도우미 — 감독 모드 카드 엔진(card*.cpp)이 쓴다. 규칙은 card.cpp 에 있다.
// (옛 5대5 본편 규칙 엔진 engine.cpp 는 2026-11-24 에 걷어냈고, 이 머리 파일만 남았다)
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace cf {

constexpr int W = 11, H = 8, NT = W * H;          // board.gd — 11-23 사용자 「11×8 ㄱㄱ · 골대 2칸 · 박스 2×4」 (옛 10×7 · 골대 1칸) · 가운데 줄 x 5 는 누구 진영도 아님
constexpr int GOAL_Y0 = H / 2 - 1, GOAL_N = 2;    // 골대 = 끝줄 가운데 두 칸 (y 3·4)
constexpr int PLAYERS = 10, PER_TEAM = 5;   // v5.68: 5대5 (game_state.gd PLAYERS_PER_TEAM)
constexpr int POOL_N = 10;                  // v7.0 드래프트 풀 종류 수 (card_az.h 슬롯)
constexpr int POINTS_PER_TURN = 2;   // 차례마다 포인트
constexpr int LONG_PASS_MIN = 5;                   // ball_actions.gd (v5.12)

struct V2 {
	int x = 0, y = 0;
};
inline bool operator==(V2 a, V2 b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(V2 a, V2 b) { return !(a == b); }
inline V2 operator+(V2 a, V2 b) { return { a.x + b.x, a.y + b.y }; }
inline V2 operator-(V2 a, V2 b) { return { a.x - b.x, a.y - b.y }; }

constexpr V2 UNPLACED{ -1, -1 };
constexpr V2 UNSET{ -99, -99 };
constexpr V2 DIRS[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }; // board.gd 순서 고정

enum Team { HOME = 0, AWAY = 1 };
enum Phase { PLACEMENT = 0, PLAY = 1, GAME_OVER = 2 };
// 크리에이터(9)는 v5.30에 풀에서 빠졌다 — 번호는 구멍으로 남긴다 (기보·저장 호환). 퀵슈터 11 · 런처 12
enum Pos { PROSPECT = 0, CANNON = 1, RECEIVER = 2, TRICKSTER = 3, TITAN = 4, MARKER = 5,
	HUNTER = 6, BOOSTER = 7, WALL = 8, SPRINTER = 10, MAESTRO = 12, NOMAD = 13,
	ALLROUND = 14 };
constexpr int LONGSHOT = BOOSTER;   // 중거리 슈터 (11-22 — 옛 부스터 자리·번호)
constexpr int PASSER = NOMAD;       // 패서 (11-22 — 옛 노마드 자리·번호)   // v7.0 — 번호는 player_state.gd Pos와 같다 (2·9·11 빈 자리) · 14 만능 = 카드 모드 전용(11-17, 드래프트 풀 밖 — 상대 13팀 만능 셋)

inline bool in_bounds(V2 t) { return t.x >= 0 && t.x < W && t.y >= 0 && t.y < H; }
inline int idx(V2 t) { return t.y * W + t.x; } // in_bounds 전제
inline int opponent(int team) { return team == HOME ? AWAY : HOME; }
inline V2 goal_tile(int team, int k) { return V2{ team == HOME ? 0 : W - 1, GOAL_Y0 + k }; }   // 골대 칸 k = 0·1
inline V2 goal_of(int team) { return goal_tile(team, 0); }   // 대표 칸 (수 번호·표시용 — 판정은 두 칸 모두)
inline V2 opponent_goal(int team) { return goal_of(opponent(team)); }
inline bool is_goal_of(int team, V2 t) { return t.x == (team == HOME ? 0 : W - 1) && t.y >= GOAL_Y0 && t.y < GOAL_Y0 + GOAL_N; }
inline bool is_goal(V2 t) { return is_goal_of(HOME, t) || is_goal_of(AWAY, t); }
inline int forward_dir(int team) { return team == HOME ? 1 : -1; }
inline bool is_box_of(int team, V2 t) {
	if (!in_bounds(t) || t.y < GOAL_Y0 - 1 || t.y > GOAL_Y0 + GOAL_N)
		return false;   // 박스 2×4 — 골대 두 칸 + 위아래 한 칸 (y 2~5)
	return team == HOME ? t.x < 2 : t.x >= W - 2;
}
inline bool is_any_box(V2 t) { return is_box_of(HOME, t) || is_box_of(AWAY, t); }
// 자기 진영 = 판 절반 (spec 3.2 v5.12 — 배치 자유화). HOME x 0~4 / AWAY x 5~9
inline bool is_own_half(int team, V2 t) {
	if (!in_bounds(t))
		return false;
	return team == HOME ? t.x < W / 2 : t.x >= W - W / 2;   // 11칸 — 가운데 줄(x 5)은 어느 쪽도 아님
}
inline int manhattan(V2 a, V2 b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y); }
inline int goal_dist(int team, V2 t) { return std::min(manhattan(t, goal_tile(team, 0)), manhattan(t, goal_tile(team, 1))); }   // 그 팀 골대까지 (가까운 칸)

} // namespace cf
