# chessfootball-engine

치즈볼(체스축구) 규칙 엔진·알파제로 탐색의 C++ 부분입니다. 게임 본편(그림·소리·동작·화면)은 포함하지 않습니다.
이 저장소의 용도는 GitHub Actions로 자가대전 기보를 만드는 것입니다 (`.github/workflows/selfplay.yml`).

- `src/engine.*` — 규칙 엔진 (판 10×7, 4대4, 턴당 행동 2)
- `src/nnue.*` — 판 입력 인코딩
- `src/az.*` — Gumbel AlphaZero 탐색 + 망 (가치·정책·보조)
- `tools/gen.cpp` — 자가대전 기보 공장 · `tools/arena.cpp` — 아레나

빌드: `c++ -O3 -std=c++17 tools/gen.cpp src/engine.cpp src/nnue.cpp src/az.cpp -o gen`

Copyright © 2026 leesehyeon. All rights reserved. 열람용으로 공개합니다 — 복제·재배포·상업적 이용은 허락이 필요합니다.
