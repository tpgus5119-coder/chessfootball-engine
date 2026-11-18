// 수 3단계 탐색·망 — tools/cz/tri.h 설명
#include "tri.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#else
#include <cblas.h>   // 리눅스(GitHub Actions 원격 기보) — OpenBLAS
#endif
#include <memory>
#include <unordered_map>

namespace tri {
using cf::AWAY;
using cf::GAME_OVER;
using cf::NT;
using cf::PER_TEAM;
using cf::V2;
using cf::W;
using cf::opponent;

static inline int flip(V2 t, int team) { return t.y * W + (team == AWAY ? W - 1 - t.x : t.x); }

void ids(const State &s, const Action &a, int &w, int &c, int &d) {
	const int team = s.turn_team;
	switch (a.t) {
		case A_PLACE:
			w = W_PLACE + a.uid, c = C_NONE, d = flip(a.dest, team);
			return;
		case A_END:
			w = W_END, c = C_NONE, d = D_STAY;
			return;
		case A_SAVE:
			w = W_KEEPER, c = C_SAVE + a.card, d = a.dest == cf::goal_of(team) ? D_GOAL : flip(a.dest, team);
			return;
		default: {
			const int use = a.t == A_PASS || a.t == A_SHOT ? 0 : a.t == A_STANCE ? 2 : 1;
			w = a.uid - PER_TEAM * team;
			c = (a.sk * 3 + use) * 6 + a.card;
			d = a.t == A_SHOT ? D_GOAL : a.t == A_STANCE ? D_STAY : flip(a.dest, team);
		}
	}
}

bool Net::load(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	int head[10] = {};
	bool ok = fread(head, 4, 9, f) == 9 && head[0] == MAGIC3 && (head[1] == 1 || (head[1] == 2 && fread(&head[9], 4, 1, f) == 1)) && head[2] == az::SPARSE && head[3] == az::DENSE && head[6] == NW && head[7] == NC && head[8] == ND;
	if (ok) {
		h1 = head[4], h2 = head[5];
		auto rd = [&](std::vector<float> &v, size_t n) {
			v.resize(n);
			return fread(v.data(), 4, n, f) == n;
		};
		blocks = head[9];
		ok = rd(w1, size_t(az::SPARSE + az::DENSE) * h1) && rd(b1, h1) && rd(rw, size_t(blocks) * 2 * (size_t(h1) * h1 + h1)) && rd(w2, size_t(h1) * h2) && rd(b2, h2) && rd(wv, h2) && fread(&bv, 4, 1, f) == 1 &&
				rd(wa, h2) && fread(&ba, 4, 1, f) == 1 && rd(pw, size_t(NW) * h2) && rd(bw, NW) && rd(e1, size_t(NW) * h2) && rd(pc, size_t(NC) * h2) &&
				rd(bc, NC) && rd(e2, size_t(NC) * h2) && rd(pd, size_t(ND) * h2) && rd(bd, ND);
	}
	fclose(f);
	if (!ok)
		h1 = h2 = 0;
	return ok;
}

namespace {

struct Rng {   // card_az.cpp Rng 와 같은 수열
	unsigned long long s;
	explicit Rng(unsigned long long seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
	unsigned long long next() {
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 0x2545F4914F6CDD1Dull;
	}
	double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
	double normal() {
		double u1 = std::max(unit(), 1e-12), u2 = unit();
		return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
	}
	double gamma(double alpha) {
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

struct SNode {   // 실제 국면
	State s;
	int mover = -1;
	bool terminal = false, root = false;
	float v = 0.0f;
	std::vector<Action> acts;
	std::vector<int> aw, ac, ad;   // 수마다 세 단계 id
	std::vector<float> b;          // 몸통 출력 (머리 셋이 같이 쓴다)
	int g1 = -1;
	bool pending = false;          // 묶음 평가를 기다리는 중 (가치·몸통 아직 없음)
	std::unique_ptr<az::NetInput> in;
};

struct GNode {   // 단계 마디 (같은 국면 · 같은 두는 팀)
	int sn, stage, who = -1, card = -1;
	bool expanded = false;
	std::vector<int> id;       // 1·2단계 = 다음 단계 id · 3단계 = sn 의 수 번호
	std::vector<float> prior;
	std::vector<int> n, child;   // child: 1·2단계 = GNode · 3단계 = SNode (−1 = 아직)
	std::vector<double> w;
	std::vector<std::vector<V2>> path;   // 3단계: 그 자식의 길 (빈 것 = 엔진 기본 길) — 결과가 다른 길 후보 (card.h outcome_paths)
	int N = 0;
	double W = 0.0;
};

// 몸통 — 국면 여러 개를 한 번에 (11-18 묶음 평가): 잔차 블록은 행렬 × 행렬(Accelerate sgemm)이라 무게를 한 번 읽어 여러 국면에 쓴다.
//   깊은 망은 무게 읽기(메모리 대역폭)가 병목이라 프로세스 여럿이 겹치면 CPU 가 70 %에 묶였다 — 묶으면 국면당 읽기가 1/묶음 수
static void trunk_batch(const Net &net, const az::NetInput *const *in, int B, std::vector<float> *const *out) {
	static thread_local std::vector<float> A, U;
	const int h1 = net.h1, h2 = net.h2;
	A.resize(size_t(B) * h1);
	for (int r = 0; r < B; r++) {
		float *a = &A[size_t(r) * h1];
		std::copy(net.b1.begin(), net.b1.end(), a);
		for (int i = 0; i < in[r]->n; i++) {
			const float *row = &net.w1[size_t(in[r]->idx[i]) * h1];
			for (int j = 0; j < h1; j++)
				a[j] += row[j];
		}
		for (int k = 0; k < az::DENSE; k++) {
			const float v = in[r]->dense[k];
			if (v == 0.0f)
				continue;
			const float *row = &net.w1[size_t(az::SPARSE + k) * h1];
			for (int j = 0; j < h1; j++)
				a[j] += v * row[j];
		}
		for (int j = 0; j < h1; j++)
			a[j] = std::min(std::max(a[j], 0.0f), 1.0f);
	}
	const size_t L = size_t(h1) * h1 + h1;
	U.resize(size_t(B) * h1);
	for (int k = 0; k < net.blocks; k++) {   // 잔차 블록: a = clamp(a + Wb·relu(Wa·a + ba) + bb, 0, 1) — train3.py 와 같다
		const float *Wa = &net.rw[2 * L * k], *ba = Wa + size_t(h1) * h1, *Wb = ba + h1, *bb = Wb + size_t(h1) * h1;
		for (int r = 0; r < B; r++)
			std::copy(ba, ba + h1, &U[size_t(r) * h1]);
		cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, B, h1, h1, 1.0f, A.data(), h1, Wa, h1, 1.0f, U.data(), h1);
		for (size_t i = 0; i < U.size(); i++)
			U[i] = std::max(U[i], 0.0f);
		for (int r = 0; r < B; r++)
			for (int j = 0; j < h1; j++)
				A[size_t(r) * h1 + j] += bb[j];
		cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, B, h1, h1, 1.0f, U.data(), h1, Wb, h1, 1.0f, A.data(), h1);
		for (size_t i = 0; i < A.size(); i++)
			A[i] = std::min(std::max(A[i], 0.0f), 1.0f);
	}
	for (int r = 0; r < B; r++) {
		std::vector<float> &b = *out[r];
		b.assign(net.b2.begin(), net.b2.end());
		const float *a = &A[size_t(r) * h1];
		for (int j = 0; j < h1; j++) {
			const float v = a[j];
			if (v == 0.0f)
				continue;
			const float *row = &net.w2[size_t(j) * h2];
			for (int k2 = 0; k2 < h2; k2++)
				b[k2] += v * row[k2];
		}
		for (int k2 = 0; k2 < h2; k2++)
			b[k2] = std::min(std::max(b[k2], 0.0f), 1.0f);
	}
}

static void trunk(const Net &net, const az::NetInput &in, std::vector<float> &b) {
	const az::NetInput *pi = &in;
	std::vector<float> *pb = &b;
	trunk_batch(net, &pi, 1, &pb);
}

static float dot(const float *r, const float *x, int n) {
	float t = 0.0f;
	for (int k = 0; k < n; k++)
		t += r[k] * x[k];
	return t;
}

static uint64_t shash(const State &s) {   // 같은 끝 국면 찾기 (전치) — 바이트 지문, 같은지는 memcmp 로 다시 본다
	const unsigned char *b = reinterpret_cast<const unsigned char *>(&s);
	uint64_t h = 1469598103934665603ull;
	size_t i = 0;
	for (; i + 8 <= sizeof(State); i += 8) {
		uint64_t w;
		std::memcpy(&w, b + i, 8);
		h = (h ^ w) * 1099511628211ull;
		h ^= h >> 29;
	}
	for (; i < sizeof(State); i++)
		h = (h ^ b[i]) * 1099511628211ull;
	return h;
}

struct Tree {
	const Net &net;
	const Params *pp;
	Rng rng;
	std::vector<SNode> sn;
	std::vector<GNode> gn;
	// 전치 (11-18 사용자 「같은 끝 국면 합치기」): 수를 둔 직후 국면(끝 처리 전) → 실제 국면 마디. 한 차례 안 카드 순서만 다른 갈래가 한 마디로 모인다
	std::unordered_map<uint64_t, std::vector<std::pair<int, State>>> tt;
	long long tt_hit = 0;
	Tree(const Net &n_, const Params &p_) : net(n_), pp(&p_), rng(p_.seed) {}
	const Params &P() const { return *pp; }
	int find(const State &c) const {
		auto it = tt.find(shash(c));
		if (it != tt.end())
			for (const auto &e : it->second)
				if (std::memcmp(&e.second, &c, sizeof(State)) == 0)
					return e.first;
		return -1;
	}
	int make_tt(const State &c, int parent_mover) {
		if (g_tt) {
			const int f = find(c);
			if (f >= 0) {
				tt_hit++;
				return f;
			}
		}
		const int si = make(c, parent_mover, false);
		if (g_tt)
			tt[shash(c)].push_back({ si, c });
		return si;
	}

