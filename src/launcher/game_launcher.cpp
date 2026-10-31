#include "game_launcher.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "../net/fluidnc_client.h"
#include "../display/ui_spiro.h"
#include "../display/ui_park.h"

namespace
{
    const esp_partition_t *gamePartition()
    {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "game");
    }
}

namespace GameLauncher
{
    bool installed()
    {
        const esp_partition_t *p = gamePartition();
        esp_app_desc_t desc;
        return p && esp_ota_get_partition_description(p, &desc) == ESP_OK;
    }

    const char *blockedReason()
    {
        if (!installed()) return "The game isn't installed on this dial.";
        const FluidNCStatus &st = fluidNC.status();
        if (st.jobActive || st.mode == MachineMode::Run || st.mode == MachineMode::Hold ||
            st.mode == MachineMode::Homing)
            return "The machine is busy. Finish or stop the job first.";
        if (uiSpiroPlotActive()) return "A spirograph plot is running. Let it finish first.";
        if (uiParkActive()) return "Park is running. Let it finish first.";
        return nullptr;
    }

    bool launch()
    {
        const esp_partition_t *game = gamePartition();
        if (!game || !installed()) return false;

        // Tell the game which slot the dial is in, so it can point the chip
        // back here the moment it starts.
        const esp_partition_t *dial = esp_ota_get_running_partition();
        Preferences prefs;
        prefs.begin("launcher", false);
        prefs.putUChar("dial", (uint8_t)dial->subtype);
        prefs.end();

        if (esp_ota_set_boot_partition(game) != ESP_OK) return false;
        Serial.println("[launcher] restarting into the game");
        delay(100);
        esp_restart();
        return true;
    }
}
