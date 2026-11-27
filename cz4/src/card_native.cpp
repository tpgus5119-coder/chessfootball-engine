#include "card_native.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cmath>
#include <cstring>
#include <type_traits>

#include "card_hash.h"
#include "card_tactic.h"

using namespace godot;
namespace ck = cf::card;

static ck::az::Net g_card_net;   // 프로세스에 망 하나
// 화면과 주고받는 수: [종류, uid, x, y, 다섯째] 5칸 — 다섯째 = 옛 스킬 칸(늘 0 · 0 아닌 수는 거절)
static void put_action(PackedInt32Array &out, const ck::Action &a) {
	out.push_back(a.t);
	out.push_back(a.uid);
	out.push_back(a.dest.x);
	out.push_back(a.dest.y);
	out.push_back(0);
}

void CardNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("new_match", "first"), &CardNative::new_match);
	ClassDB::bind_method(D_METHOD("set_knockout", "on"), &CardNative::set_knockout);
	ClassDB::bind_method(D_METHOD("set_lineup", "team", "positions", "bonus", "gk"), &CardNative::set_lineup, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("set_trial", "team_n"), &CardNative::set_trial);
	ClassDB::bind_method(D_METHOD("set_stamina", "team", "stam"), &CardNative::set_stamina);
	ClassDB::bind_method(D_METHOD("choose_lineup", "team", "positions", "bonus", "stam", "opp_form", "forms", "sampled", "seed"), &CardNative::choose_lineup);
	ClassDB::bind_static_method("CardNative", D_METHOD("stat_info", "position", "bonus"), &CardNative::stat_info);
	ClassDB::bind_method(D_METHOD("dev_stam", "uid", "stam"), &CardNative::dev_stam);
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
	ClassDB::bind_static_method("CardNative", D_METHOD("keeper_stats", "position", "bonus"), &CardNative::keeper_stats);
	ClassDB::bind_method(D_METHOD("act_range", "uid", "want", "sk"), &CardNative::act_range, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("act_power", "uid", "use", "dist", "sk"), &CardNative::act_power, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("ai_load", "bytes"), &CardNative::ai_load);
	ClassDB::bind_method(D_METHOD("ai_ready"), &CardNative::ai_ready);
	ClassDB::bind_method(D_METHOD("ai_pick", "sims", "seed", "exclude_uid"), &CardNative::ai_pick, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("ai_style", "team", "poss", "ctr", "press", "hold"), &CardNative::ai_style);
	ClassDB::bind_method(D_METHOD("ai_tactic", "team", "tac"), &CardNative::ai_tactic);
	ClassDB::bind_method(D_METHOD("tac_slider", "tac", "i"), &CardNative::tac_slider);
	ClassDB::bind_method(D_METHOD("set_tac_slider", "tac", "i", "v"), &CardNative::set_tac_slider);
	ClassDB::bind_method(D_METHOD("state_hash"), &CardNative::state_hash);
	ClassDB::bind_method(D_METHOD("save_state"), &CardNative::save_state);
	ClassDB::bind_method(D_METHOD("load_state", "bytes"), &CardNative::load_state);
}

void CardNative::new_match(int first) {
	ck::new_match(state, first);
	last_events.clear();
}

void CardNative::set_knockout(bool on) {
	state.knockout = on ? 1 : 0;
}

// 시험판 (혼자하기): 팀 필드 인원 — new_match 바로 뒤
void CardNative::set_trial(int team_n) { ck::set_trial(state, team_n); }

void CardNative::set_lineup(int team, PackedInt32Array positions, PackedInt32Array bonus, int gk) {
	if (team < 0 || team > 1 || positions.size() < 1)
		return;
	int pos[cf::PER_TEAM], b[cf::PER_TEAM][ck::GROW_K] = {};
	const int w = bonus.size() >= cf::PER_TEAM * ck::GROW_K ? ck::GROW_K : bonus.size() > cf::PER_TEAM * 4 ? 6 : 4;   // 한 선수 칸 수 — 7 (11-26 묶음 8: 스탯 여섯 + ④) · 옛 6·4
	for (int i = 0; i < cf::PER_TEAM; i++) {
		pos[i] = i < positions.size() ? int(positions[i]) : int(cf::PROSPECT);   // 모자라면 신입생
		for (int k = 0; k < w; k++)
			b[i][k] = i < positions.size() && i * w + k < bonus.size() ? int(bonus[i * w + k]) : 0;
	}
	ck::set_lineup(state, team, pos, b, gk);
}

