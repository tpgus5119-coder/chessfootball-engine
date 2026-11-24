// 카드 모드 알파제로 (card_az.h) — 탐색·망 읽기·전방 계산
#include "card_az.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cf {
namespace card {
namespace az {

static inline int flip_tile(V2 t, int team) { return t.y * W + (team == AWAY ? W - 1 - t.x : t.x); }

// 칸 → 입력·수 칸 번호 (뒤집은 칸 기준): 박스 밖 80칸 · 자기 진영 박스 밖 40칸 (−1 = 그 칸 없음)
static const struct Tiles {
	int8_t field[NT], place[NT];
	Tiles() {
		int nf = 0, np = 0;
		for (int t = 0; t < NT; t++) {
			const V2 v{ t % W, t / W };
			const bool box = is_any_box(v);
			field[t] = int8_t(box ? -1 : nf++);
			place[t] = int8_t(box || v.x >= W / 2 ? -1 : np++);
		}
	}
} g_tiles;
static inline int ftile(V2 t, int team) { return in_bounds(t) ? g_tiles.field[flip_tile(t, team)] : -1; }

static inline void add(NetInput &in, int base, int f) {   // f < 0 = 박스 칸 (선수·공·존은 거기 안 선다 — 입력 없음)
	if (f >= 0 && in.n < MAX_ACTIVE)
		in.idx[in.n++] = uint16_t(base + f);
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
			add(in, OFF_UNPLACED + rel * NPOS, sl);
			continue;
		}
		const int f = ftile(q.pos, team);
		add(in, OFF_PIECE + (rel * NPOS + sl) * NF, f);
		if (q.stance > 0)
			add(in, OFF_STANCE + (rel * 3 + std::max(0, std::min(2, q.stance - 2))) * NF, f);
		if (s.ball_owner == q.uid)
			add(in, OFF_BALL + rel * NF, f);
		if (marked(s, q))
			add(in, OFF_MARKED + rel * NF, f);
		for (int k = 0; k < 4; k++)
			if (q.bonus[k] > 0)
				add(in, OFF_PSTAT + (rel * 4 + k) * NF, f);
		if (s.gk[q.team] == q.uid)
			add(in, OFF_GK + rel * NF, f);   // 골대 안이면 f < 0 — 입력 없음 (d[22]·d[23])
		if (q.charge && rel == 0)   // 「강하게」는 두는 팀 차례 안에서만 — 상대 칸은 늘 0 이라 없다
			add(in, OFF_CHARGE, f);
	}
	if (s.phase == PLAY && s.turn_team >= 0)   // 두는 팀: 이번 차례에 행동한 선수 칸
		for (const Player &q : s.players)
			if (q.placed() && q.team == s.turn_team && q.acted)
				add(in, OFF_ACTED + (q.uid - s.turn_team * PER_TEAM) * NF, ftile(q.pos, team));
	for (int r = 0; r < 2; r++) {   // 태세 존 칸 — 팀마다 겹친 칸은 한 번
		bool on[NT] = {};
		for (const Player &q : s.players) {
			if ((q.team == team ? 0 : 1) != r)
				continue;
			V2 z[4];
			const int nz = zone_tiles(q, z);
			for (int k = 0; k < nz; k++) {
				const int t = flip_tile(z[k], team);
				if (!on[t])
					on[t] = true, add(in, OFF_ZONE + r * NF, g_tiles.field[t]);
			}
		}
	}
	if (s.phase == PLACEMENT && !s.redeploy)
		for (int r = 0; r < 2; r++)
			for (int k = 0; k < CARD_POOL_N; k++)
				if (!drafted[r][k])
					add(in, OFF_BENCH + r * POOL_N, k);
	float *d = in.dense;
	d[0] = s.phase == PLACEMENT;
	d[1] = s.phase == PLAY;
	d[2] = float(s.hold) / 5.0f;
	d[3] = float(std::max(0, std::min(s.turn_count, CARD_HALF_TURNS))) / float(CARD_HALF_TURNS);   // 연장(골든골)은 1 그대로
	d[4] = float(s.goals[team]) / 2.0f;
	d[5] = float(s.goals[opp]) / 2.0f;
	d[6] = float(s.pts) / float(POINTS_PER_TURN);
	d[7] = kickoff_turn(s);
	d[8] = float(s.placement_index) / float(2 * s.team_n);
	d[9] = float(assist_value(s, team)) / 4.0f;
	d[10] = s.opener == team;
	d[11] = s.role[team] == R_ATK;
	d[12] = s.phase == PLAY && s.ball_team() == team;
	d[13] = float(std::max(-3.0, std::min(3.0, heuristic(s, team))) / 3.0);
	d[14] = float(s.ghp[team]) / 5.0f;   // 골키퍼 남은 체력 — 우리 · 상대
	d[15] = float(s.ghp[opp]) / 5.0f;
	d[16] = float(s.gk_range[team]) / 6.0f;   // 골키퍼 롱킥 범위 — 우리 · 상대
	d[17] = float(s.gk_range[opp]) / 6.0f;
	d[18] = s.keeper == team;   // 키퍼가 공을 쥔 팀 — 우리 · 상대
	d[19] = s.keeper == opp;
	d[20] = s.ot;          // 연장(골든골) 중
	d[21] = s.knockout;    // 토너먼트(단판 — 비기면 연장) · 리그는 0
	d[22] = s.gk_in(team);   // 골키퍼가 골대 안 (밖이면 골대 체력 0) — 우리 · 상대
	d[23] = s.gk_in(opp);
	d[24] = float(s.gk_kick[team]) / 6.0f;   // 키퍼 롱킥 힘 — 우리 · 상대
	d[25] = float(s.gk_kick[opp]) / 6.0f;
}

