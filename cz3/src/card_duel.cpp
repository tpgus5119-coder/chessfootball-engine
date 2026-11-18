#include "card_duel.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace cf {
namespace card {
namespace duel {

static void auto_pick(Duel &d);

// 카드 힘 (card.cpp eff_stat 그대로 — 숨은 숫자를 세울 때 쓴다)
static int eff(const State &s, const Player &p, int type, int v) {
	return card_eff(s, p, type, v);
}

// 공격 하나를 세운다: 두 선수만 판에 — 둘 다 자기 박스 바로 앞(공격수가 공을 갖는다)
static void begin_attack(Duel &d, int team) {
	const int atk = d.pick[team], def = d.pick[opponent(team)];
	State s = d.base;
	for (Player &p : s.players) {
		p.pos = UNPLACED;
		p.placed_order = -1;
		p.stance = p.stance_left = p.bound = 0;
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
		s.hand[t] = FULL_HAND;
		s.combo[t] = s.combo_last[t] = 0;
		s.adv_next[t] = s.dbl[t] = s.carry[t] = s.lock[t] = 0;
		s.tac[t] = 0;
		s.last_card[t] = -1;
		s.goals[t] = 0;
		s.lost[t] = 0;
		s.keeper[t] = 0;   // 일기토엔 골문 체력이 없다 — 3차례 안에 쏘아 넣기만 하면 골 (v11)
	}
	for (V2 &t : s.left_from)
		t = UNSET;
	for (int t = 0; t < 2; t++) {   // 일기토에서는 스킬을 끈다 — 기본 카드 숫자만 (사용자 2026-11-07)
		std::fill(s.sk_atk[t], s.sk_atk[t] + N_CARD, 0);
		std::fill(s.sk_def[t], s.sk_def[t] + 3, 0);
		s.rb_kick[t] = s.rb_move[t] = s.poss_now[t] = s.lost_pending[t] = s.lost_now[t] = 0;
	}
	for (Player &q : s.players)
		q.stance_sk = 0;
	s.half_dmg = 0;
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
	for (const Action &a : all)
		if (a.t != A_TACTIC)
			out.push_back(a);
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
	if ((d.stage != 1 && d.stage != 2) || a.t == A_TACTIC)
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


// 상대의 숨은 것을 세운 판들 — 손에 남은 카드(종류별 장수만 안다), 태세 힘, 공 임자의 버티는 힘
static void worlds(const Duel &d, int me, std::vector<State> &out) {
	const State &s = d.s;
	const int o = opponent(me);
	const int nk = __builtin_popcount(unsigned(s.hand[o] & 0x07)), nm = __builtin_popcount(unsigned(s.hand[o] & 0x38));
	int ostance = -1, oball = -1;
	for (const Player &p : s.players)
		if (p.team == o && p.placed()) {
			if (p.stance > 0 && !d.stance_shown[p.uid])
				ostance = p.uid;
			if (s.ball_owner == p.uid && !d.hold_shown)
				oball = p.uid;
		}
	out.clear();
	for (int g1 = 0; g1 < 8; g1++) {
		if (__builtin_popcount(unsigned(g1)) != nk)
			continue;
		for (int g2 = 0; g2 < 8; g2++) {
			if (__builtin_popcount(unsigned(g2)) != nm)
				continue;
			State w = s;
			w.hand[o] = g1 | (g2 << 3);
			const int played = FULL_HAND & ~w.hand[o];
			std::vector<int> st_vals, hold_vals;
			for (int c = 0; c < N_CARD; c++)
				if ((played >> c) & 1) {
					int ty, v;
					card_info(w, o, c, ty, v);
					if (ostance >= 0 && ty == C_DEF)
						st_vals.push_back(eff(w, w.players[ostance], ty, v));
					if (oball >= 0 && ty != C_DEF)
						hold_vals.push_back(eff(w, w.players[oball], ty, v));
				}
			if (st_vals.empty())
				st_vals.push_back(ostance >= 0 ? s.players[ostance].stance : 0);
			if (hold_vals.empty())
				hold_vals.push_back(s.hold);
			for (int sv : st_vals)
				for (int hv : hold_vals) {
					State x = w;
					if (ostance >= 0)
						x.players[ostance].stance = std::max(1, sv);
					if (oball >= 0)
						x.hold = hv;
					out.push_back(x);
				}
		}
	}
	if (out.empty())
		out.push_back(s);
}

bool ai_act(const Duel &d, const az::Net &net, int sims, unsigned long long seed, Action &out,
		std::vector<Action> *acts_out, std::vector<float> *pol_out) {
	if (d.stage != 1 && d.stage != 2)
		return false;
	const int me = d.s.turn_team;
	std::vector<State> ws;
	worlds(d, me, ws);
	az::Params prm;
	prm.sims = std::max(8, sims);
	prm.gumbel = false;
	std::map<std::tuple<int, int, int, int, int>, float> score;
	std::map<std::tuple<int, int, int, int, int>, Action> pick_of;
	int k = 0;
	for (const State &w : ws) {
		prm.seed = seed + 7919ull * unsigned(k++);
		az::Result r = az::search(w, net, prm);
		if (!r.found)
			continue;
		for (size_t i = 0; i < r.acts.size() && i < r.policy.size(); i++) {
			const Action &a = r.acts[i];
			if (a.t == A_TACTIC)
				continue;
			auto key = std::make_tuple(a.t, a.uid, a.dest.x, a.dest.y, a.card);
			score[key] += r.policy[i];
			pick_of[key] = a;
		}
	}
	float best = -1.0f, sum = 0.0f;
	for (auto &kv : score) {
		sum += kv.second;
		if (kv.second > best) {
			best = kv.second;
			out = pick_of[kv.first];
		}
	}
	if (acts_out && pol_out) {
		acts_out->clear();
		pol_out->clear();
		for (auto &kv : score) {
			acts_out->push_back(pick_of[kv.first]);
			pol_out->push_back(sum > 0.0f ? kv.second / sum : 0.0f);
		}
	}
	return best >= 0.0f;
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
