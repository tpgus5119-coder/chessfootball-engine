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
		s.players[i].pos = v(s.players[i].pos);
		int &zd = s.players[i].zdir;   // 존 방향 (11-21) — DIRS 2(+y)·3(−y) 가 바뀐다 · ±x 와 사방은 그대로
		if (zd == 2 || zd == 3)
			zd ^= 1;
	}
}
inline cf::card::Action act(cf::card::Action a) {   // 받을 동료(mate)는 그대로
	a.dest = v(a.dest);
	return a;
}
} // namespace flipy