void CardNative::set_stamina(int team, PackedInt32Array stam) {
	if (team < 0 || team > 1)
		return;
	int st[cf::PER_TEAM];
	for (int i = 0; i < cf::PER_TEAM; i++)
		st[i] = i < stam.size() ? int(stam[i]) : 99;   // 모자라면 가득
	ck::set_stamina(state, team, st);
}

static ck::az::Form form_of(const PackedInt32Array &v, int at) {
	ck::az::Form f = ck::az::CAREER_FORMS[0];
	for (int i = 0; i < cf::FIELD_N && at + 2 * i + 1 < v.size(); i++)
		f.t[i] = cf::V2{ int(v[at + 2 * i]), int(v[at + 2 * i + 1]) };
	return f;
}

Dictionary CardNative::choose_lineup(int team, PackedInt32Array positions, PackedInt32Array bonus, PackedInt32Array stam, PackedInt32Array opp_form, PackedInt32Array forms, int sampled, int seed) {
	Dictionary out;
	const int n = std::min<int>(int(positions.size()), cf::PER_TEAM + 1);
	if (!g_card_net.loaded() || team < 0 || team > 1 || n < cf::PER_TEAM || state.phase != cf::PLACEMENT)
		return out;
	int pos[cf::PER_TEAM + 1], b[cf::PER_TEAM + 1][ck::GROW_K] = {}, st[cf::PER_TEAM + 1];
	for (int i = 0; i < n; i++) {
		pos[i] = int(positions[i]);
		for (int k = 0; k < ck::GROW_K; k++)
			b[i][k] = i * ck::GROW_K + k < bonus.size() ? int(bonus[i * ck::GROW_K + k]) : 0;
		st[i] = i < stam.size() ? int(stam[i]) : 99;
	}
	std::vector<ck::az::Form> fs;
	for (int at = 0; at + 2 * cf::FIELD_N <= forms.size(); at += 2 * cf::FIELD_N)
		fs.push_back(form_of(forms, at));
	const ck::az::LineupPick pk = ck::az::choose_lineup(state, team, n, pos, b, st, form_of(opp_form, 0), fs, std::max(0, sampled), (unsigned long long)(seed) * 2654435761ull + 1, g_card_net);
	if (pk.evals == 0)
		return out;
	PackedInt32Array sv, fv;
	for (int i = 0; i < cf::PER_TEAM; i++)
		sv.append(pk.starters[i]);
	for (int i = 0; i < cf::FIELD_N; i++)
		fv.append(pk.form.t[i].x), fv.append(pk.form.t[i].y);
	out["starters"] = sv;
	out["form"] = fv;
	out["v"] = pk.v;
	out["evals"] = pk.evals;
	return out;
}

PackedInt32Array CardNative::stat_info(int position, PackedInt32Array bonus) {
	ck::Player p;
	p.position = position;
	for (int k = 0; k < ck::GROW_K && k < bonus.size(); k++)
		p.bonus[k] = bonus[k];
	return PackedInt32Array({ ck::phys(p), ck::stam_max(p), ck::rest_gain(position) });
}

void CardNative::dev_stam(int uid, int stam) {
	if (uid >= 0 && uid < cf::PLAYERS) {
		ck::Player &p = state.players[uid];
		p.fat = ck::stam_max(p) - std::max(0, std::min(ck::stam_max(p), stam));
	}
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
		Array p;
		for (const cf::V2 &t : e.path)
			p.push_back(Vector2i(t.x, t.y));
		d["path"] = p;
		out.push_back(d);
	}
	return out;
}

