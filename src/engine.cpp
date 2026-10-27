// 체스축구 규칙 엔진 C++ (원본 GDScript의 1:1 거울 — 주석의 원본 함수명 참조)
// 파일 구성: 상태 → 경합(contest.gd) → 존 → 경로(paths.gd) → 액션(actions/ball_actions.gd)
//           → 열거(action_space.gd) → 평가·탐색(eval/minimax_bot.gd)
#include "engine.h"
#include "nnue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace cf {

// ---------- player_state.gd ----------

int Player::stat(int key) const {
	int base = 1;   // v7.0 기본 1 · 전문가 2 (player_state.gd stat_of — v5.8~v6.6은 3·4)
	switch (position) {
		case CANNON:
			base = key == SHT ? 2 : 1;
			break;
		case MAESTRO:   // v7.0 = 옛 런처
			base = key == PAS ? 2 : 1;
			break;
		case TRICKSTER:
			base = key == DRB ? 2 : 1;
			break;
		case HUNTER:
			base = key == TKL ? 2 : 1;
			break;
	}
	return base + (key >= 0 && key < 6 ? growth[key] : 0);   // 컵 성장분 (player_state.gd stat_of)
}

// ---------- game_state.gd ----------

int State::ball_team() const {
	if (ball_owner_uid != -1)
		return players[ball_owner_uid].team;
	if (loose_ball())
		return loose_team; // 루즈볼은 놓은 팀이 계속 공격 측 (v5.15 — 아직 아무도 뺏지 않았다)
	return opponent(first_team);
}

static int placement_team(const State &s);
static bool place_tile_ok(const State &s, int team, V2 tile, bool nomad);   // v5.68 (아래 move_blocked 옆에 정의)

// 판을 비우고 no_ball_team부터 두 명씩 다시 놓는다 — 포지션은 그대로 (spec 3.5 v5.38, _begin_redeploy)
static void skip_empty_slots(State &s);
void State::begin_redeploy(int no_ball_team) {
	first_team = no_ball_team;
	for (Player &p : players) {
		p.pos = UNPLACED;
		p.placed_order = -1;
		p.stance = 0;
		p.sta = STA_MAX;   // 하프타임 재배치: 체력 전원 2
	}
	reset_ball();
	phase = PLACEMENT;
	redeploy = 1;
	placement_index = 0;
	pts = 0;
	contest_used = 0;
	pay_uid = -1;
	boost_on = 0;
	grab[0] = grab[1] = -1;
	turn_actions = 0;
	acted_mask = 0;
	skip_empty_slots(*this);
	turn_team = placement_team(*this);
}

// 턴 수가 하프타임·종료에 닿았으면 넘기고 true (game_state.gd _clock · _halftime · _full_time)
bool State::clock() {
	if (half == 1 && turn_count >= HALF_TURNS) {
		half = 2;
		begin_redeploy(opponent(opener)); // 후반은 전반 선공이 공을 받는다 — 하프타임은 재배치뿐 (v5.68 교체 폐지)
		return true;
	}
	if (turn_count >= MAX_TURNS) { // 골 많은 팀 → 1:1이면 더 빨리 넣은 팀 → 같으면 무승부 (game_state.gd _full_time)
		phase = GAME_OVER;
		winner = -1;
		if (score[0] != score[1])
			winner = score[0] > score[1] ? HOME : AWAY;
		else if (score[0] > 0 && goal_turn[0] != goal_turn[1])
			winner = goal_turn[0] < goal_turn[1] ? HOME : AWAY;
		return true;
	}
	return false;
}

// 결과가 이미 정해졌으면 끝낸다 (game_state.gd _decided): 전반 t턴 골로 앞선 팀이 있고 후반이 t턴을 지났다
bool State::decided() {
	if (phase != PLAY || half != 2)
		return false;
	for (int t = 0; t < 2; t++)
		if (score[t] == 1 && score[1 - t] == 0 && half_turn() + 1 > goal_turn[t]) {
			phase = GAME_OVER;
			winner = t;
			return true;
		}
	return false;
}

void State::end_turn() {
	if (phase != PLAY)
		return;
	turn_count += 1;
	if (clock() || decided())
		return;
	turn_team = opponent(turn_team);
	pts = POINTS_PER_TURN;
	contest_used = 0;
	pay_uid = -1;
	boost_on = 0;
	turn_actions = 0;
	acted_mask = 0;
	turn_stamina(turn_team);   // v5.86: 태세는 체력이 다할 때까지 (v5.92 체스 모드도 같다)
	if (hold == 1 && ball_owner_uid != -1 && players[ball_owner_uid].team == turn_team)
		hold = 2; // 버티던 캐논의 차례가 돌아왔다 — 면역 끝 (그 공으로는 다시 못 버팀)
}

// 골 (spec 3.5 v5.39): 1점 + 몇 번째 턴인지 적고 **그 반을 끝낸다** (전반 → 하프타임, 후반 → 경기 끝)
void State::score_goal(int team) {
	score[team] += 1;
	goal_turn[team] = half_turn() + 1;
	reset_ball();
	turn_count = half * HALF_TURNS;
	clock();
}

void State::save_buf(int *buf) const { // save_state와 같은 배치
	buf[0] = phase;
	buf[1] = first_team;
	buf[2] = placement_index;
	buf[3] = turn_team;
	buf[4] = ball_owner_uid;
	buf[5] = pts;
	buf[6] = turn_actions;
	buf[7] = turn_count;
	buf[8] = winner;
	buf[9] = acted_mask;
	buf[10] = ball_pos.x; // 루즈볼 (v5.15)
	buf[11] = ball_pos.y;
	buf[12] = loose_team;
	buf[13] = score[0];
	buf[14] = score[1];
	buf[15] = half;
	buf[16] = redeploy;
	buf[17] = boost_on; // v5.83 부스트 (옛 하프타임 교체 차례 자리)
	buf[18] = goal_turn[0];
	buf[19] = goal_turn[1];
	buf[20] = hold;
	buf[21] = contest_used;
	buf[22] = grab[0];
	buf[23] = grab[1];
	int i = BUF_HEAD;
	for (int u = 0; u < PLAYERS; u++) {
		buf[i] = players[u].pos.x;
		buf[i + 1] = players[u].pos.y;
		buf[i + 2] = players[u].placed_order;
		buf[i + 3] = players[u].position;
		buf[i + 4] = players[u].stance;
		buf[i + 5] = players[u].team;   // v5.68: 팀 (옛 mark_pick 자리) — 4대4 시험 상태는 uid 4가 AWAY다
		buf[i + 6] = players[u].sta;    // v5.86 체력
		i += BUF_P;
	}
}

void State::load_buf(const int *buf) {
	phase = buf[0];
	first_team = buf[1];
	placement_index = buf[2];
	turn_team = buf[3];
	ball_owner_uid = buf[4];
	pts = buf[5];
	turn_actions = buf[6];
	turn_count = buf[7];
	winner = buf[8];
	acted_mask = buf[9];
	ball_pos = { buf[10], buf[11] };
	loose_team = buf[12];
	score[0] = buf[13];
	score[1] = buf[14];
	half = buf[15];
	redeploy = buf[16];
	boost_on = buf[17];
	goal_turn[0] = buf[18];
	goal_turn[1] = buf[19];
	hold = buf[20];
	contest_used = buf[21];
	pay_uid = -1;
	grab[0] = buf[22];
	grab[1] = buf[23];
	int i = BUF_HEAD;
	for (int u = 0; u < PLAYERS; u++) {
		players[u].pos = { buf[i], buf[i + 1] };
		players[u].placed_order = buf[i + 2];
		players[u].position = buf[i + 3];
		players[u].stance = buf[i + 4];
		players[u].uid = u;
		players[u].team = buf[i + 5] == AWAY ? AWAY : HOME;   // v5.68: 버퍼의 팀 (game_state.gd save_state)
		players[u].sta = buf[i + 6];
		i += BUF_P;
	}
}

// ---------- 점유 ----------

static void build_occ(const State &s, int occ[NT]) {
	for (int i = 0; i < NT; i++)
		occ[i] = -1;
	for (int u = 0; u < PLAYERS; u++)
		if (s.players[u].is_placed())
			occ[idx(s.players[u].pos)] = u;
}

static const Player *player_at(const State &s, V2 t) {
	for (int u = 0; u < PLAYERS; u++)
		if (s.players[u].is_placed() && s.players[u].pos == t)
			return &s.players[u];
	return nullptr;
}

// ---------- 배치 (spec 3.2) — v5.9부터 봇이 두는 액션이다 ----------

// v5.68 5대5: 두 명씩 번갈아 놓되 팀당 인원을 다 놓으면 남은 차례는 상대 몫 — F F O O F F O O F O (game_state.gd _team_at)

// 드래프트 풀 (spec 3.2 v5.38) — game_state.gd DRAFT_POOL과 같은 순서여야 한다.
// 11종 중 10종이 5:5로 나가 **매 경기 1종이 남는다** (POOL_N은 engine.h)
// ---------- 체력 (v5.86 — game_state.gd _turn_stamina) ----------

// 부스터 옆인가 (game_state.gd boosted — v7.0): 우리 부스터와 상하좌우로 붙어 있으면 체력을 안 내고(pay 0) 차례 시작에 2로 찬다 (부스터 자신은 아님)
static bool boosted(const State &s, const Player &p) {
	if (p.position == BOOSTER)
		return false;
	for (const V2 &d : DIRS) {
		const Player *q = player_at(s, p.pos + d);
		if (q && q->team == p.team && q->position == BOOSTER && q->uid != s.ball_owner_uid)   // 공을 든 부스터는 꺼진다 (v7.0)
			return true;
	}
	return false;
}

// 내 차례 시작 (v7.0 — game_state.gd _turn_stamina): 태세가 풀리고 +1 (최대 2 · 부스터 곁은 2로 다 참)
void State::turn_stamina(int team) {
	for (Player &p : players) {
		if (p.team != team || !p.is_placed())
			continue;
		p.stance = 0;   // v6.5 태세는 내 차례가 오면 풀린다 (v6.6 회복 없음은 v7.0 태세 체력 0으로 폐지)
		p.sta = boosted(*this, p) ? STA_MAX : std::min(p.sta + 1, STA_MAX);
	}
}

static const int DRAFT_POOL[POOL_N] = { CANNON, MAESTRO, TRICKSTER, SPRINTER, MARKER, BOOSTER,
	TITAN, HUNTER, WALL, NOMAD };   // v7.0 — game_state.gd DRAFT_POOL과 같은 순서

static int team_at(const State &s, int i) {
	int cnt[2] = { 0, 0 };
	int who = 0;
	for (int k = 0; k <= i; k++) {
		who = (k / 2) % 2;
		if (cnt[who] >= PER_TEAM)
			who = 1 - who;
		cnt[who] += 1;
	}
	return who == 0 ? s.first_team : opponent(s.first_team);
}

