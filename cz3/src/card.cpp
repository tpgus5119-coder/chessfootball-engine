// 카드 모드 규칙 엔진 — card.h 설명 · docs/spec.md # 카드 모드
// 구성: 상태·도우미 → 배치(본편과 같은 드래프트) → 존·경합 → 길 → 액션 → 열거 → 평가
#include "card.h"

#include <cstring>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace cf {
namespace card {

std::vector<Event> *g_events = nullptr;
const std::vector<V2> *g_path = nullptr;

static const int DRAFT_POOL[CARD_POOL_N] = { CANNON, MAESTRO, TRICKSTER, SPRINTER, MARKER, BOOSTER, TITAN, HUNTER, WALL, NOMAD };   // v12 — 본편 표준 순서   

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

enum Use { U_PASS, U_SHOT, U_MOVE, U_DRIBBLE, U_STANCE };

// ---------- 직업 스킬 (11-14 사용자 — 컵 커리어만, set_career) ----------
// 패시브만: 월·타이탄·마커·부스터·노마드 (「## 3」 능력 그대로). 액티브만: 캐논(킥 힘)·마에스트로(킥 범위)·트릭스터(이동 힘)·스프린터(이동 범위)·헌터(수비 힘) —
// 그 다섯은 늘 붙는 +1 이 없고, 체력 SKILL_STAM 을 내고 그 수의 카드에만 +1 (Action.sk). 게이지 없음 · 횟수 제한 없음.
// 대표 행동(rep)은 전직 조건으로만 센다 (core/career.gd)
void set_career(State &s) { s.car_on = 1; }

// 만능 패시브 (사용자 11-17 「만능 패시브 ㄱ」): 만능은 월·타이탄·마커·부스터·노마드 패시브 다섯을 모두 갖는다 — 패시브를 묻는 자리는 모두 이 함수로
static inline bool has_passive(int position, int want) { return position == want || position == ALLROUND; }

bool skill_job(int position) { return position == CANNON || position == MAESTRO || position == TRICKSTER || position == SPRINTER || position == HUNTER || position == ALLROUND; }
// 만능 (11-17 사용자): 다섯 스킬을 아무거나 — 킥 카드엔 캐논(sk 1 힘)·마에스트로(sk 2 범위), 이동 카드엔 트릭스터(sk 1)·스프린터(sk 2), 태세엔 헌터(sk 1). 비용 같음 · 패시브 다섯도 모두 (has_passive)
static bool sk_power(int pos, int sk, int want, int use) {   // 이 수가 힘 +1 스킬인가
	if (!sk)
		return false;
	const bool all = pos == ALLROUND && sk == 1;
	return (want == C_KICK && (pos == CANNON || all)) || (want == C_MOVE && use != U_STANCE && (pos == TRICKSTER || all)) ||
			(use == U_STANCE && (pos == HUNTER || all));
}
static bool sk_range(int pos, int sk, int want) {   // 이 수가 범위 +1 스킬인가
	if (!sk)
		return false;
	const bool all = pos == ALLROUND && sk == 2;
	return (want == C_KICK && (pos == MAESTRO || all)) || (want == C_MOVE && (pos == SPRINTER || all));
}

// 측정용 (11-17 — 기본 = 지금 규칙): 스킬 체력 · 효과 크기 · 판당 선수별 횟수 (0 = 제한 없음)
int g_skill_stam = SKILL_STAM, g_skill_pow = 1, g_skill_uses = 0;
bool skill_can(const State &s, const Player &p) {
	return !s.sk_passive && skill_job(p.position) && s.stam[p.uid] >= g_skill_stam && (g_skill_uses <= 0 || s.fired[p.uid] < g_skill_uses);   // 11-14: 모든 경기 (체력은 new_match 가 늘 켠다)
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
	if (!s.car_on || uid < 0 || uid >= PLAYERS)
		return;
	const int j = pool_slot(job);
	if (s.rep[uid][j] < 255)
		s.rep[uid][j] += 1;
}

// ---------- 상태 도우미 ----------

int State::ball_team() const {
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

// 공 임자를 바꾼다. 버티는 힘(hold)은 태세로 뺏었으면 그 태세 힘, 아니면 0 — 패스·드리블·줍기는 부른 쪽이 곧바로 숫자를 적는다 (v9.0)
// 가담: 같은 팀이 이어 가지면 만진 선수를 더하고, 상대에게 넘어가면 새로 센다
// 버티는 힘을 새로 적은 때: 상대 차례에 얻었으면 우리 다음 차례 시작엔 안 깎는다 (첫 차례는 그대로 — 2026-11-04)
static void mark_hold(State &s) {
	s.hold_skip = (s.ball_owner >= 0 && s.players[s.ball_owner].team != s.turn_team) ? 1 : 0;
}

static void ctr_touch(State &s, int team, int x);
static void set_owner(State &s, int uid) {
	s.ball_owner = uid;
	s.ball_pos = UNPLACED;
	s.loose_team = -1;
	s.hold = 0;
	if (uid != -1) {
		Player &p = s.players[uid];
		ctr_touch(s, p.team, p.pos.x);
		s.hold = p.stance;
		mark_hold(s);
		if (p.stance > 0 && p.stance_sk == D_COMBO)
			s.combo[p.team] += 1;   // 역습 시동 — 수비 성공 시 콤보 +1
		if (g_class_combo && p.stance > 0 && (p.position == HUNTER || has_passive(p.position, WALL)))
			s.combo[p.team] += 1;
		if (s.touch_team != p.team) {
			s.touch_team = p.team;
			s.touch_mask = 0;
			// v13 (사용자 2026-11-04): 태세였던 선수는 가담에 안 든다 — 공을 실제로 만진 선수만 (v12 태세 가담 없앰)
		}
		p.stance = 0;   // 공을 잡으면 태세가 풀린다
		s.touch_mask |= 1 << uid;
		if (__builtin_popcount(unsigned(s.touch_mask)) >= g_jk_royal && !(s.jk_done[p.team] & JK_ROYAL)) {   // 족보 로열 (v17) — 서로 다른 5명이 공을 만졌다
			const int before = s.jk_done[p.team];
			s.jk_done[p.team] |= JK_ROYAL;
			s.jk_count[p.team][1] += 1;
			if (!jk_ap(before) && s.turn_team == p.team)
				s.pts += 1;   // 로열 = 그 차례 포인트 +1 (v17b 사용자 2026-11-12) — 받는 순간, 패스에 쓴 포인트는 이미 냈다
		}
	}
}

static void ctr_touch(State &s, int team, int x);
static void drop_ball(State &s, V2 t, int team) {
	s.ball_owner = -1;
	s.ball_pos = t;
	s.loose_team = team;
	ctr_touch(s, team, t.x);
}

// 탈취(태클·가로채기·존 경합 승리)로 team 이 공을 얻었다 — 역습 시작 (v16): 뺏은 자리(공 임자 칸)의 x 를 적어 둔다
static void mark_steal(State &s, int team) {
	s.steal_turn[team] = s.turn_count;
	s.steal_turn[opponent(team)] = -1;
	if (s.ball_owner >= 0)   // 대표 행동 (11-13): 수비 성공 = 헌터
		rep_add(s, s.ball_owner, HUNTER);
}

// 공을 얻은 팀의 역습 시작 칸 — 공이 아직 판에 없으면(킥오프·키퍼 킥) 처음 공이 놓이는 칸 (11-14 — 탈취·킥오프·키퍼 킥 모두 「공 얻음」)
static void ctr_begin(State &s, int team) {
	s.ctr_on[team] = 1;
	s.ctr_x[team] = s.ball_owner >= 0 ? s.players[s.ball_owner].pos.x : s.loose_ball() ? s.ball_pos.x : -1;
}
static void ctr_touch(State &s, int team, int x) {
	if (team >= 0 && s.ctr_on[team] && s.ctr_x[team] < 0)
		s.ctr_x[team] = x;
}

// 역습 보너스 (v16 사용자 2026-11-10 · 11-14 공 얻은 팀 모두): 공을 얻은 자리에서 공이 앞으로 간 칸 수 (공의 x 기준 · 뒤로 가면 줄고 0 아래로는 안 내려감)
int counter_bonus(const State &s, int team) {
	if (!s.ctr_on[team] || s.ctr_x[team] < 0 || s.ball_team() != team)
		return 0;
	const int bx = s.ball_owner >= 0 ? s.players[s.ball_owner].pos.x : s.loose_ball() ? s.ball_pos.x : s.ctr_x[team];
	return std::max(0, (bx - s.ctr_x[team]) * forward_dir(team));
}

// ---------- 족보 (v17 사용자 2026-11-11 「족보혜택 ㄱ」 · 혜택 짝 v17b 11-12 — 포카드 범위 · 스트레이트 힘 · 로열 포인트 · 풀하우스 셋 다) ----------
// 숫자열 = 이번 공격에서 공 가진 팀이 낸 카드 숫자 (찍힌 숫자 — 부스터·캐논 같은 힘 보정 전, 노마드가 바꿔 쓰면 바꾼 숫자로 콤보와 같다).
// 카드를 낼 때마다 끝에 붙여 보고, 끝이 족보 꼴이면 완성 (tools/cz/jokbo_stats 와 같은 「이어진 숫자」). 공격이 끝나면(공을 잃음·슛) 지운다
// 측정용 바꿀 안 (11-17 — 기본값 0·5 = 지금 규칙, 경기·학습은 안 건드린다): 스트레이트 1 = 오름 3장(123·234) · 2 = 오름·내림 3장
// 풀하우스 1 = 마지막 5장이 같은 숫자 3 + 2 (순서 무관) · 2 = 마지막 4장 투페어(AABB) · 로열 = 만진 서로 다른 선수 수
int g_jk_straight = 0, g_jk_full = 0, g_jk_royal = 5;
static int jk_suffix(int seq) {
	int m = 0;
	const int l3 = seq % 1000;
	if (g_jk_straight == 0 ? seq % 10000 == 1234 : (l3 == 123 || l3 == 234 || (g_jk_straight == 2 && (l3 == 321 || l3 == 432))))
		m |= JK_STRAIGHT;
	const int d = seq % 10;
	if (d > 0 && seq % 10000 == d * 1111)
		m |= JK_FOUR;
	if (g_jk_full == 0) {
		if (seq % 10000000 == 1223344)
			m |= JK_FULL;
	} else if (g_jk_full == 1) {
		int c[10] = {}, x = seq % 100000;
		bool ok = seq >= 10000;
		for (int i = 0; i < 5; i++, x /= 10) c[x % 10]++;
		bool three = false, two = false;
		for (int k = 1; k < 10; k++) three |= c[k] == 3, two |= c[k] == 2;
		if (ok && three && two)
			m |= JK_FULL;
	} else {
		const int a = seq / 1000 % 10, b = seq / 100 % 10, e = seq / 10 % 10;
		if (seq >= 1000 && a == b && e == d && a != d)
			m |= JK_FULL;
	}
	return m;
}
static int jk_next(int seq, int value) { return (seq * 10 + std::max(1, std::min(9, value))) % 10000000; }

int jokbo_mask(const State &s, int team) { return s.role[team] == R_ATK ? s.jk_done[team] : 0; }

// 이 숫자 카드를 내면 걸려 있을 족보 — 완성하는 카드도 그 순간부터 혜택을 받는다 (범위·힘). 공 없는 팀(수비 역할)은 0
static int jk_after(const State &s, int team, int value) {
	if (s.role[team] != R_ATK)
		return 0;
	return s.jk_done[team] | jk_suffix(jk_next(s.jk_seq[team], value));
}

// 낸 카드 숫자를 붙인다 — 새로 완성한 풀하우스는 그 차례 포인트 +1 (그 공격에 한 번 — 로열로 이미 받았으면 없음, v17b)
static void jk_play(State &s, int team, int value) {
	s.jk_seq[team] = jk_next(s.jk_seq[team], value);
	const int before = s.jk_done[team];
	const int fresh = jk_suffix(s.jk_seq[team]) & ~before;
	s.jk_done[team] |= fresh;
	for (int k = 0; k < 4; k++)
		if (fresh & (1 << k))
			s.jk_count[team][k] += 1;
	if (jk_ap(fresh) && !jk_ap(before) && s.turn_team == team)
		s.pts += 1;
}

static void jk_clear(State &s, int team) { s.jk_seq[team] = s.jk_done[team] = 0; }

bool kickoff_turn(const State &s) {
	return s.phase == PLAY && s.save_team < 0 && s.ball_owner == -1 && !s.loose_ball() && s.turn_team == opponent(s.first_team);
}

// 공 가진 팀 = 킥·이동 손, 아닌 팀 = 수비·이동 손. 역할이 바뀌면 그 팀은 새 손 6장 (사용자 "공 뺏기면 카드 다시 새로")
// 공이 넘어가면 콤보도 끝난다
static void sync_roles(State &s) {
	int bt = s.ball_team();
	for (int t = 0; t < 2; t++) {
		int want = t == bt ? R_ATK : R_DEF;
		if (s.role[t] != want) {
			if (want == R_ATK)   // 공을 얻음 — 새 손을 받기 전까지 역습 보너스·빠른 전개 (11-14 하나로)
				ctr_begin(s, t);
			s.lose_fresh[t] = want == R_DEF;
			if (want == R_ATK) {
				s.poss_now[t] = 1;
			} else {
				s.lost_pending[t] = 1;
				s.rb_kick[t] = s.rb_move[t] = 0;
			}
			jk_clear(s, t);   // 공이 넘어가면 그 공격의 족보는 끝 (v17)
			s.role[t] = want;   // 손은 그대로 — 킥 ↔ 수비는 같은 칸이라 숫자 그대로 계열만 바뀐다, 이동은 승계 (사용자 2026-11-06)
			if (want == R_DEF) {
				s.combo[t] = s.combo_last[t] = 0;
				s.ctr_on[t] = 0;
				s.steal_turn[t] = -1;
			}   // 공을 잃은 팀의 콤보는 끝 (v13 — 태세는 콤보를 안 쌓으니 공을 얻은 팀은 0에서 시작)
		}
	}
}

bool marked(const State &s, const Player &p) {   // 상대 마커(공 없음)에 상하좌우로 붙었다 — 이동 액션만 (사용자 2026-11-04 「인접한 적들은 이동 액션만 가능」)
	if (!p.placed())
		return false;
	for (const Player &m : s.players)
		if (m.team != p.team && has_passive(m.position, MARKER) && m.placed() && m.uid != s.ball_owner && manhattan(m.pos, p.pos) == 1)
			return true;
	return false;
}

bool boosted(const State &s, const Player &p) {   // v12: 우리 부스터(공 없음)에 상하좌우로 붙었다 — 카드 힘 +1 (범위는 그대로, 부스터 자신은 받지 않는다)
	if (!p.placed())
		return false;
	for (const Player &b : s.players)
		if (b.team == p.team && b.uid != p.uid && has_passive(b.position, BOOSTER) && b.placed() && b.uid != s.ball_owner && manhattan(b.pos, p.pos) == 1)
			return true;
	return false;
}

void card_info(const State &s, int team, int card, int &type, int &value) {
	type = slot_type(s.role[team], card);
	value = slot_value(card);   // 손 숫자 고정 — 킥·수비 2-3-4 · 이동 1-2-3 (사용자 11-17, hand_kick·hand_move 는 늘 0)
	(void)s;
	(void)team;
}

// 마커 곁이면 **카드 숫자 −1** — 범위·힘 둘 다 (v7.8 사용자 "카드 숫자 자체를 -1하는게 깔끔"). HP값은 적힌 숫자 그대로
static int eff_num(const State &s, const Player &p, int value) {
	(void)s; (void)p;   // 마커 −1은 없앴다 (2026-11-04 — 대신 킥·태세 금지)
	return std::max(0, value);
}

// 전문가 +1은 **카드 숫자에** 붙는다 — 숫자가 범위이자 힘이라 둘 다 (v7.9 — 캐논·마에스트로 / 트릭스터·스프린터 / 헌터·월을 하나씩으로)
// 2026-11-08 사용자: 캐논 = 슛만 +1 · 트릭스터 = 드리블만 +1 · 헌터 = 수비 +1 (쓰임 use 를 모르면 −1 → 수비만 센다)

// 범위만 +1 (v12): 마에스트로 = 킥 범위 · 스프린터 = 이동 범위 — 힘(경합·버티는 힘)은 그대로
int g_range_bonus = 1;   // 마에스트로·스프린터 범위 보너스 — 측정용 (사용자 2026-11-08 「+2 로?」)
int g_air_max = 99;      // 공중볼 최대 거리 — 측정용
int g_class_combo = 0;   // 1 = 직업 콤보 (마에스트로 패스·트릭스터 드리블·스프린터 이동 성공 +1 · 헌터·월 수비로 뺏으면 +1) — 측정용 (사용자 2026-11-08)
// 직업 보너스와 컵 전성기 혜택(range_up·kick_range_up)은 같은 「+1」이다 — 겹쳐 +2 가 되지 않게 큰 쪽 하나 (v14, 사용자 2026-11-09 「1카드로 킥하면 5칸」)
// v14 (사용자 2026-11-09 「범위와 능력 숫자 반대로」 + 정정): 카드 숫자 = 힘. 범위는 쓰임마다 —
// 킥(패스·슛) = 6 − 숫자 (2 → 4칸 · 4 → 2칸, 사용자 11-09 b) · 이동(이동·드리블) = 4 − 숫자 (1 → 3칸 · 3 → 1칸, 노마드가 킥·수비 4 를 이동으로 = 0칸 = 못 씀).
// 범위 안 어디로 가도 힘은 카드 숫자 그대로. 마에스트로(킥)·스프린터(이동)·전성기 혜택·스킬의 범위 +1 은 범위만 — 상한은 두지 않는다 (사용자에게 물을 일)
static int base_range(int use, int value) { return std::max(0, (use == C_KICK ? 6 : 4) - value); }
static int eff_range(const State &s, const Player &p, int use, int value) {   // use = 쓰임 (C_KICK 킥 · 그 밖은 이동 — 노마드가 바꿔 쓰면 바꾼 쓰임)
	const int u = use == C_KICK ? C_KICK : C_MOVE;
	const int b = base_range(u, eff_num(s, p, value));
	return b <= 0 ? 0 : b;   // 0칸 카드는 그 쓰임으로 못 낸다 · 마에스트로·스프린터 범위 +1 은 스킬로만 (11-14)
}

static int eff_stat(const State &s, const Player &p, int type, int value) {   // 힘 — 부스터 곁이면 +1 (v12, 범위는 안 늚)
	return eff_num(s, p, value) + (boosted(s, p) ? 1 : 0) + (type == C_DEF ? p.bonus[ST_DEF] : 0);
}

// ---------- 스킬 (사용자 2026-11-07) ----------

// 스킬을 그 숫자 카드에 붙일 수 있나 (사용자 2026-11-09) — 힘이든 범위든 0 이 되면 안 된다:
// 힘 −1 이 붙을 수 있는 스킬은 1 카드에 ×, 범위 −1(묵직한 킥·짧은 터치)은 범위 1 카드(이동 3)에 ×
static bool may_minus(int sk) {
	switch (sk) {
		case SK_NONE: case K_ALLOUT: case M_SPURT: case D_DIVE: case K_HEAVY: case M_SHORT: case K_CORNER: case D_BODY: case D_FRONT: case D_SIDE: case K_POWER: case M_POWER: return false;
		default: return true;
	}
}
bool skill_fits(int sk, int value) {
	if (sk == SK_NONE)
		return true;
	if (value <= 1 && may_minus(sk))
		return false;
	if (sk == M_SHORT && base_range(C_MOVE, value) <= 1)
		return false;
	if (sk == K_HEAVY && base_range(C_KICK, value) <= 1)
		return false;
	return true;
}

// 노마드 바꿔 쓰기의 숫자 (사용자 2026-11-09): 킥 4 ↔ 이동 3 · 킥 3 ↔ 이동 2 · 킥 2 ↔ 이동 1 — 바꾼 쪽의 힘·범위를 따른다.
// 수비 ↔ 이동(수비 팀 노마드)은 숫자 그대로 (아직 정하지 않음)
int use_value(int type, int want, int value) {
	if (type == C_KICK && want == C_MOVE)
		return value - 1;
	if (type == C_MOVE && want == C_KICK)
		return value + 1;
	return value;
}

// 노마드가 다른 종류로 바꿔 쓰면 스킬은 안 든다
static int card_sk(const State &s, int team, int card, int want) {
	const int ty = slot_type(s.role[team], card);
	if (ty != want)
		return SK_NONE;
	return want == C_DEF ? s.sk_def[team][card] : s.sk_atk[team][card];
}

static int sk_cost(int sk) { return sk == K_POWER || sk == M_POWER || sk == D_BODY ? 2 : 1; }

static int rng_of(const State &s, const Player &p, int card, int want, int skl = -1) {   // skl −1 = 지금 수가 스킬을 켰나(s.sk_uid)
	if (skl < 0)
		skl = s.sk_uid == p.uid ? s.sk_kind : 0;
	int ty, v;
	card_info(s, p.team, card, ty, v);
	v = use_value(ty, want, v);   // 노마드 바꿔 쓰기 — 킥 n ↔ 이동 n−1 (사용자 2026-11-09)
	const int sk = card_sk(s, p.team, card, want);
	const int r = eff_range(s, p, want, v);
	// 스킬 −1 로는 0칸이 되지 않는다 (옛 하한 1 그대로) · 족보 포카드·풀하우스 범위 +1 (v17b — 킥·이동 둘 다)
	const int skr = (s.sk_passive ? sk_range(p.position, 1, want) || sk_range(p.position, 2, want) : sk_range(p.position, skl, want)) ? g_skill_pow : 0;   // 직업 스킬 범위 +1 (11-14 · 시험판 패시브는 늘)
	return r <= 0 ? 0 : std::max(1, r + (sk == K_LONG || sk == M_DASH ? 1 : sk == K_HEAVY || sk == M_SHORT ? -1 : 0)) + jk_range(jk_after(s, p.team, v)) + skr;
}

// 스킬 보정을 더한 힘 — **늘 1 이상** (사용자 2026-11-07 「스탯 0 금지」). 범위는 rng_of 만 바꾼다
static int sk_stat(const State &s, const Player &p, int card, int want, int use, V2 dest) {
	int ty, val;
	card_info(s, p.team, card, ty, val);
	val = use_value(ty, want, val);   // 노마드 바꿔 쓰기 — 킥 n ↔ 이동 n−1 (사용자 2026-11-09)
	const int team = p.team;
	const int sk = card_sk(s, team, card, want);
	const bool own = is_own_half(team, p.pos);
	const bool fwd = (dest.x - p.pos.x) * forward_dir(team) > 0;
	int d = 0;
	switch (sk) {
		case K_SHOT_UP: d = use == U_SHOT ? 1 : -1; break;
		case K_PASS_UP: d = use == U_SHOT ? -1 : 1; break;
		case K_KEEP: case M_KEEP: case K_LONG: case M_DASH: case K_KILL: case K_RUN: case K_ONE: case K_LINK: case M_COMBO: case D_WALL: case D_WIDE: case D_SAVE: case D_COMBO: d = -1; break;
		case K_HEAVY: case M_SHORT: case K_CORNER: case D_BODY: case D_FRONT: case D_SIDE: d = 1; break;
		case K_POWER: case M_POWER: d = 2; break;
		case K_EASY: case M_BREATH: case D_EASE: d = -1; break;    // 체력 카드 (11-13)
		case K_ALLOUT: case M_SPURT: case D_DIVE: d = 1; break;
		case K_DIRECT: case M_STRAIGHT: d = fwd ? 1 : -1; break;
		case K_BUILD: case M_ESCAPE: case D_LOW: d = own ? 1 : -1; break;
		case K_STRIKER: case M_BOX: case D_HIGH: d = own ? -1 : 1; break;
		case K_TEAM: d = (s.touch_team == team ? __builtin_popcount(unsigned(s.touch_mask)) : 0) >= 3 ? 1 : -1; break;
		case K_COUNTER: case M_COUNTER: d = s.ctr_on[team] ? 1 : -1; break;   // 공 얻은 뒤 새 손 전까지 (11-14)
		case M_RETURN: case D_REGAIN: d = s.lose_fresh[team] ? 1 : -1; break;    // 공 잃은 뒤 새 손 전까지 (11-14)
		case M_OFF: d = use == U_MOVE ? 1 : -1; break;
		case M_TRICK: d = use == U_MOVE ? -1 : 1; break;
		case M_SIDE: d = p.pos.y >= 2 && p.pos.y <= 4 ? -1 : 1; break;
		case M_ALONE: {
			d = 1;
			for (const Player &q : s.players)
				if (q.team == team && q.uid != p.uid && q.placed() && manhattan(q.pos, p.pos) <= 3)
					d = -1;
			break;
		}
		case D_LAST: {
			int n = 0;
			for (const Player &q : s.players)
				if (q.team == team && q.placed() && is_own_half(team, q.pos))
					n++;
			d = n <= 1 ? 1 : -1;   // 11-14: 1명 이하 +1, 아니면 −1
			break;
		}
		default: break;
	}
	if (want == C_KICK && s.rb_kick[team])
		d += 1;
	if (want == C_MOVE && use != U_STANCE && s.rb_move[team] && (use == U_DRIBBLE || s.loose_ball()))
		d += 1;
	// 쓰임별 더함: 캐논(슛)·트릭스터(드리블) 전문가 +1, 컵 선수 스탯 (eff_stat 은 수비만 더한다 — 쓰임을 모른다)
	const int w2 = want == ty ? ty : want;
	const int st = use == U_SHOT ? ST_SHOT : use == U_PASS ? ST_PASS : use == U_DRIBBLE ? ST_DRIBBLE : -1;
	const int extra = (st >= 0 ? p.bonus[st] : 0);
	const int jk = use == U_STANCE ? 0 : jk_stat(jk_after(s, team, val));   // 족보 스트레이트·풀하우스 힘 +1 (v17b)
	// 직업 스킬 힘 +1 (11-14 — 켠 수에만): 캐논 킥 · 트릭스터 이동 카드 · 헌터 태세
	const int skp = sk_power(p.position, s.sk_passive ? 1 : s.sk_uid == p.uid ? s.sk_kind : 0, want, use) ? g_skill_pow : 0;   // 시험판 패시브 = 늘 켬
	return std::max(1, eff_stat(s, p, w2, val) + d + extra + jk + skp);
}

static bool sk_pay_ok(const State &s, int sk) { return s.pts >= sk_cost(sk); }

int card_eff(const State &s, const Player &p, int type, int value) {   // 화면 「+1」 — 족보 힘 +1 (v17b 스트레이트·풀하우스)까지
	return eff_stat(s, p, type, value) + (type == C_DEF ? 0 : jk_stat(jk_after(s, p.team, value)));
}

int card_range(const State &s, const Player &p, int card, int use, int skl) {   // 스킬(롱킥·질주 +1 · 묵직한 킥·짧은 터치 −1)까지 — 엔진이 거르는 범위와 같다
	int ty, v;
	card_info(s, p.team, card, ty, v);
	if (use < 0)
		use = ty;
	return rng_of(s, p, card, use == C_KICK ? C_KICK : C_MOVE, skl && p.position == ALLROUND ? 2 : skl);
}

// 키퍼 킥 (v15 사용자 2026-11-10): 키퍼 킥으로 낼 수 있는 카드와 그 킥 숫자 — 킥 카드만, 손에 킥 카드가 하나도 없으면 이동 카드를 노마드처럼 킥 n+1 로 (−1 = 못 냄)
int gk_card_value(const State &s, int team, int hand, int card) {
	if (card < 0 || card >= N_CARD || !((hand >> card) & 1))
		return -1;
	int ty, v;
	card_info(s, team, card, ty, v);
	if (card < 3)
		return v;
	return (hand & 7) ? -1 : use_value(C_MOVE, C_KICK, v);
}
int gk_kick_range(int value) { return base_range(C_KICK, value); }


// 그 팀이 지금 낼 수 있는 카드인가 (손 칸 또는 조커). 조커는 그 팀 역할의 종류만, 콤보 중이면 이어지는 숫자만
static bool card_ok(const State &s, int team, int card) {
	return card >= 0 && card < N_CARD && ((s.hand[team] >> card) & 1);
}

// 카드를 낸다: 손에서 뺀다 (v10 — HP가 없어 값을 내지 않는다). 손을 다 써야 새 손 6장 (v8.7 사용자 "모든 카드를 다 써야")
static void spend(State &s, const Player &p, int card) {
	int team = p.team;
	s.hand[team] &= ~(1 << card);
	s.last_card[team] = card;
	if (s.hand[team] == 0) {
		s.hand[team] = FULL_HAND;
		s.last_card[team] = -1;   // 새 손을 받았으면 되돌릴 것이 없다
		s.ctr_on[team] = s.lose_fresh[team] = 0;   // 역습 보너스·빠른 전개(공 얻은 뒤)·리커버리(잃은 뒤)는 새 손을 받으면 끝 (11-14 하나로)
	}
}

// 콤보 (v13 사용자 2026-11-04 「이어질 때마다 낸 카드 숫자만큼」): 같거나 높은 숫자를 이으면 콤보 += 그 카드에 찍힌 숫자,
// 낮으면 그 숫자부터 새로 시작. 보정(+1 등)은 힘에만 들고 콤보엔 안 든다. 닥공을 쓴 다음 카드는 숫자 + 2 (옛 +1 → +2 와 같은 「추가 1」 몫을 2로 둠)
static void combo_play(State &s, int team, int value);
// 연계·패스 앤 무브 (11-14): 숫자 순서와 상관없이 콤보 +1 — 끊지 않고 마지막 숫자도 그대로
static void combo_card(State &s, int team, int sk, int value) {
	if (sk == K_KEEP || sk == M_KEEP)
		s.combo[team] += 1;
	else
		combo_play(s, team, value);
}

static void combo_play(State &s, int team, int value) {
	int step = value;
	if (s.adv_next[team]) {   // 닥공 — 이 카드는 숫자 + 2
		step = value + 2;
		s.adv_next[team] = 0;
	}
	if (s.combo[team] > 0 && value >= s.combo_last[team])
		s.combo[team] += step;
	else
		s.combo[team] = step;
	s.combo_last[team] = value;
}

// 가담 (v16 사용자 2026-11-10): 이번 공격에서 공을 실제로 만진 서로 다른 아군 1명당 +1 (최대 5) — 옛 배율 1·3·6·10·15 없앰
int assist_value(const State &s, int team) {
	return s.touch_team == team ? std::min(5, __builtin_popcount(unsigned(s.touch_mask))) : 0;
}

int attack_value(const State &s, int team) {   // 괄호 합 (v17 — 데미지 = 슛 힘 × 이 값)
	return s.combo[team] + counter_bonus(s, team) + assist_value(s, team);
}

// ---------- 배치 (본편 3.2와 같다 — 양팀 통틀어 중복 없는 드래프트) ----------

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

static void turn_start(State &s, int team) {   // 우리 차례 시작: 태세 남은 차례 −1 (0이면 풀림) · 되감기·전진 명령은 그 차례 것
	s.poss_now[opponent(team)] = 0;
	s.lost_now[team] = s.lost_pending[team];
	s.lost_pending[team] = 0;
	s.last_card[team] = -1;
	s.adv_next[team] = 0;
	for (V2 &t : s.left_from)
		t = UNSET;
	s.lock[team] = 0;   // 잠그기는 상대 차례 하나만
	if (s.ball_owner >= 0 && s.players[s.ball_owner].team == team) {   // 버티는 힘은 공을 든 팀 차례마다 −1 (첫 차례 그대로, 하한 0 — 사용자 2026-11-04)
		if (s.hold_skip)
			s.hold_skip = 0;
		else if (s.hold > 0)
			s.hold -= 1;
	}
	for (Player &p : s.players)
		if (p.team == team && p.stance > 0 && --p.stance_left <= 0) {
			p.stance = 0;
			p.stance_left = 0;
		}
	s.pts = POINTS_PER_TURN + s.carry[team];   // 포인트 이월
	s.carry[team] = 0;
	if (s.car_on) {
		s.tturn[team] += 1;
		for (const Player &p : s.players)
			if (p.team == team)
				s.tmask[p.uid] = 0;
	}
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
		p.bound = 0;
	}
	s.save_team = -1;
	s.save_dist = s.save_pow = 0;
	s.ctr_on[0] = s.ctr_on[1] = 0;
	s.steal_turn[0] = s.steal_turn[1] = -1;
	s.ball_owner = -1;
	s.ball_pos = UNPLACED;
	s.loose_team = -1;
	s.combo[0] = s.combo[1] = s.combo_last[0] = s.combo_last[1] = 0;
	s.hold = 0;
	s.touch_team = -1;
	s.touch_mask = 0;
	jk_clear(s, 0);
	jk_clear(s, 1);
	s.hand[0] = s.hand[1] = FULL_HAND;
	s.ctr_on[0] = s.ctr_on[1] = s.lose_fresh[0] = s.lose_fresh[1] = 0;
}

// 반 시작 (후반): 킥오프 대형 · scorer 가 공 없이 먼저, 상대가 킥오프
static void restart_half(State &s, int scorer) {
	reset_shape(s);
	s.first_team = scorer;
	s.role[scorer] = R_DEF;
	s.role[opponent(scorer)] = R_ATK;
	ctr_begin(s, opponent(scorer));   // 킥오프 팀 = 공을 얻은 팀 (11-14)
	s.turn_team = scorer;
	turn_start(s, scorer);
	refresh_if_stuck(s);
}

// 골 뒤 (v15b 사용자 2026-11-10 · v15c 11-11): 반은 이어진다 — 득점한 팀은 골이 난 자리 그대로, 실점한 팀만 그 반 킥오프 대형으로
// 물러서고(득점 팀 선수가 선 자리면 가장 가까운 빈 칸), 실점한 팀 차례에 키퍼 킥(1~2장)으로 다시 시작
static void keeper_restart(State &s, int scorer) {
	const int opp = opponent(scorer);
	reset_shape(s, scorer);
	s.first_team = scorer;
	s.save_team = opp;
	s.role[opp] = R_ATK;
	s.role[scorer] = R_DEF;
	s.poss_now[opp] = 1;
	s.lost_pending[scorer] = 1;
	ctr_begin(s, opp);
	s.lose_fresh[scorer] = 1;
	s.rb_kick[scorer] = s.rb_move[scorer] = 0;
	s.turn_team = opp;
	turn_start(s, opp);
}

void new_match(State &s, int first, int tac_home, int tac_away) {
	s = State();
	for (int u = 0; u < PLAYERS; u++) {
		s.players[u].uid = u;
		s.players[u].team = u < PER_TEAM ? HOME : AWAY;
	}
	s.first_team = first;
	s.opener = first;
	s.tac_sel[HOME] = s.tac[HOME] = tac_home;
	s.tac_sel[AWAY] = s.tac[AWAY] = tac_away;
	s.keeper[0] = s.keeper[1] = g_keeper_hp;
	s.role[first] = R_DEF;
	s.role[opponent(first)] = R_ATK;
	ctr_begin(s, opponent(first));   // 킥오프 팀 = 공을 얻은 팀 (11-14)
	// 11-14 사용자 「우리 게임은 컵 모드뿐」: 직업 스킬·체력 규칙은 모든 경기에 (컵 커리어는 set_stamina 로 시작 칸만 바꾼다)
	s.car_on = 1;
	s.stam_on = 1;
	for (int u = 0; u < PLAYERS; u++)
		s.stam[u] = s.stam_cap[u] = STAM_MAX;
	s.turn_team = placement_team(s);
}

void set_lineup(State &s, int team, const int position[PER_TEAM], const int bonus[PER_TEAM][4]) {
	for (int i = 0; i < PER_TEAM; i++) {
		Player &p = s.players[team * PER_TEAM + i];
		p.position = position[i];
		for (int k = 0; k < 4; k++)
			p.bonus[k] = bonus[k < 4 ? i : i][k];
		p.range_up = p.hold_up = p.kick_range_up = 0;   // 전성기 혜택은 set_perks 로 따로 (컵만)
		p.pos = UNPLACED;
		p.placed_order = -1;
	}
	s.redeploy = 1;   // 놓기만 — 포지션은 정해졌다 (후반 재배치와 같은 길)
}

// 컵 전성기 혜택 (사용자 2026-11-09): 스프린터 이동 범위 +1 · 월 수비 지속 +1 — 선수마다
void set_perks(State &s, int team, const int range_up[PER_TEAM], const int hold_up[PER_TEAM], const int kick_range_up[PER_TEAM]) {
	for (int i = 0; i < PER_TEAM; i++) {
		Player &p = s.players[team * PER_TEAM + i];
		p.range_up = range_up[i];
		p.kick_range_up = kick_range_up ? kick_range_up[i] : 0;
		p.hold_up = hold_up[i];
	}
}

// ---------- 시간 ----------

// 골문 체력 (v16 사용자 2026-11-10 「보통 지공 한 방·보통 역습 두 번」 → v17 11-11 데미지 = 슛 힘 × 괄호 합으로 다시 잼): 값은 자가대전 슛 데미지 분포로 정했다 (docs/spec.md 「### v17」).
//   33 = 역습 중앙값 30 < 33 ≤ 지공 중앙값 36 — 지공 한 방 61% · 역습 한 방 43%(두 방 90%), 켜고 400판 골 2.9·0:0 1.0% (11-11, 옛 챔피언 800판 — spec 표).
//   옛: v16 10 (더하기 데미지) · v15 0(무조건 골) · v13 150 (곱하기 데미지). 0 이면 무조건 골 — 측정용 CF_KEEPER_HP=0
int g_keeper_hp = 33;
int g_block_mode = 2;
int g_keeper_carry = 0;
void knobs_from_env() {
	auto num = [](const char *k, int &v) { if (const char *e = getenv(k)) v = atoi(e); };
	num("CF_KEEPER_HP", g_keeper_hp);
	num("CF_HALF_TURNS", g_half_turns);   // 자가학습만 (11-18 사용자: 반 100턴씩 — 실제 게임은 CARD_HALF_TURNS 그대로)
}
int g_half_turns = CARD_HALF_TURNS;
// 0:0 끝의 AI 가치. 11-09 사용자 「0대0하면 손해라고 인식해라」로 −1 이었으나 11-11 0 으로 되돌림:
// 탐색은 상대 마디 값을 부호만 바꿔 올린다(제로섬). 「양쪽 다 −1」은 상대 쪽에서 +1 로 뒤집혀, 공을 안 가진 팀이 0:0 을 노리게 배웠다
// (11-09 갈래 0:0 86% vs 본 76% · 11-10 v16 이어 배우기가 0:0 28%→55% 로 퇴보). 0 이어도 이기면 +1 이라 AI 는 이기려 한다.
// 리그 승점 0 (양쪽 손해)은 규칙 그대로 career.gd 에 있다

// 승패 (v15 사용자 2026-11-10): 골이 많은 팀 → 같으면 넣은 골 숫자(슛 위력) 합이 큰 팀 → 그래도 같으면 −1
//   (0:0 도 다른 비김과 같다 — 11-18 특수 처리 없앰 · 숫자 합까지 같으면 컵 녹아웃만 화면이 일기토, 그 밖은 무승부). lost[t] = t 골대에 들어간 골 숫자 합
static void full_time(State &s) {
	s.phase = GAME_OVER;
	if (s.goals[0] != s.goals[1])
		s.winner = s.goals[0] > s.goals[1] ? HOME : AWAY;
	else
		s.winner = s.lost[0] == s.lost[1] ? -1 : (s.lost[1] > s.lost[0] ? HOME : AWAY);   // 상대 골대에 더 큰 숫자를 넣은 팀
}

// 후반 (v10): 전반 첫 자리로 돌아가 공격권을 바꾼다 — 전반에 공 없이 먼저 둔 팀이 이번엔 공을 갖는다. 전술카드는 다시 찬다
static void start_half2(State &s) {
	s.half = 2;
	s.half_start = s.turn_count;
	s.tac[0] = s.tac_sel[0];
	s.tac[1] = s.tac_sel[1];
	s.dbl[0] = s.dbl[1] = 0;
	s.lock[0] = s.lock[1] = 0;
	if (!g_keeper_carry)
		s.keeper[0] = s.keeper[1] = g_keeper_hp;   // 반마다 골문 체력 처음부터
	restart_half(s, opponent(s.opener));   // opener = 전반에 공 없이 먼저 둔 팀
	// v11 (사용자 2026-10-30 "후반 시작하면 선수 배치 다시"): 본편처럼 재배치 — 포지션은 그대로, 전반과 같은 순서로 두 팀이 번갈아 놓고 원정이 공을 갖고 킥오프
	for (Player &p : s.players) {
		p.pos = UNPLACED;
		p.placed_order = -1;
	}
	s.placement_index = 0;
	s.redeploy = 1;
	s.phase = PLACEMENT;
	s.turn_team = placement_team(s);
}

// 반이 끝났으면 넘기고 true — 반마다 팀당 30턴(양 팀 60), 골이 나면 그 자리에서 끝 (v10 사용자 "한골넣으면 전반종료 · 후반은 공격권 체인지")
static bool end_half(State &s) {
	if (s.half == 1) {
		start_half2(s);
		return true;
	}
	full_time(s);
	return true;
}

static bool clock(State &s) {
	return s.half_turn() >= g_half_turns && end_half(s);
}

void end_turn(State &s) {
	if (s.phase != PLAY)
		return;
	if (s.stam_on) {   // 이번 차례에 카드를 안 낸 선수 +1 — 경기 시작 칸까지 (11-13)
		for (const Player &p : s.players)
			if (p.team == s.turn_team && !((s.acted >> p.uid) & 1))
				s.stam[p.uid] = std::min(s.stam_cap[p.uid], s.stam[p.uid] + 1);
		s.acted = 0;
	}
	if (s.car_on)   // 월 대표 행동 (11-13) — 우리 차례 둘을 이어 태세로 끝냄 (신입생은 다시 켜야, 월은 지속이 길어 저절로)
		for (const Player &p : s.players)
			if (p.team == s.turn_team && p.stance > 0) {
				if (s.st_turn[p.uid] > 0 && s.st_turn[p.uid] == s.tturn[p.team] - 1)
					rep_add(s, p.uid, WALL);
				s.st_turn[p.uid] = s.tturn[p.team];
			}
	if (s.car_on)   // 턴 끝 대표 행동 (사용자 11-13 확정): 캐논 = 상대 진영에서 공을 가진 채 끝난 턴 · 타이탄 = 적 2명 이상 곁 · 부스터 = 공 가진 아군 곁
		for (const Player &p : s.players) {
			if (p.team != s.turn_team || !p.placed())
				continue;
			if (s.ball_owner == p.uid && !is_own_half(p.team, p.pos))
				rep_add(s, p.uid, CANNON);
			int foes = 0;
			bool owner = false;
			for (const Player &q : s.players)
				if (q.uid != p.uid && q.placed() && manhattan(q.pos, p.pos) == 1) {
					if (q.team != p.team)
						foes += 1;
					else if (q.uid == s.ball_owner)
						owner = true;
				}
			if (foes >= 2)
				rep_add(s, p.uid, TITAN);
			if (owner)
				rep_add(s, p.uid, BOOSTER);
		}
	if (s.car_on && s.ball_owner >= 0 && s.players[s.ball_owner].team != s.turn_team)   // 마커 대표 행동 — 공 가진 적 곁에서 턴 끝 (11-13)
		for (const Player &p : s.players)
			if (p.team == s.turn_team && p.placed() && manhattan(p.pos, s.players[s.ball_owner].pos) == 1)
				rep_add(s, p.uid, MARKER);
	for (Player &p : s.players)   // 묶인 선수는 자기 팀 차례 하나를 못 움직이고 풀린다
		if (p.team == s.turn_team)
			p.bound = 0;
	s.turn_count += 1;
	if (clock(s))
		return;
	s.turn_team = opponent(s.turn_team);
	turn_start(s, s.turn_team);
	refresh_if_stuck(s);
}

// 낼 수 있는 액션 카드가 있나 (전술카드·턴 종료는 안 센다). team 차례라고 치고 본다.
// 노마드 바꿔 쓰기(이동 ↔ 킥/수비)는 안 센다 — 카드 제 종류 그대로 낼 수 있어야 「낼 카드 있음」 (사용자 2026-11-04)
static bool can_play_card(const State &s, int team) {
	if (s.save_team == team)
		return true;   // 선방 롱패스는 늘 던질 수 있다
	State t = s;
	t.turn_team = team;
	t.pts = std::max(t.pts, 1);
	if ((t.hand[team] & 0x38) && !kickoff_turn(t))   // 이동 카드가 남았으면 거의 늘 낼 수 있다 — 탐색 속도를 위해 그때는 안 센다
		return true;
	std::vector<Action> v;
	enumerate_actions(t, v);
	for (const Action &x : v) {
		if (x.t > A_STANCE)
			continue;
		if (x.card < 0)
			return true;
		int ty = -1, val = 0;
		card_info(t, team, x.card, ty, val);
		const int want = (x.t == A_PASS || x.t == A_SHOT) ? C_KICK : x.t == A_STANCE ? C_DEF : C_MOVE;
		if (ty == want)
			return true;
	}
	return false;
}

// **낼 수 있는 액션 카드가 없으면 그 팀은 바로 새 손 6장 + 그 차례는 끝** (사용자 2026-11-04 — v9.2 「양 팀 모두 막혔을 때만 함께」를 바꿈).
// 차례가 끝나 다음 팀도 막혔으면 같은 일이 이어진다 — 시계(턴 수)가 늘 늘어나므로 끝이 있다
static void refresh_if_stuck(State &s) {
	if (s.phase != PLAY || s.pts < 1 || can_play_card(s, s.turn_team))
		return;
	const int t = s.turn_team;
	s.hand[t] = FULL_HAND;
	s.last_card[t] = -1;   // 새 손을 받았으면 되돌릴 것이 없다
	s.ctr_on[t] = s.lose_fresh[t] = 0;   // 새 손 = 역습·빠른 전개·리커버리 끝
	s.refill_n[t] += 1;    // 화면 — 새 카드 받는 연출
	end_turn(s);
}

static void after_action(State &s, int actor) {
	const int r0 = actor >= 0 && actor < 2 ? s.role[actor] : -1;
	sync_roles(s);
	if (r0 == R_ATK && s.role[actor] == R_DEF && s.phase == PLAY && s.turn_team == actor) {
		s.pts = 0;   // 공을 잃은 팀 — 남은 포인트는 사라지고 바로 상대 차례, 손은 그대로 (v15 사용자 2026-11-10)
		end_turn(s);
		return;
	}
	if (s.phase == PLAY && s.pts <= 0)
		end_turn(s);
	else
		refresh_if_stuck(s);
}

// ---------- 존 · 경합 ----------

static void build_zone(const State &s, int team, int zone[NT]) {   // 태세 선수의 상하좌우 — 먼저 놓인 선수가 주인
	for (int i = 0; i < NT; i++)
		zone[i] = -1;
	const Player *w[PER_TEAM * 2];
	int n = 0;
	for (const Player &q : s.players)
		if (q.team == team && q.placed() && q.stance > 0)
			w[n++] = &q;
	std::sort(w, w + n, [](const Player *a, const Player *b) { return a->placed_order < b->placed_order; });
	for (int i = 0; i < n; i++)
		for (const V2 &d : DIRS) {
			const int sk = w[i]->stance_sk;
			if (sk == D_FRONT && d != V2{ forward_dir(team), 0 })
				continue;   // 전방 압박 — 앞 타일만
			if (sk == D_SIDE && d.y == 0)
				continue;   // 측면 봉쇄 — 좌우 타일만
			V2 t = w[i]->pos + d;
			if (in_bounds(t) && !is_goal(t) && zone[idx(t)] == -1)
				zone[idx(t)] = w[i]->uid;
			if (sk == D_WIDE)   // 넓은 존 — 범위 +1 (두 걸음 안)
				for (const V2 &d2 : DIRS) {
					V2 t2 = t + d2;
					if (t2 != w[i]->pos && in_bounds(t2) && !is_goal(t2) && zone[idx(t2)] == -1)
						zone[idx(t2)] = w[i]->uid;
				}
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
	int lose = 0, pick = 0, cover = 0, near = 0, len = 0, mid = 0;
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
		if (f->val <= s.players[du].stance) {   // 동점은 수비 (zone_contest)
			k.lose = 1;
			const int fwd = forward_dir(me.team);
			for (const Player &q : s.players)
				if (q.team == me.team && q.placed() && !(f->carry && q.uid == me.uid) && (t.x - q.pos.x) * fwd > 0)
					k.cover--;
			k.near = -manhattan(t, goal_of(me.team));   // 우리 골에서 가까울수록 나쁨
			break;
		}
	}
	return k;
}

static bool key_less(const PathKey &a, const std::vector<V2> &pa, const PathKey &b, const std::vector<V2> &pb) {
	if (a.lose != b.lose) return a.lose < b.lose;
	if (a.pick != b.pick) return a.pick < b.pick;
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
			[&](int lb) { return !best.empty() && bk.lose == 0 && bk.pick == 0 && lb > bk.len; });   // 안 지는 길이 이미 있다 — 더 긴 길은 볼 것 없다
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
	blocked[idx(goal_of(HOME))] = blocked[idx(goal_of(AWAY))] = true;
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

static int offside_line(const State &s, int team) {
	int fwd = forward_dir(team), best = -1, bv = -999;
	for (const Player &q : s.players)
		if (q.team != team && q.placed() && q.pos.x * fwd > bv) {
			bv = q.pos.x * fwd;
			best = q.pos.x;
		}
	return best;
}

static bool is_offside(const Player &p, V2 t, int line) {
	if (line < 0)
		return false;
	int fwd = forward_dir(p.team);
	return !is_own_half(p.team, t) && t.x * fwd > p.pos.x * fwd && t.x * fwd > line * fwd;
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

// 드리블 길의 존 경합. 끝 칸(뺏기면 그 칸)을 돌려준다
static V2 dribble_run(State &s, int uid, int val, const std::vector<V2> &path) {
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
			mark_steal(s, s.players[du].team);
			return t;
		}
		s.players[du].stance = 0;   // 진 수비수는 태세가 풀린다
		s.players[du].stance_left = 0;
	}
	return path.back();
}

