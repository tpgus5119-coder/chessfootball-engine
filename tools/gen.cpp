// 알파제로 자가대전 기보 공장 (tools/az/loop.py가 여러 개를 동시에 돌린다) — 본체는 gdext/src/az.cpp selfplay
//   gen <출력> <판수> <탐색 횟수> <시드> <망 파일 | -> [온도 수 N=12] [결과 비중 λ=0.7] [빠른 탐색 횟수=0] [풀탐색 비율=1]
// 빠른 탐색 횟수 > 0 이면 탐색 상한 무작위 (az.h selfplay 설명)
// 망이 '-'이면 첫 세대: 가치 = 선형 평가, 정책 = 균일 (탐색이 정책을 만든다)
#include "../../gdext/src/az.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace cf;

int main(int argc, char **argv) {
	if (argc < 6) {
		fprintf(stderr, "gen <출력> <판수> <탐색횟수> <시드> <망|-> [온도수] [λ] [빠른탐색] [풀비율]\n");
		return 1;
	}
	az::Net net;
	if (strcmp(argv[5], "-") != 0 && !net.load(argv[5])) {
		fprintf(stderr, "망을 못 읽음: %s\n", argv[5]);
		return 1;
	}
	std::vector<az::Rec> recs;
	az::GenStats st;
	az::selfplay(net, atoi(argv[2]), atoi(argv[3]), strtoull(argv[4], nullptr, 10), argc > 6 ? atoi(argv[6]) : 12,
			argc > 7 ? atof(argv[7]) : 0.7, recs, st, argc > 8 ? atoi(argv[8]) : 0, argc > 9 ? atof(argv[9]) : 1.0);
	FILE *f = fopen(argv[1], "wb");
	if (!f)
		return 1;
	fwrite(recs.data(), sizeof(az::Rec), recs.size(), f);
	fclose(f);
	fprintf(stderr, "끝: %d판 HOME %d · AWAY %d · 무 %d · 수 %lld\n", st.games, st.home, st.away, st.draws, st.moves);
	return 0;
}
