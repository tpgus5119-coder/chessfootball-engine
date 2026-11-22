#pragma once
// 카드 모드 알파제로 — 본편 az.h와 같은 Gumbel 탐색·망 모양, 입력·수 번호만 카드 모드 것 (card.h).
// 학습기는 tools/az/train.py를 CF_MODE=card로 돌린다 — 아래 칸 수가 train.py와 같아야 한다.
#include <cstdint>
#include <vector>

#include "card.h"

namespace cf {
namespace card {
namespace az {

// ---------- 입력 (평가하는 팀이 늘 +x로 공격하게 뒤집는다) — 판 4 (11-19 카드 없는 4대4 · 옛 망은 못 읽는다) ----------
constexpr int NPOS = POOL_N + 1;                       // 직업 10 + 신입생·만능 칸
constexpr int N_TB = 12;                               // 옛 전술판 효과 수 — 망 입력 자리만 (11-21 없앰, 칸은 늘 0)
constexpr int OFF_PIECE = 0;                           // (rel·NPOS + slot)·70 + 칸
constexpr int OFF_STANCE = OFF_PIECE + 2 * NPOS * NT;  // (rel·4 + 힘 1~4+)·70 + 칸
constexpr int OFF_BALL = OFF_STANCE + 2 * 4 * NT;
constexpr int OFF_LOOSE = OFF_BALL + 2 * NT;
constexpr int OFF_UNPLACED = OFF_LOOSE + 2 * NT;
constexpr int OFF_BENCH = OFF_UNPLACED + 2 * NPOS;
constexpr int OFF_MARKED = OFF_BENCH + 2 * POOL_N;
constexpr int OFF_BOOSTED = OFF_MARKED + 2 * NT;   // 11-20 부스터 없앰 — 빈 칸 (자리만 지킴)
constexpr int OFF_PSTAT = OFF_BOOSTED + 2 * NT;        // 스탯업 (rel·4 + 스탯)·3단계(1·2·3+)·70 + 칸
constexpr int OFF_JKON = OFF_PSTAT + 2 * 4 * 3 * NT;   // 그 팀에 켜진 족보: rel·N_JK + 족보 (card.h jk_enabled). 이 칸 묶음 2·3·70 중 앞 12칸만 쓰고 나머지 예비 (11-21 족보 6개 — 옛 9개 18칸은 이식 때 치우침으로 접음)
constexpr int OFF_PERK = OFF_JKON + 2 * 3 * NT;        // 범위 +1 혜택 (rel·2 + 이동·킥)·70 + 칸
constexpr int OFF_ALLROUND = OFF_PERK + 2 * 2 * NT;    // 놓인 만능: rel·70 + 칸 · 안 놓인 만능: 2·70 + rel
constexpr int OFF_BOARD = OFF_ALLROUND + 2 * NT + 2;   // 옛 전술판 자리 (늘 0 — 망 형식 유지)
constexpr int OFF_LASTPASS = OFF_BOARD + 2 * N_TB;     // 앞 140칸 = 태세가 한 차례 더 남은 선수 칸(월 2차례, 11-21) rel·70 + 칸 · 뒤 140칸 늘 0
constexpr int OFF_FWDMOVED = OFF_LASTPASS + 2 * 2 * NT; // 족보 +1 토큰 선수 칸 (11-21): rel·70 + 칸
constexpr int OFF_ZONE = OFF_FWDMOVED + 2 * NT;        // 태세 존 칸 (11-21 방향 존 — 옛 예비 칸 앞 140칸): rel·70 + 존 칸 (월은 사방 4칸)
constexpr int OFF_VIS_P = OFF_ZONE + 2 * NT;           // 이번 차례 사람이 있었던 칸 (11-21 되돌아가기 금지 — 예비 칸): 두는 팀의 me(0~3)·70 + 칸
constexpr int OFF_VIS_B = OFF_VIS_P + TEAM_N * NT;     // 이번 차례 공이 있었던 칸: 70 + 칸
constexpr int SPARSE_USED = OFF_VIS_B + NT;
constexpr int SPARSE_RESERVE = 512 - 2 * NT - (TEAM_N + 1) * NT;   // 나중에 더할 칸 (11-19 512 칸 — 11-21 존 140 · 지나간 칸 350 을 여기서 씀, 22칸 남음)
constexpr int SPARSE = SPARSE_USED + SPARSE_RESERVE;
static_assert(SPARSE == 6180 && OFF_JKON == 4382 && OFF_LASTPASS == 5248 && OFF_VIS_P == 5808 && SPARSE_RESERVE == 22, "입력 칸 수 — tools/az/train.py CF_MODE=card · tools/cz/net_rules1121.py 와 같게");
constexpr int DENSE_USED = 56;
constexpr int DENSE = 96;   // 뒤 40칸 = 예비
constexpr int MAX_ACTIVE = 96;

struct NetInput {
	uint16_t idx[MAX_ACTIVE];
	int n = 0;
	float dense[DENSE];
};
void net_input(const State &s, int team, NetInput &in);

// ---------- 수 번호 (판 4) ----------
// 이동·드리블·패스 (3 × 내 5 × 칸 70) · 슛 5 · 태세 5(11-21부터 월 = 제자리 사방만) · 배치 11×70 · (빈 1 — 옛 턴 종료) · 키퍼 킥 70
// · 뒷공간 패스(11-21 — 옛 스킬 수 칸: 11-19 부터 한 번도 안 쓴 자리) · 빈 칸 키퍼 킥 + 받을 동료(11-21 — 같은 옛 스킬 칸)
// · 방향 태세 5 × 4 (11-21 — 옛 예비 칸 첫 20칸). 앞 번호는 그대로라 지금 망을 이어 배운다
constexpr int OFF_TILE = 0;
constexpr int OFF_SHOT = OFF_TILE + 3 * PER_TEAM * NT;
constexpr int OFF_STANCE_A = OFF_SHOT + PER_TEAM;
constexpr int OFF_PLACE = OFF_STANCE_A + PER_TEAM;
constexpr int OFF_FREE_END = OFF_PLACE + (POOL_N + 1) * NT;   // 옛 턴 종료 — 11-21 없앰, 빈 칸
constexpr int OFF_SAVE = OFF_FREE_END + 1;
constexpr int OFF_RUN = OFF_SAVE + NT;                 // 뒷공간 패스: (차는 내 5 × 받는 내 5)·70 + 칸 (옛 스킬 수 칸 1901~)
constexpr int OFF_SAVE_RUN = OFF_RUN + PER_TEAM * PER_TEAM * NT;   // 빈 칸 키퍼 킥: 받는 내 5·70 + 칸
constexpr int OFF_STANCE_DIR = OFF_SAVE + NT + OFF_PLACE + OFF_STANCE_A;   // (내 5 × 4 + 방향) — 방향은 평가 팀 기준(원정은 ±x 를 바꾼다) DIRS 순서 +x·−x·+y·−y
constexpr int NA_USED = OFF_STANCE_DIR + PER_TEAM * 4;
constexpr int NA = 4096;   // 뒤는 예비
static_assert(OFF_RUN == 1901 && OFF_SAVE_RUN + PER_TEAM * NT <= OFF_STANCE_DIR && OFF_STANCE_DIR == 4016 && NA_USED == 4036 && NA >= NA_USED, "수 칸 수 — tools/az/train.py CF_MODE=card 와 같게");
int action_index(const State &s, const Action &a);

// ---------- 망 (본편과 같은 몸통, 머리표 MAGIC만 다르다) ----------
constexpr int MAGIC = 0x345A4643;   // "CFZ4" — 판 4 (11-19 카드 없음). 옛 "CFZC" 망은 안 읽힌다
struct Net {
	int h1 = 0, h2 = 0;
	int na = 0;
	std::vector<float> w1, b1, w2, b2, wv, wa, wp, bp;
	float bv = 0.0f, ba = 0.0f;
	bool load(const char *path);
	bool load_mem(const unsigned char *p, size_t n);
	bool loaded() const { return h1 > 0; }
};

// 점수 효용 (KataGo score utility, Wu 2019 — 측정용 11-10): 탐색 마디 가치 = clamp(v + w·aux, −1, 1). 기본 0 = 지금과 같다
//   aux = 보조 머리 · 끝난 판은 실제 골 피해 차 / 100. aux_from_env() 가 CF_AUX_W 를 g_aux_w 에 읽는다 (Params 기본값이 그것)
extern double g_aux_w;
void aux_from_env();

// 팀 성격 (사용자 11-12 「성격 부여하고」) — 망은 그대로, 탐색할 때 그 팀 마디의 수 prior(logit)에만 작은 덤을 더한다.
// 규칙(card.cpp)은 안 바꾼다. 네 축: 지공(짧은 패스·가담) · 역습(긴 킥·빠른 전진) · 전방압박(수비가 공 쪽으로) · 지키는 수비(자기 진영 자리)
// 티키타카 = 지공+지키기 · 토탈사커 = 지공+압박 · 게겐프레싱 = 역습+압박 · 선수비 후역습 = 역습+지키기. 다 0 이면 지금과 비트까지 같다
struct Style {
	float poss = 0.0f, ctr = 0.0f, press = 0.0f, hold = 0.0f;
	float jk = 0.0f;   // 족보 의지 (11-12 둘째) — 족보를 완성하는 카드에 덤, 이어 가는 카드엔 작은 덤
	int tac = -1;      // 전술 대본 (11-22, card_tactic.cpp): 0 게겐프레싱 · 1 티키타카 · 2 카테나치오 · −1 없음
	float eps = 0.0f;  // 허용 폭: 뿌리에서 탐색 값이 최선 − eps 안인 수들 가운데 대본 점수가 가장 높은 수를 둔다
	float pass = 0.0f; // 대본 패스 덤 (공격 중 패스·뒷공간 패스)
	bool on() const { return poss != 0.0f || ctr != 0.0f || press != 0.0f || hold != 0.0f || jk != 0.0f || tac >= 0; }
};
extern float (*g_tac_score)(const State &s, const Action &a, const Style &st);   // card_tactic.cpp 가 채운다 (없으면 대본 없음 — 자가학습 gen3 은 링크하지 않음)
float style_bias(const State &s, const Action &a, const Style &st);   // 그 수의 logit 덤 (s.turn_team 이 두는 수)

struct Params {
	int style_team = -1;   // 이 팀 마디에만 성격 덤 (−1 = 없음)
	Style style;
	double aux_w = g_aux_w;
	double temp = 0.0;   // >0 이면 개선 정책 π'^(1/temp) 에서 수를 뽑는다 (난이도 — Stockfish Skill Level · KataGo 사람 상대와 같은 생각, 측정용 11-10)
	int sims = 32, top_m = 16;
	bool gumbel = true;
	double c_visit = 50.0, c_scale = 1.0;
	unsigned long long seed = 1;
	int sample_top = 0;
	double dirichlet_eps = 0.0, dirichlet_alpha = 0.3;
};

struct Result {
	bool found = false;
	Action action;
	float root_v = 0.0f;
	std::vector<Action> acts;
	std::vector<float> policy;
	int pv_plies = 0, pv_turns = 0;   // 수읽기 깊이 (11-18 측정용): 주된 갈래(가장 많이 간 자식 줄기)의 수 · 차례 경계(두는 팀이 바뀐 횟수)
	float leaf_turns = 0.0f;          // 시뮬레이션 잎까지 평균 차례 경계
};
Result search(const State &s, const Net &net, const Params &p);
// 탐색 나무 재사용 (11-18 사용자 「이득이라면 당연히」): 지난 탐색 나무에서 지금 국면과 비트까지 같은 마디(깊이 8 안)를 찾아
// 그 가지를 새 뿌리로 이어 쓴다 — 방문·가치 합이 그대로 남고, Gumbel·디리클레 잡음은 새 뿌리에 새로 뽑는다.
// 정책 목표 π' 는 방문 분포가 아니라 완성된 Q 로 만들어 재사용 방문이 목표를 왜곡하지 않는다 (KataGo 의 방문 수 목표 문제가 없다).
// 망·aux_w·성격 팀이 다르면 버리고 새로. cap = 이어 쓸 마디 수 상한 (0 = max(4096, 8·sims)). keep == nullptr 이면 위와 비트까지 같다
struct Reuse {
	void *impl = nullptr;
	const Net *net = nullptr;
	double aux_w = 0.0;
	int style_team = -1, cap = 0;
	long long hit = 0, miss = 0, kept = 0;   // 측정: 이어 쓴 탐색 · 못 찾은 탐색 · 이어 쓴 뿌리 방문 합
	Reuse() = default;
	Reuse(const Reuse &) = delete;
	Reuse &operator=(const Reuse &) = delete;
	~Reuse();
	void clear();   // 판이 새로 시작할 때
};
Result search(const State &s, const Net &net, const Params &p, Reuse *keep);

// ---------- 기보 ----------
constexpr int PI_K = 32;
struct Rec {
	uint16_t idx[MAX_ACTIVE];
	float dense[DENSE];
	uint16_t pi_idx[PI_K];
	float pi_p[PI_K];
	float z, q, aux;
	uint32_t game;
};
static_assert(sizeof(Rec) == 2 * MAX_ACTIVE + 4 * DENSE + 6 * PI_K + 16 && sizeof(Rec) == 784, "기록 크기는 train.py(CF_MODE=card)와 같아야 한다");

struct GenStats {
	int games = 0, home = 0, away = 0, draws = 0, kos = 0;   // kos = 0:0 판 (v10 — HP가 없다)
	int duels = 0;   // 골 동점 → 일기토로 가린 판 (v11 — 10-31 자가대전에 넣음). draws = 일기토까지 비긴 판
	long long moves = 0, goals = 0, lost = 0;
};
// 양 팀 모두 탐색으로 드래프트·배치·경기를 둔다
void selfplay(const Net &net, int games, int sims, unsigned long long seed, int temp_moves, double lam,
		std::vector<Rec> &out, GenStats &st, int fast_sims = 0, double full_frac = 1.0, double rand_draft_frac = 0.0);

// 컵 경기처럼 정해진 선수단 — 유망주 0~5명 · 나머지 서로 다른 직업 · 선수 스탯 0~3 (학습에 섞는다)
void random_lineup(State &s, int team, unsigned long long r);

// 길 고르기용 상태 가치 — card::value_path 의 PathValue (ctx = const Net *, 망이 안 읽혔으면 휴리스틱 값). team 에 좋을수록 큼
float state_value(const State &s, int team, const void *ctx);

} // namespace az
} // namespace card
} // namespace cf
