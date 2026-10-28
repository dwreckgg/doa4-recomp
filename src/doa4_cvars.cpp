// Cvars the port defines. The Escape menu saves them to dead_or_alive_4.toml.

#include <rex/cvar.h>

// Extras tab: game tweaks (doa4_tweaks.h). Applied at load (OnPostLoadXexImage).
REXCVAR_DEFINE_BOOL(doa4_tag_all_stages, false, "DOA4",
                    "All stages in Tag battle (Xenia patch by ryu_highabusa)");
REXCVAR_DEFINE_BOOL(doa4_mp_hidden_characters, false, "DOA4",
                    "Hidden characters in multiplayer select (Xenia patch by ryu_highabusa)");
REXCVAR_DEFINE_BOOL(doa4_alpha152, false, "DOA4",
                    "Alpha-152 playable, replacing the Random slot (Xenia patch by ryu_highabusa)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// First-run setup (doa4_gamefiles.h): the folder the setup chose. The command
// line's --game_data_root overrides it.
REXCVAR_DEFINE_STRING(doa4_game_files, "", "DOA4",
                      "Folder holding the game files (default.xex), chosen by the first-run setup");

// Photo tab: full-quality photos (doa4_photo_service.h).
REXCVAR_DEFINE_STRING(doa4_photo_format, "png_monitor", "DOA4",
                      "Photo files: png_monitor, png_full or jpg_full")
    .allowed({"png_monitor", "png_full", "jpg_full"});
REXCVAR_DEFINE_STRING(doa4_photo_dir, "", "DOA4",
                      "Folder for photos (empty: the Pictures folder's DOA4 folder)");
REXCVAR_DEFINE_BOOL(doa4_photo_auto, false, "DOA4",
                    "Also save a full-quality copy when an in-game snapshot is saved to the Album");
