PREFIX ?= /Applications

.PHONY: all run install uninstall clean

all: build/build.ninja
	@cmake --build build

build/build.ninja:
	@cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

run:
	@build/Quarrel.app/Contents/MacOS/Quarrel

install:
	@cmake --install build --component quarrel --prefix $(PREFIX)

uninstall:
	@rm -rf $(PREFIX)/Quarrel.app

clean:
	@rm -rf build
