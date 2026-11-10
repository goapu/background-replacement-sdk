#include <stdio.h>

#include "magic_portal/magic_portal.h"

int main(void) {
    mp_engine_config config;
    mp_engine *engine = NULL;
    mp_engine_config_init(&config);
    if (mp_engine_create(&config, &engine, NULL, 0) != MP_OK) {
        return 1;
    }
    mp_render_state state = {0};
    state.struct_size = sizeof state;
    if (mp_engine_get_render_state(engine, 0, &state) != MP_OK || state.effect_enable != 0) {
        return 2;
    }
    mp_engine_destroy(engine);
    printf("c consumer ok\n");
    return 0;
}