int action_index(const State &s, const Action &a) {
	const int team = s.turn_team;
	const int me = a.uid - PER_TEAM * team;   // 두는 팀 안 번호 (배치는 uid = 드래프트 슬롯)
	const bool mine = me >= 0 && me < PER_TEAM;
	switch (a.t) {
		case A_MOVE:
		case A_DRIBBLE:
		case A_PASS: {
			if (a.t == A_MOVE && is_goal_of(team, a.dest))
				return mine ? OFF_GKIN : -1;   // 골키퍼가 골대로 돌아감
			const int f = ftile(a.dest, team);
			if (!mine || f < 0)
				return -1;
			const int kind = a.t == A_MOVE ? 0 : a.t == A_DRIBBLE ? 1 : 2;
			return OFF_TILE + (kind * PER_TEAM + me) * NF + f;
		}
		case A_SHOT:
			return mine ? OFF_SHOT + me : -1;
		case A_STANCE:   // 제자리 사방 태세 · 공 임자 「강하게」 모으기 수는 번호 없음
			return mine && s.ball_owner != a.uid && a.dest == s.players[a.uid].pos ? OFF_STANCE_A + me : -1;
		case A_PLACE: {
			if (a.uid < 0 || a.uid > POOL_N || !in_bounds(a.dest))
				return -1;
			const int p = g_tiles.place[flip_tile(a.dest, team)];
			return p < 0 ? -1 : OFF_PLACE + a.uid * NPL + p;
		}
		case A_KEEP: {   // 키퍼 롱킥: 받는 아군(me) 발밑
			const int f = ftile(a.dest, team);
			if (!mine || f < 0)
				return -1;
			return OFF_KEEP + me * NF + f;
		}
		case A_SAVE: {   // 공 없는 선수 「강하게」 모으기 — 자기 칸
			const int f = ftile(a.dest, team);
			return f < 0 ? -1 : OFF_SAVE + f;
		}
		default:
			return -1;
	}
}

