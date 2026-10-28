#include "pages.h"
#include "loader/title_loader.h"
#include "loader/boot/boot.h"
#include <string.h>

/// The Library page.
void
pound_page_render_library(const pound_page_context_t *POUND_RESTRICT context)
{
    if (POUND_UNLIKELY(NULL == context))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: context is NULL.");
        return;
    }

    pound_page_heading(context,
                       "LIBRARY",
                       "Library",
                       "Load dumped Nintendo Switch/Switch 2 games and keys.");

    pound_page_section(context, "Load Game");

    igTextWrapped("Select a dumped game file (XCI, NSP, NCA, NRO, NSO, HAC) to load.");
    igSpacing();

    static char game_path[POUND_CONFIG_PATH_MAX] = "";
    igInputText("Game Path", game_path, POUND_CONFIG_PATH_MAX, 0, NULL, NULL);
    if (igButton("Load Game", (ImVec2){0,0}))
    {
        if (game_path[0] != '\0')
        {
            pound_title_info_t title_info;
            if (pound_title_detect(game_path, &title_info) == POUND_SUCCESS)
            {
                (void)pound_title_load(&title_info);
                POUND_LOG_INFO(&thread_logger, "Loaded game from UI: %s", game_path);
            }
        }
    }

    igSpacing();
    igSeparator();
    igSpacing();

    pound_page_section(context, "Load Keys");

    igTextWrapped("Load your prod.keys or keys.txt file (required for encrypted titles).");
    igSpacing();

    static char keys_path[POUND_CONFIG_PATH_MAX] = "";
    const pound_config_t *const config = context->config;
    if (config != NULL && config->key_file[0] != '\0' && keys_path[0] == '\0')
    {
        strncpy(keys_path, config->key_file, POUND_CONFIG_PATH_MAX - 1);
        keys_path[POUND_CONFIG_PATH_MAX - 1] = '\0';
    }
    igInputText("Keys Path", keys_path, POUND_CONFIG_PATH_MAX, 0, NULL, NULL);
    if (igButton("Load Keys", (ImVec2){0,0}))
    {
        if (keys_path[0] != '\0' && config != NULL)
        {
            pound_config_set_path((char *)config->key_file, POUND_CONFIG_PATH_MAX, keys_path);
            POUND_LOG_INFO(&thread_logger, "Keys path set from UI: %s", keys_path);
        }
    }

    igSpacing();
    igSeparator();
    igSpacing();

    pound_page_section(context, "Where Compound looks");

    if (NULL == config)
    {
        POUND_LOG_ERROR(&thread_logger,
                        "The Library page has no config, so it cannot show the content folder.");
        igTextDisabled("Settings are unavailable this frame; see the Logs page.");
        return;
    }

    pound_page_labeled_value(context,
                             "Content folder",
                             ('\0' != config->content_dir[0]) ? config->content_dir : "(not set)",
                             0.3F);
    pound_page_labeled_value(context,
                             "Keys file",
                             ('\0' != config->key_file[0]) ? config->key_file : "(not set)",
                             0.3F);

    pound_page_section(context, "Formats Compound supports");

    igTextWrapped("Cartridge images (XCI) - Switch/Switch 2");
    igTextWrapped("Digital packages (NSP) - Switch/Switch 2");
    igTextWrapped("Encrypted title archives (NCA) - opened with your keys");
    igTextWrapped("Homebrew (NRO, NSO) - Switch/Switch 2 compatible");
    igSpacing();
    igPushStyleColor_Vec4(
        ImGuiCol_Text, pound_theme_imgui_color(context->theme, POUND_THEME_TEXT_SECONDARY));
    const bool font_pushed = pound_fonts_push(context->fonts, POUND_FONT_ROLE_SMALL);
    igTextWrapped("Compound never ships or embeds Nintendo's keys. You supply a keys file.");
    pound_fonts_pop(font_pushed);
    igPopStyleColor(1);
}

/*** end of file ***/
