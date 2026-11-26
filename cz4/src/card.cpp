// 감독·선수 모드 규칙 엔진 — card.h 설명
// 구성: 상태·도우미 → 선수 수치 → 배치 → 시간 → 존·경합 → 길 → 액션 → 열거 → 손평가
#include "card.h"

#include <cstring>

#include <algorithm>
#include <cstdlib>

namespace cf {
namespace card {

std::vector<Event> *g_events = nullptr;
const std::vector<V2> *g_path = nullptr;

static const int DRAFT_POOL[CARD_POOL_N] = { CANNON, MAESTRO, TRICKSTER, SPRINTER, MARKER, TITAN, HUNTER, WALL, STRIKER, PLAYMAKER, ENGINE };   // 칸 번호 = 망 입력 자리 (엔진 = 맨 뒤 — 11-25)

int pool_slot(int position) {
	for (int k = 0; k < CARD_POOL_N; k++)
		if (DRAFT_POOL[k] == position)
			return k;
	return -1;
}

static thread_local unsigned g_met = 0;   // 이 수에서 이미 겨룬 선수(uid 비트) — 존에 놓인 공 경합이 같은 수에서 두 번 겨루지 않게

static void emit(const Event &e) {
	if (e.kind == 0 && e.a >= 0 && e.b >= 0)
		g_met |= (1u << e.a) | (1u << e.b);
	if (g_events)
		g_events->push_back(e);
}

// ---------- 전직 혜택 (모두 상시 · 11-24 규칙 묶음 4) ----------
// 캐논 슛 거리 감쇠 없음 + 슛 범위 +1 · 포처 슛 관통(존을 이겨도 힘 안 깎임) + 슛 힘 +1 · 마에스트로 패스 거리 감쇠 없음 + 패스 범위 +1 ·
// 매지션 패스 관통(존을 이겨도 힘 안 깎임) + 패스 힘 +1 · 스프린터 무브 거리 감쇠 없음 + 무브 범위 +1 · 트릭스터 드리블 관통 + 무브 힘 +1 ·
// 엔진 체력 +1 · 경기 사이 회복 +2 · 헌터 디펜스 +1 · 월 상시 태세(공을 쥔 동안만 없음 — wall_sync) · 마커 피지컬 +1 · 붙은 적 범위 −1(피지컬이 클 때만) · 타이탄 피지컬 +1 · 밀기(피지컬이 클 때만)
// 성장(스탯마다 +1 한 번) = 그 스탯 힘 +1 (범위는 안 늘림 — 사용자 11-24 넷째 정정) · 예외 = 멀리형 자기 분야: 캐논 슛·마에스트로 패스·스프린터 무브 = 범위만 +1
static int field_of(int pos) {   // 그 직업의 분야 스탯 (−1 = 없음)
	switch (pos) {
		case CANNON: case STRIKER: return ST_SHOT;
		case MAESTRO: case PLAYMAKER: return ST_PASS;
		case SPRINTER: case TRICKSTER: return ST_DRIBBLE;
		default: return -1;
	}
}
static bool reach_job(int pos) { return pos == CANNON || pos == MAESTRO || pos == SPRINTER; }   // 거리 감쇠 없음 · 범위 +1 · 성장 범위
static int grow_pow(const Player &p, int st) { return field_of(p.position) == st && reach_job(p.position) ? 0 : p.bonus[st]; }
static int grow_rng(const Player &p, int st) { return field_of(p.position) == st && reach_job(p.position) ? p.bonus[st] : 0; }
static bool pierce(const Player &p, int use) { return p.position == (use == U_DRIBBLE ? TRICKSTER : use == U_PASS ? PLAYMAKER : STRIKER); }   // 관통: 존을 이겨도 힘이 안 깎임 — 트릭스터 드리블 · 매지션 패스 · 포처 슛

static void rep_add(State &s, int uid, int job) {
	if (uid < 0 || uid >= PLAYERS)
		return;
	const int j = pool_slot(job);
	if (j >= 0 && s.rep[uid][j] < 255)
		s.rep[uid][j] += 1;
}

// ---------- 상태 도우미 ----------

int State::ball_team() const {
	if (keeper >= 0)
		return keeper;   // 키퍼가 쥔 공
	if (ball_owner != -1)
		return players[ball_owner].team;
	return opponent(first_team);   // 킥오프 전 = 후공 팀이 공격 측
}

bool State::gk_in(int team) const { return gk[team] < 0 || is_goal_of(team, players[gk[team]].pos); }

static bool is_gk(const State &s, const Player &p) { return p.uid >= 0 && s.gk[p.team] == p.uid; }
static bool in_goal(const State &s, const Player &p) { return is_gk(s, p) && is_goal_of(p.team, p.pos); }

static const Player *player_at(const State &s, V2 t) {
	for (const Player &q : s.players)
		if (q.placed() && q.pos == t)
			return &q;
	return nullptr;
}

int move_stat(const Player &p) { return std::max(1, BASE_MOVE + grow_pow(p, ST_DRIBBLE) + (p.position == TRICKSTER)); }

// ---------- 피지컬 · 체력 (11-25 규칙 묶음 6) ----------
int phys(const Player &p) { return BASE_PHYS + p.bonus[ST_PHYS] + (p.position == TITAN || p.position == MARKER); }
int stam_max(const Player &p) { return BASE_STAM + p.bonus[ST_STAM] + (p.position == ENGINE); }
int stam(const Player &p) { return std::max(0, stam_max(p) - p.fat); }
int rest_gain(int position) { return position == ENGINE ? 2 : 1; }

void set_stamina(State &s, int team, const int st[PER_TEAM]) {
	for (int i = 0; i < PER_TEAM; i++) {
		Player &p = s.players[team * PER_TEAM + i];
		const int mx = stam_max(p);
		p.fat = mx - std::max(0, std::min(mx, st[i]));
	}
}

// 체력 쓰기 — 행동마다 1 (「강하게」 모으기도 1 · 행동과 합쳐 2) · 쓴 선수는 그 차례 끝에 회복 없음
static bool spent_out(const Player &p) { return MATCH_STAMINA && stam(p) < 1; }   // 체력 0 = 행동 불가 (경기 안 체력이 켜졌을 때만)

static void tire(State &s, int uid) {
	if (!MATCH_STAMINA || uid < 0 || uid >= PLAYERS)
		return;
	s.players[uid].fat += 1;
	s.players[uid].spent = 1;
}

// 차례 끝 회복: 그 차례에 체력을 안 쓴 선수 +REST_TURN (엔진도 같음 — 엔진 +2 는 경기 사이만) · 최대까지
static void rest(State &s, int team) {
	if (!MATCH_STAMINA)
		return;
	for (Player &p : s.players)
		if (p.team == team && p.position >= 0 && !p.spent)
			p.fat = std::max(0, p.fat - REST_TURN);
}

static void set_owner(State &s, int uid) {
	if (s.keeper >= 0 && uid != s.gk[s.keeper])
		s.keeper = -1;   // 키퍼가 공을 놓았다 (11-26 묶음 7)
	s.ball_owner = uid;
	s.hold = 0;
	if (uid != -1) {
		Player &p = s.players[uid];
		s.hold = act_power(s, p, U_DRIBBLE);   // 안전한 기본값 (부르는 쪽이 그 행동의 남은 힘으로 고쳐 쓴다 — 패스·드리블·뺏음)
		if (s.touch_team != p.team) {
			s.touch_team = p.team;
			s.touch_mask = 0;
			s.poss_turn = s.turn_count;
		}
		p.stance = 0;   // 공을 잡으면 태세가 풀린다
		s.touch_mask |= 1 << uid;
	}
}

// 경합(태클·존)으로 공을 뺏었다 — 대표 행동 기록 · 버티는 힘 = 뺏을 때 쓴 경합 힘 (set_owner 바로 뒤에 부른다 · 11-26 묶음 7)
static void mark_steal(State &s, int val) {
	rep_add(s, s.ball_owner, HUNTER);
	s.hold = val;
}

bool kickoff_turn(const State &s) {
	return s.phase == PLAY && s.keeper < 0 && s.ball_owner == -1 && s.turn_team == opponent(s.first_team);
}

// 공 가진 팀 = 공격
static void sync_roles(State &s) {
	const int bt = s.ball_team();
	for (int t = 0; t < 2; t++)
		s.role[t] = t == bt ? R_ATK : R_DEF;
}

bool marked(const State &s, const Player &p) {   // 상대 마커에 상하좌우로 붙었다 — 범위 −1 (힘 그대로 · 겹치지 않음 · 공 유무와 상관없이) · 마커 피지컬 > 내 피지컬일 때만 (11-25)
	if (!p.placed())
		return false;
	for (const Player &m : s.players)
		if (m.team != p.team && m.position == MARKER && m.placed() && m.stance == 0 && m.tai == 0 && s.ball_owner != m.uid && manhattan(m.pos, p.pos) == 1 && phys(m) > phys(p))   // 공을 쥐었거나 태세 중인 마커는 못 잡는다 (11-26 묶음 7)
			return true;
	return false;
}

// ---------- 선수 수치 ----------

// 판정 힘 — 늘 1 이상. 킥·드리블: 힘 = 수치 + 1 − 거리 (거리 0 = 공을 잡고 선 힘 = 수치) ·
// 거리 감쇠 없음(캐논 슛·마에스트로 패스·스프린터 드리블) = 늘 수치
int act_power(const State &s, const Player &p, int use, int dist) {
	(void)s;
	const bool kick = use == U_PASS || use == U_SHOT;
	int v, st;
	if (kick) {
		st = use == U_SHOT ? ST_SHOT : ST_PASS;
		v = BASE_KICK + grow_pow(p, st) + (use == U_SHOT ? p.position == STRIKER : p.position == PLAYMAKER);
	} else if (use == U_STANCE)
		st = ST_DEF, v = BASE_DEF + p.bonus[ST_DEF] + (p.position == HUNTER);
	else
		st = ST_DRIBBLE, v = move_stat(p);
	if (kick || use == U_DRIBBLE) {
		const int d = std::max(1, dist);
		if (field_of(p.position) == st && reach_job(p.position))
			;   // 거리 감쇠 없음
		else
			v += 1 - d;
	}
	if (p.tai == 1 && (kick || use == U_DRIBBLE))
		v += 1;   // 태세 = 다음 행동 힘 +1 (패스·슛·드리블 — 이동은 범위·다가가 태클 힘 · 디펜스 존은 안 늘림)
	return std::max(1, v);
}

bool grow_ok(int pos, const int st[GROW_K], int k) {
	int n = 0;
	for (int i = 0; i < GROW_K; i++)
		n += st[i] > 0;
	return k >= 0 && k < GROW_K && pos != PROSPECT && st[k] < GROW_MAX && n < GROW_PICKS;   // 스탯 여섯 중 넷 각 +1 한 번 (11-26 묶음 7)
}

// 범위 = 기본 + 성장(멀리형 자기 분야만 — grow_rng) + 전직 범위 +1(캐논 슛·마에스트로 패스·스프린터 무브) · 태세 +1 · 마커 곁 −1
int act_range(const State &s, const Player &p, int want) {
	const int st = want == C_SHOT ? ST_SHOT : want == C_KICK ? ST_PASS : ST_DRIBBLE;
	int r = (want == C_MOVE ? MOVE_RANGE : KICK_RANGE) + grow_rng(p, st) + (field_of(p.position) == st && reach_job(p.position));
	if (p.tai == 1)
		r += 1;
	if (marked(s, p))
		r -= 1;
	return std::max(1, r);
}

int assist_value(const State &s, int team) {
	return s.touch_team == team ? std::min(s.team_n, __builtin_popcount(unsigned(s.touch_mask))) : 0;
}

// ---------- 배치 ----------

static int team_at(const State &s, int i) {
	int cnt[2] = { 0, 0 };
	int who = 0;
	for (int k = 0; k <= i; k++) {
		who = (k / 2) % 2;
		if (cnt[who] >= s.team_n)
			who = 1 - who;
		cnt[who] += 1;
	}
	return who == 0 ? s.first_team : opponent(s.first_team);
}

static int placement_team(const State &s) {
	if (s.phase != PLACEMENT || s.placement_index >= 2 * s.team_n)
		return -1;
	return team_at(s, s.placement_index);
}

static bool is_drafted(const State &s, int position, int team) {   // 드래프트는 팀 안에서만 겹치지 않는다
	for (const Player &q : s.players)
		if (q.team == team && q.position == position)
			return true;
	return false;
}

static int next_free_uid(const State &s, int team) {
	for (const Player &q : s.players)
		if (q.team == team && !q.placed() && q.position < 0)
			return q.uid;
	return -1;
}

static int unplaced_with(const State &s, int team, int position) {
	for (const Player &q : s.players)
		if (q.team == team && q.position == position && !q.placed())
			return q.uid;
	return -1;
}

static int slot_pos(int slot) { return slot == SLOT_PROSPECT ? PROSPECT : DRAFT_POOL[slot]; }
static int unplaced_extra(const State &s, int team) { return unplaced_with(s, team, PROSPECT); }   // 유망주 칸 = 드래프트 풀 밖 선수(신입생)

static bool slot_open(const State &s, int team, int slot) {
	if (slot == SLOT_PROSPECT)
		return s.redeploy && unplaced_extra(s, team) != -1;
	return s.redeploy ? unplaced_with(s, team, DRAFT_POOL[slot]) != -1 : !is_drafted(s, DRAFT_POOL[slot], team);
}

static bool place_tile_ok(const State &s, int team, V2 t) {
	return is_own_half(team, t) && !player_at(s, t) && !is_goal(t) && !is_any_box(t);
}

// 월 = 상시 태세 (규칙 묶음 5): 공을 쥔 동안만 빼고 늘 제자리 사방 존 — 힘 = 디펜스(「강하게」 빼고). 경합에서 져도 이겨도·움직여도
// 수 하나가 끝나면 다시 선다(그 수 안에서는 보통 태세처럼 풀린다 — 한 길에서 한 번만 겨룸). 골대 안 키퍼·박스 칸 ×
static void wall_sync(State &s) {
	if (s.phase != PLAY)
		return;
	for (Player &p : s.players) {
		if (p.position != WALL || !p.placed())
			continue;
		const bool on = s.ball_owner != p.uid && !is_any_box(p.pos) && !in_goal(s, p);
		p.stance = on ? act_power(s, p, U_STANCE) : 0;
		p.stance_left = on;
	}
}

static void turn_start(State &s, int team) {   // 우리 차례 시작: 우리 태세가 풀린다 (존·버티기) · 포인트 2
	for (Player &p : s.players) {
		p.acted = 0;   // 행동 표시는 그 차례 안에서만
		if (p.team != team)
			continue;
		p.tai = 0;
		if (p.stance > 0 && --p.stance_left <= 0)
			p.stance = 0, p.stance_left = 0;
	}
	for (Player &p : s.players)
		p.spent = 0;
	s.pts = POINTS_PER_TURN;
	s.tturn[team] += 1;
	wall_sync(s);
}

static void commit_place(State &s, int uid, int position, V2 t) {
	Player &p = s.players[uid];
	p.position = position;
	p.pos = t;
	p.placed_order = s.placement_index;
	s.placement_index += 1;
	if (s.placement_index >= 2 * s.team_n) {
		s.phase = PLAY;
		s.turn_team = s.first_team;
		for (const Player &q : s.players) {   // 경기 시작 대형 — 골 뒤에 그대로 돌아온다
			s.half_pos[q.uid] = q.pos;
			s.half_order[q.uid] = q.placed_order;
		}
		turn_start(s, s.turn_team);
	} else {
		s.turn_team = placement_team(s);
	}
}

static bool do_place(State &s, int slot, V2 t) {
	if (s.phase != PLACEMENT || slot < 0 || slot > SLOT_PROSPECT)
		return false;
	int team = placement_team(s);
	if (team < 0 || !slot_open(s, team, slot) || !place_tile_ok(s, team, t))
		return false;
	int uid = s.redeploy ? (slot == SLOT_PROSPECT ? unplaced_extra(s, team) : unplaced_with(s, team, slot_pos(slot))) : next_free_uid(s, team);
	if (uid < 0)
		return false;
	commit_place(s, uid, s.redeploy ? s.players[uid].position : slot_pos(slot), t);
	return true;
}

static void refresh_if_stuck(State &s);

// 골 뒤: 두 팀 모두 경기 시작 대형 · 공·가담·태세를 비우고 실점한 팀이 공을 갖고 먼저 킥오프
static void restart_half(State &s, int scorer) {
	for (Player &p : s.players) {
		p.pos = s.half_pos[p.uid];
		p.placed_order = s.half_order[p.uid];
		p.stance = p.stance_left = p.tai = p.acted = 0;
	}
	s.ball_owner = -1;
	s.keeper = -1;
	s.hold = 0;
	s.touch_team = -1;
	s.touch_mask = 0;
	s.poss_turn = s.turn_count;
	for (int t = 0; t < 2; t++)
		s.ghp[t] = s.gk_def[t];   // 킥오프마다 골대 체력 다시 참
	s.first_team = scorer;   // first_team = 공 없는 쪽 (kickoff_turn 은 그 상대 차례)
	s.role[scorer] = R_DEF;
	s.role[opponent(scorer)] = R_ATK;
	s.turn_team = opponent(scorer);
	turn_start(s, s.turn_team);
	refresh_if_stuck(s);
}

// 골키퍼 = 판 위 선수: 골대 대표 칸(goal_tile k0 — 두 칸을 한 칸처럼)에 놓고 스탯(디펜스 = 골대 체력 · 패스 = 롱킥)을 그 선수에서 잰다
static void place_gk(State &s, int team, int uid) {
	s.gk[team] = uid;
	if (uid < 0) {
		s.gk_def[team] = GK_DEF0, s.gk_kick[team] = GK_KICK0, s.gk_range[team] = GK_RANGE0;
	} else {
		Player &p = s.players[uid];
		p.pos = goal_tile(team, 0);
		p.placed_order = 2 * FIELD_N + team;   // 필드 선수 배치 순서 뒤 (존 겹침은 필드 선수 먼저)
		int ks[3];
		keeper_stats(p.position, p.bonus, ks);
		s.gk_def[team] = ks[0], s.gk_kick[team] = ks[1], s.gk_range[team] = ks[2];
	}
	if (s.phase != PLAY)
		s.ghp[team] = s.gk_def[team];
}

void new_match(State &s, int first) {
	s = State();
	for (int u = 0; u < PLAYERS; u++) {
		s.players[u].uid = u;
		s.players[u].team = u < PER_TEAM ? HOME : AWAY;
	}
	s.first_team = first;
	s.opener = first;
	for (int t = 0; t < 2; t++) {   // 드래프트 판의 골키퍼 = 신입생 (set_lineup 이 정한 선수단으로 바꾼다)
		s.players[t * PER_TEAM + FIELD_N].position = PROSPECT;
		place_gk(s, t, t * PER_TEAM + FIELD_N);
	}
	s.role[first] = R_DEF;
	s.role[opponent(first)] = R_ATK;
	s.turn_team = placement_team(s);
}

void keeper_stats(int position, const int bonus[GROW_K], int out[3]) {
	State t;   // 판 밖 선수 — 마커·태세 없이 그 선수 맨 값
	Player p;
	p.position = position;
	for (int k = 0; k < GROW_K; k++)
		p.bonus[k] = bonus[k];
	out[0] = act_power(t, p, U_STANCE);
	out[1] = act_power(t, p, U_PASS, 1);   // 1칸 힘 = 능력치 (롱킥은 do_keep 이 거리로 깎는다)
	out[2] = act_range(t, p, C_KICK);
}

int default_gk(const int position[PER_TEAM], const int bonus[PER_TEAM][GROW_K]) {
	int best = -1, gk = PER_TEAM - 1, ks[3];
	for (int i = 0; i < PER_TEAM; i++) {
		keeper_stats(position[i], bonus[i], ks);
		if (ks[0] >= best)
			best = ks[0], gk = i;
	}
	return gk;
}

void set_lineup(State &s, int team, const int position[PER_TEAM], const int bonus[PER_TEAM][6], int gk) {
	if (gk < 0 || gk >= PER_TEAM)
		gk = default_gk(position, bonus);
	for (int i = 0, field = 0; i < PER_TEAM; i++) {
		Player &p = s.players[team * PER_TEAM + i];
		const bool keeper = i == gk;
		p.position = keeper || field++ < s.team_n ? position[i] : -1;   // 출전 인원 밖(후보)은 판에 안 선다
		for (int k = 0; k < GROW_K; k++)
			p.bonus[k] = bonus[i][k];
		p.fat = 0;   // 체력 가득 (경기 시작 체력은 set_stamina)
		p.pos = UNPLACED;
		p.placed_order = -1;
	}
	place_gk(s, team, team * PER_TEAM + gk);
	s.redeploy = 1;   // 놓기만 — 포지션은 정해졌다
}

void set_trial(State &s, int team_n) {
	s.team_n = std::max(1, std::min(FIELD_N, team_n));
	s.turn_team = placement_team(s);
}

// ---------- 시간 ----------

// 승패: 골이 많은 팀 → 같으면 무승부 (녹아웃은 비긴 채 끝나지 않는다 — 연장 골든골)
static void full_time(State &s) {
	s.phase = GAME_OVER;
	s.winner = s.goals[0] != s.goals[1] ? (s.goals[0] > s.goals[1] ? HOME : AWAY) : -1;
}

// 한 판 = 팀당 30차례 · 먼저 3골이면 바로 끝. 녹아웃이 정규 끝에 비기면 연장 — 같은 판을 차례 제한 없이 이어 먼저 넣은 팀 승 (골든골)
static bool clock(State &s) {
	const bool tied = s.goals[HOME] == s.goals[AWAY];
	if (s.goals[HOME] < WIN_GOALS && s.goals[AWAY] < WIN_GOALS && (s.turn_count < CARD_HALF_TURNS || (s.knockout && tied))) {
		if (s.turn_count >= CARD_HALF_TURNS)
			s.ot = 1;   // 비김 · 녹아웃 = 연장 (킥오프 없이 그 자리 그대로 이어 둔다)
		return false;
	}
	full_time(s);
	return true;
}

void end_turn(State &s) {
	if (s.phase != PLAY)
		return;
	for (const Player &p : s.players)   // 월 대표 행동 — 우리 차례 둘을 이어 태세로 끝냄 (기록 · 상시 태세인 월은 빼고)
		if (p.team == s.turn_team && p.stance > 0 && p.position != WALL) {
			if (s.st_turn[p.uid] > 0 && s.st_turn[p.uid] == s.tturn[p.team] - 1)
				rep_add(s, p.uid, WALL);
			s.st_turn[p.uid] = s.tturn[p.team];
		}
	for (Player &p : s.players)   // 안 쓴 태세 = 상대 차례 동안 선다: 공 임자 = 버티기(피지컬) · 공 없음 = 사방 존(디펜스)
		if (p.team == s.turn_team && p.tai == 1) {
			p.tai = 2;
			if (s.ball_owner == p.uid)
				s.hold = phys(p);
			else if (p.placed() && !is_any_box(p.pos))
				p.stance = std::max(p.stance, act_power(s, p, U_STANCE)), p.stance_left = 1;
		}
	rest(s, s.turn_team);
	s.turn_count += 1;
	if (clock(s))
		return;
	s.turn_team = opponent(s.turn_team);
	turn_start(s, s.turn_team);
	refresh_if_stuck(s);
}

// 둘 수가 하나라도 있나 — 거의 늘 있다(빈 곁 칸으로 한 걸음). 빠른 길 먼저 보고, 애매하면 수 목록을 다 만든다
static bool any_action(const State &s) {
	const bool ko = kickoff_turn(s);
	for (const Player &p : s.players) {
		if (p.team != s.turn_team || !p.placed() || ko || spent_out(p))
			continue;
		for (const V2 &d : DIRS) {
			const V2 t = p.pos + d;
			if (in_bounds(t) && !player_at(s, t) && !is_goal(t) && !is_any_box(t))
				return true;
		}
	}
	std::vector<Action> v;
	enumerate_actions(s, v);
	return !v.empty();
}

// 둘 수가 없으면 그 차례는 저절로 끝 (차례 끝 수는 없다)
static void refresh_if_stuck(State &s) {
	if (s.phase != PLAY || s.pts < 1 || any_action(s))
		return;
	end_turn(s);
}

static void zone_ball(State &s);

static void after_action(State &s, int actor) {
	wall_sync(s);   // 수가 끝나면 월은 다시 선다
	zone_ball(s);   // 공이 상대 존 칸에 놓여 있으면 그 수비수와 경합
	for (Player &q : s.players)   // 이 수의 경합에 든 선수는 태세가 풀린다 (11-26 묶음 7 — 존 힘은 경합이 이미 지웠다)
		if (g_met >> q.uid & 1u)
			q.tai = 0;
	const int r0 = s.role[actor];
	sync_roles(s);
	if (r0 == R_ATK && s.role[actor] == R_DEF && s.phase == PLAY && s.turn_team == actor) {
		s.pts = 0;   // 자기 차례에 공을 뺏기면 남은 포인트는 사라지고 바로 상대 차례
		end_turn(s);
		return;
	}
	if (s.phase == PLAY && s.pts <= 0)
		end_turn(s);
	else
		refresh_if_stuck(s);
}

// ---------- 존 · 경합 ----------

// 태세 선수의 존 칸 = 곁 네 칸. 판 밖·골대·박스 칸은 빠진다 (박스는 골키퍼의 땅)
int zone_tiles(const Player &p, V2 out[4]) {
	if (p.stance <= 0 || !p.placed())
		return 0;
	int n = 0;
	for (int k = 0; k < 4; k++) {
		const V2 t = p.pos + DIRS[k];
		if (in_bounds(t) && !is_goal(t) && !is_any_box(t))
			out[n++] = t;
	}
	return n;
}

static bool in_zone(const Player &p, V2 t) {
	V2 z[4];
	const int n = zone_tiles(p, z);
	for (int i = 0; i < n; i++)
		if (z[i] == t)
			return true;
	return false;
}

// 존 겹침 (11-26 묶음 7): 태세 힘이 큰 선수 → 같으면 태세를 먼저 건 선수(월 = 늘 맨 먼저) → 먼저 놓인 선수
static bool zone_before(const Player &a, const Player &b) {
	if (a.stance != b.stance)
		return a.stance > b.stance;
	const int oa = a.position == WALL ? 0 : a.st_ord, ob = b.position == WALL ? 0 : b.st_ord;
	if (oa != ob)
		return oa < ob;
	return a.placed_order < b.placed_order;
}

static void build_zone(const State &s, int team, int zone[NT]) {   // 태세 선수의 존 칸 — 겹치면 zone_before 가 주인
	for (int i = 0; i < NT; i++)
		zone[i] = -1;
	const Player *w[PER_TEAM * 2];
	int n = 0;
	for (const Player &q : s.players)
		if (q.team == team && q.placed() && q.stance > 0)
			w[n++] = &q;
	std::sort(w, w + n, [](const Player *a, const Player *b) { return zone_before(*a, *b); });
	for (int i = 0; i < n; i++) {
		V2 z[4];
		const int nz = zone_tiles(*w[i], z);
		for (int k = 0; k < nz; k++)
			if (zone[idx(z[k])] == -1)
				zone[idx(z[k])] = w[i]->uid;
	}
}

// 스크린 (11-25): 길 위 아군이 선 칸(끝 칸 제외) — SCREEN_MODE 2 = 그 아군이 태세 중이고 그 칸 존 수비수 du 보다 피지컬이 클 때만 경합 없음
// (1 = 태세만 · 0 = 옛 규칙 서 있기만). 스크린 판정은 이 함수 하나뿐 (길 고르기 path_key·드리블·패스·슛 모두)
static bool screened(const State &s, const Player &p, V2 tile, V2 last, int du) {
	if (tile == last)
		return false;
	const Player *q = player_at(s, tile);
	if (!q || q->team != p.team || q->uid == p.uid)
		return false;
	if (SCREEN_MODE == 0)
		return true;
	return (q->stance > 0 || q->tai != 0) && (SCREEN_MODE == 1 || phys(*q) > phys(s.players[du]));   // 태세 = 존이든 우리 차례 태세든
}

// 스크린이 실제로 한 번 막았다 (실행된 드리블·패스·슛 길) → 그 스크린 선수 태세가 풀린다 (11-26 — 태클·존에 진 수비수처럼)
// 길 고르기(path_key)의 미리 셈에서는 풀지 않는다
static void screen_spent(State &s, V2 tile) {
	for (Player &q : s.players)
		if (q.placed() && q.pos == tile) {
			q.stance = 0;
			q.stance_left = 0;
			q.tai = 0;
		}
}

// 경합: 동점이면 공 쪽이 이긴다 (11-26 묶음 7) — 존(공 힘 ≥ 태세) · 태클(버티는 힘 ≥ 태클 힘이면 지킴) · 슛(남은 힘 ≥ 골대 체력 = 골)
static inline bool ball_wins(int ball, int def) { return ball >= def; }

// 존 경합: 공 든 쪽이 그 칸의 수비수 태세에 건다 (협동 수비 없음)
static bool zone_contest(const State &s, const Player &atk, int atk_val, int du, V2 tile) {
	const int dv = s.players[du].stance;
	const bool win = ball_wins(atk_val, dv);
	emit({ 0, atk.uid, du, tile, atk_val, dv, win ? 1 : 0, {} });
	return win;
}

// ---------- 길 ----------

struct Reach {
	int step[NT];
	V2 order[NT];
	int n = 0;
	Reach() {
		for (int i = 0; i < NT; i++)
			step[i] = -1;
	}
	bool has(V2 t) const { return in_bounds(t) && step[idx(t)] >= 0; }
};

// 넓이 우선 도달 (상하좌우). blocked 칸은 못 지난다. goal_dest = 끝 칸으로만 허용되는 골대
static void reach(V2 from, int max_steps, const bool *blocked, V2 goal_dest, Reach &out) {
	V2 frontier[NT];
	int nf = 1;
	frontier[0] = from;
	for (int step = 1; step <= max_steps; step++) {
		V2 next[NT];
		int nn = 0;
		for (int fi = 0; fi < nf; fi++)
			for (const V2 &d : DIRS) {
				V2 t = frontier[fi] + d;
				if (!in_bounds(t) || t == from || out.has(t))
					continue;
				if (t == goal_dest) {
					out.step[idx(t)] = step;
					out.order[out.n++] = t;
					continue;
				}
				if (blocked[idx(t)])
					continue;
				out.step[idx(t)] = step;
				out.order[out.n++] = t;
				next[nn++] = t;
			}
		for (int i = 0; i < nn; i++)
			frontier[i] = next[i];
		nf = nn;
		if (!nf)
			break;
	}
}

// 길의 경합 미리 보기 — 경합이 있는 길은 피하고, 불가피하면 가장 덜 위험한 경합 칸으로.
// dribble_run·send_ball 과 같은 판정을 상태를 바꾸지 않고 센다: 존 칸 · 수비수마다 한 번(met) · 스크린(끝 칸 제외 아군 칸) ·
// 이기려면 힘 ≥ 태세(동점은 공 — ball_wins). 지면 거기서 공이 넘어가 끝난다
struct Foe {
	const State *s;
	const int *zone;   // null = 경합 없음 (공 없는 이동)
	int uid, val;
	bool carry;   // 공을 든 선수가 뺏기는 칸에 선다 (드리블) — 사이 아군에서 자신은 뺀다
	bool thru = false;   // 관통 — 존을 이겨도 힘이 안 깎임 (트릭스터 드리블 · 매지션 패스 · 포처 슛)
};

// 엔진 기본 길의 순서 (망이 있으면 화면·AI 는 간 뒤의 상태로 고른다: value_path). 작을수록 좋다:
// (a) 지는 경합 (b) 뺏기는 칸보다 우리 골 쪽 아군이 많은 길 (c) 뺏기는 칸이 우리 골에서 먼 길 (d) 걸음 (e) 위아래 대칭 동점 깨기.
// 이기는 경합은 감점하지 않는다
struct PathKey {
	int lose = 0, left = 0, cover = 0, near = 0, len = 0, mid = 0;   // left = −남은 힘 (남은 힘이 큰 길)
};

static PathKey path_key(const Foe *f, const std::vector<V2> &p) {
	PathKey k;
	k.len = int(p.size());
	for (const V2 &t : p)
		k.mid += std::abs(2 * t.y - (H - 1));   // 가운데 줄에서 먼 만큼 (위아래 대칭)
	if (!f)
		return k;
	const State &s = *f->s;
	const Player &me = s.players[f->uid];
	bool met[PLAYERS] = {};
	int cur = f->val;
	if (!f->zone)
		return k;
	for (const V2 &t : p) {
		int du = f->zone[idx(t)];
		if (du == -1 || met[du] || screened(s, me, t, p.back(), du))
			continue;
		met[du] = true;
		if (!ball_wins(cur, s.players[du].stance)) {   // 동점은 공 (zone_contest)
			k.lose = 1;
			const int fwd = forward_dir(me.team);
			for (const Player &q : s.players)
				if (q.team == me.team && q.placed() && !(f->carry && q.uid == me.uid) && (t.x - q.pos.x) * fwd > 0)
					k.cover--;
			k.near = -goal_dist(me.team, t);   // 우리 골에서 가까울수록 나쁨
			break;
		}
		if (!f->thru)
			cur -= s.players[du].stance;   // 이긴 만큼 남음 · 관통(트릭스터 드리블·매지션 패스)은 안 깎임
	}
	k.left = k.lose ? 0 : -cur;
	return k;
}

static bool key_less(const PathKey &a, const std::vector<V2> &pa, const PathKey &b, const std::vector<V2> &pb) {
	if (a.lose != b.lose) return a.lose < b.lose;
	if (a.left != b.left) return a.left < b.left;
	if (a.cover != b.cover) return a.cover < b.cover;
	if (a.near != b.near) return a.near < b.near;
	if (a.len != b.len) return a.len < b.len;
	if (a.mid != b.mid) return a.mid < b.mid;
	for (size_t i = 0; i < pa.size(); i++) {   // 앞 걸음일수록 가운데 줄 가까이 → 앞 걸음의 x (둘 다 위아래 뒤집어도 같다)
		int ma = std::abs(2 * pa[i].y - (H - 1)), mb = std::abs(2 * pb[i].y - (H - 1));
		if (ma != mb) return ma < mb;
		if (pa[i].x != pb[i].x) return pa[i].x < pb[i].x;
	}
	for (size_t i = 0; i < pa.size(); i++)   // 꼭 거울상인 두 길만 여기까지 — 위쪽(y 작은 쪽) 먼저
		if (pa[i].y != pb[i].y) return pa[i].y < pb[i].y;
	return false;
}

// 같은 칸 두 번 없는 모든 길을 돈다 (범위 안, 적의 몸·골대 ×, 골대는 끝으로만). at(cur) → 끝에 닿은 길, cut(걸음 하한) → true 면 가지치기
template <class At, class Cut>
static void walk_paths(V2 from, V2 dest, int max_steps, const bool *blocked, V2 goal_dest, At at_dest, Cut cut) {
	if (max_steps < 1 || manhattan(from, dest) > max_steps)
		return;
	std::vector<V2> cur;
	bool used[NT] = {};
	used[idx(from)] = true;
	auto go = [&](auto &&self, V2 at) -> void {
		if (at == dest && !cur.empty()) {
			at_dest(cur);
			return;
		}
		const int lb = int(cur.size()) + manhattan(at, dest);
		if (lb > max_steps || cut(lb))
			return;
		for (const V2 &d : DIRS) {
			V2 t = at + d;
			if (!in_bounds(t) || used[idx(t)] || (t == goal_dest ? t != dest : blocked[idx(t)] || is_goal(t)))
				continue;
			used[idx(t)] = true;
			cur.push_back(t);
			self(self, t);
			cur.pop_back();
			used[idx(t)] = false;
		}
	};
	go(go, from);
}

// 엔진이 고르는 기본 길 (안 그린 길·AI 기본값): PathKey 가 가장 작은 길. f = 경합 보기 또는 null(경합 없는 행동)
static std::vector<V2> best_path(V2 from, V2 dest, int max_steps, const Foe *f, const bool *blocked, V2 goal_dest) {
	std::vector<V2> best;
	PathKey bk;
	walk_paths(from, dest, max_steps, blocked, goal_dest,
			[&](const std::vector<V2> &cur) {
				PathKey k = path_key(f, cur);
				if (best.empty() || key_less(k, cur, bk, best))
					best = cur, bk = k;
			},
			[&](int lb) { return !best.empty() && bk.lose == 0 && bk.left == (f && f->zone ? -f->val : 0) && lb > bk.len; });   // 경합 없이 안 지는 길이 이미 있다 — 더 긴 길은 볼 것 없다
	return best;
}

// 그린 길이 쓸 만한가: 출발 칸에 붙어 한 칸씩, 같은 칸 두 번 ×, 범위 안, 벽(적의 몸·골대) ×, 끝 = dest (골대는 끝으로만)
static bool path_ok(V2 from, const std::vector<V2> &p, V2 dest, int max_steps, const bool *blocked, V2 goal_dest) {
	if (p.empty() || int(p.size()) > max_steps || p.back() != dest)
		return false;
	V2 prev = from;
	for (size_t i = 0; i < p.size(); i++) {
		V2 t = p[i];
		if (!in_bounds(t) || manhattan(prev, t) != 1 || t == from)
			return false;
		for (size_t j = 0; j < i; j++)
			if (p[j] == t)
				return false;
		if (t == goal_dest ? i + 1 != p.size() : (blocked[idx(t)] || is_goal(t)))
			return false;
		prev = t;
	}
	return true;
}

static void body_blocked(const State &s, int team, bool blocked[NT]) {   // 적의 몸과 골대는 벽 · 아군 몸은 지나간다
	for (int i = 0; i < NT; i++)
		blocked[i] = false;
	for (int k = 0; k < GOAL_N; k++)
		blocked[idx(goal_tile(HOME, k))] = blocked[idx(goal_tile(AWAY, k))] = true;
	for (const Player &q : s.players)
		if (q.placed() && q.team != team)
			blocked[idx(q.pos)] = true;
}

// 그린 길이 있으면 그것(쓸 수 없으면 빈 길 = 실행 안 함), 없으면 엔진이 고른 길
static std::vector<V2> route(V2 from, V2 dest, int max_steps, const Foe *danger, const bool *blocked, V2 goal_dest) {
	if (g_path)
		return path_ok(from, *g_path, dest, max_steps, blocked, goal_dest) ? *g_path : std::vector<V2>();
	return best_path(from, dest, max_steps, danger, blocked, goal_dest);
}

// 설 수 있는 칸: 박스·골대 칸 × · 골키퍼는 자기 진영만 (골대로 돌아가기는 move_dest_ok)
static bool stand_ok(const State &s, const Player &p, V2 t) {
	if (is_gk(s, p) && !is_own_half(p.team, t))
		return false;
	return in_bounds(t) && !player_at(s, t) && !is_goal(t) && !is_any_box(t);
}

static bool move_dest_ok(const State &s, const Player &p, V2 t) {
	return stand_ok(s, p, t) || (is_gk(s, p) && is_goal_of(p.team, t) && !in_goal(s, p));   // 키퍼는 골대로 돌아갈 수 있다
}

// 길이 떠나는 칸: 골대 안 키퍼는 골대 두 칸 어느 쪽에서든 (두 칸을 한 칸처럼)
static int origins(const State &s, const Player &p, V2 o[2]) {
	if (!in_goal(s, p))
		return o[0] = p.pos, 1;
	o[0] = goal_tile(p.team, 0), o[1] = goal_tile(p.team, 1);
	return 2;
}

// 타이탄 착지 밀기 — 도착 칸 상하좌우의 적 모두를 1칸 바깥으로. 밀린 선수는 태세가 풀린다
// 공을 안 쥐고 태세도 아닐 때만 (11-26 묶음 7) · 타이탄 피지컬 > 그 적 피지컬일 때만 (동점 = 안 밀림). 밀 칸이 막히면(판 밖·사람·박스·골대 · 키퍼는 상대 진영) 그 적만 그대로
static void titan_push(State &s, const Player &p) {
	if (p.position != TITAN || p.tai != 0 || p.stance > 0 || s.ball_owner == p.uid)
		return;
	for (const V2 &d : DIRS) {
		const Player *q = player_at(s, p.pos + d);
		if (!q || q->team == p.team || phys(p) <= phys(*q) || !stand_ok(s, *q, q->pos + d))
			continue;
		Player &qm = s.players[q->uid];
		qm.pos = qm.pos + d;
		qm.stance = 0;
		qm.stance_left = 0;
		qm.tai = 0;
		emit({ 1, qm.uid, -1, qm.pos, 0, 0, 0, {} });
	}
}

// 드리블 길의 존 경합. 끝 칸(뺏기면 그 칸)을 돌려준다
// 존을 이기면 이긴 만큼만 힘이 남아 다음 존과 겨룬다 — val 은 남은 힘으로 바뀐다 · 진 수비수는 태세가 풀린다
static V2 dribble_run(State &s, int uid, int &val, const std::vector<V2> &path) {
	int zone[NT];
	build_zone(s, opponent(s.players[uid].team), zone);
	bool met[PLAYERS] = {};
	for (const V2 &t : path) {
		int du = zone[idx(t)];
		if (du == -1 || met[du])
			continue;
		if (screened(s, s.players[uid], t, path.back(), du)) {
			screen_spent(s, t);
			continue;
		}
		met[du] = true;
		if (!zone_contest(s, s.players[uid], val, du, t)) {
			const int dv = s.players[du].stance;
			set_owner(s, du);   // 수비수가 제 칸에서 공을 갖는다
			mark_steal(s, dv);
			return t;
		}
		if (!pierce(s.players[uid], U_DRIBBLE))
			val -= s.players[du].stance;   // 트릭스터 = 이겨도 힘이 안 깎여 다음 존도 처음 힘으로
		s.players[du].stance = 0;
		s.players[du].stance_left = 0;
	}
	return path.back();
}

// ---------- 태클 (태세를 건 선수·월이 움직여 상대 공 임자 곁에 서면) ----------
// 태클 한 번: 힘 tv > 버티는 힘이면 뺏음 (동점 = 공 임자가 지킴) · 아니면 버티는 힘을 tv 만큼 깎는다(패스를 받거나 새로 잡을 때만 다시 참) · 태클한 수비수 태세는 풀림
static void tackle_do(State &s, int tuid, int tv) {
	const Player &h = s.players[s.ball_owner];
	const int hv = s.hold;
	const bool win = !ball_wins(hv, tv) || spent_out(h);   // 체력 0 인 공 임자 = 힘 비교 없이 뺏김 (11-25)
	emit({ 0, tuid, h.uid, h.pos, tv, hv, win ? 1 : 0, {} });
	if (win) {
		set_owner(s, tuid);
		mark_steal(s, tv);
	} else {
		s.hold = std::max(0, hv - tv);
		s.players[tuid].stance = 0;
		s.players[tuid].stance_left = 0;
	}
}

// ---------- 액션 ----------

// 키퍼가 공을 쥔 팀 (11-26 묶음 7): 키퍼가 공을 놓을 때까지 키퍼만 (패스·드리블)
static bool keeper_locked(const State &s, const Player &p) { return s.keeper == p.team && s.gk[p.team] >= 0 && s.ball_owner == s.gk[p.team]; }

static bool can_act(const State &s, const Player &p) {
	return s.phase == PLAY && p.placed() && p.team == s.turn_team && s.pts >= 1 && !spent_out(p) && !(keeper_locked(s, p) && p.uid != s.ball_owner);   // 한 선수가 여러 번 행동해도 된다 · 체력 0 = 행동 불가
}

static bool may_carry(const State &s, const Player &p) {   // 공 임자 · 킥오프 차례면 킥오프 팀 누구나
	return s.ball_owner == p.uid || (kickoff_turn(s) && p.team == s.turn_team && !in_goal(s, p));
}

static void pay_point(State &s, int uid) {   // 행동 = 포인트 1 · 체력 1 · 우리 차례 태세는 이 행동에 쓰고 풀린다 (힘·범위는 이 앞에서 잰다)
	s.pts -= 1;
	tire(s, uid);
	if (s.players[uid].tai == 1)
		s.players[uid].tai = 0;
}

static bool do_move(State &s, int uid, V2 dest) {
	const Player &p = s.players[uid];
	if (!can_act(s, p) || kickoff_turn(s) || s.ball_owner == uid || !move_dest_ok(s, p, dest))
		return false;
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	const int rng = act_range(s, p, C_MOVE);
	V2 from[2], to[2];
	const int nf = origins(s, p, from);
	const bool home = is_goal_of(p.team, dest);   // 키퍼가 골대로 돌아감 — 두 칸 어느 쪽에 닿아도 된다
	const int nt = home ? 2 : 1;
	to[0] = home ? goal_tile(p.team, 0) : dest, to[1] = goal_tile(p.team, 1);
	std::vector<V2> path;
	for (int i = 0; i < nf; i++)
		for (int j = 0; j < nt; j++) {
			std::vector<V2> c = route(from[i], to[j], rng, nullptr, blocked, home ? to[j] : UNSET);
			if (!c.empty() && (path.empty() || c.size() < path.size()))
				path = c;
		}
	if (path.empty())
		return false;
	if (home)
		dest = goal_tile(p.team, 0);   // 골대 안 = 대표 칸
	const int team = p.team;
	const V2 start = p.pos;
	emit({ 4, uid, -1, dest, 0, 0, 0, path });
	// 태세(우리 차례)·월이 움직여 상대 공 임자 곁에 서면 태클 — 힘 = 디펜스 + 1 − 움직인 칸 (+1 태세) · 움직이면 존은 풀린다
	const bool tk = p.tai == 1 || p.stance > 0;
	const int tv = act_power(s, p, U_STANCE) + 1 - int(path.size()) + (p.tai == 1);
	const bool was_tai = p.tai != 0;   // 태세를 건 채 움직인 타이탄은 안 민다
	pay_point(s, uid);
	s.players[uid].pos = dest;
	{
		Player &mv = s.players[uid];
		if (mv.stance > 0)
			mv.stance = 0, mv.stance_left = 0;
		if (tk && s.ball_owner >= 0 && s.players[s.ball_owner].team != team && manhattan(mv.pos, s.players[s.ball_owner].pos) == 1 && !is_any_box(mv.pos))
			tackle_do(s, uid, std::max(1, tv));
	}
	if (!was_tai)
		titan_push(s, s.players[uid]);   // 태클을 본 뒤 밀기 · 태세였거나 공을 쥐었으면 안 밂
	if (path.size() >= 3 && (dest.x - start.x) * forward_dir(team) > 0)
		rep_add(s, uid, SPRINTER);
	after_action(s, team);
	return true;
}

static bool do_dribble(State &s, int uid, V2 dest) {
	const Player &p = s.players[uid];
	if (!can_act(s, p) || !may_carry(s, p) || !stand_ok(s, p, dest))
		return false;   // 공을 쥔 키퍼는 골대에서 드리블해 나와도 된다 (11-26 묶음 7)
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	int pow = act_power(s, p, U_DRIBBLE, manhattan(p.pos, dest));   // 드리블 힘 = 가는 거리에 반비례
	const Foe f{ &s, zone, uid, pow, true, pierce(p, U_DRIBBLE) };
	std::vector<V2> path = route(p.pos, dest, act_range(s, p, C_MOVE), &f, blocked, UNSET);
	if (path.empty())
		return false;
	const int team = p.team;
	const V2 from = p.pos;
	emit({ 4, uid, -1, dest, pow, 0, 0, path });
	if (s.ball_owner == -1)
		set_owner(s, uid);   // 킥오프 — 이 선수가 공을 갖고 시작한다
	pay_point(s, uid);
	const V2 end = dribble_run(s, uid, pow, path);
	s.players[uid].pos = end;
	titan_push(s, s.players[uid]);   // 드리블 뒤 — 공을 뺏겼을 때만 (공 쥔 타이탄은 안 밂)
	if (s.ball_owner == uid) {
		s.hold = pow;   // 드리블 뒤 버티는 힘 = 그 드리블의 남은 힘
		if ((end.x - from.x) * forward_dir(team) > 0)
			rep_add(s, uid, TRICKSTER);
	}
	after_action(s, team);
	return true;
}

static void pass_targets(const State &s, const Player &p, int rng, Reach &ground, Reach &air) {
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	reach(p.pos, rng, blocked, UNSET, ground);
	if (rng >= LONG_PASS_MIN) {
		bool open[NT] = {};
		reach(p.pos, rng, open, UNSET, air);
	}
}

// 패스 받을 칸 = 동료(나 아님) 칸뿐 — 오프사이드·빈 칸 패스 없음 · 골대 안 키퍼는 못 받음
static bool pass_dest_ok(const State &s, const Player &p, V2 t) {
	if (is_any_box(t) || is_goal(t))
		return false;
	const Player *r = player_at(s, t);
	return r && r->team == p.team && r->uid != p.uid;
}

static bool send_ball(State &s, const Player &p, int &val, const std::vector<V2> &path, bool pass) {   // true = 끊겼다 · val = 남은 힘 · pass = 패스(아니면 슛)
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool met[PLAYERS] = {};
	for (const V2 &t : path) {
		int du = zone[idx(t)];
		if (du == -1 || met[du])
			continue;
		if (screened(s, p, t, path.back(), du)) {
			screen_spent(s, t);
			continue;
		}
		met[du] = true;
		if (!zone_contest(s, p, val, du, t)) {
			const int dv = s.players[du].stance;
			set_owner(s, du);
			mark_steal(s, dv);
			return true;
		}
		if (!pierce(p, pass ? U_PASS : U_SHOT))
			val -= s.players[du].stance;   // 이긴 만큼 남음 · 관통(매지션 패스 · 포처 슛)은 안 깎임
		s.players[du].stance = 0;
		s.players[du].stance_left = 0;
	}
	return false;
}

// 패스의 공 길: 5칸 이상 = 공중볼(떨어지는 칸만 — 경합 없음), 아니면 땅볼 길. 못 차면 빈 길
static std::vector<V2> kick_path(const State &s, const Player &p, V2 dest, int pow) {
	const int rng = act_range(s, p, C_KICK);
	const int dist = manhattan(p.pos, dest);
	if (dist > rng)
		return {};
	if (dist >= LONG_PASS_MIN)
		return { dest };
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	const Foe f{ &s, zone, p.uid, pow, false, pierce(p, U_PASS) };
	return route(p.pos, dest, rng, &f, blocked, UNSET);
}

static bool do_pass(State &s, int uid, V2 dest) {
	const Player &p = s.players[uid];
	if (!can_act(s, p) || !may_carry(s, p) || !pass_dest_ok(s, p, dest))
		return false;
	const int dist = manhattan(p.pos, dest);
	const bool aerial = dist >= LONG_PASS_MIN;
	int pow = act_power(s, p, U_PASS, dist);
	std::vector<V2> path = kick_path(s, p, dest, pow);
	if (path.empty())
		return false;
	const int team = p.team;
	const V2 from = p.pos;
	const int ru = player_at(s, dest)->uid;
	emit({ 4, uid, -1, dest, pow, aerial ? 1 : 0, 0, path });
	if (s.ball_owner == -1)
		set_owner(s, uid);
	pay_point(s, uid);
	if (!send_ball(s, p, pow, path, true)) {
		set_owner(s, ru);
		s.hold = pow;   // 받은 선수 버티는 힘 = 그 패스의 남은 힘
	}
	if (s.ball_team() == team && (dest.x - from.x) * forward_dir(team) > 0)
		rep_add(s, uid, MAESTRO);
	after_action(s, team);
	return true;
}

// 슛할 골대 칸과 길: 범위 안 두 칸 가운데 안 지고 · 남은 힘이 큰 쪽 (path_key). 못 차면 빈 길
static std::vector<V2> shot_route(const State &s, const Player &p, V2 &goal, int &pow) {
	const int rng = act_range(s, p, C_SHOT);
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	std::vector<V2> best;
	PathKey bk;
	for (int k = 0; k < GOAL_N; k++) {
		const V2 g = goal_tile(opponent(p.team), k);
		const int pw = act_power(s, p, U_SHOT, manhattan(p.pos, g));
		const Foe f{ &s, zone, p.uid, pw, false, pierce(p, U_SHOT) };
		std::vector<V2> path = route(p.pos, g, rng, &f, blocked, g);
		if (path.empty())
			continue;
		const PathKey key = path_key(&f, path);
		if (best.empty() || key.lose < bk.lose || (key.lose == bk.lose && (key.left < bk.left || (key.left == bk.left && path.size() < best.size()))))
			best = path, bk = key, goal = g, pow = pw;
	}
	return best;
}

static bool do_shot(State &s, int uid) {
	const Player &p = s.players[uid];
	if (!can_act(s, p) || s.ball_owner != uid || keeper_locked(s, p))
		return false;
	V2 goal = opponent_goal(p.team);
	int pow = 0;
	std::vector<V2> path = shot_route(s, p, goal, pow);
	if (path.empty())
		return false;
	const int team = p.team;
	emit({ 4, uid, -1, goal, pow, 2, 0, path });
	pay_point(s, uid);
	if (send_ball(s, p, pow, path, false)) {
		after_action(s, team);
		return true;
	}
	// 슛 길의 경합을 뚫고 골대에 닿았다 — 남은 힘 ≥ 골대 체력이면 골 · 아니면 남은 힘만큼 체력을 깎고 막힘
	const int keeper = opponent(team);
	const bool in = s.gk_in(keeper);   // 키퍼가 골대 밖이면 체력 0 (남은 체력은 그대로 두었다가 돌아오면 다시 씀)
	const bool scored = ball_wins(pow, in ? s.ghp[keeper] : 0);
	if (in)
		s.ghp[keeper] = std::max(0, s.ghp[keeper] - pow);
	if (!scored) {   // 막힘 = 키퍼가 공을 쥔다 → 놓을 때까지 그 팀은 키퍼만 (패스·드리블 — 11-26 묶음 7) · 판에 키퍼가 없으면 가상 키퍼 롱킥
		s.keeper = -1;
		set_owner(s, s.gk[keeper]);   // 가상 키퍼(−1) = 공 임자 없음
		if (s.gk[keeper] < 0)
			s.hold = 0;
		s.keeper = keeper;
		emit({ 9, keeper, -1, goal, s.ghp[keeper], pow, 0, {} });   // 사건 9 = 키퍼가 막음 (a = 키퍼 팀 · at = 골대 칸 · va = 남은 체력 · vb = 깎은 힘)
		after_action(s, team);   // 공을 잃었다 — 남은 포인트는 사라지고 상대 차례
		return true;
	}
	s.goals[team] += 1;
	emit({ 2, team, pow, goal, pow, 0, 1, {} });
	rest(s, team);
	s.turn_count += 1;   // 넣은 팀의 차례는 여기서 끝
	if (!clock(s))
		restart_half(s, team);
	return true;
}

// 가상 키퍼(판에 키퍼가 없는 시험판만) 롱킥: 킥 범위(골대 두 칸 중 가까운 칸에서 — goal_dist) 안 아군 발밑으로 (공중볼 · 경합 없음 · 포인트 1).
// 받은 선수 버티는 힘 = 킥 + 1 − 거리. 찰 곳이 없으면 차례가 넘어간다. 판 위 키퍼는 보통 패스·드리블 (11-26 묶음 7)
static bool keep_ok(const State &s, const Player &q) {
	const int team = s.keeper;
	if (team < 0 || s.gk[team] >= 0)
		return false;
	return q.team == team && q.placed() && !is_any_box(q.pos) && !is_goal(q.pos) && goal_dist(team, q.pos) <= s.gk_range[team];
}

static bool do_keep(State &s, int uid, V2 dest) {
	if (s.phase != PLAY || s.keeper != s.turn_team || s.pts < 1)
		return false;
	const int team = s.keeper;
	if (uid < 0 || uid >= PLAYERS || !keep_ok(s, s.players[uid]))
		return false;
	const Player &q = s.players[uid];
	const int pow = std::max(1, s.gk_kick[team] + 1 - goal_dist(team, q.pos));
	emit({ 4, -1, uid, q.pos, pow, 3, 0, { q.pos } });   // 사건 4 · vb 3 = 키퍼 롱킥 (a = −1 · b = 받는 선수 · at = 받는 칸 · va = 힘)
	s.keeper = -1;
	set_owner(s, uid);
	s.hold = pow;
	s.pts -= 1;
	after_action(s, team);
	return true;
}

// 태세 (11-26 묶음 7): 누구나 제자리 · 포인트 1 · 체력 1 → 그 선수 다음 행동 힘·범위 +1 (이동 = 범위 · 다가가 태클 힘 +1).
// 안 쓰고 차례가 넘어가면 상대 차례 동안 선다 (end_turn). 이미 태세 · 박스 안 · 골대 안 키퍼 · 킥오프 차례 · 키퍼가 공을 쥔 팀 = 못 함
static bool can_stance(const State &s, const Player &p) {
	return can_act(s, p) && !kickoff_turn(s) && p.tai == 0 && !is_any_box(p.pos) && !in_goal(s, p) && s.keeper != p.team;
}

static bool do_stance(State &s, int uid, V2 dest) {
	Player &p = s.players[uid];
	if (!can_stance(s, p) || dest != p.pos)
		return false;
	pay_point(s, uid);
	p.tai = 1;
	p.st_ord = ++s.st_seq;
	after_action(s, p.team);
	return true;
}

// 존과 공이 겹치면 무조건 경합 (사용자 11-24): 수가 끝났을 때 공 임자가 상대 존 칸에 있으면 그 칸 수비수와 존 경합
// (판정 = 존 경합 — 공 쪽 힘 = 버티는 힘이 건다 · 동점 = 공). 받은 칸·뺏은 칸·롱킥 받은 칸·밀린 칸·월이 다시 선 존 모두 같다.
// 이 수에서 이미 겨룬 수비수는 다시 안 겨룸(수비수마다 한 번). 이기면 버티는 힘이 그만큼 깎이고 수비수 태세가 풀림 · 지면 수비수가 공을 갖는다
static void zone_ball(State &s) {
	while (s.phase == PLAY && s.ball_owner >= 0) {
		const Player &h = s.players[s.ball_owner];
		int du = -1;
		for (const Player &q : s.players)   // 그 칸의 존 주인 = zone_before (build_zone 과 같음) · 이미 겨룬 선수 빼고
			if (q.team != h.team && !(g_met >> q.uid & 1u) && in_zone(q, h.pos) && (du == -1 || zone_before(q, s.players[du])))
				du = q.uid;
		if (du == -1)
			return;
		if (!zone_contest(s, h, spent_out(h) ? 0 : s.hold, du, h.pos)) {   // 체력 0 공 임자 = 버티는 힘 0 으로 겨룸 (11-25 — 태클과 같게 늘 뺏김)
			const int dv = s.players[du].stance;
			set_owner(s, du);
			mark_steal(s, dv);
			wall_sync(s);   // 뺏은 월 = 공을 쥐어 태세 없음
			continue;   // 뺏은 선수가 다시 상대 존 칸이면 또 겨룸
		}
		s.hold -= s.players[du].stance;
		s.players[du].stance = 0;
		s.players[du].stance_left = 0;
	}
}

static bool apply_one(State &s, const Action &a) {
	const bool u = a.uid >= 0 && a.uid < PLAYERS;
	switch (a.t) {
		case A_PLACE: return do_place(s, a.uid, a.dest);
		case A_MOVE: return u && do_move(s, a.uid, a.dest);
		case A_DRIBBLE: return u && do_dribble(s, a.uid, a.dest);
		case A_PASS: return u && do_pass(s, a.uid, a.dest);
		case A_SHOT: return u && do_shot(s, a.uid);
		case A_STANCE: return u && do_stance(s, a.uid, a.dest);
		case A_KEEP: return do_keep(s, a.uid, a.dest);
	}
	return false;   // 차례 끝 수는 없다 — 둘 수가 없을 때만 저절로 끝
}

bool apply_action(State &s, const Action &a) {
	if (s.phase == PLAY && s.keeper == s.turn_team && (s.gk[s.keeper] < 0 ? a.t != A_KEEP : a.t != A_DRIBBLE && a.t != A_PASS))
		return false;   // 키퍼가 공을 쥔 차례: 판 위 키퍼 = 패스·드리블뿐 (키퍼만 — can_act) · 가상 키퍼 = 롱킥뿐
	const bool u = a.uid >= 0 && a.uid < PLAYERS;
	const int team = s.turn_team;
	g_met = 0;
	if (!apply_one(s, a))
		return false;
	if (u && a.t != A_PLACE && s.players[a.uid].team == team && s.turn_team == team)
		s.players[a.uid].acted = 1;
	wall_sync(s);
	return true;
}

// 결과가 다른 길 후보 (승률 판단은 AI 가 한다 — 휴리스틱이 대신하지 않는다).
// 같은 행동(같은 도착 칸)을 길만 바꿔 다 해 보고, 끝 상태가 똑같은 길끼리 한 묶음 — 묶음마다 대표 하나(기본 규칙 PathKey 1등).
// out[0] = 기본 길(route 가 고르는 길)의 묶음. 이동·드리블·땅볼 패스·슛만 — 공중볼·그 밖은 빈 목록. 상태는 바꾸지 않는다
void outcome_paths(const State &s, const Action &a, std::vector<std::vector<V2>> &out) {
	out.clear();
	if (a.t < A_MOVE || a.t > A_SHOT || a.uid < 0 || a.uid >= PLAYERS || g_path)
		return;
	const Player &p = s.players[a.uid];
	const bool kick = a.t == A_PASS || a.t == A_SHOT;
	const V2 dest = a.t == A_SHOT ? (is_goal_of(opponent(p.team), a.dest) ? a.dest : opponent_goal(p.team)) : a.dest;
	if ((a.t == A_PASS && manhattan(p.pos, dest) >= LONG_PASS_MIN) || in_goal(s, p) || is_goal_of(p.team, dest))
		return;   // 공중볼 · 골대를 드나드는 키퍼 = 길 하나 (엔진 기본 길)
	bool blocked[NT];   // 그 행동의 벽 (적의 몸·골대 — 엔진 판정과 같게)
	body_blocked(s, p.team, blocked);
	std::vector<Event> *ev0 = g_events, dev;
	g_events = &dev;
	State def;
	std::memcpy(static_cast<void *>(&def), &s, sizeof(State));
	const bool dok = apply_action(def, a);
	g_events = nullptr;
	std::vector<V2> dpath;
	for (const Event &e : dev)
		if (e.kind == 4 && e.a == a.uid) {
			dpath = e.path;
			break;
		}
	if (!dok || dpath.empty()) {
		g_events = ev0;
		return;
	}
	std::vector<State> ends;
	std::vector<PathKey> keys;
	walk_paths(p.pos, dest, act_range(s, p, kick ? C_KICK : C_MOVE), blocked, a.t == A_SHOT ? dest : UNSET,
			[&](const std::vector<V2> &cur) {
				State t;
				std::memcpy(static_cast<void *>(&t), &s, sizeof(State));
				g_path = &cur;
				const bool ok = apply_action(t, a);
				g_path = nullptr;
				if (!ok)
					return;
				const PathKey k = path_key(nullptr, cur);
				size_t i = 0;
				while (i < ends.size() && std::memcmp(&ends[i], &t, sizeof(State)))
					i++;
				if (i == ends.size()) {
					ends.push_back(t);
					keys.push_back(k);
					out.push_back(cur);
				} else if (key_less(k, cur, keys[i], out[i])) {
					keys[i] = k;
					out[i] = cur;
				}
			},
			[](int) { return false; });
	g_events = ev0;
	for (size_t i = 0; i < ends.size(); i++)
		if (!std::memcmp(&ends[i], &def, sizeof(State))) {   // 기본 길의 묶음을 맨 앞으로 — 대표도 기본 길 그대로
			std::swap(out[0], out[i]);
			out[0] = dpath;
			break;
		}
}

// 길을 간 뒤의 상태로 고른다: 결과가 다른 길 후보마다 끝 상태를 val(상태, 움직인 팀, ctx) — 그 팀에 좋을수록 큼 — 로 재어
// 가장 높은 후보. val 이 null 이면 후보 맨 앞(대비책 순서 = 엔진 기본 길). 후보가 없으면 false (공중볼 등 — 길이 하나)
bool value_path(const State &s, const Action &a, PathValue val, const void *ctx, std::vector<V2> &out) {
	std::vector<std::vector<V2>> alt;
	outcome_paths(s, a, alt);
	if (alt.empty())
		return false;
	size_t bi = 0;
	if (val && alt.size() > 1) {
		std::vector<Event> *ev0 = g_events;
		g_events = nullptr;
		float bv = -1e30f;
		for (size_t i = 0; i < alt.size(); i++) {
			State t = s;
			g_path = &alt[i];
			apply_action(t, a);
			g_path = nullptr;
			const float v = val(t, s.players[a.uid].team, ctx);
			if (v > bv)
				bv = v, bi = i;   // 같은 값이면 앞 후보 (기본 길 먼저)
		}
		g_events = ev0;
	}
	out = alt[bi];
	return true;
}



// ---------- 열거 ----------

void enumerate_actions(const State &s, std::vector<Action> &out) {
	out.clear();
	if (s.phase == PLACEMENT) {
		const int team = placement_team(s);
		if (team < 0)
			return;
		const int x0 = team == HOME ? 0 : W - W / 2;   // 자기 진영 절반
		for (int slot = 0; slot <= SLOT_PROSPECT; slot++) {
			if (!slot_open(s, team, slot))
				continue;
			for (int x = x0; x < x0 + W / 2; x++)
				for (int z = 0; z < H; z++) {
					const V2 t{ x, z };
					if (place_tile_ok(s, team, t))
						out.push_back({ A_PLACE, slot, t });
				}
		}
		return;
	}
	if (s.phase != PLAY || s.pts < 1)
		return;
	const int team = s.turn_team;
	if (s.keeper == team && s.gk[team] < 0) {   // 가상 키퍼가 공을 쥐고 있다 — 롱킥뿐 (찰 곳이 없으면 차례가 넘어간다)
		for (const Player &q : s.players)
			if (keep_ok(s, q))
				out.push_back({ A_KEEP, q.uid, q.pos });
		return;
	}
	const bool ko = kickoff_turn(s);
	for (const Player &q : s.players) {
		if (q.team != team || !can_act(s, q))
			continue;   // 체력 0 = 행동 불가 (11-25) · 키퍼가 공을 쥐면 키퍼만
		const int u = q.uid;
		const bool carry = may_carry(s, q);
		{   // 이동·드리블 (공을 쥔 키퍼 드리블 = 골대 대표 칸에서)
			const int mr = act_range(s, q, C_MOVE);
			bool blocked[NT];
			body_blocked(s, team, blocked);
			V2 from[2];
			const int nf = carry ? (from[0] = q.pos, 1) : origins(s, q, from);
			Reach r;
			for (int i = 0; i < nf; i++) {   // 골대 안 키퍼 = 두 칸 어느 쪽에서든
				Reach ri;
				reach(from[i], mr, blocked, UNSET, ri);
				for (int k = 0; k < ri.n; k++)
					if (!r.has(ri.order[k]))
						r.step[idx(ri.order[k])] = ri.step[idx(ri.order[k])], r.order[r.n++] = ri.order[k];
			}
			for (int i = 0; i < r.n; i++) {
				const V2 t = r.order[i];
				if (carry ? !stand_ok(s, q, t) : ko || s.ball_owner == u || !stand_ok(s, q, t))
					continue;
				out.push_back({ carry ? A_DRIBBLE : A_MOVE, u, t });
			}
			if (!carry && !ko && is_gk(s, q) && !in_goal(s, q))   // 키퍼가 골대로 돌아감 (두 칸 중 한 칸에 닿으면 — dest = 대표 칸)
				for (int k = 0; k < GOAL_N; k++) {
					Reach rg;
					reach(q.pos, mr, blocked, goal_tile(team, k), rg);
					if (rg.has(goal_tile(team, k))) {
						out.push_back({ A_MOVE, u, goal_tile(team, 0) });
						break;
					}
				}
		}
		if (carry) {   // 패스 (땅볼 → 땅볼로 못 닿는 롱패스) · 슛
			Reach g, air;
			pass_targets(s, q, act_range(s, q, C_KICK), g, air);
			for (int i = 0; i < g.n; i++)
				if (pass_dest_ok(s, q, g.order[i]))
					out.push_back({ A_PASS, u, g.order[i] });
			for (int i = 0; i < air.n; i++) {
				const V2 t = air.order[i];
				if (manhattan(q.pos, t) >= LONG_PASS_MIN && !g.has(t) && pass_dest_ok(s, q, t))
					out.push_back({ A_PASS, u, t });
			}
			if (!ko && s.ball_owner == u && !keeper_locked(s, q)) {   // 슛 — 골대 두 칸 가운데 닿는 칸이 있으면 수 하나 (dest = 대표 칸 · 실제 칸은 do_shot 이 고른다)
				bool blocked[NT];
				body_blocked(s, team, blocked);
				const int sr_n = act_range(s, q, C_SHOT);
				bool can = false;
				for (int k = 0; k < GOAL_N && !can; k++) {
					const V2 goal = goal_tile(opponent(team), k);
					Reach sr;
					reach(q.pos, sr_n, blocked, goal, sr);
					can = sr.has(goal);
				}
				if (can)
					out.push_back({ A_SHOT, u, opponent_goal(team) });
			}
		}
		if (can_stance(s, q))   // 태세 = 제자리 (누구나 — 11-26 묶음 7)
			out.push_back({ A_STANCE, u, q.pos });
	}
}

// ---------- 손평가 (망 입력 d[13] · 망 없는 탐색 가치) ----------

double heuristic(const State &s, int team) {
	int opp = opponent(team);
	double h = (s.goals[team] - s.goals[opp]) * 3.0;
	if (s.phase == PLAY) {
		int bt = s.ball_team();
		h += bt == team ? 0.4 : -0.4;
		V2 b = s.ball_owner != -1 ? s.players[s.ball_owner].pos : (s.keeper >= 0 ? goal_of(s.keeper) : V2{ W / 2, H / 2 });
		double prog = (bt == HOME ? b.x : W - 1 - b.x) / double(W - 1);
		h += (bt == team ? 1.0 : -1.0) * 0.6 * prog;
		h += (s.turn_team == team ? 0.1 : -0.1) * s.pts;   // 이번 차례 남은 포인트
		for (const Player &q : s.players) {   // 수비 태세 존이 공 쪽으로 붙어 있나
			if (q.stance <= 0 || q.team == bt)
				continue;
			V2 z[4];
			const int nz = zone_tiles(q, z);
			bool face = false;
			for (int k = 0; k < nz; k++)
				face = face || manhattan(z[k], b) < manhattan(q.pos, b);
			h += face ? (q.team == team ? 0.05 : -0.05) : 0.0;
		}
		if (s.ball_owner >= 0) {   // 공 임자가 패스 범위로 골대에 닿는다 (골 직전 국면)
			const Player &o = s.players[s.ball_owner];
			if (goal_dist(opponent(o.team), o.pos) <= act_range(s, o, C_KICK))
				h += (o.team == team ? 1.0 : -1.0) * 0.8;
		}
	}
	return h;
}

} // namespace card
} // namespace cf
