// 알파제로 아레나 — 도전자 A vs 챔피언 B. 판마다 한 줄, 끝에 요약.
//   arena <A> <B> <판수> <탐색횟수> <시드> [무작위선발=0]   — 1이면 양팀 선발·배치 전부를 합법 수 중 무작위로 (조합 다양성 평가, 2026-10-27)
//   A·B = 망 파일 | '-' (첫 세대: 선형 가치 + 균일 정책으로 탐색) | 'mm3' (지금 게임 AI — 손평가 미니맥스 깊이 3. mm2도 됨)
// 탐색은 잡음 없이(결정론) 둔다. 봇이 결정론이라 같은 개막이면 같은 판 — 그래서 처음 두 번의 드래프트만 무작위로
// 두고, 판마다 A가 HOME·AWAY, 선공·후공을 번갈아 맡는다
//
// **기술 통계** (2026-10-27 — 사용자 "AI가 오프사이드 트랩·스크린·협동 수비·태세·박스·선발·조합을 마스터해야 한다"): 판마다
// 실제로 둔 수의 앞뒤 상태로 양쪽이 무엇을 얼마나 썼는지 센다. 경합·스크린만 엔진 관찰자(cf::Observer)로 받고, 나머지는
// 상태 비교로 센다 — 규칙 코드는 건드리지 않는다. 끝에 `#stats,A,…` `#stats,B,…` 두 줄(stdout)과 한국어 요약(stderr)
#include "../src/az.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace cf;


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
			a = pick_action(s, mm_depth, LIN_W, found);
			return found;
		}
		az::Result r = az::search(s, net, prm);
		a = r.action;
		return r.found;
	}
};

// ---------- 기술 통계 ----------

static const char *POS_KO[14] = { "유망주", "캐논", "", "트릭스터", "타이탄", "마커", "헌터", "부스터", "월", "", "스프린터", "", "마에스트로", "노마드" };

struct Stats {
	long long games = 0;
	long long stance = 0;          // 태세 켠 횟수
	long long zone_contests = 0;   // 존 태클·인터셉트가 실제로 벌어진 수 (이 쪽이 수비)
	long long coop_contests = 0;   // 그중 협동 +1이 붙은 수
	long long coop_tiles = 0;      // 협동 칸 세움: 상대 공격 차례가 시작될 때 협동 +1이 걸린 내 존 칸 수 (이 쪽이 수비 — 차례마다 합산)
	long long coop_avoid = 0;      // 협동 칸 피함: 드리블·패스·슛의 출발~도착 직사각형 안에 상대 협동 칸이 있는데 그 칸에서 경합 없이 끝낸 수 (이 쪽이 공격)
	long long manual_tackles = 0, tackle_wins = 0;
	long long screens = 0;         // 이 쪽이 공을 들고 아군으로 상대 존 칸을 가린 수 (경합 생략)
	long long offside_traps = 0;   // 상대 공격 차례 시작에 상대 선수 하나 이상이 오프사이드 위치인 차례 수 (이 쪽이 수비)
	long long long_balls = 0, long_received = 0;
	long long nomad_own = 0, nomad_enemy = 0;   // 노마드가 박스 안에 선 채 끝낸 차례
	long long boost_free = 0;      // 부스터 곁 아군이 체력 0으로 행동한 수
	long long mark_acts = 0;       // 이 쪽 마커에게 붙잡혀 범위 −1인 적이 행동한 수
	long long holds = 0;           // 캐논 버티기(hold 1) 상태로 상대 차례가 지나간 수
	long long shots = 0, goals = 0;
	long long draft[14] = {};      // 포지션 번호별 뽑힌 횟수
	long long attackers = 0;       // 캐논·트릭스터·스프린터·마에스트로 뽑힌 수 (판당 평균용)
};

struct Obs : Observer {
	Stats *st[2] = { nullptr, nullptr };   // 팀 번호(HOME/AWAY) → 그 판에서 그 팀을 맡은 쪽의 통계
	int cur_act = -1;
	bool coop_hit = false;   // 이번 액션에서 협동 +1 경합이 실제로 났는가 (coop_avoid 판정용)
	void contest(const State &, const Player &atk, const Player &def, int def_key, int coop_bonus, bool, V2) override {
		if (coop_bonus > 0)
			coop_hit = true;
		if (cur_act == A_TACKLE)
			return;   // 수동 태클은 상태 비교로 센다
		Stats &d = *st[def.team];
		d.zone_contests++;
		if (coop_bonus > 0)
			d.coop_contests++;
		(void)atk;
		(void)def_key;
	}
	void screen(const State &, const Player &p, V2) override { st[p.team]->screens++; }
};

