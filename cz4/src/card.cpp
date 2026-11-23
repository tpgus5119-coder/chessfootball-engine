// 감독 모드 규칙 엔진 — card.h 설명 · docs/spec.md # 카드 모드 (11-19 카드 없는 버튼 방식)
// 구성: 상태·도우미 → 족보 → 선수 수치 → 배치 → 존·경합 → 길 → 액션 → 열거 → 평가
#include "card.h"

#include <cstring>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace cf {
namespace card {

std::vector<Event> *g_events = nullptr;
const std::vector<V2> *g_path = nullptr;

static const int DRAFT_POOL[CARD_POOL_N] = { CANNON, MAESTRO, TRICKSTER, SPRINTER, MARKER, RECEIVER, TITAN, HUNTER, WALL, NOMAD };   // 칸 번호 = 망 입력 자리 (리시버 = 옛 부스터 칸 · 노마드 칸 = 링커)

int pool_slot(int position) {
	for (int k = 0; k < CARD_POOL_N; k++)
		if (DRAFT_POOL[k] == position)
			return k;
	return -1;
}

static void emit(const Event &e) {
	if (g_events)
		g_events->push_back(e);
}

// ---------- 직업 능력 (11-19 사용자 둘째 결정 — 모두 상시, 게이지·켜기 없음) ----------
// 월·타이탄·마커 그대로 · 캐논 킥 힘 +1 · 마에스트로 킥 범위 +1 · 트릭스터 이동 힘 +1 · 스프린터 이동 범위 +1 · 헌터 수비 힘 +1 (늘).
// 만능 = 다섯 모두. 11-20 사용자 「부스터 노마드 빼서 8종」 — 둘은 감독 모드에 안 나온다 (풀 칸 5·9 는 망 입력·수 번호 자리 지키려 남김)

// 측정용 스위치 (환경 변수 — knobs_from_env). 규칙이 아니다: 사용자 11-21 비교 실험용
int g_zone4 = 0;         // 수비 존 = 곁 한 칸 방향 존 · 월만 제자리 사방 (11-23 사용자 「수비존 모두 1개로 · 월은 4방 기본」 — 규칙. 비교용 CF_ZONE4=1 = 모두 사방 4칸)
int g_keep_pts = 0;      // CF_KEEP_PTS=1: 자기 차례에 공을 뺏겨도 남은 포인트로 차례를 이어 간다
int g_kick_flat = 0;     // CF_KICK_FLAT=1: 킥 힘이 거리로 안 줄어듦
int g_goal_hp = 0;       // 골키퍼 체력 (11-23 — 측정: 체력 3 = 판당 6.6골 · 4 = 1.7골 · 5 = 0.7 · 6 = 0.2 · 8 = 0.15(비김 80 %) → 목표 1.5~2.5 에 맞는 4 · 측정용 CF_GOAL_HP)

static inline bool has_passive(int position, int want) { return position == want || position == ALLROUND; }

bool skill_job(int position) { return position == CANNON || position == MAESTRO || position == TRICKSTER || position == SPRINTER || position == HUNTER || position == ALLROUND; }
static bool sk_power(int pos, int sk, int use) {   // 이 수가 힘 +1 스킬인가
	if (!sk)
		return false;
	const bool all = pos == ALLROUND && sk == 1;
	const bool kick = use == U_PASS || use == U_SHOT, mv = use == U_MOVE || use == U_DRIBBLE;
	// 11-23 캐논 = 「슛이 거리로 안 약해짐」(act_power) · 링커 = 패스 힘 +1 (스루패스는 누구나 — 11-23 사용자) · 트릭스터 = 무브 힘 · 헌터 = 수비 힘 (중거리 슈터는 「슛이 거리로 안 약해짐」 — act_power · 만능은 다)
	return (use == U_PASS && pos == PASSER) || (kick && all) || (mv && (pos == TRICKSTER || all)) || (use == U_STANCE && (pos == HUNTER || all));
}
static bool sk_range(int pos, int sk, int want) {   // 이 수가 범위 +1 스킬인가
	if (!sk)
		return false;
	const bool all = pos == ALLROUND && sk == 2;
	// 11-22: 마에스트로 = 패스 범위 +1 · 스프린터 = 무브 범위 +1 (성장은 perks — kick_range_up · range_up)
	return (want == C_KICK && (pos == MAESTRO || all)) || (want == C_MOVE && (pos == SPRINTER || all));
}

int g_skill_pow = 1;
bool skill_can(const State &s, const Player &p) {
	(void)s; (void)p;
	return false;   // 11-19 사용자: 직업 능력은 모두 상시 — 켜는 수·게이지 없음
}

bool skill_act(int position, int act, int sk) {
	if (position == ALLROUND)
		return sk == 1 ? act >= A_MOVE && act <= A_STANCE : sk == 2 && act >= A_MOVE && act <= A_SHOT;
	if (sk != 1)
		return false;
	switch (position) {
		case CANNON: case MAESTRO: return act == A_PASS || act == A_SHOT;
		case TRICKSTER: case SPRINTER: return act == A_MOVE || act == A_DRIBBLE;
		case HUNTER: return act == A_STANCE;
		default: return false;
	}
}

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
	if (loose_ball())
		return loose_team;
	return opponent(first_team);   // 킥오프 전 = 후공 팀이 공격 측
}

static const Player *player_at(const State &s, V2 t) {
	for (const Player &q : s.players)
		if (q.placed() && q.pos == t)
			return &q;
	return nullptr;
}

int move_stat(const Player &p) { return std::max(1, BASE_MOVE + p.bonus[ST_DRIBBLE]); }

// ---------- 족보 (11-21 사용자 — 6개, 혜택 = 걸린 순간 행동 포인트 +1) ----------
JkInfo g_jk[N_JK] = {   // [이름, 켬] — 11-21 사용자 「족보도 없애봐」: 모두 끔 (장치·이름표는 남김 — 되살리면 1) — 화면 이름도 이 표 (CardNative.jk_table)
	{ "게겐프레싱", 0 },   // 수비·압박: 존이 공 임자 칸과 겹침 = 태클 시도 (이기든 지든)
	{ "철벽", 0 },         // 수비·버스: 태세를 잡을 때 존 칸이 아군 몸·존과 세로로 이어져 2명 이상
	{ "삼각 패스", 0 },    // 공격·점유: 패스 성공 · 받은 선수 2칸 안에 패스한 선수 아닌 아군 (거리 상관없음)
	{ "지원", 0 },         // 공격·점유: 공 임자 2칸 밖 아군이 2칸 안으로 (공 임자가 드리블로 다가가도)
	{ "전진 패스", 0 },    // 공격·직선: 3칸 이상 앞으로 간 패스가 동료에게 성공 (뒷공간 패스 포함)
	{ "돌파", 0 },         // 공격·직선: 한 번의 이동·드리블로 3칸 이상 앞으로 (끝까지)
};

bool jk_enabled(const State &s, int team, int k) {
	(void)s; (void)team;
	return k >= 0 && k < N_JK && g_jk[k].on;
}

// 족보 +1 토큰 (11-21 사용자 「혜택 = 스탯 +1, 당사자에게, 다음 상대 차례까지」): 그 선수 모든 판정 힘 +1 · 겹치지 않음(최대 +1).
// 이미 선 태세 힘·지금 버티는 힘(공 임자)에도 바로 +1. 우리 다음 차례 시작에 없어진다(turn_start — 붙인 만큼 되돌림)
static void jk_token(State &s, int uid) {
	if (uid < 0 || uid >= PLAYERS)
		return;
	Player &p = s.players[uid];
	if (p.jk_up)
		return;
	p.jk_up = 1;
	if (p.stance > 0)
		p.stance += 1;
	if (s.ball_owner == uid)
		s.hold += 1;
}

// 걸림: 당사자(u1 · 패스면 받은 선수 u2 도)에게 +1 토큰. 같은 족보는 한 차례에 한 번 — 남의 차례에 걸리면 그 팀 다음 차례의 「한 번」에 셈.
// 행동 포인트는 늘지 않는다 (11-21 사용자 — 옛 포인트 +1 없앰)
static void jk_fire(State &s, int team, int k, int u1, int u2 = -1) {
	if (s.phase != PLAY || !jk_enabled(s, team, k))
		return;
	const bool now = s.turn_team == team;
	int &bits = now ? s.jk_turn[team] : s.jk_next[team];
	if ((bits >> k) & 1)
		return;   // 같은 족보는 한 차례에 한 번
	bits |= 1 << k;
	s.jk_count[team][k] += 1;
	jk_token(s, u1);
	jk_token(s, u2);
	emit({ 7, team, k, UNSET, 0, 0, 0, {} });
}

void turn_clear(State &s) {
	s.jk_turn[0] = s.jk_turn[1] = 0;
	s.jk_next[0] = s.jk_next[1] = 0;
}

// 삼각 패스 (11-21 사용자 — 패스 거리 상관없음): 패스가 동료에게 성공 · 그 순간 받은 선수 2칸 안에 패스한 선수가 아닌 다른 아군 1명 이상
static bool triangle_hit(const State &s, int kicker, int recv) {
	const Player &r = s.players[recv];
	for (const Player &q : s.players)
		if (q.team == r.team && q.placed() && q.uid != kicker && q.uid != recv && manhattan(q.pos, r.pos) <= JK_NEAR)
			return true;
	return false;
}

// 패스(스루패스 포함)가 동료에게 닿은 뒤 — 삼각 패스 · 전진 패스
static void pass_jokbo(State &s, int kicker, int recv, V2 from, V2 dest) {
	const int team = s.players[recv].team;
	if (triangle_hit(s, kicker, recv))
		jk_fire(s, team, JK_TRIANGLE, kicker, recv);   // 찬 선수 · 받은 선수(버티는 힘 +1)
	if ((dest.x - from.x) * forward_dir(team) >= JK_AHEAD)
		jk_fire(s, team, JK_FORWARD, kicker, recv);
}

