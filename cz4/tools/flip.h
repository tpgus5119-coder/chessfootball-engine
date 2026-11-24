// 위아래(y) 뒤집기 — 기보 증강(gen.cpp)과 대칭 시험(test.cpp)이 같이 쓴다 (11-18). 판 밖 표시(UNPLACED·UNSET)는 그대로
#pragma once
#include "../src/card.h"

namespace flipy {
inline cf::V2 v(cf::V2 p) {
	if (p.x >= 0 && p.y >= 0 && p.x < cf::W && p.y < cf::H && !cf::is_goal(p))   // 골대 칸 = 두 칸을 한 칸처럼 (골대 안 키퍼는 대표 칸 그대로 — 11-24 셋째)
		p.y = cf::H - 1 - p.y;
	return p;
}
inline void state(cf::card::State &s) {
	for (int i = 0; i < cf::PLAYERS; i++) {
		s.half_pos[i] = v(s.half_pos[i]);
		s.players[i].pos = v(s.players[i].pos);
	}
}
inline cf::card::Action act(cf::card::Action a) {   // 받을 동료(mate)는 그대로
	a.dest = v(a.dest);
	return a;
}
} // namespace flipy
