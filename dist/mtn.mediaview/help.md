# Picture and video viewer (libmpv)

**F3** on a picture or a video opens the file in a viewer tab. **Esc** / **F10** close the tab.
Full screen is switched only with `f` / Enter / a double click and is remembered: close the viewer in full
screen and the next one opens in it.

| Key | Action |
|---|---|
| PageDown / PageUp, Backspace | next / previous file of the folder (the panel cursor moves to the last file when the tab closes) |
| Home / End | first / last file |
| Insert | select / unselect the file and go to the next one; the panel shows the selection when the tab closes |
| + / - , mouse wheel | zoom (the wheel zooms around the pointer) |
| * or 0 | fit to the window |
| / or 1 | 100% |
| mouse drag, Alt+arrows | move the zoomed picture |
| middle button | fit / 100% |
| r or > (.) / Shift+R or < (,) | rotate by 90° clockwise / counterclockwise; < > need no Shift |
| f, Enter, double click | full screen |

Video:

| Key | Action |
|---|---|
| Space | pause / resume |
| Left / Right | seek by 5 s (Shift – 1 s, Ctrl – 60 s); a seek keeps the pause |
| ' / ; | frame forward / back |
| [ / ] / \ | slower / faster / normal speed (0.25x … 4x) |
| Up / Down | volume |
| m | mute |
| a | next audio track |
| e | what to do at the end: Stop / Repeat / Next |

Pictures: the arrows page through the folder, or move the picture when it is zoomed; Space is the next file.

Files inside archives and on sftp are opened through MTN2, with no size limit; allow the plugin `vfs.read`
for that: **Options → Plugins... → Permissions...**. Paging works as in a folder; the plugin keeps the two
next and the two previous files open (`cache-next`, `cache-prev`, `cache-max-mb` in `mediaview.conf`).

Volume, the end action and full screen are remembered. libmpv options can be overridden in `mediaview.conf`
next to the plugin.