	float aux_mix(float v, double aux) const {   // card_az.cpp with_aux 와 같은 식
		const Params &p = P();
		if (p.aux_w == 0.0)
			return v;
		return float(std::max(-1.0, std::min(1.0, double(v) + p.aux_w * std::max(-1.0, std::min(1.0, aux)))));
	}

	int gnew(int s, int stage, int who, int card) {
		GNode g;
		g.sn = s, g.stage = stage, g.who = who, g.card = card;
		gn.push_back(std::move(g));
		return int(gn.size()) - 1;
	}

	int make(State s, int parent_mover, bool root) {   // card_az.cpp Tree::make 와 같은 끝 처리
		SNode nd;
		for (int guard = 0; guard < 8 && s.phase != GAME_OVER; guard++) {
			enumerate_actions(s, nd.acts);
			if (!nd.acts.empty())
				break;
			end_turn(s);
		}
		nd.s = s;
		nd.root = root;
		if (s.phase == GAME_OVER || nd.acts.empty()) {
			nd.terminal = true;
			nd.mover = parent_mover < 0 ? s.turn_team : parent_mover;
			nd.v = s.winner == -1 ? 0.0f : (s.winner == nd.mover ? 1.0f : -1.0f);   // 비김 = 0 (11-18 사용자: 0:0 특수 처리 없앰)
			nd.v = aux_mix(nd.v, double(s.lost[opponent(nd.mover)] - s.lost[nd.mover]) / 100.0);
		} else {
			nd.mover = s.turn_team;
			const size_t k = nd.acts.size();
			nd.aw.resize(k), nd.ac.resize(k), nd.ad.resize(k);
			for (size_t i = 0; i < k; i++)
				ids(s, nd.acts[i], nd.aw[i], nd.ac[i], nd.ad[i]);
			if (net.loaded()) {
				nd.in.reset(new az::NetInput);
				az::net_input(s, s.turn_team, *nd.in);
				nd.pending = true;   // 몸통·가치는 묶음으로 (eval_pending)
			} else {
				nd.v = float(std::tanh(heuristic(s, s.turn_team) / 2.0));
			}
		}
		sn.push_back(std::move(nd));
		const int si = int(sn.size()) - 1;
		if (!sn[si].terminal)
			sn[si].g1 = gnew(si, 1, -1, -1);
		return si;
	}