int pool_slot(int position) {
	for (int k = 0; k < POOL_N; k++)
		if (DRAFT_POOL[k] == position)
			return k;
	return -1;
}

static int placement_team(const State &s) {
	if (s.phase != PLACEMENT || s.placement_index >= PLAYERS)
		return -1;
	return team_at(s, s.placement_index);
}

// v7.1: 그 팀에 이미 있는가 — 상대와 겹쳐도 된다 (game_state.gd is_drafted)
static bool is_drafted(const State &s, int position, int team) {
	for (int u = 0; u < PLAYERS; u++)
		if (s.players[u].team == team && s.players[u].position == position)
			return true;
	return false;
}

// 지금 차례 팀에서 다음에 나올 선수 (없으면 -1). 결장 선수는 건너뛴다
static int next_free_uid(const State &s, int team) {
	for (int u = 0; u < PLAYERS; u++)
		if (s.players[u].team == team && !s.players[u].is_placed() && !s.is_absent(u))
			return u;
	return -1;
}

// 재배치: 그 팀에서 이 포지션을 가진, 아직 안 놓은 선수 (game_state.gd _unplaced_with)
static int unplaced_with(const State &s, int team, int position) {
	for (int u = 0; u < PLAYERS; u++)
		if (s.players[u].team == team && s.players[u].position == position && !s.players[u].is_placed() &&
				!s.is_absent(u))
			return u;
	return -1;
}

static void skip_empty_slots(State &s) {
	while (s.placement_index < PLAYERS && next_free_uid(s, team_at(s, s.placement_index)) == -1)
		s.placement_index += 1;
}

// 지금 차례에 이 슬롯을 고를 수 있는가: 처음 배치는 벤치(드래프트), 재배치는 아직 안 놓은 내 선수의 포지션
static bool slot_open(const State &s, int team, int slot) {
	return s.redeploy ? unplaced_with(s, team, DRAFT_POOL[slot]) != -1 : !is_drafted(s, DRAFT_POOL[slot], team);
}

static bool commit_place(State &s, int uid, int position, V2 tile) {
	if (uid < 0)
		return false;
	s.players[uid].position = position;
	s.players[uid].pos = tile;
	s.players[uid].placed_order = s.placement_index;
	s.players[uid].sta = STA_MAX;   // 경기 시작 체력 = 상한 2
	s.placement_index += 1;
	skip_empty_slots(s);
	if (s.placement_index >= PLAYERS) {
		s.phase = PLAY;
		s.turn_team = s.first_team;
		s.pts = POINTS_PER_TURN;
		s.contest_used = 0;
		s.boost_on = 0;
		s.turn_actions = 0;
		s.acted_mask = 0;
	} else {
		s.turn_team = placement_team(s);
	}
	return true;
}

// 첫 인자는 선수 uid가 아니라 **드래프트 슬롯**(DRAFT_POOL 인덱스)이다 (v5.11).
// 놓을 수 있는 곳은 **자기 진영 전부**(박스 제외 — v5.12)
static bool do_place(State &s, int slot, V2 tile) {
	if (s.phase != PLACEMENT || slot < 0 || slot >= POOL_N)
		return false;
	int team = placement_team(s);
	if (team < 0 || !slot_open(s, team, slot))
		return false;
	if (!place_tile_ok(s, team, tile, DRAFT_POOL[slot] == NOMAD))
		return false;
	int uid = s.redeploy ? unplaced_with(s, team, DRAFT_POOL[slot]) : next_free_uid(s, team);
	return commit_place(s, uid, DRAFT_POOL[slot], tile);
}

// ---------- contest.gd ----------

// **수비 태세 보너스**: 태세를 켠 선수는 태클·인터셉트 +1. 월은 늘 태세인 선수라 상시 받는다 (v5.38 ③안 —
// contest.gd stance_bonus)
static int stance_bonus(const Player &p, int key) {
	return (p.wide_zone() && (key == TKL || key == INT_)) ? 1 : 0;
}

// **마커** (spec 6.3 v5.30): 마커에게 상하좌우로 붙은 적은 −1. 태세와 무관하게 상시.
// v5.27~v5.29의 견제(전원 상시)를 대신한다 — contest.gd marker_debuff와 같아야 한다
// 스크린 (v5.54): 공을 든 p의 길에서 아군이 선 칸은 경합이 없다 — 마지막 칸은 예외 (contest.gd screened)
static bool screened(const State &s, const Player &p, V2 tile, V2 last) {
	if (tile == last)
		return false;
	const Player *q = player_at(s, tile);
	return q && q->team == p.team && q->uid != p.uid;
}

// 마커에게 잡혔는가 (game_state.gd marked — v6.4): 상대 마커(공 없음)와 상하좌우로 붙음 · 버티는 캐논은 아님. 잡히면 범위 −1 · 회복 없음
bool marked(const State &s, const Player &p) {
	if (!p.is_placed() || (s.hold == 1 && p.uid == s.ball_owner_uid))
		return false;
	for (const Player &m : s.players)
		if (m.team != p.team && m.position == MARKER && m.is_placed() && m.uid != s.ball_owner_uid && manhattan(m.pos, p.pos) == 1)
			return true;
	return false;
}


// 체력을 낸다 (actions.gd pay) — 누가 얼마를 냈는지 적어 두어 경합 동점의 체력 비교가 **내기 전 숫자**를 쓰게 한다 (v5.93)
static void pay(State &s, int uid, int n) {
	if (boosted(s, s.players[uid]))   // v7.0 부스터 곁은 체력 0 (actions.gd pay)
		n = 0;
	s.pay_uid = uid;
	s.pay_n = n;
	s.players[uid].sta -= n;
}

// 체력이 need 이상인가 — 부스터 곁이면 0이어도 된다 (actions.gd sta_ok)
static bool sta_ok(const State &s, const Player &p) {
	return p.sta >= STA_ACT || boosted(s, p);
}

// 경합에서 겨루는 체력 (contest.gd battery — v5.93): 이번 액션을 한 선수는 낸 만큼을 되돌린 값(행동을 시작할 때 배터리), 나머지는 지금 배터리
static int battery(const State &s, const Player &p) {
	return p.sta + (p.uid == s.pay_uid ? s.pay_n : 0);
}

// 스탯 보정은 태세뿐이다 (GDScript Contest.positional_stat과 한 글자도 다르면 안 된다 — v7.0 부스터는 체력, 마커는 범위)
static int positional_stat(const State &, const Player &p, int key) {
	return std::max(0, p.stat(key) + stance_bonus(p, key));   // v5.93 하한 0
}

// 경합 순위 (contest.gd tie_chain — v5.93): ① 스탯 ② 체력(battery) ③ 공 가진 팀
static bool tie_chain(const State &s, int a, const Player &atk, int d, const Player &def) {
	if (a != d)
		return a > d;
	int ba = battery(s, atk), bd = battery(s, def);
	if (ba != bd)
		return ba > bd;
	return s.ball_team() == atk.team;
}

// 협동 수비 (spec 6.3 v5.59·v5.65 — contest.gd coop): 경합 칸 상하좌우에 메인 말고 **태세 중인(또는 월)** 수비팀 선수가 있으면 +1 (태클·인터셉트만)
static int coop(const State &s, const Player &def, int def_key, V2 tile) {
	if (def_key != TKL && def_key != INT_)
		return 0;
	for (const V2 &d : DIRS) {
		const Player *q = player_at(s, tile + d);
		if (q && q->team == def.team && q->uid != def.uid && q->wide_zone())
			return 1;
	}
	return 0;
}

struct CRes {
	int atk = 0, def = 0;
	bool atk_wins = false;
};


// resolve_with: 공격값 지정 (감쇠 — 5.0). 수비 값 = 제 스탯·태세·마커 + 협동 (v5.59)
Observer *g_observer = nullptr;   // 분석용 관찰자 (engine.h) — 기본은 없음

static CRes resolve_with(State &s, const Player &atk, int atk_value, const Player &def, int def_key,
		V2 tile) {
	int co = coop(s, def, def_key, tile);
	int d = positional_stat(s, def, def_key) + co;
	bool wins = tie_chain(s, atk_value, atk, d, def);
	if (g_observer)
		g_observer->contest(s, atk, def, def_key, co, wins, tile);
	return { atk_value, d, wins };
}

static CRes resolve(State &s, const Player &atk, int atk_key, const Player &def, int def_key, V2 tile) {
	return resolve_with(s, atk, positional_stat(s, atk, atk_key), def, def_key, tile);
}

// 존 (spec 5.7·5.8 v5.30): **태세를 켠 선수와 월(상시)만** 자기 상하좌우 4칸에 존을 갖는다.
// 몸 타일은 빠졌다 — 사람도 공도 적의 몸을 못 지나므로(v5.28) 경합이 날 수 없는 죽은 칸이다.
// 존은 태세·월 선수의 상하좌우 4칸뿐이다. contest.gd _zone과 같아야 한다
static void build_zone(const State &s, int team, int zone[NT]) {
	for (int i = 0; i < NT; i++)
		zone[i] = -1;
	const Player *wide[PER_TEAM];
	int nw = 0;
	for (int u = 0; u < PLAYERS; u++) {
		const Player &q = s.players[u];
		if (q.team == team && q.is_placed() && q.wide_zone()) {
			wide[nw++] = &q;
		}
	}
	std::sort(wide, wide + nw, [](const Player *a, const Player *b) { return a->placed_order < b->placed_order; });
	for (int i = 0; i < nw; i++)
		for (const V2 &d : DIRS) {
			V2 t = wide[i]->pos + d;
			if (in_bounds(t) && !is_goal(t) && zone[idx(t)] == -1)
				zone[idx(t)] = wide[i]->uid;
		}
}

// **길 하나가 일으킬 경합 수** (v5.79 — contest.gd path_contests와 같다): 실행처럼 p를 공 임자로 두고 만든 존으로,
// 이기고 짐과 상관없이 만날 수비수 수. body_too = 드리블(정면돌파 몸 칸). p는 출발 칸에 선 선수(루즈볼 줍기는 주운 칸으로 옮긴 사본)
static bool screened(const State &s, const Player &p, V2 tile, V2 last);
static int path_contests_z(const State &s, const Player &p, const std::vector<V2> &path, bool body_too, const int *zone);
static int path_contests(const State &s, const Player &p, const std::vector<V2> &path, bool body_too) {
	if (path.empty())
		return 0;
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	return path_contests_z(s, p, path, body_too, zone);
}
static int path_contests_z(const State &s, const Player &p, const std::vector<V2> &path, bool body_too, const int *zone) {
	if (path.empty())
		return 0;
	bool met[PLAYERS] = { false };
	int n = 0;
	for (const V2 &tile : path) {
		if (body_too) {
			const Player *body = player_at(s, tile);
			if (body && body->team != p.team && !met[body->uid]) {
				met[body->uid] = true;
				n++;
			}
		}
		int du = zone[idx(tile)];
		if (du != -1 && !met[du] && !screened(s, p, tile, path.back())) {
			met[du] = true;
			n++;
		}
	}
	return n;
}

