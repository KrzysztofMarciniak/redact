# redact

A tiny X11 tool for blacking out rectangles in PNG images. No metadata.

<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/f42ab51f-1b9c-4fb7-b818-dccd1678c0b7" />

## Why

Sometimes you just need to black out part of a screenshot before sharing
it, without pulling in a full image editor (and without it leaving EXIF/text
metadata behind). `redact` does exactly that and nothing else.

## Build

Dependencies: `libpng`, `libx11` (dev headers), a C compiler.

```sh
# Debian/Ubuntu
sudo apt install libpng-dev libx11-dev build-essential

make
sudo make install     # optional, installs to /usr/local/bin
```

Or build by hand:

```sh
gcc redact.c -o redact $(pkg-config --cflags --libs libpng x11) -lm
```

## Usage

```sh
./redact input.png output.png
```

A window opens showing `input.png`, centered on a black background.
Drag with the left mouse button to black out a rectangle. Nothing is
written to disk until you press `s`.

### Controls

| Key                | Action                     |
|--------------------|-----------------------------|
| drag (left mouse)  | Redact: black out a rectangle |
| `ctrl+z`           | Undo last rectangle        |
| `s`                | Save      |
| `h` / `l`          | Pan left / right            |
| `j` / `k`          | Pan down / up                |
| `+` / `-`          | Zoom in / out                |
| `0`                | Reset zoom/pan               |
| `q` / `Esc`        | Quit                          |

## Notes

- PNG only, in and out.
- Output files contain only `IHDR`/`IDAT`/`IEND` chunks, no `tEXt`,
  `tIME`, `iCCP`, `eXIf`, or any other ancillary metadata is ever
  written, regardless of what the source file contained.
- Requires a 24/32-bit TrueColor X11 display.
- Single source file, no external dependencies beyond libpng and X11.

## Other tool

Need to take the screenshot before you redact it? Check out
[**screenc**](https://github.com/krzysztofMarciniak/screenc) a very
simple C tool for creating PNG screenshots.

## License

GPLv3. See `LICENSE`.
