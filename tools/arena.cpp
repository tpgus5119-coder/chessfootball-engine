// 알파제로 아레나 — 도전자 A vs 챔피언 B. 판마다 한 줄, 끝에 요약.
//   arena <A> <B> <판수> <탐색횟수> <시드>
//   A·B = 망 파일 | '-' (첫 세대: 선형 가치 + 균일 정책으로 탐색) | 'mm3' (지금 게임 AI — 손평가 미니맥스 깊이 3. mm2도 됨)
// 탐색은 잡음 없이(결정론) 둔다. 봇이 결정론이라 같은 개막이면 같은 판 — 그래서 처음 두 번의 드래프트만 무작위로
// 두고, 판마다 A가 HOME·AWAY, 선공·후공을 번갈아 맡는다 (tools/nn/arena.cpp와 같은 규약)
#include "../src/az.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace cf;

static const double LIN_W[6] = { 0.20, 0.30, 0.60, 0.15, 0.50, 0.05 };

struct Side {
	az::Net net;
	int mm_depth = 0;   // >0 이면 미니맥스
	bool ok(const char *spec) {
		if (strncmp(spec, "mm", 2) == 0) {
			mm_depth = atoi(spec + 2);
			return mm_depth > 0;
		}
		if (strcmp(spec, "-") == 0)
			return true;
		return net.load(spec);
	}
	bool pick(const State &s, az::Params &prm, Action &a) {
		if (mm_depth > 0) {
			bool found = false;
			g_net = nullptr;
			a = pick_action(s, mm_depth, LIN_W, found);
			return found;
		}
		az::Result r = az::search(s, net, prm);
		a = r.action;
		return r.found;
	}
};

int main(int argc, char **argv) {
	if (argc < 6) {
		fprintf(stderr, "arena <A> <B> <판수> <탐색횟수> <시드>\n");
		return 1;
	}
	Side A, B;
	if (!A.ok(argv[1]) || !B.ok(argv[2])) {
		fprintf(stderr, "망을 못 읽음\n");
		return 1;
	}
	int games = atoi(argv[3]);
	az::Params prm;
	prm.sims = atoi(argv[4]);
	prm.gumbel = false;
	unsigned long long seed = strtoull(argv[5], nullptr, 10);
	int aw = 0, bw = 0, dr = 0, goals = 0;
	for (int g = 0; g < games; g++) {
		unsigned long long rs = seed * 7919ull + g / 4;
		int a_team = (g % 2 == 0) ? HOME : AWAY;
		int first = (g / 2) % 2 == 0 ? HOME : AWAY;
		State s;
		s.subs_enabled = 1;
		new_open_match(s, first);
		for (int k = 0; k < 2 && s.phase == PLACEMENT; k++) {   // 무작위 개막 두 수
			std::vector<Action> opts;
			enumerate_actions(s, opts);
			rs ^= rs >> 12;
			rs ^= rs << 25;
			rs ^= rs >> 27;
			apply_action(s, opts[(rs * 0x2545F4914F6CDD1Dull) % opts.size()]);
		}
		int guard = 0;
		while (s.phase != GAME_OVER && guard++ < 6000) {
			Action a;
			prm.seed = seed * 31 + g * 7 + guard;
			bool found = (s.turn_team == a_team ? A : B).pick(s, prm, a);
			if (!found) {
				s.end_turn();
				continue;
			}
			apply_action(s, a);
		}
		int win = s.phase == GAME_OVER ? s.winner : -1;
		goals += s.score[0] + s.score[1];
		if (win == -1)
			dr++;
		else if (win == a_team)
			aw++;
		else
			bw++;
		printf("%d,%d,%d,%d,%d\n", g, a_team, first, win, s.score[0] * 10 + s.score[1]);
		fflush(stdout);
	}
	double n = double(games);
	double sc = (aw + 0.5 * dr) / n;
	double se = std::sqrt(std::max(sc * (1 - sc), 1e-9) / n);
	fprintf(stderr, "A 승 %d · B 승 %d · 무 %d | A 점수율 %.3f ±%.3f (95%%) | 경기당 골 %.2f\n", aw, bw, dr, sc, 1.96 * se,
			goals / n);
	return 0;
}