// ---------- paths.gd ----------

struct DirSet {
	V2 d[4];
	int n = 0;
};
// v5.29: 사방(ALL)은 **네 방향 한 계열**이다. 후방 걸음은 금지가 아니라 값을 치르는 것이
// 됐으므로(뒤로 1칸당 스탯 −1 — back_steps), v5.12의 계열 둘(alt) 구조가 통째로 사라졌다
struct Dirs {
	DirSet first, rest;
};
enum Mode { M_FORWARD, M_LATERAL, M_ALL };

static Dirs step_dirs(int team, int mode) {
	V2 f{ forward_dir(team), 0 };
	DirSet lat;
	lat.d[0] = f;
	lat.d[1] = { 0, 1 };
	lat.d[2] = { 0, -1 };
	lat.n = 3;
	Dirs d;
	d.rest = lat;
	if (mode == M_ALL) {
		DirSet all4;
		all4.d[0] = f;
		all4.d[1] = { -f.x, 0 };
		all4.d[2] = { 0, 1 };
		all4.d[3] = { 0, -1 };
		all4.n = 4;
		d.first = all4;
		d.rest = all4;
		return d;
	}
	if (mode == M_FORWARD) {
		d.first.d[0] = f;
		d.first.n = 1;
	} else {
		d.first = lat;
	}
	return d;
}

// 도달 목록 — GDScript Dictionary의 **삽입 순서(BFS 발견 순서)**를 그대로 보존한다 (결정론 계약)
struct Reach {
	int step[NT];
	V2 order[NT];
	int n = 0;
	Reach() {
		for (int i = 0; i < NT; i++)
			step[i] = -1;
	}
	bool has(V2 t) const { return in_bounds(t) && step[idx(t)] >= 0; }
	void add(V2 t, int st) {
		step[idx(t)] = st;
		order[n++] = t;
	}
};

// goals_open = 골대 타일 위를 지나갈 수 있는가 (공중볼 = 롱패스만 — spec 5.0 v5.12)
static bool passable(const State &s, const int occ[NT], int team, V2 t, bool enemies_passable,
		const bool *blocked, bool goals_open) {
	if (!in_bounds(t) || (is_goal(t) && !goals_open) || (blocked && blocked[idx(t)]))
		return false;
	int o = occ[idx(t)];
	return o == -1 || s.players[o].team == team || enemies_passable;
}

static void reach_one(const State &s, const Player &p, int max_steps, const Dirs &dirs, bool enemies_passable,
		const bool *blocked, V2 goal_dest, Reach &out, bool goals_open) {
	if (max_steps < 1)
		return;
	int occ[NT];
	build_occ(s, occ);
	V2 frontier[NT];
	int nf = 0;
	for (int i = 0; i < dirs.first.n; i++) {
		V2 t = p.pos + dirs.first.d[i];
		if (t == goal_dest && in_bounds(t) && !(blocked && blocked[idx(t)])) {
			out.add(t, 1);
		} else if (passable(s, occ, p.team, t, enemies_passable, blocked, goals_open)) {
			out.add(t, 1);
			frontier[nf++] = t;
		}
	}
	for (int step = 2; step <= max_steps; step++) {
		V2 next[NT];
		int nn = 0;
		for (int fi = 0; fi < nf; fi++)
			for (int di = 0; di < dirs.rest.n; di++) {
				V2 t = frontier[fi] + dirs.rest.d[di];
				if (t == p.pos || out.has(t))
					continue;
				if (t == goal_dest) {
					if (in_bounds(t) && !(blocked && blocked[idx(t)]))
						out.add(t, step);
					continue; // 골대에서는 더 나아가지 않는다
				}
				if (!passable(s, occ, p.team, t, enemies_passable, blocked, goals_open))
					continue;
				out.add(t, step);
				next[nn++] = t;
			}
		for (int i = 0; i < nn; i++)
			frontier[i] = next[i];
		nf = nn;
	}
}

// v5.29: 계열이 하나뿐이라 그대로 한 번만 돈다 (열거 순서가 GDScript Dictionary와
// 비트 단위로 같아야 하는 결정론 계약은 reach_one이 지킨다)
static void reach(const State &s, const Player &p, int max_steps, const Dirs &dirs, bool enemies_passable,
		const bool *blocked, V2 goal_dest, Reach &out, bool goals_open = false) {
	reach_one(s, p, max_steps, dirs, enemies_passable, blocked, goal_dest, out, goals_open);
}

static bool step_ok(V2 t, V2 dest, const bool *blocked, V2 goal_dest) {
	if (!in_bounds(t) || (blocked && blocked[idx(t)]))
		return false;
	if (is_goal(t))
		return t == dest && t == goal_dest;
	return true;
}

// 자동 경로 = 경합 최소 → 그중 최단 (canonical_path). danger = 존 배열(uid/-1) 또는 null
static std::vector<V2> canonical_one(const Player &p, V2 dest, int max_steps, const Dirs &dirs,
		const int *danger, const bool *blocked, V2 goal_dest) {
	std::vector<V2> out;
	if (max_steps < 1)
		return out;
	constexpr int INF_I = 1 << 29;
	const int NK = (max_steps + 1) * NT;
	std::vector<int> cost(NK, INF_I), prev_step(NK, 0);
	std::vector<V2> prev(NK);
	auto key = [](V2 t, int step) { return step * NT + idx(t); };
	auto dang = [danger](V2 t) { return danger && danger[idx(t)] != -1 ? 1 : 0; };
	V2 frontier[NT];
	int nf = 0;
	for (int i = 0; i < dirs.first.n; i++) {
		V2 t = p.pos + dirs.first.d[i];
		if (!step_ok(t, dest, blocked, goal_dest))
			continue;
		int c = dang(t);
		int k = key(t, 1);
		if (cost[k] == INF_I || c < cost[k]) {
			cost[k] = c;
			prev[k] = p.pos;
			prev_step[k] = 0;
			frontier[nf++] = t;
		}
	}
	for (int step = 1; step < max_steps; step++) {
		V2 next[NT];
		int nn = 0;
		bool in_next[NT] = {};
		for (int fi = 0; fi < nf; fi++) {
			V2 tile = frontier[fi];
			int base = cost[key(tile, step)];
			if (tile == dest)
				continue; // 목적지 도착 후에는 더 걷지 않는다
			for (int di = 0; di < dirs.rest.n; di++) {
				V2 t = tile + dirs.rest.d[di];
				if (t == p.pos || !step_ok(t, dest, blocked, goal_dest))
					continue;
				int c = base + dang(t);
				int k = key(t, step + 1);
				if (cost[k] != INF_I && cost[k] <= c)
					continue;
				cost[k] = c;
				prev[k] = tile;
				prev_step[k] = step;
				if (!in_next[idx(t)]) {
					in_next[idx(t)] = true;
					next[nn++] = t;
				}
			}
		}
		for (int i = 0; i < nn; i++)
			frontier[i] = next[i];
		nf = nn;
	}
	int pick_step = -1, pick_cost = -1;
	for (int step = 1; step <= max_steps; step++) {
		int k = key(dest, step);
		if (cost[k] == INF_I)
			continue;
		if (pick_step == -1 || cost[k] < pick_cost) {
			pick_step = step;
			pick_cost = cost[k];
		}
	}
	if (pick_step == -1)
		return out;
	V2 cur = dest;
	int cs = pick_step;
	while (cs > 0) {
		out.push_back(cur);
		int k = key(cur, cs);
		cur = prev[k];
		cs = prev_step[k];
	}
	std::reverse(out.begin(), out.end());
	return out;
}

// v5.29: 계열이 하나뿐이라 그대로 한 번만 푼다 (paths.gd canonical_path와 같다)
static std::vector<V2> canonical_path(const Player &p, V2 dest, int max_steps, const Dirs &dirs,
		const int *danger, const bool *blocked, V2 goal_dest) {
	if (max_steps < 1)
		return {};
	return canonical_one(p, dest, max_steps, dirs, danger, blocked, goal_dest);
}

// ---------- actions.gd ----------

// v5.15: 루즈볼 동안에도 ball_owner_uid는 -1이다 — 그때를 킥오프로 착각하면 안 된다
bool kickoff_turn(const State &s) {   // actions.gd is_kickoff_turn
	return s.phase == PLAY && s.ball_owner_uid == -1 && !s.loose_ball() && s.turn_team == opponent(s.first_team);
}

static bool can_act(const State &s, const Player &p) {   // v5.86: 체력 1 이상
	return p.is_placed() && s.phase == PLAY && p.team == s.turn_team && s.pts >= 1 && sta_ok(s, p);
}

// 이 액션이 낼 수 있는 경합 수 (actions.gd contest_room) — v5.91 경합은 체력을 따로 안 든다: 제한 없음 (엔진은 일반 모드만)
static int contest_room(const Player &, int) {
	return 99;
}

// 이동 가능 (actions.gd _can_move): 태세 중에도 옮길 수 있고 옮기면 풀린다 (spec 5.12 v5.59 — 해제 액션 폐지) · 체력 1 (v5.97)
static bool can_move(const State &s, const Player &p) {
	return can_act(s, p) && s.ball_owner_uid != p.uid && !kickoff_turn(s) && sta_ok(s, p);
}

// v5.51: 공을 뺏겨도 턴은 안 끝난다 — 포인트가 0일 때만 (actions.gd _after_action과 같다)
static void after_action(State &s) {
	if (s.phase != PLAY)
		return;
	if (s.pts <= 0)
		s.end_turn();
}

// 공을 가진 걸음(드리블·주운 뒤 몰기)의 기본 벽 (actions.gd _box_blocked — v5.88): 양쪽 박스는 **지나간다**(멈추지는 못함), 골대 칸만 벽.
// 엔진은 일반 모드만 돈다 (체스 모드의 박스 벽은 GDScript 쪽)
static void box_blocked(bool blocked[NT]) {
	for (int i = 0; i < NT; i++)
		blocked[i] = false;
	blocked[idx(goal_of(HOME))] = blocked[idx(goal_of(AWAY))] = true;
}

// v5.12 통과 규칙: **아군의 몸은 지나가고(스크린) 적군의 몸은 못 지나간다.**
// 아군 타일은 경로로만 쓰이고 목적지에서는 걸러진다 (legal_* 참조)
static void enemy_standing_blocked(const State &s, int team, bool blocked[NT]) {
	for (int u = 0; u < PLAYERS; u++)
		if (s.players[u].is_placed() && s.players[u].team != team)
			blocked[idx(s.players[u].pos)] = true;
}

// 이동 사거리 = 포지션 값 − 마커에게 잡히면 1 (spec 5.2 · v6.4 — actions.gd _move_range)
static int move_rng(const State &s, const Player &p) {
	return std::max(1, p.move_range() - (marked(s, p) ? 1 : 0));
}

