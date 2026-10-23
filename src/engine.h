#pragma once
// 체스축구 규칙 엔진 C++ 이식 (원본 = core/*.gd + ai/action_space·eval·minimax_bot.gd).
// 유일한 규칙 출처는 docs/spec.md와 GDScript 원본이다 — 이 파일은 속도용 거울이며,
// tests/test_native_parity.gd가 "같은 상태 + 같은 액션 = 같은 결과·같은 열거 순서"를 보증한다.
// 결정론 계약: GDScript Dictionary의 삽입 순서(BFS 발견 순서)와 열거 순서를 그대로 재현한다.
#include <cstdlib>
#include <vector>

namespace cf {

constexpr int W = 10, H = 7, NT = W * H;          // board.gd
constexpr int PLAYERS = 10, PER_TEAM = 5;   // v5.68: 5대5 (game_state.gd PLAYERS_PER_TEAM)
constexpr int HALF_TURNS = 60, MAX_TURNS = 120, POINTS_PER_TURN = 2; // game_state.gd (v5.44 — 반마다 팀당 30턴)
constexpr int BUF_HEAD = 21, // v5.74: free_used 칸 제거
	 BUF_LEN = BUF_HEAD + PLAYERS * 6; // v5.57: 선수당 6칸 (마커가 고른 잡기 대상) // v5.38 점수·전후반·재배치·교체 차례 · v5.39 골 턴 · v5.40 콤보 · v5.45 버티기
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
enum Phase { PLACEMENT = 0, PLAY = 1, GAME_OVER = 2, SUB = 3 }; // SUB = 하프타임 교체 (v5.38)
// 크리에이터(9)는 풀에서 빠졌고 7은 폐지된 부스터 자리다 — 번호는 구멍으로 남긴다 (기보·저장 호환).
// v5.38: 퀵슈터(11) · v5.40: 타워(12 — 캡틴 자리를 이었다)
enum Pos { PROSPECT = 0, CANNON = 1, MAESTRO = 2, TRICKSTER = 3, TITAN = 4, MARKER = 5,
	HUNTER = 6, BOOSTER = 7, WALL = 8, CREATOR = 9, SPRINTER = 10, QUICK = 11, LAUNCHER = 12 };   // v5.68: 12 = 런처 (옛 타워 자리)
enum Stat { SHT = 0, PAS, DRB, INT_, TKL, PHY };
// 액션 종류 (GDScript 문자열과 1:1 — minimax_bot.gd ACT_NAME과 같은 순서가 인코딩 계약이다).
// 자리 3 = 배치 (v5.9). v5.38: 자리 6(골키퍼킥 폐지)을 **하프타임 교체**가 물려받고 8번에 **해제**가 붙었다. v5.59: 8번 해제 폐지 → **마커 수동 잡기**(A_MARK, dest.x = 대상 uid).
// 교체는 uid −1 = 안 바꿈, 아니면 dest.x = 벤치 슬롯(DRAFT_POOL 인덱스)
enum Act { A_MOVE = 0, A_DRIBBLE, A_TACKLE, A_PLACE, A_PASS, A_SHOT, A_SUB, A_STANCE, A_HOLD, A_PUSH, A_END };
constexpr int N_ACT = 11;   // v5.68: 8 = 버티기(캐논 1포인트, 옛 잡기 자리) · 9 = 제자리 밀기 · 10 = 턴 종료. 잡기 폐지

inline bool in_bounds(V2 t) { return t.x >= 0 && t.x < W && t.y >= 0 && t.y < H; }
inline int idx(V2 t) { return t.y * W + t.x; } // in_bounds 전제
inline int opponent(int team) { return team == HOME ? AWAY : HOME; }
inline V2 goal_of(int team) { return team == HOME ? V2{ 0, 3 } : V2{ 9, 3 }; }
inline V2 opponent_goal(int team) { return goal_of(opponent(team)); }
inline bool is_goal(V2 t) { return t == goal_of(HOME) || t == goal_of(AWAY); }
inline int forward_dir(int team) { return team == HOME ? 1 : -1; }
inline bool is_box_of(int team, V2 t) {
	if (!in_bounds(t) || t.y < 2 || t.y > 4)
		return false;
	return team == HOME ? t.x < 2 : t.x >= W - 2;
}
inline bool is_any_box(V2 t) { return is_box_of(HOME, t) || is_box_of(AWAY, t); }
inline int end_line_x(int team) { return team == HOME ? 0 : W - 1; }
// 자기 진영 = 판 절반 (spec 3.2 v5.12 — 배치 자유화). HOME x 0~4 / AWAY x 5~9
inline bool is_own_half(int team, V2 t) {
	if (!in_bounds(t))
		return false;
	return team == HOME ? t.x < W / 2 : t.x >= W / 2;
}
inline int manhattan(V2 a, V2 b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y); }

