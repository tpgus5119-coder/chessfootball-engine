# chessfootball-engine

치즈볼(체스축구) 감독 모드 4대4 규칙 엔진과 알파제로 탐색의 C++ 부분입니다. 게임 본편(그림·소리·동작·화면)은 포함하지 않습니다.
이 저장소의 용도는 GitHub Actions로 자가대전 기보를 만드는 것입니다 (`.github/workflows/selfplay4.yml`).

- `cz4/src/` — 규칙 엔진(card.*) · 망 입력·Gumbel 탐색(card_az.*) · 게임 저장소 gdext/src 와 같은 파일
- `cz4/tools/gen3.cpp` — 자가대전 기보 공장

빌드: `c++ -O3 -std=c++17 cz4/tools/gen3.cpp cz4/src/card.cpp cz4/src/card_az.cpp cz4/src/card_duel.cpp cz4/src/engine.cpp cz4/src/nnue.cpp -o gen3`

Copyright © 2026 leesehyeon. All rights reserved. 열람용으로 공개합니다 — 복제·재배포·상업적 이용은 허락이 필요합니다.