// 이동(공 없는 걸음)의 차단 칸 (actions.gd _move_blocked — v5.82): 양쪽 박스는 **지나갈 수 있다**(멈추지는 못함 — move_dest_ok).
// 골대 칸은 아무도 못 지난다. 적의 몸은 벽 — 스프린터만 지나간다
static void move_blocked(const State &s, const Player &p, bool blocked[NT]) {
	for (int i = 0; i < NT; i++)
		blocked[i] = false;
	blocked[idx(goal_of(HOME))] = blocked[idx(goal_of(AWAY))] = true;
	if (p.position != SPRINTER)
		enemy_standing_blocked(s, p.team, blocked);
}

// 멈출 수 있는 칸 (actions.gd _move_dest_ok): 빈 칸, 박스 밖 — 박스에는 **노마드만** 선다 (v7.0, 골대 칸은 아무도). 엔진은 일반 모드만 돈다
static bool move_dest_ok(const State &s, V2 t, const Player &p) {
	if (player_at(s, t) || is_goal(t))
		return false;
	return !is_any_box(t) || p.position == NOMAD;
}

// 배치 칸 (game_state.gd place_tile_ok): 자기 진영의 빈 칸, 박스 밖 — 노마드는 박스도 (v7.0)
static bool place_tile_ok(const State &s, int team, V2 tile, bool nomad) {
	if (!is_own_half(team, tile) || player_at(s, tile) || is_goal(tile))
		return false;
	return !is_any_box(tile) || nomad;
}

// 밀려날 뒤 칸이 열려 있는가 (actions.gd _pushable): 필드 밖·골대·박스·선수 칸으로는 못 민다 — 공 없는 노마드는 박스로도 (v7.0)
static bool pushable(const State &s, const Player &q, V2 to) {
	if (!in_bounds(to) || player_at(s, to) || is_goal(to))
		return false;
	return !is_any_box(to) || (q.position == NOMAD && s.ball_owner_uid != q.uid);
}

// 타이탄 착지 충격파 (spec 5.9 v5.6): 도착 칸 사방 인접 적 전원을 각각 바깥으로 1칸 —
// 대상마다 개별 경합, DIRS 순서 고정 (앞선 밀림이 뒤 칸을 비우거나 막는다)
// v5.61 피지컬 폐지: 경합 없이 **무조건** 밀린다 (뒤 칸이 막혔거나 버티는 캐논만 예외 — actions.gd _titan_push)
static void titan_push(State &s, const Player &p, V2 dest) {
	if (p.position != TITAN || s.ball_owner_uid == p.uid)
		return;
	for (const V2 &dir : DIRS) {
		const Player *q = player_at(s, dest + dir);
		if (!q || q->team == p.team || !pushable(s, *q, q->pos + dir) || (s.hold == 1 && q->uid == s.ball_owner_uid))
			continue;   // 버티는 캐논(v5.45)은 안 밀린다. v5.71: 박스 안 선수도 밀린다
		Player &qm = s.players[q->uid];
		qm.pos = qm.pos + dir;
		qm.stance = 0; // 밀려나면 수비 태세 해제 (spec 5.12 v5.3)
	}
}

// 제자리 밀기 (spec 5.9 v5.41) — actions.gd legal_pushes·do_push와 같아야 한다 (DIRS 순서까지)
static int legal_pushes(const State &s, const Player &p, int out[4]) {
	int n = 0;
	if (p.position != TITAN || !can_act(s, p) || s.ball_owner_uid == p.uid || kickoff_turn(s))
		return 0;
	for (const V2 &dir : DIRS) {
		const Player *q = player_at(s, p.pos + dir);
		if (q && q->team != p.team && pushable(s, *q, q->pos + dir) && !(s.hold == 1 && q->uid == s.ball_owner_uid))
			out[n++] = q->uid;   // v5.71: 박스 안 선수도 밀린다
	}
	return n;
}

static void after_action(State &s);
static bool do_push(State &s, int uid, int target_uid) {
	const Player &p = s.players[uid];
	int t[4];
	int n = legal_pushes(s, p, t);
	bool ok = false;
	for (int i = 0; i < n; i++)
		ok = ok || t[i] == target_uid;
	if (!ok)
		return false;
	V2 dir = s.players[target_uid].pos - p.pos;
	s.turn_actions += 1;
	s.pts -= 1;
	pay(s, uid, STA_ACT);
	Player &qm = s.players[target_uid]; // v5.61: 무조건 밀린다
	qm.pos = qm.pos + dir;
	qm.stance = 0;
	s.mark_acted(uid);
	after_action(s);
	return true;
}

// 드리블 경로의 태클 존 경합 — 드리블과 **루즈볼 주워 몰고 가는 이동**(v5.32)이 같이 쓴다.
// players[uid].pos = 출발 칸. 끝 칸(뺏기면 마지막 빈 칸)을 돌려준다 (GDScript _dribble_contests)
static V2 dribble_run(State &s, int uid, const std::vector<V2> &path, bool special = false) {
	V2 last_free = s.players[uid].pos;
	bool stolen = false;
	// v5.73 치달 폐지 · v5.68 후진 값 폐지 — DRB는 늘 온전한 값 (actions.gd _drb_ctx)
	(void)special;
	int drb = positional_stat(s, s.players[uid], DRB);
	int zone[NT];
	build_zone(s, opponent(s.players[uid].team), zone);   // v5.30: 태세·월만 4칸
	bool met[PLAYERS] = { false };   // 한 선수와는 한 번만 (spec 5.8 v5.18)
	const V2 start = s.players[uid].pos;
	for (const V2 &tile : path) {
		// **정면돌파** (v5.68 트릭스터 특기): 적의 몸 칸에 들어가기 직전 칸에서 그 적과 경합 — 지면 그 앞 칸에 서서 뺏긴다
		const Player *body = player_at(s, tile);
		if (body && body->team != s.players[uid].team && !met[body->uid]) {
			met[body->uid] = true;
			s.contest_used = 1;   // v5.79: 이번 턴의 경합을 썼다
			CRes cn = resolve_with(s, s.players[uid], drb, *body, TKL, last_free);
			if (!cn.atk_wins) {
				s.set_ball_owner(body->uid);
				stolen = true;
				break;
			}
			s.players[body->uid].stance = 0;   // v5.68: 경합에서 진 수비수는 태세가 풀린다
			// v5.72 감쇠 폐지 — 이겨도 공격 값 그대로 (ball_actions.gd · actions.gd 와 같다)
		}
		// **빈 칸 갱신이 경합보다 먼저다** (v5.21) — 진 그 칸까지 가서 거기 선다 (GDScript _dribble_contests와 같은 순서)
		if (body == nullptr)
			last_free = tile;
		if (zone[idx(tile)] != -1 && !met[zone[idx(tile)]] && screened(s, s.players[uid], tile, path.back()) && g_observer)
			g_observer->screen(s, s.players[uid], tile);   // 분석용 — 규칙과 무관
		if (zone[idx(tile)] != -1 && !met[zone[idx(tile)]] && !screened(s, s.players[uid], tile, path.back())) {
			met[zone[idx(tile)]] = true;
			s.contest_used = 1;   // v5.79
			int du = zone[idx(tile)];
			CRes c = resolve_with(s, s.players[uid], drb, s.players[du], TKL, tile);
			if (!c.atk_wins) {
				s.set_ball_owner(zone[idx(tile)]); // 수비수가 자기 타일에서 소유 (spec 5.3)
				stolen = true;
				break;
			}
			s.players[du].stance = 0;   // v5.68: 경합에서 진 수비수는 태세가 풀린다
			// v5.72 감쇠 폐지 — 이겨도 공격 값 그대로 (ball_actions.gd · actions.gd 와 같다)
		}
	}
	return stolen ? last_free : path.back();
}

// 루즈볼이 판에 있을 때의 이동 길 (actions.gd _carry_paths와 같다 — 전원): 상태 = (칸, 주웠나) 넓이 우선 탐색.
// 줍기 전은 보통 이동 차단, 루즈볼 칸을 밟은 뒤로는 적 몸을 못 지나고 박스에 멈추지 못한다(v5.88). out[idx(t)] = 그 칸까지 처음 닿은 경로 (빈 경로 = 못 닿음)
static bool carry_case(const State &s) {
	return s.loose_ball();
}

static void carry_paths(const State &s, const Player &p, std::vector<V2> out[NT]) {
	bool before[NT];
	move_blocked(s, p, before);   // 줍기 전 = 보통 이동 차단 (스프린터도 박스는 못 지난다)
	bool fb[NT];
	box_blocked(fb);
	enemy_standing_blocked(s, p.team, fb);
	Dirs dirs = step_dirs(p.team, M_ALL);
	bool seen[NT][2] = {};
	struct St { V2 t; bool picked; std::vector<V2> path; };
	std::vector<St> frontier{ { p.pos, false, {} } };
	int rng = move_rng(s, p);
	for (int step = 0; step < rng; step++) {
		std::vector<St> next;
		for (const St &st : frontier) {
			const DirSet &ds = step == 0 ? dirs.first : dirs.rest;
			for (int i = 0; i < ds.n; i++) {
				V2 t = st.t + ds.d[i];
				if (t == p.pos || !in_bounds(t) || is_goal(t))
					continue;
				if (st.picked ? fb[idx(t)] : before[idx(t)])
					continue; // 공을 든 뒤로는 적 몸을 못 지난다 (박스는 지나가기만 — v5.88)
				bool picked = st.picked || t == s.ball_pos;
				if (seen[idx(t)][picked ? 1 : 0])
					continue;
				seen[idx(t)][picked ? 1 : 0] = true;
				std::vector<V2> path = st.path;
				path.push_back(t);
				if (out[idx(t)].empty() && !(picked && is_any_box(t)))   // 공을 든 채로는 박스를 지나기만 한다 (v5.88)
					out[idx(t)] = path;
				next.push_back({ t, picked, path });
			}
		}
		frontier.swap(next);
	}
}

// do_move_to가 걷는 길 (actions.gd _move_path)
static std::vector<V2> move_path(const State &s, const Player &p, V2 dest) {
	if (carry_case(s)) {
		std::vector<V2> sp[NT];
		carry_paths(s, p, sp);
		return sp[idx(dest)];
	}
	bool blocked[NT];
	move_blocked(s, p, blocked);
	return canonical_path(p, dest, move_rng(s, p), step_dirs(p.team, M_ALL), nullptr, blocked, UNSET);
}

// 루즈볼을 주운 뒤 남은 걸음의 경합 수 (actions.gd _carry_contests — 주운 칸에서 출발하는 드리블로 센다)
static int carry_contests(const State &s, const Player &p, const std::vector<V2> &path) {
	if (!s.loose_ball() || path.empty())
		return 0;
	for (size_t k = 0; k < path.size(); k++)
		if (path[k] == s.ball_pos) {
			if (k + 1 >= path.size())
				return 0;
			Player pc = p;
			pc.pos = path[k];
			return path_contests(s, pc, std::vector<V2>(path.begin() + k + 1, path.end()), true);
		}
	return 0;
}

