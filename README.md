```
    _    ____  ____  _____ ____   ____   ____  
   / \  / ___||  _ \|  ___|  _ \ / ___| / ___| 
  / _ \ \___ \| |_) | |_  | | | | |     \___ \ 
 / ___ \ ___) |  _ <|  _| | |_| | |___   ___) |
/_/   \_\____/|_| \_\_|   |____/ \____| |____/ 
```

# asciivideo

**Every frame. Two characters. Zero and one.**

asciivideo is a tiny native Windows video player that throws out billions of
pixels and keeps only the ones worth remembering — rendering every frame of a
video as a live grid of `0` and `1`, voiced by the video's own soundtrack, and
ready to save that art as a real `.mp4`.

No frameworks. No Electron. No 400 MB of nothing. One `cpp`, one executable,
and `ffmpeg` doing the heavy lifting.

```
0000001110010001110010 0011001100001100
0001110110001101110100 0110000110110001
0000110001100011000011 1001001100100111
0011011100011001110010 0111011000110110
```

## Features

- **Pure ASCII** — every frame is rebuilt as `0`/`1` glyphs, each colored with
  the average color of the video cell under it.
- **Real audio** — the video's soundtrack plays along, so you *hear* it while
  you watch blocks of binary.
- **Save ASCII video** (`Ctrl+S`) — exports exactly what's on screen to a
  random-named `.mp4` in your Downloads folder, soundtrack included and color
  tagged `bt709` so it renders right on every player.
- **Open with** — one click from the Tools menu registers ascii_video in the
  Windows *Open with* list for `.mp4` files, right-clickable forever after.
- **Drag & drop**, `Ctrl+O`, or pass the file on the command line.
- Zoom with the wheel, lock the aspect ratio, on-screen info overlay.
- Remembers your recent files and settings.

## Requirements

- **Windows**
- **ffmpeg** on your `PATH` — grab it with:

  ```bat
  winget install Gyan.FFmpeg
  ```

## Build

Built with [w64devkit](https://github.com/skeeto/w64devkit):

```bat
build.bat
```

or straight from the source:

```bat
g++ -std=c++17 -municode -mwindows -O2 -static ascii_gui.cpp -o ascii_gui.exe ^
    -lcomdlg32 -lshell32 -lole32 -lwinmm -luuid -ladvapi32
```

A prebuilt `ascii_gui.exe` is included, so you can also skip straight to:

## Usage

```bat
ascii_gui.exe your_video.mp4
```

Press `Ctrl+S` any time to export the current ASCII view (with audio) to a
new `.mp4` in Downloads.

## `Open with` (one-time setup)

Run the app, then **Tools → Add to "Open with" (mp4)**. Now right-click any
`.mp4` → **Open with** → asciivideo, and every video is one click away from
becoming binary art.

## License

MIT — you can do basically whatever you want with it.
© 2026 [kr10x723](https://github.com/kr10x723) — see [LICENSE](LICENSE).