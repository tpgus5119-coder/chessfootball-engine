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
constexpr int NPOS = POOL_N + 1;                       // 직업 12 + 신입생 칸
constexpr int NF = NT - 2 * 2 * (GOAL_N + 2);           // 80 — 박스 2×4 둘 밖
constexpr int NPL = (W / 2) * H - 2 * (GOAL_N + 2);     // 40 — 자기 진영(x 0~5) 박스 밖
constexpr int OFF_PIECE = 0;                           // (rel·NPOS + slot)·NF + 칸
constexpr int OFF_STANCE = OFF_PIECE + 2 * NPOS * NF;  // (rel·4 + 존 힘 1·2·3·4+)·NF + 칸 (스크린이 깎으면 1·2 도 — 11-27 묶음 14)
constexpr int OFF_BALL = OFF_STANCE + 2 * 4 * NF;      // 공 임자: rel·NF + 칸
constexpr int OFF_UNPLACED = OFF_BALL + 2 * NF;        // 안 놓인 선수: rel·NPOS + slot
constexpr int OFF_BENCH = OFF_UNPLACED + 2 * NPOS;     // 드래프트에 남은 직업: rel·POOL_N + slot
constexpr int OFF_MARKED = OFF_BENCH + 2 * POOL_N;     // 마커에게 붙은 선수
constexpr int OFF_PSTAT = OFF_MARKED + 2 * NF;         // 성장: (rel·7 + 칸)·NF + 칸 (슛·패스·무브·디펜스·피지컬·체력 · 2차 성장 — 11-26 묶음 8)
constexpr int OFF_TAI = OFF_PSTAT + 2 * GROW_K * NF;   // 태세 중인 선수: rel·NF + 칸 (공 임자 = 버티기 · 그 밖 = 존)
constexpr int OFF_ZONE = OFF_TAI + 2 * NF;              // 태세 존 칸: rel·NF + 칸
constexpr int OFF_ACTED = OFF_ZONE + 2 * NF;           // 이번 차례에 행동한 두는 팀 선수: me·NF + 칸
constexpr int OFF_GK = OFF_ACTED + PER_TEAM * NF;      // 골대 밖에 나온 골키퍼: rel·NF + 칸
constexpr int OFF_STAM = OFF_GK + 2 * NF;             // 남은 체력: (rel·4 + min(체력, 3))·NF + 칸 (11-25)
constexpr int OFF_SLEFT = OFF_STAM + 2 * 4 * NF;       // 태세 남은 지속(stance_left): (rel·3 + 1·2·3+)·NF + 칸 (월 — 11-27 묶음 14)
constexpr int OFF_SCUT = OFF_SLEFT + 2 * 3 * NF;       // 이번 차례 도착 스크린에 깎인 존(scut > 0): rel·NF + 칸 (차례 끝에 돌아옴)
constexpr int SPARSE = OFF_SCUT + 2 * NF;
static_assert(NF == 80 && NPL == 40 && SPARSE == 6370, "입력 칸 수 (판 9 · 11-27 묶음 14 태세 힘 넷 · 지속 · 스크린) — tools/az/train.py CF_MODE=card 와 같게");
constexpr int DENSE = 26;   // 숫자 입력 (card_az.cpp net_input d[0]~d[25] — 11-26 묶음 7 버티기 씀 칸 뺌)
constexpr int MAX_ACTIVE = 128;   // 판 9 — 체력·성장 칸이 늘어 96 → 128

struct NetInput {
	uint16_t idx[MAX_ACTIVE];
	int n = 0;
	float dense[DENSE];
};
void net_input(const State &s, int team, NetInput &in);

// ---------- 수 번호 (판 10 — 11-27 상대 좌표: AlphaZero 체스처럼 「누가 · 무엇을 · 자기 자리에서 얼마나」) ----------
// 이동·드리블·패스 = (종류·PER_TEAM + me)·NREL + 상대 칸. 상대 칸 = 뒤집은 판에서 (dx, dy) — 맨해튼 RREL 안 마름모 (가운데 뺌).
// 기준 칸 = 그 선수 칸 · 골대 안 키퍼는 골대 가운데(y 3.5) 기준 dy (아래 1·2… · 위 −1·−2… — 위아래 뒤집어도 dy → −dy)
// 범위: 이동 4 + 스프린터 2 = 6 · 패스 5 + 마에스트로 2 = 7 (시험이 합법 수가 다 들어가는지 본다)
constexpr int RREL = 7;
constexpr int NREL = 2 * RREL * (RREL + 1);                   // 112
constexpr int OFF_TILE = 0;
constexpr int OFF_SHOT = OFF_TILE + 3 * PER_TEAM * NREL;      // 슛: me
constexpr int OFF_STANCE_A = OFF_SHOT + PER_TEAM;             // 제자리 태세: me
constexpr int OFF_PLACE = OFF_STANCE_A + PER_TEAM;            // 배치: slot·NPL + 자기 진영 칸 (자가학습은 무작위 대형 — 칸만 남겨 둠)
constexpr int OFF_KEEP = OFF_PLACE + (POOL_N + 1) * NPL;      // 가상 키퍼 롱킥: 받는 me (칸 = 그 선수 발밑이라 따로 없음)
constexpr int OFF_GKIN = OFF_KEEP + PER_TEAM;                 // 골키퍼가 골대로 돌아감
constexpr int NA = OFF_GKIN + 1;
static_assert(NREL == 112 && NA == 2216, "수 칸 수 (판 10 상대 좌표) — tools/az/train.py 는 gen3 --tables 로 읽는다");
int action_index(const State &s, const Action &a);