static bool do_move_to(State &s, int uid, V2 dest) {
	const Player &p = s.players[uid];
	if (!can_move(s, p))
		return false;
	if (!move_dest_ok(s, dest, p))
		return false;
	std::vector<V2> path = move_path(s, p, dest);
	if (path.empty() || carry_contests(s, p, path) > contest_room(p, STA_ACT))
		return false;
	s.turn_actions += 1;
	s.pts -= 1;   // 이동은 늘 1포인트 (v5.81: 스프린터 0포인트 줍기 폐지 — actions.gd _move_exec과 같다)
	pay(s, uid, STA_ACT);   // v5.97 이동 체력 1
	// 루즈볼 습득 (spec 5.4 v5.32): 경로의 **어느 칸이든** 공을 밟으면 줍고, 남은 걸음은
	// 주운 칸에서 출발하는 드리블로 경합한다 (GDScript _move_exec과 같다)
	bool picked = false;
	if (s.loose_ball()) {
		for (size_t k = 0; k < path.size(); k++)
			if (path[k] == s.ball_pos) {
				picked = true;
				s.set_ball_owner(uid);
				s.players[uid].pos = path[k];
				if (k + 1 < path.size())
					dest = dribble_run(s, uid, std::vector<V2>(path.begin() + k + 1, path.end()));
				break;
			}
	}
	s.players[uid].pos = dest;
	if (!picked)
		titan_push(s, s.players[uid], dest); // 공을 주운 타이탄은 착지 밀기를 안 한다 (spec 5.2 — 남은 걸음에서 뺏겼어도)
	s.mark_acted(uid); // 경합 계산 뒤에 찍는다 (mark_acted가 태세를 푼다)
	after_action(s);
	return true;
}

static bool may_dribble(const State &s, const Player &p) {
	if (!sta_ok(s, p))
		return false;   // v5.86 드리블은 체력 2
	if (s.ball_owner_uid == p.uid)
		return true;
	return kickoff_turn(s) && p.team == s.turn_team;
}

// v5.29 (사용자 확정): **전원 사방.** 후방은 금지가 아니라 값이다 — 뒤로 1칸당 DRB −1.
// 트릭스터의 후방 꺾기 특전도 엔드 라인 보정도 폐지됐다 (남는 것은 DRB 4 · 드리블 범위 3)
static Dirs dribble_dirs(const State &, const Player &p) {
	return step_dirs(p.team, M_ALL);
}

// 드리블 거리 = 이동 거리 (spec 5.3 v5.73 — 치달 폐지)
static int dribble_range(const State &s, const Player &p) {
	return move_rng(s, p);
}

// v5.68 트릭스터 정면돌파: 트릭스터는 적의 몸 칸을 지나간다 (actions.gd _nutmeg · _dribble_blocked). v6.0 포인트 1 · 체력 2 (드리블과 같다)
static bool nutmeg(const State &, const Player &p) {
	return p.position == TRICKSTER;
}

static void dribble_blocked(const State &s, const Player &p, bool blocked[NT]) {
	box_blocked(blocked);
	if (!nutmeg(s, p))
		enemy_standing_blocked(s, p.team, blocked);
}

// 트릭스터 정면돌파인가 (actions.gd _through_foe): 적의 몸 칸을 지나는 길
static bool through_foe(const State &s, const Player &p, const std::vector<V2> &path) {
	if (!nutmeg(s, p))
		return false;
	for (const V2 &t : path) {
		const Player *q = player_at(s, t);
		if (q && q->team != p.team)
			return true;
	}
	return false;
}

static bool do_dribble_to(State &s, int uid, V2 dest) {
	const Player &p = s.players[uid];
	if (!can_act(s, p) || !may_dribble(s, p) || is_any_box(dest))   // 박스는 지나가기만 (v5.88)
		return false;
	int zone0[NT]; // v5.25: 패시브 태클 존은 **전원 5칸**이다 (contest.gd tackle_zone 주석)
	build_zone(s, opponent(p.team), zone0);
	bool blocked[NT];
	dribble_blocked(s, p, blocked);
	std::vector<V2> path = canonical_path(p, dest, dribble_range(s, p), dribble_dirs(s, p), zone0, blocked, UNSET);
	if (path.empty() || player_at(s, dest) != nullptr || path_contests(s, p, path, true) > contest_room(p, STA_ACT))
		return false;
	if (s.ball_owner_uid == -1)
		s.set_ball_owner(uid); // 킥오프 (spec 3.2)
	if (s.hold == 1)
		s.hold = 2; // v5.54: 버틴 뒤 몰면 버티기가 풀린다 (actions.gd _dribble_exec)
	bool special = through_foe(s, p, path);
	s.turn_actions += 1;
	s.pts -= 1;
	pay(s, uid, STA_ACT);   // v5.86 드리블 체력 2 (정면돌파도 — v6.0)
	s.players[uid].pos = dribble_run(s, uid, path, special);
	s.mark_acted(uid);
	after_action(s);
	return true;
}

static int legal_tackle_target(const State &s, const Player &p) { // -1 = 없음
	if (!can_act(s, p) || s.ball_owner_uid == -1 || kickoff_turn(s))
		return -1;
	const Player &owner = s.players[s.ball_owner_uid];
	if (owner.team == p.team || s.hold == 1) // 버티는 캐논은 태클당하지 않는다
		return -1;
	if (is_any_box(p.pos))   // 박스 안에서는 공을 못 갖는다 — 태클 없음 (노마드, v7.0)
		return -1;
	if (contest_room(p, STA_ACT) < 1)
		return -1;   // v5.86: 태클은 경합이다 — 체력 1 + 경합 1
	if (p.position == WALL)
		return -1; // 월은 직접 태클이 없다 (v5.52) — 존의 자동 태클·인터셉트만
	int dist = manhattan(owner.pos, p.pos);
	if (dist > p.tackle_range())
		return -1;   // 붙은 공 임자에게만 — 헌터도 (v6.2 2칸 태클 폐지)
	return owner.uid;
}

static bool do_tackle(State &s, int uid, int target_uid) {
	const Player &p = s.players[uid];
	if (legal_tackle_target(s, p) != target_uid)
		return false;
	const Player &owner = s.players[target_uid];
	s.turn_actions += 1;
	s.pts -= 1;
	pay(s, uid, STA_ACT);    // 태클 1 (v5.91 경합 몫 없음)
	CRes c = resolve(s, owner, DRB, p, TKL, owner.pos); // 소유자 공격(DRB) vs 태클러 수비(TKL)
	s.contest_used = 1;
	if (!c.atk_wins) {
		s.set_ball_owner(uid);
	}
	s.mark_acted(uid);
	after_action(s);
	return true;
}

// 수비 태세 (spec 5.12 v5.3 · v5.13 복원 — v5.12의 자동 태세는 폐지)
static bool can_stance(const State &s, const Player &p) {
	return can_act(s, p) && s.ball_owner_uid != p.uid && !kickoff_turn(s) &&
			p.stance == 0 && p.position != WALL && !is_any_box(p.pos);   // 월은 늘 태세다 (v5.38 ③안) · 박스 안은 태세 불가 (노마드, v7.0)
}

static bool do_stance(State &s, int uid) {
	if (!can_stance(s, s.players[uid]))
		return false;
	s.turn_actions += 1;
	s.pts -= 1;
	pay(s, uid, 0);   // v7.0 태세는 체력 0 (actions.gd do_stance)
	s.mark_acted(uid); // 다른 태세를 푸는 표시 — 새 태세는 그 뒤에 켠다
	s.players[uid].stance = 1;
	after_action(s);
	return true;
}

// 턴 종료 (v5.68 — actions.gd can_end_turn·do_end_turn): 남은 포인트를 버린다
static bool can_end_turn(const State &s) {
	return s.phase == PLAY && s.pts > 0 && !kickoff_turn(s);   // 킥오프 첫 액션은 드리블·패스뿐 (spec 3.2)
}

static bool do_end_turn(State &s) {
	if (!can_end_turn(s))
		return false;
	s.pts = 0;
	s.end_turn();
	return true;
}


// ---------- ball_actions.gd ----------

// kickoff_ok = 킥오프 턴의 후공 선수도 공 주인으로 쳐 준다 (v5.14: 패스만. 슛은 여전히 불가)
static const Player *ball_actor(const State &s, int uid, bool kickoff_ok = false) {
	const Player &p = s.players[uid];
	if (s.phase != PLAY || p.team != s.turn_team)
		return nullptr;
	if (s.pts < 1)
		return nullptr;   // 체력은 슛만 본다 (do_shot) — 패스는 체력 0 (v7.0)
	if (s.ball_owner_uid == uid)
		return &p;
	if (kickoff_ok && kickoff_turn(s) && p.team == s.turn_team)
		return &p;
	return nullptr;
}

// 패스·슛 범위 = 기본 − 마커에게 잡히면 1 (v6.4 — ball_actions.gd _ball_range)
static int ball_range(const State &s, const Player &p, int base) {
	return std::max(1, base - (marked(s, p) ? 1 : 0));
}

// 통과 규칙: 공도 **적의 몸은 못 지나간다** (v5.28 — v5.18의 "공은 몸을 지나간다"를 되돌린다).
// 아군의 몸 위로는 그냥 날아간다. 공은 박스를 지난다 (패스는 박스 칸에 멈추지만 못한다 — v5.37 pass_ctx).
// v5.18에서 열었던 이유(골대 앞 한 줄이면 슛이 불법)는 v5.28의 **전원 횡슛**이 감당한다.
// 존(5칸)은 그대로라 인터셉트는 살아 있다 — 몸이 벽, 그 사방이 경합 칸이다.
// GDScript BallActions._foe_tiles와 같은 집합이어야 한다 (test_native_parity가 지킨다)
static void ball_blocked(const State &s, const Player &p, bool blocked[NT]) {
	for (int i = 0; i < NT; i++)
		blocked[i] = false;
	for (const Player &q : s.players)
		if (q.team != p.team && q.is_placed())
			blocked[idx(q.pos)] = true;
}

// v5.29 (사용자 확정): **전원 사방.** 후방은 금지가 아니라 값이다 — 뒤로 1칸당 PAS −1.
// 마에스트로의 후방 꺾기 특전도 엔드 라인 보정도 폐지됐다 (남는 것은 사거리 5 = 롱패스)
static Dirs pass_dirs(const State &, const Player &p) {
	return step_dirs(p.team, M_ALL);
}

struct PassCtx {
	Dirs dirs;
	int rng;
	Reach dist;
	bool blocked[NT];
};

