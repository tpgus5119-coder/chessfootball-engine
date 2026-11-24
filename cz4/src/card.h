#pragma once
// **감독 모드** 규칙 엔진 (docs/spec.md # 카드 모드 — 11-19 「카드 없는 버튼 방식」 개편).
// 본편 엔진(engine.h)과 따로 돈다 — 판·골대·박스·좌표 도우미만 같이 쓰고, 규칙은 이 파일과 card.cpp가 전부다.
// 게임 화면(CardNative 바인딩)과 자가학습(card_az · tools/cz)이 같은 코드를 쓴다 — 규칙이 두 벌이면 언젠가 어긋난다.
//
// 요약 (11-19 사용자 「카드 완전 없애라. 체력도」): 손패·덱·콤보·숫자 족보·체력이 없다.
// 수 = 선수 고르기 → 행동(이동·킥·수비) → 칸. 선수마다 고정 수치 — 킥 3 · 수비 3 · 이동 2 + 스탯업 + 직업 보정.
// 킥 힘 = min(킥 수치, 6 − 거리) (하한 1) · 킥 범위 4 · 이동 범위 = 이동 수치(기준 3) · 수비 태세 힘 = 수비 수치.
// 슛 = 슛 길의 경합(땅볼 존)을 뚫고 골대 칸에 닿으면 골 (11-21 골문 체력·슛 데미지 없앰).
// 11-21 사용자 공통 규칙: 차례 종료 수 없음(포인트를 다 쓴다) · 한 차례 안 되돌아가기 금지(사람·공) · 족보 없음(11-24 삭제).
#include "engine.h"

#include <cstdint>