	void expand(int gi) {
		const Params &p = P();
		GNode &g = gn[gi];
		const SNode &s = sn[g.sn];
		g.expanded = true;
		if (g.stage == 3) {
			static thread_local std::vector<std::vector<V2>> alt;
			for (size_t i = 0; i < s.acts.size(); i++)
				if (s.aw[i] == g.who && s.ac[i] == g.card) {
					// 길 후보는 이 (누가·무엇) 묶음을 처음 열 때만 센다 — 결과가 같은 길이 하나뿐인 대부분의 칸은 자식 하나 그대로.
					// 루즈볼 칸을 지나면 줍는다(규칙) → 줍는 길·안 줍는 길은 끝 상태가 달라 따로 후보가 되고, 가치로 갈린다
					outcome_paths(s.s, s.acts[i], alt);
					const size_t np = std::max<size_t>(1, alt.size());
					for (size_t k = 0; k < np; k++) {
						g.id.push_back(int(i));
						g.path.push_back(k == 0 ? std::vector<V2>() : alt[k]);
					}
				}
		} else {
			for (size_t i = 0; i < s.acts.size(); i++) {
				if (g.stage == 2 && s.aw[i] != g.who)
					continue;
				const int x = g.stage == 1 ? s.aw[i] : s.ac[i];
				if (std::find(g.id.begin(), g.id.end(), x) == g.id.end())
					g.id.push_back(x);
			}
		}
		const size_t k = g.id.size();
		std::vector<float> lg(k, 0.0f);
		if (net.loaded()) {
			const int h2 = net.h2;
			std::vector<float> h(s.b);
			if (g.stage >= 2)
				for (int j = 0; j < h2; j++)
					h[j] = std::min(std::max(s.b[j] + net.e1[size_t(g.who) * h2 + j] + (g.stage == 3 ? net.e2[size_t(g.card) * h2 + j] : 0.0f), 0.0f), 1.0f);
			for (size_t i = 0; i < k; i++) {
				const int x = g.stage == 3 ? s.ad[g.id[i]] : g.id[i];
				const float *row = g.stage == 1 ? &net.pw[size_t(x) * h2] : g.stage == 2 ? &net.pc[size_t(x) * h2] : &net.pd[size_t(x) * h2];
				lg[i] = dot(row, h.data(), h2) + (g.stage == 1 ? net.bw[x] : g.stage == 2 ? net.bc[x] : net.bd[x]);
			}
		}
		float mx = -1e30f, sum = 0.0f;
		for (float l : lg)
			mx = std::max(mx, l);
		g.prior.resize(k);
		for (size_t i = 0; i < k; i++)
			sum += (g.prior[i] = std::exp(lg[i] - mx));
		for (float &x : g.prior)
			x /= sum;
		if (g.stage == 3) {   // 같은 도착 칸 수가 여럿(결과가 다른 길 — tri.h 확장 지점)이면 그 칸 몫을 나눠 갖는다. 길이 하나뿐이면 그대로
			float t = 0.0f;
			for (size_t i = 0; i < k; i++) {
				int same = 0;
				for (size_t j = 0; j < k; j++)
					same += s.ad[g.id[j]] == s.ad[g.id[i]];
				t += (g.prior[i] /= float(same));
			}
			for (float &x : g.prior)
				x /= t;
		}
		if (s.root && p.dirichlet_eps > 0.0 && k > 1) {
			const double alpha = std::min(1.0, std::max(0.03, 10.0 / double(k)));
			std::vector<double> d(k);
			double ds = 0.0;
			for (size_t i = 0; i < k; i++)
				ds += (d[i] = rng.gamma(alpha));
			for (size_t i = 0; i < k; i++)
				g.prior[i] = float((1.0 - p.dirichlet_eps) * g.prior[i] + p.dirichlet_eps * d[i] / std::max(ds, 1e-12));
		}
		g.n.assign(k, 0);
		g.w.assign(k, 0.0);
		g.child.assign(k, -1);
	}

