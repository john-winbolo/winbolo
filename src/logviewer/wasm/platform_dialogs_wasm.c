/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * platform_dialogs_wasm.c - Browser file dialogs for WASM build
 *
 * Replaces platform_dialogs.c — uses HTML5 file input for opening files
 * and emscripten_browser_file for downloads.
 */

#include "platform/platform_dialogs.h"
#include <string.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include <emscripten.h>

static int g_initialized = 0;

int lv_platform_dialogs_init(void) {
    g_initialized = 1;

    /* Create a hidden file input element for opening .wbv files */
    EM_ASM({
        if (!document.getElementById('wbv-file-input')) {
            var input = document.createElement('input');
            input.type = 'file';
            input.id = 'wbv-file-input';
            input.accept = '.wbv,.WBV';
            input.style.display = 'none';
            input.addEventListener('change', function(e) {
                var file = e.target.files[0];
                if (!file) return;
                var reader = new FileReader();
                reader.onload = function(ev) {
                    var data = new Uint8Array(ev.target.result);
                    var filename = '/tmp/' + file.name;

                    /* Write to Emscripten virtual filesystem */
                    try { FS.unlink(filename); } catch(err) {}
                    FS.writeFile(filename, data);

                    /* Call C function to load the file */
                    var fnamePtr = Module.stringToNewUTF8(filename);
                    Module._wasm_open_log_file(fnamePtr);
                    Module._free(fnamePtr);
                };
                reader.readAsArrayBuffer(file);
                /* Reset so the same file can be re-selected */
                input.value = '';
            });
            document.body.appendChild(input);
        }
    });

    return 1;
}

void lv_platform_dialogs_shutdown(void) {
    g_initialized = 0;
}

void lv_platform_dialogs_set_window(void *window) {
    (void)window;
}

int lv_platform_dialog_open_file(const char *title,
                              const char *filter_name,
                              const char *filter_ext,
                              const char *default_ext,
                              char *out_path,
                              size_t out_size,
                              const char *initial_dir) {
    (void)title; (void)filter_name; (void)filter_ext;
    (void)default_ext; (void)out_path; (void)out_size; (void)initial_dir;

    /* Trigger the hidden file input — the file will be loaded asynchronously
     * via the change event handler, which calls wasm_open_log_file().
     * We return CANCEL here; the actual open happens in the callback. */
    EM_ASM({
        var input = document.getElementById('wbv-file-input');
        if (input) input.click();
    });

    return PLATFORM_DIALOG_CANCEL;
}

int lv_platform_dialog_save_file(const char *title,
                              const char *filter_name,
                              const char *filter_ext,
                              const char *default_ext,
                              const char *default_name,
                              char *out_path,
                              size_t out_size,
                              const char *initial_dir) {
    (void)title; (void)filter_name; (void)filter_ext;
    (void)default_ext; (void)default_name;
    (void)out_path; (void)out_size; (void)initial_dir;

    /* Not yet implemented — would use emscripten_browser_file download */
    return PLATFORM_DIALOG_CANCEL;
}

int lv_platform_dialog_message(const char *title, const char *message) {
    EM_ASM({
        alert(UTF8ToString($0) + ": " + UTF8ToString($1));
    }, title, message);
    return 1;
}

int lv_platform_dialog_error(const char *title, const char *message) {
    EM_ASM({
        alert("Error - " + UTF8ToString($0) + ": " + UTF8ToString($1));
    }, title, message);
    return 1;
}

int lv_platform_dialog_question(const char *title, const char *message) {
    return EM_ASM_INT({
        return confirm(UTF8ToString($0) + ": " + UTF8ToString($1)) ? 1 : 0;
    }, title, message);
}