namespace cf {
namespace card {

constexpr int CARD_HALF_TURNS = 60;   // 한 판 = 팀당 30차례 · 전후반 없음 (11-24 사용자 「전후반 없애고 ㄱ」) — 이름은 옛것(반 = 경기 하나)
constexpr int WIN_GOALS = 3;          // 먼저 3골이면 바로 끝 (11-24)
extern int g_time_input;   // 0 = 망 입력에서 시간(반 진행·후반)을 0 으로 (자가학습 CF_NO_TIME)
extern int g_half_turns;   // 반 길이(두 팀 합 차례) — 자가학습만 CF_HALF_TURNS 로 바꾼다
enum CardType { C_KICK = 0, C_MOVE = 1, C_DEF = 2, C_SHOT = 3 };   // 행동 셋 (킥·이동·수비) · C_SHOT = 슛 범위 묻기 전용 (11-22 — 킥 범위 C_KICK 은 패스)
enum Role { R_ATK = 0, R_DEF = 1 };   // 공 가진 팀 = 공격
constexpr int CARD_POOL_N = 9;   // 직업 9 (11-24 셋째 링커 → 마에스트로 합침)

// 선수 기준 수치 (11-19 사용자 · 11-21 이동 3): 킥 3 · 수비 3 · 이동 3 — 여기에 스탯업(bonus)·직업 보정을 더한다
// 기본 스탯 (11-24 사용자): 디펜스 3 · 무브 4 · 패스 5 · 슛 5 — 힘 = 능력치 + 1 − 거리 · 범위 = 기본 능력치 (무브 = 이동·드리블)
// 성장 (11-24 셋째 「전직·성장」): 그 스탯 힘 +1 과 범위 +1 함께 (디펜스는 범위 없음 · 트릭스터 무브 성장은 힘만) — bonus[스탯]
constexpr int BASE_KICK = 5, BASE_DEF = 3, BASE_MOVE = 4;
constexpr int MOVE_RANGE = BASE_MOVE;   // 이동·드리블 범위 4
constexpr int KICK_RANGE = BASE_KICK;   // 패스·슛 범위 5 — 5칸 패스 = 롱패스(공중볼, LONG_PASS_MIN)
// 골키퍼 = 판 위 선수 한 명 (11-24 셋째): 골대 칸에서 시작 · 그 선수 디펜스 = 골대 체력 · 패스 힘·범위 = 롱킥 (keeper_stats).
// 판에 키퍼가 없으면(엔진 시험판) 늘 골대 안에 있는 가상 키퍼 = 기본 선수 값 디펜스 3 · 패스 5 · 범위 5
constexpr int GK_DEF0 = BASE_DEF, GK_KICK0 = BASE_KICK, GK_RANGE0 = KICK_RANGE;

extern int g_goal_hp;   // 측정용 CF_GOAL_HP — 0 이 아니면 두 팀 골키퍼 디펜스를 이 값으로 (규칙 = 팀마다 gk_def)

// A_SAVE = 공 없는 선수 「강하게」(11-22 — 옛 키퍼 킥 번호 자리, dest = 자기 칸) · 7 = 빈 번호(옛 차례 끝·전술) · 9 = 옛 뒷공간 패스 A_RUN (11-22 부터 스루패스가 대신 — 11-24 없앰)
enum Act { A_MOVE = 0, A_DRIBBLE, A_PASS, A_SHOT, A_STANCE, A_PLACE, A_KEEP, A_FREE7, A_SAVE };   // A_KEEP(6) = 키퍼 롱킥 — uid = 받는 아군 · dest = 그 칸 (11-23 · 옛 빈 번호)

struct Player {
	V2 pos = UNPLACED;
	int placed_order = -1;
	int position = -1;   // 아직 드래프트 안 됨
	int uid = -1;
	int team = HOME;
	int stance = 0;        // 수비 태세의 힘 — 0 = 태세 없음
	int stance_left = 0;   // 우리 차례가 이만큼 더 시작되면 풀린다 (1 · 월 2 · 월 성장 3 — 11-24)
	int acted = 0;         // 이번 차례에 행동함 (11-22 사용자 「한 선수는 한 차례에 행동 하나」) — 우리 차례 시작에 0 · 「강하게」 모으기는 안 셈
	int charge = 0;        // 「강하게」 (11-22 셋째 — 사용자 「2액포 = 힘 +1 · 패스·슛·드리블·디펜스, 이동은 없음」): 2 = 이번 차례 다음 패스·슛·드리블·디펜스 1번이 힘 +1 — 포인트 2개짜리 행동의 앞 반 · 쓰거나 공을 잃거나 차례가 끝나면 0
	int bonus[4] = { 0, 0, 0, 0 };   // 성장 — 슛·패스·무브·디펜스 (그 스탯 힘 +1 · 범위 +1 — 11-24 셋째)
	int hold_up = 0;       // 특수 성장: 월 지속 +1 · 타겟맨 버티기 +1
	bool placed() const { return placed_order >= 0; }
};

struct State {
	int phase = PLACEMENT;
	int first_team = HOME;   // 이번 킥오프에서 공 없이 먼저 두는 팀
	int placement_index = 0;
	int turn_team = -1;
	int ball_owner = -1;
	int pts = 0;
	int turn_count = 0;
	int winner = -1;   // GAME_OVER에서 −1 = 무승부
	int half = 1;
	int redeploy = 0;
	V2 half_pos[PLAYERS];
	int half_order[PLAYERS] = {};
	int opener = HOME;
	int goals[2] = { 0, 0 };
	int knockout = 0;   // 1 = 단판 토너먼트 (비기면 연장 — 리그전은 0, 무승부 그대로)
	int ot = 0;         // 1 = 연장 중 (정규 g_half_turns 뒤 · 차례 제한 없는 골든골 — 먼저 넣으면 승, 11-24 사용자)
	int keeper = -1;                    // 슛을 막은 키퍼가 공을 쥔 팀 (−1 = 없음) — 그 팀 다음 차례 첫 수 = 키퍼 롱킥(아군 발밑)
	int gk[2] = { -1, -1 };             // 골키퍼 uid (11-24 셋째 — 판 위 선수, 골대 칸에서 시작) · −1 = 판에 없음(가상 키퍼 — 시험판)
	int ghp[2] = { 0, 0 };
	int gk_def[2] = { GK_DEF0, GK_DEF0 }, gk_kick[2] = { GK_KICK0, GK_KICK0 }, gk_range[2] = { GK_RANGE0, GK_RANGE0 };   // 골키퍼 디펜스(= 골대 체력)·패스 힘·패스 범위 · ghp = 남은 체력 (슛 남은 힘 > 체력이면 골 · 킥오프마다 다시 참 · 키퍼가 골대 밖이면 0 으로 셈)
	int role[2] = { R_DEF, R_ATK };
	int hold = 0;                      // 공 임자가 태클을 버티는 힘 = 그 선수 드리블 능력치 (막 뺏은 공 = 뺏은 선수 태클 능력치) — 11-21 사용자, 차례마다 줄지 않음
	int touch_team = -1, touch_mask = 0;   // 이번 공격에서 공을 만진 아군 (uid 비트) — 가담
	int poss_turn = 0;                     // 공 임자 팀이 바뀐 때의 turn_count (전술 대본 「뺏은 뒤 몇 차례」 — 규칙엔 안 쓴다, 11-22)
		Player players[PLAYERS];
	// 직업 능력은 모두 상시 (11-19 사용자 둘째 결정 — 게이지·켜기 없음). sk_uid·sk_kind 는 옛 「켠 수」 자리 (늘 −1·0)
	int sk_uid = -1;
	int sk_kind = 0;
	unsigned char fired[PLAYERS] = {};
	unsigned char rep[PLAYERS][CARD_POOL_N] = {};   // 대표 행동 수 (기록 — 전직은 보상으로만)
	int tturn[2] = { 0, 0 };
	int st_turn[PLAYERS] = {};
	int team_n = FIELD_N;    // 팀당 필드 선수 (골키퍼 빼고)
	int sk_passive = 1;      // 직업 능력 상시 (11-19 — 모든 경기). 0 = 측정용으로만