// 공중볼(롱패스 = 5칸 이상)만 적의 몸과 골대 위를 넘는다 (spec 5.0 v5.12). 사거리가 5에 못 미치면(런처 6 외엔 4)
// 롱패스 자체가 불가능하므로 두 번째 탐색을 돌리지 않는다 (열거는 탐색 봇의 핫루프다)

static void pass_ctx(const State &s, const Player &p, PassCtx &ctx) {
	ctx.dirs = pass_dirs(s, p);
	ctx.rng = ball_range(s, p, p.pass_range());
	ball_blocked(s, p, ctx.blocked);
	reach(s, p, ctx.rng, ctx.dirs, true, ctx.blocked, UNSET, ctx.dist);   // v5.28: 몸 = 벽
	bool open[NT];
	for (int i = 0; i < NT; i++)
		open[i] = false;
	Reach air;
	bool air_done = false;
	if (ctx.rng >= LONG_PASS_MIN && sta_ok(s, p)) {   // 롱패스는 체력 1 (v7.0)
		// 공중볼만 몸 위로 넘어간다 — 그래서 벽을 빼고 다시 한 번 훑는다 (GDScript와 같은 예외)
		reach(s, p, ctx.rng, ctx.dirs, true, open, UNSET, air, true);
		air_done = true;
		for (int i = 0; i < air.n; i++) {
			V2 t = air.order[i];
			if (!ctx.dist.has(t) && manhattan(p.pos, t) >= LONG_PASS_MIN)
				ctx.dist.add(t, air.step[idx(t)]);
		}
	}
	(void)air_done;
	// v5.37: 패스는 박스를 **지나갈 수는 있지만 멈추지는 못한다** — 박스 칸은 목적지에서 뺀다 (GDScript와 같은 순서 유지)
	Reach keep;
	bool far_ok = sta_ok(s, p);   // 5칸 이상은 전부 공중볼 — 체력이 없으면 땅길로 닿은 5칸도 표적이 아니다 (v7.0 롱패스 체력 1)
	for (int i = 0; i < ctx.dist.n; i++) {
		V2 t = ctx.dist.order[i];
		if (!is_any_box(t) && (far_ok || manhattan(p.pos, t) < LONG_PASS_MIN))
			keep.add(t, ctx.dist.step[idx(t)]);
	}
	ctx.dist = keep;
}

// _send_ball: 경로 인터셉트 — 한 경로에서 **한 선수와는 한 번만** 경합한다 (spec 5.8, 사람 기준). 감쇠 없음 (v5.72).
// 반환 = 인터셉트 여부. back은 옛 후진 값 자리(늘 0)
static bool send_ball(State &s, const Player &p, const std::vector<V2> &path, int key, int back = 0, int sta = 0) {
	int zone[NT];
	build_zone(s, opponent(p.team), zone);   // v5.30: 태세·월만 4칸
	s.turn_actions += 1;
	s.pts -= 1;   // v7.0 패스도 포인트 1 (옛 마에스트로 포인트 0은 없어졌다)
	pay(s, p.uid, sta);   // 짧은 패스 0 · 롱패스 1 (v7.0) — 동점 체력 비교용 기록
	int atk = std::max(0, positional_stat(s, p, key) - back);
	bool met[PLAYERS] = { false };
	for (const V2 &tile : path) {
		int du = zone[idx(tile)];
		if (du == -1 || met[du])
			continue;
		if (screened(s, p, tile, path.back())) {
			if (g_observer)
				g_observer->screen(s, p, tile);   // 분석용 — 규칙과 무관
			continue;
		}
		met[du] = true;
		s.contest_used = 1;   // v5.79
		CRes c = resolve_with(s, p, atk, s.players[du], INT_, tile);
		if (c.atk_wins) {
			s.players[du].stance = 0;   // v5.68: 경합에서 진 수비수는 태세가 풀린다
			// v5.72 감쇠 폐지 — 이겨도 공격 값 그대로 (ball_actions.gd · actions.gd 와 같다)
		} else {
			s.set_ball_owner(zone[idx(tile)]);
			return true;
		}
	}
	return false;
}

// **오프사이드 선** (spec 5.4 v5.38): 상대의 가장 뒤 선수(상대 골라인에 가장 가까운)가 선 x. 상대가 없으면 −1
// (ball_actions.gd offside_line — 같은 x면 먼저 나온 선수가 남는다)
static int offside_line(const State &s, int team) {
	int fwd = forward_dir(team);
	int best = -1, best_v = -999;
	for (const Player &q : s.players)
		if (q.team != team && q.is_placed() && q.pos.x * fwd > best_v) {
			best_v = q.pos.x * fwd;
			best = q.pos.x;
		}
	return best;
}

// 그 칸의 아군에게 패스하면 오프사이드: 상대 진영 · 패서보다 앞 · 선보다 앞 (셋 다 엄격히 — _is_offside)
static bool is_offside(const Player &p, V2 tile, int line) {
	if (line < 0)
		return false;
	int fwd = forward_dir(p.team);
	return !is_own_half(p.team, tile) && tile.x * fwd > p.pos.x * fwd && tile.x * fwd > line * fwd;
}

// 빈 칸 목적지 = 루즈볼 놓기 (spec 5.4). 아군 목적지는 오프사이드면 안 된다 (v5.38)
static bool do_pass_to(State &s, int uid, V2 dest) {
	const Player *p = ball_actor(s, uid, true);
	if (!p)
		return false;
	PassCtx ctx;
	pass_ctx(s, *p, ctx);
	if (!ctx.dist.has(dest))
		return false;
	// 목적지 = 아군이 선 칸, 또는 빈 칸(루즈볼 — 전원). 골대 칸은 구조물이라 제외
	const Player *receiver = player_at(s, dest);
	if (is_offside(*p, dest, offside_line(s, p->team)))
		return false;   // v5.70: 오프사이드 지역은 빈 칸도 못 보낸다
	if (!receiver) {
		if (is_goal(dest) || !p->space_pass())
			return false;
	} else if (receiver->team != p->team || receiver->uid == p->uid) {
		return false;
	}
	bool long_pass = manhattan(p->pos, dest) >= LONG_PASS_MIN;
	if (long_pass && !sta_ok(s, *p))
		return false;   // 롱패스는 체력 1 (v7.0)
	bool aerial = long_pass;   // 공중볼 = 롱패스. 낙하 칸에서만 경합한다
	std::vector<V2> path;
	if (aerial)
		path.push_back(dest);
	else {
		int zone[NT];
		build_zone(s, opponent(p->team), zone);   // v5.30: 태세·월만 4칸
		path = canonical_path(*p, dest, ctx.rng, ctx.dirs, zone, ctx.blocked, UNSET);
		if (path.empty())
			return false;
	}
	if (path_contests(s, *p, path, false) > contest_room(*p, STA_ACT))
		return false;
	int receiver_uid = receiver ? receiver->uid : -1;
	int team = p->team;
	if (s.ball_owner_uid == -1)
		s.set_ball_owner(uid); // 킥오프 패스 — 공은 이 선수에게서 출발한다 (spec 3.2 v5.14)
	bool intercepted = send_ball(s, *p, path, PAS, 0, long_pass ? STA_ACT : 0);
	if (!intercepted) {
		if (receiver_uid != -1) {
			s.set_ball_owner(receiver_uid);
		}
		else
			s.drop_ball(dest, team); // 공간 패스 — 소유권이 넘어간 게 아니라 턴은 안 넘어간다
	}
	s.mark_acted(uid);
	after_action(s);
	return true;
}

// v5.28 (사용자 확정): **전원 상시 횡 오픈.** 캐논의 값어치는 방향이 아니라 사거리(5칸)로 옮겼다.
// 전진 전용이던 v5.14~v5.27에서는 적진 끝 라인에 선 선수가 슛을 아예 못 했다 (첫 걸음이
// 반드시 전진이라 필드 밖으로 나간다) — 전진에 보상이 없던 원인의 절반이 이 규칙이었다
static Dirs shot_dirs(const State &, const Player &p) {
	return step_dirs(p.team, M_ALL);   // v5.72: 슛도 패스와 같은 사방 (ball_actions.gd _shot_dirs)
}

struct ShotCtx {
	Dirs dirs;
	int rng;
	V2 goal;
	Reach dist;
	bool blocked[NT];
};

static void shot_ctx(const State &s, const Player &p, ShotCtx &ctx) {
	ctx.dirs = shot_dirs(s, p);
	ctx.rng = ball_range(s, p, p.shot_range());
	ctx.goal = opponent_goal(p.team);
	ball_blocked(s, p, ctx.blocked);
	reach(s, p, ctx.rng, ctx.dirs, true, ctx.blocked, ctx.goal, ctx.dist);   // v5.28: 몸 = 벽
}

static bool do_shot_auto(State &s, int uid) {
	const Player *p = ball_actor(s, uid);
	if (!p || !sta_ok(s, *p))   // 슛 체력 1, 부스터 곁 0 (spec 5.6 v7.0)
		return false;
	ShotCtx ctx;
	shot_ctx(s, *p, ctx);
	if (!ctx.dist.has(ctx.goal))
		return false;
	int zone[NT];
	build_zone(s, opponent(p->team), zone);   // 태세·월만 4칸
	std::vector<V2> path = canonical_path(*p, ctx.goal, ctx.rng, ctx.dirs, zone, ctx.blocked, ctx.goal);
	if (path.empty() || path_contests(s, *p, path, false) > contest_room(*p, STA_ACT))
		return false;
	s.turn_actions += 1;
	s.pts -= 1;
	pay(s, uid, STA_ACT);   // 슛 체력 1 (v7.0)
	int atk = positional_stat(s, *p, SHT);   // ball_actions shot_stat
	bool met[PLAYERS] = { false };   // 한 선수와는 한 번만 (spec 5.8 v5.18)
	for (const V2 &tile : path) {
		int du = zone[idx(tile)];
		if (du == -1 || met[du])
			continue;
		if (screened(s, *p, tile, path.back())) {
			if (g_observer)
				g_observer->screen(s, *p, tile);   // 분석용 — 규칙과 무관
			continue;
		}
		met[du] = true;
		s.contest_used = 1;   // v5.79
		CRes c = resolve_with(s, *p, atk, s.players[du], INT_, tile);
		if (c.atk_wins) {
			s.players[du].stance = 0;   // 경합에서 진 수비수는 태세가 즉시 풀린다 (v5.68 — 패스·드리블과 같다). 감쇠 없음 (v5.72)
		} else {
			s.set_ball_owner(du);
			s.mark_acted(uid);
			after_action(s);
			return true;
		}
	}
	s.score_goal(p->team); // 골대에 닿았다 = 골 (spec 3.5 v5.38) — 재시작·하프타임·종료는 State가 정한다
	return true;
}

// ---------- action_space.gd ----------

