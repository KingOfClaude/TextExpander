# Text Expander

A tiny text expander for Windows that lives in your system tray. Type a short trigger like `;date` in any app and it's replaced instantly with today's date, your email address, a fresh UUID, the current git branch, or anything else you define.

It's a single C++ source file using only the Win32 API: no runtime, no dependencies, no installer, and the whole thing is under 1 MB.

```
;date      ->  12/10/2026
;tomorrow ->  13/10/2026
;sig       ->  Your Name
;uuid      ->  8f7228b4-e4dd-4979-8884-d583088a79dc
;branch    ->  feature/login-fix
```

## Features

- Works in (almost) any app: editors, browsers, chat, forms
- Tray icon: left-click to open the snippet editor, right-click for the menu
- Add, edit, rename and delete snippets in a GUI, with a live preview of each expansion
- Dynamic placeholders: dates with offsets and custom formats, UUIDs, git branch, clipboard contents
- Triggers are case-insensitive (`;DATE` works too)
- Optional **Start with Windows** toggle
- Snippets are stored in a plain text file you can edit or back up
- Only one instance runs at a time

## Building

You need a C++17 compiler. Either of these works.

**Visual Studio.** Open **x64 Native Tools Command Prompt** (from the Start menu), `cd` to the folder and run:

```
cl /EHsc /O2 /std:c++17 textexpander.cpp /link /SUBSYSTEM:WINDOWS
```

**MinGW-w64** (e.g. via MSYS2):

```
g++ -std=c++17 -O2 -municode -mwindows -static textexpander.cpp -o textexpander.exe -lcomctl32 -lole32 -lshell32 -luser32 -lgdi32 -ladvapi32
```

The Visual Studio build gets modern-style controls (the manifest is embedded via a `#pragma`). The MinGW build works identically but uses the classic Windows look.

## Usage

1. Run `textexpander.exe`. On first launch the editor opens so you can replace the placeholder name, email, phone number and address.
2. Close the window and it keeps running in the tray (it may be inside the `^` overflow area; drag it out to keep it visible).
3. Type a trigger anywhere. It's replaced the moment you finish typing it.

### Tray menu

| Item | What it does |
|---|---|
| **Edit snippets** | Opens the editor (same as left-clicking the icon) |
| **Enabled** | Pauses or resumes expansion without quitting |
| **Start with Windows** | Adds/removes the app from your user startup list |
| **Quit** | Exits and removes the tray icon |

### Editor

- Select a snippet in the list to edit it. Changing the trigger and saving **renames** it.
- **New** clears the form, **Save snippet** stores it, **Delete** removes it.
- The **Preview** line shows what the expansion will produce right now.
- **Settings** at the bottom hold the date format and the folder used by `{branch}`.
- Triggers must be 2+ characters with no spaces. Starting them with `;` is recommended so they don't fire during normal typing.

## Default snippets

