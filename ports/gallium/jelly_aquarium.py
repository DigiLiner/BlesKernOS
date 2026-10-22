"""Keep glmark2 benchmark pristine; patch only the Bles screensaver build.

Runs against pinned upstream glmark2 scene-jellyfish.cpp. The ordinary
benchmark path performs exactly its old transforms and draw call.
"""

def only(text, old, new):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'glmark2 jellyfish upstream changed: {old[:64]!r} ({count} matches)')
    return text.replace(old, new, 1)


def patch_jelly_scene(text):
    text = only(text, '#include "scene-jellyfish.h"',
                '#include "scene-jellyfish.h"\n#include <cmath>\n#include <cstdlib>')
    text = only(text,
        '''    world_.push();
    world_.translate(0.0, 5.0, -75.0);
    world_.rotate(sin(rotation_ / 10.0) * 30.0, 0.0, 1.0, 0.0);
    world_.rotate(sin(rotation_ / 20.0) * 30.0, 1.0, 0.0, 0.0);
    world_.scale(5.0, 5.0, 5.0);
    world_.translate(0.0, sin(rotation_ / 10.0) * 2.5, 0.0);''',
        '''    // No environment variable exists during the stock glmark2 benchmark.
    // Reuse one loaded mesh, two VBOs, one shader and its textures for every
    // jelly. Only the model transform and animation time vary per draw.
    const char *bk_count_setting = std::getenv("BKG_JELLY_ACTIVE") ?
        std::getenv("BKG_JELLY_COUNT") : nullptr;
    unsigned bk_count = 0;
    if (bk_count_setting && *bk_count_setting) {
        bk_count = static_cast<unsigned>(std::atoi(bk_count_setting));
        if (bk_count < 1) bk_count = 1;
        if (bk_count > 10) bk_count = 10;
    }
    const char *bk_variation_setting = std::getenv("BKG_JELLY_VARIETY");
    int bk_variation = bk_variation_setting ? std::atoi(bk_variation_setting) : 2;
    if (bk_variation < 0) bk_variation = 0;
    if (bk_variation > 3) bk_variation = 3;
    const unsigned bk_draw_count = bk_count ? bk_count : 1;
    for (unsigned bk_i = 0; bk_i < bk_draw_count; ++bk_i) {
    world_.push();
    if (bk_count) {
        // Draw order, ascent and stagger are independent for each jelly.
        // Spawn/recycle far outside the camera so the entire model enters
        // and leaves the screen, even with the largest size variation.
        const float t = static_cast<float>(currentTime_);
        const float index = static_cast<float>(bk_i);
        const float phase = index * 1.731f;
        const float speed = 2.4f + 0.65f * (0.5f + 0.5f * std::sin(phase + 2.1f));
        const float x_base = -16.0f + std::fmod(index * 12.7f + 5.0f, 32.0f);
        const float x = x_base + 2.1f * std::sin(t * 0.30f + phase);
        // Far-to-near order gives alpha blending sensible inter-jelly order.
        const float z = -86.0f + 1.6f * index;
        const float spread = static_cast<float>(bk_variation) * 0.15f;
        // Keep the existing four-jelly preset unchanged. With fewer jellyfish,
        // individuals are larger; with more, they gradually shrink to avoid
        // overcrowding (1: 3.94, 4: 3.40, 10: 2.32 base scale).
        const float base_scale = 3.4f - 0.18f * (static_cast<float>(bk_count) - 4.0f);
        const float scale = base_scale * (1.0f + spread * std::sin(phase * 2.97f));
        // The former fixed spawn (-54) exposed the bell as soon as a jelly
        // wrapped. Place its centre below the whole visible frustum, plus a
        // generous size-dependent model margin. Recycle at the matching top.
        const float half_view_y = 20.0f;
        const float model_margin = 30.0f + 10.0f * scale;
        const float travel = 2.0f * (half_view_y + model_margin);
        const float stagger = (index / static_cast<float>(bk_count)) * travel;
        const float y = -(half_view_y + model_margin)
                      + std::fmod(t * speed + stagger, travel);
        world_.translate(x, y, z);
        world_.rotate(12.0f * std::sin(t * 0.28f + phase), 0.0, 1.0, 0.0);
        world_.rotate(5.0f * std::sin(t * 0.19f + phase), 1.0, 0.0, 0.0);
        world_.scale(scale, scale, scale);
        world_.translate(0.0, 0.7f * std::sin(t * 1.0f + phase), 0.0);
    } else {
        // Exact original geometry path, preserving benchmark measurements.
        world_.translate(0.0, 5.0, -75.0);
        world_.rotate(sin(rotation_ / 10.0) * 30.0, 0.0, 1.0, 0.0);
        world_.rotate(sin(rotation_ / 20.0) * 30.0, 1.0, 0.0, 0.0);
        world_.scale(5.0, 5.0, 5.0);
        world_.translate(0.0, sin(rotation_ / 10.0) * 2.5, 0.0);
    }''')
    text = only(text, '    program_["uCurrentTime"] = currentTime_;',
         '''    if (bk_count)
        program_["uCurrentTime"] = static_cast<float>(currentTime_ + static_cast<double>(bk_i) * 0.43);
    else
        program_["uCurrentTime"] = currentTime_;''')
    text = only(text, '    world_.pop();\n}', '    world_.pop();\n    } // each aquarium jelly\n}')
    return text