// ---------- 태클 (태세 선수가 공 임자 곁으로 다가가면 저절로) ----------
// 태클러 = 태세 힘 · 공 임자 = 버티는 힘(공을 다룬 직전 숫자 — s.hold). 동점이면 당한 쪽(공 임자)이 이긴다
// v9.0 사용자 "공격수 스탯은 직전의 스탯으로" — 공 임자는 카드를 내지 않는다 (차례가 끊기지 않는다)
static bool tackle_ready(const State &s, const Player &t) {   // 태세 선수가 상대 공 임자 곁(상하좌우)에 섰다
	if (t.stance <= 0 || s.ball_owner == -1 || s.ball_owner == t.uid || is_any_box(t.pos))
		return false;
	const Player &h = s.players[s.ball_owner];
	return h.team != t.team && manhattan(h.pos, t.pos) == 1;
}

static void auto_tackle(State &s, int tuid) {
	const Player &t = s.players[tuid];
	if (t.stance <= 0 || s.ball_owner == -1 || s.ball_owner == tuid)
		return;
	const Player &h = s.players[s.ball_owner];
	if (h.team == t.team || manhattan(h.pos, t.pos) != 1 || is_any_box(t.pos))
		return;
	int tv = t.stance;
	int hv = eff_hold(s);   // 체력 0인 공 임자는 0으로 버틴다 (11-17)
	bool win = tv > hv;
	emit({ 0, tuid, h.uid, h.pos, tv, hv, win ? 1 : 0, {} });

	if (win) {
		set_owner(s, tuid);
		mark_steal(s, s.players[tuid].team);
	} else {
		s.players[tuid].stance = 0;
		s.players[tuid].stance_left = 0;
	}
}

