#pragma once
// 규칙 엔진(card.h)의 Godot 노출부 — 화면(ui/card_match.gd)이 규칙을 이것으로만 묻고 둔다
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include "card_az.h"

namespace godot {

class CardNative : public RefCounted {
	GDCLASS(CardNative, RefCounted)

	cf::card::State state;
	Array last_events;

protected:
	static void _bind_methods();

public:
	void new_match(int first);
	void set_knockout(bool on);   // 단판 토너먼트 — 정규 끝에 비기면 연장 (new_match 뒤에 부른다 · 리그는 안 부름)
	void set_lineup(int team, PackedInt32Array positions, PackedInt32Array bonus, int gk = -1);   // 선수단 5 · 성장 5×6 (슛·패스·무브·디펜스·피지컬·체력 — 옛 5×4 = 20칸도 받음) · gk = 골키퍼 칸 (−1 = 디펜스 최고)
	void set_stamina(int team, PackedInt32Array stam);   // 경기 시작 체력 (선수단 5칸 순서 · set_lineup 뒤 · 0~최대로 자름 — 안 부르면 가득)
	static PackedInt32Array stat_info(int position, PackedInt32Array bonus);   // [피지컬, 체력 최대, 회복(행동 안 한 차례·경기 사이)]
	void dev_stam(int uid, int stam);   // 개발·시험용 — 남은 체력 놓기
	void set_trial(int team_n);   // 시험판 (혼자하기 — 팀 필드 인원)
	Dictionary get_state() const;
	PackedInt32Array actions() const;             // [종류, uid, x, y, 0(옛 스킬 칸)] × n
	bool apply(int t, int uid, int dx, int dy, int sk, const Array &path = Array());   // path = 유저가 그린 길 (Vector2i 배열, 비면 엔진이 고른다)
	Array events() const { return last_events; }
	Array preview(int t, int uid, int dx, int dy, int sk, const Array &path = Array()) const;
	void force_end_turn();
	void dev_set(int uid, int x, int y, int stance);
	void dev_ball(int owner, int x, int y, int turn_team);
	void dev_pos(int uid, int position);
	void dev_ghp(int team, int hp);
	static PackedInt32Array keeper_stats(int position, PackedInt32Array bonus);   // bonus = 성장 4~6칸   // 그 선수가 키퍼일 때 [디펜스, 롱킥 힘, 롱킥 범위]
	int act_range(int uid, int want, int sk = 0) const;
	int act_power(int uid, int use, int dist = 0, int sk = 0) const;
	bool ai_load(const PackedByteArray &bytes);
	bool ai_ready() const;
	PackedInt32Array ai_pick(int sims, int seed, int exclude_uid = -1) const;   // exclude_uid = 그 선수 수는 빼고 (선수 모드 동료)
	void ai_style(int team, float poss, float ctr, float press, float hold);   // 팀 성격 덤 — team −1 = 끔
	void ai_tactic(int team, int tac);                     // 전술 대본 — 막대 = docs/전술_대본_2026-11-22.md 3절
	float tac_slider(int tac, int i) const;                // 막대 0~13 (문서 번호 −1) · 값 0~10
	void set_tac_slider(int tac, int i, float v);
	int style_team = -1;
	cf::card::az::Style style;
	int state_hash() const;                        // 온라인 2인 — 판 지문 (card_hash.h, 맥·웹 같은 값)
	PackedByteArray save_state() const;            // 자동 저장 (core/save_slot.gd) — 판 전부 (State 는 값만 든 구조체)
	bool load_state(const PackedByteArray &b);     // 크기·표시가 다르면(엔진이 바뀐 옛 판) false
};

} // namespace godot
