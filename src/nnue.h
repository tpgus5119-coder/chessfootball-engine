#pragma once
// 판 평가 신경망 (NNUE 방식 — 체스 엔진 스톡피시가 2020년부터 쓰는 구조).
// 탐색(미니맥스)은 그대로 두고 **판을 점수로 바꾸는 부분만** 신경망으로 바꾼다.
//
// 입력은 판 전체다 — 사람이 고른 평가 항목이 없다. 드래프트·배치·교체·경기 중 국면을 같은 망이 본다:
//   · 선수 하나 = (우리/상대, 포지션 슬롯, 칸) 한 칸이 켜진다 (선수 8명이면 8칸) — 희소 입력이라 첫 층이 싸다
//   · 수비 태세 · 공 임자 · 루즈볼 · 아직 안 놓은 선수 · 벤치 포지션도 같은 식으로 켜진다
//   · 단계·전후반·남은 턴·점수·골 턴·차례·포인트 같은 전역 값은 작은 실수 벡터로 붙는다
//   · 지금 AI의 전술 단서 6개(공 소유·전진도·무경합 슛각·경합 슛각·산개도)도 같이 넣는다 — 결과·탐색 점수만으로는
//     "슛 각이 열렸나"를 배우는 데 판이 아주 많이 든다 (시험: 단서 없이는 경기 중 판단이 지금 AI보다 약했다)
// **관점 정규화**: 언제나 "평가하는 팀"이 오른쪽(+x)으로 공격하도록 판을 뒤집어 넣는다 — 양 팀이 같은 망을 쓴다.
// 출력 = 그 팀 관점의 기대 결과 (−1 짐 … 0 무승부 … +1 이김)
#include <cstdint>
#include <vector>

#include "engine.h"

namespace cf {

constexpr int NN_NPOS = 11;                         // 드래프트 슬롯 10 + 그 밖(유망주 등)
constexpr int NN_OFF_PIECE = 0;                     // (rel·NPOS + slot)·70 + tile
constexpr int NN_OFF_STANCE = NN_OFF_PIECE + 2 * NN_NPOS * NT;
constexpr int NN_OFF_BALL = NN_OFF_STANCE + 2 * NT; // 공 임자가 선 칸 (rel = 임자 팀)
constexpr int NN_OFF_LOOSE = NN_OFF_BALL + 2 * NT;  // 루즈볼 칸 (rel = 놓은 팀)
constexpr int NN_OFF_UNPLACED = NN_OFF_LOOSE + 2 * NT; // 아직 판에 없는 선수 (rel·NPOS + slot)
constexpr int NN_OFF_BENCH = NN_OFF_UNPLACED + 2 * NN_NPOS; // 아무도 안 가진 드래프트 슬롯
constexpr int NN_SPARSE = NN_OFF_BENCH + NN_NPOS;
constexpr int NN_DENSE = 22;   // 전역 16 + 손으로 만든 전술 단서 6 (eval.gd features — 슛 각 등. 망이 그 위에 더 배운다)
constexpr int NN_MAX_ACTIVE = 48;

struct NetInput {
	uint16_t idx[NN_MAX_ACTIVE];
	int n = 0;
	float dense[NN_DENSE];
};

// 관점 team으로 본 입력 (학습 기보와 실전 평가가 같은 함수를 쓴다 — 두 벌이면 언젠가 어긋난다)
void net_input(const State &s, int team, NetInput &in);

struct Net {
	int h1 = 0, h2 = 0;
	std::vector<float> w1; // (NN_SPARSE + NN_DENSE) × h1 — 입력 하나당 한 줄
	std::vector<float> b1, w2, b2, w3; // w2 = h1 × h2, w3 = h2
	float b3 = 0.0f;
	bool load(const char *path); // 파일 형식은 tools/nn/train.py export와 같다
	bool load_mem(const unsigned char *p, size_t n); // 게임은 res:// 안의 파일을 바이트로 넘긴다 (.pck 안은 fopen이 못 연다)
	float eval(const NetInput &in) const;
};

// 탐색이 쓰는 망 (nullptr = 손으로 만든 선형 평가). 한 프로세스에 하나 — 아레나는 수마다 갈아 끼운다
extern const Net *g_net;
// 경기 중 수읽기에서 망을 어떻게 쓰나 (드래프트·배치·교체는 언제나 망만 쓴다 — 거기서 선형 평가는 거의 무작위다):
//   g_net_mix < 0  = 경기 중에도 망만 (순수)
//   g_net_mix ≥ 0  = 선형 평가 + g_net_mix × 망 — 선형의 전술(슛 각)은 살리고 망의 판세 판단을 얹는다
// 실측 (2026-10-17): 망만 쓰면 경기 중 판단이 선형보다 약했다 — 결과로 배운 망은 한 수 차이의 작은 점수 차를 못 가른다
extern double g_net_mix;
// 지금 고르는 수가 드래프트·배치·교체인가 — 그때는 잎이 경기 단계여도 망 하나로 잰다 (pick_action·pick_setup이 정한다)
extern bool g_root_setup;

} // namespace cf