	int pick(int gi) {
		const Params &p = P();
		const GNode &g = gn[gi];
		const SNode &s = sn[g.sn];
		double seen = 0.0;
		for (size_t i = 0; i < g.id.size(); i++)
			if (g.n[i])
				seen += g.prior[i];
		const double pq = g.N > 0 ? g.W / g.N : s.v;
		const double fpu = pq - (s.root ? p.fpu_root : p.fpu) * std::sqrt(seen);
		const double sq = std::sqrt(double(std::max(1, g.N)));
		int best = 0;
		double bs = -1e30;
		for (size_t i = 0; i < g.id.size(); i++) {
			const double q = g.n[i] ? g.w[i] / g.n[i] : fpu;
			const double sc = q + (s.root && p.c_root > 0.0 ? p.c_root : p.c_puct) * g.prior[i] * sq / (1.0 + g.n[i]);
			if (sc > bs)
				bs = sc, best = int(i);
		}
		return best;
	}

	void eval_pending(const std::vector<int> &sis) {   // 묶음 평가
		if (sis.empty())
			return;
		std::vector<const az::NetInput *> in;
		std::vector<std::vector<float> *> out;
		for (int si : sis)
			in.push_back(sn[si].in.get()), out.push_back(&sn[si].b);
		trunk_batch(net, in.data(), int(sis.size()), out.data());
		for (int si : sis) {
			SNode &nd = sn[si];
			nd.v = aux_mix(std::tanh(net.bv + dot(net.wv.data(), nd.b.data(), net.h2)), std::tanh(net.ba + dot(net.wa.data(), nd.b.data(), net.h2)));
			nd.pending = false;
			nd.in.reset();
		}
	}

