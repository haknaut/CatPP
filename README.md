# 🐾 CatPP

<p align="center">
  <strong>A blazing-fast, lightweight, pixel-perfect C++ Runner and Code Editor for Windows.</strong><br>
  Built from scratch in pure C++ using the native Win32 API and GDI+ — zero Electron bloat, zero web runtime overhead.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%2B%2B17-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white" alt="C++17" />
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-0078D6?style=for-the-badge&logo=windows&logoColor=white" alt="Windows" />
  <img src="https://img.shields.io/badge/Framework-Win32%20%7C%20GDI%2B-5C2D91?style=for-the-badge" alt="Win32 & GDI+" />
  <img src="https://img.shields.io/badge/License-MIT-success?style=for-the-badge" alt="License" />
</p>

---

## ✨ Overview

**CatPP** is a standalone, high-performance C++ code editor and instant runner designed for Windows developers who value raw speed, minimalist aesthetics, and distraction-free programming.

Unlike modern editors that run on gigabytes of RAM and heavy JavaScript runtimes, CatPP is engineered in over **8,000+ lines of hand-crafted Win32 and GDI+ C++**. It launches in milliseconds, maintains an ultra-low memory footprint, and provides an end-to-end environment to write, compile, debug, and run C++ code immediately.

---

## 🚀 Key Features

### ⚡ Pure Native Windows Speed
- **Sub-second startup**: Zero Electron, zero WebView2, zero Chromium baggage.
- **Tiny footprint**: Operates on a fraction of the RAM consumed by typical modern IDEs.
- **Responsive rendering**: High-refresh paced animation loop driven by `QueryPerformanceCounter` (QPC).

### 🎨 Modern Frameless Aesthetic
- **macOS-style window chrome**: Custom vector traffic-light window controls (Close, Minimize, Maximize) with hover and press micro-interactions.
- **Multi-Theme Engine**: Seamlessly cycle between themes (`Ctrl+T`):
  - 🌌 **Void**: Pure OLED pitch black for ultra-focus.
  - 🌘 **One Dark Pro**: Subtle, balanced dark mode with rich syntax contrast.
  - ☀️ **One Light**: Clean, high-legibility light theme.
- **Hand-crafted Custom Scrollbars**: Custom-rendered track and thumb with hover glow, drag hysteresis, and smooth easing — eliminating clunky default OS scrollbars.
- **Embedded Typography**: Bundled high-legibility fonts embedded as binary Windows resources:
  - *Google Sans Code* (Regular & Italic)
  - *Space Grotesk*

### 📝 Smart C++ Code Editor
- **RichEdit + TOM Core**: Powered by Win32 RichEdit and the Text Object Model (`ITextDocument`) for flicker-free, undo-safe text manipulation.
- **Real-Time Syntax Highlighting**: Colorizes keywords, types, control flow, strings, comments, preprocessor directives, and numbers.
- **Smooth Caret**: Custom gliding caret with smooth interpolation and blinking behavior.
- **IntelliSense & Snippets**: VS Code-style autocomplete overlay with interactive popup filtering for keywords and boilerplate snippets (`class`, `struct`, `template`, `for`, `switch`, etc.).
- **Smart Indentation**: Auto-indentation (4-space logical indents), auto-closing braces/brackets/quotes, and selected block indentation (`Tab` / `Shift+Tab`).
- **Quick Commenting**: One-touch line and block commenting (`Ctrl+/`).
- **Line Gutter**: Dedicated line numbering gutter synced with smooth editor scrolling.

### ⚙️ Integrated Compiler & Process Runner
- **One-Click Run (`Ctrl+R`)**: Automatically invokes MinGW GCC/G++ to compile and execute your C++ source in a single keystroke.
- **Interactive Console Output**: Live dual-pane view with real-time streamed stdout and stderr pipes. Supports interactive standard input (`stdin`) directly from the output console.
- **Diagnostic Jump-to-Error**: Background compilation checks syntax errors. **Double-click any compiler error** in the output console to jump the editor cursor directly to the exact line and column!
- **Asynchronous Execution**: Compilation and target process execution run in dedicated child threads/pipes, keeping the UI silky smooth at all times.

### 📂 Built-in File Explorer & Tooling
- **Project Tree Navigator (`Ctrl+E`)**: Collapsible sidebar with directory tree browsing and custom-vector SVG file/folder icons.
- **Modal Find & Replace (`Ctrl+F`, `Ctrl+H`, `F3`)**: Custom-painted dialog for finding and replacing occurrences with wrap-around navigation.
- **Drag & Drop**: Drag any `.cpp` file or project folder straight into the window to open.
- **Native-Look Modals**: No dated Windows 95 message boxes — custom styled dialogs matching the active editor theme.

