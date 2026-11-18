#pragma once
// **카드 모드** 규칙 엔진 (docs/spec.md # 카드 모드 — 사용자 확정 2026-10-28~29).
// 본편 엔진(engine.h)과 따로 돈다 — 판·골대·박스·좌표 도우미만 같이 쓰고, 규칙은 이 파일과 card.cpp가 전부다.
// 게임 화면(CardNative 바인딩)과 자가학습(card_az)이 같은 코드를 쓴다 — 규칙이 두 벌이면 언젠가 어긋난다.
//
// 요약: 액션 버튼·스탯·범위·체력·기세가 없다. 손의 **카드**(킥·이동·수비 + 숫자)가 액션이고, 숫자가 힘이고 범위는 킥 6 − 숫자 · 이동 4 − 숫자다 (v14, 2026-11-09).
// v17: 슛 데미지 = 슛 힘 × (콤보 합 + 역습 + 가담) · 골문 체력 이상이면 골, 모자라면 막힘 · 골 수 → 같으면 골 숫자 합으로 승패.
// v17: 족보(스트레이트·로열·포카드·풀하우스)를 완성하면 그 공격이 끝날 때까지 범위·힘·포인트 혜택.
#include "engine.h"

namespace cf {
namespace card {


constexpr int CARD_HALF_TURNS = 50;
extern int g_time_input;   // 0 = 망 입력에서 시간(반 진행·후반)을 0 으로 — 100턴 자가학습 동안 (11-18 사용자 정정: 시간을 모르고 「골 넣고 안 먹히기」만)
extern int g_half_turns;   // 반 길이(두 팀 합 차례) — 기본 CARD_HALF_TURNS · 자가학습만 CF_HALF_TURNS 로 바꾼다 (11-18 사용자 「전반 100턴·후반 100턴」)
   // 카드 모드 반 = 팀당 25턴 (사용자 2026-11-06) · 옛 20턴 (사용자 2026-11-04 — 본편 HALF_TURNS 60과 따로)
enum CardType { C_KICK = 0, C_MOVE = 1, C_DEF = 2 };
enum Role { R_ATK = 0, R_DEF = 1 };   // 공 가진 팀 = 킥·이동 · 아닌 팀 = 수비·이동
// 전술카드 9종 (v8.2 — HP 깎기·바꿔 들기 뺌). 화면 이름: 프리롤·로테이션·봉쇄·압박·리플레이·닥공·강슛·숨 고르기·육탄방어
// 압박(T_BIND): 우리 선수 곁(상하좌우)의 적 하나가 다음 상대 차례에 못 움직인다 · 리플레이(T_UNDO): 방금 낸 손 카드를 되돌린다
// 닥공(T_ADVANCE): 다음 카드는 콤보 +2 · 강슛(T_DOUBLE): 다음 슛 피해 ×2 · 숨 고르기(T_CARRY): 남은 포인트를 넘기고 턴 끝 · 육탄방어(T_LOCK): 다음 상대 차례 슛 피해 절반
enum Tac { T_NEWHAND = 0, T_DISCARD = 1, T_BIND = 2, T_UNDO = 3, T_ADVANCE = 4, T_DOUBLE = 5, T_CARRY = 6, T_LOCK = 7 };   // v8.6 프리롤(조커) 뺌 — 사용자 "프리롤 빼" (노마드가 그 몫)
constexpr int N_TAC = 8;
constexpr bool TACTICS_ON = false;   // v10.1 사용자 "전술카드는 일단 빼봐" — 규칙 코드는 그대로 두고 경기에 가져가지 않는다 (다시 켜면 true)
// v12 선수 10종 (사용자 2026-11-03 — 본편 v7 10종과 같은 이름·짝): 캐논(킥 +1)·마에스트로(킥 범위 +1)·트릭스터(이동 +1)·스프린터(이동 범위 +1)·
// 마커(붙은 적 카드 −1)·부스터(붙은 아군 힘 +1)·타이탄(착지 밀기)·헌터(수비 +1)·월(태세 지속 +1)·노마드(자기가 낼 때 이동 ↔ 킥/수비 바꿔 쓰기 — v10.4 프리롤의 능력).
// 옛 노마드(콤보를 안 깸)는 없앴다. 선발은 팀 안에서만 겹치지 않는다
constexpr int CARD_POOL_N = 10;
// 전술카드는 **어느 것도 콤보를 끊지 않는다** — 포인트 1만 쓴다 (v8.0 사용자 "전술카드 모두 콤보 끊지말고, 액션포인트만 1개")
constexpr int FULL_HAND = 0x3F;

// 손 칸 0~2 = 킥(공격 팀)·수비(수비 팀) 2·3·4, 칸 3~5 = 이동 1·2·3
inline int slot_value(int slot) { return slot < 3 ? slot + 2 : slot - 2; }
inline int slot_type(int role, int slot) { return slot < 3 ? (role == R_ATK ? C_KICK : C_DEF) : C_MOVE; }
// **스킬 카드** (사용자 2026-11-07 — 39장). 팀의 손 칸에 붙는다. 0 = 기본 카드. 콤보는 늘 카드에 찍힌 숫자로 (보정 전)
enum Skill { SK_NONE = 0,
	K_SHOT_UP, K_PASS_UP, K_LONG, K_HEAVY, K_POWER, K_KILL, K_RUN, K_DIRECT, K_ONE, K_CORNER, K_BUILD, K_STRIKER, K_TEAM, K_LINK, K_COUNTER,
	M_OFF, M_TRICK, M_DASH, M_SHORT, M_POWER, M_STRAIGHT, M_SIDE, M_ALONE, M_ESCAPE, M_BOX, M_COUNTER, M_RETURN, M_COMBO,
	D_WALL, D_WIDE, D_BODY, D_FRONT, D_SIDE, D_SAVE, D_LAST, D_LOW, D_HIGH, D_REGAIN, D_COMBO,
	K_KEEP, M_KEEP,
	// 체력 카드 6장 (사용자 2026-11-13 — 컵 커리어 3칸 체력): 힘 −1 이면 체력을 안 쓰고, 힘 +1 이면 체력 −2
	K_EASY, K_ALLOUT, M_BREATH, M_SPURT, D_EASE, D_DIVE, N_SKILL };
// 망 입력은 체력 카드 전의 42장만 안다 (11-13 — 학습 중인 망 형식 그대로). 그 뒤 번호는 망에 「기본」으로 보인다
constexpr int N_SKILL_NET = K_EASY;
// 뺀 전술카드 9장 (사용자 2026-11-14) + 5장 (11-18 — 16 오프더볼 · 17 드리블러 · 27 리커버리 · 33 측면 봉쇄 · 7 스루패스는 6 킬패스로 합침): 덱·보상·AI 덱에 안 들어간다. 번호는 망 입력 칸 자리라 그대로 둔다
constexpr bool skill_removed(int sk) {
	return sk == K_POWER || sk == K_ONE || sk == K_CORNER || sk == M_POWER || sk == D_BODY || sk == D_SAVE || sk == K_ALLOUT || sk == M_SPURT || sk == D_DIVE ||
			sk == K_RUN || sk == M_OFF || sk == M_TRICK || sk == M_RETURN || sk == D_SIDE;
}
// 체력 (사용자 2026-11-13, 컵 커리어만): 카드를 낸 선수 −1 · 차례 동안 카드를 안 낸 선수는 그 차례 끝에 +1 (경기 시작 칸까지)
constexpr int STAM_MAX = 3;
int stam_cost(int sk);   // 그 스킬 카드를 낼 때 드는 체력 (기본 1 · 가볍게 0 · 혼신 2)   // 콤보 유지 (사용자 2026-11-07) — 이 카드는 콤보를 늘리지도 끊지도 않는다
constexpr int N_CARD = 6;   // 카드 번호 = 손 칸 0~5

enum Act { A_MOVE = 0, A_DRIBBLE, A_PASS, A_SHOT, A_STANCE, A_PLACE, A_END, A_TACTIC, A_SAVE };   // A_SAVE = v11 선방 롱패스 (card = 버릴 두 장 c1·16+c2, dest = 받을 칸)
constexpr int N_ACT = 8;

struct Player {
	V2 pos = UNPLACED;
	int placed_order = -1;
	int position = -1;   // 아직 드래프트 안 됨
	int uid = -1;
	int team = HOME;
	int stance = 0;        // 수비 태세의 힘 (카드 숫자 + 헌터 1) — 0 = 태세 없음
	int stance_left = 0;   // 우리 차례가 이만큼 더 시작되면 풀린다 (1, 월은 2)
	int stance_sk = 0;     // 이 태세를 만든 수비 스킬
	int bonus[4] = { 0, 0, 0, 0 };
	int range_up = 0;      // 컵 전성기 스프린터: 이동(드리블) 범위 +1 (사용자 2026-11-09)
	int kick_range_up = 0; // 컵 전성기 마에스트로: 킥(패스·슛) 범위 +1 — 패스 스탯 대신 (사용자 2026-11-09)
	int hold_up = 0;       // 컵 전성기 월: 수비 태세 지속 +1 (사용자 2026-11-09)   // 컵 선수 스탯 — 슛·패스·드리블·수비 (사용자 2026-11-08). 카드를 그 쓰임으로 낼 때만 더한다
	int bound = 0;         // 묶기 — 1이면 이번(다음) 자기 팀 차례에 이동·드리블을 못 한다. 그 차례가 끝나면 풀린다
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
	V2 half_pos[PLAYERS];        // 이 반을 시작할 때 선 자리 — 골이 나면 그대로 돌아간다 (v8.3 사용자 "득점하면 다시 배치하지 말고")
	int half_order[PLAYERS] = {};
	int opener = HOME;
	int lost[2] = { 0, 0 };    // 그 팀 골대에 들어간 골 숫자(데미지) 합 — 막힌 슛은 안 든다. 골 수가 같으면 더 크게 넣은 팀이 이긴다
	int goals[2] = { 0, 0 };   // 성공한 슛 수 (기록)
	int keeper[2] = { 0, 0 };  // 그 팀 골문(골키퍼)의 남은 체력 — g_keeper_hp > 0일 때만 (v16 다시 규칙). 반 시작·그 골문에 골이 나면 다시 채운다
	int save_team = -1;        // v11: 슛을 막은 팀 — 이 팀 차례 첫 수는 선방 롱패스(A_SAVE)뿐
	int hand[2] = { FULL_HAND, FULL_HAND };
	int role[2] = { R_DEF, R_ATK };
	int tac[2] = { 0, 0 };       // 이번 반에 아직 쓸 수 있는 전술카드 (비트 = Tac)
	int tac_sel[2] = { 0, 0 };   // 이 경기에 가져온 전술카드 3종 (경기에 한 번씩 — v9.0)
	int combo[2] = { 0, 0 }, combo_last[2] = { 0, 0 };   // 팀마다 (v8.4 — 수비 태세 카드도 콤보를 쌓는다, 사용자 2026-10-29 "수비태세 카드도 콤보로")
	V2 left_from[PLAYERS] = { UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET };   // 이번 차례에 그 선수가 마지막 이동·드리블로 떠난 칸 — 거기로 바로 되돌아가지 못한다 (v8.4)
	int last_card[2] = { -1, -1 };   // 이번 차례에 방금 낸 손 카드 칸 (되감기 대상, −1 = 없음)
	int adv_next[2] = { 0, 0 };      // 전진 명령 — 이번 차례의 다음 카드는 콤보 +2
	int dbl[2] = { 0, 0 };           // 두 배 — 다음 슛 피해 ×2
	int carry[2] = { 0, 0 };         // 포인트 이월 — 다음 차례에 더 받을 포인트
	int lock[2] = { 0, 0 };          // 골문 잠그기 — 다음 상대 차례 동안 우리 골대에 들어오는 슛 피해 절반
	int last_dmg = 0, last_hit_team = -1;   // 방금 성공한 슛 (화면용)
	int refill_n[2] = { 0, 0 };   // 낼 카드가 없어 새 손을 받은 횟수 (화면용 — 지문에 안 넣는다)
	int hold = 0;                      // 공 임자가 태클을 버티는 힘 = 공을 다룬 직전 숫자 (받은 패스·드리블·주운 이동·뺏은 수비 — v9.0)
	int hold_skip = 0;                 // 1 = 상대 차례에 공을 얻었다 — 우리 다음 차례 시작에는 버티는 힘을 안 깎는다 (2026-11-04 매 차례 −1)
	int touch_team = -1, touch_mask = 0;   // 이번 공격에서 공을 만진 아군(uid 비트) — 가담 = 1명당 슛 데미지 +1 (v16, 최대 5)
	int hand_kick[2] = { 0, 0 };   // 옛 손 모양 자리 — 11-17 사용자 「손패 숫자 고정」으로 늘 0 (킥·수비 2-3-4 · 이동 1-2-3). 망 입력 칸 자리 맞춤으로만 남김
	int hand_move[2] = { 0, 0 };
	int sk_atk[2][N_CARD] = {};    // 칸 0~2 = 킥 스킬 · 칸 3~5 = 이동 스킬
	int sk_def[2][3] = {};         // 칸 0~2 = 수비 스킬 (같은 칸이 공수에 따라 킥 ↔ 수비)
	int poss_now[2] = { 0, 0 };    // 공을 얻은 첫 턴 (상대 차례가 시작되면 끝)
	int lost_pending[2] = { 0, 0 }, lost_now[2] = { 0, 0 };   // 공을 잃은 뒤 우리 첫 턴
	int lose_fresh[2] = { 0, 0 };   // 공을 잃은 뒤 그 팀이 아직 새 손을 안 받았다 — 리커버리·되찾기 (11-14). 공을 얻은 쪽은 ctr_on (역습 보너스·빠른 전개 하나로)
	int rb_kick[2] = { 0, 0 }, rb_move[2] = { 0, 0 };   // 패스를 받은 아군의 다음 킥·이동 +1 (킬패스·침투 패스)
	int half_dmg = 0;              // 이번 슛 길에서 몸으로 막기 수비수가 졌다
	int save_dist = 0, save_pow = 0;   // 키퍼 킥 첫 장을 확정했으면 그 거리·힘 (0 = 아직, v15)
	// 역습 (v16 · 11-14 정정): 공을 얻은 팀 모두(탈취·킥오프·키퍼 킥 — 빠른 전개 카드와 같은 조건) — 뺏은 자리 x 에서 앞으로 간 칸 수만큼 슛 데미지 +1.
	// 그 팀이 새 손을 받거나 공을 잃으면 끝 (ctr_on = 0). 값은 counter_bonus()
	int ctr_on[2] = { 0, 0 }, ctr_x[2] = { 0, 0 };
	int steal_turn[2] = { -1, -1 };    // 측정용: 그 팀이 탈취(태클·가로채기)로 공을 얻은 turn_count (−1 = 없음)
	int gk_kicks[2] = { 0, 0 };        // 측정용: 키퍼 킥 1장 · 2장 횟수
	int blocks[2] = { 0, 0 };          // 측정용: 골문 체력에 막힌 슛 수 (쏜 팀)
	int last_ctr = 0;                  // 방금 골문에 닿은 슛(골·막힘)의 역습 보너스 (측정·화면용)
	int last_pw = 0;                   // 방금 골문에 닿은 슛의 데미지 힘 (v17 — 데미지 = 이 값 × 괄호 합, 측정·화면용)
	// 족보 (v17 사용자 2026-11-11 「족보혜택 ㄱ」): 이번 공격에서 공 가진 팀이 낸 카드 숫자의 마지막 7장(10진 자리, 숫자 1~4)과
	// 이번 공격에 완성한 족보(JK_* 비트). 공을 잃거나 슛하면(공격이 끝나면) 둘 다 0
	int jk_seq[2] = { 0, 0 }, jk_done[2] = { 0, 0 };
	int jk_count[2][4] = {};           // 측정용: 이 판에 완성한 족보 수 (스트레이트·로열·포카드·풀하우스)
	Player players[PLAYERS];
	// 체력 (11-13, 컵 커리어만 켠다 — stam_on = 0 이면 규칙 그대로). stam = 지금 칸 · stam_cap = 경기 시작 칸 · acted = 이번 차례에 카드를 낸 선수(uid 비트)
	int stam_on = 0;
	int stam[PLAYERS] = {}, stam_cap[PLAYERS] = {};
	int acted = 0;
	// 직업 스킬 (11-14, 컵 커리어만 켠다 — car_on = 0 이면 규칙 그대로). docs/spec.md 「### 직업 패시브 5 · 액티브 5」
	// rep = 이 경기 대표 행동 수(직업 순서 pool_slot — 전직 조건) · fired = 스킬을 켠 수 · sk_uid = 지금 스킬을 켜고 카드를 내는 선수 (수 안에서만, −1 = 없음)
	// tturn = 팀 차례 번호 · st_turn = 그 선수가 마지막으로 태세인 채 끝낸 tturn (월) · tmask = 이번 차례에 낸 카드 종류 비트 (신입생 노마드 셈)
	int car_on = 0;
	int sk_uid = -1;
	int sk_kind = 0;   // 그 수의 Action.sk (1 = 힘 스킬 · 2 = 범위 스킬 — 만능만 2를 쓴다, 11-17)
	unsigned char fired[PLAYERS] = {}, tmask[PLAYERS] = {};
	unsigned char rep[PLAYERS][CARD_POOL_N] = {};
	int tturn[2] = { 0, 0 };
	int st_turn[PLAYERS] = {};
	// 시험판 스위치 (11-18 사용자 「혼자하기 4대4·5대5 · 패시브로 · 전술카드 다」 — set_trial, 화면 혼자하기만. 0·5 = 규칙 그대로)
	int team_n = PER_TEAM;   // 팀당 출전 인원 — 4면 uid 4·9(각 팀 다섯째)는 경기에 안 나온다
	int sk_passive = 0;      // 1 = 다섯 직업 스킬이 늘 켜진 패시브 (켜기 수 없음 · 체력은 카드 몫만)

