// 감독 모드 자가학습 기보 공장 (11-19 판 4 — 카드 없음 · 4대4). flat(card_az Gumbel) 망이 자가대전하고 az::Rec 로 남긴다 (tools/az/train.py CF_MODE=card)
//   gen3 <출력> <판수> <풀탐색> <빠른탐색> <풀 비율> <시드> <flat:망|flat:-> [flat:상대망 리그비율]
//     flat:- = 망 없음 (손평가 가치 탐색 — 첫 바퀴)
//     상대 망 + 리그 비율 = 그 비율의 판은 배우는 쪽(무작위 한 팀)만 이 망, 상대는 상대 망 — 배우는 쪽 수만 남긴다
//   규칙: 4대4 · 직업 스킬 패시브(sk_passive) · 75 % 무작위 선수단(8직업·커리어 스탯 — 만능 없음 11-21)
//   점수: 리그 판 = 승 +1 · 무 0 · 패 −1 · 토너먼트 판(CF_KO 비율, 기본 0.5) = 비기면 연장(차례 제한 없는 골든골)까지 같은 판에서 둔 승패 · 판 안전 상한(guard)에 걸리면 비김으로 기록
//   학습 방법: 위아래 뒤집기 CF_FLIP(기본 0.5) · 판 중간 시작 CF_BRANCH(기본 0.15) · 앞 12수 온도 표본 · λ CF_LAMBDA 0.7 · 보조 머리(공 얻음·잃음·골)
//   stderr 끝 줄 #gen3 …
#include "../src/card_az.h"
#include "flip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace cf::card;
using cf::AWAY;
using cf::GAME_OVER;
using cf::HOME;

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
};

double envf(const char *k, double d) { return getenv(k) ? atof(getenv(k)) : d; }

// flat 자가대전 기록 (11-19 — 3단계 「누가」 결함을 피해 옛 7단 flat 구조로 이어 배운다): gen.cpp fill_rec0 와 같은 az::Rec
void fill_flat(az::Rec &rec, const State &s0, const std::vector<Action> &acts0, const std::vector<float> &pol, bool use_pi, bool flip) {
	State s = s0;
	std::vector<Action> acts = acts0;
	if (flip) {
		flipy::state(s);
		for (Action &a : acts)
			a = flipy::act(a);
	}
	az::NetInput in;
	az::net_input(s, s.turn_team, in);
	for (int i = 0; i < az::MAX_ACTIVE; i++)
		rec.idx[i] = i < in.n ? in.idx[i] : 0xFFFF;
	std::memcpy(rec.dense, in.dense, sizeof(rec.dense));
	std::vector<int> order(acts.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = int(i);
	std::sort(order.begin(), order.end(), [&](int a, int b) { return pol[a] > pol[b]; });
	float mass = 0.0f;
	int kk = 0;
	for (int i : order) {
		if (kk >= az::PI_K || !use_pi)
			break;
		const int ai = az::action_index(s, acts[i]);
		if (ai < 0)
			continue;
		rec.pi_idx[kk] = uint16_t(ai), rec.pi_p[kk] = pol[i], mass += pol[i], kk++;
	}
	for (int i = 0; i < kk; i++)
		rec.pi_p[i] /= std::max(mass, 1e-6f);
	for (int i = kk; i < az::PI_K; i++)
		rec.pi_idx[i] = 0xFFFF, rec.pi_p[i] = 0.0f;
}

struct Snap {
	State s;
	int kind;   // 0 가치가 크게 틀림 · 1 골 직전 · 2 공 뺏긴 직후
};

} // namespace

