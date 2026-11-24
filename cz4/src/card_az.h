#pragma once
// 카드 모드 알파제로 — 본편 az.h와 같은 Gumbel 탐색·망 모양, 입력·수 번호만 카드 모드 것 (card.h).
// 학습기는 tools/az/train.py를 CF_MODE=card로 돌린다 — 아래 칸 수가 train.py와 같아야 한다.
#include <cstdint>
#include <vector>

#include "card.h"

namespace cf {
namespace card {
namespace az {

// ---------- 입력 (평가하는 팀이 늘 +x로 공격하게 뒤집는다) — 판 5 (11-24 다섯째 자리·늘 꺼진 칸 없앰 · 옛 판 4 망은 tools/cz/net_slim.py 로 옮긴다) ----------
// 칸 번호: 선수·공·존이 놓이는 입력과 수는 박스(골대 포함) 밖 NF = 72칸만 쓴다 (field_tile) · 배치는 자기 진영 박스 밖 NPL = 32칸 (place_tile)
constexpr int NPOS = POOL_N + 1;                       // 직업 10 + 신입생 칸
constexpr int NF = NT - 2 * 2 * (GOAL_N + 2);           // 72 — 박스 2×4 둘 밖
constexpr int NPL = (W / 2) * H - 2 * (GOAL_N + 2);     // 32 — 자기 진영(x 0~4) 박스 밖
constexpr int OFF_PIECE = 0;                           // (rel·NPOS + slot)·NF + 칸
constexpr int OFF_STANCE = OFF_PIECE + 2 * NPOS * NF;  // (rel·3 + 힘 2·3·4+)·NF + 칸 (태세 힘은 디펜스 3 − 마커 1 = 2 아래로 안 내려간다)
constexpr int OFF_BALL = OFF_STANCE + 2 * 3 * NF;      // 공 임자: rel·NF + 칸
constexpr int OFF_LOOSE = OFF_BALL + 2 * NF;           // 루즈볼: (찬 팀 rel)·NF + 칸
constexpr int OFF_UNPLACED = OFF_LOOSE + 2 * NF;       // 안 놓인 선수: rel·NPOS + slot
constexpr int OFF_BENCH = OFF_UNPLACED + 2 * NPOS;     // 드래프트에 남은 직업: rel·POOL_N + slot
constexpr int OFF_MARKED = OFF_BENCH + 2 * POOL_N;     // 마커에게 붙은 선수
constexpr int OFF_HOLDUP = OFF_MARKED + 2 * NF;        // 지속·버티기 성장(hold_up) 선수
constexpr int OFF_PSTAT = OFF_HOLDUP + 2 * NF;         // 스탯업: (rel·4 + 스탯)·NF + 칸 (스탯마다 +1 한 번 — 11-23)
constexpr int OFF_PERK = OFF_PSTAT + 2 * 4 * NF;       // 범위 +1: (rel·2 + 이동·킥)·NF + 칸
constexpr int OFF_CHARGE = OFF_PERK + 2 * 2 * NF;      // 「강하게」 모은 선수 (두는 팀만 — 모으기는 그 차례 안에서만)
constexpr int OFF_ZONE = OFF_CHARGE + NF;              // 태세 존 칸: rel·NF + 칸
constexpr int OFF_ACTED = OFF_ZONE + 2 * NF;           // 이번 차례에 행동한 두는 팀 선수: me·NF + 칸
constexpr int SPARSE = OFF_ACTED + PER_TEAM * NF;
static_assert(NF == 72 && NPL == 32 && SPARSE == 4002, "입력 칸 수 (판 5, 11-24) — tools/az/train.py CF_MODE=card · tools/cz/net_slim.py 와 같게");
constexpr int DENSE = 20;   // 숫자 입력 (card_az.cpp net_input 의 d[0]~d[19])
constexpr int MAX_ACTIVE = 96;

struct NetInput {
	uint16_t idx[MAX_ACTIVE];
	int n = 0;
	float dense[DENSE];
};
void net_input(const State &s, int team, NetInput &in);

// ---------- 수 번호 (판 5) ----------
constexpr int OFF_TILE = 0;                                   // 이동·드리블·패스: (종류·PER_TEAM + me)·NF + 칸
constexpr int OFF_SHOT = OFF_TILE + 3 * PER_TEAM * NF;        // 슛: me
constexpr int OFF_STANCE_A = OFF_SHOT + PER_TEAM;             // 제자리 태세(월 사방): me
constexpr int OFF_PLACE = OFF_STANCE_A + PER_TEAM;            // 배치: slot·NPL + 자기 진영 칸
constexpr int OFF_SAVE = OFF_PLACE + (POOL_N + 1) * NPL;      // 공 없는 선수 「강하게」 모으기: 자기 칸
constexpr int OFF_KEEP = OFF_SAVE + NF;                       // 키퍼 롱킥: (받는 me 0~3 · 4 = 빈 칸 루즈볼)·NF + 칸
constexpr int OFF_STANCE_DIR = OFF_KEEP + (PER_TEAM + 1) * NF;   // 방향 태세: me·4 + 방향 (평가 팀 기준 — 원정은 ±x 를 바꾼다) DIRS 순서 +x·−x·+y·−y
constexpr int NA = OFF_STANCE_DIR + PER_TEAM * 4;
static_assert(NA == 1672, "수 칸 수 (판 5, 11-24) — tools/az/train.py CF_MODE=card · tools/cz/net_slim.py 와 같게");
int action_index(const State &s, const Action &a);

// ---------- 망 (본편과 같은 몸통, 머리표 MAGIC만 다르다) ----------
constexpr int MAGIC = 0x345A4643;   // "CFZ4" — 감독 모드 망 (머리표의 SPARSE·DENSE·NA 가 위와 같아야 읽힌다 — 옛 판 4 망 7746·96·5120 은 안 읽힌다)
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
static_assert(sizeof(Rec) == 2 * MAX_ACTIVE + 4 * DENSE + 6 * PI_K + 16 && sizeof(Rec) == 480, "기록 크기는 train.py(CF_MODE=card)와 같아야 한다");

// 컵 경기처럼 정해진 선수단 — 유망주 0~4명 · 나머지 서로 다른 직업 · 선수 스탯 0~3 (학습에 섞는다)
void random_lineup(State &s, int team, unsigned long long r);

// 길 고르기용 상태 가치 — card::value_path 의 PathValue (ctx = const Net *, 망이 안 읽혔으면 휴리스틱 값). team 에 좋을수록 큼
float state_value(const State &s, int team, const void *ctx);

} // namespace az
} // namespace card
} // namespace cf
