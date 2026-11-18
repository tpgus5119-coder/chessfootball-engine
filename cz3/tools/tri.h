#pragma once
// 수 3단계 시제품 (11-18 사용자 「세 개로 나눠야겠네 · 현업 방식으로」) — 「누가 → 무엇을 → 어디로」 탐색·망. 규칙(card.cpp)·기존 망(card_az)은 안 건드린다.
//   1단계 누가 (NW 18): 우리 선수 0~4 · 5 = 키퍼(선방 롱패스 A_SAVE) · 6 = 차례 끝(A_END) · 7~17 = 배치할 선수 칸(드래프트 풀 0~10, A_PLACE)
//   2단계 무엇을 (NC 61): (Action.sk·3 + 쓰임 킥/이동/수비)·6 + 손 칸 — 노마드가 바꿔 쓰는 쓰임도 여기 · 54~59 = 키퍼 킥 카드 · 60 = 카드 없음(배치·차례 끝)
//   3단계 어디로 (ND 72): 도착 칸 0~69(두는 팀이 +x 로 공격하게 뒤집음) · 70 = 골대(슛 · 키퍼 킥 확정) · 71 = 제자리(태세 · 차례 끝)
//     이동/드리블/패스/슛/태세 「어떻게」는 쓰임(2단계)과 도착 칸으로 정해진다 — 같은 (누가·무엇·어디)가 둘이면 같은 망 값을 나눠 쓴다
//   **길 확장 지점** (11-18 「길도 승리 확률로」): 3단계 자식은 이미 (칸 id 가 아니라) **실제 수 하나하나**다. card.cpp 가 같은 도착 칸으로
//     결과가 다른 길 후보를 따로 내면(Action 에 길 번호) 그 수들이 같은 칸 id 의 형제 자식이 되어 PUCT 가 결과(다음 국면 가치)로 가른다 —
//     길이 하나인 대부분의 칸은 가지·비용이 그대로. 망이 길까지 배우게 하려면 ids() 의 d 를 d + ND·길 번호로 넓히고 ND 를 늘리면 된다 (지금은 같은 칸 logit 을 나눠 씀)
//   한 차례 여러 장 = 카드 한 장마다 3단계 한 벌, 그다음 국면이 다시 1단계 (같은 팀이면 같은 편 가치)
// 탐색 = PUCT 이동 묶음 나무 (Childs·Brodeur·Kocsis 2008 move groups): 단계마다 마디, 국면 평가(몸통 한 번)는 1단계 마디에서만.
//   중간 마디의 첫 값은 FPU (KataGo: 부모 Q − r·√방문한 prior 합) · 뿌리 국면의 세 단계 모두 디리클레 잡음 · 1단계는 자르지 않는다 (top-m 없음)
// 몸통 = 희소 입력 합 → h1 (자른 ReLU) → [잔차 블록 × blocks: a = clamp(a + Wb·relu(Wa·a + ba) + bb, 0, 1) — 블록마다 자름 · 입력이 평면이 아니라 dense 잔차 (AlphaZero·KataGo 잔차 탑의 전결합판, Wb 0 시작 = Fixup/ReZero)] → 자른 ReLU → h2
// 망 = 같은 몸통 + 자기회귀 머리 셋 (AlphaStar·OpenAI Five 식): 누가 = Pw·b · 무엇 = Pc·clamp(b + E1[누가]) · 어디 = Pd·clamp(b + E1[누가] + E2[무엇])
#include "../src/card_az.h"

#include <cstdint>
#include <vector>