static bool is_attacker(int pos) { return pos == CANNON || pos == TRICKSTER || pos == SPRINTER || pos == MAESTRO; }

// game_state.gd boosted — 우리 부스터(공 없음)와 상하좌우로 붙은 아군 (부스터 자신은 아님)
static bool boosted_at(const State &s, const Player &p) {
	if (p.position == BOOSTER)
		return false;
	for (const Player &m : s.players)
		if (m.team == p.team && m.position == BOOSTER && m.is_placed() && m.uid != s.ball_owner_uid && manhattan(m.pos, p.pos) == 1)
			return true;
	return false;
}

// ball_actions.gd offside_line / _is_offside (엔진 안 정의와 같다 — 읽기 전용 복제)
static int offside_line_of(const State &s, int team) {
	int fwd = forward_dir(team), best = -1, best_v = -999;
	for (const Player &q : s.players)
		if (q.team != team && q.is_placed() && q.pos.x * fwd > best_v) {
			best_v = q.pos.x * fwd;
			best = q.pos.x;
		}
	return best;
}
static bool offside_tile(const Player &passer, V2 tile, int line) {
	if (line < 0)
		return false;
	int fwd = forward_dir(passer.team);
	return !is_own_half(passer.team, tile) && tile.x * fwd > passer.pos.x * fwd && tile.x * fwd > line * fwd;
}

// 협동 칸 (읽기 전용 복제 — engine.cpp build_zone + coop): 수비팀 def_team 의 존 칸(태세·월 선수 상하좌우, 골대·몸 칸 제외) 중
// 그 칸 상하좌우에 **존 주인 말고** 태세·월 선수가 하나 더 있는 칸. 존 주인은 먼저 놓인 선수(placed_order)
static const Player *at_tile(const State &s, V2 t) {
	for (const Player &q : s.players)
		if (q.is_placed() && q.pos == t)
			return &q;
	return nullptr;
}
static int coop_tiles_of(const State &s, int def_team, bool out[NT]) {
	int zone[NT];
	for (int i = 0; i < NT; i++) {
		zone[i] = -1;
		out[i] = false;
	}
	const Player *wide[PER_TEAM];
	int nw = 0;
	for (int u = 0; u < PLAYERS; u++) {
		const Player &q = s.players[u];
		if (q.team == def_team && q.is_placed() && q.wide_zone())
			wide[nw++] = &q;
	}
	std::sort(wide, wide + nw, [](const Player *a, const Player *b) { return a->placed_order < b->placed_order; });
	for (int i = 0; i < nw; i++)
		for (const V2 &d : DIRS) {
			V2 t = wide[i]->pos + d;
			if (in_bounds(t) && !is_goal(t) && zone[idx(t)] == -1)
				zone[idx(t)] = wide[i]->uid;
		}
	int n = 0;
	for (int i = 0; i < NT; i++) {
		if (zone[i] < 0 || at_tile(s, V2{ i % W, i / W }) != nullptr)
			continue;   // 몸 칸은 경합이 날 수 없는 죽은 칸
		V2 t{ i % W, i / W };
		for (const V2 &d : DIRS) {
			const Player *q = in_bounds(t + d) ? at_tile(s, t + d) : nullptr;
			if (q && q->team == def_team && q->uid != zone[i] && q->wide_zone()) {
				out[i] = true;
				n++;
				break;
			}
		}
	}
	return n;
}

