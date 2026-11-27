## Usage

Quarrel is currently WIP so it is only supported through building from source, on macOS and on Linux, x86-64 and arm64.

```sh
git clone https://code.vexcited.com/quarrel/quarrel
# mirror: https://github.com/vexcited/quarrel

make
make run
make install # to /Applications on macOS, ~/.local on Linux
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
- Notifications go to the desktop's own server, over `org.freedesktop.Notifications`. Their sound and the sounds of a call are Discord's, each fetched from its site once and kept in the caches. They play through ciren, which reads their mp3 with libsndfile, 1.1 or later. A call in a private channel rings with Discord's ringtone, and its notification has a button to answer and one to decline where the server shows buttons.

## License

Quarrel is licensed under the [GPL-3.0](LICENSE).

## Credits

- The Unicode emoji are [Twemoji](https://github.com/jdecked/twemoji), its graphics licensed under [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/).
