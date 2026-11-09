# 사카츠쿠 PS3 (BLJM61064) PC 포팅 — 1.03 실행 기준

작성 2026-11-06. 근거는 세 가지입니다.
- 업데이트 PKG(A0103) 분석
- 복호화한 1.03 ELF
- RPCS3 v0.0.43 실행 로그 (펌웨어 4.93): 1.00 디스크 부팅 1회, 1.03 패치 부팅 1회(11분 진행. 세이브 로드, DLC 검사, 저장 포함)

RPCS3는 동작 참고용으로만 씁니다. 포팅판은 아래 규칙을 직접 구현합니다.

## 0. 현재 상태

- **1.03 전용 준비는 끝났습니다.** 코드, 입력 폴더, 부팅과 파일 경로 동작을 실측으로 확정했고, DLC는 제외했습니다.
- 남은 일은 1.03과 상관없는 포팅 본작업입니다 (7절).

## 1. 포팅 입력 (고정)

| 경로 (`PC PORT\` 기준) | 내용 | 검증 |
|---|---|---|
| `eboot_1.03\EBOOT.ELF` | 1.03 코드 (PPU, ELF64 big-endian) | sha1 `1a62725c…`. 세그먼트 HMAC 일치. RPCS3에 설치된 1.03 EBOOT에서 나온 것과 같은 바이트 |
| `runtime_1.03\dev_bdvd\` (`PS3_DISC.SFB`, `PS3_GAME\`) | 원본 디스크 사본 (1.00, 수정 없음). 읽기 전용 | 648개, 3,415,408,351바이트. 원본과 불일치 0 |
| `runtime_1.03\dev_hdd0\game\BLJM61064\` | 1.03 패치 사본. 쓰기 가능 (게임의 설치 데이터도 이 폴더에 들어감) | 6개. `pkg_1.03`과 불일치 0. EBOOT가 RPCS3 설치본과 같음 |
| `runtime_1.03\dev_hdd0\home\00000001\savedata\` | 세이브 (처음엔 비어 있음) | |
| `pkg_1.03\BLJM61064` | 업데이트 PKG를 푼 원본. 보관용 | |
| `merged_1.03` | 삭제함 (2026-11-06, 휴지통으로 이동) | 1.03은 DLC 아카이브를 패치 경로에서만 읽어서 필요 없음. 필요하면 `tools\merge_overlay.py`로 다시 만들 수 있음 |

## 2. 파일 시스템(VFS) 매핑 — `runtime_1.03` 구조 그대로

| PS3 경로 | PC 경로 | 쓰기 |
|---|---|---|
| `/dev_bdvd/` | `runtime_1.03\dev_bdvd\` | 읽기 전용 |
| `/dev_hdd0/` | `runtime_1.03\dev_hdd0\` | 쓰기 |
| `/dev_hdd0/game/NPJB00414/` (DLC) | 만들지 않음 | DLC 미사용 |
| `/app_home/`, `/dev_usb` | 미사용 (게임이 접근한 기록 없음) | |

## 3. 포팅판 응답 (1.03)

| 함수 | 응답 | 근거 |
|---|---|---|
| cellGameBootCheck | type=DISC, attributes=PATCH | RPCS3의 GD 규칙 |
| cellGamePatchCheck | 성공 | 실측 (1.00은 호출 0회) |
| cellGameContentPermit | `/dev_hdd0/game/BLJM61064`, `…/USRDIR` | 실측 접근 경로 |
| cellGameGetParamString | 패치 SFO 값 (APP_VER 01.03, TITLE) | |
| cellGameDataCheck(`BLJM61064`) | 있음 | 실측 |
| cellGameDataCheck(`"/"`) | 없음 | 실측 (1.00, 1.03 모두) |
| cellGameDataCheck(`NPJB00414`) | **없음 (DLC 미사용으로 확정)** | 실측과 같음 |
| sceNpDrmIsAvailable (DLC edat 25개) | **실패 (미보유로 확정)** | 실측과 같음 |
| cellSaveData* | `BLJM61064-SAVEDATA` 읽기/쓰기. `NPJB90607-SAVEDATA`는 읽기만 하고 없으면 없음으로 응답 | 실측 |
| cellGameCreateGameData | 설치 데이터를 만들 때 사용 | 관측 없음 (미설치 상태로도 진행됨) |
| cellGameContentErrorDialog | PC에서는 로그만 남김 | |

`cellGame`은 RPCS3에서도 HLE(재구현)이라서 RPCS3의 `cellGame.cpp`를 그대로 참고하면 됩니다.

## 4. 1.03 실행 실측 (RPCS3)

부팅: RPCS3는 원본 디스크를 `/dev_bdvd`에 마운트하고, `/dev_hdd0/game/BLJM61064/USRDIR/EBOOT.BIN`(GD, APP_VER 01.03)으로 바꿔 부팅합니다. 로그에는 `Updates found at /dev_hdd0/game/BLJM61064/`로 찍힙니다.

| 게임 시간 | 호출 / 접근 | 결과 |
|---|---|---|
| 0:09.499–0:09.509 | BootCheck → ContentPermit → **PatchCheck** → ContentPermit | PatchCheck 성공 |
| 0:12.208 | DataCheck(`"/"`) | 없음 |
| 0:15.67 | `data_ps3/install.pib` → DataCheck(`BLJM61064`) → `USRDIR/pxd_installed.pib` | 폴더 있음, 설치 표시 없음 → 미설치 상태로 진행 |
| 0:16.0 | 설치 목록의 각 파일: 디스크 원본 stat → HDD 설치본 `USRDIR/data/…` 열기 | 설치본 없음(ENOENT) → 디스크 데이터 사용 |
| 6:27.2 | cellSaveDataListAutoLoad, AutoLoad2(`BLJM61064-SAVEDATA`) | 세이브 로드 |
| 6:27.8 | DataCheck(`NPJB00414`) → sceNpDrmIsAvailable × 25 | 전부 미보유 |
| 6:47.16 | open `/dev_hdd0/game/BLJM61064/USRDIR/patch/dlc/3d/dlc.par` | 목차 2144바이트만 읽음 |
| 6:48.46 | AutoLoad2(`NPJB90607-SAVEDATA`) | 다른 타이틀의 세이브 확인 |
| 7:03.48 | open `/dev_hdd0/game/BLJM61064/USRDIR/patch/dlc/png/png_uniform_dlc.par` | 목차 9344바이트만 읽음 |
| 10:37.1 | cellSaveDataAutoSave2(`BLJM61064-SAVEDATA`) | 저장 |

DLC 아카이브를 연 횟수: 패치 `patch/dlc/3d/dlc.par` 6회, `patch/dlc/png/png_uniform_dlc.par` 2회. 디스크 `data/…dlc` 아카이브는 **0회**이고, 설치 검사 때 크기 확인(stat)만 했습니다.

## 5. 1.03 합격 기준 (포팅 빌드)

1. 실행 코드가 `EBOOT.ELF`(1.03)에서 나왔다.
2. 부팅 순서: BootCheck → ContentPermit → PatchCheck 성공 → ContentPermit.
3. 설치 확인: `install.pib` → DataCheck(`BLJM61064`) → `pxd_installed.pib` → `USRDIR/data/…` 확인.
4. 세이브 로드 직후 DataCheck(`NPJB00414`)와 edat 25개 검사가 나오고, 결과는 미보유다.
5. DLC 아카이브는 `/dev_hdd0/game/BLJM61064/USRDIR/patch/dlc/…`에서만 열리고, 목차는 2144 / 9344바이트를 읽는다.

## 6. 펌웨어 모듈 의존성 (1.03 실행 11분, RPCS3 기준)

| RPCS3 처리 | 모듈 | 포팅 대응 |
|---|---|---|
| 실제 펌웨어 코드 실행 (LLE) | liblv2, libfs, libgcm_sys, libsre, libspurs_jq, libfiber, libpngdec, libl10n, libnet, libhttp, libssl | 직접 구현하거나, 소유자 펌웨어에서 복호화해 EBOOT처럼 변환. libnet/libhttp/libssl은 오프라인 스텁 |
| RPCS3 재구현 (HLE) | libsysutil, libsysutil_game, libsysmodule, libaudio, libio, librtc, libsysutil_avconf_ext, libsysutil_np, libsysutil_np2, libsysutil_np_trophy, libsysutil_np_tus, libnetctl | RPCS3의 HLE 코드를 참고해 구현. NP 계열은 오프라인 스텁 |

EBOOT가 import하는 모듈은 28개입니다(`eboot_1.03\EBOOT_1.03_info.txt`). 그중 sceNpCommerce2와 cellRudp는 11분 동안 로드되지 않았습니다.

## 7. 남은 일 (1.03과 상관없는 포팅 본작업)

- PPU 코드: `EBOOT.ELF`를 PC 코드로 옮기는 방식(정적 재컴파일 등)을 정해야 합니다.
- SPU: SPURS 작업을 처리해야 합니다. CRI 오디오 디코더(ADX/HCA)와 엔진 pxd의 작업이 SPU에서 돕니다.
- 그래픽: GCM 명령을 PC 렌더러로 옮겨야 합니다. 디스크에 Windows용 셰이더(`shader/w32`)와 `silva_shader_w32.dll`이 있습니다.
- 오디오, 입력, 세이브, 온라인 스텁.

## 7-1. 포팅 빌드 검증 현황 (`Desktop\Sakatsuku_Port`, 2026-11-06)

ps3recomp v0.18.0 기반 포팅입니다. `cellGame` 덮어쓰기는 `stubs.cpp`의 `saka_register_patch_boot()`에 있고, 근거는 RPCS3 `cellGame.cpp`의 GD 경로입니다.

| 5절 기준 | 상태 | 근거 |
|---|---|---|
| 1. 1.03 코드 | 충족 | `game\EBOOT.ELF`(1.03)를 변환해서 빌드 |
| 2. 부팅 순서 | 충족 | 숨김 실행 `run5`: BootCheck(DISC+PATCH) → ContentPermit `/dev_bdvd/PS3_GAME` → PatchCheck CELL_OK → ContentPermit `/dev_hdd0/game/BLJM61064`. 타이틀 ID BLJM61064(패치 SFO) |
| 3. 설치 확인 | 충족 | DataCheck `"/"` → `"BLJM61064"`. 설치본 `USRDIR/data/3d/dlc/dlc.par` 등은 ENOENT로 RPCS3와 같음 |
| 4. DLC 검사 | 충족 | DataCheck `NPJB00414` 없음, edat 25개 미보유 |
| 5. DLC 아카이브 경로 | **미확인** | 타이틀(PRESS START)에서 멈춘 채 실행해서 해당 구간에 도달하지 않음. 입력이 필요함 |

타이틀 화면은 1.03 패치 부팅 상태로 정상 렌더링됩니다(D3D12, `RT_DISPLAY_BY_SIZE=1`).

## 8. 덤프 해시와 리컴프 사전 조사 (2026-11-06)

| 파일 | sha256 |
|---|---|
| `Saka Tsuku - Pro Soccer Club o Tsukurou! (Japan).iso` | `0cf82e573b621fd92b70d731e28840e33c40c631ddb53f590a5ce8375a621984` |
| `JP0177-BLJM61064_00-SAKATSUKUPS32013-A0103-V0100-PE.pkg` | `c0d210d7857d8417394f59bd87546c5ad01300c2918ac0b4fe6972a194c98912` |
| `eboot_1.03\EBOOT.ELF` | `a51447964301b558cdc2b279eca1f4e92c70dd471fd6ba939695490b0f1c100f` |

- **SPU 프로그램:**
  - EBOOT 안에 SPU ELF 4개(합계 51KB): `gs_task.elf` 0x94E4, `sl_decoder.elf` 0x17D4, `sl_lz_decode.elf` 0x16D4, `sl_memcpy.elf` 0x754.
  - CRI 오디오 SPU 작업: ADX Decoder, Base, SpursJob AtomDecoder, AsrVoice, DspPitchShifter, HcaMixer.
- **USRDIR 데이터 형식:** `.par`(PARC) 296개 2.77GB, `.cpk` 2개 478MB, `.adx` 28개 116MB, `.txb` 125개, `.arch` 57개, `.dds` 28개.
- **별도 모듈:** `.sprx` 없음. Windows용 `shader/w32/silva_shader_w32*.dll` 2개만 있음.
