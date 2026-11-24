ifeq ($(shell uname -s),Darwin)
PREFIX ?= /Applications
APP = build/Quarrel.app/Contents/MacOS/Quarrel
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
	@cmake --install build --component quarrel --prefix $(PREFIX)

uninstall:
ifeq ($(shell uname -s),Darwin)
	@rm -rf $(PREFIX)/Quarrel.app
else
	@rm -f $(PREFIX)/bin/quarrel $(PREFIX)/share/applications/quarrel.desktop
endif

clean:
	@rm -rf build
