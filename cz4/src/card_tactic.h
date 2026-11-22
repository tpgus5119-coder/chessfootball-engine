#pragma once
// 전술 대본 막대 (11-22 사용자 「막대 그래프로 모든 경우를 정할 수 있게」) — 값과 뜻 = docs/전술_대본_2026-11-22.md 3절.
// 규칙 지문 밖 파일 (자가학습 gen3 은 링크하지 않는다).
namespace cf {
namespace card {
namespace az {

constexpr int TAC_N = 3;    // 0 게겐프레싱 · 1 티키타카 · 2 카테나치오
constexpr int SL_N = 14;    // 막대 수 (문서 9절 번호 1~14 = 칸 0~13 — 11-22 밤, 역할별 13 + 「강하게」)
float tac_slider(int tac, int i);              // 0~10 (7 = 1~3칸 · 8·9 = 0~2명)
void set_tac_slider(int tac, int i, float v);  // 화면·측정이 고친다 (기본값 = 문서 3절)
float tac_eps(int tac);                        // 허용 폭 (유저 막대 아님 — 0.06)

} // namespace az
} // namespace card
} // namespace cf
