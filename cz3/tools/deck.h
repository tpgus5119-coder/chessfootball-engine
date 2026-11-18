#pragma once
// 비교 실험 덱 (11-18) — 전술카드 목록을 설정값으로 받는다 (사용자가 곧 장수를 줄인다).
//   CF_DECK = 쉼표로 이은 스킬 번호 (card.h Skill). 없으면 게임 카드 목록 35장 = core/career.gd KICK_CARDS·MOVE_CARDS·DEF_CARDS
//   칸마다 skill_p 확률로 그 칸 종류(킥·이동·수비)·숫자에 맞는(skill_fits) 카드를 겹치지 않게 뽑는다 — card_az.cpp random_deck 과 같은 방식
#include "../src/card.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace deck3 {
using namespace cf::card;

struct Deck {
	std::vector<int> pool[3];   // 0 킥 · 1 이동 · 2 수비
};

inline int kind_of(int sk) {
	if ((sk >= K_SHOT_UP && sk <= K_COUNTER) || sk == K_KEEP || sk == K_EASY || sk == K_ALLOUT)
		return 0;
	if ((sk >= M_OFF && sk <= M_COMBO) || sk == M_KEEP || sk == M_BREATH || sk == M_SPURT)
		return 1;
	return 2;
}

inline Deck from_env() {
	static const int GAME[] = { 1, 2, 3, 4, 6, 8, 11, 12, 13, 15, 40, 42,   // career.gd KICK_CARDS (30장 — 11-18 7·16·17·27·33 뺌)
		18, 19, 21, 22, 23, 24, 25, 26, 41, 44,                                // MOVE_CARDS
		29, 30, 32, 35, 36, 37, 38, 46 };                                    // DEF_CARDS
	Deck d;
	const char *e = getenv("CF_DECK");
	std::vector<int> all;
	if (e && *e) {
		std::string w;
		for (const char *c = e;; c++) {
			if (*c && *c != ',') {
				w += *c;
				continue;
			}
			if (!w.empty())
				all.push_back(atoi(w.c_str()));
			w.clear();
			if (!*c)
				break;
		}
	} else {
		all.assign(GAME, GAME + sizeof(GAME) / sizeof(int));
	}
	for (int sk : all)
		if (sk > SK_NONE && sk < N_SKILL)
			d.pool[kind_of(sk)].push_back(sk);
	return d;
}

inline void fill(State &s, int team, const Deck &d, unsigned long long r, double skill_p) {
	auto next = [&]() {
		r ^= r >> 12;
		r ^= r << 25;
		r ^= r >> 27;
		return r * 0x2545F4914F6CDD1Dull;
	};
	r = r * 6364136223846793005ull + 1442695040888963407ull + 1;
	std::vector<int> left[3] = { d.pool[0], d.pool[1], d.pool[2] };
	auto pick = [&](int kind, int value) {
		if (double(next() >> 40) / double(1ull << 24) >= skill_p)
			return 0;
		std::vector<int> ok;
		for (int k : left[kind])
			if (skill_fits(k, value))
				ok.push_back(k);
		if (ok.empty())
			return 0;
		const int sk = ok[(next() >> 32) % ok.size()];
		for (size_t i = 0; i < left[kind].size(); i++)
			if (left[kind][i] == sk) {
				left[kind].erase(left[kind].begin() + long(i));
				break;
			}
		return sk;
	};
	s.hand_kick[team] = s.hand_move[team] = 0;
	for (int c = 0; c < 3; c++) {
		s.sk_atk[team][c] = pick(0, slot_value(c));
		s.sk_def[team][c] = pick(2, slot_value(c));
		s.sk_atk[team][3 + c] = pick(1, slot_value(3 + c));
	}
}

} // namespace deck3
