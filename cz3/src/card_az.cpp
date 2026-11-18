// 카드 모드 알파제로 (card_az.h). 탐색·망 읽기·전방 계산은 본편 az.cpp와 같은 코드다 — 고치면 양쪽을 같이 본다
#include "card_az.h"
#include "card_duel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cf {
namespace card {
namespace az {

static inline int flip_tile(V2 t, int team) { return t.y * W + (team == AWAY ? W - 1 - t.x : t.x); }

static inline void add(NetInput &in, int i) {
	if (in.n < MAX_ACTIVE)
		in.idx[in.n++] = uint16_t(i);
}

void net_input(const State &s, int team, NetInput &in) {
	in.n = 0;
	bool drafted[2][NPOS] = {};   // 팀 안에서만 겹치지 않으니 벤치도 팀마다 (v7.8)
	for (const Player &q : s.players) {
		int slot = pool_slot(q.position);
		if (slot >= 0)
			drafted[q.team == team ? 0 : 1][slot] = true;
		if (q.position < 0)
			continue;
		int sl = slot < 0 ? NPOS - 1 : slot;
		int rel = q.team == team ? 0 : 1;
		if (!q.placed()) {
			add(in, OFF_UNPLACED + rel * NPOS + sl);
			continue;
		}
		int tile = flip_tile(q.pos, team);
		add(in, OFF_PIECE + (rel * NPOS + sl) * NT + tile);
		if (q.stance > 0)
			add(in, OFF_STANCE + (rel * 4 + std::max(0, std::min(3, q.stance - 2))) * NT + tile);
		if (s.ball_owner == q.uid)
			add(in, OFF_BALL + rel * NT + tile);
		if (marked(s, q))
			add(in, OFF_MARKED + rel * NT + tile);
		if (boosted(s, q))
			add(in, OFF_BOOSTED + rel * NT + tile);
		if (q.bound)
			add(in, OFF_BOUND + rel * NT + tile);
		for (int k = 0; k < 4; k++)
			if (q.bonus[k] > 0)
				add(in, OFF_PSTAT + ((rel * 4 + k) * 3 + std::min(3, q.bonus[k]) - 1) * NT + tile);
		if (q.stance > 0 && (q.stance_sk == D_WIDE || q.stance_sk == D_FRONT || q.stance_sk == D_SIDE))
			add(in, OFF_ZONE_SK + (rel * 3 + (q.stance_sk == D_WIDE ? 0 : q.stance_sk == D_FRONT ? 1 : 2)) * NT + tile);
	}
	if (s.loose_ball())
		add(in, OFF_LOOSE + (s.loose_team == team ? 0 : 1) * NT + flip_tile(s.ball_pos, team));
	if (s.phase == PLACEMENT && !s.redeploy)
		for (int r = 0; r < 2; r++)
			for (int k = 0; k < CARD_POOL_N; k++)
				if (!drafted[r][k])
					add(in, OFF_BENCH + r * POOL_N + k);
	const int opp = opponent(team);
	for (int r = 0; r < 2; r++) {   // 덱은 양 팀 다 보인다
		const int t = r == 0 ? team : opp;
		for (int c = 0; c < N_CARD; c++)
			if (s.sk_atk[t][c])
				add(in, OFF_SKILL + (r * 9 + c) * N_SKILL_NET + (s.sk_atk[t][c] < N_SKILL_NET ? s.sk_atk[t][c] : 0));
		for (int c = 0; c < 3; c++)
			if (s.sk_def[t][c])
				add(in, OFF_SKILL + (r * 9 + 6 + c) * N_SKILL_NET + (s.sk_def[t][c] < N_SKILL_NET ? s.sk_def[t][c] : 0));
	}
	// 판 2 (11-14 컵 규칙) — 판 1 칸을 다 넣은 **뒤에** 덧붙인다 (칸 수가 넘쳐 잘려도 옛 칸이 먼저 남게)
	if (s.stam_on || s.car_on) {
		for (const Player &q : s.players) {
			if (q.position < 0)
				continue;
			const int rel = q.team == team ? 0 : 1, u = q.uid;
			const int st = std::max(0, std::min(STAM_MAX, s.stam[u]));
			if (!q.placed()) {
				if (s.stam_on) {
					const int sl = pool_slot(q.position) < 0 ? NPOS - 1 : pool_slot(q.position);
					add(in, OFF_UNPL_STAM + (rel * NPOS + sl) * 4 + st);
				}
				continue;
			}
			const int tile = flip_tile(q.pos, team);
			if (s.stam_on) {
				add(in, OFF_STAM + (rel * 4 + st) * NT + tile);
				add(in, OFF_SCAP + (rel * 3 + std::max(1, std::min(STAM_MAX, s.stam_cap[u])) - 1) * NT + tile);
				if ((s.acted >> u) & 1)
					add(in, OFF_ACTED + rel * NT + tile);
			}
			if (skill_can(s, q))   // 직업 스킬을 켤 수 있음 (11-14)
				add(in, OFF_GAUGE + (rel * 4) * NT + tile);
			if (s.car_on && s.st_turn[u] > 0 && s.st_turn[u] >= s.tturn[q.team] - 1)
				add(in, OFF_WSTREAK + rel * NT + tile);
			if (q.range_up > 0)
				add(in, OFF_PERK + (rel * 3 + 0) * NT + tile);
			if (q.hold_up > 0)
				add(in, OFF_PERK + (rel * 3 + 1) * NT + tile);
			if (q.kick_range_up > 0)
				add(in, OFF_PERK + (rel * 3 + 2) * NT + tile);
		}
	} else {
		for (const Player &q : s.players)   // 컵 밖에서도 전성기 혜택은 set_perks 로 줄 수 있다
			if (q.placed() && (q.range_up > 0 || q.hold_up > 0 || q.kick_range_up > 0)) {
				const int rel = q.team == team ? 0 : 1, tile = flip_tile(q.pos, team);
				if (q.range_up > 0)
					add(in, OFF_PERK + (rel * 3 + 0) * NT + tile);
				if (q.hold_up > 0)
					add(in, OFF_PERK + (rel * 3 + 1) * NT + tile);
				if (q.kick_range_up > 0)
					add(in, OFF_PERK + (rel * 3 + 2) * NT + tile);
			}
	}
	for (const Player &q : s.players)   // 만능 (판 3, 11-17) — 조각 칸은 유망주 칸과 같다
		if (q.position == ALLROUND)
			add(in, q.placed() ? OFF_ALLROUND + (q.team == team ? 0 : 1) * NT + flip_tile(q.pos, team) : OFF_ALLROUND + 2 * NT + (q.team == team ? 0 : 1));
	for (int r = 0; r < 2; r++) {   // 체력 카드 (판 1 스킬 칸엔 「기본」으로 보인다)
		const int t = r == 0 ? team : opp;
		for (int c = 0; c < 9; c++) {
			const int sk = c < 6 ? s.sk_atk[t][c] : s.sk_def[t][c - 6];
			if (sk >= N_SKILL_NET && sk < N_SKILL)
				add(in, OFF_SKILL_ST + (r * 9 + c) * (N_SKILL - N_SKILL_NET) + (sk - N_SKILL_NET));
		}
	}
	float *d = in.dense;
	d[0] = s.phase == PLACEMENT;
	d[1] = s.phase == PLAY;
	d[2] = float(eff_hold(s)) / 5.0f;   // 공 임자가 태클을 버티는 힘 (v9.0 — 옛 「후반인가」 자리)
	d[3] = float(std::max(0, std::min(s.half_turn(), g_half_turns))) / float(g_half_turns);   // 이 반의 진행 비율 (v10 — 반은 골이 나면 일찍 끝난다)
	d[4] = float(s.goals[team]) / 2.0f;   // v10 — HP 대신 골
	d[5] = float(s.goals[opp]) / 2.0f;
	d[6] = float(s.lost[opp] - s.lost[team]) / 120.0f;   // 골 숫자 합 차 (골이 같으면 이걸로 갈린다 — v17 슛 힘 × 괄호 합이라 나누는 수 40 → 120)
	d[7] = s.turn_team == team;
	d[8] = float(s.pts) / float(POINTS_PER_TURN);
	d[9] = kickoff_turn(s);
	d[10] = float(s.placement_index) / float(PLAYERS);
	d[11] = s.touch_team == team ? float(__builtin_popcount(unsigned(s.touch_mask))) / 5.0f : 0.0f;   // 우리 공격 가담 수 (v9.0 — 옛 「재배치」 자리, 이제 재배치가 없다)
	d[12] = s.opener == team;
	d[13] = s.half == 2;   // v10 — 후반 (옛 자리는 d[9]와 같은 킥오프였다)
	if (!g_time_input)   // 시간 입력 끔 (11-18 — 100턴 자가학습 동안 · 25턴으로 돌아갈 때 켜고 짧게 이어 배움)
		d[3] = d[13] = 0.0f;
	for (int c = 0; c < 6; c++) {
		d[14 + c] = float((s.hand[team] >> c) & 1);
		d[20 + c] = float((s.hand[opp] >> c) & 1);
	}
	d[26] = s.role[team] == R_ATK;
	for (int k = 0; k < N_TAC; k++) {
		d[27 + k] = float((s.tac[team] >> k) & 1);
		d[27 + N_TAC + k] = float((s.tac[opp] >> k) & 1);
	}
	float *e = d + 27 + 2 * N_TAC;
	e[0] = std::min(1.0f, float(s.combo[team]) / 40.0f);   // v13 콤보가 카드 숫자만큼 쌓여 커짐 — 칸 수·자리는 그대로, 나누는 수만 10 → 40

	e[1] = float(s.combo_last[team]) / 4.0f;
	e[2] = std::min(1.0f, float(s.combo[opp]) / 40.0f);
	e[3] = s.phase == PLAY && s.ball_team() == team;
	e[4] = float(std::max(-3.0, std::min(3.0, heuristic(s, team))) / 3.0);
	e[5] = float(s.adv_next[team]);
	e[6] = float(s.dbl[team]);
	e[7] = float(s.dbl[opp]);
	e[8] = float(s.carry[team]);
	e[9] = float(s.lock[team]);
	e[10] = float(s.lock[opp]);
	for (int k = DENSE_BASE; k < DENSE; k++)
		d[k] = 0.0f;
	float *k = d + DENSE_BASE;   // 기본 손 모양 · 스킬 상태 (2026-11-07)
	k[0] = float(s.hand_kick[team]);
	k[1] = float(s.hand_move[team]);
	k[2] = float(s.hand_kick[opp]);
	k[3] = float(s.hand_move[opp]);
	k[4] = float(s.rb_kick[team]);
	k[5] = float(s.rb_move[team]);
	k[6] = float(s.poss_now[team]);
	k[7] = float(s.lost_now[team] || s.lost_pending[team]);
	k[8] = float(s.poss_now[opp]);
	k[9] = float(s.lost_now[opp] || s.lost_pending[opp]);
	// v16 (사용자 2026-11-10): 골문 체력 · 역습 — 예비 칸에 (망 형식 그대로)
	const float hp = float(std::max(1, g_keeper_hp));
	k[10] = g_keeper_hp > 0 ? float(s.keeper[team]) / hp : 0.0f;
	k[11] = g_keeper_hp > 0 ? float(s.keeper[opp]) / hp : 0.0f;
	k[12] = float(s.ctr_on[team]);
	k[13] = float(s.ctr_on[opp]);
	k[14] = float(counter_bonus(s, team)) / 5.0f;
	k[15] = float(counter_bonus(s, opp)) / 5.0f;
	k[16] = float(3 * attack_value(s, team)) / float(std::max(1, g_keeper_hp > 0 ? g_keeper_hp : 33));   // 쌓인 괄호 합 × 보통 슛 힘 3 / 골문 체력 (v17)
	// v17 (사용자 2026-11-11 「족보혜택 ㄱ」): 족보 — 예비 칸에 (망 형식 그대로). 걸린 혜택 · 완성한 족보 · 숫자열 끝 세 장 (v17b 11-12 혜택 짝이 바뀌어도 칸 뜻은 「범위·힘 혜택」 그대로)
	const int jm = jokbo_mask(s, team), jo = jokbo_mask(s, opp);
	k[17] = float(jk_range(jm));
	k[18] = float(jk_stat(jm));
	k[19] = float((jm & JK_STRAIGHT) != 0);
	k[20] = float((jm & JK_FOUR) != 0);
	k[21] = float((jm & JK_FULL) != 0);
	k[22] = float(jk_range(jo));
	k[23] = float(jk_stat(jo));
	for (int i = 0, q = s.role[team] == R_ATK ? s.jk_seq[team] : 0; i < 3; i++, q /= 10)
		k[24 + i] = float(q % 10) / 4.0f;
	// 판 2 컵 칸 (11-14): 스위치 · 팀 체력 합 · 0칸 선수 · 손에 남은 체력 카드 · 준비된 선수
	float *c = d + DENSE_V1;
	c[0] = float(s.stam_on != 0);
	c[1] = float(s.car_on != 0);
	for (int r = 0; r < 2; r++) {
		const int t = r == 0 ? team : opp;
		int sum = 0, zero = 0, ready = 0, stc = 0;
		for (const Player &q : s.players) {
			if (q.team != t || q.position < 0)
				continue;
			if (s.stam_on) {
				sum += s.stam[q.uid];
				zero += s.stam[q.uid] <= 0;
			}
			ready += skill_can(s, q);
		}
		for (int cc = 0; cc < N_CARD; cc++) {   // 손에 있는 칸만 — 수비 칸(0~2)은 공수에 따라 킥 ↔ 수비
			if (!((s.hand[t] >> cc) & 1))
				continue;
			const int sk = cc < 3 && s.role[t] == R_DEF ? s.sk_def[t][cc] : s.sk_atk[t][cc];
			stc += sk >= N_SKILL_NET && sk < N_SKILL;
		}
		c[2 + r] = float(sum) / float(STAM_MAX * PER_TEAM);
		c[4 + r] = float(zero) / float(PER_TEAM);
		c[6 + r] = float(stc) / 3.0f;
		c[8 + r] = float(ready) / float(PER_TEAM);
	}
}

static int card_class(const State &s, int team, int card) {   // 숫자 셋 = 0~2 (v8.6 프리롤 뺌)
	(void)s; (void)team;   // 숫자가 아니라 칸 — 3-3-3·2-2-2 에서도 카드마다 다른 수 (스킬이 칸에 붙는다)
	return card < 3 ? card : card - 3;
}

static int action_index0(const State &s, const Action &a);
int action_index(const State &s, const Action &a) {   // 스킬을 켠 수 = 같은 카드 수 번호 + OFF_SKILL_A (11-14)
	const int i = action_index0(s, a);
	if (!a.sk || i < 0)
		return i;
	if (a.sk == 2)   // 만능 범위 스킬 (11-17)
		return i < OFF_STANCE_A ? OFF_SKILL_B + i : -1;
	return i < OFF_PLACE ? OFF_SKILL_A + i : -1;
}

static int action_index0(const State &s, const Action &a) {
	const int team = s.turn_team;
	switch (a.t) {
		case A_MOVE:
		case A_DRIBBLE:
		case A_PASS: {
			int kind = a.t == A_MOVE ? 0 : a.t == A_DRIBBLE ? 1 : 2;
			int me = a.uid - PER_TEAM * team;
			if (me < 0 || me >= PER_TEAM || !in_bounds(a.dest))
				return -1;
			return OFF_TILE + ((kind * PER_TEAM + me) * NT + flip_tile(a.dest, team)) * NCLS + card_class(s, team, a.card);
		}
		case A_SHOT:
		case A_STANCE: {
			int me = a.uid - PER_TEAM * team;
			if (me < 0 || me >= PER_TEAM)
				return -1;
			return (a.t == A_SHOT ? OFF_SHOT : OFF_STANCE_A) + me * NCLS + card_class(s, team, a.card);
		}
		case A_PLACE:
			if (a.uid < 0 || a.uid > POOL_N || !in_bounds(a.dest))
				return -1;
			return OFF_PLACE + a.uid * NT + flip_tile(a.dest, team);
		case A_END:
			return OFF_END;
		case A_SAVE: {   // 키퍼 킥 (v15) — 플레이 중엔 비는 배치 칸을 빌린다: 첫 장 = (숫자×2 − 2) 짝수 칸(확정은 우리 골대 칸) · 둘째 장 = 홀수 칸
			const int v1 = gk_card_value(s, team, s.hand[team], a.card);
			const int cls = std::min(POOL_N - 1, std::max(0, s.save_dist > 0 ? v1 * 2 - 1 : v1 * 2 - 2));
			return in_bounds(a.dest) ? OFF_PLACE + cls * NT + flip_tile(a.dest, team) : -1;
		}
		case A_TACTIC:
			if (a.uid == T_BIND) {
				int tg = a.dest.x - PER_TEAM * opponent(team);
				return tg >= 0 && tg < PER_TEAM ? OFF_BIND + tg : -1;
			}
			static const int TIDX[N_TAC] = { 0, 1, -1, 2, 3, 4, 5, 6 };   // 로테이션·봉쇄·리플레이·닥공·강슛·숨 고르기·육탄방어 (압박은 대상 칸)
			return a.uid >= 0 && a.uid < N_TAC && TIDX[a.uid] >= 0 ? OFF_TACTIC + TIDX[a.uid] : -1;
	}
	return -1;
}

struct Eval {
	float v = 0.0f, aux = 0.0f;
	std::vector<float> logits;
};

bool Net::load(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	std::vector<unsigned char> buf;
	unsigned char tmp[65536];
	size_t k;
	while ((k = fread(tmp, 1, sizeof(tmp), f)) > 0)
		buf.insert(buf.end(), tmp, tmp + k);
	fclose(f);
	return load_mem(buf.data(), buf.size());
}

static float half_to_float(uint16_t h) {   // IEEE 754 반정밀도 → 단정밀도
	const uint32_t sign = uint32_t(h & 0x8000) << 16;
	uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
	if (exp == 0) {
		if (man == 0) {
			bits = sign;
		} else {   // 비정규 수
			exp = 127 - 15 + 1;
			while (!(man & 0x400)) {
				man <<= 1;
				exp--;
			}
			bits = sign | (exp << 23) | ((man & 0x3FF) << 13);
		}
	} else if (exp == 31) {
		bits = sign | 0x7F800000 | (man << 13);
	} else {
		bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
	}
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

bool Net::load_mem(const unsigned char *p, size_t n) {
	size_t at = 0;
	auto take = [&](void *dst, size_t bytes) {
		if (at + bytes > n)
			return false;
		std::memcpy(dst, p + at, bytes);
		at += bytes;
		return true;
	};
	int head[7];
	// 판 1 입력 망(SPARSE_V1 · DENSE_V1/DENSE_BASE)도 읽는다 — 덧붙인 칸 무게는 0 (11-14 판 2 입력)
	// 판 2 망(SPARSE_V2 · NA_V2 — 11-14 ~ 11-17)도 읽는다 — 만능 칸 무게 0, 만능 범위 스킬 수는 스킬 칸 값을 빌린다
	if (!take(head, sizeof(head)) || head[0] != MAGIC || (head[1] != 1 && head[1] != 2) || (head[2] != SPARSE && head[2] != SPARSE_V2 && head[2] != SPARSE_V1) ||
			(head[3] != DENSE && head[3] != DENSE_V1 && head[3] != DENSE_BASE) || (head[2] != SPARSE_V1) != (head[3] == DENSE) ||
			head[4] <= 0 || head[4] > 2048 || head[5] <= 0 || head[5] > 512 || (head[6] != NA && head[6] != NA_V2 && head[6] != NA_V1))
		return false;
	h1 = head[4];
	h2 = head[5];
	na = head[6];   // 옛 망(NA_V1)은 스킬 수 칸이 없다 — evaluate 가 같은 카드 수 값을 빌린다
	const int file_dense = head[3];   // 예비 칸 없는 옛 망(54)도 읽는다 — 예비 칸 무게는 0
	w1.assign(size_t(SPARSE + DENSE) * h1, 0.0f);
	b1.resize(h1);
	w2.resize(size_t(h1) * h2);
	b2.resize(h2);
	wv.resize(h2);
	wa.resize(h2);
	wp.resize(size_t(h2) * na);
	bp.resize(na);
	// 판 2 = 무게를 16비트 반정밀도로 (앱 용량 절반 — tools/cz/net_fp16.py, 2026-10-30 사용자 "용량 줄이고 최적화"). 읽으면서 32비트로 푼다
	const bool half = head[1] == 2;
	auto takef = [&](float *dst, size_t cnt) {
		if (!half)
			return take(dst, 4 * cnt);
		if (at + 2 * cnt > n)
			return false;
		for (size_t i = 0; i < cnt; i++, at += 2)
			dst[i] = half_to_float(uint16_t(p[at] | (p[at + 1] << 8)));
		return true;
	};
	const int file_sparse = head[2];
	bool ok = takef(w1.data(), size_t(file_sparse) * h1) && takef(&w1[size_t(SPARSE) * h1], size_t(file_dense) * h1) &&
			takef(b1.data(), b1.size()) && takef(w2.data(), w2.size()) &&
			takef(b2.data(), b2.size()) && takef(wv.data(), wv.size()) && takef(&bv, 1) &&
			takef(wa.data(), wa.size()) && takef(&ba, 1) && takef(wp.data(), wp.size()) &&
			takef(bp.data(), bp.size());
	if (!ok) {
		h1 = h2 = 0;
		return false;
	}
	// 정책 머리를 **수마다 한 줄**(NA×h2)로 뒤집어 둔다 — 파일은 h2×NA라 수 하나를 셀 때 NA칸씩 건너뛰며 읽었다 (캐시 낭비)
	std::vector<float> t(wp.size());
	for (int k = 0; k < h2; k++)
		for (int a = 0; a < na; a++)
			t[size_t(a) * h2 + k] = wp[size_t(k) * na + a];
	wp.swap(t);
	return true;
}

static void forward(const Net &net, const NetInput &in, std::vector<float> &b) {
	static thread_local std::vector<float> a;
	a.assign(net.b1.begin(), net.b1.end());
	const int h1 = net.h1;
	for (int i = 0; i < in.n; i++) {
		const float *row = &net.w1[size_t(in.idx[i]) * h1];
		for (int j = 0; j < h1; j++)
			a[j] += row[j];
	}
	for (int k = 0; k < DENSE; k++) {
		float v = in.dense[k];
		if (v == 0.0f)
			continue;
		const float *row = &net.w1[size_t(SPARSE + k) * h1];
		for (int j = 0; j < h1; j++)
			a[j] += v * row[j];
	}
	for (int j = 0; j < h1; j++)
		a[j] = std::min(std::max(a[j], 0.0f), 1.0f);
	b.assign(net.b2.begin(), net.b2.end());
	const int h2 = net.h2;
	for (int j = 0; j < h1; j++) {
		float v = a[j];
		if (v == 0.0f)
			continue;
		const float *row = &net.w2[size_t(j) * h2];
		for (int k = 0; k < h2; k++)
			b[k] += v * row[k];
	}
	for (int k = 0; k < h2; k++)
		b[k] = std::min(std::max(b[k], 0.0f), 1.0f);
}


static void evaluate(const Net &net, const State &s, const std::vector<Action> &acts, Eval &out) {
	out.logits.assign(acts.size(), 0.0f);
	if (!net.loaded()) {
		out.v = float(std::tanh(heuristic(s, s.turn_team) / 2.0));
		return;
	}
	NetInput in;
	net_input(s, s.turn_team, in);
	static thread_local std::vector<float> b;
	forward(net, in, b);
	const int h2 = net.h2;
	float v = net.bv, ax = net.ba;
	for (int k = 0; k < h2; k++) {
		v += b[k] * net.wv[k];
		ax += b[k] * net.wa[k];
	}
	out.v = std::tanh(v);
	out.aux = std::tanh(ax);
	for (size_t i = 0; i < acts.size(); i++) {
		int ai = action_index(s, acts[i]);
		if (ai >= net.na && ai >= OFF_SKILL_B)
			ai += OFF_SKILL_A - OFF_SKILL_B;   // 판 2 망 — 만능 범위 스킬 수는 같은 카드의 스킬 수 값
		if (ai >= net.na)
			ai -= OFF_SKILL_A;   // 옛 망 — 스킬 수는 같은 카드 수 값
		if (ai < 0)
			continue;
		float l = net.bp[ai];
		const float *row = &net.wp[size_t(ai) * h2];
		for (int k = 0; k < h2; k++)
			l += b[k] * row[k];
		out.logits[i] = l;
	}
}

double g_aux_w = 0.0;
void aux_from_env() {
	if (const char *e = getenv("CF_AUX_W"))
		g_aux_w = atof(e);
}

static inline float with_aux(float v, double aux, double w) {   // w = 0 이면 v 그대로 (비트까지 같게 — 더하지 않는다)
	if (w == 0.0)
		return v;
	return float(std::max(-1.0, std::min(1.0, double(v) + w * std::max(-1.0, std::min(1.0, aux)))));
}

// ---------- 팀 성격 덤 (11-12) ----------
// 크기는 logit 단위 — 망 정책의 수 사이 logit 차가 보통 몇 단위라 덤 1 안팎은 「비슷한 수 중에서 고르는 버릇」 정도다 (값과 측정: ui/card_match.gd STYLE_W 설명)
float style_bias(const State &s, const Action &a, const Style &st) {
	if (s.phase != PLAY || a.uid < 0 || a.uid >= PLAYERS)
		return 0.0f;
	const int team = s.turn_team;
	const int fwd = forward_dir(team);
	const Player &p = s.players[a.uid];
	if (p.team != team || p.pos == UNPLACED)
		return 0.0f;
	const V2 ball = s.ball_owner >= 0 ? s.players[s.ball_owner].pos : s.ball_pos;
	auto cheb = [](V2 x, V2 y) { return std::max(std::abs(x.x - y.x), std::abs(x.y - y.y)); };
	const int gain = (a.dest.x - p.pos.x) * fwd;   // 앞으로 간 칸
	float b = 0.0f;
	if (st.jk != 0.0f && s.ball_team() == team && a.card >= 0 && a.card < N_CARD && (a.t == A_MOVE || a.t == A_DRIBBLE || a.t == A_PASS || a.t == A_SHOT)) {
		// 족보 의지: 숫자열(card.cpp jk_suffix 와 같은 꼴 — 읽기만) 끝에 이 카드 숫자를 붙여 본다. 노마드 바꿔 쓰기 숫자는 무시(근사)
		int type = -1, v = 0;
		card_info(s, team, a.card, type, v);
		if (v > 0) {
			const int seq = s.jk_seq[team], nseq = (seq * 10 + std::min(9, v)) % 10000000;
			int m = 0;
			if (nseq % 10000 == 1234)
				m |= JK_STRAIGHT;
			if (nseq % 10000 == (nseq % 10) * 1111)
				m |= JK_FOUR;
			if (nseq % 10000000 == 1223344)
				m |= JK_FULL;
			if (m & ~s.jk_done[team])
				b += st.jk * 1.5f;   // 새 족보 완성
			else {
				int run = 0;   // 같은 숫자 이어 가기 (포카드) · 1·2·3 오름 이어 가기 (스트레이트)
				for (int x = nseq; x > 0 && x % 10 == v && run < 3; x /= 10)
					run++;
				const bool up = v >= 2 && seq % 10 == v - 1;
				b += st.jk * 0.3f * float(std::max(run - 1, up ? v - 1 : 0));
			}
		}
	}
	if (s.ball_team() == team) {   // 공격
		if (a.t == A_PASS) {
			const int d = cheb(p.pos, a.dest);
			b += st.poss * (d <= 2 ? 1.0f : 0.0f);                             // 지공: 짧은 패스 (긴 킥을 깎으면 슛 길이 막혀 약해짐 — 측정 11-12)
			b += st.ctr * (d >= 3 ? 1.0f : -0.5f) + st.ctr * 0.25f * float(std::max(0, gain));   // 역습: 긴 킥·앞으로
		} else if (a.t == A_DRIBBLE) {
			b += st.ctr * 0.4f * float(std::max(-1, std::min(3, gain)));         // 역습: 빠른 전진
		} else if (a.t == A_MOVE && a.uid != s.ball_owner) {
			b += st.poss * 0.3f * float(std::max(0, std::min(2, gain)));         // 지공: 가담 (공 없는 동료가 올라감)
		}
	} else if (ball != UNPLACED) {   // 수비
		if (a.t == A_MOVE) {
			const int closer = cheb(p.pos, ball) - cheb(a.dest, ball);
			b += st.press * 0.5f * float(std::max(-2, std::min(3, closer)));     // 전방압박: 공 쪽으로
			const bool own_now = is_own_half(team, p.pos), own_after = is_own_half(team, a.dest);
			b += st.hold * (own_after ? (own_now ? 0.0f : 1.0f) : -1.0f);       // 지키는 수비: 자기 진영으로 · 넘어가지 않기
		} else if (a.t == A_STANCE) {
			b += st.hold * (is_own_half(team, p.pos) ? 0.5f : 0.0f);           // 제자리에서 태세
		}
	}
	return b;
}

// ---------- 탐색 ----------
namespace {

struct Rng {
	unsigned long long s;
	explicit Rng(unsigned long long seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
	unsigned long long next() {
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 0x2545F4914F6CDD1Dull;
	}
	double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
	double gumbel() { return -std::log(-std::log(std::max(unit(), 1e-12))); }
	double normal() {   // Box–Muller
		double u1 = std::max(unit(), 1e-12), u2 = unit();
		return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
	}
	double gamma(double alpha) {   // Marsaglia–Tsang (α<1은 α+1로 뽑아 U^(1/α) 보정)
		if (alpha < 1.0)
			return gamma(alpha + 1.0) * std::pow(std::max(unit(), 1e-12), 1.0 / alpha);
		double d = alpha - 1.0 / 3.0, c = 1.0 / std::sqrt(9.0 * d);
		for (;;) {
			double x = normal(), v = 1.0 + c * x;
			if (v <= 0.0)
				continue;
			v = v * v * v;
			double u = unit();
			if (u < 1.0 - 0.0331 * x * x * x * x || std::log(std::max(u, 1e-12)) < 0.5 * x * x + d * (1.0 - v + std::log(v)))
				return d * v;
		}
	}
};

struct Node {
	State s;
	int mover = -1;            // 이 마디에서 두는 팀 (끝난 판이면 부모의 팀)
	bool terminal = false;
	float v = 0.0f;            // 망 가치 (mover 관점) · 끝난 판이면 결과
	std::vector<Action> acts;
	std::vector<float> logits;
	std::vector<int> child;    // 마디 번호 (−1 = 아직)
	std::vector<int> n;        // 자식 방문 수
	std::vector<double> w;     // 자식 가치 합 (이 마디 mover 관점)
	int N = 0;                 // 자식 방문 합
};

struct Tree {
	const Net &net;
	const Params &p;
	std::vector<Node> nodes;
	explicit Tree(const Net &n_, const Params &p_) : net(n_), p(p_) {}

	// 합법 수가 없는 국면은 턴을 넘긴 국면으로 바꾼다 (하네스·UI가 end_turn을 부르는 것과 같다)
	int make(State s, int parent_mover) {
		Node nd;
		for (int guard = 0; guard < 8 && s.phase != GAME_OVER; guard++) {
			enumerate_actions(s, nd.acts);
			if (!nd.acts.empty())
				break;
			end_turn(s);
		}
		nd.s = s;
		if (s.phase == GAME_OVER || nd.acts.empty()) {
			nd.terminal = true;
			nd.mover = parent_mover < 0 ? s.turn_team : parent_mover;
			nd.v = s.winner == -1 ? 0.0f : (s.winner == nd.mover ? 1.0f : -1.0f);   // 비김 = 0 (0:0 특수 처리 없앰, 사용자 11-18)
			nd.v = with_aux(nd.v, double(s.lost[opponent(nd.mover)] - s.lost[nd.mover]) / 100.0, p.aux_w);   // 끝: 실제 골 피해 차 (학습 aux 목표와 같은 식 · mover 관점)
		} else {
			nd.mover = s.turn_team;
			Eval e;
			evaluate(net, s, nd.acts, e);
			nd.v = with_aux(e.v, e.aux, p.aux_w);   // 망 v·aux 둘 다 s.turn_team(= mover) 관점
			nd.logits = std::move(e.logits);
			if (nd.mover == p.style_team && p.style.on())
				for (size_t i = 0; i < nd.acts.size(); i++)
					nd.logits[i] += style_bias(s, nd.acts[i], p.style);
			nd.child.assign(nd.acts.size(), -1);
			nd.n.assign(nd.acts.size(), 0);
			nd.w.assign(nd.acts.size(), 0.0);
		}
		nodes.push_back(std::move(nd));
		return int(nodes.size()) - 1;
	}

	// σ(q) = (c_visit + 최대 방문 수)·c_scale·q — 앞의 배율은 마디마다 한 번만 센다 (수마다 다시 세면 k²)
	double sigma_scale(const Node &nd) const {
		int maxn = 0;
		for (int x : nd.n)
			maxn = std::max(maxn, x);
		return (p.c_visit + maxn) * p.c_scale;
	}

	// 완성된 Q (Gumbel 논문 식): 방문한 수는 평균값, 안 한 수는 v_mix
	void completed_q(const Node &nd, std::vector<double> &q) const {
		size_t k = nd.acts.size();
		std::vector<double> pi(k);
		double mx = -1e30;
		for (size_t i = 0; i < k; i++)
			mx = std::max(mx, double(nd.logits[i]));
		double sum = 0.0;
		for (size_t i = 0; i < k; i++) {
			pi[i] = std::exp(double(nd.logits[i]) - mx);
			sum += pi[i];
		}
		double vis_pi = 0.0, vis_piq = 0.0;
		for (size_t i = 0; i < k; i++) {
			pi[i] /= sum;
			if (nd.n[i] > 0) {
				vis_pi += pi[i];
				vis_piq += pi[i] * (nd.w[i] / nd.n[i]);
			}
		}
		double vmix = nd.v;
		if (nd.N > 0 && vis_pi > 0.0)
			vmix = (nd.v + nd.N * (vis_piq / vis_pi)) / (1.0 + nd.N);
		q.resize(k);
		for (size_t i = 0; i < k; i++)
			q[i] = nd.n[i] > 0 ? nd.w[i] / nd.n[i] : vmix;
	}

	// 개선된 정책 π' = softmax(logits + σ(완성된 Q))
	void improved_policy(const Node &nd, std::vector<double> &pi) const {
		std::vector<double> q;
		completed_q(nd, q);
		size_t k = nd.acts.size();
		pi.resize(k);
		double mx = -1e30;
		const double sc = sigma_scale(nd);
		for (size_t i = 0; i < k; i++) {
			pi[i] = double(nd.logits[i]) + sc * q[i];
			mx = std::max(mx, pi[i]);
		}
		double sum = 0.0;
		for (size_t i = 0; i < k; i++) {
			pi[i] = std::exp(pi[i] - mx);
			sum += pi[i];
		}
		for (size_t i = 0; i < k; i++)
			pi[i] /= sum;
	}

	// 뿌리 아래: 결정론 선택 argmax(π'(a) − n(a)/(1+N))
	int pick_child(const Node &nd) const {
		std::vector<double> pi;
		improved_policy(nd, pi);
		int best = 0;
		double bs = -1e30;
		for (size_t i = 0; i < nd.acts.size(); i++) {
			double sc = pi[i] - double(nd.n[i]) / (1.0 + nd.N);
			if (sc > bs) {
				bs = sc;
				best = int(i);
			}
		}
		return best;
	}

	// 뿌리의 수 a를 한 번 더 파고 내려간다. 돌려주는 값 = 뿌리 mover 관점
	double leaf_turns = 0.0;   // 잎까지 차례 경계 합 (측정용)
	int leaves = 0;
	double simulate(int root, int a) {
		std::vector<std::pair<int, int>> path;   // (마디, 자식 번호)
		int cur = root;
		int ai = a;
		double leaf = 0.0;
		int leaf_mover = -1;
		for (;;) {
			Node &nd = nodes[cur];
			path.push_back({ cur, ai });
			if (nd.child[ai] == -1) {
				State c = nd.s;
				apply_action(c, nd.acts[ai]);
				int ci = make(c, nd.mover);
				nodes[cur].child[ai] = ci;
				leaf = nodes[ci].v;
				leaf_mover = nodes[ci].mover;
				break;
			}
			cur = nd.child[ai];
			Node &cn = nodes[cur];
			if (cn.terminal) {
				leaf = cn.v;
				leaf_mover = cn.mover;
				break;
			}
			ai = pick_child(cn);
		}
		int tb = 0;
		for (size_t i = 1; i < path.size(); i++)
			tb += nodes[path[i].first].mover != nodes[path[i - 1].first].mover;
		leaf_turns += tb + (leaf_mover != nodes[path.back().first].mover);
		leaves++;
		for (auto it = path.rbegin(); it != path.rend(); ++it) {
			Node &nd = nodes[it->first];
			double val = nd.mover == leaf_mover ? leaf : -leaf;
			nd.n[it->second] += 1;
			nd.w[it->second] += val;
			nd.N += 1;
		}
		Node &r = nodes[root];
		return r.mover == leaf_mover ? leaf : -leaf;
	}
};

struct ReuseImpl {
	std::vector<Node> nodes;   // 뿌리 = 0
};

static bool same_state(const State &a, const State &b) {
	return a.turn_count == b.turn_count && a.phase == b.phase && a.turn_team == b.turn_team && a.ball_owner == b.ball_owner &&
			std::memcmp(&a, &b, sizeof(State)) == 0;
}

// 지난 나무에서 s0 와 같은 마디를 찾아 그 가지만 out 으로 옮긴다 (뿌리 = 0). 못 찾거나 상한을 넘으면 false
static bool take_subtree(ReuseImpl &im, const State &s0, size_t cap, std::vector<Node> &out) {
	if (im.nodes.empty())
		return false;
	int found = -1;
	std::vector<int> cur { 0 }, nxt;
	for (int d = 0; d <= 8 && found < 0 && !cur.empty(); d++) {
		nxt.clear();
		for (int i : cur) {
			const Node &nd = im.nodes[size_t(i)];
			if (!nd.terminal && same_state(nd.s, s0)) {
				found = i;
				break;
			}
			for (int c : nd.child)
				if (c >= 0)
					nxt.push_back(c);
		}
		cur.swap(nxt);
	}
	if (found < 0)
		return false;
	std::vector<int> order { found }, map(im.nodes.size(), -1);
	map[size_t(found)] = 0;
	for (size_t h = 0; h < order.size(); h++) {
		if (order.size() > cap)
			return false;
		for (int c : im.nodes[size_t(order[h])].child)
			if (c >= 0 && map[size_t(c)] < 0) {
				map[size_t(c)] = int(order.size());
				order.push_back(c);
			}
	}
	out.clear();
	out.reserve(order.size());
	for (int i : order) {
		out.push_back(std::move(im.nodes[size_t(i)]));
		for (int &c : out.back().child)
			if (c >= 0)
				c = map[size_t(c)];
	}
	return true;
}

} // namespace

Reuse::~Reuse() { delete static_cast<ReuseImpl *>(impl); }
void Reuse::clear() {
	if (impl)
		static_cast<ReuseImpl *>(impl)->nodes.clear();
	net = nullptr;
}

Result search(const State &s0, const Net &net, const Params &p) { return search(s0, net, p, nullptr); }

Result search(const State &s0, const Net &net, const Params &p, Reuse *keep) {
	Result res;
	Tree tree(net, p);
	int root = -1;
	if (keep) {
		if (!keep->impl)
			keep->impl = new ReuseImpl;
		ReuseImpl &im = *static_cast<ReuseImpl *>(keep->impl);
		const size_t cap = size_t(keep->cap > 0 ? keep->cap : std::max(4096, 8 * p.sims));
		if (keep->net == &net && keep->aux_w == p.aux_w && keep->style_team == p.style_team && take_subtree(im, s0, cap, tree.nodes)) {
			root = 0;
			keep->hit++;
			keep->kept += tree.nodes[0].N;
		} else {
			keep->miss++;
		}
		im.nodes.clear();
	}
	tree.nodes.reserve(tree.nodes.size() + size_t(p.sims) * 2 + 8);
	if (root < 0)
		root = tree.make(s0, -1);
	Node &r0 = tree.nodes[root];
	res.acts = r0.acts;
	if (r0.terminal || r0.acts.empty()) {
		res.root_v = r0.v;
		return res;
	}
	const size_t k = r0.acts.size();
	Rng rng(p.seed);
	std::vector<double> g(k, 0.0);
	if (p.gumbel)
		for (size_t i = 0; i < k; i++)
			g[i] = rng.gumbel();
	if (p.dirichlet_eps > 0.0 && k > 1) {   // 디리클레 뿌리 잡음 — 후보 고르기 점수(g)에만 섞는다 (학습 목표 π'는 r.logits 그대로)
		std::vector<double> pr(k), dir(k);
		double mx = -1e30, sum = 0.0, dsum = 0.0;
		for (size_t i = 0; i < k; i++)
			mx = std::max(mx, double(r0.logits[i]));
		for (size_t i = 0; i < k; i++) {
			pr[i] = std::exp(double(r0.logits[i]) - mx);
			sum += pr[i];
			dir[i] = rng.gamma(p.dirichlet_alpha);
			dsum += dir[i];
		}
		for (size_t i = 0; i < k; i++) {
			pr[i] /= sum;
			dir[i] /= std::max(dsum, 1e-12);
			double mixed = (1.0 - p.dirichlet_eps) * pr[i] + p.dirichlet_eps * dir[i];
			g[i] += std::log(std::max(mixed, 1e-12)) - std::log(std::max(pr[i], 1e-12));
		}
	}
	// 뿌리 후보: logit + g 상위 m개 → 순차 반감
	std::vector<int> cand(k);
	for (size_t i = 0; i < k; i++)
		cand[i] = int(i);
	std::sort(cand.begin(), cand.end(), [&](int a, int b) { return g[a] + r0.logits[a] > g[b] + r0.logits[b]; });
	int m = std::min<int>(p.top_m, int(k));
	cand.resize(m);
	if (m > 1) {
		int phases = 0;
		for (int x = m; x > 1; x = (x + 1) / 2)
			phases++;
		int used = 0;
		while (cand.size() > 1 && used < p.sims) {
			int per = std::max(1, (p.sims - used) / (phases * int(cand.size())));
			for (int rep = 0; rep < per && used < p.sims; rep++)
				for (int a : cand) {
					if (used >= p.sims)
						break;
					tree.simulate(root, a);
					used++;
				}
			phases = std::max(1, phases - 1);
			Node &r = tree.nodes[root];
			const double sc = tree.sigma_scale(r);
			auto score = [&](int a) {
				double q = r.n[a] > 0 ? r.w[a] / r.n[a] : 0.0;
				return g[a] + r.logits[a] + sc * q;
			};
			std::sort(cand.begin(), cand.end(), [&](int a, int b) { return score(a) > score(b); });
			if (cand.size() > 1)
				cand.resize((cand.size() + 1) / 2);
		}
		// 남은 예산은 1등을 더 다진다 (뿌리 가치 추정 정밀도)
		while (used < p.sims) {
			tree.simulate(root, cand[0]);
			used++;
		}
	} else {
		for (int i = 0; i < std::max(1, p.sims / 4); i++)
			tree.simulate(root, cand[0]);
	}
	Node &r = tree.nodes[root];
	res.found = true;
	res.action = r.acts[cand[0]];
	res.leaf_turns = float(tree.leaf_turns / std::max(1, tree.leaves));
	for (int ci = r.child[cand[0]], prev = r.mover; ci >= 0;) {   // 주된 갈래: 뽑은 수 → 그다음은 가장 많이 간 자식
		const Node &nd = tree.nodes[ci];
		res.pv_plies++;
		res.pv_turns += nd.mover != prev;
		prev = nd.mover;
		if (nd.terminal || nd.N == 0)
			break;
		ci = nd.child[size_t(std::max_element(nd.n.begin(), nd.n.end()) - nd.n.begin())];
	}
	std::vector<double> pi;
	tree.improved_policy(r, pi);
	if (p.sample_top > 0) {   // 쉬움: 개선 정책 상위 k개 중에서 확률대로 뽑는다 (스탯 치트 없이 사람처럼 틀린다)
		std::vector<int> ord(k);
		for (size_t i = 0; i < k; i++)
			ord[i] = int(i);
		std::sort(ord.begin(), ord.end(), [&](int a, int b) { return pi[a] > pi[b]; });
		int kk = std::min<int>(p.sample_top, int(k));
		double sum = 0.0;
		for (int i = 0; i < kk; i++)
			sum += pi[ord[i]];
		double u = rng.unit() * sum;
		int pickd = ord[0];
		for (int i = 0; i < kk; i++) {
			u -= pi[ord[i]];
			if (u <= 0.0) {
				pickd = ord[i];
				break;
			}
		}
		res.action = r.acts[pickd];
	} else if (p.temp > 0.0) {   // 온도 표본: π'^(1/T) — T 가 작을수록 최선 수에 가깝다
		std::vector<double> wt(k);
		double mx = -1e300, sum = 0.0;
		for (size_t i = 0; i < k; i++) {
			wt[i] = std::log(std::max(pi[i], 1e-300)) / p.temp;
			mx = std::max(mx, wt[i]);
		}
		for (size_t i = 0; i < k; i++)
			sum += (wt[i] = std::exp(wt[i] - mx));
		double u = rng.unit() * sum;
		size_t pickd = k - 1;
		for (size_t i = 0; i < k; i++)
			if ((u -= wt[i]) <= 0.0) {
				pickd = i;
				break;
			}
		res.action = r.acts[pickd];
	}
		res.policy.assign(pi.begin(), pi.end());
	std::vector<double> q;
	tree.completed_q(r, q);
	double rv = 0.0;
	for (size_t i = 0; i < k; i++)
		rv += pi[i] * q[i];
	res.root_v = float(rv);
	if (keep) {
		static_cast<ReuseImpl *>(keep->impl)->nodes = std::move(tree.nodes);
		keep->net = &net;
		keep->aux_w = p.aux_w;
		keep->style_team = p.style_team;
	}
	return res;
}


void random_lineup(State &s, int team, unsigned long long r) {
	auto next = [&]() {
		r ^= r >> 12;
		r ^= r << 25;
		r ^= r >> 27;
		return r * 0x2545F4914F6CDD1Dull;
	};
	static const int POOL[CARD_POOL_N] = { CANNON, MAESTRO, TRICKSTER, SPRINTER, MARKER, BOOSTER, TITAN, HUNTER, WALL, NOMAD };
	int pos[PER_TEAM], bonus[PER_TEAM][4] = {};
	int used = 0;
	const int prospects = int(next() % (PER_TEAM + 1));   // 유망주 0~5명
	const int level = int(next() % 4);                    // 스탯 키운 정도 0~3
	const int mono = next() % 5 == 0 ? POOL[next() % CARD_POOL_N] : 0;   // 20 % 는 한 직업 팀 (컵 학교 10곳 — 사용자 11-08)
	for (int i = 0; i < PER_TEAM; i++) {
		if (i < prospects) {
			pos[i] = PROSPECT;
		} else {
			if (mono) {
				pos[i] = mono;
			} else {
				int k;
				do
					k = int(next() % CARD_POOL_N);
				while ((used >> k) & 1);
				used |= 1 << k;
				pos[i] = POOL[k];
			}
		}
		for (int b = 0; b < level; b++)
			bonus[i][next() % 4]++;
	}
	set_lineup(s, team, pos, bonus);
}

void random_deck(State &s, int team, unsigned long long r, double skill_p) {
	auto next = [&]() {
		r ^= r >> 12;
		r ^= r << 25;
		r ^= r >> 27;
		return r * 0x2545F4914F6CDD1Dull;
	};
	r = r * 6364136223846793005ull + 1442695040888963407ull + 1;
	(void)next();   // 옛 손 모양 뽑기 자리 — 난수 흐름은 그대로, 손은 늘 2-3-4·1-2-3 (11-17)
	(void)next();
	s.hand_kick[team] = s.hand_move[team] = 0;
	auto pick = [&](std::vector<int> &pool) {
		if (pool.empty() || double(next() >> 40) / double(1ull << 24) >= skill_p)
			return 0;
		const size_t i = size_t((next() >> 32) % pool.size());
		const int sk = pool[i];
		pool.erase(pool.begin() + long(i));
		return sk;
	};
	std::vector<int> kick, move, def;
	for (int k = K_SHOT_UP; k <= K_COUNTER; k++)   // 연결고리·콤비네이션·역습 시동은 뺐다 (사용자 2026-11-08 — 직업 콤보와 겹침) · 11-14 뺀 9장도
		if (k != K_LINK && !skill_removed(k))
			kick.push_back(k);
	kick.push_back(K_KEEP);
	for (int k = M_OFF; k <= M_COMBO; k++)
		if (k != M_COMBO && !skill_removed(k))
			move.push_back(k);
	move.push_back(M_KEEP);
	for (int k = D_WALL; k <= D_COMBO; k++)
		if (k != D_COMBO && !skill_removed(k))
			def.push_back(k);
	auto pick_fit = [&](std::vector<int> &pool, int value) {   // 그 숫자에 붙일 수 있는 것만 (skill_fits — 힘·범위 0 금지, 사용자 2026-11-09)
		std::vector<int> ok;
		for (int k : pool)
			if (skill_fits(k, value))
				ok.push_back(k);
		const int sk = pick(ok);
		if (sk)
			pool.erase(std::find(pool.begin(), pool.end(), sk));
		return sk;
	};
	for (int c = 0; c < 3; c++) {
		int ty, v;
		card_info(s, team, c, ty, v);
		s.sk_atk[team][c] = pick_fit(kick, v);
		s.sk_def[team][c] = pick_fit(def, v);
		card_info(s, team, 3 + c, ty, v);
		s.sk_atk[team][3 + c] = pick_fit(move, v);
	}
}

int random_tactics(unsigned long long r) {   // 8종 중 3종 (전술카드를 뺀 동안은 0 — card.h TACTICS_ON)
	if (!TACTICS_ON)
		return 0;
	int m = 0, n = 0;
	while (n < 3) {
		int k = int(r % N_TAC);
		r = r * 6364136223846793005ull + 1442695040888963407ull;
		if (!((m >> k) & 1)) {
			m |= 1 << k;
			n++;
		}
		r ^= r >> 29;
	}
	return m;
}

// 일기토 수 하나를 기록으로 (자가대전 — 망이 일기토 판단도 배우게). 정책 = duel::ai_act가 숨은 경우들을 더한 것
struct DuelRecCtx {
	std::vector<Rec> *recs;
	std::vector<int> *mover;
	uint32_t game;
};
static void duel_rec(void *ctx, const duel::Duel &d, int team, const std::vector<Action> &acts, const std::vector<float> &pol) {
	DuelRecCtx &c = *static_cast<DuelRecCtx *>(ctx);
	Rec rec;
	NetInput in;
	net_input(d.s, team, in);
	for (int i = 0; i < MAX_ACTIVE; i++)
		rec.idx[i] = i < in.n ? in.idx[i] : 0xFFFF;
	std::memcpy(rec.dense, in.dense, sizeof(rec.dense));
	std::vector<int> order(acts.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = int(i);
	std::sort(order.begin(), order.end(), [&](int a, int b) { return pol[a] > pol[b]; });
	float mass = 0.0f;
	int kk = 0;
	for (int i : order) {
		if (kk >= PI_K)
			break;
		int ai = action_index(d.s, acts[i]);
		if (ai < 0)
			continue;
		rec.pi_idx[kk] = uint16_t(ai);
		rec.pi_p[kk] = pol[i];
		mass += pol[i];
		kk++;
	}
	for (int i = 0; i < kk; i++)
		rec.pi_p[i] /= std::max(mass, 1e-6f);
	for (int i = kk; i < PI_K; i++) {
		rec.pi_idx[i] = 0xFFFF;
		rec.pi_p[i] = 0.0f;
	}
	rec.q = rec.z = rec.aux = 0.0f;
	rec.game = c.game;
	c.recs->push_back(rec);
	c.mover->push_back(team);
}

void selfplay(const Net &net, int games, int sims, unsigned long long seed, int temp_moves, double lam,
		std::vector<Rec> &out, GenStats &st, int fast_sims, double full_frac, double rand_draft_frac) {
	Params prm;
	prm.dirichlet_eps = 0.25;
	std::vector<Rec> recs;
	std::vector<int> mover;
	for (int g = 0; g < games; g++) {
		Rng rng(seed * 1000003ull + g);
		bool rand_draft = rand_draft_frac > 0.0 && rng.unit() < rand_draft_frac;
		State s;
		int th = random_tactics(rng.next()), ta = random_tactics(rng.next());
		new_match(s, AWAY, th, ta);   // 홈이 공을 먼저 (v9.0)
		recs.clear();
		mover.clear();
		int guard = 0, ply = 0;
		while (s.phase != GAME_OVER && guard++ < 8000) {
			prm.seed = rng.next();
			bool full = fast_sims <= 0 || rng.unit() < full_frac;
			prm.sims = full ? sims : fast_sims;
			Result r = search(s, net, prm);
			if (!r.found) {
				end_turn(s);
				continue;
			}
			Rec rec;
			NetInput in;
			net_input(s, s.turn_team, in);
			for (int i = 0; i < MAX_ACTIVE; i++)
				rec.idx[i] = i < in.n ? in.idx[i] : 0xFFFF;
			std::memcpy(rec.dense, in.dense, sizeof(rec.dense));
			std::vector<int> order(r.acts.size());
			for (size_t i = 0; i < order.size(); i++)
				order[i] = int(i);
			std::sort(order.begin(), order.end(), [&](int a, int b) { return r.policy[a] > r.policy[b]; });
			float mass = 0.0f;
			int kk = 0;
			for (int i : order) {
				if (kk >= PI_K || !full)
					break;
				int ai = action_index(s, r.acts[i]);
				if (ai < 0)
					continue;
				rec.pi_idx[kk] = uint16_t(ai);
				rec.pi_p[kk] = r.policy[i];
				mass += r.policy[i];
				kk++;
			}
			for (int i = 0; i < kk; i++)
				rec.pi_p[i] /= std::max(mass, 1e-6f);
			for (int i = kk; i < PI_K; i++) {
				rec.pi_idx[i] = 0xFFFF;
				rec.pi_p[i] = 0.0f;
			}
			rec.q = r.root_v;
			rec.z = rec.aux = 0.0f;
			rec.game = uint32_t(seed * 100003ull + g);
			recs.push_back(rec);
			mover.push_back(s.turn_team);
			Action a = r.action;
			if (rand_draft && s.phase == PLACEMENT && !s.redeploy) {
				a = r.acts[size_t(rng.next() % r.acts.size())];
			} else if (ply < temp_moves) {
				double u = rng.unit();
				for (size_t i = 0; i < r.acts.size(); i++) {
					u -= r.policy[i];
					if (u <= 0.0) {
						a = r.acts[i];
						break;
					}
				}
			}
			apply_action(s, a);
			ply++;
		}
		const int win_match = s.phase == GAME_OVER ? s.winner : -1;   // v15: 골 → 골 숫자 합 → 그래도 같으면 −1
		int win = win_match;
		const size_t n_match = recs.size();
		if (s.phase == GAME_OVER && win == -1) {   // 숫자 합까지 같음(0:0 포함) → 일기토는 컵 녹아웃처럼 둬서 일기토 수를 배운다 — 경기 수의 목표는 리그 기준
			const Net *nets[2] = { &net, &net };
			DuelRecCtx ctx { &recs, &mover, uint32_t(seed * 100003ull + g) };
			win = duel::resolve(s, nets, std::max(8, fast_sims > 0 ? fast_sims : sims / 4), rng.next(), duel_rec, &ctx);
			st.duels++;
		}
		st.games++;
		if (win == -1)
			st.draws++;
		else if (win == HOME)
			st.home++;
		else
			st.away++;
		if (s.goals[0] == 0 && s.goals[1] == 0)
			st.kos++;   // v10: 0:0 판 (옛 HP 0 승)
		st.goals += s.goals[0] + s.goals[1];
		st.lost += s.lost[0] + s.lost[1];
		st.moves += (long long)recs.size();
		for (size_t i = 0; i < recs.size(); i++) {
			int me = mover[i];
			const int w = i < n_match ? win_match : win;   // 경기 수 = 경기 결과(숫자 합 판정승 = 승) · 일기토 수 = 일기토 결과
			double z = w == -1 ? 0.0 : (w == me ? 1.0 : -1.0);   // 비김 = 0 (11-18)
			recs[i].z = i < n_match ? float(lam * z + (1.0 - lam) * recs[i].q) : float(z);   // 일기토 수는 탐색 가치가 없다 — 결과만
			recs[i].aux = float(std::max(-1.0, std::min(1.0, (s.lost[1 - me] - s.lost[me]) / 100.0)));
		}
		out.insert(out.end(), recs.begin(), recs.end());
	}
}

// 길 고르기용 상태 가치 (card.h value_path — 사용자 11-18 「길은 간 뒤의 상태로」): team 에 좋을수록 큼 (−1 ~ 1). ctx = const Net *
float state_value(const State &s, int team, const void *ctx) {
	if (s.phase == GAME_OVER)
		return s.winner == -1 ? 0.0f : s.winner == team ? 1.0f : -1.0f;
	Eval e;
	evaluate(*static_cast<const Net *>(ctx), s, std::vector<Action>(), e);
	return s.turn_team == team ? e.v : -e.v;
}

} // namespace az
} // namespace card
} // namespace cf