	int ball_team() const;
	bool gk_in(int team) const;   // 골키퍼가 자기 골대 칸에 있다 (판에 키퍼가 없으면 늘 참)
	int half_start = 0;
	int half_turn() const { return turn_count - half_start; }
};

extern int g_range_bonus, g_air_max;
void knobs_from_env();   // CF_HALF_TURNS · CF_NO_TIME · 측정용 스위치

struct Action {
	int t = -1;
	int uid = -1;      // 배치는 드래프트 슬롯
	V2 dest{ 0, 0 };
	int card = -1;     // 쓰지 않음 (옛 카드 번호 자리 — 늘 −1)
	int sk = 0;        // 늘 0 (11-19 직업 능력 상시 — 옛 「스킬을 켠 수」 자리)
};

// 수비 태세 존 (11-24 사용자 「모두 사방 4칸 · 1턴 지속」 — 태세 수 = 제자리)
int zone_tiles(const Player &p, V2 out[4]);   // 태세 선수의 존 칸 (판 안·골대·박스 아님) — 태세 없으면 0

enum Use { U_PASS, U_SHOT, U_MOVE, U_DRIBBLE, U_STANCE };
// 선수 수치 (보정 모두 포함 — 판정 힘). want = C_KICK·C_MOVE·C_DEF, use = 쓰임, dist = 킥 거리 (킥만 — min(수치, 6 − 거리))
int act_power(const State &s, const Player &p, int use, int dist = 0);
int act_range(const State &s, const Player &p, int want, int skl = -1);   // 킥·이동 범위 (skl = Action.sk, −1 = 지금 수)
int move_stat(const Player &p);   // 이동 수치 (버티는 힘) — 기준 2 + 스탯업
int kick_stat(const State &s, const Player &p, int use);   // 거리 전 킥 수치 (화면)

// 화면 연출용 기록 (nullptr이면 조용하다 — 탐색 중에는 끈다)
struct Event {
	int kind;   // 0 경합 · 1 밀림 · 2 골 · 4 길 · 5 슛 막힘 · 6 직업 스킬 발동(a uid · b 포지션) · 9 키퍼가 막음
	int a, b;
	V2 at;
	int va, vb, win;
	std::vector<V2> path;
	int vc = 0;   // 슛(2·5): 데미지 힘
};
extern std::vector<Event> *g_events;
extern const std::vector<V2> *g_path;

void enumerate_actions(const State &s, std::vector<Action> &out);
bool apply_action(State &s, const Action &a);
void outcome_paths(const State &s, const Action &a, std::vector<std::vector<V2>> &out);
typedef float (*PathValue)(const State &s, int team, const void *ctx);
bool value_path(const State &s, const Action &a, PathValue val, const void *ctx, std::vector<V2> &out);
int assist_value(const State &s, int team);
void end_turn(State &s);
bool marked(const State &s, const Player &p);
int pool_slot(int position);
bool kickoff_turn(const State &s);

void new_match(State &s, int first);
constexpr int SLOT_PROSPECT = CARD_POOL_N;
// 선수단 5명 (gk = 골키퍼 칸 0~4 · −1 = 디펜스가 가장 높은 선수 — 같으면 뒤 선수, core/career.gd keeper_auto 와 같다). 골키퍼는 골대 칸에 놓인다
void set_lineup(State &s, int team, const int position[PER_TEAM], const int bonus[PER_TEAM][4], int gk = -1);
int eff_hold(const State &s);
void set_perks(State &s, int team, const int hold_up[PER_TEAM]);
// 성장 (11-24 셋째 「전직·성장」): 스탯마다 +1 한 번 (부르는 쪽이 카드를 센다). st = 슛 · 패스 · 무브 · 디펜스 · 특수(월 지속 · 타겟맨 버티기) — core/career.gd grow_ok 와 같다
constexpr int GROW_MAX = 1, GROW_K = 5;
bool grow_ok(int pos, const int st[GROW_K], int k);
enum StatKind { ST_SHOT = 0, ST_PASS = 1, ST_DRIBBLE = 2, ST_DEF = 3 };
// 그 선수가 키퍼일 때 값: out = [디펜스(= 골대 체력) · 롱킥 힘(패스 1칸 힘) · 롱킥 범위(패스 범위)] — 성장·직업 보너스 포함 (헌터 디펜스 +1 · 마에스트로 패스 +1)
void keeper_stats(int position, const int bonus[4], int out[3]);
int default_gk(const int position[PER_TEAM], const int bonus[PER_TEAM][4]);   // 디펜스가 가장 높은 선수 칸 (같으면 뒤)
void set_trial(State &s, int team_n);   // 시험판: 팀 인원 · 스킬 패시브
extern int g_skill_pow;
bool skill_job(int position);
bool skill_can(const State &s, const Player &p);   // 늘 false (11-19 상시 능력 — 켜는 수 없음)
bool skill_act(int position, int act, int sk = 1);
constexpr int SK_KINDS_MAX = 1;
double heuristic(const State &s, int team);

} // namespace card
} // namespace cf
