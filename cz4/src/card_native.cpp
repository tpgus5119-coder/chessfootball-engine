#include "card_native.h"
#include "card_tactic.h"
#include "card_hash.h"

#include <godot_cpp/core/class_db.hpp>
#include <algorithm>
#include <cmath>
#include <godot_cpp/variant/vector2i.hpp>

using namespace godot;
namespace ck = cf::card;

static ck::az::Net g_card_net;   // 프로세스에 망 하나
// 화면과 주고받는 수: [종류, uid, x, y, 다섯째] 5칸 — 다섯째 = 뒷공간 패스(A_RUN)의 받을 동료 uid (11-21, 없으면 −1),
// 그 밖은 옛 스킬 칸(늘 0 — 11-19 직업 능력 상시)
static bool mate_act(int t) { return t == ck::A_RUN; }
static ck::Action dec_action(int t, int uid, int dx, int dy, int x5) {
	ck::Action a{ t, uid, { dx, dy }, -1 };
	if (mate_act(t))
		a.mate = x5;
	else
		a.sk = std::max(0, std::min(ck::SK_KINDS_MAX, x5));
	return a;
}
static void put_action(PackedInt32Array &out, const ck::Action &a) {
	out.push_back(a.t);
	out.push_back(a.uid);
	out.push_back(a.dest.x);
	out.push_back(a.dest.y);
	out.push_back(mate_act(a.t) ? a.mate : a.sk);
}

void CardNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("new_match", "first"), &CardNative::new_match);
	ClassDB::bind_method(D_METHOD("set_lineup", "team", "positions", "bonus"), &CardNative::set_lineup);
	ClassDB::bind_method(D_METHOD("set_perks", "team", "range_up", "hold_up", "kick_range_up"), &CardNative::set_perks, DEFVAL(PackedInt32Array()));
	ClassDB::bind_method(D_METHOD("set_trial", "team_n"), &CardNative::set_trial);
	ClassDB::bind_method(D_METHOD("get_state"), &CardNative::get_state);
	ClassDB::bind_method(D_METHOD("actions"), &CardNative::actions);
	ClassDB::bind_method(D_METHOD("apply", "t", "uid", "dx", "dy", "sk", "path"), &CardNative::apply, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("events"), &CardNative::events);
	ClassDB::bind_method(D_METHOD("preview", "t", "uid", "dx", "dy", "sk", "path"), &CardNative::preview, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("force_end_turn"), &CardNative::force_end_turn);
	ClassDB::bind_method(D_METHOD("dev_set", "uid", "x", "y", "stance"), &CardNative::dev_set);
	ClassDB::bind_method(D_METHOD("dev_ball", "owner", "x", "y", "turn_team"), &CardNative::dev_ball);
	ClassDB::bind_method(D_METHOD("dev_pos", "uid", "position"), &CardNative::dev_pos);
	ClassDB::bind_method(D_METHOD("dev_ghp", "team", "hp"), &CardNative::dev_ghp);
	ClassDB::bind_method(D_METHOD("act_range", "uid", "want", "sk"), &CardNative::act_range, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("jk_table"), &CardNative::jk_table);
	ClassDB::bind_method(D_METHOD("act_power", "uid", "use", "dist", "sk"), &CardNative::act_power, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("ai_load", "bytes"), &CardNative::ai_load);
	ClassDB::bind_method(D_METHOD("ai_ready"), &CardNative::ai_ready);
	ClassDB::bind_method(D_METHOD("ai_pick", "sims", "seed", "exclude_uid"), &CardNative::ai_pick, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("ai_style", "team", "poss", "ctr", "press", "hold", "jk"), &CardNative::ai_style, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("ai_tactic", "team", "tac"), &CardNative::ai_tactic);
	ClassDB::bind_method(D_METHOD("tac_slider", "tac", "i"), &CardNative::tac_slider);
	ClassDB::bind_method(D_METHOD("set_tac_slider", "tac", "i", "v"), &CardNative::set_tac_slider);
	ClassDB::bind_method(D_METHOD("duel_start", "viewer_team"), &CardNative::duel_start);
	ClassDB::bind_method(D_METHOD("duel_end"), &CardNative::duel_end);
	ClassDB::bind_method(D_METHOD("duel_set_viewer", "team"), &CardNative::duel_set_viewer);
	ClassDB::bind_method(D_METHOD("duel_info"), &CardNative::duel_info);
	ClassDB::bind_method(D_METHOD("state_hash"), &CardNative::state_hash);
}