// ---------- 액션 ----------

// 이 카드를 이 쓰임(want)으로 낼 수 있나 — 프리롤은 이동 ↔ 킥/수비를 바꿔 쓴다 (v10.4)
static bool type_ok(const Player &p, int ty, int want) {
	if (ty == want)
		return true;
	return has_passive(p.position, NOMAD) && (ty == C_MOVE || want == C_MOVE);   // v12 노마드 = 옛 프리롤 능력
}

static bool can_act(const State &s, const Player &p) {   // 0칸 선수는 카드를 못 낸다 (체력 11-13 — 그 자리에 서서 몸·존은 그대로)
	return s.phase == PLAY && p.placed() && p.team == s.turn_team && s.pts >= 1 && !(s.stam_on && s.stam[p.uid] <= 0);
}

static bool may_carry(const State &s, const Player &p) {   // 공 임자 · 킥오프 차례면 킥오프 팀 누구나
	return s.ball_owner == p.uid || (kickoff_turn(s) && p.team == s.turn_team);
}

static void pay_point(State &s) { s.pts -= 1; }

static bool do_move(State &s, int uid, V2 dest, int card) {
	const Player &p = s.players[uid];
	int ty, val;
	if (!can_act(s, p) || kickoff_turn(s) || s.ball_owner == uid || p.bound || !card_ok(s, p.team, card))
		return false;
	card_info(s, p.team, card, ty, val);
	val = use_value(ty, C_MOVE, val);   // 노마드 바꿔 쓰기 — 킥 n ↔ 이동 n−1 (사용자 2026-11-09)
	const int sk = card_sk(s, p.team, card, C_MOVE);
	if (!sk_pay_ok(s, sk))
		return false;
	if (!type_ok(p, ty, C_MOVE) || !move_dest_ok(s, p, dest))   // 같은 차례에 원래 자리로 돌아가도 된다 — 막는 것은 길 한 번 안의 왕복뿐 (사용자 2026-11-07, v8.4 오해 취소)
		return false;
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	const int mv_pow0 = sk_stat(s, p, card, C_MOVE, U_MOVE, dest);
	std::vector<V2> path = g_path ? route(p.pos, dest, rng_of(s, p, card, C_MOVE), nullptr, blocked, UNSET) : move_path(s, p, dest, rng_of(s, p, card, C_MOVE), mv_pow0, blocked);
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
	int team = p.team;
	bool was_ball_team = s.ball_team() == team;
	const V2 from = p.pos;
	s.left_from[uid] = p.pos;
	emit({ 4, uid, card, dest, val, 0, 0, path });
	pay_point(s);
	if (sk_cost(sk) == 2)
		pay_point(s);
	spend(s, p, card);
	if (pk >= 0) {   // 루즈볼 줍기 — 남은 걸음은 같은 카드의 드리블
		set_owner(s, uid);
		const int mv_pow = mv_pow0;
		s.rb_move[team] = 0;
		s.hold = mv_pow;
		mark_hold(s);
		s.players[uid].pos = path[size_t(pk)];
		if (size_t(pk) + 1 < path.size())
			dest = dribble_run(s, uid, mv_pow, std::vector<V2>(path.begin() + pk + 1, path.end()));
		s.players[uid].pos = dest;
	} else {
		s.players[uid].pos = dest;
		if (is_any_box(dest)) {   // 박스 안에서는 태세 불가 (노마드)
			s.players[uid].stance = 0;
			s.players[uid].stance_left = 0;
		}
		const Player &me = s.players[uid];
		if (!tackle_ready(s, me))   // 태세인 타이탄이 공 임자 곁에 닿으면 밀지 않고 태클만 (v8.4 사용자 "밀기하지 말고 태클")
			titan_push(s, me);
		auto_tackle(s, uid);
	}
	if (path.size() >= 3 && s.players[uid].pos == path.back() && (path.back().x - from.x) * forward_dir(team) > 0)   // 스프린터 대표 행동 — 3칸 이상 이동 + 전진(도착이 출발보다 적진 쪽)
		rep_add(s, uid, SPRINTER);
	if (was_ball_team && s.ball_team() == team) {
		combo_card(s, team, sk, val);
		jk_play(s, team, val);
		if (sk == M_COMBO)
			s.combo[team] += 1;
		if (g_class_combo && s.players[uid].position == SPRINTER)
			s.combo[team] += 1;
	}
	after_action(s, team);
	return true;
}