// 실제로 둔 한 수(또는 강제 턴 종료)의 앞뒤 상태로 센다. a == nullptr 이면 하네스가 end_turn 한 것. coop_hit = 이번 수에서 협동 경합이 났는가
static void tally(const State &before, const State &after, const Action *a, Stats *st[2], bool coop_hit = false) {
	if (a && before.phase == PLAY) {
		int team = before.turn_team;
		Stats &me = *st[team];
		const Player &p = before.players[a->uid < 0 || a->uid >= PLAYERS ? 0 : a->uid];
		bool acts = a->t == A_MOVE || a->t == A_DRIBBLE || a->t == A_TACKLE || a->t == A_PASS || a->t == A_SHOT || a->t == A_STANCE || a->t == A_PUSH;
		if (acts && a->uid >= 0 && a->uid < PLAYERS) {
			if (boosted_at(before, p))
				me.boost_free++;
			if (marked(before, p))
				st[opponent(team)]->mark_acts++;
		}
		switch (a->t) {
			case A_STANCE:
				me.stance++;
				break;
			case A_PASS:
				if (manhattan(p.pos, a->dest) >= LONG_PASS_MIN) {
					me.long_balls++;
					if (after.ball_team() == team)
						me.long_received++;
				}
				break;
			case A_SHOT:
				me.shots++;
				if (after.score[team] > before.score[team])
					me.goals++;
				break;
			case A_TACKLE:
				me.manual_tackles++;
				if (after.ball_owner_uid == a->uid)
					me.tackle_wins++;
				break;
		}
		// 협동 칸 피함 — 드리블·패스·슛: 출발 칸과 도착 칸이 만드는 직사각형(두 칸 사이의 모든 최단 길이 지나는 영역) 안에 상대 협동 칸이 있고,
		// 도착 칸이 그 칸이 아니며, 이 수에서 협동 경합이 안 났으면 「피해 갔다」로 센다 (근사 — 돌아간 길을 직접 보진 않는다)
		if ((a->t == A_DRIBBLE || a->t == A_PASS || a->t == A_SHOT) && a->uid >= 0 && a->uid < PLAYERS) {
			bool ct[NT];
			if (coop_tiles_of(before, opponent(team), ct) > 0 && !coop_hit) {
				int x0 = std::min(p.pos.x, a->dest.x), x1 = std::max(p.pos.x, a->dest.x);
				int y0 = std::min(p.pos.y, a->dest.y), y1 = std::max(p.pos.y, a->dest.y);
				bool inside = false;
				for (int x = x0; x <= x1 && !inside; x++)
					for (int y = y0; y <= y1; y++)
						if (ct[idx(V2{ x, y })] && !(x == a->dest.x && y == a->dest.y) && !(x == p.pos.x && y == p.pos.y)) {
							inside = true;
							break;
						}
				if (inside)
					me.coop_avoid++;
			}
		}
	}
	if (a && a->t == A_PLACE && before.phase == PLACEMENT && !before.redeploy) {   // 드래프트
		for (const Player &q : after.players)
			if (q.placed_order == before.placement_index && q.position >= 0 && q.position < 14) {
				Stats &me = *st[q.team];
				me.draft[q.position]++;
				if (is_attacker(q.position))
					me.attackers++;
			}
	}
	// 차례가 넘어갔다 — 끝낸 팀 E의 노마드·버티기, 새 팀 N에 대한 오프사이드 트랩
	if (before.phase == PLAY && after.turn_team != before.turn_team) {
		int e = before.turn_team;
		for (const Player &q : before.players)
			if (q.team == e && q.position == NOMAD && q.is_placed()) {
				if (is_box_of(e, q.pos))
					st[e]->nomad_own++;
				else if (is_box_of(opponent(e), q.pos))
					st[e]->nomad_enemy++;
			}
		if (before.hold == 1 && before.ball_owner_uid != -1 && before.players[before.ball_owner_uid].team != e)
			st[before.players[before.ball_owner_uid].team]->holds++;
		if (after.phase == PLAY) {
			int n = after.turn_team;
			{   // 협동 칸 세움 — 새 차례 팀 n 이 공격하는 쪽, 상대(수비)의 협동 칸 수를 수비 쪽에 쌓는다
				bool ct[NT];
				st[opponent(n)]->coop_tiles += coop_tiles_of(after, opponent(n), ct);
			}
			if (after.ball_owner_uid != -1 && after.players[after.ball_owner_uid].team == n) {
				const Player &owner = after.players[after.ball_owner_uid];
				int line = offside_line_of(after, n);
				for (const Player &q : after.players)
					if (q.team == n && q.is_placed() && q.uid != owner.uid && offside_tile(owner, q.pos, line)) {
						st[opponent(n)]->offside_traps++;
						break;
					}
			}
		}
	}
}

