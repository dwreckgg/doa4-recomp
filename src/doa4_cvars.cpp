// Cvars the port defines. The Escape menu saves them to dead_or_alive_4.toml.

#include <rex/cvar.h>

// Extras tab: game tweaks (doa4_tweaks.h). Applied at load (OnPostLoadXexImage).
REXCVAR_DEFINE_BOOL(doa4_tag_all_stages, false, "DOA4",
                    "All stages in Tag battle (Xenia patch by ryu_highabusa)");
REXCVAR_DEFINE_BOOL(doa4_mp_hidden_characters, false, "DOA4",
                    "Hidden characters in multiplayer select (Xenia patch by ryu_highabusa)");

// First-run setup (doa4_gamefiles.h): the folder the setup chose. The command
// line's --game_data_root overrides it.
REXCVAR_DEFINE_STRING(doa4_game_files, "", "DOA4",
                      "Folder holding the game files (default.xex), chosen by the first-run setup");

// The save and shader-cache folder (doa4_gamefiles.h ChooseUserData); the
// command line's --user_data_root overrides it. Empty: Documents\dead_or_alive_4.
REXCVAR_DEFINE_STRING(doa4_user_data, "", "DOA4",
                      "Folder for saves and the shader cache (empty: Documents/dead_or_alive_4)");

// Photo tab: full-quality photos (doa4_photo_service.h).
REXCVAR_DEFINE_STRING(doa4_photo_format, "png_monitor", "DOA4",
                      "Photo files: png_monitor, png_full or jpg_full")
    .allowed({"png_monitor", "png_full", "jpg_full"});
REXCVAR_DEFINE_STRING(doa4_photo_dir, "", "DOA4",
                      "Folder for photos (empty: the Pictures folder's DOA4 folder)");
REXCVAR_DEFINE_BOOL(doa4_photo_auto, false, "DOA4",
                    "Also save a full-quality copy when an in-game snapshot is saved to the Album");

// Online tab (netplay phase 1; np_online.h). Saved through doa4::SaveTomlKey by the Online tab (Task 14). Host-only:
// none of them is carried to the other player (np_handoff's census lists them as not carried).
REXCVAR_DEFINE_UINT32(netplay_port, 7200, "DOA4",
                      "Online play: preferred UDP port (7200-7215 keep the join code short)");
REXCVAR_DEFINE_BOOL(netplay_delay_auto, true, "DOA4",
                    "Online play: input delay from the measured ping");
REXCVAR_DEFINE_INT32(netplay_delay, 2, "DOA4",
                     "Online play: input delay in frames when not automatic (1-8)")
    .range(1, 8);
REXCVAR_DEFINE_STRING(netplay_last_join, "", "DOA4",
                      "Online play: the last code joined");
REXCVAR_DEFINE_BOOL(netplay_show_stats, false, "DOA4",
                    "Online play: show the connection stats line");
REXCVAR_DEFINE_BOOL(netplay_upnp, true, "DOA4",
                    "Online play: open the port on the router (UPnP) when hosting");
