#pragma once
// **감독 모드** 규칙 엔진 (docs/spec.md # 카드 모드 — 11-19 「카드 없는 버튼 방식」 개편).
// 본편 엔진(engine.h)과 따로 돈다 — 판·골대·박스·좌표 도우미만 같이 쓰고, 규칙은 이 파일과 card.cpp가 전부다.
// 게임 화면(CardNative 바인딩)과 자가학습(card_az · tools/cz)이 같은 코드를 쓴다 — 규칙이 두 벌이면 언젠가 어긋난다.
//
// 요약 (11-19 사용자 「카드 완전 없애라. 체력도」): 손패·덱·콤보·숫자 족보·체력이 없다.
// 수 = 선수 고르기 → 행동(이동·킥·수비) → 칸. 선수마다 고정 수치 — 킥 3 · 수비 3 · 이동 2 + 스탯업 + 직업·족보 보정.
// 킥 힘 = min(킥 수치, 6 − 거리) (하한 1) · 킥 범위 4 · 이동 범위 = 이동 수치(기준 3) · 수비 태세 힘 = 수비 수치.
// 슛 = 슛 길의 경합(땅볼 존)을 뚫고 골대 칸에 닿으면 골 (11-21 골문 체력·슛 데미지 없앰).
// 11-21 사용자 공통 규칙: 차례 종료 수 없음(포인트를 다 쓴다) · 한 차례 안 되돌아가기 금지(사람·공) ·
// 빈 칸 패스 = 수비 뒷공간 + 받을 동료가 같이 뛰어감(A_RUN) · 축구 족보 6개 — 걸린 순간 행동 포인트 +1, 한 차례 한 번.
#include "engine.h"

#include <cstdint>