static bool do_dribble(State &s, int uid, V2 dest, int card) {
	const Player &p = s.players[uid];
	int ty, val;
	if (!can_act(s, p) || !may_carry(s, p) || p.bound || !card_ok(s, p.team, card))
		return false;
	card_info(s, p.team, card, ty, val);
	val = use_value(ty, C_MOVE, val);   // 노마드 바꿔 쓰기 — 킥 n ↔ 이동 n−1 (사용자 2026-11-09)
	const int sk = card_sk(s, p.team, card, C_MOVE);
	if (!sk_pay_ok(s, sk))
		return false;
	if (!type_ok(p, ty, C_MOVE) || player_at(s, dest) || is_any_box(dest) || is_goal(dest))
		return false;
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	const Foe f{ &s, zone, uid, sk_stat(s, p, card, C_MOVE, U_DRIBBLE, dest), UNSET, true, 0 };
	std::vector<V2> path = route(p.pos, dest, rng_of(s, p, card, C_MOVE), &f, blocked, UNSET);
	if (path.empty())
		return false;
	int team = p.team;
	const V2 from = p.pos;
	emit({ 4, uid, card, dest, val, 0, 0, path });
	s.left_from[uid] = p.pos;
	if (s.ball_owner == -1)
		set_owner(s, uid);   // 킥오프 — 이 선수가 공을 갖고 시작한다
	const int dr_pow = sk_stat(s, p, card, C_MOVE, U_DRIBBLE, dest);
	s.rb_move[team] = 0;
	pay_point(s);
	if (sk_cost(sk) == 2)
		pay_point(s);
	spend(s, p, card);
	s.players[uid].pos = dribble_run(s, uid, dr_pow, path);
	if (s.ball_owner == uid)
		s.hold = dr_pow;   // 드리블한 숫자로 버틴다
		mark_hold(s);
	if (s.ball_team() == team) {
		combo_card(s, team, sk, val);
		jk_play(s, team, val);
		if (sk == M_COMBO)
			s.combo[team] += 1;
		if (g_class_combo && s.ball_owner == uid && s.players[uid].position == TRICKSTER)
			s.combo[team] += 1;
	}
	if (s.ball_owner == uid) {   // 대표 행동 (11-13): 전진 드리블 성공 = 트릭스터 · 3칸 이상 끝까지 = 스프린터
		if ((s.players[uid].pos.x - from.x) * forward_dir(team) > 0)
			rep_add(s, uid, TRICKSTER);
		if (path.size() >= 3 && s.players[uid].pos == path.back() && (path.back().x - from.x) * forward_dir(team) > 0)
			rep_add(s, uid, SPRINTER);
	}
	after_action(s, team);
	return true;
}

