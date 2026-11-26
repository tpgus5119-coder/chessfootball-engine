#pragma once
// 감독·선수 모드 규칙 엔진 (docs/spec.md 맨 위 「최근 변경」 · docs/현재_게임.md). 규칙은 이 파일과 card.cpp 가 전부다.
// 게임 화면(CardNative 바인딩)과 자가학습(card_az · tools/cz)이 같은 코드를 쓴다 — 규칙이 두 벌이면 언젠가 어긋난다.
//
// 요약: 수 = 선수 고르기 → 행동(이동·드리블·패스·슛·태세·키퍼 롱킥) → 칸. 차례마다 포인트 2 · 차례 끝 수 없음(포인트를 다 쓴다).
// 스탯 = 디펜스 3 · 무브 4 · 패스 5 · 슛 5 + 성장(그 스탯 힘 +1 — 멀리형 자기 분야만 범위 +1) + 전직 혜택. 힘 = 능력치 + 1 − 거리 · 범위 = 능력치.
// 경합(존·태클·슛 대 골대 체력)은 동점이면 공 쪽이 이긴다 (11-26 묶음 7 — ball_wins).
#include "engine.h"

#include <cstdint>

namespace cf {
namespace card {

constexpr int CARD_HALF_TURNS = 60;   // 한 판 = 두 팀 합 60차례 (팀당 30) · 전후반 없음 — 이름은 옛것
constexpr int WIN_GOALS = 3;          // 먼저 3골이면 바로 끝
enum CardType { C_KICK = 0, C_MOVE = 1, C_DEF = 2, C_SHOT = 3 };   // act_range 의 범위 종류 — C_KICK = 패스 · C_SHOT = 슛
enum Role { R_ATK = 0, R_DEF = 1 };   // 공 가진 팀 = 공격
constexpr int CARD_POOL_N = 11;       // 직업 11 (11-26 타워 없앰)

constexpr int BASE_KICK = 5, BASE_DEF = 3, BASE_MOVE = 4;   // 패스·슛 5 · 디펜스 3 · 무브(이동·드리블) 4
constexpr int MOVE_RANGE = BASE_MOVE;
// 규칙 묶음 6 (11-25): 피지컬(버티기·스크린·밀기·마커 잡기) · 체력(행동마다 1 · 태세도 1 · 0 = 행동 불가) 둘 다 기본 3
constexpr int BASE_PHYS = 3, BASE_STAM = 3;
constexpr int KICK_RANGE = BASE_KICK;   // 5칸 패스 = 롱패스(공중볼, LONG_PASS_MIN)
// 골키퍼 = 판 위 선수 한 명: 골대 칸에서 시작 · 그 선수 디펜스 = 골대 체력 · 패스 힘·범위 = 롱킥 (keeper_stats).
// 판에 키퍼가 없으면(엔진 시험판) 늘 골대 안에 있는 가상 키퍼 = 기본 선수 값
constexpr int GK_DEF0 = BASE_DEF, GK_KICK0 = BASE_KICK, GK_RANGE0 = KICK_RANGE;

// 수 종류 — 번호는 화면(ui/card_match.gd)·망 수 번호와 같다. 7 은 빈 번호(옛 차례 끝)
// A_KEEP = 가상 키퍼(판에 키퍼 없는 시험판) 롱킥 (uid = 받는 아군 · dest = 그 칸) · 8 = 빈 번호(옛 「강하게」)
// A_STANCE = 태세 (누구나 · 제자리 · 포인트 1 · 체력 1 — 11-26 묶음 7): 우리 차례 다음 행동 힘·범위 +1 · 안 쓰고 차례가 넘어가면
// 상대 차례 동안 공 없음 = 사방 존(디펜스) · 공 임자 = 버티기(피지컬)
enum Act { A_MOVE = 0, A_DRIBBLE, A_PASS, A_SHOT, A_STANCE, A_PLACE, A_KEEP };

struct Player {
	V2 pos = UNPLACED;
	int placed_order = -1;
	int position = -1;     // 직업 (engine.h Pos) — −1 = 아직 없음·후보
	int uid = -1;
	int team = HOME;
	int stance = 0;        // 수비 태세의 힘 — 0 = 태세 없음
	int stance_left = 0;   // 우리 차례가 이만큼 더 시작되면 풀린다 (1 — 월은 상시 태세라 셈하지 않음 · wall_sync)
	int acted = 0;         // 이번 차례에 행동함 (망 입력) — 태세는 안 셈
	int tai = 0;           // 태세: 1 = 우리 차례 안(다음 행동 힘·범위 +1) · 2 = 상대 차례 동안 선 태세(존·버티기) · 0 = 없음
	int st_ord = 0;        // 태세를 건 순서 (State.st_seq — 존 겹침: 힘 → 먼저 건 선수 · 월 = 늘 맨 먼저)
	int bonus[6] = { 0, 0, 0, 0, 0, 0 };   // 성장 — 슛·패스·무브·디펜스·피지컬·체력 (StatKind)
	int fat = 0;           // 쓴 체력 (남은 체력 = stam_max − fat · 0 = 가득) — 경기 시작 체력은 set_stamina
	int spent = 0;         // 이번 차례에 체력을 썼다 — 안 썼으면 차례 끝에 회복 (rest_gain)
	bool placed() const { return placed_order >= 0; }
};

// 값만 든 구조체 — 자동 저장(card_native.cpp)·탐색 재사용이 바이트로 옮기고 견준다 (채움 바이트가 없어야 한다)
struct State {
	int phase = PLACEMENT;
	int first_team = HOME;   // 이번 킥오프에서 공 없이 먼저 두는 팀
	int placement_index = 0;
	int turn_team = -1;
	int ball_owner = -1;
	int pts = 0;
	int turn_count = 0;
	int winner = -1;   // GAME_OVER에서 −1 = 무승부
	int redeploy = 0;  // 1 = 선수단이 정해져 놓기만 (set_lineup) · 0 = 드래프트
	V2 half_pos[PLAYERS];   // 경기 시작 대형 — 골 뒤 돌아간다
	int half_order[PLAYERS] = {};
	int opener = HOME;
	int goals[2] = { 0, 0 };
	int keeper = -1;                    // 슛을 막은 키퍼가 공을 쥔 팀 (−1 = 없음) — 키퍼가 공을 놓을 때까지 그 팀은 키퍼만(패스·드리블) · 가상 키퍼 = 롱킥
	int gk[2] = { -1, -1 };             // 골키퍼 uid · −1 = 판에 없음(가상 키퍼 — 시험판)
	int ghp[2] = { 0, 0 };              // 골대 남은 체력 (슛 남은 힘 > 체력이면 골 · 킥오프마다 다시 참 · 키퍼가 골대 밖이면 0 으로 셈)
	int gk_def[2] = { GK_DEF0, GK_DEF0 }, gk_kick[2] = { GK_KICK0, GK_KICK0 }, gk_range[2] = { GK_RANGE0, GK_RANGE0 };   // 골키퍼 디펜스(= 골대 체력)·롱킥 힘·롱킥 범위
	int role[2] = { R_DEF, R_ATK };
	int hold = 0;                          // 공 임자가 태클을 버티는 힘 (공을 다룬 그 행동의 남은 힘 · 태클에 깎임)
	int st_seq = 0;                        // 태세 순서 셈 (Player.st_ord)
	int touch_team = -1, touch_mask = 0;   // 이번 공격에서 공을 만진 아군 (uid 비트) — 가담
	int poss_turn = 0;                     // 공 임자 팀이 바뀐 때의 turn_count (전술 대본만 — 규칙엔 안 쓴다)
	Player players[PLAYERS];
	unsigned char rep[PLAYERS][CARD_POOL_N] = {};   // 대표 행동 수 (기록 — 커리어 화면이 센다)
	unsigned char knockout = 0;   // 1 = 단판 토너먼트 (비기면 연장 — 리그전은 0, 무승부 그대로)
	unsigned char ot = 0;         // 1 = 연장 중 (정규 CARD_HALF_TURNS 뒤 · 차례 제한 없는 골든골)
	// 채움 바이트 없음: rep 10×11 + 2 = 112 (4 배수 — 11-26 직업 11, 옛 spare[2] 뺌)
	int tturn[2] = { 0, 0 };      // 팀마다 시작한 차례 수 (월 대표 행동 기록)
	int st_turn[PLAYERS] = {};    // 그 선수가 태세로 끝낸 마지막 차례 (월 대표 행동 기록)
	int team_n = FIELD_N;         // 팀당 필드 선수 (골키퍼 빼고)

