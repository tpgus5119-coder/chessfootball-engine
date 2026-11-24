#pragma once
// 판 **상태 지문** (온라인 2인): 두 기기가 같은 수를 두었는데 판이 갈렸는지 견주는 값.
// 규칙은 건드리지 않는다 — 판(State)의 칸을 하나씩 읽어 FNV-1a 로 섞기만 한다.
// 구조체 바이트를 통째로 읽지 않는다(채움 바이트·bool 크기가 컴파일러마다 달라 맥·웹이 다른 값을 낼 수 있다).
#include "card.h"

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
	f.i(s.pts); f.i(s.turn_count); f.i(s.winner);
	f.i(s.redeploy); f.i(s.opener); f.i(s.hold); f.i(s.keeper);
	f.i(s.touch_team); f.i(s.touch_mask);
	for (int t = 0; t < 2; t++) {
		f.i(s.goals[t]); f.i(s.role[t]); f.i(s.ghp[t]); f.i(s.gk[t]);
		f.i(s.tturn[t]);
	}
	f.i(s.team_n); f.i(s.knockout); f.i(s.ot);
	for (int u = 0; u < PLAYERS; u++) {
		const Player &p = s.players[u];
		f.v(p.pos); f.i(p.placed_order); f.i(p.position); f.i(p.uid); f.i(p.team);
		f.i(p.stance); f.i(p.stance_left); f.i(p.charge); f.i(p.acted);
		f.v(s.half_pos[u]); f.i(s.half_order[u]); f.i(s.st_turn[u]);
	}
}

inline uint32_t state_hash(const State &s) {
	Fnv f;
	hash_state(f, s);
	return f.h;
}

} // namespace card
} // namespace cf