// ---------- 위아래 거울 (y → 7 − y) — 학습 증강 (train.py) · 시험 ----------
// in[f] = 거울 입력 칸 · act[a] = 거울 수 칸. 숫자 입력(dense)은 위아래와 무관 (heuristic 포함)
struct Mirror {
	std::vector<uint16_t> in, act;
	std::vector<int16_t> plane, tile, glob;   // ResNet 판 입력: 칸 입력이면 (판 번호, 칸 0~95) · 아니면 glob 번호 (−1 = 해당 없음)
	int planes = 0, globs = 0;
};
const Mirror &mirror();
// 거울이 규칙상 정확한 국면인가 — 골대 안 키퍼가 공을 쥐면 그 키퍼는 대표 칸(위 칸 y 3)에서 차고 몰아 위아래가 어긋난다 (규칙 그대로 · 증강에서만 뺌)
bool mirror_exact(const State &s);

// ---------- 망 — 판 10 머리표 (MAGIC "CFZ5" · 옛 CFZ4 망은 깨끗이 거절) ----------
// arch 0 flat: 입력 → h1 → h2 → 가치·보조·정책 머리
// arch 1 ResNet (KataGo 꼴 작은 판): 칸 입력 판 P장(12×8) 3×3 줄기 + 칸 없는 입력 → 채널 덤 · 잔차 블록 B개(3×3 두 번 · 가운데 전역 평균 덤)
//        → 평균·최대 모음 + 1×1 칸 머리 4장 → h2 → 같은 가치·보조·정책 머리
constexpr int MAGIC = 0x355A4643;   // "CFZ5"
constexpr int RES_HEAD = 4;        // ResNet 칸 머리 채널
struct Net {
	int arch = -1;
	int h1 = 0, h2 = 0;
	int na = 0;
	int blocks = 0, ch = 0;
	std::vector<float> w1, b1, w2, b2, wv, wa, wp, bp;
	// ResNet: ws [planes·9·ch] · wg [globs·ch] · bs [ch] · 블록마다 c1 [9·ch·ch] cb1 · q [ch·ch] · c2 cb2 · wh [ch·4] bh [4] · wf [(2ch + 4·NT)·h2] bf
	std::vector<float> ws, wg, bs, wh, bh, wf, bf;
	std::vector<std::vector<float>> c1, cb1, q, c2, cb2;
	float bv = 0.0f, ba = 0.0f;
	bool load(const char *path);
	bool load_mem(const unsigned char *p, size_t n);
	bool loaded() const { return arch >= 0; }
};

// 망 한 번 (입력 그대로) — 가치(tanh) · 수 칸 logits (ai 칸들 · 범위 밖 0). 시험·학습기 대조용
float net_eval(const Net &net, const NetInput &in, const int *ai, int n, float *logits);

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
	float surprise = 0.0f;       // KL(π' ‖ 망 정책) — 표본 무게 (KataGo 정책 놀람)
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
	float w;         // 표본 무게 (정책 놀람 — 빠른 탐색 줄은 1)
	uint32_t game;   // 판 번호 · 맨 위 비트 = 위아래 거울이 정확하지 않은 국면 (mirror_exact 거짓 — 증강에서 뺀다)
};
static_assert(sizeof(State) % 4 == 0, "State 채움 바이트 없음");
static_assert(sizeof(Rec) == 2 * MAX_ACTIVE + 4 * DENSE + 6 * PI_K + 20 && sizeof(Rec) == 572, "기록 크기는 train.py(CF_MODE=card)와 같아야 한다");

// 컵 경기처럼: 선수단 6명(유망주 0~6 · 나머지 서로 다른 직업 · 성장 5단계) 중 선발 5명 무작위 · 골키퍼 = 선발 아무나
void random_lineup(State &s, int team, unsigned long long r);
// 대형 = 필드 4명 칸 (공격 +x 좌표 — 원정은 x 거울) · 필드 선수 uid 순서. 못 놓는 칸이면 그 선수가 설 수 있는 첫 칸 (ui/card_match.gd _auto_place 와 같다)
struct Form {
	V2 t[FIELD_N];
};
extern const Form CAREER_FORMS[3];   // core/career.gd DEFAULT_FORM · FORM_PRESS · FORM_HOLD
Form random_form(unsigned long long &r);   // 자기 진영 박스 밖 서로 다른 네 칸
void place_forms(State &s, const Form f[2]);   // 배치 단계를 두 팀 대형으로 끝낸다
// 경기 전 무작위 (자가학습·아레나): 75 % 무작위 선수단 · 대형 = 75 % 무작위 · 25 % 커리어 셋 중 하나
void random_prematch(State &s, unsigned long long r);

// ---------- 경기 전 결정 (망 가치 머리) ----------
// tmpl = new_match + 상대 set_lineup(+ 체력) 까지 한 판 (배치 전). 선수단 n(≤ 6)명에서 선발 5 × 키퍼 × 대형(forms + 무작위 sampled 개)을
// 킥오프 국면 가치(동전 두 경우 평균)로 잰다. stam = 선수단 체력 (nullptr = 가득) · opp_form = 상대 대형
struct LineupPick {
	int starters[PER_TEAM] = {};   // 선수단 번호 — 필드 4명 다음 키퍼 (set_lineup 꼴 · gk = 4)
	Form form{};
	float v = -2.0f;
	int evals = 0;
};
LineupPick choose_lineup(const State &tmpl, int team, int n, const int pos[], const int bonus[][GROW_K], const int *stam, const Form &opp_form,
		const std::vector<Form> &forms, int sampled, unsigned long long seed, const Net &net);

// 길 고르기용 상태 가치 — card::value_path 의 PathValue (ctx = const Net *, 망이 안 읽혔으면 휴리스틱 값). team 에 좋을수록 큼
float state_value(const State &s, int team, const void *ctx);

} // namespace az
} // namespace card
} // namespace cf
