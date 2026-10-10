# STM32 + ESP-01S Wi-Fi Sensor Monitoring System

STM32 Nucleo-F411RE와 ESP-01S를 이용해 아날로그 센서 값을 측정하고, Wi-Fi를 통해 서버로 전송하여 상태를 모니터링하는 시스템이다.  
센서 값에 따라 NORMAL, WARNING, INTERLOCK 상태를 판정하며, 서버/통신 이상 시 COMM_ERROR 상태를 표시하도록 구현했다.

---

## 1. 프로젝트 개요

이 프로젝트의 목표는 다음과 같다.

- STM32 ADC로 센서 값 측정
- ESP-01S를 이용한 Wi-Fi 통신
- HTTP GET 방식으로 서버에 데이터 전송
- 센서 값에 따른 상태 머신 구현
- 상태별 LED 점멸 속도 차등 표시
- 서버 장애 및 통신 오류 복구 처리

---

## 2. 시스템 구성

```text
가변저항 10K (A0 / PA0)
        ↓ ADC1_IN0, 12bit
STM32 Nucleo-F411RE
  ├─ 200ms마다 ADC 샘플링 및 상태 판정
  ├─ 상태별 LED 점멸 (LD2)
  ├─ USART2 → PC (Tera Term 로그)
  └─ USART1 → ESP-01S
        ↓ Wi-Fi
Python HTTP Server (VM / port 5000)
        ↓
sensor_log.csv 저장
        ↓
Python 시각화(sensor_plot.png)
```

---

## 3. 사용 부품

- STM32 Nucleo-F411RE
- ESP-01S
- 가변저항 10K
- 브레드보드
- 점퍼선
- USB 케이블
- 3.3V 전원 환경

---

## 4. 하드웨어 연결

### STM32 ↔ ESP-01S

- STM32 TX(USART1) → ESP RX
- STM32 RX(USART1) ← ESP TX
- STM32 GND ↔ ESP GND
- 3.3V ↔ ESP VCC, EN(CH_PD)

### 센서 입력

- 가변저항 출력 → PA0 (ADC1_IN0)
- 가변저항 양 끝 → 3.3V, GND

> ESP-01S는 순간 전류 요구량이 커서 전원 품질에 민감하다. 실제 동작 중 Wi-Fi 연결 실패가 발생할 경우 별도 3.3V 전원 또는 디커플링 커패시터 추가가 필요할 수 있다.

---

## 5. 개발 환경

- STM32CubeIDE
- STM32 HAL Driver
- Tera Term
- Ubuntu Server
- Python 3
- pandas
- matplotlib

---

## 6. 주요 기능

### 6.1 ADC 센서 측정
- STM32 ADC1으로 아날로그 값을 읽는다.
- 12비트 범위(0~4095) 값을 사용한다.
- 200ms 주기로 샘플링한다.

### 6.2 Wi-Fi 전송
- ESP-01S AT firmware를 사용한다.
- STM32가 AT 명령을 통해 공유기에 접속한다.
- 5초마다 센서 값을 HTTP GET으로 서버에 전송한다.

예시 요청:
```http
GET /data?value=2048&state=WARNING HTTP/1.1
Host: 192.168.219.111:5000
Connection: close
```

### 6.3 상태 머신
센서 값에 따라 다음 상태를 판정한다.

- NORMAL
- WARNING
- INTERLOCK
- COMM_ERROR

### 6.4 히스테리시스
경계값 근처에서 상태가 빠르게 흔들리는 현상(chatter)을 막기 위해 히스테리시스를 적용했다.

| 전이 | 조건 |
|---|---|
| NORMAL → WARNING | ADC ≥ 2000 |
| WARNING → NORMAL | ADC < 1900 |
| WARNING → INTERLOCK | ADC ≥ 3000 |
| INTERLOCK → WARNING | ADC < 2800 |

### 6.5 상태별 LED 점멸
| 상태 | LED 주기 |
|---|---|
| NORMAL | 1000 ms |
| WARNING | 400 ms |
| INTERLOCK | 100 ms |
| COMM_ERROR | 2000 ms |

### 6.6 통신 오류 복구
- HTTP 전송 실패 횟수를 누적한다.
- 3회 연속 실패 시 `COMM_ERROR` 상태로 전환한다.
- `AT+CWJAP?`로 Wi-Fi 연결 여부를 확인한다.
- Wi-Fi 끊김이면 재접속 시도, Wi-Fi 정상인데 서버만 죽은 경우 서버 다운으로 판단한다.
- 서버 복구 후 자동으로 정상 전송 상태로 복귀한다.

---

## 7. 소프트웨어 구조

### 7.1 주기 분리
- ADC 샘플링: 200ms
- 서버 전송: 5000ms

### 7.2 핵심 상태 변수
- `level_state`: ADC 값으로 판정한 상태
- `sys_state`: 최종 표시 상태
- `comm_error`: 통신 오류 플래그
- `fail_count`: 전송 실패 횟수

### 7.3 동작 흐름

```text
부팅
 → Wi-Fi 연결
 → ADC 읽기
 → 상태 판정
 → 상태별 LED 점멸
 → 5초마다 서버 전송
    → 성공: NORMAL/WARNING/INTERLOCK 유지
    → 실패 누적 3회: COMM_ERROR
       → Wi-Fi 확인
          → 미접속: 재접속
          → 접속 중: 서버 다운 판단
```

