"""Apply the small RA8P1-specific patches to the doomgeneric engine sources.

Each patch is guarded by RA8_DOOM so the engine still builds unmodified on
other platforms.  The script is idempotent: already-patched files are skipped.
"""
import pathlib
import sys

ENGINE = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "doom/engine")
MARK = "RA8_DOOM"

PATCHES = {
    # 1. Do not allocate the unused 32-bit DG_ScreenBuffer (1 MB) - the RA8P1
    #    video backend scans I_VideoBuffer straight into the GLCDC framebuffer.
    "doomgeneric.c": [(
        "\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);\n",
        "#ifndef RA8_DOOM\n"
        "\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);\n"
        "#endif\n",
    )],
    "i_system.c": [
        # 2. Zone memory comes from a dedicated SDRAM block.
        (
            "byte *I_ZoneBase (int *size)\n{\n",
            "byte *I_ZoneBase (int *size)\n{\n"
            "#ifdef RA8_DOOM\n"
            "    {\n"
            "        extern byte *RA8_ZoneBase(int *size);\n"
            "        byte *zone = RA8_ZoneBase(size);\n"
            "        printf(\"zone memory: %p, %x allocated for zone\\n\", zone, *size);\n"
            "        return zone;\n"
            "    }\n"
            "#endif\n",
        ),
        # 3. Quitting from the menu restarts the board instead of returning.
        (
            "#if ORIGCODE\n    SDL_Quit();\n\n    exit(0);\n#endif\n}",
            "#if ORIGCODE\n    SDL_Quit();\n\n    exit(0);\n#endif\n"
            "#ifdef RA8_DOOM\n"
            "    {\n"
            "        extern void RA8_Quit(void);\n"
            "        RA8_Quit();\n"
            "    }\n"
            "#endif\n}",
        ),
    ],
    # 4. Proportional (analog) stick movement and turning.
    "g_game.c": [(
        "    if (forward > MAXPLMOVE) \n",
        "#ifdef RA8_DOOM\n"
        "    {\n"
        "        // Analog sticks: values are -127..127, already dead-zoned and\n"
        "        // response-curved by the controller driver.\n"
        "        extern void I_GetAnalogAxes(int *turn, int *fwd, int *strafe);\n"
        "        int ax_turn, ax_fwd, ax_strafe;\n"
        "\n"
        "        I_GetAnalogAxes(&ax_turn, &ax_fwd, &ax_strafe);\n"
        "        cmd->angleturn -= (short) ((ax_turn * angleturn[1]) / 127);\n"
        "        forward += (ax_fwd * forwardmove[speed]) / 127;\n"
        "        side += (ax_strafe * sidemove[speed]) / 127;\n"
        "    }\n"
        "#endif\n"
        "    if (forward > MAXPLMOVE) \n",
    )],
    # 5. Bind weapon cycling (used by the L1/R1 shoulder buttons).
    "m_controls.c": [
        ("int key_prevweapon = 0;\n",
         "#ifdef RA8_DOOM\nint key_prevweapon = '[';\n#else\nint key_prevweapon = 0;\n#endif\n"),
        ("int key_nextweapon = 0;\n",
         "#ifdef RA8_DOOM\nint key_nextweapon = ']';\n#else\nint key_nextweapon = 0;\n#endif\n"),
    ],
}

failed = False
for name, edits in PATCHES.items():
    path = ENGINE / name
    text = path.read_text(encoding="latin-1")
    if MARK in text:
        print(f"{name}: already patched")
        continue
    for old, new in edits:
        if text.count(old) != 1:
            print(f"{name}: anchor not found exactly once: {old[:50]!r}")
            failed = True
            break
        text = text.replace(old, new)
    else:
        path.write_text(text, encoding="latin-1", newline="\n")
        print(f"{name}: patched")
sys.exit(1 if failed else 0)