	int ball_team() const;
	bool gk_in(int team) const;   // 골키퍼가 자기 골대 칸에 있다 (판에 키퍼가 없으면 늘 참)
};

struct Action {
	int t = -1;
	int uid = -1;      // 배치는 드래프트 슬롯 · 키퍼 롱킥은 받는 선수
	V2 dest{ 0, 0 };
};

// 수비 태세 존 = 곁 네 칸 (판 안·골대·박스 아님) — 태세 없으면 0
int zone_tiles(const Player &p, V2 out[4]);

enum Use { U_PASS, U_SHOT, U_MOVE, U_DRIBBLE, U_STANCE };
int act_power(const State &s, const Player &p, int use, int dist = 0);   // 판정 힘 (보정 모두 포함 · 늘 1 이상) — dist = 킥·드리블 거리
int act_range(const State &s, const Player &p, int want);                // 범위 — want = C_KICK(패스)·C_SHOT·C_MOVE
int move_stat(const Player &p);   // 무브 수치 (거리 전)

int phys(const Player &p);       // 피지컬 = 3 + 성장 + 타이탄·마커 +1
int stam_max(const Player &p);   // 체력 최대 = 3 + 성장 + 엔진 +1
int stam(const Player &p);       // 남은 체력
int rest_gain(int position);     // 경기 사이 회복 = 1 (엔진 2) — 경기 안 「행동 안 한 차례 끝」 회복은 누구나 REST_TURN
// 경기 안 체력 (행동·태세마다 −1 · 쉬면 +1 · 0 = 못 움직임). false = 체력은 줄지 않음
constexpr bool MATCH_STAMINA = true;
constexpr int REST_TURN = 1;     // 경기 안: 그 차례에 체력을 안 쓴 선수 회복 (사용자 11-25 수정 — 엔진도 같음)
// 스크린 판정 방식 (사용자 고민 중 — 여기 한 줄만 바꾸면 된다): 0 옛 규칙(아군이 서 있기만) · 1 태세만 요구 · 2 태세 + 피지컬 > 그 칸 수비수 (지금)
constexpr int SCREEN_MODE = 1;
void set_stamina(State &s, int team, const int st[PER_TEAM]);   // 경기 시작 체력 (선수단 칸 순서 = uid 순서 · 0~최대로 자름)

// 화면 연출용 기록 (nullptr이면 조용하다 — 탐색 중에는 끈다)
struct Event {
	int kind;   // 0 경합 · 1 밀림 · 2 골 · 4 길(이동·드리블·패스·슛·롱킥) · 9 키퍼가 막음
	int a, b;
	V2 at;
	int va, vb, win;
	std::vector<V2> path;
};
extern std::vector<Event> *g_events;
extern const std::vector<V2> *g_path;   // 유저가 그린 길 (nullptr = 엔진이 고른다)

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
void set_lineup(State &s, int team, const int position[PER_TEAM], const int bonus[PER_TEAM][6], int gk = -1);
// 성장: 스탯마다 +1 한 번 (부르는 쪽이 카드를 센다). k = 슛 · 패스 · 무브 · 디펜스 — 특수 성장 칸 없음 (규칙 묶음 5) · core/career.gd grow_ok 와 같다
constexpr int GROW_MAX = 1, GROW_K = 6, GROW_PICKS = 4;   // 스탯 여섯 중 넷만 +1 (11-26 묶음 7 — 만랩 = 전직 1 + 4 = 5)
bool grow_ok(int pos, const int st[GROW_K], int k);
enum StatKind { ST_SHOT = 0, ST_PASS = 1, ST_DRIBBLE = 2, ST_DEF = 3, ST_PHYS = 4, ST_STAM = 5 };
// 그 선수가 키퍼일 때 값: out = [디펜스(= 골대 체력) · 롱킥 힘(패스 1칸 힘) · 롱킥 범위(패스 범위)] — 성장·전직 혜택 포함
void keeper_stats(int position, const int bonus[GROW_K], int out[3]);
int default_gk(const int position[PER_TEAM], const int bonus[PER_TEAM][GROW_K]);   // 디펜스가 가장 높은 선수 칸 (같으면 뒤)
void set_trial(State &s, int team_n);   // 시험판: 팀 필드 인원
double heuristic(const State &s, int team);   // 손평가 (망 입력 d[13] · 망 없는 탐색 가치)

} // namespace card
} // namespace cf
