ifeq ($(shell uname -s),Darwin)
SYSTEM = macos
else ifneq ($(or $(filter Windows_NT,$(OS)),$(filter MINGW%,$(shell uname -s))),)
SYSTEM = windows
else
SYSTEM = linux
endif

ifeq ($(SYSTEM),macos)
PREFIX ?= /Applications
APP = build/Quarrel.app/Contents/MacOS/Quarrel
else ifeq ($(SYSTEM),windows)
PREFIX ?= $(subst \,/,$(LOCALAPPDATA))/Programs/Quarrel
APP = build/Quarrel.exe
START = $(subst \,/,$(APPDATA))/Microsoft/Windows/Start Menu/Programs
else
PREFIX ?= $(HOME)/.local
APP = build/quarrel
endif

.PHONY: all run install uninstall clean

all: build/build.ninja
	@cmake --build build

build/build.ninja:
	@cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

run:
	@$(APP)

install:
	@cmake --install build --component quarrel --prefix "$(PREFIX)"
ifeq ($(SYSTEM),windows)
	@powershell -NoProfile -Command "\$$s = (New-Object -ComObject \
	  WScript.Shell).CreateShortcut('$(subst /,\,$(START))\Quarrel.lnk'); \
	  \$$s.TargetPath = '$(subst /,\,$(PREFIX))\bin\Quarrel.exe'; \$$s.Save()"
endif

uninstall:
ifeq ($(SYSTEM),macos)
	@rm -rf $(PREFIX)/Quarrel.app
else ifeq ($(SYSTEM),windows)
	@rm -rf "$(PREFIX)" "$(START)/Quarrel.lnk"
	@reg delete 'HKCU\Software\Classes\AppUserModelId\com.vexcited.quarrel' \
	  //f > /dev/null 2>&1 || true
	@reg delete \
	  'HKCU\Software\Classes\CLSID\{8B156694-5CA4-4A65-9B60-0077E494E0C4}' \
	  //f > /dev/null 2>&1 || true
else
	@rm -f $(PREFIX)/bin/quarrel $(PREFIX)/share/applications/quarrel.desktop
endif

clean:
	@rm -rf build