---

## ⌨️ Keyboard Shortcuts

| Shortcut | Description |
| :--- | :--- |
| `Ctrl + R` | **Compile & Run** active file |
| `Ctrl + S` | **Save** file |
| `Ctrl + O` | **Open** C++ file |
| `Ctrl + N` | **New** file buffer |
| `Ctrl + E` | Toggle **File Explorer** sidebar |
| `Ctrl + T` | Cycle **Theme** (Void → One Light → One Dark) |
| `Ctrl + F` | **Find** in file |
| `Ctrl + H` | **Find & Replace** |
| `F3` | Find **Next** |
| `Ctrl + /` | Toggle **Comment** on line or selection |
| `Ctrl + D` | **Duplicate** line or selection |
| `Ctrl + Shift + K` | **Delete** current line |
| `Ctrl + Z` | **Undo** |
| `Ctrl + Y` / `Ctrl + Shift + Z` | **Redo** |
| `Tab` / `Shift + Tab` | **Indent / Outdent** selection |
| `Ctrl + +` / `Ctrl + -` | **Zoom In / Out** (or `Ctrl + MouseWheel`) |
| `Ctrl + 0` | Reset **Zoom** to default |
| `Ctrl + K` | Open **Shortcuts Reference** dialog |
| `Ctrl + Left / Right` | Jump to **start / end** of line |
| `Ctrl + Home / End` | Jump to **top / bottom** of document |

---

## 🛠️ Architecture & Under the Hood

CatPP is architected around Windows system APIs for maximum control over layout and rendering:

```
┌────────────────────────────────────────────────────────┐
│                   CatPP Main Window                    │
├────────────────────────────────────────────────────────┤
│  Top Bar (macOS Traffic Lights + Title + Run Button)   │
├───────────────┬────────────────────┬───────────────────┤
│ File Explorer │    Code Editor     │   Console Output  │
│  (Vector Tree │ (RichEdit 4.1 TOM  │  (Live Pipe Async │
│   & SVG Icons)│ + Custom Gutter)   │  Stdin / Stdout)  │
└───────────────┴────────────────────┴───────────────────┘
```

- **Win32 API (`user32`, `gdi32`, `dwmapi`)**: Custom frameless windowing with DWM desktop composition and multi-DPI awareness (`WM_DPICHANGED`).
- **GDI+ (`gdiplus`)**: High-quality vector antialiasing, custom SVG icon rendering, and color blending.
- **RichEdit & TOM (`ITextDocument`)**: Bypasses the standard edit control limits, utilizing COM Text Object Model interfaces to perform non-destructive lexical coloring and formatting.
- **Inter-Process Pipes (`CreatePipe`, `CreateProcessW`)**: Bidirectional streaming of process pipes enables real-time terminal I/O without blocking the message loop.

---

## 📦 Building from Source

### Prerequisites
- **OS**: Windows 10 or 11 (64-bit)
- **Compiler**: MinGW-w64 (GCC/G++ with C++17 support)
  - Recommended via [MSYS2](https://www.msys2.org/) (`pacman -S mingw-w64-x86_64-gcc`)
  - Or any standard MinGW distribution (e.g., WinLibs)

### Quick Build
1. Clone this repository:
   ```bash
   git clone https://github.com/haknaut/CatPP.git
   cd CatPP
   ```

2. Compile using the included automated build script:
   ```cmd
   build.bat
   ```
   > **Note:** If your `g++.exe` is not installed at the default `C:\msys64\mingw64\bin\g++.exe`, create a `gpp_path.txt` file containing the full path to your `g++.exe` (e.g., `D:\MinGW\bin\g++.exe`).

3. Run the executable:
   ```cmd
   CatPP.exe
   ```

---

## 📁 Repository Structure

```
CatPP/
├── icons/               # Hand-crafted vector SVG interface icons
│   ├── folder.svg
│   ├── run.svg
│   ├── search.svg
│   ├── file-saved.svg
│   └── ...
├── app.rc               # Windows resource script (fonts, icons, SVGs)
├── build.bat            # Automated build and resource compiler script
├── GoogleSansCode-*.ttf # Embedded editor typography
├── logo.ico             # Application window icon
├── main.cpp             # Complete application implementation (Win32/GDI+)
├── svg_res_table.inc    # SVG resource mapping definitions
├── .gitignore           # Git ignore rules for build artifacts
├── LICENSE              # MIT License
└── README.md            # Project documentation
```

---

## 📄 License

This project is licensed under the [MIT License](LICENSE) &copy; 2026 Ardhendu (haknaut).
Feel free to use, modify, and distribute as you see fit.

---

<p align="center">
  Crafted with ❤️ and pure C++
</p>
