## Usage

Quarrel is currently WIP so it is only supported through building from source, on macOS, Linux and Windows, x86-64 and arm64.

```sh
git clone https://code.vexcited.com/quarrel/quarrel
# mirror: https://github.com/vexcited/quarrel

make
make run
make install # to /Applications on macOS, ~/.local on Linux, %LOCALAPPDATA%\Programs\Quarrel on Windows
```

Quarrel is C2y, it builds with clang 22 or later.

### Linux

Quarrel runs on Wayland through civue, and needs what civue needs, see its README, with:

```sh
# arch linux
sudo pacman -S clang lld cmake ninja curl libsecret pipewire wireplumber libjpeg-turbo libsndfile opus openssl zstd

# ubuntu
sudo apt install clang lld cmake ninja-build libcurl4-openssl-dev libsecret-1-dev \
  libpipewire-0.3-dev libturbojpeg0-dev libsndfile1-dev libopus-dev libssl-dev libzstd-dev
```

- The build is for the machine it runs on, `-march=native` on x86-64 and `-mcpu=native` on arm64, with link time optimization through lld when it is installed. civue draws on the CPU, with AVX2 where there is on x86-64 and NEON on arm64, where a native build adds what the core has past it: the atomics of ARMv8.1 and the dot products the noise suppression multiplies with. `cmake -DQUARREL_NATIVE=OFF` builds for any processor of the architecture, and `-DQUARREL_LTO=OFF` links faster.
- Tokens are kept by the Secret Service, as GNOME Keyring or KWallet provide it: without one, Quarrel logs in again at every launch.
- Sound and cameras go through PipeWire. Echo cancellation is PipeWire's own module, with WebRTC's canceller.
- Cameras that film in JPEG, as most do at 720p, need libturbojpeg.
- Notifications go to the desktop's own server, over `org.freedesktop.Notifications`. Their sound and the sounds of a call come from a pack in the binary, Discord's or Fluxer's, or from a file of the user's for each. They play through ciren, which reads their mp3 with libsndfile, 1.1 or later. A call in a private channel rings, and its notification has a button to answer and one to decline where the server shows buttons.

### Windows

Quarrel runs on Windows 10 and 11 through civue, and builds in the CLANGARM64 shell of MSYS2 on arm64, or its CLANG64 shell on x86-64. It needs what civue needs, see its README, with:

```sh
pacman -S make $MINGW_PACKAGE_PREFIX-{clang,lld,cmake,ninja,pkgconf,curl,openssl,opus,zstd}
```

- The build is for the machine it runs on, as on Linux.
- `make install` copies Quarrel with the DLLs it needs, the WebView2 loader and curl's certificates to `%LOCALAPPDATA%\Programs\Quarrel`, and adds it to the Start menu. `make uninstall` takes them back.
- Tokens are kept by the Credential Manager, as generic credentials named `Quarrel/<key>`. A value too large for one is sealed to the user with DPAPI, in `%APPDATA%\Quarrel`. Preferences are under `HKCU\Software\Quarrel`, caches in `%LOCALAPPDATA%\Quarrel`.
- Sound goes through WASAPI, as a communications stream: echo cancellation is the device's own, or Voice Clarity's on Windows 11 where the device has none, the canceller told which speakers the call plays to. Windows 10 with a device that has none cancels no echo. The microphone needs desktop apps to be allowed it, in Settings, Privacy & security, Microphone.
- Cameras go through Media Foundation.
- Notifications are the system's toasts, pressed through a COM class Quarrel registers for itself under `HKCU\Software\Classes`. A call in a private channel rings with a notification that has a button to answer and one to decline.

## License

Quarrel is licensed under the [GPL-3.0](LICENSE).

## Credits

- The Unicode emoji are [Twemoji](https://github.com/jdecked/twemoji), its graphics licensed under [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/).
- The sounds of the Fluxer pack are [Fluxer](https://github.com/fluxerapp/fluxer)'s, copyright Fluxer Platform AB, licensed under the [AGPL-3.0](assets/fluxer/LICENSE). See [assets/fluxer](assets/fluxer/README.md).
- The sounds of the Discord pack are Discord's, see [assets/discord](assets/discord/README.md).