Dictionary CardNative::get_state() const {
	const ck::State &s = state;
	Dictionary d;
	d["phase"] = s.phase;
	d["first_team"] = s.first_team;
	d["placement_index"] = s.placement_index;
	d["team_n"] = s.team_n;
	d["turn_team"] = s.turn_team;
	d["ball_owner"] = s.ball_owner;
	d["pts"] = s.pts;
	d["turn_count"] = s.turn_count;
	d["half_turn"] = s.turn_count;   // 화면 이름 그대로 (전후반 없음 — 경기 차례 수)
	d["knockout"] = s.knockout;   // 단판 토너먼트 (비기면 연장)
	d["ot"] = s.ot;               // 1 = 연장 중 (차례 제한 없는 골든골 — 남은 차례 없음)
	d["winner"] = s.winner;
	d["half"] = 1;   // 화면 호환 (전후반 없음)
	d["hold"] = s.hold;
	d["touch_team"] = s.touch_team;
	d["touch_mask"] = s.touch_mask;
	d["redeploy"] = s.redeploy;
	d["opener"] = s.opener;
	d["kickoff"] = ck::kickoff_turn(s);
	d["ball_team"] = s.ball_team();
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
	d["ghp_eff"] = Array::make(ck::goal_hp(s, 0), ck::goal_hp(s, 1));   // 슛 판정 골대 체력 (조커 키퍼 가득 +1 — 11-27 묶음 9)
	Array gmax, gkick, grange;
	for (int t = 0; t < 2; t++) {
		gmax.push_back(s.gk_def[t]);
		gkick.push_back(s.gk_kick[t]);
		grange.push_back(s.gk_range[t]);
	}
	d["ghp_max"] = gmax;   // 골키퍼 디펜스 = 체력 최대 [홈, 원정]
	d["gk"] = Array::make(s.gk[0], s.gk[1]);   // 골키퍼 uid [홈, 원정]
	d["gk_in"] = Array::make(s.gk_in(0), s.gk_in(1));   // 골키퍼가 골대 안 (밖이면 골대 체력 0)
	d["gk_kick"] = gkick;
	d["gk_range"] = grange;
	d["keeper"] = s.keeper;   // 슛을 막은 키퍼가 공을 쥔 팀 (−1 = 없음) — 화면의 키퍼 롱킥 차례 표시
	d["role"] = role;
	Array ps;
	for (const ck::Player &p : s.players) {
		Dictionary q;
		q["uid"] = p.uid;
		q["team"] = p.team;
		q["position"] = p.position;
		q["tai"] = p.tai;   // 태세 1 = 태세 중 (존·버티기)
		q["st_ord"] = p.st_ord;
		q["bonus"] = Array::make(p.bonus[0], p.bonus[1], p.bonus[2], p.bonus[3], p.bonus[4], p.bonus[5], p.bonus[6]);   // 성장 슛·패스·무브·디펜스·피지컬·체력 · 2차 성장
		q["spec_lv"] = ck::spec_lv(p);   // 전문 단계 0·1·2
		q["phys"] = ck::phys(p);   // 피지컬 (11-25)
		q["stam"] = ck::stam(p);   // 남은 체력
		q["stam_max"] = ck::stam_max(p);
		q["pos"] = Vector2i(p.pos.x, p.pos.y);
		q["placed_order"] = p.placed_order;
		q["stance"] = p.stance;
		q["stance_left"] = p.stance_left;
		cf::V2 z[4];
		const int nz = ck::zone_tiles(p, z);
		Array za;
		for (int k = 0; k < nz; k++)
			za.push_back(Vector2i(z[k].x, z[k].y));
		q["zone"] = za;   // 존 칸 (곁 네 칸)
		Array r;
		for (int k = 0; k < ck::CARD_POOL_N; k++)
			r.push_back(s.rep[p.uid][k]);
		q["rep"] = r;
		q["acted"] = p.acted;   // 이번 차례에 행동함
		q["move_stat"] = ck::move_stat(p);   // 이동 수치 (= 버티는 힘)
		q["marked"] = ck::marked(s, p);
		ps.push_back(q);
	}
	d["players"] = ps;
	return d;
}

