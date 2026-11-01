/* Per-game settings for the Windows layer: where the game believes it is installed and what it finds
 * in the registry. This default suits a game that only needs its own folder. To use your own, copy
 * this file and build with PROFILE=/path/to/your_profile.h (see build.sh).
 */
#ifndef RC_PROFILE_H
#define RC_PROFILE_H

#define PROFILE_EXE        "POP3.EXE"     /* the executable, relative to the game folder */
#define PROFILE_GUEST_ROOT "C:\\Ubisoft\\POP3"     /* the install folder, as the game sees it */
#define PROFILE_GUEST_EXE  "POP3.EXE"     /* the executable's name, as the game sees it */
#define PROFILE_TITLE      "Prince of Persia: The Two Thrones"         /* native window title */

/* Extra registry values, appended to the built-in ones in host/misc.c:
 *   { key, value, type (1 = REG_SZ, 4 = REG_DWORD), string data, dword data },
 * e.g. { "SOFTWARE\\Vendor\\Game", "InstallDir", 1, PROFILE_GUEST_ROOT, 0 }, */
#define PROFILE_REGISTRY

/* Named mutexes that already exist when the game starts (as if a launcher had made them): "name", */
#define PROFILE_EXISTING_MUTEXES "POP3Launcher",

/* Hardware.INI is written by the launcher's hardware test: the display modes and formats the game may use */
#define PROFILE_BUILTIN_INIS \
    { "Hardware.INI", \
      "[VIDEO]\n" \
      "Resolutions=640,480,32,800,600,32,1024,768,32,1280,720,32,1280,960,32,1280,1024,32,1600,1200,32\n" \
      "AdapterFormats=D3DFMT_X8R8G8B8\n" \
      "TextureFormats=D3DFMT_A8R8G8B8,D3DFMT_X8R8G8B8,D3DFMT_R5G6B5,D3DFMT_A1R5G5B5,D3DFMT_X1R5G5B5,D3DFMT_P8," \
      "D3DFMT_DXT1,D3DFMT_DXT3,D3DFMT_DXT5\n" \
      "RenderTargets=D3DFMT_A8R8G8B8,D3DFMT_X8R8G8B8\n" \
      "BufferDepths=D3DFMT_D24S8,D3DFMT_D24X8,D3DFMT_D16,D3DFMT_D32\n" },

#endif