struct Player {
	V2 pos = UNPLACED;
	int placed_order = -1;
	int position = PROSPECT;
	int stance = 0;
	int uid = -1;
	int team = HOME;
	int growth[6] = { 0, 0, 0, 0, 0, 0 }; // 컵 성장분 (player_state.gd growth) — 경기 중 불변이라 버퍼 밖 (set_growth로 넣는다)

	bool is_placed() const { return placed_order >= 0; }
	int stat(int key) const; // 기본 3, 포지션 보너스 4 (player_state.gd)
	int move_range() const { return position == SPRINTER ? 4 : 3; } // v5.15 스프린터만 4칸
	int dribble_range() const { return move_range(); } // v5.73: 드리블 = 이동 거리, 치달 없음 (player_state.gd dribble_range)
	int pass_range() const { return position == LAUNCHER ? 5 : 4; }   // v5.68: 런처 5 (2포인트는 무제한). 마에스트로는 4, 포인트로 스탯만
	int shot_range() const { return position == CANNON ? 5 : 4; }  // v5.38: 캐논 5
	int shot_cost() const { return position == QUICK ? 1 : 2; }    // v5.38: 슛 포인트 2, 퀵슈터 1
	int tackle_range() const { return 1; }   // v5.68: 헌터 2칸은 2포인트 특기 (legal_tackle_target)
	bool wide_zone() const { return stance == 1 || position == WALL; }
	bool space_pass() const { return true; }  // v5.30: 빈 칸 패스는 전원 것이 됐다 (5.4)
};

// game_state.gd — 경기 중 가변 상태(save_state 버퍼 대상) + 불변 설정(opener·교체 여부·결장)
struct State {
	int phase = PLACEMENT;
	int first_team = HOME;  // 지금 킥오프에서 공 없이 시작하는 팀 (골·후반마다 바뀐다)
	int placement_index = 0;
	int turn_team = -1;
	int ball_owner_uid = -1;
	V2 ball_pos = UNPLACED; // 루즈볼 (spec 5.4 v5.15) — 주인 없이 판에 놓인 공
	int loose_team = -1;    // 루즈볼을 놓은 팀 (그동안 계속 공격 측)
	int pts = 0;
	int turn_actions = 0;
	int acted_mask = 0;
	int turn_count = 0;
	int winner = -1;        // GAME_OVER에서 −1 = 무승부 (v5.38)
	int score[2] = { 0, 0 };
	int half = 1;
	int redeploy = 0;       // 지금 배치가 골·하프타임 뒤 재배치인가 (포지션 그대로)
	int sub_step = 0;       // 하프타임 교체 차례 (0 = 후반 수비 팀, 1 = 상대)
	int goal_turn[2] = { 0, 0 }; // 골을 넣은 턴 — 그 반의 몇 번째 턴 (1~30, 0 = 없음). 1:1 판정 (v5.39)
	int hold = 0;           // 타워 버티기 (v5.45 — game_state.gd hold): 0 가능 · 1 버티는 중 · 2 버틴 뒤 −1 · 3 먼저 드리블함
	// 설정 (버퍼 제외 — 경기 중 불변. set_config로 넣는다)
	int opener = HOME;      // 전반 선공
	int subs_enabled = 0;   // v5.68: 하프타임 교체 없음 (10종이 5:5로 다 나간다)
	int absent_mask = 0;    // 결장 선수 (컵 부상 — uid 비트)
	Player players[PLAYERS];