void CardNative::new_match(int first) {
	ck::new_match(state, first);
	last_events.clear();
}

void CardNative::set_perks(int team, PackedInt32Array range_up, PackedInt32Array hold_up, PackedInt32Array kick_range_up) {
	if (team < 0 || team > 1)
		return;
	int r[cf::PER_TEAM] = {}, h[cf::PER_TEAM] = {}, k[cf::PER_TEAM] = {};
	for (int i = 0; i < cf::PER_TEAM; i++) {
		r[i] = i < range_up.size() ? int(range_up[i]) : 0;
		h[i] = i < hold_up.size() ? int(hold_up[i]) : 0;
		k[i] = i < kick_range_up.size() ? int(kick_range_up[i]) : 0;
	}
	ck::set_perks(state, team, r, h, k);
}

// 시험판 (11-18 혼자하기): 팀 인원 · 직업 스킬 패시브 — new_match 바로 뒤
void CardNative::set_trial(int team_n) { ck::set_trial(state, team_n); }

void CardNative::set_lineup(int team, PackedInt32Array positions, PackedInt32Array bonus) {
	if (team < 0 || team > 1 || positions.size() < cf::PER_TEAM)
		return;
	int pos[cf::PER_TEAM], b[cf::PER_TEAM][4] = {};
	for (int i = 0; i < cf::PER_TEAM; i++) {
		pos[i] = positions[i];
		for (int k = 0; k < 4; k++)
			b[i][k] = i * 4 + k < bonus.size() ? int(bonus[i * 4 + k]) : 0;
	}
	ck::set_lineup(state, team, pos, b);
}

static Array to_events(const std::vector<ck::Event> &ev) {
	Array out;
	for (const ck::Event &e : ev) {
		Dictionary d;
		d["kind"] = e.kind;
		d["a"] = e.a;
		d["b"] = e.b;
		d["at"] = Vector2i(e.at.x, e.at.y);
		d["va"] = e.va;
		d["vb"] = e.vb;
		d["win"] = e.win;
		d["vc"] = e.vc;   // 슛: 데미지 힘 (v17)
		Array p;
		for (const cf::V2 &t : e.path)
			p.push_back(Vector2i(t.x, t.y));
		d["path"] = p;
		out.push_back(d);
	}
	return out;
}

