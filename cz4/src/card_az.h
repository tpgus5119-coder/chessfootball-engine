#pragma once
// 감독 모드 알파제로 — Gumbel 탐색 · 망 입력 · 수 번호 (card.h 규칙 위).
// 학습기는 tools/az/train.py를 CF_MODE=card로 돌린다 — 아래 칸 수가 train.py와 같아야 한다.
#include <cstdint>
#include <vector>

#include "card.h"

namespace cf {
namespace card {
namespace az {

// ---------- 입력 (평가하는 팀이 늘 +x로 공격하게 뒤집는다) — 판 9 = 규칙 묶음 6 (11-25 직업 12 · 성장 여섯 · 체력 칸 — 자가학습 처음부터, 옛 망 이식 없음) ----------
// 칸 번호: 선수·공·존이 놓이는 입력과 수는 박스(골대 포함) 밖 NF = 80칸만 쓴다 (field_tile) · 배치는 자기 진영 박스 밖 NPL = 40칸 (place_tile)
// 골대 안 키퍼는 칸 입력이 없다 — 숫자 입력 d[22]·d[23](골대 안)과 키퍼 표시(OFF_GK, 골대 밖일 때 그 칸)로 본다
constexpr int NPOS = POOL_N + 1;                       // 직업 11 + 신입생 칸
constexpr int NF = NT - 2 * 2 * (GOAL_N + 2);           // 80 — 박스 2×4 둘 밖
constexpr int NPL = (W / 2) * H - 2 * (GOAL_N + 2);     // 40 — 자기 진영(x 0~5) 박스 밖
constexpr int OFF_PIECE = 0;                           // (rel·NPOS + slot)·NF + 칸
constexpr int OFF_STANCE = OFF_PIECE + 2 * NPOS * NF;  // (rel·3 + 힘 2·3·4+)·NF + 칸 (태세 힘은 디펜스 3 − 마커 1 = 2 아래로 안 내려간다)
constexpr int OFF_BALL = OFF_STANCE + 2 * 3 * NF;      // 공 임자: rel·NF + 칸
constexpr int OFF_UNPLACED = OFF_BALL + 2 * NF;        // 안 놓인 선수: rel·NPOS + slot
constexpr int OFF_BENCH = OFF_UNPLACED + 2 * NPOS;     // 드래프트에 남은 직업: rel·POOL_N + slot
constexpr int OFF_MARKED = OFF_BENCH + 2 * POOL_N;     // 마커에게 붙은 선수
constexpr int OFF_PSTAT = OFF_MARKED + 2 * NF;         // 성장: (rel·6 + 스탯)·NF + 칸 (스탯마다 +1 한 번 — 슛·패스·무브·디펜스·피지컬·체력)
constexpr int OFF_CHARGE = OFF_PSTAT + 2 * GROW_K * NF;     // 「강하게」 모은 선수 (두는 팀만 — 모으기는 그 차례 안에서만)
constexpr int OFF_ZONE = OFF_CHARGE + NF;              // 태세 존 칸: rel·NF + 칸
constexpr int OFF_ACTED = OFF_ZONE + 2 * NF;           // 이번 차례에 행동한 두는 팀 선수: me·NF + 칸
constexpr int OFF_GK = OFF_ACTED + PER_TEAM * NF;      // 골대 밖에 나온 골키퍼: rel·NF + 칸
constexpr int OFF_STAM = OFF_GK + 2 * NF;             // 남은 체력: (rel·4 + min(체력, 3))·NF + 칸 (11-25)
constexpr int SPARSE = OFF_STAM + 2 * 4 * NF;
static_assert(NF == 80 && NPL == 40 && SPARSE == 5166, "입력 칸 수 (판 9 · 11-26 직업 11) — tools/az/train.py CF_MODE=card 와 같게");
constexpr int DENSE = 26;   // 숫자 입력 (card_az.cpp net_input d[0]~d[25])
constexpr int MAX_ACTIVE = 128;   // 판 9 — 체력·성장 칸이 늘어 96 → 128

struct NetInput {
	uint16_t idx[MAX_ACTIVE];
	int n = 0;
	float dense[DENSE];
};
void net_input(const State &s, int team, NetInput &in);

// ---------- 수 번호 (판 7 그대로) ----------
constexpr int OFF_TILE = 0;                                   // 이동·드리블·패스: (종류·PER_TEAM + me)·NF + 칸
constexpr int OFF_SHOT = OFF_TILE + 3 * PER_TEAM * NF;        // 슛: me
constexpr int OFF_STANCE_A = OFF_SHOT + PER_TEAM;             // 제자리 사방 태세: me
constexpr int OFF_PLACE = OFF_STANCE_A + PER_TEAM;            // 배치: slot·NPL + 자기 진영 칸
constexpr int OFF_SAVE = OFF_PLACE + (POOL_N + 1) * NPL;      // 공 없는 선수 「강하게」 모으기: 자기 칸
constexpr int OFF_KEEP = OFF_SAVE + NF;                       // 키퍼 롱킥: 받는 me·NF + 칸
constexpr int OFF_GKIN = OFF_KEEP + PER_TEAM * NF;            // 골키퍼가 골대로 돌아감
constexpr int NA = OFF_GKIN + 1;
static_assert(NA == 2171, "수 칸 수 (판 9 — 직업 11, 11-26) — tools/az/train.py CF_MODE=card 와 같게");
int action_index(const State &s, const Action &a);

// ---------- 망 (입력 → h1 → h2 → 가치·보조·정책 머리) ----------
constexpr int MAGIC = 0x345A4643;   // "CFZ4" — 머리표의 SPARSE·DENSE·NA 가 위와 같아야 읽힌다
struct Net {
	int h1 = 0, h2 = 0;
	int na = 0;
	std::vector<float> w1, b1, w2, b2, wv, wa, wp, bp;
	float bv = 0.0f, ba = 0.0f;
	bool load(const char *path);
	bool load_mem(const unsigned char *p, size_t n);
	bool loaded() const { return h1 > 0; }
};

// 팀 성격 — 망은 그대로, 탐색할 때 그 팀 마디의 수 prior(logit)에만 작은 덤을 더한다.
// 규칙(card.cpp)은 안 바꾼다. 네 축: 지공(짧은 패스·가담) · 역습(긴 킥·빠른 전진) · 전방압박(수비가 공 쪽으로) · 지키는 수비(자기 진영 자리)
// 티키타카 = 지공+지키기 · 토탈사커 = 지공+압박 · 게겐프레싱 = 역습+압박 · 선수비 후역습 = 역습+지키기. 다 0 이면 지금과 비트까지 같다
struct Style {
	float poss = 0.0f, ctr = 0.0f, press = 0.0f, hold = 0.0f;
	int tac = -1;      // 전술 대본 (card_tactic.cpp): 0 게겐프레싱 · 1 티키타카 · 2 카테나치오 · −1 없음
	float eps = 0.0f;  // 허용 폭: 뿌리에서 탐색 값이 최선 − eps 안인 수들 가운데 대본 점수가 가장 높은 수를 둔다
	bool on() const { return poss != 0.0f || ctr != 0.0f || press != 0.0f || hold != 0.0f || tac >= 0; }
};
extern float (*g_tac_score)(const State &s, const Action &a, const Style &st);   // card_tactic.cpp 가 채운다 (없으면 대본 없음 — 자가학습 gen3 은 링크하지 않음)
float style_bias(const State &s, const Action &a, const Style &st);   // 그 수의 logit 덤 (s.turn_team 이 두는 수)

struct Params {
	int style_team = -1;   // 이 팀 마디에만 성격 덤 (−1 = 없음)
	Style style;
	int sims = 32, top_m = 16;
	bool gumbel = true;
	double c_visit = 50.0, c_scale = 1.0;
	unsigned long long seed = 1;
	double dirichlet_eps = 0.0, dirichlet_alpha = 0.3;
};

struct Result {
	bool found = false;
	Action action;
	float root_v = 0.0f;
	std::vector<Action> acts;
	std::vector<float> policy;   // 개선 정책 π' (acts 순서)
};
Result search(const State &s, const Net &net, const Params &p);

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
static_assert(sizeof(Rec) == 2 * MAX_ACTIVE + 4 * DENSE + 6 * PI_K + 16 && sizeof(Rec) == 568, "기록 크기는 train.py(CF_MODE=card)와 같아야 한다");

// 컵 경기처럼 정해진 선수단 5명 — 유망주 0~5명 · 나머지 서로 다른 직업 · 성장 카드 · 골키퍼 = 절반은 디펜스 최고, 절반은 아무나 (학습에 섞는다)
void random_lineup(State &s, int team, unsigned long long r);

// 길 고르기용 상태 가치 — card::value_path 의 PathValue (ctx = const Net *, 망이 안 읽혔으면 휴리스틱 값). team 에 좋을수록 큼
float state_value(const State &s, int team, const void *ctx);

} // namespace az
} // namespace card
} // namespace cf