	// 한 번 내려가기: 가지마다 가상 손실(방문 +1 · 값 −1)을 걸어 같은 묶음의 다른 내려가기가 다른 길로 가게 한다 (AlphaGo·KataGo 묶음 탐색)
	struct Leaf {
		std::vector<std::pair<int, int>> path;
		int si = -1, tb = 0;
		bool ok = false;
	};
	void vloss(const Leaf &L, int sign) {
		for (auto &e : L.path) {
			GNode &g = gn[e.first];
			g.n[e.second] += sign, g.N += sign;
			g.w[e.second] -= sign, g.W -= sign;
		}
	}
	Leaf descend(int root) {
		Leaf L;
		int cur = sn[root].g1;
		for (;;) {
			if (!gn[cur].expanded)
				expand(cur);
			const int i = pick(cur);
			L.path.push_back({ cur, i });
			GNode &g = gn[cur];
			if (g.stage < 3) {
				if (g.child[i] < 0) {
					const int c = g.stage == 1 ? gnew(g.sn, 2, g.id[i], -1) : gnew(g.sn, 3, g.who, g.id[i]);
					gn[cur].child[i] = c;
				}
				cur = gn[cur].child[i];
				continue;
			}
			const int si = g.sn;
			if (g.child[i] < 0) {
				const int ai = g.id[i];
				State c = sn[si].s;
				if (!g.path[i].empty())
					cf::card::g_path = &g.path[i];
				apply_action(c, sn[si].acts[ai]);
				cf::card::g_path = nullptr;
				const size_t before = sn.size();
				const int ci = make_tt(c, sn[si].mover);
				gn[cur].child[i] = ci;
				if (sn[ci].pending && size_t(ci) < before)   // 전치로 만난 마디가 아직 평가 중 — 이번 내려가기는 버린다
					return L;
				L.si = ci, L.tb += sn[ci].mover != sn[si].mover, L.ok = true;
				return L;
			}
			const SNode &c = sn[g.child[i]];
			if (c.pending)
				return L;
			L.tb += c.mover != sn[si].mover;
			if (c.terminal) {
				L.si = g.child[i], L.ok = true;
				return L;
			}
			cur = c.g1;
		}
	}
	void backup(const Leaf &L) {
		const SNode &x = sn[L.si];
		float leaf = x.v;
		if (!x.terminal && x.g1 >= 0 && gn[x.g1].N > 0)   // 전치로 이미 있던 마디 — 그 마디의 평균 값으로
			leaf = float(gn[x.g1].W / gn[x.g1].N);
		const int leaf_mover = x.mover;
		leaf_turns += L.tb;
		leaves++;
		for (auto &e : L.path) {
			GNode &g = gn[e.first];
			const double val = sn[g.sn].mover == leaf_mover ? leaf : -leaf;
			g.w[e.second] += 1.0 + val;   // 가상 손실을 거두고 진짜 값 (방문 수는 이미 셌다)
			g.W += 1.0 + val;
		}
	}
	double leaf_turns = 0.0;
	int leaves = 0;
	// sims 번 내려가기를 batch 개씩 묶어서
	void run(int root, int sims, int batch) {
		std::vector<Leaf> ls;
		std::vector<int> pend;
		int done = 0, waste = 0;
		while (done < sims && waste < sims) {
			ls.clear(), pend.clear();
			const int want = std::min(batch, sims - done);
			for (int k = 0; k < want; k++) {
				Leaf L = descend(root);
				vloss(L, +1);
				if (!L.ok) {
					vloss(L, -1);
					waste++;
					break;   // 부딪힘 — 지금 묶음을 먼저 평가
				}
				if (sn[L.si].pending && std::find(pend.begin(), pend.end(), L.si) == pend.end())
					pend.push_back(L.si);
				ls.push_back(std::move(L));
			}
			eval_pending(pend);
			for (const Leaf &L : ls)
				backup(L);
			done += int(ls.size());
			if (ls.empty())
				break;
		}
	}
};

static int most(const GNode &g) {
	int b = 0;
	for (size_t i = 1; i < g.n.size(); i++)
		if (g.n[i] > g.n[b] || (g.n[i] == g.n[b] && g.prior[i] > g.prior[b]))
			b = int(i);
	return b;
}

} // namespace

float value(const Net &net, const az::NetInput &in) {
	std::vector<float> b;
	trunk(net, in, b);
	return std::tanh(net.bv + dot(net.wv.data(), b.data(), net.h2));
}

bool g_tt = true;

Reuse::~Reuse() { clear(); }
void Reuse::clear() {
	delete static_cast<Tree *>(impl);
	impl = nullptr;
	net = nullptr;
}

void params_from_env(Params &p) {
	if (const char *e = getenv("CF_CPUCT"))
		p.c_puct = atof(e);
	if (const char *e = getenv("CF_CROOT"))
		p.c_root = atof(e);
	if (const char *e = getenv("CF_BATCH"))
		p.batch = atoi(e);
	if (const char *e = getenv("CF_FPU"))
		p.fpu = atof(e);
	if (const char *e = getenv("CF_FPU_ROOT"))
		p.fpu_root = atof(e);
}

Result search(const State &s0, const Net &net, const Params &p, Reuse *keep) {
	Result r;
	// 나무 재사용 (11-18): 지난 탐색 나무에서 지금 국면(둔 수 직후 국면 = 전치 표 열쇠)을 찾으면 그 마디를 뿌리로 이어 쓴다
	Tree *tp = keep && keep->net == &net ? static_cast<Tree *>(keep->impl) : nullptr;
	int root = -1;
	if (tp) {
		tp->pp = &p;
		tp->rng = Rng(p.seed);
		root = tp->sn.size() < keep->cap ? tp->find(s0) : -1;
		if (root < 0 || tp->sn[root].terminal) {
			keep->miss++;
			keep->clear();
			tp = nullptr, root = -1;
		}
	} else if (keep) {
		keep->miss++;
		keep->clear();
	}
	std::unique_ptr<Tree> own;
	if (!tp) {
		tp = new Tree(net, p);
		if (keep) {
			keep->impl = tp, keep->net = &net;
			if (!keep->cap)
				keep->cap = 400000;
		} else {
			own.reset(tp);
		}
		tp->sn.reserve(size_t(p.sims) + 8);
		tp->gn.reserve(size_t(p.sims) * 4 + 8);
		root = tp->make(s0, -1, true);
		if (tp->sn[root].pending)
			tp->eval_pending({ root });
		if (g_tt)
			tp->tt[shash(s0)].push_back({ root, s0 });
	} else {
		keep->hit++;
		for (SNode &x : tp->sn)
			x.root = false;
		tp->sn[root].root = true;
		GNode &g = tp->gn[tp->sn[root].g1];
		keep->kept += g.N;
		if (g.expanded && p.dirichlet_eps > 0.0 && g.id.size() > 1) {   // 이어 쓴 뿌리에도 잡음 (처음 열 때 못 넣은 것)
			const double alpha = std::min(1.0, std::max(0.03, 10.0 / double(g.id.size())));
			std::vector<double> d(g.id.size());
			double ds = 0.0;
			for (double &x : d)
				ds += (x = tp->rng.gamma(alpha));
			for (size_t i = 0; i < d.size(); i++)
				g.prior[i] = float((1.0 - p.dirichlet_eps) * g.prior[i] + p.dirichlet_eps * d[i] / std::max(ds, 1e-12));
		}
	}
	Tree &t = *tp;
	r.acts = t.sn[root].acts;
	if (t.sn[root].terminal) {
		r.root_v = t.sn[root].v;
		return r;
	}
	const int sims = r.acts.size() == 1 ? std::max(1, p.sims / 4) : p.sims;
	t.run(root, sims, std::max(1, std::min(p.batch, sims / 8)));
	r.tt_hit = t.tt_hit;
	const GNode &g1 = t.gn[t.sn[root].g1];
	r.found = true;
	r.root_v = float(g1.W / std::max(1, g1.N));
	for (size_t i = 0; i < g1.id.size(); i++) {
		r.who[g1.id[i]] = float(g1.n[i]) / float(std::max(1, g1.N));
		r.who_legal |= 1u << g1.id[i];
	}
	// 결합 방문 분포 (3단계 잎 방문 수) — 정책 목표와 온도 표본
	r.joint.assign(r.acts.size(), 0.0f);
	for (size_t i = 0; i < g1.id.size(); i++) {
		if (g1.child[i] < 0)
			continue;
		const GNode &g2 = t.gn[g1.child[i]];
		for (size_t j = 0; j < g2.id.size(); j++) {
			if (g2.child[j] < 0)
				continue;
			const GNode &g3 = t.gn[g2.child[j]];
			for (size_t k = 0; k < g3.id.size(); k++)
				r.joint[g3.id[k]] += float(g3.n[k]) / float(std::max(1, g1.N));
		}
	}
	std::vector<int> ord(r.acts.size());
	for (size_t i = 0; i < ord.size(); i++)
		ord[i] = int(i);
	std::sort(ord.begin(), ord.end(), [&](int a, int b) { return r.joint[a] > r.joint[b]; });
	float mass = 0.0f;
	for (int a : ord) {
		if (r.nk >= K || r.joint[a] <= 0.0f)
			break;
		const SNode &s = t.sn[root];
		r.tw[r.nk] = uint8_t(s.aw[a]), r.tc[r.nk] = uint8_t(s.ac[a]), r.td[r.nk] = uint8_t(s.ad[a]);
		mass += (r.tp[r.nk] = r.joint[a]);
		r.nk++;
	}
	for (int i = 0; i < r.nk; i++)
		r.tp[i] /= std::max(mass, 1e-6f);
	r.leaf_turns = float(t.leaf_turns / std::max(1, t.leaves));
	for (int gi = t.sn[root].g1, prev = t.sn[root].mover; gi >= 0;) {   // 주된 갈래 (단계마다 가장 많이 간 가지 → 다음 국면)
		const GNode &g = t.gn[gi];
		if (!g.expanded || g.N == 0)
			break;
		const int c = g.child[size_t(most(g))];
		if (c < 0)
			break;
		if (g.stage < 3) {
			gi = c;
			continue;
		}
		const SNode &x = t.sn[c];
		r.pv_plies++;
		r.pv_turns += x.mover != prev;
		prev = x.mover;
		gi = x.terminal ? -1 : x.g1;
	}
	// 고른 수: 단계마다 가장 많이 간 가지
	const GNode *g = &g1;
	int pickd = -1;
	for (;;) {
		const int i = most(*g);
		if (g->stage == 3 || g->child[i] < 0) {
			if (g->stage == 3) {
				pickd = g->id[i];
			} else {   // 방문이 없는 가지 (예산이 아주 작을 때) — prior 1등의 첫 수
				const SNode &s = t.sn[root];
				for (size_t a = 0; a < r.acts.size() && pickd < 0; a++)
					if ((g->stage == 1 ? s.aw[a] : s.ac[a]) == g->id[i] && (g->stage == 1 || s.aw[a] == g->who))
						pickd = int(a);
			}
			break;
		}
		g = &t.gn[g->child[i]];
	}
	if (p.temp > 0.0) {
		double sum = 0.0;
		std::vector<double> wt(r.acts.size());
		for (size_t i = 0; i < wt.size(); i++)
			sum += (wt[i] = r.joint[i] > 0.0f ? std::pow(double(r.joint[i]), 1.0 / p.temp) : 0.0);
		double u = t.rng.unit() * sum;
		for (size_t i = 0; i < wt.size() && sum > 0.0; i++)
			if ((u -= wt[i]) <= 0.0 && wt[i] > 0.0) {
				pickd = int(i);
				break;
			}
	}
	r.action = r.acts[pickd < 0 ? 0 : pickd];
	if (pickd >= 0) {   // 그 수의 길 — 가장 많이 간 길 후보
		const SNode &s = t.sn[root];
		for (size_t i = 0; i < g1.id.size(); i++)
			if (g1.id[i] == s.aw[pickd] && g1.child[i] >= 0) {
				const GNode &g2 = t.gn[g1.child[i]];
				for (size_t j = 0; j < g2.id.size(); j++)
					if (g2.id[j] == s.ac[pickd] && g2.child[j] >= 0) {
						const GNode &g3 = t.gn[g2.child[j]];
						int b = -1;
						for (size_t k = 0; k < g3.id.size(); k++)
							if (g3.id[k] == pickd && (b < 0 || g3.n[k] > g3.n[b]))
								b = int(k);
						if (b >= 0)
							r.path = g3.path[b];
					}
			}
	}
	return r;
}

} // namespace tri