| Trigger | Expands to |
|---|---|
| `;sig` | `Your Name` (edit me) |
| `;email` | `you@example.com` (edit me) |
| `;phone` | `+44 7000 000000` (edit me) |
| `;addr` | `1 Example Street, Rushden, NN10 0AA` (edit me) |
| `;regards` | `Kind regards,` + new line + `Your Name` |
| `;date` | Today, e.g. `12/10/2026` |
| `;tomorrow` | Tomorrow's date |
| `;yesterday` | Yesterday's date |
| `;nextweek` | Date 7 days from now |
| `;day` | Weekday name, e.g. `Monday` |
| `;iso` | `2026-10-12` |
| `;time` | `17:34` |
| `;now` | `12/10/2026 17:34` |
| `;stamp` | `20261012-173400` (handy for filenames) |
| `;uuid` | A new random UUID each time |
| `;branch` | Current git branch (see [`{branch}`](#branch)) |
| `;shrug` | a shrug emoticon |
| `;lorem` | A sentence of lorem ipsum |

## Placeholders

Use these inside any expansion, mixed with normal text:

| Placeholder | Result |
|---|---|
| `{date}` | Date in your configured format (default `dd/mm/yyyy`) |
| `{date+N}` / `{date-N}` | Date N days ahead / behind |
| `{date:FORMAT}` | Date with a custom format, e.g. `{date:%A %d %B}` gives `Monday 12 October` |
| `{date+N:FORMAT}` | Both, e.g. `{date+7:%Y-%m-%d}` |
| `{time}` | Current time, `HH:MM` (accepts `:FORMAT` too) |
| `{now}` | Date and time (accepts `:FORMAT` too) |
| `{uuid}` | New lowercase UUID |
| `{branch}` | Current git branch |
| `{clipboard}` | Current clipboard text |

Format codes follow C `strftime`: `%d` day, `%m` month, `%Y` year, `%H` hour, `%M` minute, `%S` second, `%A` weekday name, `%B` month name, and so on. Unknown placeholders and invalid formats are left as literal text, so a typo shows up in the output rather than vanishing.

Example: a standup note snippet

```
Standup {date:%A %d %B} ({branch})
- Yesterday:
- Today:
```

### `{branch}`

The expander can't tell which project you're working in, so `{branch}` runs `git rev-parse --abbrev-ref HEAD` in the folder set under **Settings > Git folder**. Git must be on your `PATH`. If the folder isn't a repo you'll get `(not a git repo)`, and if git isn't found, `(git unavailable)`.

## Where things are stored

Snippets and settings live in:

```
%APPDATA%\TextExpander\snippets.txt
```

It's a UTF-8 text file. You can edit it by hand (restart the app afterwards) or copy it to another machine to bring your snippets with you:

```
# Text Expander snippets
@date_format=%d/%m/%Y
@git_dir=C:\code\myproject
;sig	Your Name
;regards	Kind regards,\nYour Name
```

Lines starting with `@` are settings. Every other line is `trigger<TAB>expansion`, where `\n`, `\t` and `\\` stand for a new line, tab and backslash.

To remove everything the app has stored: turn off **Start with Windows**, quit, and delete the `TextExpander` folder above.

## How it works

- A low-level keyboard hook watches what you type and keeps the last 64 characters in memory. The buffer is cleared whenever you press space, Enter, an arrow key, a shortcut such as Ctrl+C, and so on, so triggers only match when typed as one unbroken run.
- When the end of the buffer matches a trigger (longest match wins), the app sends backspaces to erase it and then types the expansion with synthetic key presses. Characters not on your keyboard layout are sent as Unicode input instead.
- The app ignores its own synthetic keystrokes and doesn't expand while its own editor window is focused, so you can type triggers into the editor safely.

### Privacy

The keyboard hook sees everything you type, so it's fair to ask what happens to it. Keystrokes are only held in a 64-character in-memory buffer used for matching. Nothing is logged, written to disk, or sent anywhere, and the program makes no network connections. The only files it writes are the snippets file and the optional startup registry entry. The source is a single file, so you can read it and verify that for yourself.

## Limitations

- **Windows only.**
- **Triggers fire instantly.** A trigger that starts another (`;date` and `;datetime`) shadows it, because `;date` fires before you can type the rest. The editor warns you when you save an overlapping pair.
- **Multi-line expansions press Enter** for each line break. In apps where Enter sends the message (some chat apps), the first line may be sent early.
- **Administrator windows:** Windows stops normal programs from seeing keystrokes in elevated apps. Run the expander as administrator too if you need it there.
- **Games, remote desktops and some terminals** may ignore synthetic keystrokes.
- **Layouts:** expansion uses your current keyboard layout. Characters that need AltGr or other layout-specific combos fall back to Unicode input, which a few older apps handle badly.
- Expansion text is typed rather than pasted, so very long snippets take a moment.

## Troubleshooting

**Nothing happens when I type a trigger.**
Check the tray menu: **Enabled** should be ticked. Make sure you're not in an elevated (administrator) window, and that the trigger matches exactly as saved (case doesn't matter).

**Garbled or repeated characters in one particular app.**
That app is probably mishandling synthetic input. Try typing the trigger in a different app to confirm, and note which app it is.

**Windows Defender or antivirus flags the `.exe`.**
Unsigned programs that install a keyboard hook look similar to keyloggers to some scanners. The safest option is to build it yourself from the source. You can also add an exclusion for the folder.

**Build error about `min` or `max`.**
The source already defines `NOMINMAX` before including `windows.h`. If you see this, make sure you have the latest copy of the file.

**The tray icon disappeared after Explorer restarted.**
It should come back by itself. If not, quit and relaunch the app.

**I launched it twice and nothing happened.**
That's intentional. Only one copy can run at a time.

## Ideas for later

- Clipboard-paste mode for long or multi-line snippets
- Per-app exclusion list
- Embedded `.ico` so the `.exe` shows the icon in File Explorer
- Cursor-position marker (`{cursor}`) for snippets that should leave the caret in the middle