PackedInt32Array CardNative::actions() const {
	std::vector<ck::Action> acts;
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
	if (sk > 0) {   // 옛 스킬 수 — 없다
		last_events.clear();
		return false;
	}
	const ck::Action a{ t, uid, { dx, dy } };
	pick_path(state, a, drawn);
	ck::g_events = &ev;
	ck::g_path = drawn.empty() ? nullptr : &drawn;
	const bool ok = ck::apply_action(state, a);
	ck::g_path = nullptr;
	ck::g_events = nullptr;
	last_events = to_events(ev);
	return ok;
}

Array CardNative::preview(int t, int uid, int dx, int dy, int sk, const Array &path) const {
	ck::State c = state;
	std::vector<ck::Event> ev;
	std::vector<cf::V2> drawn;   // 그린 길 그대로 (apply 와 같다 — 경합 미리보기가 실제와 같게)
	for (int i = 0; i < path.size(); i++) {
		Vector2i v = path[i];
		drawn.push_back({ v.x, v.y });
	}
	if (sk > 0)
		return Array();
	const ck::Action a{ t, uid, { dx, dy } };
	pick_path(c, a, drawn);
	ck::g_events = &ev;
	ck::g_path = drawn.empty() ? nullptr : &drawn;
	ck::apply_action(c, a);
	ck::g_path = nullptr;
	ck::g_events = nullptr;
	return to_events(ev);
}


void CardNative::force_end_turn() {
	ck::end_turn(state);
}

// 그 선수의 범위 (want 0 패스 · 1 이동 · 3 슛 — 엔진이 거르는 범위와 같다) · sk = 옛 스킬 칸 (안 씀)
int CardNative::act_range(int uid, int want, int sk) const {
	(void)sk;
	if (uid < 0 || uid >= cf::PLAYERS)
		return 0;
	return ck::act_range(state, state.players[uid], want == 0 ? ck::C_KICK : want == 3 ? ck::C_SHOT : ck::C_MOVE);
}

// 그 선수의 판정 힘 (use = card.h Use: 0 패스 · 1 슛 · 2 이동 · 3 드리블 · 4 태세 · dist = 킥 거리) · sk = 옛 스킬 칸 (안 씀)
int CardNative::act_power(int uid, int use, int dist, int sk) const {
	(void)sk;
	if (uid < 0 || uid >= cf::PLAYERS)
		return 0;
	return ck::act_power(state, state.players[uid], use, dist);
}


bool CardNative::ai_load(const PackedByteArray &bytes) { return g_card_net.load_mem(bytes.ptr(), size_t(bytes.size())); }

bool CardNative::ai_ready() const { return g_card_net.loaded(); }

// 드래프트 AI 고르기 (겹쳐도 되지만 상대를 완전히 따라 고르지는 않게) — 규칙은 그대로, AI 고르는 법만:
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

void CardNative::ai_style(int team, float poss, float ctr, float press, float hold) {
	style_team = team;
	style.poss = poss;
	style.ctr = ctr;
	style.press = press;
	style.hold = hold;
}

