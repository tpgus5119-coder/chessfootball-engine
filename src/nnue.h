#pragma once
// 신경망 입력 인코딩 — 알파제로 망(az.h)이 판을 읽는 모양. (옛 NNUE 판 평가 망은 2026-10-23 정리 때 뺐다 — 알파제로로 대체)
//
// 입력은 판 전체다 — 사람이 고른 평가 항목이 없다. 드래프트·배치·경기 중 국면을 같은 망이 본다:
//   · 선수 하나 = (우리/상대, 포지션 슬롯, 칸) 한 칸이 켜진다 (선수 10명이면 10칸) — 희소 입력이라 첫 층이 싸다
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

// 포지션 칸 = 드래프트 슬롯 11 + 그 밖(컵의 유망주) 1. 2026-10-23 전에는 11칸이라 슬롯 10(런처)과 '그 밖'이 같은 칸을 썼다
constexpr int NN_NPOS = POOL_N + 1;
constexpr int NN_OFF_PIECE = 0;                     // (rel·NPOS + slot)·70 + tile
constexpr int NN_OFF_STANCE = NN_OFF_PIECE + 2 * NN_NPOS * NT;
constexpr int NN_OFF_BALL = NN_OFF_STANCE + 2 * NT; // 공 임자가 선 칸 (rel = 임자 팀)
constexpr int NN_OFF_LOOSE = NN_OFF_BALL + 2 * NT;  // 루즈볼 칸 (rel = 놓은 팀)
constexpr int NN_OFF_UNPLACED = NN_OFF_LOOSE + 2 * NT; // 아직 판에 없는 선수 (rel·NPOS + slot)
constexpr int NN_OFF_BENCH = NN_OFF_UNPLACED + 2 * NN_NPOS; // 아무도 안 가진 드래프트 슬롯
constexpr int NN_OFF_GRAB = NN_OFF_BENCH + POOL_N; // v5.80 마커에게 붙잡힌 선수가 선 칸 (rel = 붙잡힌 선수의 팀)
constexpr int NN_SPARSE = NN_OFF_GRAB + 2 * NT;
constexpr int NN_DENSE = 22;   // 전역 16 + 손으로 만든 전술 단서 6 (eval.gd features — 슛 각 등. 망이 그 위에 더 배운다)
constexpr int NN_MAX_ACTIVE = 48;

struct NetInput {
	uint16_t idx[NN_MAX_ACTIVE];
	int n = 0;
	float dense[NN_DENSE];
};

// 관점 team으로 본 입력 (학습 기보와 실전 평가가 같은 함수를 쓴다 — 두 벌이면 언젠가 어긋난다)
void net_input(const State &s, int team, NetInput &in);

} // namespace cf
