export PATH := /usr/local/pspdev/bin:$(PATH)
export PSPDEV := /usr/local/pspdev

TARGET := TH06PSP
MECC_DIR := psp/third_party/me-custom-core
MECC_BUILD_DIR := $(MECC_DIR)/build
MECC_LIB := $(MECC_BUILD_DIR)/libme-core.a
MECC_INPUTS := $(wildcard $(MECC_DIR)/*.c $(MECC_DIR)/*.h $(MECC_DIR)/*.S \
               $(MECC_DIR)/kernel/*.c $(MECC_DIR)/kernel/*.h $(MECC_DIR)/kernel/*.exp) \
               $(MECC_DIR)/CMakeLists.txt $(MECC_DIR)/kernel/CMakeLists.txt

# MIDI BGM from the user's gm.dls (psp/midi): the shared synthesizer and ZUN
# sequencer from touhou_dls_common.  Its object code runs on both the SC and
# the ME, so it is compiled once with ME-safe flags (no jump tables, no
# divide-by-zero traps, no forced includes, -G0) and shared by both, which is
# what makes the SC/ME bit-exact self-test meaningful.
TDLS_DIR ?= $(CURDIR)/third_party/touhou_dls_common
TDLS_NAMES := sha256 dls smf_scan subset manifest blob synth_tables msgs_control synth zun_seq
TDLS_SRCS := $(addprefix $(TDLS_DIR)/src/,$(addsuffix .cpp,$(TDLS_NAMES)))
TDLS_HDRS := $(wildcard $(TDLS_DIR)/include/tdls/*.hpp)
TDLS_OBJS := $(addprefix psp/midi/tdls_,$(addsuffix .o,$(TDLS_NAMES)))
ifeq ($(wildcard $(TDLS_DIR)/src/synth.cpp),)
$(error touhou_dls_common not found at $(TDLS_DIR); set TDLS_DIR)
endif
TDLS_SOURCE_ID := 0x$(shell cat $(TDLS_SRCS) $(TDLS_HDRS) | sha256sum | cut -c1-8)
MIDI_HDRS := $(wildcard psp/midi/*.hpp psp/midi/*.h)
MIDI_SC_SRCS := psp/midi/MidiPsp.cpp psp/midi/MidiBuild.cpp psp/midi/MidiScript.cpp
MIDI_ME_CXXFLAGS := -I. -I$(PSPDEV)/psp/include -I$(PSPDEV)/psp/sdk/include -I$(TDLS_DIR)/include -Ipsp \
                    -std=gnu++17 -O2 -G0 -march=allegrex -mtune=allegrex -Wall -Wextra -fno-exceptions -fno-rtti \
                    -fno-pic -fno-jump-tables -mno-check-zero-division -DPSP -DTH06_PSP -DTDLS_DISABLE_TRACE -D_PSP_FW_VERSION=660

# src/main.cpp is compiled with its entry renamed, then called by psp/main.cpp.
ENGINE_SRCS := $(filter-out src/SoundPlayer.cpp src/graphics/FixedFunctionGL.cpp src/graphics/WebGL.cpp src/graphics/GLFunc.cpp, \
               $(wildcard src/*.cpp src/graphics/*.cpp src/pbg3/*.cpp src/thirdparty/*.cpp))
ENGINE_SRCS += src/midi/MidiDefault.cpp
# The boot diagnostics (on-screen boot log, exception dump, boot watchdog,
# escape combo) are in both configurations on purpose: the probe is where the
# safety net itself gets tested before the body has to rely on it.
DIAG_SRCS := psp/video.cpp psp/fileio.cpp psp/diag.cpp psp/text.cpp psp/memory.cpp psp/newlib_heap_geometry.cpp
ifeq ($(BOOT_PROBE),1)
SRCS := psp/main.cpp $(DIAG_SRCS)
else
SRCS := psp/main.cpp $(ENGINE_SRCS) shim/sdl_stub.cpp shim/img_decode.cpp psp/SoundPlayerPsp.cpp psp/render_backend_stubs.cpp psp/graphics/PspGuGfx.cpp psp/input.cpp psp/audio.cpp psp/audio_me.c psp/platform_hooks.cpp $(MIDI_SC_SRCS) psp/midi/MidiMe.cpp $(DIAG_SRCS)
endif
OBJS := $(patsubst %.cpp,%.o,$(SRCS))
OBJS := $(patsubst %.c,%.o,$(OBJS))
ifneq ($(BOOT_PROBE),1)
OBJS += $(TDLS_OBJS)
endif

PSP_EBOOT_TITLE := Touhou 6 PSP Native
PSP_EBOOT_ICON := psp/assets/ICON0.PNG
PSP_FW_VERSION := 660
BUILD_PRX := 0
EXTRA_TARGETS := EBOOT.PBP

CXXFLAGS := -std=gnu++17 -O2 -G0 -march=allegrex -mtune=allegrex -Wall -Wextra -Wno-unused-parameter \
            -fno-exceptions -fno-rtti -fno-pic -DPSP -DTH06_PSP \
            -DGAME_WINDOW_WIDTH_REAL=480 -DGAME_WINDOW_HEIGHT_REAL=272 \
            -Ipsp -Ishim -Ishim/cxxcompat -Isrc -I$(TDLS_DIR)/include -I/usr/local/pspdev/psp/include/freetype2 \
            -include psp/cxxcompat/minmax.hpp -include psp/platform_hooks.hpp
CFLAGS := -O2 -G0 -march=allegrex -mtune=allegrex -Wall -Wextra -fno-pic \
          -Ipsp -Ipsp/third_party/me-custom-core

# These three translation units account for the measured stage/background and
# sprite draw time.  Keep IEEE/NaN behaviour intact (real PSP has already
# exposed invalid-float paths that PPSSPP tolerated), but let GCC perform the
# more aggressive inlining and loop optimisation that is safe at -O3.
src/AnmManager.o src/BulletManager.o src/Stage.o: CXXFLAGS += -O3 -funroll-loops
ifeq ($(BOOT_PROBE),1)
CXXFLAGS += -DTH06_PSP_BOOT_PROBE
PSP_EBOOT_TITLE := TH06 PSP boot probe
endif
# make BOOT_PROBE=1 SELFTEST=1 — hangs on purpose so the boot watchdog can be
# re-verified end to end.  Never ship a SELFTEST build.
ifeq ($(SELFTEST),1)
CXXFLAGS += -DTH06_PSP_SELFTEST
PSP_EBOOT_TITLE := TH06 PSP watchdog selftest
endif
# make STAGE_SOAK=1 -- PPSSPP-only unattended stage 1 -> stage 2 test.
# This never belongs in the executable deployed to real hardware.
ifeq ($(STAGE_SOAK),1)
CXXFLAGS += -DTH06_PSP_STAGE_SOAK
PSP_EBOOT_TITLE := TH06 PSP stage soak test
endif
# make RECORD_SOAK=1 -- unattended recording / ending / result / Extra test.
# Uses the same replay allocation path on PPSSPP and real hardware. A separate
# diagnostic EBOOT only; never the normal menu-driven product.
ifeq ($(RECORD_SOAK),1)
CXXFLAGS += -DTH06_PSP_RECORD_SOAK
PSP_EBOOT_TITLE := TH06 PSP record soak test
endif
# Default for current MIDI-only diagnosis: RECORD_SOAK=1 RECORD_START_MIDI=1.
# One normal/ending/result/Extra pass, no preceding WAV measurement. Existing
# WAV logs remain the baseline; the two-pass harness above is historical.
ifeq ($(RECORD_START_MIDI),1)
ifneq ($(RECORD_SOAK),1)
$(error RECORD_START_MIDI=1 requires RECORD_SOAK=1)
endif
CXXFLAGS += -DTH06_PSP_RECORD_START_MIDI
PSP_EBOOT_TITLE := TH06 PSP MIDI-only soak test
endif
ifeq ($(RECORD_LAYOUT),1)
ifneq ($(RECORD_SOAK),1)
$(error RECORD_LAYOUT=1 requires RECORD_SOAK=1)
endif
CXXFLAGS += -DTH06_PSP_RECORD_LAYOUT
endif
# make RESULT_SOAK=1 -- PPSSPP-only unattended normal-play -> game-over result test.
# It takes the same teardown/registration path as choosing "No" at Continue.
ifeq ($(RESULT_SOAK),1)
CXXFLAGS += -DTH06_PSP_RESULT_SOAK
PSP_EBOOT_TITLE := TH06 PSP result soak test
endif
# make REPLAY_TRACE=1 -- replay the user's stage-1 capture without input
# injection or invulnerability, and trace only a genuinely lethal boss hit.
# Keep this as a separate diagnostic executable; never ship it as the normal
# menu-driven build.
ifeq ($(REPLAY_TRACE),1)
CXXFLAGS += -DTH06_PSP_REPLAY_TRACE
PSP_EBOOT_TITLE := TH06 PSP exact replay trace
endif
# Framebuffer comparison is a separate PPSSPP-only replay build. It must never
# enter the normal product, and it requires the same unattended replay input.
ifeq ($(RENDER_HASH_TEST),1)
ifneq ($(REPLAY_TRACE),1)
$(error RENDER_HASH_TEST=1 requires REPLAY_TRACE=1)
endif
CXXFLAGS += -DTH06_PSP_RENDER_HASH_TEST
PSP_EBOOT_TITLE := TH06 PSP pixel hash test
endif
# make PS3_REPLAY=1 -- boot the unmodified replay copied from the PS3 build.
# This is a single-EBOOT hands-off validation build, not a second XMB entry.
ifeq ($(PS3_REPLAY),1)
CXXFLAGS += -DTH06_PSP_PS3_REPLAY
PSP_EBOOT_TITLE := Touhou 6 PSP PS3 replay
endif
# make MIDI_PCMTEST=1 -- PPSSPP/PC check only: render the fixed MIDI scripts
# (psp/midi/MidiScript.cpp) on the SC, log their PCM hashes and exit.  Never
# a release EBOOT (tools/release_audit.sh rejects its marker).
ifeq ($(MIDI_PCMTEST),1)
CXXFLAGS += -DTH06_PSP_MIDI_PCMTEST
PSP_EBOOT_TITLE := TH06 PSP MIDI PCM TEST
endif
# make MIDI_MEMSIM=1 -- PPSSPP check only: MIDI commands and song buffers run
# as in ME mode on a PSP-1000 (blob released from main RAM) but nothing is
# synthesized, so PPSSPP model 0 can check the 32 MB stage transitions.
ifeq ($(MIDI_MEMSIM),1)
CXXFLAGS += -DTH06_PSP_MIDI_MEMSIM
PSP_EBOOT_TITLE := TH06 PSP MIDI MEMSIM TEST
endif
LIBS := -L$(MECC_BUILD_DIR) -lme-core \
        -lpspgum_vfpu -lpspvfpu -lpspmath -lpspgu -lpspdisplay -lpspctrl -lpsppower -lpspaudio \
        -lfreetype -lbz2 -lpng16 -ljpeg -lz -lm -lstdc++ -lsupc++ -lpspsdk \
        -Wl,--wrap=fopen -Wl,--wrap=_Znwj -Wl,--wrap=malloc -Wl,--wrap=memalign \
        -Wl,--wrap=sceKernelExitGame \
        -Wl,--wrap=sceKernelAllocPartitionMemory \
        -Wl,--wrap=sinf -Wl,--wrap=cosf -Wl,--wrap=rintf \
        -Wl,--wrap=_ZN11Pbg3Archive4LoadEPKc \
        -Wl,--wrap=_ZN11Pbg3Archive19ReadDecompressEntryEmPKc \
        -Wl,--wrap=_ZN5Chain12RunCalcChainEv \
        -Wl,--wrap=_ZN5Chain12RunDrawChainEv \
        -Wl,--wrap=_ZN3Gui8OnUpdateEPS_ \
        -Wl,--wrap=_ZN3Gui13RegisterChainEv \
        -Wl,--wrap=_ZN12AsciiManager13InitializeVmsEv \
        -Wl,--wrap=_ZN11GameManager13RegisterChainEv \
        -Wl,--wrap=_ZN11GameManager8CutChainEv \
        -Wl,--wrap=_ZN12ResultScreen13RegisterChainEl \
        -Wl,--wrap=_ZN10AnmManager11LoadSurfaceElPKc \
        -Wl,--wrap=_ZN10AnmManager7LoadAnmElPKcl \
        -Wl,--wrap=_ZN10Supervisor9StopAudioEv \
        -Wl,--wrap=_ZN5Stage8CutChainEv \
        -Wl,--wrap=_ZN13BulletManager8CutChainEv \
        -Wl,--wrap=_ZN6Player8CutChainEv \
        -Wl,--wrap=_ZN12EnemyManager8CutChainEv \
        -Wl,--wrap=_ZN10EclManager6UnloadEv \
        -Wl,--wrap=_ZN13EffectManager8CutChainEv \
        -Wl,--wrap=_ZN3Gui8CutChainEv \
        -Wl,--wrap=_ZN13ReplayManager13StopRecordingEv \
        -Wl,--wrap=_ZN13ReplayManager10SaveReplayEPKcPc \
        -Wl,--wrap=_ZN5Stage13RegisterChainEm \
        -Wl,--wrap=_ZN6Player13RegisterChainEh \
        -Wl,--wrap=_ZN13BulletManager13RegisterChainEPKc \
        -Wl,--wrap=_ZN12EnemyManager13RegisterChainEPKcS1_ \
        -Wl,--wrap=_ZN10EclManager4LoadEPKc \
        -Wl,--wrap=_ZN13EffectManager13RegisterChainEv \
        -Wl,--wrap=_ZN13EffectManager21EffectUpdateCallback4EP6Effect \
        -Wl,--wrap=_ZN6Player17CalcDamageToEnemyEPK7ZunVec3S2_Pb \
        -Wl,--wrap=_ZN13EffectManager14SpawnParticlesElPK7ZunVec3lm \
        -Wl,--wrap=_ZN13BulletManager14DespawnBulletsElb \
        -Wl,--wrap=_ZN12AsciiManager12CreatePopup1EPK7ZunVec3lm \
        -Wl,--wrap=_ZN10AnmManager23CopySurfaceToBackBufferElllll \
        -Wl,--wrap=_ZN10AnmManager27CopySurfaceRectToBackBufferElllllll \
        -Wl,--wrap=_ZN11SoundPlayer14PlaySoundByIdxE8SoundIdx \
        -Wl,--wrap=_ZNK10EclManager10CallEclSubEP15EnemyEclContexts \
        -Wl,--wrap=_ZN5Enemy16ResetEffectArrayEPS_ \
        -Wl,--wrap=_ZN5Enemy13UpdateEffectsEPS_ \
        -Wl,--wrap=_ZN12EnemyManager8OnUpdateEPS_ \
        -Wl,--wrap=_ZN13EffectManager8OnUpdateEPS_ \
        -Wl,--wrap=_ZN13EffectManager6OnDrawEPS_ \
        -Wl,--wrap=_ZN13BulletManager6OnDrawEPS_ \
        -Wl,--wrap=_ZN10AnmManager4DrawEPK5AnmVm \
        -Wl,--wrap=_ZN10AnmManager5Draw2EPK5AnmVm \
        -Wl,--wrap=_ZN13ReplayManager13RegisterChainElPKc \
        -Wl,--wrap=_ZN10Controller8GetInputEv \
        -Wl,--wrap=_ZN10Supervisor10LoadConfigEPKc \
        -Wl,--wrap=_ZN10Supervisor9PlayAudioEPKc \
        -Wl,--wrap=_ZN10MidiOutput12ReadFileDataEmPKc \
        -Wl,--wrap=_ZN10MidiOutput8LoadFileEPKc \
        -Wl,--wrap=_ZN10MidiOutput9ParseFileEl \
        -Wl,--wrap=_ZN10MidiOutput4PlayEv \
        -Wl,--wrap=_ZN10MidiOutput12StopPlaybackEv \
        -Wl,--wrap=_ZN10MidiOutput10SetFadeOutEm \
        -Wl,--wrap=_ZN8MainMenu19OnUpdateOptionsMenuEv \
        -Wl,--wrap=_ZN8MainMenu13RegisterChainEm \
        -Wl,--wrap=_ZN5Chain14AddToCalcChainEP9ChainElemi \
        -Wl,--wrap=_ZN5Chain14AddToDrawChainEP9ChainElemi

PSPSDK := $(shell /usr/local/pspdev/bin/psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

$(MECC_LIB): $(MECC_INPUTS)
	cmake -S $(MECC_DIR) -B $(MECC_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(MECC_BUILD_DIR) --target me-core

$(TARGET).elf: $(MECC_LIB)

.PHONY: release release-package release-audit
RELEASE_EBOOT ?= EBOOT.PBP
release-audit:
	./tools/release_audit.sh

release: all
	$(MAKE) release-package

# Package a previously verified normal binary without rebuilding its timestamp.
release-package: release-audit psp/assets/NotoSansJP-Regular.ttf
	@set -eu; stage_root=$$(mktemp -d); \
	stage="$$stage_root/TH06PSP"; \
	mkdir -p "$$stage/docs/images" "$$stage/licenses/NotoSansJP" "$$stage/licenses/MECC" "$$stage/licenses/touhou_dls_common" dist; \
	cp "$(RELEASE_EBOOT)" "$$stage/EBOOT.PBP"; \
	cp psp/assets/NotoSansJP-Regular.ttf README.md README_EN.md CREDITS.md LICENSE "$$stage/"; \
	cp docs/53-midi.md "$$stage/docs/"; \
	cp docs/images/install-01-copy-folder.png docs/images/install-02-dat-files-reference.png "$$stage/docs/images/"; \
	cp licenses/NotoSansJP/OFL.txt "$$stage/licenses/NotoSansJP/"; \
	cp psp/third_party/me-custom-core/LICENSE.md "$$stage/licenses/MECC/"; \
	cp third_party/touhou_dls_common/LICENSE "$$stage/licenses/touhou_dls_common/"; \
	stage_win=$$(wslpath -w "$$stage"); \
	zip_win=$$(wslpath -w "$$stage_root/th06-psp-native-preview.zip"); \
	powershell.exe -NoProfile -Command "Compress-Archive -LiteralPath '$$stage_win' -DestinationPath '$$zip_win' -Force"; \
	mv "$$stage_root/th06-psp-native-preview.zip" dist/th06-psp-native-preview.zip
	./tools/release_audit.sh

# build.mak does not otherwise notice title/icon-only changes.
$(PSP_EBOOT_SFO): Makefile
$(PSP_EBOOT): $(PSP_EBOOT_ICON)

src/main.o: CXXFLAGS += -Dmain=th06_engine_main
psp/main.o psp/SoundPlayerPsp.o psp/platform_hooks.o psp/memory.o: psp/memory.hpp psp/newlib_heap_geometry.hpp

# Shared synthesizer objects and the ME job handler: ME-safe flags only.
psp/midi/tdls_%.o: $(TDLS_DIR)/src/%.cpp $(TDLS_HDRS)
	$(CXX) $(MIDI_ME_CXXFLAGS) -c -o $@ $<
psp/midi/MidiMe.o: psp/midi/MidiMe.cpp $(MIDI_HDRS) $(TDLS_HDRS)
	$(CXX) $(MIDI_ME_CXXFLAGS) -c -o $@ $<
$(patsubst %.cpp,%.o,$(MIDI_SC_SRCS)): $(MIDI_HDRS) $(TDLS_HDRS)
psp/midi/MidiPsp.o: CXXFLAGS += -DTH06_TDLS_SOURCE_ID=$(TDLS_SOURCE_ID)u
psp/midi/MidiPsp.o: $(TDLS_SRCS)

# Audited against this SDK's newlib 4.5.0 _malloc_r (request+19, &~15).
# The scanner rejects unsupported layouts instead of guessing a free block.
psp/newlib_heap_geometry.o: CXXFLAGS += -DTH06_PSP_NEWLIB_4_5_0_DLMALLOC=1 -DTH06_PSP_NEWLIB_MALLOC_ALIGNMENT=16
psp/graphics/PspGuGfx.o: CXXFLAGS += -DTH06_PSP_NEWLIB_4_5_0_DLMALLOC=1 -DTH06_PSP_NEWLIB_MALLOC_ALIGNMENT=16
psp/audio_me.o: psp/midi/midi_me_protocol.h
psp/SoundPlayerPsp.o psp/audio.o psp/main.o psp/platform_hooks.o: psp/midi/MidiPsp.hpp

# RecordBootHeader uses __DATE__/__TIME__ from psp/main.cpp as the delivery
# identity.  Recompile that one small TU on every requested build; otherwise a
# changed backend can carry an old stamp and defeat the stale-EBOOT check.
.PHONY: FORCE_BUILD_STAMP
psp/main.o: FORCE_BUILD_STAMP
FORCE_BUILD_STAMP:
