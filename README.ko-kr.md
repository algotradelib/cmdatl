# cmdatl

<!-- badges: start -->
[![cmake-check](https://github.com/algotradelib/cmdatl/actions/workflows/cmake-check.yaml/badge.svg?branch=main)](https://github.com/algotradelib/cmdatl/actions/workflows/cmake-check.yaml?query=branch%3Amain)
[![codecov](https://codecov.io/gh/algotradelib/cmdatl/graph/badge.svg)](https://codecov.io/gh/algotradelib/cmdatl)
<!-- badges: end -->

<!-- multilingual: start -->
<p align="left">
  <a href="README.md">English</a> &middot;
  <b>한국어</b>
</p>
<!-- multilingual: end -->

> 이 문서는 [README](README.md)를 짧게 요약한 안내입니다.
> 두 문서의 내용이 다르면 영어 README를 따르세요.

cmdatl은 [algotradelib](https://github.com/algotradelib/algotradelib)을 CLI로 쓰는 도구입니다.
C++ 코드를 직접 작성하지 않아도 됩니다.
전략은 [atls](https://github.com/algotradelib/atlc)로 작성하고, 실행 설정은 TOML 파일에 적은 뒤, 명령 하나로 백테스트하거나 실행합니다.
AI 에이전트도 같은 명령으로 계좌를 조회하고, 운영자가 정한 한도 안에서 주문을 한 건씩 넣을 수 있습니다.

## 유의사항

- 계좌 조회와 주문 명령(`status`, `order` 등)은 이 버전에서 연습 계좌(mock account)에서만 동작합니다.
연습 계좌는 저장된 일봉으로 만든 시장이며, 증권사와 네트워크를 쓰지 않습니다.
증권사의 모의투자와는 다릅니다.
- `cmdatl run`은 운영자 파일에 KIS 계좌를 지정하면 그 계좌에서 주문합니다.
기본은 모의투자이고, 실전투자는 `env = "real"`로 직접 지정해야 합니다.
- 운영자가 armed 파일을 만들기 전에는 어떤 주문도 나가지 않습니다.
- Linux와 macOS에서 빌드하고 시험했습니다. Windows는 아직 지원하지 않습니다.

## 시작하기

설치 방법과 예제 명령은 [영어 README](README.md)를 참고하세요.
명령별 설명은 [docs](docs/) 폴더의 문서(영어)에 있습니다.

## 감사의 말

이 프로젝트는 생성형 AI 도구의 도움을 받아 개발했습니다.

## 라이선스

MIT 라이선스로 배포합니다 ([LICENSE](LICENSE)).
cmdatl은 어떠한 보증 없이 제공되며, cmdatl을 사용해 생긴 손해(거래 손실 포함)에 대해 저작자는 책임지지 않습니다.
이 저장소의 어떤 내용도 투자 권유가 아닙니다.
