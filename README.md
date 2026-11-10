# Dynamic Island for Windows 10 and 11

A small, dependency-free Windows overlay inspired by macOS Dynamic Island. It is
rendered with Direct2D on a DirectComposition swap chain, so transparency and
animation stay on the GPU. When nothing is moving, the animation timer stops.

## Current features

- Apple-style compact Spotify island with live album artwork and level bars
- Spring-animated Media, Calendar, and Weather pages with a vertical dot indicator
- Spotify-only play/pause, previous, and next controls
- Spotify song title, artist, and session thumbnail artwork
- Apple-style elapsed/remaining timeline with live playback progress
- Spotify-only process loopback visualizer with five real FFT frequency bands
- Spotify-only volume control that leaves every other app unchanged
- Local month calendar with previous/next navigation
- Local temperature, conditions, high/low, humidity, and wind
- Click-through transparent area around the island
- Multi-monitor/DPI-aware positioning
- Optional **Run at startup** setting in the right-click menu
- Single-process guard and no taskbar button

## Build and run

Requirements:

- Windows 10 (version 1809 or newer, including 22H2) or Windows 11
- Visual Studio 2022 or newer (Community or Build Tools) with **Desktop development with C++**
- Any Windows SDK 10.0.20348 or newer (the newest installed one is used)

From PowerShell:

```powershell
.\scripts\build.ps1 -Run
```

The release executable is written to `build\Release\DynamicIsland.exe`.

## Controls

- **Click the island** — expand or collapse
- **Click anywhere outside** — collapse without blocking the underlying click
- **Scroll while expanded** — move through Media, Calendar, and Weather
- **Click a page dot** — jump directly to that page
- **Click media buttons** — previous, play/pause, next
- **Click the Media speaker button** — mute or unmute Spotify
- **Click calendar arrows** — move between months
- **Click the Weather refresh button** — fetch fresh conditions
- **Scroll while compact** — change Spotify’s volume
- **Right-click** — expand/collapse, toggle launch at sign-in, or exit

The Media tab intentionally selects only the Spotify desktop app’s Windows media
session. Browser videos and other music apps are ignored. Start playing
something in Spotify once if the title or album artwork is not immediately
available.

## Windows 10 notes

- **Visualizer:** Windows 11 can capture audio from only Spotify's process.
  Windows 10 (build 19045 and older) has no per-process capture, so the island
  captures the default speaker output instead. It only analyzes that audio
  while Spotify's own volume meter shows it is playing. If another app plays
  sound at the same time as Spotify, the bars include that app too.
- **Font:** Segoe UI Variable is a Windows 11 font. On Windows 10 the island
  uses Segoe UI.

## Weather location

Weather refreshes every 15 minutes. By default, `settings.ini` uses approximate
network location through [wttr.in](https://github.com/chubin/wttr.in), so the
page works without an account or API key. To choose a specific place, edit the
copy beside the executable:

```ini
[weather]
location=Philadelphia, PA
units=fahrenheit
```

`location` accepts a city, postal code, or airport code. `units` accepts `auto`,
`fahrenheit`, or `celsius`. Open the Weather tab and click the refresh button
after changing the file.

## Performance choices

- DirectComposition flip-model swap chain with premultiplied alpha
- Direct2D/DWrite rendering rather than a web view
- Rendering is event-driven at idle
- Spotify audio is analyzed on a background thread with a 1024-sample Hann-window FFT
- Animations use a frame-rate-independent spring and stop once settled
- No package manager and no runtime files beside the executable