// 철벽: 새 태세의 존 칸에서 세로(같은 x, 위아래)로 아군 몸·존 칸이 이어진 줄에 서로 다른 아군 2명 이상
static bool wall_hit(const State &s, int uid) {
	const Player &p = s.players[uid];
	unsigned own[NT] = {};
	for (const Player &q : s.players) {
		if (q.team != p.team || !q.placed())
			continue;
		own[idx(q.pos)] |= 1u << q.uid;
		V2 z[4];
		const int nz = zone_tiles(q, z);
		for (int k = 0; k < nz; k++)
			own[idx(z[k])] |= 1u << q.uid;
	}
	V2 z[4];
	const int nz = zone_tiles(p, z);
	for (int k = 0; k < nz; k++) {
		unsigned m = own[idx(z[k])];
		for (int dy = -1; dy <= 1; dy += 2)
			for (V2 t{ z[k].x, z[k].y + dy }; in_bounds(t) && own[idx(t)]; t.y += dy)
				m |= own[idx(t)];
		if (__builtin_popcount(m) >= 2)
			return true;
	}
	return false;
}

// 지원: 공 임자와 아군 사이가 2칸 밖 → 2칸 안
static bool support_hit(V2 a0, V2 a1, V2 b0, V2 b1) { return manhattan(a0, b0) > JK_NEAR && manhattan(a1, b1) <= JK_NEAR; }

// 리시버 (11-24 사용자): 공을 잡을 때마다 버티는 힘 = 3 (+ 성장 1) — 어떻게 잡았든 · 태클로 깎이는 건 다른 선수와 같다
static int catch_hold(const Player &p, int v) { return p.position == RECEIVER ? RECEIVE_HOLD + p.hold_up : v; }

static void set_owner(State &s, int uid) {
	for (Player &q : s.players)
		if (q.uid != uid)
			q.charge = 0;   // 공을 놓으면 모은 힘은 사라진다
	s.ball_owner = uid;
	s.ball_pos = UNPLACED;
	s.loose_team = -1;
	s.hold = 0;
	if (uid != -1) {
		Player &p = s.players[uid];
		s.hold = catch_hold(p, act_power(s, p, U_DRIBBLE));   // 버티는 힘 = 공 가진 선수의 드리블 능력치 (11-21 사용자 — 거리 반비례 없이, 차례마다 줄지 않음)
		if (s.touch_team != p.team) {
			s.touch_team = p.team;
			s.touch_mask = 0;
			s.poss_turn = s.turn_count;
		}
		p.stance = 0;   // 공을 잡으면 태세가 풀린다
		s.touch_mask |= 1 << uid;
	}
}

// 경합(태클·가로채기·존)으로 공을 뺏었다 — 기록(대표 행동)만 (11-21 족보 6개는 뺏음을 따로 세지 않는다)
static void mark_steal(State &s) {
	if (s.ball_owner >= 0)
		rep_add(s, s.ball_owner, HUNTER);
	if (s.ball_owner >= 0)
		s.hold = catch_hold(s.players[s.ball_owner], act_power(s, s.players[s.ball_owner], U_STANCE));   // 막 뺏은 공 = 뺏은 선수의 태클(수비) 능력치로 버팀 (11-21 사용자) — 다음에 공을 다루면 드리블 능력치
}

bool kickoff_turn(const State &s) {
	return s.phase == PLAY && s.keeper < 0 && s.ball_owner == -1 && !s.loose_ball() && s.turn_team == opponent(s.first_team);
}

// 공 가진 팀 = 공격
static void sync_roles(State &s) {
	const int bt = s.ball_team();
	for (int t = 0; t < 2; t++)
		s.role[t] = t == bt ? R_ATK : R_DEF;
}

bool marked(const State &s, const Player &p) {   // 상대 마커(공 없음)에 상하좌우로 붙었다 — 모든 판정 힘 −1 (11-23 사용자 — 범위 −1 없앰 · 겹치지 않음 · 공 든 마커는 꺼짐)
	if (!p.placed())
		return false;
	for (const Player &m : s.players)
		if (m.team != p.team && has_passive(m.position, MARKER) && m.placed() && m.uid != s.ball_owner && manhattan(m.pos, p.pos) == 1)
			return true;
	return false;
}

// ---------- 선수 수치 (11-19) ----------
int g_range_bonus = 1;   // 직업 범위 스킬 크기 (측정용)
int g_air_max = 99;

static int skl_now(const State &s, const Player &p, int skl) {
	if (skl < 0)
		skl = s.sk_uid == p.uid ? s.sk_kind : 0;
	return skl;
}

int kick_stat(const State &s, const Player &p, int use) {
	(void)s;
	return BASE_KICK + p.bonus[use == U_SHOT ? ST_SHOT : ST_PASS];
}

// 판정 힘 — 늘 1 이상 (+ 족보 토큰). 킥·드리블은 거리에 반비례 (아래)
int act_power(const State &s, const Player &p, int use, int dist) {
	int v;
	const bool kick = use == U_PASS || use == U_SHOT;
	const int d = std::max(1, dist);
	if (kick) {
		v = kick_stat(s, p, use);
	} else if (use == U_STANCE) {
		v = BASE_DEF + p.bonus[ST_DEF];
	} else {
		v = move_stat(p);
	}
	if (sk_power(p.position, s.sk_passive ? 1 : skl_now(s, p, -1), use))
		v += g_skill_pow;
	// 거리 반비례: 힘 = 능력치 + 1 − 거리 (킥 4 → 1칸 4 · 2칸 3 · 3칸 2 · 4칸 1 · 드리블 3 → 3 · 2 · 1, 거리 0 = 공을 잡고 선 힘 = 능력치)
	// 측정용 CF_KICK_FLAT=1 이면 킥은 거리와 상관없이 수치 그대로
	const int dd = use == U_SHOT && p.position == CANNON ? 1 : d;   // 캐논 = 슛이 거리로 안 약해짐 (늘 1칸 힘 — 11-23 사용자 「캐논·스나이퍼 합쳐라 · 거리 반비례 없음 · 힘 +1 은 메인 스탯업」)
	if ((kick && !g_kick_flat) || use == U_DRIBBLE)
		v += 1 - dd;   // 힘 = 능력치 + 1 − 거리 (킥·드리블 같은 식 — 11-22, 킥 4 · 무브 3)
	if (p.charge == 2 && use != U_MOVE)
		v += 1;   // 「강하게」 = 포인트 2개로 힘 +1 (패스·슛·드리블·디펜스 — 이동은 없음)
	if (marked(s, p))
		v -= 1;   // 마커 곁 = 모든 판정 힘 −1 (슛·패스·드리블·디펜스 — 11-23 사용자 「인접 1칸 적 스탯 −1 · 공 들면 못 잡음 유지」 · 겹치지 않음)
	return std::max(1, v) + p.jk_up;   // 족보 +1 토큰은 거리로 깎인 뒤에 더한다 (먼 슛도 +1 그대로 — 최대 +1)
}

bool grow_ok(int pos, const int st[GROW_K], int k) {
	if (k < 0 || k >= GROW_K || pos == PROSPECT || st[k] >= GROW_MAX)
		return false;
	if (k == 4 && pos != MAESTRO && pos != SPRINTER && pos != WALL && pos != CANNON && pos != RECEIVER)
		return false;   // 리시버 = 버티기 +1 (11-24)   // 특수 = 마에스트로 패스 범위 · 스프린터 무브 범위 · 캐논 슛 범위 · 월 지속(1칸 이동해도 태세 유지) — 11-23 직업 정리
	const int never = pos == MAESTRO ? 1 : pos == SPRINTER ? 2 : -1;   // 마에스트로 패스 힘 · 스프린터 무브 힘은 못 키움 (캐논 슛 힘은 메인 스탯업, 11-23)
	return k != never;
}

int act_range(const State &s, const Player &p, int want, int skl) {
	skl = skl_now(s, p, skl);
	const bool sr = s.sk_passive ? sk_range(p.position, 1, want) || sk_range(p.position, 2, want) : sk_range(p.position, skl, want);
	int r;
	if (want == C_SHOT)
		r = KICK_RANGE + (p.position == CANNON ? p.kick_range_up : 0);   // 슛 범위 — 캐논만 성장으로 는다 (11-23 직업 정리)   // 슛 범위 4 — 중거리 슈터만 성장으로 는다 (perks, 11-22 넷째)
	else if (want == C_KICK)
		r = KICK_RANGE + (p.position == CANNON ? 0 : p.kick_range_up);   // 패스 범위 — 마에스트로 성장 (perks)
	else
		r = MOVE_RANGE + p.range_up;          // 무브 범위 3 고정 — 힘(무브 수치)과 따로 (11-22 사용자) · 스프린터 성장 (perks)
	if (sr)
		r += g_range_bonus;   // 마에스트로 패스 · 스프린터 무브 범위 +1 (11-22)
	return std::max(1, r);
}

int assist_value(const State &s, int team) {
	return s.touch_team == team ? std::min(s.team_n, __builtin_popcount(unsigned(s.touch_mask))) : 0;
}

// ---------- 배치 (본편 3.2와 같다) ----------

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

