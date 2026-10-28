#include "az.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cf {
namespace az {

// 첫 세대(망 없음)의 가치 = 지금 게임의 선형 평가 (engine.h LIN_W)

static inline int flip_tile(V2 t, int team) {
	return t.y * W + (team == AWAY ? W - 1 - t.x : t.x);
}

int action_index(const State &s, const Action &a) {
	const int team = s.turn_team;
	const int opp = opponent(team);
	switch (a.t) {
		case A_MOVE:
		case A_DRIBBLE:
		case A_PASS:
		case A_SHOT: {
			int kind = a.t == A_MOVE ? 0 : a.t == A_DRIBBLE ? 1 : a.t == A_PASS ? 2 : 3;
			int me = a.uid - PER_TEAM * team;
			if (me < 0 || me >= PER_TEAM || !in_bounds(a.dest))
				return -1;
			if (a.t == A_PASS && (a.cost == 0 || a.cost == 2))   // v5.68 마에스트로 0·2포인트 패스는 따로 센다
				return OFF_PASSC + ((a.cost == 0 ? 0 : 1) * PER_TEAM + me) * NT + flip_tile(a.dest, team);
			return OFF_TILE + (kind * PER_TEAM + me) * NT + flip_tile(a.dest, team);
		}
		case A_TACKLE:
		case A_PUSH:
		case A_GRAB: {
			int kind = a.t == A_TACKLE ? 0 : a.t == A_PUSH ? 1 : 2;
			int me = a.uid - PER_TEAM * team;
			int tg = a.dest.x - PER_TEAM * opp;
			if (me < 0 || me >= PER_TEAM || tg < 0 || tg >= PER_TEAM)
				return -1;
			return OFF_TGT + (kind * PER_TEAM + me) * PER_TEAM + tg;
		}
		case A_STANCE: {
			int me = a.uid - PER_TEAM * team;
			return me < 0 || me >= PER_TEAM ? -1 : OFF_STANCE + me;
		}
		case A_HOLD: {
			int me = a.uid - PER_TEAM * team;
			return me < 0 || me >= PER_TEAM ? -1 : OFF_HOLD + me;
		}
		case A_BOOST: {   // v5.83
			int me = a.uid - PER_TEAM * team;
			return me < 0 || me >= PER_TEAM ? -1 : OFF_BOOST + me;
		}
		case A_UNSTANCE: {   // v5.98
			int me = a.uid - PER_TEAM * team;
			return me < 0 || me >= PER_TEAM ? -1 : OFF_UNSTANCE + me;
		}
		case A_END:
			return OFF_END;
		case A_PLACE: {
			if (a.uid < 0 || a.uid >= POOL_N || !in_bounds(a.dest))
				return -1;
			return OFF_PLACE + a.uid * NT + flip_tile(a.dest, team);
		}
	}
	return -1;
}

// ---------- 망 ----------
constexpr int MAGIC = 0x415A4643; // "CFZA"

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
	if (!take(head, sizeof(head)) || head[0] != MAGIC || head[1] != 1 || head[2] != NN_SPARSE || head[3] != NN_DENSE ||
			head[4] <= 0 || head[4] > 2048 || head[5] <= 0 || head[5] > 512 || head[6] != NA)
		return false;
	h1 = head[4];
	h2 = head[5];
	w1.resize(size_t(NN_SPARSE + NN_DENSE) * h1);
	b1.resize(h1);
	w2.resize(size_t(h1) * h2);
	b2.resize(h2);
	wv.resize(h2);
	wa.resize(h2);
	wp.resize(size_t(h2) * NA);
	bp.resize(NA);
	bool ok = take(w1.data(), 4 * w1.size()) && take(b1.data(), 4 * b1.size()) && take(w2.data(), 4 * w2.size()) &&
			take(b2.data(), 4 * b2.size()) && take(wv.data(), 4 * wv.size()) && take(&bv, 4) &&
			take(wa.data(), 4 * wa.size()) && take(&ba, 4) && take(wp.data(), 4 * wp.size()) &&
			take(bp.data(), 4 * bp.size());
	if (!ok) {
		h1 = h2 = 0;
		return false;
	}
	// 정책 머리를 **수마다 한 줄**(NA×h2)로 뒤집어 둔다 — 파일은 h2×NA라 수 하나를 셀 때 NA칸씩 건너뛰며 읽었다 (캐시 낭비)
	std::vector<float> t(wp.size());
	for (int k = 0; k < h2; k++)
		for (int a = 0; a < NA; a++)
			t[size_t(a) * h2 + k] = wp[size_t(k) * NA + a];
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
	for (int k = 0; k < NN_DENSE; k++) {
		float v = in.dense[k];
		if (v == 0.0f)
			continue;
		const float *row = &net.w1[size_t(NN_SPARSE + k) * h1];
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

void evaluate(const Net &net, const State &s, const std::vector<Action> &acts, Eval &out) {
	out.logits.assign(acts.size(), 0.0f);
	if (!net.loaded()) {
		out.v = float(std::tanh(cf::evaluate(s, s.turn_team, LIN_W) / 2.0));
		out.aux = 0.0f;
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
		if (ai < 0)
			continue;
		float l = net.bp[ai];
		const float *row = &net.wp[size_t(ai) * h2];
		for (int k = 0; k < h2; k++)
			l += b[k] * row[k];
		out.logits[i] = l;
	}
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
			s.end_turn();
		}
		nd.s = s;
		if (s.phase == GAME_OVER || nd.acts.empty()) {
			nd.terminal = true;
			nd.mover = parent_mover < 0 ? s.turn_team : parent_mover;
			nd.v = s.winner == -1 ? 0.0f : (s.winner == nd.mover ? 1.0f : -1.0f);
		} else {
			nd.mover = s.turn_team;
			Eval e;
			evaluate(net, s, nd.acts, e);
			nd.v = e.v;
			nd.logits = std::move(e.logits);
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
	int root = tree.make(s0, -1);
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
	}
	res.action_idx = action_index(s0, res.action);
	res.policy.assign(pi.begin(), pi.end());
	res.visits = r.n;
	std::vector<double> q;
	tree.completed_q(r, q);
	double rv = 0.0;
	for (size_t i = 0; i < k; i++)
		rv += pi[i] * q[i];
	res.root_v = float(rv);
	return res;
}

// ---------- 자가대전 기보 ----------
void selfplay(const Net &net, int games, int sims, unsigned long long seed, int temp_moves, double lam,
		std::vector<Rec> &out, GenStats &st, int fast_sims, double full_frac, double rand_draft_frac, double cup_frac) {
	Params prm;
	prm.dirichlet_eps = 0.25;   // 자가대전만 탐험 잡음 (알파제로 ε 0.25 · α 0.3) — 아레나·게임은 Params 기본값 0
	std::vector<Rec> recs;
	std::vector<int> mover;
	for (int g = 0; g < games; g++) {
		Rng rng(seed * 1000003ull + g);
		bool rand_draft = rand_draft_frac > 0.0 && rng.unit() < rand_draft_frac;   // 이 판은 선발·배치를 무작위로
		bool cup = !rand_draft && cup_frac > 0.0 && rng.unit() < cup_frac;       // 이 판은 정해진 선수단 (az.h cup_frac)
		bool squad[2][POOL_N] = {};
		if (cup) {
			int perm[POOL_N];
			for (int i = 0; i < POOL_N; i++)
				perm[i] = i;
			for (int i = POOL_N - 1; i > 0; i--)
				std::swap(perm[i], perm[int(rng.next() % (unsigned long long)(i + 1))]);
			for (int i = 0; i < POOL_N; i++)
				squad[i < POOL_N / 2 ? HOME : AWAY][perm[i]] = true;
		}
		State s;
		new_open_match(s, g % 2 == 0 ? HOME : AWAY);
		recs.clear();
		mover.clear();
		int guard = 0;
		int ply = 0;
		while (s.phase != GAME_OVER && guard++ < 6000) {
			prm.seed = rng.next();
			bool full = fast_sims <= 0 || rng.unit() < full_frac;
			prm.sims = full ? sims : fast_sims;
			Result r = search(s, net, prm);
			if (!r.found) {
				s.end_turn();
				continue;
			}
			Rec rec;
			NetInput in;
			net_input(s, s.turn_team, in);
			for (int i = 0; i < NN_MAX_ACTIVE; i++)
				rec.idx[i] = i < in.n ? in.idx[i] : 0xFFFF;
			std::memcpy(rec.dense, in.dense, sizeof(rec.dense));
			std::vector<int> order(r.acts.size());
			for (size_t i = 0; i < order.size(); i++)
				order[i] = int(i);
			std::sort(order.begin(), order.end(), [&](int a, int b) { return r.policy[a] > r.policy[b]; });
			float mass = 0.0f;
			int kk = 0;
			for (int i : order) {
				if (kk >= PI_K || !full)   // 빠른 수는 정책 목표 없음 (train.py가 pi_idx 전부 0xFFFF 인 줄을 정책 손실에서 뺀다)
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
			rec.z = 0.0f;
			rec.aux = 0.0f;
			rec.game = uint32_t(seed * 100003ull + g);
			recs.push_back(rec);
			mover.push_back(s.turn_team);
			Action a = r.action;
			if (rand_draft && s.phase == PLACEMENT) {
				a = r.acts[size_t(rng.next() % r.acts.size())];   // 무작위 선발·배치 (정책 목표는 탐색 것 그대로)
			} else if (cup && s.phase == PLACEMENT && !(a.t == A_PLACE && a.uid >= 0 && a.uid < POOL_N && squad[s.turn_team][a.uid])) {
				// 정해진 선수단 밖의 슬롯이면 — 선수단 안의 배치 수 중 탐색이 가장 높게 친 것으로 (칸은 탐색 몫)
				int best = -1;
				for (size_t i = 0; i < r.acts.size(); i++) {
					const Action &c = r.acts[i];
					if (c.t == A_PLACE && c.uid >= 0 && c.uid < POOL_N && squad[s.turn_team][c.uid] &&
							(best < 0 || r.policy[i] > r.policy[size_t(best)]))
						best = int(i);
				}
				if (best >= 0)
					a = r.acts[size_t(best)];
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
		int win = s.phase == GAME_OVER ? s.winner : -1;
		st.games++;
		if (win == -1)
			st.draws++;
		else if (win == HOME)
			st.home++;
		else
			st.away++;
		st.moves += (long long)recs.size();
		for (size_t i = 0; i < recs.size(); i++) {
			int me = mover[i];
			double z = win == -1 ? 0.0 : (win == me ? 1.0 : -1.0);
			recs[i].z = float(lam * z + (1.0 - lam) * recs[i].q);
			int d = s.score[me] - s.score[1 - me];
			recs[i].aux = float(std::max(-1.0, std::min(1.0, d / 2.0)));
		}
		out.insert(out.end(), recs.begin(), recs.end());
	}
}

} // namespace az
} // namespace cf
