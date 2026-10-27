# DeckLink-Pico

**Pico(RP2350) LCD를 스트림덱처럼 사용하는 Windows용 터치 버튼 데크**입니다.
화면의 버튼을 눌러 즐겨찾기 사이트를 열고, 작업표시줄 앱을 실행·전환하고,
자주 쓰는 프로그램을 실행합니다. USB로 연결한 PC에서 Python 브리지를 함께 실행합니다.

## 사용 제품 · 차량용과 구분

사용 제품은 **Waveshare RP2350-Touch-LCD-3.49**입니다.
일반 Pico/Pico W 보드가 아니라 **RP2350 기반 3.49인치 터치 LCD 일체형 제품**용입니다.
구매 제품명과 보드 뒷면 MCU가 RP2350인지 먼저 확인하세요.

| 프로젝트 | 제품 | 용도 | 연결 |
|---|---|---|---|
| 이 저장소: DeckLink-Pico | RP2350-Touch-LCD-3.49 | PC 터치 버튼 데크 | USB + Windows Python 브리지 |
| [DeckLink-Car-ESP32](https://github.com/hejerd/DeckLink-Car-ESP32) | ESP32-S3-Touch-LCD-3.49 | 차량용 음악 HMI | Android 앱 + BLE/Wi-Fi |

**두 제품의 펌웨어는 서로 호환되지 않습니다.**
이 저장소에는 차량용 Android 앱과 차량 장착 사진을 포함하지 않습니다.

## 주요 기능

- **버튼 페이지 1:** 즐겨찾기 웹사이트 6개 열기.
- **버튼 페이지 2:** Windows 작업표시줄의 첫 6개 앱 실행·전환 (`Win+1` ~ `Win+6`).
- **버튼 페이지 3:** 시작 메뉴 바로가기를 찾아 지정한 프로그램 실행.
- **음악 화면:** 곡 정보, 앨범아트, 가사 표시와 재생·일시정지·이전·다음 곡 제어.
- **시스템 화면:** CPU·GPU·메모리 상태 표시. LibreHardwareMonitor 웹 서버 설정이 필요합니다.
- **화면 회전·절전:** IMU 기반 방향 전환, 화면 자동 꺼짐과 터치 깨우기.

## 구성

- `main.c`, `examples/`, `lib/`, `boards/`: Pico SDK 펌웨어.
- `desktop/deck.py`: 버튼 입력을 PC 동작으로 연결하고 아이콘·음악·시스템 정보를 보내는 브리지.
- `desktop/requirements.txt`: Windows Python 의존성.

## 1. Pico 펌웨어 빌드와 설치

Pico SDK 2.3.0, Arm GNU Toolchain, CMake, Ninja를 사용합니다.
Pico SDK 경로를 `PICO_SDK_PATH`에 설정하고 도구들이 PATH에 있도록 준비한 뒤:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
```

보드의 BOOT 버튼을 누른 채 USB를 연결하고,
`build/RP2350-Touch-LCD-3.49-LVGL.uf2`를 BOOT 모드 드라이브에 복사합니다.
재부팅 후 Windows 장치 관리자에서 보드의 COM 포트를 확인합니다.

## 2. Windows 브리지 실행

Windows용 Python 환경에서 실행합니다. `winsdk`, Windows 바로가기와 작업표시줄 연동을 사용하므로
Linux·macOS용으로 작성된 프로그램은 아닙니다.

```powershell
py -m venv .venv
.venv\Scripts\python.exe -m pip install -r desktop/requirements.txt
$env:DECKLINK_PORT = 'COM3'  # 장치 관리자에서 확인한 포트
.venv\Scripts\python.exe desktop/deck.py
```

보드와 PC는 USB로 연결합니다. 브리지가 실행 중이어야 버튼 동작과 정보 갱신을 사용할 수 있습니다.
기본 포트는 `COM3`이며 `DECKLINK_PORT` 환경변수로 바꿀 수 있습니다.

## 3. 버튼과 정보 설정

`desktop/deck.py` 상단에서 자신의 환경에 맞게 수정합니다.

| 설정 | 역할 |
|---|---|
| `FAVORITES` | 페이지 1의 사이트 이름·주소, 선택적으로 아이콘 주소 |
| `PAGE3_APPS` | 페이지 3의 앱 이름과 시작 메뉴 바로가기 검색어 |
| `LHM_URL` | LibreHardwareMonitor 웹 서버 주소. 기본값은 `localhost:8085` |
| `SENSORS` | 시스템 화면에 사용할 센서 경로. PC마다 다를 수 있음 |
| `NIGHT_OFF_HOUR`, `NIGHT_ON_HOUR` | 자동 화면 꺼짐 시간. 기본값은 20시~다음 날 8시 |
| `IDLE_OFF_SEC` | 입력·재생이 없을 때 화면을 끄기까지의 시간 |

페이지 2는 현재 작업표시줄 순서를 읽습니다. 페이지 3에 지정한 앱이 설치되어 있지 않거나
시작 메뉴 검색어가 맞지 않으면 해당 버튼으로 실행되지 않을 수 있습니다.

## 조작

- 버튼 화면에서 **좌우 스와이프:** 버튼 페이지 전환.
- **상하 스와이프:** 버튼·음악·시스템 화면 전환.
- 음악 화면 **터치:** 재생·일시정지.
- 음악 카드를 **왼쪽으로 밀기:** 다음 곡. **오른쪽으로 밀기:** 이전 곡.

음악 정보는 Windows 미디어 세션에서 읽습니다. 가사는 LRCLIB·NetEase에서 검색하고,
앨범아트는 미디어 세션 이미지와 iTunes·Deezer 검색을 사용합니다.
곡이나 재생 앱에 따라 정보·가사·앨범아트가 제공되지 않을 수 있습니다.

## 공개 범위

개인 서버 주소·토큰과 개인 즐겨찾기는 제거했습니다. 개인 자격증명을 소스에 넣지 마세요.
빌드 결과, 로그, 알림 내용, 캐시, 원본 압축파일은 공개 대상에서 제외합니다.
Waveshare 예제와 LVGL 등 타사 코드의 저작권·라이선스 표기는 각 파일에 유지했습니다.