// 패스 목적지 (도달 칸 + 공중볼 칸). 5칸 이상(마에스트로 킥 4)은 몸·골대 위를 넘는 공중볼
static void pass_targets(const State &s, const Player &p, int rng, Reach &ground, Reach &air) {
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	blocked[idx(goal_of(HOME))] = blocked[idx(goal_of(AWAY))] = true;
	reach(p.pos, rng, blocked, UNSET, ground);
	if (rng >= LONG_PASS_MIN) {
		bool open[NT] = {};
		reach(p.pos, rng, open, UNSET, air);
	}
}

static bool pass_dest_ok(const State &s, const Player &p, V2 t) {
	if (is_any_box(t) || is_goal(t))
		return false;
	const Player *r = player_at(s, t);
	if (!r)
		return true;   // 빈 칸(루즈볼)은 오프사이드 공간이어도 된다 (v7.7) — 어디든 (v9.1: 낼 카드가 없으면 손을 새로 받으니 막힘이 없다)
	return r->team == p.team && r->uid != p.uid && !is_offside(p, t, offside_line(s, p.team));
}

static bool send_ball(State &s, const Player &p, int val, const std::vector<V2> &path) {   // true = 끊겼다
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
			mark_steal(s, s.players[du].team);
			return true;
		}
		if (s.players[du].stance_sk == D_SAVE)
			s.half_dmg = 1;   // 몸으로 막기 — 경합에 지면 슛 피해 −50%
		s.players[du].stance = 0;
		s.players[du].stance_left = 0;
	}
	return false;
}

static bool do_pass(State &s, int uid, V2 dest, int card) {
	const Player &p = s.players[uid];
	int ty, val;
	if (!can_act(s, p) || !may_carry(s, p) || !card_ok(s, p.team, card))
		return false;
	card_info(s, p.team, card, ty, val);
	val = use_value(ty, C_KICK, val);   // 노마드 바꿔 쓰기 — 킥 n ↔ 이동 n−1 (사용자 2026-11-09)
	const int sk = card_sk(s, p.team, card, C_KICK);
	if (!sk_pay_ok(s, sk))
		return false;
	if (!type_ok(p, ty, C_KICK) || marked(s, p) || !pass_dest_ok(s, p, dest))
		return false;
	int rng = rng_of(s, p, card, C_KICK);
	if (manhattan(p.pos, dest) > rng)
		return false;
	std::vector<V2> path;
	bool aerial = manhattan(p.pos, dest) >= LONG_PASS_MIN;
	if (aerial && manhattan(p.pos, dest) > g_air_max)
		return false;
	if (aerial) {
		path.push_back(dest);
	} else {
		int zone[NT];
		build_zone(s, opponent(p.team), zone);
		bool blocked[NT];
		body_blocked(s, p.team, blocked);
		const Foe f{ &s, zone, uid, sk_stat(s, p, card, C_KICK, U_PASS, dest), UNSET, false, 0 };
		path = route(p.pos, dest, rng, &f, blocked, UNSET);
		if (path.empty())
			return false;
	}
	int team = p.team;
	const Player *recv = player_at(s, dest);
	int ru = recv ? recv->uid : -1;
	emit({ 4, uid, card, dest, val, aerial ? 1 : 0, 0, path });
	if (s.ball_owner == -1)
		set_owner(s, uid);
	const int ps_pow = sk_stat(s, p, card, C_KICK, U_PASS, dest);
	s.rb_kick[team] = 0;
	pay_point(s);
	if (sk_cost(sk) == 2)
		pay_point(s);
	spend(s, p, card);
	if (!send_ball(s, p, ps_pow, path)) {
		if (ru != -1) {
			set_owner(s, ru);
			s.hold = ps_pow;   // 받은 패스의 숫자로 버틴다
			mark_hold(s);
		} else {
			drop_ball(s, dest, team);
		}
	}
	if (s.ball_team() == team) {
		combo_card(s, team, sk, val);
		jk_play(s, team, val);
		if (sk == K_LINK)
			s.combo[team] += 1;
		if (g_class_combo && s.players[uid].position == MAESTRO)
			s.combo[team] += 1;
		if (sk == K_KILL)
			s.rb_kick[team] = 1;
		if (sk == K_RUN)
			s.rb_move[team] = 1;
		if ((dest.x - s.players[uid].pos.x) * forward_dir(team) > 0)
			rep_add(s, uid, MAESTRO);   // 대표 행동 — 전진 패스 성공 (11-13)
	}
	after_action(s, team);
	return true;
}

// 슛 데미지 (v17 사용자 2026-11-11 「데미지 = 슛힘(스탯) × 콤보(콤보 + 가담 + 전진(역습))」) = 데미지 힘 × 괄호 합.
//   괄호 합 = 콤보 합(슛 카드 숫자까지) + 역습 + 가담 (attack_value — 화면 콤보 박스 숫자)
//   데미지 힘 = 그 슛 카드의 힘 (sk_stat — 카드 숫자 · 캐논 · 부스터 · 스킬 · 족보 · 컵 스탯, 슛 길 경합의 힘과 같다) + 아래 곱하기를 바꾼 더하기 (하한 1).
//   옛 곱하기·나누기 (v16 까지) → 힘 더하기 (v17 — docs/spec.md 「### v17」 표, 슛 힘 실측 중앙값 3 · 평균 3.4 에서 옛 배율에 가깝게):
//   한 방 ×1.5 → 데미지 힘 +3 (경합 −1 그대로 — 카드 힘보다 +2, 힘 3.4 에서 ×1.6) · 코너 공략 ×½ → 데미지 힘 −3 (경합 +1 그대로 — 카드 힘보다 −2, ×0.4)
//   · 몸으로 막기 ×½ → 힘 −2 (×0.4) · 강슛 ×2 → 힘 +3 (×1.9) · 육탄방어 ×½ → 힘 −2 (전술카드는 지금 경기에 없다 — TACTICS_ON false). 하한 1 (0 금지)
constexpr int ONE_PW = 3, CORNER_PW = 3, SAVE_PW = 2, DBL_PW = 3, LOCK_PW = 2;
static int dmg_power(const State &s, int team, int sk, int pow, bool dbl, bool body) {
	int pw = pow;
	if (sk == K_ONE)
		pw += ONE_PW;     // 경합이 없어도 든다 (사용자 2026-11-07)
	if (sk == K_CORNER)
		pw -= CORNER_PW;
	if (dbl)
		pw += DBL_PW;
	if (s.lock[opponent(team)])
		pw -= LOCK_PW;    // 골문 잠그기
	if (body)
		pw -= SAVE_PW;    // 몸으로 막기 수비수가 슛 길 경합에 졌다
	return std::max(1, pw);
}

// 슛 미리보기 (사용자 2026-11-04 「콤보박스 숫자 늘 표시」) — do_shot 과 같은 셈(슛 힘·카드 내기·콤보·역습·가담·두 배·잠그기),
// 길이 막히거나 끊기는지·몸으로 막기는 보지 않는다. out = 쏜 뒤 콤보 · 역습 · 가담 · 데미지 힘 (v17 화면 내역). 쏠 수 없으면 false. 상태는 바꾸지 않는다
bool shot_parts(const State &s0, int uid, int card, int out[4]) {
	if (uid < 0 || uid >= PLAYERS)
		return false;
	State s = s0;
	const Player &p = s.players[uid];
	int ty, val;
	if (!can_act(s, p) || s.ball_owner != uid || !card_ok(s, p.team, card))
		return false;
	card_info(s, p.team, card, ty, val);
	val = use_value(ty, C_KICK, val);   // 노마드 바꿔 쓰기 — 킥 n ↔ 이동 n−1 (사용자 2026-11-09)
	if (!type_ok(p, ty, C_KICK))
		return false;
	int team = p.team;
	const int sk = card_sk(s, team, card, C_KICK);
	const int pow = sk_stat(s, p, card, C_KICK, U_SHOT, opponent_goal(team));
	const int ctr = counter_bonus(s, team);
	spend(s, p, card);
	combo_card(s, team, sk, val);
	if (sk == K_LINK)
		s.combo[team] += 1;
	out[0] = s.combo[team];
	out[1] = ctr;
	out[2] = assist_value(s, team);
	out[3] = dmg_power(s, team, sk, pow, s.dbl[team], false);
	return true;
}

int shot_damage(const State &s, int uid, int card) {
	int o[4];
	return shot_parts(s, uid, card, o) ? o[3] * (o[0] + o[1] + o[2]) : -1;
}

static bool do_shot(State &s, int uid, int card) {
	const Player &p = s.players[uid];
	int ty, val;
	if (!can_act(s, p) || s.ball_owner != uid || !card_ok(s, p.team, card))
		return false;
	card_info(s, p.team, card, ty, val);
	val = use_value(ty, C_KICK, val);   // 노마드 바꿔 쓰기 — 킥 n ↔ 이동 n−1 (사용자 2026-11-09)
	const int sk = card_sk(s, p.team, card, C_KICK);
	if (!sk_pay_ok(s, sk))
		return false;
	if (!type_ok(p, ty, C_KICK) || marked(s, p))
		return false;
	V2 goal = opponent_goal(p.team);
	int rng = rng_of(s, p, card, C_KICK);
	int zone[NT];
	build_zone(s, opponent(p.team), zone);
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
	const Foe f{ &s, zone, uid, sk_stat(s, p, card, C_KICK, U_SHOT, goal), UNSET, false, 0 };
	std::vector<V2> path = route(p.pos, goal, rng, &f, blocked, goal);
	if (path.empty())
		return false;
	int team = p.team;
	emit({ 4, uid, card, goal, val, 2, 0, path });
	int pow = sk_stat(s, p, card, C_KICK, U_SHOT, goal);   // 슛 길 경합의 힘 = 데미지 힘 (v17)
	const int ctr = counter_bonus(s, team);   // 슛하는 순간의 공 위치로 (새 손을 받기 전)
	s.rb_kick[team] = 0;
	pay_point(s);
	if (sk_cost(sk) == 2)
		pay_point(s);
	spend(s, p, card);
	combo_card(s, team, sk, val);   // 슛도 콤보에 든다 — 피해에 슛 자신도 든다
	jk_play(s, team, val);
	s.half_dmg = 0;
	const bool dbl = s.dbl[team];   // 두 배 — 쏘면 성공하든 막히든 쓴 것
	s.dbl[team] = 0;
	if (send_ball(s, p, pow, path)) {
		after_action(s, team);
		return true;
	}
	if (sk == K_LINK)
		s.combo[team] += 1;
	int opp = opponent(team);
	// 슛 데미지 (v17) = 데미지 힘 × (콤보 합 + 역습 + 가담) — 승패의 둘째 기준(골 숫자 합)이자 골문 체력을 깎는 값
	const int pw = dmg_power(s, team, sk, pow, dbl, s.half_dmg);
	int dmg = pw * (s.combo[team] + ctr + assist_value(s, team));   // 역습은 쏘기 전 값 (슛 카드로 새 손을 받아도)
	s.last_ctr = ctr;
	s.last_pw = pw;
	if (s.keeper[opp] > 0 && dmg < s.keeper[opp]) {   // 골문 체력을 못 넘었다 — 막힘, 깎인 만큼 남는다 (v16). 막힌 슛은 골 숫자 합에 안 든다
		s.keeper[opp] -= dmg;
		s.blocks[team] += 1;
		s.last_dmg = dmg;
		s.last_hit_team = team;
		emit({ 5, team, dmg, goal, s.keeper[opp], s.combo[team], 0, path, pw });   // 막힘 — va 남은 골문 체력 · vb 콤보 · vc 데미지 힘
		s.combo[team] = s.combo_last[team] = 0;
		s.touch_team = -1;   // 공격 끝 — 가담·족보도 새로 센다 (키퍼 킥이 빈 칸에 떨어져도 옛 공격의 가담이 남지 않게, v17)
		s.touch_mask = 0;
		jk_clear(s, 0);
		jk_clear(s, 1);
		if (g_block_mode == 2) {
			// 선방 (사용자 2026-10-30): 공은 막은 팀 골키퍼 손 — 그 팀 차례로 넘어가 첫 수로 카드 1장을 버리고 롱패스.
			// 막는 순간 공수교대 (사용자 2026-11-04 「어차피 공수교대 — 카드 새 걸로 바뀌잖아」): 막은 팀 = 공격 새 손 6장, 슛한 팀 = 수비 새 손.
			// 키퍼는 그 공격 손에서 한 장을 차고, 받은 뒤엔 이미 공격 팀이라 손을 다시 받지 않는다
			s.ball_owner = -1;
			s.ball_pos = UNPLACED;
			s.loose_team = -1;
			s.save_team = opp;
			s.save_dist = s.save_pow = 0;
			s.ctr_on[team] = 0;
			s.steal_turn[team] = -1;
			s.role[opp] = R_ATK;   // 막아도 손은 그대로 (사용자 2026-11-06)
			s.role[team] = R_DEF;
			s.poss_now[opp] = 1;
			s.lost_pending[team] = 1;
			ctr_begin(s, opp);
			s.ctr_on[team] = 0;
			s.lose_fresh[team] = 1;
			s.lose_fresh[opp] = 0;
			s.rb_kick[team] = s.rb_move[team] = 0;
			s.turn_count += 1;
			if (!clock(s)) {
				s.turn_team = opp;
				turn_start(s, opp);
			}
			return true;
		}
		if (g_block_mode == 0) {
			drop_ball(s, path.size() >= 2 ? path[path.size() - 2] : p.pos, team);   // 골문 앞 리바운드
		} else {
			int best = -1, bd = 1 << 20;
			for (const Player &q : s.players)
				if (q.team == opp && q.placed() && manhattan(q.pos, goal) < bd)
					bd = manhattan(q.pos, goal), best = q.uid;
			set_owner(s, best);
		}
		after_action(s, team);
		return true;
	}
	s.lost[opp] += dmg;
	if (s.keeper[opp] > 0)
		s.keeper[opp] = g_keeper_hp;   // 골이 난 골문은 다시 채운다 (v16 — 일기토처럼 골문 체력이 없는 판은 0 그대로)
	s.goals[team] += 1;
	s.last_dmg = dmg;
	s.last_hit_team = team;
	emit({ 2, team, dmg, goal, pow, s.combo[team], 1, {}, pw });   // va 슛 힘(경합 값) · vb 콤보 · vc 데미지 힘 — 역습 보너스는 화면이 쏘기 전 상태(ctr)로 안다
	s.combo[team] = s.combo_last[team] = 0;
	s.turn_count += 1;   // 넣은 팀의 차례는 여기서 끝 — 반은 턴 시계로만 끝난다 (v15b, 옛 v10 「골 = 반 끝」 없앰)
	if (!clock(s))
		keeper_restart(s, team);
	return true;
}

// 키퍼 킥 (v15 사용자 2026-11-10): 골문 체력을 켠 측정에서만 — 막은 팀이 킥 1~2장 · 장당 AP 1 · 거리 = 킥 범위 합 · 받은 선수 버티는 힘 = 낸 카드 중 큰 숫자.
// 인터셉트·경합 없음 · 박스 안으로는 못 찬다. 첫 장 확정 = dest 가 우리 골대 칸
static bool do_save(State &s, V2 dest, int c1) {
	const int team = s.save_team;
	if (team < 0 || s.turn_team != team)
		return false;
	std::vector<Action> acts;
	enumerate_actions(s, acts);
	bool ok = false;
	for (const Action &x : acts)
		ok = ok || (x.dest == dest && x.card == c1);
	if (!ok)
		return false;
	const int v = gk_card_value(s, team, s.hand[team], c1);
	s.hand[team] &= ~(1 << c1);
	if (s.hand[team] == 0) {
		s.hand[team] = FULL_HAND;
		s.ctr_on[team] = s.lose_fresh[team] = 0;
	}
	pay_point(s);
	if (s.save_dist == 0 && dest == goal_of(team)) {   // 첫 장 확정 — 둘째 장을 이어 낸다
		s.save_dist = gk_kick_range(v);
		s.save_pow = v;
		return true;
	}
	const int two = s.save_dist > 0;
	const int pow = std::max(v, s.save_pow);
	const int dist = s.save_dist + gk_kick_range(v);
	s.gk_kicks[two] += 1;
	s.save_team = -1;
	s.save_dist = s.save_pow = 0;
	const Player *rq = player_at(s, dest);
	emit({ 4, -1, c1, dest, dist, 1, 0, { dest } });
	if (rq) {
		set_owner(s, rq->uid);
		s.hold = pow;
		mark_hold(s);
	} else {
		drop_ball(s, dest, team);
	}
	after_action(s, team);
	return true;
}

