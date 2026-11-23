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
	const int opp = opponent(team);
	bool drafted[2][NPOS] = {};
	for (const Player &q : s.players) {
		int slot = pool_slot(q.position);
		if (slot >= 0)
			drafted[q.team == team ? 0 : 1][slot] = true;
		if (q.position < 0)
			continue;
		const int sl = slot < 0 ? NPOS - 1 : slot;
		const int rel = q.team == team ? 0 : 1;
		if (!q.placed()) {
			add(in, OFF_UNPLACED + rel * NPOS + sl);
			if (q.position == ALLROUND)
				add(in, OFF_ALLROUND + 2 * NT + rel);
			continue;
		}
		const int tile = flip_tile(q.pos, team);
		add(in, OFF_PIECE + (rel * NPOS + sl) * NT + tile);
		if (q.stance > 0)
			add(in, OFF_STANCE + (rel * 4 + std::max(0, std::min(3, q.stance - 1))) * NT + tile);
		if (q.jk_up)
			add(in, OFF_FWDMOVED + rel * NT + tile);   // 족보 +1 토큰 (11-21 — 옛 빈 칸 재사용)
		if (q.stance > 0 && q.stance_left >= 2)
			add(in, OFF_LASTPASS + rel * NT + tile);   // 태세가 한 차례 더 남음 (월 2차례 — 옛 빈 칸 재사용)
		if (s.ball_owner == q.uid)
			add(in, OFF_BALL + rel * NT + tile);
		if (marked(s, q))
			add(in, OFF_MARKED + rel * NT + tile);
		for (int k = 0; k < 4; k++)
			if (q.bonus[k] > 0)
				add(in, OFF_PSTAT + ((rel * 4 + k) * 3 + std::min(3, q.bonus[k]) - 1) * NT + tile);
		if (q.range_up > 0)
			add(in, OFF_PERK + (rel * 2 + 0) * NT + tile);
		if (q.kick_range_up > 0)
			add(in, OFF_PERK + (rel * 2 + 1) * NT + tile);
		if (q.hold_up > 0)
			add(in, OFF_BOOSTED + rel * NT + tile);   // 월 지속 성장 (11-22 셋째 — 옛 부스터 빈 칸)
		if (q.charge)
			add(in, OFF_LASTPASS + (2 + rel) * NT + tile);   // 「강하게」 모음 (11-22 셋째 — 옛 빈 칸 뒤 140칸)
		if (q.position == ALLROUND)
			add(in, OFF_ALLROUND + rel * NT + tile);
		// OFF_LASTPASS 앞 140 = 월 2차례 · 뒤 140 = 공격 태세(11-22) · OFF_FWDMOVED = 족보 토큰(족보 끔 — 늘 0)
	}
	if (s.phase == PLAY && s.turn_team >= 0) {   // 두는 팀: 이번 차례에 행동한 선수 칸 (11-22 한 선수 행동 하나 — 옛 「사람이 지나간 칸」 자리) · OFF_VIS_B(옛 공 지나간 칸)는 11-22 부터 늘 0
		for (const Player &q : s.players)
			if (q.placed() && q.team == s.turn_team && q.acted) {
				const int me = q.uid - s.turn_team * PER_TEAM;
				if (me >= 0 && me < TEAM_N)
					add(in, OFF_VIS_P + me * NT + flip_tile(q.pos, team));
			}
	}
	for (int r = 0; r < 2; r++) {   // 태세 존 칸 (11-21 방향 존) — 팀마다 겹친 칸은 한 번
		bool on[NT] = {};
		for (const Player &q : s.players) {
			if ((q.team == team ? 0 : 1) != r)
				continue;
			V2 z[4];
			const int nz = zone_tiles(q, z);
			for (int k = 0; k < nz; k++) {
				const int t = flip_tile(z[k], team);
				if (!on[t])
					on[t] = true, add(in, OFF_ZONE + r * NT + t);
			}
		}
	}
	if (s.loose_ball())
		add(in, OFF_LOOSE + (s.loose_team == team ? 0 : 1) * NT + flip_tile(s.ball_pos, team));
	if (s.phase == PLACEMENT && !s.redeploy)
		for (int r = 0; r < 2; r++)
			for (int k = 0; k < CARD_POOL_N; k++)
				if (!drafted[r][k])
					add(in, OFF_BENCH + r * POOL_N + k);
	for (int r = 0; r < 2; r++)   // 켜진 족보 (지금은 두 팀 같은 표)
		for (int k = 0; k < N_JK; k++)
			if (jk_enabled(s, r == 0 ? team : opp, k))
				add(in, OFF_JKON + r * N_JK + k);
	// OFF_BOARD 칸(옛 전술판, 11-21 없앰)은 늘 0 — 지금 망을 이어 배우려고 자리만 둔다
	float *d = in.dense;
	for (int k = 0; k < DENSE; k++)
		d[k] = 0.0f;
	d[0] = s.phase == PLACEMENT;
	d[1] = s.phase == PLAY;
	d[2] = float(eff_hold(s)) / 5.0f;
	d[3] = float(std::max(0, std::min(s.half_turn(), g_half_turns))) / float(g_half_turns);
	d[4] = float(s.goals[team]) / 2.0f;
	d[5] = float(s.goals[opp]) / 2.0f;
	// d[6] = 옛 골 데미지 차 (늘 0)
	d[17] = float(s.ghp[team]) / float(std::max(1, g_goal_hp));   // 골키퍼 체력 — 우리 골대 · 상대 골대 (11-23 다시 — 옛 골문 체력 칸 그대로)
	d[18] = float(s.ghp[opp]) / float(std::max(1, g_goal_hp));
	d[7] = s.turn_team == team;
	d[8] = float(s.pts) / float(POINTS_PER_TURN);
	d[9] = kickoff_turn(s);
	d[10] = float(s.placement_index) / float(2 * s.team_n);
	d[11] = float(assist_value(s, team)) / 4.0f;
	d[12] = s.opener == team;
	d[13] = s.half == 2;
	if (!g_time_input)
		d[3] = d[13] = 0.0f;
	d[14] = s.role[team] == R_ATK;
	d[15] = s.phase == PLAY && s.ball_team() == team;
	d[16] = float(std::max(-3.0, std::min(3.0, heuristic(s, team))) / 3.0);
	// d[19]·d[20] = 옛 역습 보너스 (11-20 없앰 — 칸 자리만 남김, 늘 0)
	// d[21]·d[22] = 옛 괄호 합 · d[25]·d[53] = 옛 역습 행동 수 · d[44]~d[49]·d[54]·d[55] = 옛 족보 진행·혜택 (11-21 없앰 — 늘 0, 이식 때 치우침으로 접음)
	// d[23]·d[24] = 옛 키퍼 킥 팀 (11-22 없앰 — 늘 0)
	for (int k = 0; k < N_JK; k++) {   // 이번 차례(남의 차례면 다음 차례 몫)에 이미 걸린 족보 — 같은 족보는 한 차례 한 번 (11-21)
		d[26 + k] = float((jokbo_mask(s, team) >> k) & 1);
		d[35 + k] = float((jokbo_mask(s, opp) >> k) & 1);
	}
	d[50] = float(s.team_n) / float(PER_TEAM);
	d[51] = float(s.sk_passive);
}