struct Eval {
	float v = 0.0f;
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
	float v = net.bv;   // 보조 머리(wa·ba)는 학습 목표용 — 탐색엔 안 쓴다
	for (int k = 0; k < h2; k++)
		v += b[k] * net.wv[k];
	out.v = std::tanh(v);
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

float (*g_tac_score)(const State &s, const Action &a, const Style &st) = nullptr;

// ---------- 팀 성격 덤 ----------
// 크기는 logit 단위 — 망 정책의 수 사이 logit 차가 보통 몇 단위라 덤 1 안팎은 「비슷한 수 중에서 고르는 버릇」 정도다 (값과 측정: ui/card_match.gd STYLE_W 설명)
float style_bias(const State &s, const Action &a, const Style &st) {
	if (s.phase != PLAY || a.uid < 0 || a.uid >= PLAYERS)
		return 0.0f;
	const int team = s.turn_team;
	const int fwd = forward_dir(team);
	const Player &p = s.players[a.uid];
	if (p.team != team || p.pos == UNPLACED)
		return 0.0f;
	const V2 ball = s.ball_owner >= 0 ? s.players[s.ball_owner].pos : UNPLACED;
	const int gain = (a.dest.x - p.pos.x) * fwd;   // 앞으로 간 칸
	float b = 0.0f;
	if (s.ball_team() == team) {   // 공격
		if (a.t == A_PASS) {
			const int d = manhattan(p.pos, a.dest);
			b += st.poss * (d <= 2 ? 1.0f : 0.0f);                             // 지공: 짧은 패스 (긴 킥을 깎으면 슛 길이 막혀 약해짐 — 측정 11-12)
			b += st.ctr * (d >= 3 ? 1.0f : -0.5f) + st.ctr * 0.25f * float(std::max(0, gain));   // 역습: 긴 킥·앞으로
		} else if (a.t == A_DRIBBLE) {
			b += st.ctr * 0.4f * float(std::max(-1, std::min(3, gain)));         // 역습: 빠른 전진
		} else if (a.t == A_MOVE && a.uid != s.ball_owner) {
			b += st.poss * 0.3f * float(std::max(0, std::min(2, gain)));         // 지공: 가담 (공 없는 동료가 올라감)
		}
	} else if (ball != UNPLACED) {   // 수비
		if (a.t == A_MOVE) {
			const int closer = manhattan(p.pos, ball) - manhattan(a.dest, ball);
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
			nd.v = s.winner == -1 ? 0.0f : (s.winner == nd.mover ? 1.0f : -1.0f);   // 비김 = 0
		} else {
			nd.mover = s.turn_team;
			Eval e;
			evaluate(net, s, nd.acts, e);
			nd.v = e.v;   // s.turn_team(= mover) 관점
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

} // namespace

Result search(const State &s0, const Net &net, const Params &p) {
	Result res;
	Tree tree(net, p);
	tree.nodes.reserve(size_t(p.sims) * 2 + 8);
	const int root = tree.make(s0, -1);
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
	std::vector<float> ts;   // 전술 대본 점수 — 높은 수 넷은 꼭 탐색한다
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
	std::vector<double> pi;
	tree.improved_policy(r, pi);
	res.policy.assign(pi.begin(), pi.end());
	std::vector<double> q;
	tree.completed_q(r, q);
	double rv = 0.0;
	for (size_t i = 0; i < k; i++)
		rv += pi[i] * q[i];
	res.root_v = float(rv);
	return res;
}


void random_lineup(State &s, int team, unsigned long long r) {
	auto next = [&]() {
		r ^= r >> 12;
		r ^= r << 25;
		r ^= r >> 27;
		return r * 0x2545F4914F6CDD1Dull;
	};
	// 커리어와 같은 꼴: 직업 11 · 성장은 카드 셈 · 선수단 5명 중 골키퍼 1명 (절반은 디펜스가 가장 높은 선수 = 커리어 기본값·상대 학교, 절반은 아무나)
	constexpr int JOBS_N = 11;
	static const int POOL[JOBS_N] = { CANNON, MAESTRO, TRICKSTER, SPRINTER, MARKER, TITAN, HUNTER, WALL, TARGETMAN, STRIKER, PLAYMAKER };
	constexpr int SQ = PER_TEAM;
	int pos[SQ], bonus[SQ][4] = {};
	int used = 0;
	const int prospects = int(next() % (SQ + 1));          // 유망주 0~5명
	const int level = int(next() % 4);                    // 팀 성장 정도 0~3 → 선수당 카드 0 ~ 2·level+1 장
	const int mono = next() % 5 == 0 ? POOL[next() % JOBS_N] : 0;   // 20 % 는 한 직업 팀
	const bool ace_team = next() % 10 < 3;   // 몰아 키운 에이스: 30 % 팀은 전직 선수 한 명이 카드 많이
	const int ace = ace_team && prospects < SQ ? prospects + int(next() % (SQ - prospects)) : -1;
	for (int i = 0; i < SQ; i++) {
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
		int st[GROW_K] = {};   // 슛 · 패스 · 무브 · 디펜스
		if (pos[i] != PROSPECT && (level > 0 || i == ace)) {   // 성장은 전직한 선수만 (카드 1장 = +1 · 스탯마다 한 번)
			const int n = i == ace ? 8 + int(next() % 6) : int(next() % (2 * level + 2));
			for (int c = 0; c < n; c++) {
				int ok[GROW_K], m = 0;
				for (int k = 0; k < GROW_K; k++)
					if (grow_ok(pos[i], st, k))
						ok[m++] = k;
				if (!m)
					break;
				st[ok[next() % m]]++;
			}
		}
		bonus[i][ST_SHOT] = st[0];
		bonus[i][ST_PASS] = st[1];
		bonus[i][ST_DRIBBLE] = st[2];
		bonus[i][ST_DEF] = st[3];
	}
	const int gk = next() % 2 ? default_gk(pos, bonus) : int(next() % SQ);
	set_lineup(s, team, pos, bonus, gk);
}

// 길 고르기용 상태 가치 (card.h value_path — 길은 간 뒤의 상태로): team 에 좋을수록 큼 (−1 ~ 1). ctx = const Net *
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
