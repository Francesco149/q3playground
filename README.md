quake 3 bsp renderer in c89 and opengl 1.x

![](https://i.imgur.com/Ig4hFPQ.gif)

dependencies: libGL, libGLU, SDL2

should be compatible at least with x86/x86\_64 windows, linux, freebsd
with gcc, clang, msvc

I might or might not add more features in the future, for now I have:

* rendering meshes and patches
* vertex lighting
* collision detection with brushes (no patches aka curved surfaces yet)

I want to at least implement physics and maybe collisions with patches

# compiling
just run ```./build``` . it's aware of ```CC```, ```CFLAGS```,
```LDFLAGS``` in case you need to override anything

windows build script is TODO

# usage
unzip the .pk3 files from your copy of quake 3. some of these will
contain .bsp files for the maps. you can run q3playground on them
like so:

```
q3playground /path/to/map.bsp
```

on *nix you might want to disable vsync by running it like

```
env vblank_mode=0 q3playground /path/to/map.bsp
```

controls are WASD, mouse, numpad +/-

run ```q3playground``` with no arguments for more info

# license
this is free and unencumbered software released into the public domain.
refer to the attached UNLICENSE or http://unlicense.org/

# references
* unofficial bsp format spec: http://www.mralligator.com/q3/
* tessellation implementation:
  http://graphics.cs.brown.edu/games/quake/quake3.html
* collision detection:
  https://web.archive.org/web/20041206085743/http://www.nathanostgard.com:80/tutorials/quake3/collision/
