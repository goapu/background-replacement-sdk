// Minimal integrator program: uses only the installed headers and library.
#include <cstdio>

#include "magic_portal/engine.h"
#include "magic_portal/magic_portal.h"

int main() {
    if (mp_abi_version() != MP_ABI_VERSION) {
        std::fprintf(stderr, "ABI mismatch\n");
        return 1;
    }
    magic_portal::Engine engine{magic_portal::EngineConfig{}};
    const magic_portal::RenderState state = engine.render_state(0);
    if (state.effect_enable || state.mask != nullptr) {
        return 2;
    }
    std::printf("consumer ok, magic_portal %s\n", mp_version_string());
    return 0;
}