static bool can_stance(const State &s, const Player &p) {
	return can_act(s, p) && s.ball_owner != p.uid && !kickoff_turn(s) && p.stance == 0 && !is_any_box(p.pos);
}

static bool do_stance(State &s, int uid, int card) {
	const Player &p = s.players[uid];
	int ty, val;
	if (!can_stance(s, p) || !card_ok(s, p.team, card))
		return false;
	card_info(s, p.team, card, ty, val);
	const int sk = card_sk(s, p.team, card, C_DEF);
	if (!sk_pay_ok(s, sk))
		return false;
	if (!type_ok(p, ty, C_DEF) || marked(s, p) || eff_stat(s, p, C_DEF, val) < 1)   // 실효 0 태세는 못 낸다 (사용자 2026-10-30 "마커에게 붙은 선수는 수비 1 못 내게")
		return false;
	int team = p.team;
	const int st_pow = sk_stat(s, p, card, C_DEF, U_STANCE, p.pos);
	pay_point(s);
	if (sk_cost(sk) == 2)
		pay_point(s);
	spend(s, p, card);
	Player &pm = s.players[uid];
	pm.stance = st_pow;
	pm.stance_sk = sk;
	pm.stance_left = (has_passive(p.position, WALL) ? 2 : 1) + (sk == D_WALL ? 1 : 0);   // 월 = 총 2턴 (11-14 — 전성기 +1 없앰)   // 1턴 지속 (사용자 "수비카드 지속1") · 월은 +1 (v12)

	// v13 (사용자 2026-11-04): 수비 태세는 콤보를 쌓지도 끊지도 않는다 (수비 팀 콤보는 공을 잃을 때 이미 0)
	auto_tackle(s, uid);   // 공 임자 곁에서 켜도 태클 — 태세 힘 vs 버티는 힘 (사용자 2026-11-04 「공 옆에 도착해서 수비 켜도 발동」)
	after_action(s, team);
	return true;
}

static int discard_slot(int hand) {   // 가장 높은 숫자 카드 (같으면 킥·수비 쪽 먼저)
	static const int ORDER[6] = { 2, 1, 5, 0, 4, 3 };
	for (int c : ORDER)
		if ((hand >> c) & 1)
			return c;
	return -1;
}

static bool bind_ok(const State &s, int team, int target) {
	if (target < 0 || target >= PLAYERS)
		return false;
	const Player &q = s.players[target];
	if (q.team == team || !q.placed() || q.bound)
		return false;
	for (const Player &m : s.players)   // 압박은 우리 선수 곁(상하좌우)의 적에게만 (v8.2 사용자 "곁의 적만")
		if (m.team == team && m.placed() && manhattan(m.pos, q.pos) == 1)
			return true;
	return false;
}

static bool undo_ok(const State &s, int team) {
	int c = s.last_card[team];
	return c >= 0 && c < N_CARD && !((s.hand[team] >> c) & 1);
}

static bool do_tactic(State &s, int kind, int target) {
	int team = s.turn_team;
	if (s.phase != PLAY || s.pts < 1 || kickoff_turn(s) || kind < 0 || kind >= N_TAC || !((s.tac[team] >> kind) & 1))
		return false;
	int opp = opponent(team);
	if (kind == T_DISCARD && discard_slot(s.hand[opp]) < 0)
		return false;
	if (kind == T_BIND && !bind_ok(s, team, target))
		return false;
	if (kind == T_UNDO && !undo_ok(s, team))
		return false;
	pay_point(s);
	s.tac[team] &= ~(1 << kind);
	if (kind == T_NEWHAND) {
		s.hand[team] = FULL_HAND;
		s.ctr_on[team] = s.lose_fresh[team] = 0;
	} else if (kind == T_DISCARD) {
		s.hand[opp] &= ~(1 << discard_slot(s.hand[opp]));
	} else if (kind == T_BIND) {
		s.players[target].bound = 1;
	} else if (kind == T_UNDO) {
		s.hand[team] |= 1 << s.last_card[team];   // HP는 돌려받지 않는다 — 카드만
		s.last_card[team] = -1;
	} else if (kind == T_ADVANCE) {
		s.adv_next[team] = 1;
	} else if (kind == T_DOUBLE) {
		s.dbl[team] = 1;
	} else if (kind == T_CARRY) {
		s.carry[team] = s.pts;   // 남은 포인트(0~1)를 넘기고 턴을 끝낸다
		s.pts = 0;
	} else if (kind == T_LOCK) {
		s.lock[team] = 1;
	}
	after_action(s, team);
	return true;
}

// 버티는 힘의 실제 값 — s.hold 는 숫자를 그대로 두고(차례마다 −1 도 그대로), 공 임자 체력이 0인 동안만 0 (사용자 11-17 「체력 0인 선수는 버티는 힘 0」)
int eff_hold(const State &s) {
	if (s.stam_on && s.ball_owner >= 0 && s.stam[s.ball_owner] <= 0)
		return 0;
	return s.hold;
}

int stam_cost(int sk) { return sk == K_EASY || sk == M_BREATH || sk == D_EASE ? 0 : sk == K_ALLOUT || sk == M_SPURT || sk == D_DIVE ? 2 : 1; }

// ---------- 시험판 (11-18 사용자 「혼자하기에 4대4·5대5 · 패시브로 적용 · 전술카드도 다」) ----------
// 시험판 덱 화면이 고르는 카드 = 게임 카드 목록 그대로 (core/career.gd KICK_CARDS·MOVE_CARDS·DEF_CARDS — 뺀 12장 5·9·10·14·20·28·31·34·39·43·45·47 없음,
// tests/test_card_ai.gd 가 두 목록이 같은지 본다). 그 숫자에 못 붙는 카드(skill_fits — MOVE_MINUS·MOVE_SHORT 칸 규칙)는 뺀다
static const int TRIAL_POOL[3][13] = {
	{ K_SHOT_UP, K_PASS_UP, K_LONG, K_HEAVY, K_KILL, K_RUN, K_DIRECT, K_BUILD, K_STRIKER, K_TEAM, K_COUNTER, K_KEEP, K_EASY },
	{ M_OFF, M_TRICK, M_DASH, M_SHORT, M_STRAIGHT, M_SIDE, M_ALONE, M_ESCAPE, M_BOX, M_COUNTER, M_RETURN, M_KEEP, M_BREATH },
	{ D_WALL, D_WIDE, D_FRONT, D_SIDE, D_LAST, D_LOW, D_HIGH, D_REGAIN, D_EASE, 0, 0, 0, 0 },
};
static const int TRIAL_POOL_N[3] = { 13, 13, 9 };

int trial_deck_options(int kind, int value, int out[13]) {
	int n = 0;
	for (int i = 0; i < TRIAL_POOL_N[kind]; i++)
		if (skill_fits(TRIAL_POOL[kind][i], value))
			out[n++] = TRIAL_POOL[kind][i];
	return n;
}

void set_trial(State &s, int team_n) {   // 덱은 화면이 고른 대로 set_deck 으로 (11-18 「전술카드 직접 선택」)
	s.team_n = std::max(1, std::min(PER_TEAM, team_n));
	s.sk_passive = 1;
	s.turn_team = placement_team(s);
}

void set_stamina(State &s, int team, const int stam[PER_TEAM]) {
	if (!s.stam_on)   // 처음 켜면 두 팀 모두 가득에서 — 부르지 않은 팀(상대)은 3칸
		for (int u = 0; u < PLAYERS; u++)
			s.stam[u] = s.stam_cap[u] = STAM_MAX;
	s.stam_on = 1;
	for (int i = 0; i < PER_TEAM; i++) {
		const int u = team * PER_TEAM + i;
		s.stam[u] = s.stam_cap[u] = std::max(0, std::min(STAM_MAX, stam[i]));
	}
}

static bool apply_inner(State &s, const Action &a);
// 체력 (11-13): 카드를 낸 선수가 그 카드 몫만큼 칸을 쓴다 (하한 0). 실패한 수는 되돌린다
// 직업 스킬 (11-14): a.sk = 1 이면 체력 SKILL_STAM 을 더 내고 그 수의 카드에만 효과(rng_of·sk_stat — s.sk_uid 로 안다)
bool apply_action(State &s, const Action &a) {
	if (a.sk) {
		if (a.sk < 0 || a.sk > SK_KINDS_MAX || a.t < A_MOVE || a.t > A_STANCE || a.uid < 0 || a.uid >= PLAYERS || s.phase != PLAY || !skill_can(s, s.players[a.uid]) ||
				!skill_act(s.players[a.uid].position, a.t, a.sk))
			return false;
	}
	if ((!s.stam_on && !s.car_on) || a.t < A_MOVE || a.t > A_STANCE || a.t == A_SAVE || a.uid < 0 || a.uid >= PLAYERS || s.phase != PLAY)
		return apply_inner(s, a);
	const int want = a.t == A_PASS || a.t == A_SHOT ? C_KICK : a.t == A_STANCE ? C_DEF : C_MOVE;
	const Player &p = s.players[a.uid];
	const int team = p.team;
	const bool card = a.card >= 0 && a.card < N_CARD;
	const int cost = a.sk ? g_skill_stam : card ? stam_cost(card_sk(s, team, a.card, want)) : 1;   // 스킬을 켜면 체력 최종 2 (11-14 정정 — 카드 몫에 더하지 않는다)
	int ty = want, val = 0;
	if (card)
		card_info(s, team, a.card, ty, val);
	const int old_acted = s.acted;
	s.acted |= 1 << a.uid;   // 먼저 표시 — 수 안에서 차례가 끝나도(end_turn) 이 선수는 쉰 것으로 안 센다
	s.sk_uid = a.sk ? a.uid : -1;
	s.sk_kind = a.sk;
	const bool ok = apply_inner(s, a);
	s.sk_uid = -1;
	s.sk_kind = 0;
	if (!ok) {
		s.acted = old_acted;
		return false;
	}
	if (s.stam_on)
		s.stam[a.uid] = std::max(0, s.stam[a.uid] - cost);   // 값은 수가 끝난 뒤에 — 수 안의 can_act 는 낸 뒤 칸이 아니라 지금 칸을 본다
	if (s.car_on) {
		if (has_passive(p.position, NOMAD) && ty != want)
			rep_add(s, a.uid, NOMAD);   // 노마드 대표 행동 — 카드 바꿔 내기
		else if (!has_passive(p.position, NOMAD) && s.tmask[a.uid] && !(s.tmask[a.uid] & (1 << want)) && __builtin_popcount(s.tmask[a.uid]) == 1)
			rep_add(s, a.uid, NOMAD);   // 신입생 셈 — 한 차례에 다른 종류 카드 둘 (바꿔 쓰기 능력이 없다)
		s.tmask[a.uid] |= 1 << want;
		if (a.sk) {
			if (s.fired[a.uid] < 255)
				s.fired[a.uid] += 1;
			emit({ 6, a.uid, p.position, p.pos, 0, 0, 0, {} });
		}
	}
	return true;
}

