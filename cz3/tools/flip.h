// 위아래(y) 뒤집기 — 기보 증강(gen.cpp)과 대칭 시험(test.cpp)이 같이 쓴다 (11-18). 판 밖 표시(UNPLACED·UNSET)는 그대로
#pragma once
#include "../src/card.h"

namespace flipy {
inline cf::V2 v(cf::V2 p) {
	if (p.x >= 0 && p.y >= 0 && p.x < cf::W && p.y < cf::H)
		p.y = cf::H - 1 - p.y;
	return p;
}
inline void state(cf::card::State &s) {
	s.ball_pos = v(s.ball_pos);
	for (int i = 0; i < cf::PLAYERS; i++) {
		s.half_pos[i] = v(s.half_pos[i]);
		s.left_from[i] = v(s.left_from[i]);
		s.players[i].pos = v(s.players[i].pos);
	}
}
inline cf::card::Action act(cf::card::Action a) {   // 전술(묶기 dest.x = uid)·턴 끝은 칸이 아니다
	if (a.t != cf::card::A_TACTIC && a.t != cf::card::A_END)
		a.dest = v(a.dest);
	return a;
}
} // namespace flipy