void enumerate_actions(const State &s, std::vector<Action> &out) {
	out.clear();
	if (s.phase == PLACEMENT) { // 배치 (spec 3.2 v5.12): 고를 수 있는 슬롯 × **자기 진영**의 빈 타일
		int team = placement_team(s);
		if (team < 0)
			return;
		int x0 = team == HOME ? 0 : W / 2; // 열거 순서 = 슬롯 → x 오름차순 → z 오름차순
		for (int slot = 0; slot < POOL_N; slot++) {
			if (!slot_open(s, team, slot))   // 처음 = 벤치(드래프트), 재배치 = 내 남은 선수의 포지션
				continue;
			for (int x = x0; x < x0 + W / 2; x++)
				for (int z = 0; z < H; z++) {
					V2 t{ x, z };
					if (place_tile_ok(s, team, t, DRAFT_POOL[slot] == NOMAD))   // 박스·골대 칸 × — 노마드는 박스 ○ (game_state.gd place_tile_ok)
						out.push_back({ A_PLACE, slot, t });
				}
		}
		return;
	}
	if (s.phase != PLAY)
		return;
	int occ[NT];
	build_occ(s, occ);
	for (int u = 0; u < PLAYERS; u++) {
		const Player &q = s.players[u];
		if (q.team != s.turn_team || !q.is_placed())
			continue;
		if (can_act(s, q) && may_dribble(s, q)) { // legal_dribbles
			bool blocked[NT];
			dribble_blocked(s, q, blocked);
			Reach r;
			reach(s, q, dribble_range(s, q), dribble_dirs(s, q), true, blocked, UNSET, r);
			int zd[NT];
			build_zone(s, opponent(q.team), zd);
			int zq[NT];
			build_zone(s, opponent(q.team), zq);
			int cap = contest_room(q, STA_ACT);
			int dr = dribble_range(s, q);
			Dirs dd = dribble_dirs(s, q);
			for (int i = 0; i < r.n; i++)
				if (occ[idx(r.order[i])] == -1 && !is_any_box(r.order[i]) && path_contests_z(s, q, canonical_path(q, r.order[i], dr,
						dd, zd, blocked, UNSET), true, zq) <= cap)   // v5.79: do_dribble_to가 걸을 길의 경합 수
					out.push_back({ A_DRIBBLE, u, r.order[i] });
		}
		// 킥오프 턴은 드리블·패스만 (spec 3.2 v5.14) — action_space.gd와 같은 골라내기
		bool kickoff = kickoff_turn(s);
		if (!kickoff) {
			if (can_move(s, q)) { // legal_moves
				bool blocked[NT];
				move_blocked(s, q, blocked);
				Reach r;
				reach(s, q, move_rng(s, q), step_dirs(q.team, M_ALL), true, blocked, UNSET, r);
				bool sc = carry_case(s);
				int mr = move_rng(s, q);
				std::vector<V2> sp[NT];
				if (sc)
					carry_paths(s, q, sp);   // 루즈볼을 주운 뒤로는 적을 못 지나고 박스에 못 멈춘다 (줍기 전은 보통 이동 차단)
				for (int i = 0; i < r.n; i++)
					if (move_dest_ok(s, r.order[i], q) && (!sc || !sp[idx(r.order[i])].empty())
							&& (!s.loose_ball() || carry_contests(s, q, sc ? sp[idx(r.order[i])] : canonical_path(q, r.order[i], mr, step_dirs(q.team, M_ALL), nullptr, blocked, UNSET)) <= contest_room(q, STA_ACT)))   // v5.86
						out.push_back({ A_MOVE, u, r.order[i] });
			}
			int tt = legal_tackle_target(s, q);
			if (tt != -1)
				out.push_back({ A_TACKLE, u, { tt, 0 } });
			int pt[4];
			int pn = legal_pushes(s, q, pt);   // v5.41 제자리 밀기 — 태클 바로 뒤 (action_space.gd와 같은 자리)
			for (int i = 0; i < pn; i++)
				out.push_back({ A_PUSH, u, { pt[i], 0 } });
		}
		const Player *ba = ball_actor(s, u, true);
		if (ba) { // legal_passes — ctx.dist 삽입 순서대로. 아군 칸 · 빈 칸(루즈볼, 골대 칸 제외)
			PassCtx pc;
			pass_ctx(s, q, pc);
			bool space = q.space_pass();
			int line = offside_line(s, q.team);
			int zp[NT];
			build_zone(s, opponent(q.team), zp);
			int zpq[NT];
			build_zone(s, opponent(q.team), zpq);
			int pcap = contest_room(q, STA_ACT);
			// 빈 칸 A_PASS = 루즈볼 놓기. 아군 칸은 오프사이드면 빠진다 (v5.38 — 빈 칸은 안 걸린다)
			for (int i = 0; i < pc.dist.n; i++) {
				V2 t = pc.dist.order[i];
				int o = occ[idx(t)];
				bool ok = !is_offside(q, t, line) && ((o != -1 && s.players[o].team == q.team && o != u)   // v5.70: 오프사이드 지역은 빈 칸도 ×
						|| (o == -1 && !is_goal(t) && space));
				if (!ok)
					continue;
				{   // v5.79: do_pass_to가 보낼 길(공중볼은 떨어지는 칸)의 경합이 이번 턴 남은 몫 안이어야 한다
					std::vector<V2> pp;
					if (manhattan(q.pos, t) >= LONG_PASS_MIN)
						pp.push_back(t);
					else
						pp = canonical_path(q, t, pc.rng, pc.dirs, zp, pc.blocked, UNSET);
					if (path_contests_z(s, q, pp, false, zpq) > pcap)
						continue;
				}
				out.push_back({ A_PASS, u, t });
			}
			if (!kickoff && sta_ok(s, q)) {   // 슛 체력 1, 부스터 곁 0 (v7.0)
				ShotCtx ctx; // legal_shots (킥오프 턴은 슛 불가 — spec 3.2 v5.14)
				shot_ctx(s, q, ctx);
				if (ctx.dist.has(ctx.goal)) {
					int zs[NT];
					build_zone(s, opponent(q.team), zs);
					std::vector<V2> sp2 = canonical_path(q, ctx.goal, ctx.rng, ctx.dirs, zs, ctx.blocked, ctx.goal);
					if (!sp2.empty() && path_contests(s, q, sp2, false) <= contest_room(q, STA_ACT))   // v5.86
						out.push_back({ A_SHOT, u, ctx.goal });
				}
			}
		}
		if (kickoff)
			continue; // 수비도 킥오프 턴에는 닫힌다
		if (can_stance(s, q)) // 수비는 선수 블록의 맨 끝 — action_space.gd와 같은 자리
			out.push_back({ A_STANCE, u, { 0, 0 } });
	}
	if (can_end_turn(s))   // 턴 종료는 맨 끝 (action_space.gd)
		out.push_back({ A_END, -1, { 0, 0 } });
}

bool apply_action(State &s, const Action &a) {
	switch (a.t) {
		case A_MOVE:
			return do_move_to(s, a.uid, a.dest);
		case A_DRIBBLE:
			return do_dribble_to(s, a.uid, a.dest);
		case A_TACKLE:
			return do_tackle(s, a.uid, a.dest.x);
		case A_PLACE:
			return do_place(s, a.uid, a.dest); // uid 칸 = 드래프트 슬롯 (spec 3.2 v5.11)
		case A_PASS:
			return do_pass_to(s, a.uid, a.dest);
		case A_SHOT:
			return do_shot_auto(s, a.uid);
		case A_STANCE:
			return do_stance(s, a.uid);
		case A_PUSH:
			return do_push(s, a.uid, a.dest.x);
		case A_END:
			return do_end_turn(s);
	}
	return false;
}

// ---------- eval.gd ----------

// 슛각 판정 — **ShotCtx를 호출자가 만들어 넘긴다.**
// 예전에는 shot_distance()와 shot_is_clean()이 각각 shot_ctx(슛 BFS)를 독립적으로 돌려서
// 미니맥스 리프마다 **정확히 두 번** 같은 계산을 했다. 그 사이 판은 전혀 안 바뀌는데도.
// shot_distance는 이 정리로 호출처가 사라져 삭제했다
static bool shot_is_clean(const State &s, const Player &p, const ShotCtx &ctx) {
	int zone[NT];
	build_zone(s, opponent(p.team), zone);   // v5.30: 태세·월만 4칸
	std::vector<V2> path = canonical_path(p, ctx.goal, ctx.rng, ctx.dirs, zone, ctx.blocked, ctx.goal);
	if (path.empty())
		return false;
	for (const V2 &t : path)
		if (zone[idx(t)] != -1)
			return false;
	return true;
}

void features(const State &s, int team, double f[6]) {
	for (int i = 0; i < 6; i++)
		f[i] = 0.0;
	int own_team = -1; // f[4] = 공 임자 배터리 (v5.94 — eval.gd NAMES)
	V2 ball_pos{ -1, -1 };
	if (s.ball_owner_uid != -1) {
		own_team = s.players[s.ball_owner_uid].team;
		ball_pos = s.players[s.ball_owner_uid].pos;
	} else if (s.loose_ball()) {
		own_team = s.loose_team; // 루즈볼도 규칙상 놓은 팀 공이다 (spec 5.4 v5.15)
		ball_pos = s.ball_pos;
	}
	if (own_team == -1)
		return;
	double sg = own_team == team ? 1.0 : -1.0;
	// 루즈볼도 온전히 우리 공이다 (v5.15 사용자 확정 — 넘어가는 것은 상대가 주운 순간부터)
	f[0] = sg;
	V2 goal = opponent_goal(own_team);
	int d = manhattan(goal, ball_pos);
	double maxd = double(W + H);
	f[1] = sg * (maxd - double(d)) / maxd;
	if (s.ball_owner_uid != -1) {
		const Player &owner = s.players[s.ball_owner_uid];
		ShotCtx ctx;
		shot_ctx(s, owner, ctx);          // 리프당 한 번만 — 두 판정이 같이 쓴다
		if (ctx.dist.has(ctx.goal)) {
			if (shot_is_clean(s, owner, ctx))
				f[2] = sg;
			else
				f[3] = sg;
		}
		f[4] = sg * double(owner.sta) / double(STA_MAX);   // v5.94 공 임자 배터리
	}
	int spread = 0, pairs = 0; // 산개도: 공을 가진 팀의 선수 간 평균 거리
	for (int i = 0; i < PLAYERS; i++) {
		const Player &a = s.players[i];
		if (a.team != own_team || !a.is_placed())
			continue;
		for (int j = i + 1; j < PLAYERS; j++) {
			const Player &b = s.players[j];
			if (b.team != own_team || !b.is_placed())
				continue;
			spread += manhattan(a.pos, b.pos);
			pairs += 1;
		}
	}
	if (pairs > 0)
		f[5] = sg * double(spread) / double(pairs) / maxd;
}

