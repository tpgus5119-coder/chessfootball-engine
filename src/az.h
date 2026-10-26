#pragma once
// 알파제로 계열 수읽기 — **Gumbel AlphaZero** (Danihelka 외 2022 "Policy improvement by planning with Gumbel").
// 탐색 횟수가 적어도(8~64) 정책이 꾸준히 좋아지도록 고친 알파제로다 — 개인 장비(맥 한 대)에 맞다.
//
// 구조:
//   · 망 하나가 세 가지를 낸다 — 가치(이 판을 둘 차례 팀이 이길 기대, −1…+1) · 정책(어느 수가 좋은가, 합법 수마다 점수)
//     · 보조(끝났을 때 골 차 — KataGo식 보조 목표. 승패만으로는 배움이 느리다)
//   · 입력은 희소 판 입력(nnue.h net_input) — 드래프트·배치·경기 중 국면을 같은 망이 본다
//   · 수의 번호(action_index): 두는 팀 관점(늘 +x로 공격하게 뒤집음) · 내 선수 0~4 · 상대 선수 0~4 · 칸 0~69
//   · 탐색: 뿌리에서 Gumbel 잡음 + 순차 반감(Sequential Halving), 아래에서는 "완성된 Q"로 만든 개선 정책을 따라 내려간다.
//     한 턴에 두 수인 것은 마디마다 "두는 팀"을 따로 들고 가서 푼다 — 팀이 바뀌는 마디에서만 부호를 뒤집는다
//   · 규칙에 난수가 없으므로(동전 던지기만 경기 밖) 우연 마디가 없다
// 학습 쪽(tools/az/)은 이 파일의 번호·망 형식을 그대로 쓴다. 바꾸면 양쪽을 같이 고친다
#include <cstdint>
#include <vector>

#include "engine.h"
#include "nnue.h"