Dictionary CardNative::get_state() const {
	const ck::State &s = cur();
	Dictionary d;
	d["phase"] = s.phase;
	d["first_team"] = s.first_team;
	d["placement_index"] = s.placement_index;
	d["team_n"] = s.team_n;
	d["turn_team"] = s.turn_team;
	d["ball_owner"] = s.ball_owner;
	d["ball_pos"] = Vector2i(s.ball_pos.x, s.ball_pos.y);
	d["loose_team"] = s.loose_team;
	d["pts"] = s.pts;
	d["turn_count"] = s.turn_count;
	d["half_turn"] = s.half_turn();
	d["winner"] = s.winner;
	d["half"] = s.half;
	d["hold"] = eff_hold(s);
	d["touch_team"] = s.touch_team;
	d["touch_mask"] = s.touch_mask;
	d["redeploy"] = s.redeploy;
	d["opener"] = s.opener;
	d["kickoff"] = ck::kickoff_turn(s);
	d["ball_team"] = s.ball_team();
	d["jk"] = Array::make(ck::jokbo_mask(s, 0), ck::jokbo_mask(s, 1));            // 이번(다음) 차례에 이미 걸린 족보 비트 (card.h Jokbo 순서 · 한 차례 한 번)
	{
		Array jn;
		for (int t = 0; t < 2; t++) {
			Array a;
			for (int k = 0; k < ck::N_JK; k++)
				a.push_back(s.jk_count[t][k]);
			jn.push_back(a);
		}
		d["jk_n"] = jn;   // 이 판에 걸린 족보 수 (화면 — 이름 띄우기·결과표)
	}
	Array goals, role;
	for (int t = 0; t < 2; t++) {
		goals.push_back(s.goals[t]);
		role.push_back(s.role[t]);
	}
	d["goals"] = goals;
	Array ghp;
	for (int t = 0; t < 2; t++)
		ghp.push_back(s.ghp[t]);
	d["ghp"] = ghp;   // 골키퍼 남은 체력 (11-23)
	d["ghp_max"] = ck::g_goal_hp;
	d["role"] = role;
	Array ps;
	for (const ck::Player &p : s.players) {
		Dictionary q;
		q["uid"] = p.uid;
		q["team"] = p.team;
		q["position"] = p.position;
		q["charge"] = p.charge;   // 「강하게」 모음 2 (11-22 셋째)
		q["bonus"] = Array::make(p.bonus[0], p.bonus[1], p.bonus[2], p.bonus[3]);   // 컵 선수 스탯 슛·패스·드리블·수비
		q["pos"] = Vector2i(p.pos.x, p.pos.y);
		q["placed_order"] = p.placed_order;
		q["stance"] = p.stance;
		q["stance_left"] = p.stance_left;
		q["jk_up"] = p.jk_up;   // 족보 +1 토큰 (11-21)
		{   // 존 칸 (11-21 방향 존 — 공개): 곁 한 칸 · 월은 사방
			cf::V2 z[4];
			const int nz = ck::zone_tiles(p, z);
			Array za;
			for (int k = 0; k < nz; k++)
				za.push_back(Vector2i(z[k].x, z[k].y));
			q["zone"] = za;
			q["zone_dir"] = p.stance > 0 ? p.zdir : -1;
		}
		q["skill_job"] = false;   // 직업 능력 상시 (11-19) — 켜는 단추 없음
		q["skill_can"] = ck::skill_can(s, p);
		{
			Array r;
			for (int k = 0; k < ck::CARD_POOL_N; k++)
				r.push_back(s.rep[p.uid][k]);
			q["rep"] = r;
		}
		q["fired"] = s.fired[p.uid];
		q["acted"] = p.acted;   // 이번 차례에 행동함 (11-22 한 선수 행동 하나)
		q["move_stat"] = ck::move_stat(p);   // 이동 수치 (= 버티는 힘)
		q["marked"] = ck::marked(s, p);
		ps.push_back(q);
	}
	d["players"] = ps;
	return d;
}

PackedInt32Array CardNative::actions() const {
	std::vector<ck::Action> acts;
	if (in_duel)
		ck::duel::actions(duel, acts);
	else
		ck::enumerate_actions(state, acts);
	PackedInt32Array out;
	for (const ck::Action &a : acts)
		put_action(out, a);
	return out;
}

// 안 그린 길 (AI 수·탭): 결과가 다른 길 후보 중 망 가치가 가장 높은 길 (사용자 11-18 「길은 간 뒤의 상태로」). 망이 없으면 엔진 기본 길
static void pick_path(const ck::State &s, const ck::Action &a, std::vector<cf::V2> &drawn) {
	if (drawn.empty() && g_card_net.loaded())
		ck::value_path(s, a, ck::az::state_value, &g_card_net, drawn);
}

