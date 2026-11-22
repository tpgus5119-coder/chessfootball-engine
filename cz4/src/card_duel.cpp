#include "card_duel.h"

#include <algorithm>

namespace cf {
namespace card {
namespace duel {

static void auto_pick(Duel &d);

// 공격 하나를 세운다: 두 선수만 판에 — 둘 다 자기 박스 바로 앞(공격수가 공을 갖는다)
static void begin_attack(Duel &d, int team) {
	const int atk = d.pick[team], def = d.pick[opponent(team)];
	State s = d.base;
	for (Player &p : s.players) {
		p.pos = UNPLACED;
		p.placed_order = -1;
		p.stance = p.stance_left = 0;
	}
	// 둘 다 **자기 박스 바로 앞 가운데 칸**에서 시작 — 홈 선수 (2,3) · 원정 선수 (W-3,3), 공격·수비 상관없이 고정 (사용자 2026-11-03 "각자 자기 박스 바로 앞칸")
	const bool home = team == HOME;
	s.players[atk].pos = V2{ home ? 2 : W - 3, H / 2 };
	s.players[atk].placed_order = 0;
	s.players[def].pos = V2{ home ? W - 3 : 2, H / 2 };
	s.players[def].placed_order = 1;
	s.phase = PLAY;
	s.first_team = opponent(team);
	s.turn_team = team;
	s.pts = POINTS_PER_TURN;
	s.ball_owner = atk;
	s.ball_pos = UNPLACED;
	s.loose_team = -1;
	s.hold = 0;
	s.touch_team = team;
	s.touch_mask = 1 << atk;
	s.role[team] = R_ATK;
	s.role[opponent(team)] = R_DEF;
	for (int t = 0; t < 2; t++) {
		s.goals[t] = 0;
	}
	turn_clear(s);   // 지나간 칸·족보 (11-21)
	s.half = 2;                    // 골이 나면 그 판은 끝(full_time) — 전반 판에서 열려도 다섯 명이 되살아나지 않게
	s.half_start = s.turn_count;   // 반 시계가 끼어들지 않게
	d.s = s;
	d.atk_team = team;
	d.atk_turn = 1;
	d.stage = team == HOME ? 1 : 2;
	for (bool &b : d.stance_shown)
		b = false;
	d.hold_shown = false;
}

static void judge(Duel &d) {
	const int h = d.turns[HOME], a = d.turns[AWAY];
	d.last_turns[0] = h;
	d.last_turns[1] = a;
	if ((h > 0) != (a > 0))
		d.winner = h > 0 ? HOME : AWAY;
	else if (h > 0 && h != a)
		d.winner = h < a ? HOME : AWAY;   // 더 적은 차례에 넣은 쪽
	if (d.winner != -2) {
		d.stage = 3;
		return;
	}
	d.round += 1;
	if (d.round >= MAX_ROUNDS) {
		d.winner = -1;
		d.stage = 3;
		return;
	}
	d.turns[0] = d.turns[1] = 0;
	auto_pick(d);
}

// 이 라운드에 먼저 공격하는 팀 — 라운드마다 번갈아 (v12.1 사용자 2026-11-03 "선공 후공 순서는 교대로" — 짝수 라운드 홈 먼저, 홀수 라운드 원정 먼저. 짝 밀기(G)는 거둠)
int first_atk(const Duel &d) {
	return d.round % 2 == 0 ? HOME : AWAY;
}

static void end_attack(Duel &d, bool scored) {
	d.turns[d.atk_team] = scored ? d.atk_turn : 0;
	if (d.atk_team == first_atk(d))
		begin_attack(d, opponent(d.atk_team));
	else
		judge(d);
}

// 라운드마다 각 팀이 **선발했던 순서대로** 한 명씩 저절로 나간다 — 다섯이 다 나가면 처음부터 (v12 사용자 2026-11-03 "일기토 선수는 선발 순서로").
// 선발 순서 = uid 순서 (드래프트가 팀 안에서 빈 uid를 앞에서부터 채운다 — card.cpp next_free_uid)
static void auto_pick(Duel &d) {
	for (int t = 0; t < 2; t++) {
		int order[PER_TEAM], n = 0;
		for (int u = 0; u < PLAYERS; u++)
			if (d.base.players[u].team == t && d.base.players[u].position >= 0)
				order[n++] = u;
		d.pick[t] = n > 0 ? order[d.round % n] : -1;
	}
	if (d.pick[0] < 0 || d.pick[1] < 0) {
		d.winner = -1;   // 나갈 선수가 없다 — 무승부로 끝
		d.stage = 3;
		return;
	}
	begin_attack(d, first_atk(d));
}

void start(Duel &d, const State &end_of_match) {
	d = Duel();
	d.base = end_of_match;
	d.base.phase = PLAY;
	auto_pick(d);
}

void actions(const Duel &d, std::vector<Action> &out) {
	out.clear();
	if (d.stage != 1 && d.stage != 2)
		return;
	std::vector<Action> all;
	enumerate_actions(d.s, all);
	out = all;
}

// 공격이 끝났나 — 골 · 수비가 공을 가짐 · 공격수 차례 3번이 지남
static void after(Duel &d, int goals_before) {
	if (d.s.goals[d.atk_team] > goals_before) {
		end_attack(d, true);
		return;
	}
	if (d.s.phase != PLAY || d.s.ball_team() == opponent(d.atk_team)) {
		end_attack(d, false);
		return;
	}
	if (d.s.turn_team != d.atk_team && d.atk_turn >= ATK_TURNS) {   // 공격수의 마지막 차례가 끝났다
		end_attack(d, false);
		return;
	}
}

static void count_turn(Duel &d, int team_before) {
	if (d.stage != 1 && d.stage != 2)
		return;
	if (team_before != d.atk_team && d.s.turn_team == d.atk_team)
		d.atk_turn += 1;
}

bool apply(Duel &d, const Action &a, std::vector<Event> *ev_out) {
	if (d.stage != 1 && d.stage != 2)
		return false;
	const int g0 = d.s.goals[d.atk_team], tb = d.s.turn_team;
	const int owner0 = d.s.ball_owner;
	std::vector<Event> ev;
	g_events = &ev;
	bool ok = apply_action(d.s, a);
	g_events = nullptr;
	if (ev_out)
		*ev_out = ev;
	if (!ok)
		return false;
	for (const Event &e : ev)
		if (e.kind == 0) {   // 경합 — 그 순간 두 숫자가 드러난다
			if (e.a >= 0 && e.a < PLAYERS)
				d.stance_shown[e.a] = true;
			if (e.b >= 0 && e.b < PLAYERS)
				d.stance_shown[e.b] = true;
			d.hold_shown = true;
		}
	if (a.t == A_STANCE && a.uid >= 0)
		d.stance_shown[a.uid] = false;   // 새 태세 — 다시 숨는다
	if (d.s.ball_owner != owner0 || a.t == A_DRIBBLE || a.t == A_PASS)
		d.hold_shown = false;            // 버티는 힘이 새 숫자로 바뀌었다
	count_turn(d, tb);
	after(d, g0);
	return true;
}

void pass_turn(Duel &d) {
	if (d.stage != 1 && d.stage != 2)
		return;
	const int g0 = d.s.goals[d.atk_team], tb = d.s.turn_team;
	end_turn(d.s);
	count_turn(d, tb);
	after(d, g0);
}


bool ai_act(const Duel &d, const az::Net &net, int sims, unsigned long long seed, Action &out,
		std::vector<Action> *acts_out, std::vector<float> *pol_out) {
	if (d.stage != 1 && d.stage != 2)
		return false;
	az::Params prm;   // 카드가 없어 숨은 숫자도 없다 (11-19) — 판 그대로 탐색
	prm.sims = std::max(8, sims);
	prm.gumbel = false;
	prm.seed = seed;
	az::Result r = az::search(d.s, net, prm);
	if (!r.found)
		return false;
	out = r.action;
	if (acts_out && pol_out) {
		*acts_out = r.acts;
		*pol_out = r.policy;
	}
	return true;
}

int resolve(const State &end_of_match, const az::Net *const net[2], int sims, unsigned long long seed,
		void (*on_move)(void *ctx, const Duel &d, int team, const std::vector<Action> &acts, const std::vector<float> &pol), void *ctx) {
	Duel d;
	start(d, end_of_match);
	std::vector<Action> acts;
	std::vector<float> pol;
	for (int guard = 0; d.winner == -2 && guard < 2000; guard++) {
		const int team = d.s.turn_team;
		Action a;
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		if (!ai_act(d, *net[team], sims, seed, a, &acts, &pol)) {
			pass_turn(d);
			continue;
		}
		if (on_move)
			on_move(ctx, d, team, acts, pol);
		if (!apply(d, a))
			pass_turn(d);
	}
	return d.winner == -2 ? -1 : d.winner;
}

} // namespace duel
} // namespace card
} // namespace cf