// 결과가 다른 길 후보 (11-18 사용자 「무슨 액션이든 승리가 목적」 — 승률 판단은 AI 가 한다, 휴리스틱이 대신하지 않는다).
// 같은 행동(같은 도착 칸·카드)을 길만 바꿔 다 해 보고, 끝 상태가 똑같은 길끼리 한 묶음 — 묶음마다 대표 하나(기본 규칙 PathKey 1등).
// out[0] = 기본 길(route 가 고르는 길)의 묶음. 이동·드리블·땅볼 패스·슛만 — 공중볼·그 밖은 빈 목록. 상태는 바꾸지 않는다
void outcome_paths(const State &s, const Action &a, std::vector<std::vector<V2>> &out) {
	out.clear();
	if (a.t < A_MOVE || a.t > A_SHOT || a.uid < 0 || a.uid >= PLAYERS || a.card < 0 || g_path)
		return;
	const Player &p = s.players[a.uid];
	const bool kick = a.t == A_PASS || a.t == A_SHOT;
	const V2 dest = a.t == A_SHOT ? opponent_goal(p.team) : a.dest;
	if (a.t == A_PASS && manhattan(p.pos, dest) >= LONG_PASS_MIN)
		return;
	bool blocked[NT];
	body_blocked(s, p.team, blocked);
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
	walk_paths(p.pos, dest, card_range(s, p, a.card, kick ? C_KICK : C_MOVE, a.sk), blocked, a.t == A_SHOT ? dest : UNSET,
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

static bool apply_inner(State &s, const Action &a) {
	switch (a.t) {
		case A_PLACE: return do_place(s, a.uid, a.dest);
		case A_MOVE: return a.uid >= 0 && a.uid < PLAYERS && do_move(s, a.uid, a.dest, a.card);
		case A_DRIBBLE: return a.uid >= 0 && a.uid < PLAYERS && do_dribble(s, a.uid, a.dest, a.card);
		case A_PASS: return a.uid >= 0 && a.uid < PLAYERS && do_pass(s, a.uid, a.dest, a.card);
		case A_SHOT: return a.uid >= 0 && a.uid < PLAYERS && do_shot(s, a.uid, a.card);
		case A_SAVE: return do_save(s, a.dest, a.card);
		case A_STANCE: return a.uid >= 0 && a.uid < PLAYERS && do_stance(s, a.uid, a.card);
		case A_TACTIC: return do_tactic(s, a.uid, a.dest.x);
		case A_END: {
			if (s.phase != PLAY || s.pts <= 0 || kickoff_turn(s))
				return false;
			std::vector<Action> legal;   // 낼 카드가 남았으면 못 끝낸다 (v8.9)
			enumerate_actions(s, legal);
			bool ok = false;
			for (const Action &x : legal)
				ok = ok || x.t == A_END;
			if (!ok)
				return false;
			s.pts = 0;
			end_turn(s);
			return true;
		}
	}
	return false;
}

// ---------- 열거 ----------

static int team_cards(const State &s, int team, int type, int out[8]) {   // 그 종류로 낼 수 있는 카드 번호
	int n = 0;
	for (int c = 0; c < N_CARD; c++)
		if (card_ok(s, team, c) && slot_type(s.role[team], c) == type)
			out[n++] = c;
	return n;
}

void enumerate_actions(const State &s, std::vector<Action> &out) {
	out.clear();
	if (s.phase == PLACEMENT) {
		int team = placement_team(s);
		if (team < 0)
			return;
		int x0 = team == HOME ? 0 : W / 2;
		for (int slot = 0; slot <= SLOT_PROSPECT; slot++) {
			if (!slot_open(s, team, slot))
				continue;
			for (int x = x0; x < x0 + W / 2; x++)
				for (int z = 0; z < H; z++) {
					V2 t{ x, z };
					if (place_tile_ok(s, team, t, false))   // 카드 모드 노마드는 본편 박스 특권이 없다
						out.push_back({ A_PLACE, slot, t, -1 });
				}
		}
		return;
	}
	if (s.phase != PLAY)
		return;
	if (s.save_team >= 0) {   // 키퍼 킥만 — 우리 선수·빈 칸 (박스 칸은 선수가 있어도 ×)
		const int team = s.save_team;
		if (s.turn_team != team)
			return;
		const V2 g = goal_of(team);
		// 키퍼 킥 (v15) — 첫 장: 바로 차기(1장) 또는 확정(dest = 우리 골대 칸) · 둘째 장: 범위 합 안으로
		for (int c = 0; c < N_CARD; c++) {
			const int v = gk_card_value(s, team, s.hand[team], c);
			if (v < 0)
				continue;
			const int reach_d = s.save_dist + gk_kick_range(v);
			for (int x = 0; x < W; x++)
				for (int z = 0; z < H; z++) {
					V2 t{ x, z };
					const Player *q = player_at(s, t);
					if (manhattan(g, t) > reach_d || (q && q->team != team) || is_any_box(t) || is_goal(t))
						continue;
					out.push_back({ A_SAVE, -1, t, c });
				}
			if (s.save_dist == 0 && s.pts >= 2) {   // 둘째 장을 낼 수 있어야 확정
				int h2 = s.hand[team] & ~(1 << c);
				if (h2 == 0)
					h2 = FULL_HAND;
				bool more = false;
				for (int c2 = 0; c2 < N_CARD; c2++)
					more = more || gk_card_value(s, team, h2, c2) >= 0;
				if (more)
					out.push_back({ A_SAVE, -1, g, c });
			}
		}
		return;
	}
	if (s.pts < 1)
		return;
	const int team = s.turn_team;
	const bool ko = kickoff_turn(s);
	int mv0[8], kk0[8], df0[8];
	const int nm0 = team_cards(s, team, C_MOVE, mv0), nk0 = team_cards(s, team, C_KICK, kk0), nd0 = team_cards(s, team, C_DEF, df0);
	bool blocked[NT];
	body_blocked(s, team, blocked);
	// skl = 직업 스킬을 켠 수 (11-14) — 그 직업 스킬이 붙는 수만, 범위·힘은 스킬을 더한 값으로 한 번 더 훑는다
	auto gen = [&](const Player &q, int sk) {   // sk = Action.sk (0 = 스킬 없음 · 만능은 1·2 둘 다 훑는다)
		const bool skl = sk != 0;
		const bool do_mv = !skl || skill_act(q.position, A_MOVE, sk), do_kk = !skl || skill_act(q.position, A_PASS, sk), do_df = !skl || skill_act(q.position, A_STANCE, sk);
		const int u = q.uid;
		const bool carry = may_carry(s, q);
		// 프리롤은 이동 카드를 킥/수비로, 킥/수비 카드를 이동으로도 쓴다 — 이 선수만의 카드 묶음 (v10.4)
		int mv[12], kk[12], df[12];
		int nm = nm0, nk = nk0, nd = nd0;
		std::copy(mv0, mv0 + nm0, mv);
		std::copy(kk0, kk0 + nk0, kk);
		std::copy(df0, df0 + nd0, df);
		if (has_passive(q.position, NOMAD)) {
			for (int i = 0; i < nk0; i++)
				mv[nm++] = kk0[i];
			for (int i = 0; i < nd0; i++)
				mv[nm++] = df0[i];
			for (int i = 0; i < nm0; i++) {
				if (s.role[team] == R_ATK)
					kk[nk++] = mv0[i];
				else
					df[nd++] = mv0[i];
			}
		}
		if (marked(s, q))   // 마커 곁 = 이동 액션만 — 노마드 바꿔 쓰기로도 킥·태세 없음 (2026-11-04)
			nk = nd = 0;
		if (nm && !q.bound && do_mv) {   // 이동 · 드리블 — 가장 큰 범위로 한 번 훑고 카드마다 걸음 수로 거른다
			int maxr = 0;
			for (int i = 0; i < nm; i++) {
				int ty, v;
				card_info(s, team, mv[i], ty, v);
				maxr = std::max(maxr, rng_of(s, q, mv[i], C_MOVE, sk));
			}
			Reach r;
			reach(q.pos, maxr, blocked, UNSET, r);
			for (int i = 0; i < r.n; i++) {
				V2 t = r.order[i];
				if (carry) {
					if (player_at(s, t) || is_any_box(t))
						continue;
				} else if (ko || s.ball_owner == u || !move_dest_ok(s, q, t)) {
					continue;
				}
				for (int c = 0; c < nm; c++) {
					int ty, v;
					card_info(s, team, mv[c], ty, v);
					if (r.step[idx(t)] > rng_of(s, q, mv[c], C_MOVE, sk))
						continue;
					if (!carry && s.loose_ball() && is_any_box(t)) {
						std::vector<V2> pth = move_path(s, q, t, rng_of(s, q, mv[c], C_MOVE, sk), 1, blocked);
						if (std::find(pth.begin(), pth.end(), s.ball_pos) != pth.end())
							continue;   // 주운 공을 들고 박스에 멈출 수 없다
					}
					out.push_back({ carry ? A_DRIBBLE : A_MOVE, u, t, mv[c], sk });
				}
			}
		}
		if (carry && nk && do_kk) {   // 패스 · 슛
			int maxr = 0;
			for (int i = 0; i < nk; i++) {
				int ty, v;
				card_info(s, team, kk[i], ty, v);
				maxr = std::max(maxr, rng_of(s, q, kk[i], C_KICK, sk));
			}
			Reach g, air;
			pass_targets(s, q, maxr, g, air);
			for (int pass = 0; pass < 2; pass++) {
				const Reach &rr = pass == 0 ? g : air;
				for (int i = 0; i < rr.n; i++) {
					V2 t = rr.order[i];
					int dist = manhattan(q.pos, t);
					if (pass == 1 && (dist < LONG_PASS_MIN || g.has(t) || dist > g_air_max))
						continue;
					if (!pass_dest_ok(s, q, t))
						continue;
					for (int c = 0; c < nk; c++) {
						int ty, v;
						card_info(s, team, kk[c], ty, v);
						int rg = rng_of(s, q, kk[c], C_KICK, sk);
						if ((pass == 0 ? rr.step[idx(t)] : dist) > rg || (pass == 1 && rg < LONG_PASS_MIN))
							continue;
						out.push_back({ A_PASS, u, t, kk[c], sk });
					}
				}
			}
			if (!ko && s.ball_owner == u) {
				V2 goal = opponent_goal(team);
				Reach sr;
				reach(q.pos, maxr, blocked, goal, sr);
				if (sr.has(goal))
					for (int c = 0; c < nk; c++) {
						int ty, v;
						card_info(s, team, kk[c], ty, v);
						if (sr.step[idx(goal)] <= rng_of(s, q, kk[c], C_KICK, sk))
							out.push_back({ A_SHOT, u, goal, kk[c], sk });
					}
			}
		}
		if (nd && do_df && can_stance(s, q))
			for (int c = 0; c < nd; c++) {
				int ty, v;
				card_info(s, team, df[c], ty, v);
				if (eff_stat(s, q, C_DEF, v) >= 1)   // 마커 −1로 0이 되는 수비 카드는 못 낸다
					out.push_back({ A_STANCE, u, q.pos, df[c], sk });
			}
	};
	for (const Player &q : s.players) {
		if (q.team != team || !q.placed() || (s.stam_on && s.stam[q.uid] <= 0))   // 0칸 = 카드 못 냄 (체력 11-13)
			continue;
		gen(q, 0);
		if (skill_can(s, q))
			for (int k = 1; k <= (q.position == ALLROUND ? 2 : 1); k++)
				gen(q, k);
	}
	if (s.pts < 2)   // 2포인트 스킬은 포인트가 남아야 낸다
		out.erase(std::remove_if(out.begin(), out.end(), [&](const Action &a) {
			const int want = a.t == A_PASS || a.t == A_SHOT ? C_KICK : a.t == A_STANCE ? C_DEF : C_MOVE;
			return a.card >= 0 && a.t <= A_STANCE && sk_cost(card_sk(s, team, a.card, want)) == 2;
		}), out.end());
	// 「턴 종료」는 낼 수 있는 액션 카드가 하나도 없을 때만 (v8.9 사용자 "턴종료는 수동으로 못하게 · 반드시 액션포인트 2개 써야함")
	const bool any_card = !out.empty();
	if (!ko) {
		int opp = opponent(team);
		for (int k = 0; k < N_TAC; k++) {
			if (!((s.tac[team] >> k) & 1) || (k == T_DISCARD && discard_slot(s.hand[opp]) < 0) || (k == T_UNDO && !undo_ok(s, team)) ||
					(k == T_ADVANCE && s.adv_next[team]) || (k == T_DOUBLE && s.dbl[team]))
				continue;
			if (k == T_BIND) {
				for (int u = 0; u < PLAYERS; u++)
					if (bind_ok(s, team, u))
						out.push_back({ A_TACTIC, k, { u, 0 }, -1 });
			} else if (k == T_LOCK && s.lock[team]) {
				continue;
			} else {
				out.push_back({ A_TACTIC, k, { 0, 0 }, -1 });
			}
		}
		if (!any_card)
			out.push_back({ A_END, -1, { 0, 0 }, -1 });
	}
}

// ---------- 손평가 (첫 세대 가치 · 망 없는 화면 AI) ----------

double heuristic(const State &s, int team) {
	int opp = opponent(team);
	double h = (s.goals[team] - s.goals[opp]) * 1.5 + (s.lost[opp] - s.lost[team]) / 50.0;
	if (g_keeper_hp > 0 && s.keeper[0] > 0 && s.keeper[1] > 0)   // 깎아 둔 상대 골문 (v16 — 다음 슛이 들어가기 쉽다)
		h += 0.5 * double(s.keeper[team] - s.keeper[opp]) / g_keeper_hp;
	if (s.phase == PLAY) {
		int bt = s.ball_team();
		h += bt == team ? 0.4 : -0.4;
		V2 b = s.ball_owner != -1 ? s.players[s.ball_owner].pos : (s.loose_ball() ? s.ball_pos : V2{ W / 2, H / 2 });
		double prog = (bt == HOME ? b.x : W - 1 - b.x) / double(W - 1);   // 공 가진 팀 관점 전진 0~1
		h += (bt == team ? 1.0 : -1.0) * 0.6 * prog;
		h += 0.01 * (attack_value(s, team) - attack_value(s, opp));   // v16 콤보 + 역습 + 가담 (쌓인 데미지)
	}
	return h;
}

} // namespace card
} // namespace cf
