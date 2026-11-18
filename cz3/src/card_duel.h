#pragma once
// **일기토** (카드 모드 — 사용자 2026-10-30 "0대0이면 승부차기 대신 일대일 승부 · 공수교대 · 서든데스 · 카드 숫자는 비밀(블러핑) · 판정 ㄱ").
// 경기가 골·골 피해까지 같으면 연다. 라운드마다 두 팀에서 선발했던 순서대로 한 명씩 저절로 나와(v12 — 고르기 없음, 다섯이 다 나가면 처음부터), 그 둘이 공수를 바꿔 한 번씩 공격한다(홈 먼저).
// 한 공격: 공격수는 중앙선에서 공을 갖고, 수비수는 자기 골 앞에서. 손은 새 6장(공격 킥·이동 / 수비 수비·이동). 공격수 3차례 안에 넣어야 한다.
// 판정: 한쪽만 넣으면 승 · 둘 다 넣으면 더 적은 차례에 넣은 쪽 · 같으면(또는 둘 다 못 넣으면) 다음 라운드 · 10라운드면 무승부.
// 비밀: 낸 카드의 숫자는 상대에게 안 보인다 — 태세 힘과 공 임자의 버티는 힘(HOLD)은 경합이 나야 드러난다. 규칙 판정은 card.cpp 그대로(같은 State로 둔다).
#include "card.h"
#include "card_az.h"

#include <vector>

namespace cf {
namespace card {
namespace duel {

constexpr int ATK_TURNS = 3;
constexpr int MAX_ROUNDS = 10;

struct Duel {
	State base;               // 경기가 끝난 판 (선수 포지션)
	State s;                  // 지금 공격 판 — 두 선수만 놓인다
	int round = 0;
	int stage = 0;            // 1 = 홈 공격 · 2 = 원정 공격 · 3 = 끝 (v12 — 고르기 없음) · 먼저 공격하는 팀은 라운드마다 바뀐다 (first_atk)
	int pick[2] = { -1, -1 }; // 이번 라운드에 나간 uid — 선발 순서대로 저절로 (라운드 % 5번째)
	int turns[2] = { 0, 0 };  // 이번 라운드 그 팀 공격이 넣은 차례 (0 = 못 넣음)
	int last_turns[2] = { 0, 0 };   // 막 끝난 라운드의 turns — 다음 라운드로 넘어가며 turns를 지우기 전에 남긴다 (화면 결과 띠)
	int atk_team = -1, atk_turn = 0;
	int winner = -2;          // −2 진행 중 · −1 무승부 · HOME/AWAY
	bool stance_shown[PLAYERS] = {};   // 태세 힘이 상대에게 드러났나 (경합이 나면)
	bool hold_shown = false;           // 공 임자의 버티는 힘이 드러났나
};

int first_atk(const Duel &d);            // 이 라운드에 먼저 공격하는 팀 (라운드마다 번갈아)
void start(Duel &d, const State &end_of_match);
void actions(const Duel &d, std::vector<Action> &out);
bool apply(Duel &d, const Action &a, std::vector<Event> *ev_out = nullptr);   // 공격 판에 한 수 — 공격이 끝나면 다음 공격·라운드로 넘긴다 (ev_out = 화면 연출 기록)
void pass_turn(Duel &d);                 // 낼 수 있는 수가 없을 때
// 상대의 숨은 숫자(손에 남은 카드·태세 힘·버티는 힘)는 엿보지 않는다 — 있을 수 있는 경우를 모두 세워 각각 탐색하고 정책을 더해 고른다
bool ai_act(const Duel &d, const az::Net &net, int sims, unsigned long long seed, Action &out,
		std::vector<Action> *acts_out = nullptr, std::vector<float> *pol_out = nullptr);   // acts_out·pol_out = 더한 정책(합 1) — 자가학습 기록용
// **경기가 골 동점으로 끝난 판을 일기토로 끝까지 둔다** (자가학습·평가 — 사용자 2026-10-31 "알파제로처럼 모든 기능을 마스터").
// 팀마다 망 net[team] (불러오지 않은 망 = 손평가 탐색). on_move(d, 둔 팀, 후보 수, 정책) = 수를 두기 직전마다. 돌려주는 값 = 이긴 팀 (−1 무승부)
int resolve(const State &end_of_match, const az::Net *const net[2], int sims, unsigned long long seed,
		void (*on_move)(void *ctx, const Duel &d, int team, const std::vector<Action> &acts, const std::vector<float> &pol) = nullptr, void *ctx = nullptr);

} // namespace duel
} // namespace card
} // namespace cf