	int ball_team() const;
	bool loose_ball() const { return ball_owner == -1 && ball_pos != UNPLACED; }
	int half_start = 0;   // 이 반이 시작된 turn_count (v10 — 골이 나면 그 반이 일찍 끝난다)
	int half_turn() const { return turn_count - half_start; }
};

// 골문 체력 (v16 사용자 2026-11-10 · v17 11-11 다시 잼 — 측정으로 정한 값 33, docs/spec.md 「### v17」). > 0이면 반마다·골 뒤 골문 체력이 이 값에서 시작하고,
// 슛 데미지가 남은 체력 이상이어야 골 · 모자라면 막히고 체력이 그만큼 깎인 채 남는다(막은 팀 키퍼 킥). 0 = 무조건 골 (v15, 측정용 CF_KEEPER_HP=0)
extern int g_keeper_hp;
extern int g_keeper_carry;
extern int g_range_bonus, g_air_max, g_class_combo;   // 측정용 (기본 1 · 99)
// 측정용 노브 — knobs_from_env() 가 CF_KEEPER_HP 를 읽는다 (옛 CF_NO_ASSIST·CF_COUNTER 는 v16 더하기 규칙으로 없앴다)
void knobs_from_env();
extern int g_block_mode;   // 막힌 공: 2 = 선방 롱패스(사용자 결정 2026-10-30, 기본) · 0 = 골문 앞 리바운드 · 1 = 골문에 가장 가까운 수비가 잡음 (측정용 옛 후보)

struct Action {
	int t = -1;
	int uid = -1;      // 배치는 드래프트 슬롯, 전술은 Tac
	V2 dest{ 0, 0 };   // 묶기는 dest.x = 대상 uid
	int card = -1;     // 카드 번호 (손 칸 0~5 · 조커 6~14)
	int sk = 0;        // 1 = 직업 스킬을 켜고 이 카드를 낸다 (11-14 — 캐논·마에스트로·트릭스터·스프린터·헌터, 체력 2)
	                   // 만능(11-17): 1 = 힘 +1 스킬(캐논 킥·트릭스터 이동·헌터 태세) · 2 = 범위 +1 스킬(마에스트로 킥·스프린터 이동)
};

// 카드 정보 — team이 card를 내면 무슨 종류·숫자인가
void card_info(const State &s, int team, int card, int &type, int &value);
enum Use { U_PASS, U_SHOT, U_MOVE, U_DRIBBLE, U_STANCE };   // 판정 힘의 쓰임
int card_power(const State &s, const Player &p, int card, int want, int use, V2 dest);   // 판정 힘 (전술카드·스킬·스탯 ± 포함 — 버티는 힘은 기본 숫자, 11-18) · 시험용
int card_range(const State &s, const Player &p, int card, int use = -1, int skl = 0);   // skl = Action.sk (만능은 1이어도 범위 스킬 2로 본다)   // use = 쓰임 (노마드가 바꿔 쓰면 바꾼 쪽, −1 = 카드 종류)
int gk_card_value(const State &s, int team, int hand, int card);   // 키퍼 킥 숫자 (−1 = 못 냄, v15)
int gk_kick_range(int value);
bool skill_fits(int sk, int value);
int use_value(int type, int want, int value);   // 노마드가 바꿔 쓸 때의 숫자 (킥 n ↔ 이동 n−1)   // 그 숫자 카드에 붙일 수 있는 스킬인가 — 힘·범위가 0 이 되면 × (사용자 2026-11-09)   // 선방 롱패스 거리 (버린 카드 숫자 → 칸)
int card_eff(const State &s, const Player &p, int type, int value);   // 실효값 (eff_stat 그대로 — 화면이 「+1」「−1」을 띄울 때, 읽기 전용)   // 그 선수가 그 카드로 갈 수 있는 걸음 (마커 −1 · 전문가 +1) — 화면이 길을 그릴 때

// 화면 연출용 기록 (nullptr이면 조용하다 — 탐색 중에는 끈다)
struct Event {
	int kind;   // 0 경합 · 1 밀림 · 2 슛 성공 · 3 태클 응수 카드 · 4 길 · 5 슛 막힘(v11 골문 체력 — a 팀 · b 피해 · va 남은 골문 체력) · 6 직업 스킬 발동(a uid · b 포지션, 11-13)
	int a, b;   // 경합: 건 쪽 uid · 당한 쪽 uid / 밀림: uid · −1 / 슛: 팀 · 피해
	V2 at;
	int va, vb, win;   // 경합 값 둘 · 건 쪽이 이겼나
	std::vector<V2> path;
	int vc = 0;   // 슛(2 골 · 5 막힘): 데미지 힘 (v17 — 데미지 = vc × 괄호 합)
};
extern std::vector<Event> *g_events;
// 유저가 그린 길 (화면 — 본편 5.0 "길은 유저가 그린 그대로"). nullptr이면 엔진이 길을 고른다(경합 최소 → 최단). 탐색 중에는 늘 nullptr
extern const std::vector<V2> *g_path;

void enumerate_actions(const State &s, std::vector<Action> &out);
bool apply_action(State &s, const Action &a);
// 결과가 다른 길 후보 (11-18): 그 행동을 길만 바꿔 다 해 보고 끝 상태가 같은 길끼리 묶어 묶음마다 대표 길 하나.
// out[0] = 기본 길(유저가 안 그렸을 때 엔진이 고르는 길). 1개면 길을 골라도 결과가 같다. 공중볼·이동/드리블/패스/슛 밖은 빈 목록.
// 고른 길로 두려면 g_path = &out[i] 로 apply_action — 「수 3단계 나누기」 망이 도착 칸 다음 단계로 쓸 자리 (지금 망·수 목록은 안 씀)
void outcome_paths(const State &s, const Action &a, std::vector<std::vector<V2>> &out);
// 길을 간 뒤의 상태로 고르기: 후보마다 끝 상태를 val 로 재어 가장 높은 것 (val = 움직인 팀에 좋을수록 큼 — 앱은 az::state_value 로 card.bin 망 가치).
// val null = 대비책(엔진 기본 길). false = 고를 길 없음(공중볼 등). 탐색(MCTS) 안에서는 쓰지 않는다 — 도착 칸당 한 수, 기본 길 그대로
typedef float (*PathValue)(const State &s, int team, const void *ctx);
bool value_path(const State &s, const Action &a, PathValue val, const void *ctx, std::vector<V2> &out);
int shot_damage(const State &s, int uid, int card);   // 슛 데미지 미리보기 (상태 그대로, 못 쏘면 -1) — v17 데미지 힘 × (콤보 + 역습 + 가담)
bool shot_parts(const State &s, int uid, int card, int out[4]);   // 쏜 뒤 콤보 · 역습 · 가담 · 데미지 힘 (v17 화면 내역, 못 쏘면 false)
int assist_value(const State &s, int team);   // 가담 = 공을 만진 서로 다른 아군 수 (최대 5, v16 — 1명당 +1)
int counter_bonus(const State &s, int team);  // 역습 보너스 = 뺏은 자리에서 앞으로 간 칸 수 (v16, 하한 0)
int attack_value(const State &s, int team);   // 괄호 합 = 콤보 + 역습 + 가담 — 슛 카드 없이 지금 쌓인 값 (화면 콤보 박스, v17 데미지 = 슛 힘 × 이 값)
// 족보 (v17 · 혜택 짝은 v17b 사용자 2026-11-12 「쉬운 족보일수록 약하게」): 포카드(같은 숫자 넷) 범위 +1 · 스트레이트(1234) 힘 +1 ·
// 로열(서로 다른 5명이 공을 만짐) 그 차례 포인트 +1 한 번 · 풀하우스(1223344) 셋 다
enum Jokbo { JK_STRAIGHT = 1, JK_ROYAL = 2, JK_FOUR = 4, JK_FULL = 8 };
int jokbo_mask(const State &s, int team);   // 지금 걸린 족보 (공 가진 팀만, 아니면 0)
inline int jk_range(int mask) { return (mask & (JK_FOUR | JK_FULL)) ? 1 : 0; }
inline int jk_stat(int mask) { return (mask & (JK_STRAIGHT | JK_FULL)) ? 1 : 0; }
inline int jk_ap(int mask) { return (mask & (JK_ROYAL | JK_FULL)) ? 1 : 0; }   // 그 차례 포인트 +1 (그 공격에 한 번 — 로열 뒤 풀하우스여도 한 번)
void end_turn(State &s);   // 합법 수가 없을 때 하네스가 부른다
bool marked(const State &s, const Player &p);
bool boosted(const State &s, const Player &p);
int pool_slot(int position);
bool kickoff_turn(const State &s);

// 새 경기 (드래프트·배치도 봇이 둔다). tac_sel = 가져갈 전술카드 3종 비트
// first = 공 없이 먼저 두는 팀. 규칙(v9.0)은 홈이 공을 먼저 갖는다 → 경기는 늘 first = AWAY (시험만 다른 꼴을 쓴다)
void new_match(State &s, int first, int tac_home, int tac_away);
// 컵 경기: 드래프트 없이 정해진 선수(포지션 · 0 = 유망주)와 스탯으로 배치부터 (사용자 2026-11-08). bonus = [선수][슛·패스·드리블·수비]
constexpr int SLOT_PROSPECT = CARD_POOL_N;   // 배치 수의 칸 번호 — 유망주
void set_lineup(State &s, int team, const int position[PER_TEAM], const int bonus[PER_TEAM][4]);
int eff_hold(const State &s);   // 공 임자의 버티는 힘 — 체력 0이면 0 (사용자 11-17). 태클·화면 HOLD·망 입력 모두 이것만 쓴다
void set_stamina(State &s, int team, const int stam[PER_TEAM]);   // 컵 커리어 — 그 팀 선수 시작 칸 (켜면 두 팀 모두 체력 규칙)
void set_perks(State &s, int team, const int range_up[PER_TEAM], const int hold_up[PER_TEAM], const int kick_range_up[PER_TEAM] = nullptr);
// 스탯 칸 4개 그대로 (망 입력 형식). 컵 커리어 스탯은 3개(킥·드리블·수비, 11-13 사용자) — 화면이 킥 값을 슛·패스 두 칸에 같이 넣는다
enum StatKind { ST_SHOT = 0, ST_PASS = 1, ST_DRIBBLE = 2, ST_DEF = 3 };
// 직업 스킬 (11-14 컵 커리어): 켜면 두 팀 모두. 캐논·마에스트로·트릭스터·스프린터·헌터는 패시브 없이 스스로 켜는 스킬(체력 SKILL_STAM)
void set_career(State &s);
void set_trial(State &s, int team_n);   // 시험판 (11-18): 팀 인원 · 스킬 패시브 — new_match 바로 뒤에만
int trial_deck_options(int slot_kind, int value, int out[13]);   // 시험판 덱 화면: 그 칸(0 킥 · 1 이동 · 2 수비, 숫자 value)에 끼울 수 있는 전술카드 (게임 카드 목록 그대로) — 개수
constexpr int SKILL_STAM = 2;
extern int g_skill_stam, g_skill_pow, g_skill_uses;   // 측정용 (11-17) — 기본 SKILL_STAM·1·0 = 지금 규칙
extern int g_jk_straight, g_jk_full, g_jk_royal;      // 측정용 (11-17) — 기본 0·0·5 = 지금 규칙
bool skill_job(int position);                       // 액티브 스킬 직업인가 (그 직업은 패시브가 없다)
bool skill_can(const State &s, const Player &p);   // 지금 스킬을 켤 수 있나 (컵 · 액티브 직업 · 체력 2 이상)
bool skill_act(int position, int act, int sk = 1);  // 그 직업 스킬(sk = Action.sk)이 이 수(A_*)에 붙나 — 만능만 sk 2(범위)를 쓴다 (11-17)
constexpr int SK_KINDS_MAX = 2;   // Action.sk 최댓값
// 손으로 만든 평가 (첫 세대 가치·화면 AI 망이 없을 때) — team 관점, 대략 −3…+3
double heuristic(const State &s, int team);

} // namespace card
} // namespace cf
