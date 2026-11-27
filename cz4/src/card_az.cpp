// 카드 모드 알파제로 (card_az.h) — 탐색·망 읽기·전방 계산
#include "card_az.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cf {
namespace card {
namespace az {

static inline int flip_tile(V2 t, int team) { return t.y * W + (team == AWAY ? W - 1 - t.x : t.x); }

// 칸 → 입력·수 칸 번호 (뒤집은 칸 기준): 박스 밖 80칸 · 자기 진영 박스 밖 40칸 (−1 = 그 칸 없음)
static const struct Tiles {
	int8_t field[NT], place[NT];
	Tiles() {
		int nf = 0, np = 0;
		for (int t = 0; t < NT; t++) {
			const V2 v{ t % W, t / W };
			const bool box = is_any_box(v);
			field[t] = int8_t(box ? -1 : nf++);
			place[t] = int8_t(box || v.x >= W / 2 ? -1 : np++);
		}
	}
} g_tiles;
static inline int ftile(V2 t, int team) { return in_bounds(t) ? g_tiles.field[flip_tile(t, team)] : -1; }

static inline void add(NetInput &in, int base, int f) {   // f < 0 = 박스 칸 (선수·공·존은 거기 안 선다 — 입력 없음)
	if (f >= 0 && in.n < MAX_ACTIVE)
		in.idx[in.n++] = uint16_t(base + f);
}

void net_input(const State &s, int team, NetInput &in) {
	in.n = 0;
	const int opp = opponent(team);
	bool drafted[2][NPOS] = {};
	for (const Player &q : s.players) {
		int slot = pool_slot(q.position);
		if (slot >= 0)
			drafted[q.team == team ? 0 : 1][slot] = true;
		if (q.position < 0)
			continue;
		const int sl = slot < 0 ? NPOS - 1 : slot;
		const int rel = q.team == team ? 0 : 1;
		if (!q.placed()) {
			add(in, OFF_UNPLACED + rel * NPOS, sl);
			continue;
		}
		const int f = ftile(q.pos, team);
		add(in, OFF_PIECE + (rel * NPOS + sl) * NF, f);
		if (q.stance > 0)
			add(in, OFF_STANCE + (rel * 4 + std::min(3, q.stance - 1)) * NF, f);   // 존 힘 1·2·3·4+ (11-27 묶음 14)
		if (q.scut > 0)
			add(in, OFF_SCUT + rel * NF, f);
		if (s.ball_owner == q.uid)
			add(in, OFF_BALL + rel * NF, f);
		if (marked(s, q))
			add(in, OFF_MARKED + rel * NF, f);
		for (int k = 0; k < GROW_K; k++)
			if (q.bonus[k] > 0)
				add(in, OFF_PSTAT + (rel * GROW_K + k) * NF, f);
		add(in, OFF_STAM + (rel * 4 + (stam(q) >= stam_max(q) ? 3 : std::min(2, stam(q)))) * NF, f);   // 남은 체력 0·1·2+·가득 (조커 판정 — 11-27 묶음 9 · 골대 안 키퍼는 칸 없음)
		if (s.gk[q.team] == q.uid)
			add(in, OFF_GK + rel * NF, f);   // 골대 안이면 f < 0 — 입력 없음 (d[22]·d[23])
		if (q.tai) {   // 태세 중 (존·버티기) · 남은 지속 1·2·3+ (월 — 11-27 묶음 14)
			add(in, OFF_TAI + rel * NF, f);
			add(in, OFF_SLEFT + (rel * 3 + std::max(0, std::min(2, q.stance_left - 1))) * NF, f);
		}
	}
	if (s.phase == PLAY && s.turn_team >= 0)   // 두는 팀: 이번 차례에 행동한 선수 칸
		for (const Player &q : s.players)
			if (q.placed() && q.team == s.turn_team && q.acted)
				add(in, OFF_ACTED + (q.uid - s.turn_team * PER_TEAM) * NF, ftile(q.pos, team));
	for (int r = 0; r < 2; r++) {   // 태세 존 칸 — 팀마다 겹친 칸은 한 번
		bool on[NT] = {};
		for (const Player &q : s.players) {
			if ((q.team == team ? 0 : 1) != r)
				continue;
			V2 z[4];
			const int nz = zone_tiles(q, z);
			for (int k = 0; k < nz; k++) {
				const int t = flip_tile(z[k], team);
				if (!on[t])
					on[t] = true, add(in, OFF_ZONE + r * NF, g_tiles.field[t]);
			}
		}
	}
	if (s.phase == PLACEMENT && !s.redeploy)
		for (int r = 0; r < 2; r++)
			for (int k = 0; k < CARD_POOL_N; k++)
				if (!drafted[r][k])
					add(in, OFF_BENCH + r * POOL_N, k);
	float *d = in.dense;
	d[0] = s.phase == PLACEMENT;
	d[1] = s.phase == PLAY;
	d[2] = float(s.hold) / 5.0f;
	d[3] = float(std::max(0, std::min(s.turn_count, CARD_HALF_TURNS))) / float(CARD_HALF_TURNS);   // 연장(골든골)은 1 그대로
	d[4] = float(s.goals[team]) / 2.0f;
	d[5] = float(s.goals[opp]) / 2.0f;
	d[6] = float(s.pts) / float(POINTS_PER_TURN);
	d[7] = kickoff_turn(s);
	d[8] = float(s.placement_index) / float(2 * s.team_n);
	d[9] = float(assist_value(s, team)) / 4.0f;
	d[10] = s.opener == team;
	d[11] = s.role[team] == R_ATK;
	d[12] = s.phase == PLAY && s.ball_team() == team;
	d[13] = float(std::max(-3.0, std::min(3.0, heuristic(s, team))) / 3.0);
	d[14] = float(s.ghp[team]) / 5.0f;   // 골키퍼 남은 체력 — 우리 · 상대
	d[15] = float(s.ghp[opp]) / 5.0f;
	d[16] = float(s.gk_range[team]) / 6.0f;   // 골키퍼 롱킥 범위 — 우리 · 상대
	d[17] = float(s.gk_range[opp]) / 6.0f;
	d[18] = s.keeper == team;   // 키퍼가 공을 쥔 팀 — 우리 · 상대
	d[19] = s.keeper == opp;
	d[20] = s.ot;          // 연장(골든골) 중
	d[21] = s.knockout;    // 토너먼트(단판 — 비기면 연장) · 리그는 0
	d[22] = s.gk_in(team);   // 골키퍼가 골대 안 (밖이면 골대 체력 0) — 우리 · 상대
	d[23] = s.gk_in(opp);
	d[24] = float(s.gk_kick[team]) / 6.0f;   // 키퍼 롱킥 힘 — 우리 · 상대
	d[25] = float(s.gk_kick[opp]) / 6.0f;
}

// 상대 칸 번호: (dx, dy) 맨해튼 1~RREL → 0~NREL−1 (−1 = 범위 밖)
static const struct Rel {
	int16_t at[2 * RREL + 1][2 * RREL + 1];
	int8_t dx[NREL], dy[NREL];
	Rel() {
		int n = 0;
		for (int y = -RREL; y <= RREL; y++)
			for (int x = -RREL; x <= RREL; x++) {
				const bool ok = (x || y) && std::abs(x) + std::abs(y) <= RREL;
				at[y + RREL][x + RREL] = int16_t(ok ? n : -1);
				if (ok)
					dx[n] = int8_t(x), dy[n] = int8_t(y), n++;
			}
	}
	int of(int x, int y) const { return std::abs(x) + std::abs(y) > RREL || (!x && !y) ? -1 : at[y + RREL][x + RREL]; }
} g_rel;

int action_index(const State &s, const Action &a) {
	const int team = s.turn_team;
	const int me = a.uid - PER_TEAM * team;   // 두는 팀 안 번호 (배치는 uid = 드래프트 슬롯)
	const bool mine = me >= 0 && me < PER_TEAM;
	switch (a.t) {
		case A_MOVE:
		case A_DRIBBLE:
		case A_PASS: {
			if (a.t == A_MOVE && is_goal_of(team, a.dest))
				return mine ? OFF_GKIN : -1;   // 골키퍼가 골대로 돌아감
			if (!mine || !in_bounds(a.dest))
				return -1;
			const V2 ref = s.players[a.uid].pos;
			if (!in_bounds(ref))
				return -1;
			// 골대 안 키퍼: dy = 골대 가운데(3.5) 기준 — 아래 칸들 1·2·… · 위 칸들 −1·−2·… (0 없음 · 위아래 거울에서 dy → −dy · 칸마다 다름)
			const int dy = is_goal(ref) ? (a.dest.y > GOAL_Y0 ? a.dest.y - GOAL_Y0 : a.dest.y - GOAL_Y0 - 1) : a.dest.y - ref.y;
			const int r = g_rel.of((a.dest.x - ref.x) * forward_dir(team), dy);
			if (r < 0)
				return -1;
			const int kind = a.t == A_MOVE ? 0 : a.t == A_DRIBBLE ? 1 : 2;
			return OFF_TILE + (kind * PER_TEAM + me) * NREL + r;
		}
		case A_SHOT:
			return mine ? OFF_SHOT + me : -1;
		case A_STANCE:   // 태세 = 제자리 (누구나 — 11-26 묶음 7)
			return mine && a.dest == s.players[a.uid].pos ? OFF_STANCE_A + me : -1;
		case A_PLACE: {
			if (a.uid < 0 || a.uid > POOL_N || !in_bounds(a.dest))
				return -1;
			const int p = g_tiles.place[flip_tile(a.dest, team)];
			return p < 0 ? -1 : OFF_PLACE + a.uid * NPL + p;
		}
		case A_KEEP:   // 키퍼 롱킥: 받는 아군(me) 발밑
			return mine ? OFF_KEEP + me : -1;
		default:
			return -1;
	}
}

// ---------- 위아래 거울 표 ----------
static const Mirror g_mirror = [] {
	Mirror m;
	m.in.resize(SPARSE);
	m.act.resize(NA);
	m.plane.assign(SPARSE, -1);
	m.tile.assign(SPARSE, -1);
	m.glob.assign(SPARSE, -1);
	for (int i = 0; i < SPARSE; i++)
		m.in[i] = uint16_t(i);
	int fl[NF], ft[NF];   // 칸 입력 번호 f → 거울 f · 판 칸(뒤집은 판 y·W + x)
	for (int t = 0; t < NT; t++)
		if (g_tiles.field[t] >= 0) {
			const V2 v{ t % W, t / W };
			fl[g_tiles.field[t]] = g_tiles.field[(H - 1 - v.y) * W + v.x];
			ft[g_tiles.field[t]] = t;
		}
	struct Blk {
		int off, n;
	};
	const Blk tile_blocks[] = { { OFF_PIECE, 2 * NPOS }, { OFF_STANCE, 8 }, { OFF_BALL, 2 }, { OFF_MARKED, 2 }, { OFF_PSTAT, 2 * GROW_K }, { OFF_TAI, 2 },
		{ OFF_ZONE, 2 }, { OFF_ACTED, PER_TEAM }, { OFF_GK, 2 }, { OFF_STAM, 8 }, { OFF_SLEFT, 6 }, { OFF_SCUT, 2 } };
	int pl = 0;
	for (const Blk &b : tile_blocks)
		for (int k = 0; k < b.n; k++, pl++)
			for (int f = 0; f < NF; f++) {
				const int i = b.off + k * NF + f;
				m.in[i] = uint16_t(b.off + k * NF + fl[f]);
				m.plane[i] = int16_t(pl);
				m.tile[i] = int16_t(ft[f]);
			}
	m.planes = pl;
	int g = 0;
	for (int i = OFF_UNPLACED; i < OFF_MARKED; i++)   // 안 놓인 선수 · 드래프트 남은 직업 = 칸 없음
		m.glob[i] = int16_t(g++);
	m.globs = g;
	for (int i = 0; i < SPARSE; i++)
		if (m.plane[i] < 0 && m.glob[i] < 0) {
			fprintf(stderr, "거울 표: 입력 %d 이 어느 묶음에도 없음\n", i);
			abort();
		}
	for (int a = 0; a < NA; a++)
		m.act[a] = uint16_t(a);
	for (int kk = 0; kk < 3 * PER_TEAM; kk++)
		for (int r = 0; r < NREL; r++)
			m.act[OFF_TILE + kk * NREL + r] = uint16_t(OFF_TILE + kk * NREL + g_rel.of(g_rel.dx[r], -g_rel.dy[r]));
	int pm[NPL];
	for (int t = 0; t < NT; t++)
		if (g_tiles.place[t] >= 0)
			pm[g_tiles.place[t]] = g_tiles.place[(H - 1 - t / W) * W + t % W];
	for (int sl = 0; sl <= POOL_N; sl++)
		for (int p = 0; p < NPL; p++)
			m.act[OFF_PLACE + sl * NPL + p] = uint16_t(OFF_PLACE + sl * NPL + pm[p]);
	return m;
}();
const Mirror &mirror() { return g_mirror; }

bool mirror_exact(const State &s) {   // 골대 안 키퍼가 공을 다룸(쥠 · 킥오프 차례 — 11-27 묶음 12) = 대표 칸이 한쪽이라 거울 아님
	if (s.ball_owner < 0)
		return !(kickoff_turn(s) && s.gk[s.turn_team] >= 0 && is_goal_of(s.turn_team, s.players[s.gk[s.turn_team]].pos));
	const Player &o = s.players[s.ball_owner];
	return !(s.gk[o.team] == o.uid && is_goal_of(o.team, o.pos));
}

struct Eval {
	float v = 0.0f;
	std::vector<float> logits;
};

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

static float half_to_float(uint16_t h) {   // IEEE 754 반정밀도 → 단정밀도
	const uint32_t sign = uint32_t(h & 0x8000) << 16;
	uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
	if (exp == 0) {
		if (man == 0) {
			bits = sign;
		} else {   // 비정규 수
			exp = 127 - 15 + 1;
			while (!(man & 0x400)) {
				man <<= 1;
				exp--;
			}
			bits = sign | (exp << 23) | ((man & 0x3FF) << 13);
		}
	} else if (exp == 31) {
		bits = sign | 0x7F800000 | (man << 13);
	} else {
		bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
	}
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

// 머리표 int 10칸: MAGIC · 판(1 fp32 · 2 fp16 · 3 w1 int8) · SPARSE · DENSE · h1 · h2 · NA · arch(0 flat · 1 ResNet) · blocks · ch
bool Net::load_mem(const unsigned char *p, size_t n) {
	size_t at = 0;
	auto take = [&](void *dst, size_t bytes) {
		if (at + bytes > n)
			return false;
		std::memcpy(dst, p + at, bytes);
		at += bytes;
		return true;
	};
	arch = -1;
	int head[10];
	if (!take(head, sizeof(head)) || head[0] != MAGIC || head[1] < 1 || head[1] > 3 || head[2] != SPARSE || head[3] != DENSE || head[6] != NA ||
			head[5] <= 0 || head[5] > 1024 || head[7] < 0 || head[7] > 1)
		return false;
	const int ar = head[7];
	if (ar == 0 && (head[4] <= 0 || head[4] > 4096))
		return false;
	if (ar == 1 && (head[8] < 1 || head[8] > 16 || head[9] < 4 || head[9] > 128 || head[1] == 3))
		return false;
	h1 = ar == 0 ? head[4] : 0;
	h2 = head[5];
	na = head[6];
	blocks = ar == 1 ? head[8] : 0;
	ch = ar == 1 ? head[9] : 0;
	// 판 2 = 무게를 16비트 반정밀도로 · 판 3 = 첫 층 w1 만 int8 (64칸 묶음마다 fp16 scale) + 나머지 판 2 와 같음 (게임 전용 — tools/net_q8/net_q8.py)
	const bool half = head[1] >= 2;
	auto takef = [&](float *dst, size_t cnt) {
		if (!half)
			return take(dst, 4 * cnt);
		if (at + 2 * cnt > n)
			return false;
		for (size_t i = 0; i < cnt; i++, at += 2)
			dst[i] = half_to_float(uint16_t(p[at] | (p[at + 1] << 8)));
		return true;
	};
	auto takev = [&](std::vector<float> &v, size_t cnt) {
		v.resize(cnt);
		return takef(v.data(), cnt);
	};
	auto take_q8 = [&](float *dst, size_t rows, int cols) {   // 판 3 w1: scale fp16 [rows × cols/64] 다음 값 int8 [rows × cols]
		constexpr int G = 64;
		if (cols % G != 0)
			return false;
		const size_t ng = rows * size_t(cols / G);
		if (at + 2 * ng + rows * size_t(cols) > n)
			return false;
		const unsigned char *sc = p + at, *q8 = p + at + 2 * ng;
		for (size_t g = 0; g < ng; g++) {
			const float k = half_to_float(uint16_t(sc[2 * g] | (sc[2 * g + 1] << 8)));
			for (int j = 0; j < G; j++)
				dst[g * G + j] = k * float(int8_t(q8[g * G + j]));
		}
		at += 2 * ng + rows * size_t(cols);
		return true;
	};
	bool ok;
	if (ar == 0) {
		w1.assign(size_t(SPARSE + DENSE) * h1, 0.0f);
		ok = (head[1] == 3 ? take_q8(w1.data(), size_t(SPARSE + DENSE), h1) : takef(w1.data(), size_t(SPARSE + DENSE) * h1)) &&
				takev(b1, h1) && takev(w2, size_t(h1) * h2) && takev(b2, h2);
	} else {
		const Mirror &m = mirror();
		const size_t C = size_t(ch);
		ok = takev(ws, size_t(m.planes) * 9 * C) && takev(wg, size_t(m.globs + DENSE) * C) && takev(bs, C);
		c1.resize(blocks), cb1.resize(blocks), q.resize(blocks), c2.resize(blocks), cb2.resize(blocks);
		for (int k = 0; k < blocks && ok; k++)
			ok = takev(c1[k], 9 * C * C) && takev(cb1[k], C) && takev(q[k], C * C) && takev(c2[k], 9 * C * C) && takev(cb2[k], C);
		ok = ok && takev(wh, C * RES_HEAD) && takev(bh, RES_HEAD) && takev(wf, (2 * C + RES_HEAD * NT) * size_t(h2)) && takev(bf, h2);
	}
	ok = ok && takev(wv, h2) && takef(&bv, 1) && takev(wa, h2) && takef(&ba, 1) && takev(wp, size_t(h2) * na) && takev(bp, na);
	if (!ok)
		return false;
	// 정책 머리를 **수마다 한 줄**(NA×h2)로 뒤집어 둔다 — 파일은 h2×NA라 수 하나를 셀 때 NA칸씩 건너뛰며 읽었다 (캐시 낭비)
	std::vector<float> t(wp.size());
	for (int k = 0; k < h2; k++)
		for (int a = 0; a < na; a++)
			t[size_t(a) * h2 + k] = wp[size_t(k) * na + a];
	wp.swap(t);
	arch = ar;
	return true;
}

static void forward_flat(const Net &net, const NetInput &in, std::vector<float> &b) {
	static thread_local std::vector<float> a;
	a.assign(net.b1.begin(), net.b1.end());
	const int h1 = net.h1;
	for (int i = 0; i < in.n; i++) {
		const float *row = &net.w1[size_t(in.idx[i]) * h1];
		for (int j = 0; j < h1; j++)
			a[j] += row[j];
	}
	for (int k = 0; k < DENSE; k++) {
		float v = in.dense[k];
		if (v == 0.0f)
			continue;
		const float *row = &net.w1[size_t(SPARSE + k) * h1];
		for (int j = 0; j < h1; j++)
			a[j] += v * row[j];
	}
	for (int j = 0; j < h1; j++)
		a[j] = std::min(std::max(a[j], 0.0f), 1.0f);
	b.assign(net.b2.begin(), net.b2.end());
	const int h2 = net.h2;
	for (int j = 0; j < h1; j++) {
		float v = a[j];
		if (v == 0.0f)
			continue;
		const float *row = &net.w2[size_t(j) * h2];
		for (int k = 0; k < h2; k++)
			b[k] += v * row[k];
	}
	for (int k = 0; k < h2; k++)
		b[k] = std::min(std::max(b[k], 0.0f), 1.0f);
}

// 3×3 합성곱 (0 채움): out[t][co] += Σ_k Σ_ci in[t + d_k][ci] · w[k][ci][co] — 칸 · 채널 순 배열 (안쪽 고리 = 채널 연속 → NEON 벡터)
// 커널 k = ky·3 + kx · d = (kx − 1, ky − 1) = PyTorch conv2d(padding 1) 와 같은 뜻. 채널 수를 틀 인자로 (16·24·32·48·64 — 고리를 펼친다)
template <int C>
static void conv3_t(const float *in, const float *w, const float *bias, float *out) {
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			float o[C];
			for (int co = 0; co < C; co++)
				o[co] = bias[co];
			for (int ky = 0; ky < 3; ky++) {
				const int yy = y + ky - 1;
				if (yy < 0 || yy >= H)
					continue;
				for (int kx = 0; kx < 3; kx++) {
					const int xx = x + kx - 1;
					if (xx < 0 || xx >= W)
						continue;
					const float *iv = in + size_t(yy * W + xx) * C;
					const float *wk = w + size_t(ky * 3 + kx) * C * C;
					for (int ci = 0; ci < C; ci++) {
						const float v = iv[ci];
						const float *row = wk + size_t(ci) * C;
						for (int co = 0; co < C; co++)
							o[co] += v * row[co];
					}
				}
			}
			std::memcpy(out + size_t(y * W + x) * C, o, sizeof(o));
		}
}

static void conv3(const float *in, const float *w, const float *bias, float *out, int C) {
	switch (C) {
		case 16: return conv3_t<16>(in, w, bias, out);
		case 24: return conv3_t<24>(in, w, bias, out);
		case 32: return conv3_t<32>(in, w, bias, out);
		case 48: return conv3_t<48>(in, w, bias, out);
		case 64: return conv3_t<64>(in, w, bias, out);
		default: break;
	}
	for (int t = 0; t < NT; t++)
		std::memcpy(out + size_t(t) * C, bias, sizeof(float) * C);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			float *o = out + size_t(y * W + x) * C;
			for (int ky = 0; ky < 3; ky++) {
				const int yy = y + ky - 1;
				if (yy < 0 || yy >= H)
					continue;
				for (int kx = 0; kx < 3; kx++) {
					const int xx = x + kx - 1;
					if (xx < 0 || xx >= W)
						continue;
					const float *iv = in + size_t(yy * W + xx) * C;
					const float *wk = w + size_t(ky * 3 + kx) * C * C;
					for (int ci = 0; ci < C; ci++) {
						const float v = iv[ci];
						const float *row = wk + size_t(ci) * C;
						for (int co = 0; co < C; co++)
							o[co] += v * row[co];
					}
				}
			}
		}
}

static void forward_res(const Net &net, const State &s, const NetInput &in, std::vector<float> &b) {
	(void)s;
	const Mirror &m = mirror();
	const int C = net.ch;
	static thread_local std::vector<float> x, u, t1, t2;
	x.resize(size_t(NT) * C), u.resize(size_t(NT) * C), t1.resize(size_t(NT) * C), t2.resize(size_t(NT) * C);
	// 줄기: 칸 없는 입력(+ 숫자) → 채널 덤 · 칸 입력 → 3×3 (켜진 칸 하나가 곁 9칸에 더한다)
	std::vector<float> g(net.bs.begin(), net.bs.end());
	for (int i = 0; i < in.n; i++) {
		const int gi = m.glob[in.idx[i]];
		if (gi >= 0) {
			const float *row = &net.wg[size_t(gi) * C];
			for (int c = 0; c < C; c++)
				g[c] += row[c];
		}
	}
	for (int k = 0; k < DENSE; k++) {
		const float v = in.dense[k];
		if (v == 0.0f)
			continue;
		const float *row = &net.wg[size_t(m.globs + k) * C];
		for (int c = 0; c < C; c++)
			g[c] += v * row[c];
	}
	for (int t = 0; t < NT; t++)
		std::memcpy(&x[size_t(t) * C], g.data(), sizeof(float) * C);
	for (int i = 0; i < in.n; i++) {
		const int pl = m.plane[in.idx[i]];
		if (pl < 0)
			continue;
		const int t0 = m.tile[in.idx[i]], x0 = t0 % W, y0 = t0 / W;
		for (int ky = 0; ky < 3; ky++)
			for (int kx = 0; kx < 3; kx++) {   // out(y, x) += w[ky][kx] · in(y + ky − 1, x + kx − 1) → 켜진 (y0, x0) 는 (y0 − ky + 1, x0 − kx + 1) 에
				const int yy = y0 - ky + 1, xx = x0 - kx + 1;
				if (yy < 0 || yy >= H || xx < 0 || xx >= W)
					continue;
				const float *row = &net.ws[(size_t(pl) * 9 + ky * 3 + kx) * C];
				float *o = &x[size_t(yy * W + xx) * C];
				for (int c = 0; c < C; c++)
					o[c] += row[c];
			}
	}
	for (int k = 0; k < net.blocks; k++) {
		for (size_t i = 0; i < x.size(); i++)
			u[i] = std::max(0.0f, x[i]);
		conv3(u.data(), net.c1[k].data(), net.cb1[k].data(), t1.data(), C);
		std::vector<float> mean(C, 0.0f);
		for (size_t i = 0; i < t1.size(); i++) {
			t1[i] = std::max(0.0f, t1[i]);
			mean[i % C] += t1[i];
		}
		std::vector<float> gb(C, 0.0f);   // 전역 평균 덤 (KataGo global pooling)
		for (int ci = 0; ci < C; ci++) {
			const float v = mean[ci] / float(NT);
			const float *row = &net.q[k][size_t(ci) * C];
			for (int co = 0; co < C; co++)
				gb[co] += v * row[co];
		}
		for (size_t i = 0; i < t1.size(); i++)
			t1[i] += gb[i % C];
		conv3(t1.data(), net.c2[k].data(), net.cb2[k].data(), t2.data(), C);
		for (size_t i = 0; i < x.size(); i++)
			x[i] += t2[i];
	}
	// 머리: relu → 평균·최대 (2C) + 1×1 칸 머리 4장 (RES_HEAD·NT, 판 순서 = 채널마다 칸 96)
	const int F = 2 * C + RES_HEAD * NT;
	static thread_local std::vector<float> f;
	f.assign(F, 0.0f);
	for (int c = 0; c < C; c++)
		f[C + c] = 0.0f;
	for (int t = 0; t < NT; t++)
		for (int c = 0; c < C; c++) {
			const float v = std::max(0.0f, x[size_t(t) * C + c]);
			f[c] += v / float(NT);
			f[C + c] = std::max(f[C + c], v);
			u[size_t(t) * C + c] = v;
		}
	for (int t = 0; t < NT; t++)
		for (int h = 0; h < RES_HEAD; h++) {
			float v = net.bh[h];
			for (int c = 0; c < C; c++)
				v += u[size_t(t) * C + c] * net.wh[size_t(c) * RES_HEAD + h];
			f[2 * C + h * NT + t] = std::max(0.0f, v);
		}
	const int h2 = net.h2;
	b.assign(net.bf.begin(), net.bf.end());
	for (int j = 0; j < F; j++) {
		const float v = f[j];
		if (v == 0.0f)
			continue;
		const float *row = &net.wf[size_t(j) * h2];
		for (int k = 0; k < h2; k++)
			b[k] += v * row[k];
	}
	for (int k = 0; k < h2; k++)
		b[k] = std::min(std::max(b[k], 0.0f), 1.0f);
}

static void forward(const Net &net, const State &s, const NetInput &in, std::vector<float> &b) {
	if (net.arch == 1)
		forward_res(net, s, in, b);
	else
		forward_flat(net, in, b);
}

float net_eval(const Net &net, const NetInput &in, const int *ai, int n, float *logits) {
	static thread_local std::vector<float> b;
	State dummy;
	forward(net, dummy, in, b);
	float v = net.bv;
	for (int k = 0; k < net.h2; k++)
		v += b[k] * net.wv[k];
	for (int i = 0; i < n; i++) {
		logits[i] = 0.0f;
		if (ai[i] < 0 || ai[i] >= net.na)
			continue;
		float l = net.bp[ai[i]];
		for (int k = 0; k < net.h2; k++)
			l += b[k] * net.wp[size_t(ai[i]) * net.h2 + k];
		logits[i] = l;
	}
	return std::tanh(v);
}

static void evaluate(const Net &net, const State &s, const std::vector<Action> &acts, Eval &out) {
	out.logits.assign(acts.size(), 0.0f);
	if (!net.loaded()) {
		out.v = float(std::tanh(heuristic(s, s.turn_team) / 2.0));
		return;
	}
	NetInput in;
	net_input(s, s.turn_team, in);
	static thread_local std::vector<float> b;
	forward(net, s, in, b);
	const int h2 = net.h2;
	float v = net.bv;   // 보조 머리(wa·ba)는 학습 목표용 — 탐색엔 안 쓴다
	for (int k = 0; k < h2; k++)
		v += b[k] * net.wv[k];
	out.v = std::tanh(v);
	for (size_t i = 0; i < acts.size(); i++) {
		const int ai = action_index(s, acts[i]);
		if (ai < 0 || ai >= net.na)
			continue;
		float l = net.bp[ai];
		const float *row = &net.wp[size_t(ai) * h2];
		for (int k = 0; k < h2; k++)
			l += b[k] * row[k];
		out.logits[i] = l;
	}
}

float (*g_tac_score)(const State &s, const Action &a, const Style &st) = nullptr;

// ---------- 팀 성격 덤 ----------
// 크기는 logit 단위 — 망 정책의 수 사이 logit 차가 보통 몇 단위라 덤 1 안팎은 「비슷한 수 중에서 고르는 버릇」 정도다 (값과 측정: ui/card_match.gd STYLE_W 설명)
float style_bias(const State &s, const Action &a, const Style &st) {
	if (s.phase != PLAY || a.uid < 0 || a.uid >= PLAYERS)
		return 0.0f;
	const int team = s.turn_team;
	const int fwd = forward_dir(team);
	const Player &p = s.players[a.uid];
	if (p.team != team || p.pos == UNPLACED)
		return 0.0f;
	const V2 ball = s.ball_owner >= 0 ? s.players[s.ball_owner].pos : UNPLACED;
	const int gain = (a.dest.x - p.pos.x) * fwd;   // 앞으로 간 칸
	float b = 0.0f;
	if (s.ball_team() == team) {   // 공격
		if (a.t == A_PASS) {
			const int d = manhattan(p.pos, a.dest);
			b += st.poss * (d <= 2 ? 1.0f : 0.0f);                             // 지공: 짧은 패스 (긴 킥을 깎으면 슛 길이 막혀 약해짐 — 측정 11-12)
			b += st.ctr * (d >= 3 ? 1.0f : -0.5f) + st.ctr * 0.25f * float(std::max(0, gain));   // 역습: 긴 킥·앞으로
		} else if (a.t == A_DRIBBLE) {
			b += st.ctr * 0.4f * float(std::max(-1, std::min(3, gain)));         // 역습: 빠른 전진
		} else if (a.t == A_MOVE && a.uid != s.ball_owner) {
			b += st.poss * 0.3f * float(std::max(0, std::min(2, gain)));         // 지공: 가담 (공 없는 동료가 올라감)
		}
	} else if (ball != UNPLACED) {   // 수비
		if (a.t == A_MOVE) {
			const int closer = manhattan(p.pos, ball) - manhattan(a.dest, ball);
			b += st.press * 0.5f * float(std::max(-2, std::min(3, closer)));     // 전방압박: 공 쪽으로
			const bool own_now = is_own_half(team, p.pos), own_after = is_own_half(team, a.dest);
			b += st.hold * (own_after ? (own_now ? 0.0f : 1.0f) : -1.0f);       // 지키는 수비: 자기 진영으로 · 넘어가지 않기
		} else if (a.t == A_STANCE) {
			b += st.hold * (is_own_half(team, p.pos) ? 0.5f : 0.0f);           // 제자리에서 태세
		}
	}
	return b;
}

// ---------- 탐색 ----------
namespace {

struct Rng {
	unsigned long long s;
	explicit Rng(unsigned long long seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
	unsigned long long next() {
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 0x2545F4914F6CDD1Dull;
	}
	double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
	double gumbel() { return -std::log(-std::log(std::max(unit(), 1e-12))); }
	double normal() {   // Box–Muller
		double u1 = std::max(unit(), 1e-12), u2 = unit();
		return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
	}
	double gamma(double alpha) {   // Marsaglia–Tsang (α<1은 α+1로 뽑아 U^(1/α) 보정)
		if (alpha < 1.0)
			return gamma(alpha + 1.0) * std::pow(std::max(unit(), 1e-12), 1.0 / alpha);
		double d = alpha - 1.0 / 3.0, c = 1.0 / std::sqrt(9.0 * d);
		for (;;) {
			double x = normal(), v = 1.0 + c * x;
			if (v <= 0.0)
				continue;
			v = v * v * v;
			double u = unit();
			if (u < 1.0 - 0.0331 * x * x * x * x || std::log(std::max(u, 1e-12)) < 0.5 * x * x + d * (1.0 - v + std::log(v)))
				return d * v;
		}
	}
};

struct Node {
	State s;
	int mover = -1;            // 이 마디에서 두는 팀 (끝난 판이면 부모의 팀)
	bool terminal = false;
	float v = 0.0f;            // 망 가치 (mover 관점) · 끝난 판이면 결과
	std::vector<Action> acts;
	std::vector<float> logits;
	std::vector<int> child;    // 마디 번호 (−1 = 아직)
	std::vector<int> n;        // 자식 방문 수
	std::vector<double> w;     // 자식 가치 합 (이 마디 mover 관점)
	int N = 0;                 // 자식 방문 합
};

struct Tree {
	const Net &net;
	const Params &p;
	std::vector<Node> nodes;
	explicit Tree(const Net &n_, const Params &p_) : net(n_), p(p_) {}