static void print_stats(const char *name, const Stats &s) {
	double g = std::max<long long>(1, s.games);
	printf("#stats,%s,games=%lld,stance=%lld,zone=%lld,coop=%lld,coop_tiles=%lld,coop_avoid=%lld,tackle=%lld,tackle_win=%lld,screen=%lld,offside=%lld,"
			"long=%lld,long_ok=%lld,nomad_own=%lld,nomad_enemy=%lld,boost_free=%lld,mark=%lld,hold=%lld,shot=%lld,goal=%lld,attackers=%lld,draft=",
			name, s.games, s.stance, s.zone_contests, s.coop_contests, s.coop_tiles, s.coop_avoid, s.manual_tackles, s.tackle_wins, s.screens, s.offside_traps,
			s.long_balls, s.long_received, s.nomad_own, s.nomad_enemy, s.boost_free, s.mark_acts, s.holds, s.shots, s.goals, s.attackers);
	bool first = true;
	for (int i = 0; i < 14; i++)
		if (s.draft[i]) {
			printf("%s%d:%lld", first ? "" : "|", i, s.draft[i]);
			first = false;
		}
	printf("\n");
	fprintf(stderr, "  %s 기술(판당): 태세 %.2f · 존 경합 %.2f(협동 %.2f) · 협동 칸 세움 %.2f(차례마다 합) · 협동 칸 피함 %.2f(출발~도착 직사각형 안에 상대 협동 칸이 있는데 거기서 경합 없이 끝낸 드리블·패스·슛) · "
			"수동 태클 %.2f(성공 %.2f) · 스크린 %.2f · 오프사이드 트랩 %.2f · 롱볼 %.2f(받음 %.2f) · "
			"노마드 박스 %.2f/%.2f · 부스터 공짜 %.2f · 마커 잡음 %.2f · 버티기 %.2f · 슛 %.2f(골 %.2f) · 공격수 %.2f명\n",
			name, s.stance / g, s.zone_contests / g, s.coop_contests / g, s.coop_tiles / g, s.coop_avoid / g, s.manual_tackles / g, s.tackle_wins / g, s.screens / g,
			s.offside_traps / g, s.long_balls / g, s.long_received / g, s.nomad_own / g, s.nomad_enemy / g, s.boost_free / g,
			s.mark_acts / g, s.holds / g, s.shots / g, s.goals / g, s.attackers / g);
	fprintf(stderr, "  %s 드래프트:", name);
	for (int i = 0; i < 14; i++)
		if (s.draft[i])
			fprintf(stderr, " %s %lld", POS_KO[i], s.draft[i]);
	fprintf(stderr, "\n");
}

int main(int argc, char **argv) {
	if (argc < 6) {
		fprintf(stderr, "arena <A> <B> <판수> <탐색횟수> <시드> [무작위선발=0]\n");
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
	const bool rand_draft = argc > 6 && atoi(argv[6]) != 0;
	int aw = 0, bw = 0, dr = 0, goals = 0;
	Stats sa, sb;
	Obs obs;
	for (int g = 0; g < games; g++) {
		unsigned long long rs = seed * 7919ull + g / 4;
		int a_team = (g % 2 == 0) ? HOME : AWAY;
		int first = (g / 2) % 2 == 0 ? HOME : AWAY;
		Stats *st[2];
		st[a_team] = &sa;
		st[opponent(a_team)] = &sb;
		obs.st[0] = st[0];
		obs.st[1] = st[1];
		State s;
		new_open_match(s, first);
		for (int k = 0; (k < 2 || rand_draft) && s.phase == PLACEMENT; k++) {   // 무작위 개막 두 수 (무작위선발이면 배치 전부)
			std::vector<Action> opts;
			enumerate_actions(s, opts);
			rs ^= rs >> 12;
			rs ^= rs << 25;
			rs ^= rs >> 27;
			Action a = opts[(rs * 0x2545F4914F6CDD1Dull) % opts.size()];
			State before = s;
			apply_action(s, a);
			tally(before, s, &a, st);   // 무작위 개막 수도 드래프트 분포에 센다
		}
		int guard = 0;
		while (s.phase != GAME_OVER && guard++ < 6000) {
			Action a;
			prm.seed = seed * 31 + g * 7 + guard;
			bool found = (s.turn_team == a_team ? A : B).pick(s, prm, a);
			State before = s;
			if (!found) {
				s.end_turn();
				tally(before, s, nullptr, st);
				continue;
			}
			obs.cur_act = a.t;
			obs.coop_hit = false;
			g_observer = &obs;   // 실제로 두는 apply 둘레에서만 — 탐색은 pick 안에서 끝났다
			apply_action(s, a);
			g_observer = nullptr;
			tally(before, s, &a, st, obs.coop_hit);
		}
		sa.games++;
		sb.games++;
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
	print_stats("A", sa);
	print_stats("B", sb);
	return 0;
}