namespace cf {
namespace az {

// ---------- 수의 번호 (정책 머리의 출력 칸) ----------
// 이동·드리블·패스·슛 (4 × 내 5 × 칸 70) · 마에스트로 패스 포인트 0·2 변형 (2 × 5 × 70; 1포인트는 보통 패스 칸) ·
// 태클·밀기·잡기 (3 × 내 5 × 상대 5) · 태세 5 · 버티기 5 · 턴 종료 1 · 배치 슬롯 11 × 70 · 부스트 5 · 해제 5 = 2966 (v5.98 — 그 전 2961, 재투입 5 × 70은 v5.90에 뺐다)
// (2026-10-23 입력 칸을 고치며 망을 새로 시작할 때 옛 교체 자리 66칸을 걷어냈다 — 그 전 NA는 3022)
constexpr int OFF_TILE = 0;
constexpr int OFF_PASSC = OFF_TILE + 4 * PER_TEAM * NT;
constexpr int OFF_TGT = OFF_PASSC + 2 * PER_TEAM * NT;
constexpr int OFF_STANCE = OFF_TGT + 3 * PER_TEAM * PER_TEAM;   // 태클 · 밀기 · 잡기(v5.80)
constexpr int OFF_HOLD = OFF_STANCE + PER_TEAM;
constexpr int OFF_END = OFF_HOLD + PER_TEAM;
constexpr int OFF_PLACE = OFF_END + 1;
constexpr int OFF_BOOST = OFF_PLACE + POOL_N * NT;   // v5.83
constexpr int OFF_UNSTANCE = OFF_BOOST + PER_TEAM;   // v5.98 태세 해제 — 맨 끝에 붙여 앞 칸 번호는 그대로
constexpr int NA = OFF_UNSTANCE + PER_TEAM;   // 2966 (v5.98 — 그 전 2961). 망 출력 칸이 바뀌어 학습은 처음부터

int action_index(const State &s, const Action &a);   // s.turn_team이 두는 팀. 모르는 수면 −1

// ---------- 망 ----------
struct Net {
	int h1 = 0, h2 = 0;
	std::vector<float> w1, b1, w2, b2;   // NNUE와 같은 몸통: (NN_SPARSE+NN_DENSE)×h1 · h1 · h1×h2 · h2
	std::vector<float> wv, wa, wp;       // 머리: 가치 h2 · 보조 h2 · 정책 (파일은 h2×NA, 읽은 뒤 NA×h2로 뒤집어 둔다)
	float bv = 0.0f, ba = 0.0f;
	std::vector<float> bp;               // NA
	bool load(const char *path);
	bool load_mem(const unsigned char *p, size_t n);
	bool loaded() const { return h1 > 0; }
};

struct Eval {
	float v = 0.0f;     // 둘 차례 팀 관점 기대 결과
	float aux = 0.0f;   // 둘 차례 팀 관점 골 차 (/2, 잘라서 −1…+1)
	std::vector<float> logits;   // 합법 수마다 (enumerate_actions 순서)
};
// 망이 없으면(loaded()=false) 첫 세대용: 가치 = 손으로 만든 선형 평가(tanh), 정책 = 고른 것 없음(전부 0)
void evaluate(const Net &net, const State &s, const std::vector<Action> &acts, Eval &out);

// ---------- 탐색 ----------
struct Params {
	int sims = 32;          // 탐색 횟수(잎 평가 수)
	int top_m = 16;         // 뿌리에서 Gumbel로 뽑는 후보 수
	bool gumbel = true;     // false = 잡음 없이(실전·아레나 결정론)
	double c_visit = 50.0, c_scale = 1.0;   // σ(q) = (c_visit + maxN)·c_scale·q
	unsigned long long seed = 1;
	// **난이도 손잡이** (게임용 — 학습·아레나는 0)
	int sample_top = 0;             // >0 이면 개선 정책 상위 k개 중에서 뽑는다 (쉬움 — 최선 수만 두지 않는다)
};

struct Result {
	bool found = false;
	Action action;
	int action_idx = -1;
	float root_v = 0.0f;              // 뿌리 가치(완성된 Q로 섞은 값) — 둘 차례 팀 관점
	std::vector<Action> acts;         // 뿌리 합법 수
	std::vector<float> policy;        // 학습 목표 — 개선된 정책 π' (acts와 같은 순서, 합 1)
	std::vector<int> visits;          // 디버그용 방문 수
};

Result search(const State &s, const Net &net, const Params &p);

// ---------- 자가대전 기보 (학습 데이터) ----------
// 국면마다 392바이트 — tools/az/train.py REC와 같아야 한다. 희소 입력 · 전역 · 정책 목표 상위 32개 · z · q · aux · 판 번호
constexpr int PI_K = 32;
struct Rec {
	uint16_t idx[NN_MAX_ACTIVE];
	float dense[NN_DENSE];
	uint16_t pi_idx[PI_K];
	float pi_p[PI_K];
	float z, q, aux;
	uint32_t game;
};
static_assert(sizeof(Rec) == 424, "기록 크기는 train.py와 같아야 한다");   // v5.95: 전역 22 (기세 2칸 폐지 — v5.86~v5.94는 432, 그 전 392)

struct GenStats {
	int games = 0, home = 0, away = 0, draws = 0;
	long long moves = 0;
};
// 양 팀 모두 Gumbel 탐색으로 드래프트·배치·경기를 둔 기보를 out에 쌓는다 (gen.cpp·게임 안 공장 모드가 같이 쓴다).
// 처음 temp_moves수는 개선 정책에서 뽑아 개막을 흩고, z = λ·결과 + (1−λ)·탐색 가치.
// **탐색 상한 무작위**(KataGo 'playout cap randomization', 2026-10-22): fast_sims > 0 이면 수마다 full_frac 확률로만
// sims회 풀탐색을 하고 그 국면만 정책 목표를 적는다(pi_idx). 나머지 수는 fast_sims회로 빨리 두고 가치 목표(z·aux)만 남긴다
// — 같은 시간에 판이 3~4배 늘어 가치 학습이 좋아지고, 정책 목표는 풀탐색 것만 써서 질이 안 떨어진다
void selfplay(const Net &net, int games, int sims, unsigned long long seed, int temp_moves, double lam,
		std::vector<Rec> &out, GenStats &st, int fast_sims = 0, double full_frac = 1.0);

} // namespace az
} // namespace cf