	// 합법 수가 없는 국면은 턴을 넘긴 국면으로 바꾼다 (하네스·UI가 end_turn을 부르는 것과 같다)
	int make(State s, int parent_mover) {
		Node nd;
		for (int guard = 0; guard < 8 && s.phase != GAME_OVER; guard++) {
			enumerate_actions(s, nd.acts);
			if (!nd.acts.empty())
				break;
			end_turn(s);
		}
		nd.s = s;
		if (s.phase == GAME_OVER || nd.acts.empty()) {
			nd.terminal = true;
			nd.mover = parent_mover < 0 ? s.turn_team : parent_mover;
			nd.v = s.winner == -1 ? 0.0f : (s.winner == nd.mover ? 1.0f : -1.0f);   // 비김 = 0
		} else {
			nd.mover = s.turn_team;
			Eval e;
			evaluate(net, s, nd.acts, e);
			nd.v = e.v;   // s.turn_team(= mover) 관점
			nd.logits = std::move(e.logits);
			if (nd.mover == p.style_team && p.style.on())
				for (size_t i = 0; i < nd.acts.size(); i++)
					nd.logits[i] += style_bias(s, nd.acts[i], p.style);
			nd.child.assign(nd.acts.size(), -1);
			nd.n.assign(nd.acts.size(), 0);
			nd.w.assign(nd.acts.size(), 0.0);
		}
		nodes.push_back(std::move(nd));
		return int(nodes.size()) - 1;
	}

	// σ(q) = (c_visit + 최대 방문 수)·c_scale·q — 앞의 배율은 마디마다 한 번만 센다 (수마다 다시 세면 k²)
	double sigma_scale(const Node &nd) const {
		int maxn = 0;
		for (int x : nd.n)
			maxn = std::max(maxn, x);
		return (p.c_visit + maxn) * p.c_scale;
	}

	// 완성된 Q (Gumbel 논문 식): 방문한 수는 평균값, 안 한 수는 v_mix
	void completed_q(const Node &nd, std::vector<double> &q) const {
		size_t k = nd.acts.size();
		std::vector<double> pi(k);
		double mx = -1e30;
		for (size_t i = 0; i < k; i++)
			mx = std::max(mx, double(nd.logits[i]));
		double sum = 0.0;
		for (size_t i = 0; i < k; i++) {
			pi[i] = std::exp(double(nd.logits[i]) - mx);
			sum += pi[i];
		}
		double vis_pi = 0.0, vis_piq = 0.0;
		for (size_t i = 0; i < k; i++) {
			pi[i] /= sum;
			if (nd.n[i] > 0) {
				vis_pi += pi[i];
				vis_piq += pi[i] * (nd.w[i] / nd.n[i]);
			}
		}
		double vmix = nd.v;
		if (nd.N > 0 && vis_pi > 0.0)
			vmix = (nd.v + nd.N * (vis_piq / vis_pi)) / (1.0 + nd.N);
		q.resize(k);
		for (size_t i = 0; i < k; i++)
			q[i] = nd.n[i] > 0 ? nd.w[i] / nd.n[i] : vmix;
	}

	// 개선된 정책 π' = softmax(logits + σ(완성된 Q))
	void improved_policy(const Node &nd, std::vector<double> &pi) const {
		std::vector<double> q;
		completed_q(nd, q);
		size_t k = nd.acts.size();
		pi.resize(k);
		double mx = -1e30;
		const double sc = sigma_scale(nd);
		for (size_t i = 0; i < k; i++) {
			pi[i] = double(nd.logits[i]) + sc * q[i];
			mx = std::max(mx, pi[i]);
		}
		double sum = 0.0;
		for (size_t i = 0; i < k; i++) {
			pi[i] = std::exp(pi[i] - mx);
			sum += pi[i];
		}
		for (size_t i = 0; i < k; i++)
			pi[i] /= sum;
	}

	// 뿌리 아래: 결정론 선택 argmax(π'(a) − n(a)/(1+N))
	int pick_child(const Node &nd) const {
		std::vector<double> pi;
		improved_policy(nd, pi);
		int best = 0;
		double bs = -1e30;
		for (size_t i = 0; i < nd.acts.size(); i++) {
			double sc = pi[i] - double(nd.n[i]) / (1.0 + nd.N);
			if (sc > bs) {
				bs = sc;
				best = int(i);
			}
		}
		return best;
	}

	// 뿌리의 수 a를 한 번 더 파고 내려간다. 돌려주는 값 = 뿌리 mover 관점
	double simulate(int root, int a) {
		std::vector<std::pair<int, int>> path;   // (마디, 자식 번호)
		int cur = root;
		int ai = a;
		double leaf = 0.0;
		int leaf_mover = -1;
		for (;;) {
			Node &nd = nodes[cur];
			path.push_back({ cur, ai });
			if (nd.child[ai] == -1) {
				State c = nd.s;
				apply_action(c, nd.acts[ai]);
				int ci = make(c, nd.mover);
				nodes[cur].child[ai] = ci;
				leaf = nodes[ci].v;
				leaf_mover = nodes[ci].mover;
				break;
			}
			cur = nd.child[ai];
			Node &cn = nodes[cur];
			if (cn.terminal) {
				leaf = cn.v;
				leaf_mover = cn.mover;
				break;
			}
			ai = pick_child(cn);
		}
		for (auto it = path.rbegin(); it != path.rend(); ++it) {
			Node &nd = nodes[it->first];
			double val = nd.mover == leaf_mover ? leaf : -leaf;
			nd.n[it->second] += 1;
			nd.w[it->second] += val;
			nd.N += 1;
		}
		Node &r = nodes[root];
		return r.mover == leaf_mover ? leaf : -leaf;
	}
};

} // namespace

Result search(const State &s0, const Net &net, const Params &p) {
	Result res;
	Tree tree(net, p);
	tree.nodes.reserve(size_t(p.sims) * 2 + 8);
	const int root = tree.make(s0, -1);
	Node &r0 = tree.nodes[root];
	res.acts = r0.acts;
	if (r0.terminal || r0.acts.empty()) {
		res.root_v = r0.v;
		return res;
	}
	const size_t k = r0.acts.size();
	Rng rng(p.seed);
	std::vector<double> g(k, 0.0);
	if (p.gumbel)
		for (size_t i = 0; i < k; i++)
			g[i] = rng.gumbel();
	if (p.dirichlet_eps > 0.0 && k > 1) {   // 디리클레 뿌리 잡음 — 후보 고르기 점수(g)에만 섞는다 (학습 목표 π'는 r.logits 그대로)
		std::vector<double> pr(k), dir(k);
		double mx = -1e30, sum = 0.0, dsum = 0.0;
		for (size_t i = 0; i < k; i++)
			mx = std::max(mx, double(r0.logits[i]));
		for (size_t i = 0; i < k; i++) {
			pr[i] = std::exp(double(r0.logits[i]) - mx);
			sum += pr[i];
			dir[i] = rng.gamma(p.dirichlet_alpha);
			dsum += dir[i];
		}
		for (size_t i = 0; i < k; i++) {
			pr[i] /= sum;
			dir[i] /= std::max(dsum, 1e-12);
			double mixed = (1.0 - p.dirichlet_eps) * pr[i] + p.dirichlet_eps * dir[i];
			g[i] += std::log(std::max(mixed, 1e-12)) - std::log(std::max(pr[i], 1e-12));
		}
	}
	// 뿌리 후보: logit + g 상위 m개 → 순차 반감
	std::vector<int> cand(k);
	for (size_t i = 0; i < k; i++)
		cand[i] = int(i);
	std::sort(cand.begin(), cand.end(), [&](int a, int b) { return g[a] + r0.logits[a] > g[b] + r0.logits[b]; });
	int m = std::min<int>(p.top_m, int(k));
	cand.resize(m);
	std::vector<float> ts;   // 전술 대본 점수 — 높은 수 넷은 꼭 탐색한다
	const bool tac = g_tac_score && p.style.tac >= 0 && r0.mover == p.style_team;
	if (tac) {
		ts.resize(k);
		for (size_t i = 0; i < k; i++)
			ts[i] = g_tac_score(s0, r0.acts[i], p.style);
		std::vector<int> ord(k);
		for (size_t i = 0; i < k; i++)
			ord[i] = int(i);
		std::sort(ord.begin(), ord.end(), [&](int a, int b) { return ts[a] > ts[b]; });
		for (int j = 0, add = 0; j < int(k) && add < 4; j++)
			if (std::find(cand.begin(), cand.end(), ord[j]) == cand.end()) {
				cand.push_back(ord[j]);
				add++;
			}
		m = int(cand.size());
	}
	const std::vector<int> cand0 = cand;
	if (m > 1) {
		int phases = 0;
		for (int x = m; x > 1; x = (x + 1) / 2)
			phases++;
		int used = 0;
		while (cand.size() > 1 && used < p.sims) {
			int per = std::max(1, (p.sims - used) / (phases * int(cand.size())));
			for (int rep = 0; rep < per && used < p.sims; rep++)
				for (int a : cand) {
					if (used >= p.sims)
						break;
					tree.simulate(root, a);
					used++;
				}
			phases = std::max(1, phases - 1);
			Node &r = tree.nodes[root];
			const double sc = tree.sigma_scale(r);
			auto score = [&](int a) {
				double q = r.n[a] > 0 ? r.w[a] / r.n[a] : 0.0;
				return g[a] + r.logits[a] + sc * q;
			};
			std::sort(cand.begin(), cand.end(), [&](int a, int b) { return score(a) > score(b); });
			if (cand.size() > 1)
				cand.resize((cand.size() + 1) / 2);
		}
		// 남은 예산은 1등을 더 다진다 (뿌리 가치 추정 정밀도)
		while (used < p.sims) {
			tree.simulate(root, cand[0]);
			used++;
		}
	} else {
		for (int i = 0; i < std::max(1, p.sims / 4); i++)
			tree.simulate(root, cand[0]);
	}
	Node &r = tree.nodes[root];
	res.found = true;
	res.action = r.acts[cand[0]];
	if (tac) {   // 허용 폭 안에서 대본 점수가 가장 높은 수 (값은 많이 간 수만 믿는다)
		int nmin = std::max(2, p.sims / (4 * std::max(1, m)));
		double best = -2.0;
		for (int a : cand0)
			if (r.n[a] >= nmin)
				best = std::max(best, r.w[a] / r.n[a]);
		int pick = cand[0];
		for (int a : cand0)
			if (r.n[a] >= nmin && r.w[a] / r.n[a] >= best - p.style.eps && (ts[a] > ts[pick] || (ts[a] == ts[pick] && r.w[a] / r.n[a] > r.w[pick] / std::max(1, r.n[pick]))))
				pick = a;
		if (r.acts[cand[0]].t == A_SHOT && r.w[cand[0]] / std::max(1, r.n[cand[0]]) > 0.0)
			pick = cand[0];   // 득점 기회(값이 이기는 쪽인 슛)는 대본이 가로채지 않는다
		if (r.n[pick] >= nmin || pick == cand[0])
			cand[0] = pick, res.action = r.acts[pick];
	}
	std::vector<double> pi;
	tree.improved_policy(r, pi);
	res.policy.assign(pi.begin(), pi.end());
	{   // 정책 놀람 KL(π' ‖ softmax(logit)) — KataGo 표본 무게
		double mx = -1e30, sum = 0.0;
		for (size_t i = 0; i < k; i++)
			mx = std::max(mx, double(r.logits[i]));
		for (size_t i = 0; i < k; i++)
			sum += std::exp(double(r.logits[i]) - mx);
		double kl = 0.0;
		for (size_t i = 0; i < k; i++)
			if (pi[i] > 1e-12)
				kl += pi[i] * (std::log(pi[i]) - (double(r.logits[i]) - mx - std::log(sum)));
		res.surprise = float(std::max(0.0, kl));
	}
	std::vector<double> q;
	tree.completed_q(r, q);
	double rv = 0.0;
	for (size_t i = 0; i < k; i++)
		rv += pi[i] * q[i];
	res.root_v = float(rv);
	return res;
}


static inline unsigned long long xs(unsigned long long &r) {
	r ^= r >> 12;
	r ^= r << 25;
	r ^= r >> 27;
	return r * 0x2545F4914F6CDD1Dull;
}

void random_lineup(State &s, int team, unsigned long long r) {
	auto next = [&]() { return xs(r); };
	// 커리어와 같은 꼴: 직업 12 · 성장 5단계(grow_steps) · 선수단 6명(core/career.gd SQUAD) 중 선발 5명 무작위 · 골키퍼 = 선발 아무나 (11-27)
	constexpr int JOBS_N = 12;
	static const int POOL[JOBS_N] = { CANNON, MAESTRO, TRICKSTER, SPRINTER, MARKER, TITAN, HUNTER, WALL, STRIKER, PLAYMAKER, JOKER, BOOSTER };
	constexpr int SQ = PER_TEAM + 1;
	int pos[SQ], bonus[SQ][GROW_K] = {};
	int used = 0;
	const int prospects = int(next() % (SQ + 1));          // 유망주 0~6명
	const int level = int(next() % 4);                    // 팀 성장 정도 0~3
	const int mono = next() % 5 == 0 ? POOL[next() % JOBS_N] : 0;   // 20 % 는 한 직업 팀
	const bool ace_team = next() % 10 < 3;   // 몰아 키운 에이스: 30 % 팀은 전직 선수 한 명이 만랩
	const int ace = ace_team && prospects < SQ ? prospects + int(next() % (SQ - prospects)) : -1;
	for (int i = 0; i < SQ; i++) {
		if (i < prospects) {
			pos[i] = PROSPECT;
		} else if (mono) {
			pos[i] = mono;
		} else {
			int k;
			do
				k = int(next() % JOBS_N);
			while ((used >> k) & 1);
			used |= 1 << k;
			pos[i] = POOL[k];
		}
		// 성장 단계 (11-26 묶음 8): 신입생 0~2 (①② · 체력 세부는 ① 까지) · 전직 선수 = 전직 단계(3 · 조커·부스터 2) ~ + level · 에이스 = 5 (만랩)
		const int det = family_of(pos[i]) >= 0 ? family_of(pos[i]) : int(next() % STAT_N);
		const int need = pos[i] == PROSPECT ? 0 : det == ST_STAM ? 2 : 3;
		const int steps = pos[i] == PROSPECT ? (level > 0 ? int(next() % (det == ST_STAM ? 2 : 3)) : 0) : i == ace ? 5 : std::min(5, need + int(next() % (level + 1)));
		int extra[3], ne = 0, cand[4], nc = 0;
		for (int k = 0; k < STAT_N; k++)
			if (group_of(k) != group_of(det))
				cand[nc++] = k;
		while (ne < 3) {   // 다른 계열 세부 넷 중 서로 다른 셋 (씨앗 순)
			const int k = cand[next() % nc];
			bool dup = false;
			for (int j = 0; j < ne; j++)
				dup = dup || extra[j] == k;
			if (!dup)
				extra[ne++] = k;
		}
		grow_steps(pos[i], det, steps, extra, bonus[i]);
	}
	int idx[SQ];   // 선발 5명 = 무작위로 섞은 앞 5명 · 키퍼 = 그 가운데 아무나
	for (int i = 0; i < SQ; i++)
		idx[i] = i;
	for (int i = SQ - 1; i > 0; i--)
		std::swap(idx[i], idx[next() % (i + 1)]);
	int p5[PER_TEAM], b5[PER_TEAM][GROW_K];
	for (int i = 0; i < PER_TEAM; i++) {
		p5[i] = pos[idx[i]];
		std::memcpy(b5[i], bonus[idx[i]], sizeof(b5[i]));
	}
	set_lineup(s, team, p5, b5, int(next() % PER_TEAM));
	int sta[PER_TEAM];   // 경기 시작 체력: 절반 팀은 가득 · 절반은 선수마다 0 ~ 최대 무작위 (커리어 경기 사이 이어진 체력 — 11-25)
	const bool tired = next() % 2;
	for (int i = 0; i < PER_TEAM; i++)
		sta[i] = tired ? int(next() % (stam_max(s.players[team * PER_TEAM + i]) + 1)) : 99;
	set_stamina(s, team, sta);
}

// core/career.gd _form(back_x, front_x) = [[b, 1], [b, 6], [f, 2], [f, 5]] — DEFAULT (1, 3) · PRESS (2, 4) · HOLD (1, 2)
const Form CAREER_FORMS[3] = { { { { 1, 1 }, { 1, 6 }, { 3, 2 }, { 3, 5 } } }, { { { 2, 1 }, { 2, 6 }, { 4, 2 }, { 4, 5 } } }, { { { 1, 1 }, { 1, 6 }, { 2, 2 }, { 2, 5 } } } };

static bool form_tile_ok(V2 t) { return t.x >= 0 && t.x < W / 2 && t.y >= 0 && t.y < H && !is_any_box(t) && !is_goal(t); }

Form random_form(unsigned long long &r) {
	Form f;
	for (int i = 0; i < FIELD_N; i++)
		for (;;) {
			const V2 t{ int(xs(r) % (W / 2)), int(xs(r) % H) };
			bool dup = !form_tile_ok(t);
			for (int j = 0; j < i; j++)
				dup = dup || f.t[j] == t;
			if (!dup) {
				f.t[i] = t;
				break;
			}
		}
	return f;
}

void place_forms(State &s, const Form f[2]) {
	std::vector<Action> acts;
	for (int guard = 0; guard < 4 * PLAYERS && s.phase == PLACEMENT; guard++) {
		const int team = s.turn_team;
		enumerate_actions(s, acts);
		if (acts.empty())
			return;
		int slot = -1, fi = 0;
		if (s.redeploy) {   // 다음에 놓을 선수 = 아직 안 놓인 필드 선수 가운데 uid 가 가장 작은 선수 · 대형 칸 = 필드 순번
			for (int u = team * PER_TEAM, k = 0; u < (team + 1) * PER_TEAM; u++) {
				const Player &q = s.players[u];
				if (q.position < 0 || s.gk[team] == u)
					continue;
				if (!q.placed()) {
					slot = q.position == PROSPECT ? SLOT_PROSPECT : pool_slot(q.position);
					fi = k;
					break;
				}
				k++;
			}
		} else {   // 드래프트 판: 놓인 필드 선수 수가 순번
			for (int u = team * PER_TEAM; u < (team + 1) * PER_TEAM; u++)
				fi += s.players[u].placed() && s.gk[team] != u;
		}
		V2 want = f[team].t[std::min(fi, FIELD_N - 1)];
		if (team == AWAY)
			want.x = W - 1 - want.x;
		int pick = -1;
		for (size_t i = 0; i < acts.size(); i++)
			if (acts[i].t == A_PLACE && (slot < 0 || acts[i].uid == slot) && (pick < 0 || acts[i].dest == want)) {
				pick = int(i);
				if (acts[i].dest == want && slot >= 0)
					break;
			}
		if (slot < 0) {   // 드래프트: 그 칸에 놓는 수 가운데 무작위 대신 첫 직업 — random_prematch 가 슬롯을 섞는다
			for (size_t i = 0; i < acts.size(); i++)
				if (acts[i].dest == want) {
					pick = int(i);
					break;
				}
		}
		if (pick < 0 || !apply_action(s, acts[pick]))
			return;
	}
}

void random_prematch(State &s, unsigned long long r) {
	if (xs(r) % 4 != 0)   // 75 % 무작위 선수단 · 25 % 드래프트 판 (직업도 배치 수로 고름 — 무작위)
		for (int t = 0; t < 2; t++)
			random_lineup(s, t, xs(r));
	Form f[2];
	for (int t = 0; t < 2; t++)
		f[t] = xs(r) % 4 == 0 ? CAREER_FORMS[xs(r) % 3] : random_form(r);
	if (!s.redeploy) {   // 드래프트 판: 배치 수(직업·칸)를 무작위로
		std::vector<Action> acts;
		for (int guard = 0; guard < 4 * PLAYERS && s.phase == PLACEMENT; guard++) {
			enumerate_actions(s, acts);
			if (acts.empty() || !apply_action(s, acts[xs(r) % acts.size()]))
				break;
		}
		return;
	}
	place_forms(s, f);
}

LineupPick choose_lineup(const State &tmpl, int team, int n, const int pos[], const int bonus[][GROW_K], const int *stam, const Form &opp_form,
		const std::vector<Form> &forms0, int sampled, unsigned long long seed, const Net &net) {
	LineupPick best;
	n = std::max(0, std::min(n, PER_TEAM + 1));
	if (n < PER_TEAM)
		return best;
	std::vector<Form> forms = forms0;
	unsigned long long r = seed ? seed : 0x9E3779B97F4A7C15ull;
	for (int i = 0; i < sampled; i++)
		forms.push_back(random_form(r));
	if (forms.empty())
		forms.push_back(CAREER_FORMS[0]);
	const int opp = opponent(team);
	for (int skip = n == PER_TEAM ? -1 : 0; skip < (n == PER_TEAM ? 0 : n); skip++) {   // 쉬는 한 명 (6명이면)
		int st[PER_TEAM], m = 0;
		for (int i = 0; i < n; i++)
			if (i != skip)
				st[m++] = i;
		for (int gk = 0; gk < PER_TEAM; gk++) {
			int ord[PER_TEAM], p5[PER_TEAM], b5[PER_TEAM][GROW_K], s5[PER_TEAM];   // 필드 4명(선수단 순) 다음 키퍼
			for (int i = 0, k = 0; i < PER_TEAM; i++)
				if (i != gk)
					ord[k++] = st[i];
			ord[PER_TEAM - 1] = st[gk];
			for (int i = 0; i < PER_TEAM; i++) {
				p5[i] = pos[ord[i]];
				std::memcpy(b5[i], bonus[ord[i]], sizeof(b5[i]));
				s5[i] = stam ? stam[ord[i]] : 99;
			}
			for (const Form &fm : forms) {
				float v = 0.0f;
				for (int first = 0; first < 2; first++) {   // 동전 두 경우 평균
					State s = tmpl;
					set_lineup(s, team, p5, b5, PER_TEAM - 1);
					set_stamina(s, team, s5);
					s.first_team = s.opener = s.turn_team = first;
					s.role[first] = R_DEF;
					s.role[opponent(first)] = R_ATK;
					Form f2[2];
					f2[team] = fm, f2[opp] = opp_form;
					place_forms(s, f2);
					if (s.phase != PLAY)
						continue;
					v += state_value(s, team, &net) * 0.5f;
					best.evals++;
				}
				if (v > best.v) {
					best.v = v;
					std::memcpy(best.starters, ord, sizeof(ord));
					best.form = fm;
				}
			}
		}
	}
	return best;
}

// 길 고르기용 상태 가치 (card.h value_path — 길은 간 뒤의 상태로): team 에 좋을수록 큼 (−1 ~ 1). ctx = const Net *
float state_value(const State &s, int team, const void *ctx) {
	if (s.phase == GAME_OVER)
		return s.winner == -1 ? 0.0f : s.winner == team ? 1.0f : -1.0f;
	Eval e;
	evaluate(*static_cast<const Net *>(ctx), s, std::vector<Action>(), e);
	return s.turn_team == team ? e.v : -e.v;
}

} // namespace az
} // namespace card
} // namespace cf
