export PATH := /usr/local/pspdev/bin:$(PATH)
export PSPDEV := /usr/local/pspdev

TARGET := TH06PSP
MECC_DIR := psp/third_party/me-custom-core
MECC_BUILD_DIR := $(MECC_DIR)/build
MECC_LIB := $(MECC_BUILD_DIR)/libme-core.a
MECC_INPUTS := $(wildcard $(MECC_DIR)/*.c $(MECC_DIR)/*.h $(MECC_DIR)/*.S \
               $(MECC_DIR)/kernel/*.c $(MECC_DIR)/kernel/*.h $(MECC_DIR)/kernel/*.exp) \
               $(MECC_DIR)/CMakeLists.txt $(MECC_DIR)/kernel/CMakeLists.txt

# src/main.cpp is compiled with its entry renamed, then called by psp/main.cpp.
ENGINE_SRCS := $(filter-out src/SoundPlayer.cpp src/graphics/FixedFunctionGL.cpp src/graphics/WebGL.cpp src/graphics/GLFunc.cpp, \
               $(wildcard src/*.cpp src/graphics/*.cpp src/pbg3/*.cpp src/thirdparty/*.cpp))
ENGINE_SRCS += src/midi/MidiDefault.cpp
# The boot diagnostics (on-screen boot log, exception dump, boot watchdog,
# escape combo) are in both configurations on purpose: the probe is where the
# safety net itself gets tested before the body has to rely on it.
DIAG_SRCS := psp/video.cpp psp/fileio.cpp psp/diag.cpp psp/text.cpp psp/memory.cpp
ifeq ($(BOOT_PROBE),1)
SRCS := psp/main.cpp $(DIAG_SRCS)
else
SRCS := psp/main.cpp $(ENGINE_SRCS) shim/sdl_stub.cpp shim/img_decode.cpp psp/SoundPlayerPsp.cpp psp/render_backend_stubs.cpp psp/graphics/PspGuGfx.cpp psp/input.cpp psp/audio.cpp psp/audio_me.c psp/platform_hooks.cpp $(DIAG_SRCS)
endif
OBJS := $(patsubst %.cpp,%.o,$(SRCS))
OBJS := $(patsubst %.c,%.o,$(OBJS))

PSP_EBOOT_TITLE := Touhou 6 PSP Native
PSP_EBOOT_ICON := psp/assets/ICON0.PNG
PSP_FW_VERSION := 660
BUILD_PRX := 0
EXTRA_TARGETS := EBOOT.PBP

CXXFLAGS := -std=gnu++17 -O2 -G0 -Wall -Wextra -Wno-unused-parameter \
            -fno-exceptions -fno-rtti -fno-pic -DPSP -DTH06_PSP \
            -DGAME_WINDOW_WIDTH_REAL=480 -DGAME_WINDOW_HEIGHT_REAL=272 \
            -Ipsp -Ishim -Ishim/cxxcompat -Isrc -I/usr/local/pspdev/psp/include/freetype2 \
            -include psp/cxxcompat/minmax.hpp -include psp/platform_hooks.hpp
CFLAGS := -O2 -G0 -Wall -Wextra -fno-pic \
          -Ipsp -Ipsp/third_party/me-custom-core
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
# make REPLAY_TRACE=1 -- replay the user's stage-1 capture without input
# injection or invulnerability, and trace only a genuinely lethal boss hit.
# Keep this as a separate diagnostic executable; never ship it as the normal
# menu-driven build.
ifeq ($(REPLAY_TRACE),1)
CXXFLAGS += -DTH06_PSP_REPLAY_TRACE
PSP_EBOOT_TITLE := TH06 PSP exact replay trace
endif
# make PS3_REPLAY=1 -- boot the unmodified replay copied from the PS3 build.
# This is a single-EBOOT hands-off validation build, not a second XMB entry.
ifeq ($(PS3_REPLAY),1)
CXXFLAGS += -DTH06_PSP_PS3_REPLAY
PSP_EBOOT_TITLE := Touhou 6 PSP PS3 replay
endif
LIBS := -L$(MECC_BUILD_DIR) -lme-core \
        -lpspgum -lpspgu -lpspdisplay -lpspctrl -lpsppower -lpspaudio \
        -lfreetype -lbz2 -lpng16 -ljpeg -lz -lm -lstdc++ -lsupc++ -lpspsdk \
        -Wl,--wrap=fopen -Wl,--wrap=_Znwj \
        -Wl,--wrap=_ZN11Pbg3Archive4LoadEPKc \
        -Wl,--wrap=_ZN11Pbg3Archive19ReadDecompressEntryEmPKc \
        -Wl,--wrap=_ZN5Chain12RunCalcChainEv \
        -Wl,--wrap=_ZN5Chain12RunDrawChainEv \
        -Wl,--wrap=_ZN3Gui8OnUpdateEPS_ \
        -Wl,--wrap=_ZN3Gui13RegisterChainEv \
        -Wl,--wrap=_ZN12AsciiManager13InitializeVmsEv \
        -Wl,--wrap=_ZN11GameManager13RegisterChainEv \
        -Wl,--wrap=_ZN11GameManager8CutChainEv \
        -Wl,--wrap=_ZN10Supervisor9StopAudioEv \
        -Wl,--wrap=_ZN5Stage8CutChainEv \
        -Wl,--wrap=_ZN13BulletManager8CutChainEv \
        -Wl,--wrap=_ZN6Player8CutChainEv \
        -Wl,--wrap=_ZN12EnemyManager8CutChainEv \
        -Wl,--wrap=_ZN10EclManager6UnloadEv \
        -Wl,--wrap=_ZN13EffectManager8CutChainEv \
        -Wl,--wrap=_ZN3Gui8CutChainEv \
        -Wl,--wrap=_ZN13ReplayManager13StopRecordingEv \
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
        -Wl,--wrap=_ZN10AnmManager27CopySurfaceRectToBackBufferElllllll \
        -Wl,--wrap=_ZN11SoundPlayer14PlaySoundByIdxE8SoundIdx \
        -Wl,--wrap=_ZNK10EclManager10CallEclSubEP15EnemyEclContexts \
        -Wl,--wrap=_ZN5Enemy16ResetEffectArrayEPS_ \
        -Wl,--wrap=_ZN5Enemy13UpdateEffectsEPS_ \
        -Wl,--wrap=_ZN12EnemyManager8OnUpdateEPS_ \
        -Wl,--wrap=_ZN13EffectManager8OnUpdateEPS_ \
        -Wl,--wrap=_ZN13EffectManager6OnDrawEPS_ \
        -Wl,--wrap=_ZN13ReplayManager13RegisterChainElPKc \
        -Wl,--wrap=_ZN10Controller8GetInputEv \
        -Wl,--wrap=_ZN10Supervisor10LoadConfigEPKc \
        -Wl,--wrap=_ZN10Supervisor9PlayAudioEPKc \
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

.PHONY: release release-audit
release-audit:
	./tools/release_audit.sh

release: all release-audit psp/assets/NotoSansJP-Regular.ttf
	@stage_root=$$(mktemp -d); \
	stage="$$stage_root/TH06PSP"; \
	mkdir -p "$$stage/licenses/NotoSansJP" "$$stage/licenses/MECC" dist; \
	cp EBOOT.PBP psp/assets/NotoSansJP-Regular.ttf README.md CREDITS.md LICENSE "$$stage/"; \
	cp licenses/NotoSansJP/OFL.txt "$$stage/licenses/NotoSansJP/"; \
	cp psp/third_party/me-custom-core/LICENSE.md "$$stage/licenses/MECC/"; \
	stage_win=$$(wslpath -w "$$stage"); \
	zip_win=$$(wslpath -w "$$stage_root/th06-psp-native-preview.zip"); \
	powershell.exe -NoProfile -Command "Compress-Archive -LiteralPath '$$stage_win' -DestinationPath '$$zip_win' -Force"; \
	mv "$$stage_root/th06-psp-native-preview.zip" dist/th06-psp-native-preview.zip
	./tools/release_audit.sh

# build.mak does not otherwise notice title/icon-only changes.
$(PSP_EBOOT_SFO): Makefile
$(PSP_EBOOT): $(PSP_EBOOT_ICON)

src/main.o: CXXFLAGS += -Dmain=th06_engine_main

# RecordBootHeader uses __DATE__/__TIME__ from psp/main.cpp as the delivery
# identity.  Recompile that one small TU on every requested build; otherwise a
# changed backend can carry an old stamp and defeat the stale-EBOOT check.
.PHONY: FORCE_BUILD_STAMP
psp/main.o: FORCE_BUILD_STAMP
FORCE_BUILD_STAMP:
