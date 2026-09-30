/*
 * Minimal wasm settings backend.
 *
 * The native xemu-settings.cc is C++ (toml++ + CNode + SDL paths) and stays
 * out of the browser build. This TU provides the config object populated
 * with compiled-in defaults, and no-op persistence. Consumers only need
 * g_config to exist with sane values.
 */
#include "qemu/osdep.h"
#include "xemu-settings.h"

/* The generated config tree is C++-only (CNode); fill defaults by hand for
 * the fields the wasm boot path actually reads. */
struct config g_config;

/* Native xemu loads settings from xemu.c's main(); the wasm build excludes
 * that TU, so populate g_config in an ELF constructor instead — runs before
 * main() no matter which entry path is taken. */
__attribute__((constructor))
static void xemu_settings_wasm_init(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_NULL;
    g_config.net.enable = false;

    /* Fixed MEMFS layout; the host page preloads these files before main().
     * Empty/NULL = absent (strlen checks in vl.c skip them). */
    g_config.sys.files.flashrom_path = "/xemu/Complex_4627.bin";
    g_config.sys.files.bootrom_path = "/xemu/mcpx_1.0.bin";
    g_config.sys.files.hdd_path = "/xemu/xbox_hdd.qcow2";
    g_config.sys.files.dvd_path = "/xemu/iso.iso";
    g_config.sys.files.eeprom_path = "/xemu/eeprom.bin";
}

bool xemu_settings_load(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_NULL;
    g_config.net.enable = false;

    /* Fixed MEMFS layout; the host page preloads these files before main().
     * Empty string = absent (strlen(path) == 0 checks in vl.c skip them). */
    g_config.sys.files.flashrom_path = "/xemu/Complex_4627.bin";
    g_config.sys.files.bootrom_path = "/xemu/mcpx_1.0.bin";
    g_config.sys.files.hdd_path = "/xemu/xbox_hdd.qcow2";
    g_config.sys.files.dvd_path = "/xemu/iso.iso";
    g_config.sys.files.eeprom_path = "/xemu/eeprom.bin";
    return true;
}

void xemu_settings_save(void)
{
    /* Persistence comes later via IndexedDB export; no-op for now. */
}

const char *xemu_settings_get_path(void)
{
    return "/xemu/xemu.toml";
}

void xemu_settings_set_path(const char *path)
{
    /* Browser FS is virtual; single fixed layout. */
    (void)path;
}

const char *xemu_settings_get_error_message(void)
{
    return "";
}

bool xemu_settings_load_gamepad_mapping(const char *guid,
                                        GamepadMappings **mapping)
{
    (void)guid;
    (void)mapping;
    return false;
}

void xemu_settings_reset_controller_mapping(const char *guid)
{
    (void)guid;
}
