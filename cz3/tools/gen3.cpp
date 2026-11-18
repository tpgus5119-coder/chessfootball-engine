// 3단계 본학습 기보 공장 (11-18 사용자 「인원 5명, 나머지 모두 적용해서 새로 자가학습」) — tri.h PUCT 탐색으로 자가대전, 기보 = tri::Rec (train3.py)
//   gen3 <출력> <판수> <풀탐색> <빠른탐색> <풀 비율> <시드> <망> [상대 망 리그 비율]
//     망 = tri 망 파일 · `-` (망 없음 = 손평가) · `flat:<파일>` = **시동 선생**: flat(card_az Gumbel) 망이 두고, 그 개선 정책을 3단계 목표(누가·무엇·어디)로 바꿔 남긴다
//     상대 망(tri) + 리그 비율 = 그 비율의 판은 배우는 쪽(무작위 한 팀)만 이 망, 상대는 상대 망 — 배우는 쪽 수만 남긴다
//   규칙: 5대5 · 직업 스킬 패시브(State.sk_passive) · 손 2-3-4/1-2-3 · 덱 = CF_DECK (없으면 게임 카드 목록 35장 — deck.h)
//   점수 (11-18): 리그 판 = 승 +1 · 무 0 · 패 −1 (0:0 특수 처리 없음) · 토너먼트 판(CF_KO 비율, 기본 0.5) = 비기면 일기토(손평가 탐색)까지 둔 승패
//   학습 방법: 위아래 뒤집기 CF_FLIP(기본 0.5) · 나무 재사용 CF_REUSE(기본 1) · 판 중간 시작 CF_BRANCH(기본 0.15 — 가치가 크게 틀린 국면·골 직전·공 뺏긴 직후)
//   판 꼴은 gen.cpp 와 같다: 35 % 정해진 선수단 · 덱 섞기(15 % 모든 칸 · 10 % 없음 · 나머지 0.6) · 앞 12수 온도 표본 · λ 0.7 · 보조 머리(공 얻음·잃음·골) 그대로
//   측정 (stderr): 공격 한 번 길이(공을 얻음 → 골·막힌 슛·공 잃음, 차례 수) · 골문 깎이기 시작 → 골 차례 수 — 수읽기 깊이 목표의 근거 (11-18)
#include "../src/card_duel.h"
#include "deck.h"
#include "flip.h"
#include "tri.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

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

void input(tri::Rec &rec, const State &s) {
	az::NetInput in;
	az::net_input(s, s.turn_team, in);
	for (int i = 0; i < az::MAX_ACTIVE; i++)
		rec.idx[i] = i < in.n ? in.idx[i] : 0xFFFF;
	std::memcpy(rec.dense, in.dense, sizeof(rec.dense));
}

inline uint8_t flip_d(uint8_t d) {   // 3단계 도착 칸 id 를 위아래로 (골대·제자리는 그대로)
	if (d >= cf::NT)
		return d;
	return uint8_t((cf::H - 1 - d / cf::W) * cf::W + d % cf::W);
}

// 수 하나를 기록으로: 수 목록 acts 와 그 분포 pol (합 1) → 누가 분포 + 결합 상위 K
void fill(tri::Rec &rec, const State &s, const std::vector<Action> &acts, const std::vector<float> &pol, bool flip) {
	State f = s;
	if (flip)
		flipy::state(f);
	input(rec, f);
	std::memset(rec.who, 0, sizeof(rec.who));
	rec.who_legal = 0;
	std::vector<int> w(acts.size()), c(acts.size()), d(acts.size()), ord(acts.size());
	for (size_t i = 0; i < acts.size(); i++) {
		tri::ids(s, acts[i], w[i], c[i], d[i]);
		rec.who_legal |= 1u << w[i];
		rec.who[w[i]] += pol[i];
		ord[i] = int(i);
	}
	std::sort(ord.begin(), ord.end(), [&](int a, int b) { return pol[a] > pol[b]; });
	// 같은 (누가·무엇·어디)가 여럿(결과가 다른 길 등)이면 한 칸으로 합친다
	std::map<int, float> joint;
	for (int i : ord)
		if (pol[i] > 0.0f)
			joint[(w[i] * tri::NC + c[i]) * tri::ND + d[i]] += pol[i];
	std::vector<std::pair<float, int>> top;
	for (auto &e : joint)
		top.push_back({ e.second, e.first });
	std::sort(top.rbegin(), top.rend());
	float mass = 0.0f;
	int k = 0;
	for (; k < tri::K && k < int(top.size()); k++)
		mass += top[k].first;
	for (int i = 0; i < tri::K; i++) {
		if (i < k) {
			const int key = top[i].second;
			rec.tw[i] = uint8_t(key / (tri::NC * tri::ND)), rec.tc[i] = uint8_t(key / tri::ND % tri::NC), rec.td[i] = uint8_t(key % tri::ND);
			if (flip)
				rec.td[i] = flip_d(rec.td[i]);
			rec.tp[i] = top[i].first / std::max(mass, 1e-6f);
		} else {
			rec.tw[i] = rec.tc[i] = rec.td[i] = 255, rec.tp[i] = 0.0f;
		}
	}
}