bool CardNative::apply(int t, int uid, int dx, int dy, int sk, const Array &path) {
	std::vector<ck::Event> ev;
	std::vector<cf::V2> drawn;
	for (int i = 0; i < path.size(); i++) {
		Vector2i v = path[i];
		drawn.push_back({ v.x, v.y });
	}
	ck::Action a = dec_action(t, uid, dx, dy, sk);
	if (!in_duel)
		pick_path(state, a, drawn);
	ck::g_events = &ev;
	ck::g_path = drawn.empty() ? nullptr : &drawn;
	bool ok;
	if (in_duel) {
		ck::g_events = nullptr;
		ok = ck::duel::apply(duel, a, &ev);
	} else {
		ok = ck::apply_action(state, a);
	}
	ck::g_path = nullptr;
	ck::g_events = nullptr;
	last_events = to_events(ev);
	return ok;
}

Array CardNative::preview(int t, int uid, int dx, int dy, int sk, const Array &path) const {
	ck::State c = cur();
	std::vector<ck::Event> ev;
	std::vector<cf::V2> drawn;   // 그린 길 그대로 (apply 와 같다 — 경합 미리보기가 실제와 같게)
	for (int i = 0; i < path.size(); i++) {
		Vector2i v = path[i];
		drawn.push_back({ v.x, v.y });
	}
	ck::Action a = dec_action(t, uid, dx, dy, sk);
	if (!in_duel)
		pick_path(c, a, drawn);
	ck::g_events = &ev;
	ck::g_path = drawn.empty() ? nullptr : &drawn;
	ck::apply_action(c, a);
	ck::g_path = nullptr;
	ck::g_events = nullptr;
	return to_events(ev);
}


void CardNative::force_end_turn() {
	if (in_duel)
		ck::duel::pass_turn(duel);
	else
		ck::end_turn(state);
}

// 그 선수의 범위 (want 0 킥 · 1 이동) — sk = 직업 스킬을 켰을 때 (엔진이 거르는 범위와 같다)
int CardNative::act_range(int uid, int want, int sk) const {
	if (uid < 0 || uid >= cf::PLAYERS)
		return 0;
	return ck::act_range(cur(), cur().players[uid], want == 0 ? ck::C_KICK : want == 3 ? ck::C_SHOT : ck::C_MOVE, sk);   // 3 = 슛 범위 (11-22 중거리 슈터)
}

// 그 선수의 판정 힘 (use = card.h Use: 0 패스 · 1 슛 · 2 이동 · 3 드리블 · 4 태세 · dist = 킥 거리)
int CardNative::act_power(int uid, int use, int dist, int sk) const {
	if (uid < 0 || uid >= cf::PLAYERS)
		return 0;
	ck::State c = cur();
	c.sk_uid = sk ? uid : -1;
	c.sk_kind = sk;
	return ck::act_power(c, c.players[uid], use, dist);
}

// 족보 표 [[이름, 켬] × 6] — 화면 이름 (card.cpp g_jk 한 곳 · 11-21 혜택은 모두 행동 포인트 +1)
Array CardNative::jk_table() const {
	Array out;
	for (int k = 0; k < ck::N_JK; k++)
		out.push_back(Array::make(String::utf8(ck::g_jk[k].name), ck::g_jk[k].on));
	return out;
}

bool CardNative::ai_load(const PackedByteArray &bytes) { return g_card_net.load_mem(bytes.ptr(), size_t(bytes.size())); }

bool CardNative::ai_ready() const { return g_card_net.loaded(); }

// 선발 AI 고르기 (사용자 2026-11-04 「겹쳐도 된다. 다만 완전 따라서 선발하지 말라」) — 규칙은 그대로, AI 고르는 법만:
// 늘 같은 다섯을 고르던 것(탐색 1등만 집음)을 탐색 확률대로 뽑아 섞고, 상대가 고른 포지션과는 둘까지만 겹친다
static ck::Action draft_vary(const ck::State &s, const ck::az::Result &r, unsigned long long seed) {
	int team = s.turn_team;
	int overlap = 0;
	bool opp_has[16] = {}, mine[16] = {};
	for (const auto &q : s.players)
		if (q.position >= 0 && q.position < 16)
			(q.team == team ? mine : opp_has)[q.position] = true;
	for (int k = 0; k < 16; k++)
		overlap += (mine[k] && opp_has[k]) ? 1 : 0;
	auto pos_of = [](int slot) { for (int k = 0; k < 16; k++) if (ck::pool_slot(k) == slot) return k; return -1; };
	std::vector<int> ok;
	double tot = 0.0;
	for (size_t i = 0; i < r.acts.size(); i++) {
		const ck::Action &a = r.acts[i];
		if (a.t != ck::A_PLACE)
			continue;
		int ps = pos_of(a.uid);
		if (ps >= 0 && opp_has[ps] && overlap >= 2)
			continue;
		ok.push_back(int(i));
		tot += std::sqrt(double(r.policy[i])) + 0.03;   // 1등에 몰린 확률을 펴서 판마다 다르게
	}
	if (ok.empty())
		return r.action;
	unsigned long long x = seed + 0x9E3779B97F4A7C15ULL;   // splitmix64 — 붙은 씨앗도 고르게 흩는다
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
	x ^= x >> 31;
	double u = double((x >> 11) & ((1ULL << 53) - 1)) / double(1ULL << 53) * tot;
	for (int i : ok) {
		u -= std::sqrt(double(r.policy[i])) + 0.03;
		if (u <= 0.0)
			return r.acts[i];
	}
	return r.acts[ok.back()];
}

void CardNative::ai_style(int team, float poss, float ctr, float press, float hold, float jk) {
	style.jk = jk;
	style_team = team;
	style.poss = poss;
	style.ctr = ctr;
	style.press = press;
	style.hold = hold;
}

void CardNative::ai_tactic(int team, int tac) {   // 전술 대본 (11-22, card_tactic.cpp): tac 0 게겐 · 1 티키 · 2 카테 · −1 끔 — 덤(ai_style)과 같은 팀에 건다
	style_team = team;
	style.tac = tac >= 0 && tac < ck::az::TAC_N ? tac : -1;
	style.eps = style.tac >= 0 ? ck::az::tac_eps(style.tac) : 0.0f;
}

float CardNative::tac_slider(int tac, int i) const { return ck::az::tac_slider(tac, i); }
void CardNative::set_tac_slider(int tac, int i, float v) {
	ck::az::set_tac_slider(tac, i, v);
	if (style.tac == tac)
		style.eps = ck::az::tac_eps(tac);
}

// exclude_uid >= 0 (선수 모드 동료 — 11-22 사용자 「선수 모드도 망 AI」): 그 선수(유저)의 수는 두지 않는다 —
// 탐색 결과가 그 선수 수면 개선 정책이 가장 높은 「다른 선수」 수를 고른다 (같은 탐색 · 같은 망)
PackedInt32Array CardNative::ai_pick(int sims, int seed, int exclude_uid) const {
	ck::az::Params p;
	p.sims = sims;
	p.seed = (unsigned long long)seed;
	p.gumbel = false;
	if (!g_card_net.loaded())
		p.top_m = 1 << 20;   // 망 없음(손평가): 수 점수가 모두 0 이라 상위 16 = 목록 앞 16개(첫 선수 이동)뿐이던 것 — 뿌리 수를 모두 본다 (docs/자가학습_감사_1119.md)
	p.style_team = style_team;
	p.style = style;
	PackedInt32Array out;
	if (in_duel) {
		ck::Action a;
		if (!ck::duel::ai_act(duel, g_card_net, std::max(8, sims / 4), (unsigned long long)seed, a))
			return out;
		put_action(out, a);
		return out;
	}
	ck::az::Result r = ck::az::search(state, g_card_net, p);
	if (!r.found)
		return out;
	if (state.phase == cf::PLACEMENT && !state.redeploy && r.action.t == ck::A_PLACE)
		r.action = draft_vary(state, r, (unsigned long long)seed);
	if (exclude_uid >= 0 && r.action.uid == exclude_uid) {   // 그 선수가 두는 수 × (스루패스로 뛰어가 받게 하는 건 됨 — 11-22 사용자)
		int best = -1;
		for (size_t i = 0; i < r.acts.size() && i < r.policy.size(); i++)
			if (r.acts[i].uid != exclude_uid && (best < 0 || r.policy[i] > r.policy[size_t(best)]))
				best = int(i);
		if (best < 0)
			return out;   // 유저 말고는 둘 수가 없다
		r.action = r.acts[size_t(best)];
	}
	put_action(out, r.action);
	return out;
}


