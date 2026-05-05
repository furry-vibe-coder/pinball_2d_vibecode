# Browser Build and itch.io Upload

## 1) Install and activate Emscripten

Use your emsdk shell (or run these once):

```bash
emsdk install latest
emsdk activate latest
source ./emsdk_env.sh
```

On Windows, you can use the "Emscripten Command Prompt" instead of `source`.

## 2) Build WebAssembly version

From project root:

```bash
make web
```

Output files are placed in `web/` (`index.html`, `.js`, `.wasm`).

## 3) Test locally

```bash
make serve-web
```

Open [http://localhost:8000](http://localhost:8000).

## 4) Upload to itch.io

1. Zip the contents of `web/` (not the folder itself if possible).
2. In itch.io project page, upload the zip as an HTML5 game.
3. Check "This file will be played in the browser".
4. Set viewport size to around 960x1280 (or enable responsive).

## Notes

- High score in web build uses browser `localStorage`.
- Native desktop build still uses `pinball_highscore.txt`.
- If `make web` fails with missing `em++`, run inside emsdk-enabled shell.
