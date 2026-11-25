// 전술 대본 3판 (11-22 사용자 「막대는 유저가 직접 조절한다고 상상 — 중복·우리 게임에 없는 것은 없애고 단순하게」)
// 설계 = docs/전술_대본_2026-11-22.md — 막대 13개(공 가진 선수 4 · 공 없는 선수 4 · 달려드는 선수 2 · 자리 지키는 선수 3 — 5판, 전술끼리 3 이상 벌어짐) · 국면(지공·역습·역압박·수비) · 직업 역할표.
// 규칙은 하나도 바꾸지 않는다(AI 버릇만). card_az.cpp search() 뿌리가 g_tac_score 로 부른다:
// 대본 점수가 높은 수 넷은 꼭 탐색 · 값이 최선 − ε 안인 수 중 점수가 가장 높은 수를 둔다 · 이기는 쪽 슛은 가로채지 않는다(보이면 슛).
#include <algorithm>
#include <cmath>

#include "card_az.h"
#include "card_tactic.h"

namespace cf {
namespace card {
namespace az {

namespace {

// ---------- 막대 기본값 (문서 3절 5판 — 번호 1~13) ----------
// 사용자 11-22 「공 있을 때 공 있는 선수·공 없는 선수, 공 없을 때도 따로 나눠 세분화」 「상중하 말고 숫자로 · 전술끼리 분명히 나뉘게 · 남는 수비 0·1·2 명확히 · 공격 태세 없애」
// — 값 0~10, 막대마다 세 전술이 3 이상 벌어진다(높은·가운데·낮은 쪽을 하나씩). 7·8·9 는 수 그대로(칸·명).
// 공 가진 선수   1 패스↔드리블(0 드리블 · 10 늘 패스)  2 앞뒤(0 옆·뒤로 돌림 · 10 늘 앞으로)  3 중앙·측면(0 가운데 · 10 측면)  4 길이(0 짧게 1칸 · 10 길게 5칸)
// 공 없는 선수   5 받으러↔침투(0 받으러 옴 · 10 앞으로 침투)  6 움직이는 방향(0 가운데 · 10 측면)  7 받으러 갈 거리(1·2·3칸)  8 남는 수비(0·1·2명)
// 달려드는 선수 9 인원(0·1·2명, 막 뺏기면 +1 — 역압박)  10 태클 세기(0 따라만 감 · 10 붙어서 태클·「강하게」 다가가기)
// 자리 지키는 선수 11 수비 라인(0 우리 골대 앞 · 10 상대 진영)  12 마크(0 지역 · 10 사람)  13 존 세우기(0 공 쪽으로 끌려감 · 10 제자리에서 존 세워 인터셉트)
// 모두          14 「강하게」(0 아낌 · 10 자주 — 포인트 2개를 한 선수에: 공 임자 = 패스·슛·드리블 +1 · 공 없는 선수 = 디펜스 +1, 11-22 밤)
//                      1  2  3  4  5  6  7  8  9 10 11 12 13 14
float SL[TAC_N][SL_N] = { { 5, 9, 2, 5, 8, 2, 2, 0, 2, 9, 8, 5, 2, 2 },    // 게겐프레싱 — 앞으로·침투·가운데로 · 남는 수비 0 · 둘이 달려들어 세게 · 높은 라인
                          { 9, 3, 8, 2, 2, 8, 1, 1, 1, 5, 5, 2, 5, 5 },    // 티키타카 — 늘 패스·옆뒤로·측면 벌림·짧게 · 1칸 곁으로 받으러 옴 · 지역
                          { 2, 6, 5, 8, 5, 5, 3, 2, 0, 2, 1, 8, 8, 8 } };  // 카테나치오 — 드리블 운반·길게 · 3칸 멀리서 받음 · 남는 수비 2 · 내려앉아 제자리 존 · 사람 마크
constexpr float SL_LO[SL_N] = { 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 };    // 막대마다 범위 (7 = 칸 · 8·9 = 명)
constexpr float SL_HI[SL_N] = { 10, 10, 10, 10, 10, 10, 3, 2, 2, 10, 10, 10, 10, 10 };
enum { PASS, FWD, WIDE, LEN, RUN, RWIDE, RDIST, REST, PRESS, TACKLE, LINE, MARK, ZONE, STRONG };
constexpr float EPS = 0.06f;   // 허용 폭 (유저 막대 아님)

// ---------- 직업 역할 (문서 5절 — 자리 깊이 · 임무) ----------
enum : unsigned { R_PRESS1 = 1, R_MARK = 2, R_RUN = 4, R_FIN = 8, R_CARRY = 16, R_LINK = 32, R_REST = 64, R_OUTLET = 128 };
struct Role {
	int d0, d1;   // 공 없을 때 깊이 (막대 11 라인이 이 위에서 옮긴다)
	int a0, a1;   // 우리 공일 때 깊이
	unsigned f;
};
constexpr int JOB_N = 15;
Role ROLE[TAC_N][JOB_N];
const Role MID{ 3, 5, 4, 6, 0 };   // 유망주·역할표 밖 = 중원

void set_roles() {
	for (auto &t : ROLE)
		for (Role &r : t)
			r = MID;
	auto put = [](int tac, int job, Role r) { ROLE[tac][job] = r; };
	put(0, HUNTER, { 4, 7, 5, 7, R_PRESS1 });
	put(0, MARKER, { 4, 6, 4, 6, R_MARK });
	put(0, SPRINTER, { 5, 7, 7, 9, R_RUN });
	put(0, CANNON, { 5, 7, 6, 8, R_FIN });
	put(0, STRIKER, { 5, 7, 7, 9, R_FIN });
	put(0, TRICKSTER, { 4, 6, 5, 8, R_CARRY });
	put(0, MAESTRO, { 4, 5, 4, 6, R_LINK });
	put(0, PLAYMAKER, { 4, 6, 5, 7, R_LINK });
	put(0, WALL, { 2, 4, 3, 4, R_REST });
	put(0, TITAN, { 3, 4, 3, 5, R_REST });
	put(1, MAESTRO, { 4, 5, 4, 6, R_LINK });
	put(1, PLAYMAKER, { 4, 5, 5, 7, R_LINK });
	put(1, TRICKSTER, { 4, 5, 5, 7, R_CARRY });
	put(1, SPRINTER, { 4, 5, 5, 8, 0 });
	put(1, CANNON, { 5, 6, 6, 8, R_FIN });
	put(1, STRIKER, { 5, 6, 7, 9, R_FIN });
	put(1, HUNTER, { 3, 5, 4, 5, R_PRESS1 });
	put(1, MARKER, { 3, 5, 4, 5, R_MARK });
	put(1, WALL, { 2, 4, 3, 4, R_REST });
	put(1, TITAN, { 3, 4, 4, 6, 0 });
	put(2, WALL, { 0, 2, 1, 3, R_REST });
	put(2, HUNTER, { 2, 3, 2, 4, R_PRESS1 });
	put(2, MARKER, { 1, 3, 2, 3, R_MARK });
	put(2, TITAN, { 1, 2, 1, 3, R_REST });
	put(2, MAESTRO, { 2, 4, 3, 5, R_LINK });
	put(2, PLAYMAKER, { 3, 4, 4, 6, R_LINK });
	put(2, SPRINTER, { 4, 5, 6, 9, R_RUN | R_OUTLET });
	put(2, CANNON, { 3, 4, 5, 7, R_FIN });
	put(2, STRIKER, { 3, 4, 6, 8, R_FIN });
	put(2, TRICKSTER, { 3, 4, 4, 7, R_CARRY });
}
struct Init {
	Init() { set_roles(); }
} init_roles;

inline int depth(int team, V2 t) { return team == HOME ? t.x : W - 1 - t.x; }   // 우리 골대 쪽 끝 0 → 상대 끝 W − 1
inline bool wide(V2 t) { return t.y <= 1 || t.y >= H - 2; }
inline int zone_gap(int d, int lo, int hi) { return d < lo ? lo - d : (d > hi ? d - hi : 0); }
const Role &role_of(int tac, const Player &p) { return p.position >= 0 && p.position < JOB_N ? ROLE[tac][p.position] : MID; }

int foes_next(const State &s, int team, V2 t) {
	int n = 0;
	for (const Player &q : s.players)
		if (q.placed() && q.team != team && manhattan(q.pos, t) == 1)
			n++;
	return n;
}

int player_at(const State &s, V2 t) {
	for (const Player &q : s.players)
		if (q.placed() && q.pos == t)
			return q.uid;
	return -1;
}

// ---------- 국면 (문서 2절) ----------
struct Sit {
	int team, opp;
	bool atk;          // 우리 공
	bool counter;      // 역습 = 상대 수비가 안 갖춰짐 + 공 앞에 치고 갈 공간 (사용자 11-22)
	bool lost_now;     // 막 뺏김 (뺏긴 차례·다음 우리 차례) — 역압박
	bool first_pass;   // 뺏은 뒤 아직 패스 전
	V2 ball;
	int ball_d;
	int near[PLAYERS], nn;   // 공에서 가까운 우리 선수
	int deep[PLAYERS];       // 우리 골대에 가까운 순서
	int foe[PLAYERS], nf;    // 상대 선수 (공 임자 빼고, 우리 골대에 가까운 = 위험한 순서)
};

// 공 앞(상대 골대 쪽) 치고 갈 공간: 공 임자 레인 ±1 에서 앞으로 1~3칸 안에 상대가 없다
bool space_ahead(const State &s, int team, V2 b) {
	for (const Player &q : s.players)
		if (q.placed() && q.team != team) {
			const int ahead = depth(team, q.pos) - depth(team, b);
			if (ahead >= 1 && ahead <= 3 && std::abs(q.pos.y - b.y) <= 1)
				return false;
		}
	return true;
}

Sit read(const State &s) {
	Sit x{};
	x.team = s.turn_team, x.opp = opponent(s.turn_team);
	x.ball = s.ball_owner >= 0 ? s.players[s.ball_owner].pos : UNPLACED;
	x.atk = s.ball_team() == x.team;
	const int age = std::max(0, (s.turn_count - s.poss_turn + 1) / 2);
	x.lost_now = !x.atk && age <= 1;
	x.ball_d = in_bounds(x.ball) ? depth(x.team, x.ball) : 4;
	if (x.atk && in_bounds(x.ball)) {
		int behind = 0;   // 상대 필드 선수 중 공보다 자기 골대 쪽 = 갖춰진 수비
		for (const Player &q : s.players)
			if (q.placed() && q.team == x.opp && depth(x.team, q.pos) > x.ball_d)
				behind++;
		const bool org = behind >= std::max(1, s.team_n - 1);
		x.counter = !org && space_ahead(s, x.team, x.ball);
	}
	x.first_pass = x.atk && s.touch_team == x.team && __builtin_popcount(unsigned(s.touch_mask)) <= 1;
	for (const Player &q : s.players)
		if (q.placed() && q.team == x.team)
			x.near[x.nn] = x.deep[x.nn] = q.uid, x.nn++;
		else if (q.placed() && q.uid != s.ball_owner)
			x.foe[x.nf++] = q.uid;
	std::sort(x.near, x.near + x.nn, [&](int a, int b) { return manhattan(s.players[a].pos, x.ball) < manhattan(s.players[b].pos, x.ball); });
	std::sort(x.deep, x.deep + x.nn, [&](int a, int b) { return depth(x.team, s.players[a].pos) < depth(x.team, s.players[b].pos); });
	std::sort(x.foe, x.foe + x.nf, [&](int a, int b) { return depth(x.team, s.players[a].pos) < depth(x.team, s.players[b].pos); });
	return x;
}

int rank_of(const int *ord, int n, int uid) {
	for (int i = 0; i < n; i++)
		if (ord[i] == uid)
			return i;
	return n;
}

// 공 가진 선수 (막대 1~4 · 역습이면 앞으로·길게가 더 세짐)
float holder(const State &s, const Sit &x, const float *S, int tac, const Player &p, const Action &a) {
	const int gain = depth(x.team, a.dest) - depth(x.team, p.pos);
	const float fwd = x.counter ? std::max(S[FWD], 9.0f) : S[FWD];
	const float len = x.counter ? std::min(10.0f, S[LEN] + 2.0f) : S[LEN];
	const Role &me = role_of(tac, p);
	float b = 0.0f;
	auto common = [&](int d) {
		b += (fwd - 5) * 0.15f * float(gain);                                       // 2 앞뒤
		b += wide(a.dest) ? (S[WIDE] - 5) * 0.15f : (a.dest.y == 3 ? -(S[WIDE] - 5) * 0.1f : 0.0f);   // 3 중앙·측면
		b -= 0.4f * std::fabs(float(d) - (1.0f + len * 0.4f));                     // 4 길이 (1칸 ~ 5칸)
	};
	switch (a.t) {
	case A_PASS: {
		const int d = manhattan(p.pos, a.dest);
		const int r = player_at(s, a.dest);
		b += (S[PASS] - 5) * 0.5f;
		common(d);
		if (r >= 0) {
			const Role &rr = role_of(tac, s.players[r]);
			if (x.first_pass && (rr.f & R_LINK) && !(me.f & R_LINK))
				b += 1.5f;   // 뺏은 뒤 첫 공은 연결 직업에게 (역할표)
			if (x.counter && (me.f & R_LINK) && (rr.f & R_RUN))
				b += 1.5f;   // 연결 → 침투 (역할표)
		}
		return b;
	}
	case A_DRIBBLE:
		b -= (S[PASS] - 5) * 0.5f;
		common(manhattan(p.pos, a.dest));
		if (me.f & R_CARRY)
			b += 0.3f * std::max(0, gain);
		return b;
	case A_SAVE:   // 공 임자 「강하게」 모으기 (14) — 슛 지대(깊이 6+)에서 더 (11-25 공 임자 제자리 A_STANCE = 버티기 — 덤 없음)
		return (S[STRONG] - 5) * (depth(x.team, p.pos) >= 6 ? 0.35f : 0.2f);
	default:
		return 0.0f;   // 슛은 보이면 쏜다 — 탐색 값이 정한다 (이기는 쪽 슛은 대본이 가로채지 않음)
	}
}

// 공 없는 우리 선수 (막대 5~8). 받으러 갈 자리 = 공에서 정확히 dist 칸(맨해튼, 막대 7) — 우선순위: 옆 → 앞 → 앞 대각 → 뒤 대각 → 뒤 (사용자 11-22)
float support_rank(int team, V2 h, V2 t, int dist) {
	if (manhattan(h, t) != dist)
		return 0.0f;
	const int dx = depth(team, t) - depth(team, h), dy = std::abs(t.y - h.y);
	if (dx == 0) return 1.0f;               // 옆
	if (dy == 0 && dx > 0) return 0.8f;     // 앞
	if (dx > 0) return 0.6f;                // 앞 대각
	if (dy > 0) return 0.4f;                // 뒤 대각
	return 0.2f;                            // 뒤
}

float runner(const State &s, const Sit &x, const float *S, int tac, const Player &p, const Action &a) {
	if (a.t != A_MOVE || s.ball_owner < 0)
		return 0.0f;
	const Role &me = role_of(tac, p);
	const V2 h = s.players[s.ball_owner].pos;
	const int dd = depth(x.team, a.dest), gain = dd - depth(x.team, p.pos);
	const int dist = int(S[RDIST]);   // 7 받으러 갈 거리 1·2·3칸
	float b = (10.0f - S[RUN]) * 0.3f * support_rank(x.team, h, a.dest, dist) * (foes_next(s, x.team, a.dest) == 0 ? 1.0f : 0.5f);
	b += (S[RUN] * 0.05f + ((me.f & R_RUN) ? 0.2f : 0.0f)) * float(std::max(0, gain));
	b += wide(a.dest) ? (S[RWIDE] - 5) * 0.12f : (a.dest.y == 3 ? -(S[RWIDE] - 5) * 0.08f : 0.0f);   // 6 움직이는 방향 중앙·측면
	const int rest = int(S[REST]);   // 0·1·2명
	if (rank_of(x.deep, x.nn, p.uid) < rest && dd > 3)
		b -= 0.8f;   // 8 남는 수비 (우리 골대에 가까운 n명은 깊이 3 안)
	b += 0.2f * float(zone_gap(depth(x.team, p.pos), me.a0, me.a1) - zone_gap(dd, me.a0, me.a1));
	return b;
}

// 공 없을 때 (막대 9~13)
float defend(const State &s, const Sit &x, const float *S, int tac, const Player &p, const Action &a) {
	const Role &me = role_of(tac, p);
	const int dd = depth(x.team, a.dest);
	const int closer = manhattan(p.pos, x.ball) - manhattan(a.dest, x.ball);
	const bool adj = manhattan(p.pos, x.ball) <= 1;
	const int rk = rank_of(x.near, x.nn, p.uid);
	// 9 달려드는 인원 = 0·1·2명 (막 뺏긴 차례엔 +1 — 역압박) · 10 태클 세기
	const int n = int(S[PRESS]) + (x.lost_now && S[PRESS] >= 1 ? 1 : 0);   // 0·1·2명
	const bool presser = !(me.f & R_OUTLET) && (rk < n || ((me.f & R_PRESS1) && S[PRESS] >= 1 && manhattan(p.pos, x.ball) <= 3));
	if (presser) {
		const float tk = std::max(2.0f, S[TACKLE]);   // 하 = 따라만 감 (그래도 달려드는 선수는 공 쪽으로)
		if (a.t == A_MOVE)
			return 0.25f * tk * std::max(-2, std::min(3, closer));
		if (a.t == A_STANCE)
			return adj ? 0.3f * S[TACKLE] + 0.5f : 0.0f;   // 붙어서 태클
		if (a.t == A_SAVE)
			return adj ? (S[STRONG] - 5) * 0.3f : 0.0f;   // 14 「강하게」 — 공 임자 곁이면 센 태클
		return 0.0f;
	}
	// 자리 지키는 선수: 11 라인 + 역할 자리 · 12 마크(사람이면 위험한 상대 곁, 지역이면 자리)
	const int line = int(std::lround(S[LINE] * 0.7f));
	const int lo = (me.f & R_OUTLET) ? me.d0 : std::max(0, std::min(me.d0, line) + (line - me.d0) / 2), hi = (me.f & R_OUTLET) ? me.d1 : std::max(lo, std::max(me.d1, line) + (line - me.d1) / 2);
	if (a.t == A_MOVE) {
		float b = (10.0f - S[MARK]) * 0.04f * float(zone_gap(depth(x.team, p.pos), lo, hi) - zone_gap(dd, lo, hi));
		const int k = rank_of(x.deep, x.nn, p.uid);   // 사람 마크: 우리 골대에 가까운 순서대로 위험한 상대에게 하나씩
		if (k < x.nf) {
			const V2 f = s.players[x.foe[k]].pos;
			b += S[MARK] * 0.06f * float(manhattan(p.pos, f) - manhattan(a.dest, f));
		}
		if (closer > 0 && S[ZONE] > 5)
			b -= (S[ZONE] - 5) * 0.1f;   // 13 존 세우기 상 = 공으로 끌려가지 않음 (내려앉아 기다리면 상대가 들어온다)
		return b;
	}
	if (a.t == A_SAVE)   // 14 「강하게」 모으기 (이어서 센 존)
		return (S[STRONG] - 5) * 0.2f;
	if (a.t == A_STANCE)   // 13 존 세우기 — 높을수록 제자리에서 존을 세워 인터셉트
		return zone_gap(depth(x.team, p.pos), lo, hi) == 0 ? 0.08f * S[ZONE] : 0.0f;
	return 0.0f;
}

float score(const State &s, const Action &a, const Style &st) {
	if (s.phase != PLAY || a.uid < 0 || a.uid >= PLAYERS || st.tac < 0 || st.tac >= TAC_N)
		return 0.0f;
	const Player &p = s.players[a.uid];
	if (p.team != s.turn_team || !p.placed())
		return 0.0f;
	const float *S = SL[st.tac];
	const Sit x = read(s);
	if (x.atk)
		return s.ball_owner == a.uid ? holder(s, x, S, st.tac, p, a) : runner(s, x, S, st.tac, p, a);
	return defend(s, x, S, st.tac, p, a);
}

struct Reg {
	Reg() { g_tac_score = score; }
} reg;

} // namespace

float tac_slider(int tac, int i) { return tac >= 0 && tac < TAC_N && i >= 0 && i < SL_N ? SL[tac][i] : 5.0f; }
void set_tac_slider(int tac, int i, float v) {
	if (tac >= 0 && tac < TAC_N && i >= 0 && i < SL_N)
		SL[tac][i] = std::max(SL_LO[i], std::min(SL_HI[i], i >= RDIST && i <= PRESS ? std::round(v) : v));   // 7·8·9 = 칸·명 (정수)
}
float tac_eps(int tac) { return tac >= 0 && tac < TAC_N ? EPS : 0.0f; }

} // namespace az
} // namespace card
} // namespace cf
