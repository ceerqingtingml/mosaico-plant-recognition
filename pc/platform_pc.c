// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>

#include "gsp_sim_bridge.h"
#include "plant_ui.h"

static plant_ui_t state;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    return plant_ui_init(ui, &state);
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    plant_ui_deinit(ui, &state);
    fprintf(stderr, "plant_backend: page=%u ssid=%s last_error=%d\n",
            state.page, state.ssid[0] ? state.ssid : "(none)",
            (int)state.last_error);
}
