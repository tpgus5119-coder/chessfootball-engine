#include "nnue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cf {

const Net *g_net = nullptr;
double g_net_mix = -1.0;
bool g_root_setup = false;

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
	for (int k = 0; k < NN_NPOS - 1; k++)
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
	d[2] = s.phase == SUB;
	d[3] = s.half == 2;
	d[4] = float(std::max(0, std::min(s.half_turn(), HALF_TURNS))) / float(HALF_TURNS);
	d[5] = float(s.score[team]);
	d[6] = float(s.score[opp]);
	d[7] = s.goal_turn[team] > 0 ? float(HALF_TURNS + 1 - s.goal_turn[team]) / float(HALF_TURNS) : 0.0f;
	d[8] = s.goal_turn[opp] > 0 ? float(HALF_TURNS + 1 - s.goal_turn[opp]) / float(HALF_TURNS) : 0.0f;
	d[9] = s.turn_team == team;
	d[10] = float(s.pts) / float(POINTS_PER_TURN);
	d[11] = kickoff_turn(s);
	d[12] = float(s.placement_index) / 8.0f;
	d[13] = s.redeploy != 0;
	d[14] = float(s.contest_used);   // v5.79: 이번 턴 경합을 썼나 (옛 하프타임 교체 차례 칸 — 교체는 v5.68에 없어졌다)
	d[15] = s.opener == team;
	double f[6];
	features(s, team, f);
	for (int k = 0; k < 6; k++)
		d[16 + k] = float(f[k]);
}

bool Net::load(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	std::vector<unsigned char> buf;
	unsigned char tmp[65536];
	size_t k;
	while ((k = fread(tmp, 1, sizeof(tmp), f)) > 0)
		buf.insert(buf.end(), tmp, tmp + k);
	fclose(f);
	return load_mem(buf.data(), buf.size());
}

bool Net::load_mem(const unsigned char *p, size_t n) {
	size_t at = 0;
	auto take = [&](void *dst, size_t bytes) {
		if (at + bytes > n)
			return false;
		std::memcpy(dst, p + at, bytes);
		at += bytes;
		return true;
	};
	int head[6];
	if (!take(head, sizeof(head)) || head[0] != 0x4E4E4643 || head[1] != 1 || head[2] != NN_SPARSE ||
			head[3] != NN_DENSE || head[4] <= 0 || head[4] > 512 || head[5] <= 0 || head[5] > 128)
		return false;
	h1 = head[4];
	h2 = head[5];
	w1.resize(size_t(NN_SPARSE + NN_DENSE) * h1);
	b1.resize(h1);
	w2.resize(size_t(h1) * h2);
	b2.resize(h2);
	w3.resize(h2);
	return take(w1.data(), 4 * w1.size()) && take(b1.data(), 4 * b1.size()) && take(w2.data(), 4 * w2.size()) &&
			take(b2.data(), 4 * b2.size()) && take(w3.data(), 4 * w3.size()) && take(&b3, 4);
}

float Net::eval(const NetInput &in) const {
	float a[512];
	float b[128];
	std::memcpy(a, b1.data(), sizeof(float) * h1);
	for (int i = 0; i < in.n; i++) {
		const float *row = &w1[size_t(in.idx[i]) * h1];
		for (int j = 0; j < h1; j++)
			a[j] += row[j];
	}
	for (int k = 0; k < NN_DENSE; k++) {
		float v = in.dense[k];
		if (v == 0.0f)
			continue;
		const float *row = &w1[size_t(NN_SPARSE + k) * h1];
		for (int j = 0; j < h1; j++)
			a[j] += v * row[j];
	}
	for (int j = 0; j < h1; j++)
		a[j] = std::min(std::max(a[j], 0.0f), 1.0f); // 자른 ReLU (NNUE)
	for (int k = 0; k < h2; k++)
		b[k] = b2[k];
	for (int j = 0; j < h1; j++) {
		float v = a[j];
		if (v == 0.0f)
			continue;
		const float *row = &w2[size_t(j) * h2];
		for (int k = 0; k < h2; k++)
			b[k] += v * row[k];
	}
	float o = b3;
	for (int k = 0; k < h2; k++)
		o += std::min(std::max(b[k], 0.0f), 1.0f) * w3[k];
	return std::tanh(o);
}

} // namespace cf