void CardNative::ai_tactic(int team, int tac) {   // 전술 대본 (card_tactic.cpp): tac 0 게겐 · 1 티키 · 2 카테 · −1 끔 — 덤(ai_style)과 같은 팀에 건다
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

// exclude_uid >= 0 (선수 모드 동료): 그 선수(유저)의 수는 두지 않는다 —
// 탐색 결과가 그 선수 수면 개선 정책이 가장 높은 「다른 선수」 수를 고른다 (같은 탐색 · 같은 망)
PackedInt32Array CardNative::ai_pick(int sims, int seed, int exclude_uid) const {
	ck::az::Params p;
	p.sims = sims;
	p.seed = (unsigned long long)seed;
	p.gumbel = false;
	if (!g_card_net.loaded())
		p.top_m = 1 << 20;   // 망 없음(손평가): 수 점수가 모두 0 이라 상위 16 = 목록 앞 16개뿐 — 뿌리 수를 모두 본다
	p.style_team = style_team;
	p.style = style;
	PackedInt32Array out;
	ck::az::Result r = ck::az::search(state, g_card_net, p);
	if (!r.found)
		return out;
	if (state.phase == cf::PLACEMENT && !state.redeploy && r.action.t == ck::A_PLACE)
		r.action = draft_vary(state, r, (unsigned long long)seed);
	if (exclude_uid >= 0 && r.action.uid == exclude_uid) {   // 그 선수가 두는 수 ×
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


static_assert(std::is_trivially_copyable<ck::State>::value, "자동 저장은 State 를 바이트로 옮긴다");
static_assert(std::has_unique_object_representations_v<ck::State>, "State 에 채움 바이트가 없어야 한다 (탐색이 바이트로 견준다)");
static constexpr uint32_t SAVE_MAGIC = 0x53564331u;   // "1CVS" — State 꼴이 바뀌면 크기로 걸러진다

PackedByteArray CardNative::save_state() const {
	PackedByteArray b;
	b.resize(int64_t(8 + sizeof(ck::State)));
	const uint32_t head[2] = { SAVE_MAGIC, uint32_t(sizeof(ck::State)) };
	memcpy(b.ptrw(), head, 8);
	memcpy(b.ptrw() + 8, &state, sizeof(ck::State));
	return b;
}

bool CardNative::load_state(const PackedByteArray &b) {
	if (b.size() != int64_t(8 + sizeof(ck::State)))
		return false;
	uint32_t head[2];
	memcpy(head, b.ptr(), 8);
	if (head[0] != SAVE_MAGIC || head[1] != sizeof(ck::State))
		return false;
	memcpy(&state, b.ptr() + 8, sizeof(ck::State));
	last_events.clear();
	return true;
}

int CardNative::state_hash() const {
	return int(ck::state_hash(state));
}

// 개발·시험용 판 짜기 (촬영·tests). 규칙 판단은 하지 않는다 — 놓기만 한다
void CardNative::dev_set(int uid, int x, int y, int stance) {
	ck::State &s = state;
	if (uid < 0 || uid >= cf::PLAYERS)
		return;
	s.players[uid].pos = cf::V2{ x, y };
	if (s.players[uid].placed_order < 0)
		s.players[uid].placed_order = uid;
	s.players[uid].stance = stance;
	s.players[uid].stance_left = stance > 0 ? 1 : 0;
	s.players[uid].acted = 0;   // 앞 국면의 「행동함」·「강하게」도
	s.players[uid].tai = 0;
}

void CardNative::dev_ball(int owner, int x, int y, int turn_team) {
	ck::State &s = state;
	s.phase = cf::PLAY;
	s.turn_team = turn_team;
	s.pts = cf::POINTS_PER_TURN;
	(void)x, (void)y;   // 옛 루즈볼 칸 자리 (화면 호환 — 안 씀)
	s.ball_owner = owner;
	s.keeper = -1;   // 판을 새로 짜면 키퍼가 쥔 공도 비운다
	const int bt = s.ball_team();
	for (int t = 0; t < 2; t++)
		s.role[t] = t == bt ? ck::R_ATK : ck::R_DEF;
	s.hold = owner >= 0 ? ck::move_stat(s.players[owner]) : 0;
}


PackedInt32Array CardNative::keeper_stats(int position, PackedInt32Array bonus) {   // 그 선수가 키퍼일 때 [디펜스, 롱킥 힘, 롱킥 범위]
	int b[ck::GROW_K] = {}, out[3];
	for (int k = 0; k < ck::GROW_K && k < bonus.size(); k++)
		b[k] = bonus[k];
	ck::keeper_stats(position, b, out);
	PackedInt32Array r;
	for (int v : out)
		r.push_back(v);
	return r;
}

void CardNative::dev_ghp(int team, int hp) {   // 개발·시험용 — 골대 남은 체력 놓기
	if (team == 0 || team == 1)
		state.ghp[team] = hp;
}

void CardNative::dev_pos(int uid, int position) {
	if (uid >= 0 && uid < cf::PLAYERS)
		state.players[uid].position = position;
}
