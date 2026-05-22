Open Siege - Windows Portable Build
====================================

Open Siege is a native Windows port of the Dynamix Darkstar 3Space engine,
the engine behind Starsiege: Tribes (1998). This build includes the
dts-viewer — a 3D asset viewer and mission explorer for Tribes assets.

You need your own copy of Tribes 1.41 (the freeware version is available
at various archive sites). Open Siege does NOT include any Tribes game data.


HOW TO RUN
----------

1. Extract this zip to any folder (e.g. C:\Games\OpenSiege\).

2. Double-click dts-viewer.exe.

3. On the first run, a prompt asks for your Tribes 1.41 install directory
   — the folder that contains a "base\" subfolder with Entities.vol inside.
   Example: C:\Tribes

4. The path is saved to:
   %APPDATA%\open-siege\config.toml

   Subsequent launches go straight to the viewer.


SMARTSCREEN WARNING
-------------------

When you first run dts-viewer.exe, Windows may show a "Windows protected
your PC" dialog. This happens because the build is not yet code-signed.

To run it:
  - Click "More info" in the SmartScreen dialog.
  - Click "Run anyway".

This is a known limitation of v1. Code signing is planned for a future
release.


TROUBLESHOOTING
---------------

"Missing DLL" error on launch:
  All required DLLs (SDL2.dll, glew32.dll, etc.) are included in this
  folder. If one is missing, re-download the zip from the GitHub Release.

Viewer crashes on start:
  Make sure your GPU supports OpenGL 3.3. Most discrete and integrated
  GPUs from 2012 onward do. Update your graphics driver if in doubt.

Config reset:
  Delete %APPDATA%\open-siege\config.toml to re-run the first-run prompt.


BUG REPORTS
-----------

https://github.com/vlouvet/open-siege/issues/new