static bool is_drafted(const State &s, int position, int team) {   // 카드 모드는 팀 안에서만 겹치지 않는다 (9종 · v7.8)
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
// 유망주 칸 = 드래프트 풀 밖 선수(유망주 · 만능 11-17) — 만능은 망 입력·배치 수에서 유망주 칸을 같이 쓴다 (만능 표시는 card_az OFF_ALLROUND)
static int unplaced_extra(const State &s, int team) {
	const int u = unplaced_with(s, team, PROSPECT);
	return u >= 0 ? u : unplaced_with(s, team, ALLROUND);
}

static bool slot_open(const State &s, int team, int slot) {
	if (slot == SLOT_PROSPECT)
		return s.redeploy && unplaced_extra(s, team) != -1;
	return s.redeploy ? unplaced_with(s, team, DRAFT_POOL[slot]) != -1 : !is_drafted(s, DRAFT_POOL[slot], team);
}

static bool place_tile_ok(const State &s, int team, V2 t, bool nomad) {
	if (!is_own_half(team, t) || player_at(s, t) || is_goal(t))
		return false;
	return !is_any_box(t) || nomad;
}


static void turn_start(State &s, int team) {   // 우리 차례 시작: (태세는 움직이거나 질 때까지 유지 — 11-23) · 버티는 힘 −1 · 지나간 칸 새로 · 포인트 2 (남의 차례 족보는 덤 없음 — 상한 2)
	const int carried = s.jk_next[team];
	turn_clear(s);
	s.jk_turn[team] = carried;   // 남의 차례에 걸린 족보는 이 차례의 「한 번」
	for (Player &p : s.players)
		p.charge = 0, p.acted = 0;   // 「강하게」·행동 표시는 그 차례 안에서만 (11-22)
	for (Player &p : s.players)
		if (p.team == team) {
			if (p.jk_up) {   // 족보 +1 토큰은 「다음 상대 차례까지」 — 붙인 만큼 되돌린다
				p.jk_up = 0;
				if (p.stance > 0)
					p.stance = std::max(1, p.stance - 1);
				if (s.ball_owner == p.uid && s.hold > 0)
					s.hold -= 1;
			}
		}
	s.pts = POINTS_PER_TURN;   // 상한 2 (사용자 11-21) — 남의 차례에 걸린 족보는 포인트를 더하지 않고 이 차례의 「한 번」만 쓴다
	s.tturn[team] += 1;
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
		for (const Player &q : s.players) {   // 이 반의 첫 자리 — 골 뒤에 그대로 돌아온다
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
	if (team < 0 || !slot_open(s, team, slot) || !place_tile_ok(s, team, t, false))
		return false;
	int uid = s.redeploy ? (slot == SLOT_PROSPECT ? unplaced_extra(s, team) : unplaced_with(s, team, slot_pos(slot))) : next_free_uid(s, team);
	if (uid < 0)
		return false;
	commit_place(s, uid, s.redeploy ? s.players[uid].position : slot_pos(slot), t);
	return true;
}

// 골 뒤 (v8.3 사용자 "득점하면 다시 배치하지 말고 그 반 배치 그대로 · 득점한 팀 먼저 공 없이 · 실점한 팀 공 가지고 그 후에"):
// 그 반을 시작한 자리로 돌아가 넣은 팀이 공 없이 먼저 두고, 다음 차례에 실점한 팀이 킥오프한다 — 반 시작과 같은 꼴
static void refresh_if_stuck(State &s);

// 실점 팀 선수가 물러설 칸 — 대형 자리가 남았으면 거기, 득점 팀 선수가 서 있으면 우리 진영의 가장 가까운 빈 칸
// (같으면 우리 골 쪽 → 위쪽 줄). 박스·골대 칸은 놓을 때처럼 ×. taken = 이미 선 칸
static V2 back_tile(const State &s, int team, V2 want, const bool *taken) {
	if (!taken[idx(want)])
		return want;
	V2 best = want;
	int bd = 1 << 20, bb = 1 << 20;
	const int fwd = forward_dir(team);
	for (int z = 0; z < H; z++)
		for (int x = 0; x < W; x++) {
			const V2 t{ x, z };
			if (taken[idx(t)] || !is_own_half(team, t) || is_goal(t) || is_any_box(t))
				continue;
			const int d = manhattan(t, want), b = x * fwd;
			if (d < bd || (d == bd && b < bb))
				bd = d, bb = b, best = t;
		}
	return best;
}

// 그 반 첫 자리(킥오프 대형)로 — 공·콤보·가담·태세를 비우고 두 팀 손을 새로.
// stay = 제자리에 두는 팀 (v15c 골 뒤 득점 팀 — 사용자 11-11 「득점팀 자동으로 배치 위치로 가지 마라」), -1 = 두 팀 다 대형으로
static void reset_shape(State &s, int stay = -1) {
	bool taken[NT] = {};
	for (const Player &p : s.players)
		if (p.team == stay && p.placed())
			taken[idx(p.pos)] = true;
	Player *back[PLAYERS];
	int nb = 0;
	for (Player &p : s.players)
		if (p.team != stay)
			back[nb++] = &p;
	std::sort(back, back + nb, [&](const Player *a, const Player *b) {   // 대형 자리가 비어 있는 선수부터 · 그다음 먼저 놓인 순서
		const bool fa = s.half_pos[a->uid] != UNPLACED && !taken[idx(s.half_pos[a->uid])];
		const bool fb = s.half_pos[b->uid] != UNPLACED && !taken[idx(s.half_pos[b->uid])];
		return fa != fb ? fa : s.half_order[a->uid] < s.half_order[b->uid];
	});
	for (int i = 0; i < nb; i++) {
		Player &p = *back[i];
		p.pos = s.half_pos[p.uid];
		p.placed_order = s.half_order[p.uid];
		if (stay >= 0 && p.pos != UNPLACED) {
			p.pos = back_tile(s, p.team, p.pos, taken);
			taken[idx(p.pos)] = true;
		}
	}
	for (Player &p : s.players) {   // 제자리 팀은 놓인 순서도 그대로 (일기토처럼 판 밖 선수가 있는 판에서 판 밖 선수를 세우지 않게)
		p.stance = 0;
		p.stance_left = 0;
		p.charge = 0;
		p.acted = 0;
	}
	s.ball_owner = -1;
	s.ball_pos = UNPLACED;
	s.loose_team = -1;
	s.keeper = -1;
	s.hold = 0;
	s.touch_team = -1;
	s.touch_mask = 0;
	s.poss_turn = s.turn_count;
}


// 반 시작(후반)·골 뒤 (11-22 사용자 「골 먹히면 킥오프」): 두 팀 모두 킥오프 대형 · scorer 가 공 없이 먼저, 상대(실점 팀)가 킥오프
static void restart_half(State &s, int scorer) {
	reset_shape(s);
	for (int t = 0; t < 2; t++)
		s.ghp[t] = g_goal_hp ? g_goal_hp : s.gk_def[t];   // 킥오프마다 두 골대 체력 다시 참 = 골키퍼 디펜스 (11-24)
	s.first_team = scorer;
	s.role[scorer] = R_DEF;
	s.role[opponent(scorer)] = R_ATK;
	s.turn_team = scorer;
	turn_start(s, scorer);
	refresh_if_stuck(s);
}

void new_match(State &s, int first) {
	s = State();
	for (int u = 0; u < PLAYERS; u++) {
		s.players[u].uid = u;
		s.players[u].team = u < PER_TEAM ? HOME : AWAY;
	}
	s.first_team = first;
	s.opener = first;
	for (int t = 0; t < 2; t++)
		s.ghp[t] = g_goal_hp ? g_goal_hp : s.gk_def[t];
	s.role[first] = R_DEF;
	s.role[opponent(first)] = R_ATK;
	s.turn_team = placement_team(s);
}

void set_lineup(State &s, int team, const int position[PER_TEAM], const int bonus[PER_TEAM][4]) {
	for (int i = 0; i < PER_TEAM; i++) {
		Player &p = s.players[team * PER_TEAM + i];
		p.position = i < s.team_n ? position[i] : -1;   // 출전 인원 밖(후보)은 판에 안 선다
		for (int k = 0; k < 4; k++)
			p.bonus[k] = bonus[i][k];
		p.range_up = p.hold_up = p.kick_range_up = 0;
		p.pos = UNPLACED;
		p.placed_order = -1;
	}
	s.redeploy = 1;   // 놓기만 — 포지션은 정해졌다
}

void set_perks(State &s, int team, const int range_up[PER_TEAM], const int hold_up[PER_TEAM], const int kick_range_up[PER_TEAM]) {
	for (int i = 0; i < PER_TEAM; i++) {
		Player &p = s.players[team * PER_TEAM + i];
		p.range_up = range_up[i];
		p.kick_range_up = kick_range_up ? kick_range_up[i] : 0;
		p.hold_up = hold_up[i];
	}
}

void set_keeper(State &s, int team, int def, int kick, int range) {
	if (team != HOME && team != AWAY)
		return;
	s.gk_def[team] = std::max(1, def);
	s.gk_kick[team] = std::max(1, kick);
	s.gk_range[team] = std::max(1, range);
	if (s.phase != PLAY)
		s.ghp[team] = g_goal_hp ? g_goal_hp : s.gk_def[team];
}

void set_trial(State &s, int team_n) {
	s.team_n = std::max(1, std::min(PER_TEAM, team_n));
	s.sk_passive = 1;
	s.turn_team = placement_team(s);
}

// ---------- 시간 ----------

int g_half_turns = CARD_HALF_TURNS;
int g_time_input = 1;
void knobs_from_env() {
	auto num = [](const char *k, int &v) { if (const char *e = getenv(k)) v = atoi(e); };
	num("CF_HALF_TURNS", g_half_turns);
	num("CF_ZONE4", g_zone4);
	num("CF_KEEP_PTS", g_keep_pts);
	num("CF_KICK_FLAT", g_kick_flat);
	num("CF_GOAL_HP", g_goal_hp);
	if (getenv("CF_NO_TIME") && atoi(getenv("CF_NO_TIME")))
		g_time_input = 0;
	if (const char *e = getenv("CF_JK"))   // 족보 켬 덮어쓰기 (측정용): "1,1,0,1,1,1" — card.h Jokbo 순서
		for (int i = 0; i < N_JK && *e; i++) {
			g_jk[i].on = atoi(e);
			e = strchr(e, ',');
			if (!e)
				break;
			e++;
		}
}

// 승패: 골이 많은 팀 → 같으면 무승부 (컵 녹아웃만 화면이 일기토)
static void full_time(State &s) {
	s.phase = GAME_OVER;
	if (s.goals[0] != s.goals[1])
		s.winner = s.goals[0] > s.goals[1] ? HOME : AWAY;
	else
		s.winner = -1;   // 골 수가 같으면 무승부 (11-21 슛 데미지 없음 — 옛 「넣은 골 데미지 합」 순위 없앰)
}

// 한 판 = 팀당 30차례 · 먼저 3골이면 바로 끝 (11-24 사용자 「전후반 없애고 ㄱ」 — 옛 후반 재배치 start_half2 없앰)
static bool clock(State &s) {
	if (s.goals[HOME] < WIN_GOALS && s.goals[AWAY] < WIN_GOALS && s.half_turn() < g_half_turns)
		return false;
	full_time(s);
	return true;
}

void end_turn(State &s) {
	if (s.phase != PLAY)
		return;
	for (const Player &p : s.players)   // 월 대표 행동 — 우리 차례 둘을 이어 태세로 끝냄 (기록)
		if (p.team == s.turn_team && p.stance > 0) {
			if (s.st_turn[p.uid] > 0 && s.st_turn[p.uid] == s.tturn[p.team] - 1)
				rep_add(s, p.uid, WALL);
			s.st_turn[p.uid] = s.tturn[p.team];
		}
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
		if (p.team != s.turn_team || !p.placed() || ko)
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

// 둘 수가 없으면 그 차례는 저절로 끝 (11-21 — 차례 끝 수는 없다, 포인트를 다 쓴다)
static void refresh_if_stuck(State &s) {
	if (s.phase != PLAY || s.pts < 1 || any_action(s))
		return;
	end_turn(s);
}

static void after_action(State &s, int actor) {
	const int r0 = actor >= 0 && actor < 2 ? s.role[actor] : -1;
	sync_roles(s);
	if (!g_keep_pts && r0 == R_ATK && s.role[actor] == R_DEF && s.phase == PLAY && s.turn_team == actor) {
		s.pts = 0;   // 자기 차례에 공을 뺏기면 남은 포인트는 사라지고 바로 상대 차례 (v15) — CF_KEEP_PTS=1 이면 남은 포인트로 이어 둔다(측정용, 11-21 「즉시 되찾기」)
		end_turn(s);
		return;
	}
	if (s.phase == PLAY && s.pts <= 0)
		end_turn(s);
	else
		refresh_if_stuck(s);
}

// ---------- 존 · 경합 ----------

// 태세 선수의 존 칸 (11-21 사용자): 고른 곁 한 칸 · 월은 사방 4칸. 판 밖·골대 칸은 빠진다 (박스 칸은 그대로 존)
int zone_tiles(const Player &p, V2 out[4]) {
	if (p.stance <= 0 || !p.placed())
		return 0;
	int n = 0;
	for (int k = 0; k < 4; k++) {
		if (p.zdir != ZD_ALL && p.zdir != k)
			continue;
		const V2 t = p.pos + DIRS[k];
		if (in_bounds(t) && !is_goal(t) && !is_any_box(t))
			out[n++] = t;   // 박스 안에는 존이 없다 (11-24 사용자 — 박스는 골키퍼의 땅)
	}
	return n;
}

static bool zone_all(const Player &p) { return g_zone4 || has_passive(p.position, WALL); }   // 월만 4방 존 (11-23) · 태세는 움직이거나 질 때까지

int stance_dir(const Player &p, V2 dest) {
	if (zone_all(p))
		return dest == p.pos ? ZD_ALL : -1;   // 월 = 방향 없이 제자리 (사방)
	for (int k = 0; k < 4; k++)
		if (dest == p.pos + DIRS[k])
			return in_bounds(dest) && !is_goal(dest) && !is_any_box(dest) ? k : -1;
	return -1;
}

static bool in_zone(const Player &p, V2 t) {
	V2 z[4];
	const int n = zone_tiles(p, z);
	for (int i = 0; i < n; i++)
		if (z[i] == t)
			return true;
	return false;
}

static void build_zone(const State &s, int team, int zone[NT]) {   // 태세 선수의 존 칸 — 겹치면 먼저 놓인 선수가 주인
	for (int i = 0; i < NT; i++)
		zone[i] = -1;
	const Player *w[PER_TEAM * 2];
	int n = 0;
	for (const Player &q : s.players)
		if (q.team == team && q.placed() && q.stance > 0)
			w[n++] = &q;
	std::sort(w, w + n, [](const Player *a, const Player *b) { return a->placed_order < b->placed_order; });
	for (int i = 0; i < n; i++) {
		V2 z[4];
		const int nz = zone_tiles(*w[i], z);
		for (int k = 0; k < nz; k++)
			if (zone[idx(z[k])] == -1)
				zone[idx(z[k])] = w[i]->uid;
	}
}

static bool screened(const State &s, const Player &p, V2 tile, V2 last) {   // 길 위 아군이 선 칸(끝 칸 제외)은 경합 없음
	if (tile == last)
		return false;
	const Player *q = player_at(s, tile);
	return q && q->team == p.team && q->uid != p.uid;
}

// 존 경합: 공 든 쪽이 건다 → 가만히 있던 수비수가 당한 쪽 — **동점이면 당한 쪽(수비)이 이긴다**
static bool zone_contest(State &s, const Player &atk, int atk_val, int du, V2 tile) {
	const Player &def = s.players[du];
	int dv = def.stance;   // 협동 수비는 없다 (사용자 2026-10-29 "카드모드에서는 협동수비 없다")
	bool win = atk_val > dv;
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

// 길의 경합 미리 보기 (사용자 11-18 「경합이 있는 경로는 피해야지. 무조건. 불가피한 경우에만 가장 실점 확률이 적은 경합 칸으로」).
// dribble_run·send_ball 과 같은 판정을 상태를 바꾸지 않고 센다: 존 칸 · 수비수마다 한 번(met) · 스크린(끝 칸 제외 아군 칸) ·
// 이기려면 힘 > 태세(동점은 수비). 지면 거기서 공이 넘어가 끝난다. arm = 이 칸을 지난 뒤부터 경합(루즈볼 줍는 이동), UNSET = 처음부터
struct Foe {
	const State *s;
	const int *zone;   // null = 경합 없음 (공 없는 이동)
	int uid, val;
	V2 arm;     // UNSET = 첫 칸부터 경합 · 아니면 이 칸(루즈볼)을 밟은 뒤부터
	bool carry;   // 공을 든 선수가 뺏기는 칸에 선다 (드리블·줍는 이동) — 사이 아군에서 자신은 뺀다
	int pick;   // 루즈볼 이동: 1 = 줍는 길이 낫다 · −1 = 줍지 말아야 한다(박스 도착) · 0 = 상관없음
};

// 망이 없을 때의 마지막 대비책 순서 (사용자 11-18 — 길은 원래 간 뒤의 상태로 고른다: value_path). 작을수록 좋다:
// (a) 지는 경합 (b) 뺏기는 칸보다 우리 골 쪽 아군이 많은 길 (c) 뺏기는 칸이 우리 골에서 먼 길 (d) 걸음 (e) 위아래 대칭 동점 깨기.
// 이기는 경합은 감점하지 않는다. 루즈볼 이동은 안 지면 줍는 길(pick)
struct PathKey {
	int lose = 0, pick = 0, left = 0, cover = 0, near = 0, len = 0, mid = 0;   // left = −남은 힘 (이긴 만큼만 남음 — 11-23, 남은 힘이 큰 길)
};

static PathKey path_key(const Foe *f, V2 from, const std::vector<V2> &p) {
	PathKey k;
	k.len = int(p.size());
	for (const V2 &t : p)
		k.mid += std::abs(2 * t.y - (H - 1));   // 가운데 줄에서 먼 만큼 (위아래 대칭)
	if (!f)
		return k;
	const State &s = *f->s;
	const Player &me = s.players[f->uid];
	bool met[PLAYERS] = {}, armed = f->arm == UNSET;
	int cur = f->val;
	if (f->pick)
		k.pick = (std::find(p.begin(), p.end(), f->arm) != p.end()) == (f->pick > 0) ? 0 : 1;
	if (!f->zone)
		return k;
	for (const V2 &t : p) {
		if (!armed) {
			armed = t == f->arm;
			continue;
		}
		int du = f->zone[idx(t)];
		if (du == -1 || met[du] || screened(s, me, t, p.back()))
			continue;
		met[du] = true;
		if (cur <= s.players[du].stance) {   // 동점은 수비 (zone_contest)
			k.lose = 1;
			const int fwd = forward_dir(me.team);
			for (const Player &q : s.players)
				if (q.team == me.team && q.placed() && !(f->carry && q.uid == me.uid) && (t.x - q.pos.x) * fwd > 0)
					k.cover--;
			k.near = -goal_dist(me.team, t);   // 우리 골에서 가까울수록 나쁨
			break;
		}
		cur -= s.players[du].stance;   // 이긴 만큼 남음 (11-23)
	}
	k.left = k.lose ? 0 : -cur;
	return k;
}

static bool key_less(const PathKey &a, const std::vector<V2> &pa, const PathKey &b, const std::vector<V2> &pb) {
	if (a.lose != b.lose) return a.lose < b.lose;
	if (a.pick != b.pick) return a.pick < b.pick;
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
				PathKey k = path_key(f, from, cur);
				if (best.empty() || key_less(k, cur, bk, best))
					best = cur, bk = k;
			},
			[&](int lb) { return !best.empty() && bk.lose == 0 && bk.pick == 0 && bk.left == (f && f->zone ? -f->val : 0) && lb > bk.len; });   // 경합 없이 안 지는 길이 이미 있다 — 더 긴 길은 볼 것 없다
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

// 그린 길이 있으면 그것(쓸 수 없으면 빈 길 = 실행 안 함), 없으면 엔진이 고른 길
static std::vector<V2> route(V2 from, V2 dest, int max_steps, const Foe *danger, const bool *blocked, V2 goal_dest);

static void body_blocked(const State &s, int team, bool blocked[NT]) {   // 적의 몸과 골대는 벽 · 아군 몸은 지나간다
	for (int i = 0; i < NT; i++)
		blocked[i] = false;
	for (int k = 0; k < GOAL_N; k++)
		blocked[idx(goal_tile(HOME, k))] = blocked[idx(goal_tile(AWAY, k))] = true;
	for (const Player &q : s.players)
		if (q.placed() && q.team != team)
			blocked[idx(q.pos)] = true;
}

static std::vector<V2> route(V2 from, V2 dest, int max_steps, const Foe *danger, const bool *blocked, V2 goal_dest) {
	if (g_path)
		return path_ok(from, *g_path, dest, max_steps, blocked, goal_dest) ? *g_path : std::vector<V2>();
	return best_path(from, dest, max_steps, danger, blocked, goal_dest);
}

// 엔진이 고르는 이동 길. 루즈볼이 있으면: 주운 뒤 남은 걸음은 드리블이라 그 존 경합을 보고, 지지 않으면 줍는 길을 고른다
// (박스 도착이면 줍는 길은 안 된다 — 줍지 않는 길). 공 없으면 최단
static std::vector<V2> move_path(const State &s, const Player &p, V2 dest, int rng, int val, const bool *blocked) {
	if (!s.loose_ball())
		return best_path(p.pos, dest, rng, nullptr, blocked, UNSET);
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	const Foe f{ &s, zone, p.uid, val, s.ball_pos, true, is_any_box(dest) ? -1 : 1 };
	return best_path(p.pos, dest, rng, &f, blocked, UNSET);
}

static bool move_dest_ok(const State &s, const Player &p, V2 t) {
	if (player_at(s, t) || is_goal(t))
		return false;
	return !is_any_box(t);
}

static bool pushable(const State &s, const Player &q, V2 to) {
	if (!in_bounds(to) || player_at(s, to) || is_goal(to))
		return false;
	return !is_any_box(to);
}

// 타이탄 착지 밀기 — 도착 칸 상하좌우의 적 모두를 1칸 바깥으로 (본편 5.9와 같다). 밀린 선수는 태세가 풀린다
static void titan_push(State &s, const Player &p) {
	if (!has_passive(p.position, TITAN) || s.ball_owner == p.uid)
		return;
	for (const V2 &d : DIRS) {
		const Player *q = player_at(s, p.pos + d);
		if (!q || q->team == p.team || !pushable(s, *q, q->pos + d))
			continue;
		Player &qm = s.players[q->uid];
		qm.pos = qm.pos + d;
		qm.stance = 0;
		qm.stance_left = 0;
		emit({ 1, qm.uid, -1, qm.pos, 0, 0, 0, {} });
	}
}

// 길 벽: 적의 몸·골대 (11-22 되돌아가기 금지 없앰 — 한 선수 행동 하나로 필요 없음)
static void path_blocked(const State &s, int team, int, bool, bool blocked[NT]) {
	body_blocked(s, team, blocked);
}

// 드리블 길의 존 경합. 끝 칸(뺏기면 그 칸)을 돌려준다
// 존을 이기면 이긴 만큼만 힘이 남아 다음 존과 겨룬다 (11-23 사용자 「인터셉트 존을 2개 이상 만나면 차감되는 만큼 남고 다음 대결」) — val 은 남은 힘으로 바뀐다
static V2 dribble_run(State &s, int uid, int &val, const std::vector<V2> &path) {
	int zone[NT];
	build_zone(s, opponent(s.players[uid].team), zone);
	bool met[PLAYERS] = {};
	for (const V2 &t : path) {
		int du = zone[idx(t)];
		if (du == -1 || met[du] || screened(s, s.players[uid], t, path.back()))
			continue;
		met[du] = true;
		if (!zone_contest(s, s.players[uid], val, du, t)) {
			set_owner(s, du);   // 수비수가 제 칸에서 공을 갖는다
			mark_steal(s);
			return t;
		}
		val -= s.players[du].stance;   // 이긴 만큼 남음
		s.players[du].stance = 0;   // 진 수비수는 태세가 풀린다
		s.players[du].stance_left = 0;
	}
	return path.back();
}

// ---------- 태클 (태세 선수의 존 칸에 상대 공 임자가 있으면 저절로 — 태세를 켤 때 · 태세로 이동해 닿을 때) ----------
// 11-21 사용자: 고른 방향 칸이 공 임자 칸일 때만 (월은 사방). 다른 방향이면 곁에 있어도 태클 없음
// 태클러 = 태세 힘 · 공 임자 = 버티는 힘(공을 다룬 직전 숫자 — s.hold). 동점이면 당한 쪽(공 임자)이 이긴다
static bool tackle_ready(const State &s, const Player &t) {   // 태세 선수의 존 칸에 상대 공 임자가 섰다
	if (t.stance <= 0 || s.ball_owner == -1 || s.ball_owner == t.uid || is_any_box(t.pos))
		return false;
	const Player &h = s.players[s.ball_owner];
	return h.team != t.team && in_zone(t, h.pos);
}

// 태클 한 번 (11-22 사용자): 힘 tv > 버티는 힘이면 뺏음 · 아니면 버티는 힘을 tv 만큼 깎는다(HP — 패스를 받거나 새로 잡을 때만 다시 참) · 태클한 수비수 태세는 풀림
static void tackle_do(State &s, int tuid, int tv) {
	const Player &t = s.players[tuid];
	jk_fire(s, t.team, JK_GEGEN, tuid);   // 게겐프레싱 = 태클 시도 (족보 꺼짐 — 늘 그냥 지나감)
	const Player &h = s.players[s.ball_owner];
	const int hv = eff_hold(s);
	const bool win = tv > hv;
	emit({ 0, tuid, h.uid, h.pos, tv, hv, win ? 1 : 0, {} });
	if (win) {
		set_owner(s, tuid);
		mark_steal(s);
	} else {
		s.hold = std::max(0, hv - tv);
		s.players[tuid].stance = 0;
		s.players[tuid].stance_left = 0;
	}
}

static void auto_tackle(State &s, int tuid) {   // 곁에서 디펜스 = 제자리 태클 (1칸 다가간 태클과 같은 힘 = 태세 힘)
	if (tackle_ready(s, s.players[tuid]))
		tackle_do(s, tuid, s.players[tuid].stance);
}


// ---------- 액션 ----------

static bool can_act(const State &s, const Player &p) {
	return s.phase == PLAY && s.keeper != s.turn_team && p.placed() && p.team == s.turn_team && s.pts >= 1;   // (우리 키퍼가 공을 쥐고 있으면 롱킥부터) · 한 선수가 여러 번 행동해도 된다 (11-23 사용자 「다 ㄱ」 — 옛 한 선수 행동 하나 없앰)
}

static bool may_carry(const State &s, const Player &p) {   // 공 임자 · 킥오프 차례면 킥오프 팀 누구나
	return s.ball_owner == p.uid || (kickoff_turn(s) && p.team == s.turn_team);
}

static void pay_point(State &s) { s.pts -= 1; }

// 이동 길의 벽 = 적의 몸·골대
static void move_blocked(const State &s, const Player &p, bool blocked[NT]) {
	path_blocked(s, p.team, p.uid, false, blocked);
}

static bool do_move(State &s, int uid, V2 dest) {
	const Player &p = s.players[uid];
	if (!can_act(s, p) || kickoff_turn(s) || s.ball_owner == uid || !move_dest_ok(s, p, dest))
		return false;
	bool blocked[NT];
	move_blocked(s, p, blocked);
	const int rng = act_range(s, p, C_MOVE);
	int pow0 = act_power(s, p, U_DRIBBLE);
	std::vector<V2> path = g_path ? route(p.pos, dest, rng, nullptr, blocked, UNSET) : move_path(s, p, dest, rng, pow0, blocked);
	if (path.empty())
		return false;
	int pk = -1;
	if (s.loose_ball())
		for (size_t k = 0; k < path.size(); k++)
			if (path[k] == s.ball_pos) {
				pk = int(k);
				break;
			}
	if (pk >= 0 && is_any_box(dest))
		return false;   // 공을 든 채 박스에 멈추지 못한다
	const int team = p.team;
	const V2 from = p.pos;
	const bool atk = s.ball_team() == team;
	const int own = s.ball_owner >= 0 && s.players[s.ball_owner].team == team ? s.ball_owner : -1;
	emit({ 4, uid, -1, dest, 0, 0, 0, path });
	pay_point(s);
	if (pk >= 0) {   // 루즈볼 줍기 — 남은 걸음은 드리블
		set_owner(s, uid);
		s.players[uid].pos = path[size_t(pk)];
		if (size_t(pk) + 1 < path.size())
			dest = dribble_run(s, uid, pow0, std::vector<V2>(path.begin() + pk + 1, path.end()));
		s.players[uid].pos = dest;
	} else {
		s.players[uid].pos = dest;
		const Player &me = s.players[uid];
		const bool at_h = s.ball_owner >= 0 && s.players[s.ball_owner].team != team && manhattan(dest, s.players[s.ball_owner].pos) == 1;
		if (!at_h)   // 공 임자 곁에 닿으면 밀지 않는다
			titan_push(s, me);
	}
	{   // 움직이면 태세가 풀린다 (11-23 사용자 「태세는 다른 행동 전까지 유지」) — 월은 지속 성장이 있으면 1칸 이동해도 유지
		// 태세 → 이동 = 태클 (11-23 사용자 「태세+이동도 태클」): 태세 중이던 선수가 움직여 상대 공 임자 곁에 서면 태클 · 힘 = 태세 힘 + 1 − 움직인 칸 (킥·드리블과 같은 거리 감쇠)
		Player &mv = s.players[uid];
		const int st0 = mv.stance;
		if (mv.stance > 0 && !(has_passive(mv.position, WALL) && mv.hold_up > 0 && path.size() == 1))
			mv.stance = 0, mv.stance_left = 0;
		if (st0 > 0 && pk < 0 && s.ball_owner >= 0 && s.players[s.ball_owner].team != team && manhattan(mv.pos, s.players[s.ball_owner].pos) == 1 && !is_any_box(mv.pos))
			tackle_do(s, uid, std::max(1, st0 + 1 - int(path.size())));
	}
	const V2 end = s.players[uid].pos;
	const bool done = end == path.back() && (pk < 0 || s.ball_owner == uid);   // 끝까지 간 이동만 족보
	if (done && own >= 0 && s.ball_owner == own && support_hit(from, end, s.players[own].pos, s.players[own].pos))
		jk_fire(s, team, JK_SUPPORT, uid);   // 지원: 공 임자 2칸 밖에서 2칸 안으로
	if (done && atk && (end.x - from.x) * forward_dir(team) >= JK_AHEAD)
		jk_fire(s, team, JK_BREAK, uid);     // 돌파 (공 없는 침투 포함)
	if (path.size() >= 3 && end == path.back() && (path.back().x - from.x) * forward_dir(team) > 0)
		rep_add(s, uid, SPRINTER);
	after_action(s, team);
	return true;
}

static bool do_dribble(State &s, int uid, V2 dest) {
	const Player &p = s.players[uid];
	if (!can_act(s, p) || !may_carry(s, p) || player_at(s, dest) || is_any_box(dest) || is_goal(dest))
		return false;
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool blocked[NT];
	path_blocked(s, p.team, uid, true, blocked);
	int pow = act_power(s, p, U_DRIBBLE, manhattan(p.pos, dest));   // 드리블 힘 = 가는 거리에 반비례 (11-21)
	const Foe f{ &s, zone, uid, pow, UNSET, true, 0 };
	std::vector<V2> path = route(p.pos, dest, act_range(s, p, C_MOVE), &f, blocked, UNSET);
	if (path.empty())
		return false;
	const int team = p.team;
	const V2 from = p.pos;
	emit({ 4, uid, -1, dest, pow, 0, 0, path });
	if (s.ball_owner == -1)
		set_owner(s, uid);   // 킥오프 — 이 선수가 공을 갖고 시작한다
	pay_point(s);
	const V2 end = dribble_run(s, uid, pow, path);
	s.players[uid].pos = end;
	if (s.ball_owner == uid) {
		s.hold = catch_hold(s.players[uid], pow);   // 드리블 뒤 버티는 힘 = 그 드리블의 판정 힘 (11-22 사용자 — 태클로 깎이고 다음 드리블·패스 받기 때 그 값으로)
		if ((end.x - from.x) * forward_dir(team) > 0)
			rep_add(s, uid, TRICKSTER);
		if (end == path.back()) {   // 끝까지 간 드리블만 족보
			for (const Player &q : s.players)
				if (q.team == team && q.placed() && q.uid != uid && support_hit(q.pos, q.pos, from, end)) {
					jk_fire(s, team, JK_SUPPORT, uid);   // 지원: 공 임자가 2칸 밖 동료의 2칸 안으로
					break;
				}
			if ((end.x - from.x) * forward_dir(team) >= JK_AHEAD)
				jk_fire(s, team, JK_BREAK, uid);
		}
	}
	after_action(s, team);
	return true;
}

static void pass_targets(const State &s, const Player &p, int rng, Reach &ground, Reach &air) {
	bool blocked[NT];
	path_blocked(s, p.team, -1, true, blocked);
	for (int k = 0; k < GOAL_N; k++)
		blocked[idx(goal_tile(HOME, k))] = blocked[idx(goal_tile(AWAY, k))] = true;
	reach(p.pos, rng, blocked, UNSET, ground);
	if (rng >= LONG_PASS_MIN) {
		bool open[NT] = {};
		reach(p.pos, rng, open, UNSET, air);
	}
}

// 패스 받을 칸 = 동료(나 아님) 칸 — 오프사이드 없음(11-21 사용자). 빈 칸 패스 = 스루패스(through_ok — 11-22 밤). 공이 이번 차례 있었던 칸 ×
// ---------- 스루패스 (11-22 밤 사용자 「주우러 가는 것도 포인트 · 포인트 1개 남았으면 적 이동 범위 밖 칸만」) ----------
// 스루패스 = 앞쪽 **빈 칸**으로 차는 패스(1포인트) — 누구나 (11-23 사용자 「스루패스 모두 가능」). 공은 그 칸에 루즈볼로 남고,
// 동료가 이동으로 주워야 내 공이 된다(그것도 포인트 1 · 그 동료의 행동). 칸 조건: 앞쪽 빈 칸 · 박스·골대 아님 · 동료(차는 선수 말고) 하나가 뛰어 닿음 ·
// **차는 수가 이번 차례 마지막 포인트면 적 누구의 이동 범위에도 들지 않는 칸** (상대가 먼저 줍지 못하게 — 줍는 건 다음 우리 차례)
static bool through_ok(const State &s, const Player &p, V2 t) {
	if (!in_bounds(t) || player_at(s, t) || is_any_box(t) || is_goal(t) || (t.x - p.pos.x) * forward_dir(p.team) <= 0)
		return false;
	bool mate = false;
	for (const Player &m : s.players)
		if (m.team == p.team && m.uid != p.uid && m.placed() && manhattan(m.pos, t) <= act_range(s, m, C_MOVE)) {
			bool blocked[NT];
			path_blocked(s, m.team, m.uid, false, blocked);
			if (!best_path(m.pos, t, act_range(s, m, C_MOVE), nullptr, blocked, UNSET).empty()) {
				mate = true;
				break;
			}
		}
	if (!mate)
		return false;
	if (s.pts <= 1)
		for (const Player &q : s.players)
			if (q.team != p.team && q.placed() && manhattan(q.pos, t) <= act_range(s, q, C_MOVE))
				return false;   // 마지막 포인트 = 적 이동 범위 밖만
	return true;
}

static bool pass_dest_ok(const State &s, const Player &p, V2 t) {
	if (is_any_box(t) || is_goal(t))
		return false;
	const Player *r = player_at(s, t);
	return r ? r->team == p.team && r->uid != p.uid : through_ok(s, p, t);
}

static bool send_ball(State &s, const Player &p, int &val, const std::vector<V2> &path) {   // true = 끊겼다 · val = 남은 힘 (이긴 만큼만 남음, 11-23)
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool met[PLAYERS] = {};
	for (const V2 &t : path) {
		int du = zone[idx(t)];
		if (du == -1 || met[du] || screened(s, p, t, path.back()))
			continue;
		met[du] = true;
		if (!zone_contest(s, p, val, du, t)) {
			set_owner(s, du);
			mark_steal(s);
			return true;
		}
		val -= s.players[du].stance;   // 이긴 만큼 남음
		s.players[du].stance = 0;
		s.players[du].stance_left = 0;
	}
	return false;
}

// 킥의 공 길 (패스·뒷공간 패스 공통): 5칸 이상 = 공중볼(떨어지는 칸만), 아니면 땅볼 길. 못 차면 빈 길
static std::vector<V2> kick_path(const State &s, const Player &p, V2 dest, int pow) {
	const int rng = act_range(s, p, C_KICK);
	const int dist = manhattan(p.pos, dest);
	if (dist > rng)
		return {};
	if (dist >= LONG_PASS_MIN)
		return dist > g_air_max ? std::vector<V2>() : std::vector<V2>{ dest };
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool blocked[NT];
	path_blocked(s, p.team, -1, true, blocked);
	const Foe f{ &s, zone, p.uid, pow, UNSET, false, 0 };
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
	const Player *rp = player_at(s, dest);
	const int ru = rp ? rp->uid : -1;   // −1 = 스루패스 (빈 칸 — 루즈볼로 남는다)
	emit({ 4, uid, -1, dest, pow, aerial ? 1 : 0, 0, path });
	if (s.ball_owner == -1)
		set_owner(s, uid);
	pay_point(s);
	const bool cut = send_ball(s, p, pow, path);
	if (!cut && ru >= 0) {
		set_owner(s, ru);
		s.hold = catch_hold(s.players[ru], pow);   // 받은 선수 버티는 힘 = 그 패스의 판정 힘 (11-22 사용자 「패스 받은 상태는 그 패스 값」 — 멀리서 온 공은 약하게 받음)
		pass_jokbo(s, uid, ru, from, dest);
	} else if (!cut) {   // 스루패스 — 그 칸에 루즈볼 (우리 공 차례는 이어진다 · 줍는 건 이동)
		for (Player &q : s.players)
			q.charge = 0;
		s.ball_owner = -1;
		s.ball_pos = dest;
		s.loose_team = team;
		s.hold = 0;
	}
	if (s.ball_team() == team && (dest.x - from.x) * forward_dir(team) > 0)
		rep_add(s, uid, MAESTRO);
	after_action(s, team);
	return true;
}

// 슛할 골대 칸과 길 (11-23 골대 2칸): 범위 안 두 칸 가운데 안 지고 · 남은 힘이 큰 쪽 (path_key). 못 차면 빈 길
static std::vector<V2> shot_route(const State &s, const Player &p, V2 &goal, int &pow) {
	const int rng = act_range(s, p, C_SHOT);
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool blocked[NT];
	path_blocked(s, p.team, -1, true, blocked);
	std::vector<V2> best;
	PathKey bk;
	for (int k = 0; k < GOAL_N; k++) {
		const V2 g = goal_tile(opponent(p.team), k);
		const int pw = act_power(s, p, U_SHOT, manhattan(p.pos, g));
		const Foe f{ &s, zone, p.uid, pw, UNSET, false, 0 };
		std::vector<V2> path = route(p.pos, g, rng, &f, blocked, g);
		if (path.empty())
			continue;
		const PathKey key = path_key(&f, p.pos, path);
		if (best.empty() || key.lose < bk.lose || (key.lose == bk.lose && (key.left < bk.left || (key.left == bk.left && path.size() < best.size()))))
			best = path, bk = key, goal = g, pow = pw;
	}
	return best;
}

static bool do_shot(State &s, int uid) {
	const Player &p = s.players[uid];
	if (!can_act(s, p) || s.ball_owner != uid)
		return false;
	V2 goal = opponent_goal(p.team);
	int pow = 0;
	std::vector<V2> path = shot_route(s, p, goal, pow);
	if (path.empty())
		return false;
	const int team = p.team;
	emit({ 4, uid, -1, goal, pow, 2, 0, path });
	pay_point(s);
	if (send_ball(s, p, pow, path)) {
		after_action(s, team);
		return true;
	}
	// 슛 길의 경합을 뚫고 골대에 닿았다 — 남은 힘만큼 골키퍼 체력을 깎는다 · 0 이하면 골 (11-23 사용자 「골대 체력 · 이긴 만큼만 남음」)
	const int keeper = opponent(team);
	s.ghp[keeper] -= pow;
	if (s.ghp[keeper] > 0) {   // 막힘 = 키퍼가 공을 쥔다 → 그 팀 다음 차례 첫 수 = 키퍼 롱킥 (포인트 1 · 킥 범위 안 아군)
		for (Player &q : s.players)
			q.charge = 0;
		s.ball_owner = -1;
		s.ball_pos = UNPLACED;
		s.loose_team = -1;
		s.hold = 0;
		s.keeper = keeper;
		emit({ 9, keeper, -1, goal, s.ghp[keeper], pow, 0, {} });   // 사건 9 = 키퍼가 막음 (a = 키퍼 팀 · at = 골대 칸 · va = 남은 체력 · vb = 깎은 힘)
		after_action(s, team);   // 공을 잃었다 — 남은 포인트는 사라지고 상대 차례 (가로채기와 같다)
		return true;
	}
	s.goals[team] += 1;
	emit({ 2, team, pow, goal, pow, 0, 1, {}, pow });   // (골대 체력은 restart_half 가 다시 채운다)
	s.turn_count += 1;   // 넣은 팀의 차례는 여기서 끝 — 반은 턴 시계로만 끝난다 (v15b)
	if (!clock(s))
		restart_half(s, team);   // 골 뒤 = 킥오프 (11-22 — 옛 키퍼 킥 없앰)
	return true;
}

// 키퍼 롱킥 (11-24 사용자 「액포 1 · 킥 범위 · 버티는 힘」): 키퍼가 쥔 공을 골대에서 킥 범위 안 아군 한 명 발밑으로 (공중볼 · 경합 없음 · 포인트 1)
// 받은 선수 버티는 힘 = 키퍼 킥 + 1 − 거리 (패스와 같은 거리 감쇠). 범위 안에 아무도 없으면 골대에 가장 가까운 아군만
static bool keep_ok(const State &s, const Player &q) {
	const int team = s.keeper;
	if (q.team != team || !q.placed() || is_any_box(q.pos))
		return false;
	if (goal_dist(team, q.pos) <= s.gk_range[team])
		return true;
	for (const Player &o : s.players)   // 범위 안에 아무도 없으면 가장 가까운 아군 (공이 멈추지 않게)
		if (o.team == team && o.placed() && !is_any_box(o.pos) && goal_dist(team, o.pos) <= s.gk_range[team])
			return false;
	int best = 1 << 20;
	for (const Player &o : s.players)
		if (o.team == team && o.placed() && !is_any_box(o.pos))
			best = std::min(best, goal_dist(team, o.pos));
	return goal_dist(team, q.pos) == best;
}

static bool do_keep(State &s, int uid) {
	if (s.phase != PLAY || s.keeper != s.turn_team || s.pts < 1 || uid < 0 || uid >= PLAYERS || !keep_ok(s, s.players[uid]))
		return false;
	const int team = s.keeper;
	const Player &q = s.players[uid];
	const int pow = std::max(1, s.gk_kick[team] + 1 - goal_dist(team, q.pos));
	emit({ 4, -1, uid, q.pos, pow, 3, 0, { q.pos } });   // 사건 4 · vb 3 = 키퍼 롱킥 (a = −1 · b = 받는 선수 · at = 받는 칸 · va = 힘)
	s.keeper = -1;
	set_owner(s, uid);
	s.hold = catch_hold(q, pow);
	pay_point(s);
	after_action(s, team);
	return true;
}

static bool can_stance(const State &s, const Player &p) {
	return can_act(s, p) && s.ball_owner != p.uid && !kickoff_turn(s) && (p.stance == 0 || !zone_all(p)) && !is_any_box(p.pos);   // 태세 중이면 방향 바꾸기만 (사방 존은 바꿀 것 없음)
}

// 「강하게」 (11-22 셋째 — 사용자 「2액포 소모하면 힘 +1 · 패스·슛·드리블·디펜스 · 이동은 없음」): 엔진에서는 「힘 모으기(1포인트, 행동으로 안 셈) → 바로 이어지는
// 그 선수의 패스·슛·드리블·디펜스(1포인트)」 두 수. 공 임자 = 제자리 A_STANCE · 공 없는 선수 = A_SAVE(자기 칸 — 이어서 디펜스). 화면은 「강하게」 단추 하나
static bool can_stance(const State &s, const Player &p);
static bool charge_ok(const State &s, const Player &p) {
	if (p.charge != 0 || s.pts < 2 || !can_act(s, p) || kickoff_turn(s))
		return false;
	return s.ball_owner == p.uid || can_stance(s, p);   // 공 임자 = 이어서 패스·슛·드리블 · 공 없는 선수 = 이어서 디펜스 (11-23 강하게 이동 없앰)
}

static bool do_charge(State &s, int uid, V2 dest) {
	Player &q = s.players[uid];
	if (dest != q.pos || !charge_ok(s, q))
		return false;
	pay_point(s);
	q.charge = 2;
	after_action(s, q.team);
	return true;
}

static bool do_stance(State &s, int uid, V2 dest) {
	if (s.ball_owner == uid)
		return do_charge(s, uid, dest);   // 공 임자 제자리 디펜스 = 「강하게」 모으기
	const Player &p = s.players[uid];
	const int dir = stance_dir(p, dest);
	if (!can_stance(s, p) || dir < 0 || (p.stance > 0 && dir == p.zdir))
		return false;   // 이미 태세면 다른 방향으로만 다시 건다 (11-23)
	const int team = p.team;
	const int pw = act_power(s, p, U_STANCE);
	pay_point(s);
	Player &pm = s.players[uid];
	pm.stance = pw;
	pm.stance_left = 1;   // 태세 = 움직이거나 경합에서 질 때까지 (11-23 — 옛 「몇 차례」 셈 없앰 · 망 입력 칸은 늘 1)
	pm.zdir = dir;
	if (wall_hit(s, uid))
		jk_fire(s, team, JK_WALL, uid);   // 철벽 — 태세를 잡는 순간 (시도로 걸림) · 그 수비수 태세 힘 +1
	auto_tackle(s, uid);   // 고른 존 칸이 공 임자 칸이면 바로 태클 (게겐프레싱)
	after_action(s, team);
	return true;
}

int eff_hold(const State &s) { return s.hold; }

static bool apply_inner(State &s, const Action &a);
// 직업 스킬 (옛 게이지 — 11-19 상시라 a.sk 는 늘 0, 0 아니면 거른다)
bool apply_action(State &s, const Action &a) {
	if (a.sk) {
		if (a.sk < 0 || a.sk > SK_KINDS_MAX || a.t < A_MOVE || a.t > A_STANCE || a.uid < 0 || a.uid >= PLAYERS || s.phase != PLAY || !skill_can(s, s.players[a.uid]) ||
				!skill_act(s.players[a.uid].position, a.t, a.sk))
			return false;
	}
	s.sk_uid = a.sk ? a.uid : -1;
	s.sk_kind = a.sk;
	const bool ok = apply_inner(s, a);
	s.sk_uid = -1;
	s.sk_kind = 0;
	if (!ok)
		return false;
	if (a.sk) {
		if (s.fired[a.uid] < 255)
			s.fired[a.uid] += 1;
		emit({ 6, a.uid, s.players[a.uid].position, s.players[a.uid].pos, 0, 0, 0, {} });
	}
	return true;
}


// 결과가 다른 길 후보 (11-18 사용자 「무슨 액션이든 승리가 목적」 — 승률 판단은 AI 가 한다, 휴리스틱이 대신하지 않는다).
// 같은 행동(같은 도착 칸·카드)을 길만 바꿔 다 해 보고, 끝 상태가 똑같은 길끼리 한 묶음 — 묶음마다 대표 하나(기본 규칙 PathKey 1등).
// out[0] = 기본 길(route 가 고르는 길)의 묶음. 이동·드리블·땅볼 패스·슛만 — 공중볼·그 밖은 빈 목록. 상태는 바꾸지 않는다
void outcome_paths(const State &s, const Action &a, std::vector<std::vector<V2>> &out) {
	out.clear();
	if (a.t < A_MOVE || a.t > A_SHOT || a.uid < 0 || a.uid >= PLAYERS || g_path)
		return;
	const Player &p = s.players[a.uid];
	const bool kick = a.t == A_PASS || a.t == A_SHOT;
	const V2 dest = a.t == A_SHOT ? (is_goal_of(opponent(p.team), a.dest) ? a.dest : opponent_goal(p.team)) : a.dest;
	if (a.t == A_PASS && manhattan(p.pos, dest) >= LONG_PASS_MIN)
		return;
	bool blocked[NT];   // 그 행동의 벽 (적의 몸·골대 + 11-21 지나간 칸 — 엔진 판정과 같게)
	if (a.t == A_MOVE)
		move_blocked(s, p, blocked);
	else
		path_blocked(s, p.team, a.t == A_DRIBBLE ? a.uid : -1, true, blocked);
	std::vector<Event> *ev0 = g_events, dev;
	g_events = &dev;
	State base;
	std::memcpy(static_cast<void *>(&base), &s, sizeof(State));
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
	walk_paths(p.pos, dest, act_range(s, p, kick ? C_KICK : C_MOVE, a.sk), blocked, a.t == A_SHOT ? dest : UNSET,
			[&](const std::vector<V2> &cur) {
				State t;
				std::memcpy(static_cast<void *>(&t), &base, sizeof(State));
				g_path = &cur;
				const bool ok = apply_action(t, a);
				g_path = nullptr;
				if (!ok)
					return;
				const PathKey k = path_key(nullptr, p.pos, cur);
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

// 길을 간 뒤의 상태로 고른다 (사용자 11-18): 결과가 다른 길 후보마다 끝 상태를 val(상태, 움직인 팀, ctx) — 그 팀에 좋을수록 큼 — 로 재어
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



static bool apply_one(State &s, const Action &a);

static bool spend_charge(State &s, int uid, bool ok) {   // 모은 힘은 그 선수의 첫 패스·슛·드리블·디펜스 1번에 쓴다
	if (ok && s.players[uid].charge == 2)
		s.players[uid].charge = 0;
	return ok;
}

static bool apply_inner(State &s, const Action &a) {   // 한 선수는 한 차례에 행동 하나 (11-22) — 「강하게」 모으기는 행동으로 안 센다(바로 이어지는 행동까지 한 행동)
	const bool strong = (a.t == A_STANCE && a.uid >= 0 && a.uid < PLAYERS && s.ball_owner == a.uid) || a.t == A_SAVE;
	const int team = s.turn_team;
	if (!apply_one(s, a))
		return false;
	if (!strong && a.uid >= 0 && a.uid < PLAYERS && a.t != A_PLACE && s.players[a.uid].team == team && s.turn_team == team)
		s.players[a.uid].acted = 1;
	return true;
}

static bool apply_one(State &s, const Action &a) {
	const bool u = a.uid >= 0 && a.uid < PLAYERS;
	switch (a.t) {
		case A_PLACE: return do_place(s, a.uid, a.dest);
		case A_MOVE: return u && s.players[a.uid].charge == 0 && do_move(s, a.uid, a.dest);   // 이동은 「강하게」 없음 (11-23 다가가 태클 없앰)
		case A_DRIBBLE: return u && spend_charge(s, a.uid, do_dribble(s, a.uid, a.dest));
		case A_PASS: return u && spend_charge(s, a.uid, do_pass(s, a.uid, a.dest));
		case A_SHOT: return u && spend_charge(s, a.uid, do_shot(s, a.uid));
		case A_STANCE: return u && (s.ball_owner == a.uid ? do_stance(s, a.uid, a.dest) : spend_charge(s, a.uid, do_stance(s, a.uid, a.dest)));
		case A_KEEP: return do_keep(s, a.uid);
		case A_SAVE: return u && s.ball_owner != a.uid && do_charge(s, a.uid, a.dest);   // 공 없는 선수 「강하게」 (옛 키퍼 킥 번호)
	}
	return false;   // 차례 끝 수는 없다 (11-21 사용자 — 포인트를 다 쓴다, 둘 수가 없을 때만 저절로 끝)
}

// ---------- 열거 ----------

void enumerate_actions(const State &s, std::vector<Action> &out) {
	out.clear();
	if (s.phase == PLACEMENT) {
		int team = placement_team(s);
		if (team < 0)
			return;
		int x0 = team == HOME ? 0 : W - W / 2;   // 11칸 — 가운데 줄(x 5)은 배치 밖
		for (int slot = 0; slot <= SLOT_PROSPECT; slot++) {
			if (!slot_open(s, team, slot))
				continue;
			for (int x = x0; x < x0 + W / 2; x++)
				for (int z = 0; z < H; z++) {
					V2 t{ x, z };
					if (place_tile_ok(s, team, t, false))
						out.push_back({ A_PLACE, slot, t, -1 });
				}
		}
		return;
	}
	if (s.phase != PLAY || s.pts < 1)
		return;
	const int team = s.turn_team;
	if (s.keeper == team) {   // 우리 키퍼가 공을 쥐고 있다 — 롱킥뿐 (킥 범위 안 아군 고르기, 11-24)
		for (const Player &q : s.players)
			if (keep_ok(s, q))
				out.push_back({ A_KEEP, q.uid, q.pos, -1, 0 });
		return;
	}
	const bool ko = kickoff_turn(s);
	auto gen = [&](const Player &q, int sk) {   // sk = Action.sk (0 = 스킬 없음 · 만능은 1·2 둘 다)
		const bool skl = sk != 0;
		const bool do_mv = !skl || skill_act(q.position, A_MOVE, sk), do_kk = !skl || skill_act(q.position, A_PASS, sk), do_df = !skl || skill_act(q.position, A_STANCE, sk);
		const int u = q.uid;
		const bool carry = may_carry(s, q);
		if (do_mv) {
			const int mr = act_range(s, q, C_MOVE, sk);   // 드리블 범위 = 이동 범위 (11-22 「범위 = 능력치」 — 옛 −1 없앰) − 1 (11-21 규칙)
			bool blocked[NT];   // 그 선수의 벽 — 적의 몸 · 이번 차례 그 선수(드리블이면 공도)가 있었던 칸
			if (carry)
				path_blocked(s, team, u, true, blocked);
			else
				move_blocked(s, q, blocked);
			Reach r;
			reach(q.pos, mr, blocked, UNSET, r);
			for (int i = 0; i < r.n; i++) {
				V2 t = r.order[i];
				if (carry) {
					if (player_at(s, t) || is_any_box(t))
						continue;
				} else if (ko || s.ball_owner == u || q.charge || !move_dest_ok(s, q, t)) {   // 모은 선수는 이동 없음 (디펜스만)
					continue;
				}
				if (!carry && s.loose_ball() && is_any_box(t)) {
					std::vector<V2> pth = move_path(s, q, t, mr, 1, blocked);
					if (std::find(pth.begin(), pth.end(), s.ball_pos) != pth.end())
						continue;   // 주운 공을 들고 박스에 멈출 수 없다
				}
				out.push_back({ carry ? A_DRIBBLE : A_MOVE, u, t, -1, sk });
			}
		}
		if (carry && do_kk) {   // 패스 · 뒷공간 패스 · 슛
			const int kr = act_range(s, q, C_KICK, sk);
			Reach g, air;
			pass_targets(s, q, kr, g, air);
			for (int pass = 0; pass < 2; pass++) {
				const Reach &rr = pass == 0 ? g : air;
				for (int i = 0; i < rr.n; i++) {
					V2 t = rr.order[i];
					int dist = manhattan(q.pos, t);
					if (pass == 1 && (dist < LONG_PASS_MIN || g.has(t) || dist > g_air_max))
						continue;
					if (dist > kr)
						continue;
					if (pass_dest_ok(s, q, t))   // 동료 칸 · 스루패스(빈 칸 — through_ok)
						out.push_back({ A_PASS, u, t, -1, sk });
				}
			}
			if (!ko && s.ball_owner == u) {   // 슛 — 골대 두 칸 가운데 닿는 칸이 있으면 수 하나 (dest = 대표 칸 · 실제 칸은 do_shot 이 고른다, 11-23)
				bool blocked[NT];
				path_blocked(s, team, -1, true, blocked);
				const int sr_n = act_range(s, q, C_SHOT, sk);   // 슛 범위 4
				bool can = false;
				for (int gk = 0; gk < GOAL_N && !can; gk++) {
					const V2 goal = goal_tile(opponent(team), gk);
					Reach sr;
					reach(q.pos, sr_n, blocked, goal, sr);
					can = sr.has(goal) && manhattan(q.pos, goal) <= sr_n;
				}
				if (can)
					out.push_back({ A_SHOT, u, opponent_goal(team), -1, sk });
			}
		}
		if (!sk && charge_ok(s, q))   // 「강하게」 모으기 — 공 임자 = 제자리 디펜스 칸 · 공 없는 선수 = A_SAVE (11-22 셋째)
			out.push_back({ s.ball_owner == u ? A_STANCE : A_SAVE, u, q.pos, -1, 0 });
		if (do_df && can_stance(s, q))   // 수비 = 곁 한 칸을 골라 존 (판 밖·골대 ×) · 월은 제자리 사방
			for (int k = zone_all(q) ? ZD_ALL : 0; k <= ZD_ALL; k++) {
				const V2 t = k == ZD_ALL ? q.pos : q.pos + DIRS[k];
				if ((k == ZD_ALL ? zone_all(q) : stance_dir(q, t) == k) && !(q.stance > 0 && k == q.zdir))
					out.push_back({ A_STANCE, u, t, -1, sk });
			}
	};
	for (const Player &q : s.players) {
		if (q.team != team || !q.placed())   // 한 선수 여러 행동 (11-23)
			continue;
		gen(q, 0);
		if (skill_can(s, q))
			for (int k = 1; k <= (q.position == ALLROUND ? 2 : 1); k++)
				gen(q, k);
	}
}

// ---------- 손평가 (망 없는 화면 AI · 첫 세대 가치) ----------

double heuristic(const State &s, int team) {
	int opp = opponent(team);
	double h = (s.goals[team] - s.goals[opp]) * 3.0;
	if (s.phase == PLAY) {
		int bt = s.ball_team();
		h += bt == team ? 0.4 : -0.4;
		V2 b = s.ball_owner != -1 ? s.players[s.ball_owner].pos : (s.loose_ball() ? s.ball_pos : V2{ W / 2, H / 2 });
		double prog = (bt == HOME ? b.x : W - 1 - b.x) / double(W - 1);
		h += (bt == team ? 1.0 : -1.0) * 0.6 * prog;
		if (s.turn_team == team || s.turn_team == opp)   // 이번 차례 남은 포인트 (11-21 족보 +1 을 손평가가 알아보게)
			h += (s.turn_team == team ? 0.1 : -0.1) * s.pts;
		for (const Player &q : s.players) {   // 태세 존이 공 쪽을 보나 (11-21 방향 존 — 손평가 탐색이 방향을 고르게)
			if (q.stance <= 0 || q.team == bt)
				continue;
			V2 z[4];
			const int nz = zone_tiles(q, z);
			bool face = false;
			for (int k = 0; k < nz; k++)
				face = face || manhattan(z[k], b) < manhattan(q.pos, b);
			h += face ? (q.team == team ? 0.05 : -0.05) : 0.0;
		}
		if (s.ball_owner >= 0) {   // 공 임자가 슛 범위 안 (골문 체력 없음 — 11-21: 길의 경합만 뚫으면 골이라 골 직전 국면)
			const Player &o = s.players[s.ball_owner];
			if (goal_dist(opponent(o.team), o.pos) <= act_range(s, o, C_KICK, 0))
				h += (o.team == team ? 1.0 : -1.0) * 0.8;
		}
	}
	return h;
}

} // namespace card
} // namespace cf