constexpr double W_GOAL = 10000.0;
constexpr double W_SCORE = 100.0; // 골 1점 차 (v5.38 — eval.gd W_SCORE와 같아야 한다)
constexpr double W_FAST = 1.0;    // 골 빠르기 (v5.39 — eval.gd W_FAST)

static double goal_value(const State &s, int team) { // eval.gd _goal_value
	int g = s.goal_turn[team];
	return W_SCORE * double(s.score[team]) + (g > 0 ? W_FAST * double(HALF_TURNS + 1 - g) : 0.0);
}

// 배치 평가 — eval.gd의 PLACE_INNER·PLACE_VALUE와 같은 값이어야 한다 (test_native_parity가 지킨다).
// 실측 근거는 eval.gd 주석 참조 (깊이2 미러전)
static double place_inner_pref(int pos) {
	switch (pos) {
	case TRICKSTER: return 6.4;
	// ※ 월의 5.4는 **v5.20 이전(태클 4 + 인터셉트 4) 실측**이라 낡았다 — 재측정 대기.
	// 고칠 때는 eval.gd의 PLACE_INNER도 반드시 같이 고친다 (동일성 검증은 두 값이 서로
	// 같은지만 보므로, 나란히 틀린 채로도 통과한다)
	case WALL: return 5.4;
	case CANNON: return -4.8;
	default: return 0.0;
	}
}

// 드래프트 가치 (v5.15 재측정 시드2개 × 6,000판 — eval.gd PLACE_VALUE와 같은 값이어야 한다).
// 두 시드가 부호까지 일치하고 평균이 2σ를 넘긴 것만 넣었다 — 근거는 eval.gd 주석
static double place_draft_value(int pos) {
	switch (pos) {
	case HUNTER: return 4.2;
	case TITAN: return 2.8;
	case MAESTRO: return -5.6;
	default: return 0.0; // 월·스프린터·캐논·트릭스터·마커 = 0 (v5.30: 크리에이터는 풀에서 빠졌다)
	}
}

static double placement_score(const State &s, int team) {
	double v = 0.0;
	for (const auto &p : s.players) {
		if (!p.is_placed())
			continue;
		double sc = place_draft_value(p.position);
		double pref = place_inner_pref(p.position);
		if (pref != 0.0)
			sc += pref * ((p.pos.y == 1 || p.pos.y == 5) ? 1.0 : -1.0);
		v += sc * (p.team == team ? 1.0 : -1.0);
	}
	return v * 0.01;
}

double evaluate(const State &s, int team, const double w[6]) {
	if (s.phase == GAME_OVER) {
		if (s.winner == -1)
			return 0.0; // 무승부 (v5.38)
		return s.winner == team ? W_GOAL : -W_GOAL;
	}
	double lead = goal_value(s, team) - goal_value(s, opponent(team));
	if (s.phase == PLACEMENT)
		return lead + placement_score(s, team);
	double f[6];
	features(s, team, f);
	double v = 0.0;
	for (int i = 0; i < 6; i++)
		v += f[i] * w[i];
	return lead + v;
}

// ---------- minimax_bot.gd ----------

// 탐색 순서 (Act 인덱스 → ORDER 값): 슛0 · 태클·밀기1 · 드리블2 · 패스3 · 이동4 · 수비·버티기·잡기·부스트5 · 배치7 · 턴 종료9
// — minimax_bot.gd ORDER와 동일. 좋은 수를 먼저 보면 알파베타 가지치기가 더 많이 잘라낸다.
// 배치가 뒤인 이유: 그 단계에서는 다른 액션이 아예 없어 순서가 의미 없다
static const int ORDER_OF[N_ACT] = { 4, 2, 1, 7, 3, 0, 5, 5, 5, 1, 9, 5, 5 };   // minimax_bot.gd ORDER — 해제 = 수비 칸
constexpr int N_BUCKET = 10;

static void ordered(std::vector<Action> &opts) {
	std::vector<Action> buckets[N_BUCKET];
	for (const Action &o : opts)
		buckets[ORDER_OF[o.t]].push_back(o);
	opts.clear();
	for (int i = 0; i < N_BUCKET; i++)
		for (const Action &o : buckets[i])
			opts.push_back(o);
}

static double search_(State &s, int d, double alpha, double beta, int root_team, const double w[6]) {
	if (s.phase == GAME_OVER) {
		if (s.winner == -1)
			return 0.0; // 무승부 (v5.38)
		double v = W_GOAL + double(d);
		return s.winner == root_team ? v : -v;
	}
	if (d == 0)
		return evaluate(s, root_team, w);
	std::vector<Action> opts;
	enumerate_actions(s, opts);
	ordered(opts);
	if (opts.empty()) {
		s.end_turn(); // 실전 하네스와 동일: 할 게 없으면 턴만 넘어간다
		return search_(s, d - 1, alpha, beta, root_team, w);
	}
	bool maximizing = s.turn_team == root_team;
	double INF = std::numeric_limits<double>::infinity();
	double best = maximizing ? -INF : INF;
	State backup = s;
	for (const Action &o : opts) {
		if (!apply_action(s, o)) {
			s = backup;
			continue;
		}
		double sc = search_(s, d - 1, alpha, beta, root_team, w);   // 공짜 수는 수읽기 한 칸을 안 먹는다 (minimax_bot.gd _search)
		s = backup;
		if (maximizing) {
			best = std::max(best, sc);
			alpha = std::max(alpha, sc);
		} else {
			best = std::min(best, sc);
			beta = std::min(beta, sc);
		}
		if (beta <= alpha)
			break;
	}
	if (std::isinf(best))
		return evaluate(s, root_team, w); // 모든 후보 실행 거부 방어 (원본과 동일)
	return best;
}

// ---------- 자가대전 공장 ----------

// game_state.gd setup — 설정(결장)은 보존하고 opener는 새 선공으로 바뀐다
static void reset_match(State &s, int first) {
	s.phase = PLACEMENT;
	s.first_team = first;
	s.opener = first;
	s.placement_index = 0;
	s.redeploy = 0;
	s.reset_ball();
	s.pts = 0;
	s.turn_actions = 0;
	s.acted_mask = 0;
	s.turn_count = 0;
	s.half = 1;
	s.score[0] = s.score[1] = 0;
	s.goal_turn[0] = s.goal_turn[1] = 0;
	s.winner = -1;
	s.contest_used = 0;
	s.boost_on = 0;
	s.grab[0] = s.grab[1] = -1;
	for (int u = 0; u < PLAYERS; u++) {
		s.players[u] = Player{};
		s.players[u].uid = u;
		s.players[u].team = u < PER_TEAM ? HOME : AWAY;
		s.players[u].position = -1; // 포지션은 드래프트로 정해진다 (v5.11)
	}
	s.turn_team = placement_team(s); // 배치 단계에도 '두는 팀'이 있다 (v5.9)
}

// 하네스용 결정론 난수 (xorshift64* — 게임 규칙과 무관, 배치·엔트리 셔플 전용)
static int rng_range(unsigned long long &st, int from, int to) {
	if (!st)
		st = 0x9E3779B97F4A7C15ull;
	st ^= st >> 12;
	st ^= st << 25;
	st ^= st >> 27;
	return from + int((st * 0x2545F4914F6CDD1Dull) % (unsigned long long)(to - from + 1));
}

void random_placement(State &s, unsigned long long &rng) {
	std::vector<Action> opts;
	while (s.phase == PLACEMENT) {
		enumerate_actions(s, opts);
		if (opts.empty())
			break;
		const Action &a = opts[rng_range(rng, 0, (int)opts.size() - 1)];
		if (!do_place(s, a.uid, a.dest))
			break;
	}
}

void new_open_match(State &s, int first) {
	reset_match(s, first);
}

void new_random_match(State &s, int first, unsigned long long seed) {
	reset_match(s, first);
	random_placement(s, seed); // 무작위 드래프트 (v5.11) — 엔트리 사전 배분이 없다
}

void selfplay(int games, int depth, unsigned long long seed, int cap, const double w[6],
		const char *record_path, SelfplayStats &out) {
	unsigned long long rng = seed;
	FILE *rec = nullptr;
	if (record_path && record_path[0]) {
		rec = fopen(record_path, "wb");
		if (rec) {
			int head[2] = { 0x43465350, 2 };
			fwrite(head, 4, 2, rec);
		}
	}
	std::vector<int> moves; // 이번 경기 기보: n_moves × (BUF_LEN + 4)
	for (int g = 0; g < games; g++) {
		State s;
		reset_match(s, g % 2 == 0 ? HOME : AWAY);
		moves.clear();
		int guard = 0;
		while (s.phase != GAME_OVER && guard < cap * 8) {
			guard += 1;
			if (s.phase == PLACEMENT) { // 처음 드래프트·골 뒤·하프타임 뒤 재배치 = 하네스 난수
				random_placement(s, rng);
				continue;
			}
			bool found = false;
			Action a = pick_action(s, depth, w, found);
			if (!found) {
				s.end_turn(); // 할 액션이 없으면 턴만 넘긴다 (하네스와 동일)
				continue;
			}
			if (rec) {
				int buf[BUF_LEN];
				s.save_buf(buf);
				moves.insert(moves.end(), buf, buf + BUF_LEN);
				int enc[4] = { a.t, a.uid, a.dest.x, a.dest.y };
				moves.insert(moves.end(), enc, enc + 4);
			}
			apply_action(s, a);
			out.actions[a.t] += 1;
		}
		out.matches += 1;
		out.turns.push_back(s.turn_count);
		out.goals += s.score[0] + s.score[1];
		if (s.phase != GAME_OVER || s.winner == -1) {
			out.draws += 1;
		} else {
			(s.winner == HOME ? out.home_wins : out.away_wins) += 1;
			(s.winner == s.opener ? out.first_wins : out.second_wins) += 1;
		}
		if (rec) {
			int head[5] = { s.opener, s.phase == GAME_OVER ? s.winner : -1, s.score[0], s.score[1],
				int(moves.size()) / (BUF_LEN + 4) };
			fwrite(head, 4, 5, rec);
			if (!moves.empty())
				fwrite(moves.data(), 4, moves.size(), rec);
		}
	}
	if (rec)
		fclose(rec);
}

Action pick_action(const State &s0, int depth, const double w[6], bool &found, double *best_out) {
	found = false;
	Action chosen;
	State s = s0;
	std::vector<Action> opts;
	enumerate_actions(s, opts);
	ordered(opts);
	if (opts.empty())
		return chosen;
	int team = s.turn_team;
	double best = -std::numeric_limits<double>::infinity();
	State backup = s;
	for (const Action &o : opts) {
		if (!apply_action(s, o)) {
			s = backup;
			continue;
		}
		double sc = search_(s, depth - 1, best, std::numeric_limits<double>::infinity(), team, w);
		s = backup;
		if (sc > best) {
			best = sc;
			chosen = o;
			found = true;
		}
	}
	if (best_out)
		*best_out = best;
	return chosen;
}

} // namespace cf