	bool has_acted(int uid) const { return (acted_mask >> uid) & 1; }
	bool is_absent(int uid) const { return (absent_mask >> uid) & 1; }
	void mark_acted(int uid) {
		acted_mask |= 1 << uid;
		players[uid].stance = 0;
	}
	void set_ball_owner(int uid) {
		if (uid != ball_owner_uid) // v5.68: 버티기는 캐논의 1포인트 행동 — 공이 바뀌면 면역 끝 (game_state.gd set_ball_owner)
			hold = 0;
		ball_owner_uid = uid;
		ball_pos = UNPLACED; // 주인이 생기면 루즈볼은 사라진다 (v5.15)
		loose_team = -1;
		if (uid != -1)
			players[uid].stance = 0;
	}
	void drop_ball(V2 tile, int team) { // 공간 패스 (spec 5.4 v5.15)
		ball_owner_uid = -1;
		hold = 0;
		ball_pos = tile;
		loose_team = team;
	}
	void reset_ball() {
		ball_owner_uid = -1;
		ball_pos = UNPLACED;
		loose_team = -1;
		hold = 0;
	}
	bool loose_ball() const { return ball_owner_uid == -1 && ball_pos != UNPLACED; }
	int ball_team() const;
	void end_turn();
	void score_goal(int team);
	void begin_redeploy(int no_ball_team);
	bool clock();
	bool decided();
	int half_turn() const { return turn_count - (half - 1) * HALF_TURNS; }
	void next_sub();
	void save_buf(int *buf) const;   // save_state와 같은 배치 (21 + 5/선수)
	void load_buf(const int *buf);
};

struct Action {
	int t = -1; // Act
	int uid = -1;
	V2 dest{ 0, 0 }; // 태클·밀기는 dest.x = 대상 uid (GDScript 인코딩 그대로)
	int cost = -1;   // v5.68 마에스트로 패스 포인트 0·1·2 (= PAS 3·4·5). 그 밖의 수는 −1 (action_space.gd "cost")
};

// ai/action_space.gd — 열거(순서 고정)와 실행. apply는 결과 ok만 돌려준다 (상태가 곧 결과)
void enumerate_actions(const State &s, std::vector<Action> &out);
bool apply_action(State &s, const Action &a);

// 신경망 입력(nnue.cpp)이 쓰는 규칙 질의
int pool_slot(int position);          // 드래프트 슬롯 (풀에 없는 포지션 = −1)
bool kickoff_turn(const State &s);    // actions.gd is_kickoff_turn

// ai/eval.gd + minimax_bot.gd. 신경망(g_net — nnue.h)이 실려 있으면 evaluate는 그 망을 쓰고,
// 드래프트·배치·교체는 한 수 앞(그 수를 둔 뒤의 평가)만 본다 — 갈래가 수백 개라 깊이 읽을 수 없고, 망이 그 판을 이미 읽는다
void features(const State &s, int team, double f[6]);
double evaluate(const State &s, int team, const double w[6]);
// 미니맥스 선택 (실행하지 않는다). found=false면 합법 수 없음. best_out = 고른 수의 탐색 점수(둔 팀 관점)
Action pick_action(const State &s, int depth, const double w[6], bool &found, double *best_out = nullptr);
constexpr double NN_SCALE = 1000.0;   // 망의 −1…+1 → 끝난 판(±W_GOAL 10000)보다 늘 작게

// ---------- 자가대전 공장 (simulator.gd run_match의 C++판) ----------
// 난수는 **배치·엔트리 셔플에만** 쓴다 (게임 로직 난수 금지 — CLAUDE.md. GDScript 하네스와 같은 예외).
// 시드 고정 = 완전 재현. record_path가 비어 있지 않으면 학습용 기보(아래 형식)를 쓴다.
//   기보 v2 (int32 LE): [매직 0x43465350, 버전 2] 다음 경기마다
//   [opener, winner(−1 무승부), HOME골, AWAY골, n_moves] + n_moves × [상태버퍼 60 + 액션 4(t,uid,dx,dy)]
//   배치(처음·재배치)는 하네스 난수라 기보에 안 넣는다 — 봇이 둔 수(경기·교체)만 남긴다
struct SelfplayStats {
	int matches = 0, draws = 0, home_wins = 0, away_wins = 0, first_wins = 0, second_wins = 0;
	int goals = 0;
	long long actions[N_ACT] = {};
	std::vector<int> turns;
};
void selfplay(int games, int depth, unsigned long long seed, int cap, const double w[6],
		const char *record_path, SelfplayStats &out);

// 배치 단계를 하네스 난수로 채운다 (처음 드래프트·재배치 공용). 배치 단계가 아니면 아무것도 안 한다
void random_placement(State &s, unsigned long long &rng_state);

// 새 경기 1판 준비: 설정 보존 + 10종 중 8종 무작위 드래프트·배치 (하네스 난수)
void new_random_match(State &s, int first, unsigned long long seed);
// 드래프트 전 빈 판 (드래프트·배치도 봇이 둔다 — 신경망 학습·아레나)
void new_open_match(State &s, int first);

} // namespace cf