namespace cf {
namespace card {

constexpr int CARD_HALF_TURNS = 60;   // 한 판 = 팀당 30차례 · 전후반 없음 (11-24 사용자 「전후반 없애고 ㄱ」) — 이름은 옛것(반 = 경기 하나)
constexpr int WIN_GOALS = 3;          // 먼저 3골이면 바로 끝 (11-24)
constexpr int RECEIVE_HOLD = 3;   // 리시버가 공을 잡을 때 버티는 힘
constexpr int GK_DEF0 = 3, GK_KICK0 = 4, GK_RANGE0 = 4;   // 골키퍼 기본: 디펜스(= 체력) 3 · 킥 힘 4 · 킥 범위 4 (11-24 사용자)
extern int g_time_input;   // 0 = 망 입력에서 시간(반 진행·후반)을 0 으로 (자가학습 CF_NO_TIME)
extern int g_half_turns;   // 반 길이(두 팀 합 차례) — 자가학습만 CF_HALF_TURNS 로 바꾼다
enum CardType { C_KICK = 0, C_MOVE = 1, C_DEF = 2, C_SHOT = 3 };   // 행동 셋 (킥·이동·수비) · C_SHOT = 슛 범위 묻기 전용 (11-22 — 킥 범위 C_KICK 은 패스)
enum Role { R_ATK = 0, R_DEF = 1 };   // 공 가진 팀 = 공격
constexpr int CARD_POOL_N = 10;
constexpr int TEAM_N = 4;   // 4대4 (11-19) — 팀마다 uid 0~3 · 5~8 만 판에 선다 (PER_TEAM 5 는 배열 자리)

// 선수 기준 수치 (11-19 사용자 · 11-21 이동 3): 킥 3 · 수비 3 · 이동 3 — 여기에 스탯업(bonus)·직업·족보 보정을 더한다
constexpr int BASE_KICK = 4, BASE_DEF = 3, BASE_MOVE = 3;
constexpr int MOVE_RANGE = 3;      // 무브 범위 3 고정 — 무브 수치(힘)와 따로 (11-22 사용자 「무브 범위 = 힘 아님」) · 스프린터만 늘어남   // 11-22 사용자 「계산식 하나 · 범위 = 능력치」: 힘 = 능력치 + 1 − 거리 · 킥 4 (옛 「3 + 2 − 거리」와 같은 값)
constexpr int KICK_RANGE = 4;     // 킥 범위 상한 (범위 보너스만큼 늘어남)

// 축구 족보 6개 (11-21 사용자 — 두 팀 모두, 옛 9개 없앰). 모두 그 한 수에서 바로 판정 · 혜택 = 걸린 순간 행동 포인트 +1 · 같은 족보는 한 차례에 한 번.
// 표 g_jk 한 곳(이름·켬) — 화면 이름도 이 표 (CardNative.jk_table)
enum Jokbo { JK_GEGEN = 0, JK_WALL, JK_TRIANGLE, JK_SUPPORT, JK_FORWARD, JK_BREAK, N_JK };
struct JkInfo { const char *name; int on; };   // on 0 = 꺼진 족보 (안 걸린다)
extern JkInfo g_jk[N_JK];
extern int g_goal_hp;   // 측정용 CF_GOAL_HP — 0 이 아니면 두 팀 골키퍼 디펜스를 이 값으로 (규칙 = 팀마다 gk_def)
extern int g_zone4;   // 0 = 곁 한 칸 방향 존 · 월만 사방 (규칙, 11-23) · 1 = 모든 선수 4방 존 (비교용)
constexpr int JK_NEAR = 2;   // 지원·삼각 패스의 「2칸 안」(맨해튼)
constexpr int JK_AHEAD = 3;  // 전진 패스·돌파의 「3칸 이상 앞」

// A_SAVE = 공 없는 선수 「강하게」(11-22 — 옛 키퍼 킥 번호 자리, dest = 자기 칸) · A_RUN = 뒷공간 패스(빈 칸 + 받을 동료 mate 가 같이 뛰어감, 11-21) · 6·7 = 빈 번호(옛 차례 끝·전술)
enum Act { A_MOVE = 0, A_DRIBBLE, A_PASS, A_SHOT, A_STANCE, A_PLACE, A_KEEP, A_FREE7, A_SAVE, A_RUN };   // A_KEEP(6) = 키퍼 롱킥 — uid = 받는 아군 · dest = 그 칸 (11-23 · 옛 빈 번호)

struct Player {
	V2 pos = UNPLACED;
	int placed_order = -1;
	int position = -1;   // 아직 드래프트 안 됨
	int uid = -1;
	int team = HOME;
	int stance = 0;        // 수비 태세의 힘 — 0 = 태세 없음
	int stance_left = 0;   // 우리 차례가 이만큼 더 시작되면 풀린다 (1 · 월은 2 — 11-21 사용자 「월 2턴 4방 존」)
	int acted = 0;         // 이번 차례에 행동함 (11-22 사용자 「한 선수는 한 차례에 행동 하나」) — 우리 차례 시작에 0 · 「강하게」 모으기는 안 셈
	int charge = 0;        // 「강하게」 (11-22 셋째 — 사용자 「2액포 = 힘 +1 · 패스·슛·드리블·디펜스, 이동은 없음」): 2 = 이번 차례 다음 패스·슛·드리블·디펜스 1번이 힘 +1 — 포인트 2개짜리 행동의 앞 반 · 쓰거나 공을 잃거나 차례가 끝나면 0
	int jk_up = 0;         // 족보 +1 토큰 (11-21 사용자): 1 이면 모든 판정 힘 +1 (겹치지 않음) · 우리 다음 차례 시작에 없어짐
	int zdir = -1;         // 존 방향 (11-21 사용자): DIRS 번호 0~3 = 곁 한 칸 · ZD_ALL = 사방 4칸(월). stance 0 이면 뜻 없음
	int bonus[4] = { 0, 0, 0, 0 };   // 스탯업 — 슛·패스(= 킥)·드리블(= 이동)·수비
	int range_up = 0;      // 이동 범위 +1 (옛 전성기 — 저장 호환)
	int kick_range_up = 0; // 킥 범위 +1
	int hold_up = 0;
	bool placed() const { return placed_order >= 0; }
};

struct State {
	int phase = PLACEMENT;
	int first_team = HOME;   // 이번 킥오프에서 공 없이 먼저 두는 팀
	int placement_index = 0;
	int turn_team = -1;
	int ball_owner = -1;
	V2 ball_pos = UNPLACED;   // 루즈볼
	int loose_team = -1;
	int pts = 0;
	int turn_count = 0;
	int winner = -1;   // GAME_OVER에서 −1 = 무승부
	int half = 1;
	int redeploy = 0;
	V2 half_pos[PLAYERS];
	int half_order[PLAYERS] = {};
	int opener = HOME;
	int goals[2] = { 0, 0 };
	int keeper = -1;                    // 슛을 막은 키퍼가 공을 쥔 팀 (−1 = 없음) — 그 팀 다음 차례 첫 수 = 키퍼 롱킥(포인트 2 · 필드 위 아군 고르기, 11-23 사용자)
	int ghp[2] = { 0, 0 };
	int gk_def[2] = { GK_DEF0, GK_DEF0 }, gk_kick[2] = { GK_KICK0, GK_KICK0 }, gk_range[2] = { GK_RANGE0, GK_RANGE0 };   // 골키퍼 스탯 (성장 = 디펜스 +1 · 킥 범위 +1 · 하나 더 아무 쪽)              // 팀 t 골대(키퍼) 남은 체력 — 슛의 남은 힘만큼 깎이고 0 이하면 골 · 킥오프(경기·반 시작·골 뒤)마다 다시 참 (11-23 사용자)
	int role[2] = { R_DEF, R_ATK };
	int hold = 0;                      // 공 임자가 태클을 버티는 힘 = 그 선수 드리블 능력치 (막 뺏은 공 = 뺏은 선수 태클 능력치) — 11-21 사용자, 차례마다 줄지 않음
	int touch_team = -1, touch_mask = 0;   // 이번 공격에서 공을 만진 아군 (uid 비트) — 가담
	int poss_turn = 0;                     // 공 임자 팀이 바뀐 때의 turn_count (전술 대본 「뺏은 뒤 몇 차례」 — 규칙엔 안 쓴다, 11-22)
	// 족보 (11-21): jk_turn = 그 팀 지금 차례에 걸린 족보 비트 · jk_next = 남의 차례에 걸려 그 팀 다음 차례 몫(+1 은 그 차례 시작에 더함) · jk_count = 판 누적
	int jk_turn[2] = { 0, 0 };
	int jk_next[2] = { 0, 0 };
	int jk_count[2][N_JK] = {};
		Player players[PLAYERS];
	// 직업 능력은 모두 상시 (11-19 사용자 둘째 결정 — 게이지·켜기 없음). sk_uid·sk_kind 는 옛 「켠 수」 자리 (늘 −1·0)
	int sk_uid = -1;
	int sk_kind = 0;
	unsigned char fired[PLAYERS] = {};
	unsigned char rep[PLAYERS][CARD_POOL_N] = {};   // 대표 행동 수 (기록 — 전직은 보상으로만)
	int tturn[2] = { 0, 0 };
	int st_turn[PLAYERS] = {};
	int team_n = TEAM_N;     // 팀당 출전 인원
	int sk_passive = 1;      // 직업 능력 상시 (11-19 — 모든 경기). 0 = 측정용으로만