namespace tri {
using namespace cf::card;

constexpr int NW = 18, NC = 61, ND = 72;
constexpr int W_KEEPER = 5, W_END = 6, W_PLACE = 7;
constexpr int C_NONE = 60, C_SAVE = 54;
constexpr int D_GOAL = 70, D_STAY = 71;
void ids(const State &s, const Action &a, int &w, int &c, int &d);   // 두는 팀 = s.turn_team

constexpr int MAGIC3 = 0x335A4643;   // "CFZ3"
struct Net {
	int h1 = 0, h2 = 0, blocks = 0;   // blocks = 깊은 몸통 잔차 블록 수 (판 2 파일 — 0 이면 판 1 얕은 몸통과 같다)
	std::vector<float> rw;            // 블록마다 [Wa h1×h1 · ba h1 · Wb h1×h1 · bb h1] (입력 줄 j 마다 h1)
	std::vector<float> w1, b1, w2, b2, wv, wa, pw, bw, e1, pc, bc, e2, pd, bd;   // 머리 무게는 id 마다 한 줄 (h2)
	float bv = 0.0f, ba = 0.0f;
	bool load(const char *path);
	bool loaded() const { return h1 > 0; }
};

struct Params {
	int sims = 64;
	double c_puct = 1.5;
	double c_root = 0.0;   // >0 이면 뿌리 국면(세 단계)의 c — 뿌리는 넓게 · 아래는 좁게 (층별 c, 11-18)
	double fpu_root = 0.1, fpu = 0.2;   // KataGo rootFpuReductionMax · fpuReductionMax 와 같은 값
	double dirichlet_eps = 0.0;         // 뿌리 국면 세 단계 (α = 10 / 가지 수, 0.03~1 — KataGo 0.03·361/합법 수와 같은 생각)
	double aux_w = az::g_aux_w;         // 점수 효용 (card_az Params.aux_w 와 같은 식 — 마디 가치 = clamp(v + w·aux))
	double temp = 0.0;                  // >0 이면 뿌리 결합 방문 분포^(1/temp) 에서 뽑는다
	int batch = 16;                     // 묶음 평가 크기 (가상 손실로 여러 잎을 한 번에 — 11-18 처리량)
	unsigned long long seed = 1;
};

constexpr int K = 32;
struct Result {
	bool found = false;
	Action action;
	std::vector<cf::V2> path;   // 고른 길 (빈 것 = 엔진 기본 길) — 둘 때 g_path = &path 로 apply_action
	float root_v = 0.0f;
	float who[NW] = {};   // 1단계 방문 분포
	uint32_t who_legal = 0;
	int nk = 0;           // 결합 (누가·무엇·어디) 방문 분포 상위 K
	uint8_t tw[K], tc[K], td[K];
	float tp[K];
	std::vector<Action> acts;   // 합법 수와 결합 방문 분포 (온도 표본용)
	std::vector<float> joint;
	int pv_plies = 0, pv_turns = 0;   // 수읽기 깊이 (card_az Result 와 같은 뜻 — 실제 수·차례 경계)
	float leaf_turns = 0.0f;
	long long tt_hit = 0;   // 전치로 합친 횟수 (나무 누적)
};
struct Reuse {   // 나무 재사용 (card_az Reuse 와 같은 뜻) — 판마다 clear
	void *impl = nullptr;
	const Net *net = nullptr;
	size_t cap = 0;   // 마디가 이만큼 넘으면 새 나무
	long long hit = 0, miss = 0, kept = 0;
	Reuse() = default;
	Reuse(const Reuse &) = delete;
	Reuse &operator=(const Reuse &) = delete;
	~Reuse();
	void clear();
};
extern bool g_tt;   // 전치 합치기 (기본 켬 · 측정용으로 끌 수 있음)
Result search(const State &s, const Net &net, const Params &p, Reuse *keep = nullptr);
void params_from_env(Params &p);   // CF_CPUCT · CF_FPU · CF_FPU_ROOT
float value(const Net &net, const az::NetInput &in);   // 망 가치 (두는 팀 관점) — 시험·길 고르기용


// 기보 한 줄 (tools/az/train3.py REC3 와 같게)
struct Rec {
	uint16_t idx[az::MAX_ACTIVE];
	float dense[az::DENSE];
	float who[NW];
	uint32_t who_legal;
	uint8_t tw[K], tc[K], td[K];
	float tp[K];
	float z, q, aux;
	uint32_t game, use_pi;
};
static_assert(sizeof(Rec) == 2 * az::MAX_ACTIVE + 4 * az::DENSE + 4 * NW + 4 + 3 * K + 4 * K + 20, "train3.py 와 같게");

} // namespace tri
