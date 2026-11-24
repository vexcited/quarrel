## Usage

Quarrel is currently WIP so it is only supported through building from source, on macOS and Linux.

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
sudo pacman -S clang cmake ninja curl libsecret pipewire wireplumber libjpeg-turbo opus openssl zstd

# ubuntu
sudo apt install clang cmake ninja-build libcurl4-openssl-dev libsecret-1-dev \
  libpipewire-0.3-dev libturbojpeg0-dev libopus-dev libssl-dev libzstd-dev
```

- Tokens are kept by the Secret Service, as GNOME Keyring or KWallet provide it: without one, Quarrel logs in again at every launch.
- Sound and cameras go through PipeWire. Echo cancellation is PipeWire's own module, with WebRTC's canceller.
- Cameras that film in JPEG, as most do at 720p, need libturbojpeg.
