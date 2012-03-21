/**
 * Recreate the primary surface endlessly.
 */

#include <math.h>
#include <config.h>
#include <stdlib.h>
#include "test_display_base.h"

SpiceServer *server;
SpiceCoreInterface *core;
SpiceTimer *ping_timer;

void show_channels(SpiceServer *server);

int ping_ms = 100;

void pinger(void *opaque)
{
    // show_channels is not thread safe - fails if disconnections / connections occur
    //show_channels(server);

    core->timer_start(ping_timer, ping_ms);
}

void set_primary_params(void *cb_opaque, uint64_t *arg1, uint64_t *arg2)
{
#if 0
    static int toggle = 0;

    if (toggle) {
        *arg1 = 800;
        *arg2 = 600;
    } else {
        *arg1 = 1024;
        *arg2 = 768;
    }
    toggle = 1 - toggle;
#endif
    static int count = 0;

    *arg1 = 800 + sin((float)count / 6) * 200;
    *arg2 = 600 + cos((float)count / 6) * 200;
    count++;
}

static Command commands[] = {
    {DESTROY_PRIMARY, 0, 0, NULL, NULL},
    {CREATE_PRIMARY, 0, 0, set_primary_params, NULL},
};

int main(void)
{
    core = basic_event_loop_init();
    server = test_init(core);
    //spice_server_set_image_compression(server, SPICE_IMAGE_COMPRESS_OFF);
    test_add_display_interface(server);
    test_set_command_list(commands, COUNT(commands));

    ping_timer = core->timer_add(pinger, NULL);
    core->timer_start(ping_timer, ping_ms);

    basic_event_loop_mainloop();

    return 0;
}
