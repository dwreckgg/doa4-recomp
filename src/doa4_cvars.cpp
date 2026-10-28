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