int main(int argc, char **argv) {
	knobs_from_env();
	az::aux_from_env();
	if (argc < 8 || strncmp(argv[7], "flat:", 5) != 0) {
		fprintf(stderr, "gen3 <출력> <판수> <풀탐색> <빠른탐색> <풀비율> <시드> <flat:망|flat:-> [flat:상대망 리그비율]\n");
		return 1;
	}
	const int games = atoi(argv[2]), full_sims = atoi(argv[3]), fast_sims = atoi(argv[4]);
	const double full_frac = atof(argv[5]);
	const unsigned long long seed = strtoull(argv[6], nullptr, 10);
	az::Net fnet, fonet;
	auto load = [](az::Net &n, const char *spec) { return strcmp(spec + 5, "-") == 0 || n.load(spec + 5); };
	if (!load(fnet, argv[7])) {
		fprintf(stderr, "망을 못 읽음: %s\n", argv[7]);
		return 1;
	}
	double league_frac = 0.0;
	if (argc >= 10 && strcmp(argv[8], "-") != 0) {
		if (strncmp(argv[8], "flat:", 5) != 0 || !load(fonet, argv[8])) {
			fprintf(stderr, "상대 망을 못 읽음: %s\n", argv[8]);
			return 1;
		}
		league_frac = atof(argv[9]);
	}
	const double ko_frac = envf("CF_KO", 0.5), flip_frac = envf("CF_FLIP", 0.5), branch_frac = envf("CF_BRANCH", 0.15);
	const double lam = envf("CF_LAMBDA", 0.7);
	const double miss_v = envf("CF_BRANCH_MISS", 1.0);
	std::vector<az::Rec> fout;
	long long snap_n[3] = {}, br_games = 0, br_kind[3] = {};
	long long moves = 0, nodes = 0, goals = 0, shots = 0, ots = 0, ko_games = 0, lg_games = 0, lg_win = 0, lg_loss = 0, flipped = 0;
	int home = 0, away = 0, draws = 0;
	Snap pool[3][1];
	Rng rb(seed ^ 0x5A5A5A5AA5A5A5A5ull);
	for (int g = 0; g < games; g++) {
		Rng rng(seed * 1000003ull + g);
		Rng rf((seed * 1000003ull + g) ^ 0x6969696996969696ull);
		bool ko = rng.unit() < ko_frac;
		const bool vs_league = league_frac > 0.0 && rng.unit() < league_frac;
		const int learner = rng.next() % 2 ? AWAY : HOME;
		int bsrc = -1;
		if (branch_frac > 0.0 && rb.unit() < branch_frac) {
			int cand[3], nc = 0;
			for (int k = 0; k < 3; k++)
				if (snap_n[k])
					cand[nc++] = k;
			if (nc)
				bsrc = cand[rb.next() % nc];
		}
		State s;
		new_match(s, AWAY);
		s.sk_passive = 1;
		if (rng.unit() < 0.75)   // 11-20 사용자 「구성 넓혀라」 35 → 75 % (나머지 25 % = 전원 신입생, 커리어 첫 경기 꼴)
			for (int t = 0; t < 2; t++)
				az::random_lineup(s, t, rng.next());
		s.knockout = ko;
		if (bsrc >= 0) {
			s = pool[bsrc][0].s;   // 판 중간 시작 — 녹아웃 여부도 그 판 그대로
			ko = s.knockout;
			br_games++, br_kind[bsrc]++;
		}
		ko_games += ko;
		lg_games += vs_league;
		const size_t r0 = fout.size();
		std::vector<int> mover;
		std::vector<float> trk, qv;
		std::vector<size_t> pend;
		std::vector<State> hist;
		int guard = 0, ply = 0;
		const uint32_t gid = uint32_t(seed * 100003ull + g);
		while (s.phase != GAME_OVER && guard++ < 8000) {
			const bool full = rng.unit() < full_frac;
			const int sims = full ? full_sims : fast_sims;
			const bool mine = !vs_league || s.turn_team == learner;
			az::Params p;
			p.sims = sims, p.seed = rng.next(), p.dirichlet_eps = mine ? 0.25 : 0.0, p.top_m = full && getenv("CF_TOP_M") ? atoi(getenv("CF_TOP_M")) : 16;
			az::Result r = az::search(s, mine ? fnet : fonet, p);
			if (!r.found) {
				end_turn(s);
				continue;
			}
			Action a = r.action;
			if (ply < 12) {
				double u = rng.unit();
				for (size_t i = 0; i < r.acts.size(); i++)
					if ((u -= r.policy[i]) <= 0.0) {
						a = r.acts[i];
						break;
					}
			}
			if (mine) {
				const bool fl = rf.unit() < flip_frac;
				az::Rec frec;
				fill_flat(frec, s, r.acts, r.policy, full, fl);
				frec.q = r.root_v;
				frec.z = frec.aux = 0.0f, frec.game = gid;
				fout.push_back(frec);
				flipped += fl;
				mover.push_back(s.turn_team);
				trk.push_back(0.0f);
				qv.push_back(frec.q);
				pend.push_back(mover.size() - 1);
				hist.push_back(s);
			}
			nodes += sims;
			shots += a.t == A_SHOT;
			const int b0 = s.phase == cf::PLAY ? s.ball_team() : -1, gh0 = s.goals[HOME], ga0 = s.goals[AWAY];
			apply_action(s, a);
			ply++;
			const bool scored = s.goals[HOME] != gh0 || s.goals[AWAY] != ga0;   // 11-21 골문 체력·막힘 없음
			int gain = -1;
			float mag = 0.5f;
			if (scored)
				gain = s.goals[HOME] != gh0 ? HOME : AWAY, mag = 1.0f;
			else if (s.phase == cf::PLAY && b0 >= 0 && s.ball_team() >= 0 && s.ball_team() != b0)
				gain = s.ball_team();
			if (gain >= 0) {
				for (size_t q : pend)
					trk[q] = mover[q] == gain ? mag : -mag;
				pend.clear();
			}
			auto keep = [&](const State &x, int kind) {
				if (x.phase != cf::PLAY)
					return;
				if (rb.next() % (unsigned long long)(++snap_n[kind]) == 0)
					pool[kind][0] = Snap{ x, kind };
			};
			if (scored)
				for (size_t k = hist.size() >= 2 ? hist.size() - 2 : 0; k < hist.size(); k++)
					keep(hist[k], 1);
			if (!scored && gain >= 0 && mag == 0.5f && s.phase == cf::PLAY)
				keep(s, 2);
		}
		const int win = s.phase == GAME_OVER ? s.winner : -1;   // 녹아웃 = 연장 골든골 (11-24 일기토 없앰) · 상한(guard)에 걸려 안 끝난 판은 비김
		ots += s.ot;
		(win == HOME ? home : win == AWAY ? away : draws)++;
		if (vs_league)
			lg_win += win == learner, lg_loss += win != -1 && win != learner;
		goals += s.goals[0] + s.goals[1];
		moves += (long long)mover.size();
		for (size_t i = 0; i < mover.size(); i++) {
			const int me = mover[i];
			double z = win == -1 ? 0.0 : (win == me ? 1.0 : -1.0);
			az::Rec &r = fout[r0 + i];
			r.z = float(lam * z + (1.0 - lam) * r.q), r.aux = trk[i];
			if (std::fabs(qv[i] - z) > miss_v && hist[i].phase == cf::PLAY && rb.next() % (unsigned long long)(++snap_n[0]) == 0)
				pool[0][0] = Snap{ hist[i], 0 };
		}
	}
	FILE *f = fopen(argv[1], "wb");
	if (!f)
		return 1;
	fwrite(fout.data(), sizeof(az::Rec), fout.size(), f);
	fclose(f);
	fprintf(stderr, "#gen3 games=%d home=%d away=%d draw=%d goals=%lld moves=%lld sims=%lld shots=%lld ot=%lld ko=%lld league=%lld lwin=%lld lloss=%lld "
			"flip=%lld branch=%lld bmiss=%lld bgoal=%lld bsteal=%lld\n",
			games, home, away, draws, goals, moves, nodes, shots, ots, ko_games, lg_games, lg_win, lg_loss, flipped, br_games, br_kind[0], br_kind[1], br_kind[2]);
	return 0;
}