void CardNative::duel_start(int viewer_team) {
	ck::duel::start(duel, state);
	in_duel = true;
	viewer = viewer_team;
	last_events.clear();
}

int CardNative::state_hash() const {
	return int(ck::state_hash(state, in_duel ? &duel : nullptr));
}

Dictionary CardNative::duel_info() const {
	Dictionary d;
	d["on"] = in_duel;
	d["stage"] = duel.stage;          // 1 홈 공격 · 2 원정 공격 · 3 끝 (v12 — 고르기 없음, 선발 순서대로)
	d["round"] = duel.round;
	d["pick"] = Array::make(duel.pick[0], duel.pick[1]);
	d["turns"] = Array::make(duel.turns[0], duel.turns[1]);
	d["last_turns"] = Array::make(duel.last_turns[0], duel.last_turns[1]);   // 막 끝난 라운드 결과 (다음 라운드로 넘어가면 turns는 지워진다)
	d["first"] = ck::duel::first_atk(duel);   // 이 라운드에 먼저 공격하는 팀
	d["atk_team"] = duel.atk_team;
	d["atk_turn"] = duel.atk_turn;
	d["winner"] = duel.winner;        // −2 진행 중 · −1 무승부
	d["max_rounds"] = ck::duel::MAX_ROUNDS;
	d["atk_turns"] = ck::duel::ATK_TURNS;
	return d;
}


// 개발용 판 짜기 (ui/_shot_pair_set.gd — 본편과 같은 배치로 대조 촬영). 규칙 판단은 하지 않는다 — 놓기만 한다
void CardNative::dev_set(int uid, int x, int y, int stance) {
	ck::State &s = cur();
	if (uid < 0 || uid >= cf::PLAYERS)
		return;
	s.players[uid].pos = cf::V2{ x, y };
	if (s.players[uid].placed_order < 0)
		s.players[uid].placed_order = uid;
	s.players[uid].stance = stance;
	s.players[uid].stance_left = stance > 0 ? 1 : 0;
	s.players[uid].jk_up = 0;   // 개발용 놓기 — 앞 국면의 족보 토큰은 지운다
	s.players[uid].acted = 0;   // 앞 국면의 「행동함」(11-22 한 선수 행동 하나)·「강하게」도
	s.players[uid].charge = 0;
	s.players[uid].zdir = ck::ZD_ALL;   // 개발용 — 사방 존
}

void CardNative::dev_ball(int owner, int x, int y, int turn_team) {
	ck::State &s = cur();
	s.phase = cf::PLAY;
	s.turn_team = turn_team;
	s.pts = cf::POINTS_PER_TURN;
	s.ball_owner = owner;
	s.ball_pos = owner >= 0 ? cf::UNPLACED : cf::V2{ x, y };
	s.loose_team = owner >= 0 ? -1 : turn_team;
	const int bt = s.ball_team();
	for (int t = 0; t < 2; t++)
		s.role[t] = t == bt ? ck::R_ATK : ck::R_DEF;
	s.hold = owner >= 0 ? ck::move_stat(s.players[owner]) : 0;
	ck::turn_clear(s);   // 개발용 판 — 지나간 칸·족보는 새로
}


void CardNative::dev_ghp(int team, int hp) {   // 개발·시험용 — 골키퍼 남은 체력 놓기 (11-23)
	if (team == 0 || team == 1)
		cur().ghp[team] = hp;
}

void CardNative::dev_pos(int uid, int position) {
	if (uid >= 0 && uid < cf::PLAYERS)
		cur().players[uid].position = position;
}
