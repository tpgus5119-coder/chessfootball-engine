#include "nnue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cf {

static inline int nn_tile(V2 t, bool flip) {
	return t.y * W + (flip ? W - 1 - t.x : t.x);
}

static inline void add(NetInput &in, int i) {
	if (in.n < NN_MAX_ACTIVE)
		in.idx[in.n++] = uint16_t(i);
}

void net_input(const State &s, int team, NetInput &in) {
	in.n = 0;
	const bool flip = team == AWAY; // 평가하는 팀이 늘 +x로 공격하게
	bool drafted[NN_NPOS] = {};
	for (const Player &q : s.players) {
		int slot = pool_slot(q.position);
		int sl = slot < 0 ? NN_NPOS - 1 : slot;
		if (slot >= 0)
			drafted[slot] = true;
		if (q.position < 0 || s.is_absent(q.uid))
			continue; // 아직 드래프트 안 된 자리·결장
		int rel = q.team == team ? 0 : 1;
		if (!q.is_placed()) {
			add(in, NN_OFF_UNPLACED + rel * NN_NPOS + sl);
			continue;
		}
		int tile = nn_tile(q.pos, flip);
		add(in, NN_OFF_PIECE + (rel * NN_NPOS + sl) * NT + tile);
		if (q.wide_zone())
			add(in, NN_OFF_STANCE + rel * NT + tile);
		if (s.ball_owner_uid == q.uid)
			add(in, NN_OFF_BALL + rel * NT + tile);
	}
	if (s.loose_ball())
		add(in, NN_OFF_LOOSE + (s.loose_team == team ? 0 : 1) * NT + nn_tile(s.ball_pos, flip));
	for (int k = 0; k < POOL_N; k++)   // 아무도 안 가진 슬롯 전부 (종전엔 마지막 슬롯이 빠졌다)
		if (!drafted[k])
			add(in, NN_OFF_BENCH + k);
	for (int tm = 0; tm < 2; tm++) {   // v5.80 마커 수동 잡기
		int gt = grab_target_of(s, tm);
		if (gt != -1)
			add(in, NN_OFF_GRAB + (s.players[gt].team == team ? 0 : 1) * NT + nn_tile(s.players[gt].pos, flip));
	}
	const int opp = opponent(team);
	float *d = in.dense;
	d[0] = s.phase == PLACEMENT;
	d[1] = s.phase == PLAY;
	d[2] = float(s.boost_on && team == s.turn_team);   // v5.83 이번 턴 부스트가 켜졌나 (옛 하프타임 교체 단계 칸을 물려받았다)
	d[3] = s.half == 2;
	d[4] = float(std::max(0, std::min(s.half_turn(), HALF_TURNS))) / float(HALF_TURNS);
	d[5] = float(s.score[team]);
	d[6] = float(s.score[opp]);
	d[7] = s.goal_turn[team] > 0 ? float(HALF_TURNS + 1 - s.goal_turn[team]) / float(HALF_TURNS) : 0.0f;
	d[8] = s.goal_turn[opp] > 0 ? float(HALF_TURNS + 1 - s.goal_turn[opp]) / float(HALF_TURNS) : 0.0f;
	d[9] = s.turn_team == team;
	d[10] = float(s.pts) / float(POINTS_PER_TURN);
	d[11] = kickoff_turn(s);
	d[12] = float(s.placement_index) / float(PLAYERS);
	d[13] = s.redeploy != 0;
	d[14] = float(s.contest_used);   // v5.79: 이번 턴 경합을 썼나 (옛 하프타임 교체 차례 칸 — 교체는 v5.68에 없어졌다)
	d[15] = s.opener == team;
	double f[6];
	features(s, team, f);
	for (int k = 0; k < 6; k++)
		d[16 + k] = float(f[k]);
}

} // namespace cf