---

## 8. 서버 구성

Python 기본 HTTP 서버를 기반으로 간단한 데이터 수집 서버를 사용했다.

서버 역할:
- `/data?value=...&state=...` 요청 수신
- timestamp, value, state, client_ip를 CSV 파일에 저장
- 응답으로 `OK` 반환

저장 예시:
```csv
2026-10-09T07:17:15,19,NORMAL,192.168.219.107
2026-10-09T07:17:20,2044,WARNING,192.168.219.107
2026-10-09T07:17:25,3678,INTERLOCK,192.168.219.107
```

---

## 9. 실행 결과

### 9.1 상태 전이 및 시각화

아래 그래프는 2026-10-09 16:00 이후 시험 구간만 시각화한 결과이다.  
ADC 값 변화에 따라 NORMAL, WARNING, INTERLOCK 상태가 임계값 기준대로 분류되는 것을 확인할 수 있다.

!Sensor test plot

### 9.2 전체 로그 기반 그래프

여러 차례 시험 전체를 포함한 그래프는 아래와 같다.

!Full sensor plot

---

## 10. 시험 항목 및 결과

| 시험 항목 | 결과 |
|---|---|
| STM32 ADC 측정 | 성공 |
| ESP-01S Wi-Fi 연결 | 성공 |
| HTTP 서버 전송 | 성공 |
| CSV 저장 | 성공 |
| NORMAL/WARNING/INTERLOCK 상태 전이 | 성공 |
| 히스테리시스 경계 시험 | 성공 |
| 상태별 LED 점멸 확인 | 성공 |
| 서버 중단 후 COMM_ERROR 전환 | 성공 |
| 서버 복구 후 자동 재전송 | 성공 |
| ESP 전원 복구 후 자동 재연결 | 성공 |
| 공유기 단절 시험 | 미실시 |

---

## 11. 트러블슈팅

### 11.1 Wi-Fi 연결 실패
- 원인 후보: 5GHz 공유기, 전원 부족, 비밀번호 오류, AP 거리 문제
- 대응:
  - 2.4GHz Wi-Fi 사용
  - SSID 스캔(`AT+CWLAP`)으로 AP 확인
  - 전원 품질 점검
  - `CWJAP?`로 실제 연결 상태 확인

### 11.2 서버 재시작 후 재연결 문제
- 증상: `ALREADY CONNECTED` 또는 `ERROR`
- 원인: 이전 TCP 세션이 닫히지 않은 상태
- 해결: `AT+CIPCLOSE` 후 `CIPSTART` 재시도

### 11.3 통신 실패 상태 표시 지연
- 증상: `COMM_ERROR` 표시가 늦게 나타남
- 해결: `comm_error = 1;` 직후 `refresh_state()` 호출

### 11.4 Python 패키지 설치 오류
- 증상: `externally-managed-environment`
- 원인: Ubuntu의 PEP 668 정책
- 해결: `python3 -m venv .venv`로 가상환경 생성 후 설치

---

## 12. 보안 정리

Wi-Fi SSID, 비밀번호, 서버 IP는 `secrets.h`로 분리했다.

예시:
```c
#define SECRET_WIFI_SSID  "YOUR_WIFI_SSID"
#define SECRET_WIFI_PW    "YOUR_WIFI_PASSWORD"
#define SECRET_SERVER_IP  "192.168.0.100"
```

실제 `secrets.h`는 `.gitignore`에 추가하고, 저장소에는 `secrets.h.example`만 포함한다.

---

## 13. 프로젝트를 통해 배운 점

- STM32의 ADC, UART, GPIO를 함께 다루는 구조를 익힐 수 있었다.
- ESP-01S와 AT 명령 기반 Wi-Fi 통신 흐름을 이해할 수 있었다.
- 상태 머신과 히스테리시스를 실제 제어 로직에 적용해 볼 수 있었다.
- 통신 실패와 센서 상태를 분리해 설계하는 중요성을 확인했다.
- 임베디드 시스템에서 복구 로직과 로그의 중요성을 경험했다.

---

## 14. 향후 개선 사항

- 공유기 단절 시험 추가 수행
- 실제 센서(온도, 조도, 가스 센서 등)로 확장
- Flask 기반 서버 또는 DB 연동
- 실시간 웹 대시보드 구성
- 인터록 상태에서 릴레이/부저 같은 실제 출력 제어 추가

---

## 15. 파일 설명

- `Core/Src/main.c` : STM32 메인 로직
- `Core/Inc/secrets.h.example` : 민감정보 예시 파일
- `server/server.py` : Python 수집 서버
- `server/sensor_log.csv` : 수집 로그
- `server/plot_sensor_log.py` : CSV 시각화 스크립트
- `docs/sensor_plot_test.png` : 2026-10-09 16:00 이후 상세 그래프
- `docs/sensor_plot_full.png` : 전체 기간 그래프

---

## 16. 결론

STM32와 ESP-01S를 이용해 센서 데이터를 Wi-Fi로 서버에 전송하고, 상태 머신과 오류 복구를 포함한 모니터링 시스템을 구현했다.  
단순 측정/전송을 넘어, 상태 판정, 시각화, 서버 장애 대응까지 포함한 임베디드 IoT 미니 프로젝트로 확장할 수 있었다.