	int ball_team() const;
	bool loose_ball() const { return ball_owner == -1 && ball_pos != UNPLACED; }
	int half_start = 0;
	int half_turn() const { return turn_count - half_start; }
};

extern int g_range_bonus, g_air_max;
void knobs_from_env();   // CF_HALF_TURNS · CF_NO_TIME · CF_JK (족보 켬 덮어쓰기)

struct Action {
	int t = -1;
	int uid = -1;      // 배치는 드래프트 슬롯
	V2 dest{ 0, 0 };
	int card = -1;     // 쓰지 않음 (옛 카드 번호 자리 — 늘 −1)
	int sk = 0;        // 늘 0 (11-19 직업 능력 상시 — 옛 「스킬을 켠 수」 자리)
	int mate = -1;     // 뒷공간 패스(A_RUN)의 받을 동료 uid (11-21) — 그 밖 −1
};

// 수비 태세 존 (11-21 사용자 「곁 한 칸을 골라 존 1칸 · 방향 공개 · 월 = 사방 4칸 1차례」)
constexpr int ZD_ALL = 4;
int zone_tiles(const Player &p, V2 out[4]);   // 태세 선수의 존 칸 (판 안·골대 아님) — 태세 없으면 0
int stance_dir(const Player &p, V2 dest);     // 태세 수의 칸 → 존 방향 (월은 제자리 = ZD_ALL), 안 되는 칸 −1

enum Use { U_PASS, U_SHOT, U_MOVE, U_DRIBBLE, U_STANCE };
// 선수 수치 (보정 모두 포함 — 판정 힘). want = C_KICK·C_MOVE·C_DEF, use = 쓰임, dist = 킥 거리 (킥만 — min(수치, 6 − 거리))
int act_power(const State &s, const Player &p, int use, int dist = 0);
int act_range(const State &s, const Player &p, int want, int skl = -1);   // 킥·이동 범위 (skl = Action.sk, −1 = 지금 수)
int move_stat(const Player &p);   // 이동 수치 (버티는 힘) — 기준 2 + 스탯업
int kick_stat(const State &s, const Player &p, int use);   // 거리 전 킥 수치 (화면)

// 화면 연출용 기록 (nullptr이면 조용하다 — 탐색 중에는 끈다)
struct Event {
	int kind;   // 0 경합 · 1 밀림 · 2 골 · 4 길 · 5 슛 막힘 · 6 직업 스킬 발동(a uid · b 포지션) · 7 족보(a 팀 · b 족보 번호)
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
bool jk_enabled(const State &s, int team, int k);   // 그 팀에 그 족보가 걸릴 수 있나 — 지금은 표의 on (팀마다 달라지면 여기 한 곳)
inline int jokbo_mask(const State &s, int team) { return s.turn_team == team ? s.jk_turn[team] : s.jk_next[team]; }   // 그 팀 지금(다음) 차례에 이미 걸린 족보
void turn_clear(State &s);   // 이번 차례 기록(지나간 칸·족보)을 새로 — 차례 시작·일기토 판
void end_turn(State &s);
bool marked(const State &s, const Player &p);
int pool_slot(int position);
bool kickoff_turn(const State &s);

void new_match(State &s, int first);
constexpr int SLOT_PROSPECT = CARD_POOL_N;
void set_lineup(State &s, int team, const int position[PER_TEAM], const int bonus[PER_TEAM][4]);
int eff_hold(const State &s);
void set_perks(State &s, int team, const int range_up[PER_TEAM], const int hold_up[PER_TEAM], const int kick_range_up[PER_TEAM] = nullptr);
// 성장 (11-22 밤 사용자 「주 능력 강제 빼고 두 번째는 카드 2장」): 전직한 선수는 스탯마다 +GROW_MAX 까지 — 첫 +1 = 카드 1장 · 두 번째 = 2장 (부르는 쪽이 센다).
// st = 슛 · 패스 · 무브 · 디펜스 · 특수(범위·지속) — core/career.gd grow_ok 와 같다
constexpr int GROW_MAX = 1, GROW_K = 5;   // 모든 스탯 +1 한 번 (11-23 사용자 「메인이 아닌 스탯도 +1 까지」 — 전직이 스탯 +1 인 직업만 그 스탯 최종 +2)
bool grow_ok(int pos, const int st[GROW_K], int k);
enum StatKind { ST_SHOT = 0, ST_PASS = 1, ST_DRIBBLE = 2, ST_DEF = 3 };
void set_keeper(State &s, int team, int def, int kick, int range);   // 골키퍼 스탯 (커리어 성장)
void set_trial(State &s, int team_n);   // 시험판: 팀 인원 · 스킬 패시브
extern int g_skill_pow;
bool skill_job(int position);
bool skill_can(const State &s, const Player &p);   // 늘 false (11-19 상시 능력 — 켜는 수 없음)
bool skill_act(int position, int act, int sk = 1);
constexpr int SK_KINDS_MAX = 2;
double heuristic(const State &s, int team);

} // namespace card
} // namespace cf
