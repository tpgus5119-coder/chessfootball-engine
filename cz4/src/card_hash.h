#pragma once
// 카드 모드 **상태 지문** (온라인 2인 — 2026-11-04): 두 기기가 같은 수를 두었는데 판이 갈렸는지 견주는 값.
// 규칙은 건드리지 않는다 — 판(State)·일기토(Duel)의 칸을 하나씩 읽어 FNV-1a 로 섞기만 한다.
// 구조체 바이트를 통째로 읽지 않는다(채움 바이트·bool 크기가 컴파일러마다 달라 맥·웹이 다른 값을 낼 수 있다).
#include "card.h"
#include "card_duel.h"

#include <cstdint>

namespace cf {
namespace card {

struct Fnv {
	uint32_t h = 2166136261u;
	void i(int v) {
		uint32_t u = uint32_t(v);
		for (int k = 0; k < 4; k++) {
			h ^= (u >> (8 * k)) & 0xFF;
			h *= 16777619u;
		}
	}
	void v(V2 p) { i(p.x); i(p.y); }
};

inline void hash_state(Fnv &f, const State &s) {
	f.i(s.phase); f.i(s.first_team); f.i(s.placement_index); f.i(s.turn_team); f.i(s.ball_owner);
	f.v(s.ball_pos); f.i(s.loose_team); f.i(s.pts); f.i(s.turn_count); f.i(s.winner); f.i(s.half);
	f.i(s.redeploy); f.i(s.opener); f.i(s.hold);
	f.i(s.touch_team); f.i(s.touch_mask); f.i(s.half_start);
	for (int t = 0; t < 2; t++) {
		f.i(s.goals[t]); f.i(s.role[t]);
		f.i(s.jk_turn[t]); f.i(s.jk_next[t]); f.i(s.tturn[t]);
	}
	f.i(s.team_n); f.i(s.sk_passive);
	for (int u = 0; u < PLAYERS; u++) {
		const Player &p = s.players[u];
		f.v(p.pos); f.i(p.placed_order); f.i(p.position); f.i(p.uid); f.i(p.team);
		f.i(p.stance); f.i(p.stance_left); f.i(p.jk_up); f.i(p.charge); f.i(p.stance > 0 ? p.zdir : -1);
		f.v(s.half_pos[u]); f.i(s.half_order[u]);
		f.i(s.players[u].acted);
		f.i(s.fired[u]); f.i(s.st_turn[u]);
	}
}

inline uint32_t state_hash(const State &s, const duel::Duel *d) {
	Fnv f;
	hash_state(f, s);
	if (d != nullptr) {
		f.i(1);
		hash_state(f, d->s);
		f.i(d->round); f.i(d->stage); f.i(d->atk_team); f.i(d->atk_turn); f.i(d->winner);
		for (int t = 0; t < 2; t++) { f.i(d->pick[t]); f.i(d->turns[t]); f.i(d->last_turns[t]); }
		for (int u = 0; u < PLAYERS; u++) f.i(d->stance_shown[u] ? 1 : 0);
		f.i(d->hold_shown ? 1 : 0);
	}
	return f.h;
}

} // namespace card
} // namespace cf
