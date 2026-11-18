#pragma once
// 카드 모드 알파제로 — 본편 az.h와 같은 Gumbel 탐색·망 모양, 입력·수 번호만 카드 모드 것 (card.h).
// 학습기는 tools/az/train.py를 CF_MODE=card로 돌린다 — 아래 칸 수가 train.py와 같아야 한다.
#include <cstdint>
#include <vector>

#include "card.h"

namespace cf {
namespace card {
namespace az {

// ---------- 입력 (평가하는 팀이 늘 +x로 공격하게 뒤집는다) ----------
constexpr int NPOS = POOL_N + 1;
constexpr int OFF_PIECE = 0;                          // (rel·NPOS + slot)·70 + 칸
constexpr int OFF_STANCE = OFF_PIECE + 2 * NPOS * NT; // (rel·4 + 힘 2~5)·70 + 칸
constexpr int OFF_BALL = OFF_STANCE + 2 * 4 * NT;
constexpr int OFF_LOOSE = OFF_BALL + 2 * NT;
constexpr int OFF_UNPLACED = OFF_LOOSE + 2 * NT;
constexpr int OFF_BENCH = OFF_UNPLACED + 2 * NPOS;
constexpr int OFF_MARKED = OFF_BENCH + 2 * POOL_N;
constexpr int OFF_BOOSTED = OFF_MARKED + 2 * NT;
constexpr int OFF_BOUND = OFF_BOOSTED + 2 * NT;   // 묶인 선수 칸 (10-29)
constexpr int OFF_SKILL = OFF_BOUND + 2 * NT;   // (rel·9 + 자리)·N_SKILL + 스킬 — 자리 0~5 = 킥·이동 칸, 6~8 = 수비 칸 (2026-11-07)
constexpr int OFF_ZONE_SK = OFF_SKILL + 2 * 9 * N_SKILL_NET;   // 체력 카드(11-13)는 망에 안 보인다   // (rel·3 + 넓은 존·전방 압박·측면 봉쇄)·70 + 칸
constexpr int OFF_PSTAT = OFF_ZONE_SK + 2 * 3 * NT;   // 컵 선수 스탯 (rel·4 + 스탯)·3단계(1·2·3+)·70 + 칸 (2026-11-08)
constexpr int SPARSE_V1 = OFF_PSTAT + 2 * 4 * 3 * NT;   // 5698 — 판 1 입력 (11-14 전 망, 앱 assets/ai/card.bin)
// ---- 판 2 입력 (11-14 컵 커리어 규칙 — 체력·직업 스킬 게이지·전성기 혜택·체력 카드). 판 1 뒤에 **덧붙이기만** 한다:
//      판 1 망을 읽으면 새 칸 무게가 0 이라 옛 망은 비트까지 같은 값을 낸다 (Net::load_mem). 모두 컵 스위치(stam_on·car_on)가 꺼져 있으면 0
constexpr int OFF_STAM = SPARSE_V1;                    // 체력 지금 칸: (rel·4 + 칸 0~3)·70 + 칸 (stam_on)
constexpr int OFF_SCAP = OFF_STAM + 2 * 4 * NT;         // 경기 시작 칸(회복 상한): (rel·3 + 1~3 − 1)·70 + 칸 (stam_on)
constexpr int OFF_GAUGE = OFF_SCAP + 2 * 3 * NT;        // 직업 스킬을 켤 수 있음 (11-14 — 액티브 직업 · 체력 2 이상): (rel·4 + 0)·70 + 칸 (car_on). 1~3 칸은 옛 게이지 자리 — 비어 있음
constexpr int OFF_PERK = OFF_GAUGE + 2 * 4 * NT;        // 전성기 혜택: (rel·3 + 이동 범위·수비 지속·킥 범위)·70 + 칸
constexpr int OFF_ACTED = OFF_PERK + 2 * 3 * NT;        // 이번 차례에 카드를 낸 선수 (차례 끝 +1 회복 없음): rel·70 + 칸 (stam_on)
constexpr int OFF_WSTREAK = OFF_ACTED + 2 * NT;         // 월 대표 행동 진행 — 지난 우리 차례를 태세로 끝냄: rel·70 + 칸 (car_on)
constexpr int OFF_UNPL_STAM = OFF_WSTREAK + 2 * NT;     // 아직 안 놓인 선수 체력: (rel·NPOS + 칸)·4 + 지금 칸 0~3 (stam_on — 배치·후반 재배치)
constexpr int OFF_SKILL_ST = OFF_UNPL_STAM + 2 * NPOS * 4;   // 체력 카드 6장: (rel·9 + 자리)·6 + (스킬 − K_EASY) — 판 1 스킬 칸은 그대로 「기본」
constexpr int SPARSE_V2 = OFF_SKILL_ST + 2 * 9 * (N_SKILL - N_SKILL_NET);   // 8134 — 판 2 (11-14 ~ 11-17 망)
// ---- 판 3 입력 (11-17 만능): 만능은 조각·배치 칸에서 유망주 칸(NPOS − 1)을 같이 쓰고, 여기서 만능임을 따로 알린다. 덧붙이기만 — 옛 망은 0 무게
constexpr int OFF_ALLROUND = SPARSE_V2;                 // 놓인 만능: rel·70 + 칸 · 안 놓인 만능: 2·70 + rel
constexpr int SPARSE = OFF_ALLROUND + 2 * NT + 2;       // 8276
static_assert(SPARSE_V1 == 5698 && SPARSE_V2 == 8134 && SPARSE == 8276, "판 1·2·3 입력 칸 수 — tools/az/train.py CF_MODE=card 와 같게");
constexpr int DENSE_BASE = 27 + 2 * N_TAC + 11;   // 54 — v8.6 전술카드 8종 · 닥공·강슛·숨 고르기·육탄방어 표시
constexpr int DENSE_RESERVE = 32;   // 스킬카드 예비 칸 (2026-11-04) — v16·v17 이 27칸을 썼다
constexpr int DENSE_V1 = DENSE_BASE + DENSE_RESERVE;   // 86 — 판 1
constexpr int DENSE_CUP = 32;   // 판 2 컵 칸 (앞 10칸 사용, 나머지 예비 0)
constexpr int DENSE = DENSE_V1 + DENSE_CUP;   // 118
constexpr int MAX_ACTIVE = 192;   // 판 1 128 — 컵 칸이 선수마다 최대 6개 늘어 (11-14)

struct NetInput {
	uint16_t idx[MAX_ACTIVE];
	int n = 0;
	float dense[DENSE];
};
void net_input(const State &s, int team, NetInput &in);

// ---------- 수 번호 ----------
// 이동·드리블·패스 (3 × 내 5 × 칸 70 × 카드 6 — 숫자 셋 × 손/조커) · 슛 5×6 · 태세 5×6 · 배치 10×70 · 턴 종료 · 전술 3 = 7064
constexpr int OFF_TILE = 0;
constexpr int NCLS = 3;   // 카드 숫자 셋 (v8.6 프리롤 뺌 — 종전 6 = 숫자 셋 × 손/조커)
constexpr int OFF_SHOT = OFF_TILE + 3 * PER_TEAM * NT * NCLS;
constexpr int OFF_STANCE_A = OFF_SHOT + PER_TEAM * NCLS;
constexpr int OFF_PLACE = OFF_STANCE_A + PER_TEAM * NCLS;
constexpr int OFF_END = OFF_PLACE + (POOL_N + 1) * NT;   // +1 = 유망주 배치 (컵)
constexpr int OFF_TACTIC = OFF_END + 1;
constexpr int OFF_BIND = OFF_TACTIC + 7;   // 대상 없는 전술 7종(로테이션·봉쇄·리플레이·닥공·강슛·숨 고르기·육탄방어) 다음 = 압박 대상 상대 0~4
constexpr int NA_V1 = OFF_BIND + PER_TEAM;   // 3963 (v8.6 · 유망주 배치 칸 포함) — 11-14 전 망 (앱 assets/ai/card.bin)
// 직업 스킬을 켠 수 (11-14): 이동·드리블·패스·슛·태세 수 번호(OFF_TILE ~ OFF_PLACE 앞) 그대로 + OFF_SKILL_A
constexpr int OFF_SKILL_A = NA_V1;
constexpr int NA_V2 = OFF_SKILL_A + OFF_PLACE;   // 7143 — 11-14 ~ 11-17 망
// 만능의 범위 스킬 수 (11-17, Action.sk = 2 — 마에스트로 킥·스프린터 이동): 이동·드리블·패스·슛 수 번호(OFF_TILE ~ OFF_STANCE_A 앞) + OFF_SKILL_B.
// 만능의 힘 스킬(sk 1)은 다른 스킬 직업과 같은 OFF_SKILL_A 칸
constexpr int OFF_SKILL_B = NA_V2;
constexpr int NA = OFF_SKILL_B + OFF_STANCE_A;   // 10308
static_assert(NA_V1 == 3963 && NA_V2 == 7143 && NA == 10308, "수 칸 수 — tools/az/train.py CF_MODE=card 와 같게");
int action_index(const State &s, const Action &a);

// ---------- 망 (본편과 같은 몸통, 머리표 MAGIC만 다르다) ----------
constexpr int MAGIC = 0x435A4643;   // "CFZC"
struct Net {
	int h1 = 0, h2 = 0;
	int na = 0;   // 파일의 수 칸 수 (NA_V1 = 스킬 수 없는 옛 망 — 스킬 수는 같은 카드 수 값 · NA_V2 = 만능 범위 스킬 칸 없는 망 — 스킬 칸 값을 빌린다)
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
	bool on() const { return poss != 0.0f || ctr != 0.0f || press != 0.0f || hold != 0.0f || jk != 0.0f; }
};
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
static_assert(sizeof(Rec) == 2 * MAX_ACTIVE + 4 * DENSE + 6 * PI_K + 16 && sizeof(Rec) == 1064, "기록 크기는 train.py(CF_MODE=card)와 같아야 한다");

struct GenStats {
	int games = 0, home = 0, away = 0, draws = 0, kos = 0;   // kos = 0:0 판 (v10 — HP가 없다)
	int duels = 0;   // 골 동점 → 일기토로 가린 판 (v11 — 10-31 자가대전에 넣음). draws = 일기토까지 비긴 판
	long long moves = 0, goals = 0, lost = 0;
};
// 양 팀 모두 탐색으로 드래프트·배치·경기를 둔다. 전술카드 3종은 판마다 무작위(하네스)
void selfplay(const Net &net, int games, int sims, unsigned long long seed, int temp_moves, double lam,
		std::vector<Rec> &out, GenStats &st, int fast_sims = 0, double full_frac = 1.0, double rand_draft_frac = 0.0);

// 하네스 난수로 고르는 전술카드 3종 (11종 중)
int random_tactics(unsigned long long r);
// 판마다 다른 덱 — 기본 손 4가지 중 하나 + 칸마다 스킬 (학습·측정, 사용자 2026-11-07). 스킬 없는 칸도 섞인다
void random_deck(State &s, int team, unsigned long long r, double skill_p = 0.6);
// 컵 경기처럼 정해진 선수단 — 유망주 0~5명 · 나머지 서로 다른 직업 · 선수 스탯 0~3 (학습에 섞는다)
void random_lineup(State &s, int team, unsigned long long r);

// 길 고르기용 상태 가치 — card::value_path 의 PathValue (ctx = const Net *, 망이 안 읽혔으면 휴리스틱 값). team 에 좋을수록 큼
float state_value(const State &s, int team, const void *ctx);

} // namespace az
} // namespace card
} // namespace cf