struct Snap {
	State s;
	int kind;   // 0 가치가 크게 틀림 · 1 골 직전 · 2 공 뺏긴 직후
};

} // namespace

int main(int argc, char **argv) {
	knobs_from_env();
	az::aux_from_env();
	if (argc < 8) {
		fprintf(stderr, "gen3 <출력> <판수> <풀탐색> <빠른탐색> <풀비율> <시드> <망|-|flat:망> [상대망 리그비율]\n");
		return 1;
	}
	const int games = atoi(argv[2]), full_sims = atoi(argv[3]), fast_sims = atoi(argv[4]);
	const double full_frac = atof(argv[5]);
	const unsigned long long seed = strtoull(argv[6], nullptr, 10);
	const bool teach = strncmp(argv[7], "flat:", 5) == 0;
	az::Net fnet;
	tri::Net tnet, onet;
	if (teach ? !fnet.load(argv[7] + 5) : strcmp(argv[7], "-") != 0 && !tnet.load(argv[7])) {
		fprintf(stderr, "망을 못 읽음: %s\n", argv[7]);
		return 1;
	}
	double league_frac = 0.0;
	if (argc >= 10 && strcmp(argv[8], "-") != 0) {
		if (!onet.load(argv[8])) {
			fprintf(stderr, "상대 망을 못 읽음: %s\n", argv[8]);
			return 1;
		}
		league_frac = atof(argv[9]);
	}
	const double ko_frac = envf("CF_KO", 0.5), flip_frac = envf("CF_FLIP", 0.5), branch_frac = envf("CF_BRANCH", 0.15);
	const bool reuse_on = envf("CF_REUSE", 1.0) != 0.0;
	const double miss_v = envf("CF_BRANCH_MISS", 1.0);   // |가치 예측 − 실제 결과| 가 이만큼 넘으면 「크게 틀림」
	tri::Params base;
	tri::params_from_env(base);
	const deck3::Deck deck = deck3::from_env();
	const az::Net heur;   // 일기토 = 손평가 탐색 (3단계 망은 일기토를 모른다 — 결과만 목표로 쓴다)
	const az::Net *hn[2] = { &heur, &heur };
	tri::Reuse ru[2];
	std::vector<tri::Rec> out;
	long long snap_n[3] = {}, br_games = 0, br_kind[3] = {}, br_recs = 0;
	long long moves = 0, nodes = 0, goals = 0, shots = 0, duels = 0, ko_games = 0, lg_games = 0, lg_win = 0, lg_loss = 0, tt_hit = 0, flipped = 0;
	int home = 0, away = 0, draws = 0;
	std::map<int, long long> atk_len, siege_len;   // 측정: 차례 수 → 번 수
	Snap pool[3][1];
	Rng rb(seed ^ 0x5A5A5A5AA5A5A5A5ull);
	for (int g = 0; g < games; g++) {
		Rng rng(seed * 1000003ull + g);
		Rng rf((seed * 1000003ull + g) ^ 0x6969696996969696ull);
		const bool ko = rng.unit() < ko_frac;
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
		new_match(s, AWAY, 0, 0);
		s.sk_passive = 1;
		if (rng.unit() < 0.35)
			for (int t = 0; t < 2; t++)
				az::random_lineup(s, t, rng.next());
		for (int t = 0; t < 2; t++) {
			const double u = rng.unit();
			deck3::fill(s, t, deck, rng.next(), u < 0.15 ? 1.0 : u < 0.25 ? 0.0 : 0.6);
		}
		if (bsrc >= 0) {
			s = pool[bsrc][0].s;
			br_games++, br_kind[bsrc]++;
		}
		ko_games += ko;
		lg_games += vs_league;
		ru[0].clear(), ru[1].clear();
		const size_t r0 = out.size();
		std::vector<int> mover;
		std::vector<float> trk, qv;
		std::vector<size_t> pend;
		std::vector<State> hist;   // 기록한 수의 국면 (판 중간 시작 후보 고르기)
		std::vector<int> goal_before;   // 골 직전 국면 표시
		int guard = 0, ply = 0;
		const uint32_t gid = uint32_t(seed * 100003ull + g);
		// 측정: 공격 (공 가진 팀이 바뀐 차례 경계 수) · 골문 공략 (그 팀이 골문을 처음 깎은 뒤 골까지)
		int atk_team = -1, atk_turns = 0, siege_turns[2] = { -1, -1 }, last_turn_team = s.turn_team;
		while (s.phase != GAME_OVER && guard++ < 8000) {
			const bool full = rng.unit() < full_frac;
			const int sims = full ? full_sims : fast_sims;
			const bool mine = !vs_league || s.turn_team == learner;
			Action a;
			std::vector<cf::V2> apath;
			tri::Rec rec;
			bool have = false;
			if (teach) {
				az::Params p;
				p.sims = sims, p.seed = rng.next(), p.dirichlet_eps = 0.25, p.top_m = 16;
				az::Result r = az::search(s, fnet, p);
				if (!r.found) {
					end_turn(s);
					continue;
				}
				a = r.action;
				if (ply < 12) {
					double u = rng.unit();
					for (size_t i = 0; i < r.acts.size(); i++)
						if ((u -= r.policy[i]) <= 0.0) {
							a = r.acts[i];
							break;
						}
				}
				const bool fl = rf.unit() < flip_frac;
				fill(rec, s, r.acts, r.policy, fl);
				flipped += fl;
				rec.q = r.root_v, have = true;
			} else {
				tri::Params p = base;
				p.sims = sims, p.seed = rng.next(), p.dirichlet_eps = mine ? 0.25 : 0.0, p.temp = ply < 12 ? 1.0 : 0.0;
				const tri::Net &net = mine ? tnet : onet;
				tri::Result r = tri::search(s, net, p, reuse_on ? &ru[&net == &tnet ? 0 : 1] : nullptr);
				tt_hit += r.tt_hit;
				if (!r.found) {
					end_turn(s);
					continue;
				}
				a = r.action, apath = r.path;
				if (mine) {
					const bool fl = rf.unit() < flip_frac;
					fill(rec, s, r.acts, r.joint, fl);
					std::memcpy(rec.who, r.who, sizeof(rec.who));   // 1단계 분포는 1단계 마디 방문 그대로
					flipped += fl;
					rec.q = r.root_v, have = true;
				}
			}
			nodes += sims;
			if (have) {
				rec.z = rec.aux = 0.0f, rec.game = gid, rec.use_pi = full;
				out.push_back(rec);
				mover.push_back(s.turn_team);
				trk.push_back(0.0f);
				qv.push_back(rec.q);
				pend.push_back(mover.size() - 1);
				hist.push_back(s);
				br_recs += bsrc >= 0;
			}
			shots += a.t == A_SHOT;
			const int b0 = s.phase == cf::PLAY ? s.ball_team() : -1, gh0 = s.goals[HOME], ga0 = s.goals[AWAY];
			const int bk0 = s.blocks[0] + s.blocks[1], shooter = s.turn_team, kp0 = a.t == A_SHOT ? s.keeper[cf::opponent(shooter)] : 0;
			if (!apath.empty())
				g_path = &apath;
			apply_action(s, a);
			g_path = nullptr;
			ply++;
			const bool scored = s.goals[HOME] != gh0 || s.goals[AWAY] != ga0, blocked = s.blocks[0] + s.blocks[1] != bk0;
			int gain = -1;
			float mag = 0.5f;
			if (scored)
				gain = s.goals[HOME] != gh0 ? HOME : AWAY, mag = 1.0f;
			else if (blocked && kp0 > 0)
				gain = shooter, mag = float(std::min(1.0, double(s.last_dmg) / kp0));
			else if (s.phase == cf::PLAY && b0 >= 0 && s.ball_team() >= 0 && s.ball_team() != b0)
				gain = s.ball_team();
			if (gain >= 0) {
				for (size_t q : pend)
					trk[q] = mover[q] == gain ? mag : -mag;
				pend.clear();
			}
			// 판 중간 시작 후보: 골 직전(골 난 수 앞 2수) · 공 뺏긴 직후
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
			// 측정
			if (s.turn_team != last_turn_team) {
				last_turn_team = s.turn_team;
				if (atk_team >= 0)
					atk_turns++;
				for (int t = 0; t < 2; t++)
					if (siege_turns[t] >= 0)
						siege_turns[t]++;
			}
			const int bt = s.phase == cf::PLAY ? s.ball_team() : -1;
			if (scored || blocked || (b0 >= 0 && bt != b0)) {
				if (atk_team >= 0)
					atk_len[atk_turns]++;
				atk_team = -1;
			}
			if (blocked && siege_turns[shooter] < 0)
				siege_turns[shooter] = 0;
			if (scored) {
				const int sc = s.goals[HOME] != gh0 ? HOME : AWAY;
				siege_len[std::max(0, siege_turns[sc])]++;   // 처음 깎은 슛이 바로 골이면 0
				siege_turns[0] = siege_turns[1] = -1;
			}
			if (bt >= 0 && atk_team < 0 && !scored)
				atk_team = bt, atk_turns = 0;
		}
		int win = s.phase == GAME_OVER ? s.winner : -1;
		const int win_match = win;
		if (s.phase == GAME_OVER && win == -1 && ko) {
			win = duel::resolve(s, hn, std::max(8, fast_sims), rng.next());
			duels++;
		}
		(win == HOME ? home : win == AWAY ? away : draws)++;
		if (vs_league)
			lg_win += win == learner, lg_loss += win != -1 && win != learner;
		goals += s.goals[0] + s.goals[1];
		moves += (long long)mover.size();
		(void)win_match;
		for (size_t i = 0; i < mover.size(); i++) {
			const double z = win == -1 ? 0.0 : (win == mover[i] ? 1.0 : -1.0);
			tri::Rec &r = out[r0 + i];
			r.z = float(0.7 * z + 0.3 * r.q), r.aux = trk[i];
			if (std::fabs(qv[i] - z) > miss_v && hist[i].phase == cf::PLAY && rb.next() % (unsigned long long)(++snap_n[0]) == 0)
				pool[0][0] = Snap{ hist[i], 0 };
		}
	}
	FILE *f = fopen(argv[1], "wb");
	if (!f)
		return 1;
	fwrite(out.data(), sizeof(tri::Rec), out.size(), f);
	fclose(f);
	long long rh = ru[0].hit + ru[1].hit, rm = ru[0].miss + ru[1].miss;
	auto hist_txt = [](const std::map<int, long long> &m) {
		std::string t;
		for (auto &e : m)
			t += std::to_string(e.first) + ":" + std::to_string(e.second) + "|";
		return t;
	};
	fprintf(stderr, "#len atk=%s siege=%s\n", hist_txt(atk_len).c_str(), hist_txt(siege_len).c_str());
	fprintf(stderr, "#gen3 games=%d home=%d away=%d draw=%d goals=%lld moves=%lld sims=%lld shots=%lld duels=%lld ko=%lld league=%lld lwin=%lld lloss=%lld "
			"reuse_hit=%lld reuse_miss=%lld tt=%lld flip=%lld branch=%lld bmiss=%lld bgoal=%lld bsteal=%lld brecs=%lld\n",
			games, home, away, draws, goals, moves, nodes, shots, duels, ko_games, lg_games, lg_win, lg_loss, rh, rm, tt_hit, flipped, br_games, br_kind[0], br_kind[1], br_kind[2], br_recs);
	return 0;
}