int action_index(const State &s, const Action &a) {
	const int team = s.turn_team;
	int i = -1;
	switch (a.t) {
		case A_MOVE:
		case A_DRIBBLE:
		case A_PASS: {
			const int kind = a.t == A_MOVE ? 0 : a.t == A_DRIBBLE ? 1 : 2;
			const int me = a.uid - PER_TEAM * team;
			if (me < 0 || me >= PER_TEAM || !in_bounds(a.dest))
				return -1;
			i = OFF_TILE + (kind * PER_TEAM + me) * NT + flip_tile(a.dest, team);
			break;
		}
		case A_RUN: {   // 뒷공간 패스 (11-21): (차는 · 받는) × 칸
			const int me = a.uid - PER_TEAM * team, mm = a.mate - PER_TEAM * team;
			if (me < 0 || me >= PER_TEAM || mm < 0 || mm >= PER_TEAM || !in_bounds(a.dest))
				return -1;
			i = OFF_RUN + (me * PER_TEAM + mm) * NT + flip_tile(a.dest, team);
			break;
		}
		case A_SHOT:
		case A_STANCE: {
			const int me = a.uid - PER_TEAM * team;
			if (me < 0 || me >= PER_TEAM)
				return -1;
			i = (a.t == A_SHOT ? OFF_SHOT : OFF_STANCE_A) + me;
			if (a.t == A_STANCE) {   // 월(제자리 사방) = 옛 태세 칸 · 그 밖 = 방향 태세 칸 (11-23 규칙 · 비교용 CF_ZONE4=1 이면 모두 제자리)
				int dir = stance_dir(s.players[a.uid], a.dest);
				if (dir < 0)
					return -1;
				if (dir != ZD_ALL) {
					if (team == AWAY && dir < 2)
						dir ^= 1;   // 원정은 x 를 뒤집어 본다 (flip_tile 과 같게)
					i = OFF_STANCE_DIR + me * 4 + dir;
				}
			}
			break;
		}
		case A_PLACE:
			if (a.uid < 0 || a.uid > POOL_N || !in_bounds(a.dest))
				return -1;
			return OFF_PLACE + a.uid * NT + flip_tile(a.dest, team);
		case A_SAVE:   // 공 없는 선수 「강하게」 모으기 — 옛 키퍼 킥 칸 OFF_SAVE + 자기 칸
			if (!in_bounds(a.dest))
				return -1;
			return OFF_SAVE + flip_tile(a.dest, team);
		default:
			return -1;
	}
	return a.sk ? -1 : i;   // 스킬 수는 11-19 부터 없다 (옛 스킬 칸은 뒷공간 패스가 씀)
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
	if (!take(head, sizeof(head)) || head[0] != MAGIC || head[1] < 1 || head[1] > 3 || head[2] != SPARSE || head[3] != DENSE ||
			head[4] <= 0 || head[4] > 2048 || head[5] <= 0 || head[5] > 512 || head[6] != NA)
		return false;
	h1 = head[4];
	h2 = head[5];
	na = head[6];
	w1.assign(size_t(SPARSE + DENSE) * h1, 0.0f);
	b1.resize(h1);
	w2.resize(size_t(h1) * h2);
	b2.resize(h2);
	wv.resize(h2);
	wa.resize(h2);
	wp.resize(size_t(h2) * na);
	bp.resize(na);
	// 판 2 = 무게를 16비트 반정밀도로 (앱 용량 절반 — tools/cz/net_fp16.py, 2026-10-30 사용자 "용량 줄이고 최적화"). 읽으면서 32비트로 푼다
	// 판 3 = 첫 층 w1 만 int8 (64칸 묶음마다 fp16 scale) + 나머지 판 2 와 같음 (게임 전용 ≈19.6MB — tools/net_q8/net_q8.py, 11-22)
	const bool half = head[1] >= 2;
	auto takef = [&](float *dst, size_t cnt) {
		if (!half)
			return take(dst, 4 * cnt);
		if (at + 2 * cnt > n)
			return false;
		for (size_t i = 0; i < cnt; i++, at += 2)
			dst[i] = half_to_float(uint16_t(p[at] | (p[at + 1] << 8)));
		return true;
	};
	auto take_q8 = [&](float *dst, size_t rows, int cols) {   // 판 3 w1: scale fp16 [rows × cols/64] 다음 값 int8 [rows × cols]
		constexpr int G = 64;
		if (cols % G != 0)
			return false;
		const size_t ng = rows * size_t(cols / G);
		if (at + 2 * ng + rows * size_t(cols) > n)
			return false;
		const unsigned char *sc = p + at, *q = p + at + 2 * ng;
		for (size_t g = 0; g < ng; g++) {
			const float k = half_to_float(uint16_t(sc[2 * g] | (sc[2 * g + 1] << 8)));
			for (int j = 0; j < G; j++)
				dst[g * G + j] = k * float(int8_t(q[g * G + j]));
		}
		at += 2 * ng + rows * size_t(cols);
		return true;
	};
	bool ok = (head[1] == 3 ? take_q8(w1.data(), size_t(SPARSE + DENSE), h1) : takef(w1.data(), size_t(SPARSE + DENSE) * h1)) &&
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
		const int ai = action_index(s, acts[i]);
		if (ai < 0 || ai >= net.na)
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

float (*g_tac_score)(const State &s, const Action &a, const Style &st) = nullptr;

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
	auto cheb = [](V2 x, V2 y) { return manhattan(x, y); };   // 거리 = 게임과 같은 맨해튼 (11-21 — 옛 체비쇼프는 대각 2칸 패스를 「짧은 패스」로 셌다)
	const int gain = (a.dest.x - p.pos.x) * fwd;   // 앞으로 간 칸
	float b = 0.0f;
	if (st.jk != 0.0f && s.ball_team() == team && (a.t == A_PASS || a.t == A_RUN) && gain >= JK_AHEAD)
		b += st.jk * 1.0f;   // 족보 의지 (11-21 족보 6개): 전진 패스가 되는 킥에 덤
	if (s.ball_team() == team) {   // 공격
		if (a.t == A_PASS || a.t == A_RUN) {
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
			nd.v = with_aux(nd.v, double(s.goals[nd.mover] - s.goals[opponent(nd.mover)]) / 5.0, p.aux_w);   // 끝: 실제 골 차 / 5 (학습 aux 목표와 같은 식 · mover 관점 — 11-21 골 데미지 없음)
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
	std::vector<float> ts;   // 전술 대본 점수 (11-22) — 높은 수 넷은 꼭 탐색한다
	const bool tac = g_tac_score && p.style.tac >= 0 && r0.mover == p.style_team;
	if (tac) {
		ts.resize(k);
		for (size_t i = 0; i < k; i++)
			ts[i] = g_tac_score(s0, r0.acts[i], p.style);
		std::vector<int> ord(k);
		for (size_t i = 0; i < k; i++)
			ord[i] = int(i);
		std::sort(ord.begin(), ord.end(), [&](int a, int b) { return ts[a] > ts[b]; });
		for (int j = 0, add = 0; j < int(k) && add < 4; j++)
			if (std::find(cand.begin(), cand.end(), ord[j]) == cand.end()) {
				cand.push_back(ord[j]);
				add++;
			}
		m = int(cand.size());
	}
	const std::vector<int> cand0 = cand;
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
	if (tac) {   // 허용 폭 안에서 대본 점수가 가장 높은 수 (값은 많이 간 수만 믿는다)
		int nmin = std::max(2, p.sims / (4 * std::max(1, m)));
		double best = -2.0;
		for (int a : cand0)
			if (r.n[a] >= nmin)
				best = std::max(best, r.w[a] / r.n[a]);
		int pick = cand[0];
		for (int a : cand0)
			if (r.n[a] >= nmin && r.w[a] / r.n[a] >= best - p.style.eps && (ts[a] > ts[pick] || (ts[a] == ts[pick] && r.w[a] / r.n[a] > r.w[pick] / std::max(1, r.n[pick]))))
				pick = a;
		if (r.acts[cand[0]].t == A_SHOT && r.w[cand[0]] / std::max(1, r.n[cand[0]]) > 0.0)
			pick = cand[0];   // 득점 기회(값이 이기는 쪽인 슛)는 대본이 가로채지 않는다
		if (r.n[pick] >= nmin || pick == cand[0])
			cand[0] = pick, res.action = r.acts[pick];
	}
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
	// 11-20 사용자 「구성 넓혀라」 — 커리어와 같은 꼴: 8직업 · 스탯은 킥(슛·패스 같이)·무브·디펜스 각 0~3
	// 만능은 뺌 (11-20 사용자 「만능은 없고, 각 직업 4명씩」 — 옛 만능 에이스는 직업을 받음)
	constexpr int JOBS_N = 9;   // 11-23 직업 9개 (스나이퍼를 캐논에 합침 — 옛 부스터 자리는 새 캐릭터 몫으로 빔 · 패서 = 옛 노마드 자리)
	static const int POOL[JOBS_N] = { CANNON, MAESTRO, TRICKSTER, SPRINTER, MARKER, TITAN, HUNTER, WALL, PASSER };
	int pos[PER_TEAM], bonus[PER_TEAM][4] = {}, rng_up[PER_TEAM] = {}, krng_up[PER_TEAM] = {}, hold_up[PER_TEAM] = {};
	int used = 0;
	const int prospects = int(next() % (PER_TEAM + 1));   // 유망주 0~5명
	const int level = int(next() % 4);                    // 팀 성장 정도 0~3 → 선수당 스탯 합 0 ~ 3 × level
	const int mono = next() % 5 == 0 ? POOL[next() % JOBS_N] : 0;   // 20 % 는 한 직업 팀
	// 몰아 키운 에이스 (11-23 사용자 「몰아 키운 에이스도 자가학습에 넣어」): 30 % 팀은 전직 선수 한 명이 카드 8~13장 (커리어 한 해 ≈ 26장 · 한 선수 다 키우기 ≈ 13장)
	const bool ace_team = next() % 10 < 3;
	const int ace = ace_team && prospects < PER_TEAM ? prospects + int(next() % (PER_TEAM - prospects)) : -1;
	for (int i = 0; i < PER_TEAM; i++) {
		if (i < prospects) {
			pos[i] = PROSPECT;
		} else if (mono) {
			pos[i] = mono;
		} else {
			int k;
			do
				k = int(next() % JOBS_N);
			while ((used >> k) & 1);
			used |= 1 << k;
			pos[i] = POOL[k];
		}
		int st[GROW_K] = {};   // 슛 · 패스 · 무브 · 디펜스 · 특수(마에스트로 패스 범위 · 중거리 슈터 슛 범위 · 스프린터 무브 범위 · 월 지속)
		if (pos[i] != PROSPECT && (level > 0 || i == ace)) {   // 성장은 전직한 선수만 — 커리어와 같은 카드 셈 (카드 1장 = +1 · 스탯마다 한 번, 11-23)
			const int n = i == ace ? 8 + int(next() % 6) : int(next() % (2 * level + 2));   // 카드 0 ~ 7장 · 에이스 8 ~ 13장
			for (int c = 0; c < n; c++) {
				int ok[GROW_K], m = 0;
				for (int k = 0; k < GROW_K; k++)
					if (grow_ok(pos[i], st, k))
						ok[m++] = k;
				if (!m)
					break;
				st[ok[next() % m]]++;   // 카드 1장 = +1 (11-23 스탯마다 한 번)
			}
		}
		(pos[i] == MAESTRO || pos[i] == CANNON ? krng_up : pos[i] == WALL ? hold_up : rng_up)[i] = st[4];
		bonus[i][ST_SHOT] = st[0];
		bonus[i][ST_PASS] = st[1];
		bonus[i][ST_DRIBBLE] = st[2];
		bonus[i][ST_DEF] = st[3];
	}
	set_lineup(s, team, pos, bonus);
	set_perks(s, team, rng_up, hold_up, krng_up);
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
		new_match(s, AWAY);   // 홈이 공을 먼저 (v9.0)
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
		st.lost += 0;   // 옛 골 데미지 합 (11-21 없음)
		st.moves += (long long)recs.size();
		for (size_t i = 0; i < recs.size(); i++) {
			int me = mover[i];
			const int w = i < n_match ? win_match : win;   // 경기 수 = 경기 결과(숫자 합 판정승 = 승) · 일기토 수 = 일기토 결과
			double z = w == -1 ? 0.0 : (w == me ? 1.0 : -1.0);   // 비김 = 0 (11-18)
			recs[i].z = i < n_match ? float(lam * z + (1.0 - lam) * recs[i].q) : float(z);   // 일기토 수는 탐색 가치가 없다 — 결과만
			recs[i].aux = float(std::max(-1.0, std::min(1.0, (s.goals[me] - s.goals[1 - me]) / 5.0)));   // 골 차 / 5 (11-21 — 옛 골 데미지 차 / 100)
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
